#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <format>
#include <string>

#include "core/settings.h"
#include "fx/fx.h"
#include "loader/registry.h"
#include "overlay/overlay.h"
#include "script/bind.h"
#include "script/runtime.h"
#include "ui/ui.h"

namespace bridger::script {
namespace {

int fx_upscale_info(lua_State* L) {
    std::uint32_t width = 0, height = 0;
    float jitter[2]{};
    const bool available = fx::api()->upscale_info(&width, &height, jitter);
    lua_newtable(L);
    lua_pushboolean(L, available); lua_setfield(L, -2, "available");
    lua_pushinteger(L, width); lua_setfield(L, -2, "width");
    lua_pushinteger(L, height); lua_setfield(L, -2, "height");
    lua_pushnumber(L, jitter[0]); lua_setfield(L, -2, "jitter_x");
    lua_pushnumber(L, jitter[1]); lua_setfield(L, -2, "jitter_y");
    return 1;
}

int fx_pre_pass(lua_State* L) {
    const char* source = luaL_checkstring(L, 1);
    BridgerFxShaderDesc shader{};
    shader.source = source;
    shader.name = "Lua pre upscale shader";
    shader.kind = BRIDGER_FX_SHADER_FULLSCREEN;
    BridgerFxPassDesc pass{};
    pass.name = "Lua pre upscale pass";
    pass.stage = BRIDGER_FX_STAGE_PRE_UPSCALE;
    pass.enabled = true;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "history"); pass.wants_history = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
        lua_getfield(L, 2, "priority"); pass.priority = static_cast<int>(luaL_optinteger(L, -1, 0)); lua_pop(L, 1);
    }
    const auto shader_id = fx::api()->create_shader(&shader);
    pass.shader = shader_id;
    const auto effect = fx::api()->create_pass(&pass);
    if (!effect) fx::api()->destroy_shader(shader_id);
    lua_pushinteger(L, effect);
    return 1;
}
int fx_revert(lua_State*) { fx::api()->revert_owned(); return 0; }

std::uint64_t hash_arg(lua_State* L, int index) {
    const char* text = luaL_checkstring(L, index);
    return std::strtoull(text, nullptr, 16);
}

void push_pipeline(lua_State* L, const BridgerFxPipelineInfo& info) {
    lua_newtable(L);
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(info.hash));
    lua_pushstring(L, hex); lua_setfield(L, -2, "hash");
    lua_pushboolean(L, info.compute != 0); lua_setfield(L, -2, "compute");
    lua_pushinteger(L, info.draws_last_frame); lua_setfield(L, -2, "draws");
    lua_pushinteger(L, info.draws_total); lua_setfield(L, -2, "draws_total");
    lua_pushinteger(L, info.first_use); lua_setfield(L, -2, "first_use");
    lua_pushinteger(L, info.render_targets); lua_setfield(L, -2, "render_targets");
    lua_pushboolean(L, info.replaced != 0); lua_setfield(L, -2, "replaced");
    lua_pushboolean(L, info.hooked != 0); lua_setfield(L, -2, "hooked");
}

