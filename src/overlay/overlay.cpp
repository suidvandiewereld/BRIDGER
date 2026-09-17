#include "overlay/overlay.h"

#include <Windows.h>
#include <windowsx.h>

#include <MinHook.h>
#include "overlay/d3d.h"
#include "overlay/inspect.h"
#include "debugger/debugger.h"
#include "debugger/picker.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <format>
#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/guard.h"
#include "core/memory.h"
#include "core/settings.h"
#include "decima/dumper.h"
#include "content/content.h"
#include "fx/fx.h"
#include "loader/registry.h"
#include "overlay/renderer.h"
#include "script/runtime.h"
#include "ui/font.h"
#include "ui/input.h"
#include "ui/ui.h"

namespace bridger::overlay {
namespace {


using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

using ShowCursorFn = int(WINAPI*)(BOOL);
using SetCursorFn = HCURSOR(WINAPI*)(HCURSOR);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);

constexpr int kPresentIndex = 8;
constexpr int kResizeIndex = 13;
constexpr int kExecuteIndex = 10;
constexpr std::uint32_t kToggleKey = VK_F1;
constexpr std::uint32_t kWindowKey = VK_F2;

PresentFn g_present = nullptr;
ResizeFn g_resize = nullptr;
ExecuteFn g_execute = nullptr;

ShowCursorFn g_show_cursor = nullptr;
SetCursorFn g_set_cursor = nullptr;
ClipCursorFn g_clip_cursor = nullptr;
SetCursorPosFn g_set_cursor_pos = nullptr;

RECT g_wanted_clip{};
bool g_wants_clip = false;
int g_cursor_pushes = 0;
int g_swallowed_hides = 0;
HCURSOR g_arrow = nullptr;
HCURSOR g_previous_cursor = nullptr;
bool g_had_previous = false;

ID3D12CommandQueue* g_queue = nullptr;
HWND g_window = nullptr;
WNDPROC g_original_proc = nullptr;
Renderer g_renderer;
bool g_visible = false;
std::atomic<bool> g_picking{false};
std::atomic<float> g_pick_x{-1.0f};
std::atomic<float> g_pick_y{-1.0f};
std::atomic<bool> g_pick_click{false};
std::atomic<int> g_client_width{0};
constexpr UINT kPickBegin = WM_APP + 0x51;
constexpr UINT kPickEnd = WM_APP + 0x52;

bool mouse_free() {
    return g_visible || g_picking.load();
}
bool g_started = false;
float g_open = 0.0f;
std::int64_t g_last_tick = 0;

ui::Rect g_window_rect{120.0f, 120.0f, 1180.0f, 800.0f};
bool g_window_open = true;
int g_tab = 0;
int g_panel = 0;
std::string g_panel_filter;
std::string g_type_filter;
std::string g_symbol_filter;
std::string g_selected_type;
std::string g_selected_symbol;
decima::TypeDetail g_type_detail;
mem::Module g_game;
std::vector<std::string> g_log_lines;

ui::font::Image g_logo;

void load_logo(const std::filesystem::path& root) {
    std::ifstream stream(root / "assets" / "logo.bin", std::ios::binary);
    if (!stream.is_open()) {
        log::warn("overlay: assets/logo.bin not found");
        return;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    stream.read(reinterpret_cast<char*>(&width), sizeof(width));
    stream.read(reinterpret_cast<char*>(&height), sizeof(height));
    if (!stream || width == 0 || height == 0 || width > 2048 || height > 2048) {
        log::warn("overlay: assets/logo.bin is malformed");
        return;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height);
    stream.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    if (!stream) {
        log::warn("overlay: assets/logo.bin is truncated");
        return;
    }
    g_logo = ui::font::place_image(pixels.data(), static_cast<int>(width), static_cast<int>(height));
    if (g_logo.valid) {
        log::info("overlay: logo {}x{} placed in the atlas", width, height);
    }
}

const char* state_label(loader::State state) {
    switch (state) {
        case loader::State::Loaded: return "LOADED";
        case loader::State::Disabled: return "DISABLED";
        case loader::State::Failed: return "FAILED";
        default: return "IDLE";
    }
}

ui::Color state_tone(loader::State state) {
    const auto& t = ui::theme();
    switch (state) {
        case loader::State::Loaded: return t.good;
        case loader::State::Failed: return t.bad;
        case loader::State::Disabled: return t.text_faint;
        default: return t.warn;
    }
}

void draw_logo_mark(const ui::Rect& area) {
    if (!g_logo.valid) {
        return;
    }
    const auto& t = ui::theme();
    const float aspect = static_cast<float>(g_logo.width) / static_cast<float>(g_logo.height);

    const float reference_aspect = 320.0f / 360.0f;
    const float watermark_height =
        std::min(360.0f, area.height() * 0.62f) * std::sqrt(reference_aspect / aspect);
    const float watermark_width = watermark_height * aspect;
    const float top = area.y0 + t.chrome_height + 26.0f;
    const ui::Rect watermark{area.x1 - watermark_width - 26.0f,
                             top,
                             area.x1 - 26.0f,
                             top + watermark_height};
    ui::draw().image(watermark, g_logo.u0, g_logo.v0, g_logo.u1, g_logo.v1,
                     ui::with_alpha(t.accent, 0.05f));
}

void draw_mods() {
    auto& registry = loader::Registry::instance();
    const auto& mods = registry.mods();
    const auto& t = ui::theme();

    ui::header("installed");

    if (mods.empty()) {
        ui::text_colored(t.text_dim, "No mods found.");
        ui::text_colored(t.text_faint, "Drop a folder with a manifest.json into Bridger/mods.");
        return;
    }

    {
        const bool watching = registry.auto_reload();
        if (ui::checkbox("Auto reload on rebuild", watching)) {
            registry.set_auto_reload(!watching);
        }
        ui::text_colored(t.text_faint,
                         "Mods load from a staged copy, so a build can overwrite the DLL "
                         "while the game runs.");
    }
    ui::begin_scroll("mods", ui::remaining_height() - 34.0f);
    for (const auto& mod : mods) {
        const ui::IdScope scope(mod.id);
        const bool enabled = registry.enabled(mod.id);
        if (ui::checkbox(mod.name.empty() ? mod.id : mod.name, enabled)) {
            registry.set_enabled(mod.id, !enabled);
        }
        ui::same_line(0.0f);
        ui::align_right(90.0f);
        ui::tracked(state_tone(mod.state), state_label(mod.state), 1.4f);
        if (mod.state == loader::State::Loaded || mod.state == loader::State::Failed) {
            ui::indent(26.0f);
            if (ui::button("Reload")) {
                registry.request_reload(mod.id);
            }
            ui::unindent(26.0f);
        }

        ui::indent(26.0f);
        {
            const ui::font::ScopedFace micro_face(ui::font::Face::Micro);
            ui::begin_row(16.0f);
            ui::row_cell(150.0f, t.text_faint, mod.id);
            ui::row_cell(80.0f, t.text_faint, mod.version);
            ui::row_cell(180.0f, t.text_faint, mod.author);
            ui::end_row();
        }
        if (!mod.description.empty()) {
            ui::text_colored(t.text_dim, mod.description);
        }
        if (!mod.error.empty()) {
            ui::text_colored(t.bad, mod.error);
        }
        ui::unindent(26.0f);
        ui::rule();
    }
    ui::end_scroll();
    ui::text_colored(t.text_faint, "Changes take effect on the next launch.");
}

void draw_log() {
    bridger::log::snapshot(g_log_lines);
    const auto& t = ui::theme();
    ui::header("session log");
    const ui::font::ScopedFace micro_face(ui::font::Face::Micro);
    ui::begin_scroll("log", ui::remaining_height());
    for (const auto& line : g_log_lines) {
        ui::Color color = t.text_dim;
        if (line.find("[error]") != std::string::npos) {
            color = t.accent;
        } else if (line.find("[warn ]") != std::string::npos) {
            color = t.warn;
        } else if (line.find("[trace]") != std::string::npos) {
            color = t.text_faint;
        }
        ui::text_colored(color, line);
    }
    ui::end_scroll();
}

bool matches(const std::string& value, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }
    const auto found = std::search(value.begin(), value.end(), filter.begin(), filter.end(),
                                   [](char a, char b) {
                                       return std::tolower(static_cast<unsigned char>(a))
                                            == std::tolower(static_cast<unsigned char>(b));
                                   });
    return found != value.end();
}

bool draw_scan_gate(std::string_view what) {
    const auto& t = ui::theme();
    if (decima::scanning()) {
        ui::header("scanning");
        ui::text_colored(t.text_dim, decima::scan_stage());
        ui::text_colored(t.text_faint, "Reading process memory. This takes a few seconds.");
        return false;
    }
    ui::header("not indexed");
    ui::text_colored(t.text_dim, std::format("No {} indexed this session.", what));
    ui::text_colored(t.text_faint, "Scanning walks committed memory for RTTI descriptors");
    ui::text_colored(t.text_faint, "and exported symbol groups. It does not modify the game.");
    ui::spacing(10.0f);
    if (ui::button("Scan now", 150.0f)) {
        decima::start_scan(g_game);
    }
    return false;
}

void draw_type_detail() {
    const auto& t = ui::theme();
    const auto& detail = g_type_detail;
    if (detail.name.empty()) {
        ui::text_colored(t.text_faint, "Select a type.");
        return;
    }

    ui::tracked(t.accent, detail.name, 1.2f);
    {
        const ui::font::ScopedFace micro_face(ui::font::Face::Micro);
        ui::label_value("kind", detail.kind);
        ui::label_value("address", std::format("{:#x}", detail.address));
        if (detail.address >= g_game.base) {
            ui::label_value("rva", std::format("{:#x}", detail.address - g_game.base));
        }
        ui::label_value("runtime id", std::format("{}", detail.id));
        if (detail.kind == "class") {
            ui::label_value("size", std::format("{} bytes", detail.size));
            ui::label_value("alignment", std::format("{}", detail.alignment));
            ui::label_value("handlers", std::format("{}", detail.message_handlers));
            if (detail.constructor != 0) {
                ui::label_value("constructor", std::format("{:#x}", detail.constructor));
            }
            if (detail.destructor != 0) {
                ui::label_value("destructor", std::format("{:#x}", detail.destructor));
            }
        } else {
            ui::label_value("width", std::format("{} bytes", detail.size));
        }
    }

    if (!detail.bases.empty()) {
        ui::header("bases");
        const ui::font::ScopedFace micro_face(ui::font::Face::Micro);
        for (const auto& base : detail.bases) {
            ui::begin_row(15.0f);
            ui::row_cell(70.0f, t.text_faint, std::format("+{}", base.offset));
            ui::row_cell(320.0f, t.text, base.name);
            ui::end_row();
        }
        ui::spacing(6.0f);
    }

    if (!detail.members.empty()) {
        ui::header(std::format("members ({})", detail.members.size()));
        const ui::font::ScopedFace micro_face(ui::font::Face::Micro);
        ui::begin_scroll("type_members", ui::remaining_height());
        std::string category;
        for (const auto& member : detail.members) {
            if (member.category != category) {
                category = member.category;
                if (!category.empty()) {
                    ui::spacing(4.0f);
                    ui::text_colored(t.text_faint, category);
                }
            }
            ui::begin_row(15.0f);
            ui::row_cell(64.0f, t.text_faint, std::format("+{}", member.offset));
            ui::row_cell(150.0f, t.text, member.name);
            ui::row_cell(220.0f, t.text_dim, member.type);
            ui::end_row();
        }
        ui::end_scroll();
    } else if (!detail.values.empty()) {
        ui::header(std::format("values ({})", detail.values.size()));
        const ui::font::ScopedFace micro_face(ui::font::Face::Micro);
        ui::begin_scroll("type_values", ui::remaining_height());
        for (const auto& value : detail.values) {
            ui::begin_row(15.0f);
            ui::row_cell(70.0f, t.text_faint, std::format("{}", value.value));
            ui::row_cell(320.0f, t.text, value.name);
            ui::end_row();
        }
        ui::end_scroll();
    } else if (detail.kind == "class") {
        ui::text_colored(t.text_faint, "No reflected members.");
    }
}

void draw_types() {
    const auto index = decima::type_index();
    if (index->empty()) {
        draw_scan_gate("types");
        return;
    }

    const auto& t = ui::theme();
    const float height = ui::remaining_height() - 4.0f;
    const float list_width = 330.0f;

    ui::begin_column(list_width, height);
    ui::input_text("types", g_type_filter, list_width, "filter types");
    std::size_t shown = 0;
    ui::begin_scroll("type_list", ui::remaining_height() - 22.0f);
    {
        const ui::font::ScopedFace micro_face(ui::font::Face::Micro);
        for (const auto& [name, address] : *index) {
            if (!matches(name, g_type_filter)) {
                continue;
            }
            if (++shown > 400) {
                break;
            }
            if (ui::selectable(name, name == g_selected_type, list_width - 22.0f)) {
                g_selected_type = name;
                decima::describe_type(address, g_type_detail);
            }
        }
    }
    ui::end_scroll();
    {
        const ui::font::ScopedFace micro_face(ui::font::Face::Micro);
        ui::text_colored(t.text_faint, std::format("{} of {}", shown, index->size()));
    }
    ui::end_column();

    ui::begin_column(ui::available_width() - 4.0f, height);
    draw_type_detail();
    ui::end_column();

    ui::newline();
}

void draw_symbol_detail() {
    const auto& t = ui::theme();
    const auto details = decima::symbol_details();
    const auto found = details->find(g_selected_symbol);
    if (found == details->end()) {
        ui::text_colored(t.text_faint, "Select a symbol.");
        return;
    }

    const auto& detail = found->second;
    ui::tracked(t.accent, detail.name, 1.2f);
    const ui::font::ScopedFace micro_face(ui::font::Face::Micro);
    ui::label_value("group", detail.group);
    ui::label_value("kind", detail.kind);
    if (detail.address != 0) {
        ui::label_value("address", std::format("{:#x}", detail.address));
    }
    if (detail.rva != 0) {
        ui::label_value("rva", std::format("{:#x}", detail.rva));
    }
    if (!detail.header.empty()) {
        ui::label_value("header", detail.header);
    }

    if (!detail.signature.empty()) {
        ui::header("signature");
        ui::text_colored(t.text, detail.signature);
    }
}

void draw_symbols() {
    const auto details = decima::symbol_details();
    if (details->empty()) {
        draw_scan_gate("symbols");
        return;
    }

    const auto& t = ui::theme();
    const float height = ui::remaining_height() - 4.0f;
    const float list_width = 400.0f;

    ui::begin_column(list_width, height);
    ui::input_text("symbols", g_symbol_filter, list_width, "filter symbols");
    std::size_t shown = 0;
    ui::begin_scroll("symbol_list", ui::remaining_height() - 22.0f);
    {
        const ui::font::ScopedFace micro_face(ui::font::Face::Micro);
        for (const auto& [key, detail] : *details) {
            if (!matches(key, g_symbol_filter)) {
                continue;
            }
            if (++shown > 400) {
                break;
            }
            if (ui::selectable(key, key == g_selected_symbol, list_width - 22.0f)) {
                g_selected_symbol = key;
            }
        }
    }
    ui::end_scroll();
    {
        const ui::font::ScopedFace micro_face(ui::font::Face::Micro);
        ui::text_colored(t.text_faint, std::format("{} of {}", shown, details->size()));
    }
    ui::end_column();

    ui::begin_column(ui::available_width() - 4.0f, height);
    draw_symbol_detail();
    ui::end_column();

    ui::newline();
}

const loader::Mod* find_mod(const std::string& id) {
    for (const auto& mod : loader::Registry::instance().mods()) {
        if (mod.id == id) {
            return &mod;
        }
    }
    return nullptr;
}

void draw_panel_rail(const std::vector<loader::Panel>& panels, float height) {
    const auto& t = ui::theme();
    const ui::Rect body = ui::window_body();
    ui::draw().rect({body.x0, body.y0, body.x0 + t.rail_width, body.y1}, t.window_low);
    ui::draw().rect({body.x0 + t.rail_width, body.y0, body.x0 + t.rail_width + 1.0f, body.y1},
                    t.hairline);

    const float width = t.rail_width - t.padding * 2.0f;
    ui::begin_column(width, height);
    ui::begin_scroll("panel_rail", height);
    std::string owner;
    for (std::size_t i = 0; i < panels.size(); ++i) {
        const auto& panel = panels[i];
        if (panel.owner != owner) {
            owner = panel.owner;
            const loader::Mod* mod = find_mod(owner);
            if (i > 0) {
                ui::spacing(14.0f);
            }
            const ui::font::ScopedFace caption(ui::font::Face::Caption);
            ui::tracked(t.text_faint, mod != nullptr && !mod->name.empty() ? mod->name : owner,
                        t.tracking);
        }
        const bool selected = static_cast<int>(i) == g_panel;
        if (ui::selectable(panel.label, selected, width - 14.0f)) {
            g_panel = static_cast<int>(i);
        }
    }
    ui::end_scroll();
    ui::end_column();
    ui::set_cursor_x(t.rail_width);
}

void draw_panel_heading(const loader::Panel& panel) {
    const auto& t = ui::theme();
    const loader::Mod* mod = find_mod(panel.owner);
    const float search_width = 260.0f;
    const ui::Vec2 top = ui::cursor();

    {
        const ui::font::ScopedFace title(ui::font::Face::Title);
        ui::text_colored(t.text, panel.label);
    }
    if (mod != nullptr) {
        const ui::font::ScopedFace micro(ui::font::Face::Micro);
        const auto saved = settings::count(mod->id);
        auto line = std::format("{}   v{}", mod->id, mod->version);
        if (!mod->author.empty()) {
            line += std::format("   {}", mod->author);
        }
        line += saved == 0 ? "   defaults"
                           : std::format("   {} saved", saved);
        ui::text_colored(t.text_faint, line);
    }
    const ui::Vec2 below = ui::cursor();

    const float field_y = top.y + ((below.y - top.y) - t.row_height - 8.0f - t.spacing) * 0.5f;
    ui::set_cursor_y(field_y);
    ui::align_right(search_width);
    ui::input_text("panel_filter", g_panel_filter, search_width, "search settings");
    if (!g_panel_filter.empty()) {
        ui::same_line(0.0f);
        ui::align_right(search_width + 60.0f);
        if (ui::ghost_button("clear", 56.0f)) {
            g_panel_filter.clear();
        }
    }
    ui::set_cursor_y(below.y + 10.0f);
}

void draw_panels() {
    loader::DispatchGuard dispatch;
    const auto& panels = loader::Registry::instance().panels();
    const auto& t = ui::theme();

    if (panels.empty()) {
        ui::header("mod panels");
        ui::text_colored(t.text_dim, "No mod panels registered.");
        ui::text_colored(t.text_faint, "Mods add panels with bridger::ui::panel().");
        return;
    }

    g_panel = std::clamp(g_panel, 0, static_cast<int>(panels.size()) - 1);

    const float height = ui::remaining_height();
    draw_panel_rail(panels, height);

    ui::begin_column(ui::available_width(), height);
    const auto& panel = panels[static_cast<std::size_t>(g_panel)];
    draw_panel_heading(panel);

    ui::begin_scroll("panel_page", ui::remaining_height());
    ui::set_filter(g_panel_filter);
    ui::begin_settings();
    if (panel.draw != nullptr) {
        panel.draw(panel.user);
    }
    ui::end_settings();
    ui::set_filter({});
    ui::end_scroll();
    ui::end_column();

    ui::newline();
}

void draw_interface() {
    const std::size_t loaded = std::count_if(
        loader::Registry::instance().mods().begin(), loader::Registry::instance().mods().end(),
        [](const loader::Mod& mod) { return mod.state == loader::State::Loaded; });
    ui::window_status(std::format("{}     {} MODS LOADED     {} TYPES     {} SYMBOLS",
                                  "F1 CLOSE     F2 DEBUGGER",
                                  loaded, decima::type_index()->size(),
                                  decima::symbol_index()->size()));
    if (!ui::begin_window("bridger", g_window_rect, g_window_open)) {
        return;
    }

    draw_logo_mark(g_window_rect);

    if (inspect::pending()) {
        g_tab = 4;
    }
    const char* labels[] = {"Mods",    "Log",    "Types",   "Symbols", "Inspect",
                            "Panels",  "Shaders", "Script"};
    for (int i = 0; i < 8; ++i) {
        if (ui::window_tab(labels[i], g_tab == i) && g_tab != i) {
            g_tab = i;
            if (i == 1) {
                ui::scroll_to_end("log");
            }
        }
    }

    switch (g_tab) {
        case 0: draw_mods(); break;
        case 1: draw_log(); break;
        case 2: draw_types(); break;
        case 3: draw_symbols(); break;
        case 4: inspect::draw(); break;
        case 5: draw_panels(); break;
        case 6: fx::draw_panel(); break;
        case 7: script::draw_panel(); break;
        default: draw_mods(); break;
    }

    ui::end_window();

    if (!g_window_open) {
        g_visible = false;
        g_window_open = true;
    }
}

int WINAPI show_cursor_detour(BOOL show) {
    if (mouse_free()) {
        if (show != FALSE) {
            if (g_swallowed_hides > 0) {
                --g_swallowed_hides;
            }
            return 0;
        }
        ++g_swallowed_hides;
        return -1;
    }
    return g_show_cursor(show);
}

HCURSOR WINAPI set_cursor_detour(HCURSOR cursor) {
    if (g_picking) {
        return g_set_cursor(LoadCursorW(nullptr, IDC_CROSS));
    }
    if (g_visible) {
        return g_set_cursor(g_arrow != nullptr ? g_arrow : cursor);
    }
    return g_set_cursor(cursor);
}

BOOL WINAPI clip_cursor_detour(const RECT* bounds) {
    if (mouse_free()) {
        g_wants_clip = bounds != nullptr;
        if (bounds != nullptr) {
            g_wanted_clip = *bounds;
        }
        return g_clip_cursor(nullptr);
    }
    return g_clip_cursor(bounds);
}

BOOL WINAPI set_cursor_pos_detour(int x, int y) {
    if (mouse_free()) {
        return TRUE;
    }
    return g_set_cursor_pos(x, y);
}

void acquire_mouse(bool recentre = true) {
    if (g_arrow == nullptr) {
        g_arrow = LoadCursorW(nullptr, IDC_ARROW);
    }

    g_previous_cursor = GetCursor();
    g_had_previous = true;

    RECT current{};
    RECT desktop{};
    g_wants_clip = false;
    if (g_clip_cursor != nullptr && GetClipCursor(&current) != 0) {
        const bool full = GetWindowRect(GetDesktopWindow(), &desktop) != 0
                       && current.left <= desktop.left && current.top <= desktop.top
                       && current.right >= desktop.right && current.bottom >= desktop.bottom;
        if (!full) {
            g_wanted_clip = current;
            g_wants_clip = true;
        }
        g_clip_cursor(nullptr);
    }

    ReleaseCapture();

    g_swallowed_hides = 0;
    if (g_show_cursor != nullptr) {
        g_cursor_pushes = 0;
        while (g_cursor_pushes < 32 && g_show_cursor(TRUE) < 0) {
            ++g_cursor_pushes;
        }
    }

    if (recentre && g_set_cursor_pos != nullptr && g_window != nullptr) {
        POINT centre{static_cast<LONG>((g_window_rect.x0 + g_window_rect.x1) * 0.5f),
                     static_cast<LONG>(g_window_rect.y0 + 120.0f)};
        ClientToScreen(g_window, &centre);
        g_set_cursor_pos(centre.x, centre.y);
        ui::input::on_mouse_move((g_window_rect.x0 + g_window_rect.x1) * 0.5f,
                                 g_window_rect.y0 + 120.0f);
    }

    if (g_set_cursor != nullptr) {
        g_set_cursor(g_arrow);
    }
}

void release_mouse() {
    if (g_show_cursor != nullptr) {
        for (int i = 0; i < g_cursor_pushes; ++i) {
            g_show_cursor(FALSE);
        }
        for (int i = 0; i < g_swallowed_hides; ++i) {
            g_show_cursor(FALSE);
        }
    }
    g_cursor_pushes = 0;
    g_swallowed_hides = 0;

    if (g_clip_cursor != nullptr) {
        if (g_wants_clip) {
            g_clip_cursor(&g_wanted_clip);
        } else {
            g_clip_cursor(nullptr);
        }
    }

    if (g_had_previous && g_set_cursor != nullptr) {
        g_set_cursor(g_previous_cursor);
    }
    g_had_previous = false;

    if (g_window != nullptr) {
        const auto thread = static_cast<LPARAM>(GetCurrentThreadId());

        PostMessageW(g_window, WM_ACTIVATEAPP, FALSE, thread);
        PostMessageW(g_window, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);
        PostMessageW(g_window, WM_KILLFOCUS, 0, 0);

        PostMessageW(g_window, WM_SETFOCUS, 0, 0);
        PostMessageW(g_window, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
        PostMessageW(g_window, WM_ACTIVATEAPP, TRUE, thread);
        PostMessageW(g_window, WM_SETCURSOR, reinterpret_cast<WPARAM>(g_window),
                     MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_KEYDOWN && wparam == kToggleKey && !ui::capturing_key()) {
        toggle();
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == kWindowKey && !ui::capturing_key()) {
        debugger::toggle();
        return 0;
    }

    if (message == kPickBegin) {
        if (!g_picking.exchange(true)) {
            debugger::picker::set_collecting(true);
            if (!g_visible) {
                acquire_mouse(false);
            }
            SetForegroundWindow(window);
            log::info("picker: click an entity in the game window, Esc to cancel");
        }
        return 0;
    }
    if (message == kPickEnd) {
        if (g_picking.exchange(false)) {
            debugger::picker::set_collecting(false);
            g_pick_click = false;
            if (!g_visible) {
                release_mouse();
            }
        }
        return 0;
    }

    if (g_picking) {
        switch (message) {
            case WM_MOUSEMOVE: {
                RECT client{};
                GetClientRect(window, &client);
                g_client_width = client.right - client.left;
                g_pick_x = static_cast<float>(GET_X_LPARAM(lparam));
                g_pick_y = static_cast<float>(GET_Y_LPARAM(lparam));
                return 0;
            }
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
                g_pick_click = true;
                return 0;
            case WM_RBUTTONDOWN:
                PostMessageW(window, kPickEnd, 0, 0);
                return 0;
            case WM_KEYDOWN:
                if (wparam == VK_ESCAPE) {
                    PostMessageW(window, kPickEnd, 0, 0);
                }
                return 0;
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_INPUT:
            case WM_KEYUP:
            case WM_CHAR:
                return 0;
            case WM_SETCURSOR:
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                return TRUE;
            default:
                break;
        }
    }

    if (g_visible) {
        switch (message) {
            case WM_MOUSEMOVE:
                ui::input::on_mouse_move(static_cast<float>(GET_X_LPARAM(lparam)),
                                         static_cast<float>(GET_Y_LPARAM(lparam)));
                return 0;
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
                ui::input::on_mouse_button(true);
                return 0;
            case WM_LBUTTONUP:
                ui::input::on_mouse_button(false);
                return 0;
            case WM_MOUSEWHEEL:
                ui::input::on_wheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA);
                return 0;
            case WM_CHAR:
                ui::input::on_character(static_cast<char>(wparam));
                return 0;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                ui::input::on_key(static_cast<std::uint32_t>(wparam), true);
                return 0;
            case WM_KEYUP:
            case WM_SYSKEYUP:
                ui::input::on_key(static_cast<std::uint32_t>(wparam), false);
                return 0;
            case WM_INPUT:
                return 0;
            case WM_RBUTTONDOWN:
                ui::input::on_right_button(true);
                return 0;
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            case WM_MOUSEHWHEEL:
                return 0;
            case WM_SETCURSOR:
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            default:
                break;
        }
    }

    return CallWindowProcW(g_original_proc, window, message, wparam, lparam);
}

void STDMETHODCALLTYPE execute_detour(ID3D12CommandQueue* queue, UINT count,
                                      ID3D12CommandList* const* lists) {
    if (g_queue == nullptr && queue != nullptr) {
        const auto desc = queue->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_queue = queue;
        }
    }
    g_execute(queue, count, lists);
    fx::submitted(queue, count, lists);
}

HRESULT STDMETHODCALLTYPE resize_detour(IDXGISwapChain3* swapchain, UINT count, UINT width,
                                        UINT height, DXGI_FORMAT format, UINT flags) {
    g_renderer.release_targets();
    return g_resize(swapchain, count, width, height, format, flags);
}

void draw_pick_hud(float width, float height) {
    const auto& t = ui::theme();
    auto& list = ui::draw();

    const int client = g_client_width.load();
    const float scale = client > 0 ? width / static_cast<float>(client) : 1.0f;
    const float mx = g_pick_x.load() * scale;
    const float my = g_pick_y.load() * scale;

    int hovered = -1;
    const auto candidates = debugger::picker::update(mx, my, hovered);

    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (i != hovered) {
            list.circle({candidates[i].x, candidates[i].y}, 2.5f, ui::with_alpha(t.accent, 0.55f));
        }
    }

