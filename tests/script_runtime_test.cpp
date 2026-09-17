
#include <Windows.h>

#include <MinHook.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include "core/log.h"
#include "fx/fx_internal.h"
#include "loader/registry.h"
#include "script/runtime.h"

using namespace bridger;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    std::printf("%s %s\n", condition ? "  ok  " : "  FAIL", what.c_str());
    if (!condition) {
        ++g_failures;
    }
}

__declspec(noinline) int add3(int a, int b, int c) {
    volatile int sum = a + b;
    return sum + c;
}

__declspec(noinline) double scale(double value, float factor) {
    volatile double r = value * factor;
    return r;
}

__declspec(noinline) bool gate(int level) {
    volatile int v = level;
    return v > 10;
}

__declspec(noinline) int twice(int value) {
    volatile int v = value;
    return v * 2;
}

__declspec(noinline) int read_through(const int* p) {
    return *p + 1;
}

__declspec(noinline) int fill_out(int seed, float* out_value, int* out_count) {
    volatile int s = seed;
    *out_value = static_cast<float>(s) * 0.5f;
    *out_count = s + 1;
    return 7;
}

int g_cell = 41;

script::Evaluation eval(const std::string& code, const std::string& target = "console") {
    return script::evaluate(target, code, 5000);
}

bool result_is(const std::string& code, const std::string& expected, const std::string& target = "console") {
    const auto result = eval(code, target);
    if (!result.ok || result.result != expected) {
        std::printf("         %s -> %s%s\n", code.c_str(), result.ok ? "" : "error: ",
                    result.result.c_str());
    }
    return result.ok && result.result == expected;
}

