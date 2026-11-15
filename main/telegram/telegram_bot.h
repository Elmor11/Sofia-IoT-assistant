
#ifndef TELEGRAM_BOT_H
#define TELEGRAM_BOT_H

#include <string>
#include <cstdint>

class TelegramBot {
public:
    // Obtener instancia unica
    static TelegramBot& GetInstance();

    // Iniciar el modulo de Telegram
    bool Start();

    // Detener el modulo
    void Stop();

    // Consultar estado del bot
    bool IsRunning() const;

private:
    TelegramBot() = default;

    // Evitar copias de la instancia
    TelegramBot(const TelegramBot&) = delete;
    TelegramBot& operator=(const TelegramBot&) = delete;

    // Procesar comandos recibidos
    std::string ProcessCommand(
        const std::string& command
    );

    // Estado interno
    bool running_ = false;
};

#endif
