#include "oled_music.h"

#if CONFIG_OLED_REACTIVE_EYES

#include "display.h"

#include <algorithm>
#include <cstdio>

namespace {

constexpr int kBarHeight = 5;
constexpr int kIconSize = 11;
constexpr int kMinHeightForTime = 60;

void FillRect(lv_layer_t* layer, int x1, int y1, int x2, int y2, lv_color_t color) {
    if (x2 < x1 || y2 < y1) {
        return;
    }
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
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

void FormatClock(char* out, size_t size, int64_t ms) {
    int64_t total = std::max<int64_t>(0, ms) / 1000;
    int64_t hours = total / 3600;
    int minutes = static_cast<int>((total / 60) % 60);
    int seconds = static_cast<int>(total % 60);
    if (hours > 0) {
        snprintf(out, size, "%d:%02d:%02d", static_cast<int>(hours), minutes, seconds);
    } else {
        snprintf(out, size, "%d:%02d", minutes, seconds);
    }
}

}  // namespace

OledMusicPanel::OledMusicPanel(lv_obj_t* parent, int x, int y, int w, int h)
    : w_(w), state_(kMusicStopped) {
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

    title_ = lv_label_create(obj_);
    lv_obj_set_width(title_, w);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(title_, 0, 0);
    lv_label_set_text(title_, "");

    bar_y_ = line + 4;
    if (h >= kMinHeightForTime) {
        time_ = lv_label_create(obj_);
        lv_obj_set_width(time_, w);
        lv_obj_set_style_text_align(time_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(time_, 0, bar_y_ + kBarHeight + 2);
        lv_label_set_text(time_, "");
        icons_y_ = h - kIconSize - 1;
    }
}

OledMusicPanel::~OledMusicPanel() {
    if (obj_ != nullptr) {
        lv_obj_delete(obj_);
    }
}

void OledMusicPanel::SetTrack(const char* title, const char* artist, int64_t position_ms,
                              int64_t duration_ms, int state) {
    state_ = state;
    position_ms_ = std::max<int64_t>(0, position_ms);
    duration_ms_ = std::max<int64_t>(0, duration_ms);
    stamp_ms_ = lv_tick_get();

    std::string text = title != nullptr ? title : "";
    if (artist != nullptr && artist[0] != '\0') {
        text += text.empty() ? "" : " - ";
        text += artist;
    }
    // Setting identical text again would restart the scrolling.
    if (text != text_) {
        text_ = std::move(text);
        lv_label_set_text(title_, text_.c_str());
    }
    last_fill_ = -1;
    last_second_ = -1;
    UpdateTime();
    lv_obj_invalidate(obj_);
}

void OledMusicPanel::Show(bool visible) {
    if (visible) {
        lv_obj_remove_flag(obj_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj_, LV_OBJ_FLAG_HIDDEN);
    }
}

bool OledMusicPanel::IsShown() const { return !lv_obj_has_flag(obj_, LV_OBJ_FLAG_HIDDEN); }

bool OledMusicPanel::Active() const { return state_ != kMusicStopped; }

int64_t OledMusicPanel::CurrentPositionMs() const {
    int64_t position = position_ms_;
    if (state_ == kMusicPlaying) {
        position += static_cast<int64_t>(lv_tick_elaps(stamp_ms_));
    }
    if (duration_ms_ > 0) {
        position = std::min(position, duration_ms_);
    }
    return position;
}

void OledMusicPanel::UpdateTime() {
    if (time_ == nullptr) {
        return;
    }
    int64_t position = CurrentPositionMs();
    int64_t second = position / 1000;
    if (second == last_second_) {
        return;
    }
    last_second_ = second;
    char now[16];
    FormatClock(now, sizeof(now), position);
    char text[40];
    if (duration_ms_ > 0) {
        char total[16];
        FormatClock(total, sizeof(total), duration_ms_);
        snprintf(text, sizeof(text), "%s / %s", now, total);
    } else {
        snprintf(text, sizeof(text), "%s", now);
    }
    lv_label_set_text(time_, text);
}

void OledMusicPanel::Tick() {
    UpdateTime();
    // Redraw only when the bar moves by at least one pixel.
    int fill = 0;
    if (duration_ms_ > 0) {
        fill = static_cast<int>((w_ - 2) * CurrentPositionMs() / duration_ms_);
    }
    if (fill != last_fill_) {
        last_fill_ = fill;
        lv_obj_invalidate(obj_);
    }
}

void OledMusicPanel::OnDraw(lv_event_t* event) {
    auto* self = static_cast<OledMusicPanel*>(lv_event_get_user_data(event));
    lv_layer_t* layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(self->obj_, &area);
    lv_obj_t* parent = lv_obj_get_parent(self->obj_);
    lv_color_t fg = lv_obj_get_style_text_color(parent, LV_PART_MAIN);
    lv_color_t bg = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);

    // Progress bar: outline with the elapsed part filled.
    int x1 = area.x1;
    int x2 = area.x2;
    int y1 = area.y1 + self->bar_y_;
    int y2 = y1 + kBarHeight - 1;
    FillRect(layer, x1, y1, x2, y2, fg);
    FillRect(layer, x1 + 1, y1 + 1, x2 - 1, y2 - 1, bg);
    int fill = 0;
    if (self->duration_ms_ > 0) {
        fill = static_cast<int>((self->w_ - 2) * self->CurrentPositionMs() / self->duration_ms_);
    }
    FillRect(layer, x1 + 1, y1 + 1, x1 + fill, y2 - 1, fg);

    if (self->icons_y_ < 0) {
        return;
    }
    // Pause (or play when paused) and next, centered below the time.
    int top = area.y1 + self->icons_y_;
    int bottom = top + kIconSize - 1;
    int mid = (top + bottom) / 2;
    int cx = (x1 + x2) / 2;
    int px = cx - 18;
    if (self->state_ == kMusicPlaying) {
        FillRect(layer, px, top, px + 3, bottom, fg);
        FillRect(layer, px + 7, top, px + 10, bottom, fg);
    } else {
        FillTriangle(layer, px, top, px, bottom, px + 10, mid, fg);
    }
    int nx = cx + 8;
    FillTriangle(layer, nx, top, nx, bottom, nx + 8, mid, fg);
    FillRect(layer, nx + 10, top, nx + 11, bottom, fg);
}

#endif  // CONFIG_OLED_REACTIVE_EYES
