#include <Windows.h>
#include <intrin.h>

#include <MinHook.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <string>

#include "content/content.h"
#include "content/content_internal.h"
#include "core/log.h"
#include "decima/dumper.h"
#include "loader/registry.h"

namespace bridger::content {
namespace {

std::string caller(const void* return_address) {
    auto owner = loader::owner_of_address(return_address);
    if (owner.empty()) {
        owner = loader::Registry::instance().active_mod();
    }
    return owner.empty() ? std::string("core") : owner;
}

#define BRIDGER_CALLER caller(_ReturnAddress())

Entry* find(BridgerContentHandle handle) {
    auto& s = state();
    const auto found = std::find_if(s.entries.begin(), s.entries.end(),
                                    [&](const Entry& e) { return e.handle == handle; });
    return found == s.entries.end() ? nullptr : &*found;
}

BridgerContentHandle publish(Entry&& entry) {
    auto& s = state();
    entry.handle = s.next_handle++;
    const auto handle = entry.handle;
    s.entries.push_back(std::move(entry));
    return handle;
}

const BridgerField* lookup(const char* type_name, const char* field_name) {
    if (field_name == nullptr) {
        return nullptr;
    }
    const auto* fields = fields_of(type_name);
    if (fields == nullptr) {
        return nullptr;
    }
    for (const auto& field : *fields) {
        if (std::strcmp(field.name, field_name) == 0) {
            return &field;
        }
    }
    return nullptr;
}

int api_field_offset(const char* type_name, const char* field_name) {
    const auto* field = lookup(type_name, field_name);
    if (field == nullptr || field->getter != nullptr) {
        return -1;
    }
    return static_cast<int>(field->offset);
}

bool api_field(const char* type_name, const char* field_name, BridgerField* out) {
    const auto* field = lookup(type_name, field_name);
    if (field == nullptr || out == nullptr) {
        return false;
    }
    *out = *field;
    return true;
}

std::uint32_t api_field_count(const char* type_name) {
    const auto* fields = fields_of(type_name);
    return fields != nullptr ? static_cast<std::uint32_t>(fields->size()) : 0;
}

bool api_field_at(const char* type_name, std::uint32_t index, BridgerField* out) {
    const auto* fields = fields_of(type_name);
    if (fields == nullptr || out == nullptr || index >= fields->size()) {
        return false;
    }
    *out = (*fields)[index];
    return true;
}

bool api_object_field(const void* object, const char* field_name, BridgerField* out) {
    const auto* table = loader::api();
    if (table == nullptr || object == nullptr) {
        return false;
    }
    const void* rtti = table->rtti_of(object);
    const char* name = rtti != nullptr ? table->rtti_name(rtti) : nullptr;
    return name != nullptr && api_field(name, field_name, out);
}

std::uint32_t api_type_alignment(const char* type_name) {
    const void* rtti = class_rtti(type_name);
    return rtti != nullptr ? read_at<std::uint16_t>(rtti, kClassAlignment) : 0;
}

void* api_create(const char* type_name) {
    const void* rtti = class_rtti(type_name);
    if (rtti == nullptr) {
        log::warn("content: no class named {}", type_name != nullptr ? type_name : "(null)");
        return nullptr;
    }
    std::size_t bytes = 0;
    void* memory = allocate_object(rtti, bytes);
    if (memory == nullptr) {
        log::warn("content: {} has no usable size", type_name);
        return nullptr;
    }
    if (!run_constructor(rtti, memory)) {
        _aligned_free(memory);
        log::warn("content: {} has no constructor that ran", type_name);
        return nullptr;
    }
    Entry entry;
    entry.kind = Kind::Object;
    entry.owner = BRIDGER_CALLER;
    entry.detail = type_name;
    entry.memory = memory;
    entry.bytes = bytes;
    entry.rtti = rtti;

    auto& s = state();
    std::scoped_lock lock(s.mutex);
    publish(std::move(entry));
    return memory;
}

void* api_clone(const void* source) {
    const auto* table = loader::api();
    if (table == nullptr || source == nullptr) {
        return nullptr;
    }
    const void* rtti = table->rtti_of(source);
    if (rtti == nullptr || read_at<std::uint8_t>(rtti, kKind) != 4) {
        log::warn("content: {} does not carry class RTTI", source);
        return nullptr;
    }
    std::size_t bytes = 0;
    void* memory = allocate_object(rtti, bytes);
    if (memory == nullptr) {
        return nullptr;
    }
    if (!safe_read(source, memory, bytes)) {
        _aligned_free(memory);
        log::warn("content: {} is not readable for its whole {} bytes", source, bytes);
        return nullptr;
    }
    const char* name = table->rtti_name(rtti);

    Entry entry;
    entry.kind = Kind::Object;
    entry.owner = BRIDGER_CALLER;
    entry.detail = std::format("{} (clone of {})", name != nullptr ? name : "?", source);
    entry.memory = memory;
    entry.bytes = bytes;
    entry.rtti = nullptr;

    auto& s = state();
    std::scoped_lock lock(s.mutex);
    publish(std::move(entry));
    return memory;
}

bool api_destroy(void* object) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const auto found = std::find_if(s.entries.begin(), s.entries.end(), [&](const Entry& e) {
        return e.kind == Kind::Object && e.memory == object;
    });
    if (found == s.entries.end()) {
        return false;
    }
    bool intact = false;
    revert_entry(*found, intact);
    s.entries.erase(found);
    return true;
}

bool api_owns(const void* object) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    return std::any_of(s.entries.begin(), s.entries.end(), [&](const Entry& e) {
        return e.kind == Kind::Object && e.memory == object;
    });
}

