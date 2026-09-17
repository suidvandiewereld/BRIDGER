#include "script/vm.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include <MinHook.h>

#include "core/log.h"
#include "script/runtime.h"

namespace bridger::script {
namespace {

std::filesystem::path g_root;
std::atomic<bool> g_exiting{false};

std::mutex g_instances_mutex;
std::map<std::string, std::shared_ptr<Instance>, std::less<>> g_instances;

std::mutex g_console_mutex;
std::deque<Line> g_console;
std::uint64_t g_console_written = 0;
constexpr std::size_t kConsoleLines = 4000;

constexpr unsigned kLoadBudgetMs = 5000;
constexpr unsigned kTickBudgetMs = 1000;
constexpr unsigned kConsoleBudgetMs = 15000;
constexpr std::string_view kConsoleId = "console";

void* allocate(void* user, void* block, std::size_t old_size, std::size_t new_size) {
    auto* instance = static_cast<Instance*>(user);
    if (block != nullptr) {
        instance->memory.fetch_sub(old_size, std::memory_order_relaxed);
    }
    if (new_size == 0) {
        std::free(block);
        return nullptr;
    }
    auto* moved = std::realloc(block, new_size);
    if (moved == nullptr) {
        if (block != nullptr) {
            instance->memory.fetch_add(old_size, std::memory_order_relaxed);
        }
        return nullptr;
    }
    instance->memory.fetch_add(new_size, std::memory_order_relaxed);
    return moved;
}

int panic(lua_State* L) {
    const char* message = lua_tostring(L, -1);
    log::error("script: unprotected Lua error: {}", message != nullptr ? message : "?");
    return 0;
}

void budget_hook(lua_State* L, lua_Debug*) {
    auto& instance = current(L);
    if (instance.deadline != 0 && now_ticks() > instance.deadline) {
        instance.deadline = 0;
        luaL_error(L, "ran for more than %d ms without returning (an endless loop?)",
                   static_cast<int>(instance.budget_ms));
    }
}

int traceback(lua_State* L) {
    const char* message = lua_tostring(L, 1);
    if (message == nullptr) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) {
            return 1;
        }
        message = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, message, 1);
    return 1;
}

