#include "ui/draw.h"

#include <algorithm>
#include <cmath>

#include "ui/font.h"

namespace bridger::ui {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kHalfPi = 1.57079633f;

float clamp01(float value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

Vec2 miter(Vec2 a, Vec2 b) {
    Vec2 out{a.x + b.x, a.y + b.y};
    const float square = out.x * out.x + out.y * out.y;
    if (square > 1.0e-6f) {
        float scale = 1.0f / square;
        if (scale > 4.0f) {
            scale = 4.0f;
        }
        out.x *= scale;
        out.y *= scale;
    }
    return out;
}

}

Color with_alpha(Color color, float alpha) {
    const auto current = static_cast<float>((color >> 24) & 0xFF);
    const auto scaled = static_cast<std::uint32_t>(current * clamp01(alpha));
    return (color & 0x00FFFFFFu) | (scaled << 24);
}

Color mix(Color from, Color to, float amount) {
    const float t = clamp01(amount);
    Color out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const auto a = static_cast<float>((from >> shift) & 0xFF);
        const auto b = static_cast<float>((to >> shift) & 0xFF);
        const auto value = static_cast<std::uint32_t>(a + (b - a) * t);
        out |= (value & 0xFF) << shift;
    }
    return out;
}

void DrawList::reset(Vec2 display) {
    vertices_.clear();
    indices_.clear();
    commands_.clear();
    for (Channel& channel : channels_) {
        channel.vertices.clear();
        channel.indices.clear();
        channel.commands.clear();
        channel.clips.clear();
        channel.clips.push_back({0.0f, 0.0f, display.x, display.y});
        channel.emitted = 0;
    }
    layer_ = kLayerBase;
    alpha_ = 1.0f;
    display_ = display;
}

void DrawList::set_alpha(float value) {
    alpha_ = clamp01(value);
}

const Rect& DrawList::clip() const {
    return active().clips.back();
}

void DrawList::set_layer(int layer) {
    flush();
    layer_ = std::clamp(layer, 0, static_cast<int>(kLayerCount) - 1);
}

void DrawList::flush() {
    Channel& channel = active();
    const auto total = static_cast<std::uint32_t>(channel.indices.size());
    if (total > channel.emitted) {
        channel.commands.push_back({channel.clips.back(), channel.emitted, total - channel.emitted});
        channel.emitted = total;
    }
}

void DrawList::push_clip(const Rect& area) {
    flush();
    Channel& channel = active();
    channel.clips.push_back(area.clipped(channel.clips.back()));
}

void DrawList::pop_clip() {
    flush();
    Channel& channel = active();
    if (channel.clips.size() > 1) {
        channel.clips.pop_back();
    }
}

bool DrawList::rejected(const Rect& area) const {
    const Rect& current = active().clips.back();
    return area.x1 <= current.x0 || area.x0 >= current.x1
        || area.y1 <= current.y0 || area.y0 >= current.y1;
}

void DrawList::quad(const Rect& area, float u0, float v0, float u1, float v1, Color color) {
    if (rejected(area)) {
        return;
    }
    Channel& channel = active();
    const auto base = static_cast<std::uint32_t>(channel.vertices.size());
    const Color tint = with_alpha(color, alpha_);
    channel.vertices.push_back({area.x0, area.y0, u0, v0, tint});
    channel.vertices.push_back({area.x1, area.y0, u1, v0, tint});
    channel.vertices.push_back({area.x1, area.y1, u1, v1, tint});
    channel.vertices.push_back({area.x0, area.y1, u0, v1, tint});
    for (const std::uint32_t offset : {0u, 1u, 2u, 0u, 2u, 3u}) {
        channel.indices.push_back(base + offset);
    }
}

void DrawList::quad_gradient(const Rect& area, Color from, Color to, bool horizontal) {
    if (rejected(area)) {
        return;
    }
    Channel& channel = active();
    const auto white = font::white_pixel();
    const auto base = static_cast<std::uint32_t>(channel.vertices.size());
    const Color a = with_alpha(from, alpha_);
    const Color b = with_alpha(to, alpha_);
    channel.vertices.push_back({area.x0, area.y0, white.x, white.y, a});
    channel.vertices.push_back({area.x1, area.y0, white.x, white.y, horizontal ? b : a});
    channel.vertices.push_back({area.x1, area.y1, white.x, white.y, b});
    channel.vertices.push_back({area.x0, area.y1, white.x, white.y, horizontal ? a : b});
    for (const std::uint32_t offset : {0u, 1u, 2u, 0u, 2u, 3u}) {
        channel.indices.push_back(base + offset);
    }
}

