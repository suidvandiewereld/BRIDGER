
#include "fx/depth_capture.h"
#include "fx/fx_internal.h"
#include "fx/fx.h"
#include "fx/ngx.h"
#include "fx/pipeline.h"
#include "fx/readback.h"
#include "fx/rt.h"
#include "core/settings.h"

namespace bridger::fx {
namespace {

Shader make_shader(Handle handle, const char* name, const char* file, BridgerFxShaderKind kind,
                   unsigned compiles, unsigned failures, double ms, bool ready,
                   const char* log = "") {
    Shader shader;
    shader.handle = handle;
    shader.name = name;
    shader.owner = "fxdemo";
    shader.kind = kind;
    shader.path = std::filesystem::path("Bridger/mods/fxdemo/shaders")
                / file;
    shader.compiles = compiles;
    shader.failures = failures;
    shader.last_compile_ms = ms;
    shader.ready = ready;
    shader.dirty = false;
    shader.log = log;
    return shader;
}

Effect make_effect(Handle handle, const char* name, bool is_pass, BridgerFxStage stage, int priority,
                   bool enabled, bool ran, float gpu_ms, unsigned draws, unsigned verts,
                   BridgerFxSpace space = BRIDGER_FX_SPACE_WORLD, Handle output = 0,
                   const char* problem = "") {
    Effect effect;
    effect.handle = handle;
    effect.name = name;
    effect.owner = "fxdemo";
    effect.is_pass = is_pass;
    effect.stage = stage;
    effect.priority = priority;
    effect.enabled = enabled;
    effect.ran = ran;
    effect.gpu_ms = gpu_ms;
    effect.draw_calls = draws;
    effect.vertex_total = verts;
    effect.space = space;
    effect.output = output;
    effect.problem = problem;
    return effect;
}

Texture make_texture(Handle handle, const char* name, bool is_target, unsigned width,
                     unsigned height, float scale, bool with_depth) {
    Texture texture;
    texture.handle = handle;
    texture.name = name;
    texture.owner = "fxdemo";
    texture.is_target = is_target;
    texture.scale = scale;
    texture.color.resource = reinterpret_cast<ID3D12Resource*>(static_cast<std::uintptr_t>(handle));
    texture.color.width = width;
    texture.color.height = height;
    if (with_depth) {
        texture.depth.resource =
            reinterpret_cast<ID3D12Resource*>(static_cast<std::uintptr_t>(handle) + 0x1000);
    }
    return texture;
}

State& build() {
    static State s;
    static bool built = false;
    if (built) {
        return s;
    }
    built = true;

    s.settings.depth_capture = true;
    s.settings.depth_candidate = 0;
    s.stats.width = 2560;
    s.stats.height = 1440;
    s.stats.frame = 41207;
    s.stats.effects_run = 7;
    s.stats.draw_calls = 11;
    s.stats.vertices = 20448;
    s.stats.copies = 3;
    s.stats.cpu_ms = 0.31f;
    s.stats.gpu_ms = 1.42f;
    s.stats.depth_captured = true;
    s.stats.depth_note = "1707x960";

    s.frame.device_ready = true;
    s.frame.time = 412.8f;
    s.frame.camera.valid = true;
    s.frame.camera.position[0] = -1842.37;
    s.frame.camera.position[1] = 118.94;
    s.frame.camera.position[2] = 2201.61;
    s.frame.camera.forward[0] = -0.682f;
    s.frame.camera.forward[1] = 0.726f;
    s.frame.camera.forward[2] = -0.087f;
    s.frame.camera.fov_degrees = 84.0f;
    s.frame.camera.near_plane = 0.2f;
    s.camera_samples = 24681;

    s.shaders[1] = make_shader(1, "grade.hlsl", "grade.hlsl", BRIDGER_FX_SHADER_FULLSCREEN, 3, 0,
                               15.4, true);
    s.shaders[2] = make_shader(2, "bloom_extract.hlsl", "bloom_extract.hlsl",
                               BRIDGER_FX_SHADER_FULLSCREEN, 1, 0, 6.8, true);
    s.shaders[3] = make_shader(3, "bloom_blur.hlsl", "bloom_blur.hlsl",
                               BRIDGER_FX_SHADER_FULLSCREEN, 1, 0, 8.0, true);
    s.shaders[4] = make_shader(4, "bloom_combine.hlsl", "bloom_combine.hlsl",
                               BRIDGER_FX_SHADER_FULLSCREEN, 1, 0, 3.9, true);
    s.shaders[5] = make_shader(5, "marker.hlsl", "marker.hlsl", BRIDGER_FX_SHADER_WORLD, 4, 1, 7.4,
                               true, "marker.hlsl(18,14): warning X3206: implicit truncation of "
                                     "vector type");
    s.shaders[6] = make_shader(6, "scanner.hlsl", "scanner.hlsl", BRIDGER_FX_SHADER_SCREEN, 2, 1,
                               11.6, false,
                               "scanner.hlsl(31,9): error X3004: undeclared identifier 'ripple'");
    s.shaders[7].handle = 7;
    s.shaders[7].name = "builtin.world";
    s.shaders[7].builtin = true;
    s.shaders[7].ready = true;

    s.effects[10] = make_effect(10, "ground grid", false, BRIDGER_FX_STAGE_SCENE, 0, true, true,
                                0.21f, 1, 18432);
    s.effects[11] = make_effect(11, "markers", false, BRIDGER_FX_STAGE_SCENE, 10, true, true, 0.14f,
                                4, 1728);
    s.effects[12] = make_effect(12, "marker outlines", false, BRIDGER_FX_STAGE_SCENE, 11, true, true,
                                0.08f, 2, 288);
    s.effects[13] = make_effect(13, "bloom extract", true, BRIDGER_FX_STAGE_POST, 0, true, true,
                                0.19f, 1, 3, BRIDGER_FX_SPACE_WORLD, 30);
    s.effects[14] = make_effect(14, "bloom blur h", true, BRIDGER_FX_STAGE_POST, 1, true, true,
                                0.22f, 1, 3, BRIDGER_FX_SPACE_WORLD, 31);
    s.effects[15] = make_effect(15, "bloom blur v", true, BRIDGER_FX_STAGE_POST, 2, true, true,
                                0.23f, 1, 3, BRIDGER_FX_SPACE_WORLD, 32);
    s.effects[16] = make_effect(16, "bloom combine", true, BRIDGER_FX_STAGE_POST, 3, true, true,
                                0.27f, 1, 3);
    s.effects[17] = make_effect(17, "colour grade", true, BRIDGER_FX_STAGE_POST, 20, true, false,
                                0.0f, 0, 0, BRIDGER_FX_SPACE_WORLD, 0, "shader not compiled");
    s.effects[18] = make_effect(18, "scanner", false, BRIDGER_FX_STAGE_OVERLAY, 0, false, false,
                                0.0f, 0, 0, BRIDGER_FX_SPACE_SCREEN);

    s.textures[30] = make_texture(30, "fxdemo bright", true, 640, 360, 0.25f, false);
    s.textures[31] = make_texture(31, "fxdemo blur a", true, 640, 360, 0.25f, false);
    s.textures[32] = make_texture(32, "fxdemo blur b", true, 640, 360, 0.25f, false);
    s.textures[33] = make_texture(33, "noise.dds", false, 512, 512, 0.0f, false);
    s.meshes[40].handle = 40;
    return s;
}

}

State& state() {
    return build();
}

void dump_backbuffer() {}

void save_settings() {}

namespace detail {

const char* camera_source() {
    return "engine camera entity";
}

unsigned pipeline_count() {
    return 11;
}

}

namespace readback {

std::string last_message() { return "01-upscaler-input.bmp (1707x960)"; }
unsigned pending() { return 0; }
unsigned written() { return 3; }

}

namespace ngx {

Snapshot snapshot() {
    Snapshot s;
    s.hooked = true;
    s.seen = true;
    s.calls = 41207;
    s.frames_since = 0;
    s.render_width = 1707;
    s.render_height = 960;
    s.output_width = 2560;
    s.output_height = 1440;
    s.subrect_width = 1707;
    s.subrect_height = 960;
    s.jitter_x = -0.2341f;
    s.jitter_y = 0.4112f;
    s.mv_scale_x = -1707.0f;
    s.mv_scale_y = -960.0f;
    s.color_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    s.depth_format = DXGI_FORMAT_R32G8X24_TYPELESS;
    s.motion_format = DXGI_FORMAT_R16G16_FLOAT;
    s.output_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    s.module_name = "nvngx";
    s.layout = "reversed";
    return s;
}

void capture(unsigned) {}
void set_preset(Preset) {}
Preset preset() { return Preset::Default; }
bool preset_applied() { return false; }
const char* preset_status() { return "left to the game"; }
void dump_frame() {}
bool dump_pending() { return false; }
ID3D12Device* device() { return nullptr; }

}

namespace depth {

std::vector<Candidate> candidates() {
    Candidate upscaled;
    upscaled.width = 1707;
    upscaled.height = 960;
    upscaled.format = DXGI_FORMAT_R32G8X24_TYPELESS;
    upscaled.clears = 1;
    Candidate shadow;
    shadow.width = 4096;
    shadow.height = 4096;
    shadow.format = DXGI_FORMAT_D32_FLOAT;
    shadow.clears = 4;
    return {upscaled, shadow};
}

unsigned known_views() {
    return 14;
}

}


namespace pipeline {

Snapshot snapshot() {
    Snapshot s;
    s.hooks_installed = true;
    s.pipelines = 6212;
    s.graphics = 5890;
    s.compute = 322;
    s.blobs = 4107;
    s.retained_bytes = 61u * 1024u * 1024u;
    s.draws_last_frame = 3814;
    s.dispatches_last_frame = 96;
    s.used_last_frame = 412;
    s.hooked = 1;
    s.replaced = 137;
    s.material_active = true;
    s.material_owner = "materialhook";
    s.material_applied = 137;
    s.material_skipped = 3;
    s.stream_twins = 131;
    s.stream_draws_last_frame = 412;
    s.stream_skipped_last_frame = 3;
    return s;
}

std::vector<Row> busiest(unsigned limit) {
    std::vector<Row> rows;
    Row a; a.hash = 0x3f1c9a02be77d410ull; a.draws = 1210; a.render_targets = 4; a.rtv0 = DXGI_FORMAT_R8G8B8A8_UNORM; a.dsv = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; a.material = true; a.replaced = true;
    Row b; b.hash = 0x91b04c7e2d5aa3f8ull; b.draws = 640; b.render_targets = 1; b.rtv0 = DXGI_FORMAT_R16G16B16A16_FLOAT; b.hooked = true;
    Row c; c.hash = 0x0c77e1d9a4b25f61ull; c.draws = 12; c.compute = true;
    rows = {a, b, c};
    if (rows.size() > limit) rows.resize(limit);
    return rows;
}

std::string dump(std::uint64_t) { return {}; }
void set_enabled(bool) {}
bool enabled() { return true; }
void set_retain(bool) {}
bool retain() { return true; }

}

namespace rt {

Settings& settings() {
    static Settings s;
    return s;
}

Snapshot snapshot() {
    Snapshot s;
    s.boot = true;
    s.ready = true;
    s.detail = "traced and composited before NGX";
    s.raytracing_tier = 11;
    s.regions = 412;
    s.regions_skipped = 3;
    s.vertices = 2'184'960;
    s.budget_vertices = 6u << 20;
    s.blas_bytes = 141u * 1024u * 1024u;
    s.scratch_bytes = 96u * 1024u * 1024u;
    s.passes = 1830;
    s.width = 1707;
    s.height = 960;
    return s;
}

}

}

namespace bridger::settings {
void set_bool(std::string_view, std::string_view, bool) {}
void flush_now() {}
}
