#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "loader/registry.h"
#include "script/runtime.h"
#include "ui/font.h"
#include "ui/ui.h"

namespace bridger::script {
namespace {

std::string g_target = "console";
std::string g_input;
std::vector<std::string> g_history;
int g_history_index = -1;
std::vector<std::string> g_completions;
std::size_t g_seen_lines = 0;

ui::Color tone(int kind, std::string_view source) {
    const auto& t = ui::theme();
    if (source.starts_with("> ")) {
        return t.text_faint;
    }
    switch (kind) {
        case 1: return t.accent;
        case 2: return t.text_dim;
        case 3: return t.warn;
        case 4: return t.bad;
        default: return t.text;
    }
}

std::string common_prefix(const std::vector<std::string>& items) {
    if (items.empty()) {
        return {};
    }
    std::string prefix = items.front();
    for (const auto& item : items) {
        std::size_t i = 0;
        while (i < prefix.size() && i < item.size() && prefix[i] == item[i]) {
            ++i;
        }
        prefix.resize(i);
    }
    return prefix;
}

void draw_scripts(float height) {
    const auto& t = ui::theme();
    ui::begin_column(250.0f, height);
    ui::header("scripts");
    const auto list = statuses();
    ui::begin_scroll("script_list", ui::remaining_height() - 60.0f);
    for (const auto& status : list) {
        const ui::IdScope scope(status.id);
        const bool selected = status.id == g_target;
        if (ui::selectable(status.id, selected, 222.0f)) {
            g_target = status.id;
            ui::focus("script_input");
        }
        const ui::font::ScopedFace micro(ui::font::Face::Micro);
        ui::indent(10.0f);
        const auto summary = std::format("{}  {} hook{}  {} KiB", status.alive ? "running" : "stopped",
                                         status.hooks, status.hooks == 1 ? "" : "s", status.memory_kb);
        ui::text_colored(status.alive ? t.text_faint : t.warn, summary);
        if (status.errors > 0) {
            ui::text_colored(t.bad, std::format("{} error{}", status.errors, status.errors == 1 ? "" : "s"));
            if (!status.last_error.empty()) {
                auto first = status.last_error.substr(0, status.last_error.find('\n'));
                if (first.size() > 140) {
                    first = first.substr(0, 137) + "...";
                }
                ui::text_wrapped(t.text_dim, first);
            }
        }
        if (status.id != "console" && ui::ghost_button("reload", 70.0f)) {
            loader::Registry::instance().request_reload(status.id);
        }
        ui::unindent(10.0f);
        ui::spacing(4.0f);
    }
    for (const auto& mod : loader::Registry::instance().mods()) {
        if (!mod.script || mod.state != loader::State::Failed) {
            continue;
        }
        const ui::IdScope scope(mod.id);
        ui::text_colored(t.bad, mod.id);
        const ui::font::ScopedFace micro(ui::font::Face::Micro);
        ui::indent(10.0f);
        ui::text_wrapped(t.text_dim, mod.error);
        ui::unindent(10.0f);
        ui::spacing(4.0f);
    }
    ui::end_scroll();
    {
        const ui::font::ScopedFace micro(ui::font::Face::Micro);
        ui::text_wrapped(t.text_faint, "Save a .lua file in Bridger/scripts or a mod folder and it "
                                       "reloads here. tools/bridger.py opens this console in a terminal.");
    }
    ui::end_column();
}

void submit() {
    if (g_input.find_first_not_of(" \t") == std::string::npos) {
        return;
    }
    if (g_history.empty() || g_history.back() != g_input) {
        g_history.push_back(g_input);
    }
    g_history_index = -1;
    g_completions.clear();
    script::submit(g_target, g_input);
    g_input.clear();
}

void recall(int direction) {
    if (g_history.empty()) {
        return;
    }
    if (g_history_index < 0) {
        g_history_index = direction < 0 ? static_cast<int>(g_history.size()) - 1 : -1;
    } else {
        g_history_index += direction;
    }
    if (g_history_index < 0 || g_history_index >= static_cast<int>(g_history.size())) {
        g_history_index = -1;
        g_input.clear();
    } else {
        g_input = g_history[static_cast<std::size_t>(g_history_index)];
    }
    ui::move_caret_to_end(g_input);
}

void draw_console(float height) {
    const auto& t = ui::theme();
    ui::begin_column(ui::available_width() - 4.0f, height);
    {
        const ui::font::ScopedFace title(ui::font::Face::Title);
        ui::text_colored(t.text, g_target == "console" ? "console" : "console  in  " + g_target);
    }
    ui::same_line();
    ui::align_right(80.0f);
    if (ui::ghost_button("clear", 70.0f)) {
        console_clear();
    }
    ui::newline();

    const auto lines = console_lines(1500);
    const float completion_height = g_completions.empty() ? 0.0f : 64.0f;
    const float log_height = ui::remaining_height() - 44.0f - completion_height;
    if (lines.size() != g_seen_lines) {
        g_seen_lines = lines.size();
        ui::scroll_to_end("script_console");
    }
    ui::begin_scroll("script_console", log_height);
    {
        const ui::font::ScopedFace micro(ui::font::Face::Micro);
        for (const auto& line : lines) {
            const bool echo = line.source.starts_with("> ");
            const auto text = echo ? std::format("{} {}", line.source, line.text)
                            : line.source == "console" ? line.text
                                                       : std::format("[{}] {}", line.source, line.text);
            ui::text_colored(tone(line.kind, line.source), text);
        }
        if (lines.empty()) {
            ui::text_colored(t.text_faint, "Type Lua and press Enter. help() lists the essentials; "
                                           "Tab completes; Up and Down walk the history.");
        }
    }
    ui::end_scroll();

    if (!g_completions.empty()) {
        const ui::font::ScopedFace micro(ui::font::Face::Micro);
        std::string strip;
        for (std::size_t i = 0; i < g_completions.size() && i < 60; ++i) {
            const auto& item = g_completions[i];
            const auto split = item.find_last_of(".:");
            strip += (i == 0 ? "" : "   ") + (split == std::string::npos ? item : item.substr(split + 1));
        }
        ui::text_wrapped(t.text_dim, strip);
    }

    const auto result = ui::input_line("script_input", g_input, ui::available_width() - 4.0f,
                                       "Lua, e.g. game.player_entity()");
    if (result.changed) {
        g_completions.clear();
    }
    if (result.submitted) {
        submit();
    } else if (result.previous) {
        recall(-1);
    } else if (result.next) {
        recall(1);
    } else if (result.complete) {
        auto items = complete(g_target, g_input);
        if (items.size() == 1) {
            g_input = items.front();
            g_completions.clear();
        } else if (!items.empty()) {
            const auto prefix = common_prefix(items);
            if (prefix.size() > g_input.size()) {
                g_input = prefix;
            }
            g_completions = std::move(items);
        }
        ui::move_caret_to_end(g_input);
    }
    ui::end_column();
}

}

void draw_panel() {
    const float height = ui::remaining_height() - 4.0f;
    draw_scripts(height);
    draw_console(height);
    ui::newline();
}

}
