#include "fx/depth_capture.h"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "core/log.h"

namespace bridger::fx::depth {
namespace {

constexpr int kCreateDsvIndex = 21;
constexpr int kResourceBarrierIndex = 26;
constexpr int kClearDsvIndex = 47;

using CreateDsvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*,
                                             const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                             D3D12_CPU_DESCRIPTOR_HANDLE);
using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE,
                                            D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                           const D3D12_RESOURCE_BARRIER*);

CreateDsvFn g_create_dsv = nullptr;
ClearDsvFn g_clear_dsv = nullptr;
BarrierFn g_barrier = nullptr;
void* g_barrier_target = nullptr;
bool g_prepared = false;
bool g_tracking = false;

std::mutex g_mutex;
std::unordered_map<SIZE_T, ID3D12Resource*> g_views;
std::vector<Candidate> g_candidates;
std::vector<unsigned> g_pending_clears;
unsigned g_frame = 0;

std::atomic<ID3D12Resource*> g_watched{nullptr};
std::atomic<int> g_watched_state{-1};

void STDMETHODCALLTYPE create_dsv_detour(ID3D12Device* device, ID3D12Resource* resource,
                                         const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                         D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    g_create_dsv(device, resource, desc, handle);
    std::scoped_lock lock(g_mutex);
    if (resource != nullptr) {
        g_views[handle.ptr] = resource;
    } else {
        g_views.erase(handle.ptr);
    }
}

void STDMETHODCALLTYPE clear_dsv_detour(ID3D12GraphicsCommandList* list,
                                        D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_CLEAR_FLAGS flags,
                                        FLOAT depth, UINT8 stencil, UINT rect_count,
                                        const D3D12_RECT* rects) {
    g_clear_dsv(list, handle, flags, depth, stencil, rect_count, rects);
    std::scoped_lock lock(g_mutex);
    const auto found = g_views.find(handle.ptr);
    if (found == g_views.end()) {
        return;
    }
    ID3D12Resource* resource = found->second;
    for (std::size_t i = 0; i < g_candidates.size(); ++i) {
        if (g_candidates[i].resource == resource) {
            ++g_pending_clears[i];
            g_candidates[i].last_seen_frame = g_frame;
            return;
        }
    }
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return;
    }
    Candidate candidate;
    candidate.resource = resource;
    candidate.width = static_cast<unsigned>(desc.Width);
    candidate.height = desc.Height;
    candidate.format = desc.Format;
    candidate.msaa = desc.SampleDesc.Count > 1 || desc.DepthOrArraySize != 1 || desc.MipLevels != 1;
    candidate.last_seen_frame = g_frame;
    g_candidates.push_back(candidate);
    g_pending_clears.push_back(1);
}

void STDMETHODCALLTYPE barrier_detour(ID3D12GraphicsCommandList* list, UINT count,
                                      const D3D12_RESOURCE_BARRIER* barriers) {
    ID3D12Resource* watched = g_watched.load(std::memory_order_relaxed);
    if (watched != nullptr) {
        for (UINT i = 0; i < count; ++i) {
            if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION
                    && barriers[i].Transition.pResource == watched) {
                g_watched_state.store(static_cast<int>(barriers[i].Transition.StateAfter),
                                      std::memory_order_relaxed);
            }
        }
    }
    g_barrier(list, count, barriers);
}

}

bool prepare(void** device_vtable, void** command_list_vtable) {
    if (g_prepared) {
        return true;
    }
    if (device_vtable == nullptr || command_list_vtable == nullptr) {
        return false;
    }
    bool ok = true;
    ok &= MH_CreateHook(device_vtable[kCreateDsvIndex], reinterpret_cast<void*>(&create_dsv_detour),
                        reinterpret_cast<void**>(&g_create_dsv)) == MH_OK;
    ok &= MH_CreateHook(command_list_vtable[kClearDsvIndex], reinterpret_cast<void*>(&clear_dsv_detour),
                        reinterpret_cast<void**>(&g_clear_dsv)) == MH_OK;
    g_barrier_target = command_list_vtable[kResourceBarrierIndex];
    if (!ok) {
        log::warn("fx: depth capture hooks could not be created");
        return false;
    }
    g_prepared = true;
    return true;
}