    if (mx >= 0.0f && my >= 0.0f) {
        const ui::Color cross = ui::with_alpha(t.text, 0.8f);
        list.rect({mx - 12.0f, my - 0.5f, mx - 4.0f, my + 0.5f}, cross);
        list.rect({mx + 4.0f, my - 0.5f, mx + 12.0f, my + 0.5f}, cross);
        list.rect({mx - 0.5f, my - 12.0f, mx + 0.5f, my - 4.0f}, cross);
        list.rect({mx - 0.5f, my + 4.0f, mx + 0.5f, my + 12.0f}, cross);
    }

    if (hovered >= 0) {
        const auto& pick = candidates[static_cast<std::size_t>(hovered)];
        const float r = std::max(pick.radius, 18.0f);
        list.arc({pick.x, pick.y}, r, 0.0f, 6.2831853f, t.accent, 2.0f);
        list.circle({pick.x, pick.y}, 3.0f, t.accent_bright);

        const auto title = pick.type;
        const auto detail = std::format("{:.1f} m   {:#x}", pick.distance, pick.entity);
        float label_width = 0.0f;
        {
            const ui::font::ScopedFace strong(ui::font::Face::Strong);
            label_width = ui::font::measure(title);
        }
        {
            const ui::font::ScopedFace micro(ui::font::Face::Micro);
            label_width = std::max(label_width, ui::font::measure(detail));
        }
        float lx = std::round(pick.x + r + 12.0f);
        float ly = std::round(pick.y - 22.0f);
        if (lx + label_width + 24.0f > width) {
            lx = std::round(pick.x - r - 12.0f - label_width - 24.0f);
        }
        ly = std::clamp(ly, 8.0f, height - 52.0f);
        const ui::Rect box{lx, ly, lx + label_width + 24.0f, ly + 44.0f};
        list.rect(box, ui::with_alpha(t.window, 0.94f));
        list.rect({box.x0, box.y0, box.x0 + 2.0f, box.y1}, t.accent);
        {
            const ui::font::ScopedFace strong(ui::font::Face::Strong);
            list.text({box.x0 + 12.0f, box.y0 + 4.0f}, t.text, title);
        }
        {
            const ui::font::ScopedFace micro(ui::font::Face::Micro);
            list.text({box.x0 + 12.0f, box.y0 + 24.0f}, t.text_faint, detail);
        }
    }

