#include "debugger/debugger.h"

#include <Windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/settings.h"
#include "debugger/globals.h"
#include "debugger/model.h"
#include "decima/dumper.h"
#include "overlay/overlay.h"
#include "ui/font.h"
#include "ui/input.h"
#include "ui/raster.h"
#include "ui/ui.h"

namespace bridger::debugger {
namespace {

constexpr UINT kToggleMessage = WM_APP + 1;
constexpr UINT kStopMessage = WM_APP + 2;
constexpr UINT kCaptureMessage = WM_APP + 3;
constexpr UINT kOpenMessage = WM_APP + 4;
constexpr const wchar_t* kClassName = L"BridgerDebugger";
constexpr const char* kSettings = "bridger";

constexpr float kBar = 48.0f;
constexpr float kStatus = 28.0f;
constexpr float kPad = 12.0f;
constexpr float kPaneHeader = 40.0f;
constexpr double kChangeFlash = 1.5;
constexpr std::size_t kRowLimit = 6000;
constexpr std::uint32_t kArrayLimit = 512;
constexpr std::size_t kLogLimit = 4000;

std::thread g_thread;
std::atomic<HWND> g_hwnd{nullptr};
std::atomic<bool> g_running{false};
mem::Module g_game;

ui::Context* g_ui = nullptr;
ui::input::Context* g_input = nullptr;
std::vector<std::uint32_t> g_pixels;
int g_width = 0;
int g_height = 0;
bool g_tracking_leave = false;
double g_last_input = 0.0;
std::mutex g_capture_mutex;
std::filesystem::path g_capture;
std::atomic<std::uintptr_t> g_open_request{0};

double now_seconds() {
    static const auto frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / frequency;
}

enum class Source { Globals = 0, Types = 1, Symbols = 2 };

struct Location {
    std::uintptr_t address = 0;
    std::string type;
};

struct Row {
    int depth = 0;
    std::string path;
    std::uintptr_t address = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::string name;
    std::string type;
    std::string value;
    std::uintptr_t follow = 0;
    bool expandable = false;
    bool expanded = false;
    bool property = false;
};

struct Change {
    std::string value;
    double at = -100.0;
};

struct Watch {
    std::string label;
    std::uintptr_t address = 0;
    std::string type;
    std::string value;
    double changed_at = -100.0;
};

struct ListState {
    float scroll = 0.0f;
};

Source g_source = Source::Globals;
std::string g_globals_filter;
std::string g_types_filter;
std::string g_symbols_filter;
std::vector<std::string> g_type_names;
std::vector<std::size_t> g_type_matches;
std::string g_type_matches_for = "\x01";
std::vector<std::pair<std::string, std::uintptr_t>> g_symbols;
std::vector<std::size_t> g_symbol_matches;
std::string g_symbol_matches_for = "\x01";
std::string g_selected_source;
ListState g_source_list;

Location g_location;
std::vector<Location> g_back;
std::vector<Location> g_forward;
std::string g_address_input;
std::string g_status_line;
std::string g_field_filter;
std::set<std::string> g_expanded;
std::vector<Row> g_rows;
std::map<std::string, Change> g_changes;
std::string g_selected_path;
bool g_live = true;
double g_last_refresh = -100.0;
bool g_dirty = true;
ListState g_object_list;

ListState g_memory_list;
std::vector<Watch> g_watches;
ListState g_watch_list;

std::vector<std::string> g_log;
std::uint64_t g_log_seen = 0;
std::string g_log_filter;
ListState g_log_list;
bool g_log_follow = true;

double g_frame_time = 0.0;

float snap(float value) {
    return std::round(value);
}

ui::Rect snap(const ui::Rect& area) {
    return {snap(area.x0), snap(area.y0), snap(area.x1), snap(area.y1)};
}

bool hovered(const ui::Rect& area) {
    return area.contains(ui::input::state().mouse);
}

bool clicked(const ui::Rect& area) {
    return ui::input::state().pressed && hovered(area);
}

void text(float x, float y, ui::Color color, std::string_view value, float max_width) {
    ui::draw().text_clipped({snap(x), snap(y)}, color, value, std::max(0.0f, max_width));
}

float middle(const ui::Rect& area) {
    return snap(area.y0 + (area.height() - ui::font::line_height()) * 0.5f);
}

void text_centered(const ui::Rect& area, ui::Color color, std::string_view value) {
    const float width = ui::font::measure(value);
    text(area.x0 + (area.width() - width) * 0.5f, middle(area), color, value, area.width());
}

void label(float x, float y, ui::Color color, std::string_view value) {
    const ui::font::ScopedFace caption(ui::font::Face::Caption);
    std::string upper(value);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    ui::draw().text_tracked({snap(x), snap(y)}, color, upper, 1.3f);
}

ui::Rect pane(const ui::Rect& area, std::string_view title, std::string_view detail = {}) {
    const auto& t = ui::theme();
    ui::draw().rect(area, t.window);
    label(area.x0 + kPad, area.y0 + 14.0f, t.text_faint, title);
    if (!detail.empty()) {
        const ui::font::ScopedFace micro(ui::font::Face::Micro);
        const float width = ui::font::measure(detail);
        text(area.x1 - kPad - width, area.y0 + 13.0f, t.text_faint, detail, width + 1.0f);
    }
    return {area.x0, area.y0 + kPaneHeader, area.x1, area.y1};
}

enum class Style { Ghost, Primary };

bool button(const ui::Rect& area, std::string_view caption, bool enabled = true,
            Style style = Style::Ghost) {
    const auto& t = ui::theme();
    auto& list = ui::draw();
    const bool hot = enabled && hovered(area);
    if (style == Style::Primary) {
        list.rect(area, hot ? ui::with_alpha(t.accent, 0.30f) : t.accent_dim);
        list.rect_outline(area, t.accent, 1.0f);
    } else {
        if (hot) {
            list.rect(area, t.raised);
        }
        list.rect_outline(area, hot ? t.border : t.hairline, 1.0f);
    }
    const ui::font::ScopedFace micro(ui::font::Face::Caption);
    const ui::Color color = !enabled ? t.text_faint
                          : style == Style::Primary || hot ? t.text : t.text_dim;
    text_centered(area, color, caption);
    return enabled && clicked(area);
}

enum class Glyph { Back, Forward, Refresh, Pick, Minimize, Maximize, Restore, Close };

void glyph(const ui::Rect& area, Glyph kind, ui::Color color) {
    auto& list = ui::draw();
    const ui::Vec2 c{snap(area.center().x), snap(area.center().y)};
    switch (kind) {
        case Glyph::Back:
            list.line({c.x + 2.5f, c.y - 5.0f}, {c.x - 2.5f, c.y}, color, 1.5f);
            list.line({c.x - 2.5f, c.y}, {c.x + 2.5f, c.y + 5.0f}, color, 1.5f);
            break;
        case Glyph::Forward:
            list.line({c.x - 2.5f, c.y - 5.0f}, {c.x + 2.5f, c.y}, color, 1.5f);
            list.line({c.x + 2.5f, c.y}, {c.x - 2.5f, c.y + 5.0f}, color, 1.5f);
            break;
        case Glyph::Refresh:
            list.arc(c, 5.5f, 0.6f, 5.4f, color, 1.5f);
            list.triangle({c.x + 6.5f, c.y - 5.0f}, {c.x + 6.5f, c.y + 0.5f}, {c.x + 1.5f, c.y - 2.0f},
                          color);
            break;
        case Glyph::Pick:
            list.arc(c, 5.0f, 0.0f, 6.2831853f, color, 1.4f);
            list.rect({c.x - 0.5f, c.y - 8.0f, c.x + 0.5f, c.y - 3.0f}, color);
            list.rect({c.x - 0.5f, c.y + 3.0f, c.x + 0.5f, c.y + 8.0f}, color);
            list.rect({c.x - 8.0f, c.y - 0.5f, c.x - 3.0f, c.y + 0.5f}, color);
            list.rect({c.x + 3.0f, c.y - 0.5f, c.x + 8.0f, c.y + 0.5f}, color);
            break;
        case Glyph::Minimize:
            list.rect({c.x - 5.0f, c.y, c.x + 5.0f, c.y + 1.0f}, color);
            break;
        case Glyph::Maximize:
            list.rect_outline({c.x - 5.0f, c.y - 5.0f, c.x + 5.0f, c.y + 5.0f}, color, 1.0f);
            break;
        case Glyph::Restore:
            list.rect_outline({c.x - 5.0f, c.y - 3.0f, c.x + 3.0f, c.y + 5.0f}, color, 1.0f);
            list.rect({c.x - 3.0f, c.y - 5.0f, c.x + 5.0f, c.y - 4.0f}, color);
            list.rect({c.x + 4.0f, c.y - 5.0f, c.x + 5.0f, c.y + 3.0f}, color);
            break;
        case Glyph::Close:
            list.line({c.x - 5.0f, c.y - 5.0f}, {c.x + 5.0f, c.y + 5.0f}, color, 1.2f);
            list.line({c.x + 5.0f, c.y - 5.0f}, {c.x - 5.0f, c.y + 5.0f}, color, 1.2f);
            break;
    }
}

bool icon_button(const ui::Rect& area, Glyph kind, bool enabled = true) {
    const auto& t = ui::theme();
    const bool hot = enabled && hovered(area);
    if (hot) {
        ui::draw().rect(area, t.raised);
    }
    glyph(area, kind, !enabled ? t.text_faint : hot ? t.text : t.text_dim);
    return enabled && clicked(area);
}

int segmented(const ui::Rect& area, const char* const* labels, int count, int selected) {
    const auto& t = ui::theme();
    auto& list = ui::draw();
    list.rect(area, t.sunken);
    int chosen = selected;
    const float width = std::floor(area.width() / static_cast<float>(count));
    for (int i = 0; i < count; ++i) {
        const float x0 = area.x0 + width * static_cast<float>(i);
        const float x1 = i == count - 1 ? area.x1 : x0 + width;
        const ui::Rect segment{x0, area.y0, x1, area.y1};
        const bool on = i == selected;
        const bool hot = hovered(segment);
        if (on) {
            list.rect(segment.inset(2.0f), t.raised);
        } else if (hot) {
            list.rect(segment.inset(2.0f), ui::with_alpha(t.raised, 0.5f));
        }
        const ui::font::ScopedFace caption(ui::font::Face::Caption);
        text_centered(segment, on ? t.text : hot ? t.text_dim : t.text_faint, labels[i]);
        if (clicked(segment)) {
            chosen = i;
        }
    }
    list.rect_outline(area, t.hairline, 1.0f);
    return chosen;
}

bool field(const ui::Rect& area, std::string_view id, std::string& value,
           std::string_view placeholder) {
    ui::begin_area(snap(area));
    const bool changed = ui::input_text(id, value, snap(area).width(), placeholder);
    ui::end_area();
    return changed;
}

void row_ground(const ui::Rect& row, bool selected, bool hot) {
    const auto& t = ui::theme();
    auto& list = ui::draw();
    if (selected) {
        list.rect(row, t.accent_dim);
        list.rect({row.x0, row.y0, row.x0 + 2.0f, row.y1}, t.accent);
    } else if (hot) {
        list.rect(row, ui::with_alpha(t.wash, 0.035f));
    }
}

struct Visible {
    std::size_t first = 0;
    std::size_t last = 0;
    float top = 0.0f;
};

Visible list_view(ListState& state, const ui::Rect& area, std::size_t count, float row) {
    const auto& t = ui::theme();
    const auto& in = ui::input::state();
    const float content = static_cast<float>(count) * row;
    const float limit = std::max(0.0f, content - area.height());
    if (hovered(area) && in.wheel != 0.0f) {
        state.scroll -= in.wheel * row * 3.0f;
    }
    state.scroll = snap(std::clamp(state.scroll, 0.0f, limit));

    if (limit > 0.0f) {
        const float bar = std::max(24.0f, area.height() * (area.height() / content));
        const float y = snap(area.y0 + (area.height() - bar) * (state.scroll / limit));
        const ui::Rect track{area.x1 - 10.0f, area.y0, area.x1, area.y1};
        const bool on_track = hovered(track);
        ui::draw().rect({area.x1 - (on_track ? 6.0f : 4.0f), y + 2.0f, area.x1 - 2.0f, y + bar - 2.0f},
                        on_track ? t.text_faint : t.border);
        if (in.down && on_track) {
            const float local = (in.mouse.y - area.y0 - bar * 0.5f) / std::max(1.0f, area.height() - bar);
            state.scroll = snap(std::clamp(local, 0.0f, 1.0f) * limit);
        }
    }

    Visible visible;
    visible.first = static_cast<std::size_t>(state.scroll / row);
    visible.last = std::min(count, visible.first + static_cast<std::size_t>(area.height() / row) + 2);
    visible.top = snap(area.y0 - std::fmod(state.scroll, row));
    return visible;
}

bool matches(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto found = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                   [](char a, char b) {
                                       return std::tolower(static_cast<unsigned char>(a))
                                           == std::tolower(static_cast<unsigned char>(b));
                                   });
    return found != haystack.end();
}

void load_location(const Location& location) {
    g_location = location;
    g_address_input = location.address != 0 ? std::format("{:x}", location.address) : location.type;
    g_expanded.clear();
    g_changes.clear();
    g_selected_path.clear();
    g_object_list.scroll = 0.0f;
    g_memory_list.scroll = 0.0f;
    g_dirty = true;
}

void navigate(const Location& location) {
    if (g_location.address != 0 || !g_location.type.empty()) {
        g_back.push_back(g_location);
    }
    g_forward.clear();
    load_location(location);
}

void navigate_address(std::uintptr_t address, std::string type_hint = {}) {
    navigate({address, std::move(type_hint)});
}

void go_back() {
    if (g_back.empty()) {
        return;
    }
    g_forward.push_back(g_location);
    const auto location = g_back.back();
    g_back.pop_back();
    load_location(location);
}

void go_forward() {
    if (g_forward.empty()) {
        return;
    }
    g_back.push_back(g_location);
    const auto location = g_forward.back();
    g_forward.pop_back();
    load_location(location);
}

void submit_address() {
    std::uintptr_t address = 0;
    if (model::parse_address(g_address_input, address)) {
        navigate_address(address);
        return;
    }
    decima::TypeDetail detail;
    if (model::describe(g_address_input, detail)) {
        navigate({0, g_address_input});
        return;
    }
    g_status_line = std::format("'{}' is neither a hexadecimal address nor a type name",
                                g_address_input);
}

void add_value(std::string name, const std::string& type, std::uintptr_t address,
               std::uint32_t offset, int depth, const std::string& parent, std::uintptr_t getter);

void add_fields(const std::string& type, std::uintptr_t base, int depth, const std::string& parent) {
    for (const auto& field : model::fields_of(type)) {
        if (g_rows.size() >= kRowLimit) {
            return;
        }
        add_value(field.name, field.type, base + field.offset, field.offset, depth, parent,
                  field.getter);
    }
}

void add_value(std::string name, const std::string& type, std::uintptr_t address,
               std::uint32_t offset, int depth, const std::string& parent, std::uintptr_t getter) {
    Row row;
    row.depth = depth;
    row.path = parent + "/" + name;
    row.name = std::move(name);
    row.type = type;
    row.offset = offset;
    row.address = address;

    if (getter != 0) {
        row.property = true;
        row.value = std::format("property, getter {:#x}", getter);
        g_rows.push_back(std::move(row));
        return;
    }

    const auto shape = model::shape_of(type);
    row.size = model::size_of(type);
    if (g_location.address != 0 || depth > 0) {
        row.value = model::format_value(type, address, row.follow);
    } else {
        row.value = row.size != 0 ? std::format("{} bytes", row.size) : std::string{};
    }

    std::uint32_t count = 0;
    std::uintptr_t data = 0;
    std::string element;
    std::uint32_t element_size = 0;
    const bool live = g_location.address != 0;
    switch (shape) {
        case model::Shape::Struct:
            row.expandable = true;
            break;
        case model::Shape::Pointer:
            row.expandable = row.follow != 0;
            break;
        case model::Shape::Array:
            element = model::element_of(type);
            element_size = model::size_of(element);
            row.expandable = live && element_size != 0 && model::read(address, count)
                          && model::read(address + 8, data) && count > 0 && data != 0;
            break;
        default:
            break;
    }
    row.expanded = row.expandable && g_expanded.contains(row.path);

    if (live) {
        auto& change = g_changes[row.path];
        if (!change.value.empty() && change.value != row.value) {
            change.at = now_seconds();
        }
        change.value = row.value;
    }

    const bool expanded = row.expanded;
    const auto path = row.path;
    const auto follow = row.follow;
    g_rows.push_back(std::move(row));
    if (!expanded || depth > 24) {
        return;
    }

    switch (shape) {
        case model::Shape::Struct:
            add_fields(type, address, depth + 1, path);
            break;
        case model::Shape::Pointer: {
            const auto target = model::type_name_at(follow);
            if (!target.empty()) {
                add_fields(target, follow, depth + 1, path);
            }
            break;
        }
        case model::Shape::Array: {
            const auto shown = std::min(count, kArrayLimit);
            for (std::uint32_t i = 0; i < shown && g_rows.size() < kRowLimit; ++i) {
                add_value(std::format("[{}]", i), element, data + i * element_size,
                          i * element_size, depth + 1, path, 0);
            }
            if (count > shown) {
                Row more;
                more.depth = depth + 1;
                more.path = path + "/...";
                more.name = std::format("... {} more", count - shown);
                g_rows.push_back(std::move(more));
            }
            break;
        }
        default:
            break;
    }
}

std::string root_type() {
    if (g_location.address != 0) {
        auto name = model::type_name_at(g_location.address);
        return name.empty() ? g_location.type : name;
    }
    return g_location.type;
}

void refresh_rows() {
    g_rows.clear();
    g_status_line.clear();
    if (g_location.address == 0 && g_location.type.empty()) {
        return;
    }
    const auto type = root_type();
    if (type.empty()) {
        g_status_line = std::format("{:#x} has no RTTI in vtable slot 0. Enter a type name to "
                                    "read it as one, or look at it in the memory pane.",
                                    g_location.address);
        return;
    }
    add_fields(type, g_location.address, 0, "");
    if (g_rows.empty()) {
        g_status_line = std::format("{} has no reflected fields.", type);
    }
}

const Row* selected_row() {
    for (const auto& row : g_rows) {
        if (row.path == g_selected_path) {
            return &row;
        }
    }
    return nullptr;
}

void add_watch(const Row& row) {
    Watch watch;
    watch.label = root_type() + row.path;
    std::replace(watch.label.begin(), watch.label.end(), '/', '.');
    watch.address = row.address;
    watch.type = row.type;
    g_watches.push_back(std::move(watch));
}

std::vector<ui::Rect> g_bar_controls;
ui::Rect g_maximize_button;
bool g_maximized = false;
bool g_maximize_pressed = false;

enum class WindowAction { None, Minimize, Maximize, Close };
WindowAction g_window_action = WindowAction::None;

void draw_title_bar(const ui::Rect& area) {
    const auto& t = ui::theme();
    auto& list = ui::draw();
    g_bar_controls.clear();
    list.rect(area, t.window_low);
    list.rect({area.x0, area.y1 - 1.0f, area.x1, area.y1}, t.hairline);

    float x = area.x0 + 16.0f;
    {
        const ui::font::ScopedFace brand(ui::font::Face::Brand);
        list.text({x, middle(area)}, t.text, "BRIDGER");
        x += snap(ui::font::measure("BRIDGER")) + 10.0f;
    }
    {
        const ui::font::ScopedFace caption(ui::font::Face::Caption);
        list.text_tracked({x, middle(area) + 1.0f}, t.accent, "DEBUGGER", 1.4f);
        x += snap(ui::font::measure_tracked("DEBUGGER", 1.4f)) + 24.0f;
    }

    const float control = 46.0f;
    const ui::Rect close{area.x1 - control, area.y0, area.x1, area.y1 - 1.0f};
    const ui::Rect maximize{close.x0 - control, area.y0, close.x0, area.y1 - 1.0f};
    const ui::Rect minimize{maximize.x0 - control, area.y0, maximize.x0, area.y1 - 1.0f};
    g_maximize_button = maximize;
    g_bar_controls.push_back(minimize);
    g_bar_controls.push_back(close);
    if (hovered(minimize)) {
        list.rect(minimize, t.raised);
    }
    glyph(minimize, Glyph::Minimize, hovered(minimize) ? t.text : t.text_dim);
    if (hovered(maximize)) {
        list.rect(maximize, t.raised);
    }
    glyph(maximize, g_maximized ? Glyph::Restore : Glyph::Maximize,
          hovered(maximize) ? t.text : t.text_dim);
    const bool close_hot = hovered(close);
    if (close_hot) {
        list.rect(close, ui::rgba(196, 43, 28));
    }
    glyph(close, Glyph::Close, close_hot ? ui::rgba(255, 255, 255) : t.text_dim);
    if (clicked(minimize)) {
        g_window_action = WindowAction::Minimize;
    }
    if (clicked(close)) {
        g_window_action = WindowAction::Close;
    }

    const float y = snap(area.y0 + (area.height() - 32.0f) * 0.5f);
    const ui::Rect back{x, y, x + 32.0f, y + 32.0f};
    const ui::Rect forward{x + 34.0f, y, x + 66.0f, y + 32.0f};
    g_bar_controls.push_back(back);
    g_bar_controls.push_back(forward);
    if (icon_button(back, Glyph::Back, !g_back.empty())) {
        go_back();
    }
    if (icon_button(forward, Glyph::Forward, !g_forward.empty())) {
        go_forward();
    }
    x = forward.x1 + 10.0f;

    const float right_limit = minimize.x0 - 16.0f;
    const float after = 36.0f + 8.0f + 92.0f + 8.0f + 64.0f;
    const float width = std::clamp(right_limit - x - after - 40.0f, 200.0f, 640.0f);
    const ui::Rect address{x, y, x + width, y + 32.0f};
    g_bar_controls.push_back(address);
    ui::begin_area(address);
    const auto line = ui::input_line("debugger_address", g_address_input, width,
                                     "Address or type name");
    ui::end_area();
    if (line.submitted) {
        submit_address();
    }
    x = address.x1 + 8.0f;

    const ui::Rect refresh{x, y, x + 36.0f, y + 32.0f};
    g_bar_controls.push_back(refresh);
    if (icon_button(refresh, Glyph::Refresh)) {
        g_dirty = true;
    }
    x = refresh.x1 + 8.0f;

    const ui::Rect pick{x, y, x + 92.0f, y + 32.0f};
    g_bar_controls.push_back(pick);
    {
        const bool active = overlay::picking();
        const bool hot = hovered(pick);
        auto& bar = ui::draw();
        if (active) {
            bar.rect(pick, t.accent_dim);
            bar.rect_outline(pick, t.accent, 1.0f);
        } else {
            if (hot) {
                bar.rect(pick, t.raised);
            }
            bar.rect_outline(pick, hot ? t.border : t.hairline, 1.0f);
        }
        const ui::Color color = active || hot ? t.text : t.text_dim;
        glyph({pick.x0 + 4.0f, pick.y0, pick.x0 + 30.0f, pick.y1}, Glyph::Pick,
              active ? t.accent_bright : color);
        const ui::font::ScopedFace caption(ui::font::Face::Caption);
        text(pick.x0 + 32.0f, middle(pick), color, active ? "Picking" : "Pick", 56.0f);
        if (clicked(pick)) {
            if (active) {
                overlay::cancel_pick();
            } else if (!overlay::begin_pick()) {
                g_status_line = "Picking needs the in-game overlay, which is not running.";
            }
        }
    }
    x = pick.x1 + 8.0f;

    const ui::Rect live{x, y, x + 64.0f, y + 32.0f};
    g_bar_controls.push_back(live);
    if (button(live, "Live", true, g_live ? Style::Primary : Style::Ghost)) {
        g_live = !g_live;
    }
}

void draw_sources(const ui::Rect& area) {
    const auto& t = ui::theme();
    auto& list = ui::draw();
    const auto body = pane(area, "Explore");

    const char* const labels[] = {"Globals", "Types", "Symbols"};
    const ui::Rect modes{body.x0 + kPad, body.y0, body.x1 - kPad, body.y0 + 30.0f};
    const int mode = segmented(modes, labels, 3, static_cast<int>(g_source));
    if (mode != static_cast<int>(g_source)) {
        g_source = static_cast<Source>(mode);
        g_source_list.scroll = 0.0f;
    }

    const ui::Rect search{body.x0 + kPad, modes.y1 + 8.0f, body.x1 - kPad, modes.y1 + 40.0f};
    switch (g_source) {
        case Source::Globals:
            field(search, "globals_filter", g_globals_filter, "Filter globals");
            break;
        case Source::Types:
            field(search, "types_filter", g_types_filter, "Search types");
            break;
        case Source::Symbols:
            field(search, "symbols_filter", g_symbols_filter, "Search exports");
            break;
    }

    const ui::Rect rows{body.x0, search.y1 + 10.0f, body.x1, body.y1};
    list.push_clip(rows);
    const float two_line = 42.0f;
    const float one_line = 28.0f;

    if (g_source == Source::Globals) {
        const auto all = globals::snapshot();
        std::vector<const globals::Global*> shown;
        for (const auto& global : all) {
            if (matches(global.type, g_globals_filter) || matches(global.source, g_globals_filter)) {
                shown.push_back(&global);
            }
        }
        if (all.empty()) {
            const ui::font::ScopedFace micro(ui::font::Face::Micro);
            text(rows.x0 + kPad, rows.y0 + 6.0f, t.text_faint,
                 globals::running()
                     ? std::format("Scanning exports  {} / {}", globals::examined(), globals::total())
                     : std::string("No globals found. Is symbols.json deployed?"),
                 rows.width() - kPad * 2.0f);
        }
        const auto visible = list_view(g_source_list, rows, shown.size(), two_line);
        for (std::size_t i = visible.first; i < visible.last; ++i) {
            const auto& global = *shown[i];
            const float y = visible.top + static_cast<float>(i - visible.first) * two_line;
            const ui::Rect row{rows.x0, y, rows.x1, y + two_line};
            const auto key = std::format("g{:x}", global.slot);
            row_ground(row, g_selected_source == key, hovered(row) && hovered(rows));
            const auto object = globals::resolve(global);
            {
                const ui::font::ScopedFace body_face(ui::font::Face::Body);
                text(row.x0 + kPad, y + 4.0f, t.text, global.type, row.width() - kPad * 2.0f);
            }
            {
                const ui::font::ScopedFace micro(ui::font::Face::Micro);
                text(row.x0 + kPad, y + 23.0f, t.text_faint,
                     object == 0 ? std::format("{}  null", global.source)
                                 : std::format("{}  {:#x}", global.source, object),
                     row.width() - kPad * 2.0f);
            }
            if (clicked(row) && hovered(rows)) {
                g_selected_source = key;
                if (object != 0) {
                    navigate_address(object);
                } else {
                    g_status_line = std::format("{} is null right now", global.source);
                }
            }
        }
    } else if (g_source == Source::Types) {
        if (g_type_names.empty()) {
            g_type_names = decima::type_names();
            std::sort(g_type_names.begin(), g_type_names.end());
        }
        if (g_type_matches_for != g_types_filter) {
            g_type_matches.clear();
            for (std::size_t i = 0; i < g_type_names.size(); ++i) {
                if (matches(g_type_names[i], g_types_filter)) {
                    g_type_matches.push_back(i);
                }
            }
            g_type_matches_for = g_types_filter;
            g_source_list.scroll = 0.0f;
        }
        const ui::font::ScopedFace body_face(ui::font::Face::Body);
        const auto visible = list_view(g_source_list, rows, g_type_matches.size(), one_line);
        for (std::size_t i = visible.first; i < visible.last; ++i) {
            const auto& name = g_type_names[g_type_matches[i]];
            const float y = visible.top + static_cast<float>(i - visible.first) * one_line;
            const ui::Rect row{rows.x0, y, rows.x1, y + one_line};
            const bool selected = g_location.address == 0 && g_location.type == name;
            const bool hot = hovered(row) && hovered(rows);
            row_ground(row, selected, hot);
            text(row.x0 + kPad, middle(row), selected || hot ? t.text : t.text_dim, name,
                 row.width() - kPad * 2.0f);
            if (clicked(row) && hovered(rows)) {
                navigate({0, name});
            }
        }
    } else {
        if (g_symbols.empty()) {
            const auto index = decima::symbol_index();
            g_symbols.assign(index->begin(), index->end());
        }
        if (g_symbol_matches_for != g_symbols_filter) {
            g_symbol_matches.clear();
            for (std::size_t i = 0; i < g_symbols.size(); ++i) {
                if (matches(g_symbols[i].first, g_symbols_filter)) {
                    g_symbol_matches.push_back(i);
                }
            }
            g_symbol_matches_for = g_symbols_filter;
            g_source_list.scroll = 0.0f;
        }
        const auto visible = list_view(g_source_list, rows, g_symbol_matches.size(), two_line);
        for (std::size_t i = visible.first; i < visible.last; ++i) {
            const auto& [name, address] = g_symbols[g_symbol_matches[i]];
            const float y = visible.top + static_cast<float>(i - visible.first) * two_line;
            const ui::Rect row{rows.x0, y, rows.x1, y + two_line};
            row_ground(row, g_selected_source == name, hovered(row) && hovered(rows));
            const auto split = name.find("::");
            {
                const ui::font::ScopedFace body_face(ui::font::Face::Body);
                text(row.x0 + kPad, y + 4.0f, t.text,
                     split == std::string::npos ? name : name.substr(split + 2),
                     row.width() - kPad * 2.0f);
            }
            {
                const ui::font::ScopedFace micro(ui::font::Face::Micro);
                text(row.x0 + kPad, y + 23.0f, t.text_faint,
                     std::format("{}  +{:x}", split == std::string::npos ? "" : name.substr(0, split),
                                 address - g_game.base),
                     row.width() - kPad * 2.0f);
            }
            if (clicked(row) && hovered(rows)) {
                g_selected_source = name;
                navigate_address(address);
            }
        }
    }
    list.pop_clip();
}

void draw_object(const ui::Rect& area) {
    const auto& t = ui::theme();
    auto& list = ui::draw();
    const auto type = root_type();
    const auto body = pane(area, "Object",
                           g_rows.empty() ? std::string{} : std::format("{} fields", g_rows.size()));

    {
        const ui::font::ScopedFace title(ui::font::Face::Title);
        const auto heading = type.empty() ? std::string("Nothing open") : type;
        text(body.x0 + kPad, body.y0 - 2.0f, type.empty() ? t.text_faint : t.text, heading,
             body.width() * 0.5f);
        const float after = body.x0 + kPad + std::min(ui::font::measure(heading), body.width() * 0.5f) + 12.0f;
        const ui::font::ScopedFace mono(ui::font::Face::Mono);
        if (g_location.address != 0) {
            text(after, body.y0 + 5.0f, t.text_faint, std::format("{:#x}", g_location.address), 200.0f);
        } else if (!type.empty()) {
            text(after, body.y0 + 5.0f, t.text_faint, "layout", 200.0f);
        }
    }

    const Row* selected = selected_row();
    float x = body.x1 - kPad;
    const float by = body.y0 - 4.0f;
    const auto action = [&](std::string_view caption, float width, bool enabled) {
        x -= width;
        const bool pressed = button({x, by, x + width, by + 30.0f}, caption, enabled);
        x -= 6.0f;
        return pressed;
    };
    if (action("Collapse all", 96.0f, !g_expanded.empty())) {
        g_expanded.clear();
        g_dirty = true;
    }
    if (action("Watch", 64.0f, selected != nullptr && !selected->property)) {
        add_watch(*selected);
    }
    if (action("Open", 60.0f, selected != nullptr && selected->follow != 0)) {
        navigate_address(selected->follow);
        return;
    }

    const ui::Rect filter{body.x0 + kPad, body.y0 + 36.0f, body.x1 - kPad, body.y0 + 68.0f};
    field(filter, "field_filter", g_field_filter, "Filter fields by name or type");

    const float c_offset = body.x0 + kPad;
    const float c_name = body.x0 + 70.0f;
    const float c_type = snap(c_name + std::clamp(body.width() * 0.30f, 180.0f, 320.0f));
    const float c_value = snap(c_type + std::clamp(body.width() * 0.22f, 150.0f, 260.0f));
    const ui::Rect header{body.x0, filter.y1 + 10.0f, body.x1, filter.y1 + 36.0f};
    {
        const ui::font::ScopedFace caption(ui::font::Face::Caption);
        const float ty = middle(header);
        text(c_offset, ty, t.text_faint, "OFFSET", 60.0f);
        text(c_name, ty, t.text_faint, "FIELD", c_type - c_name);
        text(c_type, ty, t.text_faint, "TYPE", c_value - c_type);
        text(c_value, ty, t.text_faint, "VALUE", body.x1 - c_value);
    }
    list.rect({body.x0 + kPad, header.y1 - 1.0f, body.x1 - kPad, header.y1}, t.hairline);
    const ui::Rect content{body.x0, header.y1, body.x1, body.y1};

    if (g_rows.empty()) {
        const ui::font::ScopedFace body_face(ui::font::Face::Body);
        const auto message = !g_status_line.empty()
                                 ? g_status_line
                                 : std::string("Pick a global, type or export on the left, or enter "
                                               "an address or type name above.");
        list.push_clip(content);
        text(content.x0 + kPad, content.y0 + 16.0f, t.text_dim, message, content.width() - kPad * 2.0f);
        list.pop_clip();
        return;
    }

    std::vector<const Row*> shown;
    shown.reserve(g_rows.size());
    for (const auto& row : g_rows) {
        if (g_field_filter.empty() || matches(row.name, g_field_filter)
                || matches(row.type, g_field_filter)) {
            shown.push_back(&row);
        }
    }

    list.push_clip(content);
    const double now = now_seconds();
    const auto& in = ui::input::state();
    std::string toggle_path;
    std::uintptr_t open_address = 0;
    const float row_height = 26.0f;
    const auto visible = list_view(g_object_list, content, shown.size(), row_height);
    for (std::size_t i = visible.first; i < visible.last; ++i) {
        const Row& row = *shown[i];
        const float ry = visible.top + static_cast<float>(i - visible.first) * row_height;
        const ui::Rect line{content.x0, ry, content.x1, ry + row_height};
        const bool hot = hovered(line) && hovered(content);
        row_ground(line, row.path == g_selected_path, hot);

        {
            const ui::font::ScopedFace mono(ui::font::Face::Mono);
            if (!row.property && !row.name.starts_with("...")) {
                text(c_offset, middle(line), t.text_faint, std::format("{:04x}", row.offset), 54.0f);
            }
        }
        const float indent = c_name + static_cast<float>(row.depth) * 16.0f;
        const ui::Rect expander{indent - 4.0f, ry, indent + 14.0f, ry + row_height};
        if (row.expandable) {
            const ui::Color color = hovered(expander) ? t.accent_bright : t.text_dim;
            const ui::Vec2 c{indent + 5.0f, snap(line.center().y)};
            if (row.expanded) {
                list.triangle({c.x - 4.0f, c.y - 2.0f}, {c.x + 4.0f, c.y - 2.0f}, {c.x, c.y + 3.0f}, color);
            } else {
                list.triangle({c.x - 2.0f, c.y - 4.0f}, {c.x + 3.0f, c.y}, {c.x - 2.0f, c.y + 4.0f}, color);
            }
        }
        {
            const ui::font::ScopedFace body_face(ui::font::Face::Body);
            text(indent + 16.0f, middle(line), row.property ? t.text_faint : t.text, row.name,
                 c_type - indent - 24.0f);
        }
        {
            const ui::font::ScopedFace micro(ui::font::Face::Micro);
            text(c_type, middle(line), t.text_faint, row.type, c_value - c_type - 12.0f);
        }
        const auto change = g_changes.find(row.path);
        const bool flashing = change != g_changes.end() && now - change->second.at < kChangeFlash;
        if (flashing) {
            const float fade = static_cast<float>(1.0 - (now - change->second.at) / kChangeFlash);
            list.rect({c_value - 6.0f, ry + 3.0f, line.x1 - kPad, ry + row_height - 3.0f},
                      ui::with_alpha(t.warn, 0.14f * fade));
        }
        {
            const ui::font::ScopedFace mono(ui::font::Face::Mono);
            const ui::Color value_color = flashing ? t.warn
                                        : row.follow != 0 ? t.accent
                                        : row.property ? t.text_faint : t.text_dim;
            text(c_value, middle(line), value_color, row.value, line.x1 - c_value - kPad - 8.0f);
        }

        if (hot && in.pressed) {
            if (row.expandable && hovered(expander)) {
                toggle_path = row.path;
            } else {
                g_selected_path = row.path;
                if (in.double_clicked) {
                    if (row.follow != 0) {
                        open_address = row.follow;
                    } else if (row.expandable) {
                        toggle_path = row.path;
                    }
                }
            }
        }
    }
    list.pop_clip();

    if (!toggle_path.empty()) {
        if (!g_expanded.erase(toggle_path)) {
            g_expanded.insert(toggle_path);
        }
        g_dirty = true;
    }
    if (open_address != 0) {
        navigate_address(open_address);
    }
}

void draw_memory(const ui::Rect& area) {
    const auto& t = ui::theme();
    auto& list = ui::draw();

    const Row* selected = selected_row();
    std::uintptr_t base = g_location.address;
    std::uint32_t length = 256;
    if (const auto type = root_type(); !type.empty() && g_location.address != 0) {
        length = std::clamp<std::uint32_t>(model::size_of(type), 64, 0x4000);
    }
    std::uintptr_t highlight_from = 0;
    std::uintptr_t highlight_to = 0;
    if (selected != nullptr && !selected->property && selected->address != 0) {
        highlight_from = selected->address;
        highlight_to = selected->address + std::max<std::uint32_t>(selected->size, 1);
        if (highlight_from < base || highlight_to > base + length) {
            base = selected->address & ~static_cast<std::uintptr_t>(0xF);
            length = std::clamp<std::uint32_t>(selected->size + 64, 128, 0x4000);
        }
    }
    base &= ~static_cast<std::uintptr_t>(0xF);

    const auto body = pane(area, "Memory",
                           base != 0 ? std::format("{:#x}   {} bytes", base, length) : std::string{});
    if (base == 0) {
        const ui::font::ScopedFace micro(ui::font::Face::Micro);
        text(body.x0 + kPad, body.y0, t.text_faint, "Open an object or address to see its bytes.",
             body.width() - kPad * 2.0f);
        return;
    }

    const ui::font::ScopedFace mono(ui::font::Face::Mono);
    const float glyph_width = ui::font::measure("0");
    const ui::Rect content{body.x0, body.y0 - 6.0f, body.x1, body.y1};
    const std::size_t lines = (length + 15) / 16;
    const float row_height = 20.0f;
    list.push_clip(content);
    const auto visible = list_view(g_memory_list, content, lines, row_height);
    for (std::size_t i = visible.first; i < visible.last; ++i) {
        const std::uintptr_t at = base + i * 16;
        const float y = visible.top + static_cast<float>(i - visible.first) * row_height;
        const ui::Rect line{content.x0, y, content.x1, y + row_height};
        const float ty = middle(line);
        float x = content.x0 + kPad;
        text(x, ty, t.text_faint, std::format("{:012x}", at), glyph_width * 13.0f);
        x += snap(glyph_width * 14.0f);

        std::uint8_t bytes[16]{};
        bool readable[16]{};
        for (int b = 0; b < 16; ++b) {
            readable[b] = model::read_bytes(at + b, bytes + b, 1);
        }
        const float hex_x = x;
        for (int b = 0; b < 16; ++b) {
            const std::uintptr_t address = at + b;
            const float bx = snap(hex_x + static_cast<float>(b) * glyph_width * 3.0f
                                  + (b >= 8 ? glyph_width : 0.0f));
            const bool lit = address >= highlight_from && address < highlight_to;
            if (lit) {
                const bool next_lit = b < 15 && address + 1 < highlight_to;
                list.rect({bx - 3.0f, y + 2.0f, bx + glyph_width * (next_lit ? 3.0f : 2.0f) + 3.0f,
                           y + row_height - 2.0f},
                          t.accent_dim);
            }
            const ui::Color color = !readable[b] ? ui::with_alpha(t.bad, 0.7f)
                                  : lit ? t.text
                                  : bytes[b] == 0 ? t.text_faint : t.text_dim;
            text(bx, ty, color, readable[b] ? std::format("{:02x}", bytes[b]) : std::string("??"),
                 glyph_width * 2.5f);
        }
        x = snap(hex_x + glyph_width * 49.0f + 12.0f);
        std::string ascii(16, '.');
        for (int b = 0; b < 16; ++b) {
            if (readable[b] && bytes[b] >= 32 && bytes[b] < 127) {
                ascii[b] = static_cast<char>(bytes[b]);
            }
        }
        text(x, ty, t.text_faint, ascii, glyph_width * 17.0f);
        x += snap(glyph_width * 18.0f);

        std::string notes;
        for (int q = 0; q < 2; ++q) {
            std::uintptr_t pointer = 0;
            std::memcpy(&pointer, bytes + q * 8, sizeof pointer);
            if (const auto name = model::type_name_at(pointer); !name.empty()) {
                notes += std::format("{}+{:x} {}   ", q == 0 ? "" : " ", q * 8, name);
            } else if (pointer >= g_game.text_begin && pointer < g_game.text_end) {
                notes += std::format("{}+{:x} ds.exe+{:x}   ", q == 0 ? "" : " ", q * 8,
                                     pointer - g_game.base);
            }
        }
        if (!notes.empty()) {
            text(x, ty, t.accent, notes, content.x1 - x - kPad);
        }
    }
    list.pop_clip();
}

void draw_watches(const ui::Rect& area) {
    const auto& t = ui::theme();
    auto& list = ui::draw();
    const auto body = pane(area, "Watches",
                           g_watches.empty() ? std::string{} : std::format("{}", g_watches.size()));
    if (g_watches.empty()) {
        const ui::font::ScopedFace micro(ui::font::Face::Micro);
        text(body.x0 + kPad, body.y0, t.text_faint, "Select a field and press Watch to pin it.",
             body.width() - kPad * 2.0f);
        return;
    }
    const double now = now_seconds();
    const ui::Rect content{body.x0, body.y0 - 6.0f, body.x1, body.y1};
    const float row_height = 46.0f;
    list.push_clip(content);
    int remove = -1;
    const auto visible = list_view(g_watch_list, content, g_watches.size(), row_height);
    for (std::size_t i = visible.first; i < visible.last; ++i) {
        const auto& watch = g_watches[i];
        const float y = visible.top + static_cast<float>(i - visible.first) * row_height;
        const ui::Rect row{content.x0, y, content.x1, y + row_height};
        const bool hot = hovered(row) && hovered(content);
        row_ground(row, false, hot);
        {
            const ui::font::ScopedFace micro(ui::font::Face::Micro);
            text(row.x0 + kPad, y + 6.0f, t.text_faint, watch.label, row.width() - kPad * 2.0f - 30.0f);
        }
        {
            const ui::font::ScopedFace mono(ui::font::Face::Mono);
            text(row.x0 + kPad, y + 24.0f, now - watch.changed_at < kChangeFlash ? t.warn : t.text,
                 watch.value, row.width() - kPad * 2.0f);
        }
        const ui::Rect close{row.x1 - kPad - 24.0f, y + 4.0f, row.x1 - kPad, y + 22.0f};
        if (hot && icon_button(close, Glyph::Close)) {
            remove = static_cast<int>(i);
        } else if (hot && ui::input::state().pressed && ui::input::state().double_clicked) {
            navigate_address(watch.address, watch.type);
        }
    }
    list.pop_clip();
    if (remove >= 0) {
        g_watches.erase(g_watches.begin() + remove);
    }
}

void draw_log(const ui::Rect& area) {
    const auto& t = ui::theme();
    auto& list = ui::draw();

    std::vector<std::string> fresh;
    g_log_seen = log::snapshot_since(g_log_seen, fresh);
    for (auto& line : fresh) {
        g_log.push_back(std::move(line));
    }
    if (g_log.size() > kLogLimit) {
        g_log.erase(g_log.begin(), g_log.begin() + static_cast<std::ptrdiff_t>(g_log.size() - kLogLimit));
    }

    const auto body = pane(area, "Log");
    const ui::Rect filter{body.x0 + kPad, body.y0 - 4.0f, body.x1 - kPad, body.y0 + 28.0f};
    field(filter, "log_filter", g_log_filter, "Filter log");
    const ui::Rect content{body.x0, filter.y1 + 8.0f, body.x1, body.y1};

    std::vector<const std::string*> shown;
    for (const auto& line : g_log) {
        if (matches(line, g_log_filter)) {
            shown.push_back(&line);
        }
    }

    const float row_height = 20.0f;
    const float limit = std::max(0.0f, static_cast<float>(shown.size()) * row_height - content.height());
    const auto& in = ui::input::state();
    if (hovered(content) && in.wheel != 0.0f) {
        g_log_follow = in.wheel < 0.0f && g_log_list.scroll - in.wheel * row_height * 3.0f >= limit - 1.0f;
    }
    if (g_log_follow) {
        g_log_list.scroll = limit;
    }

    const ui::font::ScopedFace mono(ui::font::Face::Mono);
    list.push_clip(content);
    const auto visible = list_view(g_log_list, content, shown.size(), row_height);
    for (std::size_t i = visible.first; i < visible.last; ++i) {
        const auto& line = *shown[i];
        const float y = visible.top + static_cast<float>(i - visible.first) * row_height;
        const ui::Rect row{content.x0, y, content.x1, y + row_height};
        const bool error = line.find("[error]") != std::string::npos;
        const bool warn = line.find("[warn ]") != std::string::npos;
        const auto body_at = line.find("] ", line.find("] ") + 2);
        const std::string_view stamp = body_at != std::string::npos
                                           ? std::string_view(line).substr(1, 8) : std::string_view{};
        const std::string_view message = body_at != std::string::npos
                                             ? std::string_view(line).substr(body_at + 2) : std::string_view(line);
        const float ty = middle(row);
        const float gutter = snap(ui::font::measure("00:00:00") + 12.0f);
        if (error || warn) {
            list.rect({row.x0 + kPad - 6.0f, y + 5.0f, row.x0 + kPad - 4.0f, y + row_height - 5.0f},
                      error ? t.bad : t.warn);
        }
        text(row.x0 + kPad, ty, t.text_faint, stamp, gutter);
        text(row.x0 + kPad + gutter, ty, error ? t.bad : warn ? t.warn : t.text_dim, message,
             row.width() - kPad * 2.0f - gutter);
    }
    list.pop_clip();
}

std::size_t type_count() {
    static std::size_t cached = 0;
    if (cached == 0) {
        cached = decima::type_names().size();
    }
    return cached;
}

void draw_status(const ui::Rect& area) {
    const auto& t = ui::theme();
    auto& list = ui::draw();
    list.rect(area, t.window_low);
    list.rect({area.x0, area.y0, area.x1, area.y0 + 1.0f}, t.hairline);
    const ui::font::ScopedFace micro(ui::font::Face::Micro);
    const float y = middle(area);
    float x = area.x0 + 16.0f;
    const auto item = [&](const std::string& value, ui::Color color) {
        text(x, y, color, value, area.x1 - x);
        x += snap(ui::font::measure(value)) + 20.0f;
    };
    const bool scanning = globals::running();
    list.circle({x + 3.0f, snap(area.center().y)}, 3.0f, scanning ? t.warn : t.good);
    x += 14.0f;
    item(scanning ? std::format("Scanning exports {}/{}", globals::examined(), globals::total())
                  : std::format("{} globals", globals::snapshot().size()),
         t.text_dim);
    item(std::format("{} types", type_count()), t.text_faint);
    item(std::format("{} exports", decima::indexed_symbols()), t.text_faint);
    item(std::format("ds.exe {:#x}", g_game.base), t.text_faint);

    const auto right = std::format("{:.1f} ms     F2 in game to hide", g_frame_time * 1000.0);
    const float width = ui::font::measure(right);
    text(area.x1 - 16.0f - width, y, t.text_faint, right, width + 1.0f);
}

void handle_shortcuts() {
    const auto& in = ui::input::state();
    if (ui::editing()) {
        return;
    }
    if (in.pressed_key(VK_BACK) || (in.alt && in.pressed_key(VK_LEFT))) {
        go_back();
    }
    if (in.alt && in.pressed_key(VK_RIGHT)) {
        go_forward();
    }
    if (in.pressed_key(VK_F5)) {
        g_dirty = true;
    }
}

void draw_frame(float width, float height, double now) {
    const bool refresh = g_dirty || (g_live && now - g_last_refresh > 0.1);
    if (refresh) {
        refresh_rows();
        for (auto& watch : g_watches) {
            std::uintptr_t follow = 0;
            auto value = model::format_value(watch.type, watch.address, follow);
            if (!watch.value.empty() && value != watch.value) {
                watch.changed_at = now;
            }
            watch.value = std::move(value);
        }
        g_last_refresh = now;
        g_dirty = false;
    }

    handle_shortcuts();

    const auto& t = ui::theme();
    const float left = snap(std::clamp(width * 0.21f, 260.0f, 360.0f));
    const float right = snap(std::clamp(width * 0.27f, 320.0f, 480.0f));
    const float top = kBar;
    const float bottom = height - kStatus;
    const float middle_x0 = left + 1.0f;
    const float middle_x1 = width - right - 1.0f;
    const float split = snap(top + (bottom - top) * 0.60f);
    const float watch_split = snap(top + (bottom - top) * 0.36f);

    ui::draw().rect({0.0f, 0.0f, width, height}, t.hairline);
    draw_sources({0.0f, top, left, bottom});
    draw_object({middle_x0, top, middle_x1, split});
    draw_memory({middle_x0, split + 1.0f, middle_x1, bottom});
    draw_watches({middle_x1 + 1.0f, top, width, watch_split});
    draw_log({middle_x1 + 1.0f, watch_split + 1.0f, width, bottom});
    draw_status({0.0f, bottom, width, height});
    draw_title_bar({0.0f, 0.0f, width, top});
}

struct Dwm {
    struct Margins {
        int left;
        int right;
        int top;
        int bottom;
    };
    HRESULT(WINAPI* extend)(HWND, const Margins*) = nullptr;
    HRESULT(WINAPI* attribute)(HWND, DWORD, LPCVOID, DWORD) = nullptr;
};

Dwm dwm() {
    Dwm out;
    if (const HMODULE module = LoadLibraryW(L"dwmapi.dll"); module != nullptr) {
        out.extend = reinterpret_cast<decltype(out.extend)>(
            GetProcAddress(module, "DwmExtendFrameIntoClientArea"));
        out.attribute = reinterpret_cast<decltype(out.attribute)>(
            GetProcAddress(module, "DwmSetWindowAttribute"));
    }
    return out;
}

void style_frame(HWND hwnd) {
    const auto api = dwm();
    if (api.extend != nullptr) {
        const Dwm::Margins margins{0, 0, 1, 0};
        api.extend(hwnd, &margins);
    }
    if (api.attribute != nullptr) {
        const BOOL dark = TRUE;
        api.attribute(hwnd, 20  , &dark, sizeof dark);
        const int round = 2  ;
        api.attribute(hwnd, 33  , &round, sizeof round);
        const COLORREF border = RGB(46, 52, 60);
        api.attribute(hwnd, 34  , &border, sizeof border);
    }
}

void save_placement(HWND hwnd) {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof placement;
    if (GetWindowPlacement(hwnd, &placement) == 0) {
        return;
    }
    const auto& r = placement.rcNormalPosition;
    settings::set_number(kSettings, "debugger.x", r.left);
    settings::set_number(kSettings, "debugger.y", r.top);
    settings::set_number(kSettings, "debugger.width", r.right - r.left);
    settings::set_number(kSettings, "debugger.height", r.bottom - r.top);
    settings::set_bool(kSettings, "debugger.maximized", placement.showCmd == SW_SHOWMAXIMIZED);
    settings::set_bool(kSettings, "debugger.visible", IsWindowVisible(hwnd) != 0);
}

double g_last_frame = 0.0;
bool g_rendering = false;

void render(HWND hwnd) {
    if (g_rendering) {
        return;
    }
    g_rendering = true;
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        g_rendering = false;
        return;
    }
    if (width != g_width || height != g_height) {
        g_width = width;
        g_height = height;
        g_pixels.assign(static_cast<std::size_t>(width) * height, 0);
    }
    g_maximized = IsZoomed(hwnd) != 0;

