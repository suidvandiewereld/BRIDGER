#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "loader/registry.h"
#include "script/ffi.h"
#include "script/reflect.h"

namespace bridger::script {

enum class Output { Print, Result, Info, Warn, Error };

struct PanelRecord;

struct Instance {
    std::string id;
    std::filesystem::path directory;
    std::filesystem::path entry;
    bool console = false;

    lua_State* L = nullptr;

    std::recursive_timed_mutex mutex;
    std::atomic<bool> alive{false};
    std::atomic<int> in_flight{0};
    bool loading = false;
    bool drawing = false;

    std::int64_t deadline = 0;
    unsigned budget_ms = 0;
    unsigned eval_budget_ms = 15000;
    std::string* capture = nullptr;

    std::filesystem::file_time_type stamp{};

    std::mutex status_mutex;
    std::string last_error;
    std::uint64_t error_count = 0;
    std::uint64_t last_error_logged = 0;
    std::uint64_t suppressed = 0;
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::size_t> memory{0};
    std::vector<std::filesystem::path> sources;

    std::vector<std::unique_ptr<PanelRecord>> panels;
    std::vector<int> callback_slots;
};

struct PanelRecord {
    Instance* instance = nullptr;
    int ref = LUA_NOREF;
    std::string failure;
};

Instance& current(lua_State* L);

class Enter {
public:
    Enter(Instance& instance, unsigned wait_ms, unsigned budget_ms);
    ~Enter();
    Enter(const Enter&) = delete;
    Enter& operator=(const Enter&) = delete;

    [[nodiscard]] explicit operator bool() const { return locked_; }

private:
    Instance& instance_;
    bool locked_ = false;
    std::int64_t saved_deadline_ = 0;
    std::unique_ptr<loader::ScopedActor> actor_;
};

bool protected_call(Instance& instance, int nargs, int nresults, std::string_view what);

void emit(Instance& instance, Output kind, std::string_view text);

void console_append(Output kind, std::string_view source, std::string_view text);

void open_engine(lua_State* L);
void open_host(lua_State* L);
bool run_prelude(lua_State* L, std::string& error);

void release_hooks(Instance& instance);
void release_callbacks(Instance& instance);
std::size_t count_hooks(const Instance& instance);

std::shared_ptr<Instance> find_instance(std::string_view id);
const std::filesystem::path& root();

void start_console_server();
void stop_console_server();

void push_value(lua_State* L, std::uintptr_t address, const reflect::Type* type);
void write_value(lua_State* L, int index, std::uintptr_t address, const reflect::Type* type,
                 std::string_view what);
void push_object(lua_State* L, std::uintptr_t address, const reflect::Layout* layout);
bool to_address(lua_State* L, int index, std::uintptr_t& out);

inline std::string utf8(const std::u8string& text) {
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::int64_t now_ticks();
std::int64_t ticks_per_ms();

}