void DrawList::fill_convex(const Vec2* points, int count, Color color, float feather) {
    if (points == nullptr || count < 3 || (color >> 24) == 0) {
        return;
    }

    Rect bounds{points[0].x, points[0].y, points[0].x, points[0].y};
    for (int i = 1; i < count; ++i) {
        bounds.x0 = std::min(bounds.x0, points[i].x);
        bounds.y0 = std::min(bounds.y0, points[i].y);
        bounds.x1 = std::max(bounds.x1, points[i].x);
        bounds.y1 = std::max(bounds.y1, points[i].y);
    }
    if (rejected(bounds.expand(feather))) {
        return;
    }

    float area2 = 0.0f;
    for (int i = 0; i < count; ++i) {
        const int j = (i + 1) % count;
        area2 += points[i].x * points[j].y - points[j].x * points[i].y;
    }
    const float sign = area2 < 0.0f ? -1.0f : 1.0f;

    scratch_.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int j = (i + 1) % count;
        float dx = points[j].x - points[i].x;
        float dy = points[j].y - points[i].y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length > 1.0e-6f) {
            dx /= length;
            dy /= length;
        } else {
            dx = 0.0f;
            dy = 0.0f;
        }
        scratch_[static_cast<std::size_t>(i)] = {dy * sign, -dx * sign};
    }

    Channel& channel = active();
    const auto white = font::white_pixel();
    const Color tint = with_alpha(color, alpha_);
    const Color clear = tint & 0x00FFFFFFu;
    const auto base = static_cast<std::uint32_t>(channel.vertices.size());
    const float reach = feather * 0.5f;

    channel.vertices.reserve(channel.vertices.size() + static_cast<std::size_t>(count) * 2);
    channel.indices.reserve(channel.indices.size() + static_cast<std::size_t>(count) * 6
                            + static_cast<std::size_t>(count - 2) * 3);

    for (int i = 0; i < count; ++i) {
        const int previous = (i + count - 1) % count;
        const Vec2 normal = miter(scratch_[static_cast<std::size_t>(previous)],
                                  scratch_[static_cast<std::size_t>(i)]);
        channel.vertices.push_back({points[i].x - normal.x * reach, points[i].y - normal.y * reach,
                                    white.x, white.y, tint});
        channel.vertices.push_back({points[i].x + normal.x * reach, points[i].y + normal.y * reach,
                                    white.x, white.y, clear});
    }

    for (int i = 2; i < count; ++i) {
        channel.indices.push_back(base);
        channel.indices.push_back(base + static_cast<std::uint32_t>(i - 1) * 2);
        channel.indices.push_back(base + static_cast<std::uint32_t>(i) * 2);
    }
    for (int i = 0; i < count; ++i) {
        const int j = (i + 1) % count;
        const std::uint32_t inner = base + static_cast<std::uint32_t>(i) * 2;
        const std::uint32_t next = base + static_cast<std::uint32_t>(j) * 2;
        channel.indices.push_back(inner);
        channel.indices.push_back(inner + 1);
        channel.indices.push_back(next + 1);
        channel.indices.push_back(inner);
        channel.indices.push_back(next + 1);
        channel.indices.push_back(next);
    }
}

void DrawList::rounded_path(const Rect& area, float radius, unsigned corners) {
    path_.clear();
    if (radius <= 0.5f || corners == kCornerNone) {
        path_.push_back({area.x0, area.y0});
        path_.push_back({area.x1, area.y0});
        path_.push_back({area.x1, area.y1});
        path_.push_back({area.x0, area.y1});
        return;
    }

    const int segments = std::clamp(static_cast<int>(std::ceil(radius * 0.65f)) + 1, 3, 10);
    const float centers[4][2] = {
        {area.x0 + radius, area.y0 + radius},
        {area.x1 - radius, area.y0 + radius},
        {area.x1 - radius, area.y1 - radius},
        {area.x0 + radius, area.y1 - radius},
    };
    const float starts[4] = {kPi, kPi + kHalfPi, 0.0f, kHalfPi};
    const unsigned flags[4] = {kCornerTopLeft, kCornerTopRight, kCornerBottomRight,
                               kCornerBottomLeft};
    const Vec2 sharp[4] = {{area.x0, area.y0}, {area.x1, area.y0},
                           {area.x1, area.y1}, {area.x0, area.y1}};

    for (int corner = 0; corner < 4; ++corner) {
        if ((corners & flags[corner]) == 0) {
            path_.push_back(sharp[corner]);
            continue;
        }
        for (int step = 0; step <= segments; ++step) {
            const float angle =
                starts[corner] + kHalfPi * static_cast<float>(step) / static_cast<float>(segments);
            path_.push_back({centers[corner][0] + std::cos(angle) * radius,
                             centers[corner][1] + std::sin(angle) * radius});
        }
    }
}