    const double started = now_seconds();
    const float dt = g_last_frame > 0.0 ? static_cast<float>(started - g_last_frame) : 1.0f / 60.0f;
    g_last_frame = started;

    ui::begin_frame({static_cast<float>(width), static_cast<float>(height)}, dt);
    draw_frame(static_cast<float>(width), static_cast<float>(height), started);
    ui::end_frame();

    ui::raster::clear(g_pixels.data(), width, height, ui::theme().window);
    ui::raster::render(ui::draw(), ui::font::atlas(), ui::font::atlas_size(), g_pixels.data(),
                       width, height);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const HDC dc = GetDC(hwnd);
    SetDIBitsToDevice(dc, 0, 0, width, height, 0, 0, 0, height, g_pixels.data(), &info,
                      DIB_RGB_COLORS);
    ReleaseDC(hwnd, dc);
    g_frame_time = now_seconds() - started;

    std::filesystem::path capture_to;
    {
        std::scoped_lock lock(g_capture_mutex);
        capture_to.swap(g_capture);
    }
    if (!capture_to.empty()) {
        BITMAPFILEHEADER file{};
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + static_cast<DWORD>(g_pixels.size() * 4);
        std::ofstream out(capture_to, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&file), sizeof file);
        out.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof info.bmiHeader);
        out.write(reinterpret_cast<const char*>(g_pixels.data()),
                  static_cast<std::streamsize>(g_pixels.size() * 4));
    }

    const auto action = g_window_action;
    g_window_action = WindowAction::None;
    g_rendering = false;
    switch (action) {
        case WindowAction::Minimize:
            ShowWindow(hwnd, SW_MINIMIZE);
            break;
        case WindowAction::Maximize:
            ShowWindow(hwnd, IsZoomed(hwnd) != 0 ? SW_RESTORE : SW_MAXIMIZE);
            break;
        case WindowAction::Close:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        default:
            break;
    }
}

