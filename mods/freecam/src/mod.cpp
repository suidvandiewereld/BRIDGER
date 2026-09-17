#include "bridger/mod.hpp"
#include "bridger/fx.hpp"

#include <Windows.h>
#include <tlhelp32.h>
#include <intrin.h>

#include "../freecam_api.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

BRIDGER_MOD("freecam", "Camera", "2.3.0", "Marquis", "Free camera and first-person view.")

namespace {

constexpr std::uintptr_t kEntitySetOrientation = 0x2123ed0;
constexpr std::uintptr_t kTpcCommit = 0x3348f00;
constexpr std::uintptr_t kDsTpcCommit = 0x24e2140;
constexpr std::size_t kComponentEntity = 0x48;
constexpr std::size_t kTpcOutput = 0x88;
constexpr std::size_t kDsTpcOutput = 0xa8;
constexpr std::size_t kEntityFollowed = 0x70;
constexpr std::size_t kEntityOrientation = 0xc8;
constexpr std::size_t kCameraFov = 964;
constexpr std::uintptr_t kIsPlayerCamera = 0x21a0020;
constexpr std::uintptr_t kSetVisible = 0x219f180;
constexpr std::uintptr_t kSetVisibleImpl = 0x219c940;
constexpr std::uintptr_t kHelperTransform = 0x21a08c0;
constexpr std::uintptr_t kStringConstruct = 0x175b570;
constexpr std::uintptr_t kStringDestroy = 0x175b890;
constexpr std::size_t kCameraNearPlane = 1084;
constexpr std::size_t kPlayerGameFromEntity = 0x560;
constexpr std::size_t kPlayerLookPitch = 0x464;
constexpr std::size_t kPlayerLookHeading = 0x468;
void* g_player_game = nullptr;
bool g_steer_applied = false;
std::atomic<bool> g_use_steer_angles = true;

constexpr double kPi = 3.14159265358979323846;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct WorldTransform {
    double px = 0.0;
    double py = 0.0;
    double pz = 0.0;
    Vec3 m[3];
    std::uint32_t pad = 0;
};
static_assert(sizeof(WorldTransform) == 64);

using SetOrientationFn = void (*)(void*, const WorldTransform*);
using TpcCommitFn = void (*)(void*);
using DsTpcCommitFn = void (*)(void*, bool);

bridger::Hook<SetOrientationFn> g_set_orientation;
using PlayerUpdateFn = void (*)(void*, float);
struct alignas(16) AimVector { float x, y, z, w; };
using AimDirectionFn = AimVector* (*)(void*, AimVector*);
bridger::Hook<PlayerUpdateFn> g_player_update;
bridger::Hook<AimDirectionFn> g_aim_direction;
std::atomic<unsigned> g_native_turn_updates = 0;
std::atomic<unsigned> g_turn_checks = 0;
std::atomic<unsigned> g_turn_flagged = 0;
std::atomic<unsigned> g_turn_applies = 0;
constexpr std::size_t kCtrlStickX = 0x2948;
constexpr std::size_t kCtrlStickY = 0x294c;
constexpr std::size_t kParamsState = 0x314;
constexpr std::size_t kParamsFlagsWord4 = 0x580;
constexpr std::uint32_t kMoveHeadingToTurnHeading = 1u << 11;
std::atomic<float> g_turn_threshold = 20.0f;
std::atomic<float> g_turn_done = 6.0f;
std::atomic<float> g_turn_stick = 0.4f;
std::atomic<bool> g_turn_hold = false;
std::atomic<int> g_tap_frames = 5;
std::atomic<int> g_tap_cooldown = 25;
bool g_injecting = false;
unsigned g_inject_frames = 0;
unsigned g_inject_bursts = 0;
int g_burst_frames = 0;
int g_cooldown = 0;
float g_turn_delta = 0.0f;
int g_last_state = 0;

using TurnCheckFn = void (*)(void*);
using TurnApplyFn = void (*)(void*, float);
bridger::Hook<TurnCheckFn> g_turn_check;
bridger::Hook<TurnApplyFn> g_turn_apply;
bridger::Hook<TpcCommitFn> g_tpc_commit;
bridger::Hook<DsTpcCommitFn> g_ds_tpc_commit;

std::atomic<bool> g_enabled = false;
std::atomic<bool> g_first_person = false;
std::atomic<bool> g_snap_pending = false;
std::atomic<bool> g_use_wasd = false;
std::atomic<float> g_speed = 8.0f;
std::atomic<float> g_eye_height = 1.72f;
std::atomic<float> g_eye_forward = 0.09f;
std::atomic<float> g_eye_up = 0.06f;
std::atomic<float> g_eye_side = 0.0f;
bool g_rows_logged = false;
float g_view_heading = 0.0f;
bool g_view_valid = false;
std::atomic<bool> g_turn_body = true;
std::atomic<float> g_body_offset = 0.0f;
float g_body_heading = 0.0f;
bool g_in_vehicle = false;

float wrap_degrees(float value) {
    value = std::fmod(value + 180.0f, 360.0f);
    if (value < 0.0f) {
        value += 360.0f;
    }
    return value - 180.0f;
}
void* g_player = nullptr;
std::atomic<unsigned> g_camera_updates = 0;
char g_hidden_type[64] = "";
char g_followed_type[64] = "";
float heading_of(const Vec3& f) {
    return static_cast<float>(std::atan2(-f.x, f.y) * 180.0 / kPi);
}
std::atomic<float> g_fov = 100.0f;
std::atomic<float> g_near_plane = 0.05f;
bool g_override_valid = false;
void* g_followed = nullptr;
std::atomic<bool> g_hide_body = false;
std::atomic<bool> g_hide_head = true;
std::atomic<float> g_eye_clearance = 0.12f;
std::atomic<float> g_eye_down_forward = 0.18f;
std::atomic<float> g_eye_down_drop = 0.04f;
std::atomic<bool> g_face_trusted = false;
std::atomic<float> g_eye_shoulder = 0.28f;
std::atomic<float> g_eye_shoulder_lift = 0.06f;
void* g_hidden_entity = nullptr;

using SetVisibleFn = void (*)(void*, bool);

bridger::Hook<SetVisibleFn> g_set_visible_hook;
std::atomic<bool> g_trace_visibility = false;
std::atomic<unsigned> g_trace_lines = 0;
constexpr unsigned kTraceLimit = 80;

void sync_body_visibility(void* wanted_hidden) {
    if (wanted_hidden == g_hidden_entity) {
        return;
    }
    const auto set_visible = bridger::at_rva<SetVisibleFn>(kSetVisible);
    if (set_visible == nullptr) {
        return;
    }
    if (g_hidden_entity != nullptr) {
        set_visible(g_hidden_entity, true);
        bridger::info("shown again: {} ({})", g_hidden_entity, g_hidden_type);
    }
    if (wanted_hidden != nullptr) {
        const auto name = bridger::rtti_name(bridger::rtti_of(wanted_hidden));
        const auto length = std::min(name.size(), sizeof g_hidden_type - 1);
        std::memcpy(g_hidden_type, name.data(), length);
        g_hidden_type[length] = 0;
        set_visible(wanted_hidden, false);
        bridger::info("hidden for first person: {} ({})", wanted_hidden, g_hidden_type);
    } else {
        g_hidden_type[0] = 0;
    }
    g_hidden_entity = wanted_hidden;
}
constexpr std::size_t kEntityArtParts = 0x180;
constexpr std::uintptr_t kPartLookup = 0x20fbed0;
constexpr std::uintptr_t kPartEnable = 0x20fe500;
using PartLookupFn = int (*)(void*, const std::uint32_t*);
using PartEnableFn = void (*)(void*, bool, void*, void*, void*);
using PartVirtualFn = void (*)(void*, bool, void*);

std::mutex g_head_parts_lock;
std::vector<std::uint32_t> g_head_parts;
void* g_parts_entity = nullptr;
std::vector<std::uint32_t> g_parts_hidden;
std::atomic<unsigned> g_parts_hidden_count = 0;
std::atomic<unsigned> g_parts_unknown_count = 0;

bool set_part_enabled(void* entity, std::uint32_t hash, bool on) {
    const auto lookup = bridger::at_rva<PartLookupFn>(kPartLookup);
    const auto enable = bridger::at_rva<PartEnableFn>(kPartEnable);
    if (entity == nullptr || lookup == nullptr || enable == nullptr) {
        return false;
    }
    auto* data = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(entity) + kEntityArtParts);
    if (data == nullptr || data[0x38] == 0) {
        return false;
    }
    auto* resource = *reinterpret_cast<void**>(data + 0x30);
    auto* context = *reinterpret_cast<std::uint8_t**>(data + 0x48);
    auto** parts = *reinterpret_cast<void***>(data + 0x108);
    if (resource == nullptr || context == nullptr || parts == nullptr) {
        return false;
    }
    const int index = lookup(resource, &hash);
    if (index < 0 || parts[index] == nullptr) {
        return false;
    }
    void* part = parts[index];
    void* extra = *reinterpret_cast<void**>(context + 0xb8);
    return bridger::guarded([&] {
        enable(part, on, context, extra, data);
        const auto* vtable = *reinterpret_cast<PartVirtualFn**>(part);
        vtable[7](part, on, extra);
    });
}

std::uint8_t part_flags(void* entity, std::uint32_t hash) {
    const auto lookup = bridger::at_rva<PartLookupFn>(kPartLookup);
    std::uint8_t flags = 0;
    if (entity == nullptr || lookup == nullptr) return 0;
    bridger::guarded([&] {
        auto* data = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(entity) + kEntityArtParts);
        if (data == nullptr || data[0x38] == 0) return;
        auto* resource = *reinterpret_cast<void**>(data + 0x30);
        auto** parts = *reinterpret_cast<std::uint8_t***>(data + 0x108);
        if (resource == nullptr || parts == nullptr) return;
        const int index = lookup(resource, &hash);
        if (index >= 0 && parts[index] != nullptr) flags = parts[index][0x2c];
    });
    return flags;
}

constexpr std::uint32_t kHeadRigParts[] = {0x5d2964aa, 0x3b68948c, 0x1185219f, 0x14e3a140,
                                           0x5a5b3a7d, 0x6714b91c, 0x3572a37f};
std::atomic<bool> g_collapse_head = true;

std::vector<std::uint32_t> head_parts_to_hide() {
    std::vector<std::uint32_t> out;
    for (const auto hash : g_head_parts) {
        if (g_collapse_head && std::find(std::begin(kHeadRigParts), std::end(kHeadRigParts), hash)
                                   != std::end(kHeadRigParts)) {
            continue;
        }
        out.push_back(hash);
    }
    return out;
}

void sync_head_parts(void* entity) {
    if (entity != nullptr && entity == g_parts_entity) {
        std::vector<std::uint32_t> wanted;
        {
            std::lock_guard lock(g_head_parts_lock);
            wanted = head_parts_to_hide();
        }
        for (auto it = g_parts_hidden.begin(); it != g_parts_hidden.end();) {
            if (std::find(wanted.begin(), wanted.end(), *it) == wanted.end()) {
                set_part_enabled(entity, *it, true);
                it = g_parts_hidden.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto hash : wanted) {
            if (part_flags(entity, hash) == 0x0f && set_part_enabled(entity, hash, false)) {
                if (std::find(g_parts_hidden.begin(), g_parts_hidden.end(), hash) == g_parts_hidden.end()) {
                    g_parts_hidden.push_back(hash);
                }
            }
        }
        g_parts_hidden_count = static_cast<unsigned>(g_parts_hidden.size());
        return;
    }
    if (entity == g_parts_entity) {
        return;
    }
    if (g_parts_entity != nullptr) {
        for (const auto hash : g_parts_hidden) {
            set_part_enabled(g_parts_entity, hash, true);
        }
        bridger::info("head parts shown again ({})", g_parts_hidden.size());
    }
    g_parts_hidden.clear();
    unsigned unknown = 0;
    if (entity != nullptr) {
        std::vector<std::uint32_t> wanted;
        {
            std::lock_guard lock(g_head_parts_lock);
            wanted = head_parts_to_hide();
        }
        for (const auto hash : wanted) {
            const auto flags = part_flags(entity, hash);
            if (flags == 0) {
                ++unknown;
            } else if (flags == 0x0f && set_part_enabled(entity, hash, false)) {
                g_parts_hidden.push_back(hash);
            }
        }
        bridger::info("head parts hidden: {} ({} not on this model)", g_parts_hidden.size(), unknown);
    }
    g_parts_entity = entity;
    g_parts_hidden_count = static_cast<unsigned>(g_parts_hidden.size());
    g_parts_unknown_count = unknown;
}

struct PartRow {
    std::uint32_t hash;
    int index;
};
std::vector<PartRow> g_part_rows;
std::atomic<bool> g_part_scan_done = false;

template <class Keep>
std::vector<PartRow> drawn_parts(void* entity, Keep keep) {
    std::vector<PartRow> rows;
    const auto lookup = bridger::at_rva<PartLookupFn>(kPartLookup);
    if (entity != nullptr && lookup != nullptr) {
        bridger::guarded([&] {
            auto* data = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(entity) + kEntityArtParts);
            if (data == nullptr) return;
            auto* resource = *reinterpret_cast<std::uint8_t**>(data + 0x30);
            auto** parts = *reinterpret_cast<std::uint8_t***>(data + 0x108);
            if (resource == nullptr || parts == nullptr) return;
            std::vector<std::uint8_t*> stack;
            auto push_array = [&](std::uint8_t* array_at) {
                const auto count = *reinterpret_cast<std::uint32_t*>(array_at);
                auto** items = *reinterpret_cast<std::uint8_t***>(array_at + 8);
                for (std::uint32_t i = 0; items != nullptr && i < count && i < 4096; ++i) {
                    if (items[i] != nullptr) stack.push_back(items[i]);
                }
            };
            push_array(resource + 0xd0);
            while (!stack.empty() && rows.size() < 512) {
                auto* node = stack.back();
                stack.pop_back();
                const auto hash = *reinterpret_cast<std::uint32_t*>(node + 0x20);
                const int index = lookup(resource, &hash);
                if (index >= 0 && parts[index] != nullptr && keep(hash, parts[index][0x2c])) {
                    rows.push_back({hash, index});
                }
                push_array(node + 0x28);
            }
        });
    }
    std::sort(rows.begin(), rows.end(), [](const PartRow& a, const PartRow& b) { return a.index < b.index; });
    return rows;
}

void scan_parts() {
    auto rows = drawn_parts(g_player, [](std::uint32_t hash, std::uint8_t flags) {
        return (flags & 0x01) != 0
               || std::find(g_parts_hidden.begin(), g_parts_hidden.end(), hash) != g_parts_hidden.end();
    });
    std::lock_guard lock(g_head_parts_lock);
    g_part_rows = std::move(rows);
    g_part_scan_done = true;
}

void reapply_head_parts() {
    void* entity = g_parts_entity;
    sync_head_parts(nullptr);
    sync_head_parts(entity);
}

std::vector<std::uint32_t> parse_hashes(const std::string& text) {
    std::vector<std::uint32_t> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !std::isxdigit(static_cast<unsigned char>(text[i]))) ++i;
        std::size_t j = i;
        while (j < text.size() && std::isxdigit(static_cast<unsigned char>(text[j]))) ++j;
        if (j > i && j - i <= 8) {
            out.push_back(static_cast<std::uint32_t>(std::stoul(text.substr(i, j - i), nullptr, 16)));
        }
        i = j;
    }
    return out;
}

bool g_helper_found = false;
double g_helper_pos[3] = {0.0, 0.0, 0.0};

constexpr std::uintptr_t kGetChildren = 0x219eda0;
struct EngineArray {
    std::uint32_t count;
    std::uint32_t capacity;
    void** data;
};
using GetChildrenFn = EngineArray* (*)(EngineArray*, void*);

struct ChildRow {
    void* entity;
    std::string key;
    std::string label;
};
std::vector<ChildRow> g_children;
std::vector<std::string> g_hidden_child_keys;
std::vector<void*> g_children_hidden;
void* g_children_owner = nullptr;
std::atomic<int> g_hovered_child = -1;
bool g_hover_blink_off = false;

std::string child_label(void* entity) {
    return std::string{bridger::rtti_name(bridger::rtti_of(entity))};
}

void scan_children() {
    std::vector<ChildRow> rows;
    const auto get = bridger::at_rva<GetChildrenFn>(kGetChildren);
    void* owner = g_player;
    if (owner != nullptr && get != nullptr) {
        EngineArray list{};
        bridger::guarded([&] { get(&list, owner); });
        for (std::uint32_t i = 0; list.data != nullptr && i < list.count && i < 256; ++i) {
            void* child = list.data[i];
            if (child == nullptr) continue;
            const auto type = bridger::rtti_name(bridger::rtti_of(child));
            if (type == "CameraEntity") continue;
            const auto label = child_label(child);
            int same = 0;
            for (const auto& row : rows) {
                if (row.label == label) ++same;
            }
            std::string key = std::format("{}#{}", label, same);
            std::replace(key.begin(), key.end(), ' ', '_');
            rows.push_back({child, key, label});
        }
    }
    std::lock_guard lock(g_head_parts_lock);
    g_children = std::move(rows);
}

struct ChildPartRow {
    int child;
    std::string key;
    std::uint32_t hash;
    int index;
};
std::vector<ChildPartRow> g_child_part_rows;
std::vector<std::string> g_hidden_child_part_keys;
std::vector<std::pair<void*, std::uint32_t>> g_child_parts_hidden;
std::atomic<bool> g_child_part_scan_done = false;

void scan_child_parts() {
    std::vector<ChildRow> children;
    {
        std::lock_guard lock(g_head_parts_lock);
        children = g_children;
    }
    std::vector<ChildPartRow> rows;
    for (int c = 0; c < static_cast<int>(children.size()); ++c) {
        const void* owner = children[c].entity;
        auto parts = drawn_parts(children[c].entity, [owner](std::uint32_t hash, std::uint8_t flags) {
            if ((flags & 0x01) != 0) return true;
            for (const auto& [entity, h] : g_child_parts_hidden) {
                if (entity == owner && h == hash) return true;
            }
            return false;
        });
        for (const auto& part : parts) {
            rows.push_back({c, std::format("{}:{:08x}", children[c].key, part.hash), part.hash, part.index});
        }
    }
    std::lock_guard lock(g_head_parts_lock);
    g_child_part_rows = std::move(rows);
    g_child_part_scan_done = true;
}

void sync_child_parts(bool active) {
    std::vector<std::pair<void*, std::uint32_t>> wanted;
    if (active) {
        std::lock_guard lock(g_head_parts_lock);
        for (const auto& key : g_hidden_child_part_keys) {
            const auto colon = key.rfind(':');
            if (colon == std::string::npos) continue;
            const auto child_key_text = key.substr(0, colon);
            const auto hash = static_cast<std::uint32_t>(std::strtoul(key.c_str() + colon + 1, nullptr, 16));
            for (const auto& child : g_children) {
                if (child.key == child_key_text) wanted.emplace_back(child.entity, hash);
            }
        }
    }
    for (const auto& pair : g_child_parts_hidden) {
        if (std::find(wanted.begin(), wanted.end(), pair) == wanted.end()) {
            set_part_enabled(pair.first, pair.second, true);
        }
    }
    std::vector<std::pair<void*, std::uint32_t>> now;
    for (const auto& pair : wanted) {
        const bool already = std::find(g_child_parts_hidden.begin(), g_child_parts_hidden.end(), pair)
                             != g_child_parts_hidden.end();
        if (already || set_part_enabled(pair.first, pair.second, false)) now.push_back(pair);
    }
    g_child_parts_hidden = std::move(now);
}

void sync_children(bool active) {
    sync_child_parts(active);
    const auto set_visible = bridger::at_rva<SetVisibleFn>(kSetVisible);
    if (set_visible == nullptr) return;
    std::vector<ChildRow> rows;
    std::vector<std::string> keys;
    {
        std::lock_guard lock(g_head_parts_lock);
        rows = g_children;
        keys = g_hidden_child_keys;
    }
    std::vector<void*> wanted;
    if (active) {
        for (const auto& row : rows) {
            if (std::find(keys.begin(), keys.end(), row.key) != keys.end()) {
                wanted.push_back(row.entity);
            }
        }
    }
    const int hovered = g_hovered_child;
    void* hover_entity = hovered >= 0 && hovered < static_cast<int>(rows.size()) ? rows[hovered].entity : nullptr;
    g_hover_blink_off = (GetTickCount64() / 250) % 2 == 0;
    if (hover_entity != nullptr && g_hover_blink_off) wanted.push_back(hover_entity);

    for (void* entity : g_children_hidden) {
        if (std::find(wanted.begin(), wanted.end(), entity) == wanted.end()) {
            bridger::guarded([&] { set_visible(entity, true); });
        }
    }
    for (void* entity : wanted) {
        if (std::find(g_children_hidden.begin(), g_children_hidden.end(), entity) == g_children_hidden.end()) {
            bridger::guarded([&] { set_visible(entity, false); });
        }
    }
    g_children_hidden = std::move(wanted);
}

