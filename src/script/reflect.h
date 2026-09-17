#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bridger::script::reflect {

enum class Codec : std::uint8_t {
    Unknown,
    Bool,
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    I64,
    U64,
    F32,
    F64,
    Enum,
    String,
    WString,
    Uuid,
    Pointer,
    Handle,
    Array,
    Struct,
};

struct Layout;

struct Type {
    Codec codec = Codec::Unknown;
    std::uint32_t size = 0;
    std::string name;
    const void* rtti = nullptr;
    const Type* item = nullptr;
    bool owning = false;

    [[nodiscard]] bool number() const {
        return codec >= Codec::I8 && codec <= Codec::F64;
    }
};

struct Field {
    std::string name;
    std::string type_name;
    std::uint32_t offset = 0;
    const Type* type = nullptr;
    std::uintptr_t getter = 0;
    std::uintptr_t setter = 0;
    std::string category;
};

struct Layout {
    const void* rtti = nullptr;
    std::string name;
    std::uint32_t size = 0;
    std::vector<Field> fields;
    std::unordered_map<std::string, std::size_t> by_name;
    std::vector<std::string> lineage;
    std::vector<std::string> messages;

    [[nodiscard]] const Field* find(std::string_view field) const;
    [[nodiscard]] std::string suggest(std::string_view field) const;
};

const Type* type(std::string_view name);

const Layout* layout(std::string_view class_name);
const Layout* layout(const void* class_rtti);

const void* rtti_of(std::uintptr_t address);
const char* rtti_name(const void* rtti);
bool is_a(const void* rtti, const void* base);
const void* find(std::string_view name);

bool enum_value(const void* rtti, std::string_view name, std::int64_t& out);
std::string enum_name(const void* rtti, std::int64_t value);
std::vector<std::pair<std::string, std::int64_t>> enum_values(const void* rtti);

bool read(std::uintptr_t address, void* out, std::size_t bytes);
bool write(std::uintptr_t address, const void* bytes, std::size_t size);
std::string read_c_string(std::uintptr_t address, std::size_t limit = 4096);
std::string read_wide_string(std::uintptr_t address, std::size_t limit = 4096);

bool read_integer(std::uintptr_t address, Codec codec, std::uint32_t size, std::int64_t& out);
bool write_integer(std::uintptr_t address, Codec codec, std::uint32_t size, std::int64_t value);

std::string format_uuid(const std::uint8_t bytes[16]);
bool parse_uuid(std::string_view text, std::uint8_t bytes[16]);

std::uint32_t size_for_ffi(std::string_view name, bool& is_enum);

}
