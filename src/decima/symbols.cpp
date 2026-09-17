#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/log.h"
#include "core/process.h"
#include "decima/dumper.h"
#include "decima/exported.h"

namespace bridger::decima {
namespace {

constexpr std::size_t kMaxRegionBytes = 512ull * 1024 * 1024;
constexpr std::size_t kChunkBytes = 64 * 1024;
constexpr std::size_t kOverlapBytes = 256;
constexpr std::uint32_t kMaxSymbols = 100000;

void escape(std::ostream& out, std::string_view text) {
    out << '"';
    for (const char c : text) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: {
                const auto byte = static_cast<unsigned char>(c);
                if (byte < 0x20 || byte > 0x7E) {
                    out << "\\u00" << "0123456789abcdef"[(byte >> 4) & 0xF]
                        << "0123456789abcdef"[byte & 0xF];
                } else {
                    out << c;
                }
                break;
            }
        }
    }
    out << '"';
}

const char* kind_name(std::uint8_t kind) {
    switch (static_cast<SymbolKind>(kind)) {
        case SymbolKind::Atom:       return "atom";
        case SymbolKind::Enum:       return "enum";
        case SymbolKind::Class:      return "class";
        case SymbolKind::Struct:     return "struct";
        case SymbolKind::Typedef:    return "typedef";
        case SymbolKind::Function:   return "function";
        case SymbolKind::Variable:   return "variable";
        case SymbolKind::Container:  return "container";
        case SymbolKind::Reference:  return "reference";
        case SymbolKind::Pointer:    return "pointer";
        case SymbolKind::SourceFile: return "source_file";
    }
    return "unknown";
}

bool printable(const std::string& text) {
    if (text.empty() || text.size() > 200) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte >= 0x21 && byte <= 0x7E;
    });
}

bool plausible_group(const proc::AddressSpace& space, const mem::Module& game,
                     const ExportedSymbolGroup& group) {
    if (group.symbol_count == 0 || group.symbol_count > kMaxSymbols) {
        return false;
    }
    if (group.symbol_capacity < group.symbol_count) {
        return false;
    }

    const auto vtable = reinterpret_cast<std::uintptr_t>(group.vtable);
    std::uintptr_t first_slot = 0;
    if (!space.read(vtable, first_slot)) {
        return false;
    }
    if (first_slot < game.text_begin || first_slot >= game.text_end) {
        return false;
    }

    if (!printable(space.read_string(reinterpret_cast<std::uintptr_t>(group.name_space)))) {
        return false;
    }

    const auto symbols = reinterpret_cast<std::uintptr_t>(group.symbols);
    ExportedSymbol first{};
    if (!space.read(symbols, first)) {
        return false;
    }
    if (first.kind > kMaxSymbolKind) {
        return false;
    }
    return printable(space.read_string(reinterpret_cast<std::uintptr_t>(first.name)));
}

std::map<std::string, std::uintptr_t> collect_groups(const proc::AddressSpace& space,
                                                    const mem::Module& game) {
    std::map<std::string, std::uintptr_t> groups;
    std::vector<std::uint8_t> buffer(kChunkBytes);

    for (const auto& region : space.regions()) {
        if (!region.writable || region.size() > kMaxRegionBytes) {
            continue;
        }
        for (auto base = region.begin; base < region.end; base += kChunkBytes - kOverlapBytes) {
            const auto want = std::min<std::size_t>(kChunkBytes, region.end - base);
            if (want < sizeof(ExportedSymbolGroup)) {
                break;
            }
            if (!space.read(base, buffer.data(), want)) {
                continue;
            }
            for (std::size_t offset = 0; offset + sizeof(ExportedSymbolGroup) <= want; offset += 8) {
                ExportedSymbolGroup group{};
                std::memcpy(&group, buffer.data() + offset, sizeof(group));
                if (!plausible_group(space, game, group)) {
                    continue;
                }
                auto name = space.read_string(reinterpret_cast<std::uintptr_t>(group.name_space));
                groups.emplace(std::move(name), base + offset);
            }
        }
    }
    return groups;
}

std::string signature_of(const proc::AddressSpace& space, const ExportedSymbol& symbol) {
    const auto& definition = symbol.exported;
    const auto tokens = reinterpret_cast<std::uintptr_t>(definition.tokens);
    const auto count = std::min<std::uint32_t>(definition.token_count, 32);
    if (count == 0) {
        return {};
    }

    std::vector<std::string> parts;
    for (std::uint32_t i = 0; i < count; ++i) {
        ExportedSymbolToken token{};
        if (!space.read(tokens + i * sizeof(ExportedSymbolToken), token)) {
            break;
        }
        auto text = space.read_string(reinterpret_cast<std::uintptr_t>(token.type_name));
        auto modifiers = space.read_string(reinterpret_cast<std::uintptr_t>(token.modifiers));
        if (!modifiers.empty()) {
            text += modifiers;
        }
        parts.push_back(text);
    }
    if (parts.empty()) {
        return {};
    }

    std::string out = parts[0] + " (";
    for (std::size_t i = 1; i < parts.size(); ++i) {
        if (i > 1) {
            out += ", ";
        }
        out += parts[i];
    }
    out += parts.size() > 1 ? ")" : "void)";
    return out;
}