void update_hover() {
    if (!bridger::overlay_visible()) {
        g_hovered_child = -1;
        return;
    }
    POINT cursor{};
    HWND window = GetForegroundWindow();
    RECT client{};
    if (!GetCursorPos(&cursor) || window == nullptr || !ScreenToClient(window, &cursor)
        || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) {
        g_hovered_child = -1;
        return;
    }
    const auto frame = bridger::fx::frame();
    if (frame.width == 0 || frame.height == 0) {
        g_hovered_child = -1;
        return;
    }
    const float mx = static_cast<float>(cursor.x) * static_cast<float>(frame.width) / static_cast<float>(client.right);
    const float my = static_cast<float>(cursor.y) * static_cast<float>(frame.height) / static_cast<float>(client.bottom);
    std::vector<ChildRow> rows;
    {
        std::lock_guard lock(g_head_parts_lock);
        rows = g_children;
    }
    int best = -1;
    float best_d = 80.0f * static_cast<float>(frame.width) / 1920.0f;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto* t = reinterpret_cast<const WorldTransform*>(
            static_cast<std::uint8_t*>(rows[i].entity) + kEntityOrientation);
        float sx = 0.0f;
        float sy = 0.0f;
        if (!bridger::fx::project({t->px, t->py, t->pz}, sx, sy)) continue;
        const float d = std::hypot(sx - mx, sy - my);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    g_hovered_child = best;
}

void toggle_hovered_child();

char g_helper_edit[64] = "HeadHelper";
float g_game_near = 0.0f;

struct Mat44 {
    float m[16];
};

struct EngineString {
    void* data = nullptr;
};

using HelperTransformFn = int (*)(const void*, int, const EngineString*, WorldTransform*);
using StringConstructFn = EngineString* (*)(EngineString*, const char*);
using StringDestroyFn = void (*)(EngineString*);

bool finite3(double x, double y, double z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

bool helper_location(const void* entity, const char* name, double* xyz, Vec3* rows = nullptr) {
    const auto lookup = bridger::at_rva<HelperTransformFn>(kHelperTransform);
    const auto construct = bridger::at_rva<StringConstructFn>(kStringConstruct);
    const auto destroy = bridger::at_rva<StringDestroyFn>(kStringDestroy);
    if (lookup == nullptr || construct == nullptr || destroy == nullptr || entity == nullptr
        || name == nullptr || name[0] == 0) {
        return false;
    }
    const auto* origin = reinterpret_cast<const WorldTransform*>(
        static_cast<const std::uint8_t*>(entity) + kEntityOrientation);

    EngineString text;
    construct(&text, name);
    WorldTransform out{};
    out.px = -12345.0;
    out.py = -12345.0;
    out.pz = -12345.0;
    const int status = lookup(entity, 0, &text, &out);
    destroy(&text);

    if (status == -1 || out.px == -12345.0 || !finite3(out.px, out.py, out.pz)) {
        return false;
    }
    const double dx = out.px - origin->px;
    const double dy = out.py - origin->py;
    const double dz = out.pz - origin->pz;
    if (dx * dx + dy * dy + dz * dz > 25.0) {
        return false;
    }
    xyz[0] = out.px;
    xyz[1] = out.py;
    xyz[2] = out.pz;
    if (rows != nullptr) {
        rows[0] = out.m[0];
        rows[1] = out.m[1];
        rows[2] = out.m[2];
    }
    return true;
}

Vec3 normalize(const Vec3& a);
Vec3 scale(const Vec3& a, float s);

float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

void head_frame(const Vec3* rows, const Vec3& body_forward, Vec3& face, Vec3& up) {
    const Vec3 world_up{0.0f, 0.0f, 1.0f};
    int best_face = 0;
    int best_up = 2;
    float face_score = -2.0f;
    float up_score = -2.0f;
    for (int i = 0; i < 3; ++i) {
        const Vec3 row = normalize(rows[i]);
        const float f = dot(row, body_forward);
        if (std::fabs(f) > std::fabs(face_score)) { face_score = f; best_face = i; }
        const float u = dot(row, world_up);
        if (std::fabs(u) > std::fabs(up_score)) { up_score = u; best_up = i; }
    }
    face = scale(normalize(rows[best_face]), face_score < 0.0f ? -1.0f : 1.0f);
    up = scale(normalize(rows[best_up]), up_score < 0.0f ? -1.0f : 1.0f);
}

void probe_helpers() {
    static const char* const candidates[] = {
        "Head", "head", "Helper_Head", "HeadHelper", "Head_Helper", "Camera", "camera",
        "CameraHelper", "Helper_Camera", "Eye", "Eyes", "EyeL", "EyeR", "Eye_L", "Eye_R",
        "LeftEye", "RightEye", "Neck", "neck", "Face", "face", "HeadTop", "HeadLookAt",
        "LookAt", "Lookat", "Spine", "Root", "Hips", "Pelvis", "Chest", "Backpack", "BB",
        "FirstPersonCamera", "FPCamera", "FP_Camera", "SubjectiveCamera", "PhotoCamera",
        "HelperEye", "HelperHead", "HeadCamera", "Head_Camera", "camera_head", "head_camera",
    };
    const void* entity = g_followed;
    if (entity == nullptr) {
        bridger::warn("probe: no followed entity yet, turn first person on first");
        return;
    }
    unsigned hits = 0;
    for (const char* name : candidates) {
        double xyz[3];
        if (helper_location(entity, name, xyz)) {
            ++hits;
            bridger::info("helper '{}' at {:.2f} {:.2f} {:.2f}", name, xyz[0], xyz[1], xyz[2]);
        }
    }
    if (hits == 0) {
        const auto lookup = bridger::at_rva<HelperTransformFn>(kHelperTransform);
        const auto construct = bridger::at_rva<StringConstructFn>(kStringConstruct);
        const auto destroy = bridger::at_rva<StringDestroyFn>(kStringDestroy);
        if (lookup != nullptr && construct != nullptr && destroy != nullptr) {
            EngineString text;
            construct(&text, "Head");
            WorldTransform out{};
            const int status = lookup(entity, 0, &text, &out);
            destroy(&text);
            bridger::info("probe: raw status for 'Head' = {} position {:.2f} {:.2f} {:.2f}", status,
                          out.px, out.py, out.pz);
        }
    }
    bridger::info("probe: {} of {} helper names known", hits, std::size(candidates));
}
bool g_is_player_camera = false;
float g_game_fov = 0.0f;

void* g_camera_entity = nullptr;
void* g_component = nullptr;
const char* g_source = "none";
unsigned g_commits = 0;
unsigned g_overrides = 0;
float g_dt = 0.0f;
LARGE_INTEGER g_last_commit{};

WorldTransform g_game{};
float g_game_heading = 0.0f;
float g_game_pitch = 0.0f;

WorldTransform g_ours{};
double g_x = 0.0;
double g_y = 0.0;
double g_z = 0.0;
float g_heading = 0.0f;
float g_pitch = 0.0f;

int g_convention = -1;
float g_convention_error = 1.0f;

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

Vec3 normalize(const Vec3& a) {
    const float len = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return len > 1e-6f ? scale(a, 1.0f / len) : Vec3{};
}

float distance2(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

Vec3 forward_of(float heading_deg, float pitch_deg) {
    const double h = heading_deg * kPi / 180.0;
    const double p = pitch_deg * kPi / 180.0;
    const double cp = std::cos(p);
    return {static_cast<float>(-std::sin(h) * cp), static_cast<float>(std::cos(h) * cp),
            static_cast<float>(std::sin(p))};
}

void angles_of(const Vec3& f, float& heading_deg, float& pitch_deg) {
    heading_deg = static_cast<float>(std::atan2(-f.x, f.y) * 180.0 / kPi);
    pitch_deg = static_cast<float>(std::atan2(f.z, std::sqrt(f.x * f.x + f.y * f.y)) * 180.0 / kPi);
}

Vec3 candidate(int which, const Vec3& right, const Vec3& up) {
    switch (which) {
    case 0: return right;
    case 1: return scale(right, -1.0f);
    case 2: return up;
    default: return scale(up, -1.0f);
    }
}

void frame_of(const Vec3& f, Vec3& right, Vec3& up) {
    right = normalize(cross(f, Vec3{0.0f, 0.0f, 1.0f}));
    up = normalize(cross(right, f));
}

void calibrate(const WorldTransform& game) {
    const Vec3 f = normalize(game.m[1]);
    if (std::fabs(f.z) > 0.985f) {
        return;
    }
    Vec3 right;
    Vec3 up;
    frame_of(f, right, up);
    int best = -1;
    float best_error = 1.0f;
    for (int a = 0; a < 4; ++a) {
        for (int b = 0; b < 4; ++b) {
            const float error = distance2(candidate(a, right, up), game.m[0])
                                + distance2(candidate(b, right, up), game.m[2]);
            if (error < best_error) {
                best_error = error;
                best = a * 4 + b;
            }
        }
    }
    if (best >= 0 && best_error < 0.05f && best != g_convention) {
        bridger::info("rotation convention {}: m0 = {}, m2 = {} (error {:.4f})", best,
                      best / 4, best % 4, best_error);
        g_convention = best;
    }
    g_convention_error = best_error;
}

void rows_for(float heading_deg, float pitch_deg, Vec3* rows) {
    const Vec3 f = forward_of(heading_deg, pitch_deg);
    Vec3 right;
    Vec3 up;
    frame_of(f, right, up);
    const int convention = g_convention >= 0 ? g_convention : 0;
    rows[0] = candidate(convention / 4, right, up);
    rows[1] = f;
    rows[2] = candidate(convention % 4, right, up);
}

void build_transform() {
    g_ours.px = g_x;
    g_ours.py = g_y;
    g_ours.pz = g_z;
    rows_for(g_heading, g_pitch, g_ours.m);
    g_ours.pad = g_game.pad;
}

bool held(int key) { return bridger::key_down(static_cast<unsigned>(key)); }

void set_enabled(bool value) {
    if (value == g_enabled) {
        return;
    }
    g_enabled = value;
    if (value) {
        g_first_person = false;
        g_snap_pending = true;
    }
    bridger::info("freecam {}", value ? "on" : "off");
}

void toggle() { set_enabled(!g_enabled); }

void set_first_person(bool value) {
    if (value == g_first_person) {
        return;
    }
    g_first_person = value;
    if (value) {
        g_enabled = false;
        g_view_valid = false;
        g_view_valid = false;
    }
    bridger::info("first person {}", value ? "on" : "off");
}

void toggle_first_person() { set_first_person(!g_first_person); }

enum class Dropout : int {
    None = 0,
    NoFollowed = 1,
    NotPlayerCamera = 2,
    Passthrough = 3,
};

const char* dropout_name(int reason) {
    switch (static_cast<Dropout>(reason)) {
        case Dropout::NoFollowed: return "nothing followed";
        case Dropout::NotPlayerCamera: return "not the player camera";
        case Dropout::Passthrough: return "no override to substitute";
        default: return "none";
    }
}

std::atomic<bool> g_hide_blocked = false;
char g_hide_candidate[64] = "";

std::atomic<int> g_dropout = 0;
std::atomic<unsigned> g_dropout_counts[4] = {};
std::atomic<unsigned> g_passthroughs = 0;
std::atomic<unsigned> g_entity_swaps = 0;
std::atomic<unsigned> g_frames_since_dropout = 0;
char g_camera_type[64] = "";
char g_last_dropout_type[64] = "";

void note_dropout(Dropout reason, void* entity) {
    const int value = static_cast<int>(reason);
    ++g_dropout_counts[value];
    g_frames_since_dropout = 0;
    if (g_dropout.exchange(value) != value) {
        const auto name = bridger::rtti_name(bridger::rtti_of(entity));
        const auto length = std::min(name.size(), sizeof g_last_dropout_type - 1);
        std::memcpy(g_last_dropout_type, name.data(), length);
        g_last_dropout_type[length] = 0;
        bridger::info("first person dropout: {} (camera entity {} is a {})", dropout_name(value),
                      entity, g_last_dropout_type[0] != 0 ? g_last_dropout_type : "?");
    }
}

void place_eye(void* body);
void* g_eye_body = nullptr;

void first_person_commit(void* entity, WorldTransform* out) {
    void* followed = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(entity) + kEntityFollowed);
    if (followed == nullptr || !g_is_player_camera) {
        note_dropout(followed == nullptr ? Dropout::NoFollowed : Dropout::NotPlayerCamera, entity);
        g_override_valid = false;
        g_view_valid = false;
        sync_body_visibility(nullptr);
        return;
    }
    if (g_dropout.exchange(static_cast<int>(Dropout::None)) != static_cast<int>(Dropout::None)) {
        bridger::info("first person recovered after {} frame(s)", g_frames_since_dropout.load());
    }
    ++g_frames_since_dropout;
    if (followed != g_followed) {
        g_followed = followed;
        const auto name = bridger::rtti_name(bridger::rtti_of(followed));
        const auto length = std::min(name.size(), sizeof g_followed_type - 1);
        std::memcpy(g_followed_type, name.data(), length);
        g_followed_type[length] = 0;
        bridger::info("first person follows {} ({})", followed, g_followed_type);
        if (name == "DSPlayerEntity") {
            g_player = followed;
        }
    }
    void* body = g_player != nullptr ? g_player : followed;
    g_in_vehicle = body != followed;

    void* hide_target = nullptr;
    if (g_hide_body && bridger::rtti_name(bridger::rtti_of(body)) == "DSPlayerEntity") {
        hide_target = body;
    }
    sync_body_visibility(hide_target);
    g_hide_blocked = g_hide_body && hide_target == nullptr;
    if (g_hide_blocked) {
        const auto name = bridger::rtti_name(bridger::rtti_of(body));
        const auto length = std::min(name.size(), sizeof g_hide_candidate - 1);
        std::memcpy(g_hide_candidate, name.data(), length);
        g_hide_candidate[length] = 0;
    }
    const auto* anchor = reinterpret_cast<const WorldTransform*>(
        static_cast<std::uint8_t*>(body) + kEntityOrientation);

    g_view_heading = heading_of(normalize(g_game.m[1]));
    g_body_heading = heading_of(anchor->m[1]);
    g_view_valid = true;
    g_ours = g_game;
    place_eye(body);
    g_eye_body = body;
    *out = g_ours;
    g_override_valid = true;
    ++g_overrides;
}

constexpr std::size_t kPoseBuffers[] = {0x1a0, 0x210};
void* find_skinned_model(void* entity);
constexpr int kJointSpine = 5;
constexpr int kLimbJoints[] = {9, 38, 63, 68};

std::atomic<bool> g_fight_eye = true;
std::atomic<float> g_fight_speed = 4.5f;
std::atomic<float> g_fight_hold = 0.8f;
std::atomic<float> g_fight_back = 0.0f;
std::atomic<float> g_fight_up = 0.1f;
std::atomic<float> g_fight_side = -1.4f;
std::atomic<float> g_fight_fov = 12.0f;
std::atomic<float> g_fight_limb = 0.0f;
std::atomic<bool> g_fight_attack = false;
constexpr std::uintptr_t kGetAttackEvent = 0x219fc10;
constexpr std::uintptr_t kGetPlayerInfo = 0x257fba0;
constexpr std::uintptr_t kGetPlayerParameterBool = 0x257ed70;
std::atomic<std::uint32_t> g_fight_state = 0;
std::atomic<std::uint32_t> g_fight_flags = 0;
std::atomic<float> g_fight_blend = 0.0f;
double g_fight_pos[3] = {0.0, 0.0, 0.0};
double g_fight_focus[3] = {0.0, 0.0, 0.0};
Vec3 g_fight_axis{0.0f, 1.0f, 0.0f};
std::atomic<float> g_fight_reach = 1.2f;
bool g_fight_pos_valid = false;
float g_fight_timer = 0.0f;
float g_limb_prev[2][4][3] = {};
bool g_limb_prev_valid[2] = {false, false};

void update_fight_eye(void* entity, float dt) {
    if (!g_fight_eye || entity == nullptr || dt <= 0.0f || dt > 0.25f) {
        g_fight_blend = 0.0f;
        g_fight_pos_valid = false;
        g_limb_prev_valid[0] = g_limb_prev_valid[1] = false;
        return;
    }
    auto* model = static_cast<std::uint8_t*>(find_skinned_model(entity));
    if (model == nullptr) return;
    const auto* t = reinterpret_cast<const WorldTransform*>(static_cast<std::uint8_t*>(entity) + kEntityOrientation);

    float fastest = 0.0f;
    double spine_world[3] = {0.0, 0.0, 0.0};
    bool have_spine = false;
    bridger::guarded([&] {
        for (int b = 0; b < 2; ++b) {
            auto* begin = *reinterpret_cast<float**>(model + kPoseBuffers[b]);
            auto* end = *reinterpret_cast<float**>(model + kPoseBuffers[b] + 8);
            if (begin == nullptr || end <= begin || (end - begin) / 16 <= kLimbJoints[3]) continue;
            for (int l = 0; l < 4; ++l) {
                const float* m = begin + kLimbJoints[l] * 16;
                if (g_limb_prev_valid[b]) {
                    const float dx = m[12] - g_limb_prev[b][l][0];
                    const float dy = m[13] - g_limb_prev[b][l][1];
                    const float dz = m[14] - g_limb_prev[b][l][2];
                    fastest = std::max(fastest, std::sqrt(dx * dx + dy * dy + dz * dz) / dt);
                }
                g_limb_prev[b][l][0] = m[12];
                g_limb_prev[b][l][1] = m[13];
                g_limb_prev[b][l][2] = m[14];
            }
            g_limb_prev_valid[b] = true;
            if (!have_spine) {
                const float* s = begin + kJointSpine * 16;
                spine_world[0] = t->px + s[12] * t->m[0].x + s[13] * t->m[1].x + s[14] * t->m[2].x;
                spine_world[1] = t->py + s[12] * t->m[0].y + s[13] * t->m[1].y + s[14] * t->m[2].y;
                spine_world[2] = t->pz + s[12] * t->m[0].z + s[13] * t->m[1].z + s[14] * t->m[2].z;
                have_spine = true;
            }
        }
    });
    if (fastest > 30.0f) fastest = 0.0f;
    g_fight_limb = fastest;
    if (!have_spine) return;

    void* link = nullptr;
    if (const auto get = bridger::at_rva<void* (*)(void*)>(kGetAttackEvent); get != nullptr) {
        bridger::guarded([&] { link = get(entity); });
    }
    g_fight_attack = link != nullptr;

    std::uint32_t state = 0;
    bool combo_attacking = false;
    bool combo_state = false;
    bool attack_mode = false;
    bridger::guarded([&] {
        using InfoFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*,
                                void*, void*, void*, void*, void*, void*, void*, void*, void*, void*,
                                void*, void*, void*, void*, void*);
        if (const auto info = bridger::at_rva<InfoFn>(kGetPlayerInfo); info != nullptr) {
            std::uint64_t out[25] = {};
            auto* o = out;
            info(o, o + 1, o + 2, o + 3, o + 4, o + 5, o + 6, o + 7, o + 8, o + 9, o + 10, o + 11,
                 o + 12, o + 13, o + 14, o + 15, o + 16, o + 17, o + 18, o + 19, o + 20, o + 21,
                 o + 22, o + 23, o + 24);
            state = static_cast<std::uint32_t>(out[18]);
        }
        if (const auto flag = bridger::at_rva<bool (*)(std::uint32_t)>(kGetPlayerParameterBool); flag != nullptr) {
            combo_attacking = flag(22);
            combo_state = flag(24);
            attack_mode = flag(76);
        }
    });
    g_fight_state = state;
    g_fight_flags = (combo_attacking ? 1u : 0u) | (combo_state ? 2u : 0u) | (attack_mode ? 4u : 0u);
    if (link != nullptr || state == 14 || combo_attacking || combo_state || attack_mode) {
        g_fight_timer = g_fight_hold;
    }
    g_fight_timer = std::max(0.0f, g_fight_timer - dt);
    const float target = g_fight_timer > 0.0f ? 1.0f : 0.0f;
    const float rate = target > g_fight_blend ? 1.0f / 0.15f : 1.0f / 0.5f;
    float blend = g_fight_blend;
    blend += std::clamp(target - blend, -rate * dt, rate * dt);
    g_fight_blend = blend;

    const Vec3 facing = normalize(Vec3{t->m[1].x, t->m[1].y, 0.0f});
    if (!g_fight_pos_valid || blend <= 0.0f) {
        g_fight_axis = facing;
    } else {
        const float k = 1.0f - std::exp(-dt * 2.5f);
        g_fight_axis = normalize(Vec3{g_fight_axis.x + (facing.x - g_fight_axis.x) * k,
                                      g_fight_axis.y + (facing.y - g_fight_axis.y) * k, 0.0f});
        if (g_fight_axis.x == 0.0f && g_fight_axis.y == 0.0f) g_fight_axis = facing;
    }
    const Vec3 fwd = g_fight_axis;
    const Vec3 right = normalize(cross(fwd, Vec3{0.0f, 0.0f, 1.0f}));
    const double half = g_fight_reach * 0.5f - g_fight_back;
    const double focus[3] = {spine_world[0] + fwd.x * half, spine_world[1] + fwd.y * half,
                             spine_world[2] - 0.1};
    const double want[3] = {focus[0] + right.x * g_fight_side, focus[1] + right.y * g_fight_side,
                            focus[2] + g_fight_up};
    if (!g_fight_pos_valid || blend <= 0.0f) {
        std::copy(std::begin(want), std::end(want), g_fight_pos);
        std::copy(std::begin(focus), std::end(focus), g_fight_focus);
        g_fight_pos_valid = true;
    } else {
        const double k = 1.0 - std::exp(-dt * 8.0);
        for (int a = 0; a < 3; ++a) {
            g_fight_pos[a] += (want[a] - g_fight_pos[a]) * k;
            g_fight_focus[a] += (focus[a] - g_fight_focus[a]) * k;
        }
    }
}

