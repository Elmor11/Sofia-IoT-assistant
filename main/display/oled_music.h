#ifndef OLED_MUSIC_H
#define OLED_MUSIC_H

#include <sdkconfig.h>

#if CONFIG_OLED_REACTIVE_EYES

#include <lvgl.h>

#include <cstdint>
#include <string>

// Mini music player for monochrome OLED displays: scrolling "title - artist", a progress bar,
// "elapsed / total" and pause / next icons. It shows what the phone app reports through
// Display::SetNowPlaying(); the position keeps moving on its own between updates.
// Every method must be called with the LVGL lock held.
class OledMusicPanel {
public:
    // The panel occupies the given box in screen coordinates.
    OledMusicPanel(lv_obj_t* parent, int x, int y, int w, int h);
    ~OledMusicPanel();

    // `state` is a MusicPlaybackState (see display.h).
    void SetTrack(const char* title, const char* artist, int64_t position_ms, int64_t duration_ms,
                  int state);
    void Show(bool visible);
    bool IsShown() const;
    // False once the phone reported that nothing is playing.
    bool Active() const;
    bool Playing() const;
    // Advances the progress bar and the time text. Call periodically while shown.
    void Tick();

private:
    lv_obj_t* obj_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* time_ = nullptr;  // nullptr on displays too short for a time row
    int w_ = 0;
    int bar_y_ = 0;
    int icons_y_ = -1;  // -1 when there is no room for the icon row

    std::string text_;
    int state_;
    int64_t position_ms_ = 0;
    int64_t duration_ms_ = 0;
    uint32_t stamp_ms_ = 0;  // lv_tick_get() when position_ms_ was reported
    int last_fill_ = -1;
    int64_t last_second_ = -1;

    int64_t CurrentPositionMs() const;
    void UpdateTime();
    static void OnDraw(lv_event_t* event);
};

#endif  // CONFIG_OLED_REACTIVE_EYES

#endif  // OLED_MUSIC_H