    {
        const auto hint = std::format("Click an entity to open it in the debugger      "
                                      "Esc or right-click to cancel      {} on screen",
                                      candidates.size());
        const ui::font::ScopedFace body(ui::font::Face::Body);
        const float text_width = ui::font::measure(hint);
        const float caption_width = 110.0f;
        const float box_width = std::round(text_width + caption_width + 40.0f);
        const float x0 = std::round((width - box_width) * 0.5f);
        const ui::Rect bar{x0, 24.0f, x0 + box_width, 64.0f};
        list.rect(bar, ui::with_alpha(t.window, 0.94f));
        list.rect_outline(bar, t.border, 1.0f);
        {
            const ui::font::ScopedFace caption(ui::font::Face::Caption);
            list.text_tracked({bar.x0 + 18.0f, std::round(bar.y0 + (bar.height() - ui::font::line_height()) * 0.5f)},
                              t.accent, "PICKING", 1.6f);
        }
        list.text({bar.x0 + 18.0f + caption_width, std::round(bar.y0 + (bar.height() - ui::font::line_height()) * 0.5f)},
                  t.text_dim, hint);
    }

    if (g_pick_click.exchange(false) && hovered >= 0) {
        const auto entity = candidates[static_cast<std::size_t>(hovered)].entity;
        log::info("picker: opened {} at {:#x}", candidates[static_cast<std::size_t>(hovered)].type,
                  entity);
        debugger::open(entity);
        PostMessageW(g_window, kPickEnd, 0, 0);
    }
}

