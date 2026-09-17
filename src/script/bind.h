#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "script/vm.h"

namespace bridger::script {

inline constexpr const char* kViewMeta = "bridger.View";
inline constexpr const char* kArrayMeta = "bridger.Array";
inline constexpr const char* kFunctionMeta = "bridger.Function";
inline constexpr const char* kHookMeta = "bridger.Hook";
inline constexpr const char* kEntryMeta = "bridger.ContentEntry";

struct View {
    std::uintptr_t address = 0;
    const reflect::Layout* layout = nullptr;
    std::uint32_t owned = 0;
    bool object = false;
};

struct ArrayView {
    std::uintptr_t address = 0;
    const reflect::Type* type = nullptr;
};

struct Callable {
    void* address = nullptr;
    std::string label;
    ffi::Signature signature;
    bool known = false;
    std::vector<const reflect::Layout*> layouts;
    const reflect::Layout* result_layout = nullptr;
};

View* to_view(lua_State* L, int index);
View& check_view(lua_State* L, int index);
void push_view(lua_State* L, std::uintptr_t address, const reflect::Layout* layout, bool object);
View& push_owned(lua_State* L, const reflect::Layout* layout, std::uint32_t size,
                 const void* bytes);
void push_array(lua_State* L, std::uintptr_t address, const reflect::Type* type);

const Callable* make_callable(void* address, std::string label, ffi::Signature signature, bool known);
const Callable* callable_for_symbol(std::string_view key, std::string& error);
const Callable* to_callable(lua_State* L, int index);
void push_callable(lua_State* L, const Callable* callable);
ffi::TypeInfo type_info();
std::vector<std::string> similar_symbols(std::string_view key, std::size_t limit);

struct Marshal {
    std::array<std::uint64_t, 48> ints{};
    std::array<double, 4> xmms{};
    std::size_t count = 0;
    std::vector<std::unique_ptr<std::uint8_t[]>> buffers;
    struct Out {
        std::uintptr_t address = 0;
        const reflect::Type* type = nullptr;
        int depth = 1;
    };
    std::vector<Out> outs;

    void set(std::size_t slot, std::uint64_t bits) {
        ints[slot] = bits;
        if (slot < 4) {
            xmms[slot] = std::bit_cast<double>(bits);
        }
    }
    std::uint8_t* allocate(std::size_t bytes);
};

int call_native(lua_State* L, const Callable& callable, int first);

void marshal_argument(lua_State* L, int index, const ffi::Type& type, std::size_t slot,
                      Marshal& marshal, std::string_view what);
void push_result(lua_State* L, const ffi::Type& type, const reflect::Layout* layout,
                 const ffi::Result& result, std::uintptr_t hidden);
void to_result(lua_State* L, int index, const ffi::Type& type, std::uintptr_t hidden,
               ffi::Result& out, std::string_view what);
void push_argument(lua_State* L, const ffi::Type& type, const reflect::Layout* layout,
                   std::uint64_t bits, double xmm, bool in_register);

void fill_struct(lua_State* L, int table, std::uintptr_t address, const reflect::Layout* layout);

bool is_skip(lua_State* L, int index);
bool is_out(lua_State* L, int index);
void push_skip(lua_State* L);
void push_out(lua_State* L);

void push_entry(lua_State* L, std::uint32_t handle, std::string label);

void open_values(lua_State* L);
void open_hooks(lua_State* L);

}