LRESULT hit_test(HWND hwnd, LPARAM lparam) {
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ScreenToClient(hwnd, &point);
    RECT client{};
    GetClientRect(hwnd, &client);
    const ui::Vec2 at{static_cast<float>(point.x), static_cast<float>(point.y)};

    if (IsZoomed(hwnd) == 0) {
        const int edge = 6;
        const bool left = point.x < edge;
        const bool right = point.x >= client.right - edge;
        const bool top = point.y < edge;
        const bool bottom = point.y >= client.bottom - edge;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    if (at.y < kBar) {
        if (g_maximize_button.contains(at)) {
            return HTMAXBUTTON;
        }
        for (const auto& control : g_bar_controls) {
            if (control.contains(at)) {
                return HTCLIENT;
            }
        }
        return HTCAPTION;
    }
    return HTCLIENT;
}

void mouse_at_screen(HWND hwnd, LPARAM lparam) {
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ScreenToClient(hwnd, &point);
    ui::input::on_mouse_move(static_cast<float>(point.x), static_cast<float>(point.y));
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    const auto input_arrived = [] { g_last_input = now_seconds(); };
    switch (message) {
        case WM_NCCALCSIZE:
            if (wparam == TRUE) {
                if (IsZoomed(hwnd) != 0) {
                    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
                    const int frame = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                    const int frame_y = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                    params->rgrc[0].left += frame;
                    params->rgrc[0].right -= frame;
                    params->rgrc[0].top += frame_y;
                    params->rgrc[0].bottom -= frame_y;
                }
                return 0;
            }
            break;
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd, message, wparam, -1);
        case WM_NCHITTEST:
            return hit_test(hwnd, lparam);
        case WM_NCMOUSEMOVE:
            if (!g_tracking_leave) {
                TRACKMOUSEEVENT track{sizeof track, TME_LEAVE | TME_NONCLIENT, hwnd, 0};
                g_tracking_leave = TrackMouseEvent(&track) != 0;
            }
            mouse_at_screen(hwnd, lparam);
            input_arrived();
            break;
        case WM_NCMOUSELEAVE:
            g_tracking_leave = false;
            g_maximize_pressed = false;
            ui::input::on_mouse_move(-10000.0f, -10000.0f);
            input_arrived();
            break;
        case WM_NCLBUTTONDOWN:
            if (wparam == HTMAXBUTTON) {
                g_maximize_pressed = true;
                return 0;
            }
            break;
        case WM_NCLBUTTONUP:
            if (wparam == HTMAXBUTTON) {
                if (g_maximize_pressed) {
                    ShowWindow(hwnd, IsZoomed(hwnd) != 0 ? SW_RESTORE : SW_MAXIMIZE);
                }
                g_maximize_pressed = false;
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (!g_tracking_leave) {
                TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, hwnd, 0};
                g_tracking_leave = TrackMouseEvent(&track) != 0;
            }
            ui::input::on_mouse_move(static_cast<float>(GET_X_LPARAM(lparam)),
                                     static_cast<float>(GET_Y_LPARAM(lparam)));
            input_arrived();
            return 0;
        case WM_MOUSELEAVE:
            g_tracking_leave = false;
            ui::input::on_mouse_move(-10000.0f, -10000.0f);
            input_arrived();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetCapture(hwnd);
            ui::input::on_mouse_button(true);
            input_arrived();
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            ui::input::on_mouse_button(false);
            input_arrived();
            return 0;
        case WM_RBUTTONDOWN:
            ui::input::on_right_button(true);
            input_arrived();
            return 0;
        case WM_MOUSEWHEEL:
            ui::input::on_wheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA);
            input_arrived();
            return 0;
        case WM_CHAR:
            ui::input::on_character(static_cast<char>(wparam));
            input_arrived();
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            ui::input::on_key(static_cast<std::uint32_t>(wparam), true);
            input_arrived();
            if (message == WM_SYSKEYDOWN && wparam != VK_F4 && wparam != VK_SPACE) {
                return 0;
            }
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            ui::input::on_key(static_cast<std::uint32_t>(wparam), false);
            input_arrived();
            if (message == WM_SYSKEYUP) {
                return 0;
            }
            break;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = {1040, 640};
            return 0;
        }
        case WM_SIZE:
        case WM_PAINT:
            if (message == WM_PAINT) {
                PAINTSTRUCT paint{};
                BeginPaint(hwnd, &paint);
                EndPaint(hwnd, &paint);
            }
            if (IsWindowVisible(hwnd) != 0 && IsIconic(hwnd) == 0) {
                render(hwnd);
            }
            input_arrived();
            return 0;
        case WM_EXITSIZEMOVE:
            save_placement(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            save_placement(hwnd);
            return 0;
        case kToggleMessage:
            if (IsWindowVisible(hwnd) != 0) {
                ShowWindow(hwnd, SW_HIDE);
            } else {
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                input_arrived();
            }
            save_placement(hwnd);
            return 0;
        case kCaptureMessage:
            input_arrived();
            return 0;
        case kOpenMessage:
            if (const auto address = g_open_request.exchange(0); address != 0) {
                navigate_address(address);
                if (IsWindowVisible(hwnd) == 0) {
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                }
                if (IsIconic(hwnd) != 0) {
                    ShowWindow(hwnd, SW_RESTORE);
                }
                SetForegroundWindow(hwnd);
                save_placement(hwnd);
            }
            input_arrived();
            return 0;
        case kStopMessage:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void run() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_ui = ui::create_context();
    g_input = ui::input::create_context();
    ui::set_context(g_ui);
    ui::input::set_context(g_input);
    ui::input::on_mouse_move(-10000.0f, -10000.0f);

    auto theme = ui::theme();
    theme.row_height = 24.0f;
    theme.spacing = 6.0f;
    theme.padding = 12.0f;
    ui::set_theme(theme);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof cls;
    cls.style = CS_DBLCLKS;
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    cls.hbrBackground = CreateSolidBrush(RGB(15, 17, 20));
    cls.lpszClassName = kClassName;
    RegisterClassExW(&cls);

    const int x = static_cast<int>(settings::get_number(kSettings, "debugger.x", CW_USEDEFAULT));
    const int y = static_cast<int>(settings::get_number(kSettings, "debugger.y", CW_USEDEFAULT));
    const int w = static_cast<int>(settings::get_number(kSettings, "debugger.width", 1560));
    const int h = static_cast<int>(settings::get_number(kSettings, "debugger.height", 940));
    const bool visible = settings::get_bool(kSettings, "debugger.visible", true);
    const bool maximized = settings::get_bool(kSettings, "debugger.maximized", false);

    const HWND hwnd = CreateWindowExW(0, kClassName, L"Bridger Debugger", WS_OVERLAPPEDWINDOW,
                                      x, y, w, h, nullptr, nullptr, instance, nullptr);
    if (hwnd == nullptr) {
        log::error("debugger: could not create the window ({})", GetLastError());
        g_running.store(false);
        return;
    }
    style_frame(hwnd);
    RECT placed{};
    GetWindowRect(hwnd, &placed);
    if (MonitorFromRect(&placed, MONITOR_DEFAULTTONULL) == nullptr) {
        SetWindowPos(hwnd, nullptr, 80, 80, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_hwnd.store(hwnd);
    if (visible) {
        ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE);
    }
    log::info("debugger window ready, F2 in game shows or hides it");

    globals::scan(g_game);

    MSG msg{};
    bool quit = false;
    while (!quit) {
        const bool active = now_seconds() - g_last_input < 1.0;
        MsgWaitForMultipleObjectsEx(0, nullptr, active ? 16 : 100, QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!quit && IsWindowVisible(hwnd) != 0 && IsIconic(hwnd) == 0) {
            render(hwnd);
        }
    }

    g_hwnd.store(nullptr);
    UnregisterClassW(kClassName, instance);
    ui::set_context(nullptr);
    ui::input::set_context(nullptr);
    ui::destroy_context(g_ui);
    ui::input::destroy_context(g_input);
    g_running.store(false);
}

}

