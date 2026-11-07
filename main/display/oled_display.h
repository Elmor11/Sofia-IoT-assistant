#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include "lvgl_display.h"
#include "oled_eyes.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <memory>

class OledDisplay : public LvglDisplay {
private:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* content_left_ = nullptr;
    lv_obj_t* content_right_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* emotion_label_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
#if CONFIG_OLED_REACTIVE_EYES
    std::unique_ptr<OledEyes> eyes_;
    lv_timer_t* layout_timer_ = nullptr;
    bool text_mode_ = false;      // text is wanted, eyes are (shrinking to) the compact box
    bool text_ui_shown_ = false;  // chat text is visible
    bool status_shown_ = false;   // status bar is visible too (alerts and notifications only)
    uint32_t text_idle_since_ms_ = 0;
    void SetTextUiVisible(bool text, bool status);
    void UpdateEyesLayout();
    static void OnLayoutTimer(lv_timer_t* timer);
#endif

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    void SetupUI_128x64();
    void SetupUI_128x32();

public:
    OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                int height, bool mirror_x, bool mirror_y);
    ~OledDisplay();

    virtual void SetupUI() override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void RemoveChatMessage(const char* role, const char* content) override;
#if CONFIG_OLED_REACTIVE_EYES
    virtual void ClearChatMessages() override;
#endif
    virtual bool SetEmotion(const char* emotion) override;
    virtual void SetTheme(Theme* theme) override;
    virtual bool IsMonochrome() const override { return true; }
    void SetPowerSaveMode(bool on) override;
};

#endif  // OLED_DISPLAY_H