std::string read_file(const std::filesystem::path& path, bool& ok) {
    std::ifstream stream(path, std::ios::binary);
    ok = stream.is_open();
    if (!ok) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string chunk_name(const std::filesystem::path& path) {
    std::error_code ec;
    auto relative = std::filesystem::relative(path, g_root, ec);
    const auto text = (ec || relative.empty() ? path : relative).generic_string();
    return "@" + text;
}

int search_module(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    const char* path = lua_tostring(L, -1);
    lua_getfield(L, -2, "searchpath");
    lua_pushstring(L, name);
    lua_pushstring(L, path != nullptr ? path : "");
    lua_call(L, 2, 2);
    if (lua_isnil(L, -2)) {
        return 1;
    }
    const std::filesystem::path file(std::u8string(reinterpret_cast<const char8_t*>(lua_tostring(L, -2))));
    bool ok = false;
    const auto source = read_file(file, ok);
    if (!ok) {
        lua_pushfstring(L, "\n\tcannot read '%s'", lua_tostring(L, -2));
        return 1;
    }
    auto& instance = current(L);
    {
        std::scoped_lock lock(instance.status_mutex);
        if (std::find(instance.sources.begin(), instance.sources.end(), file)
            == instance.sources.end()) {
            instance.sources.push_back(file);
        }
    }
    const auto chunk = chunk_name(file);
    if (luaL_loadbufferx(L, source.data(), source.size(), chunk.c_str(), "t") != LUA_OK) {
        return lua_error(L);
    }
    lua_pushstring(L, utf8(file.u8string()).c_str());
    return 2;
}

bool create_state(Instance& instance, std::string& error) {
    lua_State* L = lua_newstate(allocate, &instance);
    if (L == nullptr) {
        error = "could not create a Lua state";
        return false;
    }
    instance.L = L;
    *static_cast<Instance**>(lua_getextraspace(L)) = &instance;
    lua_atpanic(L, panic);
    lua_sethook(L, budget_hook, LUA_MASKCOUNT, 4096);

    luaL_openlibs(L);

    lua_getglobal(L, "os");
    lua_pushcfunction(L, [](lua_State* S) -> int {
        return luaL_error(S, "os.exit would close the game; unload the script instead");
    });
    lua_setfield(L, -2, "exit");
    lua_pop(L, 1);

    const auto scripts = g_root / "scripts";
    const auto path = std::format("{0}/?.lua;{0}/?/init.lua;{1}/?.lua;{1}/lib/?.lua;{1}/lib/?/init.lua",
                                  utf8(instance.directory.generic_u8string()),
                                  utf8(scripts.generic_u8string()));
    lua_getglobal(L, "package");
    lua_pushstring(L, path.c_str());
    lua_setfield(L, -2, "path");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "cpath");
    lua_getfield(L, -1, "searchers");
    lua_pushcfunction(L, search_module);
    lua_rawseti(L, -2, 2);
    lua_pushnil(L);
    lua_rawseti(L, -2, 3);
    lua_pushnil(L);
    lua_rawseti(L, -2, 4);
    lua_pop(L, 2);

    lua_newtable(L);
    lua_pushstring(L, instance.id.c_str());
    lua_setfield(L, -2, "id");
    lua_pushstring(L, utf8(instance.directory.generic_u8string()).c_str());
    lua_setfield(L, -2, "dir");
    lua_pushstring(L, utf8(g_root.generic_u8string()).c_str());
    lua_setfield(L, -2, "root");
    lua_pushboolean(L, instance.console);
    lua_setfield(L, -2, "console");
    lua_setglobal(L, "script");

    instance.alive.store(true, std::memory_order_release);
    try {
        open_engine(L);
        open_host(L);
    } catch (...) {
        error = "binding the engine into Lua failed";
        return false;
    }
    Enter enter(instance, INFINITE, kLoadBudgetMs);
    if (!run_prelude(L, error)) {
        return false;
    }
    return true;
}

void destroy_state(Instance& instance) {
    if (instance.L == nullptr) {
        return;
    }
    std::scoped_lock lock(instance.mutex);
    lua_close(instance.L);
    instance.L = nullptr;
}

void instance_tick(void* user, float dt) {
    auto& instance = *static_cast<Instance*>(user);
    Enter enter(instance, 250, kTickBudgetMs);
    if (!enter) {
        return;
    }
    lua_State* L = instance.L;
    if (lua_getglobal(L, "__bridger_tick") != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return;
    }
    lua_pushnumber(L, dt);
    protected_call(instance, 1, 0, "frame");
}

void run_unload_callbacks(Instance& instance) {
    Enter enter(instance, 2000, kLoadBudgetMs);
    if (!enter) {
        return;
    }
    lua_State* L = instance.L;
    if (lua_getglobal(L, "__bridger_unload") != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return;
    }
    protected_call(instance, 0, 0, "on_unload");
}

struct Kept {
    enum class Tag { Nil, Boolean, Integer, Number, String, Table, Address } tag = Tag::Nil;
    bool boolean = false;
    std::int64_t integer = 0;
    double number = 0.0;
    std::string text;
    std::vector<std::pair<Kept, Kept>> entries;
};

std::mutex g_kept_mutex;
std::map<std::string, std::map<std::string, Kept>, std::less<>> g_kept;

