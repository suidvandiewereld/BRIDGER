#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <format>

#include "content/content.h"
#include "content/content_internal.h"
#include "core/memory.h"
#include "decima/dumper.h"
#include "overlay/inspect.h"
#include "script/bind.h"

namespace bridger::script {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool matches(std::string_view haystack, const std::vector<std::string>& words) {
    const auto text = lower(haystack);
    return std::all_of(words.begin(), words.end(),
                       [&](const std::string& word) { return text.find(word) != std::string::npos; });
}

std::vector<std::string> split_words(std::string_view pattern) {
    std::vector<std::string> words;
    std::string current;
    for (const char c : pattern) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                words.push_back(lower(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        words.push_back(lower(current));
    }
    return words;
}

void push_strings(lua_State* L, const std::vector<std::string>& values) {
    lua_createtable(L, static_cast<int>(values.size()), 0);
    lua_Integer i = 0;
    for (const auto& value : values) {
        lua_pushlstring(L, value.data(), value.size());
        lua_rawseti(L, -2, ++i);
    }
}

std::uintptr_t check_address(lua_State* L, int index) {
    std::uintptr_t address = 0;
    if (!to_address(L, index, address)) {
        luaL_error(L, "argument %d: expected an address or an engine object, got %s", index,
                   luaL_typename(L, index));
    }
    return address;
}

struct Entry {
    BridgerContentHandle handle = 0;
    char label[120]{};
};

int entry_revert(lua_State* L) {
    auto* entry = static_cast<Entry*>(luaL_checkudata(L, 1, kEntryMeta));
    lua_pushboolean(L, entry->handle != 0 && content::api()->revert(entry->handle));
    entry->handle = 0;
    return 1;
}

int entry_intact(lua_State* L) {
    auto* entry = static_cast<Entry*>(luaL_checkudata(L, 1, kEntryMeta));
    lua_pushboolean(L, entry->handle != 0 && content::api()->intact(entry->handle));
    return 1;
}

int entry_reapply(lua_State* L) {
    auto* entry = static_cast<Entry*>(luaL_checkudata(L, 1, kEntryMeta));
    lua_pushboolean(L, entry->handle != 0 && content::api()->reapply(entry->handle));
    return 1;
}

int entry_tostring(lua_State* L) {
    auto* entry = static_cast<Entry*>(luaL_checkudata(L, 1, kEntryMeta));
    lua_pushstring(L, std::format("{} ({})", entry->label,
                                  entry->handle == 0 ? "reverted" : "applied")
                          .c_str());
    return 1;
}

bool group_exists(const std::string& group) {
    const auto index = decima::symbol_index();
    const auto prefix = group + "::";
    const auto it = index->lower_bound(prefix);
    return it != index->end() && it->first.starts_with(prefix);
}

void push_group(lua_State* L, const std::string& group);

int group_index(lua_State* L) {
    const std::string group = lua_tostring(L, lua_upvalueindex(1));
    const char* name = luaL_checkstring(L, 2);
    const auto full = std::format("{}::{}", group, name);
    if (decima::find_symbol(group.c_str(), name) != nullptr) {
        std::string error;
        const auto* callable = callable_for_symbol(full, error);
        if (callable == nullptr) {
            return luaL_error(L, "%s", error.c_str());
        }
        push_callable(L, callable);
    } else if (group_exists(full)) {
        push_group(L, full);
    } else {
        std::string error;
        callable_for_symbol(full, error);
        return luaL_error(L, "%s", error.c_str());
    }
    lua_pushvalue(L, 2);
    lua_pushvalue(L, -2);
    lua_rawset(L, 1);
    return 1;
}

int group_list(lua_State* L) {
    const char* group = lua_tostring(L, lua_upvalueindex(1));
    const auto index = decima::symbol_index();
    const auto prefix = std::string(group) + "::";
    std::vector<std::string> names;
    for (auto it = index->lower_bound(prefix); it != index->end() && it->first.starts_with(prefix); ++it) {
        names.push_back(it->first.substr(prefix.size()));
    }
    push_strings(L, names);
    return 1;
}

int engine_index(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    if (std::string_view(key).find("::") != std::string_view::npos) {
        std::string error;
        const auto* callable = callable_for_symbol(key, error);
        if (callable == nullptr) {
            return luaL_error(L, "%s", error.c_str());
        }
        push_callable(L, callable);
        return 1;
    }
    if (!group_exists(key)) {
        const auto words = split_words(key);
        std::vector<std::string> close;
        const auto index = decima::symbol_index();
        std::string last;
        for (const auto& [name, address] : *index) {
            const auto group = name.substr(0, name.find("::"));
            if (group != last && matches(group, words)) {
                close.push_back(group);
                if (close.size() >= 5) {
                    break;
                }
            }
            last = group;
        }
        std::string hint;
        for (const auto& group : close) {
            hint += (hint.empty() ? " (close: " : ", ") + group;
        }
        return luaL_error(L, "no engine symbol group '%s'%s", key, hint.empty() ? "" : (hint + ")").c_str());
    }
    push_group(L, key);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, -2);
    lua_rawset(L, 1);
    return 1;
}

void push_group(lua_State* L, const std::string& group) {
    lua_newtable(L);
    lua_pushstring(L, group.c_str());
    lua_pushcclosure(L, group_list, 1);
    lua_setfield(L, -2, "list");
    lua_newtable(L);
    lua_pushstring(L, group.c_str());
    lua_pushcclosure(L, group_index, 1);
    lua_setfield(L, -2, "__index");
    lua_pushstring(L, group.c_str());
    lua_setfield(L, -2, "__name");
    lua_setmetatable(L, -2);
}

int engine_find(lua_State* L) {
    const auto words = split_words(luaL_checkstring(L, 1));
    const auto limit = static_cast<std::size_t>(luaL_optinteger(L, 2, 50));
    const auto index = decima::symbol_index();
    std::vector<std::string> found;
    for (const auto& [name, address] : *index) {
        if (matches(name, words)) {
            found.push_back(name);
            if (found.size() >= limit) {
                break;
            }
        }
    }
    push_strings(L, found);
    return 1;
}

int engine_groups(lua_State* L) {
    const auto index = decima::symbol_index();
    std::vector<std::string> groups;
    for (const auto& [name, address] : *index) {
        auto group = name.substr(0, name.find("::"));
        if (groups.empty() || groups.back() != group) {
            groups.push_back(std::move(group));
        }
    }
    push_strings(L, groups);
    return 1;
}

int lua_fn(lua_State* L) {
    void* address = nullptr;
    std::string label;
    if (lua_type(L, 1) == LUA_TSTRING) {
        const std::string key = lua_tostring(L, 1);
        const auto split = key.find("::");
        if (split == std::string::npos) {
            return luaL_error(L, "fn() takes an address or a Group::Name");
        }
        address = decima::find_symbol(key.substr(0, split).c_str(), key.substr(split + 2).c_str());
        if (address == nullptr) {
            return luaL_error(L, "no engine symbol %s", key.c_str());
        }
        label = key;
    } else {
        address = reinterpret_cast<void*>(check_address(L, 1));
        label = std::format("function at {:#x}", reinterpret_cast<std::uintptr_t>(address));
    }
    ffi::Signature signature;
    std::string error;
    if (!ffi::parse(luaL_checkstring(L, 2), type_info(), signature, error)) {
        return luaL_error(L, "%s", error.c_str());
    }
    if (!content::in_code(address)) {
        return luaL_error(L, "%p is not inside the game's code", address);
    }
    push_callable(L, make_callable(address, std::move(label), std::move(signature), true));
    return 1;
}

const reflect::Layout* optional_layout(lua_State* L, int index) {
    if (lua_isnoneornil(L, index)) {
        return nullptr;
    }
    const char* name = luaL_checkstring(L, index);
    const auto* layout = reflect::layout(name);
    if (layout == nullptr) {
        luaL_error(L, "no class named '%s'", name);
    }
    return layout;
}

int game_object(lua_State* L) {
    const auto address = check_address(L, 1);
    push_object(L, address, optional_layout(L, 2));
    return 1;
}

int game_struct(lua_State* L) {
    const auto address = check_address(L, 1);
    const auto* layout = optional_layout(L, 2);
    if (layout == nullptr) {
        return luaL_error(L, "game.struct(address, \"Type\") needs a type");
    }
    push_view(L, address, layout, false);
    return 1;
}

int game_new(lua_State* L) {
    const auto* layout = optional_layout(L, 1);
    if (layout == nullptr || layout->size == 0 || layout->size > (1u << 20)) {
        return luaL_error(L, "game.new(\"Type\") needs a sized struct type");
    }
    auto& view = push_owned(L, layout, layout->size, nullptr);
    if (lua_istable(L, 2)) {
        fill_struct(L, 2, view.address, layout);
    }
    return 1;
}

int game_type(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const auto* type = reflect::type(name);
    if (type->codec == reflect::Codec::Unknown && reflect::find(name) == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, type->size);
    lua_setfield(L, -2, "size");
    if (type->codec == reflect::Codec::Enum) {
        lua_pushstring(L, "enum");
        lua_setfield(L, -2, "kind");
        lua_newtable(L);
        for (const auto& [label, value] : reflect::enum_values(type->rtti)) {
            lua_pushinteger(L, value);
            lua_setfield(L, -2, label.c_str());
        }
        lua_setfield(L, -2, "values");
        return 1;
    }
    const auto* layout = reflect::layout(name);
    lua_pushstring(L, layout != nullptr ? "class" : "value");
    lua_setfield(L, -2, "kind");
    if (layout == nullptr) {
        return 1;
    }
    push_strings(L, layout->lineage);
    lua_setfield(L, -2, "lineage");
    push_strings(L, layout->messages);
    lua_setfield(L, -2, "messages");
    lua_createtable(L, static_cast<int>(layout->fields.size()), 0);
    lua_Integer i = 0;
    for (const auto& field : layout->fields) {
        lua_createtable(L, 0, 4);
        lua_pushstring(L, field.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, field.type_name.c_str());
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, field.offset);
        lua_setfield(L, -2, "offset");
        lua_pushstring(L, field.category.c_str());
        lua_setfield(L, -2, "category");
        lua_rawseti(L, -2, ++i);
    }
    lua_setfield(L, -2, "fields");
    return 1;
}

