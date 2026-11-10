#include "music_bridge.h"

#if CONFIG_MUSIC_BRIDGE_BLE

#if !CONFIG_BT_NIMBLE_ENABLED
#error "CONFIG_MUSIC_BRIDGE_BLE needs the NimBLE host: add CONFIG_BT_NIMBLE_ENABLED=y to the board's sdkconfig_append"
#endif

#include "board.h"
#include "settings.h"
#include "application.h"
#include "display.h"
#include "mcp_server.h"

#include <algorithm>
#include <memory>

#include <cJSON.h>
#include <esp_bt.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <psa/crypto.h>
#include <ssid_manager.h>

#include <cstring>
#include <vector>

#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

// Defined by NimBLE but missing from its header; the ESP-IDF examples declare it the same way.
extern "C" void ble_store_config_init(void);

#define TAG "MusicBridge"

namespace {

constexpr size_t kMaxStateBytes = 240;
constexpr size_t kMaxProvisionBytes = 200;
constexpr int kMaxBadAttempts = 8;
constexpr size_t kMaxTextChars = 40;
constexpr size_t kMaxQueryBytes = 100;
constexpr int64_t kMaxMs = 24LL * 3600 * 1000;

// Proposed UUIDs, shared with the companion app (docs/music-bridge.md):
//   service 6e7a0001-3b1d-4f5e-9a2c-5b8d1c0f7a10, Cmd ...0002, State ...0003, Prov ...0004.
const ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(0x10, 0x7a, 0x0f, 0x1c, 0x8d, 0x5b, 0x2c, 0x9a,
                                                    0x5e, 0x4f, 0x1d, 0x3b, 0x01, 0x00, 0x7a, 0x6e);
const ble_uuid128_t kCmdUuid = BLE_UUID128_INIT(0x10, 0x7a, 0x0f, 0x1c, 0x8d, 0x5b, 0x2c, 0x9a,
                                                0x5e, 0x4f, 0x1d, 0x3b, 0x02, 0x00, 0x7a, 0x6e);
const ble_uuid128_t kStateUuid = BLE_UUID128_INIT(0x10, 0x7a, 0x0f, 0x1c, 0x8d, 0x5b, 0x2c, 0x9a,
                                                  0x5e, 0x4f, 0x1d, 0x3b, 0x03, 0x00, 0x7a, 0x6e);
const ble_uuid128_t kProvUuid = BLE_UUID128_INIT(0x10, 0x7a, 0x0f, 0x1c, 0x8d, 0x5b, 0x2c, 0x9a,
                                                 0x5e, 0x4f, 0x1d, 0x3b, 0x04, 0x00, 0x7a, 0x6e);

// The OLED font only has basic Latin: drop what it cannot draw and fold common accents.
std::string ToDisplayText(const char* text, size_t max_chars) {
    std::string out;
    if (text == nullptr) {
        return out;
    }
    auto bytes = reinterpret_cast<const uint8_t*>(text);
    for (size_t i = 0; bytes[i] != '\0' && out.size() < max_chars; ++i) {
        uint8_t c = bytes[i];
        if (c >= 0x20 && c <= 0x7e) {
            out += static_cast<char>(c);
        } else if (c < 0x20 || c == 0x7f) {
            out += ' ';
        } else if (c == 0xc3 && bytes[i + 1] != '\0') {
            static const char kFold[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
            uint8_t next = bytes[++i];
            if (next >= 0x80 && next <= 0xbf) {
                out += kFold[next - 0x80];
            }
        } else if (c >= 0xc0) {
            // Skip the rest of any other multi-byte character.
            while ((bytes[i + 1] & 0xc0) == 0x80) {
                ++i;
            }
        }
    }
    return out;
}

// Cuts at a byte limit without splitting a UTF-8 character.
std::string TruncateUtf8(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    size_t end = max_bytes;
    while (end > 0 && (static_cast<uint8_t>(text[end]) & 0xc0) == 0x80) {
        --end;
    }
    return text.substr(0, end);
}

int GapEvent(struct ble_gap_event* event, void* arg) {
    auto& bridge = MusicBridge::GetInstance();
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(TAG, "GAP connect status=%d", event->connect.status);
            if (event->connect.status == 0) {
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                    ESP_LOGI(TAG, "Link: peer addr type=%d itvl=%d latency=%d timeout=%d",
                             desc.peer_id_addr.type, desc.conn_itvl, desc.conn_latency,
                             desc.supervision_timeout);
                }
                bridge.OnConnected(event->connect.conn_handle);
                // Ask the phone to encrypt the link; the state characteristic needs it. While
                // setting up Wi-Fi the credentials are already encrypted, so skip the pairing.
                if (!bridge.IsProvisioning()) {
                    ble_gap_security_initiate(event->connect.conn_handle);
                }
            } else {
                bridge.Advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            // Host-side reason; 0x200 + HCI code (e.g. 0x208 timeout, 0x213 remote user, 0x23e failed)
            ESP_LOGW(TAG, "Disconnect reason=0x%x", event->disconnect.reason);
            bridge.OnDisconnected();
            bridge.Advertise();
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            bridge.OnEncrypted(event->enc_change.status == 0);
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == *bridge.CmdValueHandle()) {
                bridge.OnSubscribe(event->subscribe.cur_notify != 0);
            } else if (event->subscribe.attr_handle == *bridge.ProvValueHandle()) {
                bridge.OnProvisionSubscribe(event->subscribe.cur_notify != 0);
            }
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            bridge.Advertise();
            break;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // The phone lost its bond: forget the old one and pair again.
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU updated to %d", event->mtu.value);
            break;
        default:
            break;
    }
    return 0;
}

