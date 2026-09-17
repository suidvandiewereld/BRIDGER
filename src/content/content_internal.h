#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "bridger/api.h"
#include "core/guard.h"
#include "core/memory.h"

namespace bridger::content {

inline const mem::Module& game_module() {
    static const mem::Module module = mem::module_of();
    return module;
}

inline bool in_image(const void* pointer) {
    const auto& module = game_module();
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return module.valid() && address >= module.base && address < module.base + module.size;
}

inline bool in_code(const void* pointer) {
    const auto& module = game_module();
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return module.valid() && address >= module.text_begin && address < module.text_end;
}

inline bool in_image_data(const void* pointer) {
    return in_image(pointer) && !in_code(pointer);
}

template <typename T>
T read_at(const void* base, std::size_t offset) {
    T value{};
    safe_read(static_cast<const std::uint8_t*>(base) + offset, &value, sizeof value);
    return value;
}

inline constexpr std::size_t kKind = 4;
inline constexpr std::size_t kClassHandlerCount = 8;
inline constexpr std::size_t kClassSize = 16;
inline constexpr std::size_t kClassAlignment = 20;
inline constexpr std::size_t kClassConstructor = 24;
inline constexpr std::size_t kClassDestructor = 32;
inline constexpr std::size_t kClassName = 56;
inline constexpr std::size_t kClassHandlers = 112;
inline constexpr std::size_t kHandlerStride = 16;

const void* class_rtti(const char* name);

enum class Kind {
    Object,
    Patch,
    Injection,
    Handler,
    Method,
    Function,
};

inline constexpr std::size_t kMaxHandlerSlots = 256;

inline constexpr std::size_t kMaxForcedSlots = 64;

using EngineHandler = void (*)(void* self, void* message);

struct ThunkSlot {
    std::atomic<BridgerMessageHandler> fn{nullptr};
    void* user = nullptr;
    EngineHandler original = nullptr;
    BridgerContentOrder order = BRIDGER_CONTENT_REPLACE;
};

struct Entry {
    BridgerContentHandle handle = 0;
    Kind kind = Kind::Object;
    std::string owner;
    std::string detail;

    void* memory = nullptr;
    std::size_t bytes = 0;
    const void* rtti = nullptr;

    void* address = nullptr;
    std::vector<std::uint8_t> original;
    std::vector<std::uint8_t> written;

    std::int32_t* count_slot = nullptr;
    void*** data_slot = nullptr;
    std::vector<void*> table;
    std::vector<void*> items;
    std::int32_t engine_count = 0;
    void** engine_data = nullptr;
    bool persistent = false;

    std::size_t slot = kMaxHandlerSlots;
    void* previous_handler = nullptr;
    void* rtti_class = nullptr;

    void* target = nullptr;
    void* trampoline = nullptr;
    std::size_t forced = kMaxForcedSlots;

    void** vtable_slot = nullptr;
    void* previous_method = nullptr;
    void* written_method = nullptr;
    std::vector<void*> cloned_vtable;
    void* instance = nullptr;
    void** instance_old_vtable = nullptr;
};

struct State {
    std::recursive_mutex mutex;
    std::vector<Entry> entries;
    BridgerContentHandle next_handle = 1;
    std::array<ThunkSlot, kMaxHandlerSlots> thunks{};
    std::array<std::uint64_t, kMaxForcedSlots> forced{};
    std::array<bool, kMaxForcedSlots> forced_taken{};
    bool any_persistent = false;
};

State& state();

bool write_protected(void* address, const void* bytes, std::size_t size);

const std::vector<BridgerField>* fields_of(const char* type_name);

void dispatch_handler(std::size_t slot, void* self, void* message);
EngineHandler thunk_for(std::size_t slot);
void* forced_stub_for(std::size_t slot);

void* handler_for_rtti(const void* class_rtti, const void* message_rtti);

bool entry_intact(const Entry& entry);
bool reapply_entry(Entry& entry);
bool revert_entry(Entry& entry, bool& was_intact);

void* allocate_object(const void* rtti, std::size_t& bytes_out);
bool run_constructor(const void* rtti, void* memory);

void** vtable_of_class(const char* class_name);
std::size_t vtable_length(void** vtable);

}
