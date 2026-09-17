#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "ui/draw.h"

namespace bridger::ui::input {

struct State {
    Vec2 mouse;
    Vec2 mouse_delta;
    bool down = false;
    bool pressed = false;
    bool released = false;
    bool double_clicked = false;
    bool right_pressed = false;
    float wheel = 0.0f;
    std::string characters;

    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    std::array<bool, 256> key_pressed{};
    std::array<bool, 256> key_held{};

    std::uint32_t captured = 0;

    [[nodiscard]] bool pressed_key(std::uint32_t key) const {
        return key < 256 && key_pressed[key];
    }
    [[nodiscard]] bool held_key(std::uint32_t key) const {
        return key < 256 && key_held[key];
    }
};

struct Context;
[[nodiscard]] Context* create_context();
void destroy_context(Context* context);
void set_context(Context* context);

void on_mouse_move(float x, float y);
void on_mouse_button(bool down);
void on_right_button(bool down);
void on_wheel(float delta);
void on_character(char value);
void on_key(std::uint32_t key, bool down);

void begin_frame();
void end_frame();

[[nodiscard]] const State& state();

[[nodiscard]] std::string clipboard();
void set_clipboard(std::string_view value);

[[nodiscard]] std::string key_name(std::uint32_t key);

}
