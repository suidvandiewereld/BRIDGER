
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "fx/fx.h"
#include "script/runtime.h"
#include "ui/draw.h"
#include "ui/font.h"
#include "ui/input.h"
#include "ui/ui.h"

namespace {

constexpr int kWidth = 1180;
constexpr int kHeight = 860;

struct Pixel {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

std::vector<Pixel> g_frame(static_cast<std::size_t>(kWidth) * kHeight);

void clear(float r, float g, float b) {
    for (Pixel& pixel : g_frame) {
        pixel = {r, g, b};
    }
}

float edge(const bridger::ui::Vertex& a, const bridger::ui::Vertex& b, float x, float y) {
    return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}

void raster(const bridger::ui::Vertex& a, const bridger::ui::Vertex& b,
            const bridger::ui::Vertex& c, const bridger::ui::Rect& clip,
            const std::vector<std::uint8_t>& atlas, int atlas_size) {
    const float min_x = std::max({0.0f, clip.x0, std::min({a.x, b.x, c.x})});
    const float max_x = std::min({static_cast<float>(kWidth), clip.x1, std::max({a.x, b.x, c.x})});
    const float min_y = std::max({0.0f, clip.y0, std::min({a.y, b.y, c.y})});
    const float max_y = std::min({static_cast<float>(kHeight), clip.y1, std::max({a.y, b.y, c.y})});

    const float area = edge(a, b, c.x, c.y);
    if (std::abs(area) < 1.0e-6f) {
        return;
    }

    for (int y = static_cast<int>(min_y); y < static_cast<int>(std::ceil(max_y)); ++y) {
        for (int x = static_cast<int>(min_x); x < static_cast<int>(std::ceil(max_x)); ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            float w0 = edge(b, c, px, py) / area;
            float w1 = edge(c, a, px, py) / area;
            float w2 = edge(a, b, px, py) / area;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                continue;
            }

            const float u = a.u * w0 + b.u * w1 + c.u * w2;
            const float v = a.v * w0 + b.v * w1 + c.v * w2;
            float coverage = 1.0f;
            if (!atlas.empty()) {
                const int tx = std::clamp(static_cast<int>(u * atlas_size), 0, atlas_size - 1);
                const int ty = std::clamp(static_cast<int>(v * atlas_size), 0, atlas_size - 1);
                coverage = atlas[static_cast<std::size_t>(ty) * atlas_size + tx] / 255.0f;
            }

            const auto channel = [&](int shift) {
                return (((a.color >> shift) & 0xFF) * w0 + ((b.color >> shift) & 0xFF) * w1
                        + ((c.color >> shift) & 0xFF) * w2) / 255.0f;
            };
            const float alpha = channel(24) * coverage;
            if (alpha <= 0.0f) {
                continue;
            }
            Pixel& pixel = g_frame[static_cast<std::size_t>(y) * kWidth + x];
            pixel.r += (channel(0) - pixel.r) * alpha;
            pixel.g += (channel(8) - pixel.g) * alpha;
            pixel.b += (channel(16) - pixel.b) * alpha;
        }
    }
}

bool write_bmp(const char* path) {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        return false;
    }
    const int stride = (kWidth * 3 + 3) & ~3;
    const std::uint32_t image_size = static_cast<std::uint32_t>(stride) * kHeight;
    const std::uint32_t file_size = 54 + image_size;

