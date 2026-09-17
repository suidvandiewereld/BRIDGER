#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "bridger/mod.hpp"

namespace bridger {

class Handle {
public:
    using Uuid = std::array<std::uint8_t, 16>;

    explicit Handle(std::string_view key, std::string_view type_name = {})
        : key_(key), type_(type_name) {}

    bool capture(const void* object) {
        Uuid uuid{};
        if (!uuid_of(object, uuid)) {
            return false;
        }
        remembered_ = uuid;
        has_uuid_ = true;
        pointer_ = const_cast<void*>(object);
        config::set(key_, to_text(uuid));
        return true;
    }

    bool offer(const void* object) {
        if (object == nullptr || pointer_ == object) {
            return pointer_ != nullptr && pointer_ == object;
        }
        if (!load()) {
            return false;
        }
        if (!type_.empty() && !object_is_a(object, type_.c_str())) {
            return false;
        }
        Uuid uuid{};
        if (!uuid_of(object, uuid) || uuid != remembered_) {
            return false;
        }
        pointer_ = const_cast<void*>(object);
        return true;
    }

    [[nodiscard]] void* get() {
        if (pointer_ == nullptr) {
            return nullptr;
        }
        Uuid uuid{};
        if (!uuid_of(pointer_, uuid) || !has_uuid_ || uuid != remembered_) {
            pointer_ = nullptr;
        }
        return pointer_;
    }

    template <typename T>
    [[nodiscard]] T* as() {
        return static_cast<T*>(get());
    }

    [[nodiscard]] bool bound() { return get() != nullptr; }

    [[nodiscard]] bool remembered() {
        return load();
    }

    [[nodiscard]] std::string text() {
        return load() ? to_text(remembered_) : std::string{};
    }

    [[nodiscard]] std::string_view type() const { return type_; }

    void forget() {
        pointer_ = nullptr;
        has_uuid_ = false;
        remembered_ = {};
        loaded_ = true;
        config::erase(key_);
    }

    void unbind() { pointer_ = nullptr; }

private:
    static bool uuid_of(const void* object, Uuid& out) {
        if (object == nullptr || rtti_of(object) == nullptr) {
            return false;
        }
        Uuid uuid{};
        if (!guarded([&] {
                const auto* bytes = static_cast<const std::uint8_t*>(object) + 8;
                for (std::size_t i = 0; i < uuid.size(); ++i) {
                    uuid[i] = bytes[i];
                }
            })) {
            return false;
        }
        if (uuid == Uuid{}) {
            return false;
        }
        out = uuid;
        return true;
    }

    static std::string to_text(const Uuid& uuid) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string text;
        text.reserve(uuid.size() * 2);
        for (const auto byte : uuid) {
            text.push_back(digits[byte >> 4]);
            text.push_back(digits[byte & 0xf]);
        }
        return text;
    }

    static bool from_text(const std::string& text, Uuid& out) {
        if (text.size() != out.size() * 2) {
            return false;
        }
        for (std::size_t i = 0; i < out.size(); ++i) {
            int value = 0;
            for (int half = 0; half < 2; ++half) {
                const unsigned char c = text[i * 2 + half];
                const int digit = (c >= '0' && c <= '9') ? c - '0'
                                : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                : -1;
                if (digit < 0) {
                    return false;
                }
                value = value * 16 + digit;
            }
            out[i] = static_cast<std::uint8_t>(value);
        }
        return true;
    }

    bool load() {
        if (!loaded_) {
            loaded_ = true;
            has_uuid_ = from_text(config::get(key_, std::string_view{}), remembered_);
        }
        return has_uuid_;
    }

    std::string key_;
    std::string type_;
    void* pointer_ = nullptr;
    Uuid remembered_{};
    bool has_uuid_ = false;
    bool loaded_ = false;
};

}
