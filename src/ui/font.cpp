#include "ui/font.h"

#include <Windows.h>

#include <algorithm>
#include <array>

namespace bridger::ui::font {
namespace {

constexpr int kAtlasSize = 1024;
constexpr unsigned char kFirst = 32;
constexpr unsigned char kLast = 126;
constexpr int kPadding = 1;
constexpr int kFaceCount = static_cast<int>(Face::Count);

struct FaceSpec {
    int height;
    int weight;
    bool monospace;
    const wchar_t* family = nullptr;
};

constexpr FaceSpec kFaceSpecs[kFaceCount] = {
    {12, 400, false},
    {12, 600, false},
    {15, 400, false},
    {15, 600, false},
    {20, 600, false},
    {13, 400, true},
    {24, 400, false, L"Bridges Black"},
};

struct FaceData {
    std::array<Glyph, 256> glyphs{};
    std::array<bool, 256> present{};
    float line_height = 0.0f;
    float ascent = 0.0f;
};

std::vector<std::uint8_t> g_atlas;
std::array<FaceData, kFaceCount> g_faces{};
int g_pen_x = 0;
int g_pen_y = 0;
int g_row_height = 0;
Vec2 g_white{};
thread_local Face g_current = Face::Body;
bool g_ready = false;

FaceData& data(Face face) {
    return g_faces[static_cast<int>(face)];
}

void blit(int x, int y, int width, int height, const std::uint8_t* source, int pitch) {
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const int value = source[row * pitch + column];
            const int scaled = value >= 64 ? 255 : value * 255 / 64;
            g_atlas[static_cast<std::size_t>(y + row) * kAtlasSize + (x + column)] =
                static_cast<std::uint8_t>(scaled);
        }
    }
}