int game_types(lua_State* L) {
    const auto words = split_words(luaL_optstring(L, 1, ""));
    const auto limit = static_cast<std::size_t>(luaL_optinteger(L, 2, 100));
    std::vector<std::string> found;
    for (const auto& name : decima::type_names()) {
        if (matches(name, words)) {
            found.push_back(name);
            if (found.size() >= limit) {
                break;
            }
        }
    }
    push_strings(L, found);
    return 1;
}

int game_rtti(lua_State* L) {
    const void* rtti = reflect::find(luaL_checkstring(L, 1));
    if (rtti == nullptr) {
        return luaL_error(L, "no type named '%s'", lua_tostring(L, 1));
    }
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(rtti)));
    return 1;
}

int game_symbol(lua_State* L) {
    const std::string key = luaL_checkstring(L, 1);
    const auto split = key.find("::");
    void* address = split == std::string::npos
                        ? nullptr
                        : decima::find_symbol(key.substr(0, split).c_str(), key.substr(split + 2).c_str());
    if (address == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(address)));
    }
    return 1;
}

int game_rva(lua_State* L) {
    const auto rva = luaL_checkinteger(L, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(loader::Registry::instance().image_base())
                           + rva);
    return 1;
}

std::uintptr_t data_ref(const char* key, int index) {
    const std::string text(key);
    const auto split = text.find("::");
    if (split == std::string::npos) {
        return 0;
    }
    const auto* start = static_cast<const std::uint8_t*>(
        decima::find_symbol(text.substr(0, split).c_str(), text.substr(split + 2).c_str()));
    if (start == nullptr) {
        return 0;
    }
    std::uint8_t code[128]{};
    if (!reflect::read(reinterpret_cast<std::uintptr_t>(start), code, sizeof code)) {
        return 0;
    }
    int seen = 0;
    for (std::size_t i = 0; i + 10 < sizeof code;) {
        if (code[i] >= 0x48 && code[i] <= 0x4f && code[i + 1] == 0x8b) {
            const auto modrm = code[i + 2];
            if ((modrm >> 6) == 0 && (modrm & 7) == 5) {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, code + i + 3, sizeof displacement);
                const auto* target = start + i + 7 + displacement;
                const bool cookie = code[i + 7] == 0x48 && code[i + 8] == 0x33 && code[i + 9] == 0xc4;
                if (!cookie && content::in_image_data(target) && seen++ == index) {
                    return reinterpret_cast<std::uintptr_t>(target);
                }
                i += 7;
                continue;
            }
        }
        ++i;
    }
    return 0;
}

