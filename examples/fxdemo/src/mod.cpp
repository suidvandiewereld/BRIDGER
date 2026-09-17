
#include "bridger/mod.hpp"

#include <Windows.h>

#include <cmath>
#include <vector>

BRIDGER_MOD("fxdemo", "Shader Demo", "1.0.0", "Bridger",
            "Tour of the shader system: bloom, colour grade, world grid and markers, screen scanner.")

namespace {

namespace fx = bridger::fx;

bridger::Setting<bool> s_grade{"grade.enabled", "Colour grade", true};
bridger::Setting<float> s_exposure{"grade.exposure", "Exposure", 0.0f, "Stops."};
bridger::Setting<float> s_saturation{"grade.saturation", "Saturation", 1.0f};
bridger::Setting<float> s_temperature{"grade.temperature", "Temperature", 0.0f, "Cool to warm."};
bridger::Setting<float> s_vignette{"grade.vignette", "Vignette", 0.35f};
bridger::Setting<float> s_grain{"grade.grain", "Grain", 0.0f,
                                "Per-pixel noise, re-rolled every frame. Any non-zero value "
                                "shimmers by design; that is what grain is."};
bridger::Setting<float> s_fog{"grade.fog", "Depth fog", 0.0f,
                              "Needs 'Capture game depth' in the Shaders tab."};
bridger::Setting<float> s_fog_start{"grade.fog_start", "Fog start", 20.0f, "Metres."};

bridger::Setting<bool> s_bloom{"bloom.enabled", "Bloom", true};
bridger::Setting<float> s_bloom_threshold{"bloom.threshold", "Threshold", 0.8f,
                                          "Luminance where the glow starts, with a soft knee "
                                          "below it."};
bridger::Setting<float> s_bloom_intensity{"bloom.intensity", "Intensity", 0.35f};
bridger::Setting<float> s_bloom_radius{"bloom.radius", "Radius", 1.5f};

bridger::Setting<bool> s_grid{"world.grid", "Ground grid", true,
                              "A 1 m grid at roughly foot height around the camera. If it slides "
                              "against the terrain when the camera turns, flip 'FOV is "
                              "horizontal' in the Shaders tab."};
bridger::Setting<bool> s_pre_grid{"world.pre_upscale", "Pre upscale grid", false,
    "Hard edged grid before DLSS. Also enable Pre upscale effects in Shaders."};
bridger::Setting<float> s_grid_extent{"world.grid_extent", "Grid extent", 25.0f, "Metres."};
bridger::Setting<float> s_ground_offset{"world.ground", "Ground offset", 1.75f,
                                        "Metres below the camera the grid is drawn."};
bridger::Setting<bool> s_markers{"world.markers", "Markers", true};
bridger::Setting<bool> s_scanner{"screen.scanner", "Scanner", false};
bridger::Setting<float> s_scanner_radius{"screen.scanner_radius", "Scanner radius", 0.25f};
bridger::Setting<int> s_key_drop{"key.drop", "Drop marker", VK_F7};
bridger::Setting<int> s_key_scanner{"key.scanner", "Toggle scanner", VK_F8};

struct alignas(16) GradeConstants {
    float exposure;
    float saturation;
    float temperature;
    float vignette;
    float grain;
    float fog_density;
    float fog_start;
    float unused;
    float fog_color[4];
};

struct alignas(16) BloomConstants {
    float threshold;
    float intensity;
    float radius;
    float unused;
    float direction[2];
    float unused2[2];
};

struct alignas(16) MarkerConstants {
    float pulse;
    float rim;
    float unused[2];
    float color[4];
};

struct alignas(16) ScannerConstants {
    float radius;
    float strength;
    float unused[2];
    float center[2];
    float unused2[2];
};

fx::Shader g_grade_shader;
fx::Shader g_extract_shader;
fx::Shader g_blur_shader;
fx::Shader g_combine_shader;
fx::Shader g_marker_shader;
fx::Shader g_scanner_shader;

fx::RenderTarget g_bright;
fx::RenderTarget g_blur_a;
fx::RenderTarget g_blur_b;

fx::Pass g_extract;
fx::Pass g_blur_h;
fx::Pass g_blur_v;
fx::Pass g_combine;
fx::Pass g_grade;

fx::DrawList g_grid_list;
fx::DrawList g_marker_list;
fx::DrawList g_wire_list;
fx::DrawList g_scanner_list;
fx::Mesh g_cube;

struct Marker {
    fx::Vec3d position;
    float hue;
};
std::vector<Marker> g_markers;
unsigned g_frames = 0;

std::uint32_t hue_color(float hue, float alpha = 1.0f) {
    const float h = std::fmod(hue, 1.0f) * 6.0f;
    const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (h < 1) { r = 1; g = x; } else if (h < 2) { r = x; g = 1; } else if (h < 3) { g = 1; b = x; }
    else if (h < 4) { g = x; b = 1; } else if (h < 5) { r = x; b = 1; } else { r = 1; b = x; }
    return fx::rgbaf(0.25f + 0.75f * r, 0.25f + 0.75f * g, 0.25f + 0.75f * b, alpha);
}

void drop_marker() {
    const auto camera = fx::camera();
    if (!camera.valid) {
        bridger::warn("no camera yet, nothing dropped");
        return;
    }
    Marker marker;
    marker.position = fx::Vec3d{camera.position[0], camera.position[1], camera.position[2]}
                    + fx::Vec3{camera.forward[0], camera.forward[1], 0.0f} * 3.0f
                    + fx::Vec3{0.0f, 0.0f, -s_ground_offset.get() + 0.5f};
    marker.hue = static_cast<float>(g_markers.size()) * 0.137f;
    g_markers.push_back(marker);
    bridger::info("marker {} dropped at {:.1f} {:.1f} {:.1f}", g_markers.size(), marker.position.x,
                  marker.position.y, marker.position.z);
}

void toggle_scanner() {
    s_scanner.set(!s_scanner.get());
}

void push_constants() {
    GradeConstants grade{};
    grade.exposure = s_exposure;
    grade.saturation = s_saturation;
    grade.temperature = s_temperature;
    grade.vignette = s_vignette;
    grade.grain = s_grain;
    grade.fog_density = s_fog;
    grade.fog_start = s_fog_start;
    grade.fog_color[0] = 0.62f;
    grade.fog_color[1] = 0.68f;
    grade.fog_color[2] = 0.74f;
    grade.fog_color[3] = 1.0f;
    g_grade.constants(grade);
    g_grade.enable(s_grade);

    BloomConstants bloom{};
    bloom.threshold = s_bloom_threshold;
    bloom.intensity = s_bloom_intensity;
    bloom.radius = s_bloom_radius;
    g_extract.constants(bloom);
    g_combine.constants(bloom);
    bloom.direction[0] = 1.0f;
    g_blur_h.constants(bloom);
    bloom.direction[0] = 0.0f;
    bloom.direction[1] = 1.0f;
    g_blur_v.constants(bloom);
    const bool bloom_on = s_bloom;
    g_extract.enable(bloom_on);
    g_blur_h.enable(bloom_on);
    g_blur_v.enable(bloom_on);
    g_combine.enable(bloom_on);

    MarkerConstants marker{};
    marker.pulse = 1.0f;
    marker.rim = 0.8f;
    marker.color[0] = 0.5f;
    marker.color[1] = 0.9f;
    marker.color[2] = 1.0f;
    marker.color[3] = 1.0f;
    g_marker_list.constants(marker);

    ScannerConstants scanner{};
    scanner.radius = s_scanner_radius;
    scanner.strength = 1.0f;
    scanner.center[0] = 0.5f;
    scanner.center[1] = 0.5f;
    g_scanner_list.constants(scanner);
    g_scanner_list.enable(s_scanner);
}

void on_frame(float) {
    ++g_frames;
    push_constants();
    g_grid_list.order(s_pre_grid ? BRIDGER_FX_STAGE_PRE_UPSCALE : BRIDGER_FX_STAGE_SCENE, 0);

    const fx::Frame frame = fx::frame();
    if (frame.camera.valid) {
        const fx::Vec3 eye = fx::to_float({frame.camera.position[0], frame.camera.position[1],
                                           frame.camera.position[2]});
        const fx::Vec3 forward{frame.camera.forward[0], frame.camera.forward[1], frame.camera.forward[2]};
        const float ground = eye.z - s_ground_offset.get();

        if (s_grid) {
            const fx::Vec3 center{eye.x, eye.y, ground};
            if (s_pre_grid) {
                const float extent = s_grid_extent;
                const float half_width = 0.012f;
                const auto color = fx::rgba(120, 200, 230, 255);
                for (float x = std::floor(center.x - extent); x <= center.x + extent; x += 1.0f)
                    g_grid_list.quad({x-half_width, center.y-extent, ground}, {x+half_width, center.y-extent, ground},
                        {x+half_width, center.y+extent, ground}, {x-half_width, center.y+extent, ground}, color);
                for (float y = std::floor(center.y - extent); y <= center.y + extent; y += 1.0f)
                    g_grid_list.quad({center.x-extent, y-half_width, ground}, {center.x+extent, y-half_width, ground},
                        {center.x+extent, y+half_width, ground}, {center.x-extent, y+half_width, ground}, color);
            } else {
            g_grid_list.grid(center, s_grid_extent, 1.0f, fx::rgba(120, 200, 230, 70), 1.5f);
            g_grid_list.grid(center, s_grid_extent, 5.0f, fx::rgba(120, 200, 230, 140), 2.5f);
            const fx::Vec3 ahead = center + fx::Vec3{forward.x, forward.y, 0.0f} * 4.0f;
            g_grid_list.axes(ahead, 1.0f, 3.0f);
            }
        }

        if (s_markers) {
            for (const auto& marker : g_markers) {
                const fx::Vec3 p = fx::to_float(marker.position);
                const float spin = frame.time * 45.0f;
                const float bob = 0.15f * std::sin(frame.time * 2.0f + marker.hue * 20.0f);
                const fx::Mat4 model = fx::Mat4::translation(p + fx::Vec3{0.0f, 0.0f, bob})
                                     * fx::Mat4::rotation({0.0f, 0.0f, 1.0f}, spin)
                                     * fx::Mat4::scale(0.5f);
                g_marker_list.mesh(g_cube, model, hue_color(marker.hue));
                g_marker_list.sphere(p + fx::Vec3{0.0f, 0.0f, 1.2f + bob}, 0.18f,
                                     hue_color(marker.hue + 0.5f), 12);
                g_wire_list.box(p - fx::Vec3{0.6f, 0.6f, 0.6f}, p + fx::Vec3{0.6f, 0.6f, 0.6f},
                                hue_color(marker.hue, 0.8f), 2.0f);
                g_wire_list.line(p - fx::Vec3{0.0f, 0.0f, 0.5f}, {p.x, p.y, ground},
                                 fx::rgba(255, 255, 255, 120), 1.5f);
                g_wire_list.circle({p.x, p.y, ground + 0.02f}, {0.0f, 0.0f, 1.0f}, 1.0f,
                                   hue_color(marker.hue, 0.6f), 2.0f);
            }
        }
    }

    if (s_scanner) {
        g_scanner_list.rect(0.0f, 0.0f, static_cast<float>(frame.width),
                            static_cast<float>(frame.height), 0xffffffffu);
    }
}

void draw_status(const char* label, const fx::Shader& shader) {
    namespace ui = bridger::ui;
    if (shader.ready()) {
        ui::readout(label, ui::good(), "ok");
    } else {
        ui::readout(label, ui::bad(), "failed, see the Shaders tab");
    }
}

void draw() {
    namespace ui = bridger::ui;

    if (ui::begin_group("Colour grade")) {
        ui::row(s_grade);
        ui::row(s_exposure, -3.0f, 3.0f, "ev");
        ui::row(s_saturation, 0.0f, 2.0f);
        ui::row(s_temperature, -1.0f, 1.0f);
        ui::row(s_vignette, 0.0f, 1.0f);
        ui::row(s_grain, 0.0f, 0.2f);
        ui::row(s_fog, 0.0f, 0.1f, "/m");
        ui::row(s_fog_start, 0.0f, 200.0f, "m");
        ui::note("Fog reads the game's depth buffer: turn on 'Capture game depth' in the Shaders tab.");
    }
    ui::end_group();

    if (ui::begin_group("Bloom")) {
        ui::row(s_bloom);
        ui::row(s_bloom_threshold, 0.0f, 2.0f);
        ui::row(s_bloom_intensity, 0.0f, 2.0f);
        ui::row(s_bloom_radius, 0.5f, 4.0f, "px");
        ui::note("Four passes: bright extract into a quarter-res target, two blurs, combine. "
                 "The extract uses a 13-tap Karis-weighted downsample, which is what keeps "
                 "small bright features from flickering as the camera moves.");
    }
    ui::end_group();

    if (ui::begin_group("World")) {
        ui::row(s_grid);
        ui::row(s_pre_grid);
        ui::row(s_grid_extent, 5.0f, 100.0f, "m");
        ui::row(s_ground_offset, 0.0f, 4.0f, "m");
        ui::row(s_markers);
        if (ui::key_row(s_key_drop)) {
            bridger::rebind(drop_marker, static_cast<unsigned>(s_key_drop.get()));
        }
        if (ui::button("Drop marker", 150.0f)) {
            drop_marker();
        }
        ui::same_line(8.0f);
        if (ui::ghost_button("Clear markers", 150.0f)) {
            g_markers.clear();
        }
        ui::newline();
        ui::readoutf("markers", "{}", g_markers.size());
    }
    ui::end_group();

    if (ui::begin_group("Screen")) {
        ui::row(s_scanner);
        ui::row(s_scanner_radius, 0.05f, 0.6f);
        if (ui::key_row(s_key_scanner)) {
            bridger::rebind(toggle_scanner, static_cast<unsigned>(s_key_scanner.get()));
        }
    }
    ui::end_group();

    if (ui::begin_group("Status", false)) {
        const auto frame = fx::frame();
        ui::readout("device", frame.device_ready ? ui::good() : ui::warn(),
                    frame.device_ready ? "ready" : "waiting");
        ui::readoutf("camera", "{}", frame.camera.valid ? "valid" : "none");
        ui::readoutf("game depth", "{}", frame.depth_available ? "captured" : "not captured");
        ui::readoutf("frames", "{}", g_frames);
        draw_status("grade.hlsl", g_grade_shader);
        draw_status("bloom_extract.hlsl", g_extract_shader);
        draw_status("bloom_blur.hlsl", g_blur_shader);
        draw_status("bloom_combine.hlsl", g_combine_shader);
        draw_status("marker.hlsl", g_marker_shader);
        draw_status("scanner.hlsl", g_scanner_shader);
        if (ui::ghost_button("Reload all shaders", 180.0f)) {
            for (fx::Shader* shader : {&g_grade_shader, &g_extract_shader, &g_blur_shader,
                                       &g_combine_shader, &g_marker_shader, &g_scanner_shader}) {
                shader->reload();
            }
        }
    }
    ui::end_group();
}

}