Kept capture_value(lua_State* L, int index, int depth) {
    Kept kept;
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
        case LUA_TBOOLEAN:
            kept.tag = Kept::Tag::Boolean;
            kept.boolean = lua_toboolean(L, index) != 0;
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, index)) {
                kept.tag = Kept::Tag::Integer;
                kept.integer = lua_tointeger(L, index);
            } else {
                kept.tag = Kept::Tag::Number;
                kept.number = lua_tonumber(L, index);
            }
            break;
        case LUA_TSTRING: {
            std::size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            kept.tag = Kept::Tag::String;
            kept.text.assign(text, length);
            break;
        }
        case LUA_TTABLE:
            if (depth > 16) {
                break;
            }
            kept.tag = Kept::Tag::Table;
            lua_pushnil(L);
            while (lua_next(L, index) != 0) {
                auto key = capture_value(L, -2, depth + 1);
                auto value = capture_value(L, -1, depth + 1);
                if (key.tag != Kept::Tag::Nil && value.tag != Kept::Tag::Nil) {
                    kept.entries.emplace_back(std::move(key), std::move(value));
                }
                lua_pop(L, 1);
            }
            break;
        case LUA_TUSERDATA: {
            std::uintptr_t address = 0;
            if (to_address(L, index, address) && address != 0) {
                kept.tag = Kept::Tag::Address;
                kept.integer = static_cast<std::int64_t>(address);
            }
            break;
        }
        default:
            break;
    }
    return kept;
}

void restore_value(lua_State* L, const Kept& kept) {
    switch (kept.tag) {
        case Kept::Tag::Boolean: lua_pushboolean(L, kept.boolean); break;
        case Kept::Tag::Integer: lua_pushinteger(L, kept.integer); break;
        case Kept::Tag::Number: lua_pushnumber(L, kept.number); break;
        case Kept::Tag::String: lua_pushlstring(L, kept.text.data(), kept.text.size()); break;
        case Kept::Tag::Address:
            push_object(L, static_cast<std::uintptr_t>(kept.integer), nullptr);
            break;
        case Kept::Tag::Table:
            lua_createtable(L, 0, static_cast<int>(kept.entries.size()));
            for (const auto& [key, value] : kept.entries) {
                restore_value(L, key);
                restore_value(L, value);
                lua_rawset(L, -3);
            }
            break;
        default: lua_pushnil(L); break;
    }
}

void save_kept(Instance& instance) {
    Enter enter(instance, 2000, kLoadBudgetMs);
    if (!enter) {
        return;
    }
    lua_State* L = instance.L;
    if (lua_getglobal(L, "__bridger_kept") != LUA_TTABLE) {
        lua_pop(L, 1);
        return;
    }
    std::map<std::string, Kept> saved;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            saved[lua_tostring(L, -2)] = capture_value(L, -1, 0);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    std::scoped_lock lock(g_kept_mutex);
    g_kept[instance.id] = std::move(saved);
}

int lua_kept(lua_State* L) {
    auto& instance = current(L);
    const char* name = luaL_checkstring(L, 1);
    std::scoped_lock lock(g_kept_mutex);
    const auto script = g_kept.find(instance.id);
    if (script == g_kept.end()) {
        return 0;
    }
    const auto value = script->second.find(name);
    if (value == script->second.end()) {
        return 0;
    }
    restore_value(L, value->second);
    return 1;
}

std::string render_results(Instance& instance, int base) {
    lua_State* L = instance.L;
    const int top = lua_gettop(L);
    std::string text;
    for (int i = base; i <= top; ++i) {
        if (!text.empty()) {
            text += ", ";
        }
        if (lua_getglobal(L, "__bridger_repr") == LUA_TFUNCTION) {
            lua_pushvalue(L, i);
            if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_type(L, -1) == LUA_TSTRING) {
                text += lua_tostring(L, -1);
            } else {
                text += luaL_tolstring(L, i, nullptr);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        } else {
            lua_pop(L, 1);
            text += luaL_tolstring(L, i, nullptr);
            lua_pop(L, 1);
        }
    }
    if (top >= base) {
        lua_pushvalue(L, base);
        lua_setglobal(L, "_");
    }
    return text;
}