int fx_pipelines(lua_State* L) {
    const auto count = fx::api()->pipeline_count();
    lua_createtable(L, static_cast<int>(count), 0);
    for (std::uint32_t i = 0; i < count; ++i) {
        BridgerFxPipelineInfo info{};
        if (!fx::api()->pipeline_at(i, &info)) continue;
        push_pipeline(L, info);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

int fx_pipeline(lua_State* L) {
    BridgerFxPipelineInfo info{};
    if (!fx::api()->pipeline_find(hash_arg(L, 1), &info)) { lua_pushnil(L); return 1; }
    push_pipeline(L, info);
    return 1;
}

int fx_pipeline_dump(lua_State* L) {
    char path[512];
    const auto n = fx::api()->pipeline_dump(hash_arg(L, 1), path, sizeof path);
    if (n == 0) { lua_pushnil(L); return 1; }
    lua_pushstring(L, path);
    return 1;
}

int fx_material_hook(lua_State* L) {
    BridgerFxMaterialHookDesc desc{};
    desc.path = luaL_checkstring(L, 1);
    desc.min_render_targets = 2;
    std::vector<std::string> defines;
    std::vector<const char*> pointers;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "entry"); desc.entry = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr; lua_pop(L, 1);
        lua_getfield(L, 2, "min_targets"); desc.min_render_targets = static_cast<std::uint32_t>(luaL_optinteger(L, -1, 2)); lua_pop(L, 1);
        lua_getfield(L, 2, "max_targets"); desc.max_render_targets = static_cast<std::uint32_t>(luaL_optinteger(L, -1, 0)); lua_pop(L, 1);
        lua_getfield(L, 2, "only"); if (lua_isstring(L, -1)) desc.only_pipeline = std::strtoull(lua_tostring(L, -1), nullptr, 16); lua_pop(L, 1);
        lua_getfield(L, 2, "defines");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                if (lua_isstring(L, -1)) defines.emplace_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    for (const auto& d : defines) pointers.push_back(d.c_str());
    desc.defines = pointers.empty() ? nullptr : pointers.data();
    desc.define_count = static_cast<std::uint32_t>(pointers.size());
    lua_pushinteger(L, fx::api()->material_hook(&desc));
    return 1;
}

int fx_pipeline_problem(lua_State* L) {
    const auto handle = static_cast<BridgerFxHandle>(luaL_checkinteger(L, 1));
    std::string text(4096, 0);
    const auto n = fx::api()->pipeline_problem(handle, text.data(), text.size());
    text.resize(std::min(n, text.size() - 1));
    lua_pushstring(L, text.c_str());
    return 1;
}

int fx_pipeline_revert(lua_State* L) {
    fx::api()->pipeline_revert(static_cast<BridgerFxHandle>(luaL_checkinteger(L, 1)));
    return 0;
}

std::string joined(lua_State* L, int first) {
    std::string text;
    const int top = lua_gettop(L);
    for (int i = first; i <= top; ++i) {
        if (i != first) {
            text += "\t";
        }
        std::size_t length = 0;
        const char* part = luaL_tolstring(L, i, &length);
        text.append(part, length);
        lua_pop(L, 1);
    }
    return text;
}

int lua_print(lua_State* L) {
    emit(current(L), Output::Print, joined(L, 1));
    return 0;
}

int log_info(lua_State* L) {
    emit(current(L), Output::Info, joined(L, 1));
    return 0;
}

int log_warn(lua_State* L) {
    emit(current(L), Output::Warn, joined(L, 1));
    return 0;
}

int log_error(lua_State* L) {
    emit(current(L), Output::Error, joined(L, 1));
    return 0;
}

int input_key_down(lua_State* L) {
    const auto key = static_cast<int>(luaL_checkinteger(L, 1));
    const bool down = !overlay::visible() && (GetAsyncKeyState(key) & 0x8000) != 0;
    lua_pushboolean(L, down);
    return 1;
}

int input_overlay(lua_State* L) {
    lua_pushboolean(L, overlay::visible());
    return 1;
}

int lua_now(lua_State* L) {
    static const std::int64_t origin = now_ticks();
    lua_pushnumber(L, static_cast<double>(now_ticks() - origin)
                          / static_cast<double>(ticks_per_ms() * 1000));
    return 1;
}

int settings_get(lua_State* L) {
    const auto& instance = current(L);
    const char* key = luaL_checkstring(L, 1);
    switch (lua_type(L, 2)) {
        case LUA_TBOOLEAN:
            lua_pushboolean(L, settings::get_bool(instance.id, key, lua_toboolean(L, 2) != 0));
            return 1;
        case LUA_TNUMBER: {
            const double value = settings::get_number(instance.id, key, lua_tonumber(L, 2));
            if (lua_isinteger(L, 2) && value == static_cast<double>(static_cast<lua_Integer>(value))) {
                lua_pushinteger(L, static_cast<lua_Integer>(value));
            } else {
                lua_pushnumber(L, value);
            }
            return 1;
        }
        case LUA_TSTRING: {
            const auto text = settings::get_text(instance.id, key, lua_tostring(L, 2));
            lua_pushlstring(L, text.data(), text.size());
            return 1;
        }
        default:
            return luaL_error(L, "setting '%s' needs a boolean, number or string default", key);
    }
}

int settings_set(lua_State* L) {
    const auto& instance = current(L);
    const char* key = luaL_checkstring(L, 1);
    switch (lua_type(L, 2)) {
        case LUA_TBOOLEAN: settings::set_bool(instance.id, key, lua_toboolean(L, 2) != 0); break;
        case LUA_TNUMBER: settings::set_number(instance.id, key, lua_tonumber(L, 2)); break;
        case LUA_TSTRING: settings::set_text(instance.id, key, lua_tostring(L, 2)); break;
        case LUA_TNIL: settings::erase(instance.id, key); break;
        default: return luaL_error(L, "setting '%s' takes a boolean, number or string", key);
    }
    return 0;
}

void panel_draw(void* user) {
    auto* record = static_cast<PanelRecord*>(user);
    auto& instance = *record->instance;
    const auto& theme = ui::theme();
    Enter enter(instance, 25, 250);
    if (!enter) {
        ui::text_colored(theme.text_faint, instance.alive ? "the script is busy on another thread"
                                                          : "the script has stopped");
        return;
    }
    lua_State* L = instance.L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, record->ref);
    instance.drawing = true;
    const bool ok = protected_call(instance, 0, 0, "panel");
    instance.drawing = false;
    if (!ok) {
        std::scoped_lock lock(instance.status_mutex);
        record->failure = instance.last_error;
    }
}