HRESULT STDMETHODCALLTYPE present_detour(IDXGISwapChain3* swapchain, UINT interval, UINT flags) {
    if (g_queue != nullptr) {
        if (!g_renderer.ready()) {
            DXGI_SWAP_CHAIN_DESC desc{};
            swapchain->GetDesc(&desc);
            if (g_window == nullptr && desc.OutputWindow != nullptr) {
                g_window = desc.OutputWindow;
                g_original_proc = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrW(g_window, GWLP_WNDPROC,
                                      reinterpret_cast<LONG_PTR>(window_proc)));
            }
            g_renderer.initialise(swapchain, g_queue);
        }

        if (g_renderer.ready()) {
            LARGE_INTEGER frequency{};
            LARGE_INTEGER now{};
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&now);
            float dt = 1.0f / 60.0f;
            if (g_last_tick != 0 && frequency.QuadPart != 0) {
                dt = static_cast<float>(static_cast<double>(now.QuadPart - g_last_tick)
                                        / static_cast<double>(frequency.QuadPart));
            }
            g_last_tick = now.QuadPart;
            dt = std::clamp(dt, 0.0f, 0.1f);

            DXGI_SWAP_CHAIN_DESC desc{};
            swapchain->GetDesc(&desc);
            fx::begin_frame(dt, desc.BufferDesc.Width, desc.BufferDesc.Height);

            loader::Registry::instance().poll_for_changes(dt);
            loader::Registry::instance().process_pending_reloads();

            {
                loader::DispatchGuard dispatch;
                for (const auto& tick : loader::Registry::instance().ticks()) {
                    tick.fn(tick.user, dt);
                }
            }

            settings::flush(dt);

            const float target = g_visible ? 1.0f : 0.0f;
            g_open += (target - g_open) * (1.0f - std::exp(-16.0f * dt));
            if (std::abs(target - g_open) < 0.002f) {
                g_open = target;
            }

            const ui::DrawList* list = nullptr;
            if (g_picking) {
                ui::begin_frame({static_cast<float>(desc.BufferDesc.Width),
                                 static_cast<float>(desc.BufferDesc.Height)}, dt);
                if (g_open > 0.004f) {
                    draw_interface();
                }
                draw_pick_hud(static_cast<float>(desc.BufferDesc.Width),
                              static_cast<float>(desc.BufferDesc.Height));
                ui::end_frame();
                list = &ui::draw();
            } else if (g_open > 0.004f) {
                const float eased = g_open * g_open * (3.0f - 2.0f * g_open);
                const float slide = (1.0f - eased) * 16.0f;

                ui::begin_frame({static_cast<float>(desc.BufferDesc.Width),
                                 static_cast<float>(desc.BufferDesc.Height)}, dt);
                ui::draw().set_alpha(eased);
                g_window_rect.y0 += slide;
                g_window_rect.y1 += slide;
                draw_interface();
                g_window_rect.y0 -= slide;
                g_window_rect.y1 -= slide;
                ui::end_frame();
                list = &ui::draw();
            }
            g_renderer.render(list, swapchain, g_queue);
        }
    }
    return g_present(swapchain, interval, flags);
}