void set_tracking(bool enabled) {
    if (!g_prepared || g_barrier_target == nullptr || enabled == g_tracking) {
        return;
    }
    if (enabled && g_barrier == nullptr) {
        if (MH_CreateHook(g_barrier_target, reinterpret_cast<void*>(&barrier_detour),
                          reinterpret_cast<void**>(&g_barrier)) != MH_OK) {
            log::warn("fx: depth capture barrier hook could not be created");
            g_barrier_target = nullptr;
            return;
        }
    }
    const auto status = enabled ? MH_EnableHook(g_barrier_target) : MH_DisableHook(g_barrier_target);
    if (status == MH_OK) {
        g_tracking = enabled;
        log::info("fx: depth capture {}", enabled ? "on" : "off");
    } else {
        log::warn("fx: depth capture barrier hook toggle failed ({})", static_cast<int>(status));
    }
    if (!enabled) {
        g_watched.store(nullptr);
        g_watched_state.store(-1);
    }
}

bool tracking() {
    return g_tracking;
}

void end_frame(unsigned frame) {
    std::scoped_lock lock(g_mutex);
    g_frame = frame;
    for (std::size_t i = 0; i < g_candidates.size(); ++i) {
        g_candidates[i].clears = g_pending_clears[i];
        g_pending_clears[i] = 0;
    }
    for (std::size_t i = 0; i < g_candidates.size();) {
        if (frame - g_candidates[i].last_seen_frame > 300) {
            if (g_watched.load() == g_candidates[i].resource) {
                g_watched.store(nullptr);
                g_watched_state.store(-1);
            }
            g_candidates.erase(g_candidates.begin() + static_cast<std::ptrdiff_t>(i));
            g_pending_clears.erase(g_pending_clears.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

bool select(int preferred_index, unsigned swapchain_width, unsigned swapchain_height,
            Candidate& out) {
    std::scoped_lock lock(g_mutex);
    std::vector<const Candidate*> live;
    for (const auto& candidate : g_candidates) {
        if (candidate.clears > 0 && !candidate.msaa && candidate.last_seen_frame + 1 >= g_frame) {
            live.push_back(&candidate);
        }
    }
    if (live.empty()) {
        return false;
    }
    const Candidate* chosen = nullptr;
    if (preferred_index >= 0 && preferred_index < static_cast<int>(live.size())) {
        chosen = live[static_cast<std::size_t>(preferred_index)];
    } else {
        for (const auto* candidate : live) {
            if (candidate->width == swapchain_width && candidate->height == swapchain_height) {
                chosen = candidate;
                break;
            }
        }
        if (chosen == nullptr) {
            chosen = *std::max_element(live.begin(), live.end(), [](const Candidate* a, const Candidate* b) {
                return static_cast<std::uint64_t>(a->width) * a->height
                     < static_cast<std::uint64_t>(b->width) * b->height;
            });
        }
    }
    if (g_watched.load() != chosen->resource) {
        g_watched.store(chosen->resource);
        g_watched_state.store(-1);
    }
    out = *chosen;
    return true;
}

void watch(ID3D12Resource* resource) {
    if (g_watched.load() != resource) {
        g_watched.store(resource);
        g_watched_state.store(-1);
    }
}

D3D12_RESOURCE_STATES known_state(ID3D12Resource* resource, D3D12_RESOURCE_STATES fallback) {
    if (g_watched.load() != resource) {
        return fallback;
    }
    const int state = g_watched_state.load();
    return state < 0 ? fallback : static_cast<D3D12_RESOURCE_STATES>(state);
}

std::vector<Candidate> candidates() {
    std::scoped_lock lock(g_mutex);
    std::vector<Candidate> live;
    for (const auto& candidate : g_candidates) {
        if (candidate.clears > 0 && !candidate.msaa && candidate.last_seen_frame + 1 >= g_frame) {
            live.push_back(candidate);
        }
    }
    return live;
}

unsigned known_views() {
    std::scoped_lock lock(g_mutex);
    return static_cast<unsigned>(g_views.size());
}

void reset() {
    std::scoped_lock lock(g_mutex);
    g_candidates.clear();
    g_pending_clears.clear();
    g_watched.store(nullptr);
    g_watched_state.store(-1);
}

}