int AccessCallback(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt,
                   void* arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;  // reading a notify-only characteristic
    }
    bool is_state = ble_uuid_cmp(ctxt->chr->uuid, &kStateUuid.u) == 0;
    bool is_prov = ble_uuid_cmp(ctxt->chr->uuid, &kProvUuid.u) == 0;
    if (!is_state && !is_prov) {
        return 0;
    }
    uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length == 0 || length > (is_prov ? kMaxProvisionBytes : kMaxStateBytes)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    std::string data(length, '\0');
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, data.data(), length, &copied) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    auto bytes = reinterpret_cast<const uint8_t*>(data.data());
    if (is_prov) {
        MusicBridge::GetInstance().OnProvisionWrite(bytes, copied);
    } else {
        MusicBridge::GetInstance().OnStateWrite(bytes, copied);
    }
    return 0;
}

struct ble_gatt_chr_def g_characteristics[] = {
    {
        .uuid = &kCmdUuid.u,
        .access_cb = AccessCallback,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = MusicBridge::GetInstance().CmdValueHandle(),
    },
    {
        .uuid = &kStateUuid.u,
        .access_cb = AccessCallback,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
    },
    {
        // Wi-Fi credentials, encrypted with the secret from the QR code (no link encryption).
        .uuid = &kProvUuid.u,
        .access_cb = AccessCallback,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = MusicBridge::GetInstance().ProvValueHandle(),
    },
    {},
};

const struct ble_gatt_svc_def g_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kServiceUuid.u,
        .characteristics = g_characteristics,
    },
    {},
};

void HostTask(void* param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void OnReset(int reason) { ESP_LOGW(TAG, "BLE host reset, reason=%d", reason); }

void OnSyncCallback() { MusicBridge::GetInstance().OnSync(); }

ToolResult SendToPhone(const char* command, const char* query, int delta) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(root.get(), "c", command);
    if (query != nullptr) {
        cJSON_AddStringToObject(root.get(), "q", query);
    }
    if (delta != 0) {
        cJSON_AddNumberToObject(root.get(), "d", delta);
    }
    char* text = cJSON_PrintUnformatted(root.get());
    if (text == nullptr) {
        return std::unexpected("Out of memory");
    }
    std::string json(text);
    cJSON_free(text);
    if (!MusicBridge::GetInstance().SendCommand(json)) {
        return std::unexpected(
            "The phone is not connected. Ask the user to open the Sofia Music app on the phone.");
    }
    return true;
}

}  // namespace

