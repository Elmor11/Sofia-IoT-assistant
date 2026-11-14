#include "pomodoro.h"

#include "application.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"

#include <cJSON.h>
#include <esp_log.h>
#include <nvs.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>

#define TAG "Pomodoro"

extern const char pomodoro_break_ogg_start[] asm("_binary_pomodoro_break_ogg_start");
extern const char pomodoro_break_ogg_end[] asm("_binary_pomodoro_break_ogg_end");
extern const char pomodoro_focus_ogg_start[] asm("_binary_pomodoro_focus_ogg_start");
extern const char pomodoro_focus_ogg_end[] asm("_binary_pomodoro_focus_ogg_end");

namespace {

constexpr const char* kNvsNamespace = "pomodoro";
constexpr const char* kNvsKey = "history";
constexpr int64_t kBankPeriodUs = 2 * 60 * 1000000LL;  // bank study time every 2 minutes
constexpr time_t kValidTime = 1704067200;              // 2024-01-01: the clock was set

// Local day number. The server time already includes the timezone offset (see Ota).
uint32_t Today() {
    time_t now = time(nullptr);
    return now >= kValidTime ? static_cast<uint32_t>(now / 86400) : 0;
}

const char* PhaseName(int phase) {
    switch (phase) {
        case kPomodoroFocus:
            return "focus";
        case kPomodoroShortBreak:
            return "short_break";
        case kPomodoroLongBreak:
            return "long_break";
        default:
            return "off";
    }
}

}  // namespace

Pomodoro::Pomodoro() {
    esp_timer_create_args_t phase_args = {};
    phase_args.callback = [](void*) {
        Application::GetInstance().Schedule([]() { Pomodoro::GetInstance().OnPhaseEnd(); });
    };
    phase_args.name = "pomodoro";
    ESP_ERROR_CHECK(esp_timer_create(&phase_args, &phase_timer_));

    esp_timer_create_args_t bank_args = {};
    bank_args.callback = [](void*) {
        Application::GetInstance().Schedule([]() { Pomodoro::GetInstance().BankStudy(); });
    };
    bank_args.name = "pomodoro_bank";
    ESP_ERROR_CHECK(esp_timer_create(&bank_args, &bank_timer_));
}

int64_t Pomodoro::ElapsedMs() const {
    int64_t elapsed = elapsed_before_ms_;
    if (phase_ != kPomodoroOff && !paused_) {
        elapsed += (esp_timer_get_time() - run_start_us_) / 1000;
    }
    return std::min(elapsed, phase_total_ms_);
}

void Pomodoro::StartRunning() {
    paused_ = false;
    run_start_us_ = esp_timer_get_time();
    esp_timer_stop(phase_timer_);
    int64_t left_ms = std::max<int64_t>(0, phase_total_ms_ - elapsed_before_ms_);
    esp_timer_start_once(phase_timer_, left_ms * 1000 + 1);
    esp_timer_stop(bank_timer_);
    if (phase_ == kPomodoroFocus) {
        esp_timer_start_periodic(bank_timer_, kBankPeriodUs);
    }
}

void Pomodoro::StopRunning() {
    elapsed_before_ms_ = ElapsedMs();
    paused_ = true;
    esp_timer_stop(phase_timer_);
    esp_timer_stop(bank_timer_);
}

void Pomodoro::EnterPhase(int phase) {
    phase_ = phase;
    elapsed_before_ms_ = 0;
    banked_ms_ = 0;
    int seconds = phase == kPomodoroFocus        ? focus_s_
                  : phase == kPomodoroShortBreak ? short_s_
                                                 : long_s_;
    phase_total_ms_ = static_cast<int64_t>(seconds) * 1000;
    StartRunning();
    ESP_LOGI(TAG, "Phase %s, round %d/%d, %d s", PhaseName(phase), cycle_, cycles_, seconds);
    UpdateDisplay();
}

void Pomodoro::Stop() {
    if (phase_ == kPomodoroOff) {
        return;
    }
    BankStudy();
    esp_timer_stop(phase_timer_);
    esp_timer_stop(bank_timer_);
    phase_ = kPomodoroOff;
    paused_ = false;
    cycle_ = 0;
    ESP_LOGI(TAG, "Stopped");
    UpdateDisplay();
}

