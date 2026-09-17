#include "ui/ui.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "ui/font.h"
#include "ui/input.h"

namespace bridger::ui {
namespace {

constexpr float kHelpDelay = 0.45f;

struct Layout {
    Rect bounds;
    float cursor_x = 0.0f;
    float cursor_y = 0.0f;
    float line_height = 0.0f;
    float indent = 0.0f;
    float last_line = 0.0f;
    float last_end_x = 0.0f;
    bool scrolling = false;
    std::string id;
    float start_y = 0.0f;
};

struct Scroll {
    float offset = 0.0f;
    float target = 0.0f;
    float content = 0.0f;
    float bar = 0.0f;
};

struct Edit {
    int caret = 0;
    int anchor = 0;
    float scroll = 0.0f;
};

struct GroupFrame {
    std::string key;
    float start_y = 0.0f;
    bool indented = false;
};

struct Group {
    float open = 1.0f;
    bool expanded = true;
    int matches = 0;
    int counted = 0;
};

}

struct Context {
    DrawList list;
    Theme theme;
    Vec2 display;
    float dt = 0.0f;
    std::vector<Layout> layouts;
    std::unordered_map<std::string, Scroll> scrolls;
    std::unordered_map<std::string, Group> groups;
    std::unordered_map<std::uint32_t, float> anim;

    std::string focus;
    std::string focus_request;
    Edit edit;
    std::string numeric;
    std::string numeric_buffer;

    std::string popup;
    Rect popup_rect;
    Rect popup_anchor;
    bool popup_seen = false;

    std::string capture;

    std::uint32_t hot = 0;
    std::uint32_t active = 0;
    std::uint32_t next_hot = 0;
    std::uint32_t hover_id = 0;
    float hover_time = 0.0f;

    bool dragging = false;
    Vec2 drag_offset;
    float row_height = 0.0f;
    float row_x = 0.0f;
    float row_y = 0.0f;

    std::string filter;
    std::vector<float> settings;
    std::vector<GroupFrame> group_stack;
    std::uint32_t id_seed = 0;
    std::vector<std::uint32_t> id_stack;
    Rect last_gutter;
    bool last_row_hovered = false;
    std::uint32_t last_row_id = 0;

    Rect body;
    std::string status;
    float chrome_y0 = 0.0f;
    float tabs_start = 0.0f;
    float tabs_cursor = 0.0f;
    float tabs_limit = 0.0f;
    float tabs_end = 0.0f;