MusicBridge& MusicBridge::GetInstance() {
    static MusicBridge instance;
    return instance;
}

void MusicBridge::Start() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;

    LoadIdentity();

#if CONFIG_IDF_TARGET_ESP32 && !CONFIG_BTDM_CTRL_MODE_BLE_ONLY
    // BLE only: give the memory reserved for Bluetooth Classic back to the heap.
    esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
#endif
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.reset_cb = OnReset;
    ble_hs_cfg.sync_cb = OnSyncCallback;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // "Just works" pairing with bonding; the link is encrypted but not authenticated.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(g_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(g_services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to register the GATT service, rc=%d", rc);
        return;
    }
    ble_svc_gap_device_name_set(device_name_.c_str());
    ble_store_config_init();
    nimble_port_freertos_init(HostTask);

    RegisterTools();
    ESP_LOGI(TAG, "Started, advertising as %s", device_name_.c_str());
}

// The BLE name comes from the Bluetooth MAC; the pairing secret is random and kept in NVS so the
// QR code is the same on every boot.
void MusicBridge::LoadIdentity() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_BT);
    char name[16];
    snprintf(name, sizeof(name), "Sofia-%02X%02X", mac[4], mac[5]);
    device_name_ = name;

    Settings settings("sofia_link", true);
    std::string stored = settings.GetString("secret");
    bool valid = stored.size() == 2 * sizeof(secret_);
    for (char c : stored) {
        valid = valid && isxdigit(static_cast<unsigned char>(c));
    }
    if (!valid) {
        uint8_t raw[sizeof(secret_)];
        esp_fill_random(raw, sizeof(raw));
        char hex[2 * sizeof(secret_) + 1];
        for (size_t i = 0; i < sizeof(raw); ++i) {
            snprintf(hex + 2 * i, 3, "%02X", raw[i]);
        }
        stored = hex;
        settings.SetString("secret", stored);
        ESP_LOGI(TAG, "Generated a new pairing secret");
    }
    for (size_t i = 0; i < sizeof(secret_); ++i) {
        secret_[i] = static_cast<uint8_t>(strtoul(stored.substr(2 * i, 2).c_str(), nullptr, 16));
    }
    secret_hex_.clear();
    for (char c : stored) {
        secret_hex_ += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
}

bool MusicBridge::BeginWifiProvisioning(const char* caption) {
    qr_caption_ = caption != nullptr ? caption : "Escanea el\nQR con la\napp Sofia";
    if (device_name_.empty()) {
        return false;  // Start() has not run (or failed)
    }
    // "SF" + last 4 hex digits of the name + secret: 26 characters, the most a version 2 QR fits.
    std::string payload = "SF" + device_name_.substr(device_name_.size() - 4) + secret_hex_;
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr ||
        !display->SetPairingQr(payload.c_str(), qr_caption_.c_str())) {
        return false;
    }
    qr_payload_ = payload;
    bad_attempts_ = 0;
    provisioning_ = true;
    ESP_LOGI(TAG, "Waiting for Wi-Fi credentials from the phone app (%s)", device_name_.c_str());
    return true;
}

// Changes the text next to the QR code (the QR itself stays the same).
void MusicBridge::ShowProvisionCaption(const char* caption) {
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr && !qr_payload_.empty()) {
        display->SetPairingQr(qr_payload_.c_str(), caption);
    }
}

void MusicBridge::NotifyProvisioning(const char* json) {
    if (!connected_ || !prov_subscribed_) {
        return;
    }
    struct os_mbuf* om = ble_hs_mbuf_from_flat(json, strlen(json));
    if (om != nullptr) {
        ble_gatts_notify_custom(conn_handle_, prov_val_handle_, om);
    }
}