int game_data_ref(lua_State* L) {
    const auto address = data_ref(luaL_checkstring(L, 1), static_cast<int>(luaL_optinteger(L, 2, 0)));
    if (address == 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, static_cast<lua_Integer>(address));
    }
    return 1;
}

int game_singleton(lua_State* L) {
    const auto slot = data_ref(luaL_checkstring(L, 1), static_cast<int>(luaL_optinteger(L, 2, 0)));
    std::uintptr_t value = 0;
    if (slot == 0 || !reflect::read(slot, &value, 8)) {
        lua_pushnil(L);
        return 1;
    }
    push_object(L, value, optional_layout(L, 3));
    return 1;
}

int game_create(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    void* object = content::api()->create(name);
    if (object == nullptr) {
        return luaL_error(L, "%s could not be constructed (abstract, or not a class)", name);
    }
    push_object(L, reinterpret_cast<std::uintptr_t>(object), reflect::layout(name));
    if (lua_istable(L, 2)) {
        fill_struct(L, 2, reinterpret_cast<std::uintptr_t>(object), reflect::layout(name));
    }
    return 1;
}

int game_clone(lua_State* L) {
    const auto address = check_address(L, 1);
    void* object = content::api()->clone(reinterpret_cast<const void*>(address));
    if (object == nullptr) {
        return luaL_error(L, "the object at %p could not be cloned", reinterpret_cast<void*>(address));
    }
    push_object(L, reinterpret_cast<std::uintptr_t>(object), nullptr);
    if (lua_istable(L, 2)) {
        if (auto* view = to_view(L, -1); view != nullptr) {
            fill_struct(L, 2, view->address, view->layout);
        }
    }
    return 1;
}