    std::vector<std::uint32_t> spans;
    std::string tooltip_text;
    Vec2 tooltip_at;
};

namespace {

Context g_default_context;
thread_local Context* t_context = &g_default_context;

Context& ctx() {
    return *t_context;
}

std::uint32_t hash_bytes(std::string_view value, std::uint32_t seed) {
    std::uint32_t hash = seed == 0 ? 2166136261u : seed;
    for (const char c : value) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 16777619u;
    }
    return hash == 0 ? 1u : hash;
}

std::uint32_t hash_id(std::string_view value) {
    std::uint32_t hash = hash_bytes(value, ctx().id_seed);
    hash ^= static_cast<std::uint32_t>(ctx().layouts.size()) * 2654435761u;
    return hash == 0 ? 1u : hash;
}

std::string scoped_key(std::string_view value) {
    std::string key;
    for (const auto& frame : ctx().group_stack) {
        key += frame.key;
        key.push_back('/');
    }
    key += value;
    return key;
}

Layout& layout() {
    return ctx().layouts.back();
}

float smooth(float current, float target, float speed) {
    const float blend = 1.0f - std::exp(-speed * ctx().dt);
    const float next = current + (target - current) * blend;
    return std::abs(target - next) < 0.001f ? target : next;
}

float animate(std::uint32_t id, float target, float speed) {
    float& value = ctx().anim[id];
    value = smooth(value, target, speed);
    return value;
}

bool hovering(const Rect& area) {
    const auto& in = input::state();
    if (!area.contains(in.mouse)) {
        return false;
    }
    if (!ctx().popup.empty() && ctx().list.layer() < kLayerPopup
            && ctx().popup_rect.contains(in.mouse)) {
        return false;
    }
    return ctx().list.clip().contains(in.mouse);
}

Rect claim(float width, float height) {
    Layout& current = layout();
    const float x = current.cursor_x;
    const float y = current.cursor_y;
    const float w = width > 0.0f ? width : (current.bounds.x1 - x);
    current.line_height = std::max(current.line_height, height);
    current.cursor_x = x + w + ctx().theme.spacing;
    return {x, y, x + w, y + height};
}

float center_y(const Rect& area) {
    return area.y0 + (area.height() - font::line_height()) * 0.5f;
}

void text_at(const Rect& area, Color color, std::string_view value, bool centered) {
    const float text_width = font::measure(value);
    const float x = centered ? area.x0 + (area.width() - text_width) * 0.5f : area.x0;
    ctx().list.text_clipped({x, center_y(area)}, color, value, area.width());
}

bool press(std::uint32_t id, const Rect& area) {
    const auto& in = input::state();
    bool result = false;
    if (hovering(area)) {
        ctx().next_hot = id;
        if (in.pressed) {
            ctx().active = id;
        }
    }
    if (ctx().active == id && in.released) {
        if (hovering(area)) {
            result = true;
        }
        ctx().active = 0;
    }
    return result;
}

std::string upper(std::string_view value) {
    std::string out(value);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

float settings_label_width() {
    return ctx().settings.empty() ? ctx().theme.label_width : ctx().settings.back();
}

int caret_from_x(std::string_view value, float origin, float x) {
    float pen = origin;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const font::Glyph* glyph = font::lookup(static_cast<unsigned char>(value[i]));
        const float advance = glyph != nullptr ? glyph->advance : 0.0f;
        if (x < pen + advance * 0.5f) {
            return static_cast<int>(i);
        }
        pen += advance;
    }
    return static_cast<int>(value.size());
}

float measure_prefix(std::string_view value, int count) {
    return font::measure(value.substr(0, static_cast<std::size_t>(
        std::clamp(count, 0, static_cast<int>(value.size())))));
}

int word_boundary(std::string_view value, int from, int direction) {
    int index = from;
    const auto size = static_cast<int>(value.size());
    const auto is_word = [&](int at) {
        return at >= 0 && at < size
            && (std::isalnum(static_cast<unsigned char>(value[static_cast<std::size_t>(at)])) != 0
                || value[static_cast<std::size_t>(at)] == '_');
    };
    if (direction < 0) {
        while (index > 0 && !is_word(index - 1)) {
            --index;
        }
        while (index > 0 && is_word(index - 1)) {
            --index;
        }
    } else {
        while (index < size && !is_word(index)) {
            ++index;
        }
        while (index < size && is_word(index)) {
            ++index;
        }
    }
    return index;
}

struct FieldResult {
    bool changed = false;
    bool committed = false;
    bool cancelled = false;
};

FieldResult edit_field(std::string& value, const Rect& area, std::string_view placeholder,
                       bool focused, Color text_color, Color hint_color) {
    const Theme& t = ctx().theme;
    FieldResult result;
    Edit& edit = ctx().edit;

    if (!focused) {
        ctx().list.push_clip(area);
        ctx().list.text({area.x0, center_y(area)}, value.empty() ? hint_color : text_color,
                        value.empty() ? placeholder : value);
        ctx().list.pop_clip();
        return result;
    }

    const auto& in = input::state();
    const auto size = static_cast<int>(value.size());
    edit.caret = std::clamp(edit.caret, 0, size);
    edit.anchor = std::clamp(edit.anchor, 0, size);

    if (in.pressed && area.expand(4.0f).contains(in.mouse)) {
        edit.caret = caret_from_x(value, area.x0 - edit.scroll, in.mouse.x);
        edit.anchor = edit.caret;
    } else if (in.down && ctx().active != 0 && area.expand(4.0f).contains(in.mouse)) {
        edit.caret = caret_from_x(value, area.x0 - edit.scroll, in.mouse.x);
    }
    if (in.double_clicked && area.expand(4.0f).contains(in.mouse)) {
        edit.anchor = 0;
        edit.caret = size;
    }

    const auto selection_low = [&] { return std::min(edit.caret, edit.anchor); };
    const auto selection_high = [&] { return std::max(edit.caret, edit.anchor); };
    const auto erase_selection = [&] {
        if (edit.caret == edit.anchor) {
            return false;
        }
        const int low = selection_low();
        value.erase(static_cast<std::size_t>(low),
                    static_cast<std::size_t>(selection_high() - low));
        edit.caret = low;
        edit.anchor = low;
        return true;
    };

    const bool shift = in.shift;
    const auto move_to = [&](int position) {
        edit.caret = std::clamp(position, 0, static_cast<int>(value.size()));
        if (!shift) {
            edit.anchor = edit.caret;
        }
    };

    if (in.ctrl && in.pressed_key('A')) {
        edit.anchor = 0;
        edit.caret = static_cast<int>(value.size());
    }
    if (in.ctrl && (in.pressed_key('C') || in.pressed_key('X')) && edit.caret != edit.anchor) {
        const int low = selection_low();
        input::set_clipboard(std::string_view(value).substr(
            static_cast<std::size_t>(low), static_cast<std::size_t>(selection_high() - low)));
        if (in.pressed_key('X')) {
            result.changed = erase_selection();
        }
    }
    if (in.ctrl && in.pressed_key('V')) {
        const std::string pasted = input::clipboard();
        if (!pasted.empty()) {
            erase_selection();
            value.insert(static_cast<std::size_t>(edit.caret), pasted);
            edit.caret += static_cast<int>(pasted.size());
            edit.anchor = edit.caret;
            result.changed = true;
        }
    }

    if (!in.ctrl && !in.characters.empty()) {
        erase_selection();
        value.insert(static_cast<std::size_t>(edit.caret), in.characters);
        edit.caret += static_cast<int>(in.characters.size());
        edit.anchor = edit.caret;
        result.changed = true;
    }

    if (in.pressed_key(VK_BACK)) {
        if (!erase_selection() && edit.caret > 0) {
            const int to = in.ctrl ? word_boundary(value, edit.caret, -1) : edit.caret - 1;
            value.erase(static_cast<std::size_t>(to), static_cast<std::size_t>(edit.caret - to));
            edit.caret = to;
            edit.anchor = to;
        }
        result.changed = true;
    }
    if (in.pressed_key(VK_DELETE)) {
        if (!erase_selection() && edit.caret < static_cast<int>(value.size())) {
            const int to = in.ctrl ? word_boundary(value, edit.caret, 1) : edit.caret + 1;
            value.erase(static_cast<std::size_t>(edit.caret),
                        static_cast<std::size_t>(to - edit.caret));
        }
        result.changed = true;
    }

    if (in.pressed_key(VK_LEFT)) {
        move_to(in.ctrl ? word_boundary(value, edit.caret, -1) : edit.caret - 1);
    }
    if (in.pressed_key(VK_RIGHT)) {
        move_to(in.ctrl ? word_boundary(value, edit.caret, 1) : edit.caret + 1);
    }
    if (in.pressed_key(VK_HOME)) {
        move_to(0);
    }
    if (in.pressed_key(VK_END)) {
        move_to(static_cast<int>(value.size()));
    }
    if (in.pressed_key(VK_RETURN)) {
        result.committed = true;
    }
    if (in.pressed_key(VK_ESCAPE)) {
        result.cancelled = true;
    }

    edit.caret = std::clamp(edit.caret, 0, static_cast<int>(value.size()));
    edit.anchor = std::clamp(edit.anchor, 0, static_cast<int>(value.size()));

    const float caret_x = measure_prefix(value, edit.caret);
    if (caret_x - edit.scroll > area.width() - 2.0f) {
        edit.scroll = caret_x - area.width() + 2.0f;
    }
    if (caret_x - edit.scroll < 0.0f) {
        edit.scroll = caret_x;
    }
    edit.scroll = std::max(0.0f, std::min(edit.scroll,
                                          std::max(0.0f, font::measure(value) - area.width() + 2.0f)));

    ctx().list.push_clip(area);
    const float origin = area.x0 - edit.scroll;
    if (edit.caret != edit.anchor) {
        const float from = origin + measure_prefix(value, selection_low());
        const float to = origin + measure_prefix(value, selection_high());
        ctx().list.rect({from, area.y0 + 1.0f, to, area.y1 - 1.0f}, t.accent_dim);
    }
    if (value.empty() && !placeholder.empty()) {
        ctx().list.text({origin, center_y(area)}, hint_color, placeholder);
    } else {
        ctx().list.text({origin, center_y(area)}, text_color, value);
    }
    const float blink = std::fmod(static_cast<float>(GetTickCount64()) * 0.001f, 1.06f);
    if (blink < 0.62f) {
        const float x = origin + caret_x;
        ctx().list.rect({x, area.y0 + 1.0f, x + 1.0f, area.y1 - 1.0f}, t.accent);
    }
    ctx().list.pop_clip();
    return result;
}

struct Row {
    Rect area;
    Rect control;
    bool visible = false;
    bool hovered = false;
};

void queue_tooltip(std::string_view help) {
    if (help.empty()) {
        return;
    }
    ctx().tooltip_text = std::string(help);
    ctx().tooltip_at = input::state().mouse;
}

Row begin_setting_row(std::string_view label, std::string_view help, float height) {
    const Theme& t = ctx().theme;
    Row row;

    if (!ctx().filter.empty() && !fuzzy(label, ctx().filter, &ctx().spans)) {
        return row;
    }
    for (const auto& frame : ctx().group_stack) {
        ++ctx().groups[frame.key].counted;
    }

    Layout& current = layout();
    row.area = {current.bounds.x0 + current.indent, current.cursor_y, current.bounds.x1,
                current.cursor_y + height};
    current.cursor_y -= ctx().theme.spacing;
    row.visible = true;
    row.hovered = hovering(row.area);

    const auto id = hash_id(label) ^ 0x5bd1e995u;
    if (row.hovered) {
        if (ctx().hover_id != id) {
            ctx().hover_id = id;
            ctx().hover_time = 0.0f;
        } else {
            ctx().hover_time += ctx().dt;
            if (ctx().hover_time > kHelpDelay) {
                queue_tooltip(help);
            }
        }
    }

    const float lit = animate(id, row.hovered ? 1.0f : 0.0f, 14.0f);
    if (lit > 0.01f) {
        ctx().list.rect(row.area, with_alpha(t.wash, lit * 0.03f));
    }
    ctx().list.rect({row.area.x0, row.area.y1 - 1.0f, row.area.x1, row.area.y1},
                    with_alpha(t.hairline, 0.7f));

    const float label_width = settings_label_width();
    {
        const font::ScopedFace strong(font::Face::Strong);
        const Color tone = mix(t.text_dim, t.text, 0.35f + lit * 0.65f);
        if (ctx().filter.empty()) {
            ctx().list.text_clipped({row.area.x0 + 12.0f, center_y(row.area)}, tone, label,
                                    label_width - 12.0f);
        } else {
            ctx().list.text_spans({row.area.x0 + 12.0f, center_y(row.area)}, tone, t.accent, label,
                                  ctx().spans.data(), static_cast<int>(ctx().spans.size()),
                                  label_width - 12.0f);
        }
        if (!help.empty() && lit > 0.02f) {
            const float mark_x = row.area.x0 + 12.0f + std::min(label_width - 16.0f,
                                                        font::measure(label)) + 7.0f;
            ctx().list.circle({mark_x + 2.0f, row.area.center().y}, 1.8f,
                              with_alpha(t.text_faint, lit * 0.85f));
        }
    }

    row.control = {row.area.x0 + label_width, row.area.y0, row.area.x1 - t.gutter, row.area.y1};
    ctx().last_gutter = {row.area.x1 - t.gutter + 4.0f, row.area.y0, row.area.x1, row.area.y1};
    ctx().last_row_hovered = row.hovered;
    ctx().last_row_id = id;
    current.line_height = std::max(current.line_height, height);
    return row;
}

void draw_chevron(Vec2 center, float size, Color color, float openness) {
    const float arm = -0.5f * size + openness * 0.85f * size;
    const float elbow = 0.5f * size - openness * 0.95f * size;
    const Vec2 points[3] = {
        {center.x - size, center.y + arm},
        {center.x, center.y + elbow},
        {center.x + size, center.y + arm},
    };
    ctx().list.polyline(points, 3, color, 1.7f, false);
}

}

void newline() {
    Layout& current = ctx().layouts.back();
    if (current.line_height <= 0.0f) {
        current.line_height = ctx().theme.row_height;
    }
    current.last_line = current.line_height;
    current.last_end_x = current.cursor_x;
    current.cursor_y += current.line_height + ctx().theme.spacing;
    current.cursor_x = current.bounds.x0 + current.indent;
    current.line_height = 0.0f;
}

float remaining_height() {
    const Layout& current = ctx().layouts.back();
    return std::max(0.0f, current.bounds.y1 - current.cursor_y);
}

void tracked(Color color, std::string_view value, float size_tracking) {
    const font::ScopedFace caption(font::Face::Caption);
    const Rect area = claim(0.0f, font::line_height());
    ctx().list.text_tracked({area.x0, area.y0}, color, value, size_tracking);
    newline();
}

void header(std::string_view label) {
    const Theme& t = ctx().theme;
    const font::ScopedFace caption(font::Face::Caption);
    const std::string text_value = upper(label);
    Layout& current = ctx().layouts.back();
    const float y = current.cursor_y;
    ctx().list.text_tracked({current.cursor_x, y}, t.text_faint, text_value, t.tracking);
    const float width = font::measure_tracked(text_value, t.tracking);
    const float line_y = y + font::line_height() * 0.5f;
    ctx().list.rect({current.cursor_x + width + 12.0f, line_y, current.bounds.x1, line_y + 1.0f},
                    t.hairline);
    current.line_height = font::line_height();
    newline();
}

void rule() {
    const Theme& t = ctx().theme;
    Layout& current = ctx().layouts.back();
    const float y = current.cursor_y + 2.0f;
    ctx().list.rect({current.bounds.x0, y, current.bounds.x1, y + 1.0f}, t.hairline);
    current.line_height = 5.0f;
    newline();
}

