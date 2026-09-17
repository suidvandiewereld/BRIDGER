#include "loader/registry.h"

#include <Windows.h>

#include <MinHook.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <chrono>
#include <deque>
#include <format>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "content/content.h"
#include "core/guard.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/memory.h"
#include "decima/dumper.h"
#include "fx/fx.h"
#include "overlay/overlay.h"
#include "script/runtime.h"
#include "ui/ui.h"

namespace bridger::loader {
namespace {

using json = nlohmann::json;

std::string text_field(const json& node, const char* key, std::string fallback = {}) {
    if (node.contains(key) && node[key].is_string()) {
        return node[key].get<std::string>();
    }
    return fallback;
}

void api_log(BridgerLogLevel level, const char* message) {
    const auto& owner = Registry::instance().active_mod();
    const auto text = owner.empty() ? std::string(message)
                                    : std::format("[{}] {}", owner, message);
    switch (level) {
        case BRIDGER_LOG_TRACE: log::trace("{}", text); break;
        case BRIDGER_LOG_WARN:  log::warn("{}", text); break;
        case BRIDGER_LOG_ERROR: log::error("{}", text); break;
        default:                log::info("{}", text); break;
    }
}

void* api_resolve(std::uintptr_t rva) {
    const auto base = Registry::instance().image_base();
    return base ? reinterpret_cast<void*>(base + rva) : nullptr;
}

const void* api_find_type(const char* name) {
    return name ? decima::find_type(name) : nullptr;
}

void* api_find_symbol(const char* group, const char* name) {
    return (group && name) ? decima::find_symbol(group, name) : nullptr;
}

struct HookRecord {
    std::string owner;
    void* target = nullptr;
};
std::mutex g_hooks_mutex;
std::vector<HookRecord> g_hooks;

bool api_install_hook(void* target, void* detour, void** original) {
    if (target == nullptr || detour == nullptr) {
        return false;
    }
    const auto created = MH_CreateHook(target, detour, original);
    if (created != MH_OK) {
        log::warn("hook creation failed at {}: {}", target, MH_StatusToString(created));
        return false;
    }
    const auto enabled = MH_EnableHook(target);
    if (enabled != MH_OK) {
        log::warn("hook enable failed at {}: {}", target, MH_StatusToString(enabled));
        MH_RemoveHook(target);
        return false;
    }
    {
        std::scoped_lock lock(g_hooks_mutex);
        g_hooks.push_back({Registry::instance().active_mod(), target});
    }
    return true;
}

bool api_remove_hook(void* target) {
    if (target == nullptr) {
        return false;
    }
    {
        std::scoped_lock lock(g_hooks_mutex);
        std::erase_if(g_hooks, [&](const HookRecord& h) { return h.target == target; });
    }
    MH_DisableHook(target);
    return MH_RemoveHook(target) == MH_OK;
}

void api_register_panel(const char* label, BridgerPanelDraw draw, void* user) {
    if (label == nullptr || draw == nullptr) {
        return;
    }
    Panel panel;
    panel.owner = Registry::instance().active_mod();
    panel.label = label;
    panel.draw = draw;
    panel.user = user;
    Registry::instance().add_panel(std::move(panel));
}

void ui_text(const char* value) { ui::text(value ? value : ""); }
void ui_text_colored(std::uint32_t color, const char* value) { ui::text_colored(color, value ? value : ""); }
void ui_label_value(const char* label, const char* value) {
    ui::label_value(label ? label : "", value ? value : "");
}
bool ui_button(const char* label, float width) { return ui::button(label ? label : "", width); }
bool ui_checkbox(const char* label, bool value) { return ui::checkbox(label ? label : "", value); }
bool ui_selectable(const char* label, bool selected, float width) {
    return ui::selectable(label ? label : "", selected, width);
}
void ui_separator() { ui::separator(); }
void ui_spacing(float amount) { ui::spacing(amount); }
void ui_same_line(float offset) { ui::same_line(offset); }
void ui_progress(float fraction, float width, std::uint32_t color) {
    ui::progress(fraction, width, color);
}
void ui_begin_scroll(const char* id, float height) { ui::begin_scroll(id ? id : "scroll", height); }
void ui_end_scroll() { ui::end_scroll(); }
std::uint32_t ui_accent() { return ui::theme().accent; }
std::uint32_t ui_dim() { return ui::theme().text_dim; }
bool ui_slider_float(const char* label, float* value, float min, float max, float width) {
    return value != nullptr && ui::slider_float(label ? label : "", *value, min, max, width);
}
bool ui_slider_int(const char* label, int* value, int min, int max, float width) {
    return value != nullptr && ui::slider_int(label ? label : "", *value, min, max, width);
}
bool ui_input_text(const char* id, char* buffer, std::size_t capacity, const char* placeholder,
                   float width) {
    if (buffer == nullptr || capacity == 0) {
        return false;
    }
    std::string value(buffer, strnlen(buffer, capacity));
    const bool changed = ui::input_text(id ? id : "input", value, width, placeholder ? placeholder : "");
    if (changed) {
        const auto length = std::min(value.size(), capacity - 1);
        std::memcpy(buffer, value.data(), length);
        buffer[length] = 0;
    }
    return changed;
}
void ui_header(const char* label) { ui::header(label ? label : ""); }
void ui_indent(float amount) { ui::indent(amount); }
void ui_unindent(float amount) { ui::unindent(amount); }
float ui_available_width() { return ui::available_width(); }

const char* or_empty(const char* value) { return value != nullptr ? value : ""; }

void ui_begin_settings(float label_width) { ui::begin_settings(label_width); }
void ui_end_settings() { ui::end_settings(); }
bool ui_setting_bool(const char* label, bool* value, const char* help) {
    return value != nullptr && ui::setting_bool(or_empty(label), *value, or_empty(help));
}
bool ui_setting_float(const char* label, float* value, float min, float max, const char* suffix,
                      const char* help) {
    return value != nullptr
        && ui::setting_float(or_empty(label), *value, min, max, or_empty(suffix), or_empty(help));
}
bool ui_setting_int(const char* label, int* value, int min, int max, const char* suffix,
                    const char* help) {
    return value != nullptr
        && ui::setting_int(or_empty(label), *value, min, max, or_empty(suffix), or_empty(help));
}
bool ui_setting_combo(const char* label, int* index, const char* const* items, int count,
                      const char* help) {
    return index != nullptr
        && ui::setting_combo(or_empty(label), *index, items, count, or_empty(help));
}
bool ui_setting_key(const char* label, std::uint32_t* key, const char* help) {
    return key != nullptr && ui::setting_key(or_empty(label), *key, or_empty(help));
}
bool ui_setting_text(const char* label, char* buffer, std::size_t capacity,
                     const char* placeholder, const char* help) {
    if (buffer == nullptr || capacity == 0) {
        return false;
    }
    std::string value(buffer, strnlen(buffer, capacity));
    const bool changed =
        ui::setting_text(or_empty(label), value, or_empty(placeholder), or_empty(help));
    if (changed) {
        const auto length = std::min(value.size(), capacity - 1);
        std::memcpy(buffer, value.data(), length);
        buffer[length] = 0;
    }
    return changed;
}
void ui_readout(const char* label, const char* value) {
    ui::readout(or_empty(label), or_empty(value));
}
void ui_readout_colored(const char* label, std::uint32_t color, const char* value) {
    ui::readout_colored(or_empty(label), color, or_empty(value));
}
bool ui_revert_marker(bool modified) { return ui::revert_marker(modified); }
bool ui_begin_group(const char* label, bool default_open) {
    return ui::begin_group(or_empty(label), default_open);
}
void ui_end_group() { ui::end_group(); }
void ui_note(const char* value) { ui::note(or_empty(value)); }
void ui_badge(const char* label, std::uint32_t color) { ui::badge(or_empty(label), color); }
void ui_keycap(const char* label) { ui::keycap(or_empty(label)); }
void ui_tooltip(const char* value) { ui::tooltip(or_empty(value)); }
void ui_text_wrapped(std::uint32_t color, const char* value) {
    ui::text_wrapped(color, or_empty(value));
}
bool ui_ghost_button(const char* label, float width) {
    return ui::ghost_button(or_empty(label), width);
}
void ui_newline() { ui::newline(); }
void ui_push_id(const char* value) { ui::push_id(or_empty(value)); }
void ui_pop_id() { ui::pop_id(); }
std::uint32_t ui_color(BridgerColor role) {
    const auto& t = ui::theme();
    switch (role) {
        case BRIDGER_COLOR_DIM: return t.text_dim;
        case BRIDGER_COLOR_FAINT: return t.text_faint;
        case BRIDGER_COLOR_ACCENT: return t.accent;
        case BRIDGER_COLOR_GOOD: return t.good;
        case BRIDGER_COLOR_WARN: return t.warn;
        case BRIDGER_COLOR_BAD: return t.bad;
        default: return t.text;
    }
}

BridgerUi g_ui{
    ui_text,
    ui_text_colored,
    ui_label_value,
    ui_button,
    ui_checkbox,
    ui_selectable,
    ui_separator,
    ui_spacing,
    ui_same_line,
    ui_progress,
    ui_begin_scroll,
    ui_end_scroll,
    ui_accent,
    ui_dim,
    ui_slider_float,
    ui_slider_int,
    ui_input_text,
    ui_header,
    ui_indent,
    ui_unindent,
    ui_available_width,
    ui_begin_settings,
    ui_end_settings,
    ui_setting_bool,
    ui_setting_float,
    ui_setting_int,
    ui_setting_combo,
    ui_setting_key,
    ui_setting_text,
    ui_readout,
    ui_readout_colored,
    ui_revert_marker,
    ui_begin_group,
    ui_end_group,
    ui_note,
    ui_badge,
    ui_keycap,
    ui_tooltip,
    ui_text_wrapped,
    ui_ghost_button,
    ui_color,
    ui_newline,
    ui_push_id,
    ui_pop_id,
};

std::uint32_t api_guarded(BridgerGuarded body, void* user) {
    if (body == nullptr) {
        return 0;
    }
    const auto fault = guarded_call(body, user);
    if (fault != 0) {
        const auto& owner = Registry::instance().active_mod();
        log::error("mod {} faulted with code {:#x} inside a guarded call {}",
                   owner.empty() ? "?" : owner, fault, describe_last_fault());
    }
    return fault;
}

constexpr std::uintptr_t kWorldUpdate = 0x21bbb70;
constexpr std::uintptr_t kSecondsPerTick = 0x48dbaa8;
constexpr std::size_t kParamsTimeAdvances = 0x1;
constexpr std::size_t kParamsDeltaTicks = 0x20;

using WorldUpdateFn = void (*)(void* self, const void* params);
WorldUpdateFn g_game_tick_original = nullptr;
bool g_game_tick_hooked = false;
unsigned long g_game_tick_thread = 0;
std::atomic<std::uint64_t> g_game_tick_last{0};

thread_local std::string t_actor;
thread_local bool t_has_actor = false;

struct Queued {
    BridgerCallback fn = nullptr;
    void* user = nullptr;
};

struct Hotkey {
    std::string owner;
    std::atomic<std::uint32_t> key = 0;
    std::atomic<std::uint32_t> modifiers = 0;
    BridgerCallback fn = nullptr;
    void* user = nullptr;
    bool was_down = false;
};

std::shared_mutex g_dispatch_mutex;

std::mutex g_queue_mutex;
std::vector<Queued> g_queue;
std::deque<Hotkey> g_hotkeys;

bool key_is_down(std::uint32_t key) {
    return (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
}

bool modifiers_held(std::uint32_t modifiers) {
    if ((modifiers & BRIDGER_MOD_SHIFT) && !key_is_down(VK_SHIFT)) return false;
    if ((modifiers & BRIDGER_MOD_CONTROL) && !key_is_down(VK_CONTROL)) return false;
    if ((modifiers & BRIDGER_MOD_ALT) && !key_is_down(VK_MENU)) return false;
    return true;
}

void poll_hotkeys() {
    const bool suppressed = overlay::visible();
    for (auto& hotkey : g_hotkeys) {
        const std::uint32_t key = hotkey.key.load(std::memory_order_relaxed);
        if (key == 0) {
            hotkey.was_down = false;
            continue;
        }
        const bool down = key_is_down(key);
        if (down && !hotkey.was_down && !suppressed
                && modifiers_held(hotkey.modifiers.load(std::memory_order_relaxed))) {
            hotkey.fn(hotkey.user);
        }
        hotkey.was_down = down;
    }
}

void drain_queue() {
    std::vector<Queued> pending;
    {
        std::scoped_lock lock(g_queue_mutex);
        pending.swap(g_queue);
    }
    for (const auto& item : pending) {
        item.fn(item.user);
    }
}

std::string thread_description(unsigned long thread_id) {
    using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto fn = kernel ? reinterpret_cast<GetDescription>(
                                 GetProcAddress(kernel, "GetThreadDescription"))
                           : nullptr;
    std::string result;
    if (fn != nullptr) {
        const auto handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id);
        PWSTR text = nullptr;
        if (handle != nullptr && SUCCEEDED(fn(handle, &text)) && text != nullptr) {
            for (const wchar_t* p = text; *p != 0; ++p) {
                result.push_back(*p < 128 ? static_cast<char>(*p) : '?');
            }
            LocalFree(text);
        }
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }
    return result.empty() ? "unnamed" : result;
}

void game_tick_detour(void* self, const void* params) {
    g_game_tick_original(self, params);

    float dt = 0.0f;
    const auto* bytes = static_cast<const std::uint8_t*>(params);
    const auto* seconds_per_tick = static_cast<const float*>(api_resolve(kSecondsPerTick));
    if (bytes != nullptr && seconds_per_tick != nullptr && bytes[kParamsTimeAdvances] != 0) {
        std::int32_t ticks = 0;
        std::memcpy(&ticks, bytes + kParamsDeltaTicks, sizeof ticks);
        dt = static_cast<float>(ticks) * *seconds_per_tick;
    }
    if (!(dt >= 0.0f)) {
        dt = 0.0f;
    }
    dt = std::clamp(dt, 0.0f, 0.1f);

    if (g_game_tick_thread == 0) {
        promote_fault_handler();
        g_game_tick_thread = GetCurrentThreadId();
        log::info("game tick running on thread {} ({})", g_game_tick_thread,
                  thread_description(g_game_tick_thread));
    }
    g_game_tick_last.store(GetTickCount64(), std::memory_order_relaxed);

    fx::sample_camera();
    content::tick();
    drain_queue();
    poll_hotkeys();
    std::shared_lock dispatch(g_dispatch_mutex);
    for (const auto& tick : Registry::instance().game_ticks()) {
        tick.fn(tick.user, dt);
    }
}

bool install_game_tick_hook() {
    if (g_game_tick_hooked) {
        return true;
    }
    const auto target = api_resolve(kWorldUpdate);
    if (target == nullptr) {
        return false;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(game_tick_detour),
                      reinterpret_cast<void**>(&g_game_tick_original)) != MH_OK) {
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        return false;
    }
    g_game_tick_hooked = true;
    log::info("game tick hooked at the world update ({:#x})", kWorldUpdate);
    return true;
}

void remove_game_tick_hook() {
    if (!g_game_tick_hooked) {
        return;
    }
    const auto target = api_resolve(kWorldUpdate);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    g_game_tick_hooked = false;
    g_game_tick_original = nullptr;
    g_game_tick_thread = 0;
    g_hotkeys.clear();
    std::scoped_lock lock(g_queue_mutex);
    g_queue.clear();
}

void api_run_on_game_thread(BridgerCallback fn, void* user) {
    if (fn == nullptr) {
        return;
    }
    if (!install_game_tick_hook()) {
        log::error("run_on_game_thread: the game tick hook could not be installed, callback dropped");
        return;
    }
    std::scoped_lock lock(g_queue_mutex);
    g_queue.push_back({fn, user});
}

bool api_rebind_hotkey(BridgerCallback fn, void* user, std::uint32_t key,
                       std::uint32_t modifiers) {
    if (fn == nullptr) {
        return false;
    }
    for (auto& hotkey : g_hotkeys) {
        if (hotkey.fn == fn && hotkey.user == user) {
            hotkey.key.store(key, std::memory_order_relaxed);
            hotkey.modifiers.store(modifiers, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

bool api_register_hotkey(std::uint32_t key, std::uint32_t modifiers, BridgerCallback fn, void* user) {
    if (fn == nullptr || key == 0 || !install_game_tick_hook()) {
        return false;
    }
    const auto dead = std::find_if(g_hotkeys.begin(), g_hotkeys.end(), [](const Hotkey& h) {
        return h.fn == nullptr;
    });
    Hotkey& hotkey = dead != g_hotkeys.end() ? *dead : g_hotkeys.emplace_back();
    hotkey.owner = Registry::instance().active_mod();
    hotkey.key = key;
    hotkey.modifiers = modifiers;
    hotkey.fn = fn;
    hotkey.user = user;
    return true;
}

bool api_overlay_visible() {
    return overlay::visible();
}

bool api_key_down(std::uint32_t key) {
    return !overlay::visible() && key_is_down(key);
}

constexpr std::size_t kKind = 4;
constexpr std::size_t kClassBaseCount = 5;
constexpr std::size_t kClassHandlerCount = 8;
constexpr std::size_t kClassSize = 16;
constexpr std::size_t kClassName = 56;
constexpr std::size_t kClassBases = 88;
constexpr std::size_t kClassHandlers = 112;
constexpr std::size_t kSmallName = 16;

template <typename T>
T read_at(const void* base, std::size_t offset) {
    T value{};
    safe_read(static_cast<const std::uint8_t*>(base) + offset, &value, sizeof value);
    return value;
}

const mem::Module& game_module() {
    static const mem::Module module = mem::module_of();
    return module;
}

bool in_image(const void* pointer) {
    const auto& module = game_module();
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return module.valid() && address >= module.base && address < module.base + module.size;
}

const char* api_rtti_name(const void* rtti) {
    if (rtti == nullptr || !in_image(rtti)) {
        return nullptr;
    }
    switch (read_at<std::uint8_t>(rtti, kKind)) {
        case 4: return read_at<const char*>(rtti, kClassName);
        case 0:
        case 3:
        case 5: return read_at<const char*>(rtti, kSmallName);
        case 1:
        case 2: {
            const void* data = read_at<const void*>(rtti, kSmallName);
            return data != nullptr ? read_at<const char*>(data, 0) : nullptr;
        }
        default: return nullptr;
    }
}

bool derives_from(const void* rtti, const void* base, int depth) {
    if (rtti == base) {
        return true;
    }
    if (rtti == nullptr || depth > 32 || !in_image(rtti) || read_at<std::uint8_t>(rtti, kKind) != 4) {
        return false;
    }
    const auto count = read_at<std::uint8_t>(rtti, kClassBaseCount);
    const auto* bases = read_at<const std::uint8_t*>(rtti, kClassBases);
    if (bases == nullptr) {
        return false;
    }
    for (std::uint8_t i = 0; i < count; ++i) {
        if (derives_from(read_at<const void*>(bases, i * 16), base, depth + 1)) {
            return true;
        }
    }
    return false;
}

bool api_rtti_is_a(const void* rtti, const void* base) {
    return rtti != nullptr && base != nullptr && derives_from(rtti, base, 0);
}

std::uint32_t api_type_size(const void* rtti) {
    if (rtti == nullptr || !in_image(rtti)) {
        return 0;
    }
    switch (read_at<std::uint8_t>(rtti, kKind)) {
        case 4: return read_at<std::uint32_t>(rtti, kClassSize);
        case 3:
        case 5: return read_at<std::uint8_t>(rtti, 5);
        case 1:
        case 2: {
            const void* data = read_at<const void*>(rtti, kSmallName);
            return data != nullptr ? read_at<std::uint32_t>(data, 8) : 0;
        }
        default: return 0;
    }
}

void* api_find_handler(const char* class_name, const char* message_name) {
    if (class_name == nullptr || message_name == nullptr) {
        return nullptr;
    }
    const void* rtti = decima::find_type(class_name);
    if (rtti == nullptr || read_at<std::uint8_t>(rtti, kKind) != 4) {
        return nullptr;
    }
    const auto count = read_at<std::uint8_t>(rtti, kClassHandlerCount);
    const auto* table = read_at<const std::uint8_t*>(rtti, kClassHandlers);
    if (table == nullptr || !in_image(table)) {
        return nullptr;
    }
    for (std::uint8_t i = 0; i < count; ++i) {
        const void* message = read_at<const void*>(table, i * 16);
        const char* name = api_rtti_name(message);
        if (name != nullptr && std::strcmp(name, message_name) == 0) {
            return read_at<void*>(table, i * 16 + 8);
        }
    }
    return nullptr;
}

const void* api_rtti_of(const void* object) {
    if (object == nullptr) {
        return nullptr;
    }
    const void* vtable = read_at<const void*>(object, 0);
    if (!in_image(vtable)) {
        return nullptr;
    }
    const auto& module = game_module();
    const auto slot = read_at<std::uintptr_t>(vtable, 0);
    if (slot < module.text_begin || slot >= module.text_end) {
        return nullptr;
    }
    auto at = slot;
    for (int hops = 0; hops < 2; ++hops) {
        std::uint8_t code[8]{};
        if (!safe_read(reinterpret_cast<const void*>(at), code, sizeof code)) {
            return nullptr;
        }
        std::int32_t displacement = 0;
        if (code[0] == 0x48 && code[1] == 0x8d && code[2] == 0x05 && code[7] == 0xc3) {
            std::memcpy(&displacement, code + 3, sizeof displacement);
            const void* rtti = reinterpret_cast<const void*>(at + 7 + displacement);
            return in_image(rtti) ? rtti : nullptr;
        }
        std::size_t jump = 0;
        if (code[0] == 0x48 && code[1] == 0x83 && code[2] == 0xe9 && code[4] == 0xe9) {
            jump = 4;
        } else if (code[0] != 0xe9) {
            return nullptr;
        }
        std::memcpy(&displacement, code + jump + 1, sizeof displacement);
        at = at + jump + 5 + displacement;
        if (at < module.text_begin || at >= module.text_end) {
            return nullptr;
        }
    }
    return nullptr;
}

void* api_scan_pattern(const char* pattern) {
    if (pattern == nullptr) {
        return nullptr;
    }
    const mem::Pattern signature(pattern);
    if (signature.empty()) {
        return nullptr;
    }
    return reinterpret_cast<void*>(signature.scan(game_module()));
}

std::mutex g_services_mutex;
struct Service {
    const void* table = nullptr;
    std::string owner;
};
std::unordered_map<std::string, Service> g_services;

bool api_provide(const char* name, const void* table) {
    if (name == nullptr || table == nullptr) {
        return false;
    }
    std::scoped_lock lock(g_services_mutex);
    g_services[name] = {table, Registry::instance().active_mod()};
    const auto& owner = Registry::instance().active_mod();
    log::info("{} provides service {}", owner.empty() ? "core" : owner, name);
    return true;
}

const void* api_require(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    std::scoped_lock lock(g_services_mutex);
    const auto found = g_services.find(name);
    return found == g_services.end() ? nullptr : found->second.table;
}

bool api_mod_loaded(const char* id) {
    if (id == nullptr) {
        return false;
    }
    for (const auto& mod : Registry::instance().mods()) {
        if (mod.id == id) {
            return mod.state == State::Loaded;
        }
    }
    return false;
}

void api_register_game_tick(BridgerTick tick, void* user) {
    if (tick == nullptr) {
        return;
    }
    Tick entry;
    entry.owner = Registry::instance().active_mod();
    entry.fn = tick;
    entry.user = user;
    Registry::instance().add_game_tick(std::move(entry));
}

void api_register_tick(BridgerTick tick, void* user) {
    if (tick == nullptr) {
        return;
    }
    Tick entry;
    entry.owner = Registry::instance().active_mod();
    entry.fn = tick;
    entry.user = user;
    Registry::instance().add_tick(std::move(entry));
}

std::size_t api_settings_get_text(const char* mod_id, const char* key, const char* fallback,
                                 char* buffer, std::size_t capacity) {
    if (mod_id == nullptr || key == nullptr) {
        return 0;
    }
    const std::string value =
        settings::get_text(mod_id, key, fallback != nullptr ? fallback : "");
    if (buffer != nullptr && capacity > 0) {
        const auto length = std::min(value.size(), capacity - 1);
        std::memcpy(buffer, value.data(), length);
        buffer[length] = 0;
    }
    return value.size();
}

bool api_settings_has(const char* mod_id, const char* key) {
    return mod_id != nullptr && key != nullptr && settings::has(mod_id, key);
}
bool api_settings_get_bool(const char* mod_id, const char* key, bool fallback) {
    return (mod_id != nullptr && key != nullptr) ? settings::get_bool(mod_id, key, fallback)
                                                 : fallback;
}
double api_settings_get_number(const char* mod_id, const char* key, double fallback) {
    return (mod_id != nullptr && key != nullptr) ? settings::get_number(mod_id, key, fallback)
                                                 : fallback;
}
void api_settings_set_bool(const char* mod_id, const char* key, bool value) {
    if (mod_id != nullptr && key != nullptr) {
        settings::set_bool(mod_id, key, value);
    }
}
void api_settings_set_number(const char* mod_id, const char* key, double value) {
    if (mod_id != nullptr && key != nullptr) {
        settings::set_number(mod_id, key, value);
    }
}
void api_settings_set_text(const char* mod_id, const char* key, const char* value) {
    if (mod_id != nullptr && key != nullptr) {
        settings::set_text(mod_id, key, value != nullptr ? value : "");
    }
}
void api_settings_erase(const char* mod_id, const char* key) {
    if (mod_id != nullptr && key != nullptr) {
        settings::erase(mod_id, key);
    }
}

BridgerApi g_api{
    BRIDGER_API_VERSION,
    0,
    api_log,
    api_resolve,
    api_find_type,
    api_find_symbol,
    api_install_hook,
    api_remove_hook,
    api_register_panel,
    api_register_tick,
    api_guarded,
    &g_ui,
    api_register_game_tick,
    api_find_handler,
    api_rtti_of,
    api_rtti_name,
    api_rtti_is_a,
    api_type_size,
    api_scan_pattern,
    api_run_on_game_thread,
    api_register_hotkey,
    api_overlay_visible,
    api_key_down,
    api_provide,
    api_require,
    api_mod_loaded,
    api_settings_has,
    api_settings_get_bool,
    api_settings_get_number,
    api_settings_get_text,
    api_settings_set_bool,
    api_settings_set_number,
    api_settings_set_text,
    api_settings_erase,
    api_rebind_hotkey,
    fx::api(),
    content::api(),
};

}

DispatchGuard::DispatchGuard() { g_dispatch_mutex.lock_shared(); }
DispatchGuard::~DispatchGuard() { g_dispatch_mutex.unlock_shared(); }

const BridgerApi* api() {
    return &g_api;
}

std::string owner_of_address(const void* address) {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    if (value == 0) {
        return {};
    }
    for (const auto& mod : Registry::instance().mods()) {
        if (mod.state != State::Loaded || mod.module == nullptr) {
            continue;
        }
        const auto begin = reinterpret_cast<std::uintptr_t>(mod.module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(begin);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            continue;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(begin + dos->e_lfanew);
        if (value >= begin && value < begin + nt->OptionalHeader.SizeOfImage) {
            return mod.id;
        }
    }
    return {};
}

bool ensure_game_tick() {
    return install_game_tick_hook();
}

bool on_game_thread() {
    return g_game_tick_thread != 0 && GetCurrentThreadId() == g_game_tick_thread;
}

bool game_thread_ticking(unsigned within_ms) {
    const auto last = g_game_tick_last.load(std::memory_order_relaxed);
    return last != 0 && GetTickCount64() - last <= within_ms;
}

ScopedActor::ScopedActor(std::string id) : previous_(t_actor), had_previous_(t_has_actor) {
    t_actor = std::move(id);
    t_has_actor = true;
}

ScopedActor::~ScopedActor() {
    t_actor = std::move(previous_);
    t_has_actor = had_previous_;
}

Registry& Registry::instance() {
    static Registry registry;
    return registry;
}

const std::string& Registry::active_mod() const {
    return t_has_actor ? t_actor : active_;
}

void Registry::configure(const std::filesystem::path& root, std::uintptr_t image_base) {
    root_ = root;
    image_base_ = image_base;
    g_api.image_base = image_base;
    settings::configure(root);
    load_config();
}

void Registry::load_config() {
    disabled_.clear();
    const auto path = root_ / "config.json";
    std::ifstream stream(path);
    if (!stream.is_open()) {
        return;
    }
    try {
        json document = json::parse(stream, nullptr, true, true);
        if (document.contains("disabled") && document["disabled"].is_array()) {
            for (const auto& entry : document["disabled"]) {
                if (entry.is_string()) {
                    disabled_.push_back(entry.get<std::string>());
                }
            }
        }
    } catch (const std::exception& error) {
        log::warn("config.json is not valid json: {}", error.what());
    }
}

void Registry::save_config() const {
    json document;
    document["disabled"] = disabled_;
    std::ofstream stream(root_ / "config.json", std::ios::trunc);
    if (stream.is_open()) {
        stream << document.dump(2) << "\n";
    }
}

bool Registry::enabled(const std::string& id) const {
    return std::find(disabled_.begin(), disabled_.end(), id) == disabled_.end();
}

void Registry::set_enabled(const std::string& id, bool value) {
    const auto it = std::find(disabled_.begin(), disabled_.end(), id);
    if (value && it != disabled_.end()) {
        disabled_.erase(it);
    } else if (!value && it == disabled_.end()) {
        disabled_.push_back(id);
    }
    save_config();
}

void Registry::add_panel(Panel panel) {
    panels_.push_back(std::move(panel));
}

void Registry::add_tick(Tick tick) {
    ticks_.push_back(std::move(tick));
}

void Registry::add_game_tick(Tick tick) {
    if (!install_game_tick_hook()) {
        log::error("mod {} asked for a game tick but the hook could not be installed",
                   tick.owner.empty() ? "?" : tick.owner);
        return;
    }
    game_ticks_.push_back(std::move(tick));
}

bool Registry::read_manifest(Mod& mod) const {
    if (mod.loose) {
        mod.name = mod.id;
        mod.version = "script";
        mod.script = true;
        return true;
    }
    std::error_code exists_ec;
    if (!std::filesystem::exists(mod.directory / "manifest.json", exists_ec)
        && std::filesystem::exists(mod.directory / "main.lua", exists_ec)) {
        mod.name = mod.id;
        mod.version = "script";
        mod.entry = "main.lua";
        mod.script = true;
        mod.dependencies.clear();
        return true;
    }
    std::ifstream stream(mod.directory / "manifest.json");
    try {
        json document = json::parse(stream, nullptr, true, true);
        mod.id = text_field(document, "id", mod.id);
        mod.name = text_field(document, "name", mod.id);
        mod.version = text_field(document, "version", "0.0.0");
        mod.author = text_field(document, "author");
        mod.description = text_field(document, "description");
        std::error_code lua_ec;
        const bool has_main = std::filesystem::exists(mod.directory / "main.lua", lua_ec);
        mod.entry = text_field(document, "entry", has_main ? "main.lua" : mod.id + ".dll");
        mod.script = mod.entry.size() > 4 && mod.entry.ends_with(".lua");
        mod.dependencies.clear();
        if (document.contains("dependencies") && document["dependencies"].is_array()) {
            for (const auto& item : document["dependencies"]) {
                if (item.is_string()) {
                    mod.dependencies.push_back(item.get<std::string>());
                }
            }
        }
    } catch (const std::exception& error) {
        mod.error = std::string("manifest: ") + error.what();
        return false;
    }
    return true;
}

void Registry::discover() {
    mods_.clear();
    mods_.reserve(kModCapacity);
    const auto directory = root_ / "mods";

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (!std::filesystem::exists(directory, ec)) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto manifest = entry.path() / "manifest.json";
        if (!std::filesystem::exists(manifest, ec)
            && !std::filesystem::exists(entry.path() / "main.lua", ec)) {
            continue;
        }

        Mod mod;
        mod.directory = entry.path();
        mod.id = entry.path().filename().string();

        if (!read_manifest(mod)) {
            mod.state = State::Failed;
            mods_.push_back(std::move(mod));
            continue;
        }

        if (!enabled(mod.id)) {
            mod.state = State::Disabled;
        }
        mods_.push_back(std::move(mod));
    }

    for (auto& mod : find_new_scripts()) {
        mods_.push_back(std::move(mod));
    }
    log::info("discovered {} mod(s)", mods_.size());
}

std::vector<Mod> Registry::find_new_scripts() const {
    std::vector<Mod> found;
    const auto known = [&](const std::string& id) {
        return std::any_of(mods_.begin(), mods_.end(), [&](const Mod& m) { return m.id == id; })
            || std::any_of(found.begin(), found.end(), [&](const Mod& m) { return m.id == id; });
    };
    std::error_code ec;

    const auto folders = root_ / "mods";
    for (const auto& entry : std::filesystem::directory_iterator(folders, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto id = entry.path().filename().string();
        if (known(id) || std::filesystem::exists(entry.path() / "manifest.json", ec)
            || !std::filesystem::exists(entry.path() / "main.lua", ec)) {
            continue;
        }
        Mod mod;
        mod.id = id;
        mod.directory = entry.path();
        read_manifest(mod);
        mod.state = enabled(mod.id) ? State::Discovered : State::Disabled;
        found.push_back(std::move(mod));
    }

    const auto loose = root_ / "scripts";
    std::filesystem::create_directories(loose, ec);
    for (const auto& entry : std::filesystem::directory_iterator(loose, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".lua") {
            continue;
        }
        const auto id = entry.path().stem().string();
        if (id.empty() || id.front() == '_' || id.front() == '.') {
            continue;
        }
        if (known(id)) {
            continue;
        }
        Mod mod;
        mod.id = id;
        mod.directory = loose;
        mod.entry = entry.path().filename().string();
        mod.loose = true;
        read_manifest(mod);
        mod.state = enabled(mod.id) ? State::Discovered : State::Disabled;
        found.push_back(std::move(mod));
    }
    return found;
}

std::vector<std::size_t> Registry::resolve_order() const {
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < mods_.size(); ++i) {
        index[mods_[i].id] = i;
    }

    std::vector<std::size_t> order;
    std::unordered_set<std::string> placed;
    std::unordered_set<std::string> visiting;

    auto visit = [&](auto&& self, std::size_t position) -> void {
        const auto& mod = mods_[position];
        if (placed.count(mod.id) || visiting.count(mod.id)) {
            return;
        }
        visiting.insert(mod.id);
        for (const auto& dependency : mod.dependencies) {
            const auto found = index.find(dependency);
            if (found != index.end()) {
                self(self, found->second);
            }
        }
        visiting.erase(mod.id);
        placed.insert(mod.id);
        order.push_back(position);
    };

    for (std::size_t i = 0; i < mods_.size(); ++i) {
        visit(visit, i);
    }
    return order;
}

bool Registry::load_one(Mod& mod) {
    for (const auto& dependency : mod.dependencies) {
        const auto found = std::find_if(mods_.begin(), mods_.end(),
                                        [&](const Mod& other) { return other.id == dependency; });
        if (found == mods_.end()) {
            mod.error = "missing dependency " + dependency;
            return false;
        }
        if (found->state != State::Loaded) {
            mod.error = "dependency not loaded: " + dependency;
            return false;
        }
    }

    const auto path = mod.directory / mod.entry;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        mod.error = "missing entry " + mod.entry;
        return false;
    }

    if (mod.script) {
        mod.source_time = script::sources_stamp(mod);
        settings::load(mod.id);
        set_active_mod(mod.id);
        const bool ok = script::load(mod, mod.error);
        set_active_mod({});
        if (!ok) {
            script::stop(mod.id);
            remove_owned(mod.id, nullptr);
            script::close(mod.id);
        }
        return ok;
    }

    mod.source_time = std::filesystem::last_write_time(path, ec);
    const auto cache = root_ / "cache";
    std::filesystem::create_directories(cache, ec);
    mod.shadow = cache / (mod.id + "-" + mod.entry);
    std::filesystem::copy_file(path, mod.shadow,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        mod.error = std::format("could not stage {}: {}", mod.entry, ec.message());
        return false;
    }

    const auto handle = LoadLibraryW(mod.shadow.c_str());
    if (handle == nullptr) {
        mod.error = std::format("LoadLibrary failed ({})", GetLastError());
        return false;
    }
    mod.module = handle;

    const auto info_fn = reinterpret_cast<BridgerModInfoFn>(
        GetProcAddress(handle, "BridgerMod_Info"));
    const auto load_fn = reinterpret_cast<BridgerModLoadFn>(
        GetProcAddress(handle, "BridgerMod_Load"));
    mod.unload = reinterpret_cast<BridgerModUnloadFn>(
        GetProcAddress(handle, "BridgerMod_Unload"));

    if (load_fn == nullptr) {
        mod.error = "missing BridgerMod_Load";
        return false;
    }

    settings::load(mod.id);

    if (info_fn != nullptr) {
        if (const auto* info = info_fn(); info != nullptr) {
            if (info->api_version != BRIDGER_API_VERSION) {
                mod.error = std::format("api version {}, loader expects {}",
                                        info->api_version, BRIDGER_API_VERSION);
                return false;
            }
            if (info->name != nullptr) {
                mod.name = info->name;
            }
            if (info->version != nullptr) {
                mod.version = info->version;
            }
            if (info->author != nullptr) {
                mod.author = info->author;
            }
            if (info->description != nullptr) {
                mod.description = info->description;
            }
        }
    }

    set_active_mod(mod.id);
    const bool ok = load_fn(api());
    set_active_mod({});
    if (!ok && mod.error.empty()) {
        mod.error = "BridgerMod_Load returned false";
    }
    return ok;
}

void Registry::load_all() {
    if (const auto status = MH_Initialize();
        status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log::warn("MinHook failed to initialise ({}), mod hooks will not work",
                  MH_StatusToString(status));
    }
    if (!install_game_tick_hook()) {
        log::warn("game tick hook unavailable: world-space shaders will have no camera");
    }

    std::unique_lock dispatch(g_dispatch_mutex);
    for (const auto position : resolve_order()) {
        auto& mod = mods_[position];
        if (mod.state == State::Disabled || mod.state == State::Failed) {
            continue;
        }
        if (load_one(mod)) {
            mod.state = State::Loaded;
            log::info("loaded mod {} {}", mod.id, mod.version);
        } else {
            mod.state = State::Failed;
            log::error("mod {} failed: {}", mod.id, mod.error);
            if (mod.module != nullptr) {
                FreeLibrary(static_cast<HMODULE>(mod.module));
                mod.module = nullptr;
            }
        }
    }
}

void Registry::remove_owned(const std::string& id, void* module) {
    std::erase_if(panels_, [&](const Panel& p) { return p.owner == id; });
    std::erase_if(ticks_, [&](const Tick& t) { return t.owner == id; });
    std::erase_if(game_ticks_, [&](const Tick& t) { return t.owner == id; });
    {
        std::scoped_lock lock(g_hooks_mutex);
        std::size_t leaked = 0;
        for (const auto& hook : g_hooks) {
            if (hook.owner != id) {
                continue;
            }
            MH_DisableHook(hook.target);
            MH_RemoveHook(hook.target);
            ++leaked;
        }
        if (leaked > 0) {
            log::warn("mod {} left {} hook(s) installed; removed them for you", id, leaked);
        }
        std::erase_if(g_hooks, [&](const HookRecord& h) { return h.owner == id; });
    }
    {
        std::scoped_lock lock(g_services_mutex);
        std::erase_if(g_services, [&](const auto& entry) { return entry.second.owner == id; });
    }
    for (auto& hotkey : g_hotkeys) {
        if (hotkey.owner != id) {
            continue;
        }
        hotkey.key.store(0, std::memory_order_relaxed);
        hotkey.modifiers.store(0, std::memory_order_relaxed);
        hotkey.fn = nullptr;
        hotkey.user = nullptr;
        hotkey.was_down = false;
        hotkey.owner.clear();
    }
    if (module != nullptr) {
        const auto begin = reinterpret_cast<std::uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(begin);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(begin + dos->e_lfanew);
            const auto end = begin + nt->OptionalHeader.SizeOfImage;
            std::scoped_lock lock(g_queue_mutex);
            std::erase_if(g_queue, [&](const Queued& q) {
                const auto fn = reinterpret_cast<std::uintptr_t>(q.fn);
                return fn >= begin && fn < end;
            });
        }
    }
    content::destroy_owned(id);
    fx::destroy_owned(id);
}

bool Registry::unload_one(Mod& mod) {
    if (mod.state != State::Loaded) {
        return false;
    }
    if (mod.script) {
        script::stop(mod.id);
        remove_owned(mod.id, nullptr);
        settings::flush_now();
        script::close(mod.id);
        mod.state = State::Discovered;
        return true;
    }
    if (mod.unload != nullptr) {
        mod.unload();
    }
    remove_owned(mod.id, mod.module);
    settings::flush_now();
    if (mod.module != nullptr) {
        FreeLibrary(static_cast<HMODULE>(mod.module));
        mod.module = nullptr;
    }
    if (!mod.shadow.empty()) {
        std::error_code ec;
        std::filesystem::remove(mod.shadow, ec);
        mod.shadow.clear();
    }
    mod.unload = nullptr;
    mod.state = State::Discovered;
    return true;
}

void Registry::reload(const std::string& id) {
    const auto found = std::find_if(mods_.begin(), mods_.end(),
                                    [&](const Mod& m) { return m.id == id; });
    if (found == mods_.end()) {
        log::warn("reload: no mod {}", id);
        return;
    }
    const bool was_loaded = found->state == State::Loaded;
    std::error_code gone_ec;
    const bool gone = found->script && !std::filesystem::exists(found->directory / found->entry, gone_ec);
    if (was_loaded && found->script && !gone) {
        std::string syntax;
        if (!script::check_syntax(*found, syntax)) {
            found->source_time = script::sources_stamp(*found);
            found->error = syntax;
            log::error("{} not reloaded, the running version stays: {}", found->id, syntax);
            return;
        }
    }
    if (was_loaded) {
        unload_one(*found);
    }
    found->error.clear();
    if (!read_manifest(*found)) {
        found->state = State::Failed;
        log::error("mod {} failed to reload: {}", found->id, found->error);
        return;
    }
    if (load_one(*found)) {
        found->state = State::Loaded;
        log::info("reloaded mod {} {}", found->id, found->version);
    } else {
        found->state = State::Failed;
        log::error("mod {} failed to reload: {}", found->id, found->error);
        if (found->module != nullptr) {
            FreeLibrary(static_cast<HMODULE>(found->module));
            found->module = nullptr;
        }
    }
}

void Registry::poll_for_changes(float dt) {
    if (!auto_reload_) {
        return;
    }
    watch_timer_ += dt;
    if (watch_timer_ < 0.5f) {
        return;
    }
    discover_timer_ += 0.5f;
    if (discover_timer_ >= 2.0f) {
        discover_timer_ = 0.0f;
        auto found = find_new_scripts();
        if (!found.empty()) {
            std::scoped_lock lock(pending_mutex_);
            for (auto& mod : found) {
                const bool queued = std::any_of(pending_new_.begin(), pending_new_.end(),
                                                [&](const Mod& m) { return m.id == mod.id; });
                if (!queued) {
                    log::info("new script mod {} found", mod.id);
                    pending_new_.push_back(std::move(mod));
                }
            }
        }
    }
    watch_timer_ = 0.0f;
    for (const auto& mod : mods_) {
        const bool watched = mod.state == State::Loaded || (mod.script && mod.state == State::Failed);
        if (!watched || mod.entry.empty()) {
            continue;
        }
        std::error_code ec;
        const auto path = mod.directory / mod.entry;
        if (mod.script && !std::filesystem::exists(path, ec)) {
            if (mod.state == State::Loaded) {
                log::info("{} was deleted, stopping it", mod.id);
                request_reload(mod.id);
            }
            continue;
        }
        const auto stamp = mod.script ? script::sources_stamp(mod)
                                      : std::filesystem::last_write_time(path, ec);
        if (ec || stamp == mod.source_time || stamp == decltype(stamp){}) {
            continue;
        }
        const auto age = decltype(stamp)::clock::now() - stamp;
        if (age < std::chrono::milliseconds(mod.script ? 120 : 400)) {
            continue;
        }
        log::info("{} changed on disk, reloading", mod.id);
        request_reload(mod.id);
    }
}

void Registry::request_reload(const std::string& id) {
    std::scoped_lock lock(pending_mutex_);
    if (std::find(pending_.begin(), pending_.end(), id) == pending_.end()) {
        pending_.push_back(id);
    }
}

void Registry::process_pending_reloads() {
    std::vector<std::string> ids;
    std::vector<Mod> added;
    {
        std::scoped_lock lock(pending_mutex_);
        if (pending_.empty() && pending_new_.empty()) {
            return;
        }
        ids.swap(pending_);
        added.swap(pending_new_);
    }
    std::unique_lock dispatch(g_dispatch_mutex);
    for (auto& mod : added) {
        if (std::any_of(mods_.begin(), mods_.end(), [&](const Mod& m) { return m.id == mod.id; })) {
            continue;
        }
        if (mods_.size() >= mods_.capacity()) {
            log::warn("{} found, but the mod list is full; restart the game to load it", mod.id);
            continue;
        }
        auto& placed = mods_.emplace_back(std::move(mod));
        if (placed.state == State::Disabled) {
            continue;
        }
        if (load_one(placed)) {
            placed.state = State::Loaded;
            log::info("loaded mod {} {}", placed.id, placed.version);
        } else {
            placed.state = State::Failed;
            log::error("mod {} failed: {}", placed.id, placed.error);
        }
    }
    for (const auto& id : ids) {
        reload(id);
    }
}

void Registry::unload_all() {
    for (auto it = mods_.rbegin(); it != mods_.rend(); ++it) {
        if (it->state != State::Loaded) {
            continue;
        }
        unload_one(*it);
    }
    settings::flush_now();
    content::revert_all();
    panels_.clear();
    ticks_.clear();
    remove_game_tick_hook();
    game_ticks_.clear();
    {
        std::scoped_lock lock(g_services_mutex);
        g_services.clear();
    }
    MH_Uninitialize();
}

}
