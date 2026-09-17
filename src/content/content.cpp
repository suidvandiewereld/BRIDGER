#include "content/content.h"

#include <Windows.h>
#include <intrin.h>

#include <MinHook.h>

#include <algorithm>
#include <malloc.h>
#include <unordered_map>
#include <utility>

#include "content/content_internal.h"
#include "core/log.h"
#include "decima/dumper.h"
#include "loader/registry.h"

namespace bridger::content {

State& state() {
    static State instance;
    return instance;
}

const void* class_rtti(const char* name) {
    const void* rtti = name != nullptr && *name != 0 ? decima::find_type(name) : nullptr;
    if (rtti == nullptr || !in_image(rtti) || read_at<std::uint8_t>(rtti, kKind) != 4) {
        return nullptr;
    }
    return rtti;
}

bool write_protected(void* address, const void* bytes, std::size_t size) {
    if (address == nullptr || bytes == nullptr || size == 0) {
        return false;
    }
    DWORD previous = 0;
    if (VirtualProtect(address, size, PAGE_READWRITE, &previous) == 0) {
        log::warn("content: {} byte(s) at {} are not writable", size, address);
        return false;
    }
    std::memcpy(address, bytes, size);
    DWORD restored = 0;
    VirtualProtect(address, size, previous, &restored);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return true;
}

namespace {

std::uint32_t size_of_type(const std::string& name) {
    const auto* table = loader::api();
    if (table == nullptr || name.empty()) {
        return 0;
    }
    return table->type_size(decima::find_type(name.c_str()));
}

struct FieldSet {
    bool known = false;
    std::vector<BridgerField> fields;
    std::vector<std::unique_ptr<std::string>> storage;
};

const char* intern(FieldSet& set, const std::string& text) {
    set.storage.push_back(std::make_unique<std::string>(text));
    return set.storage.back()->c_str();
}

void collect_fields(const std::string& type_name, std::uint32_t base_offset, int depth,
                    FieldSet& set) {
    if (depth > 8) {
        return;
    }
    const void* rtti = decima::find_type(type_name.c_str());
    if (rtti == nullptr) {
        return;
    }
    decima::TypeDetail detail;
    if (!decima::describe_type(reinterpret_cast<std::uintptr_t>(rtti), detail)) {
        return;
    }
    for (const auto& base : detail.bases) {
        collect_fields(base.name, base_offset + base.offset, depth + 1, set);
    }
    for (const auto& member : detail.members) {
        BridgerField field{};
        field.name = intern(set, member.name);
        field.type = intern(set, member.type);
        field.offset = base_offset + member.offset;
        field.size = size_of_type(member.type);
        field.getter = reinterpret_cast<const void*>(member.getter);
        field.setter = reinterpret_cast<const void*>(member.setter);
        set.fields.push_back(field);
    }
}

std::mutex g_fields_mutex;
std::unordered_map<std::string, FieldSet> g_fields;

std::size_t read_code(const void* address, std::uint8_t* buffer, std::size_t wanted) {
    for (std::size_t size = wanted; size >= 32; size /= 2) {
        if (safe_read(address, buffer, size)) {
            return size;
        }
    }
    return 0;
}

const void* rtti_from_vtable(void** vtable) {
    void* getter = nullptr;
    if (!safe_read(vtable, &getter, sizeof getter) || !in_code(getter)) {
        return nullptr;
    }
    std::uint8_t code[8];
    if (!safe_read(getter, code, sizeof code)) {
        return nullptr;
    }
    if (code[0] != 0x48 || code[1] != 0x8d || code[2] != 0x05 || code[7] != 0xc3) {
        return nullptr;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, code + 3, sizeof displacement);
    return static_cast<const std::uint8_t*>(getter) + 7 + displacement;
}

void** vtable_from_constructor(const void* constructor, const void* rtti) {
    if (!in_code(constructor)) {
        return nullptr;
    }
    std::uint8_t code[288];
    std::size_t length = read_code(constructor, code, sizeof code);
    if (length == 0) {
        return nullptr;
    }
    const auto* base = static_cast<const std::uint8_t*>(constructor);

    for (std::size_t i = 0; i + 5 <= (std::min)(length, std::size_t{24}); ++i) {
        if (code[i] != 0xe9) {
            continue;
        }
        std::int32_t displacement = 0;
        std::memcpy(&displacement, code + i + 1, sizeof displacement);
        const auto* target = base + i + 5 + displacement;
        if (!in_code(target)) {
            break;
        }
        length = read_code(target, code, sizeof code);
        base = target;
        break;
    }
    if (length < 11) {
        return nullptr;
    }

    void** candidate = nullptr;
    void** weak = nullptr;
    for (std::size_t i = 0; i + 11 <= length; ++i) {
        if (code[i] == 0x48 && code[i + 1] == 0x8d && code[i + 2] == 0x05) {
            std::int32_t displacement = 0;
            std::memcpy(&displacement, code + i + 3, sizeof displacement);
            auto* loaded = const_cast<std::uint8_t*>(base + i + 7 + displacement);
            candidate = in_image_data(loaded) ? reinterpret_cast<void**>(loaded) : nullptr;
            continue;
        }
        if (candidate == nullptr || code[i] != 0x48 || code[i + 1] != 0x89) {
            continue;
        }
        const auto modrm = code[i + 2];
        if ((modrm >> 6) != 0 || ((modrm >> 3) & 7) != 0 || (modrm & 7) == 4 || (modrm & 7) == 5) {
            continue;
        }
        if (rtti_from_vtable(candidate) == rtti) {
            return candidate;
        }
        if (weak == nullptr) {
            void* slot0 = nullptr;
            if (safe_read(candidate, &slot0, sizeof slot0) && in_code(slot0)) {
                weak = candidate;
            }
        }
    }
    return weak;
}

}

const std::vector<BridgerField>* fields_of(const char* type_name) {
    if (type_name == nullptr || *type_name == 0) {
        return nullptr;
    }
    std::scoped_lock lock(g_fields_mutex);
    const std::string key(type_name);
    if (const auto found = g_fields.find(key); found != g_fields.end()) {
        return found->second.known ? &found->second.fields : nullptr;
    }
    FieldSet set;
    set.known = decima::find_type(key.c_str()) != nullptr;
    if (set.known) {
        collect_fields(key, 0, 0, set);
    }
    auto& stored = g_fields.emplace(key, std::move(set)).first->second;
    return stored.known ? &stored.fields : nullptr;
}

namespace {

template <std::size_t N>
void handler_thunk(void* self, void* message) {
    dispatch_handler(N, self, message);
}

template <std::size_t... I>
constexpr std::array<EngineHandler, sizeof...(I)> make_thunks(std::index_sequence<I...>) {
    return {&handler_thunk<I>...};
}

constexpr auto kThunks = make_thunks(std::make_index_sequence<kMaxHandlerSlots>());

}

EngineHandler thunk_for(std::size_t slot) {
    return slot < kMaxHandlerSlots ? kThunks[slot] : nullptr;
}

namespace {

template <std::size_t N>
std::uint64_t forced_stub() {
    return state().forced[N];
}

template <std::size_t... I>
constexpr std::array<std::uint64_t (*)(), sizeof...(I)> make_forced(std::index_sequence<I...>) {
    return {&forced_stub<I>...};
}

constexpr auto kForcedStubs = make_forced(std::make_index_sequence<kMaxForcedSlots>());

}

void* forced_stub_for(std::size_t slot) {
    return slot < kMaxForcedSlots ? reinterpret_cast<void*>(kForcedStubs[slot]) : nullptr;
}

void* handler_for_rtti(const void* rtti, const void* message_rtti) {
    if (rtti == nullptr || message_rtti == nullptr || !in_image(rtti)
        || read_at<std::uint8_t>(rtti, kKind) != 4) {
        return nullptr;
    }
    const auto count = read_at<std::uint8_t>(rtti, kClassHandlerCount);
    const auto* table = read_at<const std::uint8_t*>(rtti, kClassHandlers);
    for (std::uint8_t i = 0; table != nullptr && i < count; ++i) {
        if (read_at<const void*>(table, i * kHandlerStride) == message_rtti) {
            return read_at<void*>(table, i * kHandlerStride + 8);
        }
    }
    const auto bases = read_at<std::uint8_t>(rtti, 5);
    const auto* base_table = read_at<const std::uint8_t*>(rtti, 88);
    for (std::uint8_t i = 0; base_table != nullptr && i < bases; ++i) {
        const void* base = read_at<const void*>(base_table, i * 16);
        if (auto* found = handler_for_rtti(base, message_rtti); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

void dispatch_handler(std::size_t slot, void* self, void* message) {
    if (slot >= kMaxHandlerSlots) {
        return;
    }
    auto& entry = state().thunks[slot];
    const auto fn = entry.fn.load(std::memory_order_acquire);
    const auto original = entry.original;
    if (fn == nullptr) {
        if (original != nullptr) {
            original(self, message);
        }
        return;
    }
    switch (entry.order) {
        case BRIDGER_CONTENT_BEFORE:
            fn(self, message, entry.user);
            if (original != nullptr) {
                original(self, message);
            }
            break;
        case BRIDGER_CONTENT_AFTER:
            if (original != nullptr) {
                original(self, message);
            }
            fn(self, message, entry.user);
            break;
        case BRIDGER_CONTENT_REPLACE:
        default:
            fn(self, message, entry.user);
            break;
    }
}

bool entry_intact(const Entry& entry) {
    switch (entry.kind) {
        case Kind::Object:
            return entry.memory != nullptr;

        case Kind::Patch: {
            if (entry.address == nullptr || entry.written.empty()) {
                return false;
            }
            std::vector<std::uint8_t> now(entry.written.size());
            return safe_read(entry.address, now.data(), now.size())
                && std::memcmp(now.data(), entry.written.data(), now.size()) == 0;
        }

        case Kind::Injection: {
            if (entry.count_slot == nullptr || entry.data_slot == nullptr) {
                return false;
            }
            std::int32_t count = 0;
            void** data = nullptr;
            if (!safe_read(entry.count_slot, &count, sizeof count)
                || !safe_read(entry.data_slot, &data, sizeof data)) {
                return false;
            }
            return data == entry.table.data()
                && count == entry.engine_count + static_cast<std::int32_t>(entry.items.size());
        }

        case Kind::Handler:
            return entry.target != nullptr;

        case Kind::Function:
            return entry.target != nullptr;

        case Kind::Method: {
            if (entry.instance != nullptr) {
                void** now = nullptr;
                return safe_read(entry.instance, &now, sizeof now)
                    && now == const_cast<void**>(entry.cloned_vtable.data()) + 1;
            }
            if (entry.vtable_slot == nullptr) {
                return false;
            }
            void* now = nullptr;
            return safe_read(entry.vtable_slot, &now, sizeof now) && now == entry.written_method;
        }
    }
    return false;
}

namespace {

void rebuild_table(Entry& entry) {
    entry.table.clear();
    entry.table.reserve(static_cast<std::size_t>(entry.engine_count) + entry.items.size());
    for (std::int32_t i = 0; i < entry.engine_count; ++i) {
        void* element = nullptr;
        safe_read(entry.engine_data + i, &element, sizeof element);
        entry.table.push_back(element);
    }
    entry.table.insert(entry.table.end(), entry.items.begin(), entry.items.end());
}

void publish_injection(Entry& entry) {
    const auto total = entry.engine_count + static_cast<std::int32_t>(entry.items.size());
    *entry.data_slot = entry.table.data();
    _ReadWriteBarrier();
    *entry.count_slot = total;
}

void withdraw_injection(Entry& entry) {
    *entry.count_slot = entry.engine_count;
    _ReadWriteBarrier();
    *entry.data_slot = entry.engine_data;
}

struct InjectionWork {
    Entry* entry = nullptr;
    bool ok = false;
};

}

bool reapply_entry(Entry& entry) {
    if (entry_intact(entry)) {
        return false;
    }
    switch (entry.kind) {
        case Kind::Patch:
            return entry.address != nullptr
                && write_protected(entry.address, entry.written.data(), entry.written.size());

        case Kind::Injection: {
            if (entry.count_slot == nullptr || entry.data_slot == nullptr) {
                return false;
            }
            InjectionWork work{&entry, false};
            guarded_call(
                [](void* user) {
                    auto& w = *static_cast<InjectionWork*>(user);
                    Entry& e = *w.entry;
                    const auto count = *e.count_slot;
                    auto* data = *e.data_slot;
                    if (count < 0 || count > 65536 || (count > 0 && data == nullptr)) {
                        return;
                    }
                    e.engine_count = count;
                    e.engine_data = data;
                    rebuild_table(e);
                    publish_injection(e);
                    w.ok = true;
                },
                &work);
            return work.ok;
        }


        case Kind::Method:
            if (entry.instance != nullptr || entry.vtable_slot == nullptr) {
                return false;
            }
            return write_protected(entry.vtable_slot, &entry.written_method,
                                   sizeof entry.written_method);

        case Kind::Handler:
        case Kind::Function:
        case Kind::Object:
        default:
            return false;
    }
}

bool revert_entry(Entry& entry, bool& was_intact) {
    was_intact = entry_intact(entry);
    switch (entry.kind) {
        case Kind::Object: {
            if (entry.memory == nullptr) {
                return true;
            }
            const auto destructor = entry.rtti != nullptr
                                        ? read_at<void*>(entry.rtti, kClassDestructor)
                                        : nullptr;
            if (in_code(destructor)) {
                struct Work {
                    void* dtor;
                    const void* rtti;
                    void* object;
                } work{destructor, entry.rtti, entry.memory};
                guarded_call(
                    [](void* user) {
                        auto& w = *static_cast<Work*>(user);
                        reinterpret_cast<void (*)(const void*, void*)>(w.dtor)(w.rtti, w.object);
                    },
                    &work);
            }
            _aligned_free(entry.memory);
            entry.memory = nullptr;
            return true;
        }

        case Kind::Patch: {
            if (entry.address == nullptr) {
                return true;
            }
            if (!was_intact) {
                log::warn("content: the patch at {} was overwritten; leaving it alone",
                          entry.address);
                entry.address = nullptr;
                return false;
            }
            const bool ok =
                write_protected(entry.address, entry.original.data(), entry.original.size());
            entry.address = nullptr;
            return ok;
        }

        case Kind::Injection: {
            if (entry.count_slot == nullptr) {
                return true;
            }
            bool ok = true;
            if (!was_intact) {
                log::warn("content: the engine rebuilt the array behind {}; leaving it alone",
                          entry.detail);
                ok = false;
            } else {
                InjectionWork work{&entry, false};
                guarded_call(
                    [](void* user) {
                        auto& w = *static_cast<InjectionWork*>(user);
                        withdraw_injection(*w.entry);
                        w.ok = true;
                    },
                    &work);
                ok = work.ok;
            }
            entry.count_slot = nullptr;
            entry.data_slot = nullptr;
            entry.table.clear();
            return ok;
        }

        case Kind::Handler: {
            if (entry.target == nullptr) {
                return true;
            }
            MH_DisableHook(entry.target);
            const bool ok = MH_RemoveHook(entry.target) == MH_OK;
            if (entry.slot < kMaxHandlerSlots) {
                auto& thunk = state().thunks[entry.slot];
                thunk.fn.store(nullptr, std::memory_order_release);
                thunk.user = nullptr;
                thunk.original = nullptr;
            }
            entry.target = nullptr;
            entry.trampoline = nullptr;
            return ok;
        }

        case Kind::Method: {
            if (entry.instance != nullptr) {
                bool ok = was_intact;
                if (was_intact) {
                    struct Work {
                        void* object;
                        void** vtable;
                    } work{entry.instance, entry.instance_old_vtable};
                    guarded_call(
                        [](void* user) {
                            auto& w = *static_cast<Work*>(user);
                            *static_cast<void***>(w.object) = w.vtable;
                        },
                        &work);
                } else {
                    log::warn("content: {} no longer uses the mod's vtable; leaving it alone",
                              entry.detail);
                }
                entry.instance = nullptr;
                entry.cloned_vtable.clear();
                return ok;
            }
            if (entry.vtable_slot == nullptr) {
                return true;
            }
            if (!was_intact) {
                log::warn("content: {} now holds something else; leaving it alone", entry.detail);
                entry.vtable_slot = nullptr;
                return false;
            }
            const bool ok = write_protected(entry.vtable_slot, &entry.previous_method,
                                            sizeof entry.previous_method);
            entry.vtable_slot = nullptr;
            return ok;
        }
    }
    return false;
}

void* allocate_object(const void* rtti, std::size_t& bytes_out) {
    const auto size = read_at<std::uint32_t>(rtti, kClassSize);
    auto alignment = read_at<std::uint16_t>(rtti, kClassAlignment);
    if (size == 0 || size > (16u << 20)) {
        return nullptr;
    }
    if (alignment < 16) {
        alignment = 16;
    }
    void* memory = _aligned_malloc(size, alignment);
    if (memory == nullptr) {
        return nullptr;
    }
    std::memset(memory, 0, size);
    bytes_out = size;
    return memory;
}

bool run_constructor(const void* rtti, void* memory) {
    const auto constructor = read_at<void*>(rtti, kClassConstructor);
    if (!in_code(constructor)) {
        return false;
    }
    struct Work {
        void* ctor;
        const void* rtti;
        void* memory;
        bool ok;
    } work{constructor, rtti, memory, false};
    guarded_call(
        [](void* user) {
            auto& w = *static_cast<Work*>(user);
            reinterpret_cast<void (*)(const void*, void*)>(w.ctor)(w.rtti, w.memory);
            w.ok = true;
        },
        &work);
    return work.ok;
}

void** vtable_of_class(const char* class_name) {
    const void* rtti = class_rtti(class_name);
    if (rtti == nullptr) {
        return nullptr;
    }
    return vtable_from_constructor(read_at<void*>(rtti, kClassConstructor), rtti);
}

std::size_t vtable_length(void** vtable) {
    if (vtable == nullptr) {
        return 0;
    }
    std::size_t length = 0;
    for (std::size_t i = 0; i < 512; ++i) {
        void* slot = nullptr;
        if (!safe_read(vtable + i, &slot, sizeof slot) || !in_code(slot)) {
            break;
        }
        ++length;
    }
    return length;
}

void tick() {
    auto& s = state();
    if (!s.any_persistent) {
        return;
    }
    std::scoped_lock lock(s.mutex);
    for (auto& entry : s.entries) {
        if (entry.kind != Kind::Injection || !entry.persistent || entry_intact(entry)) {
            continue;
        }
        if (reapply_entry(entry)) {
            log::info("content: re-applied {} after the engine rebuilt the array", entry.detail);
        }
    }
}

void destroy_owned(std::string_view owner) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    std::size_t reverted = 0;
    for (auto it = s.entries.rbegin(); it != s.entries.rend(); ++it) {
        if (it->owner != owner) {
            continue;
        }
        bool intact = false;
        revert_entry(*it, intact);
        ++reverted;
    }
    std::erase_if(s.entries, [&](const Entry& e) { return e.owner == owner; });
    s.any_persistent = std::any_of(s.entries.begin(), s.entries.end(), [](const Entry& e) {
        return e.kind == Kind::Injection && e.persistent;
    });
    if (reverted > 0) {
        log::info("content: reverted {} entr{} owned by {}", reverted, reverted == 1 ? "y" : "ies",
                  owner);
    }
}

void revert_all() {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    for (auto it = s.entries.rbegin(); it != s.entries.rend(); ++it) {
        bool intact = false;
        revert_entry(*it, intact);
    }
    s.entries.clear();
    s.any_persistent = false;
}

}
