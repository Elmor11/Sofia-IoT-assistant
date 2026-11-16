#include "telegram_bot.h"

#include "application.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "sdkconfig.h"

#if CONFIG_POMODORO_TIMER
#include "pomodoro/pomodoro.h"
#endif

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/semphr.h"

namespace {

constexpr char kTag[] = "TelegramBot";
constexpr size_t kMaxHttpResponseBytes = 16 * 1024;
constexpr TickType_t kPollInterval = pdMS_TO_TICKS(1000);
constexpr int kHttpTimeoutMs = 15000;
constexpr uint32_t kTaskStackSize = 6144;
constexpr UBaseType_t kTaskPriority = 3;
constexpr size_t kMaxPendingResponses = 4;
constexpr size_t kMaxTelegramResponseBytes = 1024;
constexpr size_t kMaxResponsesPerPoll = 2;

StaticSemaphore_t s_lifecycle_mutex_storage;
SemaphoreHandle_t s_lifecycle_mutex = nullptr;
portMUX_TYPE s_lifecycle_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

struct HttpResponse {
    std::string body;
    bool too_large = false;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    auto* response = static_cast<HttpResponse*>(event->user_data);
    const size_t data_len = static_cast<size_t>(event->data_len);
    if (data_len > kMaxHttpResponseBytes - response->body.size()) {
        response->too_large = true;
        return ESP_FAIL;
    }

    response->body.append(static_cast<const char*>(event->data), data_len);
    return ESP_OK;
}

bool PerformRequest(const std::string& url, esp_http_client_method_t method,
                    const char* payload, HttpResponse* response) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = kHttpTimeoutMs;
    config.event_handler = HttpEventHandler;
    config.user_data = response;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(kTag, "No se pudo crear el cliente HTTP");
        return false;
    }

    esp_http_client_set_method(client, method);
    if (payload != nullptr) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, payload, static_cast<int>(strlen(payload)));
    }

    const esp_err_t result = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (response->too_large) {
        ESP_LOGW(kTag, "Respuesta de Telegram demasiado grande");
        return false;
    }
    if (result != ESP_OK || status_code < 200 || status_code >= 300) {
        ESP_LOGW(kTag, "Solicitud a Telegram fallo");
        return false;
    }

    return true;
}

bool IsSuccessfulTelegramResponse(const std::string& response);

bool SendMessage(const std::string& api_base, const std::string& chat_id,
                 const std::string& message) {
    cJSON* payload = cJSON_CreateObject();
    if (payload == nullptr) {
        ESP_LOGE(kTag, "No se pudo crear la respuesta de Telegram");
        return false;
    }
    if (cJSON_AddStringToObject(payload, "chat_id", chat_id.c_str()) == nullptr ||
        cJSON_AddStringToObject(payload, "text", message.c_str()) == nullptr) {
        cJSON_Delete(payload);
        ESP_LOGE(kTag, "No se pudo crear la respuesta de Telegram");
        return false;
    }

    char* serialized_payload = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (serialized_payload == nullptr) {
        ESP_LOGE(kTag, "No se pudo serializar la respuesta de Telegram");
        return false;
    }

    HttpResponse response;
    const bool sent =
        PerformRequest(api_base + "sendMessage", HTTP_METHOD_POST, serialized_payload, &response);
    cJSON_free(serialized_payload);
    return sent && IsSuccessfulTelegramResponse(response.body);
}

bool IsSuccessfulTelegramResponse(const std::string& response) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_ParseWithLength(response.c_str(), response.size()), cJSON_Delete);
    const cJSON* ok = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root.get(), "ok");
    return cJSON_IsTrue(ok);
}

std::string ChatIdToString(const cJSON* chat_id) {
    if (!cJSON_IsNumber(chat_id)) {
        return {};
    }

    char value[24];
    std::snprintf(value, sizeof(value), "%.0f", chat_id->valuedouble);
    return value;
}

int JsonInt(const cJSON* object, const char* name, int default_value) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(value) ? value->valueint : default_value;
}

