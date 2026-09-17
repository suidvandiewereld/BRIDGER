#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "ui/draw.h"

namespace bridger::ui::font {

enum class Face : int {
    Micro = 0,
    Caption = 1,
    Body = 2,
    Strong = 3,
    Title = 4,
    Mono = 5,
    Brand = 6,
    Count = 7,
};

struct Glyph {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float bearing_x = 0.0f;
    float bearing_y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float advance = 0.0f;
};

struct Image {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    int width = 0;
    int height = 0;
    bool valid = false;
};

bool register_file(const wchar_t* path);
bool build(const wchar_t* family);
Image place_image(const std::uint8_t* pixels, int width, int height);

void set_face(Face face);
[[nodiscard]] Face face();

[[nodiscard]] const Glyph* lookup(unsigned char code);
[[nodiscard]] Vec2 white_pixel();
[[nodiscard]] const std::vector<std::uint8_t>& atlas();
[[nodiscard]] int atlas_size();
[[nodiscard]] float line_height();
[[nodiscard]] float line_height(Face face);
[[nodiscard]] float ascent();
[[nodiscard]] float measure(std::string_view text);
[[nodiscard]] float measure(Face face, std::string_view text);
[[nodiscard]] float measure_tracked(std::string_view text, float tracking);

class ScopedFace {
public:
    explicit ScopedFace(Face next) : previous_(face()) { set_face(next); }
    ~ScopedFace() { set_face(previous_); }
    ScopedFace(const ScopedFace&) = delete;
    ScopedFace& operator=(const ScopedFace&) = delete;

private:
    Face previous_;
};

}
