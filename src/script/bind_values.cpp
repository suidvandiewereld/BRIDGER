#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "content/content.h"
#include "content/content_internal.h"
#include "core/log.h"
#include "decima/dumper.h"
#include "overlay/inspect.h"
#include "script/bind.h"

namespace bridger::script {
namespace {

int g_skip_tag = 0;
int g_out_tag = 0;

std::once_flag g_signatures_once;
std::unordered_map<std::string, std::vector<std::string>> g_signatures;

void load_signatures() {
    std::call_once(g_signatures_once, [] {
        const auto path = root() / "signatures.json";
        std::ifstream stream(path);
        if (!stream.is_open()) {
            log::warn("script: no {}; engine functions need an explicit signature", path.string());
            return;
        }
        try {
            const auto document = nlohmann::json::parse(stream);
            for (const auto& [key, tokens] : document.at("signatures").items()) {
                g_signatures.emplace(key, tokens.get<std::vector<std::string>>());
            }
            log::info("script: {} engine function signatures", g_signatures.size());
        } catch (const std::exception& error) {
            log::warn("script: signatures.json unreadable: {}", error.what());
        }
    });
}

std::mutex g_callables_mutex;
std::map<std::string, std::unique_ptr<Callable>> g_callables;

const reflect::Layout* layout_for(const ffi::Type& type) {
    if (type.name.empty()) {
        return nullptr;
    }
    return reflect::layout(type.name);
}

[[noreturn]] void raise(lua_State* L, const std::string& message) {
    lua_pushlstring(L, message.data(), message.size());
    lua_error(L);
    std::abort();
}

std::string view_name(const View& view) {
    return view.layout != nullptr ? view.layout->name : std::string("memory");
}

std::string type_name_of(lua_State* L, int index) {
    if (auto* view = to_view(L, index); view != nullptr) {
        return view_name(*view);
    }
    if (luaL_testudata(L, index, kArrayMeta) != nullptr) {
        return "Array";
    }
    return luaL_typename(L, index);
}

bool read_bytes(std::uintptr_t address, void* out, std::size_t size) {
    return reflect::read(address, out, size);
}

bool try_push_value(lua_State* L, std::uintptr_t address, const reflect::Type* type,
                    std::string& error) {
    using reflect::Codec;
    switch (type->codec) {
        case Codec::Bool: {
            std::uint8_t value = 0;
            if (!read_bytes(address, &value, 1)) break;
            lua_pushboolean(L, value != 0);
            return true;
        }
        case Codec::I8: case Codec::U8: case Codec::I16: case Codec::U16:
        case Codec::I32: case Codec::U32: case Codec::I64: case Codec::U64:
        case Codec::Enum: {
            std::int64_t value = 0;
            if (!reflect::read_integer(address, type->codec, type->size, value)) break;
            lua_pushinteger(L, value);
            return true;
        }
        case Codec::F32: {
            float value = 0.0f;
            if (!read_bytes(address, &value, 4)) break;
            lua_pushnumber(L, value);
            return true;
        }
        case Codec::F64: {
            double value = 0.0;
            if (!read_bytes(address, &value, 8)) break;
            lua_pushnumber(L, value);
            return true;
        }
        case Codec::String:
        case Codec::WString: {
            std::uintptr_t pointer = 0;
            if (!read_bytes(address, &pointer, 8)) break;
            const auto text = pointer == 0 ? std::string()
                            : type->codec == Codec::String ? reflect::read_c_string(pointer)
                                                           : reflect::read_wide_string(pointer);
            lua_pushlstring(L, text.data(), text.size());
            return true;
        }
        case Codec::Uuid: {
            std::uint8_t bytes[16]{};
            if (!read_bytes(address, bytes, 16)) break;
            const auto text = reflect::format_uuid(bytes);
            lua_pushlstring(L, text.data(), text.size());
            return true;
        }
        case Codec::Pointer: {
            std::uintptr_t pointer = 0;
            if (!read_bytes(address, &pointer, 8)) break;
            const reflect::Layout* layout =
                type->item != nullptr && type->item->codec == Codec::Struct
                    ? reflect::layout(type->item->rtti)
                    : nullptr;
            push_object(L, pointer, layout);
            return true;
        }
        case Codec::Handle: {
            std::uintptr_t pointer = 0;
            if (!read_bytes(address, &pointer, 8)) break;
            lua_pushinteger(L, static_cast<lua_Integer>(pointer));
            return true;
        }
        case Codec::Array:
            push_array(L, address, type);
            return true;
        case Codec::Struct:
            push_view(L, address, reflect::layout(type->rtti), false);
            return true;
        case Codec::Unknown:
            lua_pushinteger(L, static_cast<lua_Integer>(address));
            return true;
    }
    error = std::format("cannot read {} at {:#x}; the object may be gone", type->name, address);
    return false;
}

void write_scalar(lua_State* L, int index, std::uintptr_t address, const reflect::Type* type,
                  std::string_view what) {
    using reflect::Codec;
    bool ok = false;
    switch (type->codec) {
        case Codec::Bool: {
            const std::uint8_t value = lua_toboolean(L, index) ? 1 : 0;
            ok = reflect::write(address, &value, 1);
            break;
        }
        case Codec::F32: {
            const float value = static_cast<float>(luaL_checknumber(L, index));
            ok = reflect::write(address, &value, 4);
            break;
        }
        case Codec::F64: {
            const double value = luaL_checknumber(L, index);
            ok = reflect::write(address, &value, 8);
            break;
        }
        case Codec::Enum: {
            std::int64_t value = 0;
            if (lua_type(L, index) == LUA_TSTRING) {
                const char* label = lua_tostring(L, index);
                if (!reflect::enum_value(type->rtti, label, value)) {
                    raise(L, std::format("{} has no value named '{}'", type->name, label));
                }
            } else {
                value = luaL_checkinteger(L, index);
            }
            ok = reflect::write_integer(address, Codec::Enum, type->size, value);
            break;
        }
        default: {
            std::int64_t value = 0;
            if (lua_isinteger(L, index)) {
                value = lua_tointeger(L, index);
            } else if (lua_type(L, index) == LUA_TNUMBER) {
                value = static_cast<std::int64_t>(lua_tonumber(L, index));
            } else if (lua_type(L, index) == LUA_TBOOLEAN) {
                value = lua_toboolean(L, index);
            } else {
                raise(L, std::format("{} expects a number, got {}", what, type_name_of(L, index)));
            }
            ok = reflect::write_integer(address, type->codec, type->size, value);
            break;
        }
    }
    if (!ok) {
        raise(L, std::format("cannot write {} at {:#x}", what, address));
    }
}

bool try_push_property(lua_State* L, const View& view, const reflect::Field& field, std::string& error) {
    std::uint8_t buffer[512]{};
    const std::uint64_t ints[4] = {view.address, reinterpret_cast<std::uint64_t>(buffer), 0, 0};
    const double xmms[4] = {};
    ffi::Result result;
    if (ffi::call(reinterpret_cast<void*>(field.getter), ints, xmms, 2, result) != 0) {
        error = std::format("the getter of {} faulted", field.name);
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(buffer);
    if (field.type->codec == reflect::Codec::Struct) {
        push_owned(L, reflect::layout(field.type->rtti),
                   std::clamp<std::uint32_t>(field.type->size, 1, sizeof buffer), buffer);
        return true;
    }
    if (field.type->codec == reflect::Codec::Array) {
        error = std::format("{} is an array property, which scripts cannot read", field.name);
        return false;
    }
    return try_push_value(L, address, field.type, error);
}

void write_property(lua_State* L, int index, const View& view, const reflect::Field& field,
                    std::string_view what) {
    if (field.setter == 0) {
        raise(L, std::format("{} is a read-only property", what));
    }
    if (field.type->size > 256) {
        raise(L, std::format("{} is too large to set from a script", what));
    }
    std::uint8_t buffer[512]{};
    if (field.getter != 0) {
        const std::uint64_t get_ints[4] = {view.address, reinterpret_cast<std::uint64_t>(buffer), 0, 0};
        const double xmms[4] = {};
        ffi::Result ignored;
        ffi::call(reinterpret_cast<void*>(field.getter), get_ints, xmms, 2, ignored);
    }
    write_value(L, index, reinterpret_cast<std::uintptr_t>(buffer), field.type, what);
    const std::uint64_t ints[4] = {view.address, reinterpret_cast<std::uint64_t>(buffer), 0, 0};
    const double xmms[4] = {};
    ffi::Result result;
    if (ffi::call(reinterpret_cast<void*>(field.setter), ints, xmms, 2, result) != 0) {
        raise(L, std::format("the setter of {} faulted", what));
    }
}

int read_field(lua_State* L, const View& view, const char* key) {
    if (view.layout == nullptr) {
        raise(L, std::format("no reflected type at {:#x}; use mem.read or game.struct", view.address));
    }
    const auto* field = view.layout->find(key);
    if (field == nullptr) {
        const auto closest = view.layout->suggest(key);
        raise(L, std::format("{} has no field '{}'{}", view.layout->name, key,
                             closest.empty() ? "" : std::format(" (did you mean '{}'?)", closest)));
    }
    std::string error;
    const bool ok = field->getter != 0 ? try_push_property(L, view, *field, error)
                                       : try_push_value(L, view.address + field->offset, field->type, error);
    if (!ok) {
        raise(L, std::format("{}.{}: {}", view.layout->name, key, error));
    }
    return 1;
}

int view_index(lua_State* L) {
    auto& view = check_view(L, 1);
    if (lua_type(L, 2) == LUA_TSTRING) {
        lua_pushvalue(L, 2);
        if (lua_rawget(L, lua_upvalueindex(1)) != LUA_TNIL) {
            return 1;
        }
        lua_pop(L, 1);
        return read_field(L, view, lua_tostring(L, 2));
    }
    raise(L, std::format("{} fields are indexed by name", view_name(view)));
}

int view_newindex(lua_State* L) {
    auto& view = check_view(L, 1);
    const char* key = luaL_checkstring(L, 2);
    if (view.layout == nullptr) {
        raise(L, std::format("no reflected type at {:#x}", view.address));
    }
    const auto* field = view.layout->find(key);
    if (field == nullptr) {
        const auto closest = view.layout->suggest(key);
        raise(L, std::format("{} has no field '{}'{}", view.layout->name, key,
                             closest.empty() ? "" : std::format(" (did you mean '{}'?)", closest)));
    }
    const auto what = std::format("{}.{}", view.layout->name, key);
    if (field->getter != 0 || field->setter != 0) {
        write_property(L, 3, view, *field, what);
        return 0;
    }
    write_value(L, 3, view.address + field->offset, field->type, what);
    return 0;
}

int view_tostring(lua_State* L) {
    auto& view = check_view(L, 1);
    const auto* layout = view.layout;
    if (layout != nullptr && !view.object && !layout->fields.empty() && layout->fields.size() <= 4
        && std::all_of(layout->fields.begin(), layout->fields.end(),
                       [](const reflect::Field& f) { return f.type->number(); })) {
        std::string text = layout->name + "(";
        for (std::size_t i = 0; i < layout->fields.size(); ++i) {
            const auto& field = layout->fields[i];
            std::string error;
            std::string part = "?";
            if (try_push_value(L, view.address + field.offset, field.type, error)) {
                part = lua_isinteger(L, -1) ? std::format("{}", lua_tointeger(L, -1))
                                            : std::format("{:.4g}", lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            text += (i == 0 ? "" : ", ") + part;
        }
        text += ")";
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }
    const auto text = std::format("{}@{:#x}", view_name(view), view.address);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int view_eq(lua_State* L) {
    auto* a = to_view(L, 1);
    auto* b = to_view(L, 2);
    lua_pushboolean(L, a != nullptr && b != nullptr && a->address == b->address);
    return 1;
}

int view_pairs_next(lua_State* L) {
    auto& view = check_view(L, 1);
    if (view.layout == nullptr) {
        return 0;
    }
    std::size_t next = 0;
    if (!lua_isnil(L, 2)) {
        const auto* field = view.layout->find(luaL_checkstring(L, 2));
        next = field != nullptr ? static_cast<std::size_t>(field - view.layout->fields.data()) + 1
                                : view.layout->fields.size();
    }
    for (; next < view.layout->fields.size(); ++next) {
        const auto& field = view.layout->fields[next];
        lua_pushstring(L, field.name.c_str());
        std::string error;
        const bool ok = field.getter != 0 ? try_push_property(L, view, field, error)
                                          : try_push_value(L, view.address + field.offset, field.type, error);
        if (!ok) {
            lua_pushnil(L);
        }
        return 2;
    }
    return 0;
}

int view_pairs(lua_State* L) {
    check_view(L, 1);
    lua_pushcfunction(L, view_pairs_next);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

int method_type(lua_State* L) {
    auto& view = check_view(L, 1);
    lua_pushstring(L, view_name(view).c_str());
    return 1;
}

int method_address(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(check_view(L, 1).address));
    return 1;
}

int method_is_a(lua_State* L) {
    auto& view = check_view(L, 1);
    const char* name = luaL_checkstring(L, 2);
    if (view.layout == nullptr) {
        lua_pushboolean(L, false);
        return 1;
    }
    const auto& lineage = view.layout->lineage;
    lua_pushboolean(L, std::find(lineage.begin(), lineage.end(), name) != lineage.end());
    return 1;
}

int method_valid(lua_State* L) {
    auto& view = check_view(L, 1);
    if (view.owned != 0) {
        lua_pushboolean(L, true);
    } else if (view.object) {
        const void* rtti = reflect::rtti_of(view.address);
        lua_pushboolean(L, rtti != nullptr && view.layout != nullptr && rtti == view.layout->rtti);
    } else {
        std::uint8_t probe = 0;
        lua_pushboolean(L, reflect::read(view.address, &probe, 1));
    }
    return 1;
}

int method_fields(lua_State* L) {
    auto& view = check_view(L, 1);
    lua_newtable(L);
    if (view.layout == nullptr) {
        return 1;
    }
    lua_Integer i = 0;
    for (const auto& field : view.layout->fields) {
        lua_createtable(L, 0, 5);
        lua_pushstring(L, field.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, field.type_name.c_str());
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, field.offset);
        lua_setfield(L, -2, "offset");
        lua_pushstring(L, field.category.c_str());
        lua_setfield(L, -2, "category");
        lua_pushboolean(L, field.getter != 0);
        lua_setfield(L, -2, "property");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

int method_get(lua_State* L) {
    auto& view = check_view(L, 1);
    return read_field(L, view, luaL_checkstring(L, 2));
}

int method_set(lua_State* L) {
    check_view(L, 1);
    luaL_checkstring(L, 2);
    lua_settop(L, 3);
    view_newindex(L);
    lua_settop(L, 1);
    return 1;
}

int method_field_address(lua_State* L) {
    auto& view = check_view(L, 1);
    const char* key = luaL_checkstring(L, 2);
    const auto* field = view.layout != nullptr ? view.layout->find(key) : nullptr;
    if (field == nullptr) {
        raise(L, std::format("{} has no field '{}'", view_name(view), key));
    }
    lua_pushinteger(L, static_cast<lua_Integer>(view.address + field->offset));
    return 1;
}

int method_table(lua_State* L) {
    auto& view = check_view(L, 1);
    lua_newtable(L);
    if (view.layout == nullptr) {
        return 1;
    }
    for (const auto& field : view.layout->fields) {
        std::string error;
        const bool ok = field.getter != 0 ? try_push_property(L, view, field, error)
                                          : try_push_value(L, view.address + field.offset, field.type, error);
        if (ok) {
            lua_setfield(L, -2, field.name.c_str());
        }
    }
    return 1;
}

int method_cast(lua_State* L) {
    auto& view = check_view(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const auto* layout = reflect::layout(name);
    if (layout == nullptr) {
        raise(L, std::format("no class named '{}'", name));
    }
    push_view(L, view.address, layout, false);
    return 1;
}

int method_copy(lua_State* L) {
    auto& view = check_view(L, 1);
    const auto size = view.owned != 0 ? view.owned
                    : view.layout != nullptr ? view.layout->size : 0;
    if (size == 0 || size > (1u << 20)) {
        raise(L, std::format("{} has no size to copy", view_name(view)));
    }
    std::vector<std::uint8_t> bytes(size);
    if (!reflect::read(view.address, bytes.data(), size)) {
        raise(L, std::format("cannot read {} at {:#x}", view_name(view), view.address));
    }
    push_owned(L, view.layout, size, bytes.data());
    return 1;
}

int method_inspect(lua_State* L) {
    auto& view = check_view(L, 1);
    overlay::inspect::focus(view.address);
    lua_settop(L, 1);
    return 1;
}

int method_read(lua_State* L) {
    auto& view = check_view(L, 1);
    const auto offset = luaL_checkinteger(L, 2);
    const auto* type = reflect::type(luaL_checkstring(L, 3));
    std::string error;
    if (!try_push_value(L, view.address + static_cast<std::uintptr_t>(offset), type, error)) {
        raise(L, error);
    }
    return 1;
}

int method_write(lua_State* L) {
    auto& view = check_view(L, 1);
    const auto offset = luaL_checkinteger(L, 2);
    const char* name = luaL_checkstring(L, 3);
    write_value(L, 4, view.address + static_cast<std::uintptr_t>(offset), reflect::type(name), name);
    return 0;
}

int method_handler(lua_State* L) {
    auto& view = check_view(L, 1);
    const char* message = luaL_checkstring(L, 2);
    const void* rtti = view.object ? reflect::rtti_of(view.address)
                     : view.layout != nullptr ? view.layout->rtti : nullptr;
    const void* message_rtti = reflect::find(message);
    const void* handler = content::handler_for_rtti(rtti, message_rtti);
    if (handler == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(handler)));
    }
    return 1;
}

int method_messages(lua_State* L) {
    auto& view = check_view(L, 1);
    lua_newtable(L);
    if (view.layout == nullptr) {
        return 1;
    }
    lua_Integer i = 0;
    std::vector<std::string> seen;
    for (const auto& name : view.layout->lineage) {
        const auto* layout = reflect::layout(name);
        if (layout == nullptr) {
            continue;
        }
        for (const auto& message : layout->messages) {
            if (std::find(seen.begin(), seen.end(), message) != seen.end()) {
                continue;
            }
            seen.push_back(message);
            lua_pushstring(L, message.c_str());
            lua_rawseti(L, -2, ++i);
        }
    }
    return 1;
}

struct MessageObject {
    const void* rtti = nullptr;
    void* memory = nullptr;

    ~MessageObject() {
        if (memory == nullptr) {
            return;
        }
        const auto destructor = content::read_at<void*>(rtti, content::kClassDestructor);
        if (content::in_code(destructor)) {
            struct Work {
                void* dtor;
                const void* rtti;
                void* object;
            } work{destructor, rtti, memory};
            guarded_call(
                [](void* user) {
                    auto& w = *static_cast<Work*>(user);
                    reinterpret_cast<void (*)(const void*, void*)>(w.dtor)(w.rtti, w.object);
                },
                &work);
        }
        _aligned_free(memory);
    }
};

int method_send(lua_State* L) {
    auto& view = check_view(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const void* rtti = content::class_rtti(name);
    if (rtti == nullptr) {
        raise(L, std::format("no message class named '{}'", name));
    }
    MessageObject message;
    message.rtti = rtti;
    std::size_t bytes = 0;
    message.memory = content::allocate_object(rtti, bytes);
    if (message.memory == nullptr || !content::run_constructor(rtti, message.memory)) {
        if (message.memory != nullptr) {
            _aligned_free(message.memory);
            message.memory = nullptr;
        }
        raise(L, std::format("{} could not be constructed", name));
    }
    if (lua_istable(L, 3)) {
        fill_struct(L, 3, reinterpret_cast<std::uintptr_t>(message.memory), reflect::layout(rtti));
    }
    const bool delivered = content::api()->deliver(reinterpret_cast<void*>(view.address), message.memory);
    lua_pushboolean(L, delivered);
    return 1;
}

int method_clone(lua_State* L) {
    auto& view = check_view(L, 1);
    void* copy = content::api()->clone(reinterpret_cast<const void*>(view.address));
    if (copy == nullptr) {
        raise(L, std::format("{} could not be cloned", view_name(view)));
    }
    push_object(L, reinterpret_cast<std::uintptr_t>(copy), nullptr);
    return 1;
}

int method_vtable(lua_State* L) {
    auto& view = check_view(L, 1);
    std::uintptr_t table = 0;
    reflect::read(view.address, &table, 8);
    lua_pushinteger(L, static_cast<lua_Integer>(table));
    return 1;
}

struct ArrayHeader {
    std::uint32_t count = 0;
    std::uint32_t capacity = 0;
    std::uintptr_t data = 0;
};

ArrayView& check_array(lua_State* L, int index) {
    return *static_cast<ArrayView*>(luaL_checkudata(L, index, kArrayMeta));
}

ArrayHeader read_header(lua_State* L, const ArrayView& array) {
    ArrayHeader header;
    if (!reflect::read(array.address, &header, sizeof header)) {
        raise(L, std::format("cannot read {} at {:#x}", array.type->name, array.address));
    }
    return header;
}

std::uintptr_t element_address(lua_State* L, const ArrayView& array, lua_Integer index,
                               const ArrayHeader& header) {
    const auto* item = array.type->item;
    if (item == nullptr || item->size == 0) {
        raise(L, std::format("{} has elements of unknown size", array.type->name));
    }
    if (index < 1 || static_cast<std::uint64_t>(index) > header.count) {
        raise(L, std::format("index {} is outside {} of {} element(s)", index, array.type->name,
                             header.count));
    }
    return header.data + static_cast<std::uintptr_t>(index - 1) * item->size;
}

int array_len(lua_State* L) {
    const auto& array = check_array(L, 1);
    lua_pushinteger(L, read_header(L, array).count);
    return 1;
}

int array_index(lua_State* L) {
    const auto& array = check_array(L, 1);
    if (lua_type(L, 2) == LUA_TSTRING) {
        lua_pushvalue(L, 2);
        if (lua_rawget(L, lua_upvalueindex(1)) != LUA_TNIL) {
            return 1;
        }
        raise(L, std::format("{} has no method '{}'", array.type->name, lua_tostring(L, 2)));
    }
    const auto header = read_header(L, array);
    const auto index = luaL_checkinteger(L, 2);
    if (index < 1 || static_cast<std::uint64_t>(index) > header.count) {
        lua_pushnil(L);
        return 1;
    }
    std::string error;
    if (!try_push_value(L, element_address(L, array, index, header), array.type->item, error)) {
        raise(L, error);
    }
    return 1;
}

int array_newindex(lua_State* L) {
    const auto& array = check_array(L, 1);
    const auto header = read_header(L, array);
    const auto address = element_address(L, array, luaL_checkinteger(L, 2), header);
    write_value(L, 3, address, array.type->item, array.type->name + " element");
    return 0;
}

int array_tostring(lua_State* L) {
    const auto& array = check_array(L, 1);
    ArrayHeader header;
    reflect::read(array.address, &header, sizeof header);
    lua_pushstring(L, std::format("{}[{}]@{:#x}", array.type->name, header.count, array.address).c_str());
    return 1;
}

int array_next(lua_State* L) {
    const auto& array = check_array(L, 1);
    const auto index = luaL_optinteger(L, 2, 0) + 1;
    const auto header = read_header(L, array);
    if (index > static_cast<lua_Integer>(header.count)) {
        return 0;
    }
    lua_pushinteger(L, index);
    std::string error;
    if (!try_push_value(L, element_address(L, array, index, header), array.type->item, error)) {
        lua_pushnil(L);
    }
    return 2;
}

int array_pairs(lua_State* L) {
    check_array(L, 1);
    lua_pushcfunction(L, array_next);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    return 3;
}

int array_count(lua_State* L) {
    return array_len(L);
}

int array_data(lua_State* L) {
    const auto& array = check_array(L, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(read_header(L, array).data));
    return 1;
}

int array_table(lua_State* L) {
    const auto& array = check_array(L, 1);
    const auto header = read_header(L, array);
    lua_createtable(L, static_cast<int>(std::min<std::uint32_t>(header.count, 1u << 16)), 0);
    for (lua_Integer i = 1; i <= static_cast<lua_Integer>(header.count) && i <= (1 << 16); ++i) {
        std::string error;
        if (try_push_value(L, element_address(L, array, i, header), array.type->item, error)) {
            lua_rawseti(L, -2, i);
        }
    }
    return 1;
}

int array_address(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(check_array(L, 1).address));
    return 1;
}

int function_call(lua_State* L) {
    const auto* callable = to_callable(L, 1);
    return call_native(L, *callable, 2);
}

int function_tostring(lua_State* L) {
    const auto* callable = to_callable(L, 1);
    const auto text = std::format("{} @ {:#x} : {}", callable->label,
                                  reinterpret_cast<std::uintptr_t>(callable->address),
                                  callable->known ? callable->signature.describe()
                                                  : std::string("unknown signature"));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int function_index(lua_State* L) {
    const auto* callable = to_callable(L, 1);
    const char* key = luaL_checkstring(L, 2);
    const std::string_view name = key;
    if (name == "address") {
        lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(callable->address)));
    } else if (name == "signature") {
        lua_pushstring(L, callable->known ? callable->signature.describe().c_str() : "");
    } else if (name == "name") {
        lua_pushstring(L, callable->label.c_str());
    } else if (name == "stub") {
        std::uint8_t code[4]{};
        reflect::read(reinterpret_cast<std::uintptr_t>(callable->address), code, sizeof code);
        const bool stub = code[0] == 0xc3 || (code[0] == 0xc2 && code[3] == 0xcc)
                       || (code[0] == 0x32 && code[1] == 0xc0 && code[2] == 0xc3)
                       || (code[0] == 0x33 && code[1] == 0xc0 && code[2] == 0xc3);
        lua_pushboolean(L, stub);
    } else {
        lua_pushvalue(L, 2);
        if (lua_rawget(L, lua_upvalueindex(1)) == LUA_TNIL) {
            raise(L, std::format("engine functions have no member '{}'", key));
        }
    }
    return 1;
}

void make_metatable(lua_State* L, const char* name, std::initializer_list<luaL_Reg> metamethods,
                    std::initializer_list<luaL_Reg> methods, lua_CFunction index) {
    luaL_newmetatable(L, name);
    for (const auto& entry : metamethods) {
        lua_pushcfunction(L, entry.func);
        lua_setfield(L, -2, entry.name);
    }
    lua_newtable(L);
    for (const auto& entry : methods) {
        lua_pushcfunction(L, entry.func);
        lua_setfield(L, -2, entry.name);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, "methods");
    lua_pushcclosure(L, index, 1);
    lua_setfield(L, -2, "__index");
    lua_pushstring(L, name);
    lua_setfield(L, -2, "__name");
    lua_pop(L, 1);
}

void write_struct_from(lua_State* L, int index, std::uintptr_t address, const reflect::Type* type,
                       std::string_view what) {
    const auto* layout = reflect::layout(type->rtti);
    if (auto* view = to_view(L, index); view != nullptr) {
        const auto size = type->size;
        if (size == 0) {
            raise(L, std::format("{} has no size", type->name));
        }
        std::vector<std::uint8_t> bytes(size);
        if (!reflect::read(view->address, bytes.data(), size)
            || !reflect::write(address, bytes.data(), size)) {
            raise(L, std::format("cannot copy into {}", what));
        }
        return;
    }
    if (lua_istable(L, index)) {
        fill_struct(L, index, address, layout);
        return;
    }
    raise(L, std::format("{} takes a {} or a table of its fields, got {}", what, type->name,
                         type_name_of(L, index)));
}

}

std::uint8_t* Marshal::allocate(std::size_t bytes) {
    auto buffer = std::make_unique<std::uint8_t[]>(std::max<std::size_t>(bytes, 16));
    std::memset(buffer.get(), 0, std::max<std::size_t>(bytes, 16));
    auto* raw = buffer.get();
    buffers.push_back(std::move(buffer));
    return raw;
}

View* to_view(lua_State* L, int index) {
    return static_cast<View*>(luaL_testudata(L, index, kViewMeta));
}

View& check_view(lua_State* L, int index) {
    return *static_cast<View*>(luaL_checkudata(L, index, kViewMeta));
}

void push_view(lua_State* L, std::uintptr_t address, const reflect::Layout* layout, bool object) {
    auto* view = static_cast<View*>(lua_newuserdatauv(L, sizeof(View), 0));
    *view = View{address, layout, 0, object};
    luaL_setmetatable(L, kViewMeta);
}

View& push_owned(lua_State* L, const reflect::Layout* layout, std::uint32_t size, const void* bytes) {
    auto* view = static_cast<View*>(lua_newuserdatauv(L, sizeof(View) + size, 0));
    view->layout = layout;
    view->owned = size;
    view->object = false;
    view->address = reinterpret_cast<std::uintptr_t>(view + 1);
    if (bytes != nullptr) {
        std::memcpy(view + 1, bytes, size);
    } else {
        std::memset(view + 1, 0, size);
    }
    luaL_setmetatable(L, kViewMeta);
    return *view;
}

void push_array(lua_State* L, std::uintptr_t address, const reflect::Type* type) {
    auto* array = static_cast<ArrayView*>(lua_newuserdatauv(L, sizeof(ArrayView), 0));
    *array = ArrayView{address, type};
    luaL_setmetatable(L, kArrayMeta);
}

void push_object(lua_State* L, std::uintptr_t address, const reflect::Layout* layout) {
    if (address == 0) {
        lua_pushnil(L);
        return;
    }
    const void* rtti = reflect::rtti_of(address);
    if (rtti != nullptr) {
        const auto* actual = reflect::layout(rtti);
        if (actual != nullptr
            && (layout == nullptr || layout == actual || reflect::is_a(rtti, layout->rtti))) {
            lua_getfield(L, LUA_REGISTRYINDEX, "bridger.objects");
            if (lua_rawgetp(L, -1, reinterpret_cast<void*>(address)) == LUA_TUSERDATA) {
                if (auto* cached = to_view(L, -1); cached != nullptr && cached->layout == actual) {
                    lua_remove(L, -2);
                    return;
                }
            }
            lua_pop(L, 1);
            push_view(L, address, actual, true);
            lua_pushvalue(L, -1);
            lua_rawsetp(L, -3, reinterpret_cast<void*>(address));
            lua_remove(L, -2);
            return;
        }
    }
    if (layout != nullptr) {
        push_view(L, address, layout, false);
        return;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(address));
}

void push_value(lua_State* L, std::uintptr_t address, const reflect::Type* type) {
    std::string error;
    if (!try_push_value(L, address, type, error)) {
        raise(L, error);
    }
}

void write_value(lua_State* L, int index, std::uintptr_t address, const reflect::Type* type,
                 std::string_view what) {
    using reflect::Codec;
    index = lua_absindex(L, index);
    switch (type->codec) {
        case Codec::String:
        case Codec::WString:
            raise(L, std::format("{} is an engine string; the engine owns its memory, so scripts "
                                 "can read it but not assign it", what));
        case Codec::Uuid: {
            std::uint8_t bytes[16]{};
            if (auto* view = to_view(L, index); view != nullptr) {
                if (!reflect::read(view->address, bytes, 16)) {
                    raise(L, std::format("cannot read the GGUUID for {}", what));
                }
            } else if (!reflect::parse_uuid(luaL_checkstring(L, index), bytes)) {
                raise(L, std::format("{} expects a GGUUID like 0a1b2c3d-...", what));
            }
            if (!reflect::write(address, bytes, 16)) {
                raise(L, std::format("cannot write {}", what));
            }
            return;
        }
        case Codec::Pointer: {
            if (type->owning) {
                raise(L, std::format("{} is a {}, which holds a reference count; assigning it from a "
                                     "script would corrupt that count. mem.write(address, 'ptr', v) "
                                     "writes the raw pointer if you are sure", what, type->name));
            }
            std::uintptr_t pointer = 0;
            if (!to_address(L, index, pointer)) {
                raise(L, std::format("{} expects an object or an address, got {}", what,
                                     type_name_of(L, index)));
            }
            if (!reflect::write(address, &pointer, 8)) {
                raise(L, std::format("cannot write {}", what));
            }
            return;
        }
        case Codec::Handle: {
            const auto value = static_cast<std::uintptr_t>(luaL_checkinteger(L, index));
            if (!reflect::write(address, &value, 8)) {
                raise(L, std::format("cannot write {}", what));
            }
            return;
        }
        case Codec::Array:
            raise(L, std::format("{} is an array; assign its elements instead", what));
        case Codec::Struct:
            write_struct_from(L, index, address, type, what);
            return;
        case Codec::Unknown:
            raise(L, std::format("{} has type {}, which scripts cannot write", what, type->name));
        default:
            write_scalar(L, index, address, type, what);
            return;
    }
}

bool to_address(lua_State* L, int index, std::uintptr_t& out) {
    switch (lua_type(L, index)) {
        case LUA_TNIL:
            out = 0;
            return true;
        case LUA_TNUMBER:
            out = static_cast<std::uintptr_t>(lua_tointeger(L, index));
            return true;
        case LUA_TLIGHTUSERDATA:
            out = reinterpret_cast<std::uintptr_t>(lua_touserdata(L, index));
            return true;
        case LUA_TUSERDATA:
            if (auto* view = to_view(L, index); view != nullptr) {
                out = view->address;
                return true;
            }
            if (auto* array = static_cast<ArrayView*>(luaL_testudata(L, index, kArrayMeta));
                array != nullptr) {
                out = array->address;
                return true;
            }
            if (luaL_testudata(L, index, kFunctionMeta) != nullptr) {
                out = reinterpret_cast<std::uintptr_t>(to_callable(L, index)->address);
                return true;
            }
            return false;
        default:
            return false;
    }
}

void fill_struct(lua_State* L, int table, std::uintptr_t address, const reflect::Layout* layout) {
    table = lua_absindex(L, table);
    if (layout == nullptr) {
        raise(L, "no reflected layout to fill from a table");
    }
    const auto sequence = luaL_len(L, table);
    for (lua_Integer i = 1; i <= sequence; ++i) {
        if (static_cast<std::size_t>(i) > layout->fields.size()) {
            raise(L, std::format("{} has only {} field(s)", layout->name, layout->fields.size()));
        }
        const auto& field = layout->fields[static_cast<std::size_t>(i - 1)];
        lua_rawgeti(L, table, i);
        write_value(L, -1, address + field.offset, field.type,
                    std::format("{}.{}", layout->name, field.name));
        lua_pop(L, 1);
    }
    lua_pushnil(L);
    while (lua_next(L, table) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char* key = lua_tostring(L, -2);
            const auto* field = layout->find(key);
            if (field == nullptr) {
                const auto closest = layout->suggest(key);
                raise(L, std::format("{} has no field '{}'{}", layout->name, key,
                                     closest.empty() ? "" : std::format(" (did you mean '{}'?)", closest)));
            }
            write_value(L, -1, address + field->offset, field->type,
                        std::format("{}.{}", layout->name, key));
        }
        lua_pop(L, 1);
    }
}

bool is_skip(lua_State* L, int index) {
    return lua_islightuserdata(L, index) && lua_touserdata(L, index) == &g_skip_tag;
}

bool is_out(lua_State* L, int index) {
    return lua_islightuserdata(L, index) && lua_touserdata(L, index) == &g_out_tag;
}

void push_skip(lua_State* L) {
    lua_pushlightuserdata(L, &g_skip_tag);
}

void push_out(lua_State* L) {
    lua_pushlightuserdata(L, &g_out_tag);
}

ffi::TypeInfo type_info() {
    return [](std::string_view name, bool& is_enum) { return reflect::size_for_ffi(name, is_enum); };
}

const Callable* make_callable(void* address, std::string label, ffi::Signature signature, bool known) {
    const auto key = std::format("{}|{:#x}|{}", label, reinterpret_cast<std::uintptr_t>(address),
                                 known ? signature.describe() : std::string("?"));
    std::scoped_lock lock(g_callables_mutex);
    if (const auto found = g_callables.find(key); found != g_callables.end()) {
        return found->second.get();
    }
    auto callable = std::make_unique<Callable>();
    callable->address = address;
    callable->label = std::move(label);
    callable->known = known;
    for (const auto& param : signature.params) {
        callable->layouts.push_back(param.kind == ffi::Kind::Pointer
                                            || param.kind == ffi::Kind::StructMemory
                                            || param.kind == ffi::Kind::StructValue
                                        ? layout_for(param)
                                        : nullptr);
    }
    callable->result_layout = layout_for(signature.result);
    callable->signature = std::move(signature);
    const auto* raw = callable.get();
    g_callables.emplace(key, std::move(callable));
    return raw;
}

std::vector<std::string> similar_symbols(std::string_view key, std::size_t limit) {
    const auto index = decima::symbol_index();
    std::string needle(key);
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto split = needle.rfind("::");
    const std::string tail = split == std::string::npos ? needle : needle.substr(split + 2);
    std::vector<std::string> out;
    for (const auto& [name, address] : *index) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!tail.empty() && lower.find(tail) != std::string::npos) {
            out.push_back(name);
            if (out.size() >= limit) {
                break;
            }
        }
    }
    return out;
}

const Callable* callable_for_symbol(std::string_view key, std::string& error) {
    const auto split = key.find("::");
    if (split == std::string_view::npos) {
        error = std::format("'{}' is not a symbol name; they look like Group::Name", key);
        return nullptr;
    }
    const std::string group(key.substr(0, split));
    const std::string name(key.substr(split + 2));
    void* address = decima::find_symbol(group.c_str(), name.c_str());
    if (address == nullptr) {
        const auto closest = similar_symbols(name, 4);
        error = std::format("no engine symbol {}", key);
        if (!closest.empty()) {
            error += " (close: ";
            for (std::size_t i = 0; i < closest.size(); ++i) {
                error += (i == 0 ? "" : ", ") + closest[i];
            }
            error += ")";
        }
        return nullptr;
    }
    load_signatures();
    const auto found = g_signatures.find(std::string(key));
    if (found == g_signatures.end()) {
        return make_callable(address, std::string(key), {}, false);
    }
    return make_callable(address, std::string(key), ffi::from_tokens(found->second, type_info()), true);
}

const Callable* to_callable(lua_State* L, int index) {
    return *static_cast<const Callable**>(luaL_checkudata(L, index, kFunctionMeta));
}

void push_callable(lua_State* L, const Callable* callable) {
    auto* slot = static_cast<const Callable**>(lua_newuserdatauv(L, sizeof(const Callable*), 0));
    *slot = callable;
    luaL_setmetatable(L, kFunctionMeta);
}

void marshal_argument(lua_State* L, int index, const ffi::Type& type, std::size_t slot,
                      Marshal& marshal, std::string_view what) {
    using ffi::Kind;
    index = lua_absindex(L, index);
    const auto integer = [&]() -> std::uint64_t {
        if (lua_isinteger(L, index)) {
            return static_cast<std::uint64_t>(lua_tointeger(L, index));
        }
        if (lua_type(L, index) == LUA_TNUMBER) {
            return static_cast<std::uint64_t>(static_cast<std::int64_t>(lua_tonumber(L, index)));
        }
        if (lua_type(L, index) == LUA_TBOOLEAN) {
            return lua_toboolean(L, index) ? 1 : 0;
        }
        raise(L, std::format("{} expects a number, got {}", what, type_name_of(L, index)));
    };

    switch (type.kind) {
        case Kind::Void:
            raise(L, std::format("{} is declared void", what));
        case Kind::Bool:
            marshal.set(slot, lua_toboolean(L, index) ? 1 : 0);
            return;
        case Kind::I8: case Kind::U8: case Kind::I16: case Kind::U16:
        case Kind::I32: case Kind::U32: case Kind::I64: case Kind::U64:
            marshal.set(slot, integer());
            return;
        case Kind::F32:
            marshal.set(slot, ffi::pack_f32(static_cast<float>(luaL_checknumber(L, index))));
            return;
        case Kind::F64:
            marshal.set(slot, ffi::pack_f64(luaL_checknumber(L, index)));
            return;
        case Kind::Enum: {
            if (lua_type(L, index) == LUA_TSTRING) {
                std::int64_t value = 0;
                const auto* resolved = reflect::type(type.name);
                if (!reflect::enum_value(resolved->rtti, lua_tostring(L, index), value)) {
                    raise(L, std::format("{}: {} has no value named '{}'", what, type.name,
                                         lua_tostring(L, index)));
                }
                marshal.set(slot, static_cast<std::uint64_t>(value));
            } else {
                marshal.set(slot, integer());
            }
            return;
        }
        case Kind::Pointer: {
            if (is_out(L, index)) {
                Marshal::Out out;
                const int depth = type.depth + (type.reference ? 1 : 0);
                std::size_t size = 8;
                if (depth >= 2) {
                    out.type = type.name.empty() ? nullptr : reflect::type("cptr<" + type.name + ">");
                } else if (!type.name.empty()) {
                    const auto* pointee = reflect::type(type.name);
                    if (pointee->size != 0) {
                        size = pointee->size;
                        out.type = pointee;
                    }
                }
                out.address = reinterpret_cast<std::uintptr_t>(marshal.allocate(size));
                marshal.outs.push_back(out);
                marshal.set(slot, out.address);
                return;
            }
            if (lua_type(L, index) == LUA_TSTRING) {
                marshal.set(slot, reinterpret_cast<std::uint64_t>(lua_tostring(L, index)));
                return;
            }
            if (lua_istable(L, index)) {
                const auto* pointee = reflect::type(type.name);
                if (pointee->codec != reflect::Codec::Struct) {
                    raise(L, std::format("{} points at {}, which cannot be built from a table", what,
                                         type.name.empty() ? "void" : type.name));
                }
                auto* buffer = marshal.allocate(pointee->size);
                fill_struct(L, index, reinterpret_cast<std::uintptr_t>(buffer),
                            reflect::layout(pointee->rtti));
                marshal.set(slot, reinterpret_cast<std::uint64_t>(buffer));
                return;
            }
            std::uintptr_t address = 0;
            if (!to_address(L, index, address)) {
                raise(L, std::format("{} expects {} (an object, an address, a table or out), got {}",
                                     what, type.name.empty() ? "a pointer" : type.name + "*",
                                     type_name_of(L, index)));
            }
            marshal.set(slot, address);
            return;
        }
        case Kind::StructValue:
        case Kind::StructMemory: {
            const auto* resolved = reflect::type(type.name);
            const auto size = std::max<std::uint32_t>(type.size, resolved->size);
            auto* buffer = marshal.allocate(size);
            const auto address = reinterpret_cast<std::uintptr_t>(buffer);
            if (auto* view = to_view(L, index); view != nullptr) {
                if (!reflect::read(view->address, buffer, size)) {
                    raise(L, std::format("{}: cannot read the {} passed", what, type.name));
                }
            } else if (lua_istable(L, index)) {
                fill_struct(L, index, address, reflect::layout(resolved->rtti));
            } else if (type.kind == Kind::StructValue && lua_isinteger(L, index)) {
                const auto bits = static_cast<std::uint64_t>(lua_tointeger(L, index));
                std::memcpy(buffer, &bits, std::min<std::size_t>(size, 8));
            } else {
                raise(L, std::format("{} expects a {} (a value or a table of its fields), got {}", what,
                                     type.name, type_name_of(L, index)));
            }
            if (type.kind == Kind::StructValue) {
                std::uint64_t bits = 0;
                std::memcpy(&bits, buffer, std::min<std::size_t>(size, 8));
                marshal.set(slot, bits);
            } else {
                marshal.set(slot, address);
            }
            return;
        }
    }
}

void push_result(lua_State* L, const ffi::Type& type, const reflect::Layout* layout,
                 const ffi::Result& result, std::uintptr_t hidden) {
    using ffi::Kind;
    const auto rax = result.rax;
    switch (type.kind) {
        case Kind::Void: return;
        case Kind::Bool: lua_pushboolean(L, (rax & 0xff) != 0); return;
        case Kind::I8: lua_pushinteger(L, static_cast<std::int8_t>(rax)); return;
        case Kind::U8: lua_pushinteger(L, static_cast<std::uint8_t>(rax)); return;
        case Kind::I16: lua_pushinteger(L, static_cast<std::int16_t>(rax)); return;
        case Kind::U16: lua_pushinteger(L, static_cast<std::uint16_t>(rax)); return;
        case Kind::I32: lua_pushinteger(L, static_cast<std::int32_t>(rax)); return;
        case Kind::U32: lua_pushinteger(L, static_cast<std::uint32_t>(rax)); return;
        case Kind::I64:
        case Kind::U64: lua_pushinteger(L, static_cast<lua_Integer>(rax)); return;
        case Kind::F32: lua_pushnumber(L, ffi::unpack_f32(ffi::pack_f64(result.xmm0))); return;
        case Kind::F64: lua_pushnumber(L, result.xmm0); return;
        case Kind::Enum:
            switch (type.size) {
                case 1: lua_pushinteger(L, static_cast<std::int8_t>(rax)); return;
                case 2: lua_pushinteger(L, static_cast<std::int16_t>(rax)); return;
                case 8: lua_pushinteger(L, static_cast<lua_Integer>(rax)); return;
                default: lua_pushinteger(L, static_cast<std::int32_t>(rax)); return;
            }
        case Kind::Pointer:
            if (rax == 0) {
                lua_pushnil(L);
            } else if ((type.name == "char" || type.name == "tchar") && type.depth == 1) {
                const auto text = reflect::read_c_string(static_cast<std::uintptr_t>(rax));
                lua_pushlstring(L, text.data(), text.size());
            } else {
                push_object(L, static_cast<std::uintptr_t>(rax), layout);
            }
            return;
        case Kind::StructValue:
            push_owned(L, layout, std::max<std::uint32_t>(type.size, 8), &rax);
            return;
        case Kind::StructMemory: {
            const auto* resolved = reflect::type(type.name);
            if (resolved->codec == reflect::Codec::String || resolved->codec == reflect::Codec::WString
                || resolved->codec == reflect::Codec::Uuid) {
                std::string error;
                if (!try_push_value(L, hidden, resolved, error)) {
                    lua_pushnil(L);
                }
                return;
            }
            std::vector<std::uint8_t> bytes(std::max<std::uint32_t>(type.size, 1));
            reflect::read(hidden, bytes.data(), bytes.size());
            push_owned(L, layout, static_cast<std::uint32_t>(bytes.size()), bytes.data());
            return;
        }
    }
}

void to_result(lua_State* L, int index, const ffi::Type& type, std::uintptr_t hidden,
               ffi::Result& out, std::string_view what) {
    using ffi::Kind;
    Marshal marshal;
    switch (type.kind) {
        case Kind::Void:
            return;
        case Kind::F32:
            out.xmm0 = std::bit_cast<double>(ffi::pack_f32(static_cast<float>(luaL_checknumber(L, index))));
            return;
        case Kind::F64:
            out.xmm0 = luaL_checknumber(L, index);
            return;
        case Kind::StructMemory: {
            const auto* resolved = reflect::type(type.name);
            if (hidden == 0 || resolved->codec != reflect::Codec::Struct) {
                raise(L, std::format("{}: a {} result cannot be replaced from a script", what, type.name));
            }
            write_value(L, index, hidden, resolved, what);
            out.rax = hidden;
            return;
        }
        default:
            marshal_argument(L, index, type, 0, marshal, what);
            out.rax = marshal.ints[0];
            return;
    }
}

void push_argument(lua_State* L, const ffi::Type& type, const reflect::Layout* layout,
                   std::uint64_t bits, double xmm, bool in_register) {
    using ffi::Kind;
    switch (type.kind) {
        case Kind::F32:
            lua_pushnumber(L, in_register ? ffi::unpack_f32(std::bit_cast<std::uint64_t>(xmm))
                                          : ffi::unpack_f32(bits));
            return;
        case Kind::F64:
            lua_pushnumber(L, in_register ? xmm : ffi::unpack_f64(bits));
            return;
        case Kind::StructMemory:
            push_view(L, static_cast<std::uintptr_t>(bits), layout, false);
            return;
        default: {
            ffi::Result result;
            result.rax = bits;
            push_result(L, type, layout, result, 0);
            return;
        }
    }
}

int call_native(lua_State* L, const Callable& callable, int first) {
    if (!callable.known) {
        raise(L, std::format("{} has no recorded signature; wrap it with fn({:#x}, \"result(params)\")",
                             callable.label, reinterpret_cast<std::uintptr_t>(callable.address)));
    }
    const auto& signature = callable.signature;
    const int given = std::max(0, lua_gettop(L) - first + 1);
    if (static_cast<std::size_t>(given) != signature.params.size()) {
        raise(L, std::format("{} takes {} argument(s), {} given: {}", callable.label,
                             signature.params.size(), given, signature.describe()));
    }
    if (signature.slot_count() > 44) {
        raise(L, std::format("{} has too many parameters to call", callable.label));
    }

    Marshal marshal;
    std::size_t slot = 0;
    std::uintptr_t hidden = 0;
    if (signature.hidden_result()) {
        const auto* resolved = reflect::type(signature.result.name);
        hidden = reinterpret_cast<std::uintptr_t>(
            marshal.allocate(std::max<std::uint32_t>(signature.result.size, resolved->size) + 64));
        marshal.set(slot++, hidden);
    }
    for (std::size_t i = 0; i < signature.params.size(); ++i) {
        marshal_argument(L, first + static_cast<int>(i), signature.params[i], slot++, marshal,
                         std::format("argument {} of {}", i + 1, callable.label));
    }
    marshal.count = slot;

    ffi::Result result;
    const auto fault = ffi::call(callable.address, marshal.ints.data(), marshal.xmms.data(),
                                 marshal.count, result);
    if (fault != 0) {
        raise(L, std::format("{} faulted with {:#010x} {}; the arguments were not what it expects, or "
                             "it was called off the game thread", callable.label, fault,
                             describe_last_fault()));
    }
    const int top = lua_gettop(L);
    push_result(L, signature.result, callable.result_layout, result, hidden);
    for (const auto& out : marshal.outs) {
        if (out.type == nullptr) {
            std::uintptr_t value = 0;
            reflect::read(out.address, &value, 8);
            lua_pushinteger(L, static_cast<lua_Integer>(value));
            continue;
        }
        std::string error;
        if (out.type->codec == reflect::Codec::Struct) {
            push_owned(L, reflect::layout(out.type->rtti), out.type->size,
                       reinterpret_cast<const void*>(out.address));
        } else if (!try_push_value(L, out.address, out.type, error)) {
            lua_pushnil(L);
        }
    }
    return lua_gettop(L) - top;
}

void open_values(lua_State* L) {
    lua_newtable(L);
    lua_newtable(L);
    lua_pushstring(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, "bridger.objects");

    make_metatable(L, kViewMeta,
                   {{"__newindex", view_newindex},
                    {"__tostring", view_tostring},
                    {"__eq", view_eq},
                    {"__pairs", view_pairs}},
                   {{"type", method_type},
                    {"address", method_address},
                    {"is_a", method_is_a},
                    {"valid", method_valid},
                    {"fields", method_fields},
                    {"get", method_get},
                    {"set", method_set},
                    {"field_address", method_field_address},
                    {"table", method_table},
                    {"cast", method_cast},
                    {"copy", method_copy},
                    {"inspect", method_inspect},
                    {"read", method_read},
                    {"write", method_write},
                    {"handler", method_handler},
                    {"messages", method_messages},
                    {"send", method_send},
                    {"clone", method_clone},
                    {"vtable", method_vtable}},
                   view_index);

    make_metatable(L, kArrayMeta,
                   {{"__len", array_len},
                    {"__newindex", array_newindex},
                    {"__tostring", array_tostring},
                    {"__pairs", array_pairs}},
                   {{"count", array_count},
                    {"data", array_data},
                    {"table", array_table},
                    {"address", array_address}},
                   array_index);

    make_metatable(L, kFunctionMeta,
                   {{"__call", function_call},
                    {"__tostring", function_tostring}},
                   {}, function_index);

    push_skip(L);
    lua_setglobal(L, "skip");
    push_out(L);
    lua_setglobal(L, "out");
}

}