std::string address(void* fn) {
    return std::format("{}", reinterpret_cast<std::uintptr_t>(fn));
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

void test_console() {
    std::puts("console");
    check(result_is("1 + 1", "2"), "an expression returns its value");
    check(result_is("x = 5", ""), "a statement runs for effect");
    check(result_is("x * 2", "10"), "state persists between evaluations");
    check(result_is("'a', {1, 2, k = true}", "\"a\", { 1, 2, k = true }"), "values are pretty printed");
    const auto printed = eval("print('hello', 3)");
    check(printed.ok && printed.output == "hello\t3\n", "print output is captured");
    const auto error = eval("error('boom')");
    check(!error.ok && error.result.find("boom") != std::string::npos
              && error.result.find("traceback") != std::string::npos,
          "errors come back with a traceback");
    const auto syntax = eval("local = 1");
    check(!syntax.ok && syntax.result.find("console") != std::string::npos, "syntax errors are reported");

    check(result_is("script.budget(150)", "15000"), "the evaluation budget can be lowered");
    const auto runaway = eval("while true do end");
    check(!runaway.ok && runaway.result.find("without returning") != std::string::npos,
          "an endless loop is stopped at its budget");
    check(result_is("1", "1"), "and the state is still usable after");
    eval("script.budget(15000)");

    const auto items = script::complete("console", "game.ty");
    check(std::find(items.begin(), items.end(), "game.type") != items.end()
              && std::find(items.begin(), items.end(), "game.types") != items.end(),
          "completion lists table members");
    const auto globals = script::complete("console", "every_f");
    check(globals.size() == 1 && globals.front() == "every_frame", "completion lists globals");
    check(result_is("fx.upscale_info().available", "false"), "FX query works without NGX");
    check(result_is("fx.pre_upscale_pass('float4 ps_main(BridgerScreenPixel i):SV_TARGET{return 1;}', {history=true,priority=2}) > 0", "true"),
          "Lua can create a pre upscale pass");
    check(fx::state().effects.size() == 1 && fx::state().effects.begin()->second.owner == "console",
          "Lua FX belongs to its scoped script actor");
    check(result_is("fx.revert()", "") && fx::state().effects.empty() && fx::state().shaders.empty(),
          "Lua FX revert removes shader and pass");
}

void test_calls() {
    std::puts("calls");
    eval(std::format("add3_address = {}; scale_address = {}; read_address = {}; fill_address = {}",
                     address(&add3), address(&scale), address(&read_through), address(&fill_out)));
    check(result_is("fn(add3_address, 'int(int a, int b, int c)')(1, 2, 3)", "6"),
          "call a native function through a signature");
    check(result_is("fn(scale_address, 'double(double, float)')(1.5, 4)", "6.0"),
          "floating arguments and result");
    check(result_is(std::format("mem.read({}, 'int32')", address(&g_cell)), "41"), "mem.read");
    eval(std::format("mem.write({}, 'int32', 99)", address(&g_cell)));
    check(g_cell == 99, "mem.write");
    const auto fault = eval("fn(read_address, 'int(ptr)')(0)");
    check(!fault.ok && fault.result.find("faulted") != std::string::npos,
          "a faulting call becomes a Lua error");
    check(result_is("fn(fill_address, 'int(int, float*, int*)')(10, out, out)", "7, 5.0, 11"),
          "out parameters come back after the result");
    const auto arity = eval("fn(add3_address, 'int(int, int, int)')(1)");
    check(!arity.ok && arity.result.find("takes 3 argument") != std::string::npos,
          "wrong argument counts are explained");
}

void test_hooks() {
    std::puts("hooks");
    eval(std::format("add3_address = {}; gate_address = {}; scale_address = {}", address(&add3),
                     address(&gate), address(&scale)));

    check(eval("h1 = hook(add3_address, 'int(int, int, int)', function(original, a, b, c) "
               "return original(a * 2, b, c) + 1000 end)").ok,
          "replace hook installed");
    check(add3(1, 2, 3) == 1007, "it rewrites an argument and the result");

    check(eval("h2 = hook(add3_address, 'int(int, int, int)', function(original, a, b, c) "
               "return original(a, b, c) * 10 end)").ok,
          "a second script subscriber on the same function");
    const int chained = add3(1, 2, 3);
    check(chained == (2 + 2 + 3) * 10 + 1000,
          std::format("the chain composes in subscription order ({})", chained));
    if (chained != (2 + 2 + 3) * 10 + 1000) {
        for (const auto& line : script::console_lines(6)) {
            std::printf("         [%s] %s\n", line.source.c_str(), line.text.c_str());
        }
    }

    eval("h1:remove()");
    const int remaining = add3(1, 2, 3);
    check(remaining == 60, std::format("removing one subscriber keeps the other ({})", remaining));
    eval("h2:remove()");
    check(add3(1, 2, 3) == 6, "removing the last restores the function");

    check(eval("gate_hook = before(gate_address, 'bool(int)', function(level) "
               "if level == 3 then return true end end)").ok,
          "before hook installed");
    check(gate(3) && !gate(4) && gate(11), "before answers some calls and lets the rest through");
    eval("gate_hook:remove()");

    check(eval("seen = 0; after(scale_address, 'double(double, float)', function(result, v, f) "
               "seen = seen + 1; if v < 0 then return 0 end end)").ok,
          "after hook installed");
    check(scale(2.0, 3.0f) == 6.0 && scale(-2.0, 3.0f) == 0.0, "after sees and replaces the result");
    check(result_is("seen", "2"), "and counted both calls");

    eval("before(add3_address, 'int(int, int, int)', function() error('broken hook') end)");
    const auto lines_before = script::console_lines(5000).size();
    check(add3(1, 2, 3) == 6, "a hook that errors falls through to the original");
    check(script::console_lines(5000).size() > lines_before, "and reports the error");

    check(eval("skipped = hook(gate_address, 'bool(int)', function() return skip end)").ok,
          "hook returning skip");
    check(!gate(50), "skip answers zero without the original");
    eval("skipped:remove()");

    check(eval("cb = callback('int(int)', function(x) return x * 3 end)").ok, "native callback");
    const auto cb = eval("cb.address");
    const auto callback = reinterpret_cast<int (*)(int)>(std::stoull(cb.result));
    check(cb.ok && callback(7) == 21, "the engine can call a script function through a pointer");
}

void test_tasks() {
    std::puts("tasks and timers");
    eval("done = false; ticks = 0; every_frame(function() ticks = ticks + 1 end); "
         "task(function() wait_frames(2); done = true end); "
         "fired = false; later(0.05, function() fired = true end)");
    eval("__bridger_tick(0.02)");
    check(result_is("done, fired", "false, false"), "nothing is due after one frame");
    eval("__bridger_tick(0.02)");
    eval("__bridger_tick(0.02)");
    check(result_is("done, fired, ticks", "true, true, 3"), "tasks, timers and frame callbacks run");
    eval("task(function() error('task error') end)");
    check(result_is("1", "1"), "a failing task does not take the state with it");
    check(result_is("vec3(1, 2, 3) + vec3(1, 1, 1)", "vec3(2.000, 3.000, 4.000)"), "vectors");
}

void test_mods(const std::filesystem::path& root) {
    std::puts("script mods");
    auto& registry = loader::Registry::instance();
    const auto file = root / "scripts" / "keeper.lua";
    write_file(file, "local state = keep('state', { loads = 0 })\n"
                     "state.loads = state.loads + 1\n"
                     "loads = state.loads\n"
                     "speed = setting('speed', 8.5, { min = 0, max = 20 })\n");
    registry.discover();
    registry.load_all();
    const auto find = [&]() -> const loader::Mod* {
        for (const auto& mod : registry.mods()) {
            if (mod.id == "keeper") {
                return &mod;
            }
        }
        return nullptr;
    };
    check(find() != nullptr && find()->script && find()->state == loader::State::Loaded,
          "a loose .lua file is a mod");
    check(result_is("loads, speed.value", "1, 8.5", "keeper"), "it ran in its own state");
    eval("speed:set(12)", "keeper");

    registry.request_reload("keeper");
    registry.process_pending_reloads();
    check(result_is("loads, speed.value", "2, 12.0", "keeper"), "a reload keeps kept state and settings");

    write_file(file, "this is not lua\n");
    registry.request_reload("keeper");
    registry.process_pending_reloads();
    check(find()->state == loader::State::Loaded && !find()->error.empty(),
          "a syntax error keeps the running version");
    check(result_is("loads", "2", "keeper"), "which still answers");

    eval(std::format("twice_address = {}", address(&twice)));
    write_file(file, std::format("hook({}, 'int(int)', function(original, v) return 0 end)\n"
                                 "error('fails at load')\n",
                                 address(&twice)));
    registry.request_reload("keeper");
    registry.process_pending_reloads();
    check(find()->state == loader::State::Failed
              && find()->error.find("fails at load") != std::string::npos,
          "a runtime error at load fails the mod with the reason");
    const auto targets = script::targets();
    check(std::find(targets.begin(), targets.end(), "keeper") == targets.end(),
          "and its state is gone");
    check(twice(21) == 42, "along with the hook it made before failing");

    write_file(root / "scripts" / "fresh.lua", "fresh = true\n");
    for (int i = 0; i < 5; ++i) {
        registry.poll_for_changes(1.0f);
    }
    registry.process_pending_reloads();
    check(result_is("fresh", "true", "fresh"), "a script created while running is picked up");
}

void test_pipe() {
    std::puts("pipe");
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 50 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
        pipe = CreateFileW(L"\\\\.\\pipe\\bridger", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(20);
        }
    }
    check(pipe != INVALID_HANDLE_VALUE, "connect to the console pipe");
    if (pipe == INVALID_HANDLE_VALUE) {
        return;
    }
    const std::string request = R"({"op":"eval","target":"console","code":"6 * 7"})" "\n";
    DWORD written = 0;
    WriteFile(pipe, request.data(), static_cast<DWORD>(request.size()), &written, nullptr);
    std::string response;
    char buffer[4096];
    while (response.find('\n') == std::string::npos) {
        DWORD got = 0;
        if (!ReadFile(pipe, buffer, sizeof buffer, &got, nullptr) || got == 0) {
            break;
        }
        response.append(buffer, got);
    }
    check(response.find("\"result\":\"42\"") != std::string::npos
              && response.find("\"ok\":true") != std::string::npos,
          "evaluate over the pipe");
    CloseHandle(pipe);
}

}

int main(int argc, char** argv) {
    const int serve_seconds = argc > 2 && std::string(argv[1]) == "--serve" ? std::atoi(argv[2]) : 0;
    const auto root = std::filesystem::temp_directory_path() / "bridger_script_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "scripts", ec);
    log::init(root);
    if (MH_Initialize() != MH_OK) {
        std::puts("MinHook failed to initialise");
        return 1;
    }
    auto& registry = loader::Registry::instance();
    registry.configure(root, reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
    script::start(root);

    test_console();
    test_calls();
    test_hooks();
    test_tasks();
    test_mods(root);
    test_pipe();

    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "passed" : "FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    std::fflush(stdout);
    if (serve_seconds > 0) {
        std::printf("serving the console pipe for %d s\n", serve_seconds);
        std::fflush(stdout);
        Sleep(static_cast<DWORD>(serve_seconds) * 1000);
    }
    TerminateProcess(GetCurrentProcess(), g_failures == 0 ? 0 : 1);
    return 0;
}