void write_definition(std::ostream& out, const proc::AddressSpace& space, const mem::Module& game,
                      const ExportedSymbolDefinition& definition) {
    const auto address = reinterpret_cast<std::uintptr_t>(definition.address);
    out << "{\"address\": " << address;
    if (address >= game.base && address < game.base + game.size) {
        out << ", \"rva\": " << (address - game.base);
    }
    out << ", \"name\": ";
    escape(out, space.read_string(reinterpret_cast<std::uintptr_t>(definition.name)));
    out << ", \"header\": ";
    escape(out, space.read_string(reinterpret_cast<std::uintptr_t>(definition.header_file)));
    out << ", \"source\": ";
    escape(out, space.read_string(reinterpret_cast<std::uintptr_t>(definition.source_file)));

    out << ", \"tokens\": [";
    const auto tokens = reinterpret_cast<std::uintptr_t>(definition.tokens);
    const auto count = std::min<std::uint32_t>(definition.token_count, 64);
    for (std::uint32_t i = 0; i < count; ++i) {
        ExportedSymbolToken token{};
        if (!space.read(tokens + i * sizeof(ExportedSymbolToken), token)) {
            break;
        }
        if (i != 0) {
            out << ", ";
        }
        out << "{\"type\": ";
        escape(out, space.read_string(reinterpret_cast<std::uintptr_t>(token.type_name)));
        out << ", \"modifiers\": ";
        escape(out, space.read_string(reinterpret_cast<std::uintptr_t>(token.modifiers)));
        out << ", \"name\": ";
        escape(out, space.read_string(reinterpret_cast<std::uintptr_t>(token.name)));
        out << "}";
    }
    out << "]}";
}

}

std::shared_ptr<const NameIndex> symbol_index();
void publish_symbols(const NameIndex& additions);

std::shared_ptr<const SymbolDetails>& details_slot() {
    static std::shared_ptr<const SymbolDetails> slot = std::make_shared<const SymbolDetails>();
    return slot;
}

std::shared_ptr<const SymbolDetails> symbol_details() {
    return std::atomic_load(&details_slot());
}

void scan_symbols(const mem::Module& game) {
    proc::AddressSpace space;
    space.refresh();

    const auto groups = collect_groups(space, game);

    NameIndex index;
    SymbolDetails details;

    const auto image_end = game.base + game.size;
    for (const auto& [group_name, address] : groups) {
        ExportedSymbolGroup group{};
        if (!space.read(address, group)) {
            continue;
        }
        const auto symbols = reinterpret_cast<std::uintptr_t>(group.symbols);
        for (std::uint32_t i = 0; i < group.symbol_count; ++i) {
            ExportedSymbol symbol{};
            if (!space.read(symbols + i * sizeof(ExportedSymbol), symbol)) {
                break;
            }
            const auto label = space.read_string(reinterpret_cast<std::uintptr_t>(symbol.name));
            if (label.empty()) {
                continue;
            }

            SymbolDetail detail;
            detail.group = group_name;
            detail.name = label;
            detail.kind = kind_name(symbol.kind);
            detail.address = reinterpret_cast<std::uintptr_t>(symbol.exported.address);
            if (detail.address == 0) {
                detail.address = reinterpret_cast<std::uintptr_t>(symbol.internal.address);
            }
            if (detail.address >= game.base && detail.address < image_end) {
                detail.rva = detail.address - game.base;
            }
            detail.header = space.read_string(
                reinterpret_cast<std::uintptr_t>(symbol.exported.header_file));
            detail.signature = signature_of(space, symbol);

            const auto key = group_name + "::" + label;
            if (detail.address != 0) {
                index.emplace(key, detail.address);
            }
            details.emplace(key, std::move(detail));
        }
    }

    log::info("scan: {} symbols across {} groups", details.size(), groups.size());
    publish_symbols(index);
    std::atomic_store(&details_slot(),
                      std::make_shared<const SymbolDetails>(std::move(details)));
}

std::shared_ptr<const NameIndex>& symbol_slot() {
    static std::shared_ptr<const NameIndex> slot = std::make_shared<const NameIndex>();
    return slot;
}

std::shared_ptr<const NameIndex> symbol_index() {
    return std::atomic_load(&symbol_slot());
}

void publish_symbols(const NameIndex& additions) {
    auto merged = std::make_shared<NameIndex>(*symbol_index());
    for (const auto& [key, address] : additions) {
        merged->insert_or_assign(key, address);
    }
    std::atomic_store(&symbol_slot(),
                      std::shared_ptr<const NameIndex>(std::move(merged)));
}