Context* create_context() {
    return new Context();
}

void destroy_context(Context* context) {
    if (context != &g_default_context) {
        delete context;
    }
}

void set_context(Context* context) {
    t_context = context != nullptr ? context : &g_default_context;
}

const Theme& theme() {
    return ctx().theme;
}

DrawList& draw() {
    return ctx().list;
}

void begin_frame(Vec2 display, float delta_seconds) {
    input::begin_frame();
    ctx().dt = std::clamp(delta_seconds, 0.0f, 0.1f);
    ctx().display = display;
    ctx().list.reset(display);
    ctx().layouts.clear();
    ctx().hot = ctx().next_hot;
    ctx().next_hot = 0;
    ctx().settings.clear();
    ctx().group_stack.clear();
    ctx().id_seed = 0;
    ctx().id_stack.clear();
    ctx().popup_seen = false;
    ctx().tooltip_text.clear();
    for (auto& [key, group] : ctx().groups) {
        group.matches = group.counted;
        group.counted = 0;
    }
}

void end_frame() {
    const auto& in = input::state();
    if (!in.down) {
        ctx().active = 0;
    }

    if (!ctx().popup.empty()) {
        const bool outside = in.pressed && !ctx().popup_rect.contains(in.mouse)
                          && !ctx().popup_anchor.contains(in.mouse);
        if (outside || in.pressed_key(VK_ESCAPE) || !ctx().popup_seen) {
            ctx().popup.clear();
        }
    }

    if (!ctx().tooltip_text.empty()) {
        const Theme& t = ctx().theme;
        const font::ScopedFace body(font::Face::Micro);
        const float width = font::measure(ctx().tooltip_text) + 18.0f;
        const float height = font::line_height() + 12.0f;
        float x = ctx().tooltip_at.x + 16.0f;
        float y = ctx().tooltip_at.y + 20.0f;
        x = std::min(x, ctx().display.x - width - 6.0f);
        y = std::min(y, ctx().display.y - height - 6.0f);
        const Rect box{x, y, x + width, y + height};
        const int previous = ctx().list.layer();
        ctx().list.set_layer(kLayerTooltip);
        ctx().list.rect(box, t.chrome);
        ctx().list.rect_outline(box, t.border, 1.0f);
        ctx().list.rect({box.x0, box.y0, box.x0 + 2.0f, box.y1}, t.text_faint);
        ctx().list.text({box.x0 + 9.0f, center_y(box)}, t.text, ctx().tooltip_text);
        ctx().list.set_layer(previous);
    }

    ctx().list.finish();
    input::end_frame();
}

bool begin_window(std::string_view title, Rect& area, bool& open) {
    const Theme& t = ctx().theme;
    const float chrome = t.chrome_height;
    const bool has_status = !ctx().status.empty();

    const auto& in = input::state();
    const auto drag_id = hash_id("##window_drag");

    const Rect grab_title{area.x0, area.y0, ctx().tabs_start, area.y0 + chrome};
    const Rect grab_rest{std::max(ctx().tabs_end, ctx().tabs_start), area.y0, area.x1 - chrome,
                         area.y0 + chrome};
    if ((hovering(grab_title) || hovering(grab_rest)) && in.pressed) {
        ctx().dragging = true;
        ctx().drag_offset = {in.mouse.x - area.x0, in.mouse.y - area.y0};
        ctx().active = drag_id;
    }
    if (ctx().dragging && !in.down) {
        ctx().dragging = false;
    }
    if (ctx().dragging) {
        const float width = area.width();
        const float height = area.height();
        area.x0 = std::clamp(in.mouse.x - ctx().drag_offset.x, 0.0f, ctx().display.x - width);
        area.y0 = std::clamp(in.mouse.y - ctx().drag_offset.y, 0.0f, ctx().display.y - height);
        area.x1 = area.x0 + width;
        area.y1 = area.y0 + height;
    }

    ctx().list.rect(area, t.window);
    ctx().list.rect({area.x0, area.y0, area.x1, area.y0 + chrome}, t.chrome);
    ctx().list.rect({area.x0, area.y0 + chrome - 1.0f, area.x1, area.y0 + chrome}, t.hairline);

    {
        const font::ScopedFace title_face(font::Face::Brand);
        const std::string heading = upper(title);
        const float title_y = area.y0 + (chrome - font::line_height()) * 0.5f;
        ctx().list.text_tracked({area.x0 + t.padding, title_y}, t.text, heading, 2.0f);
        const float end = area.x0 + t.padding + font::measure_tracked(heading, 2.0f) + t.padding;
        ctx().list.rect({end, area.y0, end + 1.0f, area.y0 + chrome - 1.0f}, t.hairline);
        ctx().tabs_start = end + 1.0f;
    }
    ctx().tabs_cursor = ctx().tabs_start;
    ctx().tabs_limit = area.x1 - chrome;
    ctx().chrome_y0 = area.y0;

    const Rect close{area.x1 - chrome, area.y0, area.x1, area.y0 + chrome - 1.0f};
    const auto close_id = hash_id("##window_close");
    const bool clicked = press(close_id, close);
    const float lit = animate(close_id, ctx().hot == close_id ? 1.0f : 0.0f, 16.0f);
    ctx().list.rect({close.x0, close.y0, close.x0 + 1.0f, close.y1}, t.hairline);
    if (lit > 0.01f) {
        ctx().list.rect({close.x0 + 1.0f, close.y0, close.x1, close.y1},
                        with_alpha(t.bad, lit * 0.28f));
    }
    const Color mark = mix(t.text_faint, t.text, lit);
    const Vec2 centre = close.center();
    ctx().list.line({centre.x - 5.0f, centre.y - 5.0f}, {centre.x + 5.0f, centre.y + 5.0f}, mark, 1.4f);
    ctx().list.line({centre.x + 5.0f, centre.y - 5.0f}, {centre.x - 5.0f, centre.y + 5.0f}, mark, 1.4f);
    if (clicked) {
        open = false;
    }

    float bottom = area.y1;
    if (has_status) {
        const font::ScopedFace micro(font::Face::Micro);
        const float strip = t.status_height;
        bottom = area.y1 - strip;
        ctx().list.rect({area.x0, bottom, area.x1, area.y1}, t.chrome);
        ctx().list.rect({area.x0, bottom, area.x1, bottom + 1.0f}, t.hairline);
        ctx().list.text_tracked({area.x0 + t.padding, bottom + (strip - font::line_height()) * 0.5f},
                                t.text_faint, ctx().status, 1.4f);
        ctx().status.clear();
    }
    ctx().list.rect_outline(area, t.border, 1.0f);

    ctx().body = {area.x0 + 1.0f, area.y0 + chrome, area.x1 - 1.0f, bottom};

    Layout root;
    root.bounds = {area.x0 + t.padding, area.y0 + chrome + t.padding,
                   area.x1 - t.padding, bottom - t.padding};
    root.cursor_x = root.bounds.x0;
    root.cursor_y = root.bounds.y0;
    root.id = std::string(title);
    ctx().layouts.push_back(root);
    ctx().list.push_clip(area);
    return true;
}

void set_theme(const Theme& theme) {
    ctx().theme = theme;
}

void begin_area(const Rect& area) {
    Layout root;
    root.bounds = area;
    root.cursor_x = area.x0;
    root.cursor_y = area.y0;
    root.id = "area";
    ctx().layouts.push_back(root);
    ctx().list.push_clip(area);
}

void end_area() {
    ctx().layouts.pop_back();
    ctx().list.pop_clip();
}

bool editing() {
    return !ctx().focus.empty();
}

Rect window_body() {
    return ctx().body;
}

void window_status(std::string_view value) {
    ctx().status.assign(value);
}

bool window_tab(std::string_view label, bool selected) {
    const Theme& t = ctx().theme;
    const font::ScopedFace caption(font::Face::Caption);
    const std::string text_value = upper(label);
    const float text_width = font::measure_tracked(text_value, t.tracking);
    const float x0 = ctx().tabs_cursor;
    const float x1 = std::min(x0 + text_width + 30.0f, ctx().tabs_limit);
    ctx().tabs_cursor = x1;
    ctx().tabs_end = x1;
    if (x1 - x0 < 8.0f) {
        return false;
    }
    const Rect area{x0, ctx().chrome_y0, x1, ctx().chrome_y0 + t.chrome_height - 1.0f};
    const auto id = hash_id(label) ^ 0x51ed270bu;
    const bool clicked = press(id, area);
    const float lit = animate(id, ctx().hot == id ? 1.0f : 0.0f, 16.0f);
    const float on = animate(id ^ 0x9E3779B9u, selected ? 1.0f : 0.0f, 15.0f);

    if (on > 0.01f) {
        ctx().list.rect({area.x0, area.y0, area.x1, area.y1 + 1.0f}, with_alpha(t.window, on));
        ctx().list.rect({area.x0, area.y0, area.x1, area.y0 + 2.0f}, with_alpha(t.accent, on));
    } else if (lit > 0.01f) {
        ctx().list.rect(area, with_alpha(t.wash, lit * 0.035f));
    }
    const Color tone = mix(mix(t.text_faint, t.text_dim, lit), t.text, on);
    ctx().list.push_clip(area);
    ctx().list.text_tracked({area.x0 + (area.width() - text_width) * 0.5f, center_y(area)}, tone,
                            text_value, t.tracking);
    ctx().list.pop_clip();
    return clicked;
}

