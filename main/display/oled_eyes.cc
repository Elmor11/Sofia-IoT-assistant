#include "oled_eyes.h"

#if CONFIG_OLED_REACTIVE_EYES

#include <algorithm>
#include <cmath>
#include <cstring>

#include <esp_random.h>

struct OledEyes::Look {
    const char* name;
    float eye_w, eye_h, look_x, look_y;
    float lid_inner, lid_outer, lid_bottom, pupil;
    float left_scale, right_scale, tear, heart;
    float wiggle;
    bool glance;
    bool blink;
};

namespace {

using Mood = OledEyes::Look;

constexpr uint32_t kTickMs = 50;
constexpr uint32_t kBlinkMs = 160;
constexpr float kBlinkDepth = 0.92f;
constexpr float kFollow = 0.4f;  // fraction of the remaining distance covered each tick
// Spring toward the goal: a little overshoot makes the eyes feel alive.
constexpr float kSpring = 0.28f;
constexpr float kDamping = 0.60f;
constexpr int kMinPropWidth = 64;  // props (book, hammer, weights) only in a wide box

// Idle timing: first activity after kFirstActivityMs alone in neutral, then one every
// kActivityGapMs (+ random), and asleep after kSleepAfterMs.
constexpr uint32_t kFirstActivityMs = 40 * 1000;
constexpr uint32_t kActivityGapMs = 20 * 1000;
constexpr uint32_t kSleepAfterMs = 2 * 60 * 1000;

// Columns: w, h, look x/y, lid inner/outer/bottom, pupil, left/right scale, tear, heart, wiggle,
// glance, blink. Inner is the corner nearest the nose: angry eyes cover more of it, sad eyes more
// of the outer corner.
const Mood kMoods[] = {
    {"neutral", 1.0f, 1.0f, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f, 0, 0, 0, true, true},
    {"happy", 1.0f, 0.95f, 0, -0.5f, 0, 0, 0.50f, 0, 1.0f, 1.0f, 0, 0, 0, true, false},
    {"laughing", 1.0f, 0.95f, 0, 0, 0, 0, 0.55f, 0, 1.0f, 1.0f, 0, 0, 1.2f, false, false},
    {"funny", 1.0f, 0.95f, 0, 0, 0, 0, 0.50f, 0, 1.0f, 1.0f, 0, 0, 0.8f, false, false},
    {"loving", 1.0f, 1.0f, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f, 0, 1, 0.5f, false, false},
    {"embarrassed", 1.0f, 0.9f, -2, 2, 0, 0, 0.40f, 0, 1.0f, 1.0f, 0, 0, 0, false, true},
    {"confident", 1.0f, 0.95f, 0, 0, 0.25f, 0.25f, 0.20f, 0, 1.0f, 1.0f, 0, 0, 0, true, true},
    {"delicious", 1.0f, 0.95f, 0, -1, 0, 0, 0.45f, 0, 1.0f, 1.0f, 0, 0, 0, false, false},
    {"sad", 0.95f, 1.0f, 0, 2, 0.05f, 0.55f, 0, 0, 1.0f, 1.0f, 0, 0, 0, false, true},
    {"crying", 0.95f, 1.0f, 0, 2, 0.05f, 0.55f, 0, 0, 1.0f, 1.0f, 1, 0, 0.4f, false, false},
    {"sleepy", 1.0f, 0.9f, 0, 2, 0.50f, 0.50f, 0, 0, 1.0f, 1.0f, 0, 0, 0, false, true},
    {"silly", 1.0f, 1.0f, 2, 0, 0, 0, 0, 0.40f, 1.2f, 0.7f, 0, 0, 0, false, true},
    {"angry", 1.0f, 1.0f, 0, 0, 0.60f, 0.05f, 0, 0, 1.0f, 1.0f, 0, 0, 0.3f, false, true},
    {"surprised", 1.15f, 1.25f, 0, 0, 0, 0, 0, 0.35f, 1.0f, 1.0f, 0, 0, 0, false, true},
    {"shocked", 1.2f, 1.3f, 0, 0, 0, 0, 0, 0.25f, 1.0f, 1.0f, 0, 0, 0.8f, false, false},
    {"thinking", 1.0f, 1.0f, 3, -3, 0, 0.15f, 0, 0, 1.0f, 0.85f, 0, 0, 0, false, true},
    {"winking", 1.0f, 0.95f, 0, 0, 0, 0, 0.35f, 0, 1.0f, 0.10f, 0, 0, 0, false, false},
    {"relaxed", 1.0f, 0.95f, 0, 0, 0.30f, 0.30f, 0.25f, 0, 1.0f, 1.0f, 0, 0, 0, true, true},
    {"confused", 1.0f, 1.0f, 0, 0, 0, 0.25f, 0, 0, 0.85f, 1.1f, 0, 0, 0, true, true},
    {"cool", 1.0f, 0.95f, 0, 1, 0.45f, 0.45f, 0, 0, 1.0f, 1.0f, 0, 0, 0, false, true},
    {"kissy", 1.0f, 1.0f, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f, 0, 1, 0, false, false},
};

// Idle activities. Not moods: the server cannot ask for them. The eyes move aside to leave room
// for the drawing on the right.
const Mood kDaydreamLook = {"", 0.8f, 0.8f, -4, -2, 0, 0.15f, 0, 0,
                            1.0f, 1.0f, 0, 0, 0, false, true};
const Mood kPlayLook = {"", 0.8f, 0.8f, 0, 0, 0, 0, 0, 0.35f, 1.0f, 1.0f, 0, 0, 0, false, true};
const Mood kHumLook = {"", 0.9f, 0.9f, -3, -0.5f, 0, 0, 0.50f, 0,
                       1.0f, 1.0f, 0, 0, 0, false, false};
const Mood kSleepLook = {"", 0.9f, 0.10f, -3, 3, 0, 0, 0, 0, 1.0f, 1.0f, 0, 0, 0, false, false};
const Mood kDrowsyLook = {"", 0.9f, 0.95f, -3, 1, 0.40f, 0.40f, 0, 0,
                          1.0f, 1.0f, 0, 0, 0, false, false};

// Scene activities. The eyes are smaller so the props fit next to them.
const Mood kReadLook = {"", 0.70f, 0.62f, 0, -3, 0.25f, 0.25f, 0, 0,
                        1.0f, 1.0f, 0, 0, 0, false, true};
const Mood kHammerLook = {"", 0.75f, 0.75f, -3, 0, 0.30f, 0.05f, 0, 0,
                          1.0f, 1.0f, 0, 0, 0, false, true};
const Mood kExerciseLook = {"", 0.78f, 0.75f, 0, 0, 0.20f, 0.20f, 0, 0,
                            1.0f, 1.0f, 0, 0, 0, false, false};
const Mood kDanceLook = {"", 0.95f, 0.95f, 0, 0, 0, 0, 0.45f, 0,
                         1.0f, 1.0f, 0, 0, 0, false, false};

constexpr uint32_t kReadLineMs = 2400;  // eyes sweep one line of text
constexpr uint32_t kReadPageMs = 3 * kReadLineMs;
constexpr uint32_t kFlipMs = 500;     // page turn
constexpr uint32_t kHammerMs = 1000;  // one lift-and-strike
constexpr uint32_t kRepMs = 1800;     // one dumbbell curl
constexpr uint32_t kBeatMs = 500;     // 120 BPM

// Hammer angle (degrees, clockwise from 3 o'clock) during one swing: lift, hold, strike.
float HammerAngle(uint32_t phase) {
    if (phase < 450) {
        float t = phase / 450.0f;
        return 180.0f + 55.0f * (1.0f - (1.0f - t) * (1.0f - t));
    }
    if (phase < 800) {
        return 235.0f + 3.0f * std::sin((phase - 450) * 0.03f);
    }
    float t = (phase - 800) / 200.0f;
    return 235.0f - 55.0f * t * t;
}

// Falling asleep: heavy lids, a nod, a short jolt awake, then the eyes close. Asleep after that.
constexpr uint32_t kAsleepMs = 5000;
constexpr uint32_t kBreathMs = 4000;  // one slow breath while asleep

const Mood* FindMood(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (const auto& mood : kMoods) {
        if (strcmp(mood.name, name) == 0) {
            return &mood;
        }
    }
    return nullptr;
}

uint32_t RandomBelow(uint32_t limit) { return esp_random() % limit; }

float RandomSigned() { return (static_cast<float>(RandomBelow(2001)) - 1000.0f) / 1000.0f; }

void FillRect(lv_layer_t* layer, int x1, int y1, int x2, int y2, int radius, lv_color_t color) {
    if (x2 < x1 || y2 < y1) {
        return;
    }
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    lv_area_t area = {x1, y1, x2, y2};
    lv_draw_rect(layer, &dsc, &area);
}

void FillTriangle(lv_layer_t* layer, int x0, int y0, int x1, int y1, int x2, int y2,
                  lv_color_t color) {
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = LV_OPA_COVER;
    dsc.p[0].x = x0;
    dsc.p[0].y = y0;
    dsc.p[1].x = x1;
    dsc.p[1].y = y1;
    dsc.p[2].x = x2;
    dsc.p[2].y = y2;
    lv_draw_triangle(layer, &dsc);
}

void FillHeart(lv_layer_t* layer, int cx, int cy, int size, lv_color_t color) {
    int r = std::max(2, size / 4);
    FillRect(layer, cx - 2 * r, cy - 2 * r, cx, cy, r, color);
    FillRect(layer, cx, cy - 2 * r, cx + 2 * r, cy, r, color);
    FillTriangle(layer, cx - 2 * r, cy - r, cx + 2 * r, cy - r, cx, cy + 2 * r, color);
}

void FillLine(lv_layer_t* layer, int x1, int y1, int x2, int y2, int width, lv_color_t color) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.opa = LV_OPA_COVER;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

void FillCircle(lv_layer_t* layer, int cx, int cy, int r, lv_color_t color) {
    FillRect(layer, cx - r, cy - r, cx + r, cy + r, r, color);
}

void FillStar(lv_layer_t* layer, int cx, int cy, int r, lv_color_t color) {
    int h = r / 2;
    FillTriangle(layer, cx, cy - r, cx - r, cy + h, cx + r, cy + h, color);
    FillTriangle(layer, cx, cy + r, cx - r, cy - h, cx + r, cy - h, color);
}

// A "Z" of the given size with its top-left corner at (x, y): two bars and a slanted stroke.
void FillZ(lv_layer_t* layer, int x, int y, int size, lv_color_t color) {
    int t = std::max(1, size / 5);
    FillRect(layer, x, y, x + size, y + t - 1, 0, color);
    FillRect(layer, x, y + size - t + 1, x + size, y + size, 0, color);
    FillTriangle(layer, x + size - t, y + t, x + size, y + t, x, y + size - t + 1, color);
    FillTriangle(layer, x + size, y + t, x + t, y + size - t + 1, x, y + size - t + 1, color);
}

// A music note: head with a stem and a small flag.
void FillNote(lv_layer_t* layer, int x, int y, int size, lv_color_t color) {
    int r = std::max(2, size / 4);
    FillCircle(layer, x, y, r, color);
    int stem_x = x + r - 1;
    FillRect(layer, stem_x, y - size, stem_x + 1, y, 0, color);
    FillRect(layer, stem_x, y - size, stem_x + r + 1, y - size + std::max(1, r / 2), 0, color);
}

}  // namespace

