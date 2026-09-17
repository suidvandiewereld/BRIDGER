#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace bridger::ui {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Rect {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;

    [[nodiscard]] float width() const { return x1 - x0; }
    [[nodiscard]] float height() const { return y1 - y0; }
    [[nodiscard]] Vec2 center() const { return {(x0 + x1) * 0.5f, (y0 + y1) * 0.5f}; }
    [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
    [[nodiscard]] bool contains(Vec2 point) const {
        return point.x >= x0 && point.x < x1 && point.y >= y0 && point.y < y1;
    }
    [[nodiscard]] Rect inset(float amount) const {
        return {x0 + amount, y0 + amount, x1 - amount, y1 - amount};
    }
    [[nodiscard]] Rect expand(float amount) const { return inset(-amount); }
    [[nodiscard]] Rect moved(float dx, float dy) const {
        return {x0 + dx, y0 + dy, x1 + dx, y1 + dy};
    }
    [[nodiscard]] Rect clipped(const Rect& other) const {
        return {x0 > other.x0 ? x0 : other.x0, y0 > other.y0 ? y0 : other.y0,
                x1 < other.x1 ? x1 : other.x1, y1 < other.y1 ? y1 : other.y1};
    }
};

using Color = std::uint32_t;

constexpr Color rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    return static_cast<Color>(r) | (static_cast<Color>(g) << 8) | (static_cast<Color>(b) << 16)
         | (static_cast<Color>(a) << 24);
}

Color with_alpha(Color color, float alpha);
Color mix(Color from, Color to, float amount);

enum Corner : unsigned {
    kCornerNone = 0,
    kCornerTopLeft = 1,
    kCornerTopRight = 2,
    kCornerBottomRight = 4,
    kCornerBottomLeft = 8,
    kCornerTop = kCornerTopLeft | kCornerTopRight,
    kCornerBottom = kCornerBottomLeft | kCornerBottomRight,
    kCornerLeft = kCornerTopLeft | kCornerBottomLeft,
    kCornerRight = kCornerTopRight | kCornerBottomRight,
    kCornerAll = 15,
};

enum Layer : int {
    kLayerBase = 0,
    kLayerFloating = 1,
    kLayerPopup = 2,
    kLayerTooltip = 3,
    kLayerCount = 4,
};

struct Vertex {
    float x;
    float y;
    float u;
    float v;
    Color color;
};

struct Command {
    Rect clip;
    std::uint32_t index_offset;
    std::uint32_t index_count;
};

class DrawList {
public:
    void reset(Vec2 display);
    void set_alpha(float alpha);
    [[nodiscard]] float alpha() const { return alpha_; }

    void push_clip(const Rect& rect);
    void pop_clip();
    [[nodiscard]] const Rect& clip() const;

    void set_layer(int layer);
    [[nodiscard]] int layer() const { return layer_; }

    void rect(const Rect& area, Color color, float radius = 0.0f, unsigned corners = kCornerAll);
    void rect_outline(const Rect& area, Color color, float thickness = 1.0f, float radius = 0.0f,
                      unsigned corners = kCornerAll);
    void rect_gradient(const Rect& area, Color top, Color bottom);
    void rect_gradient_x(const Rect& area, Color left, Color right);
    void shadow(const Rect& area, float radius, float spread, Color color);
    void line(Vec2 from, Vec2 to, Color color, float thickness = 1.0f);
    void polyline(const Vec2* points, int count, Color color, float thickness, bool closed = false);
    void triangle(Vec2 a, Vec2 b, Vec2 c, Color color);
    void circle(Vec2 center, float radius, Color color);
    void arc(Vec2 center, float radius, float from, float to, Color color, float thickness);
    void image(const Rect& area, float u0, float v0, float u1, float v1, Color color);

    void text(Vec2 position, Color color, std::string_view value);
    void text_tracked(Vec2 position, Color color, std::string_view value, float tracking);
    void text_clipped(Vec2 position, Color color, std::string_view value, float max_width);
    void text_spans(Vec2 position, Color color, Color highlight, std::string_view value,
                    const std::uint32_t* spans, int span_count, float max_width);

    void finish();
    [[nodiscard]] bool rejected(const Rect& area) const;

    [[nodiscard]] const std::vector<Vertex>& vertices() const { return vertices_; }
    [[nodiscard]] const std::vector<std::uint32_t>& indices() const { return indices_; }
    [[nodiscard]] const std::vector<Command>& commands() const { return commands_; }
    [[nodiscard]] Vec2 display() const { return display_; }

private:
    struct Channel {
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<Command> commands;
        std::vector<Rect> clips;
        std::uint32_t emitted = 0;
    };

    Channel& active() { return channels_[layer_]; }
    [[nodiscard]] const Channel& active() const { return channels_[layer_]; }

    void quad(const Rect& area, float u0, float v0, float u1, float v1, Color color);
    void quad_gradient(const Rect& area, Color a, Color b, bool horizontal);
    void fill_convex(const Vec2* points, int count, Color color, float feather = 1.0f);
    void rounded_path(const Rect& area, float radius, unsigned corners);
    void flush();

    std::vector<Vertex> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<Command> commands_;
    Channel channels_[kLayerCount];
    std::vector<Vec2> path_;
    std::vector<Vec2> scratch_;
    int layer_ = kLayerBase;
    float alpha_ = 1.0f;
    Vec2 display_;
};

}