void end_window() {
    ctx().list.pop_clip();
    if (!ctx().layouts.empty()) {
        ctx().layouts.pop_back();
    }
}

float available_width() {
    const Layout& current = layout();
    return current.bounds.x1 - current.cursor_x;
}

Vec2 cursor() {
    const Layout& current = layout();
    return {current.cursor_x, current.cursor_y};
}

void set_cursor_x(float x) {
    layout().cursor_x = layout().bounds.x0 + x;
}

void set_cursor_y(float y) {
    Layout& current = layout();
    current.cursor_y = y;
    current.cursor_x = current.bounds.x0 + current.indent;
    current.line_height = 0.0f;
}

void text(std::string_view value) {
    text_colored(ctx().theme.text, value);
}

void text_colored(Color color, std::string_view value) {
    const Rect area = claim(font::measure(value) + 1.0f, font::line_height());
    ctx().list.text_clipped({area.x0, area.y0}, color, value, area.width());
    newline();
}

float text_wrapped(Color color, std::string_view value) {
    Layout& current = layout();
    const float width = std::max(60.0f, current.bounds.x1 - current.cursor_x);
    const float line = font::line_height();
    float y = current.cursor_y;
    std::size_t start = 0;

    while (start < value.size()) {
        std::size_t end = start;
        std::size_t last_break = std::string_view::npos;
        float pen = 0.0f;
        while (end < value.size()) {
            const font::Glyph* glyph = font::lookup(static_cast<unsigned char>(value[end]));
            const float advance = glyph != nullptr ? glyph->advance : 0.0f;
            if (pen + advance > width && end > start) {
                break;
            }
            if (value[end] == ' ') {
                last_break = end;
            }
            pen += advance;
            ++end;
        }
        std::size_t stop = end;
        if (end < value.size() && last_break != std::string_view::npos && last_break > start) {
            stop = last_break;
        }
        ctx().list.text_clipped({current.cursor_x, y}, color, value.substr(start, stop - start),
                                width);
        y += line + 2.0f;
        start = stop;
        while (start < value.size() && value[start] == ' ') {
            ++start;
        }
    }

    const float height = std::max(line, y - current.cursor_y - 2.0f);
    current.line_height = std::max(current.line_height, height);
    newline();
    return height;
}

void note(std::string_view value) {
    const Theme& t = ctx().theme;
    const font::ScopedFace micro(font::Face::Micro);
    Layout& current = layout();
    current.cursor_x = current.bounds.x0 + current.indent + settings_label_width();
    text_wrapped(t.text_faint, value);
}

void label_value(std::string_view label, std::string_view value) {
    const Theme& t = ctx().theme;
    Layout& current = layout();
    const float x = current.cursor_x;
    const float y = current.cursor_y;
    ctx().list.text_clipped({x, y}, t.text_faint, label, 164.0f);
    ctx().list.text({x + 170.0f, y}, t.text, value);
    current.line_height = font::line_height();
    newline();
}

void separator() {
    const Theme& t = ctx().theme;
    Layout& current = layout();
    const float y = current.cursor_y + 3.0f;
    ctx().list.rect({current.bounds.x0, y, current.bounds.x1, y + 1.0f}, t.border);
    current.line_height = 7.0f;
    newline();
}

void spacing(float amount) {
    Layout& current = layout();
    current.cursor_y += amount > 0.0f ? amount : ctx().theme.spacing;
    current.cursor_x = current.bounds.x0 + current.indent;
}

void same_line(float offset) {
    Layout& current = layout();
    const float height = current.line_height > 0.0f ? current.line_height : current.last_line;
    if (height <= 0.0f) {
        return;
    }
    if (current.line_height <= 0.0f) {
        current.cursor_y -= height + ctx().theme.spacing;
        current.line_height = height;
        current.cursor_x = current.last_end_x;
    }
    if (offset > 0.0f) {
        current.cursor_x = current.bounds.x0 + offset;
    }
}

void align_right(float width) {
    Layout& current = layout();
    current.cursor_x = std::max(current.bounds.x0, current.bounds.x1 - width);
}

void indent(float amount) {
    Layout& current = layout();
    current.indent += amount;
    current.cursor_x = current.bounds.x0 + current.indent;
}

void unindent(float amount) {
    Layout& current = layout();
    current.indent = std::max(0.0f, current.indent - amount);
    current.cursor_x = current.bounds.x0 + current.indent;
}

namespace {

bool button_impl(std::string_view label, float width, bool filled) {
    const Theme& t = ctx().theme;
    const float w = width > 0.0f ? width : font::measure(label) + t.padding * 2.0f;
    const Rect area = claim(w, t.row_height + 6.0f);
    const auto id = hash_id(label);
    const bool clicked = press(id, area);
    const bool held = ctx().active == id && hovering(area);
    const float lit = animate(id, held ? 1.0f : (ctx().hot == id ? 0.62f : 0.0f), 18.0f);

    if (filled) {
        ctx().list.rect(area, mix(t.panel, t.raised, lit));
        ctx().list.rect_outline(area, mix(t.hairline, t.border, lit), 1.0f);
    } else if (lit > 0.01f) {
        ctx().list.rect(area, with_alpha(t.raised, lit * 0.8f));
    }
    text_at(area, mix(t.text_dim, t.text, lit), label, true);
    newline();
    return clicked;
}

}

bool button(std::string_view label, float width) {
    return button_impl(label, width, true);
}

bool ghost_button(std::string_view label, float width) {
    return button_impl(label, width, false);
}

namespace {

void draw_checkbox(const Rect& box, float on, float lit) {
    const Theme& t = ctx().theme;
    const float size = box.width();
    ctx().list.rect(box, mix(mix(t.panel, t.raised, lit), t.accent, on));
    ctx().list.rect_outline(box, mix(mix(t.hairline, t.border, lit), t.accent, on), 1.0f);
    if (on > 0.02f) {
        const Vec2 tick[3] = {
            {box.x0 + size * 0.24f, box.y0 + size * 0.52f},
            {box.x0 + size * 0.42f, box.y0 + size * 0.70f},
            {box.x0 + size * 0.76f, box.y0 + size * 0.30f},
        };
        ctx().list.polyline(tick, 3, with_alpha(t.window, on), size * 0.12f, false);
    }
}

}

bool checkbox(std::string_view label, bool value) {
    const Theme& t = ctx().theme;
    const float size = 17.0f;
    const float w = size + t.spacing + font::measure(label);
    const Rect area = claim(w, std::max(size, font::line_height()));
    const auto id = hash_id(label);
    const bool clicked = press(id, area);

    const float lit = animate(id, (ctx().hot == id) ? 1.0f : 0.0f, 16.0f);
    const float on = animate(id ^ 0x85EBCA6Bu, value ? 1.0f : 0.0f, 20.0f);
    const Rect box{area.x0, area.center().y - size * 0.5f, area.x0 + size,
                   area.center().y + size * 0.5f};
    draw_checkbox(box, on, lit);
    ctx().list.text({box.x1 + t.spacing + 2.0f, center_y(area)},
                    mix(value ? t.text : t.text_dim, t.text, lit * 0.4f), label);
    newline();
    return clicked;
}

bool tab(std::string_view label, bool selected) {
    const Theme& t = ctx().theme;
    const font::ScopedFace caption(font::Face::Caption);
    const std::string text_value = upper(label);
    const float w = font::measure_tracked(text_value, t.tracking) + t.padding * 1.7f;
    const Rect area = claim(w, 34.0f);
    const auto id = hash_id(label);
    const bool clicked = press(id, area);

    const float text_width = font::measure_tracked(text_value, t.tracking);
    const float x = area.x0 + (area.width() - text_width) * 0.5f;
    const float y = center_y(area);
    const float lit = animate(id, ctx().hot == id ? 1.0f : 0.0f, 16.0f);
    const float on = animate(id ^ 0x9E3779B9u, selected ? 1.0f : 0.0f, 15.0f);
    const Color tone = mix(mix(t.text_faint, t.text_dim, lit), t.text, on);
    ctx().list.text_tracked({x, y}, tone, text_value, t.tracking);
    if (on > 0.01f) {
        const float half = area.width() * 0.42f * on;
        const float centre = area.center().x;
        ctx().list.rect({centre - half, area.y1 - 2.0f, centre + half, area.y1},
                        with_alpha(t.accent, on));
    }
    return clicked;
}

bool selectable(std::string_view label, bool selected, float width) {
    const Theme& t = ctx().theme;
    const Rect area = claim(width, t.row_height);
    const auto id = hash_id(label);
    const bool clicked = press(id, area);
    const float lit = animate(id, ctx().hot == id ? 1.0f : 0.0f, 18.0f);
    if (selected) {
        ctx().list.rect(area, t.accent_dim);
        ctx().list.rect({area.x0, area.y0, area.x0 + 2.0f, area.y1}, t.accent);
    } else if (lit > 0.01f) {
        ctx().list.rect(area, with_alpha(t.wash, lit * 0.04f));
    }
    ctx().list.text_clipped({area.x0 + 12.0f, center_y(area)},
                            selected ? t.text : mix(t.text_dim, t.text, lit), label,
                            area.width() - 12.0f);
    newline();
    return clicked;
}

