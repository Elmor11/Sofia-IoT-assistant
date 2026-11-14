#ifndef OLED_POMODORO_H
#define OLED_POMODORO_H

#include <sdkconfig.h>

#if CONFIG_OLED_REACTIVE_EYES

#include <lvgl.h>

#include <cstdint>

// Pomodoro gauge for monochrome OLED displays: a rounded half-circle progress arc that fills from
// left to right with the time spent in the phase, the elapsed time centered below it, the round
// at the bottom left and the phase at the bottom right. The space inside the arc is left for the
// eyes (see InnerBox()). The time keeps moving on its own between updates. Every method must be
// called with the LVGL lock held.
class OledPomodoroPanel {
public:
    // The panel occupies the given box in screen coordinates.
    OledPomodoroPanel(lv_obj_t* parent, int x, int y, int w, int h);
    ~OledPomodoroPanel();

    // `phase` is a PomodoroPhase (see display.h).
    void SetState(int phase, int64_t elapsed_ms, int64_t total_ms, bool running, int round,
                  int rounds);
    void Show(bool visible);
    bool IsShown() const;
    bool Active() const;
    int Phase() const { return phase_; }
    bool Running() const { return running_; }
    // Free box inside the arc, in screen coordinates (for the eyes).
    lv_area_t InnerBox() const { return inner_; }
    // Advances the arc and the time text. Call periodically while shown.
    void Tick();

private:
    lv_obj_t* obj_ = nullptr;
    lv_obj_t* time_ = nullptr;
    lv_obj_t* round_label_ = nullptr;
    lv_obj_t* phase_label_ = nullptr;
    int cx_ = 0;  // arc center, relative to the panel
    int cy_ = 0;
    int radius_ = 0;
    lv_area_t inner_ = {0, 0, 0, 0};

    int phase_ = 0;
    bool running_ = false;
    int64_t elapsed_ms_ = 0;
    int64_t total_ms_ = 0;
    uint32_t stamp_ms_ = 0;  // lv_tick_get() when elapsed_ms_ was reported
    int last_step_ = -1;
    int64_t last_second_ = -1;

    int64_t CurrentElapsedMs() const;
    float Fraction() const;
    void UpdateTime();
    static void OnDraw(lv_event_t* event);
};

#endif  // CONFIG_OLED_REACTIVE_EYES

#endif  // OLED_POMODORO_H
