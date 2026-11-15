
#include "telegram_bot.h"

#include <esp_log.h>

static const char* TAG = "TelegramBot";

// Obtener instancia unica
TelegramBot& TelegramBot::GetInstance() {
    static TelegramBot instance;
    return instance;
}

// Iniciar el modulo
bool TelegramBot::Start() {

    if (running_) {
        ESP_LOGW(TAG, "El bot ya esta iniciado");
        return true;
    }

    ESP_LOGI(TAG, "Iniciando modulo Telegram");

    // Pendiente: iniciar conexion HTTPS
    // y tarea de recepcion de mensajes.

    running_ = true;

    ESP_LOGI(TAG, "Modulo Telegram inicializado");

    return true;
}

// Detener el modulo
void TelegramBot::Stop() {

    if (!running_) {
        return;
    }

    running_ = false;

    ESP_LOGI(TAG, "Modulo Telegram detenido");
}

// Consultar estado del modulo
bool TelegramBot::IsRunning() const {
    return running_;
}

// Procesar comandos
std::string TelegramBot::ProcessCommand(
    const std::string& command) {

    if (command == "/start") {
        return "Bienvenido a Sofia IoT.\n"
               "Comandos disponibles:\n"
               "/start\n"
               "/estado\n"
               "/ayuda";
    }

    if (command == "/estado") {
        return "Modulo Telegram inicializado. "
               "Estado real de Sofia pendiente.";
    }

    if (command == "/ayuda") {
        return "Comandos disponibles:\n"
               "/start\n"
               "/estado\n"
               "/ayuda";
    }

    return "Comando no reconocido. Usa /ayuda.";
}