BridgerContentHandle api_patch(void* address, const void* bytes, std::size_t size) {
    if (address == nullptr || bytes == nullptr || size == 0 || size > (1u << 20)) {
        return 0;
    }
    Entry entry;
    entry.kind = Kind::Patch;
    entry.owner = BRIDGER_CALLER;
    entry.detail = std::format("{} bytes at {}", size, address);
    entry.address = address;
    entry.original.resize(size);
    if (!safe_read(address, entry.original.data(), size)) {
        log::warn("content: {} is not readable, so it cannot be patched revertibly", address);
        return 0;
    }
    entry.written.assign(static_cast<const std::uint8_t*>(bytes),
                         static_cast<const std::uint8_t*>(bytes) + size);
    if (!write_protected(address, entry.written.data(), size)) {
        return 0;
    }
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    return publish(std::move(entry));
}

BridgerContentHandle api_inject(std::int32_t* count, void*** data, void* const* items,
                                std::uint32_t item_count, bool persistent) {
    if (count == nullptr || data == nullptr || items == nullptr || item_count == 0
        || item_count > 4096) {
        return 0;
    }
    std::int32_t engine_count = 0;
    void** engine_data = nullptr;
    if (!safe_read(count, &engine_count, sizeof engine_count)
        || !safe_read(data, &engine_data, sizeof engine_data)) {
        log::warn("content: the array at {} is not readable", static_cast<void*>(count));
        return 0;
    }
    if (engine_count < 0 || engine_count > 65536 || (engine_count > 0 && engine_data == nullptr)) {
        log::warn("content: the array at {} holds {} elements, which is not a count",
                  static_cast<void*>(count), engine_count);
        return 0;
    }

    Entry entry;
    entry.kind = Kind::Injection;
    entry.owner = BRIDGER_CALLER;
    entry.detail = std::format("{} item(s) into the array at {}", item_count,
                               static_cast<void*>(count));
    entry.count_slot = count;
    entry.data_slot = data;
    entry.engine_count = engine_count;
    entry.engine_data = engine_data;
    entry.items.assign(items, items + item_count);
    entry.persistent = persistent;

    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const auto handle = publish(std::move(entry));
    if (!reapply_entry(*find(handle))) {
        s.entries.pop_back();
        log::warn("content: injecting into the array at {} faulted", static_cast<void*>(count));
        return 0;
    }
    if (persistent) {
        s.any_persistent = true;
        if (!loader::ensure_game_tick()) {
            log::warn("content: no game tick, so the injection will not re-apply on scene load");
        }
    }
    return handle;
}

std::size_t free_slot() {
    auto& s = state();
    for (std::size_t i = 0; i < kMaxHandlerSlots; ++i) {
        if (s.thunks[i].fn.load(std::memory_order_relaxed) != nullptr) {
            continue;
        }
        const bool taken = std::any_of(s.entries.begin(), s.entries.end(), [&](const Entry& e) {
            return e.kind == Kind::Handler && e.slot == i;
        });
        if (!taken) {
            return i;
        }
    }
    return kMaxHandlerSlots;
}