// Frame: nonce (12) | AES-128-GCM ciphertext | tag (16). The key is the first 16 bytes of
// SHA-256("sofia-wifi-v1" + secret).
bool MusicBridge::DecryptCredentials(const uint8_t* data, size_t length, std::string& json) {
    constexpr size_t kNonce = 12;
    constexpr size_t kTag = 16;
    if (length < kNonce + kTag + 2) {
        return false;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    static const char kSalt[] = "sofia-wifi-v1";
    uint8_t input[sizeof(kSalt) - 1 + sizeof(secret_)];
    memcpy(input, kSalt, sizeof(kSalt) - 1);
    memcpy(input + sizeof(kSalt) - 1, secret_, sizeof(secret_));
    uint8_t digest[32];
    size_t digest_length = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, input, sizeof(input), digest, sizeof(digest),
                         &digest_length) != PSA_SUCCESS) {
        return false;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_key_id_t key = 0;
    if (psa_import_key(&attributes, digest, 16, &key) != PSA_SUCCESS) {
        return false;
    }
    static const uint8_t kAad[] = {'S', 'O', 'F', 'I', 'A', '-', 'W', 'I', 'F', 'I'};
    std::vector<uint8_t> plain(length - kNonce - kTag);
    size_t plain_length = 0;
    psa_status_t status = psa_aead_decrypt(key, PSA_ALG_GCM, data, kNonce, kAad, sizeof(kAad),
                                           data + kNonce, length - kNonce, plain.data(),
                                           plain.size(), &plain_length);
    psa_destroy_key(key);
    if (status != PSA_SUCCESS) {
        return false;
    }
    json.assign(reinterpret_cast<const char*>(plain.data()), plain_length);
    return true;
}

void MusicBridge::OnProvisionWrite(const uint8_t* data, size_t length) {
    if (!provisioning_) {
        ESP_LOGW(TAG, "Ignoring Wi-Fi credentials: not in provisioning mode");
        return;
    }
    if (bad_attempts_ >= kMaxBadAttempts) {
        NotifyProvisioning("{\"w\":\"locked\"}");
        return;
    }
    std::string json;
    if (!DecryptCredentials(data, length, json)) {
        ++bad_attempts_;
        ESP_LOGW(TAG, "Wi-Fi credentials rejected (%d/%d)", bad_attempts_.load(), kMaxBadAttempts);
        ShowProvisionCaption("QR no valido\nEscanealo\nde nuevo");
        NotifyProvisioning("{\"w\":\"bad_key\"}");
        return;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(json.c_str()), cJSON_Delete);
    cJSON* ssid = root ? cJSON_GetObjectItem(root.get(), "s") : nullptr;
    cJSON* password = root ? cJSON_GetObjectItem(root.get(), "p") : nullptr;
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0' || strlen(ssid->valuestring) > 32 ||
        (password != nullptr &&
         (!cJSON_IsString(password) || strlen(password->valuestring) > 63))) {
        NotifyProvisioning("{\"w\":\"bad_data\"}");
        return;
    }
    std::string ssid_value = ssid->valuestring;
    std::string password_value = password != nullptr ? password->valuestring : "";
    provisioning_ = false;
    NotifyProvisioning("{\"w\":\"saved\"}");
    ShowProvisionCaption("Red guardada\nReiniciando...");
    ESP_LOGI(TAG, "Wi-Fi \"%s\" received, restarting", ssid_value.c_str());

    Application::GetInstance().Schedule([ssid_value, password_value]() {
        SsidManager::GetInstance().AddSsid(ssid_value, password_value);
        // Leave a moment for the "saved" notification to go out before restarting.
        static esp_timer_handle_t restart_timer = nullptr;
        if (restart_timer == nullptr) {
            esp_timer_create_args_t args = {};
            args.callback = [](void*) {
                Application::GetInstance().Schedule([]() { Application::GetInstance().Reboot(); });
            };
            args.name = "wifi_prov_restart";
            esp_timer_create(&args, &restart_timer);
        }
        esp_timer_start_once(restart_timer, 1500 * 1000);
    });
}