Evaluation evaluate_now(Instance& instance, const std::string& code) {
    Evaluation out;
    Enter enter(instance, 2000, instance.eval_budget_ms != 0 ? instance.eval_budget_ms : kConsoleBudgetMs);
    if (!enter) {
        out.result = instance.alive ? "the script is busy on another thread" : "the script has stopped";
        return out;
    }
    lua_State* L = instance.L;
    const int base = lua_gettop(L);
    std::string output;
    instance.capture = &output;

    const std::string expression = "return " + code;
    int status = luaL_loadbufferx(L, expression.data(), expression.size(), "=console", "t");
    if (status != LUA_OK) {
        lua_pop(L, 1);
        status = luaL_loadbufferx(L, code.data(), code.size(), "=console", "t");
    }
    if (status != LUA_OK) {
        out.result = lua_tostring(L, -1);
        lua_settop(L, base);
        instance.capture = nullptr;
        return out;
    }
    lua_pushcfunction(L, traceback);
    lua_insert(L, base + 1);
    status = lua_pcall(L, 0, LUA_MULTRET, base + 1);
    if (status != LUA_OK) {
        out.result = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "error";
    } else {
        out.ok = true;
        out.result = render_results(instance, base + 2);
    }
    lua_settop(L, base);
    instance.capture = nullptr;
    out.output = std::move(output);
    return out;
}

struct EvalWork {
    std::shared_ptr<Instance> instance;
    std::string code;
    Evaluation result;
    bool done = false;
    bool cancelled = false;
    std::mutex mutex;
    std::condition_variable ready;
};

}

Instance& current(lua_State* L) {
    return **static_cast<Instance**>(lua_getextraspace(L));
}

const std::filesystem::path& root() {
    return g_root;
}

std::int64_t now_ticks() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

std::int64_t ticks_per_ms() {
    static const std::int64_t value = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return std::max<std::int64_t>(1, frequency.QuadPart / 1000);
    }();
    return value;
}

Enter::Enter(Instance& instance, unsigned wait_ms, unsigned budget_ms) : instance_(instance) {
    instance_.in_flight.fetch_add(1, std::memory_order_acq_rel);
    if (!instance_.alive.load(std::memory_order_acquire)) {
        return;
    }
    const bool got = wait_ms == INFINITE
                         ? (instance_.mutex.lock(), true)
                         : instance_.mutex.try_lock_for(std::chrono::milliseconds(wait_ms));
    if (!got) {
        return;
    }
    if (!instance_.alive.load(std::memory_order_acquire) || instance_.L == nullptr) {
        instance_.mutex.unlock();
        return;
    }
    locked_ = true;
    saved_deadline_ = instance_.deadline;
    if (budget_ms != 0) {
        const auto deadline = now_ticks() + static_cast<std::int64_t>(budget_ms) * ticks_per_ms();
        if (instance_.deadline == 0 || deadline < instance_.deadline) {
            instance_.deadline = deadline;
            instance_.budget_ms = budget_ms;
        }
    }
    actor_ = std::make_unique<loader::ScopedActor>(instance_.id);
    instance_.callbacks.fetch_add(1, std::memory_order_relaxed);
}

Enter::~Enter() {
    if (locked_) {
        actor_.reset();
        instance_.deadline = saved_deadline_;
        instance_.mutex.unlock();
    }
    instance_.in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

bool protected_call(Instance& instance, int nargs, int nresults, std::string_view what) {
    lua_State* L = instance.L;
    const int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    const int status = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);
    if (status == LUA_OK) {
        return true;
    }
    std::string message = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "unknown error";
    lua_pop(L, 1);
    std::uint64_t repeats = 0;
    {
        std::scoped_lock lock(instance.status_mutex);
        const auto now = GetTickCount64();
        const bool repeat = message == instance.last_error && now - instance.last_error_logged < 2000;
        instance.last_error = message;
        ++instance.error_count;
        if (repeat) {
            ++instance.suppressed;
            return false;
        }
        repeats = instance.suppressed;
        instance.suppressed = 0;
        instance.last_error_logged = now;
    }
    emit(instance, Output::Error,
         repeats == 0 ? std::format("error in {}: {}", what, message)
                      : std::format("error in {} (and {} repeat(s) hidden): {}", what, repeats, message));
    return false;
}