void failed_panel_draw(PanelRecord& record) {
    const auto& theme = ui::theme();
    ui::text_colored(theme.bad, "This panel raised an error and stopped drawing.");
    ui::text_wrapped(theme.text_dim, record.failure);
    if (ui::button("Retry")) {
        record.failure.clear();
    }
}

void panel_entry(void* user) {
    auto* record = static_cast<PanelRecord*>(user);
    if (!record->failure.empty()) {
        failed_panel_draw(*record);
        return;
    }
    panel_draw(user);
}

int lua_panel(lua_State* L) {
    auto& instance = current(L);
    const char* label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (instance.console) {
        return luaL_error(L, "panels belong to script mods; the console has none");
    }
    if (!instance.loading) {
        return luaL_error(L, "declare panels while the script loads, at its top level");
    }
    auto record = std::make_unique<PanelRecord>();
    record->instance = &instance;
    lua_pushvalue(L, 2);
    record->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    loader::Panel panel;
    panel.owner = instance.id;
    panel.label = label;
    panel.draw = panel_entry;
    panel.user = record.get();
    loader::Registry::instance().add_panel(std::move(panel));
    instance.panels.push_back(std::move(record));
    return 0;
}

void require_drawing(lua_State* L) {
    if (!current(L).drawing) {
        luaL_error(L, "ui functions only work inside a panel function");
    }
}

ui::Color color_at(lua_State* L, int index, ui::Color fallback) {
    const auto& theme = ui::theme();
    if (lua_isinteger(L, index)) {
        return static_cast<ui::Color>(lua_tointeger(L, index));
    }
    if (lua_type(L, index) != LUA_TSTRING) {
        return fallback;
    }
    const std::string_view name = lua_tostring(L, index);
    if (name == "text") return theme.text;
    if (name == "dim") return theme.text_dim;
    if (name == "faint") return theme.text_faint;
    if (name == "accent") return theme.accent;
    if (name == "good") return theme.good;
    if (name == "warn") return theme.warn;
    if (name == "bad") return theme.bad;
    return fallback;
}

std::string text_at(lua_State* L, int index) {
    std::size_t length = 0;
    const char* text = luaL_tolstring(L, index, &length);
    std::string out(text, length);
    lua_pop(L, 1);
    return out;
}

int ui_text(lua_State* L) {
    require_drawing(L);
    ui::text_colored(color_at(L, 2, ui::theme().text), text_at(L, 1));
    return 0;
}

int ui_wrapped(lua_State* L) {
    require_drawing(L);
    ui::text_wrapped(color_at(L, 2, ui::theme().text_dim), text_at(L, 1));
    return 0;
}

int ui_header(lua_State* L) {
    require_drawing(L);
    ui::header(text_at(L, 1));
    return 0;
}

int ui_separator(lua_State* L) {
    require_drawing(L);
    ui::separator();
    return 0;
}

int ui_spacing(lua_State* L) {
    require_drawing(L);
    ui::spacing(static_cast<float>(luaL_optnumber(L, 1, 0.0)));
    return 0;
}

int ui_same_line(lua_State* L) {
    require_drawing(L);
    ui::same_line(static_cast<float>(luaL_optnumber(L, 1, 0.0)));
    return 0;
}

int ui_newline(lua_State* L) {
    require_drawing(L);
    ui::newline();
    return 0;
}