OledEyes::OledEyes(lv_obj_t* parent, int compact_x, int compact_y, int compact_w,
                   int compact_h) {
    SetCompactBox(compact_x, compact_y, compact_w, compact_h);
    std::copy(box_target_, box_target_ + 4, box_);
    obj_ = lv_obj_create(parent);
    lv_obj_remove_style_all(obj_);
    lv_obj_set_size(obj_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(obj_, 0, 0);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj_, OnDraw, LV_EVENT_DRAW_MAIN, this);

    SetMood("neutral");
    current_ = target_;

    now_ms_ = lv_tick_get();
    idle_since_ms_ = now_ms_;
    next_blink_ms_ = now_ms_ + 2000 + RandomBelow(3000);
    next_glance_ms_ = now_ms_ + 3000 + RandomBelow(3000);
    timer_ = lv_timer_create(OnTimer, kTickMs, this);
}

OledEyes::~OledEyes() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
    }
    if (obj_ != nullptr) {
        lv_obj_delete(obj_);
    }
}

bool OledEyes::IsMood(const char* mood) { return FindMood(mood) != nullptr; }

void OledEyes::SetCompactBox(int x, int y, int w, int h) {
    box_target_[0] = static_cast<float>(x);
    box_target_[1] = static_cast<float>(y);
    box_target_[2] = static_cast<float>(x + w - 1);
    box_target_[3] = static_cast<float>(y + h - 1);
}