bool build_face(HDC dc, const wchar_t* family, const FaceSpec& spec, FaceData& out) {
    const wchar_t* name = spec.monospace ? L"Consolas" : spec.family != nullptr ? spec.family : family;
    const DWORD family_pitch =
        spec.monospace ? (FIXED_PITCH | FF_MODERN) : (DEFAULT_PITCH | FF_DONTCARE);
    const HFONT handle = CreateFontW(-spec.height, 0, 0, 0, spec.weight, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, family_pitch, name);
    if (handle == nullptr) {
        return false;
    }
    const auto previous = static_cast<HFONT>(SelectObject(dc, handle));

    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    out.line_height = static_cast<float>(metrics.tmHeight);
    out.ascent = static_cast<float>(metrics.tmAscent);

    const MAT2 identity{{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    std::vector<std::uint8_t> scratch;
    bool ok = true;

    for (unsigned char code = kFirst; code <= kLast; ++code) {
        GLYPHMETRICS gm{};
        const auto wide = static_cast<wchar_t>(code);
        const DWORD size = GetGlyphOutlineW(dc, wide, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &identity);
        if (size == GDI_ERROR) {
            ok = false;
            continue;
        }

        Glyph glyph;
        glyph.advance = static_cast<float>(gm.gmCellIncX);
        glyph.bearing_x = static_cast<float>(gm.gmptGlyphOrigin.x);
        glyph.bearing_y = out.ascent - static_cast<float>(gm.gmptGlyphOrigin.y);
        glyph.width = static_cast<float>(gm.gmBlackBoxX);
        glyph.height = static_cast<float>(gm.gmBlackBoxY);

        if (size > 0 && gm.gmBlackBoxX > 0 && gm.gmBlackBoxY > 0) {
            scratch.assign(size, 0);
            if (GetGlyphOutlineW(dc, wide, GGO_GRAY8_BITMAP, &gm, size, scratch.data(), &identity)
                    == GDI_ERROR) {
                ok = false;
                continue;
            }
            const int width = static_cast<int>(gm.gmBlackBoxX);
            const int height = static_cast<int>(gm.gmBlackBoxY);
            const int pitch = (width + 3) & ~3;

            if (g_pen_x + width + kPadding >= kAtlasSize) {
                g_pen_x = kPadding;
                g_pen_y += g_row_height + kPadding;
                g_row_height = 0;
            }
            if (g_pen_y + height + kPadding >= kAtlasSize) {
                ok = false;
                break;
            }

            blit(g_pen_x, g_pen_y, width, height, scratch.data(), pitch);
            glyph.u0 = static_cast<float>(g_pen_x) / kAtlasSize;
            glyph.v0 = static_cast<float>(g_pen_y) / kAtlasSize;
            glyph.u1 = static_cast<float>(g_pen_x + width) / kAtlasSize;
            glyph.v1 = static_cast<float>(g_pen_y + height) / kAtlasSize;

            g_pen_x += width + kPadding;
            g_row_height = std::max(g_row_height, height);
        }

        out.glyphs[code] = glyph;
        out.present[code] = true;
    }

    SelectObject(dc, previous);
    DeleteObject(handle);
    return ok;
}

}

bool register_file(const wchar_t* path) {
    return AddFontResourceExW(path, FR_PRIVATE, nullptr) > 0;
}

bool build(const wchar_t* family) {
    g_atlas.assign(static_cast<std::size_t>(kAtlasSize) * kAtlasSize, 0);
    g_faces = {};
    g_ready = false;

    const HDC screen = GetDC(nullptr);
    const HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (dc == nullptr) {
        return false;
    }

    g_pen_x = kPadding;
    g_pen_y = kPadding;
    g_row_height = 2;

    g_atlas[static_cast<std::size_t>(g_pen_y) * kAtlasSize + g_pen_x] = 255;
    g_atlas[static_cast<std::size_t>(g_pen_y) * kAtlasSize + g_pen_x + 1] = 255;
    g_atlas[static_cast<std::size_t>(g_pen_y + 1) * kAtlasSize + g_pen_x] = 255;
    g_atlas[static_cast<std::size_t>(g_pen_y + 1) * kAtlasSize + g_pen_x + 1] = 255;
    g_white = {(static_cast<float>(g_pen_x) + 0.5f) / kAtlasSize,
               (static_cast<float>(g_pen_y) + 0.5f) / kAtlasSize};
    g_pen_x += 2 + kPadding;

    bool ok = true;
    for (int index = 0; index < kFaceCount; ++index) {
        const bool built = build_face(dc, family, kFaceSpecs[index], g_faces[index]);
        ok &= built || static_cast<Face>(index) == Face::Brand;
    }

    DeleteDC(dc);
    g_current = Face::Body;
    g_ready = true;
    return ok;
}

Image place_image(const std::uint8_t* pixels, int width, int height) {
    Image image;
    if (pixels == nullptr || width <= 0 || height <= 0 || width + 2 * kPadding > kAtlasSize) {
        return image;
    }

    if (g_pen_x + width + kPadding >= kAtlasSize) {
        g_pen_x = kPadding;
        g_pen_y += g_row_height + kPadding;
        g_row_height = 0;
    }
    if (g_pen_y + height + kPadding >= kAtlasSize) {
        return image;
    }

    for (int row = 0; row < height; ++row) {
        std::copy_n(pixels + static_cast<std::size_t>(row) * width, width,
                    g_atlas.begin() + static_cast<std::size_t>(g_pen_y + row) * kAtlasSize + g_pen_x);
    }

    image.u0 = static_cast<float>(g_pen_x) / kAtlasSize;
    image.v0 = static_cast<float>(g_pen_y) / kAtlasSize;
    image.u1 = static_cast<float>(g_pen_x + width) / kAtlasSize;
    image.v1 = static_cast<float>(g_pen_y + height) / kAtlasSize;
    image.width = width;
    image.height = height;
    image.valid = true;

    g_pen_x += width + kPadding;
    g_row_height = std::max(g_row_height, height);
    return image;
}

void set_face(Face next) {
    g_current = next;
}

Face face() {
    return g_current;
}

const Glyph* lookup(unsigned char code) {
    FaceData& current = data(g_current);
    if (!g_ready) {
        return nullptr;
    }
    if (!current.present[code]) {
        const auto space = static_cast<unsigned char>(' ');
        return current.present[space] ? &current.glyphs[space] : nullptr;
    }
    return &current.glyphs[code];
}

Vec2 white_pixel() {
    return g_white;
}

const std::vector<std::uint8_t>& atlas() {
    return g_atlas;
}

int atlas_size() {
    return kAtlasSize;
}

float line_height() {
    return data(g_current).line_height;
}

float line_height(Face which) {
    return data(which).line_height;
}

float ascent() {
    return data(g_current).ascent;
}

float measure(std::string_view text) {
    float total = 0.0f;
    for (const char raw : text) {
        if (const Glyph* glyph = lookup(static_cast<unsigned char>(raw)); glyph != nullptr) {
            total += glyph->advance;
        }
    }
    return total;
}

float measure(Face which, std::string_view text) {
    const ScopedFace scoped(which);
    return measure(text);
}

float measure_tracked(std::string_view text, float tracking) {
    if (text.empty()) {
        return 0.0f;
    }
    return measure(text) + tracking * static_cast<float>(text.size() - 1);
}

}
