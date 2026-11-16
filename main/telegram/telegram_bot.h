
#ifndef TELEGRAM_BOT_H
#define TELEGRAM_BOT_H

#include <atomic>
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
};

#endif