bool OledEyes::FitsScene(Activity activity) const {
    switch (scene_) {
        case Scene::kMusic:
            return activity == Activity::kDance;
        case Scene::kFocus:
            return activity == Activity::kRead || activity == Activity::kHammer;
        case Scene::kBreak:
            return activity == Activity::kExercise || activity == Activity::kHum;
        case Scene::kNone:
            break;
    }
    return false;
}

OledEyes::Activity OledEyes::NextSceneActivity() const {
    switch (scene_) {
        case Scene::kMusic:
            return Activity::kDance;
        case Scene::kFocus:
            return activity_ == Activity::kRead ? Activity::kHammer : Activity::kRead;
        case Scene::kBreak:
            return activity_ == Activity::kExercise ? Activity::kHum : Activity::kExercise;
        case Scene::kNone:
            break;
    }
    return Activity::kNone;
}

void OledEyes::SetMood(const char* mood) {
    const Mood* found = FindMood(mood);
    if (found == nullptr) {
        found = &kMoods[0];
    }
    // Any mood from outside means something is happening: wake up and restart the idle count.
    mood_ = found;
    activity_ = Activity::kNone;
    idle_since_ms_ = lv_tick_get();
    next_activity_ms_ = 0;
    ApplyLook(*found);
    if (timer_ != nullptr) {
        lv_timer_resume(timer_);
        lv_obj_invalidate(obj_);
    }
}

void OledEyes::ApplyLook(const Look& look) {
    target_.eye_w = look.eye_w;
    target_.eye_h = look.eye_h;
    target_.look_x = look.look_x;
    target_.look_y = look.look_y;
    target_.lid_inner = look.lid_inner;
    target_.lid_outer = look.lid_outer;
    target_.lid_bottom = look.lid_bottom;
    target_.pupil = look.pupil;
    target_.left_scale = look.left_scale;
    target_.right_scale = look.right_scale;
    target_.tear = look.tear;
    target_.heart = look.heart;
    wiggle_ = look.wiggle;
    glance_ = look.glance;
    blink_ = look.blink;
    glance_x_ = 0.0f;
    glance_y_ = 0.0f;
}

void OledEyes::StartActivity(Activity activity) {
    activity_ = activity;
    activity_start_ms_ = now_ms_;
    decor_key_ = 0;
    switch (activity) {
        case Activity::kDaydream:
            ApplyLook(kDaydreamLook);
            variant_ = static_cast<int>(RandomBelow(3));
            activity_end_ms_ = now_ms_ + 7000 + RandomBelow(3000);
            break;
        case Activity::kPlay:
            ApplyLook(kPlayLook);
            ball_x_ = 0.1f;
            ball_y_ = 0.2f + 0.6f * RandomBelow(100) / 100.0f;
            ball_vx_ = 0.022f;
            ball_vy_ = RandomBelow(2) ? 0.035f : -0.035f;
            activity_end_ms_ = now_ms_ + 8000 + RandomBelow(4000);
            break;
        case Activity::kHum:
            ApplyLook(kHumLook);
            activity_end_ms_ = now_ms_ + 5000 + RandomBelow(2000);
            break;
        case Activity::kSleep:
            ApplyLook(kDrowsyLook);
            activity_end_ms_ = 0;  // until something happens
            break;
        case Activity::kRead:
            ApplyLook(kReadLook);
            activity_end_ms_ = now_ms_ + 20000 + RandomBelow(10000);
            break;
        case Activity::kHammer:
            ApplyLook(kHammerLook);
            activity_end_ms_ = now_ms_ + 8 * kHammerMs + RandomBelow(4) * kHammerMs;
            break;
        case Activity::kExercise:
            ApplyLook(kExerciseLook);
            activity_end_ms_ = now_ms_ + 6 * kRepMs;
            break;
        case Activity::kDance:
            ApplyLook(kDanceLook);
            activity_end_ms_ = 0;  // while the music plays
            break;
        case Activity::kNone:
            break;
    }
}

