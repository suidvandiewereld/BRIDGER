#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/log.h"
#include "loader/registry.h"
#include "script/runtime.h"
#include "script/vm.h"

namespace bridger::script {
namespace {

constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\bridger";
constexpr DWORD kBufferSize = 1 << 16;

using json = nlohmann::json;

std::atomic<bool> g_running{false};

const char* state_name(loader::State state) {
    switch (state) {
        case loader::State::Loaded: return "loaded";
        case loader::State::Disabled: return "disabled";
        case loader::State::Failed: return "failed";
        default: return "idle";
    }
}

json line_json(const Line& line) {
    static constexpr const char* kKinds[] = {"print", "result", "info", "warn", "error"};
    return json{{"kind", kKinds[std::clamp(line.kind, 0, 4)]}, {"source", line.source}, {"text", line.text}};
}

json handle(const json& request) {
    const auto op = request.value("op", std::string());
    if (op == "ping") {
        return {{"ok", true}, {"version", BRIDGER_VERSION}, {"lua", LUA_RELEASE},
                {"game_thread", loader::game_thread_ticking(1000)}};
    }
    if (op == "eval") {
        const auto target = request.value("target", std::string("console"));
        const auto code = request.value("code", std::string());
        const auto timeout = request.value("timeout_ms", 10000u);
        const auto result = evaluate(target, code, timeout);
        return {{"ok", result.ok}, {"result", result.result}, {"output", result.output}};
    }
    if (op == "complete") {
        return {{"ok", true},
                {"items", complete(request.value("target", std::string("console")),
                                   request.value("prefix", std::string()))}};
    }
    if (op == "scripts") {
        json scripts = json::array();
        for (const auto& status : statuses()) {
            scripts.push_back({{"id", status.id},
                               {"alive", status.alive},
                               {"error", status.last_error},
                               {"errors", status.errors},
                               {"hooks", status.hooks},
                               {"callbacks", status.callbacks},
                               {"memory_kb", status.memory_kb}});
        }
        json mods = json::array();
        for (const auto& mod : loader::Registry::instance().mods()) {
            mods.push_back({{"id", mod.id},
                            {"name", mod.name},
                            {"state", state_name(mod.state)},
                            {"script", mod.script},
                            {"entry", utf8((mod.directory / mod.entry).generic_u8string())},
                            {"error", mod.error}});
        }
        return {{"ok", true}, {"scripts", scripts}, {"mods", mods}};
    }
    if (op == "reload") {
        const auto id = request.value("id", std::string());
        loader::Registry::instance().request_reload(id);
        return {{"ok", true}, {"result", std::format("reload of {} requested", id)}};
    }
    if (op == "log") {
        std::vector<std::string> lines;
        const auto next = log::snapshot_since(request.value("since", 0ull), lines);
        return {{"ok", true}, {"lines", lines}, {"next", next}};
    }
    if (op == "console") {
        std::vector<Line> lines;
        const auto next = console_since(request.value("since", 0ull), lines);
        json out = json::array();
        for (const auto& line : lines) {
            out.push_back(line_json(line));
        }
        return {{"ok", true}, {"lines", out}, {"next", next}};
    }
    return {{"ok", false}, {"result", std::format("unknown op '{}'", op)}};
}

bool write_all(HANDLE pipe, const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(text.size() - sent, kBufferSize));
        if (!WriteFile(pipe, text.data() + sent, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        sent += written;
    }
    return true;
}

void serve(HANDLE pipe) {
    std::string pending;
    std::vector<char> buffer(kBufferSize);
    while (g_running.load(std::memory_order_relaxed)) {
        DWORD got = 0;
        if (!ReadFile(pipe, buffer.data(), kBufferSize, &got, nullptr) || got == 0) {
            break;
        }
        pending.append(buffer.data(), got);
        std::size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            const auto line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (line.find_first_not_of(" \r\t") == std::string::npos) {
                continue;
            }
            json response;
            try {
                response = handle(json::parse(line));
            } catch (const std::exception& error) {
                response = {{"ok", false}, {"result", std::string("bad request: ") + error.what()}};
            }
            if (!write_all(pipe, response.dump(-1, ' ', false, json::error_handler_t::replace) + "\n")) {
                break;
            }
        }
    }
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

SECURITY_ATTRIBUTES* owner_only(SECURITY_ATTRIBUTES& attributes) {
    using Convert = BOOL(WINAPI*)(LPCWSTR, DWORD, PSECURITY_DESCRIPTOR*, PULONG);
    static PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (descriptor == nullptr) {
        const auto advapi = LoadLibraryW(L"advapi32.dll");
        const auto convert = advapi != nullptr
            ? reinterpret_cast<Convert>(
                  GetProcAddress(advapi, "ConvertStringSecurityDescriptorToSecurityDescriptorW"))
            : nullptr;
        if (convert == nullptr || !convert(L"D:P(A;;GA;;;OW)(A;;GA;;;SY)", 1, &descriptor, nullptr)) {
            return nullptr;
        }
    }
    attributes.nLength = sizeof attributes;
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return &attributes;
}

void listen() {
    SECURITY_ATTRIBUTES attributes{};
    const auto* security = owner_only(attributes);
    if (security == nullptr) {
        log::warn("script: console pipe uses the default access list");
    }
    log::info("script: console listening on \\\\.\\pipe\\bridger");
    while (g_running.load(std::memory_order_relaxed)) {
        const HANDLE pipe = CreateNamedPipeW(
            kPipeName, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, kBufferSize, kBufferSize, 0,
            const_cast<SECURITY_ATTRIBUTES*>(security));
        if (pipe == INVALID_HANDLE_VALUE) {
            log::error("script: console pipe could not be created ({})", GetLastError());
            return;
        }
        const bool connected = ConnectNamedPipe(pipe, nullptr) != 0
                            || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected || !g_running.load(std::memory_order_relaxed)) {
            CloseHandle(pipe);
            continue;
        }
        std::thread(serve, pipe).detach();
    }
}

}

void start_console_server() {
    if (g_running.exchange(true)) {
        return;
    }
    std::thread(listen).detach();
}

void stop_console_server() {
    g_running.store(false);
}

}
