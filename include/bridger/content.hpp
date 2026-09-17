#pragma once

#include "bridger/api.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#ifndef BRIDGER_MOD_EXPORT
#error "include bridger/mod.hpp, which pulls this header in after the loader API"
#endif

namespace bridger::content {

namespace detail {

[[nodiscard]] inline const BridgerContent* table() {
    return api != nullptr && api->version >= 8 ? api->content : nullptr;
}

}

[[nodiscard]] inline bool available() {
    return detail::table() != nullptr;
}

[[nodiscard]] inline int offset_of(const char* type_name, const char* field_name) {
    const auto* content = detail::table();
    return content != nullptr ? content->field_offset(type_name, field_name) : -1;
}

[[nodiscard]] inline bool describe(const char* type_name, const char* field_name,
                                   BridgerField& out) {
    const auto* content = detail::table();
    return content != nullptr && content->field(type_name, field_name, &out);
}

[[nodiscard]] inline std::vector<BridgerField> fields(const char* type_name) {
    std::vector<BridgerField> out;
    const auto* content = detail::table();
    if (content == nullptr) {
        return out;
    }
    const auto count = content->field_count(type_name);
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        BridgerField field{};
        if (content->field_at(type_name, i, &field)) {
            out.push_back(field);
        }
    }
    return out;
}

template <typename T>
class Field {
public:
    constexpr Field(const char* type_name, const char* field_name)
        : type_(type_name), name_(field_name) {}

    [[nodiscard]] bool valid() const { return resolve() >= 0; }
    [[nodiscard]] const char* type_name() const { return type_; }
    [[nodiscard]] const char* name() const { return name_; }
    [[nodiscard]] int offset() const { return resolve(); }

    [[nodiscard]] T* at(void* object) const {
        const auto where = resolve();
        if (object == nullptr || where < 0) {
            return nullptr;
        }
        return reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + where);
    }

    [[nodiscard]] const T* at(const void* object) const {
        return at(const_cast<void*>(object));
    }

    [[nodiscard]] T get(const void* object, T fallback = T{}) const {
        const auto* where = at(object);
        return where != nullptr ? *where : fallback;
    }

    bool set(void* object, const T& value) const {
        auto* where = at(object);
        if (where == nullptr) {
            return false;
        }
        *where = value;
        return true;
    }

private:
    int resolve() const {
        if (offset_ == kUnresolved) {
            offset_ = offset_of(type_, name_);
        }
        return offset_;
    }

    static constexpr int kUnresolved = -2;
    const char* type_;
    const char* name_;
    mutable int offset_ = kUnresolved;
};

class Object {
public:
    Object() = default;
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    Object(Object&& other) noexcept : memory_(std::exchange(other.memory_, nullptr)) {}
    Object& operator=(Object&& other) noexcept {
        if (this != &other) {
            reset();
            memory_ = std::exchange(other.memory_, nullptr);
        }
        return *this;
    }
    ~Object() { reset(); }

    [[nodiscard]] static Object create(const char* type_name) {
        const auto* content = detail::table();
        return Object(content != nullptr ? content->create(type_name) : nullptr);
    }

    [[nodiscard]] static Object clone(const void* source) {
        const auto* content = detail::table();
        return Object(content != nullptr ? content->clone(source) : nullptr);
    }

    void reset() {
        if (memory_ != nullptr) {
            if (const auto* content = detail::table(); content != nullptr) {
                content->destroy(memory_);
            }
            memory_ = nullptr;
        }
    }

    [[nodiscard]] void* get() const { return memory_; }
    template <typename T>
    [[nodiscard]] T* as() const {
        return static_cast<T*>(memory_);
    }
    explicit operator bool() const { return memory_ != nullptr; }

private:
    explicit Object(void* memory) : memory_(memory) {}
    void* memory_ = nullptr;
};

namespace detail {

class Owned {
public:
    Owned() = default;
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    Owned(Owned&& other) noexcept : handle_(std::exchange(other.handle_, 0)) {}
    Owned& operator=(Owned&& other) noexcept {
        if (this != &other) {
            revert();
            handle_ = std::exchange(other.handle_, 0);
        }
        return *this;
    }
    ~Owned() { revert(); }

    void revert() {
        if (handle_ != 0) {
            if (const auto* content = table(); content != nullptr) {
                content->revert(handle_);
            }
            handle_ = 0;
        }
    }

