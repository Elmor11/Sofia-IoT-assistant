"""Host tests for Telegram command routing and the Pomodoro scheduling quota."""

import os
import pathlib
import shlex
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
TELEGRAM_SOURCE = ROOT / "main" / "telegram" / "telegram_bot.cc"


def create_host_temp_directory():
    return tempfile.TemporaryDirectory(prefix=".telegram-host-", dir=ROOT / "scripts" / "tests")


def extract_function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def find_compiler():
    configured = shlex.split(os.environ.get("CXX", ""))
    candidates = [configured] if configured else []
    candidates.extend([["c++"], ["g++"], ["clang++"], ["cl"]])
    if os.name == "nt":
        candidates.extend([
            [r"C:\msys64\ucrt64\bin\g++.exe"],
            [r"C:\msys64\mingw64\bin\g++.exe"],
        ])
    for candidate in candidates:
        if candidate:
            compiler = shutil.which(candidate[0])
            if compiler:
                return [compiler, *candidate[1:]]
    return None


@unittest.skipUnless(find_compiler(), "requires a C++17 host compiler")
class TelegramPomodoroTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = find_compiler()
        cls.tmp = create_host_temp_directory()
        cls.addClassCleanup(cls.tmp.cleanup)
        build_dir = pathlib.Path(cls.tmp.name)
        source = TELEGRAM_SOURCE.read_text(encoding="utf-8")
        process_command = extract_function(source, "std::string TelegramBot::ProcessCommand")
        driver = build_dir / "telegram_pomodoro_driver.cc"
        driver.write_text(textwrap.dedent(r'''
            #include <atomic>
            #include <cassert>
            #include <cstdint>
            #include <deque>
            #include <functional>
            #include <string>
            #include <utility>
            #include <vector>

            #define CONFIG_POMODORO_TIMER 1

            constexpr uint8_t kMaxPendingPomodoroRequests = 2;

            class Application {
            public:
                static Application& GetInstance() {
                    static Application instance;
                    return instance;
                }

                void Schedule(std::function<void()>&& callback) {
                    pending.push_back(std::move(callback));
                }

                void Drain() {
                    auto callbacks = std::move(pending);
                    pending.clear();
                    for (auto& callback : callbacks) {
                        callback();
                    }
                }

                std::vector<std::function<void()>> pending;
            };

            class Pomodoro {
            public:
                struct ActionResult {
                    bool success;
                    std::string status;
                };

                static Pomodoro& GetInstance() {
                    static Pomodoro instance;
                    return instance;
                }

                ActionResult ExecuteAction(const std::string& action) {
                    actions.push_back(action);
                    return {true, "{\"phase\":\"focus\"}"};
                }

                std::vector<std::string> actions;
            };

            std::string FormatPomodoroStatus(const std::string& status) {
                return "formatted:" + status;
            }

            class TelegramBot {
            public:
                std::string ProcessCommand(const std::string& command);

                void QueueResponse(std::string response) {
                    pending_responses.push_back(std::move(response));
                }

                std::atomic<uint8_t> pending_pomodoro_requests_{0};
                std::atomic<bool> running_{true};
                std::deque<std::string> pending_responses;
            };
        ''') + "\n" + process_command + textwrap.dedent(r'''
            void Reset() {
                Application::GetInstance().pending.clear();
                Pomodoro::GetInstance().actions.clear();
            }

            void AssertPomodoroCommand(const std::string& command, const std::string& action) {
                Reset();
                TelegramBot bot;
                assert(bot.ProcessCommand(command).empty());
                assert(Application::GetInstance().pending.size() == 1);
                assert(Pomodoro::GetInstance().actions.empty());
                Application::GetInstance().Drain();
                assert(Pomodoro::GetInstance().actions == std::vector<std::string>{action});
                assert(bot.pending_pomodoro_requests_.load() == 0);
                assert(bot.pending_responses.size() == 1);
                assert(bot.pending_responses.front() == "formatted:{\"phase\":\"focus\"}");
            }

            int main(int argc, char** argv) {
                assert(argc == 2);
                const std::string scenario = argv[1];
                TelegramBot bot;

                if (scenario == "basic") {
                    assert(bot.ProcessCommand("/start").find("Bienvenido") != std::string::npos);
                    assert(bot.ProcessCommand("/estado").find("Modulo Telegram") != std::string::npos);
                    assert(bot.ProcessCommand("/ayuda").find("/pomodoro") != std::string::npos);
                    assert(bot.ProcessCommand("/desconocido") ==
                           "Comando no reconocido. Usa /ayuda.");
                    assert(Application::GetInstance().pending.empty());
                    return 0;
                }
                if (scenario == "pomodoro_actions") {
                    AssertPomodoroCommand("/pomodoro iniciar", "start");
                    AssertPomodoroCommand("/pomodoro pausar", "pause");
                    AssertPomodoroCommand("/pomodoro reanudar", "resume");
                    AssertPomodoroCommand("/pomodoro detener", "stop");
                    AssertPomodoroCommand("/pomodoro estado", "status");
                    return 0;
                }
                if (scenario == "quota") {
                    Reset();
                    assert(bot.ProcessCommand("/pomodoro iniciar").empty());
                    assert(bot.ProcessCommand("/pomodoro estado").empty());
                    assert(bot.pending_pomodoro_requests_.load() == 2);
                    assert(Application::GetInstance().pending.size() == 2);
                    assert(bot.ProcessCommand("/pomodoro pausar") ==
                           "Pomodoro esta ocupado. Intentalo de nuevo en unos segundos.");
                    assert(Application::GetInstance().pending.size() == 2);

                    bot.running_.store(false);
                    Application::GetInstance().Drain();
                    assert(bot.pending_pomodoro_requests_.load() == 0);
                    assert(Pomodoro::GetInstance().actions.size() == 2);

                    assert(bot.ProcessCommand("/pomodoro pausar").empty());
                    assert(bot.pending_pomodoro_requests_.load() == 1);
                    Application::GetInstance().Drain();
                    assert(bot.pending_pomodoro_requests_.load() == 0);
                    assert(Pomodoro::GetInstance().actions.size() == 3);
                    return 0;
                }
                return 1;
            }
        '''), encoding="utf-8")

        cls.executable = build_dir / "telegram_command_host_test"
        if cls.compiler[0].lower() == "cl":
            command = cls.compiler + ["/nologo", "/std:c++17", "/EHsc", str(driver),
                                      f"/Fe{cls.executable}.exe"]
            cls.executable = cls.executable.with_suffix(".exe")
        else:
            command = cls.compiler + ["-std=c++17", "-fno-exceptions", str(driver),
                                      "-o", str(cls.executable)]
        compilation_env = os.environ.copy()
        compiler_directory = str(pathlib.Path(cls.compiler[0]).parent)
        compilation_env["PATH"] = compiler_directory + os.pathsep + compilation_env["PATH"]
        cls.execution_env = compilation_env
        compilation = subprocess.run(command, capture_output=True, text=True, env=compilation_env)
        if compilation.returncode:
            raise RuntimeError(
                "Failed to compile Telegram/Pomodoro host harness.\n"
                f"Command: {shlex.join(command)}\n"
                f"stdout:\n{compilation.stdout}\n"
                f"stderr:\n{compilation.stderr}"
            )

    def run_scenario(self, scenario):
        command = [str(self.executable), scenario]
        result = subprocess.run(command, capture_output=True, text=True, env=self.execution_env)
        if result.returncode:
            self.fail(
                "Telegram/Pomodoro host scenario failed.\n"
                f"Command: {shlex.join(command)}\n"
                f"exit code: {result.returncode}\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )

    def test_general_commands_and_unknown_command(self):
        self.run_scenario("basic")

    def test_all_pomodoro_commands_schedule_production_router(self):
        self.run_scenario("pomodoro_actions")

    def test_pomodoro_pending_request_quota_is_released(self):
        self.run_scenario("quota")


@unittest.skipUnless(find_compiler(), "requires a C++17 host compiler")
class TelegramJsonProcessingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = find_compiler()
        cls.tmp = create_host_temp_directory()
        cls.addClassCleanup(cls.tmp.cleanup)
        build_dir = pathlib.Path(cls.tmp.name)
        source = TELEGRAM_SOURCE.read_text(encoding="utf-8")
        chat_id_to_string = extract_function(source, "std::string ChatIdToString")
        process_updates = extract_function(source, "bool ProcessUpdatesResponse")
        http_response = extract_function(source, "struct HttpResponse") + ";"
        http_event_handler = extract_function(source, "esp_err_t HttpEventHandler")
        driver = build_dir / "telegram_json_driver.cc"
        driver.write_text(textwrap.dedent(r'''
            #include <atomic>
            #include <cassert>
            #include <cstdio>
            #include <cstdint>
            #include <memory>
            #include <string>
            #include <vector>

            #include <cjson/cJSON.h>

            #define ESP_LOGW(...) do {} while (false)

            using esp_err_t = int;
            constexpr esp_err_t ESP_OK = 0;
            constexpr esp_err_t ESP_FAIL = -1;
            constexpr size_t kMaxHttpResponseBytes = 16 * 1024;

            enum esp_http_client_event_id_t {
                HTTP_EVENT_ERROR,
                HTTP_EVENT_ON_DATA,
            };

            struct esp_http_client_event_t {
                esp_http_client_event_id_t event_id;
                void* user_data;
                void* data;
                int data_len;
            };

            class TelegramBot {
            public:
                std::atomic<bool> running_{true};
                int64_t last_update_id_ = 0;
                std::vector<std::string> commands;
                int scheduled_pomodoro_operations = 0;

                std::string ProcessCommand(const std::string& command) {
                    commands.push_back(command);
                    if (command.rfind("/pomodoro ", 0) == 0) {
                        ++scheduled_pomodoro_operations;
                        return {};
                    }
                    return "reply:" + command;
                }
            };

            std::vector<std::string> sent_messages;

            bool SendMessage(const std::string&, const std::string&, const std::string& message) {
                sent_messages.push_back(message);
                return true;
            }
        ''') + "\n" + http_response + "\n" + http_event_handler + "\n" +
                          chat_id_to_string + "\n" + process_updates + textwrap.dedent(r'''
            void Reset() {
                sent_messages.clear();
            }

            bool Process(TelegramBot& bot, const std::string& json) {
                return ProcessUpdatesResponse(bot, json, "https://example.invalid/", "9001");
            }

            int main(int argc, char** argv) {
                assert(argc == 2);
                const std::string scenario = argv[1];
                TelegramBot bot;
                Reset();

                if (scenario == "authorized") {
                    assert(Process(bot,
                        R"({"ok":true,"result":[{"update_id":101,"message":{"chat":{"id":9001},"text":"/pomodoro iniciar"}}]})"));
                    assert(bot.commands == std::vector<std::string>{"/pomodoro iniciar"});
                    assert(bot.scheduled_pomodoro_operations == 1);
                    assert(bot.last_update_id_ == 101);
                    assert(sent_messages.empty());
                    return 0;
                }
                if (scenario == "unauthorized") {
                    assert(Process(bot,
                        R"({"ok":true,"result":[{"update_id":102,"message":{"chat":{"id":9999},"text":"/pomodoro iniciar"}}]})"));
                    assert(bot.commands.empty());
                    assert(bot.scheduled_pomodoro_operations == 0);
                    assert(bot.last_update_id_ == 102);
                    assert(sent_messages.empty());
                    return 0;
                }
                if (scenario == "invalid_response") {
                    assert(!Process(bot, R"({"ok":true,"result":[)"));
                    assert(!Process(bot, R"({"ok":false,"result":[]})"));
                    assert(!Process(bot, R"({"ok":true})"));
                    assert(!Process(bot, R"({"ok":true,"result":{}})"));
                    assert(bot.commands.empty());
                    assert(bot.last_update_id_ == 0);
                    return 0;
                }
                if (scenario == "malformed_updates") {
                    assert(Process(bot, R"({"ok":true,"result":[
                        {},
                        {"update_id":"11","message":{"chat":{"id":9001},"text":"/start"}},
                        {"update_id":12},
                        {"update_id":13,"message":{}},
                        {"update_id":14,"message":{"chat":{"id":"9001"},"text":"/start"}},
                        {"update_id":15,"message":{"chat":{"id":9001},"text":42}}
                    ]})"));
                    assert(bot.commands.empty());
                    assert(bot.scheduled_pomodoro_operations == 0);
                    assert(bot.last_update_id_ == 15);
                    assert(sent_messages.empty());
                    return 0;
                }
                if (scenario == "duplicates_and_offset") {
                    bot.last_update_id_ = 30;
                    assert(Process(bot, R"({"ok":true,"result":[
                        {"update_id":30,"message":{"chat":{"id":9001},"text":"/start"}},
                        {"update_id":31,"message":{"chat":{"id":9999},"text":"/pomodoro iniciar"}},
                        {"update_id":32,"message":{"chat":{"id":9001},"text":"/estado"}},
                        {"update_id":32,"message":{"chat":{"id":9001},"text":"/pomodoro iniciar"}}
                    ]})"));
                    assert(bot.commands == std::vector<std::string>{"/estado"});
                    assert(bot.scheduled_pomodoro_operations == 0);
                    assert(bot.last_update_id_ == 32);
                    assert(sent_messages == std::vector<std::string>{"reply:/estado"});
                    return 0;
                }
                if (scenario == "response_limit") {
                    HttpResponse response;
                    std::string chunk(kMaxHttpResponseBytes, 'x');
                    esp_http_client_event_t event = {
                        HTTP_EVENT_ON_DATA, &response, chunk.data(), static_cast<int>(chunk.size())
                    };
                    assert(HttpEventHandler(&event) == ESP_OK);
                    assert(response.body.size() == kMaxHttpResponseBytes);
                    event.data = chunk.data();
                    event.data_len = 1;
                    assert(HttpEventHandler(&event) == ESP_FAIL);
                    assert(response.too_large);
                    return 0;
                }
                return 1;
            }
        '''), encoding="utf-8")

        cls.executable = build_dir / "telegram_json_host_test"
        if cls.compiler[0].lower() == "cl":
            raise unittest.SkipTest("the JSON harness currently requires a GCC-compatible cJSON library")
        command = cls.compiler + [
            "-std=c++17", "-fno-exceptions", str(driver),
            "-IC:\\msys64\\ucrt64\\include",
            "-LC:\\msys64\\ucrt64\\lib", "-lcjson",
            "-o", str(cls.executable),
        ]
        compilation_env = os.environ.copy()
        compiler_directory = str(pathlib.Path(cls.compiler[0]).parent)
        compilation_env["PATH"] = compiler_directory + os.pathsep + compilation_env["PATH"]
        cls.execution_env = compilation_env
        compilation = subprocess.run(command, capture_output=True, text=True, env=compilation_env)
        if compilation.returncode:
            raise RuntimeError(
                "Failed to compile Telegram JSON host harness.\n"
                f"Command: {shlex.join(command)}\n"
                f"stdout:\n{compilation.stdout}\n"
                f"stderr:\n{compilation.stderr}"
            )

    def run_scenario(self, scenario):
        command = [str(self.executable), scenario]
        result = subprocess.run(command, capture_output=True, text=True, env=self.execution_env)
        if result.returncode:
            self.fail(
                "Telegram JSON host scenario failed.\n"
                f"Command: {shlex.join(command)}\n"
                f"exit code: {result.returncode}\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )

    def test_authorized_and_unauthorized_chat_ids(self):
        self.run_scenario("authorized")
        self.run_scenario("unauthorized")

    def test_invalid_or_incomplete_get_updates_responses(self):
        self.run_scenario("invalid_response")
        self.run_scenario("malformed_updates")

    def test_duplicate_updates_maintain_offset_and_http_limit(self):
        self.run_scenario("duplicates_and_offset")
        self.run_scenario("response_limit")


@unittest.skipUnless(find_compiler(), "requires a C++17 host compiler")
class TelegramLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = find_compiler()
        cls.tmp = create_host_temp_directory()
        cls.addClassCleanup(cls.tmp.cleanup)
        build_dir = pathlib.Path(cls.tmp.name)
        source = TELEGRAM_SOURCE.read_text(encoding="utf-8")
        lifecycle_mutex = extract_function(source, "SemaphoreHandle_t GetLifecycleMutex")
        start = extract_function(source, "bool TelegramBot::Start")
        stop = extract_function(source, "void TelegramBot::Stop")
        task_entry = extract_function(source, "void TelegramBot::TaskEntry")
        run = extract_function(source, "void TelegramBot::Run")
        driver = build_dir / "telegram_lifecycle_driver.cc"
        driver.write_text(textwrap.dedent(r'''
            #include <atomic>
            #include <cassert>
            #include <csetjmp>
            #include <cstdint>
            #include <string>

            #define ESP_LOGE(...) do {} while (false)
            #define ESP_LOGI(...) do {} while (false)
            #define ESP_LOGW(...) do {} while (false)
            #define CONFIG_SOFIA_TELEGRAM_TOKEN "host-test-token"
            #define CONFIG_SOFIA_TELEGRAM_CHAT_ID "9001"

            using BaseType_t = int;
            using UBaseType_t = unsigned;
            using TickType_t = uint32_t;
            constexpr BaseType_t pdPASS = 1;
            constexpr BaseType_t pdTRUE = 1;
            constexpr TickType_t portMAX_DELAY = UINT32_MAX;
            constexpr TickType_t kPollInterval = 1;
            constexpr uint32_t kTaskStackSize = 6144;
            constexpr UBaseType_t kTaskPriority = 3;
            constexpr size_t kMaxResponsesPerPoll = 2;

            struct FakeTask {
                void (*entry)(void*) = nullptr;
                void* parameter = nullptr;
                unsigned notifications = 0;
                bool waiting = false;
            };
            using TaskHandle_t = FakeTask*;
            using SemaphoreHandle_t = int*;
            struct StaticSemaphore_t {};
            using portMUX_TYPE = int;

            constexpr int portMUX_INITIALIZER_UNLOCKED = 0;
            StaticSemaphore_t s_lifecycle_mutex_storage;
            SemaphoreHandle_t s_lifecycle_mutex = nullptr;
            portMUX_TYPE s_lifecycle_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

            FakeTask created_task;
            FakeTask* current_task = nullptr;
            int create_count = 0;
            int notify_count = 0;
            int wake_count = 0;
            int wait_count = 0;
            int http_call_count = 0;
            bool http_request_active = false;
            std::jmp_buf wait_boundary;
            bool can_leave_wait = false;

            #define portENTER_CRITICAL(lock) do {} while (false)
            #define portEXIT_CRITICAL(lock) do {} while (false)

            SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*) {
                static int mutex;
                return &mutex;
            }
            BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdPASS; }
            BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdPASS; }

            BaseType_t xTaskCreate(void (*entry)(void*), const char*, uint32_t, void* parameter,
                                   UBaseType_t, TaskHandle_t* task) {
                ++create_count;
                created_task.entry = entry;
                created_task.parameter = parameter;
                *task = &created_task;
                return pdPASS;
            }
            BaseType_t xTaskNotifyGive(TaskHandle_t task) {
                ++notify_count;
                ++task->notifications;
                if (task->waiting) {
                    task->waiting = false;
                    ++wake_count;
                }
                return pdPASS;
            }
            uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t timeout) {
                assert(current_task != nullptr);
                if (current_task->notifications != 0) {
                    const unsigned received = current_task->notifications;
                    current_task->notifications = clear_on_exit ? 0 : received - 1;
                    return received;
                }
                if (timeout == portMAX_DELAY) {
                    current_task->waiting = true;
                    ++wait_count;
                    if (can_leave_wait) {
                        std::longjmp(wait_boundary, 1);
                    }
                }
                return 0;
            }

            void LogTelegramMemory(const char*) {}
            void vTaskDelay(TickType_t) {}

            enum esp_http_client_method_t { HTTP_METHOD_GET };
            struct HttpResponse {
                std::string body;
            };
            class TelegramBot;
            TelegramBot* request_bot = nullptr;

            bool PerformRequest(const std::string&, esp_http_client_method_t, const char*,
                                HttpResponse*);

            class TelegramBot {
            public:
                bool Start();
                void Stop();
                static void TaskEntry(void* parameter);
                void Run();

                bool PopResponse(std::string*) { return false; }
                std::atomic<bool> running_{false};
                TaskHandle_t task_ = nullptr;
                int64_t last_update_id_ = 0;
            };

            bool PerformRequest(const std::string&, esp_http_client_method_t, const char*,
                                HttpResponse*) {
                ++http_call_count;
                assert(request_bot != nullptr);
                http_request_active = true;
                request_bot->running_.store(false);
                http_request_active = false;
                return false;
            }

            bool SendMessage(const std::string&, const std::string&, const std::string&) {
                return true;
            }

            bool ProcessUpdatesResponse(TelegramBot&, const std::string&, const std::string&,
                                        const std::string&) {
                return true;
            }
        ''') + "\n" + lifecycle_mutex + "\n" + start + "\n" + stop + "\n" +
                          task_entry + "\n" + run + textwrap.dedent(r'''
            void Reset() {
                created_task = {};
                current_task = nullptr;
                create_count = 0;
                notify_count = 0;
                wake_count = 0;
                wait_count = 0;
                http_call_count = 0;
                http_request_active = false;
                request_bot = nullptr;
                can_leave_wait = false;
            }

            int main(int argc, char** argv) {
                assert(argc == 2);
                const std::string scenario = argv[1];
                Reset();
                TelegramBot bot;
                request_bot = &bot;

                if (scenario == "lifecycle") {
                    assert(bot.Start());
                    const TaskHandle_t original_task = bot.task_;
                    assert(create_count == 1);
                    assert(bot.Start());
                    assert(create_count == 1);
                    assert(bot.task_ == original_task);

                    http_request_active = true;
                    bot.Stop();
                    assert(!bot.running_.load());
                    assert(http_request_active);
                    assert(wait_count == 0);

                    assert(bot.Start());
                    assert(bot.running_.load());
                    assert(bot.task_ == original_task);
                    assert(create_count == 1);

                    for (int reconnect = 0; reconnect != 3; ++reconnect) {
                        bot.Stop();
                        assert(bot.Start());
                        assert(bot.task_ == original_task);
                    }
                    assert(create_count == 1);

                    bot.last_update_id_ = 77;
                    bot.Stop();
                    assert(bot.Start());
                    assert(bot.last_update_id_ == 77);
                    return 0;
                }
                if (scenario == "notifications") {
                    assert(bot.Start());
                    bot.Stop();
                    assert(bot.Start());
                    assert(notify_count == 1);
                    assert(created_task.notifications == 1);

                    current_task = bot.task_;
                    can_leave_wait = true;
                    if (setjmp(wait_boundary) == 0) {
                        created_task.entry(created_task.parameter);
                        assert(false);
                    }
                    assert(http_call_count == 1);
                    assert(created_task.notifications == 0);
                    assert(created_task.waiting);
                    assert(wait_count == 1);

                    xTaskNotifyGive(bot.task_);
                    assert(created_task.notifications == 1);
                    assert(wake_count == 1);
                    assert(create_count == 1);
                    return 0;
                }
                if (scenario == "http_error") {
                    assert(bot.Start());
                    const TaskHandle_t original_task = bot.task_;
                    bot.Run();
                    assert(http_call_count == 1);
                    assert(!bot.running_.load());
                    assert(create_count == 1);
                    assert(bot.task_ == original_task);
                    return 0;
                }
                return 1;
            }
        '''), encoding="utf-8")

        cls.executable = build_dir / "telegram_lifecycle_host_test"
        if cls.compiler[0].lower() == "cl":
            raise unittest.SkipTest("the lifecycle harness requires a GCC-compatible compiler")
        command = cls.compiler + ["-std=c++17", "-fno-exceptions", str(driver),
                                  "-o", str(cls.executable)]
        compilation_env = os.environ.copy()
        compiler_directory = str(pathlib.Path(cls.compiler[0]).parent)
        compilation_env["PATH"] = compiler_directory + os.pathsep + compilation_env["PATH"]
        cls.execution_env = compilation_env
        compilation = subprocess.run(command, capture_output=True, text=True, env=compilation_env)
        if compilation.returncode:
            raise RuntimeError(
                "Failed to compile Telegram lifecycle host harness.\n"
                f"Command: {shlex.join(command)}\n"
                f"stdout:\n{compilation.stdout}\n"
                f"stderr:\n{compilation.stderr}"
            )

    def run_scenario(self, scenario):
        command = [str(self.executable), scenario]
        result = subprocess.run(command, capture_output=True, text=True, env=self.execution_env)
        if result.returncode:
            self.fail(
                "Telegram lifecycle host scenario failed.\n"
                f"Command: {shlex.join(command)}\n"
                f"exit code: {result.returncode}\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )

    def test_start_stop_and_reconnections_reuse_one_task(self):
        self.run_scenario("lifecycle")

    def test_notifications_before_and_after_wait_are_preserved(self):
        self.run_scenario("notifications")

    def test_http_failure_does_not_create_another_task(self):
        self.run_scenario("http_error")


if __name__ == "__main__":
    unittest.main()