void start(const std::filesystem::path&  , const mem::Module& game) {
    if (g_running.exchange(true)) {
        return;
    }
    g_game = game;
    g_thread = std::thread(run);
}

void toggle() {
    if (const HWND hwnd = g_hwnd.load(); hwnd != nullptr) {
        PostMessageW(hwnd, kToggleMessage, 0, 0);
    }
}

void capture(const std::filesystem::path& file) {
    {
        std::scoped_lock lock(g_capture_mutex);
        g_capture = file;
    }
    if (const HWND hwnd = g_hwnd.load(); hwnd != nullptr) {
        PostMessageW(hwnd, kCaptureMessage, 0, 0);
    }
}

void open(std::uintptr_t address) {
    g_open_request.store(address);
    if (const HWND hwnd = g_hwnd.load(); hwnd != nullptr) {
        PostMessageW(hwnd, kOpenMessage, 0, 0);
    }
}

void stop() {
    if (const HWND hwnd = g_hwnd.load(); hwnd != nullptr) {
        PostMessageW(hwnd, kStopMessage, 0, 0);
    }
    if (g_thread.joinable()) {
        const HANDLE handle = static_cast<HANDLE>(g_thread.native_handle());
        if (WaitForSingleObject(handle, 1000) == WAIT_OBJECT_0) {
            g_thread.join();
        } else {
            g_thread.detach();
        }
    }
}

}