void emit(Instance& instance, Output kind, std::string_view text) {
    switch (kind) {
        case Output::Warn: log::warn("[{}] {}", instance.id, text); break;
        case Output::Error: log::error("[{}] {}", instance.id, text); break;
        default: log::info("[{}] {}", instance.id, text); break;
    }
    console_append(kind, instance.id, text);
    if (instance.capture != nullptr) {
        instance.capture->append(text);
        instance.capture->push_back('\n');
    }
}

void console_append(Output kind, std::string_view source, std::string_view text) {
    std::scoped_lock lock(g_console_mutex);
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        const auto line = text.substr(start, end == std::string_view::npos ? text.npos : end - start);
        g_console.push_back({static_cast<int>(kind), std::string(source), std::string(line)});
        ++g_console_written;
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    while (g_console.size() > kConsoleLines) {
        g_console.pop_front();
    }
}

std::shared_ptr<Instance> find_instance(std::string_view id) {
    std::scoped_lock lock(g_instances_mutex);
    const auto found = g_instances.find(id);
    return found == g_instances.end() ? nullptr : found->second;
}

void start(const std::filesystem::path& root) {
    g_root = root;
    MH_Initialize();
    auto console = std::make_shared<Instance>();
    console->id = std::string(kConsoleId);
    console->console = true;
    console->directory = root / "scripts";
    std::error_code ec;
    std::filesystem::create_directories(console->directory, ec);

    std::string error;
    if (!create_state(*console, error)) {
        log::error("script: the console state could not start: {}", error);
        destroy_state(*console);
        return;
    }
    {
        std::scoped_lock lock(g_instances_mutex);
        g_instances[console->id] = console;
    }
    loader::Registry::instance().add_game_tick({"", instance_tick, console.get()});
    start_console_server();
    log::info("script: runtime ready ({})", LUA_RELEASE);
}

void prepare_exit() {
    g_exiting.store(true, std::memory_order_release);
}

void shutdown() {
    g_exiting.store(true, std::memory_order_release);
    stop_console_server();
    std::vector<std::shared_ptr<Instance>> all;
    {
        std::scoped_lock lock(g_instances_mutex);
        for (auto& [id, instance] : g_instances) {
            all.push_back(instance);
        }
    }
    for (auto& instance : all) {
        instance->alive.store(false, std::memory_order_release);
        release_hooks(*instance);
    }
}

bool load(const loader::Mod& mod, std::string& error) {
    if (find_instance(mod.id) != nullptr) {
        error = "a script with this id is already running";
        return false;
    }
    auto instance = std::make_shared<Instance>();
    instance->id = mod.id;
    instance->directory = mod.directory;
    instance->entry = mod.directory / mod.entry;

    bool readable = false;
    const auto source = read_file(instance->entry, readable);
    if (!readable) {
        error = std::format("cannot read {}", mod.entry);
        return false;
    }
    if (!create_state(*instance, error)) {
        instance->alive.store(false, std::memory_order_release);
        destroy_state(*instance);
        return false;
    }
    {
        std::scoped_lock lock(g_instances_mutex);
        g_instances[instance->id] = instance;
    }
    loader::Registry::instance().add_game_tick({mod.id, instance_tick, instance.get()});

    Enter enter(*instance, INFINITE, kLoadBudgetMs);
    lua_State* L = instance->L;
    lua_pushcfunction(L, lua_kept);
    lua_setglobal(L, "__bridger_restore");
    const auto chunk = chunk_name(instance->entry);
    if (luaL_loadbufferx(L, source.data(), source.size(), chunk.c_str(), "t") != LUA_OK) {
        error = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "syntax error";
        lua_pop(L, 1);
        return false;
    }
    instance->loading = true;
    bool ok = protected_call(*instance, 0, 0, "load");
    if (ok && lua_getglobal(L, "__bridger_after_load") == LUA_TFUNCTION) {
        ok = protected_call(*instance, 0, 0, "load");
    } else if (ok) {
        lua_pop(L, 1);
    }
    instance->loading = false;
    if (!ok) {
        std::scoped_lock lock(instance->status_mutex);
        error = instance->last_error;
        return false;
    }
    return true;
}