std::string FormatPomodoroStatus(const std::string& status) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_ParseWithLength(status.c_str(), status.size()), cJSON_Delete);
    if (root == nullptr) {
        return "No se pudo obtener el estado de Pomodoro.";
    }

    const cJSON* phase = cJSON_GetObjectItemCaseSensitive(root.get(), "phase");
    if (!cJSON_IsString(phase)) {
        return "No se pudo obtener el estado de Pomodoro.";
    }

    const char* phase_name = phase->valuestring;
    if (strcmp(phase_name, "off") == 0) {
        return "Pomodoro detenido.\nEstudio hoy: " +
               std::to_string(JsonInt(root.get(), "today_min", 0)) + " min.";
    }

    const char* translated_phase = strcmp(phase_name, "focus") == 0
                                       ? "estudio"
                                       : strcmp(phase_name, "short_break") == 0
                                             ? "descanso corto"
                                             : "descanso largo";
    const int left_seconds = JsonInt(root.get(), "left_s", 0);
    const int round = JsonInt(root.get(), "round", 0);
    const int rounds = JsonInt(root.get(), "rounds", 0);
    const bool paused = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(), "paused"));

    char remaining[16];
    std::snprintf(remaining, sizeof(remaining), "%d:%02d", left_seconds / 60,
                  left_seconds % 60);
    std::string result = "Pomodoro: ";
    result += translated_phase;
    if (paused) {
        result += " (en pausa)";
    }
    result += "\nRonda: " + std::to_string(round) + "/" + std::to_string(rounds);
    result += "\nRestante: ";
    result += remaining;
    result += "\nEstudio hoy: " + std::to_string(JsonInt(root.get(), "today_min", 0)) + " min.";
    return result;
}

SemaphoreHandle_t GetLifecycleMutex() {
    portENTER_CRITICAL(&s_lifecycle_mutex_init_lock);
    if (s_lifecycle_mutex == nullptr) {
        s_lifecycle_mutex = xSemaphoreCreateMutexStatic(&s_lifecycle_mutex_storage);
    }
    SemaphoreHandle_t mutex = s_lifecycle_mutex;
    portEXIT_CRITICAL(&s_lifecycle_mutex_init_lock);
    return mutex;
}

}  // namespace

TelegramBot& TelegramBot::GetInstance() {
    static TelegramBot instance;
    return instance;
}

bool TelegramBot::Start() {
    SemaphoreHandle_t lifecycle_mutex = GetLifecycleMutex();
    xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
    if (task_ == nullptr) {
        running_.store(true);
        const BaseType_t created = xTaskCreate(TaskEntry, "telegram_bot", kTaskStackSize, this,
                                               kTaskPriority, &task_);
        if (created != pdPASS) {
            running_.store(false);
            task_ = nullptr;
        }
        xSemaphoreGive(lifecycle_mutex);

        if (created != pdPASS) {
            ESP_LOGE(kTag, "No se pudo crear la tarea de Telegram");
            return false;
        }

        ESP_LOGI(kTag, "Modulo Telegram iniciado");
        return true;
    }

    if (running_.load()) {
        xSemaphoreGive(lifecycle_mutex);
        ESP_LOGW(kTag, "El bot ya esta iniciado");
        return true;
    }

    running_.store(true);
    xTaskNotifyGive(task_);
    xSemaphoreGive(lifecycle_mutex);

    ESP_LOGI(kTag, "Modulo Telegram reanudado");
    return true;
}

void TelegramBot::Stop() {
    SemaphoreHandle_t lifecycle_mutex = GetLifecycleMutex();
    xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
    const bool running = running_.exchange(false);
    xSemaphoreGive(lifecycle_mutex);

    if (!running) {
        return;
    }

    ESP_LOGI(kTag, "Detencion del modulo Telegram solicitada");
}

bool TelegramBot::IsRunning() const {
    return running_.load();
}

void TelegramBot::QueueResponse(std::string response) {
    if (response.size() > kMaxTelegramResponseBytes) {
        response.resize(kMaxTelegramResponseBytes);
    }

    std::lock_guard<std::mutex> lock(response_mutex_);
    if (pending_responses_.size() == kMaxPendingResponses) {
        ESP_LOGW(kTag, "Cola de respuestas de Telegram llena");
        return;
    }
    pending_responses_.push_back(std::move(response));
}

bool TelegramBot::PopResponse(std::string* response) {
    std::lock_guard<std::mutex> lock(response_mutex_);
    if (pending_responses_.empty()) {
        return false;
    }
    *response = std::move(pending_responses_.front());
    pending_responses_.pop_front();
    return true;
}