BridgerContentHandle api_override_handler(const char* class_name, const char* message_name,
                                          BridgerMessageHandler fn, void* user,
                                          BridgerContentOrder order) {
    if (fn == nullptr) {
        return 0;
    }
    auto* rtti = const_cast<void*>(class_rtti(class_name));
    const void* message = message_name != nullptr ? decima::find_type(message_name) : nullptr;
    if (rtti == nullptr || message == nullptr) {
        log::warn("content: no {} or no {}", class_name != nullptr ? class_name : "(null)",
                  message_name != nullptr ? message_name : "(null)");
        return 0;
    }

    auto* target = handler_for_rtti(rtti, message);
    if (target == nullptr) {
        log::error("content: {}::{} has no handler to replace. Adding a message a class never "
                   "handled needs the engine's registration-time dispatch structure, which this "
                   "layer does not build.",
                   class_name, message_name);
        return 0;
    }

    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const auto slot = free_slot();
    if (slot == kMaxHandlerSlots) {
        log::error("content: all {} handler overrides are in use", kMaxHandlerSlots);
        return 0;
    }

    void* trampoline = nullptr;
    const auto created = MH_CreateHook(target, reinterpret_cast<void*>(thunk_for(slot)),
                                       &trampoline);
    if (created != MH_OK) {
        log::warn("content: could not take over {}::{} at {}: {}", class_name, message_name,
                  target, MH_StatusToString(created));
        return 0;
    }
    auto& pool = s.thunks[slot];
    pool.user = user;
    pool.original = reinterpret_cast<EngineHandler>(trampoline);
    pool.order = order;
    pool.fn.store(fn, std::memory_order_release);

    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        pool.fn.store(nullptr, std::memory_order_release);
        pool.user = nullptr;
        pool.original = nullptr;
        log::warn("content: could not enable the override on {}::{}", class_name, message_name);
        return 0;
    }

    Entry entry;
    entry.kind = Kind::Handler;
    entry.owner = BRIDGER_CALLER;
    entry.detail = std::format("{}::{}", class_name, message_name);
    entry.slot = slot;
    entry.rtti_class = rtti;
    entry.target = target;
    entry.trampoline = trampoline;
    entry.previous_handler = trampoline;
    return publish(std::move(entry));
}

void* api_original_handler(BridgerContentHandle handle) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const auto* entry = find(handle);
    return entry != nullptr && entry->kind == Kind::Handler ? entry->previous_handler : nullptr;
}

void* api_handler_for(const void* receiver, const char* message_name) {
    const auto* loader_api = loader::api();
    if (loader_api == nullptr || receiver == nullptr || message_name == nullptr) {
        return nullptr;
    }
    const void* message = decima::find_type(message_name);
    return message != nullptr ? handler_for_rtti(loader_api->rtti_of(receiver), message) : nullptr;
}

bool api_deliver(void* receiver, void* message) {
    const auto* loader_api = loader::api();
    if (loader_api == nullptr || receiver == nullptr || message == nullptr) {
        return false;
    }
    const void* message_rtti = loader_api->rtti_of(message);
    auto* handler = handler_for_rtti(loader_api->rtti_of(receiver), message_rtti);
    if (handler == nullptr) {
        return false;
    }
    struct Work {
        void* handler;
        void* receiver;
        void* message;
        bool ok;
    } work{handler, receiver, message, false};
    guarded_call(
        [](void* user) {
            auto& w = *static_cast<Work*>(user);
            reinterpret_cast<EngineHandler>(w.handler)(w.receiver, w.message);
            w.ok = true;
        },
        &work);
    return work.ok;
}

BridgerContentHandle install_function(void* target, void* detour, void** original,
                                      std::size_t forced, std::string detail,
                                      std::string owner) {
    void* trampoline = nullptr;
    const auto created = MH_CreateHook(target, detour, &trampoline);
    if (created != MH_OK) {
        log::warn("content: could not replace {}: {}", target, MH_StatusToString(created));
        return 0;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        log::warn("content: could not enable the replacement at {}", target);
        return 0;
    }
    Entry entry;
    entry.kind = Kind::Function;
    entry.owner = std::move(owner);
    entry.detail = std::move(detail);
    entry.target = target;
    entry.trampoline = trampoline;
    entry.forced = forced;
    if (original != nullptr) {
        *original = trampoline;
    }
    return publish(std::move(entry));
}