void apply_fight_eye() {
    const float blend = g_fight_blend;
    if (blend <= 0.0f || !g_fight_pos_valid) return;
    const float s = blend * blend * (3.0f - 2.0f * blend);
    g_ours.px += (g_fight_pos[0] - g_ours.px) * s;
    g_ours.py += (g_fight_pos[1] - g_ours.py) * s;
    g_ours.pz += (g_fight_pos[2] - g_ours.pz) * s;

    const Vec3 shot = normalize(Vec3{static_cast<float>(g_fight_focus[0] - g_fight_pos[0]),
                                     static_cast<float>(g_fight_focus[1] - g_fight_pos[1]),
                                     static_cast<float>(g_fight_focus[2] - g_fight_pos[2])});
    const Vec3 view = normalize(g_game.m[1]);
    Vec3 f = normalize(Vec3{view.x + (shot.x - view.x) * s, view.y + (shot.y - view.y) * s,
                            view.z + (shot.z - view.z) * s});
    if (f.x == 0.0f && f.y == 0.0f && f.z == 0.0f) f = shot;
    const Vec3 r = normalize(cross(f, Vec3{0.0f, 0.0f, 1.0f}));
    const Vec3 u = cross(r, f);
    g_ours.m[0] = r;
    g_ours.m[1] = f;
    g_ours.m[2] = u;
}

constexpr int kHandAttachJoints[] = {26, 55};
std::atomic<float> g_hand_clearance = 0.32f;
std::atomic<float> g_hand_push = 0.0f;
std::atomic<float> g_hand_ease = 0.08f;

constexpr std::size_t kPoseSlots = 0x160;
constexpr std::size_t kPoseSlotStride = 0x70;
constexpr std::size_t kPoseSlotIndex = 0x240;
constexpr std::size_t kPoseSlotBegin = 0x40;

float* current_pose(std::uint8_t* model, int min_joint) {
    const int index = *reinterpret_cast<const int*>(model + kPoseSlotIndex);
    if (index < 0 || index > 1) return nullptr;
    auto* slot = model + kPoseSlots + static_cast<std::size_t>(index) * kPoseSlotStride;
    auto* begin = *reinterpret_cast<float**>(slot + kPoseSlotBegin);
    auto* end = *reinterpret_cast<float**>(slot + kPoseSlotBegin + 8);
    if (begin == nullptr || end <= begin || (end - begin) / 16 <= min_joint) return nullptr;
    return begin;
}

Vec3 g_hand_offset{0.0f, 0.0f, 0.0f};
unsigned g_hand_frame = 0;

void keep_eye_clear_of_hands(void* body) {
    const float clearance = g_hand_clearance;
    if (clearance <= 0.0f || body == nullptr) {
        g_hand_offset = {0.0f, 0.0f, 0.0f};
        g_hand_push = 0.0f;
        return;
    }
    auto* model = static_cast<std::uint8_t*>(find_skinned_model(body));
    if (model == nullptr) return;
    const auto* t = reinterpret_cast<const WorldTransform*>(static_cast<std::uint8_t*>(body) + kEntityOrientation);
    Vec3 target{0.0f, 0.0f, 0.0f};
    bridger::guarded([&] {
        const float* begin = current_pose(model, kHandAttachJoints[1]);
        if (begin == nullptr) return;
        for (const int joint : kHandAttachJoints) {
            const float* m = begin + joint * 16;
            const double hx = t->px + m[12] * t->m[0].x + m[13] * t->m[1].x + m[14] * t->m[2].x;
            const double hy = t->py + m[12] * t->m[0].y + m[13] * t->m[1].y + m[14] * t->m[2].y;
            const double hz = t->pz + m[12] * t->m[0].z + m[13] * t->m[1].z + m[14] * t->m[2].z;
            double dx = g_ours.px + target.x - hx;
            double dy = g_ours.py + target.y - hy;
            double dz = g_ours.pz + target.z - hz;
            const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d >= clearance) continue;
            if (d < 1e-4) {
                dx = -t->m[1].x;
                dy = -t->m[1].y;
                dz = 0.0;
            } else {
                dx /= d;
                dy /= d;
                dz /= d;
            }
            const double behind = dx * t->m[1].x + dy * t->m[1].y;
            if (behind < 0.0) {
                dx -= t->m[1].x * behind;
                dy -= t->m[1].y * behind;
            }
            if (dz < 0.0) dz = 0.0;
            const double left = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (left < 0.2) {
                dx = 0.0;
                dy = 0.0;
                dz = 1.0;
            } else {
                dx /= left;
                dy /= left;
                dz /= left;
            }
            const double push = clearance - std::max(d, 1e-4);
            target.x += static_cast<float>(dx * push);
            target.y += static_cast<float>(dy * push);
            target.z += static_cast<float>(dz * push);
        }
    });
    const unsigned frame = g_camera_updates;
    if (frame != g_hand_frame) {
        g_hand_frame = frame;
        const float ease = g_hand_ease;
        const float dt = std::clamp(g_dt, 0.0f, 0.1f);
        const float k = ease > 0.0f ? 1.0f - std::exp(-dt / ease) : 1.0f;
        g_hand_offset.x += (target.x - g_hand_offset.x) * k;
        g_hand_offset.y += (target.y - g_hand_offset.y) * k;
        g_hand_offset.z += (target.z - g_hand_offset.z) * k;
    }
    g_ours.px += g_hand_offset.x;
    g_ours.py += g_hand_offset.y;
    g_ours.pz += g_hand_offset.z;
    g_hand_push = std::sqrt(g_hand_offset.x * g_hand_offset.x + g_hand_offset.y * g_hand_offset.y
                            + g_hand_offset.z * g_hand_offset.z);
}

double g_eye_rel[3] = {0.0, 0.0, 0.0};
bool g_eye_rel_valid = false;
std::atomic<unsigned> g_helper_misses = 0;

void place_eye(void* body) {
    const auto* anchor = reinterpret_cast<const WorldTransform*>(
        static_cast<std::uint8_t*>(body) + kEntityOrientation);
    const float forward = g_eye_forward;
    const float raise = g_eye_up;
    double head[3];
    Vec3 rows[3];
    const bool was_found = g_helper_found;
    g_helper_found = helper_location(body, g_helper_edit, head, rows);
    if (g_helper_found != was_found) {
        bridger::info("eye anchor: {}", g_helper_found ? "head helper" : "origin + eye height");
    }
    if (g_helper_found) {
        g_helper_pos[0] = head[0];
        g_helper_pos[1] = head[1];
        g_helper_pos[2] = head[2];
        if (!g_rows_logged) {
            g_rows_logged = true;
            bridger::info("head bone rows: {:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f}",
                          rows[0].x, rows[0].y, rows[0].z, rows[1].x, rows[1].y, rows[1].z,
                          rows[2].x, rows[2].y, rows[2].z);
        }
        Vec3 look{g_game.m[1].x, g_game.m[1].y, 0.0f};
        look = normalize(look);
        if (look.x == 0.0f && look.y == 0.0f) {
            look = normalize(Vec3{anchor->m[1].x, anchor->m[1].y, 0.0f});
        }

        const Vec3 body_fwd = normalize(Vec3{anchor->m[1].x, anchor->m[1].y, 0.0f});

        Vec3 world_rows[3];
        Vec3 entity_rows[3];
        for (int i = 0; i < 3; ++i) {
            world_rows[i] = normalize(rows[i]);
            const Vec3& r = rows[i];
            entity_rows[i] = normalize(Vec3{
                r.x * anchor->m[0].x + r.y * anchor->m[1].x + r.z * anchor->m[2].x,
                r.x * anchor->m[0].y + r.y * anchor->m[1].y + r.z * anchor->m[2].y,
                r.x * anchor->m[0].z + r.y * anchor->m[1].z + r.z * anchor->m[2].z});
        }
        static float score[2][3] = {};
        static float up_score[2][3] = {};
        const Vec3* spaces[2] = {world_rows, entity_rows};
        for (int s = 0; s < 2; ++s) {
            for (int i = 0; i < 3; ++i) {
                score[s][i] += (dot(spaces[s][i], body_fwd) - score[s][i]) * 0.01f;
                up_score[s][i] += (spaces[s][i].z - up_score[s][i]) * 0.01f;
            }
        }
        int best_s = 0;
        int best_f = 0;
        for (int s = 0; s < 2; ++s) {
            for (int i = 0; i < 3; ++i) {
                if (std::fabs(score[s][i]) > std::fabs(score[best_s][best_f])) {
                    best_s = s;
                    best_f = i;
                }
            }
        }
        int best_u = best_f == 0 ? 1 : 0;
        for (int i = 0; i < 3; ++i) {
            if (i != best_f && std::fabs(up_score[best_s][i]) > std::fabs(up_score[best_s][best_u])) {
                best_u = i;
            }
        }
        Vec3 face = body_fwd;
        Vec3 skull_up{0.0f, 0.0f, 1.0f};
        if (std::fabs(score[best_s][best_f]) > 0.7f && std::fabs(up_score[best_s][best_u]) > 0.7f) {
            face = scale(spaces[best_s][best_f], score[best_s][best_f] < 0.0f ? -1.0f : 1.0f);
            skull_up = scale(spaces[best_s][best_u], up_score[best_s][best_u] < 0.0f ? -1.0f : 1.0f);
        }
        g_face_trusted = face.x != body_fwd.x || face.y != body_fwd.y;
        const Vec3 right = normalize(cross(face, skull_up));
        const float side = g_eye_side;
        Vec3 offset{face.x * forward + skull_up.x * raise + right.x * side,
                    face.y * forward + skull_up.y * raise + right.y * side,
                    face.z * forward + skull_up.z * raise + right.z * side};

        const float ahead = dot(offset, look);
        const float away_now = -dot(look, body_fwd);
        const float clear = g_eye_clearance * std::clamp(1.0f - (away_now + 0.26f) / 0.5f, 0.0f, 1.0f);
        if (ahead < clear) {
            offset.x += look.x * (clear - ahead);
            offset.y += look.y * (clear - ahead);
        }

        {
            const float down = std::clamp(-normalize(g_game.m[1]).z, 0.0f, 1.0f);
            float t = std::clamp((down - 0.17f) / 0.7f, 0.0f, 1.0f);
            t = t * t * (3.0f - 2.0f * t);
            const float out = g_eye_down_forward * t;
            offset.x += look.x * out;
            offset.y += look.y * out;
            offset.z -= g_eye_down_drop * t;
        }
        if (body_fwd.x != 0.0f || body_fwd.y != 0.0f) {
            const Vec3 body_right = normalize(cross(body_fwd, Vec3{0.0f, 0.0f, 1.0f}));
            const float behind = dot(offset, body_fwd);
            if (behind < 0.0f) {
                offset.x -= body_fwd.x * behind;
                offset.y -= body_fwd.y * behind;
            }
            const float away = -dot(look, body_fwd);
            const float turned = dot(look, body_right);
            float t = std::clamp((away + 0.26f) / 1.26f, 0.0f, 1.0f);
            t = t * t * (3.0f - 2.0f * t);
            const float out = g_eye_shoulder * t * (turned >= 0.0f ? 1.0f : -1.0f);
            offset.x += body_right.x * out;
            offset.y += body_right.y * out;
            offset.z += g_eye_shoulder_lift * t;
        }
        g_ours.px = head[0] + offset.x;
        g_ours.py = head[1] + offset.y;
        g_ours.pz = head[2] + offset.z;
        g_eye_rel[0] = g_ours.px - anchor->px;
        g_eye_rel[1] = g_ours.py - anchor->py;
        g_eye_rel[2] = g_ours.pz - anchor->pz;
        g_eye_rel_valid = true;
        keep_eye_clear_of_hands(body);
        apply_fight_eye();
    } else if (g_eye_rel_valid) {
        ++g_helper_misses;
        g_ours.px = anchor->px + g_eye_rel[0];
        g_ours.py = anchor->py + g_eye_rel[1];
        g_ours.pz = anchor->pz + g_eye_rel[2];
        apply_fight_eye();
    } else {
        const Vec3 facing = normalize(Vec3{anchor->m[1].x, anchor->m[1].y, 0.0f});
        g_ours.px = anchor->px + facing.x * forward;
        g_ours.py = anchor->py + facing.y * forward;
        g_ours.pz = anchor->pz + g_eye_height;
    }
}

void snap_to_game() {
    g_x = g_game.px;
    g_y = g_game.py;
    g_z = g_game.pz;
    g_heading = g_game_heading;
    g_pitch = g_game_pitch;
}

void step_input(float dt) {
    const float turn = 90.0f * dt;
    if (held(VK_LEFT)) { g_heading += turn; }
    if (held(VK_RIGHT)) { g_heading -= turn; }
    if (held(VK_UP)) { g_pitch += turn; }
    if (held(VK_DOWN)) { g_pitch -= turn; }
    g_heading = std::fmod(g_heading + 540.0f, 360.0f) - 180.0f;
    g_pitch = std::clamp(g_pitch, -89.0f, 89.0f);

    if (held(VK_ADD) || held(VK_PRIOR)) { g_speed = std::min(400.0f, g_speed.load() * (1.0f + dt)); }
    if (held(VK_SUBTRACT) || held(VK_NEXT)) { g_speed = std::max(0.25f, g_speed.load() / (1.0f + dt)); }

    float pace = g_speed * dt;
    if (held(VK_SHIFT)) { pace *= 4.0f; }
    if (held(VK_CONTROL)) { pace *= 0.25f; }

    const bool wasd = g_use_wasd;
    const bool fwd = held(VK_NUMPAD8) || (wasd && held('W'));
    const bool back = held(VK_NUMPAD5) || held(VK_NUMPAD2) || (wasd && held('S'));
    const bool left = held(VK_NUMPAD4) || (wasd && held('A'));
    const bool right_key = held(VK_NUMPAD6) || (wasd && held('D'));
    const bool down = held(VK_NUMPAD7) || (wasd && held('Q'));
    const bool up_key = held(VK_NUMPAD9) || (wasd && held('E'));

    const Vec3 f = forward_of(g_heading, g_pitch);
    Vec3 right;
    Vec3 up;
    frame_of(f, right, up);

    double move[3] = {0.0, 0.0, 0.0};
    const auto add = [&move](const Vec3& axis, double s) {
        move[0] += axis.x * s;
        move[1] += axis.y * s;
        move[2] += axis.z * s;
    };
    if (fwd) { add(f, 1.0); }
    if (back) { add(f, -1.0); }
    if (right_key) { add(right, 1.0); }
    if (left) { add(right, -1.0); }
    if (up_key) { move[2] += 1.0; }
    if (down) { move[2] -= 1.0; }

    g_x += move[0] * pace;
    g_y += move[1] * pace;
    g_z += move[2] * pace;
}

void on_commit(void* component, const char* source, WorldTransform* out) {
    void* entity = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(component) + kComponentEntity);
    if (entity != g_camera_entity || component != g_component) {
        ++g_entity_swaps;
        {
            const auto name = bridger::rtti_name(bridger::rtti_of(entity));
            const auto length = std::min(name.size(), sizeof g_camera_type - 1);
            std::memcpy(g_camera_type, name.data(), length);
            g_camera_type[length] = 0;
        }
        bridger::info("{} {} commits for camera entity {} ({}) on thread {}", source, component,
                      entity, g_camera_type[0] != 0 ? g_camera_type : "?", GetCurrentThreadId());
        g_camera_entity = entity;
        g_component = component;
        g_source = source;
        g_is_player_camera = false;
        g_followed = nullptr;
    }
    if (const auto is_player = bridger::at_rva<void (*)(const void*, bool*)>(kIsPlayerCamera);
        is_player != nullptr) {
        bool player_camera = false;
        is_player(entity, &player_camera);
        if (player_camera != g_is_player_camera) {
            bridger::info("camera entity {} player camera: {}", entity, player_camera);
            g_is_player_camera = player_camera;
        }
    }
    ++g_commits;

    LARGE_INTEGER now;
    LARGE_INTEGER freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_last_commit.QuadPart != 0
                   ? static_cast<float>(static_cast<double>(now.QuadPart - g_last_commit.QuadPart)
                                        / static_cast<double>(freq.QuadPart))
                   : 0.0f;
    g_last_commit = now;
    dt = std::clamp(dt, 0.0f, 0.1f);
    g_dt = dt;

    g_game = *out;
    ++g_camera_updates;

    g_steer_applied = false;
    if (g_first_person && g_use_steer_angles && g_player != nullptr) {
        bridger::guarded([&] {
            auto* player_game = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(g_player) + kPlayerGameFromEntity);
            if (player_game == nullptr) return;
            if (player_game != g_player_game) {
                if (bridger::rtti_name(bridger::rtti_of(player_game)) != "PlayerGame") return;
                g_player_game = player_game;
            }
            const float pitch = *reinterpret_cast<float*>(player_game + kPlayerLookPitch) * 180.0f / static_cast<float>(kPi);
            const float heading = *reinterpret_cast<float*>(player_game + kPlayerLookHeading) * 180.0f / static_cast<float>(kPi);
            if (!std::isfinite(pitch) || !std::isfinite(heading) || std::fabs(pitch) > 90.0f) return;
            Vec3 rows[3];
            rows_for(heading, pitch, rows);
            g_game.m[0] = rows[0];
            g_game.m[1] = rows[1];
            g_game.m[2] = rows[2];
            g_steer_applied = true;
        });
    }
    angles_of(normalize(g_game.m[1]), g_game_heading, g_game_pitch);
    g_game_fov = *reinterpret_cast<const float*>(static_cast<std::uint8_t*>(entity) + kCameraFov);
    g_game_near = *reinterpret_cast<const float*>(static_cast<std::uint8_t*>(entity) + kCameraNearPlane);

    if (g_first_person) {
        calibrate(g_game);
        first_person_commit(entity, out);
        return;
    }
    sync_body_visibility(nullptr);
    if (!g_enabled) {
        calibrate(g_game);
        g_override_valid = false;
        return;
    }
    if (g_snap_pending.exchange(false)) {
        if (g_convention < 0) {
            g_enabled = false;
        g_view_valid = false;
            bridger::warn("freecam refused: the game's rotation basis has not been recognised yet "
                          "(error {:.4f}), try again with the camera level", g_convention_error);
            return;
        }
        snap_to_game();
        bridger::info("freecam anchored at {:.1f} {:.1f} {:.1f} heading {:.1f} pitch {:.1f}", g_x,
                      g_y, g_z, g_heading, g_pitch);
        bridger::info("game basis m0 {:.3f} {:.3f} {:.3f}  m1 {:.3f} {:.3f} {:.3f}  m2 {:.3f} {:.3f} {:.3f}",
                      g_game.m[0].x, g_game.m[0].y, g_game.m[0].z, g_game.m[1].x, g_game.m[1].y,
                      g_game.m[1].z, g_game.m[2].x, g_game.m[2].y, g_game.m[2].z);
        build_transform();
        bridger::info("ours basis m0 {:.3f} {:.3f} {:.3f}  m1 {:.3f} {:.3f} {:.3f}  m2 {:.3f} {:.3f} {:.3f}",
                      g_ours.m[0].x, g_ours.m[0].y, g_ours.m[0].z, g_ours.m[1].x, g_ours.m[1].y,
                      g_ours.m[1].z, g_ours.m[2].x, g_ours.m[2].y, g_ours.m[2].z);
    }
    step_input(dt);
    build_transform();
    *out = g_ours;
    g_override_valid = true;
    ++g_overrides;
}