int ui_indent(lua_State* L) {
    require_drawing(L);
    ui::indent(static_cast<float>(luaL_optnumber(L, 1, 16.0)));
    return 0;
}

int ui_unindent(lua_State* L) {
    require_drawing(L);
    ui::unindent(static_cast<float>(luaL_optnumber(L, 1, 16.0)));
    return 0;
}

int ui_button(lua_State* L) {
    require_drawing(L);
    lua_pushboolean(L, ui::button(text_at(L, 1), static_cast<float>(luaL_optnumber(L, 2, 0.0))));
    return 1;
}

int ui_ghost_button(lua_State* L) {
    require_drawing(L);
    lua_pushboolean(L, ui::ghost_button(text_at(L, 1), static_cast<float>(luaL_optnumber(L, 2, 0.0))));
    return 1;
}

int ui_checkbox(lua_State* L) {
    require_drawing(L);
    bool value = lua_toboolean(L, 2) != 0;
    const bool clicked = ui::checkbox(text_at(L, 1), value);
    if (clicked) {
        value = !value;
    }
    lua_pushboolean(L, value);
    lua_pushboolean(L, clicked);
    return 2;
}

int ui_slider(lua_State* L) {
    require_drawing(L);
    const auto label = text_at(L, 1);
    if (lua_isinteger(L, 2) && lua_isinteger(L, 3) && lua_isinteger(L, 4)) {
        int value = static_cast<int>(lua_tointeger(L, 2));
        const bool changed = ui::slider_int(label, value, static_cast<int>(lua_tointeger(L, 3)),
                                            static_cast<int>(lua_tointeger(L, 4)),
                                            static_cast<float>(luaL_optnumber(L, 5, 0.0)));
        lua_pushinteger(L, value);
        lua_pushboolean(L, changed);
        return 2;
    }
    float value = static_cast<float>(luaL_checknumber(L, 2));
    const bool changed = ui::slider_float(label, value, static_cast<float>(luaL_checknumber(L, 3)),
                                          static_cast<float>(luaL_checknumber(L, 4)),
                                          static_cast<float>(luaL_optnumber(L, 5, 0.0)));
    lua_pushnumber(L, value);
    lua_pushboolean(L, changed);
    return 2;
}

int ui_input(lua_State* L) {
    require_drawing(L);
    std::string value = luaL_optstring(L, 2, "");
    const bool changed = ui::input_text(text_at(L, 1), value,
                                        static_cast<float>(luaL_optnumber(L, 4, 0.0)),
                                        luaL_optstring(L, 3, ""));
    lua_pushlstring(L, value.data(), value.size());
    lua_pushboolean(L, changed);
    return 2;
}

int ui_readout(lua_State* L) {
    require_drawing(L);
    const auto label = text_at(L, 1);
    const auto value = text_at(L, 2);
    if (lua_isnoneornil(L, 3)) {
        ui::readout(label, value);
    } else {
        ui::readout_colored(label, color_at(L, 3, ui::theme().text), value);
    }
    return 0;
}

int ui_note(lua_State* L) {
    require_drawing(L);
    ui::note(text_at(L, 1));
    return 0;
}

int ui_badge(lua_State* L) {
    require_drawing(L);
    ui::badge(text_at(L, 1), color_at(L, 2, ui::theme().accent));
    return 0;
}

int ui_keycap(lua_State* L) {
    require_drawing(L);
    ui::keycap(text_at(L, 1));
    return 0;
}

int ui_tooltip(lua_State* L) {
    require_drawing(L);
    ui::tooltip(text_at(L, 1));
    return 0;
}

int ui_group(lua_State* L) {
    require_drawing(L);
    lua_pushboolean(L, ui::begin_group(text_at(L, 1), lua_isnoneornil(L, 2) || lua_toboolean(L, 2)));
    return 1;
}

int ui_end_group(lua_State* L) {
    require_drawing(L);
    ui::end_group();
    return 0;
}

int ui_progress(lua_State* L) {
    require_drawing(L);
    ui::progress(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_optnumber(L, 2, 0.0)),
                 color_at(L, 3, ui::theme().accent));
    return 0;
}

int ui_push_id(lua_State* L) {
    require_drawing(L);
    ui::push_id(text_at(L, 1));
    return 0;
}

int ui_pop_id(lua_State* L) {
    require_drawing(L);
    ui::pop_id();
    return 0;
}