void OledEyes::StopActivity() {
    if (activity_ == Activity::kNone) {
        return;
    }
    activity_ = Activity::kNone;
    ApplyLook(mood_ != nullptr ? *mood_ : kMoods[0]);
    next_activity_ms_ = now_ms_ + kActivityGapMs + RandomBelow(kActivityGapMs);
    lv_obj_invalidate(obj_);
}

bool OledEyes::UpdateActivity() {
    const bool neutral = visible_ && mood_ == &kMoods[0];
    const bool ended =
        activity_end_ms_ != 0 && static_cast<int32_t>(now_ms_ - activity_end_ms_) >= 0;

    if (scene_ != Scene::kNone && neutral && compact_target_ == 1.0f) {
        // A scene: act it out, one activity after another. No idle sleep meanwhile.
        idle_since_ms_ = now_ms_;
        next_activity_ms_ = 0;
        if (!FitsScene(activity_) || ended) {
            StartActivity(NextSceneActivity());
            return true;
        }
    } else {
        // Idle activities only happen when the eyes are alone on screen in neutral.
        bool scene_only = activity_ == Activity::kRead || activity_ == Activity::kHammer ||
                          activity_ == Activity::kExercise || activity_ == Activity::kDance;
        bool alone = neutral && compact_target_ == 0.0f && compact_ == 0.0f;
        if (!alone || scene_only) {
            StopActivity();
            idle_since_ms_ = now_ms_;
            next_activity_ms_ = 0;
            return false;
        }

        uint32_t idle_ms = now_ms_ - idle_since_ms_;
        if (activity_ != Activity::kSleep && idle_ms >= kSleepAfterMs) {
            StartActivity(Activity::kSleep);
            return true;
        }
        if (activity_ == Activity::kNone) {
            if (idle_ms < kFirstActivityMs ||
                (next_activity_ms_ != 0 &&
                 static_cast<int32_t>(now_ms_ - next_activity_ms_) < 0)) {
                return false;
            }
            static const Activity kChoices[] = {Activity::kDaydream, Activity::kPlay,
                                                Activity::kHum};
            StartActivity(kChoices[RandomBelow(3)]);
            return true;
        }
        if (ended) {
            StopActivity();
            return true;
        }
    }

    uint32_t elapsed = now_ms_ - activity_start_ms_;
    uint32_t key = 0;
    switch (activity_) {
        case Activity::kDaydream:
            // The thought bubbles appear one after another.
            key = 1 + std::min<uint32_t>(elapsed / 500, 4);
            break;
        case Activity::kPlay: {
            ball_x_ += ball_vx_;
            ball_y_ += ball_vy_;
            if (ball_x_ < 0.05f || ball_x_ > 0.95f) {
                ball_vx_ = -ball_vx_;
                ball_x_ = std::clamp(ball_x_, 0.05f, 0.95f);
            }
            if (ball_y_ < 0.08f || ball_y_ > 0.92f) {
                ball_vy_ = -ball_vy_;
                ball_y_ = std::clamp(ball_y_, 0.08f, 0.92f);
            }
            // The eyes follow the ball.
            target_.look_x = (ball_x_ - 0.5f) * 10.0f;
            target_.look_y = (ball_y_ - 0.5f) * 6.0f;
            key = elapsed / kTickMs + 1;
            break;
        }
        case Activity::kHum:
            // Sway from side to side to the tune.
            target_.look_x = kHumLook.look_x + 2.0f * std::sin(elapsed * 0.004f);
            key = elapsed / 100 + 1;
            break;
        case Activity::kSleep: {
            // Falling asleep, step by step; the pose eases between the steps.
            float lids = 0.40f;
            float look_y = 1.0f;
            if (elapsed >= kAsleepMs) {
                ApplyLook(kSleepLook);  // closed; also where the eyes open from on waking
                lids = 0.0f;
                look_y = kSleepLook.look_y;
            } else if (elapsed >= 2800) {
                lids = 0.85f;  // closing for good
                look_y = 2.0f;
            } else if (elapsed >= 2200) {
                lids = 0.25f;  // jolt awake for a moment
                look_y = 0.0f;
            } else if (elapsed >= 1500) {
                lids = 0.60f;  // nod
                look_y = 4.0f;
            }
            if (elapsed < kAsleepMs) {
                target_.lid_inner = lids;
                target_.lid_outer = lids;
            }
            target_.look_y = look_y;
            // Slow breathing and drifting Zs; few redraws keep the I2C bus quiet.
            key = elapsed / 180 + 1;
            break;
        }
        case Activity::kRead: {
            // Eyes sweep each line and jump back; a page turns every few lines.
            uint32_t in_page = elapsed % kReadPageMs;
            bool flipping = elapsed >= kReadPageMs && in_page < kFlipMs;
            if (flipping) {
                target_.look_x = 2.5f - 5.0f * in_page / kFlipMs;
            } else {
                uint32_t in_line = elapsed % kReadLineMs;
                target_.look_x = in_line < 2000 ? -2.5f + 5.0f * in_line / 2000.0f : -2.5f;
            }
            target_.look_y = kReadLook.look_y + 0.6f * ((elapsed / kReadLineMs) % 3);
            key = elapsed / 100 + 1;
            break;
        }
        case Activity::kHammer: {
            // Watch the nail; squint at every strike.
            uint32_t phase = elapsed % kHammerMs;
            bool impact = elapsed >= kHammerMs && phase < 110;
            target_.look_x = kHammerLook.look_x + 1.0f;
            target_.look_y = phase < 800 ? -0.5f : 1.5f;
            target_.eye_h = kHammerLook.eye_h * (impact ? 0.55f : 1.0f);
            target_.eye_w = kHammerLook.eye_w * (impact ? 1.12f : 1.0f);
            key = elapsed / kTickMs + 1;
            break;
        }
        case Activity::kExercise: {
            // Alternate curls; the eye on the lifting side squeezes with the effort.
            uint32_t rep = elapsed / kRepMs;
            float lift = std::sin(3.14159f * (elapsed % kRepMs) / kRepMs);
            bool left = rep % 2 == 0;
            target_.left_scale = left ? 1.0f - 0.2f * lift : 1.0f;
            target_.right_scale = left ? 1.0f : 1.0f - 0.2f * lift;
            target_.lid_inner = kExerciseLook.lid_inner + 0.3f * lift;
            target_.lid_outer = target_.lid_inner;
            target_.look_y = 0.5f - 1.5f * lift;
            key = elapsed / kTickMs + 1;
            break;
        }
        case Activity::kDance: {
            // Bounce on every beat, sway every bar, swap the big eye each beat and pop the eyes
            // open on the last beat of eight.
            uint32_t beat = elapsed / kBeatMs;
            float bounce = std::fabs(std::sin(3.14159f * (elapsed % kBeatMs) / kBeatMs));
            bool pop = beat % 8 == 7;
            target_.look_y = 1.5f - 3.0f * bounce;
            target_.look_x = 2.5f * std::sin(6.2832f * elapsed / (4.0f * kBeatMs));
            target_.eye_h = (pop ? 1.1f : kDanceLook.eye_h) - 0.12f * (1.0f - bounce);
            target_.lid_bottom = pop ? 0.0f : kDanceLook.lid_bottom;
            target_.left_scale = beat % 2 ? 1.1f : 0.9f;
            target_.right_scale = beat % 2 ? 0.9f : 1.1f;
            key = elapsed / kTickMs + 1;
            break;
        }
        case Activity::kNone:
            break;
    }
    if (key != decor_key_) {
        decor_key_ = key;
        return true;
    }
    return false;
}