void Pomodoro::OnPhaseEnd() {
    if (phase_ == kPomodoroOff || paused_ || ElapsedMs() < phase_total_ms_) {
        return;  // stopped, paused or restarted after the timer fired
    }
    auto display = Board::GetInstance().GetDisplay();
    if (phase_ == kPomodoroFocus) {
        BankStudy();
        LoadHistory();
        RollHistory();
        if (history_.sessions[0] < UINT8_MAX) {
            history_.sessions[0]++;
        }
        SaveHistory();
        bool long_break = cycle_ >= cycles_;
        EnterPhase(long_break ? kPomodoroLongBreak : kPomodoroShortBreak);
        PlayChime(true);
        if (display != nullptr) {
            display->ShowNotification(long_break ? "Descanso largo!" : "A descansar!", 4000);
        }
    } else if (phase_ == kPomodoroLongBreak) {
        Stop();
        PlayChime(false);
        if (display != nullptr) {
            display->ShowNotification("Pomodoro completo!", 5000);
        }
    } else {
        cycle_++;
        EnterPhase(kPomodoroFocus);
        PlayChime(false);
        if (display != nullptr) {
            display->ShowNotification("A estudiar!", 4000);
        }
    }
}

void Pomodoro::PlayChime(bool to_break) {
    auto& app = Application::GetInstance();
    // Do not talk over a conversation; the notification is enough then.
    if (app.GetDeviceState() != kDeviceStateIdle) {
        return;
    }
    if (to_break) {
        app.PlaySound(std::string_view(pomodoro_break_ogg_start,
                                       pomodoro_break_ogg_end - pomodoro_break_ogg_start));
    } else {
        app.PlaySound(std::string_view(pomodoro_focus_ogg_start,
                                       pomodoro_focus_ogg_end - pomodoro_focus_ogg_start));
    }
}

void Pomodoro::UpdateDisplay() {
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetPomodoro(phase_, ElapsedMs(), phase_total_ms_, !paused_, cycle_, cycles_);
    }
}

void Pomodoro::BankStudy() {
    if (phase_ != kPomodoroFocus) {
        return;
    }
    int64_t seconds = (ElapsedMs() - banked_ms_) / 1000;
    if (seconds <= 0) {
        return;
    }
    banked_ms_ += seconds * 1000;
    AddStudySeconds(static_cast<int>(seconds));
}

void Pomodoro::AddStudySeconds(int seconds) {
    LoadHistory();
    RollHistory();
    unbanked_s_ += seconds;
    int minutes = unbanked_s_ / 60;
    unbanked_s_ %= 60;
    if (minutes <= 0) {
        return;
    }
    history_.minutes[0] = static_cast<uint16_t>(std::min(65535, history_.minutes[0] + minutes));
    history_.total_minutes += minutes;
    SaveHistory();
}

void Pomodoro::LoadHistory() {
    if (history_loaded_) {
        return;
    }
    history_loaded_ = true;
    History stored = {};
    size_t size = sizeof(stored);
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_blob(handle, kNvsKey, &stored, &size) == ESP_OK && size == sizeof(stored) &&
            stored.version == kHistoryVersion) {
            history_ = stored;
        }
        nvs_close(handle);
    }
    history_.version = kHistoryVersion;
}

