#ifndef MUSIC_BRIDGE_H
#define MUSIC_BRIDGE_H

#include <sdkconfig.h>

#if CONFIG_MUSIC_BRIDGE_BLE

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// Link with the phone's companion app over BLE (see docs/music-bridge.md for the protocol).
//
// The device is the GATT server and the phone connects to it:
//   - Cmd characteristic (notify): voice commands for the phone, e.g. {"c":"play","q":"..."}.
//   - State characteristic (write): what is playing on the phone; shown on the OLED player.
// Voice commands reach it as MCP tools named self.music.*.
class MusicBridge {
public:
    static MusicBridge& GetInstance();

    // Registers the MCP tools and starts advertising. Call once, after NVS is initialized.
    void Start();

    bool IsConnected() const { return connected_.load(); }
    bool IsProvisioning() const { return provisioning_.load(); }
    // Sends a command line to the phone. False when the phone is not connected and ready.
    bool SendCommand(const std::string& json);

    // Used by the BLE callbacks.
    void OnConnected(uint16_t conn_handle);
    void OnDisconnected();
    void OnEncrypted(bool ok);
    void OnSubscribe(bool notify);
    void OnStateWrite(const uint8_t* data, size_t length);
    void Advertise();
    void OnSync();
    uint16_t* CmdValueHandle() { return &cmd_val_handle_; }
    uint16_t* ProvValueHandle() { return &prov_val_handle_; }
    void OnProvisionSubscribe(bool notify) { prov_subscribed_ = notify; }
    void OnProvisionWrite(const uint8_t* data, size_t length);

    // Shows the pairing QR code and accepts Wi-Fi credentials from the phone app. False when the
    // display cannot show a QR code (use the hotspot instead). Needs Start() first.
    bool BeginWifiProvisioning();

private:
    MusicBridge() = default;

    void RegisterTools();
    void LoadIdentity();
    bool DecryptCredentials(const uint8_t* data, size_t length, std::string& json);
    void NotifyProvisioning(const char* json);
    void ShowNowPlaying(const std::string& title, const std::string& artist, int64_t position_ms,
                        int64_t duration_ms, int state);

    std::atomic<bool> connected_{false};
    std::atomic<bool> ready_{false};  // encrypted and the phone subscribed to notifications
    std::atomic<bool> encrypted_{false};
    std::atomic<bool> subscribed_{false};
    std::atomic<uint16_t> conn_handle_{0};
    uint16_t cmd_val_handle_ = 0;
    uint8_t own_addr_type_ = 0;
    uint16_t prov_val_handle_ = 0;
    std::atomic<bool> prov_subscribed_{false};
    std::atomic<bool> provisioning_{false};
    std::atomic<int> bad_attempts_{0};
    std::string device_name_;
    uint8_t secret_[10] = {};
    std::string secret_hex_;  // uppercase, 20 characters

    std::mutex mutex_;
    std::string title_;
    std::string artist_;
    int64_t position_ms_ = 0;
    int64_t duration_ms_ = 0;
    int state_ = 2;
};

#endif  // CONFIG_MUSIC_BRIDGE_BLE

#endif  // MUSIC_BRIDGE_H