constexpr std::uintptr_t kVehicleSetAlpha = 0x2c9f220;
constexpr std::uintptr_t kVehicleProximityAlphaReturn = 0x2caac7f;
using VehicleSetAlphaFn = void (*)(void*, float);
bridger::Hook<VehicleSetAlphaFn> g_vehicle_alpha_hook;
std::atomic<bool> g_keep_vehicles = true;

void vehicle_alpha_detour(void* vehicle, float alpha) {
    if (g_keep_vehicles && (g_enabled || g_first_person)) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        if (caller - bridger::image_base() == kVehicleProximityAlphaReturn) {
            alpha = 1.0f;
        }
    }
    g_vehicle_alpha_hook.call(vehicle, alpha);
}

constexpr std::uintptr_t kSetShaderVariable = 0x2102390;
constexpr std::uint32_t kAlphaVariableHash = 0x42a726aa;
using SetShaderVariableFn = void (*)(void*, const std::uint32_t*, float*, int, int);
bridger::Hook<SetShaderVariableFn> g_shader_variable_hook;
constexpr std::size_t kMaxOwnedVariables = 512;
std::atomic<void*> g_owned_variables[kMaxOwnedVariables] = {};
std::atomic<unsigned> g_owned_variable_count = 0;
std::atomic<void*> g_owned_entities[512] = {};
std::atomic<unsigned> g_owned_entity_count = 0;
std::atomic<bool> g_keep_sam_opaque = true;
std::atomic<unsigned> g_alpha_overrides = 0;

constexpr std::size_t kMaxFadeCallers = 16;
std::atomic<std::uintptr_t> g_fade_callers[kMaxFadeCallers] = {};
std::atomic<unsigned> g_fade_caller_count = 0;
std::mutex g_fade_caller_lock;

bool is_fade_caller(std::uintptr_t caller) {
    const unsigned n = g_fade_caller_count;
    for (unsigned i = 0; i < n && i < kMaxFadeCallers; ++i) {
        if (g_fade_callers[i].load(std::memory_order_relaxed) == caller) return true;
    }
    return false;
}

std::atomic<unsigned> g_variable_log_budget = 0;

void note_faded_variables(void* variables);

void shader_variable_detour(void* variables, const std::uint32_t* hash, float* value, int count, int index) {
    if (g_variable_log_budget.load(std::memory_order_relaxed) > 0 && hash != nullptr && value != nullptr) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - bridger::image_base();
        const unsigned n = g_owned_variable_count;
        bool owned = false;
        for (unsigned i = 0; i < n && i < kMaxOwnedVariables; ++i) {
            if (g_owned_variables[i].load(std::memory_order_relaxed) == variables) owned = true;
        }
        if (true) {
            static std::mutex lock;
            static std::vector<std::tuple<void*, std::uint32_t, int, float>> last;
            bool changed = true;
            {
                std::lock_guard guard(lock);
                for (auto& [v, h, i, f] : last) {
                    if (v == variables && h == *hash && i == index) {
                        changed = std::fabs(f - *value) > 0.001f;
                        f = *value;
                        goto found;
                    }
                }
                last.emplace_back(variables, *hash, index, *value);
            found:;
            }
            if (changed && g_variable_log_budget.fetch_sub(1) > 0) {
                bridger::info("variable change: hash {:08x} = {:.3f} (count {} index {}) on {} ({}) owned {} from rva {:#x}",
                              *hash, *value, count, index, variables, bridger::rtti_name(bridger::rtti_of(variables)),
                              owned, caller);
            }
        }
    }
    if (g_first_person && g_keep_sam_opaque && hash != nullptr && value != nullptr
        && *hash == kAlphaVariableHash && *value < 1.0f) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - bridger::image_base();
        bool block = is_fade_caller(caller);
        if (block) note_faded_variables(variables);
        if (!block) {
            const unsigned n = g_owned_variable_count;
            for (unsigned i = 0; i < n && i < kMaxOwnedVariables; ++i) {
                if (g_owned_variables[i].load(std::memory_order_relaxed) == variables) {
                    block = true;
                    std::lock_guard lock(g_fade_caller_lock);
                    if (!is_fade_caller(caller) && g_fade_caller_count < kMaxFadeCallers) {
                        g_fade_callers[g_fade_caller_count].store(caller);
                        ++g_fade_caller_count;
                        bridger::info("player fade routine learned: rva {:#x} (alpha {:.2f})", caller, *value);
                    }
                    break;
                }
            }
        }
        if (block) {
            ++g_alpha_overrides;
            float opaque = 1.0f;
            g_shader_variable_hook.call(variables, hash, &opaque, count, index);
            return;
        }
    }
    g_shader_variable_hook.call(variables, hash, value, count, index);
}

void collect_descendants(void* entity, std::vector<void*>& out, int depth) {
    const auto get = bridger::at_rva<GetChildrenFn>(kGetChildren);
    if (get == nullptr || entity == nullptr || depth > 4 || out.size() >= 512) return;
    std::vector<void*> mine;
    bridger::guarded([&] {
        EngineArray list{};
        get(&list, entity);
        for (std::uint32_t i = 0; list.data != nullptr && i < list.count && i < 256; ++i) {
            if (list.data[i] != nullptr) mine.push_back(list.data[i]);
        }
    });
    for (void* child : mine) {
        if (bridger::rtti_name(bridger::rtti_of(child)) == "CameraEntity") continue;
        out.push_back(child);
        collect_descendants(child, out, depth + 1);
    }
}

constexpr std::uintptr_t kModelShow = 0x2187870;
constexpr std::uintptr_t kProximityShowReturns[] = {0x2670899, 0x2670955, 0x2754f7a};
using ModelShowFn = std::uint64_t (*)(void*, bool, std::uint64_t);
bridger::Hook<ModelShowFn> g_model_show_hook;
std::atomic<unsigned> g_show_forced = 0;

bool owned_entity(void* entity);

std::uint64_t model_show_detour(void* entity, bool show, std::uint64_t extra) {
    if (g_variable_log_budget.load(std::memory_order_relaxed) > 0) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - bridger::image_base();
        static std::mutex lock;
        static std::vector<std::pair<void*, bool>> last;
        bool changed = true;
        {
            std::lock_guard guard(lock);
            auto it = std::find_if(last.begin(), last.end(), [&](const auto& p) { return p.first == entity; });
            if (it != last.end()) {
                changed = it->second != show;
                it->second = show;
            } else {
                last.emplace_back(entity, show);
            }
        }
        if (changed && g_variable_log_budget.fetch_sub(1) > 0) {
            bridger::info("show flag change: {} on {} ({}) owned {} from rva {:#x}", show ? 1 : 0, entity,
                          bridger::rtti_name(bridger::rtti_of(entity)), owned_entity(entity), caller);
        }
    }
    if (!show && g_first_person && g_keep_sam_opaque) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - bridger::image_base();
        for (const auto site : kProximityShowReturns) {
            if (caller == site) {
                if (g_show_forced.fetch_add(1) < 3) {
                    bridger::info("kept gear drawn: proximity hide from rva {:#x}", caller);
                }
                return g_model_show_hook.call(entity, true, extra);
            }
        }
        if (owned_entity(entity)) {
            static std::atomic<unsigned> logged = 0;
            if (logged.fetch_add(1) < 20) {
                bridger::info("model hidden by other caller rva {:#x} on {} ({})", caller, entity,
                              bridger::rtti_name(bridger::rtti_of(entity)));
            }
        }
    }
    return g_model_show_hook.call(entity, show, extra);
}

constexpr std::uintptr_t kBackpackShow = 0x2965c60;
constexpr std::uintptr_t kBackpackProximityReturn = 0x26710ee;
using BackpackShowFn = void (*)(void*, bool, bool);
bridger::Hook<BackpackShowFn> g_backpack_show_hook;
std::atomic<unsigned> g_backpack_show_forced = 0;

void backpack_show_detour(void* backpack, bool show, bool extra) {
    if (!show && g_first_person && g_keep_sam_opaque) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - bridger::image_base();
        if (caller == kBackpackProximityReturn) {
            if (g_backpack_show_forced.fetch_add(1) < 3) {
                bridger::info("kept cargo drawn: backpack proximity hide from rva {:#x}", caller);
            }
            show = true;
        }
    }
    g_backpack_show_hook.call(backpack, show, extra);
}

std::atomic<bool> g_alpha_test_zero = false;
std::mutex g_faded_lock;
std::vector<void*> g_faded_variables;

void note_faded_variables(void* variables) {
    std::lock_guard lock(g_faded_lock);
    if (std::find(g_faded_variables.begin(), g_faded_variables.end(), variables) == g_faded_variables.end()
        && g_faded_variables.size() < 1024) {
        g_faded_variables.push_back(variables);
    }
}

void alpha_test_toggle() {
    const bool zero = !g_alpha_test_zero.load();
    g_alpha_test_zero = zero;
    bridger::run_on_game_thread([] {
        std::uint32_t hash = kAlphaVariableHash;
        float value = g_alpha_test_zero.load() ? 0.0f : 1.0f;
        std::vector<void*> targets;
        {
            std::lock_guard lock(g_faded_lock);
            targets = g_faded_variables;
        }
        unsigned written = 0;
        for (void* variables : targets) {
            if (bridger::guarded([&] { g_shader_variable_hook.call(variables, &hash, &value, 1, -1); })) ++written;
        }
        bridger::info("alpha test: wrote {:.0f} to {} faded components", value, written);
    });
}

std::string game_dump_path(const char* name);
std::vector<std::unique_ptr<bridger::fx::DrawHook>> g_skip_hooks;
std::string g_skip_loaded;

bool skip_draw(const BridgerFxDraw*, void*) { return false; }

void update_skip_pipelines() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 500) return;
    last = now;
    std::string text;
    if (FILE* file = std::fopen(game_dump_path("skip_pipelines.txt").c_str(), "r")) {
        char buffer[4096];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, n);
        std::fclose(file);
    }
    if (text == g_skip_loaded) return;
    g_skip_loaded = text;
    g_skip_hooks.clear();
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !std::isxdigit(static_cast<unsigned char>(text[i]))) ++i;
        std::size_t j = i;
        while (j < text.size() && std::isxdigit(static_cast<unsigned char>(text[j]))) ++j;
        if (j - i == 16) {
            auto hook = std::make_unique<bridger::fx::DrawHook>();
            if (hook->create(std::stoull(text.substr(i, 16), nullptr, 16), skip_draw)) g_skip_hooks.push_back(std::move(hook));
        }
        i = j;
    }
    bridger::info("skip pipelines: {} hooked", g_skip_hooks.size());
}

constexpr std::size_t kProbeCounts = 256;
std::atomic<std::uint32_t> g_probe_counts[kProbeCounts] = {};
std::atomic<unsigned> g_probe_count_n = 0;
struct ProbeHit {
    std::uint64_t pipeline;
    std::uint32_t count;
    std::uint32_t hits;
};
std::mutex g_probe_lock;
std::vector<ProbeHit> g_probe_hits;
std::vector<std::unique_ptr<bridger::fx::DrawHook>> g_probe_hooks;
ULONGLONG g_probe_until = 0;
std::string g_probe_loaded;

bool probe_draw(const BridgerFxDraw* draw, void*) {
    const unsigned n = g_probe_count_n.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < n; ++i) {
        if (g_probe_counts[i].load(std::memory_order_relaxed) == draw->count) {
            std::lock_guard lock(g_probe_lock);
            for (auto& h : g_probe_hits) {
                if (h.pipeline == draw->pipeline && h.count == draw->count) {
                    ++h.hits;
                    return true;
                }
            }
            g_probe_hits.push_back({draw->pipeline, draw->count, 1});
            return true;
        }
    }
    return true;
}

void update_count_probe() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (g_probe_until != 0 && now >= g_probe_until) {
        g_probe_hooks.clear();
        g_probe_until = 0;
        std::vector<ProbeHit> hits;
        {
            std::lock_guard lock(g_probe_lock);
            hits.swap(g_probe_hits);
        }
        std::sort(hits.begin(), hits.end(), [](const ProbeHit& a, const ProbeHit& b) { return a.hits > b.hits; });
        bridger::info("count probe: {} matches", hits.size());
        for (const auto& h : hits) {
            bridger::info("count probe: pipeline {:016x} count {} draws {}", h.pipeline, h.count, h.hits);
        }
    }
    if (now - last < 500) return;
    last = now;
    std::string text;
    if (FILE* file = std::fopen(game_dump_path("count_probe.txt").c_str(), "r")) {
        char buffer[8192];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, n);
        std::fclose(file);
    }
    if (text == g_probe_loaded || g_probe_until != 0) return;
    g_probe_loaded = text;
    unsigned count = 0;
    std::size_t i = 0;
    while (i < text.size() && count < kProbeCounts) {
        while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
        std::size_t j = i;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
        if (j > i) g_probe_counts[count++].store(static_cast<std::uint32_t>(std::stoul(text.substr(i, j - i))));
        i = j;
    }
    g_probe_count_n = count;
    if (count == 0) return;
    for (const auto& p : bridger::fx::pipelines()) {
        if (p.compute || p.draws_last_frame == 0) continue;
        auto hook = std::make_unique<bridger::fx::DrawHook>();
        if (hook->create(p.hash, probe_draw)) g_probe_hooks.push_back(std::move(hook));
    }
    g_probe_until = now + 2000;
    bridger::info("count probe: {} counts, {} pipelines hooked", count, g_probe_hooks.size());
}

constexpr std::uintptr_t kGearFadeRoutine = 0x26705a0;
using GearFadeFn = void (*)(void*, float);
bridger::Hook<GearFadeFn> g_gear_fade_hook;
std::atomic<void*> g_gear_fade_owner = nullptr;

void gear_fade_detour(void* owner, float value) {
    if (g_gear_fade_owner.exchange(owner) != owner) {
        void* inner = nullptr;
        bridger::guarded([&] { inner = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(owner) + 0x60); });
        bridger::info("gear fade routine: owner {} ({}) arg {:.3f} inner {} ({})", owner,
                      bridger::rtti_name(bridger::rtti_of(owner)), value, inner,
                      inner ? bridger::rtti_name(bridger::rtti_of(inner)) : std::string_view("-"));
    }
    g_gear_fade_hook.call(owner, value);
}

constexpr std::size_t kWatchHits = 64;
std::atomic<std::uintptr_t> g_watch_rips[kWatchHits] = {};
std::atomic<std::uintptr_t> g_watch_stacks[kWatchHits][4] = {};
std::atomic<unsigned> g_watch_hit_count = 0;
std::atomic<unsigned> g_watch_total = 0;
std::atomic<std::uintptr_t> g_watch_address = 0;
PVOID g_watch_handler = nullptr;
std::string g_watch_loaded;
unsigned g_watch_logged = 0;

LONG CALLBACK watch_handler(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    auto* ctx = info->ContextRecord;
    if ((ctx->Dr6 & 1) == 0 || g_watch_address.load(std::memory_order_relaxed) == 0) return EXCEPTION_CONTINUE_SEARCH;
    ctx->Dr6 = 0;
    ++g_watch_total;
    const std::uintptr_t base = bridger::image_base();
    const std::uintptr_t rip = ctx->Rip;
    const unsigned n = g_watch_hit_count.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < n && i < kWatchHits; ++i) {
        if (g_watch_rips[i].load(std::memory_order_relaxed) == rip) return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (n < kWatchHits) {
        g_watch_rips[n].store(rip, std::memory_order_relaxed);
        unsigned found = 0;
        const auto* stack = reinterpret_cast<const std::uintptr_t*>(ctx->Rsp);
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(stack, &mbi, sizeof mbi) != 0 && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY)) != 0) {
            for (unsigned s = 0; s < 64 && found < 4; ++s) {
                const std::uintptr_t v = stack[s];
                if (v > base && v < base + 0x8000000) g_watch_stacks[n][found++].store(v, std::memory_order_relaxed);
            }
        }
        g_watch_hit_count.store(n + 1, std::memory_order_relaxed);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void watch_apply(std::uintptr_t address) {
    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    unsigned applied = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (thread == nullptr) continue;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &ctx)) {
                if (address != 0) {
                    ctx.Dr0 = address;
                    ctx.Dr7 = (ctx.Dr7 & ~0x000F0003ull) | 0x1ull | (0x1ull << 16) | (0x3ull << 18);
                } else {
                    ctx.Dr0 = 0;
                    ctx.Dr7 &= ~0x000F0003ull;
                }
                if (SetThreadContext(thread, &ctx)) ++applied;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snap);
    bridger::info("watchpoint {:#x}: applied to {} threads", address, applied);
}

void update_watchpoint() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    {
        static std::string loaded;
        static ULONGLONG checked = 0;
        if (now - checked >= 250) {
            checked = now;
            std::string text;
            if (FILE* file = std::fopen(game_dump_path("var_log.txt").c_str(), "r")) {
                char buffer[64];
                std::size_t read;
                while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, read);
                std::fclose(file);
            }
            if (text != loaded) {
                loaded = text;
                const unsigned budget = static_cast<unsigned>(std::strtoul(text.c_str(), nullptr, 10));
                g_variable_log_budget = budget;
                bridger::info("variable change log armed: {}", budget);
            }
        }
    }
    const unsigned n = g_watch_hit_count.load();
    for (; g_watch_logged < n && g_watch_logged < kWatchHits; ++g_watch_logged) {
        const auto base = bridger::image_base();
        const auto rip = g_watch_rips[g_watch_logged].load();
        std::string stack;
        for (int s = 0; s < 4; ++s) {
            const auto v = g_watch_stacks[g_watch_logged][s].load();
            if (v != 0) stack += std::format(" {:#x}", v - base);
        }
        if (rip > base && rip < base + 0x8000000) {
            bridger::info("watch hit: writer rva {:#x}, stack{} (total hits {})", rip - base, stack, g_watch_total.load());
        } else {
            bridger::info("watch hit: writer {:#x} (outside the game image), stack{} (total hits {})", rip, stack, g_watch_total.load());
        }
    }
    if (now - last < 500) return;
    last = now;
    std::string text;
    if (FILE* file = std::fopen(game_dump_path("watch_address.txt").c_str(), "r")) {
        char buffer[256];
        std::size_t read;
        while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, read);
        std::fclose(file);
    }
    if (text == g_watch_loaded) return;
    g_watch_loaded = text;
    std::uintptr_t address = 0;
    for (char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            address = address * 16 + static_cast<std::uintptr_t>(std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (std::tolower(c) - 'a' + 10));
        }
    }
    if (address != 0 && g_watch_handler == nullptr) g_watch_handler = AddVectoredExceptionHandler(1, watch_handler);
    g_watch_address = address;
    watch_apply(address);
}

void watch_shutdown() {
    if (g_watch_address.load() != 0) watch_apply(0);
    g_watch_address = 0;
    if (g_watch_handler != nullptr) {
        RemoveVectoredExceptionHandler(g_watch_handler);
        g_watch_handler = nullptr;
    }
}

constexpr std::uint64_t kGearDitherPipelines[] = {0x710434a5b5c0e55e, 0x6ab1517077b514c9, 0x6746851c9beb738a,
                                                   0x48ce79216863f5f3, 0xedb2c7f2fd3a0ae5};
std::vector<std::unique_ptr<bridger::fx::Replacement>> g_gear_dither_off;
std::atomic<bool> g_solid_gear = true;

