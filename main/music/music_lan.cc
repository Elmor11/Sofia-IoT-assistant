// Music bridge over the local network: the same JSON messages as the BLE characteristics, one per
// line over TCP. See docs/music-bridge.md ("Conexión por la red local").

#include "music_bridge.h"

#if CONFIG_MUSIC_BRIDGE_BLE

#include "display.h"

#include <algorithm>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <psa/crypto.h>

#include <cstring>
#include <memory>
#include <string>

#define TAG "MusicLan"

namespace {

constexpr uint16_t kTcpPort = 8765;
constexpr uint16_t kDiscoveryPort = 8766;
constexpr size_t kMaxLineBytes = 240;
constexpr int kAuthTimeoutMs = 10000;
constexpr int kMaxBadAuths = 8;
constexpr char kDiscoveryQuery[] = "SOFIA?";

bool HexToBytes(const char* hex, uint8_t* out, size_t out_length) {
    if (hex == nullptr || strlen(hex) != 2 * out_length) {
        return false;
    }
    for (size_t i = 0; i < out_length; ++i) {
        char pair[3] = {hex[2 * i], hex[2 * i + 1], '\0'};
        if (!isxdigit(static_cast<unsigned char>(pair[0])) ||
            !isxdigit(static_cast<unsigned char>(pair[1]))) {
            return false;
        }
        out[i] = static_cast<uint8_t>(strtoul(pair, nullptr, 16));
    }
    return true;
}

void SetTimeouts(int fd) {
    // Detect a phone that left the network without closing the socket.
    int on = 1;
    int idle = 30;
    int interval = 10;
    int count = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

void SendRaw(int fd, const char* text) {
    send(fd, text, strlen(text), 0);
}

}  // namespace

void MusicBridge::StartLan() {
    if (lan_started_) {
        return;
    }
    lan_started_ = true;
    LoadIdentity();
    RegisterTools();
    if (xTaskCreate(LanTask, "music_lan", 4096, this, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the LAN task");
        lan_started_ = false;
    }
}

void MusicBridge::LanTask(void* arg) {
    static_cast<MusicBridge*>(arg)->RunLan();
    vTaskDelete(nullptr);
}

// The phone proves it scanned the QR code: HMAC-SHA256(secret, nonce as sent), in hex.
bool MusicBridge::VerifyLanAuth(const std::string& nonce_hex, const char* mac_hex) {
    uint8_t mac[32];
    if (!HexToBytes(mac_hex, mac, sizeof(mac)) || psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_key_id_t key = 0;
    if (psa_import_key(&attributes, secret_, sizeof(secret_), &key) != PSA_SUCCESS) {
        return false;
    }
    psa_status_t status = psa_mac_verify(key, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                                         reinterpret_cast<const uint8_t*>(nonce_hex.data()),
                                         nonce_hex.size(), mac, sizeof(mac));
    psa_destroy_key(key);
    return status == PSA_SUCCESS;
}

bool MusicBridge::SendLan(const std::string& line) {
    std::lock_guard<std::mutex> lock(lan_send_mutex_);
    int fd = lan_fd_.load();
    if (fd < 0) {
        return false;
    }
    std::string framed = line + "\n";
    size_t sent = 0;
    while (sent < framed.size()) {
        int rc = send(fd, framed.data() + sent, framed.size() - sent, 0);
        if (rc <= 0) {
            ESP_LOGW(TAG, "Send failed, errno=%d", errno);
            return false;
        }
        sent += rc;
    }
    return true;
}

void MusicBridge::RunLan() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (listen_fd < 0 || udp_fd < 0) {
        ESP_LOGE(TAG, "socket() failed, errno=%d", errno);
        return;
    }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kTcpPort);
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "TCP bind/listen failed, errno=%d", errno);
        return;
    }
    addr.sin_port = htons(kDiscoveryPort);
    if (bind(udp_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "UDP bind failed, errno=%d", errno);
        return;
    }
    ESP_LOGI(TAG, "Listening as %s on TCP %u (discovery UDP %u)", device_name_.c_str(), kTcpPort,
             kDiscoveryPort);

    int client_fd = -1;
    bool authed = false;
    std::string nonce;
    std::string line;
    TickType_t connected_at = 0;
    int bad_auths = 0;
    char buffer[128];

    auto close_client = [&](const char* why) {
        if (client_fd < 0) {
            return;
        }
        ESP_LOGI(TAG, "Phone disconnected (%s)", why);
        {
            std::lock_guard<std::mutex> lock(lan_send_mutex_);
            lan_fd_ = -1;
            lan_ready_ = false;
        }
        close(client_fd);
        client_fd = -1;
        authed = false;
        line.clear();
        if (!connected_) {
            ShowNowPlaying("", "", 0, 0, kMusicStopped);
        }
    };

    while (true) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listen_fd, &read_set);
        FD_SET(udp_fd, &read_set);
        int max_fd = std::max(listen_fd, udp_fd);
        if (client_fd >= 0) {
            FD_SET(client_fd, &read_set);
            max_fd = std::max(max_fd, client_fd);
        }
        timeval timeout = {1, 0};
        int ready = select(max_fd + 1, &read_set, nullptr, nullptr, &timeout);
        if (ready < 0) {
            ESP_LOGW(TAG, "select failed, errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (client_fd >= 0 && !authed &&
            xTaskGetTickCount() - connected_at > pdMS_TO_TICKS(kAuthTimeoutMs)) {
            close_client("no auth");
        }

        if (FD_ISSET(udp_fd, &read_set)) {
            sockaddr_in from = {};
            socklen_t from_length = sizeof(from);
            int n = recvfrom(udp_fd, buffer, sizeof(buffer) - 1, 0,
                             reinterpret_cast<sockaddr*>(&from), &from_length);
            if (n > 0) {
                buffer[n] = '\0';
                if (strncmp(buffer, kDiscoveryQuery, sizeof(kDiscoveryQuery) - 1) == 0) {
                    char reply[64];
                    snprintf(reply, sizeof(reply), "{\"n\":\"%s\",\"p\":%u}", device_name_.c_str(),
                             kTcpPort);
                    sendto(udp_fd, reply, strlen(reply), 0, reinterpret_cast<sockaddr*>(&from),
                           from_length);
                }
            }
        }

        if (FD_ISSET(listen_fd, &read_set)) {
            int fd = accept(listen_fd, nullptr, nullptr);
            if (fd >= 0) {
                // One phone at a time: the newest connection wins.
                close_client("replaced");
                if (bad_auths >= kMaxBadAuths) {
                    SendRaw(fd, "{\"auth\":\"locked\"}\n");
                    close(fd);
                } else {
                    SetTimeouts(fd);
                    client_fd = fd;
                    connected_at = xTaskGetTickCount();
                    uint8_t raw[16];
                    esp_fill_random(raw, sizeof(raw));
                    nonce.clear();
                    for (uint8_t byte : raw) {
                        char hex[3];
                        snprintf(hex, sizeof(hex), "%02x", byte);
                        nonce += hex;
                    }
                    std::string hello = "{\"hello\":\"" + device_name_ + "\",\"nonce\":\"" + nonce +
                                        "\"}\n";
                    SendRaw(fd, hello.c_str());
                    ESP_LOGI(TAG, "Phone connected, waiting for auth");
                }
            }
        }

        if (client_fd >= 0 && FD_ISSET(client_fd, &read_set)) {
            int n = recv(client_fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                close_client(n == 0 ? "closed" : "error");
                continue;
            }
            for (int i = 0; i < n && client_fd >= 0; ++i) {
                char c = buffer[i];
                if (c != '\n') {
                    if (c != '\r') {
                        line += c;
                    }
                    if (line.size() > kMaxLineBytes) {
                        close_client("line too long");
                    }
                    continue;
                }
                if (line.empty()) {
                    continue;
                }
                if (!authed) {
                    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(line.c_str()),
                                                                        cJSON_Delete);
                    cJSON* auth = root ? cJSON_GetObjectItem(root.get(), "auth") : nullptr;
                    if (cJSON_IsString(auth) && VerifyLanAuth(nonce, auth->valuestring)) {
                        authed = true;
                        SendRaw(client_fd, "{\"auth\":\"ok\"}\n");
                        std::lock_guard<std::mutex> lock(lan_send_mutex_);
                        lan_fd_ = client_fd;
                        lan_ready_ = true;
                        ESP_LOGI(TAG, "Phone authenticated");
                    } else {
                        ++bad_auths;
                        ESP_LOGW(TAG, "Bad auth (%d/%d)", bad_auths, kMaxBadAuths);
                        SendRaw(client_fd, "{\"auth\":\"bad\"}\n");
                        close_client("bad auth");
                    }
                } else {
                    OnStateWrite(reinterpret_cast<const uint8_t*>(line.data()), line.size());
                }
                line.clear();
            }
        }
    }
}

#endif  // CONFIG_MUSIC_BRIDGE_BLE