int ui_scroll(lua_State* L) {
    require_drawing(L);
    ui::begin_scroll(text_at(L, 1), static_cast<float>(luaL_checknumber(L, 2)));
    return 0;
}

int ui_end_scroll(lua_State* L) {
    require_drawing(L);
    ui::end_scroll();
    return 0;
}

int ui_width(lua_State* L) {
    require_drawing(L);
    lua_pushnumber(L, ui::available_width());
    return 1;
}

int ui_revert(lua_State* L) {
    require_drawing(L);
    lua_pushboolean(L, ui::revert_marker(lua_toboolean(L, 1) != 0));
    return 1;
}

std::string option_string(lua_State* L, int options, const char* name) {
    if (!lua_istable(L, options)) {
        return {};
    }
    lua_getfield(L, options, name);
    std::string out = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return out;
}

double option_number(lua_State* L, int options, const char* name, double fallback) {
    if (!lua_istable(L, options)) {
        return fallback;
    }
    lua_getfield(L, options, name);
    const double out = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1);
    return out;
}

bool option_flag(lua_State* L, int options, const char* name) {
    if (!lua_istable(L, options)) {
        return false;
    }
    lua_getfield(L, options, name);
    const bool out = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return out;
}

int ui_setting(lua_State* L) {
    require_drawing(L);
    const auto label = text_at(L, 1);
    const int options = 3;
    const auto help = option_string(L, options, "help");

    if (lua_type(L, 2) == LUA_TBOOLEAN) {
        bool value = lua_toboolean(L, 2) != 0;
        const bool changed = ui::setting_bool(label, value, help);
        lua_pushboolean(L, value);
        lua_pushboolean(L, changed);
        return 2;
    }
    if (lua_type(L, 2) == LUA_TSTRING) {
        std::string value = lua_tostring(L, 2);
        const bool changed = ui::setting_text(label, value, option_string(L, options, "placeholder"), help);
        lua_pushlstring(L, value.data(), value.size());
        lua_pushboolean(L, changed);
        return 2;
    }
    if (option_flag(L, options, "key")) {
        auto key = static_cast<std::uint32_t>(luaL_checkinteger(L, 2));
        const bool changed = ui::setting_key(label, key, help);
        lua_pushinteger(L, key);
        lua_pushboolean(L, changed);
        return 2;
    }
    if (lua_istable(L, options)) {
        lua_getfield(L, options, "choices");
        if (lua_istable(L, -1)) {
            const int choices = lua_gettop(L);
            std::vector<std::string> storage;
            const auto count = luaL_len(L, choices);
            for (lua_Integer i = 1; i <= count; ++i) {
                lua_rawgeti(L, choices, i);
                storage.push_back(text_at(L, -1));
                lua_pop(L, 1);
            }
            std::vector<const char*> items;
            for (const auto& item : storage) {
                items.push_back(item.c_str());
            }
            int index = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
            const bool changed = ui::setting_combo(label, index, items.data(),
                                                   static_cast<int>(items.size()), help);
            lua_pop(L, 1);
            lua_pushinteger(L, index + 1);
            lua_pushboolean(L, changed);
            return 2;
        }
        lua_pop(L, 1);
    }
    const auto suffix = option_string(L, options, "suffix");
    if (option_flag(L, options, "int") || (lua_isinteger(L, 2) && !lua_istable(L, options))) {
        int value = static_cast<int>(luaL_checkinteger(L, 2));
        const bool changed = ui::setting_int(label, value, static_cast<int>(option_number(L, options, "min", 0)),
                                             static_cast<int>(option_number(L, options, "max", 100)),
                                             suffix, help);
        lua_pushinteger(L, value);
        lua_pushboolean(L, changed);
        return 2;
    }
    float value = static_cast<float>(luaL_checknumber(L, 2));
    const bool changed = ui::setting_float(label, value, static_cast<float>(option_number(L, options, "min", 0.0)),
                                           static_cast<float>(option_number(L, options, "max", 1.0)),
                                           suffix, help);
    lua_pushnumber(L, value);
    lua_pushboolean(L, changed);
    return 2;
}

int script_reload(lua_State* L) {
    const auto& instance = current(L);
    if (instance.console) {
        return luaL_error(L, "the console is not a mod; use console.reset()");
    }
    loader::Registry::instance().request_reload(instance.id);
    return 0;
}

