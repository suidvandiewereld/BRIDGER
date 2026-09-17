#include "debugger/globals.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "debugger/model.h"
#include "decima/dumper.h"

namespace bridger::debugger::globals {
namespace {

constexpr std::size_t kWindow = 160;

std::mutex g_mutex;
std::vector<Global> g_found;
std::atomic<bool> g_running{false};
std::atomic<std::size_t> g_examined{0};
std::atomic<std::size_t> g_total{0};

void run(mem::Module game) {
    const auto symbols = decima::symbol_index();
    g_total.store(symbols->size());
    g_examined.store(0);

    const auto in_data = [&](std::uintptr_t address) {
        return address >= game.base && address < game.base + game.size
            && (address < game.text_begin || address >= game.text_end);
    };

    std::map<std::uintptr_t, Global> by_slot;
    std::uint8_t code[kWindow];
    for (const auto& [key, address] : *symbols) {
        g_examined.fetch_add(1, std::memory_order_relaxed);
        if (address == 0 || !model::read_bytes(address, code, sizeof code)) {
            continue;
        }
        for (std::size_t i = 0; i + 10 < kWindow; ++i) {
            const std::uint8_t rex = code[i];
            const std::uint8_t op = code[i + 1];
            const std::uint8_t modrm = code[i + 2];
            if (rex < 0x48 || rex > 0x4f || (op != 0x8b && op != 0x8d)
                    || (modrm >> 6) != 0 || (modrm & 7) != 5) {
                continue;
            }
            std::int32_t displacement = 0;
            std::memcpy(&displacement, code + i + 3, sizeof displacement);
            const std::uintptr_t target = address + i + 7 + static_cast<std::intptr_t>(displacement);
            const bool cookie = code[i + 7] == 0x48 && code[i + 8] == 0x33 && code[i + 9] == 0xc4;
            if (cookie || !in_data(target)) {
                i += 6;
                continue;
            }

            const bool indirect = op == 0x8b;
            std::uintptr_t object = target;
            if (indirect && !model::read(target, object)) {
                i += 6;
                continue;
            }
            auto type = model::type_name_at(object);
            if (!type.empty()) {
                auto& entry = by_slot[target];
                if (entry.references == 0) {
                    entry.slot = target;
                    entry.indirect = indirect;
                    entry.type = std::move(type);
                    entry.source = key;
                }
                ++entry.references;
            }
            i += 6;
        }
    }

    std::vector<Global> found;
    found.reserve(by_slot.size());
    for (auto& [slot, global] : by_slot) {
        found.push_back(std::move(global));
    }
    std::sort(found.begin(), found.end(), [](const Global& a, const Global& b) {
        return a.type != b.type ? a.type < b.type : a.slot < b.slot;
    });
    const auto count = found.size();
    {
        std::scoped_lock lock(g_mutex);
        g_found = std::move(found);
    }
    log::info("debugger: {} engine globals found across {} exports", count, symbols->size());
    g_running.store(false);
}

}

void scan(const mem::Module& game) {
    if (g_running.exchange(true)) {
        return;
    }
    std::thread(run, game).detach();
}

std::vector<Global> snapshot() {
    std::scoped_lock lock(g_mutex);
    return g_found;
}

bool running() {
    return g_running.load();
}

std::size_t examined() {
    return g_examined.load(std::memory_order_relaxed);
}

std::size_t total() {
    return g_total.load();
}

std::uintptr_t resolve(const Global& global) {
    if (!global.indirect) {
        return global.slot;
    }
    std::uintptr_t object = 0;
    return model::read(global.slot, object) ? object : 0;
}

}
