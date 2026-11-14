#ifndef DISPLAY_H
#define DISPLAY_H

#include "emoji_collection.h"
#include "text_glyph.h"

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#define HAVE_LVGL 1
#include <lvgl.h>
#endif

#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class LvglFont;
class LvglImage;

// Playback state reported by the phone's music app.
enum MusicPlaybackState { kMusicPaused = 0, kMusicPlaying = 1, kMusicStopped = 2 };

// Phase of the pomodoro study timer.
enum PomodoroPhase {
    kPomodoroOff = 0,
    kPomodoroFocus = 1,
    kPomodoroShortBreak = 2,
    kPomodoroLongBreak = 3,
};

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }
    virtual std::shared_ptr<LvglFont> GetTextFont() const { return nullptr; }

private:
    std::string name_;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000);
    // Returns true when the requested emotion was accepted and set in the rendering
    // system. False means invalid input, a missing resource, an uninitialised display, or
    // an unsupported operation. For asynchronous displays, true does not promise that the
    // physical refresh has already completed. Callers are free to ignore the result.
    virtual bool SetEmotion(const char* emotion);
    virtual void SetChatMessage(const char* role, const char* content);
    // Remove a matching message without clearing newer content, when supported.
    virtual void RemoveChatMessage(const char* role, const char* content) {}
    virtual void ClearChatMessages();
    // Track playing on the phone. position_ms is the position at the time of the call; displays
    // that draw a player advance it on their own while the state is kMusicPlaying. Ignored by
    // displays without a player.
    virtual void SetNowPlaying(const char* title, const char* artist, int64_t position_ms,
                               int64_t duration_ms, int state) {}
    // Pomodoro timer: `phase` is a PomodoroPhase (kPomodoroOff hides it). `elapsed_ms` is the
    // time spent in the phase at the time of the call; displays advance it on their own while
    // `running`. `round` / `rounds` count focus phases. Ignored by displays without a timer.
    virtual void SetPomodoro(int phase, int64_t elapsed_ms, int64_t total_ms, bool running,
                             int round, int rounds) {}
    // Shows a full-screen QR code with a short caption (for Wi-Fi provisioning with the phone
    // app). Returns false when the display cannot show a scannable QR code.
    virtual bool SetPairingQr(const char* payload, const char* caption) { return false; }
    // Removes the QR code shown by SetPairingQr, if any.
    virtual void HidePairingQr() {}
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) { return false; }
    virtual void ClearTextGlyphs() {}
    virtual void SetEmojiCollection(std::shared_ptr<EmojiCollection>) {}
    virtual void SetupUI() { setup_ui_called_ = true; }
    virtual bool IsMonochrome() const { return false; }
    virtual bool SupportsGuiOperations() const { return false; }
    virtual void SetHideSubtitle(bool hide) { (void)hide; }
    virtual bool InsertAnimDialog(const char* name, uint32_t duration_ms) {
        (void)name;
        (void)duration_ms;
        return false;
    }
    virtual bool MountAssets(const char* partition_label) {
        (void)partition_label;
        return false;
    }
    virtual void UnmountAssets() {}
    virtual bool GetAssetData(const std::string& name, const uint8_t*& data, size_t& size) {
        (void)name;
        (void)data;
        (void)size;
        return false;
    }
    virtual void LoadAssets() {}
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image);
    virtual bool SetTextFont(std::shared_ptr<LvglFont> text_font) {
        (void)text_font;
        return false;
    }
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80) {
        (void)jpeg_data;
        (void)quality;
        return false;
    }

    inline int width() const { return width_; }
    inline int height() const { return height_; }
    inline bool IsSetupUICalled() const { return setup_ui_called_; }

protected:
    int width_ = 0;
    int height_ = 0;
    bool setup_ui_called_ = false;  // Track if SetupUI() has been called

    Theme* current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

class DisplayLockGuard {
public:
    explicit DisplayLockGuard(Display* display)
        : display_(display), locked_(display_->Lock(30000)) {
        if (!locked_) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        if (locked_) {
            display_->Unlock();
        }
    }

    DisplayLockGuard(const DisplayLockGuard&) = delete;
    DisplayLockGuard& operator=(const DisplayLockGuard&) = delete;

    explicit operator bool() const { return locked_; }

private:
    Display* display_;
    bool locked_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override { return true; }
    virtual void Unlock() override {}
};

#endif
