#include "ui/raster.h"

#include <algorithm>
#include <cmath>

namespace bridger::ui::raster {
namespace {

struct Edge {
    double a = 0.0;
    double b = 0.0;
    double c = 0.0;
    bool top_left = false;
};

Edge make_edge(const Vertex& from, const Vertex& to) {
    Edge edge;
    edge.a = static_cast<double>(from.y) - to.y;
    edge.b = static_cast<double>(to.x) - from.x;
    edge.c = static_cast<double>(from.x) * to.y - static_cast<double>(from.y) * to.x;
    edge.top_left = (edge.a == 0.0 && edge.b < 0.0) || edge.a > 0.0;
    return edge;
}

bool inside(double w, const Edge& edge) {
    return w > 0.0 || (w == 0.0 && edge.top_left);
}

inline std::uint32_t channel(Color color, int shift) {
    return (color >> shift) & 0xFF;
}

void blend(std::uint32_t& target, std::uint32_t r, std::uint32_t g, std::uint32_t b,
           std::uint32_t alpha) {
    if (alpha == 0) {
        return;
    }
    if (alpha >= 255) {
        target = 0xFF000000u | (r << 16) | (g << 8) | b;
        return;
    }
    const std::uint32_t inverse = 255 - alpha;
    const std::uint32_t tr = (target >> 16) & 0xFF;
    const std::uint32_t tg = (target >> 8) & 0xFF;
    const std::uint32_t tb = target & 0xFF;
    const std::uint32_t nr = (r * alpha + tr * inverse + 127) / 255;
    const std::uint32_t ng = (g * alpha + tg * inverse + 127) / 255;
    const std::uint32_t nb = (b * alpha + tb * inverse + 127) / 255;
    target = 0xFF000000u | (nr << 16) | (ng << 8) | nb;
}

void triangle(Vertex v0, Vertex v1, Vertex v2, const Rect& clip,
              const std::vector<std::uint8_t>& atlas, int atlas_size, std::uint32_t* pixels,
              int width, int height) {
    double area = (static_cast<double>(v1.x) - v0.x) * (static_cast<double>(v2.y) - v0.y)
                - (static_cast<double>(v2.x) - v0.x) * (static_cast<double>(v1.y) - v0.y);
    if (std::abs(area) < 1.0e-9) {
        return;
    }
    if (area < 0.0) {
        std::swap(v1, v2);
        area = -area;
    }

    const int x0 = std::max({0, static_cast<int>(std::floor(std::max(clip.x0, std::min({v0.x, v1.x, v2.x}))))});
    const int x1 = std::min({width, static_cast<int>(std::ceil(std::min(clip.x1, std::max({v0.x, v1.x, v2.x}))))});
    const int y0 = std::max({0, static_cast<int>(std::floor(std::max(clip.y0, std::min({v0.y, v1.y, v2.y}))))});
    const int y1 = std::min({height, static_cast<int>(std::ceil(std::min(clip.y1, std::max({v0.y, v1.y, v2.y}))))});
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    const int cx0 = std::max(x0, static_cast<int>(std::ceil(clip.x0 - 0.5f)));
    const int cx1 = std::min(x1, static_cast<int>(std::ceil(clip.x1 - 0.5f)));
    const int cy0 = std::max(y0, static_cast<int>(std::ceil(clip.y0 - 0.5f)));
    const int cy1 = std::min(y1, static_cast<int>(std::ceil(clip.y1 - 0.5f)));

    const Edge e0 = make_edge(v1, v2);
    const Edge e1 = make_edge(v2, v0);
    const Edge e2 = make_edge(v0, v1);

    const bool flat_color = v0.color == v1.color && v1.color == v2.color;
    const bool flat_uv = v0.u == v1.u && v1.u == v2.u && v0.v == v1.v && v1.v == v2.v;
    const bool textured = !atlas.empty() && atlas_size > 0;
    const auto sample = [&](double u, double v) -> std::uint32_t {
        if (!textured) {
            return 255;
        }
        const double fx = u * atlas_size - 0.5;
        const double fy = v * atlas_size - 0.5;
        const double x0f = std::floor(fx);
        const double y0f = std::floor(fy);
        const double ax = fx - x0f;
        const double ay = fy - y0f;
        const int last = atlas_size - 1;
        const int tx0 = std::clamp(static_cast<int>(x0f), 0, last);
        const int ty0 = std::clamp(static_cast<int>(y0f), 0, last);
        const int tx1 = std::clamp(static_cast<int>(x0f) + 1, 0, last);
        const int ty1 = std::clamp(static_cast<int>(y0f) + 1, 0, last);
        const auto at = [&](int x, int y) {
            return static_cast<double>(atlas[static_cast<std::size_t>(y) * atlas_size + x]);
        };
        const double top = at(tx0, ty0) + (at(tx1, ty0) - at(tx0, ty0)) * ax;
        const double bottom = at(tx0, ty1) + (at(tx1, ty1) - at(tx0, ty1)) * ax;
        return static_cast<std::uint32_t>(std::clamp(top + (bottom - top) * ay + 0.5, 0.0, 255.0));
    };
    const std::uint32_t flat_coverage = flat_uv ? sample(v0.u, v0.v) : 255;
    if (flat_uv && flat_coverage == 0) {
        return;
    }

    for (int y = cy0; y < cy1; ++y) {
        const double py = y + 0.5;
        std::uint32_t* row = pixels + static_cast<std::size_t>(y) * width;
        for (int x = cx0; x < cx1; ++x) {
            const double px = x + 0.5;
            const double w0 = e0.a * px + e0.b * py + e0.c;
            if (!inside(w0, e0)) {
                continue;
            }
            const double w1 = e1.a * px + e1.b * py + e1.c;
            if (!inside(w1, e1)) {
                continue;
            }
            const double w2 = e2.a * px + e2.b * py + e2.c;
            if (!inside(w2, e2)) {
                continue;
            }

            std::uint32_t coverage = flat_coverage;
            if (!flat_uv) {
                const double b0 = w0 / area;
                const double b1 = w1 / area;
                const double b2 = w2 / area;
                coverage = sample(v0.u * b0 + v1.u * b1 + v2.u * b2,
                                  v0.v * b0 + v1.v * b1 + v2.v * b2);
                if (coverage == 0) {
                    continue;
                }
            }

            std::uint32_t r;
            std::uint32_t g;
            std::uint32_t b;
            std::uint32_t a;
            if (flat_color) {
                r = channel(v0.color, 0);
                g = channel(v0.color, 8);
                b = channel(v0.color, 16);
                a = channel(v0.color, 24);
            } else {
                const double b0 = w0 / area;
                const double b1 = w1 / area;
                const double b2 = w2 / area;
                const auto mixed = [&](int shift) {
                    return static_cast<std::uint32_t>(
                        std::clamp(channel(v0.color, shift) * b0 + channel(v1.color, shift) * b1
                                       + channel(v2.color, shift) * b2 + 0.5,
                                   0.0, 255.0));
                };
                r = mixed(0);
                g = mixed(8);
                b = mixed(16);
                a = mixed(24);
            }
            blend(row[x], r, g, b, (a * coverage + 127) / 255);
        }
    }
}

}

void render(const DrawList& list, const std::vector<std::uint8_t>& atlas, int atlas_size,
            std::uint32_t* pixels, int width, int height) {
    const auto& vertices = list.vertices();
    const auto& indices = list.indices();
    for (const auto& command : list.commands()) {
        const std::uint32_t end = command.index_offset + command.index_count;
        for (std::uint32_t i = command.index_offset; i + 3 <= end && i + 3 <= indices.size(); i += 3) {
            triangle(vertices[indices[i]], vertices[indices[i + 1]], vertices[indices[i + 2]],
                     command.clip, atlas, atlas_size, pixels, width, height);
        }
    }
}

void clear(std::uint32_t* pixels, int width, int height, Color color) {
    const std::uint32_t value = 0xFF000000u | (channel(color, 0) << 16) | (channel(color, 8) << 8)
                              | channel(color, 16);
    std::fill(pixels, pixels + static_cast<std::size_t>(width) * height, value);
}

}