    [[nodiscard]] bool intact() const {
        const auto* content = table();
        return handle_ != 0 && content != nullptr && content->intact(handle_);
    }

    bool reapply() {
        const auto* content = table();
        return handle_ != 0 && content != nullptr && content->reapply(handle_);
    }

    [[nodiscard]] bool installed() const { return handle_ != 0; }
    [[nodiscard]] BridgerContentHandle handle() const { return handle_; }
    explicit operator bool() const { return handle_ != 0; }

protected:
    bool adopt(BridgerContentHandle handle) {
        revert();
        handle_ = handle;
        return handle_ != 0;
    }

private:
    BridgerContentHandle handle_ = 0;
};

}

class Patch : public detail::Owned {
public:
    bool write(void* address, const void* bytes, std::size_t size) {
        const auto* content = detail::table();
        return content != nullptr && adopt(content->patch(address, bytes, size));
    }

    template <typename T>
    bool write(T* address, const T& value) {
        return write(address, &value, sizeof value);
    }
};

class Injection : public detail::Owned {
public:
    bool add(std::int32_t* count, void*** data, void* const* items, std::size_t item_count,
             bool persistent = true) {
        const auto* content = detail::table();
        if (content == nullptr || item_count == 0) {
            return false;
        }
        return adopt(content->inject(count, data, items,
                                     static_cast<std::uint32_t>(item_count), persistent));
    }

    bool add(std::int32_t* count, void*** data, std::initializer_list<void*> items,
             bool persistent = true) {
        return add(count, data, items.begin(), items.size(), persistent);
    }

    bool add(void* owner, std::size_t count_offset, std::size_t data_offset,
             std::initializer_list<void*> items, bool persistent = true) {
        if (owner == nullptr) {
            return false;
        }
        auto* bytes = static_cast<std::uint8_t*>(owner);
        return add(reinterpret_cast<std::int32_t*>(bytes + count_offset),
                   reinterpret_cast<void***>(bytes + data_offset), items, persistent);
    }
};

using Handler = void (*)(void* self, void* message);

class Behaviour : public detail::Owned {
public:
    bool replace(const char* class_name, const char* message, Handler fn) {
        return install(class_name, message, fn, BRIDGER_CONTENT_REPLACE);
    }
    bool before(const char* class_name, const char* message, Handler fn) {
        return install(class_name, message, fn, BRIDGER_CONTENT_BEFORE);
    }
    bool after(const char* class_name, const char* message, Handler fn) {
        return install(class_name, message, fn, BRIDGER_CONTENT_AFTER);
    }

    [[nodiscard]] Handler original() const {
        const auto* content = detail::table();
        if (content == nullptr || !installed()) {
            return nullptr;
        }
        return reinterpret_cast<Handler>(content->original_handler(handle()));
    }

    void call_original(void* self, void* message) const {
        if (const auto fn = original(); fn != nullptr) {
            fn(self, message);
        }
    }

private:
    bool install(const char* class_name, const char* message, Handler fn,
                 BridgerContentOrder order) {
        const auto* content = detail::table();
        if (content == nullptr || fn == nullptr) {
            return false;
        }
        const auto thunk = +[](void* self, void* message, void* user) {
            reinterpret_cast<Handler>(user)(self, message);
        };
        return adopt(content->override_handler(class_name, message, thunk,
                                               reinterpret_cast<void*>(fn), order));
    }
};

