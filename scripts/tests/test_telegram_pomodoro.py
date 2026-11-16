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
        cls.tmp = tempfile.TemporaryDirectory()
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

        cls.executable = build_dir / "telegram_pomodoro_host_test"
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


if __name__ == "__main__":
    unittest.main()