void DrawList::rect(const Rect& area, Color color, float radius, unsigned corners) {
    if (area.width() <= 0.0f || area.height() <= 0.0f || (color >> 24) == 0) {
        return;
    }
    const float limit = std::min(area.width(), area.height()) * 0.5f;
    const float r = std::min(radius, limit);
    if (r <= 0.5f || corners == kCornerNone) {
        const auto white = font::white_pixel();
        quad(area, white.x, white.y, white.x, white.y, color);
        return;
    }
    rounded_path(area, r, corners);
    fill_convex(path_.data(), static_cast<int>(path_.size()), color, 1.0f);
}

void DrawList::rect_outline(const Rect& area, Color color, float thickness, float radius,
                            unsigned corners) {
    if (area.width() <= 0.0f || area.height() <= 0.0f || (color >> 24) == 0) {
        return;
    }
    const float limit = std::min(area.width(), area.height()) * 0.5f;
    const float r = std::min(radius, limit);
    if (r <= 0.5f || corners == kCornerNone) {
        rect({area.x0, area.y0, area.x1, area.y0 + thickness}, color);
        rect({area.x0, area.y1 - thickness, area.x1, area.y1}, color);
        rect({area.x0, area.y0 + thickness, area.x0 + thickness, area.y1 - thickness}, color);
        rect({area.x1 - thickness, area.y0 + thickness, area.x1, area.y1 - thickness}, color);
        return;
    }
    const float half = thickness * 0.5f;
    rounded_path(area.inset(half), std::max(0.5f, r - half), corners);
    polyline(path_.data(), static_cast<int>(path_.size()), color, thickness, true);
}

void DrawList::rect_gradient(const Rect& area, Color top, Color bottom) {
    if (area.width() <= 0.0f || area.height() <= 0.0f) {
        return;
    }
    quad_gradient(area, top, bottom, false);
}

void DrawList::rect_gradient_x(const Rect& area, Color left, Color right) {
    if (area.width() <= 0.0f || area.height() <= 0.0f) {
        return;
    }
    quad_gradient(area, left, right, true);
}

void DrawList::shadow(const Rect& area, float radius, float spread, Color color) {
    if (spread <= 0.5f || (color >> 24) == 0 || area.empty()) {
        return;
    }
    const float scales[3] = {1.0f, 0.55f, 0.24f};
    const float weights[3] = {0.34f, 0.40f, 0.55f};
    for (int pass = 0; pass < 3; ++pass) {
        const float feather = spread * scales[pass];
        if (feather < 0.75f) {
            continue;
        }
        const Rect core = area.expand(feather * 0.5f);
        rounded_path(core, radius + feather * 0.5f, kCornerAll);
        fill_convex(path_.data(), static_cast<int>(path_.size()),
                    with_alpha(color, weights[pass]), feather);
    }
}

