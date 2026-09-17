#include "debugger/picker.h"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <unordered_map>

#include "core/log.h"
#include "debugger/model.h"
#include "fx/fx.h"

namespace bridger::debugger::picker {
namespace {

constexpr std::uintptr_t kEntityUpdate = 0x218a9d0;
constexpr std::uintptr_t kEntityPosition = 200;

constexpr std::size_t kRing = 1 << 16;
constexpr std::uint64_t kForgetFrames = 900;
constexpr double kMaxDistance = 1500.0;

using UpdateFn = std::uintptr_t (*)(void*, void*, void*, void*);

UpdateFn g_original = nullptr;
void* g_target = nullptr;
bool g_installed = false;
std::atomic<bool> g_enabled{false};

std::array<std::atomic<std::uintptr_t>, kRing> g_ring{};
std::atomic<std::size_t> g_head{0};

std::uintptr_t detour(void* self, void* a, void* b, void* c) {
    const auto slot = g_head.fetch_add(1, std::memory_order_relaxed) & (kRing - 1);
    g_ring[slot].store(reinterpret_cast<std::uintptr_t>(self), std::memory_order_relaxed);
    return g_original(self, a, b, c);
}

struct Known {
    std::uint64_t seen = 0;
    std::string type;
};

std::unordered_map<std::uintptr_t, Known> g_known;
std::uint64_t g_frame = 0;

}

bool install(const mem::Module& game) {
    if (g_installed) {
        return true;
    }
    g_target = reinterpret_cast<void*>(game.from_rva(kEntityUpdate));
    if (MH_CreateHook(g_target, reinterpret_cast<void*>(&detour),
                      reinterpret_cast<void**>(&g_original)) != MH_OK) {
        log::warn("picker: could not hook the entity update; picking in the game window is off");
        return false;
    }
    g_installed = true;
    return true;
}

void set_collecting(bool collecting) {
    if (!g_installed || g_enabled.exchange(collecting) == collecting) {
        return;
    }
    const auto status = collecting ? MH_EnableHook(g_target) : MH_DisableHook(g_target);
    if (status != MH_OK && status != MH_ERROR_ENABLED && status != MH_ERROR_DISABLED) {
        log::warn("picker: entity update hook {} failed ({})", collecting ? "enable" : "disable",
                  static_cast<int>(status));
    }
}

std::vector<Candidate> update(float cursor_x, float cursor_y, int& hovered) {
    hovered = -1;
    std::vector<Candidate> out;
    ++g_frame;

    for (auto& slot : g_ring) {
        const auto entity = slot.exchange(0, std::memory_order_relaxed);
        if (entity == 0) {
            continue;
        }
        auto [found, inserted] = g_known.try_emplace(entity);
        if (inserted) {
            found->second.type = model::type_name_at(entity);
        }
        found->second.seen = g_frame;
    }

    const auto* fx = fx::api();
    BridgerFxFrame frame{};
    if (fx != nullptr) {
        fx->frame(&frame);
    }
    if (fx == nullptr || !frame.camera.valid) {
        return out;
    }
    const float pixels_per_metre = frame.projection[5] * static_cast<float>(frame.height) * 0.5f;

    float best = 1.0f;
    for (auto it = g_known.begin(); it != g_known.end();) {
        if (g_frame - it->second.seen > kForgetFrames || it->second.type.empty()) {
            it = g_known.erase(it);
            continue;
        }
        const auto entity = it->first;
        double position[3]{};
        if (!model::read_bytes(entity + kEntityPosition, position, sizeof position)
                || !std::isfinite(position[0]) || !std::isfinite(position[1])
                || !std::isfinite(position[2])) {
            ++it;
            continue;
        }
        const double dx = position[0] - frame.camera.position[0];
        const double dy = position[1] - frame.camera.position[1];
        const double dz = position[2] - frame.camera.position[2];
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        float screen[2]{};
        if (distance < 0.05 || distance > kMaxDistance
                || !fx->project(position, screen, nullptr)
                || screen[0] < 0.0f || screen[1] < 0.0f
                || screen[0] > static_cast<float>(frame.width)
                || screen[1] > static_cast<float>(frame.height)) {
            ++it;
            continue;
        }

        Candidate candidate;
        candidate.entity = entity;
        candidate.type = it->second.type;
        candidate.x = screen[0];
        candidate.y = screen[1];
        candidate.distance = distance;
        candidate.radius = std::clamp(pixels_per_metre / static_cast<float>(distance), 14.0f, 160.0f);

        const float ddx = candidate.x - cursor_x;
        const float ddy = candidate.y - cursor_y;
        const float score = std::sqrt(ddx * ddx + ddy * ddy) / candidate.radius
                          + static_cast<float>(distance) * 0.0005f;
        if (score < best) {
            best = score;
            hovered = static_cast<int>(out.size());
        }
        out.push_back(std::move(candidate));
        ++it;
    }
    return out;
}

}
