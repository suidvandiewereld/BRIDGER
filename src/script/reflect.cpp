#include "script/reflect.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include "core/guard.h"
#include "decima/dumper.h"
#include "loader/registry.h"

namespace bridger::script::reflect {
namespace {

constexpr std::size_t kKind = 4;
constexpr std::size_t kEnumSize = 5;
constexpr std::size_t kClassHandlerCount = 8;
constexpr std::size_t kClassHandlers = 112;
constexpr std::uint8_t kKindEnum = 3;
constexpr std::uint8_t kKindClass = 4;
constexpr std::uint8_t kKindEnumFlags = 5;

template <typename T>
T read_value(const void* base, std::size_t offset) {
    T value{};
    safe_read(static_cast<const std::uint8_t*>(base) + offset, &value, sizeof value);
    return value;
}

std::uint8_t kind_of(const void* rtti) {
    return rtti != nullptr ? read_value<std::uint8_t>(rtti, kKind) : 0xff;
}

struct Atom {
    std::string_view name;
    Codec codec;
    std::uint32_t size;
};

constexpr Atom kAtoms[] = {
    {"bool", Codec::Bool, 1},     {"int8", Codec::I8, 1},      {"uint8", Codec::U8, 1},
    {"int16", Codec::I16, 2},     {"uint16", Codec::U16, 2},   {"int", Codec::I32, 4},
    {"int32", Codec::I32, 4},     {"uint", Codec::U32, 4},     {"uint32", Codec::U32, 4},
    {"int64", Codec::I64, 8},     {"uint64", Codec::U64, 8},   {"float", Codec::F32, 4},
    {"double", Codec::F64, 8},    {"wchar", Codec::U16, 2},    {"tchar", Codec::I8, 1},
    {"ucs4", Codec::U32, 4},      {"uintptr", Codec::U64, 8},  {"HalfFloat", Codec::U16, 2},
};

std::shared_mutex g_types_mutex;
std::unordered_map<std::string, std::unique_ptr<Type>> g_types;

std::shared_mutex g_layouts_mutex;
std::unordered_map<const void*, std::unique_ptr<Layout>> g_layouts;

std::shared_mutex g_enums_mutex;
std::unordered_map<const void*, std::vector<std::pair<std::string, std::int64_t>>> g_enums;

std::unique_ptr<Type> build_type(const std::string& name) {
    auto out = std::make_unique<Type>();
    out->name = name;

    for (const auto& atom : kAtoms) {
        if (atom.name == name) {
            out->codec = atom.codec;
            out->size = atom.size;
            return out;
        }
    }
    if (name == "String") {
        out->codec = Codec::String;
        out->size = 8;
        return out;
    }
    if (name == "WString") {
        out->codec = Codec::WString;
        out->size = 8;
        return out;
    }
    if (name == "GGUUID") {
        out->codec = Codec::Uuid;
        out->size = 16;
        out->rtti = decima::find_type("GGUUID");
        return out;
    }

    const auto open = name.find('<');
    if (open != std::string::npos && name.back() == '>') {
        const std::string outer = name.substr(0, open);
        const std::string inner = name.substr(open + 1, name.size() - open - 2);
        if (outer == "Ref" || outer == "cptr" || outer == "WeakPtr") {
            out->codec = Codec::Pointer;
            out->size = 8;
            out->item = type(inner);
            out->owning = outer != "cptr";
            return out;
        }
        if (outer == "StreamingRef") {
            out->codec = Codec::Handle;
            out->size = 8;
            out->item = type(inner);
            return out;
        }
        if (outer == "UUIDRef") {
            out->codec = Codec::Uuid;
            out->size = 16;
            out->item = type(inner);
            return out;
        }
        if (outer == "Array") {
            out->codec = Codec::Array;
            out->size = 16;
            out->item = type(inner);
            return out;
        }
        return out;
    }

    const void* rtti = decima::find_type(name.c_str());
    const auto kind = kind_of(rtti);
    if (kind == kKindEnum || kind == kKindEnumFlags) {
        out->codec = Codec::Enum;
        out->rtti = rtti;
        out->size = read_value<std::uint8_t>(rtti, kEnumSize);
        if (out->size != 1 && out->size != 2 && out->size != 4 && out->size != 8) {
            out->size = 4;
        }
        return out;
    }
    if (kind == kKindClass) {
        out->codec = Codec::Struct;
        out->rtti = rtti;
        if (const auto* table = loader::api(); table != nullptr) {
            out->size = table->type_size(rtti);
        }
        return out;
    }
    return out;
}

void flatten(const std::string& class_name, std::uint32_t base_offset, int depth, Layout& layout) {
    if (depth > 12) {
        return;
    }
    const void* rtti = decima::find_type(class_name.c_str());
    if (rtti == nullptr) {
        return;
    }
    decima::TypeDetail detail;
    if (!decima::describe_type(reinterpret_cast<std::uintptr_t>(rtti), detail)
        || detail.kind != "class") {
        return;
    }
    if (depth > 0) {
        layout.lineage.push_back(detail.name);
    }
    for (const auto& base : detail.bases) {
        flatten(base.name, base_offset + base.offset, depth + 1, layout);
    }
    for (const auto& member : detail.members) {
        Field field;
        field.name = member.name;
        field.type_name = member.type;
        field.offset = base_offset + member.offset;
        field.getter = member.getter;
        field.setter = member.setter;
        field.category = member.category;
        field.type = type(member.type);
        layout.fields.push_back(std::move(field));
    }
}

std::unique_ptr<Layout> build_layout(const void* rtti) {
    const char* name = rtti_name(rtti);
    if (name == nullptr || kind_of(rtti) != kKindClass) {
        return nullptr;
    }
    auto layout = std::make_unique<Layout>();
    layout->rtti = rtti;
    layout->name = name;
    layout->lineage.push_back(name);
    if (const auto* table = loader::api(); table != nullptr) {
        layout->size = table->type_size(rtti);
    }
    flatten(layout->name, 0, 0, *layout);
    for (std::size_t i = 0; i < layout->fields.size(); ++i) {
        layout->by_name[layout->fields[i].name] = i;
    }
    const auto count = read_value<std::uint8_t>(rtti, kClassHandlerCount);
    const auto* handlers = read_value<const std::uint8_t*>(rtti, kClassHandlers);
    for (std::uint8_t i = 0; handlers != nullptr && i < count; ++i) {
        const void* message = read_value<const void*>(handlers, i * 16u);
        if (const char* message_name = rtti_name(message); message_name != nullptr) {
            layout->messages.emplace_back(message_name);
        }
    }
    return layout;
}

std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        row[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            const bool same = std::tolower(static_cast<unsigned char>(a[i - 1]))
                           == std::tolower(static_cast<unsigned char>(b[j - 1]));
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (same ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

struct WriteWork {
    std::uintptr_t address;
    const void* bytes;
    std::size_t size;
};

}

const Field* Layout::find(std::string_view field) const {
    const auto found = by_name.find(std::string(field));
    return found == by_name.end() ? nullptr : &fields[found->second];
}

std::string Layout::suggest(std::string_view field) const {
    std::string best;
    std::size_t best_distance = std::max<std::size_t>(3, field.size() / 3);
    for (const auto& candidate : fields) {
        const auto distance = edit_distance(field, candidate.name);
        if (distance < best_distance) {
            best_distance = distance;
            best = candidate.name;
        }
    }
    return best;
}

const Type* type(std::string_view raw) {
    std::string name(raw);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
        name.pop_back();
    }
    {
        std::shared_lock lock(g_types_mutex);
        if (const auto found = g_types.find(name); found != g_types.end()) {
            return found->second.get();
        }
    }
    auto built = build_type(name);
    std::unique_lock lock(g_types_mutex);
    const auto [where, inserted] = g_types.emplace(name, std::move(built));
    return where->second.get();
}

const Layout* layout(std::string_view class_name) {
    const void* rtti = decima::find_type(std::string(class_name).c_str());
    return rtti != nullptr ? layout(rtti) : nullptr;
}

const Layout* layout(const void* rtti) {
    if (rtti == nullptr) {
        return nullptr;
    }
    {
        std::shared_lock lock(g_layouts_mutex);
        if (const auto found = g_layouts.find(rtti); found != g_layouts.end()) {
            return found->second.get();
        }
    }
    auto built = build_layout(rtti);
    if (built == nullptr) {
        return nullptr;
    }
    std::unique_lock lock(g_layouts_mutex);
    const auto [where, inserted] = g_layouts.emplace(rtti, std::move(built));
    return where->second.get();
}

const void* rtti_of(std::uintptr_t address) {
    if (address < 0x10000 || address >= 0x7fffffffffffull || (address & 7) != 0) {
        return nullptr;
    }
    const auto* table = loader::api();
    if (table == nullptr) {
        return nullptr;
    }
    const void* rtti = table->rtti_of(reinterpret_cast<const void*>(address));
    return kind_of(rtti) == kKindClass ? rtti : nullptr;
}

const char* rtti_name(const void* rtti) {
    const auto* table = loader::api();
    return table != nullptr && rtti != nullptr ? table->rtti_name(rtti) : nullptr;
}

bool is_a(const void* rtti, const void* base) {
    const auto* table = loader::api();
    return table != nullptr && table->rtti_is_a(rtti, base);
}

const void* find(std::string_view name) {
    return decima::find_type(std::string(name).c_str());
}

std::vector<std::pair<std::string, std::int64_t>> enum_values(const void* rtti) {
    {
        std::shared_lock lock(g_enums_mutex);
        if (const auto found = g_enums.find(rtti); found != g_enums.end()) {
            return found->second;
        }
    }
    std::vector<std::pair<std::string, std::int64_t>> values;
    decima::TypeDetail detail;
    if (rtti != nullptr
        && decima::describe_type(reinterpret_cast<std::uintptr_t>(rtti), detail)) {
        for (const auto& value : detail.values) {
            values.emplace_back(value.name, value.value);
        }
    }
    std::unique_lock lock(g_enums_mutex);
    g_enums.emplace(rtti, values);
    return values;
}

bool enum_value(const void* rtti, std::string_view name, std::int64_t& out) {
    for (const auto& [label, value] : enum_values(rtti)) {
        if (label == name) {
            out = value;
            return true;
        }
    }
    return false;
}

std::string enum_name(const void* rtti, std::int64_t value) {
    for (const auto& [label, number] : enum_values(rtti)) {
        if (number == value) {
            return label;
        }
    }
    return {};
}

bool read(std::uintptr_t address, void* out, std::size_t bytes) {
    if (address < 0x10000 || address >= 0x7fffffffffffull) {
        return false;
    }
    return safe_read(reinterpret_cast<const void*>(address), out, bytes);
}

bool write(std::uintptr_t address, const void* bytes, std::size_t size) {
    if (address < 0x10000 || address >= 0x7fffffffffffull || bytes == nullptr || size == 0) {
        return false;
    }
    WriteWork work{address, bytes, size};
    return guarded_call(
               [](void* raw) {
                   const auto& w = *static_cast<WriteWork*>(raw);
                   std::memcpy(reinterpret_cast<void*>(w.address), w.bytes, w.size);
               },
               &work)
        == 0;
}

std::string read_c_string(std::uintptr_t address, std::size_t limit) {
    std::string out;
    char chunk[64];
    while (out.size() < limit) {
        if (read(address + out.size(), chunk, sizeof chunk)) {
            const auto* end = static_cast<const char*>(std::memchr(chunk, 0, sizeof chunk));
            out.append(chunk, end != nullptr ? static_cast<std::size_t>(end - chunk) : sizeof chunk);
            if (end != nullptr) {
                break;
            }
            continue;
        }
        char c = 0;
        if (!read(address + out.size(), &c, 1) || c == 0) {
            break;
        }
        out.push_back(c);
    }
    if (out.size() > limit) {
        out.resize(limit);
    }
    return out;
}

std::string read_wide_string(std::uintptr_t address, std::size_t limit) {
    std::wstring wide;
    wchar_t c = 0;
    while (wide.size() < limit && read(address + wide.size() * sizeof(wchar_t), &c, sizeof c)
           && c != 0) {
        wide.push_back(c);
    }
    if (wide.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), bytes,
                        nullptr, nullptr);
    return out;
}

bool read_integer(std::uintptr_t address, Codec codec, std::uint32_t size, std::int64_t& out) {
    switch (codec == Codec::Enum || codec == Codec::Bool ? Codec::Unknown : codec) {
        case Codec::I8: { std::int8_t v; if (!read(address, &v, 1)) return false; out = v; return true; }
        case Codec::U8: { std::uint8_t v; if (!read(address, &v, 1)) return false; out = v; return true; }
        case Codec::I16: { std::int16_t v; if (!read(address, &v, 2)) return false; out = v; return true; }
        case Codec::U16: { std::uint16_t v; if (!read(address, &v, 2)) return false; out = v; return true; }
        case Codec::I32: { std::int32_t v; if (!read(address, &v, 4)) return false; out = v; return true; }
        case Codec::U32: { std::uint32_t v; if (!read(address, &v, 4)) return false; out = v; return true; }
        case Codec::I64:
        case Codec::U64: { std::int64_t v; if (!read(address, &v, 8)) return false; out = v; return true; }
        default: break;
    }
    switch (size) {
        case 1: { std::int8_t v; if (!read(address, &v, 1)) return false; out = codec == Codec::Bool ? static_cast<std::uint8_t>(v) : v; return true; }
        case 2: { std::int16_t v; if (!read(address, &v, 2)) return false; out = v; return true; }
        case 4: { std::int32_t v; if (!read(address, &v, 4)) return false; out = v; return true; }
        case 8: { std::int64_t v; if (!read(address, &v, 8)) return false; out = v; return true; }
        default: return false;
    }
}

bool write_integer(std::uintptr_t address, Codec codec, std::uint32_t size, std::int64_t value) {
    switch (codec) {
        case Codec::I8: case Codec::U8: size = 1; break;
        case Codec::I16: case Codec::U16: size = 2; break;
        case Codec::I32: case Codec::U32: size = 4; break;
        case Codec::I64: case Codec::U64: size = 8; break;
        default: break;
    }
    switch (size) {
        case 1: { const auto v = static_cast<std::uint8_t>(value); return write(address, &v, 1); }
        case 2: { const auto v = static_cast<std::uint16_t>(value); return write(address, &v, 2); }
        case 4: { const auto v = static_cast<std::uint32_t>(value); return write(address, &v, 4); }
        case 8: return write(address, &value, 8);
        default: return false;
    }
}

std::string format_uuid(const std::uint8_t bytes[16]) {
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        out += std::format("{:02x}", bytes[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            out += '-';
        }
    }
    return out;
}

bool parse_uuid(std::string_view text, std::uint8_t bytes[16]) {
    int nibbles = 0;
    std::uint8_t current = 0;
    for (const char c : text) {
        if (c == '-' || c == '{' || c == '}') {
            continue;
        }
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        if (digit < 0 || nibbles >= 32) {
            return false;
        }
        current = static_cast<std::uint8_t>((current << 4) | digit);
        if (++nibbles % 2 == 0) {
            bytes[nibbles / 2 - 1] = current;
            current = 0;
        }
    }
    return nibbles == 32;
}

std::uint32_t size_for_ffi(std::string_view name, bool& is_enum) {
    const auto* resolved = type(name);
    is_enum = resolved->codec == Codec::Enum;
    return resolved->size;
}

}