BridgerContentHandle api_replace_function(void* target, void* detour, void** original) {
    if (target == nullptr || detour == nullptr || !in_code(target)) {
        return 0;
    }
    const auto owner = BRIDGER_CALLER;
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    return install_function(target, detour, original, kMaxForcedSlots,
                            std::format("the function at {}", target), owner);
}

BridgerContentHandle api_force_result(void* target, std::uint64_t result) {
    if (target == nullptr || !in_code(target)) {
        return 0;
    }
    const auto owner = BRIDGER_CALLER;
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    std::size_t slot = kMaxForcedSlots;
    for (std::size_t i = 0; i < kMaxForcedSlots; ++i) {
        if (!s.forced_taken[i]) {
            slot = i;
            break;
        }
    }
    if (slot == kMaxForcedSlots) {
        log::error("content: all {} forced results are in use", kMaxForcedSlots);
        return 0;
    }
    s.forced[slot] = result;
    s.forced_taken[slot] = true;
    const auto handle = install_function(target, forced_stub_for(slot), nullptr, slot,
                                         std::format("{} always returns {}", target, result),
                                         owner);
    if (handle == 0) {
        s.forced_taken[slot] = false;
        s.forced[slot] = 0;
    }
    return handle;
}

void** api_vtable(const char* class_name) {
    return vtable_of_class(class_name);
}

void** api_vtable_of(const void* object) {
    if (object == nullptr) {
        return nullptr;
    }
    void** table = nullptr;
    if (!safe_read(object, &table, sizeof table)) {
        return nullptr;
    }
    return table;
}

BridgerContentHandle api_override_method(void** vtable, std::uint32_t slot, void* fn,
                                         void** original) {
    if (vtable == nullptr || fn == nullptr || slot >= 512) {
        return 0;
    }
    void* previous = nullptr;
    if (!safe_read(vtable + slot, &previous, sizeof previous)) {
        log::warn("content: vtable slot {} at {} is not readable", slot,
                  static_cast<void*>(vtable));
        return 0;
    }
    Entry entry;
    entry.kind = Kind::Method;
    entry.owner = BRIDGER_CALLER;
    entry.detail = std::format("slot {} of the vtable at {}", slot, static_cast<void*>(vtable));
    entry.vtable_slot = vtable + slot;
    entry.previous_method = previous;
    entry.written_method = fn;
    if (!write_protected(entry.vtable_slot, &fn, sizeof fn)) {
        return 0;
    }
    if (original != nullptr) {
        *original = previous;
    }
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    return publish(std::move(entry));
}

BridgerContentHandle api_override_instance(void* object, std::uint32_t slot, void* fn,
                                           void** original) {
    if (object == nullptr || fn == nullptr || slot >= 512) {
        return 0;
    }
    void** current = api_vtable_of(object);
    if (current == nullptr) {
        return 0;
    }

    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const auto owner = BRIDGER_CALLER;

    for (auto& existing : s.entries) {
        if (existing.kind != Kind::Method || existing.instance != object
            || existing.cloned_vtable.empty() || existing.owner != owner) {
            continue;
        }
        auto* clone = existing.cloned_vtable.data() + 1;
        if (slot + 1 >= existing.cloned_vtable.size()) {
            return 0;
        }
        Entry entry;
        entry.kind = Kind::Method;
        entry.owner = owner;
        entry.detail = std::format("slot {} of {}", slot, object);
        entry.vtable_slot = clone + slot;
        entry.previous_method = clone[slot];
        entry.written_method = fn;
        clone[slot] = fn;
        if (original != nullptr) {
            *original = entry.previous_method;
        }
        return publish(std::move(entry));
    }

    const auto length = vtable_length(current);
    if (length == 0 || slot >= length) {
        log::warn("content: the vtable at {} has {} slot(s), so {} is out of range",
                  static_cast<void*>(current), length, slot);
        return 0;
    }

    Entry entry;
    entry.kind = Kind::Method;
    entry.owner = owner;
    entry.detail = std::format("slot {} of {}", slot, object);
    entry.cloned_vtable.assign(length + 1, nullptr);
    safe_read(current - 1, entry.cloned_vtable.data(), sizeof(void*));
    if (!safe_read(current, entry.cloned_vtable.data() + 1, length * sizeof(void*))) {
        return 0;
    }
    entry.previous_method = entry.cloned_vtable[1 + slot];
    entry.written_method = fn;
    entry.cloned_vtable[1 + slot] = fn;
    entry.instance = object;
    entry.instance_old_vtable = current;
    entry.vtable_slot = entry.cloned_vtable.data() + 1 + slot;

    auto* published = entry.cloned_vtable.data() + 1;
    struct Work {
        void* object;
        void** vtable;
        bool ok;
    } work{object, published, false};
    guarded_call(
        [](void* user) {
            auto& w = *static_cast<Work*>(user);
            *static_cast<void***>(w.object) = w.vtable;
            w.ok = true;
        },
        &work);
    if (!work.ok) {
        return 0;
    }
    if (original != nullptr) {
        *original = entry.previous_method;
    }
    return publish(std::move(entry));
}

