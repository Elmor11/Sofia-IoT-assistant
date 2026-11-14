#ifndef POMODORO_H
#define POMODORO_H

#include <esp_timer.h>

#include <cstdint>
#include <string>

// Pomodoro study timer: focus / short break / long break cycles driven by voice (MCP tools),
// shown on the display as a gauge, with a short chime at every phase change. Focus time is added
// to a 7-day study history kept in NVS as one small packed blob.
//
// All state lives on the main task: the MCP tools already run there and the timers only
// schedule work onto it.
class Pomodoro {
public:
    static Pomodoro& GetInstance() {
        static Pomodoro instance;
        return instance;
    }

    void RegisterTools();

private:
    // Packed so the NVS blob layout is stable; it is persistent data. Bump kVersion when it changes.
    struct __attribute__((packed)) History {
        uint8_t version;
        uint32_t day;           // local day number (days since 1970) of index 0
        uint16_t minutes[7];    // study minutes, [0] = `day`, [1] = the day before, ...
        uint8_t sessions[7];    // completed focus phases per day, same indexing
        uint32_t total_minutes; // all-time study minutes
    };
    static constexpr uint8_t kHistoryVersion = 1;

    Pomodoro();

    int phase_ = 0;  // PomodoroPhase (display.h)
    bool paused_ = false;
    int cycle_ = 0;   // 1-based focus phase number within the set
    int cycles_ = 4;  // focus phases before the long break
    int focus_s_ = 25 * 60;
    int short_s_ = 5 * 60;
    int long_s_ = 15 * 60;
    int64_t phase_total_ms_ = 0;
    int64_t run_start_us_ = 0;     // esp_timer time when the phase last (re)started running
    int64_t elapsed_before_ms_ = 0;  // elapsed time accumulated before run_start_us_
    int64_t banked_ms_ = 0;          // focus time of this phase already added to the history
    int unbanked_s_ = 0;             // study seconds below one minute, not stored yet

    History history_ = {};
    bool history_loaded_ = false;

    esp_timer_handle_t phase_timer_ = nullptr;  // fires when the current phase ends
    esp_timer_handle_t bank_timer_ = nullptr;   // banks study time every few minutes

    int64_t ElapsedMs() const;
    void EnterPhase(int phase);
    void OnPhaseEnd();
    void StartRunning();
    void StopRunning();
    void Stop();
    void BankStudy();
    void AddStudySeconds(int seconds);
    void PlayChime(bool to_break);
    void UpdateDisplay();
    void LoadHistory();
    void SaveHistory();
    void RollHistory();
    std::string StatusJson();
};

#endif  // POMODORO_H