bool input_text(std::string_view id_text, std::string& value, float width,
                std::string_view placeholder) {
    const Theme& t = ctx().theme;
    const Rect area = claim(width, t.row_height + 8.0f);
    const auto id = hash_id(id_text);
    const std::string key = scoped_key(id_text);

    const auto& in = input::state();
    const bool focused_before = ctx().focus == key;
    if (hovering(area) && in.pressed) {
        if (!focused_before) {
            ctx().edit = {static_cast<int>(value.size()), static_cast<int>(value.size()), 0.0f};
        }
        ctx().focus = key;
        ctx().active = id;
    } else if (in.pressed && !hovering(area) && focused_before) {
        ctx().focus.clear();
    }

    const bool focused = ctx().focus == key;
    const float lit = animate(id, focused ? 1.0f : (hovering(area) ? 0.4f : 0.0f), 16.0f);
    ctx().list.rect(area, mix(t.sunken, t.panel, lit * 0.5f));
    ctx().list.rect_outline(area, mix(t.border, t.accent, lit), 1.0f);

    const FieldResult result = edit_field(value, area.inset(8.0f), placeholder, focused,
                                          t.text, t.text_faint);
    if (result.committed || result.cancelled) {
        ctx().focus.clear();
    }
    newline();
    return result.changed;
}

LineResult input_line(std::string_view id_text, std::string& value, float width,
                      std::string_view placeholder) {
    const Theme& t = ctx().theme;
    const Rect area = claim(width, t.row_height + 8.0f);
    const auto id = hash_id(id_text);
    const std::string key = scoped_key(id_text);

    const auto& in = input::state();
    if (ctx().focus_request == key || (hovering(area) && in.pressed)) {
        if (ctx().focus != key) {
            ctx().edit = {static_cast<int>(value.size()), static_cast<int>(value.size()), 0.0f};
        }
        ctx().focus = key;
        ctx().active = id;
        if (ctx().focus_request == key) {
            ctx().focus_request.clear();
        }
    } else if (in.pressed && !hovering(area) && ctx().focus == key) {
        ctx().focus.clear();
    }

    const bool focused = ctx().focus == key;
    const float lit = animate(id, focused ? 1.0f : (hovering(area) ? 0.4f : 0.0f), 16.0f);
    ctx().list.rect(area, mix(t.sunken, t.panel, lit * 0.5f));
    ctx().list.rect_outline(area, mix(t.border, t.accent, lit), 1.0f);

    const FieldResult field = edit_field(value, area.inset(8.0f), placeholder, focused, t.text,
                                         t.text_faint);
    LineResult result;
    result.focused = focused;
    result.changed = field.changed;
    result.submitted = focused && field.committed;
    result.previous = focused && in.pressed_key(VK_UP);
    result.next = focused && in.pressed_key(VK_DOWN);
    result.complete = focused && in.pressed_key(VK_TAB);
    if (field.cancelled) {
        ctx().focus.clear();
    }
    newline();
    return result;
}

void focus(std::string_view id_text) {
    ctx().focus_request = scoped_key(id_text);
}

void move_caret_to_end(std::string_view value) {
    ctx().edit = {static_cast<int>(value.size()), static_cast<int>(value.size()), 0.0f};
}

namespace {

bool slider_drag(std::uint32_t id, const Rect& area, float& fraction) {
    const auto& in = input::state();
    if (hovering(area)) {
        ctx().next_hot = id;
        if (in.pressed) {
            ctx().active = id;
        }
    }
    bool held = false;
    if (ctx().active == id) {
        if (in.down) {
            const float raw = std::clamp((in.mouse.x - area.x0) / std::max(1.0f, area.width()),
                                         0.0f, 1.0f);
            fraction = in.shift ? fraction + (raw - fraction) * 0.15f : raw;
            held = true;
        }
        if (in.released || !in.down) {
            ctx().active = 0;
        }
    }
    return held;
}

void slider_draw(std::uint32_t id, const Rect& area, float fraction, std::string_view label,
                 std::string_view value_text, bool show_label) {
    const Theme& t = ctx().theme;
    const float lit = animate(id, (ctx().hot == id || ctx().active == id) ? 1.0f : 0.0f, 16.0f);

    ctx().list.rect(area, mix(t.sunken, t.panel, lit * 0.6f));
    const Rect fill{area.x0, area.y0, area.x0 + area.width() * fraction, area.y1};
    if (fill.width() > 1.0f) {
        ctx().list.rect(fill, mix(t.accent_dim, with_alpha(t.accent, 0.42f), lit));
    }
    ctx().list.rect_outline(area, mix(t.hairline, t.border, lit), 1.0f);

    const float knob_x = std::clamp(fill.x1, area.x0 + 2.0f, area.x1 - 2.0f);
    ctx().list.rect({knob_x - 1.0f, area.y0 + 2.0f, knob_x + 1.0f, area.y1 - 2.0f},
                    mix(t.text_dim, t.accent_bright, lit));

    if (show_label) {
        ctx().list.text_clipped({area.x0 + 9.0f, center_y(area)}, mix(t.text_dim, t.text, lit),
                                label, area.width() * 0.55f);
    }
    const font::ScopedFace mono(font::Face::Mono);
    const float value_width = font::measure(value_text);
    ctx().list.text({area.x1 - 9.0f - value_width, center_y(area)},
                    mix(t.text, t.accent_bright, lit * 0.5f), value_text);
}

bool numeric_entry(const std::string& key, const Rect& area, double& out) {
    const auto& in = input::state();
    if (ctx().numeric != key) {
        if (in.double_clicked && hovering(area)) {
            ctx().numeric = key;
            char seed[48];
            std::snprintf(seed, sizeof seed, "%g", out);
            ctx().numeric_buffer = seed;
            ctx().edit = {static_cast<int>(ctx().numeric_buffer.size()), 0, 0.0f};
            ctx().focus = key;
        }
        return false;
    }

    const Theme& t = ctx().theme;
    ctx().list.rect(area, t.sunken);
    ctx().list.rect_outline(area, t.accent, 1.0f);
    const FieldResult result = edit_field(ctx().numeric_buffer, area.inset(8.0f), "value", true,
                                          t.text, t.text_faint);
    (void)result;

    const bool outside = in.pressed && !hovering(area);
    if (result.cancelled) {
        ctx().numeric.clear();
        ctx().focus.clear();
        return false;
    }
    if (result.committed || outside) {
        ctx().numeric.clear();
        ctx().focus.clear();
        if (!ctx().numeric_buffer.empty()) {
            char* end = nullptr;
            const double parsed = std::strtod(ctx().numeric_buffer.c_str(), &end);
            if (end != ctx().numeric_buffer.c_str()) {
                out = parsed;
                return true;
            }
        }
    }
    return false;
}

std::string format_float(float value, std::string_view suffix) {
    char buffer[48];
    const float magnitude = std::abs(value);
    const char* format = magnitude >= 100.0f ? "%.1f" : (magnitude >= 10.0f ? "%.2f" : "%.3f");
    std::snprintf(buffer, sizeof buffer, format, static_cast<double>(value));
    std::string out(buffer);
    if (out.find('.') != std::string::npos) {
        while (out.size() > 1 && out.back() == '0') {
            out.pop_back();
        }
        if (!out.empty() && out.back() == '.') {
            out.pop_back();
        }
    }
    if (!suffix.empty()) {
        out += ' ';
        out += suffix;
    }
    return out;
}

}

bool slider_float(std::string_view label, float& value, float min, float max, float width) {
    const Theme& t = ctx().theme;
    const Rect area = claim(width, t.row_height + 6.0f);
    const auto id = hash_id(label);
    const std::string key = scoped_key(label);
    const float span = max - min;

    double typed = value;
    if (numeric_entry(key, area, typed)) {
        value = std::clamp(static_cast<float>(typed), min, max);
        newline();
        return true;
    }
    if (ctx().numeric == key) {
        newline();
        return false;
    }

    float fraction = span != 0.0f ? std::clamp((value - min) / span, 0.0f, 1.0f) : 0.0f;
    bool changed = false;
    if (slider_drag(id, area, fraction)) {
        const float next = min + span * fraction;
        if (next != value) {
            value = next;
            changed = true;
        }
    }
    slider_draw(id, area, fraction, label, format_float(value, {}), true);
    newline();
    return changed;
}

