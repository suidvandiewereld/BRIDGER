#include "debugger/model.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <map>
#include <memory>
#include <mutex>

#include "core/process.h"
#include "loader/registry.h"

namespace bridger::debugger::model {
namespace {

const proc::AddressSpace& space() {
    static const proc::AddressSpace instance = [] {
        proc::AddressSpace value;
        value.refresh();
        return value;
    }();
    return instance;
}

bool starts_with(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::mutex g_cache_mutex;
std::map<std::string, std::shared_ptr<const decima::TypeDetail>, std::less<>> g_details;
std::map<std::string, std::shared_ptr<const std::vector<Field>>, std::less<>> g_fields;

void collect(const std::string& type_name, std::uint32_t base_offset, int depth,
             std::vector<Field>& out) {
    if (depth > 12) {
        return;
    }
    decima::TypeDetail detail;
    if (!describe(type_name, detail)) {
        return;
    }
    for (const auto& base : detail.bases) {
        collect(base.name, base_offset + base.offset, depth + 1, out);
    }
    for (const auto& member : detail.members) {
        Field field;
        field.offset = base_offset + member.offset;
        field.name = member.name;
        field.type = member.type;
        field.category = member.category;
        field.getter = member.getter;
        out.push_back(std::move(field));
    }
}

}

bool read_bytes(std::uintptr_t address, void* destination, std::size_t bytes) {
    return space().read(address, destination, bytes);
}

std::string read_string(std::uintptr_t address, std::size_t limit) {
    return space().read_string(address, limit);
}

bool parse_address(const std::string& text, std::uintptr_t& out) {
    auto trimmed = text;
    std::erase_if(trimmed, [](unsigned char c) { return std::isspace(c) != 0 || c == '`'; });
    if (trimmed.rfind("0x", 0) == 0 || trimmed.rfind("0X", 0) == 0) {
        trimmed.erase(0, 2);
    }
    if (trimmed.empty() || trimmed.size() > 16) {
        return false;
    }
    std::uintptr_t value = 0;
    for (const unsigned char c : trimmed) {
        const int digit = std::isdigit(c) ? c - '0'
                        : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                        : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                        : -1;
        if (digit < 0) {
            return false;
        }
        value = value * 16 + static_cast<std::uintptr_t>(digit);
    }
    out = value;
    return true;
}

const void* rtti_at(std::uintptr_t address) {
    const auto* api = loader::api();
    if (api == nullptr || address < 0x10000 || address >= 0x7fffffffffffull || (address & 7) != 0) {
        return nullptr;
    }
    return api->rtti_of(reinterpret_cast<const void*>(address));
}

std::string type_name_at(std::uintptr_t address) {
    const auto* rtti = rtti_at(address);
    if (rtti == nullptr) {
        return {};
    }
    const char* name = loader::api()->rtti_name(rtti);
    return name != nullptr ? std::string(name) : std::string{};
}

bool describe(const std::string& name, decima::TypeDetail& out) {
    {
        std::scoped_lock lock(g_cache_mutex);
        if (const auto found = g_details.find(name); found != g_details.end()) {
            if (found->second == nullptr) {
                return false;
            }
            out = *found->second;
            return true;
        }
    }
    std::shared_ptr<const decima::TypeDetail> detail;
    if (const auto* rtti = decima::find_type(name.c_str()); rtti != nullptr) {
        auto described = std::make_shared<decima::TypeDetail>();
        if (decima::describe_type(reinterpret_cast<std::uintptr_t>(rtti), *described)) {
            detail = std::move(described);
        }
    }
    std::scoped_lock lock(g_cache_mutex);
    g_details[name] = detail;
    if (detail == nullptr) {
        return false;
    }
    out = *detail;
    return true;
}

std::string enum_value_name(const std::string& type, std::int64_t value) {
    decima::TypeDetail detail;
    if (!describe(type, detail) || detail.values.empty()) {
        return {};
    }
    for (const auto& entry : detail.values) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return {};
}

std::string format_value(const std::string& type, std::uintptr_t at, std::uintptr_t& follow) {
    const auto integer = [&](auto probe) -> std::string {
        decltype(probe) value{};
        if (!read(at, value)) {
            return "??";
        }
        auto text = std::format("{}", value);
        if (const auto label = enum_value_name(type, static_cast<std::int64_t>(value));
                !label.empty()) {
            text += "  (" + label + ")";
        }
        return text;
    };

    if (type == "bool") {
        std::uint8_t value = 0;
        return read(at, value) ? (value != 0 ? "true" : "false") : "??";
    }
    if (type == "uint8" || type == "int8") return integer(std::uint8_t{});
    if (type == "uint16") return integer(std::uint16_t{});
    if (type == "int16") return integer(std::int16_t{});
    if (type == "uint32" || type == "uint") return integer(std::uint32_t{});
    if (type == "int32" || type == "int") return integer(std::int32_t{});
    if (type == "uint64") return integer(std::uint64_t{});
    if (type == "int64") return integer(std::int64_t{});
    if (type == "float") {
        float value = 0.0f;
        return read(at, value) ? std::format("{:g}", value) : "??";
    }
    if (type == "double") {
        double value = 0.0;
        return read(at, value) ? std::format("{:g}", value) : "??";
    }
    if (type == "WorldPosition" || type == "WorldTransform") {
        double x = 0, y = 0, z = 0;
        if (read(at, x) && read(at + 8, y) && read(at + 16, z)) {
            return std::format("{:.2f}, {:.2f}, {:.2f}", x, y, z);
        }
        return "??";
    }
    if (type == "Vec2" || type == "Vec3" || type == "Vec4") {
        float v[4]{};
        const int count = type.back() - '0';
        std::string out;
        for (int i = 0; i < count; ++i) {
            if (!read(at + i * 4, v[i])) {
                return "??";
            }
            out += std::format("{}{:g}", i ? ", " : "", v[i]);
        }
        return out;
    }
    if (type == "GGUUID") {
        std::uint8_t bytes[16]{};
        if (!read_bytes(at, bytes, sizeof bytes)) {
            return "??";
        }
        std::string out;
        for (int i = 0; i < 16; ++i) {
            out += std::format("{:02x}", bytes[i]);
            if (i == 3 || i == 5 || i == 7 || i == 9) {
                out += '-';
            }
        }
        return out;
    }
    if (type == "String" || type == "tchar") {
        std::uintptr_t pointer = 0;
        if (!read(at, pointer) || pointer == 0) {
            return "(empty)";
        }
        const auto text = read_string(pointer, 96);
        return text.empty() ? "(empty)" : '"' + text + '"';
    }
    if (shape_of(type) == Shape::Array) {
        std::uint32_t count = 0;
        std::uintptr_t data = 0;
        if (read(at, count) && read(at + 8, data)) {
            return std::format("{} item(s) @ {:#x}", count, data);
        }
        return "??";
    }
    if (shape_of(type) == Shape::Pointer) {
        std::uintptr_t pointer = 0;
        if (!read(at, pointer)) {
            return "??";
        }
        if (pointer == 0) {
            return "null";
        }
        const auto name = type_name_at(pointer);
        if (!name.empty()) {
            follow = pointer;
            return std::format("{:#x}  {}", pointer, name);
        }
        return std::format("{:#x}", pointer);
    }
    if (decima::TypeDetail detail; describe(type, detail) && detail.kind != "class") {
        std::uint64_t raw = 0;
        const auto bytes = std::clamp<std::uint32_t>(detail.size, 1, 8);
        if (!read_bytes(at, &raw, bytes)) {
            return "??";
        }
        auto text = std::format("{}", raw);
        if (const auto label = enum_value_name(type, static_cast<std::int64_t>(raw));
                !label.empty()) {
            text += "  (" + label + ")";
        }
        return text;
    }
    if (shape_of(type) == Shape::Struct) {
        return std::format("{{{}}}", type);
    }

    std::uintptr_t raw = 0;
    if (!read(at, raw)) {
        return "??";
    }
    const auto name = type_name_at(raw);
    if (!name.empty()) {
        follow = raw;
        return std::format("{:#x}  {}", raw, name);
    }
    return std::format("{:#018x}", raw);
}

std::vector<Field> fields_of(const std::string& type_name) {
    {
        std::scoped_lock lock(g_cache_mutex);
        if (const auto found = g_fields.find(type_name); found != g_fields.end()) {
            return *found->second;
        }
    }
    auto fields = std::make_shared<std::vector<Field>>();
    collect(type_name, 0, 0, *fields);
    std::stable_sort(fields->begin(), fields->end(),
                     [](const Field& a, const Field& b) { return a.offset < b.offset; });
    std::scoped_lock lock(g_cache_mutex);
    g_fields[type_name] = fields;
    return *fields;
}

Shape shape_of(const std::string& type) {
    if (starts_with(type, "Array<") || starts_with(type, "Array_")) {
        return Shape::Array;
    }
    if (starts_with(type, "Ref<") || starts_with(type, "cptr<") || starts_with(type, "WeakPtr<")
        || starts_with(type, "StreamingRef<") || starts_with(type, "UUIDRef<")
        || (!type.empty() && type.back() == '*')) {
        return Shape::Pointer;
    }
    decima::TypeDetail detail;
    if (describe(type, detail) && detail.kind == "class" && detail.size > 0) {
        return Shape::Struct;
    }
    return Shape::Value;
}

std::string element_of(const std::string& type) {
    if (starts_with(type, "Array<") && type.back() == '>') {
        return type.substr(6, type.size() - 7);
    }
    if (starts_with(type, "Array_")) {
        return type.substr(6);
    }
    return {};
}

std::uint32_t size_of(const std::string& type) {
    static const std::map<std::string, std::uint32_t, std::less<>> primitives = {
        {"bool", 1},   {"int8", 1},    {"uint8", 1},  {"int16", 2},    {"uint16", 2},
        {"int32", 4},  {"uint32", 4},  {"int", 4},    {"uint", 4},     {"float", 4},
        {"int64", 8},  {"uint64", 8},  {"double", 8}, {"String", 8},   {"tchar", 8},
        {"Vec2", 8},   {"Vec3", 12},   {"Vec4", 16},  {"GGUUID", 16},  {"WorldPosition", 24},
    };
    if (const auto found = primitives.find(type); found != primitives.end()) {
        return found->second;
    }
    switch (shape_of(type)) {
        case Shape::Pointer: return 8;
        case Shape::Array: return 16;
        default: break;
    }
    decima::TypeDetail detail;
    return describe(type, detail) ? detail.size : 0;
}

}