int game_destroy(lua_State* L) {
    lua_pushboolean(L, content::api()->destroy(reinterpret_cast<void*>(check_address(L, 1))));
    return 1;
}

int game_patch(lua_State* L) {
    const auto address = check_address(L, 1);
    std::size_t size = 0;
    const char* bytes = luaL_checklstring(L, 2, &size);
    const auto handle = content::api()->patch(reinterpret_cast<void*>(address), bytes, size);
    if (handle == 0) {
        return luaL_error(L, "cannot patch %d byte(s) at %p", static_cast<int>(size),
                          reinterpret_cast<void*>(address));
    }
    push_entry(L, handle, std::format("{} byte(s) at {:#x}", size, address));
    return 1;
}

int game_inject(lua_State* L) {
    const auto owner = check_address(L, 1);
    const auto count_offset = static_cast<std::uintptr_t>(luaL_checkinteger(L, 2));
    const auto data_offset = static_cast<std::uintptr_t>(luaL_checkinteger(L, 3));
    luaL_checktype(L, 4, LUA_TTABLE);
    const bool persistent = lua_toboolean(L, 5) != 0;
    std::vector<void*> items;
    const auto count = luaL_len(L, 4);
    for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(L, 4, i);
        items.push_back(reinterpret_cast<void*>(check_address(L, -1)));
        lua_pop(L, 1);
    }
    if (items.empty()) {
        return luaL_error(L, "nothing to inject");
    }
    const auto handle = content::api()->inject(
        reinterpret_cast<std::int32_t*>(owner + count_offset), reinterpret_cast<void***>(owner + data_offset),
        items.data(), static_cast<std::uint32_t>(items.size()), persistent);
    if (handle == 0) {
        return luaL_error(L, "the injection was refused; see the log");
    }
    push_entry(L, handle, std::format("{} item(s) into {:#x}+{:#x}", items.size(), owner, count_offset));
    return 1;
}

int game_handler(lua_State* L) {
    const void* rtti = content::class_rtti(luaL_checkstring(L, 1));
    const void* message = decima::find_type(luaL_checkstring(L, 2));
    const void* handler = content::handler_for_rtti(rtti, message);
    if (handler == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(handler)));
    }
    return 1;
}

