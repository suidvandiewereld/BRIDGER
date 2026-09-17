#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/memory.h"

namespace bridger::decima {

struct DumpStats {
    std::size_t classes = 0;
    std::size_t enums = 0;
    std::size_t members = 0;
    std::size_t in_image = 0;
    std::size_t runtime_only = 0;
    std::size_t regions_scanned = 0;
    std::size_t bytes_scanned = 0;
};

struct SymbolStats {
    std::size_t groups = 0;
    std::size_t symbols = 0;
    std::size_t functions = 0;
    std::size_t variables = 0;
    std::size_t with_address = 0;
};

struct TypeBase {
    std::string name;
    std::uint32_t offset = 0;
};

struct TypeMember {
    std::string name;
    std::string type;
    std::string category;
    std::uint32_t offset = 0;
    std::uint16_t flags = 0;
    std::uintptr_t getter = 0;
    std::uintptr_t setter = 0;
};

struct TypeValue {
    std::string name;
    std::int32_t value = 0;
};

struct TypeDetail {
    std::string name;
    std::string kind;
    std::uintptr_t address = 0;
    std::uint32_t id = 0;
    std::uint32_t size = 0;
    std::uint16_t alignment = 0;
    std::uint16_t flags = 0;
    unsigned message_handlers = 0;
    std::uintptr_t constructor = 0;
    std::uintptr_t destructor = 0;
    std::vector<TypeBase> bases;
    std::vector<TypeMember> members;
    std::vector<TypeValue> values;
};

struct SymbolDetail {
    std::string group;
    std::string name;
    std::string kind;
    std::string signature;
    std::string header;
    std::uintptr_t address = 0;
    std::uintptr_t rva = 0;
};

bool wait_for_registration(const mem::Module& game, unsigned timeout_ms);

DumpStats dump_types(const mem::Module& game, const std::filesystem::path& output);
SymbolStats dump_symbols(const mem::Module& game, const std::filesystem::path& output);

void scan_types(const mem::Module& game);
void scan_symbols(const mem::Module& game);
void start_scan(const mem::Module& game);

[[nodiscard]] bool scanning();
[[nodiscard]] const char* scan_stage();

using NameIndex = std::map<std::string, std::uintptr_t>;
using SymbolDetails = std::map<std::string, SymbolDetail>;

std::shared_ptr<const NameIndex> type_index();
std::shared_ptr<const NameIndex> symbol_index();
std::shared_ptr<const SymbolDetails> symbol_details();

bool describe_type(std::uintptr_t address, TypeDetail& out);

std::size_t load_static_index(const std::filesystem::path& file, std::uintptr_t image_base);
const void* find_type(const char* name);
void* find_symbol(const char* group, const char* name);

std::size_t load_static_symbols(const std::filesystem::path& file, std::uintptr_t image_base);
std::size_t indexed_types();
std::size_t indexed_symbols();
std::vector<std::string> type_names();

}