bool slider_int(std::string_view label, int& value, int min, int max, float width) {
    const Theme& t = ctx().theme;
    const Rect area = claim(width, t.row_height + 6.0f);
    const auto id = hash_id(label);
    const std::string key = scoped_key(label);
    const int span = max - min;

    double typed = value;
    if (numeric_entry(key, area, typed)) {
        value = std::clamp(static_cast<int>(std::lround(typed)), min, max);
        newline();
        return true;
    }
    if (ctx().numeric == key) {
        newline();
        return false;
    }

    float fraction = span != 0 ? std::clamp(static_cast<float>(value - min)
                                            / static_cast<float>(span), 0.0f, 1.0f)
                               : 0.0f;
    bool changed = false;
    if (slider_drag(id, area, fraction)) {
        const int next = min + static_cast<int>(std::lround(fraction * static_cast<float>(span)));
        if (next != value) {
            value = next;
            changed = true;
        }
        fraction = span != 0 ? static_cast<float>(value - min) / static_cast<float>(span) : 0.0f;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%d", value);
    slider_draw(id, area, fraction, label, buffer, true);
    newline();
    return changed;
}

void scroll_to_end(std::string_view id_text) {
    Scroll& scroll = ctx().scrolls[std::string(id_text)];
    scroll.target = std::max(0.0f, scroll.content);
    scroll.offset = scroll.target;
}

float delta_time() {
    return ctx().dt;
}

void begin_column(float width, float height) {
    Layout& parent = layout();
    Layout child;
    child.bounds = {parent.cursor_x, parent.cursor_y, parent.cursor_x + width,
                    parent.cursor_y + height};
    child.cursor_x = child.bounds.x0;
    child.cursor_y = child.bounds.y0;
    child.id = "column";
    ctx().layouts.push_back(child);
}

void end_column() {
    const Layout child = layout();
    ctx().layouts.pop_back();

    Layout& parent = layout();
    parent.cursor_x = child.bounds.x1 + ctx().theme.padding;
    parent.line_height = std::max(parent.line_height, child.bounds.y1 - child.bounds.y0);
}

void begin_scroll(std::string_view id_text, float height) {
    Layout& parent = layout();
    const Rect area{parent.bounds.x0, parent.cursor_y, parent.bounds.x1,
                    parent.cursor_y + std::max(0.0f, height)};

    const std::string key(id_text);
    Scroll& scroll = ctx().scrolls[key];

    const float view = std::max(0.0f, area.height() - 12.0f);
    const float limit = std::max(0.0f, scroll.content - view);

    const auto& in = input::state();
    const bool hover = hovering(area);
    if (hover && in.wheel != 0.0f) {
        scroll.target -= in.wheel * 64.0f;
    }
    if (hover && limit > 0.0f) {
        if (in.pressed_key(VK_NEXT)) {
            scroll.target += view * 0.85f;
        }
        if (in.pressed_key(VK_PRIOR)) {
            scroll.target -= view * 0.85f;
        }
    }

    const float track_x = area.x1 - 9.0f;
    const Rect track{track_x, area.y0 + 6.0f, track_x + 5.0f, area.y1 - 6.0f};
    const auto bar_id = hash_bytes(key, 0x7f4a7c15u);
    if (limit > 0.0f) {
        const float ratio = view / std::max(view, scroll.content);
        const float bar = std::max(28.0f, view * ratio);
        const float travel = std::max(1.0f, view - bar);
        if (hovering(track) && in.pressed) {
            ctx().active = bar_id;
        }
        if (ctx().active == bar_id && in.down) {
            const float local = in.mouse.y - track.y0 - bar * 0.5f;
            scroll.target = std::clamp(local / travel, 0.0f, 1.0f) * limit;
            scroll.offset = scroll.target;
        }
    }

    scroll.target = std::clamp(scroll.target, 0.0f, limit);
    scroll.offset = std::clamp(smooth(scroll.offset, scroll.target, 26.0f), 0.0f, limit);
    const bool near_bar = hovering(track) || ctx().active == bar_id;
    scroll.bar = smooth(scroll.bar, limit > 0.0f ? (near_bar ? 1.0f : (hover ? 0.7f : 0.35f)) : 0.0f,
                        12.0f);

    Layout child;
    child.bounds = {area.x0, area.y0 + 6.0f, area.x1 - 14.0f, area.y1 - 6.0f};
    child.cursor_x = child.bounds.x0;
    child.cursor_y = child.bounds.y0 - scroll.offset;
    child.scrolling = true;
    child.id = key;
    child.start_y = child.cursor_y;
    ctx().layouts.push_back(child);
    ctx().list.push_clip({area.x0, area.y0, area.x1, area.y1});
}

void end_scroll() {
    const Theme& t = ctx().theme;
    const Layout child = layout();
    ctx().layouts.pop_back();

    Scroll& scroll = ctx().scrolls[child.id];
    scroll.content = child.cursor_y - child.start_y;

    const float view = child.bounds.y1 - child.bounds.y0;
    const float limit = std::max(0.0f, scroll.content - view);

    const float fade = 20.0f;
    const Color clear = with_alpha(t.window, 0.0f);
    if (scroll.offset > 1.0f) {
        ctx().list.rect_gradient({child.bounds.x0, child.bounds.y0 - 6.0f,
                                  child.bounds.x1 + 14.0f, child.bounds.y0 + fade},
                                 t.window, clear);
    }
    if (scroll.offset < limit - 1.0f) {
        ctx().list.rect_gradient({child.bounds.x0, child.bounds.y1 - fade,
                                  child.bounds.x1 + 14.0f, child.bounds.y1 + 6.0f},
                                 clear, t.window);
    }

    ctx().list.pop_clip();

    if (limit > 0.0f && scroll.bar > 0.01f) {
        const float track_x = child.bounds.x1 + 5.0f;
        const float ratio = view / std::max(view, scroll.content);
        const float bar = std::max(28.0f, view * ratio);
        const float travel = std::max(0.0f, view - bar);
        const float position = travel * (scroll.offset / limit);
        ctx().list.rect({track_x + 2.0f, child.bounds.y0, track_x + 3.0f, child.bounds.y1},
                        with_alpha(t.hairline, scroll.bar));
        ctx().list.rect({track_x + 1.0f, child.bounds.y0 + position, track_x + 4.0f,
                         child.bounds.y0 + position + bar},
                        with_alpha(mix(t.text_faint, t.text_dim, scroll.bar),
                                   0.4f + scroll.bar * 0.6f));
    }

    Layout& parent = layout();
    parent.cursor_y = child.bounds.y1 + 6.0f + t.spacing;
    parent.cursor_x = parent.bounds.x0 + parent.indent;
    parent.line_height = 0.0f;
}

void begin_row(float height) {
    Layout& current = layout();
    ctx().row_height = height;
    ctx().row_x = current.cursor_x;
    ctx().row_y = current.cursor_y;
}

void row_cell(float width, Color color, std::string_view value) {
    const Rect area{ctx().row_x, ctx().row_y, ctx().row_x + width, ctx().row_y + ctx().row_height};
    ctx().list.text_clipped({area.x0, center_y(area)}, color, value, width - 6.0f);
    ctx().row_x += width;
}

void end_row() {
    Layout& current = layout();
    current.cursor_y = ctx().row_y + ctx().row_height;
    current.cursor_x = current.bounds.x0 + current.indent;
    current.line_height = 0.0f;
}

void progress(float fraction, float width, Color color) {
    const Theme& t = ctx().theme;
    const Rect area = claim(width, 6.0f);
    ctx().list.rect(area, t.sunken);
    const float clamped = std::clamp(fraction, 0.0f, 1.0f);
    if (clamped > 0.001f) {
        ctx().list.rect({area.x0, area.y0, area.x0 + area.width() * clamped, area.y1}, color);
    }
    newline();
}

void badge(std::string_view label, Color color) {
    const font::ScopedFace caption(font::Face::Caption);
    const std::string text_value = upper(label);
    const float width = font::measure_tracked(text_value, 1.0f) + 16.0f;
    const Rect area = claim(width, font::line_height() + 6.0f);
    ctx().list.rect(area, with_alpha(color, 0.16f));
    ctx().list.rect_outline(area, with_alpha(color, 0.4f), 1.0f);
    ctx().list.text_tracked({area.x0 + 8.0f, center_y(area)}, color, text_value, 1.0f);
}

void keycap(std::string_view label) {
    const Theme& t = ctx().theme;
    const font::ScopedFace mono(font::Face::Mono);
    const float width = std::max(24.0f, font::measure(label) + 14.0f);
    const Rect area = claim(width, font::line_height() + 8.0f);
    ctx().list.rect(area, t.raised);
    ctx().list.rect_outline(area, t.border, 1.0f);
    ctx().list.rect({area.x0 + 1.0f, area.y1 - 2.0f, area.x1 - 1.0f, area.y1 - 1.0f},
                    with_alpha(rgba(0, 0, 0, 255), 0.35f));
    text_at(area, t.text_dim, label, true);
}

void tooltip(std::string_view value) {
    queue_tooltip(value);
}

void push_id(std::string_view value) {
    ctx().id_stack.push_back(ctx().id_seed);
    ctx().id_seed = hash_bytes(value, ctx().id_seed);
}

void push_id(std::uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "#%u", value);
    push_id(buffer);
}

void pop_id() {
    if (!ctx().id_stack.empty()) {
        ctx().id_seed = ctx().id_stack.back();
        ctx().id_stack.pop_back();
    }
}

void set_filter(std::string_view pattern) {
    ctx().filter.assign(pattern);
}

bool filtering() {
    return !ctx().filter.empty();
}