int game_vtable(lua_State* L) {
    void** table = content::api()->vtable(luaL_checkstring(L, 1));
    if (table == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(table)));
    }
    return 1;
}

int game_inspect(lua_State* L) {
    overlay::inspect::focus(check_address(L, 1));
    lua_settop(L, 1);
    return 1;
}

int game_on_game_thread(lua_State* L) {
    lua_pushboolean(L, loader::on_game_thread());
    return 1;
}

int game_scan(lua_State* L) {
    const mem::Pattern pattern(luaL_checkstring(L, 1));
    if (pattern.empty()) {
        return luaL_error(L, "not a byte pattern; write it like \"48 8B ?? 05\"");
    }
    const auto found = pattern.scan(content::game_module());
    if (found == 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, static_cast<lua_Integer>(found));
    }
    return 1;
}

int enum_type_index(lua_State* L) {
    const char* type_name = lua_tostring(L, lua_upvalueindex(1));
    const char* label = luaL_checkstring(L, 2);
    const auto* type = reflect::type(type_name);
    std::int64_t value = 0;
    if (!reflect::enum_value(type->rtti, label, value)) {
        std::string names;
        for (const auto& [name, number] : reflect::enum_values(type->rtti)) {
            if (names.size() > 300) {
                names += ", ...";
                break;
            }
            names += (names.empty() ? "" : ", ") + name;
        }
        return luaL_error(L, "%s has no value '%s' (values: %s)", type_name, label, names.c_str());
    }
    lua_pushinteger(L, value);
    return 1;
}

int enum_index(lua_State* L) {
    const char* type_name = luaL_checkstring(L, 2);
    const auto* type = reflect::type(type_name);
    if (type->codec != reflect::Codec::Enum) {
        return luaL_error(L, "'%s' is not an enum", type_name);
    }
    lua_newtable(L);
    for (const auto& [name, value] : reflect::enum_values(type->rtti)) {
        lua_pushinteger(L, value);
        lua_setfield(L, -2, name.c_str());
    }
    lua_newtable(L);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, enum_type_index, 1);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, -2);
    lua_rawset(L, 1);
    return 1;
}

int enum_name(lua_State* L) {
    const auto* type = reflect::type(luaL_checkstring(L, 1));
    if (type->codec != reflect::Codec::Enum) {
        return luaL_error(L, "'%s' is not an enum", lua_tostring(L, 1));
    }
    const auto name = reflect::enum_name(type->rtti, luaL_checkinteger(L, 2));
    if (name.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, name.c_str());
    }
    return 1;
}

int mem_read(lua_State* L) {
    const auto address = check_address(L, 1);
    const std::string_view name = luaL_checkstring(L, 2);
    if (name == "ptr" || name == "pointer") {
        std::uintptr_t value = 0;
        if (!reflect::read(address, &value, 8)) {
            return luaL_error(L, "cannot read %p", reinterpret_cast<void*>(address));
        }
        lua_pushinteger(L, static_cast<lua_Integer>(value));
        return 1;
    }
    push_value(L, address, reflect::type(name));
    return 1;
}

int mem_write(lua_State* L) {
    const auto address = check_address(L, 1);
    const std::string_view name = luaL_checkstring(L, 2);
    const auto* type = reflect::type(name);
    if (name == "ptr" || name == "pointer" || type->codec == reflect::Codec::Pointer) {
        const auto value = check_address(L, 3);
        if (!reflect::write(address, &value, 8)) {
            return luaL_error(L, "cannot write %p", reinterpret_cast<void*>(address));
        }
        return 0;
    }
    write_value(L, 3, address, type, name);
    return 0;
}

int mem_bytes(lua_State* L) {
    const auto address = check_address(L, 1);
    const auto count = static_cast<std::size_t>(luaL_checkinteger(L, 2));
    if (count > (16u << 20)) {
        return luaL_error(L, "that is more than 16 MiB");
    }
    std::string bytes(count, '\0');
    if (!reflect::read(address, bytes.data(), count)) {
        return luaL_error(L, "cannot read %d byte(s) at %p", static_cast<int>(count),
                          reinterpret_cast<void*>(address));
    }
    lua_pushlstring(L, bytes.data(), bytes.size());
    return 1;
}