void DrawList::polyline(const Vec2* points, int count, Color color, float thickness, bool closed) {
    if (points == nullptr || count < 2 || (color >> 24) == 0) {
        return;
    }

    float weight = thickness;
    float fade = 1.0f;
    if (weight < 1.0f) {
        fade = std::max(0.06f, weight);
        weight = 1.0f;
    }
    const Color tint = with_alpha(color, alpha_ * fade);
    if ((tint >> 24) == 0) {
        return;
    }
    const Color clear = tint & 0x00FFFFFFu;

    Rect bounds{points[0].x, points[0].y, points[0].x, points[0].y};
    for (int i = 1; i < count; ++i) {
        bounds.x0 = std::min(bounds.x0, points[i].x);
        bounds.y0 = std::min(bounds.y0, points[i].y);
        bounds.x1 = std::max(bounds.x1, points[i].x);
        bounds.y1 = std::max(bounds.y1, points[i].y);
    }
    if (rejected(bounds.expand(weight + 1.0f))) {
        return;
    }

    const int segments = closed ? count : count - 1;
    scratch_.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < segments; ++i) {
        const int j = (i + 1) % count;
        float dx = points[j].x - points[i].x;
        float dy = points[j].y - points[i].y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length > 1.0e-6f) {
            dx /= length;
            dy /= length;
        } else {
            dx = 0.0f;
            dy = 0.0f;
        }
        scratch_[static_cast<std::size_t>(i)] = {dy, -dx};
    }
    if (!closed) {
        scratch_[static_cast<std::size_t>(count - 1)] =
            scratch_[static_cast<std::size_t>(segments - 1)];
    }

    Channel& channel = active();
    const auto white = font::white_pixel();
    const auto base = static_cast<std::uint32_t>(channel.vertices.size());
    const float core = std::max(0.0f, (weight - 1.0f) * 0.5f);
    const float edge = core + 1.0f;

    channel.vertices.reserve(channel.vertices.size() + static_cast<std::size_t>(count) * 4);
    channel.indices.reserve(channel.indices.size() + static_cast<std::size_t>(segments) * 18);

    for (int i = 0; i < count; ++i) {
        Vec2 normal = scratch_[static_cast<std::size_t>(i)];
        const bool joined = closed || (i > 0 && i < count - 1);
        if (joined) {
            const int previous = (i + count - 1) % count;
            normal = miter(scratch_[static_cast<std::size_t>(previous)], normal);
        }
        channel.vertices.push_back({points[i].x + normal.x * edge, points[i].y + normal.y * edge,
                                    white.x, white.y, clear});
        channel.vertices.push_back({points[i].x + normal.x * core, points[i].y + normal.y * core,
                                    white.x, white.y, tint});
        channel.vertices.push_back({points[i].x - normal.x * core, points[i].y - normal.y * core,
                                    white.x, white.y, tint});
        channel.vertices.push_back({points[i].x - normal.x * edge, points[i].y - normal.y * edge,
                                    white.x, white.y, clear});
    }

    for (int i = 0; i < segments; ++i) {
        const int j = (i + 1) % count;
        const std::uint32_t a = base + static_cast<std::uint32_t>(i) * 4;
        const std::uint32_t b = base + static_cast<std::uint32_t>(j) * 4;
        const std::uint32_t strip[3][2] = {{a, b}, {a + 1, b + 1}, {a + 2, b + 2}};
        for (const auto& pair : strip) {
            channel.indices.push_back(pair[0]);
            channel.indices.push_back(pair[0] + 1);
            channel.indices.push_back(pair[1] + 1);
            channel.indices.push_back(pair[0]);
            channel.indices.push_back(pair[1] + 1);
            channel.indices.push_back(pair[1]);
        }
    }
}

void DrawList::line(Vec2 from, Vec2 to, Color color, float thickness) {
    const Vec2 points[2] = {from, to};
    polyline(points, 2, color, thickness, false);
}

void DrawList::triangle(Vec2 a, Vec2 b, Vec2 c, Color color) {
    const Vec2 points[3] = {a, b, c};
    fill_convex(points, 3, color, 1.0f);
}

void DrawList::circle(Vec2 center, float radius, Color color) {
    if (radius <= 0.0f) {
        return;
    }
    const int segments = std::clamp(static_cast<int>(std::ceil(radius * 1.6f)) + 4, 8, 48);
    path_.clear();
    for (int i = 0; i < segments; ++i) {
        const float angle = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(segments);
        path_.push_back({center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius});
    }
    fill_convex(path_.data(), static_cast<int>(path_.size()), color, 1.0f);
}

void DrawList::arc(Vec2 center, float radius, float from, float to, Color color, float thickness) {
    if (radius <= 0.0f || std::abs(to - from) < 1.0e-4f) {
        return;
    }
    const float sweep = std::abs(to - from);
    const int segments = std::clamp(static_cast<int>(std::ceil(sweep * radius * 0.35f)) + 2, 3, 64);
    path_.clear();
    for (int i = 0; i <= segments; ++i) {
        const float angle =
            from + (to - from) * static_cast<float>(i) / static_cast<float>(segments);
        path_.push_back({center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius});
    }
    polyline(path_.data(), static_cast<int>(path_.size()), color, thickness, false);
}

