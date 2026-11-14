#include "oled_pomodoro.h"

#if CONFIG_OLED_REACTIVE_EYES

#include "display.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr int kArcWidth = 5;
constexpr int kSteps = 360;  // redraw only when the arc grows by half a degree

void DrawArc(lv_layer_t* layer, int cx, int cy, int radius, int width, float start, float end,
             bool rounded, lv_color_t color) {
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.center.x = cx;
    dsc.center.y = cy;
    dsc.radius = static_cast<uint16_t>(radius);
    // LVGL expects 0..360; arcs that cross 0 degrees are given as start > end.
    dsc.start_angle = std::fmod(start, 360.0f);
    dsc.end_angle = std::fmod(end, 360.0f);
    dsc.rounded = rounded ? 1 : 0;
    dsc.opa = LV_OPA_COVER;
    lv_draw_arc(layer, &dsc);
}

lv_obj_t* MakeLabel(lv_obj_t* parent, int x, int y, int w, lv_text_align_t align) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, "");
    return label;
}

}  // namespace

OledPomodoroPanel::OledPomodoroPanel(lv_obj_t* parent, int x, int y, int w, int h) {
    obj_ = lv_obj_create(parent);
    lv_obj_remove_style_all(obj_);
    lv_obj_set_size(obj_, w, h);
    lv_obj_set_pos(obj_, x, y);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(obj_, OnDraw, LV_EVENT_DRAW_MAIN, this);

    const lv_font_t* font = lv_obj_get_style_text_font(parent, LV_PART_MAIN);
    int line = lv_font_get_line_height(font);

    // Half circle standing on the text row; the time sits under its center.
    cx_ = w / 2;
    cy_ = std::max(h / 2, h - line);
    radius_ = std::max(8, std::min(w / 2 - 2, cy_ - 1));
    int bottom = h - line;
    time_ = MakeLabel(obj_, w / 4, bottom, w / 2, LV_TEXT_ALIGN_CENTER);
    round_label_ = MakeLabel(obj_, 0, bottom, w / 4, LV_TEXT_ALIGN_LEFT);
    phase_label_ = MakeLabel(obj_, w - w / 4 - 4, bottom, w / 4 + 4, LV_TEXT_ALIGN_RIGHT);

    // Inside the arc: as wide as fits at mid-height, from just under the top to the base.
    int inner_r = radius_ - kArcWidth - 1;
    int half_w = static_cast<int>(inner_r * 0.87f);
    inner_ = {x + cx_ - half_w, y + cy_ - inner_r + 4, x + cx_ + half_w - 1, y + cy_ - 1};
}

OledPomodoroPanel::~OledPomodoroPanel() {
    if (obj_ != nullptr) {
        lv_obj_delete(obj_);
    }
}

void OledPomodoroPanel::SetState(int phase, int64_t elapsed_ms, int64_t total_ms, bool running,
                                 int round, int rounds) {
    phase_ = phase;
    running_ = running;
    elapsed_ms_ = std::max<int64_t>(0, elapsed_ms);
    total_ms_ = std::max<int64_t>(0, total_ms);
    stamp_ms_ = lv_tick_get();

    char text[12];
    if (phase == kPomodoroFocus) {
        snprintf(text, sizeof(text), "%d/%d", round, rounds);
    } else {
        text[0] = '\0';
    }
    lv_label_set_text(round_label_, text);
    lv_label_set_text(phase_label_, !running                  ? "Pausa"
                                    : phase == kPomodoroFocus ? "Foco"
                                                              : "Relax");
    last_step_ = -1;
    last_second_ = -1;
    UpdateTime();
    lv_obj_invalidate(obj_);
}

void OledPomodoroPanel::Show(bool visible) {
    if (visible) {
        lv_obj_remove_flag(obj_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj_, LV_OBJ_FLAG_HIDDEN);
    }
}

bool OledPomodoroPanel::IsShown() const { return !lv_obj_has_flag(obj_, LV_OBJ_FLAG_HIDDEN); }

bool OledPomodoroPanel::Active() const { return phase_ != kPomodoroOff; }

int64_t OledPomodoroPanel::CurrentElapsedMs() const {
    int64_t elapsed = elapsed_ms_;
    if (running_) {
        elapsed += static_cast<int64_t>(lv_tick_elaps(stamp_ms_));
    }
    return std::min(elapsed, total_ms_);
}

float OledPomodoroPanel::Fraction() const {
    if (total_ms_ <= 0) {
        return 0.0f;
    }
    return static_cast<float>(CurrentElapsedMs()) / static_cast<float>(total_ms_);
}

void OledPomodoroPanel::UpdateTime() {
    int64_t second = CurrentElapsedMs() / 1000;
    if (second == last_second_) {
        return;
    }
    last_second_ = second;
    char text[16];
    snprintf(text, sizeof(text), "%d:%02d", static_cast<int>(second / 60),
             static_cast<int>(second % 60));
    lv_label_set_text(time_, text);
}

void OledPomodoroPanel::Tick() {
    UpdateTime();
    int step = static_cast<int>(Fraction() * kSteps);
    if (step != last_step_) {
        last_step_ = step;
        lv_obj_invalidate(obj_);
    }
}

void OledPomodoroPanel::OnDraw(lv_event_t* event) {
    auto* self = static_cast<OledPomodoroPanel*>(lv_event_get_user_data(event));
    lv_layer_t* layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(self->obj_, &area);
    lv_obj_t* parent = lv_obj_get_parent(self->obj_);
    lv_color_t fg = lv_obj_get_style_text_color(parent, LV_PART_MAIN);

    const int cx = area.x1 + self->cx_;
    const int cy = area.y1 + self->cy_;
    const int r = self->radius_;

    // Track: a thin half circle along the middle of the band, with rounded caps at the base.
    DrawArc(layer, cx, cy, r - kArcWidth / 2, 1, 180.0f, 360.0f, false, fg);
    // Progress: the thick rounded band, from the left end over the top.
    float fraction = self->Fraction();
    if (fraction > 0.003f) {
        DrawArc(layer, cx, cy, r, kArcWidth, 180.0f, 180.0f + 180.0f * fraction, true, fg);
    } else {
        DrawArc(layer, cx, cy, r, kArcWidth, 180.0f, 181.0f, true, fg);  // starting dot
    }
}

#endif  // CONFIG_OLED_REACTIVE_EYES