void TelegramBot::TaskEntry(void* parameter) {
    auto* bot = static_cast<TelegramBot*>(parameter);
    while (true) {
        ulTaskNotifyTake(pdTRUE, 0);
        bot->Run();

        SemaphoreHandle_t lifecycle_mutex = GetLifecycleMutex();
        xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
        const bool restarted = bot->running_.load();
        xSemaphoreGive(lifecycle_mutex);

        if (restarted) {
            ulTaskNotifyTake(pdTRUE, 0);
            continue;
        }

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

void TelegramBot::Run() {
    const std::string token = CONFIG_SOFIA_TELEGRAM_TOKEN;
    const std::string authorized_chat_id = CONFIG_SOFIA_TELEGRAM_CHAT_ID;
    if (token.empty() || authorized_chat_id.empty()) {
        ESP_LOGE(kTag, "La configuracion de Telegram no esta completa");
        return;
    }

    const std::string api_base = "https://api.telegram.org/bot" + token + "/";
    while (running_.load()) {
        for (size_t i = 0; i < kMaxResponsesPerPoll; ++i) {
            std::string response;
            if (!PopResponse(&response)) {
                break;
            }
            if (!SendMessage(api_base, authorized_chat_id, response)) {
                ESP_LOGW(kTag, "No se pudo enviar la respuesta de Telegram");
            }
        }

        HttpResponse response;
        const std::string url = api_base + "getUpdates?timeout=10&offset=" +
                                std::to_string(last_update_id_ + 1);
        if (!PerformRequest(url, HTTP_METHOD_GET, nullptr, &response)) {
            if (running_.load()) {
                vTaskDelay(kPollInterval);
            }
            continue;
        }

        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
            cJSON_ParseWithLength(response.body.c_str(), response.body.size()), cJSON_Delete);
        const cJSON* ok = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root.get(), "ok");
        const cJSON* updates = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root.get(), "result");
        if (!cJSON_IsTrue(ok) || !cJSON_IsArray(updates)) {
            ESP_LOGW(kTag, "Respuesta getUpdates invalida");
            vTaskDelay(kPollInterval);
            continue;
        }

        const cJSON* update = nullptr;
        cJSON_ArrayForEach(update, updates) {
            if (!running_.load()) {
                break;
            }

            const cJSON* update_id = cJSON_GetObjectItemCaseSensitive(update, "update_id");
            if (!cJSON_IsNumber(update_id)) {
                continue;
            }

            const int64_t current_update_id = static_cast<int64_t>(update_id->valuedouble);
            if (current_update_id <= last_update_id_) {
                continue;
            }
            last_update_id_ = current_update_id;

            const cJSON* message = cJSON_GetObjectItemCaseSensitive(update, "message");
            const cJSON* chat = message == nullptr ? nullptr :
                cJSON_GetObjectItemCaseSensitive(message, "chat");
            const cJSON* chat_id = chat == nullptr ? nullptr :
                cJSON_GetObjectItemCaseSensitive(chat, "id");
            const cJSON* text = message == nullptr ? nullptr :
                cJSON_GetObjectItemCaseSensitive(message, "text");
            if (!cJSON_IsString(text) || ChatIdToString(chat_id) != authorized_chat_id) {
                continue;
            }

            const std::string reply = ProcessCommand(text->valuestring);
            if (!reply.empty() && !SendMessage(api_base, authorized_chat_id, reply)) {
                ESP_LOGW(kTag, "No se pudo enviar la respuesta de Telegram");
            }
        }
    }
}

std::string TelegramBot::ProcessCommand(const std::string& command) {
    if (command == "/start") {
        return "Bienvenido a Sofia IoT.\n"
               "Comandos disponibles:\n"
               "/start\n"
               "/estado\n"
               "/ayuda\n"
               "/pomodoro";
    }

    if (command == "/estado") {
        return "Modulo Telegram inicializado. "
               "Estado real de Sofia pendiente.";
    }

    if (command == "/ayuda") {
        return "Comandos disponibles:\n"
               "/start\n"
               "/estado\n"
               "/ayuda\n"
               "/pomodoro";
    }

#if CONFIG_POMODORO_TIMER
    std::string pomodoro_action;
    if (command == "/pomodoro iniciar") {
        pomodoro_action = "start";
    } else if (command == "/pomodoro pausar") {
        pomodoro_action = "pause";
    } else if (command == "/pomodoro reanudar") {
        pomodoro_action = "resume";
    } else if (command == "/pomodoro detener") {
        pomodoro_action = "stop";
    } else if (command == "/pomodoro estado") {
        pomodoro_action = "status";
    } else if (command == "/pomodoro" || command.rfind("/pomodoro ", 0) == 0) {
        return "Usa /pomodoro iniciar, pausar, reanudar, detener o estado.";
    }

    if (!pomodoro_action.empty()) {
        Application::GetInstance().Schedule([this, action = std::move(pomodoro_action)]() {
            const auto result = Pomodoro::GetInstance().ExecuteAction(action);
            QueueResponse(result.success ? FormatPomodoroStatus(result.status)
                                         : "Pomodoro: " + result.status);
        });
        return {};
    }
#else
    if (command == "/pomodoro" || command.rfind("/pomodoro ", 0) == 0) {
        return "La funcion Pomodoro no esta disponible.";
    }
#endif

    return "Comando no reconocido. Usa /ayuda.";
}