[[nodiscard]] inline Handler handler_for(const void* receiver, const char* message) {
    const auto* content = detail::table();
    if (content == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<Handler>(content->handler_for(receiver, message));
}

inline bool deliver(void* receiver, void* message) {
    const auto* content = detail::table();
    return content != nullptr && content->deliver(receiver, message);
}

template <typename Signature>
class Function : public detail::Owned {
public:
    bool replace(void* target, Signature detour) {
        const auto* content = detail::table();
        if (content == nullptr || target == nullptr || detour == nullptr) {
            return false;
        }
        void* trampoline = nullptr;
        const bool ok = adopt(content->replace_function(target, reinterpret_cast<void*>(detour),
                                                        &trampoline));
        original_ = reinterpret_cast<Signature>(trampoline);
        return ok;
    }

    [[nodiscard]] Signature original() const { return original_; }

    template <typename... Args>
    decltype(auto) call(Args&&... args) const {
        return original_(std::forward<Args>(args)...);
    }

private:
    Signature original_ = nullptr;
};

class Gate : public detail::Owned {
public:
    bool force(void* target, std::uint64_t result) {
        const auto* content = detail::table();
        return content != nullptr && adopt(content->force_result(target, result));
    }
    bool force(void* target, bool result) {
        return force(target, static_cast<std::uint64_t>(result ? 1 : 0));
    }
};

[[nodiscard]] inline void** vtable(const char* class_name) {
    const auto* content = detail::table();
    return content != nullptr ? content->vtable(class_name) : nullptr;
}

[[nodiscard]] inline void** vtable_of(const void* object) {
    const auto* content = detail::table();
    return content != nullptr ? content->vtable_of(object) : nullptr;
}

template <typename Signature>
class Method : public detail::Owned {
public:
    bool replace(const char* class_name, std::uint32_t slot, Signature fn) {
        return replace_shared(vtable(class_name), slot, fn);
    }

    bool replace_shared(void** table, std::uint32_t slot, Signature fn) {
        const auto* content = detail::table();
        if (content == nullptr || table == nullptr) {
            return false;
        }
        void* previous = nullptr;
        const bool ok = adopt(content->override_method(table, slot,
                                                       reinterpret_cast<void*>(fn), &previous));
        original_ = reinterpret_cast<Signature>(previous);
        return ok;
    }

    bool replace(void* object, std::uint32_t slot, Signature fn) {
        const auto* content = detail::table();
        if (content == nullptr) {
            return false;
        }
        void* previous = nullptr;
        const bool ok = adopt(content->override_instance(object, slot,
                                                         reinterpret_cast<void*>(fn), &previous));
        original_ = reinterpret_cast<Signature>(previous);
        return ok;
    }

    [[nodiscard]] Signature original() const { return original_; }

    template <typename... Args>
    decltype(auto) call(Args&&... args) const {
        return original_(std::forward<Args>(args)...);
    }

private:
    Signature original_ = nullptr;
};

template <typename T>
class State {
public:
    using Uuid = std::array<std::uint8_t, 16>;

    struct Entry {
        Uuid id{};
        void* pointer = nullptr;
        T value{};
    };

    [[nodiscard]] T& of(const void* object) {
        Uuid id{};
        read_uuid(object, id);
        for (auto& entry : entries_) {
            if (entry.id == id) {
                entry.pointer = const_cast<void*>(object);
                return entry.value;
            }
        }
        entries_.push_back({id, const_cast<void*>(object), T{}});
        return entries_.back().value;
    }

    [[nodiscard]] T* find(const void* object) {
        Uuid id{};
        read_uuid(object, id);
        for (auto& entry : entries_) {
            if (entry.id == id) {
                return &entry.value;
            }
        }
        return nullptr;
    }

    std::size_t reap() {
        const auto before = entries_.size();
        std::erase_if(entries_, [](const Entry& entry) {
            Uuid now{};
            return !read_uuid(entry.pointer, now) || now != entry.id;
        });
        return before - entries_.size();
    }

    void forget(const void* object) {
        Uuid id{};
        read_uuid(object, id);
        std::erase_if(entries_, [&](const Entry& entry) { return entry.id == id; });
    }

    void clear() { entries_.clear(); }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

    [[nodiscard]] auto begin() { return entries_.begin(); }
    [[nodiscard]] auto end() { return entries_.end(); }

private:
    static bool read_uuid(const void* object, Uuid& out) {
        out = {};
        if (object == nullptr || !copy_uuid(object, out)) {
            out = {};
            return false;
        }
        return out != Uuid{};
    }

    static bool copy_uuid(const void* object, Uuid& out) noexcept {
        __try {
            std::memcpy(out.data(), static_cast<const std::uint8_t*>(object) + 8, out.size());
            return true;
        } __except (1) {
            return false;
        }
    }

    std::vector<Entry> entries_;
};

inline void revert_all() {
    if (const auto* content = detail::table(); content != nullptr) {
        content->revert_all(mod_identity);
    }
}

[[nodiscard]] inline std::uint32_t count() {
    const auto* content = detail::table();
    return content != nullptr ? content->count(mod_identity) : 0;
}

}