bool api_revert(BridgerContentHandle handle) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    auto* entry = find(handle);
    if (entry == nullptr) {
        return false;
    }
    bool intact = false;
    const bool ok = revert_entry(*entry, intact);
    std::erase_if(s.entries, [&](const Entry& e) { return e.handle == handle; });
    s.any_persistent = std::any_of(s.entries.begin(), s.entries.end(), [](const Entry& e) {
        return e.kind == Kind::Injection && e.persistent;
    });
    return ok;
}

bool api_intact(BridgerContentHandle handle) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const auto* entry = find(handle);
    return entry != nullptr && entry_intact(*entry);
}

bool api_reapply(BridgerContentHandle handle) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    auto* entry = find(handle);
    return entry != nullptr && reapply_entry(*entry);
}

void api_revert_all(const char* mod_id) {
    if (mod_id == nullptr || *mod_id == 0) {
        revert_all();
        return;
    }
    destroy_owned(mod_id);
}

std::uint32_t api_count(const char* mod_id) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    if (mod_id == nullptr || *mod_id == 0) {
        return static_cast<std::uint32_t>(s.entries.size());
    }
    return static_cast<std::uint32_t>(
        std::count_if(s.entries.begin(), s.entries.end(),
                      [&](const Entry& e) { return e.owner == mod_id; }));
}

bool api_entry_at(std::uint32_t index, BridgerContentEntry* out) {
    if (out == nullptr) {
        return false;
    }
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    if (index >= s.entries.size()) {
        return false;
    }
    const auto& entry = s.entries[index];
    static thread_local std::string owner;
    static thread_local std::string detail;
    owner = entry.owner;
    detail = entry.detail;
    void* address = nullptr;
    switch (entry.kind) {
        case Kind::Object: address = entry.memory; break;
        case Kind::Patch: address = entry.address; break;
        case Kind::Injection: address = entry.count_slot; break;
        case Kind::Handler: address = entry.target; break;
        case Kind::Method: address = entry.vtable_slot; break;
        case Kind::Function: address = entry.target; break;
    }
    out->handle = entry.handle;
    out->kind = static_cast<BridgerContentKind>(static_cast<int>(entry.kind));
    out->owner = owner.c_str();
    out->detail = detail.c_str();
    out->address = reinterpret_cast<std::uint64_t>(address);
    out->intact = entry_intact(entry);
    return true;
}

#undef BRIDGER_CALLER

const BridgerContent g_content{
    api_field_offset,
    api_field,
    api_field_count,
    api_field_at,
    api_object_field,
    api_type_alignment,
    api_create,
    api_clone,
    api_destroy,
    api_owns,
    api_patch,
    api_inject,
    api_override_handler,
    api_original_handler,
    api_handler_for,
    api_deliver,
    api_replace_function,
    api_force_result,
    api_vtable,
    api_vtable_of,
    api_override_method,
    api_override_instance,
    api_revert,
    api_intact,
    api_reapply,
    api_revert_all,
    api_count,
    api_entry_at,
};

}

const BridgerContent* api() {
    return &g_content;
}

}