bool capture_vtables(void**& swapchain_vtable, void**& queue_vtable, void**& device_vtable,
                     void**& list_vtable) {
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"BridgerProbe";
    RegisterClassExW(&cls);

    const HWND window = CreateWindowExW(0, cls.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16,
                                        nullptr, nullptr, cls.hInstance, nullptr);
    if (window == nullptr) {
        UnregisterClassW(cls.lpszClassName, cls.hInstance);
        return false;
    }

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    IDXGIFactory4* factory = nullptr;
    IDXGISwapChain1* swapchain = nullptr;
    bool ok = false;

    if (SUCCEEDED(d3d::create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        device_vtable = *reinterpret_cast<void***>(device);
        if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&allocator)))
                && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                                       nullptr, IID_PPV_ARGS(&list)))) {
            list->Close();
            list_vtable = *reinterpret_cast<void***>(list);
        }
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))
                && SUCCEEDED(d3d::create_factory(IID_PPV_ARGS(&factory)))) {
            DXGI_SWAP_CHAIN_DESC1 chain{};
            chain.BufferCount = 2;
            chain.Width = 16;
            chain.Height = 16;
            chain.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            chain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            chain.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            chain.SampleDesc.Count = 1;
            if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue, window, &chain, nullptr, nullptr,
                                                          &swapchain))) {
                swapchain_vtable = *reinterpret_cast<void***>(swapchain);
                queue_vtable = *reinterpret_cast<void***>(queue);
                ok = true;
            }
        }
    }

    if (swapchain != nullptr) swapchain->Release();
    if (factory != nullptr) factory->Release();
    if (list != nullptr) list->Release();
    if (allocator != nullptr) allocator->Release();
    if (queue != nullptr) queue->Release();
    if (device != nullptr) device->Release();
    DestroyWindow(window);
    UnregisterClassW(cls.lpszClassName, cls.hInstance);
    return ok;
}

}

