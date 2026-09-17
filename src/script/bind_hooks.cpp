#define _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING

#include <Windows.h>

#include <algorithm>
#include <format>
#include <map>
#include <mutex>

#include "content/content.h"
#include "content/content_internal.h"
#include "core/log.h"
#include "decima/dumper.h"
#include "script/bind.h"

namespace bridger::script {
namespace {

constexpr unsigned kHookBudgetMs = 1000;
constexpr unsigned kHookWaitMs = 2000;

enum class Mode { Replace, Before, After };

struct Subscriber {
    std::uint64_t id = 0;
    std::shared_ptr<Instance> instance;
    int ref = LUA_NOREF;
    Mode mode = Mode::Replace;
    std::uintptr_t only_self = 0;
};

struct Target {
    void* address = nullptr;
    const Callable* callable = nullptr;
    int slot = -1;
    bool callback = false;
    std::shared_ptr<const std::vector<Subscriber>> chain = std::make_shared<const std::vector<Subscriber>>();
    std::atomic<std::uint64_t> calls{0};
};

std::mutex g_mutex;
std::map<void*, std::unique_ptr<Target>> g_targets;
std::vector<std::unique_ptr<Target>> g_retired;
std::uint64_t g_next_id = 1;

struct Args {
    std::array<std::uint64_t, 48> ints{};
    std::array<double, 4> xmms{};
    std::size_t count = 0;
};

struct Invocation {
    Target* target = nullptr;
    const std::vector<Subscriber>* chain = nullptr;
    void* trampoline = nullptr;
};

ffi::Result run_chain(Invocation& invocation, std::size_t index, const Args& args);

std::uintptr_t hidden_of(const Target& target, const Args& args) {
    return target.callable->signature.hidden_result() ? static_cast<std::uintptr_t>(args.ints[0]) : 0;
}

ffi::Result call_through(Invocation& invocation, const Args& args) {
    ffi::Result result;
    if (invocation.trampoline == nullptr) {
        return result;
    }
    const auto fault = ffi::call(invocation.trampoline, args.ints.data(), args.xmms.data(),
                                 std::max<std::size_t>(args.count, 4), result);
    if (fault != 0) {
        log::error("script: {} faulted ({:#010x}) with the arguments a hook passed it",
                   invocation.target->callable->label, fault);
    }
    return result;
}

int push_arguments(lua_State* L, const Target& target, const Args& args) {
    const auto& signature = target.callable->signature;
    std::size_t slot = signature.hidden_result() ? 1 : 0;
    for (std::size_t i = 0; i < signature.params.size(); ++i, ++slot) {
        const bool in_register = slot < 4;
        push_argument(L, signature.params[i], target.callable->layouts[i], args.ints[slot],
                      in_register ? args.xmms[slot] : 0.0, in_register);
    }
    return static_cast<int>(signature.params.size());
}

struct Continuation {
    Invocation* invocation = nullptr;
    std::size_t next = 0;
    const Args* args = nullptr;
    bool called = false;
    ffi::Result result;
};

struct ContinuationRef {
    Continuation* continuation = nullptr;
    bool live = false;
};

int lua_original(lua_State* L) {
    auto* ref = static_cast<ContinuationRef*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (ref == nullptr || !ref->live) {
        return luaL_error(L, "original() can only be called while the hook that received it runs");
    }
    auto& continuation = *ref->continuation;
    const auto& target = *continuation.invocation->target;
    const auto& signature = target.callable->signature;

    Args next = *continuation.args;
    Marshal marshal;
    const int given = lua_gettop(L);
    if (given > 0) {
        if (static_cast<std::size_t>(given) != signature.params.size()) {
            return luaL_error(L, "original() takes all %d argument(s) or none",
                              static_cast<int>(signature.params.size()));
        }
        std::size_t slot = signature.hidden_result() ? 1 : 0;
        for (std::size_t i = 0; i < signature.params.size(); ++i, ++slot) {
            marshal_argument(L, static_cast<int>(i) + 1, signature.params[i], slot, marshal,
                             std::format("argument {} of original", i + 1));
            next.ints[slot] = marshal.ints[slot];
            if (slot < 4) {
                next.xmms[slot] = marshal.xmms[slot];
            }
        }
    }
    continuation.result = run_chain(*continuation.invocation, continuation.next, next);
    continuation.called = true;
    const int top = lua_gettop(L);
    push_result(L, signature.result, target.callable->result_layout, continuation.result,
                hidden_of(target, next));
    return lua_gettop(L) - top;
}

struct Job {
    Invocation* invocation = nullptr;
    std::size_t index = 0;
    const Args* args = nullptr;
    const Subscriber* subscriber = nullptr;
    Continuation continuation;
    ffi::Result result;
};

int subscriber_body(lua_State* L) {
    auto& job = *static_cast<Job*>(lua_touserdata(L, lua_upvalueindex(1)));
    const auto& subscriber = *job.subscriber;
    const auto& target = *job.invocation->target;
    const auto& signature = target.callable->signature;
    const auto hidden = hidden_of(target, *job.args);
    auto& continuation = job.continuation;

    lua_rawgeti(L, LUA_REGISTRYINDEX, subscriber.ref);
    int nargs = 0;
    if (subscriber.mode == Mode::Replace) {
        lua_pushvalue(L, lua_upvalueindex(2));
        lua_pushcclosure(L, lua_original, 1);
        ++nargs;
    } else if (subscriber.mode == Mode::After) {
        continuation.result = run_chain(*job.invocation, job.index + 1, *job.args);
        continuation.called = true;
        if (signature.result.kind != ffi::Kind::Void) {
            push_result(L, signature.result, target.callable->result_layout, continuation.result,
                        hidden);
            ++nargs;
        }
    }
    nargs += push_arguments(L, target, *job.args);
    lua_call(L, nargs, 1);

    const int value = lua_gettop(L);
    const bool nothing = lua_isnil(L, value);
    const bool skipped = is_skip(L, value);
    const bool is_void = signature.result.kind == ffi::Kind::Void;
    const auto next = [&] { return run_chain(*job.invocation, job.index + 1, *job.args); };

    switch (subscriber.mode) {
        case Mode::Replace:
            if (skipped || (is_void && !nothing)) {
                job.result = continuation.called ? continuation.result : ffi::Result{};
            } else if (nothing) {
                job.result = continuation.called ? continuation.result : next();
            } else {
                job.result = continuation.result;
                to_result(L, value, signature.result, hidden, job.result, target.callable->label);
            }
            break;
        case Mode::Before:
            if (nothing) {
                job.result = next();
            } else if (skipped || is_void) {
                job.result = ffi::Result{};
            } else {
                to_result(L, value, signature.result, hidden, job.result, target.callable->label);
            }
            break;
        case Mode::After:
            job.result = continuation.result;
            if (!nothing && !skipped && !is_void) {
                to_result(L, value, signature.result, hidden, job.result, target.callable->label);
            }
            break;
    }
    return 0;
}

ffi::Result run_subscriber(Invocation& invocation, std::size_t index, const Args& args,
                           const Subscriber& subscriber) {
    auto& instance = *subscriber.instance;
    lua_State* L = instance.L;
    const int base = lua_gettop(L);

    Job job;
    job.invocation = &invocation;
    job.index = index;
    job.args = &args;
    job.subscriber = &subscriber;
    job.continuation.invocation = &invocation;
    job.continuation.next = index + 1;
    job.continuation.args = &args;

    auto* ref = static_cast<ContinuationRef*>(lua_newuserdatauv(L, sizeof(ContinuationRef), 0));
    ref->continuation = &job.continuation;
    ref->live = true;
    lua_pushlightuserdata(L, &job);
    lua_pushvalue(L, base + 1);
    lua_pushcclosure(L, subscriber_body, 2);
    const bool ok = protected_call(instance, 0, 0, invocation.target->callable->label);
    ref->live = false;
    lua_settop(L, base);
    if (ok) {
        return job.result;
    }
    return job.continuation.called ? job.continuation.result
                                   : run_chain(invocation, index + 1, args);
}

ffi::Result run_chain(Invocation& invocation, std::size_t index, const Args& args) {
    const auto& chain = *invocation.chain;
    const auto& signature = invocation.target->callable->signature;
    for (; index < chain.size(); ++index) {
        const auto& subscriber = chain[index];
        if (subscriber.only_self != 0) {
            const std::size_t self = signature.hidden_result() ? 1 : 0;
            if (args.ints[self] != subscriber.only_self) {
                continue;
            }
        }
        Enter enter(*subscriber.instance, kHookWaitMs, kHookBudgetMs);
        if (!enter) {
            continue;
        }
        return run_subscriber(invocation, index, args, subscriber);
    }
    return call_through(invocation, args);
}

std::uint64_t dispatch(void* user, ffi::Frame& frame) {
    auto* target = static_cast<Target*>(user);
    target->calls.fetch_add(1, std::memory_order_relaxed);
    const auto chain = std::atomic_load(&target->chain);

    Args args;
    args.count = std::min<std::size_t>(std::max<std::size_t>(frame.slots, 4), args.ints.size());
    std::copy_n(frame.ints, args.count, args.ints.begin());
    std::copy_n(frame.xmms, 4, args.xmms.begin());

    Invocation invocation{target, chain.get(), frame.trampoline};
    ffi::Result result;
    try {
        result = run_chain(invocation, 0, args);
    } catch (...) {
        result = call_through(invocation, args);
    }
    *frame.xmm0_out = result.xmm0;
    return result.rax;
}

struct Resolved {
    const Callable* callable = nullptr;
    int handler = 0;
};

const Callable* message_handler(lua_State* L, const char* class_name, const char* message_name) {
    const void* rtti = content::class_rtti(class_name);
    if (rtti == nullptr) {
        luaL_error(L, "no class named '%s'", class_name);
    }
    const void* message = decima::find_type(message_name);
    if (message == nullptr) {
        luaL_error(L, "no message type named '%s'", message_name);
    }
    auto* address = content::handler_for_rtti(rtti, message);
    if (address == nullptr) {
        std::string handled;
        if (const auto* layout = reflect::layout(rtti); layout != nullptr) {
            for (const auto& name : layout->messages) {
                handled += (handled.empty() ? "" : ", ") + name;
            }
        }
        luaL_error(L, "%s does not handle %s (it handles: %s)", class_name, message_name,
                   handled.empty() ? "nothing of its own" : handled.c_str());
    }
    ffi::Signature signature;
    ffi::Type self;
    self.kind = ffi::Kind::Pointer;
    self.size = 8;
    self.depth = 1;
    self.name = class_name;
    ffi::Type argument = self;
    argument.name = message_name;
    signature.params = {self, argument};
    return make_callable(address, std::format("{}::{}", class_name, message_name),
                         std::move(signature), true);
}

Resolved resolve(lua_State* L, int first, bool wants_function) {
    Resolved out;
    const int count = lua_gettop(L) - first + 1;
    const auto needs = [&](int index) {
        if (wants_function) {
            luaL_checktype(L, index, LUA_TFUNCTION);
            out.handler = index;
        }
    };
    if (auto* view = luaL_testudata(L, first, kFunctionMeta); view != nullptr) {
        out.callable = to_callable(L, first);
        needs(first + 1);
    } else if (lua_type(L, first) == LUA_TSTRING && lua_type(L, first + 1) == LUA_TSTRING
               && count >= (wants_function ? 3 : 2)
               && std::string_view(lua_tostring(L, first)).find("::") == std::string_view::npos) {
        out.callable = message_handler(L, lua_tostring(L, first), lua_tostring(L, first + 1));
        needs(first + 2);
    } else if (lua_type(L, first) == LUA_TSTRING) {
        std::string error;
        out.callable = callable_for_symbol(lua_tostring(L, first), error);
        if (out.callable == nullptr) {
            lua_pushstring(L, error.c_str());
            lua_error(L);
        }
        needs(first + 1);
    } else if (lua_isinteger(L, first) && lua_type(L, first + 1) == LUA_TSTRING) {
        ffi::Signature signature;
        std::string error;
        if (!ffi::parse(lua_tostring(L, first + 1), type_info(), signature, error)) {
            lua_pushstring(L, error.c_str());
            lua_error(L);
        }
        const auto address = static_cast<std::uintptr_t>(lua_tointeger(L, first));
        out.callable = make_callable(reinterpret_cast<void*>(address),
                                     std::format("function at {:#x}", address), std::move(signature),
                                     true);
        needs(first + 2);
    } else {
        luaL_error(L, "expected \"Group::Name\", an engine function, \"Class\", \"Message\", or an "
                      "address and a signature");
    }
    if (!out.callable->known) {
        luaL_error(L, "%s has no recorded signature; hook it through fn(address, \"result(params)\")",
                   out.callable->label.c_str());
    }
    if (!content::in_code(out.callable->address)) {
        luaL_error(L, "%s is not inside the game's code", out.callable->label.c_str());
    }
    return out;
}

struct HookHandle {
    std::uint64_t id = 0;
    Target* target = nullptr;
};

std::uint64_t subscribe(lua_State* L, const Callable& callable, int handler, Mode mode,
                        std::uintptr_t only_self, Target*& target_out) {
    auto& instance = current(L);
    std::scoped_lock lock(g_mutex);

    auto found = g_targets.find(callable.address);
    Target* target = nullptr;
    if (found == g_targets.end()) {
        auto created = std::make_unique<Target>();
        created->address = callable.address;
        created->callable = &callable;
        std::string error;
        created->slot = ffi::create_hook(callable.address, callable.signature.slot_count(), dispatch,
                                         created.get(), error);
        if (created->slot < 0) {
            luaL_error(L, "cannot hook %s: %s", callable.label.c_str(), error.c_str());
        }
        target = created.get();
        g_targets.emplace(callable.address, std::move(created));
    } else {
        target = found->second.get();
        if (target->callback) {
            luaL_error(L, "%s is a script callback, not an engine function", callable.label.c_str());
        }
    }

    lua_pushvalue(L, handler);
    Subscriber subscriber;
    subscriber.id = g_next_id++;
    subscriber.instance = find_instance(instance.id);
    subscriber.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    subscriber.mode = mode;
    subscriber.only_self = only_self;
    if (subscriber.instance == nullptr) {
        luaL_unref(L, LUA_REGISTRYINDEX, subscriber.ref);
        luaL_error(L, "the script is not registered");
    }

    auto next = std::make_shared<std::vector<Subscriber>>(*std::atomic_load(&target->chain));
    next->push_back(std::move(subscriber));
    const auto id = next->back().id;
    std::atomic_store(&target->chain, std::shared_ptr<const std::vector<Subscriber>>(std::move(next)));

    if (!ffi::enable_hook(target->slot) && std::atomic_load(&target->chain)->size() == 1) {
        luaL_error(L, "cannot enable the hook on %s", callable.label.c_str());
    }
    target_out = target;
    return id;
}

void push_hook(lua_State* L, std::uint64_t id, Target* target) {
    auto* handle = static_cast<HookHandle*>(lua_newuserdatauv(L, sizeof(HookHandle), 0));
    handle->id = id;
    handle->target = target;
    luaL_setmetatable(L, kHookMeta);
}

void unsubscribe_where(const std::function<bool(const Subscriber&)>& match, lua_State* L) {
    std::scoped_lock lock(g_mutex);
    for (auto& [address, target] : g_targets) {
        const auto chain = std::atomic_load(&target->chain);
        if (std::none_of(chain->begin(), chain->end(), match)) {
            continue;
        }
        auto next = std::make_shared<std::vector<Subscriber>>();
        for (const auto& subscriber : *chain) {
            if (!match(subscriber)) {
                next->push_back(subscriber);
            } else if (L != nullptr && subscriber.instance.get() == &current(L)) {
                luaL_unref(L, LUA_REGISTRYINDEX, subscriber.ref);
            }
        }
        const bool empty = next->empty();
        std::atomic_store(&target->chain, std::shared_ptr<const std::vector<Subscriber>>(std::move(next)));
        if (empty && !target->callback) {
            ffi::disable_hook(target->slot);
        }
    }
}

void collect_idle(unsigned wait_ms) {
    std::vector<std::unique_ptr<Target>> removed;
    {
        std::scoped_lock lock(g_mutex);
        for (auto it = g_targets.begin(); it != g_targets.end();) {
            auto& target = it->second;
            if (target->callback || !std::atomic_load(&target->chain)->empty()) {
                ++it;
                continue;
            }
            if (wait_ms == 0 && ffi::in_flight(target->slot) > 0) {
                ++it;
                continue;
            }
            removed.push_back(std::move(target));
            it = g_targets.erase(it);
        }
    }
    for (auto& target : removed) {
        if (ffi::remove_hook(target->slot, wait_ms)) {
            continue;
        }
        std::scoped_lock lock(g_mutex);
        g_retired.push_back(std::move(target));
    }
}

int lua_subscribe(lua_State* L, Mode mode) {
    const auto resolved = resolve(L, 1, true);
    collect_idle(0);
    Target* target = nullptr;
    const auto id = subscribe(L, *resolved.callable, resolved.handler, mode, 0, target);
    push_hook(L, id, target);
    return 1;
}

int lua_hook(lua_State* L) { return lua_subscribe(L, Mode::Replace); }
int lua_before(lua_State* L) { return lua_subscribe(L, Mode::Before); }
int lua_after(lua_State* L) { return lua_subscribe(L, Mode::After); }

int lua_on(lua_State* L) {
    luaL_checkstring(L, 1);
    luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    return lua_subscribe(L, Mode::After);
}

int return_upvalue(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

int lua_force(lua_State* L) {
    const auto resolved = resolve(L, 1, false);
    const int value = lua_gettop(L);
    const auto& callable = *resolved.callable;
    const auto kind = callable.signature.result.kind;
    bool hooked = false;
    {
        std::scoped_lock lock(g_mutex);
        hooked = g_targets.contains(callable.address);
    }
    const bool integral = kind != ffi::Kind::F32 && kind != ffi::Kind::F64 && kind != ffi::Kind::Void
                       && kind != ffi::Kind::StructMemory;
    if (integral && !hooked) {
        ffi::Result result;
        to_result(L, value, callable.signature.result, 0, result, callable.label);
        const auto handle = content::api()->force_result(callable.address, result.rax);
        if (handle == 0) {
            return luaL_error(L, "cannot force %s (hooked by a mod?)", callable.label.c_str());
        }
        push_entry(L, handle, std::format("{} forced", callable.label));
        return 1;
    }
    if (kind == ffi::Kind::StructMemory) {
        return luaL_error(L, "%s returns a struct; hook it and return a value instead",
                          callable.label.c_str());
    }
    lua_pushvalue(L, value);
    lua_pushcclosure(L, return_upvalue, 1);
    const int handler = lua_gettop(L);
    Target* target = nullptr;
    const auto id = subscribe(L, callable, handler, Mode::Before, 0, target);
    push_hook(L, id, target);
    return 1;
}

int hook_remove(lua_State* L) {
    auto* handle = static_cast<HookHandle*>(luaL_checkudata(L, 1, kHookMeta));
    if (handle->id != 0) {
        const auto id = handle->id;
        unsubscribe_where([id](const Subscriber& s) { return s.id == id; }, L);
        handle->id = 0;
    }
    return 0;
}

int hook_index(lua_State* L) {
    auto* handle = static_cast<HookHandle*>(luaL_checkudata(L, 1, kHookMeta));
    const std::string_view key = luaL_checkstring(L, 2);
    if (key == "remove") {
        lua_pushcfunction(L, hook_remove);
    } else if (key == "calls") {
        lua_pushinteger(L, static_cast<lua_Integer>(handle->target->calls.load()));
    } else if (key == "name") {
        lua_pushstring(L, handle->target->callable->label.c_str());
    } else if (key == "active") {
        lua_pushboolean(L, handle->id != 0);
    } else {
        return luaL_error(L, "hooks have remove(), calls, name and active");
    }
    return 1;
}

int hook_tostring(lua_State* L) {
    auto* handle = static_cast<HookHandle*>(luaL_checkudata(L, 1, kHookMeta));
    lua_pushstring(L, std::format("hook on {} ({} calls{})", handle->target->callable->label,
                                  handle->target->calls.load(), handle->id == 0 ? ", removed" : "")
                          .c_str());
    return 1;
}

int view_subscribe(lua_State* L, Mode mode) {
    auto& view = check_view(L, 1);
    const char* message = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (view.layout == nullptr) {
        return luaL_error(L, "no class at %p", reinterpret_cast<void*>(view.address));
    }
    const auto* callable = message_handler(L, view.layout->name.c_str(), message);
    Target* target = nullptr;
    const auto id = subscribe(L, *callable, 3, mode, view.address, target);
    push_hook(L, id, target);
    return 1;
}

int view_on(lua_State* L) { return view_subscribe(L, Mode::After); }
int view_before(lua_State* L) { return view_subscribe(L, Mode::Before); }
int view_replace(lua_State* L) { return view_subscribe(L, Mode::Replace); }

Target* make_callback_target(lua_State* L, const Callable& shape, int handler, void* fallback,
                             std::string label) {
    const Mode mode = fallback != nullptr ? Mode::Replace : Mode::Before;
    auto& instance = current(L);
    auto target = std::make_unique<Target>();
    target->callback = true;
    std::string error;
    void* address = nullptr;
    target->slot = ffi::create_callback(shape.signature.slot_count(), dispatch, target.get(), fallback,
                                        address, error);
    if (target->slot < 0) {
        luaL_error(L, "%s", error.c_str());
    }
    target->address = address;
    target->callable = make_callable(address, std::move(label), shape.signature, true);

    lua_pushvalue(L, handler);
    Subscriber subscriber;
    subscriber.instance = find_instance(instance.id);
    subscriber.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    subscriber.mode = mode;
    std::scoped_lock lock(g_mutex);
    subscriber.id = g_next_id++;
    std::atomic_store(&target->chain, std::make_shared<const std::vector<Subscriber>>(
                                          std::vector<Subscriber>{std::move(subscriber)}));
    auto* raw = target.get();
    g_targets.emplace(address, std::move(target));
    return raw;
}

int lua_callback(lua_State* L) {
    ffi::Signature signature;
    std::string error;
    if (!ffi::parse(luaL_checkstring(L, 1), type_info(), signature, error)) {
        return luaL_error(L, "%s", error.c_str());
    }
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const auto* shape = make_callable(nullptr, "callback", std::move(signature), true);
    auto* target = make_callback_target(L, *shape, 2, nullptr, "script callback");
    push_callable(L, target->callable);
    return 1;
}

int view_override(lua_State* L) {
    auto& view = check_view(L, 1);
    const auto slot = static_cast<std::uint32_t>(luaL_checkinteger(L, 2));
    ffi::Signature signature;
    std::string error;
    if (!ffi::parse(luaL_checkstring(L, 3), type_info(), signature, error)) {
        return luaL_error(L, "%s", error.c_str());
    }
    luaL_checktype(L, 4, LUA_TFUNCTION);
    void** table = content::api()->vtable_of(reinterpret_cast<const void*>(view.address));
    void* previous = nullptr;
    if (table == nullptr || !reflect::read(reinterpret_cast<std::uintptr_t>(table + slot), &previous, 8)
        || !content::in_code(previous)) {
        return luaL_error(L, "%s has no virtual method %d", std::format("{:#x}", view.address).c_str(),
                          static_cast<int>(slot));
    }
    const auto* shape = make_callable(nullptr, "method", std::move(signature), true);
    auto* target = make_callback_target(
        L, *shape, 4, previous,
        std::format("{}@{:#x} vtable[{}]", view.layout != nullptr ? view.layout->name : "object",
                    view.address, slot));
    void* displaced = nullptr;
    const auto handle = content::api()->override_instance(reinterpret_cast<void*>(view.address), slot,
                                                          target->address, &displaced);
    if (handle == 0) {
        return luaL_error(L, "the vtable override was refused");
    }
    push_entry(L, handle, target->callable->label);
    return 1;
}

}

void release_hooks(Instance& instance) {
    unsubscribe_where([&](const Subscriber& s) { return s.instance.get() == &instance; }, nullptr);
    collect_idle(2000);
}

void release_callbacks(Instance& instance) {
    std::vector<std::unique_ptr<Target>> released;
    {
        std::scoped_lock lock(g_mutex);
        for (auto it = g_targets.begin(); it != g_targets.end();) {
            const auto chain = std::atomic_load(&it->second->chain);
            const bool mine = it->second->callback
                           && std::any_of(chain->begin(), chain->end(), [&](const Subscriber& s) {
                                  return s.instance.get() == &instance;
                              });
            if (!mine) {
                ++it;
                continue;
            }
            std::atomic_store(&it->second->chain, std::make_shared<const std::vector<Subscriber>>());
            released.push_back(std::move(it->second));
            it = g_targets.erase(it);
        }
    }
    for (auto& target : released) {
        ffi::release_callback(target->slot, 2000);
        std::scoped_lock lock(g_mutex);
        g_retired.push_back(std::move(target));
    }
}

std::size_t count_hooks(const Instance& instance) {
    std::scoped_lock lock(g_mutex);
    std::size_t count = 0;
    for (const auto& [address, target] : g_targets) {
        const auto chain = std::atomic_load(&target->chain);
        count += static_cast<std::size_t>(std::count_if(chain->begin(), chain->end(),
                                                        [&](const Subscriber& s) {
                                                            return s.instance.get() == &instance;
                                                        }));
    }
    return count;
}

void open_hooks(lua_State* L) {
    luaL_newmetatable(L, kHookMeta);
    lua_pushcfunction(L, hook_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, hook_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    const luaL_Reg globals[] = {
        {"hook", lua_hook},     {"before", lua_before},     {"after", lua_after},
        {"on", lua_on},         {"force", lua_force},       {"callback", lua_callback},
        {nullptr, nullptr},
    };
    for (const auto* entry = globals; entry->name != nullptr; ++entry) {
        lua_pushcfunction(L, entry->func);
        lua_setglobal(L, entry->name);
    }

    luaL_getmetatable(L, kFunctionMeta);
    lua_getfield(L, -1, "methods");
    lua_pushcfunction(L, lua_hook);
    lua_setfield(L, -2, "hook");
    lua_pushcfunction(L, lua_before);
    lua_setfield(L, -2, "before");
    lua_pushcfunction(L, lua_after);
    lua_setfield(L, -2, "after");
    lua_pushcfunction(L, lua_force);
    lua_setfield(L, -2, "force");
    lua_pop(L, 2);

    luaL_getmetatable(L, kViewMeta);
    lua_getfield(L, -1, "methods");
    lua_pushcfunction(L, view_on);
    lua_setfield(L, -2, "on");
    lua_pushcfunction(L, view_before);
    lua_setfield(L, -2, "before");
    lua_pushcfunction(L, view_replace);
    lua_setfield(L, -2, "replace");
    lua_pushcfunction(L, view_override);
    lua_setfield(L, -2, "override");
    lua_pop(L, 2);
}

}