std::size_t load_static_symbols(const std::filesystem::path& file, std::uintptr_t image_base) {
    NameIndex index;
    std::ifstream stream(file);
    if (!stream.is_open()) {
        log::warn("no static symbol index at {}, find_symbol needs a scan first", file.string());
        return 0;
    }
    try {
        const auto document = nlohmann::json::parse(stream);
        for (const auto& [key, rva] : document.at("symbols").items()) {
            if (rva.is_number_unsigned()) {
                index.emplace(key, image_base + rva.get<std::uintptr_t>());
            }
        }
    } catch (const std::exception& error) {
        log::warn("static symbol index unreadable: {}", error.what());
    }
    const auto count = index.size();
    publish_symbols(index);
    log::info("static symbol index: {} symbols", count);
    return count;
}

void* find_symbol(const char* group, const char* name) {
    const auto index = symbol_index();
    const auto found = index->find(std::string(group) + "::" + name);
    return found == index->end() ? nullptr : reinterpret_cast<void*>(found->second);
}

std::size_t indexed_symbols() {
    return symbol_index()->size();
}

SymbolStats dump_symbols(const mem::Module& game, const std::filesystem::path& output) {
    SymbolStats stats;
    NameIndex dumped;

    proc::AddressSpace space;
    space.refresh();

    std::map<std::string, std::uintptr_t> groups;
    std::vector<std::uint8_t> buffer(kChunkBytes);

    for (const auto& region : space.regions()) {
        if (!region.writable || region.size() > kMaxRegionBytes) {
            continue;
        }
        for (auto base = region.begin; base < region.end; base += kChunkBytes - kOverlapBytes) {
            const auto want = std::min<std::size_t>(kChunkBytes, region.end - base);
            if (want < sizeof(ExportedSymbolGroup)) {
                break;
            }
            if (!space.read(base, buffer.data(), want)) {
                continue;
            }
            for (std::size_t offset = 0; offset + sizeof(ExportedSymbolGroup) <= want; offset += 8) {
                ExportedSymbolGroup group{};
                std::memcpy(&group, buffer.data() + offset, sizeof(group));
                if (!plausible_group(space, game, group)) {
                    continue;
                }
                auto name = space.read_string(reinterpret_cast<std::uintptr_t>(group.name_space));
                groups.emplace(std::move(name), base + offset);
            }
        }
    }

    log::info("found {} exported symbol groups", groups.size());

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    std::ofstream out(output, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        log::error("could not open {}", output.string());
        return stats;
    }

    out << "{\n \"image_base\": " << game.base << ",\n \"groups\": {\n";
    bool first_group = true;
    for (const auto& [name, address] : groups) {
        ExportedSymbolGroup group{};
        if (!space.read(address, group)) {
            continue;
        }
        ++stats.groups;

        if (!first_group) {
            out << ",\n";
        }
        first_group = false;

        out << "  ";
        escape(out, name);
        out << ": {\"address\": " << address
            << ", \"export_mask\": " << group.export_mask
            << ", \"symbols\": [";

        const auto symbols = reinterpret_cast<std::uintptr_t>(group.symbols);
        bool first_symbol = true;
        for (std::uint32_t i = 0; i < group.symbol_count; ++i) {
            ExportedSymbol symbol{};
            if (!space.read(symbols + i * sizeof(ExportedSymbol), symbol)) {
                break;
            }
            ++stats.symbols;
            {
                const auto label = space.read_string(reinterpret_cast<std::uintptr_t>(symbol.name));
                const auto address = reinterpret_cast<std::uintptr_t>(symbol.exported.address);
                if (!label.empty() && address != 0) {
                    dumped.emplace(name + "::" + label, address);
                }
            }
            if (static_cast<SymbolKind>(symbol.kind) == SymbolKind::Function) {
                ++stats.functions;
            } else if (static_cast<SymbolKind>(symbol.kind) == SymbolKind::Variable) {
                ++stats.variables;
            }
            if (symbol.exported.address != nullptr) {
                ++stats.with_address;
            }

            if (!first_symbol) {
                out << ", ";
            }
            first_symbol = false;

            out << "\n   {\"kind\": \"" << kind_name(symbol.kind) << "\", \"namespace\": ";
            escape(out, space.read_string(reinterpret_cast<std::uintptr_t>(symbol.name_space)));
            out << ", \"name\": ";
            escape(out, space.read_string(reinterpret_cast<std::uintptr_t>(symbol.name)));
            out << ", \"exported\": ";
            write_definition(out, space, game, symbol.exported);
            out << ", \"internal\": ";
            write_definition(out, space, game, symbol.internal);
            out << "}";
        }
        out << "\n  ]}";
    }
    out << "\n }\n}\n";

    return stats;
}

}