void sync_gear_dither(bool off) {
    if (off == !g_gear_dither_off.empty()) return;
    if (!off) {
        g_gear_dither_off.clear();
        bridger::info("gear dither restored");
        return;
    }
    std::vector<std::uint64_t> pipelines;
    wchar_t module[MAX_PATH];
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring dir(module);
    dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\Bridger\\mods\\freecam\\shaders\\";
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((dir + L"gear_nodither_*.dxbc").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name(found.cFileName);
            const auto start = name.find(L"gear_nodither_") + 14;
            if (name.size() >= start + 16) {
                pipelines.push_back(std::stoull(std::wstring(name.substr(start, 16)), nullptr, 16));
            }
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    if (pipelines.empty()) pipelines.assign(std::begin(kGearDitherPipelines), std::end(kGearDitherPipelines));
    unsigned applied = 0;
    for (const auto pipeline : pipelines) {
        const auto path = std::format("shaders/gear_nodither_{:016x}.dxbc", pipeline);
        auto replacement = std::make_unique<bridger::fx::Replacement>();
        if (replacement->create(pipeline, BRIDGER_FX_STAGE_PS, path.c_str())) {
            ++applied;
            g_gear_dither_off.push_back(std::move(replacement));
        }
    }
    bridger::info("gear dither off: {} of {} pipelines replaced", applied, pipelines.size());
}

void update_owned_variables() {
    static std::vector<void*> tree;
    static void* tree_owner = nullptr;
    static ULONGLONG refreshed = 0;
    const ULONGLONG now = GetTickCount64();
    if (g_player != tree_owner || now - refreshed > 5000) {
        tree.clear();
        collect_descendants(g_player, tree, 0);
        tree_owner = g_player;
        refreshed = now;
    }
    unsigned n = 0;
    auto add = [&](void* entity) {
        if (entity == nullptr || n >= kMaxOwnedVariables) return;
        const auto plausible = [](std::uintptr_t p) { return p >= 0x10000 && p < 0x7ff000000000 && (p & 7) == 0; };
        if (bridger::rtti_name(bridger::rtti_of(entity)).empty()) return;
        const auto data = *reinterpret_cast<std::uintptr_t*>(static_cast<std::uint8_t*>(entity) + kEntityArtParts);
        if (!plausible(data) || bridger::rtti_name(bridger::rtti_of(reinterpret_cast<void*>(data))) != "ArtPartsData") return;
        bridger::guarded([&] {
            const auto variables = *reinterpret_cast<std::uintptr_t*>(data + 0xf0);
            if (plausible(variables)) g_owned_variables[n++].store(reinterpret_cast<void*>(variables), std::memory_order_relaxed);
        });
    };
    add(g_player);
    for (void* entity : tree) add(entity);
    g_owned_variable_count = n;
    unsigned e = 0;
    g_owned_entities[e++].store(g_player, std::memory_order_relaxed);
    for (void* entity : tree) {
        if (e >= kMaxOwnedVariables) break;
        g_owned_entities[e++].store(entity, std::memory_order_relaxed);
    }
    g_owned_entity_count = e;
}

bool owned_entity(void* entity) {
    const unsigned n = g_owned_entity_count;
    for (unsigned i = 0; i < n && i < kMaxOwnedVariables; ++i) {
        if (g_owned_entities[i].load(std::memory_order_relaxed) == entity) return true;
    }
    return false;
}

void set_visible_detour(void* entity, bool visible) {
    if (g_trace_visibility && !visible && g_trace_lines < kTraceLimit) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const auto base = bridger::image_base();
        const auto rva = caller > base ? caller - base : 0;
        static std::uintptr_t seen[kTraceLimit] = {};
        for (unsigned i = 0; i < g_trace_lines; ++i) {
            if (seen[i] == rva) {
                return g_set_visible_hook.call(entity, visible);
            }
        }
        seen[g_trace_lines] = rva;
        ++g_trace_lines;
        bridger::info("SetVisible(false) on {} ({}) from rva {:#x}", entity,
                      bridger::rtti_name(bridger::rtti_of(entity)), rva);
    }
    g_set_visible_hook.call(entity, visible);
}

void tpc_commit_detour(void* self) {
    on_commit(self, "ThirdPersonPlayerCameraComponent",
              reinterpret_cast<WorldTransform*>(static_cast<std::uint8_t*>(self) + kTpcOutput));
    g_tpc_commit.call(self);
}

void ds_tpc_commit_detour(void* self, bool flag) {
    on_commit(self, "DSThirdPersonPlayerCameraComponent",
              reinterpret_cast<WorldTransform*>(static_cast<std::uint8_t*>(self) + kDsTpcOutput));
    g_ds_tpc_commit.call(self, flag);
}

template<class T> T& field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
}

std::atomic<bool> g_recording = false;
std::atomic<int> g_marker = 0;
std::atomic<bool> g_marker_request = false;
FILE* g_record_file = nullptr;
unsigned g_record_frame = 0;
unsigned g_f_turn_checks = 0;
unsigned g_f_turn_applies = 0;
float g_f_turn_amount = 0.0f;
unsigned g_f_slot610 = 0;
unsigned g_f_slot610_phase = 0;
unsigned g_f_aim_calls = 0;
float g_f_aim_heading = 0.0f;

using Slot610Fn = void (*)(void*, float, unsigned char);
bridger::Hook<Slot610Fn> g_slot610;

std::string game_dump_path(const char* name) {
    wchar_t module[MAX_PATH];
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring dir(module);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring path = dir + L"\\Bridger\\dumps\\";
    std::string out(path.begin(), path.end());
    return out + name;
}

void record_open() {
    if (g_record_file != nullptr) {
        return;
    }
    const auto path = game_dump_path("turn_record.csv");
    g_record_file = std::fopen(path.c_str(), "w");
    if (g_record_file == nullptr) {
        bridger::error("recorder: cannot open {}", path);
        g_recording = false;
        return;
    }
    std::fputs("frame,marker,dt,w0,w1,w2,w3,w4,w5,w6,w7,body_heading,camera_heading,aim_heading,"
               "aim_calls,speed,p314,c4289,ctrl4270,ctrl4278,ctrl4280,ctrl4289,"
               "turn_checks,aim_turn,turn_applies,turn_amount,slot610,slot610_phases,first_person\n",
               g_record_file);
    g_record_frame = 0;
    bridger::info("recorder: writing {}", path);
}

void record_close() {
    if (g_record_file != nullptr) {
        std::fclose(g_record_file);
        g_record_file = nullptr;
        bridger::info("recorder: closed after {} frames", g_record_frame);
    }
}

void toggle_recording() {
    g_recording = !g_recording;
    bridger::info("recorder {}", g_recording ? "started (F8 marks)" : "stopped");
}

void drop_marker() {
    if (g_recording) {
        g_marker_request = true;
    }
}

void inject_turn_input(void* state) {
    void* player = field<void*>(state, 0x30);
    void* controller = field<void*>(state, 0x418);
    void* params = field<void*>(state, 0x428);
    if (player == nullptr || controller == nullptr || params == nullptr) {
        g_injecting = false;
        return;
    }
    if (g_player == nullptr && bridger::rtti_name(bridger::rtti_of(player)) == "DSPlayerEntity") {
        g_player = player;
    }
    if (!g_first_person || !g_turn_body || !g_view_valid || g_in_vehicle || player != g_player) {
        g_injecting = false;
        return;
    }
    const auto* orientation = reinterpret_cast<const WorldTransform*>(
        static_cast<std::uint8_t*>(player) + kEntityOrientation);
    const float body = heading_of(normalize(Vec3{orientation->m[1].x, orientation->m[1].y, 0.0f}));
    g_body_heading = body;
    const float delta = wrap_degrees(g_view_heading + g_body_offset - body);
    g_turn_delta = delta;
    float& x = field<float>(controller, kCtrlStickX);
    float& y = field<float>(controller, kCtrlStickY);
    const bool user_input = std::sqrt(x * x + y * y) > 0.15f;
    const int locomotion = field<int>(params, kParamsState);
    const bool pivoting = locomotion == 1
                          || (field<std::uint32_t>(params, kParamsFlagsWord4) & kMoveHeadingToTurnHeading) != 0;
    g_last_state = locomotion;
    if (user_input) {
        g_injecting = false;
        g_cooldown = 0;
        return;
    }
    if (g_injecting) {
        const bool tap_over = !g_turn_hold && g_burst_frames >= g_tap_frames;
        if (std::fabs(delta) < g_turn_done || tap_over) {
            g_injecting = false;
            g_cooldown = g_tap_cooldown;
            return;
        }
        x = 0.0f;
        y = g_turn_stick;
        ++g_inject_frames;
        ++g_burst_frames;
        return;
    }
    if (g_cooldown > 0) {
        if (pivoting) {
            g_cooldown = g_tap_cooldown;
        } else {
            --g_cooldown;
        }
        return;
    }
    if (std::fabs(delta) > g_turn_threshold) {
        g_injecting = true;
        g_burst_frames = 0;
        ++g_inject_bursts;
        x = 0.0f;
        y = g_turn_stick;
        ++g_inject_frames;
        ++g_burst_frames;
    }
}

void player_update_detour(void* state, float dt) {
    inject_turn_input(state);
    g_player_update.call(state, dt);
    if (!g_recording) {
        record_close();
        return;
    }
    record_open();
    if (g_record_file == nullptr) {
        return;
    }
    void* player = field<void*>(state, 0x30);
    void* controller = field<void*>(state, 0x418);
    void* params = field<void*>(state, 0x428);
    if (player == nullptr || params == nullptr) {
        return;
    }
    if (g_player == nullptr && bridger::rtti_name(bridger::rtti_of(player)) == "DSPlayerEntity") {
        g_player = player;
        bridger::info("recorder: player entity {}", player);
    }
    if (player != g_player) {
        return;
    }
    if (g_marker_request.exchange(false)) {
        ++g_marker;
        bridger::info("recorder: marker {}", g_marker.load());
    }
    const auto* orientation = reinterpret_cast<const WorldTransform*>(
        static_cast<std::uint8_t*>(player) + kEntityOrientation);
    const float body = heading_of(normalize(Vec3{orientation->m[1].x, orientation->m[1].y, 0.0f}));
    const float camera = heading_of(normalize(g_game.m[1]));
    static double last_pos[3] = {0.0, 0.0, 0.0};
    const double dx = orientation->px - last_pos[0];
    const double dy = orientation->py - last_pos[1];
    const double speed = dt > 0.0f ? std::sqrt(dx * dx + dy * dy) / dt : 0.0;
    last_pos[0] = orientation->px;
    last_pos[1] = orientation->py;
    last_pos[2] = orientation->pz;
    std::uint32_t w[8];
    for (int i = 0; i < 8; ++i) {
        w[i] = field<std::uint32_t>(params, 0x570 + i * 4);
    }
    const auto ctrl = [&](std::size_t off) -> unsigned {
        return controller != nullptr ? field<std::uint8_t>(controller, off) : 0u;
    };
    std::fprintf(g_record_file,
                 "%u,%d,%.4f,%x,%x,%x,%x,%x,%x,%x,%x,%.1f,%.1f,%.1f,%u,%.2f,%d,%u,%u,%u,%u,%u,"
                 "%u,%u,%u,%.3f,%u,%u,%d\n",
                 g_record_frame, g_marker.load(), dt, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                 body, camera, g_f_aim_heading, g_f_aim_calls, speed, field<int>(params, 0x314),
                 ctrl(0x4289), ctrl(0x4270), ctrl(0x4278), ctrl(0x4280), ctrl(0x4289),
                 g_f_turn_checks, (w[2] & 0x8000u) != 0 ? 1u : 0u, g_f_turn_applies,
                 g_f_turn_amount, g_f_slot610, g_f_slot610_phase, g_first_person ? 1 : 0);
    ++g_record_frame;
    if ((g_record_frame % 30) == 0) {
        std::fflush(g_record_file);
    }
    g_f_turn_checks = 0;
    g_f_turn_applies = 0;
    g_f_turn_amount = 0.0f;
    g_f_slot610 = 0;
    g_f_slot610_phase = 0;
    g_f_aim_calls = 0;
}

AimVector* aim_direction_detour(void* control, AimVector* out) {
    AimVector* result = g_aim_direction.call(control, out);
    if (result != nullptr && g_player != nullptr && field<void*>(control, 0x30) == g_player) {
        ++g_f_aim_calls;
        g_f_aim_heading = heading_of(Vec3{result->x, result->y, 0.0f});
    }
    return result;
}

void turn_check_detour(void* self) {
    g_turn_check.call(self);
    ++g_turn_checks;
    ++g_f_turn_checks;
    void* state = field<void*>(self, 0x50);
    void* params = state != nullptr ? field<void*>(state, 0x428) : nullptr;
    if (params != nullptr && (field<std::uint32_t>(params, 0x578) & 0x8000u) != 0) {
        ++g_turn_flagged;
    }
}

void turn_apply_detour(void* self, float amount) {
    ++g_turn_applies;
    ++g_f_turn_applies;
    g_f_turn_amount = amount;
    g_turn_apply.call(self, amount);
}

void slot610_detour(void* self, float value, unsigned char phase) {
    ++g_f_slot610;
    g_f_slot610_phase |= 1u << (phase & 31);
    g_slot610.call(self, value, phase);
}

struct FlickRecord {
    float move = 0.0f;
    float turn = 0.0f;
    float game_turn = 0.0f;
    bool passthrough = false;
    int dropout = 0;
    unsigned helper_misses = 0;
    float fight = 0.0f;
};
std::atomic<unsigned> g_flicks = 0;
FlickRecord g_last_flick{};
WorldTransform g_prev_sent{};
bool g_prev_sent_valid = false;
Vec3 g_prev_game_view{};
unsigned g_prev_helper_misses = 0;

struct FlightRow {
    double t;
    double sent[3];
    float sent_heading, sent_pitch;
    double game[3];
    float game_heading, game_pitch;
    double head[3];
    bool head_found;
    double body[3];
    float body_heading;
    bool passthrough, override_valid, in_vehicle;
    int dropout;
    float fight;
    unsigned helper_misses;
};
constexpr std::size_t kFlightRows = 256;
FlightRow g_flight[kFlightRows]{};
std::size_t g_flight_next = 0;
std::mutex g_flight_lock;
std::atomic<unsigned> g_flight_dumps = 0;

void flight_record(const WorldTransform& sent, bool passthrough) {
    LARGE_INTEGER now;
    LARGE_INTEGER freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    FlightRow row{};
    row.t = static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
    row.sent[0] = sent.px; row.sent[1] = sent.py; row.sent[2] = sent.pz;
    angles_of(normalize(sent.m[1]), row.sent_heading, row.sent_pitch);
    row.game[0] = g_game.px; row.game[1] = g_game.py; row.game[2] = g_game.pz;
    angles_of(normalize(g_game.m[1]), row.game_heading, row.game_pitch);
    row.head[0] = g_helper_pos[0]; row.head[1] = g_helper_pos[1]; row.head[2] = g_helper_pos[2];
    row.head_found = g_helper_found;
    if (g_eye_body != nullptr) {
        bridger::guarded([&] {
            const auto* a = reinterpret_cast<const WorldTransform*>(static_cast<std::uint8_t*>(g_eye_body) + kEntityOrientation);
            row.body[0] = a->px; row.body[1] = a->py; row.body[2] = a->pz;
            row.body_heading = heading_of(normalize(Vec3{a->m[1].x, a->m[1].y, 0.0f}));
        });
    }
    row.passthrough = passthrough;
    row.override_valid = g_override_valid;
    row.in_vehicle = g_in_vehicle;
    row.dropout = g_dropout.load();
    row.fight = g_fight_blend.load();
    row.helper_misses = g_helper_misses.load();
    std::lock_guard lock(g_flight_lock);
    g_flight[g_flight_next % kFlightRows] = row;
    ++g_flight_next;
}

void flight_dump() {
    std::vector<FlightRow> rows;
    {
        std::lock_guard lock(g_flight_lock);
        const std::size_t count = std::min(g_flight_next, kFlightRows);
        for (std::size_t i = g_flight_next - count; i < g_flight_next; ++i) rows.push_back(g_flight[i % kFlightRows]);
    }
    const auto path = game_dump_path("flick_record.csv");
    FILE* file = std::fopen(path.c_str(), "w");
    if (file == nullptr) {
        bridger::error("flick recorder: cannot open {}", path);
        return;
    }
    std::fputs("t,sent_x,sent_y,sent_z,sent_heading,sent_pitch,game_x,game_y,game_z,game_heading,game_pitch,"
               "head_x,head_y,head_z,head_found,body_x,body_y,body_z,body_heading,passthrough,override_valid,"
               "in_vehicle,dropout,fight,helper_misses\n", file);
    const double t0 = rows.empty() ? 0.0 : rows.back().t;
    for (const auto& r : rows) {
        std::fprintf(file, "%.4f,%.3f,%.3f,%.3f,%.2f,%.2f,%.3f,%.3f,%.3f,%.2f,%.2f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.2f,%d,%d,%d,%d,%.2f,%u\n",
                     r.t - t0, r.sent[0], r.sent[1], r.sent[2], r.sent_heading, r.sent_pitch, r.game[0], r.game[1],
                     r.game[2], r.game_heading, r.game_pitch, r.head[0], r.head[1], r.head[2], r.head_found ? 1 : 0,
                     r.body[0], r.body[1], r.body[2], r.body_heading, r.passthrough ? 1 : 0, r.override_valid ? 1 : 0,
                     r.in_vehicle ? 1 : 0, r.dropout, r.fight, r.helper_misses);
    }
    std::fclose(file);
    ++g_flight_dumps;
    bridger::info("flick recorder: wrote {} frames to {}", rows.size(), path);
}

void check_flick(const WorldTransform& sent, bool passthrough) {
    flight_record(sent, passthrough);
    const Vec3 view = normalize(sent.m[1]);
    const Vec3 game_view = normalize(g_game.m[1]);
    if (g_prev_sent_valid) {
        const double dx = sent.px - g_prev_sent.px;
        const double dy = sent.py - g_prev_sent.py;
        const double dz = sent.pz - g_prev_sent.pz;
        const auto move = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
        const auto degrees = [](const Vec3& a, const Vec3& b) {
            return std::acos(std::clamp(dot(a, b), -1.0f, 1.0f)) * 180.0f / static_cast<float>(kPi);
        };
        const float turn = degrees(view, normalize(g_prev_sent.m[1]));
        const float game_turn = degrees(game_view, g_prev_game_view);
        if (move > 0.35f || turn > 20.0f) {
            ++g_flicks;
            const unsigned misses = g_helper_misses;
            g_last_flick = {move, turn, game_turn, passthrough, g_dropout.load(),
                            misses - g_prev_helper_misses, g_fight_blend.load()};
            bridger::info("first person flick: moved {:.2f} m, turned {:.0f} deg (game view turned {:.0f} deg), "
                          "passthrough {}, dropout {}, head lookup misses {}, fight {:.2f}",
                          move, turn, game_turn, passthrough, dropout_name(g_dropout.load()),
                          misses - g_prev_helper_misses, g_fight_blend.load());
        }
    }
    g_prev_sent = sent;
    g_prev_sent_valid = true;
    g_prev_game_view = game_view;
    g_prev_helper_misses = g_helper_misses;
}

void set_orientation_detour(void* entity, const WorldTransform* transform) {
    const bool ours = (g_enabled || g_first_person) && g_override_valid && entity == g_camera_entity
                      && g_camera_entity != nullptr;
    if (g_first_person && entity == g_camera_entity && g_camera_entity != nullptr) {
        check_flick(ours ? g_ours : *transform, !ours);
    } else if (!g_first_person) {
        g_prev_sent_valid = false;
    }
    if (ours) {
        transform = &g_ours;
    } else if (g_first_person && entity == g_camera_entity && g_camera_entity != nullptr) {
        ++g_passthroughs;
        note_dropout(Dropout::Passthrough, entity);
    }
    g_set_orientation.call(entity, transform);
    if (ours && g_first_person) {
        if (g_fov > 0.0f) {
            const float blend = g_fight_blend;
            const float s = blend * blend * (3.0f - 2.0f * blend);
            *reinterpret_cast<float*>(static_cast<std::uint8_t*>(entity) + kCameraFov) = g_fov + g_fight_fov * s;
        }
        if (g_near_plane > 0.0f) {
            *reinterpret_cast<float*>(static_cast<std::uint8_t*>(entity) + kCameraNearPlane) = g_near_plane;
        }
    }
}

std::atomic<unsigned> g_eye_fixes = 0;
std::atomic<float> g_eye_fix_max = 0.0f;

constexpr std::size_t kEntityComponents = 0x90;
constexpr int kJointUpperNeck = 30;
constexpr int kHeadJoints[] = {31, 32, 33, 34};

std::atomic<unsigned> g_collapse_frames = 0;
void* g_skinned_owner = nullptr;
void* g_skinned_model = nullptr;

void* find_skinned_model(void* entity) {
    if (entity == g_skinned_owner) return g_skinned_model;
    void* found = nullptr;
    bridger::guarded([&] {
        auto* container = static_cast<std::uint8_t*>(entity) + kEntityComponents;
        const auto count = *reinterpret_cast<std::uint32_t*>(container);
        auto** items = *reinterpret_cast<void***>(container + 8);
        for (std::uint32_t i = 0; items != nullptr && i < count && i < 512; ++i) {
            if (items[i] != nullptr && bridger::rtti_name(bridger::rtti_of(items[i])) == "SkinnedModel") {
                found = items[i];
                break;
            }
        }
    });
    g_skinned_owner = entity;
    g_skinned_model = found;
    return found;
}