void stop(const std::string& id) {
    const auto instance = find_instance(id);
    if (instance == nullptr) {
        return;
    }
    if (instance->alive.load(std::memory_order_acquire) && !g_exiting.load(std::memory_order_acquire)) {
        run_unload_callbacks(*instance);
        save_kept(*instance);
    }
    instance->alive.store(false, std::memory_order_release);
    release_hooks(*instance);
}

void close(const std::string& id) {
    std::shared_ptr<Instance> instance;
    {
        std::scoped_lock lock(g_instances_mutex);
        const auto found = g_instances.find(id);
        if (found == g_instances.end()) {
            return;
        }
        instance = found->second;
        g_instances.erase(found);
    }
    instance->alive.store(false, std::memory_order_release);
    const auto deadline = GetTickCount64() + 3000;
    while (instance->in_flight.load(std::memory_order_acquire) > 0 && GetTickCount64() < deadline) {
        Sleep(1);
    }
    if (instance->in_flight.load(std::memory_order_acquire) > 0) {
        log::warn("script {}: a callback did not return within 3 s, its state is left open", id);
        return;
    }
    release_callbacks(*instance);
    destroy_state(*instance);
}

std::filesystem::file_time_type sources_stamp(const loader::Mod& mod) {
    std::error_code ec;
    auto newest = std::filesystem::last_write_time(mod.directory / mod.entry, ec);
    if (ec) {
        return {};
    }
    const auto consider = [&](const std::filesystem::path& path) {
        std::error_code stamp_ec;
        const auto stamp = std::filesystem::last_write_time(path, stamp_ec);
        if (!stamp_ec && stamp > newest) {
            newest = stamp;
        }
    };
    if (!mod.loose) {
        auto it = std::filesystem::recursive_directory_iterator(
            mod.directory, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            const auto name = it->path().filename().string();
            if (it->is_directory(ec) && (name.starts_with('.') || it.depth() >= 3)) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->path().extension() == ".lua") {
                consider(it->path());
            }
        }
    }
    if (const auto instance = find_instance(mod.id); instance != nullptr) {
        std::scoped_lock lock(instance->status_mutex);
        for (const auto& path : instance->sources) {
            consider(path);
        }
    }
    return newest;
}

bool check_syntax(const loader::Mod& mod, std::string& error) {
    bool readable = false;
    const auto path = mod.directory / mod.entry;
    const auto source = read_file(path, readable);
    if (!readable) {
        error = std::format("cannot read {}", mod.entry);
        return false;
    }
    lua_State* L = luaL_newstate();
    if (L == nullptr) {
        return true;
    }
    const auto chunk = chunk_name(path);
    const bool ok = luaL_loadbufferx(L, source.data(), source.size(), chunk.c_str(), "t") == LUA_OK;
    if (!ok) {
        error = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "syntax error";
    }
    lua_close(L);
    return ok;
}

