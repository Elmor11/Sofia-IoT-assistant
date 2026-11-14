#ifndef OLED_EYES_H
#define OLED_EYES_H

#include <sdkconfig.h>

#if CONFIG_OLED_REACTIVE_EYES

#include <lvgl.h>

// Pair of animated eyes for monochrome OLED displays. The eyes follow the mood names the server
// sends through Display::SetEmotion(), blink on their own and glance around while idle.
// Left alone in neutral they daydream, play with a ball, hum and finally fall asleep.
// They fill the whole screen, or shrink with an animation into a compact box so text fits beside
// them. Every method must be called with the LVGL lock held.
class OledEyes {
public:
    struct Look;  // a mood or idle-activity pose, defined in oled_eyes.cc

    // `parent` is the screen; the compact box is given in screen coordinates.
    OledEyes(lv_obj_t* parent, int compact_x, int compact_y, int compact_w, int compact_h);
    ~OledEyes();

    // True when `mood` is one of the names drawn by the eyes. Other names (alert icons) are not.
    static bool IsMood(const char* mood);

    void SetMood(const char* mood);
    void Show(bool visible);
    bool IsVisible() const { return visible_; }

    // Animates between the full screen (false) and the compact box (true).
    void SetCompact(bool compact) { compact_target_ = compact ? 1.0f : 0.0f; }
    // Moves the compact box (screen coordinates); the eyes glide to the new place.
    void SetCompactBox(int x, int y, int w, int h);

    // What is going on around the eyes. While a scene is set (and the mood is neutral) the eyes
    // act it out in the compact box: dance to the music, read and hammer while studying,
    // exercise and hum during a break.
    enum class Scene { kNone, kMusic, kFocus, kBreak };
    void SetScene(Scene scene) { scene_ = scene; }
    // 0 when the eyes fill the screen, 1 when they sit in the compact box.
    float CompactProgress() const { return compact_; }

private:
    struct Pose {
        float eye_w = 1.0f;       // eye width multiplier
        float eye_h = 1.0f;       // eye height multiplier
        float look_x = 0.0f;      // gaze offset, in 1/32 of the widget width
        float look_y = 0.0f;      // gaze offset, in 1/32 of the widget height
        float lid_inner = 0.0f;   // top lid coverage at the inner corner, fraction of eye height
        float lid_outer = 0.0f;   // top lid coverage at the outer corner, fraction of eye height
        float lid_bottom = 0.0f;  // bottom lid coverage, turns the eye into an arch
        float pupil = 0.0f;       // pupil diameter, fraction of eye width
        float left_scale = 1.0f;
        float right_scale = 1.0f;
        float tear = 0.0f;        // 1 draws a falling tear
        float heart = 0.0f;       // 1 draws hearts instead of eyes
    };

    lv_obj_t* obj_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    float box_[4] = {0, 0, 0, 0};         // compact box x1, y1, x2, y2, relative to the object
    float box_target_[4] = {0, 0, 0, 0};  // where box_ is gliding to
    float compact_ = 0.0f;
    float compact_target_ = 0.0f;
    bool visible_ = true;
    Pose current_;
    Pose target_;
    Pose velocity_ = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // springy motion toward the goal
    float glance_tilt_ = 0.0f;  // one eye a bit bigger than the other while glancing
    int breath_px_ = 0;         // idle breathing offset, whole pixels
    float wiggle_ = 0.0f;  // vertical shake amplitude, in 1/32 of the widget height
    bool glance_ = true;   // random glances allowed for this mood
    bool blink_ = true;    // random blinks allowed for this mood

    uint32_t now_ms_ = 0;
    uint32_t next_blink_ms_ = 0;
    uint32_t blink_start_ms_ = 0;
    uint32_t next_glance_ms_ = 0;
    float glance_x_ = 0.0f;
    float glance_y_ = 0.0f;
    float blink_factor_ = 1.0f;
    float tear_phase_ = 0.0f;
    float wiggle_offset_ = 0.0f;

    // Idle life: while the eyes sit alone in neutral for a while they daydream, play or hum, and
    // after a long time they fall asleep. Any mood from outside wakes them up.
    enum class Activity {
        kNone,
        kDaydream,
        kPlay,
        kHum,
        kSleep,
        kRead,
        kHammer,
        kExercise,
        kDance,
    };
    Scene scene_ = Scene::kNone;
    const Look* mood_ = nullptr;  // mood set from outside, restored when an activity ends
    Activity activity_ = Activity::kNone;
    uint32_t idle_since_ms_ = 0;
    uint32_t activity_start_ms_ = 0;
    uint32_t activity_end_ms_ = 0;
    uint32_t next_activity_ms_ = 0;
    uint32_t decor_key_ = 0;  // changes when the activity decoration needs a redraw
    int variant_ = 0;         // per-activity random choice (daydream picture)
    float ball_x_ = 0.5f, ball_y_ = 0.5f, ball_vx_ = 0.0f, ball_vy_ = 0.0f;  // fractions of area

    static void OnTimer(lv_timer_t* timer);
    static void OnDraw(lv_event_t* event);
    void Tick();
    void ApplyLook(const Look& look);
    bool UpdateActivity();  // returns true when the decoration changed
    void StartActivity(Activity activity);
    void StopActivity();
    bool FitsScene(Activity activity) const;
    Activity NextSceneActivity() const;
    void Draw(lv_layer_t* layer, const lv_area_t& area, lv_color_t fg, lv_color_t bg) const;
    void DrawActivity(lv_layer_t* layer, const lv_area_t& area, lv_color_t fg,
                      lv_color_t bg) const;
    void DrawEye(lv_layer_t* layer, int cx, int cy, int w, int h, bool left, lv_color_t fg,
                 lv_color_t bg, int unit) const;
};

#endif  // CONFIG_OLED_REACTIVE_EYES

#endif  // OLED_EYES_H