    std::uint8_t header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    std::memcpy(header + 2, &file_size, 4);
    const std::uint32_t offset = 54;
    std::memcpy(header + 10, &offset, 4);
    const std::uint32_t info_size = 40;
    std::memcpy(header + 14, &info_size, 4);
    const std::int32_t width = kWidth;
    const std::int32_t height = kHeight;
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &height, 4);
    const std::uint16_t planes = 1;
    const std::uint16_t bits = 24;
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bits, 2);
    std::memcpy(header + 34, &image_size, 4);
    std::fwrite(header, 1, sizeof header, file);

    std::vector<std::uint8_t> row(static_cast<std::size_t>(stride), 0);
    for (int y = kHeight - 1; y >= 0; --y) {
        for (int x = 0; x < kWidth; ++x) {
            const Pixel& pixel = g_frame[static_cast<std::size_t>(y) * kWidth + x];
            const auto to_byte = [](float value) {
                return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            row[static_cast<std::size_t>(x) * 3 + 0] = to_byte(pixel.b);
            row[static_cast<std::size_t>(x) * 3 + 1] = to_byte(pixel.g);
            row[static_cast<std::size_t>(x) * 3 + 2] = to_byte(pixel.r);
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
    return true;
}

void draw_sample_panel() {
    namespace ui = bridger::ui;
    static bool free_camera = true;
    static bool first_person = false;
    static float speed = 8.0f;
    static bool wasd = false;
    static float eye_height = 1.72f;
    static float eye_forward = 0.18f;
    static int tap_frames = 5;
    static int mode = 1;
    static std::uint32_t key_free = 0x72;
    static std::uint32_t key_first = 0x75;
    static std::string helper = "HeadHelper";
    static const char* const modes[] = {"Off", "Balanced", "Aggressive"};

    ui::setting_bool("Free camera", free_camera, "Detaches the camera from the player.");
    ui::setting_bool("First person", first_person, "Puts the camera at Sam's head.");

    if (ui::begin_group("Movement")) {
        ui::setting_float("Speed", speed, 0.25f, 100.0f, "m/s",
                          "Shift multiplies by four, control divides by four.");
        ui::revert_marker(true);
        ui::setting_bool("Accept WASD and QE", wasd,
                         "Convenient, but the same keys still walk Sam around.");
        if (ui::button("Snap to game camera", 190.0f)) {
        }
        ui::same_line();
        if (ui::ghost_button("Halve", 80.0f)) {
        }
        ui::same_line();
        if (ui::ghost_button("Double", 80.0f)) {
        }
    }
    ui::end_group();

    if (ui::begin_group("Keys")) {
        ui::setting_key("Free camera", key_free);
        ui::setting_key("First person", key_first);
        ui::note("Movement keys are fixed: numpad 8/5 forward and back, 4/6 strafe, "
                 "7/9 down and up, arrows to look.");
    }
    ui::end_group();

    if (ui::begin_group("Eye position")) {
        ui::setting_float("Eye height", eye_height, 1.0f, 2.2f, "m",
                          "Metres above the player entity's origin.");
        ui::setting_float("Forward of face", eye_forward, -0.1f, 0.4f, "m");
        ui::revert_marker(true);
        ui::setting_int("Tap length", tap_frames, 1, 30, "frames");
        ui::setting_combo("Turn style", mode, modes, 3, "Chooses how the body follows the view.");
        ui::setting_text("Head helper", helper, "helper name");
    }
    ui::end_group();

    if (ui::begin_group("Diagnostics")) {
        ui::readout("camera entity", "0x00000212f4a01b80");
        ui::readout("game camera", "-1842.4  118.9  2201.6");
        ui::readout("freecam angles", "137.0 / -12.4 deg");
        ui::readout_colored("recorder", ui::theme().good, "recording  marker 3");
        ui::readout("frame", "6.9 ms");
    }
    ui::end_group();
}

}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "ui_preview.bmp";
    const float hover_x = argc > 3 ? std::strtof(argv[2], nullptr) : 640.0f;
    const float hover_y = argc > 3 ? std::strtof(argv[3], nullptr) : 402.0f;
    const std::string mode = argc > 4 ? argv[4] : "";
    const bool to_bottom = mode == "bottom" || mode == "combo";
    const bool click = mode == "combo";
    const bool shaders = mode.rfind("shaders", 0) == 0;
    const bool shaders_bottom = mode == "shaders_bottom";
    const bool script = mode.rfind("script", 0) == 0;
    const auto colon = mode.find(':');
    const float wheel =
        colon != std::string::npos ? std::strtof(mode.c_str() + colon + 1, nullptr) : 0.0f;

    namespace ui = bridger::ui;
    ui::font::register_file(BRIDGER_SOURCE_DIR "/assets/Bridges-Black.ttf");
    if (!ui::font::build(L"Segoe UI")) {
        std::printf("warning: font atlas built with missing glyphs\n");
    }

    ui::input::on_mouse_move(hover_x, hover_y);

    for (int frame = 0; frame < 6; ++frame) {
        if (click && frame == 3) {
            ui::input::on_mouse_button(true);
        }
        if (click && frame == 4) {
            ui::input::on_mouse_button(false);
        }
        if (wheel != 0.0f && frame > 0) {
            ui::input::on_wheel(-wheel);
        }
        ui::begin_frame({static_cast<float>(kWidth), static_cast<float>(kHeight)}, 1.0f);

        ui::Rect window{40.0f, 40.0f, kWidth - 40.0f, kHeight - 40.0f};
        bool open = true;
        ui::window_status("F1 CLOSE     2 MODS LOADED     18342 TYPES     5120 SYMBOLS");
        ui::begin_window("bridger", window, open);

        const char* const tabs[] = {"Mods",   "Log",     "Types",   "Symbols", "Inspect",
                                    "Panels", "Shaders", "Script"};
        for (int i = 0; i < 8; ++i) {
            ui::window_tab(tabs[i], i == (script ? 7 : shaders ? 6 : 5));
        }

        if (script || shaders) {
            if (shaders_bottom && frame > 0) {
                ui::scroll_to_end("fx_panel");
            }
            if (script) {
                bridger::script::draw_panel();
            } else {
                const auto page = mode.find('@');
                if (page != std::string::npos) {
                    bridger::fx::select_panel_page(std::atoi(mode.c_str() + page + 1));
                }
                bridger::fx::draw_panel();
            }
            ui::end_window();
            ui::end_frame();
            continue;
        }

        const auto& t = ui::theme();
        const float height = ui::remaining_height();
        const ui::Rect body = ui::window_body();
        ui::draw().rect({body.x0, body.y0, body.x0 + t.rail_width, body.y1}, t.window_low);
        ui::draw().rect({body.x0 + t.rail_width, body.y0, body.x0 + t.rail_width + 1.0f, body.y1},
                        t.hairline);
        const float rail = t.rail_width - t.padding * 2.0f;
        ui::begin_column(rail, height);
        {
            const ui::font::ScopedFace caption(ui::font::Face::Caption);
            ui::tracked(t.text_faint, "Camera", t.tracking);
        }
        ui::selectable("Free Camera", true, rail - 14.0f);
        ui::spacing(14.0f);
        {
            const ui::font::ScopedFace caption(ui::font::Face::Caption);
            ui::tracked(t.text_faint, "Hello Bridger", t.tracking);
        }
        ui::selectable("Hello", false, rail - 14.0f);
        ui::end_column();
        ui::set_cursor_x(t.rail_width);

        ui::begin_column(ui::available_width(), height);
        const ui::Vec2 top = ui::cursor();
        {
            const ui::font::ScopedFace title(ui::font::Face::Title);
            ui::text_colored(t.text, "Free Camera");
        }
        {
            const ui::font::ScopedFace micro(ui::font::Face::Micro);
            ui::text_colored(t.text_faint, "freecam   v2.3.0   Marquis   18 saved");
        }
        const ui::Vec2 below = ui::cursor();
        static std::string filter;
        const float field_y = top.y + ((below.y - top.y) - t.row_height - 8.0f - t.spacing) * 0.5f;
        ui::set_cursor_y(field_y);
        ui::align_right(260.0f);
        ui::input_text("panel_filter", filter, 260.0f, "search settings");
        ui::set_cursor_y(below.y + 10.0f);

        if (to_bottom && frame > 0) {
            ui::scroll_to_end("panel_page");
        }
        ui::begin_scroll("panel_page", ui::remaining_height());
        ui::begin_settings();
        draw_sample_panel();
        ui::end_settings();
        ui::end_scroll();
        ui::end_column();

        ui::newline();
        ui::end_window();
        ui::end_frame();
    }

    clear(0.26f, 0.27f, 0.29f);

    const ui::DrawList& list = ui::draw();
    const auto& atlas = ui::font::atlas();
    const int atlas_size = ui::font::atlas_size();
    for (const auto& command : list.commands()) {
        for (std::uint32_t i = 0; i < command.index_count; i += 3) {
            const auto base = command.index_offset + i;
            raster(list.vertices()[list.indices()[base]],
                   list.vertices()[list.indices()[base + 1]],
                   list.vertices()[list.indices()[base + 2]], command.clip, atlas, atlas_size);
        }
    }

    if (!write_bmp(path)) {
        std::printf("could not write %s\n", path);
        return 1;
    }
    std::printf("wrote %s  (%zu vertices, %zu indices, %zu commands)\n", path,
                list.vertices().size(), list.indices().size(), list.commands().size());
    return 0;
}