void collapse_head_model(std::uint8_t* model) {
    bool done = false;
    bridger::guarded([&] {
        for (const auto offset : kPoseBuffers) {
            auto* begin = *reinterpret_cast<float**>(model + offset);
            auto* end = *reinterpret_cast<float**>(model + offset + 8);
            if (begin == nullptr || end <= begin || (end - begin) / 16 <= kHeadJoints[3]) continue;
            const float* neck = begin + kJointUpperNeck * 16;
            for (const int joint : kHeadJoints) {
                float* m = begin + joint * 16;
                for (int k = 0; k < 12; ++k) m[k] = 0.0f;
                m[12] = neck[12];
                m[13] = neck[13];
                m[14] = neck[14];
                m[15] = 1.0f;
            }
            done = true;
        }
    });
    if (done) ++g_collapse_frames;
}

void collapse_head(void* entity) {
    auto* model = static_cast<std::uint8_t*>(find_skinned_model(entity));
    if (model != nullptr) collapse_head_model(model);
}

constexpr std::uintptr_t kSkinnedPostUpdate = 0x21e2000;
using SkinnedPostUpdateFn = void (*)(void*, void*);
bridger::Hook<SkinnedPostUpdateFn> g_skinned_post_update_hook;
std::atomic<void*> g_collapse_model = nullptr;
std::atomic<unsigned> g_collapse_hook_frames = 0;
std::atomic<bool> g_camera_at_eye = true;

void skinned_post_update_detour(void* self, void* message) {
    if (self != nullptr && self == g_collapse_model.load(std::memory_order_relaxed)) {
        collapse_head_model(static_cast<std::uint8_t*>(self));
        ++g_collapse_hook_frames;
    }
    g_skinned_post_update_hook.call(self, message);
}

std::atomic<bool> g_head_shadow = true;
struct HeadDraw {
    std::uint64_t pipeline;
    std::uint32_t count;
};
std::mutex g_head_draw_lock;
std::vector<HeadDraw> g_head_draws;
std::vector<std::unique_ptr<bridger::fx::DrawHook>> g_head_skip_hooks;
std::atomic<bool> g_head_skip_active = false;
std::atomic<unsigned> g_head_skipped = 0;

constexpr int kCalibPhases = 4;
constexpr int kCalibPhaseFrames = 8;
struct CalibHit {
    std::uint64_t pipeline;
    std::uint32_t count;
    std::uint32_t render_targets;
    unsigned hits[kCalibPhases];
};
std::mutex g_calib_lock;
std::vector<CalibHit> g_calib_hits;
std::vector<std::unique_ptr<bridger::fx::DrawHook>> g_calib_hooks;
std::atomic<int> g_calib_phase = -1;
int g_calib_frame = 0;
bool g_calib_requested = false;
std::vector<std::uint32_t> g_calib_parts;
std::atomic<unsigned> g_calib_runs = 0;

constexpr std::uint32_t kHeadCalibParts[] = {0x5d2964aa, 0x3b68948c, 0x1185219f, 0x14e3a140, 0x5a5b3a7d,
                                             0x6714b91c, 0x3572a37f, 0x6100d683, 0x41e46476, 0x7c796aab,
                                             0x2d5e2938, 0x3fe9d827, 0x71a41fa6, 0x479f767e, 0x21d6e665};

bool head_skip_draw(const BridgerFxDraw* draw, void*) {
    if (!g_head_skip_active.load(std::memory_order_relaxed)) return true;
    std::lock_guard lock(g_head_draw_lock);
    for (const auto& h : g_head_draws) {
        if (h.pipeline == draw->pipeline && h.count == draw->count) {
            g_head_skipped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    return true;
}

bool calib_draw(const BridgerFxDraw* draw, void* user) {
    const int phase = g_calib_phase.load(std::memory_order_relaxed);
    if (phase < 0 || phase >= kCalibPhases) return true;
    std::lock_guard lock(g_calib_lock);
    for (auto& h : g_calib_hits) {
        if (h.pipeline == draw->pipeline && h.count == draw->count) {
            ++h.hits[phase];
            return true;
        }
    }
    CalibHit hit{draw->pipeline, draw->count, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(user)), {}};
    hit.hits[phase] = 1;
    g_calib_hits.push_back(hit);
    return true;
}

std::string head_draws_text() {
    std::string text;
    std::lock_guard lock(g_head_draw_lock);
    for (const auto& h : g_head_draws) {
        text += std::format("{}{:016x}:{}", text.empty() ? "" : " ", h.pipeline, h.count);
    }
    return text;
}

void install_head_skips() {
    g_head_skip_hooks.clear();
    std::vector<std::uint64_t> pipelines;
    {
        std::lock_guard lock(g_head_draw_lock);
        for (const auto& h : g_head_draws) {
            if (std::find(pipelines.begin(), pipelines.end(), h.pipeline) == pipelines.end()) {
                pipelines.push_back(h.pipeline);
            }
        }
    }
    for (const auto pipeline : pipelines) {
        auto hook = std::make_unique<bridger::fx::DrawHook>();
        if (hook->create(pipeline, head_skip_draw)) g_head_skip_hooks.push_back(std::move(hook));
    }
}

void set_head_draws(const std::string& text) {
    std::vector<HeadDraw> draws;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !std::isxdigit(static_cast<unsigned char>(text[i]))) ++i;
        std::size_t j = i;
        while (j < text.size() && std::isxdigit(static_cast<unsigned char>(text[j]))) ++j;
        if (j - i == 16 && j < text.size() && text[j] == ':') {
            std::size_t k = j + 1;
            while (k < text.size() && std::isdigit(static_cast<unsigned char>(text[k]))) ++k;
            if (k > j + 1) {
                draws.push_back({std::stoull(text.substr(i, 16), nullptr, 16),
                                 static_cast<std::uint32_t>(std::stoul(text.substr(j + 1, k - j - 1)))});
            }
            j = k;
        }
        i = j;
    }
    {
        std::lock_guard lock(g_head_draw_lock);
        g_head_draws = std::move(draws);
    }
}

bridger::Setting<std::string> s_head_draws{"lens.head_draws_v2", "Head draws", "",
                                           "Calibrated pipeline:index-count pairs the head draws with."};

void calib_set_parts(void* entity, bool on) {
    for (const auto hash : g_calib_parts) set_part_enabled(entity, hash, on);
}

void calib_abort(void* entity) {
    g_calib_hooks.clear();
    g_calib_phase = -1;
    if (entity != nullptr) calib_set_parts(entity, true);
    g_calib_parts.clear();
}

void calib_finish(void* entity) {
    std::vector<CalibHit> hits;
    {
        std::lock_guard lock(g_calib_lock);
        hits.swap(g_calib_hits);
    }
    calib_abort(entity);
    std::vector<HeadDraw> draws;
    unsigned depth_only = 0;
    for (const auto& h : hits) {
        const bool on = h.hits[1] >= kCalibPhaseFrames / 2 && h.hits[3] >= kCalibPhaseFrames / 2;
        const bool off = h.hits[0] <= 2 && h.hits[2] <= 2;
        if (!on || !off) continue;
        if (h.render_targets < 1) {
            ++depth_only;
            bridger::info("head draws: depth-only pipeline {:016x} count {} left drawing", h.pipeline, h.count);
            continue;
        }
        draws.push_back({h.pipeline, h.count});
    }
    {
        std::lock_guard lock(g_head_draw_lock);
        g_head_draws = draws;
    }
    s_head_draws.set(head_draws_text());
    install_head_skips();
    ++g_calib_runs;
    bridger::info("head draws: {} G-buffer draws over {} pipelines ({} depth-only left), from {} candidates",
                  draws.size(), g_head_skip_hooks.size(), depth_only, hits.size());
}

void update_head_shadow(void* entity) {
    const bool wanted = entity != nullptr && g_head_shadow;
    static bool was_wanted = false;
    if (wanted && !was_wanted) {
        std::lock_guard lock(g_head_draw_lock);
        if (g_head_draws.empty()) g_calib_requested = true;
    }
    was_wanted = wanted;
    if (g_calib_phase >= 0 && !wanted) {
        calib_abort(g_parts_entity != nullptr ? g_parts_entity : entity);
        bridger::info("head draws: calibration abandoned");
    }
    if (wanted && g_calib_requested && g_calib_phase < 0) {
        g_calib_requested = false;
        {
            std::lock_guard lock(g_calib_lock);
            g_calib_hits.clear();
        }
        g_calib_parts.clear();
        std::vector<std::uint32_t> candidates(std::begin(kHeadCalibParts), std::end(kHeadCalibParts));
        {
            std::lock_guard lock(g_head_parts_lock);
            for (const auto hash : g_head_parts) {
                if (std::find(candidates.begin(), candidates.end(), hash) == candidates.end()) candidates.push_back(hash);
            }
        }
        for (const auto hash : candidates) {
            if (part_flags(entity, hash) == 0x0f) g_calib_parts.push_back(hash);
        }
        g_calib_hooks.clear();
        for (const auto& p : bridger::fx::pipelines()) {
            if (p.compute || p.draws_last_frame == 0) continue;
            auto hook = std::make_unique<bridger::fx::DrawHook>();
            if (hook->create(p.hash, calib_draw, nullptr, reinterpret_cast<void*>(static_cast<std::uintptr_t>(p.render_targets)))) {
                g_calib_hooks.push_back(std::move(hook));
            }
        }
        g_calib_frame = 0;
        g_calib_phase = 0;
        bridger::info("head draws: calibrating {} parts over {} pipelines", g_calib_parts.size(), g_calib_hooks.size());
    }
    if (g_calib_phase >= 0) {
        const int phase = g_calib_frame / kCalibPhaseFrames;
        if (phase >= kCalibPhases) {
            calib_finish(entity);
        } else {
            g_calib_phase = phase;
            calib_set_parts(entity, phase % 2 == 1);
            ++g_calib_frame;
        }
    }
    g_head_skip_active = wanted && g_calib_phase < 0;
}

std::atomic<bool> g_bt_near = false;
std::atomic<bool> g_probe_combat = false;

struct PatchedFloat {
    float* at;
    float original;
};
std::vector<PatchedFloat> g_collision_patch;
void* g_collision_mode = nullptr;
std::atomic<float> g_game_cam_distance = 0.0f;
std::atomic<bool> g_no_camera_collision = true;
std::atomic<float> g_hidden_cam_distance = 1.0f;

constexpr std::size_t kCamCompResource = 0x30;
constexpr std::size_t kCamResMode = 0x20;
constexpr std::size_t kModeParameters = 0x200;
constexpr std::size_t kParamBase = 0x20;
constexpr std::size_t kParmVal = 0x10;
constexpr std::size_t kParamDistances[] = {0x108, 0x278, 0x290, 0x4b0};
constexpr std::size_t kParamOffsets[] = {0x120, 0x2e0, 0x300, 0x490};
constexpr std::size_t kModeExtraDistances[] = {564, 576, 588, 600, 608, 628, 640, 648, 668, 672, 680, 700};

void patch_float(float* at, float value) {
    for (const auto& p : g_collision_patch) {
        if (p.at == at) return;
    }
    g_collision_patch.push_back({at, *at});
    *at = value;
}

void camera_collision_off(void* camera_entity) {
    if (!g_collision_patch.empty() || camera_entity == nullptr) return;
    bridger::guarded([&] {
        auto* container = static_cast<std::uint8_t*>(camera_entity) + kEntityComponents;
        const auto count = *reinterpret_cast<std::uint32_t*>(container);
        auto** items = *reinterpret_cast<std::uint8_t***>(container + 8);
        std::uint8_t* component = nullptr;
        for (std::uint32_t i = 0; items != nullptr && i < count && i < 64; ++i) {
            if (items[i] != nullptr
                && bridger::rtti_name(bridger::rtti_of(items[i])) == "DSThirdPersonPlayerCameraComponent") {
                component = items[i];
                break;
            }
        }
        if (component == nullptr) return;
        auto* resource = *reinterpret_cast<std::uint8_t**>(component + kCamCompResource);
        if (resource == nullptr) return;
        auto* mode = *reinterpret_cast<std::uint8_t**>(resource + kCamResMode);
        if (mode == nullptr || bridger::rtti_name(bridger::rtti_of(mode)) != "DSCameraModeResource") return;

        const auto n = *reinterpret_cast<std::uint32_t*>(mode + kModeParameters);
        auto** params = *reinterpret_cast<std::uint8_t***>(mode + kModeParameters + 8);
        for (std::uint32_t i = 0; params != nullptr && i < n && i < 4096; ++i) {
            for (auto* p = params[i]; p != nullptr; p = *reinterpret_cast<std::uint8_t**>(p + kParamBase)) {
                for (const auto off : kParamDistances) patch_float(reinterpret_cast<float*>(p + off + kParmVal), g_hidden_cam_distance.load());
                for (const auto off : kParamOffsets) {
                    for (int a = 0; a < 3; ++a) patch_float(reinterpret_cast<float*>(p + off + kParmVal + a * 4), 0.0f);
                }
                if (g_collision_patch.size() > 200000) break;
            }
        }
        for (const auto off : kModeExtraDistances) patch_float(reinterpret_cast<float*>(mode + off), 0.0f);
        g_collision_mode = mode;
    });
    bridger::info("camera collision: pulled the third-person camera in ({} values)", g_collision_patch.size());
}

void camera_collision_restore() {
    if (g_collision_patch.empty()) return;
    bridger::guarded([&] {
        for (const auto& p : g_collision_patch) *p.at = p.original;
    });
    bridger::info("camera collision: restored {} values", g_collision_patch.size());
    g_collision_patch.clear();
    g_collision_mode = nullptr;
}

void forget_world() {
    g_player = nullptr;
    g_owned_variable_count = 0;
    g_owned_entity_count = 0;
    g_player_game = nullptr;
    g_followed = nullptr;
    g_eye_body = nullptr;
    g_override_valid = false;
    g_view_valid = false;
    g_hidden_entity = nullptr;
    g_parts_entity = nullptr;
    g_parts_hidden.clear();
    g_parts_hidden_count = 0;
    g_children_owner = nullptr;
    g_children_hidden.clear();
    g_child_parts_hidden.clear();
    g_hovered_child = -1;
    g_skinned_owner = nullptr;
    g_skinned_model = nullptr;
    g_collapse_model = nullptr;
    g_hand_offset = {0.0f, 0.0f, 0.0f};
    g_calib_hooks.clear();
    g_calib_phase = -1;
    g_calib_parts.clear();
    g_head_skip_active = false;
    g_fight_pos_valid = false;
    g_fight_blend = 0.0f;
    g_fight_timer = 0.0f;
    g_limb_prev_valid[0] = g_limb_prev_valid[1] = false;
    std::lock_guard lock(g_head_parts_lock);
    g_children.clear();
    g_part_rows.clear();
    g_child_part_rows.clear();
}

void late_eye_tick(float dt) {
    static unsigned last_updates = 0;
    static int stale_ticks = 0;
    const unsigned updates = g_camera_updates;
    stale_ticks = updates == last_updates ? stale_ticks + 1 : 0;
    last_updates = updates;
    if (stale_ticks >= 2) {
        if (stale_ticks == 2) {
            forget_world();
            bridger::info("camera stopped updating; dropped cached entities");
        }
        return;
    }

    update_owned_variables();
    update_skip_pipelines();
    update_count_probe();
    sync_gear_dither(g_first_person && g_solid_gear);
    update_watchpoint();
    if (g_first_person && g_no_camera_collision && g_camera_entity != nullptr) {
        camera_collision_off(g_camera_entity);
    } else {
        camera_collision_restore();
    }
    if (g_helper_found) {
        const double dx = g_game.px - g_helper_pos[0];
        const double dy = g_game.py - g_helper_pos[1];
        const double dz = g_game.pz - g_helper_pos[2];
        g_game_cam_distance = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
    }

    bool camera_at_eye = true;
    if (g_first_person && g_override_valid) {
        const auto cam = bridger::fx::camera();
        if (cam.valid) {
            const double dx = cam.position[0] - g_ours.px;
            const double dy = cam.position[1] - g_ours.py;
            const double dz = cam.position[2] - g_ours.pz;
            camera_at_eye = dx * dx + dy * dy + dz * dz < 0.5 * 0.5;
        }
    }
    g_camera_at_eye = camera_at_eye;
    void* head_target = nullptr;
    if (g_first_person && g_hide_head && g_override_valid && camera_at_eye && g_player != nullptr
        && bridger::rtti_name(bridger::rtti_of(g_player)) == "DSPlayerEntity") {
        head_target = g_player;
    }
    const bool shadow_mode = g_head_shadow;
    sync_head_parts(shadow_mode ? nullptr : head_target);
    if (head_target != nullptr && g_collapse_head && !shadow_mode) {
        collapse_head(head_target);
        g_collapse_model = find_skinned_model(head_target);
    } else {
        g_collapse_model = nullptr;
    }
    update_head_shadow(head_target);
    update_fight_eye(g_first_person && g_override_valid && !g_in_vehicle ? g_player : nullptr, dt);

    if (g_player != g_children_owner) {
        g_children_owner = g_player;
        scan_children();
    }
    update_hover();
    static bool delete_was_down = false;
    const bool delete_down = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
    if (delete_down && !delete_was_down && g_hovered_child >= 0) {
        toggle_hovered_child();
    }
    delete_was_down = delete_down;
    sync_children(head_target != nullptr);

    if (!g_first_person || !g_override_valid || g_camera_entity == nullptr || g_eye_body == nullptr) {
        return;
    }
    const double before[3] = {g_ours.px, g_ours.py, g_ours.pz};
    if (!bridger::guarded([] { place_eye(g_eye_body); })) {
        forget_world();
        return;
    }
    const double dx = g_ours.px - before[0];
    const double dy = g_ours.py - before[1];
    const double dz = g_ours.pz - before[2];
    const auto moved = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
    if (moved < 0.001f) {
        return;
    }
    if (moved > 0.02f) {
        ++g_eye_fixes;
        if (moved > g_eye_fix_max) {
            g_eye_fix_max = moved;
            bridger::info("late eye placement moved the camera {:.3f} m", moved);
        }
    }
    g_set_orientation.call(g_camera_entity, &g_ours);
}

bridger::Setting<int> s_key_free{"key.free_camera", "Free camera", VK_F3};
bridger::Setting<int> s_key_first{"key.first_person", "First person", VK_F6};
bridger::Setting<int> s_key_record{"key.record", "Record", VK_F7,
                                   "Writes a CSV of camera and player state."};
bridger::Setting<int> s_key_marker{"key.marker", "Drop marker", VK_F8,
                                   "Stamps a numbered row into the recording."};

bridger::Setting<float> s_speed{"speed", "Speed", 8.0f,
                                "Shift multiplies by four, control divides by four."};
bridger::Setting<bool> s_use_wasd{"use_wasd", "Accept WASD and QE", false,
                                  "Convenient, but the same keys still walk Sam around."};

bridger::Setting<float> s_eye_height{"eye.height", "Eye height", 1.72f,
                                     "Metres above the player entity's origin."};
bridger::Setting<float> s_eye_forward{"eye.face_forward", "Forward of head bone", 0.09f,
                                      "Metres along the face, following the head's rotation."};
bridger::Setting<float> s_eye_up{"eye.face_up", "Above head bone", 0.06f,
                                 "Metres along the skull's up axis."};
bridger::Setting<float> s_eye_side{"eye.side", "To the right", 0.0f,
                                   "metres to Sam's right; negative puts it over the left shoulder of the head bone."};
bridger::Setting<std::string> s_helper{"eye.helper", "Head helper", "HeadHelper",
                                       "Skeleton helper the eye is anchored to."};

bridger::Setting<bool> s_turn_body{"turn.enabled", "Turn body to view", true,
                                   "Injects stick input so Sam faces where you are looking."};
bridger::Setting<float> s_turn_threshold{"turn.threshold", "Turn when off by", 20.0f,
                                         "Degrees of disagreement before injecting."};
bridger::Setting<float> s_turn_done{"turn.done", "Stop when within", 6.0f,
                                    "Degrees at which injection stops."};
bridger::Setting<float> s_turn_stick{"turn.stick", "Stick magnitude", 0.4f};
bridger::Setting<bool> s_turn_hold{"turn.hold", "Hold until aligned", false,
                                   "Holds the stick instead of tapping it. Makes Sam walk."};