bool fuzzy(std::string_view text_value, std::string_view pattern,
           std::vector<std::uint32_t>* spans) {
    if (spans != nullptr) {
        spans->clear();
    }
    if (pattern.empty()) {
        return true;
    }
    std::size_t p = 0;
    for (std::size_t i = 0; i < text_value.size() && p < pattern.size(); ++i) {
        const auto a = std::tolower(static_cast<unsigned char>(text_value[i]));
        const auto b = std::tolower(static_cast<unsigned char>(pattern[p]));
        if (a == b) {
            if (spans != nullptr) {
                spans->push_back(static_cast<std::uint32_t>(i));
            }
            ++p;
        }
    }
    if (p < pattern.size()) {
        if (spans != nullptr) {
            spans->clear();
        }
        return false;
    }
    return true;
}

bool capturing_key() {
    return !ctx().capture.empty();
}

void begin_settings(float label_width) {
    ctx().settings.push_back(label_width > 0.0f ? label_width : ctx().theme.label_width);
}

void end_settings() {
    if (!ctx().settings.empty()) {
        ctx().settings.pop_back();
    }
}

bool begin_group(std::string_view label, bool default_open) {
    const Theme& t = ctx().theme;
    const std::string key = scoped_key(label);
    auto found = ctx().groups.find(key);
    if (found == ctx().groups.end()) {
        Group created;
        created.expanded = default_open;
        created.open = default_open ? 1.0f : 0.0f;
        found = ctx().groups.emplace(key, created).first;
    }
    Group& group = found->second;

    const bool filtered = !ctx().filter.empty();
    if (filtered && group.matches == 0) {
        ctx().group_stack.push_back({key, layout().cursor_y, false});
        ctx().id_seed = hash_bytes(key, ctx().id_seed);
        return false;
    }

    Layout& current = layout();
    const float header_height = 46.0f;
    const Rect area{current.bounds.x0 + current.indent, current.cursor_y, current.bounds.x1,
                    current.cursor_y + header_height};
    const auto id = hash_id(label) ^ 0x1b873593u;
    if (press(id, area) && !filtered) {
        group.expanded = !group.expanded;
    }
    const bool open = filtered || group.expanded;
    const float lit = animate(id, hovering(area) ? 1.0f : 0.0f, 16.0f);
    group.open = smooth(group.open, open ? 1.0f : 0.0f, 20.0f);

    const Rect band{area.x0, area.y0 + 20.0f, area.x1, area.y1};
    {
        const font::ScopedFace caption(font::Face::Caption);
        const std::string text_value = upper(label);
        ctx().list.text_tracked({band.x0 + 12.0f, center_y(band)},
                                mix(mix(t.text_faint, t.text_dim, group.open), t.text, lit),
                                text_value, t.tracking);
        if (group.open < 0.98f && group.matches > 0) {
            const float width = font::measure_tracked(text_value, t.tracking);
            char count[16];
            std::snprintf(count, sizeof count, "%d", group.matches);
            ctx().list.text_tracked({band.x0 + 22.0f + width, center_y(band)},
                                    with_alpha(t.text_faint, 1.0f - group.open), count, t.tracking);
        }
    }
    draw_chevron({band.x1 - 8.0f, band.center().y}, 4.0f, mix(t.text_faint, t.text_dim, lit),
                 group.open);
    ctx().list.rect({band.x0, band.y1 - 1.0f, band.x1, band.y1},
                    mix(t.border, t.text_faint, lit * 0.5f));

    current.line_height = header_height;
    newline();
    current.cursor_y -= t.spacing;

    ctx().id_seed = hash_bytes(key, ctx().id_seed);
    if (group.open < 0.02f) {
        ctx().group_stack.push_back({key, layout().cursor_y, false});
        return false;
    }

    ctx().group_stack.push_back({key, layout().cursor_y, true});
    return true;
}

void end_group() {
    if (ctx().group_stack.empty()) {
        return;
    }
    const GroupFrame frame = ctx().group_stack.back();
    ctx().group_stack.pop_back();

    if (frame.indented) {
        spacing(6.0f);
    }

    ctx().id_seed = 0;
    for (const auto& open : ctx().group_stack) {
        ctx().id_seed = hash_bytes(open.key, ctx().id_seed);
    }
}

bool setting_bool(std::string_view label, bool& value, std::string_view help) {
    const Theme& t = ctx().theme;
    const Row row = begin_setting_row(label, help, t.row_height + 16.0f);
    if (!row.visible) {
        return false;
    }
    const auto id = hash_id(label) ^ 0xc2b2ae35u;
    const float size = 17.0f;
    const Rect box{row.control.x0, row.area.center().y - size * 0.5f, row.control.x0 + size,
                   row.area.center().y + size * 0.5f};
    const bool clicked = press(id, row.area);
    const float lit = animate(id, row.hovered ? 1.0f : 0.0f, 16.0f);
    const float on = animate(id ^ 0x85EBCA6Bu, value ? 1.0f : 0.0f, 20.0f);
    draw_checkbox(box, on, lit);
    {
        const font::ScopedFace micro(font::Face::Micro);
        ctx().list.text({box.x1 + 10.0f, center_y(row.area)},
                        value ? t.text_dim : t.text_faint, value ? "on" : "off");
    }
    if (clicked) {
        value = !value;
    }
    newline();
    return clicked;
}

bool setting_float(std::string_view label, float& value, float min, float max,
                   std::string_view suffix, std::string_view help) {
    const Theme& t = ctx().theme;
    const Row row = begin_setting_row(label, help, t.row_height + 16.0f);
    if (!row.visible) {
        return false;
    }
    const auto id = hash_id(label) ^ 0x27d4eb2fu;
    const std::string key = scoped_key(label);
    const Rect area{row.control.x0, row.area.y0 + 7.0f, row.control.x1, row.area.y1 - 7.0f};

    double typed = value;
    if (numeric_entry(key, area, typed)) {
        value = std::clamp(static_cast<float>(typed), min, max);
        newline();
        return true;
    }
    if (ctx().numeric == key) {
        newline();
        return false;
    }

    const float span = max - min;
    float fraction = span != 0.0f ? std::clamp((value - min) / span, 0.0f, 1.0f) : 0.0f;
    bool changed = false;
    if (slider_drag(id, area, fraction)) {
        const float next = std::clamp(min + span * fraction, min, max);
        if (next != value) {
            value = next;
            changed = true;
        }
    }
    slider_draw(id, area, fraction, label, format_float(value, suffix), false);
    newline();
    return changed;
}

bool setting_int(std::string_view label, int& value, int min, int max, std::string_view suffix,
                 std::string_view help) {
    const Theme& t = ctx().theme;
    const Row row = begin_setting_row(label, help, t.row_height + 16.0f);
    if (!row.visible) {
        return false;
    }
    const auto id = hash_id(label) ^ 0x165667b1u;
    const std::string key = scoped_key(label);
    const Rect area{row.control.x0, row.area.y0 + 7.0f, row.control.x1, row.area.y1 - 7.0f};

    double typed = value;
    if (numeric_entry(key, area, typed)) {
        value = std::clamp(static_cast<int>(std::lround(typed)), min, max);
        newline();
        return true;
    }
    if (ctx().numeric == key) {
        newline();
        return false;
    }

    const int span = max - min;
    float fraction = span != 0 ? std::clamp(static_cast<float>(value - min)
                                            / static_cast<float>(span), 0.0f, 1.0f)
                               : 0.0f;
    bool changed = false;
    if (slider_drag(id, area, fraction)) {
        const int next = min + static_cast<int>(std::lround(fraction * static_cast<float>(span)));
        if (next != value) {
            value = next;
            changed = true;
        }
        fraction = span != 0 ? static_cast<float>(value - min) / static_cast<float>(span) : 0.0f;
    }
    char buffer[48];
    if (suffix.empty()) {
        std::snprintf(buffer, sizeof buffer, "%d", value);
    } else {
        std::snprintf(buffer, sizeof buffer, "%d %.*s", value, static_cast<int>(suffix.size()),
                      suffix.data());
    }
    slider_draw(id, area, fraction, label, buffer, false);
    newline();
    return changed;
}

