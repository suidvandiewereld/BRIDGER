#pragma once

#include "bridger/api.h"

#include <algorithm>
#include <cstdint>
#include <atomic>
#include <cstring>
#include <mutex>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#if __has_include("decima/prelude.h")
#include "decima/prelude.h"
#define BRIDGER_HAS_DECIMA 1
#endif

namespace bridger {

inline const BridgerApi* api = nullptr;
inline const char* mod_identity = "";

[[nodiscard]] inline const char* mod_id() { return mod_identity; }

bool on_load();
void on_unload();

inline void write(BridgerLogLevel level, std::string_view message) {
    if (api != nullptr) {
        api->log(level, std::string(message).c_str());
    }
}

template <typename... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    write(BRIDGER_LOG_TRACE, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(BRIDGER_LOG_INFO, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(BRIDGER_LOG_WARN, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(BRIDGER_LOG_ERROR, std::format(fmt, std::forward<Args>(args)...));
}

[[nodiscard]] inline std::uintptr_t image_base() {
    return api != nullptr ? api->image_base : 0;
}

template <typename T = void>
[[nodiscard]] T* resolve(std::uintptr_t rva) {
    return api != nullptr ? static_cast<T*>(api->resolve(rva)) : nullptr;
}

template <typename Signature>
[[nodiscard]] Signature at_rva(std::uintptr_t rva) {
    return reinterpret_cast<Signature>(resolve(rva));
}

template <typename Signature>
[[nodiscard]] Signature symbol(const char* group, const char* name) {
    if (api == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<Signature>(api->find_symbol(group, name));
}

[[nodiscard]] inline const void* type(const char* name) {
    return api != nullptr ? api->find_type(name) : nullptr;
}

[[nodiscard]] inline void* handler(const char* class_name, const char* message_name) {
    return api != nullptr ? api->find_handler(class_name, message_name) : nullptr;
}

template <typename Signature>
[[nodiscard]] Signature handler(const char* class_name, const char* message_name) {
    return reinterpret_cast<Signature>(handler(class_name, message_name));
}

[[nodiscard]] inline const void* rtti_of(const void* object) {
    return api != nullptr ? api->rtti_of(object) : nullptr;
}

[[nodiscard]] inline std::string_view rtti_name(const void* rtti) {
    const char* name = api != nullptr ? api->rtti_name(rtti) : nullptr;
    return name != nullptr ? std::string_view(name) : std::string_view();
}

[[nodiscard]] inline bool is_a(const void* rtti, const void* base) {
    return api != nullptr && api->rtti_is_a(rtti, base);
}

[[nodiscard]] inline bool is_a(const void* rtti, const char* base_name) {
    return is_a(rtti, type(base_name));
}

[[nodiscard]] inline bool object_is_a(const void* object, const char* type_name) {
    return is_a(rtti_of(object), type(type_name));
}

[[nodiscard]] inline std::uint32_t type_size(const void* rtti) {
    return api != nullptr ? api->type_size(rtti) : 0;
}

[[nodiscard]] inline std::uint32_t type_size(const char* name) {
    return type_size(type(name));
}

template <typename Signature = void*>
[[nodiscard]] Signature scan(const char* pattern) {
    return reinterpret_cast<Signature>(api != nullptr ? api->scan_pattern(pattern) : nullptr);
}

using Callback = void (*)();

namespace detail {
inline void callback_thunk(void* user) {
    reinterpret_cast<Callback>(user)();
}
}

inline void run_on_game_thread(Callback fn) {
    if (api != nullptr) {
        api->run_on_game_thread(detail::callback_thunk, reinterpret_cast<void*>(fn));
    }
}

inline bool hotkey(unsigned key, Callback fn, unsigned modifiers = BRIDGER_MOD_NONE) {
    return api != nullptr
           && api->register_hotkey(key, modifiers, detail::callback_thunk,
                                   reinterpret_cast<void*>(fn));
}

inline bool rebind(Callback fn, unsigned key, unsigned modifiers = BRIDGER_MOD_NONE) {
    return api != nullptr
           && api->rebind_hotkey(detail::callback_thunk, reinterpret_cast<void*>(fn), key,
                                 modifiers);
}

[[nodiscard]] inline bool overlay_visible() {
    return api != nullptr && api->overlay_visible();
}

[[nodiscard]] inline bool key_down(unsigned key) {
    return api != nullptr && api->key_down(key);
}

template <typename Table>
bool provide(const char* name, const Table* table) {
    return api != nullptr && api->provide(name, table);
}

template <typename Table>
[[nodiscard]] const Table* require(const char* name) {
    return api != nullptr ? static_cast<const Table*>(api->require(name)) : nullptr;
}

[[nodiscard]] inline bool mod_loaded(const char* id) {
    return api != nullptr && api->mod_loaded(id);
}

template <typename Signature>
class Hook {
public:
    Hook() = default;
    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;
    ~Hook() { remove(); }

    bool install(void* target, Signature detour) {
        if (api == nullptr || target == nullptr || installed_) {
            return false;
        }
        installed_ = api->install_hook(target, reinterpret_cast<void*>(detour),
                                       reinterpret_cast<void**>(&original_));
        if (installed_) {
            target_ = target;
        }
        return installed_;
    }

    bool install(std::uintptr_t rva, Signature detour) {
        return install(resolve(rva), detour);
    }

    bool install(const char* group, const char* name, Signature detour) {
        return install(api != nullptr ? api->find_symbol(group, name) : nullptr, detour);
    }

    bool install_handler(const char* class_name, const char* message_name, Signature detour) {
        return install(api != nullptr ? api->find_handler(class_name, message_name) : nullptr,
                       detour);
    }

    void remove() {
        if (installed_ && api != nullptr) {
            api->remove_hook(target_);
        }
        installed_ = false;
        target_ = nullptr;
        original_ = nullptr;
    }

    [[nodiscard]] bool installed() const { return installed_; }
    [[nodiscard]] Signature original() const { return original_; }

    template <typename... Args>
    decltype(auto) call(Args&&... args) const {
        return original_(std::forward<Args>(args)...);
    }

private:
    void* target_ = nullptr;
    Signature original_ = nullptr;
    bool installed_ = false;
};

template <typename Fn>
bool guarded(Fn&& body) {
    if (api == nullptr) {
        return false;
    }
    auto callable = std::forward<Fn>(body);
    const auto thunk = +[](void* user) { (*static_cast<decltype(callable)*>(user))(); };
    return api->guarded(thunk, &callable) == 0;
}

[[nodiscard]] inline bool is_stub(const void* fn) {
    if (fn == nullptr) {
        return true;
    }
    bool stub = false;
    guarded([&] {
        const auto* code = static_cast<const std::uint8_t*>(fn);
        stub = code[0] == 0xc3
            || (code[0] == 0xc2 && code[3] == 0xcc)
            || (code[0] == 0x32 && code[1] == 0xc0 && code[2] == 0xc3)
            || (code[0] == 0x33 && code[1] == 0xc0 && code[2] == 0xc3);
    });
    return stub;
}

namespace detail {

inline bool in_data_section(std::uintptr_t rva) {
    const auto base = image_base();
    if (base == 0 || rva == 0) {
        return false;
    }
    const auto read16 = [](std::uintptr_t at) { return *reinterpret_cast<const std::uint16_t*>(at); };
    const auto read32 = [](std::uintptr_t at) { return *reinterpret_cast<const std::uint32_t*>(at); };
    const auto nt = base + read32(base + 0x3c);
    if (read32(nt) != 0x00004550) {
        return false;
    }
    const auto count = read16(nt + 6);
    const auto sections = nt + 4 + 20 + read16(nt + 0x14);
    for (unsigned i = 0; i < count; ++i) {
        const auto entry = sections + i * 40;
        const auto address = read32(entry + 12);
        const auto size = read32(entry + 8);
        if (rva < address || rva >= address + size) {
            continue;
        }
        constexpr std::uint32_t executable = 0x20000000, initialised = 0x00000040;
        const auto flags = read32(entry + 36);
        return (flags & executable) == 0 && (flags & initialised) != 0;
    }
    return false;
}

}

[[nodiscard]] inline void* data_ref(const char* group, const char* name, int index = 0,
                                    std::size_t window = 128) {
    if (api == nullptr) {
        return nullptr;
    }
    const auto* start = static_cast<const std::uint8_t*>(api->find_symbol(group, name));
    if (start == nullptr) {
        return nullptr;
    }
    void* found = nullptr;
    guarded([&] {
        int seen = 0;
        for (std::size_t i = 0; i + 10 < window;) {
            if (start[i] >= 0x48 && start[i] <= 0x4f && start[i + 1] == 0x8b) {
                const auto modrm = start[i + 2];
                if ((modrm >> 6) == 0 && (modrm & 7) == 5) {
                    std::int32_t displacement = 0;
                    std::memcpy(&displacement, start + i + 3, sizeof displacement);
                    const auto* target = start + i + 7 + displacement;
                    const bool cookie = start[i + 7] == 0x48 && start[i + 8] == 0x33
                                     && start[i + 9] == 0xc4;
                    const auto rva = reinterpret_cast<std::uintptr_t>(target) - image_base();
                    if (!cookie && detail::in_data_section(rva) && seen++ == index) {
                        found = const_cast<std::uint8_t*>(target);
                        return;
                    }
                    i += 7;
                    continue;
                }
            }
            ++i;
        }
    });
    return found;
}

template <typename T = void>
[[nodiscard]] T* singleton(const char* group, const char* name, int index = 0) {
    auto* slot = static_cast<void**>(data_ref(group, name, index));
    return slot != nullptr ? static_cast<T*>(*slot) : nullptr;
}

namespace detail {

inline std::recursive_mutex& mod_mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

inline std::atomic<bool>& mod_faulted() {
    static std::atomic<bool> value{false};
    return value;
}

inline Callback& fault_handler() {
    static Callback handler = nullptr;
    return handler;
}

}

[[nodiscard]] inline bool faulted() {
    return detail::mod_faulted().load(std::memory_order_relaxed);
}

inline void clear_fault() {
    detail::mod_faulted().store(false, std::memory_order_relaxed);
}

inline void on_fault(Callback fn) { detail::fault_handler() = fn; }

template <typename Fn>
bool safely(Fn&& fn) {
    if (api == nullptr || faulted()) {
        return false;
    }
    std::lock_guard lock(detail::mod_mutex());
    if (guarded(std::forward<Fn>(fn))) {
        return true;
    }
    detail::mod_faulted().store(true, std::memory_order_relaxed);
    error("stopped after an engine access fault");
    if (const auto handler = detail::fault_handler(); handler != nullptr) {
        guarded([&] { handler(); });
    }
    if (api->version >= 8 && api->content != nullptr) {
        api->content->revert_all(mod_id());
    }
    if (api->version >= 9 && api->fx != nullptr && api->fx->revert_owned != nullptr) {
        api->fx->revert_owned();
    }
    return false;
}

template <typename Fn>
void on_game_thread(Fn&& fn) {
    if (api == nullptr || faulted()) {
        return;
    }
    using Held = std::decay_t<Fn>;
    auto* held = new Held(std::forward<Fn>(fn));
    api->run_on_game_thread(
        [](void* user) {
            auto* owned = static_cast<Held*>(user);
            safely(*owned);
            delete owned;
        },
        held);
}

using Tick = void (*)(float);

inline void tick(Tick fn) {
    if (api != nullptr) {
        api->register_tick(
            [](void* user, float dt) { reinterpret_cast<Tick>(user)(dt); },
            reinterpret_cast<void*>(fn));
    }
}

inline void game_tick(Tick fn) {
    if (api != nullptr) {
        api->register_game_tick(
            [](void* user, float dt) { reinterpret_cast<Tick>(user)(dt); },
            reinterpret_cast<void*>(fn));
    }
}

namespace config {

[[nodiscard]] inline bool has(std::string_view key) {
    return api != nullptr && api->settings_has(mod_id(), std::string(key).c_str());
}
[[nodiscard]] inline bool get(std::string_view key, bool fallback) {
    return api != nullptr ? api->settings_get_bool(mod_id(), std::string(key).c_str(), fallback)
                          : fallback;
}
[[nodiscard]] inline double get(std::string_view key, double fallback) {
    return api != nullptr ? api->settings_get_number(mod_id(), std::string(key).c_str(), fallback)
                          : fallback;
}
[[nodiscard]] inline std::string get(std::string_view key, std::string_view fallback) {
    if (api == nullptr) {
        return std::string(fallback);
    }
    char buffer[1024];
    api->settings_get_text(mod_id(), std::string(key).c_str(), std::string(fallback).c_str(),
                           buffer, sizeof buffer);
    return std::string(buffer);
}
inline void set(std::string_view key, bool value) {
    if (api != nullptr) {
        api->settings_set_bool(mod_id(), std::string(key).c_str(), value);
    }
}
inline void set(std::string_view key, double value) {
    if (api != nullptr) {
        api->settings_set_number(mod_id(), std::string(key).c_str(), value);
    }
}
inline void set(std::string_view key, std::string_view value) {
    if (api != nullptr) {
        api->settings_set_text(mod_id(), std::string(key).c_str(), std::string(value).c_str());
    }
}
inline void erase(std::string_view key) {
    if (api != nullptr) {
        api->settings_erase(mod_id(), std::string(key).c_str());
    }
}

}

template <typename T>
class Setting {
public:
    Setting(const char* key, const char* label, T fallback, const char* help = "")
        : key_(key), label_(label), help_(help), value_(fallback), fallback_(std::move(fallback)) {}

    [[nodiscard]] const T& get() const {
        sync();
        return value_;
    }
    operator const T&() const { return get(); }

    void set(const T& next) {
        sync();
        if (value_ == next) {
            return;
        }
        value_ = next;
        store();
    }

    [[nodiscard]] T* data() {
        sync();
        return &value_;
    }
    void commit() { store(); }

    [[nodiscard]] bool modified() const { return !(get() == fallback_); }
    void reset() { set(fallback_); }

    [[nodiscard]] const T& fallback() const { return fallback_; }
    [[nodiscard]] const char* key() const { return key_; }
    [[nodiscard]] const char* label() const { return label_; }
    [[nodiscard]] const char* help() const { return help_; }

private:
    void sync() const {
        if (loaded_ || api == nullptr) {
            return;
        }
        loaded_ = true;
        if constexpr (std::is_same_v<T, bool>) {
            value_ = api->settings_get_bool(mod_id(), key_, fallback_);
        } else if constexpr (std::is_same_v<T, std::string>) {
            char buffer[1024];
            api->settings_get_text(mod_id(), key_, fallback_.c_str(), buffer, sizeof buffer);
            value_ = buffer;
        } else {
            value_ = static_cast<T>(api->settings_get_number(
                mod_id(), key_, static_cast<double>(fallback_)));
        }
    }

    void store() const {
        if (api == nullptr) {
            return;
        }
        loaded_ = true;
        if constexpr (std::is_same_v<T, bool>) {
            api->settings_set_bool(mod_id(), key_, value_);
        } else if constexpr (std::is_same_v<T, std::string>) {
            api->settings_set_text(mod_id(), key_, value_.c_str());
        } else {
            api->settings_set_number(mod_id(), key_, static_cast<double>(value_));
        }
    }

    const char* key_;
    const char* label_;
    const char* help_;
    mutable T value_;
    T fallback_;
    mutable bool loaded_ = false;
};

namespace ui {

using Draw = void (*)();

inline void panel(const char* label, Draw draw) {
    if (api != nullptr) {
        api->register_panel(label, [](void* user) { reinterpret_cast<Draw>(user)(); },
                            reinterpret_cast<void*>(draw));
    }
}

inline void text(std::string_view value) {
    api->ui->text(std::string(value).c_str());
}

inline void text(std::uint32_t color, std::string_view value) {
    api->ui->text_colored(color, std::string(value).c_str());
}

template <typename... Args>
void textf(std::format_string<Args...> fmt, Args&&... args) {
    text(std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void textf(std::uint32_t color, std::format_string<Args...> fmt, Args&&... args) {
    text(color, std::format(fmt, std::forward<Args>(args)...));
}

inline void field(std::string_view label, std::string_view value) {
    api->ui->label_value(std::string(label).c_str(), std::string(value).c_str());
}

template <typename... Args>
void fieldf(std::string_view label, std::format_string<Args...> fmt, Args&&... args) {
    field(label, std::format(fmt, std::forward<Args>(args)...));
}

inline bool button(std::string_view label, float width = 0.0f) {
    return api->ui->button(std::string(label).c_str(), width);
}

inline bool checkbox(std::string_view label, bool& value) {
    if (api->ui->checkbox(std::string(label).c_str(), value)) {
        value = !value;
        return true;
    }
    return false;
}

inline bool selectable(std::string_view label, bool selected, float width = 0.0f) {
    return api->ui->selectable(std::string(label).c_str(), selected, width);
}

inline void separator() { api->ui->separator(); }
inline void spacing(float amount = 0.0f) { api->ui->spacing(amount); }
inline void same_line(float offset = 0.0f) { api->ui->same_line(offset); }
inline void progress(float fraction, float width, std::uint32_t color) {
    api->ui->progress(fraction, width, color);
}
inline void begin_scroll(const char* id, float height) { api->ui->begin_scroll(id, height); }
inline void end_scroll() { api->ui->end_scroll(); }

[[nodiscard]] inline std::uint32_t accent() { return api->ui->accent_color(); }
[[nodiscard]] inline std::uint32_t dim() { return api->ui->dim_color(); }

inline bool slider(std::string_view label, float& value, float min, float max, float width = 0.0f) {
    return api->ui->slider_float(std::string(label).c_str(), &value, min, max, width);
}
inline bool slider(std::string_view label, int& value, int min, int max, float width = 0.0f) {
    return api->ui->slider_int(std::string(label).c_str(), &value, min, max, width);
}
inline bool input_text(std::string_view id, std::string& value, std::string_view placeholder = {},
                       float width = 0.0f) {
    char buffer[256];
    const auto length = std::min<std::size_t>(value.size(), sizeof buffer - 1);
    value.copy(buffer, length);
    buffer[length] = 0;
    const bool changed = api->ui->input_text(std::string(id).c_str(), buffer, sizeof buffer,
                                             std::string(placeholder).c_str(), width);
    if (changed) {
        value.assign(buffer);
    }
    return changed;
}
inline void header(std::string_view label) { api->ui->header(std::string(label).c_str()); }
inline void indent(float amount = 16.0f) { api->ui->indent(amount); }
inline void unindent(float amount = 16.0f) { api->ui->unindent(amount); }
[[nodiscard]] inline float available_width() { return api->ui->available_width(); }

inline void begin_settings(float label_width = 0.0f) { api->ui->begin_settings(label_width); }
inline void end_settings() { api->ui->end_settings(); }

struct SettingsScope {
    explicit SettingsScope(float label_width = 0.0f) { begin_settings(label_width); }
    ~SettingsScope() { end_settings(); }
    SettingsScope(const SettingsScope&) = delete;
    SettingsScope& operator=(const SettingsScope&) = delete;
};

inline bool begin_group(std::string_view label, bool default_open = true) {
    return api->ui->begin_group(std::string(label).c_str(), default_open);
}
inline void end_group() { api->ui->end_group(); }

inline void readout(std::string_view label, std::string_view value) {
    api->ui->readout(std::string(label).c_str(), std::string(value).c_str());
}
template <typename... Args>
void readoutf(std::string_view label, std::format_string<Args...> fmt, Args&&... args) {
    readout(label, std::format(fmt, std::forward<Args>(args)...));
}
inline void readout(std::string_view label, std::uint32_t color, std::string_view value) {
    api->ui->readout_colored(std::string(label).c_str(), color, std::string(value).c_str());
}
template <typename... Args>
void readoutf(std::string_view label, std::uint32_t color, std::format_string<Args...> fmt,
              Args&&... args) {
    readout(label, color, std::format(fmt, std::forward<Args>(args)...));
}

inline void note(std::string_view value) { api->ui->note(std::string(value).c_str()); }
inline void badge(std::string_view label, std::uint32_t color) {
    api->ui->badge(std::string(label).c_str(), color);
}
inline void keycap(std::string_view label) { api->ui->keycap(std::string(label).c_str()); }
inline void tooltip(std::string_view value) { api->ui->tooltip(std::string(value).c_str()); }
inline void wrapped(std::uint32_t color, std::string_view value) {
    api->ui->text_wrapped(color, std::string(value).c_str());
}
inline void newline() { api->ui->newline(); }

inline void push_id(std::string_view value) { api->ui->push_id(std::string(value).c_str()); }
inline void pop_id() { api->ui->pop_id(); }

struct IdScope {
    explicit IdScope(std::string_view value) { push_id(value); }
    ~IdScope() { pop_id(); }
    IdScope(const IdScope&) = delete;
    IdScope& operator=(const IdScope&) = delete;
};
inline bool ghost_button(std::string_view label, float width = 0.0f) {
    return api->ui->ghost_button(std::string(label).c_str(), width);
}
[[nodiscard]] inline std::uint32_t color(BridgerColor role) { return api->ui->color(role); }
[[nodiscard]] inline std::uint32_t faint() { return color(BRIDGER_COLOR_FAINT); }
[[nodiscard]] inline std::uint32_t good() { return color(BRIDGER_COLOR_GOOD); }
[[nodiscard]] inline std::uint32_t warn() { return color(BRIDGER_COLOR_WARN); }
[[nodiscard]] inline std::uint32_t bad() { return color(BRIDGER_COLOR_BAD); }

inline bool setting(std::string_view label, bool& value, std::string_view help = {}) {
    return api->ui->setting_bool(std::string(label).c_str(), &value, std::string(help).c_str());
}
inline bool setting(std::string_view label, float& value, float min, float max,
                    std::string_view suffix = {}, std::string_view help = {}) {
    return api->ui->setting_float(std::string(label).c_str(), &value, min, max,
                                  std::string(suffix).c_str(), std::string(help).c_str());
}
inline bool setting(std::string_view label, int& value, int min, int max,
                    std::string_view suffix = {}, std::string_view help = {}) {
    return api->ui->setting_int(std::string(label).c_str(), &value, min, max,
                                std::string(suffix).c_str(), std::string(help).c_str());
}

inline bool row(Setting<bool>& value) {
    bool changed = api->ui->setting_bool(value.label(), value.data(), value.help());
    if (changed) {
        value.commit();
    }
    if (api->ui->revert_marker(value.modified())) {
        value.reset();
        changed = true;
    }
    return changed;
}

inline bool row(Setting<float>& value, float min, float max, std::string_view suffix = {}) {
    bool changed = api->ui->setting_float(value.label(), value.data(), min, max,
                                          std::string(suffix).c_str(), value.help());
    if (changed) {
        value.commit();
    }
    if (api->ui->revert_marker(value.modified())) {
        value.reset();
        changed = true;
    }
    return changed;
}

inline bool row(Setting<int>& value, int min, int max, std::string_view suffix = {}) {
    bool changed = api->ui->setting_int(value.label(), value.data(), min, max,
                                        std::string(suffix).c_str(), value.help());
    if (changed) {
        value.commit();
    }
    if (api->ui->revert_marker(value.modified())) {
        value.reset();
        changed = true;
    }
    return changed;
}

inline bool key_row(Setting<int>& value) {
    auto key = static_cast<std::uint32_t>(value.get());
    bool changed = api->ui->setting_key(value.label(), &key, value.help());
    if (changed) {
        value.set(static_cast<int>(key));
    }
    if (api->ui->revert_marker(value.modified())) {
        value.reset();
        changed = true;
    }
    return changed;
}

inline bool combo_row(Setting<int>& value, const char* const* items, int count) {
    bool changed = api->ui->setting_combo(value.label(), value.data(), items, count, value.help());
    if (changed) {
        value.commit();
    }
    if (api->ui->revert_marker(value.modified())) {
        value.reset();
        changed = true;
    }
    return changed;
}

inline bool text_row(Setting<std::string>& value, std::string_view placeholder = {}) {
    char buffer[512];
    const auto& current = value.get();
    const auto length = std::min(current.size(), sizeof buffer - 1);
    current.copy(buffer, length);
    buffer[length] = 0;
    bool changed = api->ui->setting_text(value.label(), buffer, sizeof buffer,
                                         std::string(placeholder).c_str(), value.help());
    if (changed) {
        value.set(std::string(buffer));
    }
    if (api->ui->revert_marker(value.modified())) {
        value.reset();
        changed = true;
    }
    return changed;
}

}

}

#include "bridger/fx.hpp"

#include "bridger/content.hpp"

#define BRIDGER_MOD(mod_id, mod_name, mod_version, mod_author, mod_description)                   \
    BRIDGER_MOD_EXPORT const BridgerModInfo* BridgerMod_Info() {                                  \
        static const BridgerModInfo info{BRIDGER_API_VERSION, mod_id, mod_name, mod_version,      \
                                         mod_author, mod_description};                            \
        return &info;                                                                             \
    }                                                                                             \
    BRIDGER_MOD_EXPORT bool BridgerMod_Load(const BridgerApi* loader) {                           \
        ::bridger::api = loader;                                                                  \
        ::bridger::mod_identity = mod_id;                                                         \
        BRIDGER_BIND_DECIMA();                                                                    \
        return ::bridger::on_load();                                                              \
    }                                                                                             \
    BRIDGER_MOD_EXPORT void BridgerMod_Unload() { ::bridger::on_unload(); }

#ifdef BRIDGER_HAS_DECIMA
#define BRIDGER_BIND_DECIMA() (::decima::image_base = loader->image_base)
#else
#define BRIDGER_BIND_DECIMA() ((void)0)
#endif