int script_budget(lua_State* L) {
    auto& instance = current(L);
    const auto previous = instance.eval_budget_ms;
    if (!lua_isnoneornil(L, 1)) {
        instance.eval_budget_ms = static_cast<unsigned>(std::clamp<lua_Integer>(luaL_checkinteger(L, 1), 10, 600000));
    }
    lua_pushinteger(L, previous);
    return 1;
}

int script_reload_other(lua_State* L) {
    loader::Registry::instance().request_reload(luaL_checkstring(L, 1));
    return 0;
}

int script_list(lua_State* L) {
    lua_newtable(L);
    lua_Integer i = 0;
    for (const auto& status : statuses()) {
        lua_createtable(L, 0, 6);
        lua_pushstring(L, status.id.c_str());
        lua_setfield(L, -2, "id");
        lua_pushboolean(L, status.alive);
        lua_setfield(L, -2, "alive");
        lua_pushstring(L, status.last_error.c_str());
        lua_setfield(L, -2, "error");
        lua_pushinteger(L, static_cast<lua_Integer>(status.errors));
        lua_setfield(L, -2, "errors");
        lua_pushinteger(L, static_cast<lua_Integer>(status.hooks));
        lua_setfield(L, -2, "hooks");
        lua_pushinteger(L, static_cast<lua_Integer>(status.memory_kb));
        lua_setfield(L, -2, "memory_kb");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

void set_functions(lua_State* L, std::initializer_list<luaL_Reg> functions) {
    for (const auto& entry : functions) {
        lua_pushcfunction(L, entry.func);
        lua_setfield(L, -2, entry.name);
    }
}

}

void open_host(lua_State* L) {
    lua_newtable(L);
    set_functions(L, {{"upscale_info", fx_upscale_info}, {"pre_upscale_pass", fx_pre_pass}, {"revert", fx_revert},
                      {"pipelines", fx_pipelines}, {"pipeline", fx_pipeline}, {"pipeline_dump", fx_pipeline_dump},
                      {"material_hook", fx_material_hook}, {"pipeline_problem", fx_pipeline_problem},
                      {"pipeline_revert", fx_pipeline_revert}});
    lua_setglobal(L, "fx");
    lua_pushcfunction(L, lua_print);
    lua_setglobal(L, "print");

    lua_newtable(L);
    set_functions(L, {{"info", log_info}, {"warn", log_warn}, {"error", log_error}});
    lua_setglobal(L, "log");

    lua_pushcfunction(L, lua_now);
    lua_setglobal(L, "now");
    lua_pushcfunction(L, lua_panel);
    lua_setglobal(L, "panel");

    lua_newtable(L);
    set_functions(L, {{"raw_down", input_key_down}, {"overlay", input_overlay}});
    lua_setglobal(L, "input");

    lua_newtable(L);
    set_functions(L, {{"get", settings_get}, {"set", settings_set}});
    lua_setglobal(L, "__settings");

    lua_newtable(L);
    set_functions(L, {{"text", ui_text},
                      {"wrapped", ui_wrapped},
                      {"header", ui_header},
                      {"separator", ui_separator},
                      {"spacing", ui_spacing},
                      {"same_line", ui_same_line},
                      {"newline", ui_newline},
                      {"indent", ui_indent},
                      {"unindent", ui_unindent},
                      {"button", ui_button},
                      {"ghost_button", ui_ghost_button},
                      {"checkbox", ui_checkbox},
                      {"slider", ui_slider},
                      {"input", ui_input},
                      {"readout", ui_readout},
                      {"note", ui_note},
                      {"badge", ui_badge},
                      {"keycap", ui_keycap},
                      {"tooltip", ui_tooltip},
                      {"group", ui_group},
                      {"end_group", ui_end_group},
                      {"progress", ui_progress},
                      {"push_id", ui_push_id},
                      {"pop_id", ui_pop_id},
                      {"scroll", ui_scroll},
                      {"end_scroll", ui_end_scroll},
                      {"width", ui_width},
                      {"revert", ui_revert},
                      {"setting", ui_setting}});
    lua_setglobal(L, "ui");

    lua_getglobal(L, "script");
    set_functions(L, {{"reload", script_reload},
                      {"reload_mod", script_reload_other},
                      {"list", script_list},
                      {"budget", script_budget}});
    lua_pop(L, 1);
}

}