int mem_poke(lua_State* L) {
    const auto address = check_address(L, 1);
    std::size_t size = 0;
    const char* bytes = luaL_checklstring(L, 2, &size);
    lua_pushboolean(L, reflect::write(address, bytes, size));
    return 1;
}

int mem_string(lua_State* L) {
    const auto text = reflect::read_c_string(check_address(L, 1),
                                             static_cast<std::size_t>(luaL_optinteger(L, 2, 4096)));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int mem_wstring(lua_State* L) {
    const auto text = reflect::read_wide_string(check_address(L, 1),
                                                static_cast<std::size_t>(luaL_optinteger(L, 2, 4096)));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int mem_alloc(lua_State* L) {
    const auto size = luaL_checkinteger(L, 1);
    if (size <= 0 || size > (1 << 24)) {
        return luaL_error(L, "mem.alloc takes 1 byte to 16 MiB");
    }
    push_owned(L, optional_layout(L, 2), static_cast<std::uint32_t>(size), nullptr);
    return 1;
}

int mem_readable(lua_State* L) {
    const auto address = check_address(L, 1);
    const auto size = static_cast<std::size_t>(luaL_optinteger(L, 2, 1));
    std::vector<std::uint8_t> probe(std::min<std::size_t>(size, 1 << 20));
    lua_pushboolean(L, !probe.empty() && reflect::read(address, probe.data(), probe.size()));
    return 1;
}

void set_functions(lua_State* L, std::initializer_list<luaL_Reg> functions) {
    for (const auto& entry : functions) {
        lua_pushcfunction(L, entry.func);
        lua_setfield(L, -2, entry.name);
    }
}

}

void push_entry(lua_State* L, std::uint32_t handle, std::string label) {
    auto* entry = static_cast<Entry*>(lua_newuserdatauv(L, sizeof(Entry), 0));
    entry->handle = handle;
    const auto length = std::min(label.size(), sizeof entry->label - 1);
    std::memcpy(entry->label, label.data(), length);
    entry->label[length] = 0;
    luaL_setmetatable(L, kEntryMeta);
}

void open_engine(lua_State* L) {
    open_values(L);
    open_hooks(L);

    luaL_newmetatable(L, kEntryMeta);
    lua_newtable(L);
    set_functions(L, {{"revert", entry_revert}, {"intact", entry_intact}, {"reapply", entry_reapply}});
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, entry_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    lua_newtable(L);
    set_functions(L, {{"find", engine_find}, {"groups", engine_groups}});
    lua_newtable(L);
    lua_pushcfunction(L, engine_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "engine");

    lua_pushcfunction(L, lua_fn);
    lua_setglobal(L, "fn");

    lua_newtable(L);
    set_functions(L, {{"object", game_object},
                      {"struct", game_struct},
                      {"new", game_new},
                      {"type", game_type},
                      {"types", game_types},
                      {"rtti", game_rtti},
                      {"symbol", game_symbol},
                      {"rva", game_rva},
                      {"data_ref", game_data_ref},
                      {"singleton", game_singleton},
                      {"create", game_create},
                      {"clone", game_clone},
                      {"destroy", game_destroy},
                      {"patch", game_patch},
                      {"inject", game_inject},
                      {"handler", game_handler},
                      {"vtable", game_vtable},
                      {"inspect", game_inspect},
                      {"on_game_thread", game_on_game_thread},
                      {"scan", game_scan}});
    lua_pushinteger(L, static_cast<lua_Integer>(loader::Registry::instance().image_base()));
    lua_setfield(L, -2, "base");
    lua_setglobal(L, "game");

    lua_newtable(L);
    set_functions(L, {{"name", enum_name}});
    lua_newtable(L);
    lua_pushcfunction(L, enum_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "enum");

    lua_newtable(L);
    set_functions(L, {{"read", mem_read},
                      {"write", mem_write},
                      {"bytes", mem_bytes},
                      {"poke", mem_poke},
                      {"string", mem_string},
                      {"wstring", mem_wstring},
                      {"alloc", mem_alloc},
                      {"readable", mem_readable},
                      {"scan", game_scan},
                      {"patch", game_patch}});
    lua_setglobal(L, "mem");
}

}
