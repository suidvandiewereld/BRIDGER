#include "decima/dumper.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/log.h"
#include "core/process.h"
#include "decima/rtti.h"

namespace bridger::decima {
namespace {

constexpr std::uintptr_t kAnchorRva = 0x44c1760;
constexpr std::size_t kMaxRegionBytes = 512ull * 1024 * 1024;
constexpr std::size_t kChunkBytes = 64 * 1024;
constexpr std::size_t kOverlapBytes = 256;

struct Found {
    std::uintptr_t address = 0;
    Kind kind = Kind::Class;
};

class Resolver {
public:
    explicit Resolver(const proc::AddressSpace& space) : space_(space) {}

    std::string name_of(std::uintptr_t address, int depth = 0) const {
        if (address == 0 || depth > 8) {
            return {};
        }
        RTTI header{};
        if (!space_.read(address, header)) {
            return {};
        }
        switch (header.kind) {
            case Kind::Class: {
                RTTIClass type{};
                return space_.read(address, type)
                     ? space_.read_string(reinterpret_cast<std::uintptr_t>(type.name))
                     : std::string{};
            }
            case Kind::Enum:
            case Kind::EnumFlags: {
                RTTIEnum type{};
                return space_.read(address, type)
                     ? space_.read_string(reinterpret_cast<std::uintptr_t>(type.name))
                     : std::string{};
            }
            case Kind::Atom: {
                RTTIAtom type{};
                return space_.read(address, type)
                     ? space_.read_string(reinterpret_cast<std::uintptr_t>(type.name))
                     : std::string{};
            }
            case Kind::Pointer:
            case Kind::Container: {
                RTTIPointer type{};
                if (!space_.read(address, type)) {
                    return {};
                }
                RTTIPointerData data{};
                if (!space_.read(reinterpret_cast<std::uintptr_t>(type.data), data)) {
                    return {};
                }
                const auto label = space_.read_string(reinterpret_cast<std::uintptr_t>(data.name));
                const auto item = name_of(reinterpret_cast<std::uintptr_t>(type.item_type), depth + 1);
                if (label.empty() || item.empty()) {
                    return {};
                }
                return label + "<" + item + ">";
            }
            default:
                return {};
        }
    }

private:
    const proc::AddressSpace& space_;
};

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

bool plausible_class(const proc::AddressSpace& space, const RTTIClass& type) {
    if (type.kind != Kind::Class) {
        return false;
    }
    if (type.size == 0 && type.base_count == 0 && type.member_count == 0) {
        return false;
    }
    switch (type.alignment) {
        case 0: case 1: case 2: case 4: case 8: case 16: case 32: case 64: break;
        default: return false;
    }
    if (type.base_count != 0 && !space.mapped(reinterpret_cast<std::uintptr_t>(type.bases),
                                              type.base_count * sizeof(RTTIBaseEntry))) {
        return false;
    }
    if (type.member_count != 0 && !space.mapped(reinterpret_cast<std::uintptr_t>(type.members),
                                                type.member_count * sizeof(RTTIMemberEntry))) {
        return false;
    }
    return true;
}

bool plausible_enum(const RTTIEnum& type) {
    if (type.kind != Kind::Enum && type.kind != Kind::EnumFlags) {
        return false;
    }
    if (type.size == 0 || type.size > 8 || type.value_count == 0) {
        return false;
    }
    return type.size == type.size_again;
}

void write_class(std::ostream& out, const proc::AddressSpace& space, const Resolver& resolver,
                 const RTTIClass& type, DumpStats& stats) {
    out << ", \"kind\": \"class\", \"id\": " << type.id
        << ", \"size\": " << type.size
        << ", \"alignment\": " << type.alignment
        << ", \"flags\": " << type.flags
        << ", \"message_handlers\": " << static_cast<unsigned>(type.message_handler_count)
        << ", \"constructor\": " << reinterpret_cast<std::uintptr_t>(type.constructor)
        << ", \"destructor\": " << reinterpret_cast<std::uintptr_t>(type.destructor)
        << ", \"bases\": [";

    const auto bases = reinterpret_cast<std::uintptr_t>(type.bases);
    for (unsigned i = 0; i < type.base_count; ++i) {
        RTTIBaseEntry entry{};
        if (!space.read(bases + i * sizeof(RTTIBaseEntry), entry)) {
            break;
        }
        if (i != 0) {
            out << ", ";
        }
        out << "{\"name\": ";
        escape(out, resolver.name_of(reinterpret_cast<std::uintptr_t>(entry.type)));
        out << ", \"offset\": " << entry.offset << "}";
    }

    out << "], \"members\": [";

    const auto members = reinterpret_cast<std::uintptr_t>(type.members);
    std::string category;
    bool wrote = false;
    for (unsigned i = 0; i < type.member_count; ++i) {
        RTTIMemberEntry member{};
        if (!space.read(members + i * sizeof(RTTIMemberEntry), member)) {
            break;
        }
        const auto label = space.read_string(reinterpret_cast<std::uintptr_t>(member.name));
        if (member.type == nullptr) {
            category = label;
            continue;
        }
        if (wrote) {
            out << ", ";
        }
        wrote = true;
        ++stats.members;
        out << "{\"name\": ";
        escape(out, label);
        out << ", \"type\": ";
        escape(out, resolver.name_of(reinterpret_cast<std::uintptr_t>(member.type)));
        out << ", \"offset\": " << member.offset
            << ", \"flags\": " << member.flags
            << ", \"category\": ";
        escape(out, category);
        out << "}";
    }
    out << "]}";
}

void write_enum(std::ostream& out, const proc::AddressSpace& space, const RTTIEnum& type) {
    out << ", \"kind\": " << (type.kind == Kind::EnumFlags ? "\"enum flags\"" : "\"enum\"")
        << ", \"id\": " << type.id
        << ", \"size\": " << static_cast<unsigned>(type.size)
        << ", \"values\": [";

    const auto values = reinterpret_cast<std::uintptr_t>(type.values);
    for (unsigned i = 0; i < type.value_count; ++i) {
        RTTIEnumValue value{};
        if (!space.read(values + i * sizeof(RTTIEnumValue), value)) {
            break;
        }
        if (i != 0) {
            out << ", ";
        }
        out << "{\"name\": ";
        escape(out, space.read_string(reinterpret_cast<std::uintptr_t>(value.name)));
        out << ", \"value\": " << value.value << "}";
    }
    out << "]}";
}


std::map<std::string, Found> collect_descriptors(const proc::AddressSpace& space,
                                                DumpStats& stats) {
    std::map<std::string, Found> unique;
    std::vector<std::uint8_t> buffer(kChunkBytes);

    for (const auto& region : space.regions()) {
        if (!region.writable || region.size() > kMaxRegionBytes) {
            continue;
        }
        ++stats.regions_scanned;

        for (auto base = region.begin; base < region.end; base += kChunkBytes - kOverlapBytes) {
            const auto want = std::min<std::size_t>(kChunkBytes, region.end - base);
            if (want < sizeof(RTTIClass)) {
                break;
            }
            if (!space.read(base, buffer.data(), want)) {
                continue;
            }
            stats.bytes_scanned += want;

            for (std::size_t offset = 0; offset + sizeof(RTTIClass) <= want; offset += 8) {
                const auto address = base + offset;
                const auto* candidate = buffer.data() + offset;

                RTTIClass klass{};
                std::memcpy(&klass, candidate, sizeof(klass));
                if (plausible_class(space, klass)) {
                    if (space.identifier(reinterpret_cast<std::uintptr_t>(klass.name))) {
                        unique.emplace(space.read_string(reinterpret_cast<std::uintptr_t>(klass.name)),
                                       Found{address, Kind::Class});
                    }
                    continue;
                }

                RTTIEnum enumeration{};
                std::memcpy(&enumeration, candidate, sizeof(enumeration));
                if (plausible_enum(enumeration)
                        && space.identifier(reinterpret_cast<std::uintptr_t>(enumeration.name))) {
                    unique.emplace(space.read_string(reinterpret_cast<std::uintptr_t>(enumeration.name)),
                                   Found{address, enumeration.kind});
                }
            }
        }
    }
    return unique;
}

std::atomic<bool> g_scanning{false};
std::atomic<const char*> g_stage{"idle"};

}

std::shared_ptr<const NameIndex>& type_slot() {
    static std::shared_ptr<const NameIndex> slot = std::make_shared<const NameIndex>();
    return slot;
}

std::shared_ptr<const NameIndex> type_index() {
    return std::atomic_load(&type_slot());
}

void publish_types(NameIndex index) {
    std::atomic_store(&type_slot(), std::make_shared<const NameIndex>(std::move(index)));
}

std::map<std::string, std::uintptr_t>& static_index() {
    static std::map<std::string, std::uintptr_t> index;
    return index;
}

std::size_t load_static_index(const std::filesystem::path& file, std::uintptr_t image_base) {
    auto& index = static_index();
    index.clear();
    std::ifstream stream(file);
    if (!stream.is_open()) {
        log::warn("no static type index at {}, find_type needs a scan first", file.string());
        return 0;
    }
    try {
        const auto document = nlohmann::json::parse(stream);
        for (const auto& [name, rva] : document.at("types").items()) {
            if (rva.is_number_unsigned()) {
                index.emplace(name, image_base + rva.get<std::uintptr_t>());
            }
        }
    } catch (const std::exception& error) {
        log::warn("static type index unreadable: {}", error.what());
        index.clear();
    }
    log::info("static type index: {} types", index.size());
    return index.size();
}

const void* find_type(const char* name) {
    const auto index = type_index();
    if (const auto found = index->find(name); found != index->end()) {
        return reinterpret_cast<const void*>(found->second);
    }
    const auto& fallback = static_index();
    const auto found = fallback.find(name);
    return found == fallback.end() ? nullptr : reinterpret_cast<const void*>(found->second);
}

std::size_t indexed_types() {
    return type_index()->size();
}

std::vector<std::string> type_names() {
    const auto scanned = type_index();
    const auto& shipped = static_index();
    std::vector<std::string> names;
    names.reserve(scanned->size() + shipped.size());
    for (const auto& [name, address] : shipped) {
        names.push_back(name);
    }
    for (const auto& [name, address] : *scanned) {
        if (!shipped.contains(name)) {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool wait_for_registration(const mem::Module& game, unsigned timeout_ms) {
    const auto anchor = reinterpret_cast<const volatile std::uint32_t*>(game.from_rva(kAnchorRva));
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed += 100) {
        const std::uint32_t id = *anchor;
        if (id != kUnregistered) {
            log::info("rtti registration observed after {} ms (anchor id {})", elapsed, id);
            Sleep(3000);
            return true;
        }
        Sleep(100);
    }
    log::warn("rtti registration not observed within {} ms", timeout_ms);
    return false;
}

DumpStats dump_types(const mem::Module& game, const std::filesystem::path& output) {
    DumpStats stats;

    proc::AddressSpace space;
    space.refresh();

    auto unique = collect_descriptors(space, stats);

    log::info("scan complete, {} candidates", unique.size());

    NameIndex index;
    for (const auto& [name, entry] : unique) {
        index.emplace(name, entry.address);
    }
    publish_types(index);

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    std::ofstream out(output, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        log::error("could not open {}", output.string());
        return stats;
    }

    const Resolver resolver(space);
    const auto image_end = game.base + game.size;

    out << "{\n \"image_base\": " << game.base << ",\n \"types\": {\n";
    bool first = true;
    for (const auto& [name, entry] : unique) {
        if (name.empty()) {
            continue;
        }

        const bool inside = entry.address >= game.base && entry.address < image_end;
        if (inside) {
            ++stats.in_image;
        } else {
            ++stats.runtime_only;
        }

        if (!first) {
            out << ",\n";
        }
        first = false;

        out << "  ";
        escape(out, name);
        out << ": {\"address\": " << entry.address;
        if (inside) {
            out << ", \"rva\": " << (entry.address - game.base);
        }

        if (entry.kind == Kind::Class) {
            RTTIClass type{};
            if (space.read(entry.address, type)) {
                ++stats.classes;
                write_class(out, space, resolver, type, stats);
            } else {
                out << "}";
            }
        } else {
            RTTIEnum type{};
            if (space.read(entry.address, type)) {
                ++stats.enums;
                write_enum(out, space, type);
            } else {
                out << "}";
            }
        }
    }
    out << "\n }\n}\n";

    return stats;
}


bool scanning() {
    return g_scanning.load(std::memory_order_acquire);
}

const char* scan_stage() {
    return g_stage.load(std::memory_order_relaxed);
}

void scan_types(const mem::Module& game) {
    DumpStats stats;
    proc::AddressSpace space;
    space.refresh();

    const auto unique = collect_descriptors(space, stats);

    NameIndex index;
    for (const auto& [name, entry] : unique) {
        if (!name.empty()) {
            index.emplace(name, entry.address);
        }
    }
    const auto count = index.size();
    publish_types(std::move(index));

    log::info("scan: {} types from {:.1f} MiB", count,
              static_cast<double>(stats.bytes_scanned) / (1024.0 * 1024.0));
    (void)game;
}

void start_scan(const mem::Module& game) {
    if (g_scanning.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::thread([game]() {
        g_stage.store("indexing types", std::memory_order_relaxed);
        scan_types(game);
        g_stage.store("indexing symbols", std::memory_order_relaxed);
        scan_symbols(game);
        g_stage.store("idle", std::memory_order_relaxed);
        g_scanning.store(false, std::memory_order_release);
        log::info("scan finished");
    }).detach();
}

bool describe_type(std::uintptr_t address, TypeDetail& out) {
    proc::AddressSpace space;
    const Resolver resolver(space);

    RTTI header{};
    if (!space.read(address, header)) {
        return false;
    }

    out = {};
    out.address = address;

    if (header.kind == Kind::Class) {
        RTTIClass type{};
        if (!space.read(address, type)) {
            return false;
        }
        out.kind = "class";
        out.name = space.read_string(reinterpret_cast<std::uintptr_t>(type.name));
        out.id = type.id;
        out.size = type.size;
        out.alignment = type.alignment;
        out.flags = type.flags;
        out.message_handlers = type.message_handler_count;
        out.constructor = reinterpret_cast<std::uintptr_t>(type.constructor);
        out.destructor = reinterpret_cast<std::uintptr_t>(type.destructor);

        const auto bases = reinterpret_cast<std::uintptr_t>(type.bases);
        for (unsigned i = 0; i < type.base_count; ++i) {
            RTTIBaseEntry entry{};
            if (!space.read(bases + i * sizeof(RTTIBaseEntry), entry)) {
                break;
            }
            out.bases.push_back({resolver.name_of(reinterpret_cast<std::uintptr_t>(entry.type)),
                                 entry.offset});
        }

        const auto members = reinterpret_cast<std::uintptr_t>(type.members);
        std::string category;
        for (unsigned i = 0; i < type.member_count; ++i) {
            RTTIMemberEntry member{};
            if (!space.read(members + i * sizeof(RTTIMemberEntry), member)) {
                break;
            }
            const auto label = space.read_string(reinterpret_cast<std::uintptr_t>(member.name));
            if (member.type == nullptr) {
                category = label;
                continue;
            }
            TypeMember entry;
            entry.name = label;
            entry.type = resolver.name_of(reinterpret_cast<std::uintptr_t>(member.type));
            entry.category = category;
            entry.offset = member.offset;
            entry.flags = member.flags;
            entry.getter = reinterpret_cast<std::uintptr_t>(member.getter);
            entry.setter = reinterpret_cast<std::uintptr_t>(member.setter);
            out.members.push_back(std::move(entry));
        }
        return true;
    }

    if (header.kind == Kind::Enum || header.kind == Kind::EnumFlags) {
        RTTIEnum type{};
        if (!space.read(address, type)) {
            return false;
        }
        out.kind = header.kind == Kind::EnumFlags ? "enum flags" : "enum";
        out.name = space.read_string(reinterpret_cast<std::uintptr_t>(type.name));
        out.id = type.id;
        out.size = type.size;

        const auto values = reinterpret_cast<std::uintptr_t>(type.values);
        for (unsigned i = 0; i < type.value_count; ++i) {
            RTTIEnumValue value{};
            if (!space.read(values + i * sizeof(RTTIEnumValue), value)) {
                break;
            }
            out.values.push_back({space.read_string(reinterpret_cast<std::uintptr_t>(value.name)),
                                  value.value});
        }
        return true;
    }

    return false;
}

}