void MusicBridge::OnSync() {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &own_addr_type_);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "No BLE address, rc=%d", rc);
        return;
    }
    Advertise();
}

void MusicBridge::Advertise() {
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<const uint8_t*>(device_name_.c_str());
    fields.name_len = device_name_.size();
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed, rc=%d", rc);
        return;
    }
    // The 128-bit service UUID does not fit next to the name, so it goes in the scan response.
    struct ble_hs_adv_fields response = {};
    response.uuids128 = const_cast<ble_uuid128_t*>(&kServiceUuid);
    response.num_uuids128 = 1;
    response.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&response);

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type_, nullptr, BLE_HS_FOREVER, &params, GapEvent, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed, rc=%d", rc);
    }
}

void MusicBridge::OnConnected(uint16_t conn_handle) {
    ESP_LOGI(TAG, "Phone connected");
    conn_handle_ = conn_handle;
    encrypted_ = false;
    subscribed_ = false;
    connected_ = true;
    if (provisioning_) {
        ShowProvisionCaption("Telefono\nconectado\nEnviando red...");
    }
}

void MusicBridge::OnDisconnected() {
    ESP_LOGI(TAG, "Phone disconnected");
    connected_ = false;
    encrypted_ = false;
    subscribed_ = false;
    if (provisioning_) {
        ShowProvisionCaption(qr_caption_.c_str());
    }
    // Nothing can be playing as far as the device knows.
    ShowNowPlaying("", "", 0, 0, kMusicStopped);
}

void MusicBridge::OnEncrypted(bool ok) {
    ESP_LOGI(TAG, "Link encryption %s", ok ? "on" : "failed");
    encrypted_ = ok;
}

void MusicBridge::OnSubscribe(bool notify) {
    ESP_LOGI(TAG, "Phone %s command notifications", notify ? "subscribed to" : "left");
    subscribed_ = notify;
}

bool MusicBridge::SendCommand(const std::string& json) {
    if (!connected_ || !encrypted_ || !subscribed_) {
        return false;
    }
    uint16_t conn_handle = conn_handle_;
    // A notification longer than the ATT payload would be cut. The app must request a big MTU.
    if (json.size() + 3 > ble_att_mtu(conn_handle)) {
        ESP_LOGE(TAG, "Command does not fit the MTU (%d)", ble_att_mtu(conn_handle));
        return false;
    }
    struct os_mbuf* om = ble_hs_mbuf_from_flat(json.data(), json.size());
    if (om == nullptr) {
        return false;
    }
    int rc = ble_gatts_notify_custom(conn_handle, cmd_val_handle_, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "Notify failed, rc=%d", rc);
        return false;
    }
    ESP_LOGI(TAG, "Sent command: %s", json.c_str());
    return true;
}

void MusicBridge::OnStateWrite(const uint8_t* data, size_t length) {
    ESP_LOGI(TAG, "State write: %u bytes: %.*s", static_cast<unsigned>(length),
             static_cast<int>(length > 120 ? 120 : length), reinterpret_cast<const char*>(data));
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_ParseWithLength(reinterpret_cast<const char*>(data), length), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get())) {
        ESP_LOGW(TAG, "Ignoring state that is not a JSON object");
        return;
    }
    cJSON* state = cJSON_GetObjectItem(root.get(), "s");
    if (!cJSON_IsNumber(state) || state->valueint < kMusicPaused || state->valueint > kMusicStopped) {
        ESP_LOGW(TAG, "Ignoring state without a valid \"s\"");
        return;
    }
    cJSON* title = cJSON_GetObjectItem(root.get(), "t");
    cJSON* artist = cJSON_GetObjectItem(root.get(), "a");
    cJSON* position = cJSON_GetObjectItem(root.get(), "p");
    cJSON* duration = cJSON_GetObjectItem(root.get(), "d");
    auto clamp_ms = [](cJSON* item) -> int64_t {
        if (!cJSON_IsNumber(item)) {
            return 0;
        }
        return std::clamp<int64_t>(static_cast<int64_t>(item->valuedouble), 0, kMaxMs);
    };
    ShowNowPlaying(ToDisplayText(cJSON_IsString(title) ? title->valuestring : nullptr, kMaxTextChars),
                   ToDisplayText(cJSON_IsString(artist) ? artist->valuestring : nullptr, kMaxTextChars),
                   clamp_ms(position), clamp_ms(duration), state->valueint);
}