bridger::Setting<int> s_tap_frames{"turn.tap_frames", "Tap length", 5};
bridger::Setting<int> s_tap_cooldown{"turn.tap_cooldown", "Wait after tap", 25,
                                     "Frames to let the pivot play out before tapping again."};
bridger::Setting<float> s_body_offset{"turn.body_offset", "Body heading offset", 0.0f};

bridger::Setting<float> s_fov{"lens.fov", "Field of view", 100.0f, "Zero keeps the game's FOV."};
bridger::Setting<float> s_near_plane{"lens.near", "Near plane", 0.05f,
                                     "Zero keeps the game's 0.2, which clips the chest."};
bridger::Setting<bool> s_hide_body{"lens.hide_body", "Hide body", false};
bridger::Setting<bool> s_hide_head{"lens.hide_head", "Hide head", true,
                                    "Turns off Sam's head, face and visor model parts while in "
                                    "first person, so the camera never shows their insides."};
bridger::Setting<bool> s_steer_angles{"lens.steer_angles_v3", "Ignore wall push-in", true,
                                       "Takes the view from where you steer instead of from the "
                                       "third-person camera, which swings when walls push it in."};
bridger::Setting<bool> s_no_collision{"lens.no_camera_collision", "No camera collision", true,
                                       "Pulls the hidden third-person camera in to Sam while in first person, so walls never push it and snap the view."};
bridger::Setting<float> s_hidden_cam_distance{"lens.hidden_camera_distance", "Hidden camera distance", 1.0f,
                                              "How far behind Sam the game's own camera is held in first person. Too close hides your gear; too far lets walls push it and snap the view."};
bridger::Setting<bool> s_solid_gear{"lens.solid_gear", "Keep gear solid", true,
                                     "Turns off the near-camera dither on Sam's gun and cargo materials while in first person."};
bridger::Setting<bool> s_fight_eye{"fight.enabled", "Fight camera", true,
                                    "In melee, detaches the eye from the head so strikes are "
                                    "framed instead of seen from inside the swing."};
bridger::Setting<float> s_fight_speed{"fight.speed", "Strike speed", 4.5f,
                                      "Hand or foot speed relative to the body that counts as a "
                                      "strike. Lower triggers more easily."};
bridger::Setting<float> s_fight_hold{"fight.hold", "Hold after strike", 0.8f};
bridger::Setting<float> s_fight_back{"fight.back_v4", "Shift shot back", 0.0f};
bridger::Setting<float> s_fight_up{"fight.up_v3", "Raise", 0.1f};
bridger::Setting<float> s_fight_side{"fight.side_v4", "Side distance (negative is left)", -1.4f};
bridger::Setting<float> s_fight_reach{"fight.reach", "Opponent distance", 1.2f,
                                      "Metres in front of Sam the shot centres between him and his target."};
bridger::Setting<float> s_fight_fov{"fight.fov", "Extra field of view", 12.0f};
bridger::Setting<bool> s_collapse_head{"lens.collapse_head", "Fold head into neck", true,
                                       "Instead of switching the head off, folds it into the top "
                                       "of the neck so the neck closes the collar."};
bridger::Setting<bool> s_head_shadow{"lens.head_shadow", "Keep head shadow", true,
                                     "Skips the head's own draws instead of folding it or switching "
                                     "it off, so its shadow stays. Blinks the head for half a second "
                                     "once to learn which draws are the head."};
bridger::Setting<std::string> s_head_parts{"lens.head_parts_v5", "Head parts",
                                           "5d2964aa 3b68948c 1185219f 14e3a140 5a5b3a7d 6714b91c 3572a37f "
                                           "6100d683 41e46476 7c796aab 2d5e2938 3fe9d827 71a41fa6 479f767e",
                                           "Hex part hashes to hide, space separated."};
bridger::Setting<float> s_eye_down_forward{"eye.down_forward_v3", "Forward when looking down", 0.18f,
                                           "Metres the eye leans out over the chest at a steep "
                                           "look down, so the open collar is never below it."};
bridger::Setting<float> s_eye_down_drop{"eye.down_drop_v3", "Drop when looking down", 0.04f};
bridger::Setting<std::string> s_hidden_children{"lens.hidden_attachments_v2", "Hidden attachments", "",
                                                "Resource ids of attached objects hidden in first person."};

bridger::Setting<std::string> s_hidden_child_parts{"lens.hidden_attachment_parts", "Hidden attachment parts", "",
                                                   "Parts of attached objects hidden in first person."};

void save_hidden_children() {
    std::string text;
    std::string parts;
    {
        std::lock_guard lock(g_head_parts_lock);
        for (const auto& key : g_hidden_child_keys) text += (text.empty() ? "" : " ") + key;
        for (const auto& key : g_hidden_child_part_keys) parts += (parts.empty() ? "" : " ") + key;
    }
    s_hidden_children.set(text);
    s_hidden_child_parts.set(parts);
}