void OledEyes::Show(bool visible) {
    visible_ = visible;
    if (visible) {
        lv_obj_remove_flag(obj_, LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(timer_);
    } else {
        lv_obj_add_flag(obj_, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(timer_);
    }
}

void OledEyes::OnTimer(lv_timer_t* timer) {
    static_cast<OledEyes*>(lv_timer_get_user_data(timer))->Tick();
}

void OledEyes::Tick() {
    static_assert(sizeof(Pose) == 12 * sizeof(float), "Pose must be a plain float array");
    now_ms_ = lv_tick_get();
    bool dirty = UpdateActivity();

    // Blink: the eyes close and reopen over kBlinkMs.
    float blink_factor = 1.0f;
    if (blink_start_ms_ != 0) {
        float t = static_cast<float>(now_ms_ - blink_start_ms_) / kBlinkMs;
        if (t >= 1.0f) {
            blink_start_ms_ = 0;
            // Now and then a quick double blink.
            next_blink_ms_ = now_ms_ + (RandomBelow(5) == 0 ? 120 : 2500 + RandomBelow(4000));
        } else {
            blink_factor = 1.0f - kBlinkDepth * (1.0f - std::fabs(2.0f * t - 1.0f));
        }
    } else if (blink_ && now_ms_ >= next_blink_ms_) {
        blink_start_ms_ = now_ms_ | 1;
    }
    if (blink_factor != blink_factor_) {
        blink_factor_ = blink_factor;
        dirty = true;
    }

    // Idle glance: look somewhere nearby, then come back.
    if (glance_ && now_ms_ >= next_glance_ms_) {
        bool centered = glance_x_ != 0.0f || glance_y_ != 0.0f || RandomBelow(10) < 3;
        glance_x_ = centered ? 0.0f : RandomSigned() * 3.0f;
        glance_y_ = centered ? 0.0f : RandomSigned() * 1.5f;
        // A curious tilt: one eye a little bigger while looking away.
        glance_tilt_ = centered || RandomBelow(2) ? 0.0f : RandomSigned() * 0.1f;
        next_glance_ms_ = now_ms_ + 1000 + RandomBelow(3500);
    }

    float wiggle_offset = wiggle_ > 0.0f ? wiggle_ * std::sin(now_ms_ * 0.02f) : 0.0f;
    if (wiggle_offset != wiggle_offset_) {
        wiggle_offset_ = wiggle_offset;
        dirty = true;
    }

    if (target_.tear > 0.5f) {
        tear_phase_ += 0.04f;
        if (tear_phase_ > 1.0f) {
            tear_phase_ = 0.0f;
        }
        dirty = true;
    }

    // Quiet breathing while nothing else moves the eyes: one pixel up and down.
    int breath_px = activity_ == Activity::kNone && wiggle_ == 0.0f
                        ? static_cast<int>(std::lround(std::sin(now_ms_ * 0.00175f)))
                        : 0;
    if (breath_px != breath_px_) {
        breath_px_ = breath_px;
        dirty = true;
    }

    if (compact_ != compact_target_) {
        compact_ += (compact_target_ - compact_) * kFollow;
        if (std::fabs(compact_target_ - compact_) < 0.01f) {
            compact_ = compact_target_;
        }
        dirty = true;
    }
    for (int i = 0; i < 4; i++) {
        float diff = box_target_[i] - box_[i];
        if (diff != 0.0f) {
            box_[i] = std::fabs(diff) < 0.5f ? box_target_[i] : box_[i] + diff * kFollow;
            dirty = true;
        }
    }

    // Spring every pose value toward the goal (slight overshoot, then rest).
    Pose goal = target_;
    goal.look_x += glance_x_;
    goal.look_y += glance_y_;
    goal.left_scale += glance_tilt_;
    goal.right_scale -= glance_tilt_;
    auto* current = reinterpret_cast<float*>(&current_);
    auto* velocity = reinterpret_cast<float*>(&velocity_);
    const auto* wanted = reinterpret_cast<const float*>(&goal);
    for (size_t i = 0; i < sizeof(Pose) / sizeof(float); i++) {
        float diff = wanted[i] - current[i];
        if (std::fabs(diff) < 0.01f && std::fabs(velocity[i]) < 0.01f) {
            velocity[i] = 0.0f;
            if (diff != 0.0f) {
                current[i] = wanted[i];
                dirty = true;
            }
        } else {
            velocity[i] = velocity[i] * kDamping + diff * kSpring;
            current[i] += velocity[i];
            dirty = true;
        }
    }

    if (dirty) {
        lv_obj_invalidate(obj_);
    }
}

void OledEyes::OnDraw(lv_event_t* event) {
    auto* self = static_cast<OledEyes*>(lv_event_get_user_data(event));
    lv_layer_t* layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(self->obj_, &area);
    lv_obj_t* parent = lv_obj_get_parent(self->obj_);
    lv_color_t fg = lv_obj_get_style_text_color(parent, LV_PART_MAIN);
    lv_color_t bg = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);

    // Interpolate between the full object and the compact box.
    const float t = self->compact_;
    auto mix = [t](int full, int compact) {
        return static_cast<int>(std::lround(full + (compact - full) * t));
    };
    auto at = [](int origin, float offset) {
        return origin + static_cast<int>(std::lround(offset));
    };
    lv_area_t box = {mix(area.x1, at(area.x1, self->box_[0])),
                     mix(area.y1, at(area.y1, self->box_[1])),
                     mix(area.x2, at(area.x1, self->box_[2])),
                     mix(area.y2, at(area.y1, self->box_[3]))};
    self->Draw(layer, box, fg, bg);
}

void OledEyes::Draw(lv_layer_t* layer, const lv_area_t& area, lv_color_t fg,
                    lv_color_t bg) const {
    const float width = static_cast<float>(area.x2 - area.x1 + 1);
    const float height = static_cast<float>(area.y2 - area.y1 + 1);
    const float unit_x = width / 32.0f;
    const float unit_y = height / 32.0f;

    const float base_w = std::min(width * 0.30f, height * 0.62f) * current_.eye_w;
    const float base_h = std::min(height * 0.70f, width * 0.60f) * current_.eye_h;
    const float gap = width * 0.10f;
    const float center_x = area.x1 + width / 2.0f + current_.look_x * unit_x;
    // Asleep: closed eyes that rise and fall with each slow breath.
    const uint32_t activity_ms = now_ms_ - activity_start_ms_;
    const bool asleep =
        activity_ == Activity::kSleep && activity_ms >= kAsleepMs && compact_ == 0.0f;
    const float breath =
        asleep ? 1.2f * std::sin(activity_ms * (6.2832f / static_cast<float>(kBreathMs))) : 0.0f;
    const float center_y = area.y1 + height / 2.0f +
                           (current_.look_y + wiggle_offset_ + breath) * unit_y + breath_px_;
    // Squash and stretch: a closing eye gets a little wider.
    const float squash = 1.0f + 0.18f * (1.0f - blink_factor_);
    const float offset = gap / 2.0f + base_w / 2.0f;

    for (int side = 0; side < 2; side++) {
        bool left = side == 0;
        float scale = left ? current_.left_scale : current_.right_scale;
        int w = std::max(2, static_cast<int>(std::lround(base_w * scale * squash)));
        int h = std::max(2, static_cast<int>(std::lround(base_h * scale * blink_factor_)));
        int cx = static_cast<int>(std::lround(center_x + (left ? -offset : offset)));
        int cy = static_cast<int>(std::lround(center_y));
        if (asleep) {
            // A closed eye as a downward curve: an ellipse with its upper part covered.
            int arc_h = std::max(4, static_cast<int>(std::lround(w * 0.45f)));
            int thick = std::max(2, static_cast<int>(std::lround(unit_y * 1.5f)));
            int x1 = cx - w / 2;
            int x2 = x1 + w - 1;
            int y1 = cy - arc_h / 2;
            FillRect(layer, x1, y1, x2, y1 + arc_h - 1, arc_h / 2, fg);
            FillRect(layer, x1 - 1, y1 - thick, x2 + 1, y1 + arc_h - 1 - thick, arc_h / 2, bg);
            continue;
        }
        DrawEye(layer, cx, cy, w, h, left, fg, bg, static_cast<int>(unit_y));
    }

    // Decorations once the eyes have settled where the activity happens.
    if (activity_ != Activity::kNone && compact_ == compact_target_) {
        DrawActivity(layer, area, fg, bg);
    }
}

void OledEyes::DrawActivity(lv_layer_t* layer, const lv_area_t& area, lv_color_t fg,
                            lv_color_t bg) const {
    const float width = static_cast<float>(area.x2 - area.x1 + 1);
    const float height = static_cast<float>(area.y2 - area.y1 + 1);
    auto px = [&](float fraction) {
        return area.x1 + static_cast<int>(std::lround(fraction * width));
    };
    auto py = [&](float fraction) {
        return area.y1 + static_cast<int>(std::lround(fraction * height));
    };
    auto size = [&](float fraction, int minimum) {
        return std::max(minimum, static_cast<int>(std::lround(fraction * height)));
    };
    const uint32_t elapsed = now_ms_ - activity_start_ms_;
    const bool props = width >= kMinPropWidth;

    switch (activity_) {
        case Activity::kDaydream: {
            // Thought bubbles rising to the upper right, the last one holding what she imagines.
            if (elapsed >= 300) {
                FillCircle(layer, px(0.70f), py(0.80f), size(0.04f, 1), fg);
            }
            if (elapsed >= 800) {
                FillCircle(layer, px(0.77f), py(0.60f), size(0.07f, 2), fg);
            }
            if (elapsed >= 1300) {
                int r = size(0.22f, 6);
                int cx = std::min(px(0.86f), area.x2 - r);
                int cy = std::max(py(0.30f), area.y1 + r);
                FillCircle(layer, cx, cy, r, fg);
                FillCircle(layer, cx, cy, r - 1, bg);
            }
            if (elapsed >= 1800) {
                int r = size(0.22f, 6);
                int cx = std::min(px(0.86f), area.x2 - r);
                int cy = std::max(py(0.30f), area.y1 + r);
                int inner = std::max(2, r - 4);
                if (variant_ == 0) {
                    FillHeart(layer, cx, cy, inner * 2, fg);
                } else if (variant_ == 1) {
                    FillStar(layer, cx, cy, inner, fg);
                } else {
                    FillNote(layer, cx - inner / 3, cy + inner / 2, inner + 1, fg);
                }
            }
            break;
        }
        case Activity::kPlay: {
            // Ball with a dark rim so it stays visible over the eyes.
            int r = size(0.05f, 2);
            int cx = px(ball_x_);
            int cy = py(ball_y_);
            FillCircle(layer, cx, cy, r + 1, bg);
            FillCircle(layer, cx, cy, r, fg);
            break;
        }
        case Activity::kHum: {
            // Two notes floating up on the right, half a cycle apart.
            for (int i = 0; i < 2; i++) {
                float p = static_cast<float>((elapsed + i * 1000) % 2000) / 2000.0f;
                float x = (i == 0 ? 0.80f : 0.90f) + 0.03f * std::sin(p * 6.28f);
                FillNote(layer, px(x), py(0.85f - 0.6f * p), size(0.18f, 5), fg);
            }
            break;
        }
        case Activity::kSleep: {
            if (elapsed < kAsleepMs) {
                break;  // still falling asleep
            }
            // Three Zs drifting up and growing.
            for (int i = 0; i < 3; i++) {
                float p = static_cast<float>((elapsed + i * 1200) % 3600) / 3600.0f;
                int s = size(0.08f + 0.14f * p, 3);
                FillZ(layer, px(0.72f + 0.16f * p), py(0.60f - 0.50f * p), s, fg);
            }
            break;
        }
        case Activity::kRead: {
            if (!props) {
                break;
            }
            // An open book below the eyes: two pages with lines of text, a page turning over.
            int bw = std::min(static_cast<int>(width * 0.5f), 64);
            int bh = size(0.36f, 10);
            int cx = px(0.5f);
            int bottom = area.y2;
            int top = bottom - bh + 1;
            for (int side = -1; side <= 1; side += 2) {
                int x1 = side < 0 ? cx - bw / 2 : cx + 1;
                int x2 = side < 0 ? cx - 1 : cx + bw / 2;
                FillRect(layer, x1, top, x2, bottom, 0, fg);
                FillRect(layer, x1 + 1, top + 1, x2 - 1, bottom - 2, 0, bg);
                for (int line = 0; line < 3; line++) {
                    int ly = top + 4 + line * 3;
                    if (ly < bottom - 3) {
                        int end = (line == 2) ? x2 - (x2 - x1) / 2 : x2 - 3;
                        FillRect(layer, x1 + 3, ly, end, ly, 0, fg);
                    }
                }
            }
            uint32_t in_page = elapsed % kReadPageMs;
            if (elapsed >= kReadPageMs && in_page < kFlipMs) {
                float c = std::cos(3.14159f * in_page / kFlipMs);
                int edge = cx + static_cast<int>(std::lround(c * (bw / 2)));
                int x1 = std::min(cx, edge);
                int x2 = std::max(cx, edge);
                FillRect(layer, x1, top - 2, x2, bottom - 2, 0, fg);
                if (x2 - x1 > 2) {
                    FillRect(layer, x1 + 1, top - 1, x2 - 1, bottom - 3, 0, bg);
                }
            }
            break;
        }
        case Activity::kHammer: {
            if (!props) {
                break;
            }
            // Hand at the right; the hammer lifts and strikes a nail standing on a board.
            int hx = px(0.90f);
            int hy = py(0.50f);
            int length = size(0.42f, 10);
            int head = size(0.26f, 7);
            uint32_t phase = elapsed % kHammerMs;
            float rad = HammerAngle(phase) * 3.14159f / 180.0f;
            int ex = hx + static_cast<int>(std::lround(length * std::cos(rad)));
            int ey = hy + static_cast<int>(std::lround(length * std::sin(rad)));
            FillLine(layer, hx, hy, ex, ey, 2, fg);
            // The head is perpendicular to the handle.
            int dx = static_cast<int>(std::lround(head / 2.0f * -std::sin(rad)));
            int dy = static_cast<int>(std::lround(head / 2.0f * std::cos(rad)));
            FillLine(layer, ex - dx, ey - dy, ex + dx, ey + dy, 4, fg);
            // Nail under the strike point, sinking a bit with every blow.
            int nail_x = hx - length;
            int nail_top = hy + head / 2 + 1;
            int board = nail_top + size(0.20f, 6);
            int sunk = static_cast<int>((elapsed / kHammerMs) % 5);
            int nail_len = std::max(1, (board - nail_top) - sunk);
            FillRect(layer, nail_x - 2, board - nail_len - 1, nail_x + 2, board - nail_len - 1,
                     0, fg);
            FillRect(layer, nail_x, board - nail_len, nail_x, board - 1, 0, fg);
            FillRect(layer, nail_x - 10, board, nail_x + 10, board + 1, 0, fg);
            if (elapsed >= kHammerMs && phase < 140) {
                // Sparks.
                static const int kSparkAngles[] = {200, 235, 305, 340};
                for (int a : kSparkAngles) {
                    float r = a * 3.14159f / 180.0f;
                    FillLine(layer, nail_x + static_cast<int>(5 * std::cos(r)),
                             nail_top + static_cast<int>(5 * std::sin(r)),
                             nail_x + static_cast<int>(9 * std::cos(r)),
                             nail_top + static_cast<int>(9 * std::sin(r)), 1, fg);
                }
            }
            break;
        }
        case Activity::kExercise: {
            if (!props) {
                break;
            }
            // A dumbbell on each side, curled one after the other; a drop of sweat now and then.
            uint32_t rep = elapsed / kRepMs;
            float lift = std::sin(3.14159f * (elapsed % kRepMs) / kRepMs);
            int low = py(0.80f);
            int high = py(0.45f);
            int half = size(0.16f, 5);
            int plate = size(0.24f, 7);
            for (int side = 0; side < 2; side++) {
                bool active = (rep % 2) == static_cast<uint32_t>(side);
                int cx = px(side == 0 ? 0.12f : 0.88f);
                int cy = low - static_cast<int>(std::lround((low - high) * (active ? lift : 0)));
                FillRect(layer, cx - half, cy - 1, cx + half, cy, 0, fg);
                FillRect(layer, cx - half - 2, cy - plate / 2, cx - half, cy + plate / 2, 1, fg);
                FillRect(layer, cx + half, cy - plate / 2, cx + half + 2, cy + plate / 2, 1, fg);
            }
            if (rep % 3 == 2) {
                float p = static_cast<float>(elapsed % kRepMs) / kRepMs;
                int dx = px(0.74f);
                int dy = py(0.10f + 0.5f * p);
                FillTriangle(layer, dx, dy - 3, dx - 2, dy + 1, dx + 2, dy + 1, fg);
                FillCircle(layer, dx, dy + 1, 2, fg);
            }
            break;
        }
        case Activity::kDance: {
            if (!props) {
                break;
            }
            // In a wide box, notes hop at both sides on the beat.
            float bounce = std::fabs(std::sin(3.14159f * (elapsed % kBeatMs) / kBeatMs));
            int lift = static_cast<int>(std::lround(bounce * size(0.15f, 3)));
            FillNote(layer, px(0.08f), py(0.75f) - lift, size(0.25f, 6), fg);
            FillNote(layer, px(0.90f), py(0.70f) - (size(0.15f, 3) - lift), size(0.25f, 6), fg);
            break;
        }
        case Activity::kNone:
            break;
    }
}

void OledEyes::DrawEye(lv_layer_t* layer, int cx, int cy, int w, int h, bool left,
                       lv_color_t fg, lv_color_t bg, int unit) const {
    if (current_.heart > 0.5f) {
        FillHeart(layer, cx, cy, std::max(w, h), fg);
        return;
    }

    const int x1 = cx - w / 2;
    const int x2 = x1 + w - 1;
    const int y1 = cy - h / 2;
    const int y2 = y1 + h - 1;
    FillRect(layer, x1, y1, x2, y2, static_cast<int>(w * 0.45f), fg);

    if (current_.pupil > 0.05f) {
        int d = std::max(2, static_cast<int>(std::lround(current_.pupil * w)));
        FillRect(layer, cx - d / 2, cy - d / 2, cx - d / 2 + d - 1, cy - d / 2 + d - 1, d / 2, bg);
    }

    // Top lid: a trapezoid in the background color that slopes between the two corners.
    int inner = static_cast<int>(std::lround(current_.lid_inner * h));
    int outer = static_cast<int>(std::lround(current_.lid_outer * h));
    if (inner > 0 || outer > 0) {
        int top = y1 - 1;
        int x_inner = left ? x2 + 1 : x1 - 1;
        int x_outer = left ? x1 - 1 : x2 + 1;
        FillTriangle(layer, x_outer, top, x_inner, top, x_inner, y1 + inner, bg);
        FillTriangle(layer, x_outer, top, x_inner, y1 + inner, x_outer, y1 + outer, bg);
    }

    // Bottom lid: a rounded cover that leaves only an arch of the eye, like a smile.
    if (current_.lid_bottom > 0.02f) {
        int cover_top = y2 - static_cast<int>(std::lround(current_.lid_bottom * h)) + 1;
        FillRect(layer, x1 - 1, cover_top, x2 + 1, y2 + 3, (w + 2) / 2, bg);
    }

    if (target_.tear > 0.5f) {
        int tear_x = left ? x1 + w / 4 : x2 - w / 4;
        int tear_y = y2 + 1 + static_cast<int>(tear_phase_ * std::max(2, unit * 5));
        FillRect(layer, tear_x - 1, tear_y, tear_x + 1, tear_y + 3, 1, fg);
    }
}

#endif  // CONFIG_OLED_REACTIVE_EYES