void MusicBridge::ShowNowPlaying(const std::string& title, const std::string& artist,
                                 int64_t position_ms, int64_t duration_ms, int state) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        title_ = title;
        artist_ = artist;
        position_ms_ = position_ms;
        duration_ms_ = duration_ms;
        state_ = state;
    }
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetNowPlaying(title.c_str(), artist.c_str(), position_ms, duration_ms, state);
    }
}

void MusicBridge::RegisterTools() {
    auto& server = McpServer::GetInstance();

    server.AddTool("self.music.play",
                   "Play a song, artist, album or playlist on the user's phone through the Sofia "
                   "Music app. Use it when the user asks to play, put on or listen to music. Pass "
                   "what the user asked for as `query`, for example a song title and its artist. "
                   "The player shows up on the device screen by itself.",
                   PropertyList({Property("query", kPropertyTypeString)}),
                   [](const PropertyList& properties) -> ToolResult {
                       std::string query = TruncateUtf8(properties["query"].value<std::string>(),
                                                        kMaxQueryBytes);
                       if (query.empty()) {
                           return std::unexpected("The query is empty");
                       }
                       return SendToPhone("play", query.c_str(), 0);
                   });

    struct Simple {
        const char* tool;
        const char* command;
        const char* description;
        int delta;
    };
    static const Simple kSimple[] = {
        {"self.music.pause", "pause", "Pause the music playing on the user's phone.", 0},
        {"self.music.resume", "resume", "Resume the music paused on the user's phone.", 0},
        {"self.music.next", "next", "Skip to the next song on the user's phone.", 0},
        {"self.music.previous", "prev", "Go back to the previous song on the user's phone.", 0},
        {"self.music.volume_up", "vol",
         "Raise the volume of the music on the user's phone. This is the phone's volume, not the "
         "device speaker.",
         1},
        {"self.music.volume_down", "vol",
         "Lower the volume of the music on the user's phone. This is the phone's volume, not the "
         "device speaker.",
         -1},
    };
    for (const auto& entry : kSimple) {
        server.AddTool(entry.tool, entry.description, PropertyList(),
                       [&entry](const PropertyList&) -> ToolResult {
                           return SendToPhone(entry.command, nullptr, entry.delta);
                       });
    }

    server.AddTool("self.music.get_now_playing",
                   "Get the song currently playing on the user's phone: title, artist, position "
                   "and duration in seconds, and state (playing, paused or stopped). Use it to "
                   "answer what is playing.",
                   PropertyList(), [this](const PropertyList&) -> ToolResult {
                       std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateObject(),
                                                                            cJSON_Delete);
                       {
                           std::lock_guard<std::mutex> lock(mutex_);
                           static const char* const kStates[] = {"paused", "playing", "stopped"};
                           cJSON_AddStringToObject(root.get(), "state", kStates[state_]);
                           cJSON_AddStringToObject(root.get(), "title", title_.c_str());
                           cJSON_AddStringToObject(root.get(), "artist", artist_.c_str());
                           cJSON_AddNumberToObject(root.get(), "position_s", position_ms_ / 1000);
                           cJSON_AddNumberToObject(root.get(), "duration_s", duration_ms_ / 1000);
                       }
                       cJSON_AddBoolToObject(root.get(), "phone_connected", IsConnected());
                       char* text = cJSON_PrintUnformatted(root.get());
                       if (text == nullptr) {
                           return std::unexpected("Out of memory");
                       }
                       std::string json(text);
                       cJSON_free(text);
                       return json;
                   });
}

#endif  // CONFIG_MUSIC_BRIDGE_BLE
