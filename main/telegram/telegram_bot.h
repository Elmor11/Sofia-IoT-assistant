
#ifndef TELEGRAM_BOT_H
#define TELEGRAM_BOT_H

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class TelegramBot {
public:
    static TelegramBot& GetInstance();

    bool Start();
    void Stop();
    bool IsRunning() const;

    TelegramBot() = default;

    TelegramBot(const TelegramBot&) = delete;
    TelegramBot& operator=(const TelegramBot&) = delete;

    // Tarea independiente de Telegram
    static void TaskEntry(void* parameter);
    void Run();

    // Procesamiento de comandos
    std::string ProcessCommand(
        const std::string& command
    );

    // Estado interno
    std::atomic<bool> running_{false};
    TaskHandle_t task_ = nullptr;
    int64_t last_update_id_ = 0;
    std::atomic<uint8_t> pending_pomodoro_requests_{0};
    std::mutex response_mutex_;
    std::deque<std::string> pending_responses_;

    void QueueResponse(std::string response);
    bool PopResponse(std::string* response);
};

#endif