std::vector<std::string> split_keys(const std::string& text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto j = std::min(text.find(' ', i), text.size());
        if (j > i) out.push_back(text.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

void toggle_hovered_child() {
    const int hovered = g_hovered_child;
    {
        std::lock_guard lock(g_head_parts_lock);
        if (hovered < 0 || hovered >= static_cast<int>(g_children.size())) return;
        const auto& key = g_children[hovered].key;
        auto it = std::find(g_hidden_child_keys.begin(), g_hidden_child_keys.end(), key);
        if (it == g_hidden_child_keys.end()) {
            g_hidden_child_keys.push_back(key);
            bridger::info("attachment hidden in first person: {} ({})", g_children[hovered].label, key);
        } else {
            g_hidden_child_keys.erase(it);
            bridger::info("attachment shown again: {} ({})", g_children[hovered].label, key);
        }
    }
    save_hidden_children();
}

bridger::Setting<float> s_eye_clearance{"eye.clearance", "Clearance ahead of head", 0.12f,
                                        "Minimum metres the eye stays ahead of the head bone "
                                        "along the view. Raise if you see inside the head."};
bridger::Setting<float> s_eye_shoulder{"eye.shoulder", "Over-shoulder offset", 0.28f,
                                       "Metres the eye moves out over the shoulder you look back "
                                       "across, so it clears the neck and torso."};
bridger::Setting<float> s_eye_shoulder_lift{"eye.shoulder_lift", "Over-shoulder lift", 0.06f};
bridger::Setting<bool> s_keep_vehicles{"lens.keep_vehicles", "Keep vehicles visible", true,
                                       "Stops trucks and bikes fading out when the camera is "
                                       "inside or next to them."};
bridger::Setting<bool> s_hide_hud_bt{"lens.hide_hud_near_bt", "Hide HUD near BTs", false,
                                     "Hides the game UI while Sam's DOOMS sense is reacting "
                                     "and you are in first person."};
bridger::Setting<int> s_hud_reveal_key{"lens.hud_reveal_key", "Reveal HUD while held", '1',
                                       "Held down, this shows the HUD again so the weapon "
                                       "wheel is usable. Backspace unbinds it."};

void apply_saved() {
    g_speed = s_speed.get();
    g_use_wasd = s_use_wasd.get();
    g_eye_height = s_eye_height.get();
    g_eye_forward = s_eye_forward.get();
    g_eye_up = s_eye_up.get();
    g_eye_side = s_eye_side.get();
    g_turn_body = s_turn_body.get();
    g_turn_threshold = s_turn_threshold.get();
    g_turn_done = s_turn_done.get();
    g_turn_stick = s_turn_stick.get();
    g_turn_hold = s_turn_hold.get();
    g_tap_frames = s_tap_frames.get();
    g_tap_cooldown = s_tap_cooldown.get();
    g_body_offset = s_body_offset.get();
    g_fov = s_fov.get();
    g_near_plane = s_near_plane.get();
    g_hide_body = s_hide_body.get();
    g_keep_vehicles = s_keep_vehicles.get();
    g_eye_clearance = s_eye_clearance.get();
    g_eye_down_forward = s_eye_down_forward.get();
    g_eye_down_drop = s_eye_down_drop.get();
    g_hide_head = s_hide_head.get();
    g_collapse_head = s_collapse_head.get();
    g_head_shadow = s_head_shadow.get();
    set_head_draws(s_head_draws.get());
    install_head_skips();
    g_fight_eye = s_fight_eye.get();
    g_solid_gear = s_solid_gear.get();
    g_no_camera_collision = s_no_collision.get();
    g_hidden_cam_distance = s_hidden_cam_distance.get();
    g_use_steer_angles = s_steer_angles.get();
    g_fight_speed = s_fight_speed.get();
    g_fight_hold = s_fight_hold.get();
    g_fight_back = s_fight_back.get();
    g_fight_up = s_fight_up.get();
    g_fight_side = s_fight_side.get();
    g_fight_fov = s_fight_fov.get();
    g_fight_reach = s_fight_reach.get();
    {
        std::lock_guard lock(g_head_parts_lock);
        g_hidden_child_keys = split_keys(s_hidden_children.get());
        g_hidden_child_part_keys = split_keys(s_hidden_child_parts.get());
    }
    {
        std::lock_guard lock(g_head_parts_lock);
        g_head_parts = parse_hashes(s_head_parts.get());
    }
    g_eye_shoulder = s_eye_shoulder.get();
    g_eye_shoulder_lift = s_eye_shoulder_lift.get();

    const std::string& helper = s_helper.get();
    const auto length = std::min(helper.size(), sizeof g_helper_edit - 1);
    std::memcpy(g_helper_edit, helper.data(), length);
    g_helper_edit[length] = 0;
}

constexpr std::uintptr_t kGameStateSingleton = 0x754d7b8;
constexpr std::uintptr_t kBtSenseByte = 0x40f;
constexpr std::uintptr_t kIsCombatMuleOrBT = 0x308c190;
constexpr std::uintptr_t kPlayerProfileSingleton = 0x75426a8;
constexpr std::uintptr_t kPlayerParamsOffset = 368;

enum class DrawHudMode : std::uint8_t { On = 0, Partially = 1, Off = 2 };

std::atomic<std::uint8_t> g_probe_40d = 0;
std::atomic<std::uint8_t> g_probe_40e = 0;
std::atomic<std::uint8_t> g_probe_40f = 0;

std::atomic<std::uintptr_t> g_profile = 0;
std::atomic<bool> g_profile_verified = false;
char g_profile_type[64] = "";
std::atomic<std::uint8_t> g_draw_hud = 0;
std::atomic<bool> g_hud_hidden = false;
std::atomic<std::uint8_t> g_hud_restore = 0;
std::atomic<bool> g_hud_forced = false;
std::atomic<float> g_reveal_linger = 0.0f;

using IsCombatMuleOrBtFn = bool (*)();

std::uint8_t* draw_hud_slot() {
    auto** slot = bridger::resolve<std::uint8_t*>(kPlayerProfileSingleton);
    if (slot == nullptr) {
        return nullptr;
    }
    std::uint8_t* profile = *slot;
    g_profile = reinterpret_cast<std::uintptr_t>(profile);
    if (profile == nullptr) {
        g_profile_verified = false;
        return nullptr;
    }

    if (!g_profile_verified) {
        const void* rtti = bridger::rtti_of(profile);
        const std::string_view name = bridger::rtti_name(rtti);
        if (name.empty()) {
            return nullptr;
        }
        const auto length = std::min(name.size(), sizeof g_profile_type - 1);
        std::memcpy(g_profile_type, name.data(), length);
        g_profile_type[length] = 0;
        if (name != "PlayerProfile") {
            return nullptr;
        }
        g_profile_verified = true;
    }

    const auto count = *reinterpret_cast<std::uint32_t*>(profile + kPlayerParamsOffset);
    auto* data = *reinterpret_cast<std::uint8_t**>(profile + kPlayerParamsOffset + 8);
    if (count == 0 || data == nullptr) {
        return nullptr;
    }
    return data;
}

void set_draw_hud(DrawHudMode mode) {
    bridger::guarded([&] {
        if (std::uint8_t* slot = draw_hud_slot(); slot != nullptr) {
            *slot = static_cast<std::uint8_t>(mode);
        }
    });
}

void restore_hud() {
    if (!g_hud_hidden) {
        return;
    }
    set_draw_hud(static_cast<DrawHudMode>(g_hud_restore.load()));
    g_hud_hidden = false;
}

constexpr float kRevealLinger = 0.35f;

void probe_bt_state(float dt) {
    bridger::guarded([&] {
        auto** slot = bridger::resolve<std::uint8_t*>(kGameStateSingleton);
        if (slot == nullptr || *slot == nullptr) {
            return;
        }
        std::uint8_t* base = *slot;
        g_probe_40d = base[0x40d];
        g_probe_40e = base[0x40e];
        g_probe_40f = base[kBtSenseByte];
        g_bt_near = base[kBtSenseByte] != 0;
    });

    if (const auto fn = bridger::at_rva<IsCombatMuleOrBtFn>(kIsCombatMuleOrBT); fn != nullptr) {
        bridger::guarded([&] { g_probe_combat = fn(); });
    }

    bridger::guarded([&] {
        std::uint8_t* hud = draw_hud_slot();
        if (hud == nullptr) {
            return;
        }
        g_draw_hud = *hud;

        const auto reveal_key = static_cast<unsigned>(s_hud_reveal_key.get());
        if (reveal_key != 0 && bridger::key_down(reveal_key)) {
            g_reveal_linger = kRevealLinger;
        } else if (g_reveal_linger > 0.0f) {
            g_reveal_linger = std::max(0.0f, g_reveal_linger.load() - dt);
        }
        const bool revealing = g_reveal_linger > 0.0f;

        const bool want_hidden = !revealing
                              && (g_hud_forced
                                  || (s_hide_hud_bt.get() && g_first_person && g_bt_near));
        if (want_hidden && !g_hud_hidden) {
            g_hud_restore = *hud;
            *hud = static_cast<std::uint8_t>(DrawHudMode::Off);
            g_hud_hidden = true;
        } else if (!want_hidden && g_hud_hidden) {
            *hud = g_hud_restore.load();
            g_hud_hidden = false;
        } else if (g_hud_hidden && *hud != static_cast<std::uint8_t>(DrawHudMode::Off)) {
            *hud = static_cast<std::uint8_t>(DrawHudMode::Off);
        }
    });
}

const char* draw_hud_name(std::uint8_t mode) {
    switch (mode) {
        case 0: return "On";
        case 1: return "Partially";
        case 2: return "Off";
        default: return "?";
    }
}

void draw() {
    namespace ui = bridger::ui;

    bool enabled = g_enabled;
    if (ui::setting("Free camera", enabled, "Detaches the camera from the player.")) {
        set_enabled(enabled);
    }
    bool first_person = g_first_person;
    if (ui::setting("First person", first_person, "Puts the camera at Sam's head.")) {
        set_first_person(first_person);
    }

    if (g_camera_entity == nullptr) {
        ui::note("Waiting for the player camera to update. Load into the world.");
    }

    if (ui::begin_group("Movement")) {
        if (ui::row(s_speed, 0.25f, 100.0f, "m/s")) {
            g_speed = s_speed.get();
        }
        if (ui::row(s_use_wasd)) {
            g_use_wasd = s_use_wasd.get();
        }
        if (ui::button("Snap to game camera", 190.0f)) {
            g_snap_pending = true;
        }
        ui::same_line();
        if (ui::ghost_button("Halve", 80.0f)) {
            s_speed.set(std::max(0.25f, s_speed.get() * 0.5f));
            g_speed = s_speed.get();
        }
        ui::same_line();
        if (ui::ghost_button("Double", 80.0f)) {
            s_speed.set(std::min(100.0f, s_speed.get() * 2.0f));
            g_speed = s_speed.get();
        }
    }
    ui::end_group();

    if (ui::begin_group("Keys")) {
        if (ui::key_row(s_key_free)) {
            bridger::rebind(toggle, static_cast<unsigned>(s_key_free.get()));
        }
        if (ui::key_row(s_key_first)) {
            bridger::rebind(toggle_first_person, static_cast<unsigned>(s_key_first.get()));
        }
        if (ui::key_row(s_key_record)) {
            bridger::rebind(toggle_recording, static_cast<unsigned>(s_key_record.get()));
        }
        if (ui::key_row(s_key_marker)) {
            bridger::rebind(drop_marker, static_cast<unsigned>(s_key_marker.get()));
        }
        ui::note("Movement keys are fixed: numpad 8/5 forward and back, 4/6 strafe, 7/9 down "
                 "and up, arrows to look, numpad +/- or page up/down for speed.");
    }
    ui::end_group();

    if (ui::begin_group("Eye position", false)) {
        if (ui::row(s_eye_height, 1.0f, 2.2f, "m")) { g_eye_height = s_eye_height.get(); }
        if (ui::row(s_eye_forward, -0.1f, 0.4f, "m")) { g_eye_forward = s_eye_forward.get(); }
        if (ui::row(s_eye_up, -0.2f, 0.2f, "m")) { g_eye_up = s_eye_up.get(); }
        if (ui::row(s_eye_side, -0.2f, 0.2f, "m")) { g_eye_side = s_eye_side.get(); }
        if (ui::text_row(s_helper, "helper name")) {
            const std::string& helper = s_helper.get();
            const auto length = std::min(helper.size(), sizeof g_helper_edit - 1);
            std::memcpy(g_helper_edit, helper.data(), length);
            g_helper_edit[length] = 0;
        }
        if (ui::button("Probe helpers", 150.0f)) {
            bridger::run_on_game_thread(probe_helpers);
        }
        if (g_helper_found) {
            ui::readoutf("resolved", "{:.2f}  {:.2f}  {:.2f}", g_helper_pos[0], g_helper_pos[1],
                         g_helper_pos[2]);
            ui::readoutf("following", "{}", g_followed_type[0] != 0 ? g_followed_type : "-");
            if (g_hidden_entity != nullptr) {
                ui::readoutf("hiding", "{}", g_hidden_type[0] != 0 ? g_hidden_type : "?");
            } else if (g_hide_blocked) {
                ui::readoutf("hiding", ui::warn(), "nothing: body is a {}",
                             g_hide_candidate[0] != 0 ? g_hide_candidate : "?");
            } else {
                ui::readout("hiding", "nothing");
            }
        } else {
            ui::readout("resolved", ui::warn(), "unknown, using origin plus eye height");
        }
    }
    ui::end_group();

    if (ui::begin_group("Lens", false)) {
        if (ui::row(s_fov, 0.0f, 120.0f, "deg")) {
            g_fov = s_fov.get() < 30.0f ? 0.0f : s_fov.get();
        }
        if (ui::row(s_near_plane, 0.0f, 1.0f, "m")) {
            g_near_plane = s_near_plane.get() < 0.02f ? 0.0f : s_near_plane.get();
        }
        if (ui::row(s_hide_body)) {
            g_hide_body = s_hide_body.get();
        }
        if (ui::row(s_hide_head)) {
            g_hide_head = s_hide_head.get();
        }
        if (ui::row(s_collapse_head)) {
            g_collapse_head = s_collapse_head.get();
        }
        if (ui::row(s_head_shadow)) {
            g_head_shadow = s_head_shadow.get();
        }
        {
            std::size_t draws;
            {
                std::lock_guard lock(g_head_draw_lock);
                draws = g_head_draws.size();
            }
            const int phase = g_calib_phase.load();
            ui::readoutf("head draws", draws > 0 ? ui::good() : ui::faint(), "{} known, {} skipped{}", draws,
                         g_head_skipped.load(), phase >= 0 ? std::format(", calibrating phase {}", phase) : "");
        }
        if (ui::button("Relearn head draws", 180.0f)) {
            bridger::run_on_game_thread([] { g_calib_requested = true; });
        }


        if (ui::row(s_steer_angles)) g_use_steer_angles = s_steer_angles.get();

        if (ui::row(s_no_collision)) g_no_camera_collision = s_no_collision.get();
        if (ui::row(s_solid_gear)) g_solid_gear = s_solid_gear.get();
        if (ui::row(s_hidden_cam_distance, 0.05f, 3.0f, "m")) {
            g_hidden_cam_distance = s_hidden_cam_distance.get();
            bridger::run_on_game_thread([] { camera_collision_restore(); });
        }
        ui::readoutf("game camera to head", "{:.2f} m", g_game_cam_distance.load());

        ui::header("Fight camera");
        if (ui::row(s_fight_eye)) g_fight_eye = s_fight_eye.get();
        if (ui::row(s_fight_hold, 0.1f, 3.0f, "s")) g_fight_hold = s_fight_hold.get();
        if (ui::row(s_fight_back, -0.3f, 1.5f, "m")) g_fight_back = s_fight_back.get();
        if (ui::row(s_fight_up, 0.0f, 0.8f, "m")) g_fight_up = s_fight_up.get();
        if (ui::row(s_fight_side, -3.0f, 3.0f, "m")) g_fight_side = s_fight_side.get();
        if (ui::row(s_fight_reach, 0.0f, 3.0f, "m")) g_fight_reach = s_fight_reach.get();
        if (ui::row(s_fight_fov, 0.0f, 30.0f, "deg")) g_fight_fov = s_fight_fov.get();
        ui::readoutf("attack event", g_fight_attack ? ui::good() : ui::faint(), "{}",
                     g_fight_attack ? "live" : "none");
        ui::readoutf("action state", g_fight_state == 14 ? ui::good() : ui::faint(), "{}{}",
                     g_fight_state.load(), g_fight_state == 14 ? " (FastAttack)" : "");
        const auto flags = g_fight_flags.load();
        ui::readoutf("combat flags", flags != 0 ? ui::good() : ui::faint(), "combo attacking {} / combo {} / attack mode {}",
                     (flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0);
        ui::readoutf("fight blend", g_fight_blend > 0.0f ? ui::good() : ui::faint(), "{:.2f}",
                     g_fight_blend.load());
        ui::readoutf("head folded", g_collapse_hook_frames > 0 ? ui::good() : ui::faint(),
                     "{} frames ({} in the model update)", g_collapse_frames.load(),
                     g_collapse_hook_frames.load());
        ui::readoutf("camera at eye", g_camera_at_eye ? ui::good() : ui::warn(), "{}",
                     g_camera_at_eye ? "yes" : "no (game camera elsewhere, head shown)");
        if (ui::text_row(s_head_parts, "hex hashes")) {
            {
                std::lock_guard lock(g_head_parts_lock);
                g_head_parts = parse_hashes(s_head_parts.get());
            }
            bridger::run_on_game_thread(reapply_head_parts);
        }
        ui::readoutf("head parts", g_parts_hidden_count > 0 ? ui::good() : ui::faint(),
                     "{} hidden, {} not on this model", g_parts_hidden_count.load(),
                     g_parts_unknown_count.load());
        if (ui::button("List Sam's parts", 180.0f)) {
            bridger::run_on_game_thread(scan_parts);
        }
        std::vector<PartRow> rows;
        std::vector<std::uint32_t> listed;
        {
            std::lock_guard lock(g_head_parts_lock);
            rows = g_part_rows;
            listed = g_head_parts;
        }
        if (g_part_scan_done && rows.empty()) {
            ui::note("No parts found. Be in first person on foot or on a vehicle, then list again.");
        }
        for (const auto& row : rows) {
            bool hidden = std::find(listed.begin(), listed.end(), row.hash) != listed.end();
            if (ui::setting(std::format("#{} {:08x}", row.index, row.hash).c_str(), hidden,
                            "Tick to hide this part in first person.")) {
                if (hidden) {
                    listed.push_back(row.hash);
                } else {
                    listed.erase(std::remove(listed.begin(), listed.end(), row.hash), listed.end());
                }
                std::string text;
                for (const auto hash : listed) {
                    text += std::format("{}{:08x}", text.empty() ? "" : " ", hash);
                }
                s_head_parts.set(text);
                {
                    std::lock_guard lock(g_head_parts_lock);
                    g_head_parts = listed;
                }
                bridger::run_on_game_thread(reapply_head_parts);
            }
        }
        if (!rows.empty()) {
            ui::note("Ticking a part hides it at once. Untick anything that should stay, like "
                     "arms or the trike.");
        }

        ui::header("Attached objects");
        ui::note("With this overlay open, point the mouse at the thing on Sam you want gone. The "
                 "nearest attached object blinks; press Delete to hide it in first person "
                 "(Delete again brings it back).");
        std::vector<ChildRow> children;
        std::vector<std::string> child_keys;
        {
            std::lock_guard lock(g_head_parts_lock);
            children = g_children;
            child_keys = g_hidden_child_keys;
        }
        const int hovered = g_hovered_child;
        ui::readoutf("under mouse", hovered >= 0 ? ui::good() : ui::faint(), "{}",
                     hovered >= 0 && hovered < static_cast<int>(children.size())
                         ? std::format("#{} {}", hovered, children[hovered].label)
                         : std::string("nothing"));
        if (ui::button("Rescan attachments", 180.0f)) {
            bridger::run_on_game_thread(scan_children);
        }
        for (int i = 0; i < static_cast<int>(children.size()); ++i) {
            const auto& child = children[i];
            bool hidden = std::find(child_keys.begin(), child_keys.end(), child.key) != child_keys.end();
            if (ui::setting(std::format("#{} {}", i, child.label).c_str(), hidden,
                            "Tick to hide this attached object in first person.")) {
                {
                    std::lock_guard lock(g_head_parts_lock);
                    auto& keys = g_hidden_child_keys;
                    keys.erase(std::remove(keys.begin(), keys.end(), child.key), keys.end());
                    if (hidden) keys.push_back(child.key);
                }
                save_hidden_children();
            }
        }

        ui::header("Parts of attached objects");
        ui::note("For things that do not blink, like the glasses: list every drawn part of every "
                 "attached object, then tick through them in first person.");
        if (ui::button("List attachment parts", 200.0f)) {
            bridger::run_on_game_thread([] {
                scan_children();
                scan_child_parts();
            });
        }
        std::vector<ChildPartRow> part_rows;
        std::vector<std::string> part_keys;
        {
            std::lock_guard lock(g_head_parts_lock);
            part_rows = g_child_part_rows;
            part_keys = g_hidden_child_part_keys;
        }
        if (g_child_part_scan_done && part_rows.empty()) {
            ui::note("No parts found on attached objects.");
        }
        for (const auto& row : part_rows) {
            bool hidden = std::find(part_keys.begin(), part_keys.end(), row.key) != part_keys.end();
            const std::string owner = row.child < static_cast<int>(children.size())
                                          ? children[row.child].label
                                          : std::string("?");
            if (ui::setting(std::format("#{}.{} {} {:08x}", row.child, row.index, owner, row.hash).c_str(),
                            hidden, "Tick to hide this part in first person.")) {
                {
                    std::lock_guard lock(g_head_parts_lock);
                    auto& keys = g_hidden_child_part_keys;
                    keys.erase(std::remove(keys.begin(), keys.end(), row.key), keys.end());
                    if (hidden) keys.push_back(row.key);
                }
                save_hidden_children();
            }
        }
        if (ui::row(s_eye_clearance, 0.0f, 0.3f, "m")) {
            g_eye_clearance = s_eye_clearance.get();
        }
        if (ui::row(s_eye_down_forward, 0.0f, 0.5f, "m")) {
            g_eye_down_forward = s_eye_down_forward.get();
        }
        if (ui::row(s_eye_down_drop, 0.0f, 0.2f, "m")) {
            g_eye_down_drop = s_eye_down_drop.get();
        }
        ui::readoutf("head axes", g_face_trusted ? ui::good() : ui::faint(), "{}",
                     g_face_trusted ? "following head bone" : "body frame (learning)");
        if (ui::row(s_eye_shoulder, 0.0f, 0.6f, "m")) {
            g_eye_shoulder = s_eye_shoulder.get();
        }
        if (ui::row(s_eye_shoulder_lift, 0.0f, 0.3f, "m")) {
            g_eye_shoulder_lift = s_eye_shoulder_lift.get();
        }
        if (ui::row(s_keep_vehicles)) {
            g_keep_vehicles = s_keep_vehicles.get();
        }

        ui::readoutf("game fov / near", "{:.1f} / {:.3f}", g_game_fov, g_game_near);
    }
    ui::end_group();

    if (ui::begin_group("Body turning", false)) {
        if (ui::row(s_turn_body)) { g_turn_body = s_turn_body.get(); }
        if (ui::row(s_turn_threshold, 5.0f, 90.0f, "deg")) {
            g_turn_threshold = s_turn_threshold.get();
        }
        if (ui::row(s_turn_done, 1.0f, 30.0f, "deg")) { g_turn_done = s_turn_done.get(); }
        if (ui::row(s_turn_stick, 0.1f, 1.0f)) { g_turn_stick = s_turn_stick.get(); }
        if (ui::row(s_turn_hold)) { g_turn_hold = s_turn_hold.get(); }
        if (ui::row(s_tap_frames, 1, 30, "frames")) { g_tap_frames = s_tap_frames.get(); }
        if (ui::row(s_tap_cooldown, 0, 90, "frames")) { g_tap_cooldown = s_tap_cooldown.get(); }
        if (ui::row(s_body_offset, -180.0f, 180.0f, "deg")) {
            g_body_offset = s_body_offset.get();
        }
        ui::readoutf("view / body heading", "{:.0f} / {:.0f} deg{}", g_view_heading,
                     g_body_heading, g_in_vehicle ? "  (vehicle)" : "");
        ui::readoutf("off by / state / injecting", "{:.0f} deg / {} / {}", g_turn_delta,
                     g_last_state, g_injecting ? "yes" : "no");
        ui::readoutf("inject bursts / frames", "{} / {}", g_inject_bursts, g_inject_frames);
    }
    ui::end_group();

    if (ui::begin_group("BTs and the HUD")) {
        if (ui::row(s_hide_hud_bt)) {
            if (!s_hide_hud_bt.get()) {
                bridger::run_on_game_thread(restore_hud);
            }
        }
        ui::note("Needs first person. The game's own HUD setting is put back the moment either "
                 "condition stops holding.");
        ui::key_row(s_hud_reveal_key);

        ui::readoutf("DOOMS sense", g_bt_near ? ui::good() : ui::faint(), "{}",
                     g_bt_near ? "BTs near" : "clear");
        ui::readoutf("game HUD", "{}{}", draw_hud_name(g_draw_hud.load()),
                     g_hud_hidden ? "  (hidden by this mod)" : "");
        if (g_reveal_linger > 0.0f) {
            ui::readout("reveal", ui::good(), "key held, HUD shown");
        }

        bool forced = g_hud_forced;
        if (ui::setting("Force HUD off", forced, "Ignores both conditions. For testing.")) {
            g_hud_forced = forced;
            if (!forced) {
                bridger::run_on_game_thread(restore_hud);
            }
        }

        if (!g_profile_verified) {
            ui::readout("PlayerProfile", ui::bad(),
                        g_profile == 0 ? "not resolved, load into the world"
                                       : "resolved but the type does not match");
            ui::readoutf("reported type", "{}", g_profile_type[0] != 0 ? g_profile_type : "-");
        }
        ui::readoutf("profile", "{:#x}", g_profile.load());
        ui::readoutf("state bytes", "{} {} {}  combat {}", g_probe_40d.load(), g_probe_40e.load(),
                     g_probe_40f.load(), g_probe_combat.load());
    }
    ui::end_group();

    if (ui::begin_group("Flicks", true)) {
        const auto flick = g_last_flick;
        ui::readoutf("flicks", g_flicks > 0 ? ui::good() : ui::faint(), "{}", g_flicks.load());
        if (g_flicks > 0) {
            ui::readoutf("last jump", "{:.2f} m, {:.0f} deg", flick.move, flick.turn);
            ui::readoutf("game view turned", "{:.0f} deg", flick.game_turn);
            ui::readoutf("third person leaked", "{}", flick.passthrough ? "yes" : "no");
            ui::readoutf("dropout", "{}", dropout_name(flick.dropout));
            ui::readoutf("head lookup misses", "{} (now held, total {})", flick.helper_misses,
                         g_helper_misses.load());
            ui::readoutf("fight camera", "{:.2f}", flick.fight);
        }
        ui::note("Walk around the interior until it flicks, then read this. If the game view "
                 "turned as much as the camera did, the game's own camera snapped.");
        ui::readoutf("F10 snapshots", "{}", g_flight_dumps.load());
        if (ui::button("Save last 3 seconds", 200.0f)) flight_dump();
    }
    ui::end_group();

    if (ui::begin_group("First person dropouts", false)) {
        ui::readoutf("camera entity type", "{}", g_camera_type[0] != 0 ? g_camera_type : "-");
        ui::readoutf("entity swaps", "{}", g_entity_swaps.load());
        const int reason = g_dropout.load();
        ui::readoutf("current", reason != 0 ? ui::bad() : ui::good(), "{}", dropout_name(reason));
        ui::readoutf("frames since", "{}", g_frames_since_dropout.load());
        ui::readoutf("nothing followed", "{}", g_dropout_counts[1].load());
        ui::readoutf("not player camera", "{}", g_dropout_counts[2].load());
        ui::readoutf("passthroughs", g_passthroughs > 0 ? ui::warn() : ui::faint(), "{}",
                     g_passthroughs.load());
        ui::readoutf("last dropout on a", "{}",
                     g_last_dropout_type[0] != 0 ? g_last_dropout_type : "-");
        bool trace = g_trace_visibility;
        if (ui::setting("Trace entity hiding", trace,
                        "Logs every Entity::SetVisible(false) with the caller's rva.")) {
            g_trace_visibility = trace;
        }
        ui::readoutf("traced", "{} / {}", g_trace_lines.load(), kTraceLimit);
        ui::readoutf("late eye fixes / max", "{} / {:.3f} m", g_eye_fixes.load(), g_eye_fix_max.load());
        if (ui::button("Clear counts", 130.0f)) {
            g_trace_lines = 0;
            for (auto& count : g_dropout_counts) {
                count = 0;
            }
            g_passthroughs = 0;
            g_entity_swaps = 0;
        }
        ui::note("Walk indoors in first person, then read which counter moved. The session log "
                 "records each dropout with the camera entity's type.");
    }
    ui::end_group();

    if (ui::begin_group("Diagnostics", false)) {
        ui::readoutf("player camera", "{}", g_is_player_camera);
        ui::readoutf("source", "{}", g_source);
        ui::readoutf("camera entity", "{}", g_camera_entity);
        ui::readoutf("game camera", "{:.1f}  {:.1f}  {:.1f}", g_game.px, g_game.py, g_game.pz);
        ui::readoutf("game angles", "{:.1f} / {:.1f} deg", g_game_heading, g_game_pitch);
        ui::readoutf("freecam", "{:.1f}  {:.1f}  {:.1f}", g_x, g_y, g_z);
        ui::readoutf("freecam angles", "{:.1f} / {:.1f} deg", g_heading, g_pitch);
        ui::readoutf("basis", "convention {} (error {:.4f})", g_convention, g_convention_error);
        ui::readoutf("commits / overrides", "{} / {}", g_commits, g_overrides);
        ui::readoutf("frame", "{:.1f} ms", g_dt * 1000.0f);
        ui::readoutf("native movement updates", "{}", g_native_turn_updates.load());
        ui::readoutf("turn checks / flagged / applies", "{} / {} / {}", g_turn_checks.load(),
                     g_turn_flagged.load(), g_turn_applies.load());
        ui::readoutf("turn state", "player {} vehicle {} view {}", g_player, g_in_vehicle,
                     g_view_valid);
        ui::readoutf("following", "{}", g_followed);
        ui::readoutf("recorder", g_recording ? ui::good() : ui::faint(), "{}  marker {}",
                     g_recording ? "recording" : "idle", g_marker.load());
    }
    ui::end_group();
}

const FreecamApi g_service{
    2,
    []() -> bool { return g_enabled; },
    [](bool value) { set_enabled(value); },
    [](double* xyz) {
        if (xyz != nullptr) {
            xyz[0] = g_x;
            xyz[1] = g_y;
            xyz[2] = g_z;
        }
    },
    [](float* heading, float* pitch) {
        if (heading != nullptr) { *heading = g_heading; }
        if (pitch != nullptr) { *pitch = g_pitch; }
    },
    [](double x, double y, double z) {
        g_x = x;
        g_y = y;
        g_z = z;
    },
    []() -> float { return g_speed; },
    [](float value) { g_speed = std::clamp(value, 0.25f, 400.0f); },
    []() -> bool { return g_first_person; },
    [](bool value) { set_first_person(value); },
};

}

bool bridger::on_load() {
    if (!g_set_orientation.install(kEntitySetOrientation, set_orientation_detour)) {
        bridger::error("could not hook Entity::SetOrientation");
        return false;
    }
    if (!g_player_update.install(0x269a2d0, player_update_detour)
        || !g_aim_direction.install(0x2639fd0, aim_direction_detour)) {
        bridger::error("could not hook native player movement and animated turning");
        g_player_update.remove();
        g_aim_direction.remove();
        g_set_orientation.remove();
        return false;
    }
    if (!g_turn_check.install(0x26a53d0, turn_check_detour)) {
        bridger::warn("could not hook the aim-turn check (diagnostics only)");
    }
    if (!g_turn_apply.install(0x26a5880, turn_apply_detour)) {
        bridger::warn("could not hook the aim-turn apply (diagnostics only)");
    }
    if (!g_slot610.install(0x2636550, slot610_detour)) {
        bridger::warn("could not hook the animation-side turn slot (diagnostics only)");
    }
    apply_saved();
    if (!g_set_visible_hook.install(kSetVisibleImpl, set_visible_detour)) {
        bridger::warn("could not hook Entity::SetVisible (visibility tracing unavailable)");
    }
    if (!g_gear_fade_hook.install(kGearFadeRoutine, gear_fade_detour)) {
        bridger::warn("could not hook the gear fade routine");
    }
    if (!g_model_show_hook.install(kModelShow, model_show_detour)) {
        bridger::warn("could not hook the model show switch (gear may vanish when looking down)");
    }
    if (!g_backpack_show_hook.install(kBackpackShow, backpack_show_detour)) {
        bridger::warn("could not hook the backpack show switch (cargo may vanish when looked at)");
    }
    if (!g_skinned_post_update_hook.install(kSkinnedPostUpdate, skinned_post_update_detour)) {
        bridger::warn("could not hook SkinnedModel post-update (head fold will not draw)");
    }
    if (!g_shader_variable_hook.install(kSetShaderVariable, shader_variable_detour)) {
        bridger::warn("could not hook the model alpha variable (Sam may fade when looking down)");
    }
    if (!g_vehicle_alpha_hook.install(kVehicleSetAlpha, vehicle_alpha_detour)) {
        bridger::warn("could not hook VehicleEntity::SetAlpha (vehicles will fade near the camera)");
    }
    bridger::game_tick(probe_bt_state);
    bridger::game_tick(late_eye_tick);
    bridger::hotkey(static_cast<unsigned>(s_key_record.get()), toggle_recording);
    bridger::hotkey(static_cast<unsigned>(s_key_marker.get()), drop_marker);
    if (!g_tpc_commit.install(kTpcCommit, tpc_commit_detour)) {
        bridger::warn("could not hook ThirdPersonPlayerCameraComponent commit");
    }
    if (!g_ds_tpc_commit.install(kDsTpcCommit, ds_tpc_commit_detour)) {
        bridger::warn("could not hook DSThirdPersonPlayerCameraComponent commit");
    }
    if (!g_tpc_commit.installed() && !g_ds_tpc_commit.installed()) {
        bridger::error("no camera component commit hook, freecam cannot find the camera");
        return false;
    }

    bridger::ui::panel("Free Camera", draw);
    if (!bridger::hotkey(static_cast<unsigned>(s_key_free.get()), toggle)) {
        bridger::warn("could not register the free camera hotkey, use the panel toggle");
    }
    bridger::hotkey(static_cast<unsigned>(s_key_first.get()), toggle_first_person);
    bridger::hotkey(VK_F10, flight_dump);
    bridger::hotkey(VK_F12, alpha_test_toggle);
    bridger::hotkey(VK_F11, [] {
        g_variable_log_budget = 3000;
        bridger::info("variable change log started (F11)");
    });
    bridger::provide("freecam.v1", &g_service);
    bridger::info("camera ready, {} free camera, {} first person",
              s_key_free.get(), s_key_first.get());
    return true;
}

void bridger::on_unload() {
    restore_hud();
    g_set_visible_hook.remove();
    g_skip_hooks.clear();
    g_probe_hooks.clear();
    watch_shutdown();
    g_gear_dither_off.clear();
    g_gear_fade_hook.remove();
    g_model_show_hook.remove();
    g_backpack_show_hook.remove();
    g_skinned_post_update_hook.remove();
    g_shader_variable_hook.remove();
    g_vehicle_alpha_hook.remove();
    g_enabled = false;
    g_first_person = false;
    sync_body_visibility(nullptr);
    camera_collision_restore();
    sync_head_parts(nullptr);
    g_hovered_child = -1;
    sync_children(false);
    g_set_orientation.remove();
    g_recording = false;
    record_close();
    g_slot610.remove();
    g_player_update.remove();
    g_aim_direction.remove();
    g_turn_check.remove();
    g_turn_apply.remove();
    g_tpc_commit.remove();
    g_ds_tpc_commit.remove();
}