bool visible() {
    return g_visible;
}

bool begin_pick() {
    if (!g_started || g_window == nullptr) {
        return false;
    }
    return PostMessageW(g_window, kPickBegin, 0, 0) != 0;
}

void cancel_pick() {
    if (g_window != nullptr) {
        PostMessageW(g_window, kPickEnd, 0, 0);
    }
}

bool picking() {
    return g_picking;
}

void toggle() {
    g_visible = !g_visible;
    if (g_picking) {
    } else if (g_visible) {
        acquire_mouse();
    } else {
        release_mouse();
    }
    log::info("overlay {}", g_visible ? "opened" : "closed");
}

struct InspectService {
    int version;
    void (*focus)(std::uint64_t address);
};
const InspectService g_inspect_service{
    1, [](std::uint64_t address) { inspect::focus(static_cast<std::uintptr_t>(address)); }};

bool initialise(const std::filesystem::path& root, const mem::Module& game) {
    g_game = game;
    if (g_started) {
        return true;
    }
    if (const auto* api = loader::api(); api != nullptr) {
        api->provide("bridger.inspect.v1", &g_inspect_service);
    }

    if (!d3d::load()) {
        return false;
    }

    if (!ui::font::register_file((root / "assets" / "Bridges-Black.ttf").c_str())) {
        log::warn("overlay: assets/Bridges-Black.ttf not found, the wordmark falls back to Segoe UI");
    }
    if (!ui::font::build(L"Segoe UI")) {
        log::warn("overlay: font atlas built with missing glyphs");
    }

    load_logo(root);

    void** swapchain_vtable = nullptr;
    void** queue_vtable = nullptr;
    void** device_vtable = nullptr;
    void** list_vtable = nullptr;
    if (!capture_vtables(swapchain_vtable, queue_vtable, device_vtable, list_vtable)) {
        log::error("overlay: could not create a probe swapchain");
        return false;
    }

    if (MH_Initialize() == MH_ERROR_NOT_INITIALIZED) {
        log::error("overlay: MinHook is unavailable");
        return false;
    }

    bool ok = true;
    ok &= MH_CreateHook(swapchain_vtable[kPresentIndex], &present_detour,
                        reinterpret_cast<void**>(&g_present)) == MH_OK;
    ok &= MH_CreateHook(swapchain_vtable[kResizeIndex], &resize_detour,
                        reinterpret_cast<void**>(&g_resize)) == MH_OK;
    ok &= MH_CreateHook(queue_vtable[kExecuteIndex], &execute_detour,
                        reinterpret_cast<void**>(&g_execute)) == MH_OK;

    ok &= MH_CreateHookApi(L"user32", "ShowCursor", &show_cursor_detour,
                           reinterpret_cast<void**>(&g_show_cursor)) == MH_OK;
    ok &= MH_CreateHookApi(L"user32", "SetCursor", &set_cursor_detour,
                           reinterpret_cast<void**>(&g_set_cursor)) == MH_OK;
    ok &= MH_CreateHookApi(L"user32", "ClipCursor", &clip_cursor_detour,
                           reinterpret_cast<void**>(&g_clip_cursor)) == MH_OK;
    ok &= MH_CreateHookApi(L"user32", "SetCursorPos", &set_cursor_pos_detour,
                           reinterpret_cast<void**>(&g_set_cursor_pos)) == MH_OK;
    if (!ok) {
        log::error("overlay: failed to create the swapchain hooks");
        return false;
    }

    fx::configure(root);
    fx::prepare_hooks(device_vtable, list_vtable);

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        log::error("overlay: failed to enable the swapchain hooks");
        return false;
    }

    debugger::picker::install(game);

    g_started = true;
    log::info("overlay ready, press F1 in game");
    return true;
}

void shutdown() {
    debugger::picker::set_collecting(false);
    debugger::stop();
    settings::flush_now();
    if (!g_started) {
        return;
    }
    if (g_visible) {
        g_visible = false;
        release_mouse();
    }
    if (g_window != nullptr && g_original_proc != nullptr) {
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_proc));
    }
    MH_DisableHook(MH_ALL_HOOKS);
    g_renderer.shutdown();
    g_started = false;
}

}