void DrawList::image(const Rect& area, float u0, float v0, float u1, float v1, Color color) {
    if (area.width() <= 0.0f || area.height() <= 0.0f || (color >> 24) == 0) {
        return;
    }
    quad(area, u0, v0, u1, v1, color);
}

namespace {

Rect glyph_area(float pen, float baseline_top, const font::Glyph& glyph) {
    const float x = std::round(pen + glyph.bearing_x);
    const float y = std::round(baseline_top + glyph.bearing_y);
    return {x, y, x + glyph.width, y + glyph.height};
}

}

void DrawList::text(Vec2 position, Color color, std::string_view value) {
    text_clipped(position, color, value, 1.0e9f);
}

void DrawList::text_clipped(Vec2 position, Color color, std::string_view value, float max_width) {
    text_spans(position, color, color, value, nullptr, 0, max_width);
}

void DrawList::text_spans(Vec2 position, Color color, Color highlight, std::string_view value,
                          const std::uint32_t* spans, int span_count, float max_width) {
    const Rect line_bounds{position.x, position.y, position.x + max_width,
                           position.y + font::line_height()};
    if (rejected(line_bounds) || (color >> 24) == 0) {
        return;
    }

    const bool truncated = font::measure(value) > max_width;
    float limit = position.x + max_width;
    float ellipsis = 0.0f;
    if (truncated) {
        ellipsis = font::measure("...");
        limit -= ellipsis;
    }

    float pen = position.x;
    int span = 0;
    std::size_t index = 0;
    for (const char raw : value) {
        const auto code = static_cast<unsigned char>(raw);
        const font::Glyph* glyph = font::lookup(code);
        if (glyph == nullptr) {
            ++index;
            continue;
        }
        if (pen + glyph->advance > limit) {
            break;
        }
        while (span < span_count && spans[span] < index) {
            ++span;
        }
        const bool marked = span < span_count && spans[span] == index;
        if (glyph->width > 0.0f && glyph->height > 0.0f) {
            const Rect area = glyph_area(pen, position.y, *glyph);
            quad(area, glyph->u0, glyph->v0, glyph->u1, glyph->v1, marked ? highlight : color);
        }
        pen += glyph->advance;
        ++index;
    }

    if (truncated && ellipsis > 0.0f) {
        for (int dot = 0; dot < 3; ++dot) {
            const font::Glyph* glyph = font::lookup(static_cast<unsigned char>('.'));
            if (glyph == nullptr) {
                break;
            }
            if (glyph->width > 0.0f && glyph->height > 0.0f) {
                const Rect area = glyph_area(pen, position.y, *glyph);
                quad(area, glyph->u0, glyph->v0, glyph->u1, glyph->v1, color);
            }
            pen += glyph->advance;
        }
    }
}

void DrawList::text_tracked(Vec2 position, Color color, std::string_view value, float tracking) {
    const Rect line_bounds{position.x, position.y,
                           position.x + font::measure_tracked(value, tracking),
                           position.y + font::line_height()};
    if (rejected(line_bounds) || (color >> 24) == 0) {
        return;
    }
    float pen = position.x;
    for (const char raw : value) {
        const auto code = static_cast<unsigned char>(raw);
        const font::Glyph* glyph = font::lookup(code);
        if (glyph == nullptr) {
            continue;
        }
        if (glyph->width > 0.0f && glyph->height > 0.0f) {
            const Rect area = glyph_area(pen, position.y, *glyph);
            quad(area, glyph->u0, glyph->v0, glyph->u1, glyph->v1, color);
        }
        pen += glyph->advance + tracking;
    }
}

void DrawList::finish() {
    const int current = layer_;
    for (int index = 0; index < kLayerCount; ++index) {
        layer_ = index;
        flush();
    }
    layer_ = current;

    vertices_.clear();
    indices_.clear();
    commands_.clear();
    for (const Channel& channel : channels_) {
        if (channel.indices.empty()) {
            continue;
        }
        const auto vertex_base = static_cast<std::uint32_t>(vertices_.size());
        const auto index_base = static_cast<std::uint32_t>(indices_.size());
        vertices_.insert(vertices_.end(), channel.vertices.begin(), channel.vertices.end());
        indices_.reserve(indices_.size() + channel.indices.size());
        for (const std::uint32_t index : channel.indices) {
            indices_.push_back(index + vertex_base);
        }
        for (const Command& command : channel.commands) {
            commands_.push_back({command.clip, command.index_offset + index_base,
                                 command.index_count});
        }
    }
}

}