bool bridger::on_load() {
    if (fx::table() == nullptr) {
        bridger::error("this loader has no shader system");
        return false;
    }

    g_grade_shader.load(BRIDGER_FX_SHADER_FULLSCREEN, "shaders/grade.hlsl");
    g_extract_shader.load(BRIDGER_FX_SHADER_FULLSCREEN, "shaders/bloom_extract.hlsl");
    g_blur_shader.load(BRIDGER_FX_SHADER_FULLSCREEN, "shaders/bloom_blur.hlsl");
    g_combine_shader.load(BRIDGER_FX_SHADER_FULLSCREEN, "shaders/bloom_combine.hlsl");
    g_marker_shader.load(BRIDGER_FX_SHADER_WORLD, "shaders/marker.hlsl");
    g_scanner_shader.load(BRIDGER_FX_SHADER_SCREEN, "shaders/scanner.hlsl");

    g_bright.create(0.25f, BRIDGER_FX_FORMAT_RGBA16F, false, "fxdemo bright");
    g_blur_a.create(0.25f, BRIDGER_FX_FORMAT_RGBA16F, false, "fxdemo blur a");
    g_blur_b.create(0.25f, BRIDGER_FX_FORMAT_RGBA16F, false, "fxdemo blur b");

    fx::PassOptions extract;
    extract.priority = 0;
    extract.output = g_bright.handle();
    extract.clear_output = true;
    g_extract.create("bloom extract", g_extract_shader, extract);

    fx::PassOptions blur_h;
    blur_h.priority = 1;
    blur_h.output = g_blur_a.handle();
    g_blur_h.create("bloom blur h", g_blur_shader, blur_h);
    g_blur_h.texture(0, g_bright);

    fx::PassOptions blur_v;
    blur_v.priority = 2;
    blur_v.output = g_blur_b.handle();
    g_blur_v.create("bloom blur v", g_blur_shader, blur_v);
    g_blur_v.texture(0, g_blur_a);

    fx::PassOptions combine;
    combine.priority = 3;
    g_combine.create("bloom combine", g_combine_shader, combine);
    g_combine.texture(0, g_blur_b);

    fx::PassOptions grade;
    grade.priority = 20;
    g_grade.create("colour grade", g_grade_shader, grade);

    fx::DrawListOptions grid;
    grid.blend = BRIDGER_FX_BLEND_ALPHA;
    g_grid_list.create("ground grid", BRIDGER_FX_SPACE_WORLD, grid);

    fx::DrawListOptions markers;
    markers.shader = g_marker_shader.handle();
    markers.depth = BRIDGER_FX_DEPTH_TEST_WRITE;
    markers.cull = BRIDGER_FX_CULL_BACK;
    markers.blend = BRIDGER_FX_BLEND_OPAQUE;
    markers.priority = 10;
    g_marker_list.create("markers", BRIDGER_FX_SPACE_WORLD, markers);

    fx::DrawListOptions wire;
    wire.depth = BRIDGER_FX_DEPTH_TEST;
    wire.priority = 11;
    g_wire_list.create("marker outlines", BRIDGER_FX_SPACE_WORLD, wire);

    fx::DrawListOptions scanner;
    scanner.shader = g_scanner_shader.handle();
    scanner.stage = BRIDGER_FX_STAGE_OVERLAY;
    scanner.blend = BRIDGER_FX_BLEND_ALPHA;
    g_scanner_list.create("scanner", BRIDGER_FX_SPACE_SCREEN, scanner);

    std::vector<fx::Vertex> vertices;
    std::vector<std::uint32_t> indices;
    fx::geometry::box(vertices, indices, {1.0f, 1.0f, 1.0f});
    g_cube.create(vertices, indices);

    bridger::hotkey(static_cast<unsigned>(s_key_drop.get()), drop_marker);
    bridger::hotkey(static_cast<unsigned>(s_key_scanner.get()), toggle_scanner);
    bridger::tick(on_frame);
    bridger::ui::panel("Shader Demo", draw);
    bridger::info("fxdemo loaded, shaders in {}", fx::directory());
    return true;
}

void bridger::on_unload() {
    g_markers.clear();
}
