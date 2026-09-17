#pragma once

#include <cstddef>
#include <cstdint>

namespace bridger::decima {

enum class SymbolKind : std::uint8_t {
    Atom = 0,
    Enum = 1,
    Class = 2,
    Struct = 3,
    Typedef = 4,
    Function = 5,
    Variable = 6,
    Container = 7,
    Reference = 8,
    Pointer = 9,
    SourceFile = 10,
};

inline constexpr std::uint8_t kMaxSymbolKind = 10;

struct ExportedSymbolToken {
    const char* type_name;
    const char* modifiers;
    const void* type;
    const char* name;
    std::uint8_t flags;
    std::uint8_t padding[7];
};

struct ExportedSymbolDefinition {
    void* address;
    const char* name;
    const char* header_file;
    const char* source_file;
    std::uint32_t token_count;
    std::uint32_t token_capacity;
    const ExportedSymbolToken* tokens;
    std::uint8_t unknown[16];
};

struct ExportedSymbol {
    std::uint8_t kind;
    std::uint8_t padding_00[7];
    const void* type;
    const char* name_space;
    const char* name;
    std::uint64_t unknown_20;
    ExportedSymbolDefinition exported;
    ExportedSymbolDefinition internal;
    std::uint8_t unknown_a8[64];
};

struct ExportedSymbolGroup {
    void* vtable;
    std::uint32_t export_mask;
    std::uint32_t padding_0c;
    const char* name_space;
    std::uint32_t symbol_count;
    std::uint32_t symbol_capacity;
    const ExportedSymbol* symbols;
    std::uint32_t dependency_count;
    std::uint32_t dependency_capacity;
    const void* dependencies;
};

static_assert(sizeof(ExportedSymbolToken) == 0x28);
static_assert(sizeof(ExportedSymbolDefinition) == 0x40);
static_assert(sizeof(ExportedSymbol) == 0xE8);
static_assert(sizeof(ExportedSymbolGroup) == 0x38);
static_assert(offsetof(ExportedSymbolGroup, name_space) == 16);
static_assert(offsetof(ExportedSymbolGroup, symbols) == 32);
static_assert(offsetof(ExportedSymbol, name) == 24);
static_assert(offsetof(ExportedSymbol, exported) == 40);
static_assert(offsetof(ExportedSymbol, internal) == 104);

}
