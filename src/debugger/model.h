#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "decima/dumper.h"

namespace bridger::debugger::model {

[[nodiscard]] bool read_bytes(std::uintptr_t address, void* destination, std::size_t bytes);

template <typename T>
[[nodiscard]] bool read(std::uintptr_t address, T& out) {
    return read_bytes(address, &out, sizeof(T));
}

[[nodiscard]] std::string read_string(std::uintptr_t address, std::size_t limit);

[[nodiscard]] bool parse_address(const std::string& text, std::uintptr_t& out);

[[nodiscard]] const void* rtti_at(std::uintptr_t address);
[[nodiscard]] std::string type_name_at(std::uintptr_t address);

[[nodiscard]] bool describe(const std::string& name, decima::TypeDetail& out);
[[nodiscard]] std::string enum_value_name(const std::string& type, std::int64_t value);

[[nodiscard]] std::string format_value(const std::string& type, std::uintptr_t at,
                                       std::uintptr_t& follow);

struct Field {
    std::uint32_t offset = 0;
    std::string name;
    std::string type;
    std::string category;
    std::uintptr_t getter = 0;

    [[nodiscard]] bool property() const { return getter != 0; }
};

[[nodiscard]] std::vector<Field> fields_of(const std::string& type_name);

enum class Shape {
    Value,
    Pointer,
    Struct,
    Array,
};

[[nodiscard]] Shape shape_of(const std::string& type);
[[nodiscard]] std::string element_of(const std::string& type);
[[nodiscard]] std::uint32_t size_of(const std::string& type);

}
