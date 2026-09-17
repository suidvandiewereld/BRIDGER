#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "core/guard.h"
#include "core/licence.h"
#include "core/log.h"
#include "core/memory.h"
#include "decima/dumper.h"
#include "fx/fx.h"
#include "fx/pipeline.h"
#include "fx/ngx.h"
#include "loader/patches.h"
#include "loader/registry.h"
#include "debugger/debugger.h"
#include "overlay/overlay.h"
#include "script/runtime.h"

namespace {

struct Startup {
    bool dump_on_start = false;
    bool load_mods = true;
    bool enable_overlay = true;
    bool debugger = true;
};

std::filesystem::path game_directory() {
    wchar_t buffer[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) {
        return {};
    }
    return std::filesystem::path(buffer).parent_path();
}

Startup read_startup(const std::filesystem::path& root) {
    Startup startup;
    std::ifstream stream(root / "config.json");
    if (!stream.is_open()) {
        return startup;
    }
    try {
        const auto document = nlohmann::json::parse(stream, nullptr, true, true);
        if (document.contains("dump_on_start")) {
            startup.dump_on_start = document["dump_on_start"].get<bool>();
        }
        if (document.contains("load_mods")) {
            startup.load_mods = document["load_mods"].get<bool>();
        }
        if (document.contains("debugger")) {
            startup.debugger = document["debugger"].get<bool>();
        }
        if (document.contains("overlay")) {
            startup.enable_overlay = document["overlay"].get<bool>();
        }
    } catch (const std::exception& error) {
        bridger::log::warn("config.json could not be read: {}", error.what());
    }
    return startup;
}

struct DumpContext {
    bridger::mem::Module game;
    std::filesystem::path root;
};

void run_dumps(void* raw) {
    auto* context = static_cast<DumpContext*>(raw);

    const auto types = context->root / L"dumps" / L"rtti.json";
    const auto stats = bridger::decima::dump_types(context->game, types);
    bridger::log::info("dumped {} classes and {} enums, {} members", stats.classes, stats.enums,
                       stats.members);
    bridger::log::info("{} in image, {} registered at runtime", stats.in_image, stats.runtime_only);

    const auto symbols = context->root / L"dumps" / L"symbols.json";
    const auto symbol_stats = bridger::decima::dump_symbols(context->game, symbols);
    bridger::log::info("dumped {} groups, {} symbols ({} functions, {} with an address)",
                       symbol_stats.groups, symbol_stats.symbols, symbol_stats.functions,
                       symbol_stats.with_address);
}

DWORD WINAPI bootstrap(LPVOID) {
    const auto root = game_directory() / L"Bridger";
    bridger::log::init(root);

    const auto game = bridger::mem::module_of();
    if (!game.valid()) {
        bridger::log::error("could not resolve the host module");
        return 1;
    }

    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    bridger::log::info("bridger {} attached to {}", BRIDGER_VERSION,
                       std::filesystem::path(executable).filename().string());
    bridger::log::info("image base {:#x}, size {:#x}", game.base, game.size);
    bridger::log::info("bridger.dll base {:#x}", reinterpret_cast<std::uintptr_t>(
                           GetModuleHandleW(L"bridger.dll")));

    bridger::patches::install(game.base, root);

    const auto startup = read_startup(root);

    bridger::fx::configure(root);
    bridger::fx::ngx::watch_for_module();

    bridger::promote_fault_handler();
    bridger::fx::pipeline::watch_device_creation();

    bridger::log::info("phase: waiting for rtti registration");
    bridger::decima::wait_for_registration(game, 180000);

    // The game has initialised Steam by now, so this is the first point the licence can be
    // checked. Everything a user can see or run sits behind it (LICENSE, section 4).
    bridger::log::info("phase: checking the game licence");
    const auto licence = bridger::licence::verify();
    if (!licence.ok) {
        bridger::log::error("licence check failed: {}", licence.reason);
        bridger::log::error("Bridger stays inactive: no mods, scripts, overlay or debugger");
        bridger::log::info("phase: ready (inactive)");
        return 0;
    }
    bridger::log::info("{}", licence.detail);

    if (startup.load_mods) {
        bridger::log::info("phase: loading mods");
        auto& registry = bridger::loader::Registry::instance();
        registry.configure(root, game.base);
        bridger::decima::load_static_index(root / L"types.json", game.base);
        bridger::decima::load_static_symbols(root / L"symbols.json", game.base);
        bridger::script::start(root);
        registry.discover();
        registry.load_all();
    }

    if (startup.enable_overlay) {
        bridger::log::info("phase: starting overlay");
        if (bridger::overlay::initialise(root, game) && startup.debugger) {
            bridger::debugger::start(root, game);
        }
    }

    if (startup.dump_on_start) {
        bridger::log::info("phase: dumping rtti and symbols");
        DumpContext context{game, root};
        const auto fault = bridger::guarded_call(run_dumps, &context);
        if (fault != 0) {
            bridger::log::error("dump faulted with code {:#x}", fault);
        }
    }

    bridger::log::info("phase: ready");
    return 0;
}

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);
            const auto thread = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr);
            if (thread != nullptr) {
                CloseHandle(thread);
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            if (reserved != nullptr) {
                bridger::log::shutdown();
                break;
            }
            bridger::overlay::shutdown();
            bridger::fx::ngx::shutdown();
            bridger::script::prepare_exit();
            bridger::loader::Registry::instance().unload_all();
            bridger::script::shutdown();
            bridger::log::shutdown();
            break;
        default:
            break;
    }
    return TRUE;
}
