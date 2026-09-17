#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ui/draw.h"

namespace bridger::ui {

struct Theme {
    Color scrim = rgba(0, 0, 0, 110);
    Color window = rgba(15, 17, 20, 246);
    Color window_low = rgba(10, 12, 14, 246);
    Color chrome = rgba(21, 24, 28, 255);
    Color panel = rgba(19, 22, 26, 255);
    Color raised = rgba(30, 34, 40, 255);
    Color sunken = rgba(12, 14, 17, 255);
    Color border = rgba(46, 52, 60, 255);
    Color hairline = rgba(32, 36, 42, 255);

    Color accent = rgba(124, 205, 224, 255);
    Color accent_bright = rgba(168, 227, 240, 255);
    Color accent_dim = rgba(124, 205, 224, 38);
    Color wash = rgba(232, 238, 245, 255);

    Color text = rgba(226, 231, 237, 255);
    Color text_dim = rgba(146, 154, 165, 255);
    Color text_faint = rgba(96, 104, 115, 255);

    Color good = rgba(126, 202, 148, 255);
    Color warn = rgba(226, 176, 100, 255);
    Color bad = rgba(226, 112, 104, 255);

    float padding = 20.0f;
    float spacing = 8.0f;
    float row_height = 24.0f;
    float radius = 0.0f;
    float radius_large = 0.0f;
    float chrome_height = 48.0f;
    float status_height = 28.0f;
    float rail_width = 232.0f;
    float tracking = 1.6f;
    float label_width = 210.0f;
    float gutter = 24.0f;
};

struct Context;
[[nodiscard]] Context* create_context();
void destroy_context(Context* context);
void set_context(Context* context);

const Theme& theme();
void set_theme(const Theme& theme);

void begin_frame(Vec2 display, float delta_seconds);
void end_frame();
DrawList& draw();

bool begin_window(std::string_view title, Rect& area, bool& open);
void begin_area(const Rect& area);
void end_area();
[[nodiscard]] bool editing();
void end_window();
void window_status(std::string_view value);
bool window_tab(std::string_view label, bool selected);
[[nodiscard]] Rect window_body();

void text(std::string_view value);
void text_colored(Color color, std::string_view value);
float text_wrapped(Color color, std::string_view value);
void label_value(std::string_view label, std::string_view value);
void separator();
void rule();
void newline();
void header(std::string_view label);
void tracked(Color color, std::string_view value, float size_tracking);
void spacing(float amount = 0.0f);
void same_line(float offset = 0.0f);
void indent(float amount);
void unindent(float amount);

[[nodiscard]] float available_width();
[[nodiscard]] float remaining_height();
[[nodiscard]] Vec2 cursor();
void set_cursor_x(float x);
void set_cursor_y(float y);
void align_right(float width);

bool button(std::string_view label, float width = 0.0f);
bool ghost_button(std::string_view label, float width = 0.0f);
bool checkbox(std::string_view label, bool value);
bool tab(std::string_view label, bool selected);
bool selectable(std::string_view label, bool selected, float width = 0.0f);
bool input_text(std::string_view id, std::string& value, float width, std::string_view placeholder);

struct LineResult {
    bool changed = false;
    bool submitted = false;
    bool previous = false;
    bool next = false;
    bool complete = false;
    bool focused = false;
};
LineResult input_line(std::string_view id, std::string& value, float width, std::string_view placeholder);
void focus(std::string_view id);
void move_caret_to_end(std::string_view value);
bool slider_float(std::string_view label, float& value, float min, float max, float width = 0.0f);
bool slider_int(std::string_view label, int& value, int min, int max, float width = 0.0f);

void begin_column(float width, float height);
void end_column();

void begin_scroll(std::string_view id, float height);
void scroll_to_end(std::string_view id);
[[nodiscard]] float delta_time();
void end_scroll();

void begin_row(float height);
void row_cell(float width, Color color, std::string_view value);
void end_row();

void progress(float fraction, float width, Color color);

void begin_settings(float label_width = 0.0f);
void end_settings();

bool setting_bool(std::string_view label, bool& value, std::string_view help = {});
bool setting_float(std::string_view label, float& value, float min, float max,
                   std::string_view suffix = {}, std::string_view help = {});
bool setting_int(std::string_view label, int& value, int min, int max,
                 std::string_view suffix = {}, std::string_view help = {});
bool setting_combo(std::string_view label, int& index, const char* const* items, int count,
                   std::string_view help = {});
bool setting_key(std::string_view label, std::uint32_t& key, std::string_view help = {});
bool setting_text(std::string_view label, std::string& value, std::string_view placeholder = {},
                  std::string_view help = {});

void readout(std::string_view label, std::string_view value);
void readout_colored(std::string_view label, Color color, std::string_view value);

bool revert_marker(bool modified);

bool begin_group(std::string_view label, bool default_open = true);
void end_group();

[[nodiscard]] bool capturing_key();

void push_id(std::string_view value);
void push_id(std::uint32_t value);
void pop_id();

struct IdScope {
    explicit IdScope(std::string_view value) { push_id(value); }
    explicit IdScope(std::uint32_t value) { push_id(value); }
    ~IdScope() { pop_id(); }
    IdScope(const IdScope&) = delete;
    IdScope& operator=(const IdScope&) = delete;
};

void set_filter(std::string_view pattern);
[[nodiscard]] bool filtering();

bool fuzzy(std::string_view text, std::string_view pattern, std::vector<std::uint32_t>* spans);

void tooltip(std::string_view text);
void badge(std::string_view label, Color color);
void keycap(std::string_view label);
void note(std::string_view value);

}