bool setting_combo(std::string_view label, int& index, const char* const* items, int count,
                   std::string_view help) {
    const Theme& t = ctx().theme;
    const Row row = begin_setting_row(label, help, t.row_height + 16.0f);
    if (!row.visible) {
        return false;
    }
    if (items == nullptr || count <= 0) {
        newline();
        return false;
    }

    const auto id = hash_id(label) ^ 0x9e3779b9u;
    const std::string key = scoped_key(label);
    const Rect box{row.control.x0, row.area.y0 + 7.0f,
                   std::min(row.control.x1, row.control.x0 + 260.0f), row.area.y1 - 7.0f};
    const bool clicked = press(id, box);
    const bool open = ctx().popup == key;
    const float lit = animate(id, open ? 1.0f : (row.hovered ? 0.55f : 0.0f), 16.0f);

    ctx().list.rect(box, mix(t.panel, t.raised, lit * 0.6f));
    ctx().list.rect_outline(box, mix(t.border, t.accent, open ? 1.0f : lit * 0.35f), 1.0f);
    const int safe = std::clamp(index, 0, count - 1);
    ctx().list.text_clipped({box.x0 + 9.0f, center_y(box)}, mix(t.text_dim, t.text, 0.4f + lit),
                            items[safe] != nullptr ? items[safe] : "", box.width() - 30.0f);
    draw_chevron({box.x1 - 13.0f, box.center().y}, 4.0f, mix(t.text_faint, t.text_dim, lit),
                 open ? 1.0f : 0.0f);

    if (clicked) {
        if (open) {
            ctx().popup.clear();
        } else {
            ctx().popup = key;
            ctx().popup_anchor = box;
        }
    }

    bool changed = false;
    if (ctx().popup == key) {
        ctx().popup_seen = true;
        const float item_height = 26.0f;
        const float height = std::min(static_cast<float>(count) * item_height + 10.0f, 260.0f);
        float top = box.y1 + 4.0f;
        if (top + height > ctx().display.y - 8.0f) {
            top = std::max(8.0f, box.y0 - 4.0f - height);
        }
        const Rect list{box.x0, top, box.x1, top + height};
        ctx().popup_rect = list;
        ctx().popup_anchor = box;

        const int previous_layer = ctx().list.layer();
        ctx().list.set_layer(kLayerPopup);
        ctx().list.push_clip({0.0f, 0.0f, ctx().display.x, ctx().display.y});
        ctx().list.rect(list, t.chrome);
        ctx().list.rect_outline(list, t.border, 1.0f);

        for (int item = 0; item < count; ++item) {
            const Rect entry{list.x0 + 4.0f, list.y0 + 5.0f + static_cast<float>(item) * item_height,
                             list.x1 - 4.0f,
                             list.y0 + 5.0f + static_cast<float>(item + 1) * item_height};
            if (entry.y1 > list.y1 - 4.0f) {
                break;
            }
            const auto entry_id = id ^ (static_cast<std::uint32_t>(item + 1) * 0x85ebca6bu);
            const bool chose = press(entry_id, entry);
            const bool over = hovering(entry);
            if (over) {
                ctx().list.rect(entry, with_alpha(t.wash, 0.10f));
            }
            if (item == safe) {
                ctx().list.rect({entry.x0, entry.y0 + 4.0f, entry.x0 + 2.0f, entry.y1 - 4.0f},
                                t.accent);
            }
            ctx().list.text_clipped({entry.x0 + 10.0f, center_y(entry)},
                                    item == safe ? t.text : (over ? t.text : t.text_dim),
                                    items[item] != nullptr ? items[item] : "",
                                    entry.width() - 16.0f);
            if (chose) {
                if (index != item) {
                    index = item;
                    changed = true;
                }
                ctx().popup.clear();
            }
        }

        ctx().list.pop_clip();
        ctx().list.set_layer(previous_layer);
    }

    newline();
    return changed;
}

bool setting_key(std::string_view label, std::uint32_t& key, std::string_view help) {
    const Theme& t = ctx().theme;
    const Row row = begin_setting_row(label, help, t.row_height + 16.0f);
    if (!row.visible) {
        return false;
    }

    const auto id = hash_id(label) ^ 0x2545f491u;
    const std::string slot = scoped_key(label);
    const Rect box{row.control.x0, row.area.y0 + 7.0f,
                   std::min(row.control.x1, row.control.x0 + 150.0f), row.area.y1 - 7.0f};
    const bool clicked = press(id, box);
    const bool capturing = ctx().capture == slot;

    if (clicked && !capturing) {
        ctx().capture = slot;
    }

    bool changed = false;
    if (capturing) {
        const auto& in = input::state();
        if (in.pressed_key(VK_ESCAPE)) {
            ctx().capture.clear();
        } else if (in.pressed_key(VK_BACK) || in.pressed_key(VK_DELETE)) {
            if (key != 0) {
                key = 0;
                changed = true;
            }
            ctx().capture.clear();
        } else if (in.captured != 0) {
            if (key != in.captured) {
                key = in.captured;
                changed = true;
            }
            ctx().capture.clear();
        }
    }

    const float lit = animate(id, row.hovered ? 1.0f : 0.0f, 16.0f);
    if (capturing) {
        const float pulse = 0.55f + 0.45f * std::sin(
            static_cast<float>(GetTickCount64()) * 0.006f);
        ctx().list.rect(box, with_alpha(t.accent, 0.12f));
        ctx().list.rect_outline(box, with_alpha(t.accent, pulse), 1.0f);
        const font::ScopedFace micro(font::Face::Micro);
        text_at(box, t.accent_bright, "press a key", true);
    } else {
        ctx().list.rect(box, mix(t.raised, t.panel, 1.0f - lit * 0.5f));
        ctx().list.rect_outline(box, mix(t.border, t.accent, lit * 0.4f), 1.0f);
        ctx().list.rect({box.x0 + 1.0f, box.y1 - 2.0f, box.x1 - 1.0f, box.y1 - 1.0f},
                        with_alpha(rgba(0, 0, 0, 255), 0.3f));
        const font::ScopedFace mono(font::Face::Mono);
        text_at(box, key == 0 ? t.text_faint : mix(t.text_dim, t.text, lit),
                input::key_name(key), true);
    }

    if (capturing || row.hovered) {
        const font::ScopedFace micro(font::Face::Micro);
        ctx().list.text({box.x1 + 10.0f, center_y(row.area)}, t.text_faint,
                        capturing ? "esc cancels, backspace unbinds" : "click to rebind");
    }

    newline();
    return changed;
}

bool setting_text(std::string_view label, std::string& value, std::string_view placeholder,
                  std::string_view help) {
    const Theme& t = ctx().theme;
    const Row row = begin_setting_row(label, help, t.row_height + 16.0f);
    if (!row.visible) {
        return false;
    }

    const auto id = hash_id(label) ^ 0x6a09e667u;
    const std::string key = scoped_key(label);
    const Rect box{row.control.x0, row.area.y0 + 7.0f, row.control.x1, row.area.y1 - 7.0f};

    const auto& in = input::state();
    const bool focused_before = ctx().focus == key;
    if (hovering(box) && in.pressed) {
        if (!focused_before) {
            ctx().edit = {static_cast<int>(value.size()), static_cast<int>(value.size()), 0.0f};
        }
        ctx().focus = key;
        ctx().active = id;
    } else if (in.pressed && !hovering(box) && focused_before) {
        ctx().focus.clear();
    }

    const bool focused = ctx().focus == key;
    const float lit = animate(id, focused ? 1.0f : (row.hovered ? 0.4f : 0.0f), 16.0f);
    ctx().list.rect(box, mix(t.sunken, t.panel, lit * 0.5f));
    ctx().list.rect_outline(box, mix(t.border, t.accent, lit), 1.0f);

    const FieldResult result = edit_field(value, box.inset(8.0f), placeholder, focused,
                                          t.text, t.text_faint);
    if (result.committed || result.cancelled) {
        ctx().focus.clear();
    }
    newline();
    return result.changed;
}

void readout(std::string_view label, std::string_view value) {
    readout_colored(label, ctx().theme.text_dim, value);
}

void readout_colored(std::string_view label, Color color, std::string_view value) {
    const Theme& t = ctx().theme;
    if (!ctx().filter.empty() && !fuzzy(label, ctx().filter, &ctx().spans)) {
        return;
    }
    for (const auto& frame : ctx().group_stack) {
        ++ctx().groups[frame.key].counted;
    }

    Layout& current = layout();
    const float height = 19.0f;
    const Rect area{current.bounds.x0 + current.indent, current.cursor_y, current.bounds.x1,
                    current.cursor_y + height};
    const float label_width = settings_label_width();
    {
        const font::ScopedFace micro(font::Face::Micro);
        ctx().list.text_clipped({area.x0 + 12.0f, center_y(area)}, t.text_faint, label,
                                label_width - 24.0f);
    }
    {
        const font::ScopedFace mono(font::Face::Mono);
        ctx().list.text_clipped({area.x0 + label_width, center_y(area)}, color, value,
                                area.width() - label_width);
    }
    current.line_height = height;
    ctx().last_gutter = {area.x1 - t.gutter, area.y0, area.x1, area.y1};
    ctx().last_row_hovered = false;
    ctx().last_row_id = 0;
    newline();
}

bool revert_marker(bool modified) {
    if (!modified) {
        return false;
    }
    const Theme& t = ctx().theme;
    const Rect area = ctx().last_gutter;
    if (area.width() <= 0.0f) {
        return false;
    }
    const auto id = ctx().last_row_id ^ 0xa5a5f00du;
    const bool clicked = press(id, area);
    const bool over = hovering(area);
    const float lit = animate(id, over ? 1.0f : (ctx().last_row_hovered ? 0.5f : 0.18f), 16.0f);

    const Vec2 centre = area.center();
    const Color tone = mix(t.text_faint, t.accent_bright, lit);
    ctx().list.arc(centre, 5.0f, -2.5f, 1.9f, tone, 1.4f);
    const Vec2 head[3] = {{centre.x - 6.6f, centre.y - 3.2f},
                          {centre.x - 2.0f, centre.y - 4.4f},
                          {centre.x - 5.4f, centre.y + 0.6f}};
    ctx().list.triangle(head[0], head[1], head[2], tone);

    if (over) {
        queue_tooltip("reset to default");
    }
    return clicked;
}

}
