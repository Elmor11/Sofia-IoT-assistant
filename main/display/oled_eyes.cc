#include "oled_eyes.h"

#if CONFIG_OLED_REACTIVE_EYES

#include <algorithm>
#include <cmath>
#include <cstring>

#include <esp_random.h>

namespace {

constexpr uint32_t kTickMs = 50;
constexpr uint32_t kBlinkMs = 160;
constexpr float kBlinkDepth = 0.92f;
constexpr float kFollow = 0.4f;  // fraction of the remaining distance covered each tick

struct Mood {
    const char* name;
    float eye_w, eye_h, look_x, look_y;
    float lid_inner, lid_outer, lid_bottom, pupil;
    float left_scale, right_scale, tear, heart;
    float wiggle;
    bool glance;
    bool blink;
};

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

}  // namespace

OledEyes::OledEyes(lv_obj_t* parent, int compact_x, int compact_y, int compact_w,
                   int compact_h) {
    compact_box_ = {compact_x, compact_y, compact_x + compact_w - 1, compact_y + compact_h - 1};
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

void OledEyes::SetMood(const char* mood) {
    const Mood* found = FindMood(mood);
    if (found == nullptr) {
        found = &kMoods[0];
    }
    target_.eye_w = found->eye_w;
    target_.eye_h = found->eye_h;
    target_.look_x = found->look_x;
    target_.look_y = found->look_y;
    target_.lid_inner = found->lid_inner;
    target_.lid_outer = found->lid_outer;
    target_.lid_bottom = found->lid_bottom;
    target_.pupil = found->pupil;
    target_.left_scale = found->left_scale;
    target_.right_scale = found->right_scale;
    target_.tear = found->tear;
    target_.heart = found->heart;
    wiggle_ = found->wiggle;
    glance_ = found->glance;
    blink_ = found->blink;
    glance_x_ = 0.0f;
    glance_y_ = 0.0f;
    if (timer_ != nullptr) {
        lv_timer_resume(timer_);
        lv_obj_invalidate(obj_);
    }
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
    bool dirty = false;

    // Blink: the eyes close and reopen over kBlinkMs.
    float blink_factor = 1.0f;
    if (blink_start_ms_ != 0) {
        float t = static_cast<float>(now_ms_ - blink_start_ms_) / kBlinkMs;
        if (t >= 1.0f) {
            blink_start_ms_ = 0;
            next_blink_ms_ = now_ms_ + 2500 + RandomBelow(4000);
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

    if (compact_ != compact_target_) {
        compact_ += (compact_target_ - compact_) * kFollow;
        if (std::fabs(compact_target_ - compact_) < 0.01f) {
            compact_ = compact_target_;
        }
        dirty = true;
    }

    // Ease every pose value toward the goal; snap when close so the eyes come to rest.
    Pose goal = target_;
    goal.look_x += glance_x_;
    goal.look_y += glance_y_;
    auto* current = reinterpret_cast<float*>(&current_);
    const auto* wanted = reinterpret_cast<const float*>(&goal);
    for (size_t i = 0; i < sizeof(Pose) / sizeof(float); i++) {
        float diff = wanted[i] - current[i];
        if (std::fabs(diff) < 0.01f) {
            if (diff != 0.0f) {
                current[i] = wanted[i];
                dirty = true;
            }
        } else {
            current[i] += diff * kFollow;
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
    lv_area_t box = {mix(area.x1, area.x1 + self->compact_box_.x1),
                     mix(area.y1, area.y1 + self->compact_box_.y1),
                     mix(area.x2, area.x1 + self->compact_box_.x2),
                     mix(area.y2, area.y1 + self->compact_box_.y2)};
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
    const float center_y =
        area.y1 + height / 2.0f + (current_.look_y + wiggle_offset_) * unit_y;
    const float offset = gap / 2.0f + base_w / 2.0f;

    for (int side = 0; side < 2; side++) {
        bool left = side == 0;
        float scale = left ? current_.left_scale : current_.right_scale;
        int w = std::max(2, static_cast<int>(std::lround(base_w * scale)));
        int h = std::max(2, static_cast<int>(std::lround(base_h * scale * blink_factor_)));
        int cx = static_cast<int>(std::lround(center_x + (left ? -offset : offset)));
        int cy = static_cast<int>(std::lround(center_y));
        DrawEye(layer, cx, cy, w, h, left, fg, bg, static_cast<int>(unit_y));
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