Evaluation evaluate(const std::string& target, const std::string& code, unsigned timeout_ms) {
    const auto instance = find_instance(target.empty() ? kConsoleId : target);
    if (instance == nullptr) {
        return {false, std::format("no script named '{}'", target), {}};
    }
    console_append(Output::Print, "> " + instance->id, code);

    Evaluation result;
    if (!loader::on_game_thread() && loader::game_thread_ticking(750)) {
        auto work = std::make_shared<EvalWork>();
        work->instance = instance;
        work->code = code;
        const auto* table = loader::api();
        table->run_on_game_thread(
            [](void* user) {
                const std::unique_ptr<std::shared_ptr<EvalWork>> held(
                    static_cast<std::shared_ptr<EvalWork>*>(user));
                auto& w = **held;
                {
                    std::scoped_lock lock(w.mutex);
                    if (w.cancelled) {
                        return;
                    }
                }
                auto evaluation = evaluate_now(*w.instance, w.code);
                std::scoped_lock lock(w.mutex);
                w.result = std::move(evaluation);
                w.done = true;
                w.ready.notify_all();
            },
            new std::shared_ptr<EvalWork>(work));
        std::unique_lock lock(work->mutex);
        if (!work->ready.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [&] { return work->done; })) {
            work->cancelled = true;
            result.result = std::format("the game thread did not run this within {} ms", timeout_ms);
        } else {
            result = std::move(work->result);
        }
    } else {
        result = evaluate_now(*instance, code);
    }
    if (!result.ok) {
        console_append(Output::Error, instance->id, result.result);
    } else if (!result.result.empty()) {
        console_append(Output::Result, instance->id, result.result);
    }
    return result;
}

void submit(const std::string& target, const std::string& code) {
    std::thread([target, code] { evaluate(target, code, 15000); }).detach();
}

std::vector<std::string> complete(const std::string& target, const std::string& prefix) {
    std::vector<std::string> out;
    const auto instance = find_instance(target.empty() ? kConsoleId : target);
    if (instance == nullptr) {
        return out;
    }
    Enter enter(*instance, 200, 500);
    if (!enter) {
        return out;
    }
    lua_State* L = instance->L;
    const int base = lua_gettop(L);
    if (lua_getglobal(L, "__bridger_complete") == LUA_TFUNCTION) {
        lua_pushstring(L, prefix.c_str());
        if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_istable(L, -1)) {
            const auto count = luaL_len(L, -1);
            for (lua_Integer i = 1; i <= count && i <= 400; ++i) {
                if (lua_rawgeti(L, -1, i) == LUA_TSTRING) {
                    out.emplace_back(lua_tostring(L, -1));
                }
                lua_pop(L, 1);
            }
        }
    }
    lua_settop(L, base);
    return out;
}

std::vector<Line> console_lines(std::size_t max) {
    std::scoped_lock lock(g_console_mutex);
    const auto count = std::min(max, g_console.size());
    return {g_console.end() - static_cast<std::ptrdiff_t>(count), g_console.end()};
}

std::uint64_t console_since(std::uint64_t since, std::vector<Line>& out) {
    std::scoped_lock lock(g_console_mutex);
    out.clear();
    const std::uint64_t first = g_console_written - g_console.size();
    for (std::uint64_t i = std::max(since, first); i < g_console_written; ++i) {
        out.push_back(g_console[static_cast<std::size_t>(i - first)]);
    }
    return g_console_written;
}

void console_clear() {
    std::scoped_lock lock(g_console_mutex);
    g_console.clear();
}

std::vector<std::string> targets() {
    std::vector<std::string> out{std::string(kConsoleId)};
    std::scoped_lock lock(g_instances_mutex);
    for (const auto& [id, instance] : g_instances) {
        if (id != kConsoleId && instance->alive.load(std::memory_order_relaxed)) {
            out.push_back(id);
        }
    }
    return out;
}

std::vector<Status> statuses() {
    std::vector<std::shared_ptr<Instance>> all;
    {
        std::scoped_lock lock(g_instances_mutex);
        for (const auto& [id, instance] : g_instances) {
            all.push_back(instance);
        }
    }
    std::vector<Status> out;
    for (const auto& instance : all) {
        Status status;
        status.id = instance->id;
        status.alive = instance->alive.load(std::memory_order_relaxed);
        {
            std::scoped_lock lock(instance->status_mutex);
            status.last_error = instance->last_error;
            status.errors = instance->error_count;
        }
        status.callbacks = instance->callbacks.load(std::memory_order_relaxed);
        status.hooks = count_hooks(*instance);
        status.memory_kb = instance->memory.load(std::memory_order_relaxed) / 1024;
        out.push_back(std::move(status));
    }
    return out;
}

}