void Pomodoro::SaveHistory() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open NVS to save the study history");
        return;
    }
    if (nvs_set_blob(handle, kNvsKey, &history_, sizeof(history_)) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

// Moves the 7-day window so that index 0 is today. Without a valid clock the time goes to the
// last known day.
void Pomodoro::RollHistory() {
    uint32_t today = Today();
    if (today == 0) {
        return;
    }
    if (history_.day == 0 || today <= history_.day) {
        if (history_.day == 0) {
            history_.day = today;
        }
        return;
    }
    uint32_t shift = today - history_.day;
    for (int i = 6; i >= 0; i--) {
        bool keep = shift < 7 && i >= static_cast<int>(shift);
        history_.minutes[i] = keep ? history_.minutes[i - shift] : 0;
        history_.sessions[i] = keep ? history_.sessions[i - shift] : 0;
    }
    history_.day = today;
}

std::string Pomodoro::StatusJson() {
    LoadHistory();
    RollHistory();
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateObject(), cJSON_Delete);
    cJSON* json = root.get();
    cJSON_AddStringToObject(json, "phase", PhaseName(phase_));
    if (phase_ != kPomodoroOff) {
        int64_t elapsed = ElapsedMs();
        cJSON_AddBoolToObject(json, "paused", paused_);
        cJSON_AddNumberToObject(json, "round", cycle_);
        cJSON_AddNumberToObject(json, "rounds", cycles_);
        cJSON_AddNumberToObject(json, "elapsed_s", elapsed / 1000);
        cJSON_AddNumberToObject(json, "left_s", (phase_total_ms_ - elapsed) / 1000);
    }
    // Study time not stored yet (current phase and the sub-minute remainder) counts for today.
    int pending_s = unbanked_s_;
    if (phase_ == kPomodoroFocus) {
        pending_s += static_cast<int>((ElapsedMs() - banked_ms_) / 1000);
    }
    int today_min = history_.minutes[0] + pending_s / 60;
    cJSON_AddNumberToObject(json, "today_min", today_min);
    cJSON_AddNumberToObject(json, "today_pomodoros", history_.sessions[0]);
    int week[7];
    int week_total = 0;
    for (int i = 0; i < 7; i++) {
        week[i] = i == 0 ? today_min : history_.minutes[i];
        week_total += week[i];
    }
    cJSON_AddItemToObject(json, "last_7_days_min", cJSON_CreateIntArray(week, 7));
    cJSON_AddNumberToObject(json, "week_total_min", week_total);
    cJSON_AddNumberToObject(json, "all_time_min", history_.total_minutes + pending_s / 60);
    char* text = cJSON_PrintUnformatted(json);
    if (text == nullptr) {
        return "{}";
    }
    std::string result(text);
    cJSON_free(text);
    return result;
}

void Pomodoro::RegisterTools() {
    // One tool for everything: each tool costs memory in the tools/list reply.
    McpServer::GetInstance().AddTool(
        "self.pomodoro",
        "Pomodoro study timer shown on the screen. `action`: start, pause, resume, stop or status "
        "(time left, round, study minutes today / last 7 days, index 0 = today / all time). "
        "Minutes only for start; defaults 25/5/15, 4 rounds.",
        PropertyList({Property("action", kPropertyTypeString),
                      Property("focus_minutes", kPropertyTypeInteger, 25, 1, 120),
                      Property("break_minutes", kPropertyTypeInteger, 5, 1, 60),
                      Property("long_break_minutes", kPropertyTypeInteger, 15, 1, 60),
                      Property("rounds", kPropertyTypeInteger, 4, 1, 12)}),
        [this](const PropertyList& properties) -> ToolResult {
            std::string action = properties["action"].value<std::string>();
            if (action == "start") {
                if (phase_ != kPomodoroOff) {
                    Stop();
                }
                focus_s_ = properties["focus_minutes"].value<int>() * 60;
                short_s_ = properties["break_minutes"].value<int>() * 60;
                long_s_ = properties["long_break_minutes"].value<int>() * 60;
                cycles_ = properties["rounds"].value<int>();
                cycle_ = 1;
                EnterPhase(kPomodoroFocus);
            } else if (action == "status") {
                // Nothing to change.
            } else if (phase_ == kPomodoroOff) {
                return std::unexpected("No pomodoro is running");
            } else if (action == "pause") {
                if (!paused_) {
                    BankStudy();
                    StopRunning();
                    UpdateDisplay();
                }
            } else if (action == "resume") {
                if (paused_) {
                    StartRunning();
                    UpdateDisplay();
                }
            } else if (action == "stop") {
                Stop();
            } else {
                return std::unexpected("Unknown action; use start, pause, resume, stop or status");
            }
            return StatusJson();
        });
}
