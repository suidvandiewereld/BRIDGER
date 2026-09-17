#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "bridger/api.h"
#include "fx/image.h"
#include "fx/math.h"

namespace bridger::fx {

using Handle = BridgerFxHandle;

constexpr std::size_t kMaxConstants = 4096;
constexpr unsigned kUserSlots = 4;

struct GpuTexture {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
    unsigned width = 0;
    unsigned height = 0;
    unsigned mips = 1;
    unsigned srv = ~0u;
    unsigned rtv = ~0u;
    unsigned dsv = ~0u;

    [[nodiscard]] bool valid() const { return resource != nullptr; }
};

struct Shader {
    Handle handle = 0;
    std::string name;
    std::string owner;
    BridgerFxShaderKind kind = BRIDGER_FX_SHADER_FULLSCREEN;
    std::string source;
    std::filesystem::path path;
    std::string vs_entry;
    std::string ps_entry;
    std::vector<std::string> defines;
    std::vector<std::filesystem::path> watched;
    std::filesystem::file_time_type stamp{};
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    std::string log;
    bool ready = false;
    bool dirty = true;
    bool builtin = false;
    unsigned generation = 0;
    unsigned compiles = 0;
    unsigned failures = 0;
    double last_compile_ms = 0.0;
};

struct Texture {
    Handle handle = 0;
    std::string name;
    std::string owner;
    bool is_target = false;
    float scale = 0.0f;
    unsigned width = 0;
    unsigned height = 0;
    BridgerFxFormat format = BRIDGER_FX_FORMAT_RGBA8;
    bool with_depth = false;
    Image pending;
    bool has_pending = false;
    std::filesystem::path path;
    GpuTexture color;
    GpuTexture depth;
    bool failed = false;
    std::string error;
};

struct Mesh {
    Handle handle = 0;
    std::string owner;
    std::vector<BridgerFxVertex> vertices;
    std::vector<std::uint32_t> indices;
    ID3D12Resource* vertex_buffer = nullptr;
    ID3D12Resource* index_buffer = nullptr;
    unsigned vertex_count = 0;
    unsigned index_count = 0;
    bool uploaded = false;
};

struct MeshDraw {
    Handle mesh = 0;
    float model[16]{};
    float tint[4]{};
};

struct Effect {
    Handle handle = 0;
    std::string name;
    std::string owner;
    bool is_pass = false;
    BridgerFxStage stage = BRIDGER_FX_STAGE_POST;
    int priority = 0;
    Handle shader = 0;
    Handle output = 0;
    bool enabled = true;
    BridgerFxBlend blend = BRIDGER_FX_BLEND_OPAQUE;
    BridgerFxDepth depth = BRIDGER_FX_DEPTH_NONE;
    BridgerFxCull cull = BRIDGER_FX_CULL_NONE;
    BridgerFxTopology topology = BRIDGER_FX_TOPOLOGY_TRIANGLES;
    BridgerFxFill fill = BRIDGER_FX_FILL_SOLID;
    BridgerFxSpace space = BRIDGER_FX_SPACE_WORLD;
    bool clear_output = false;
    float clear_color[4]{};
    bool wants_history = false;
    std::vector<std::uint8_t> constants;
    Handle textures[kUserSlots]{};
    std::vector<BridgerFxVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<MeshDraw> mesh_draws;
    float gpu_ms = 0.0f;
    unsigned draw_calls = 0;
    unsigned vertex_total = 0;
    bool ran = false;
    std::string problem;
};

struct Settings {
    bool fov_horizontal = true;
    bool depth_capture = false;
    bool depth_reversed = true;
    int depth_candidate = -1;
    bool hot_reload = true;
    bool bypass = false;
    bool timings = true;
    bool pre_upscale = false;
    int dlss_preset = 0;
    bool pipeline_hooks = true;
    bool pipeline_retain = true;
};

struct Stats {
    unsigned effects_run = 0;
    unsigned draw_calls = 0;
    unsigned vertices = 0;
    unsigned copies = 0;
    float cpu_ms = 0.0f;
    float gpu_ms = 0.0f;
    unsigned frame = 0;
    unsigned width = 0;
    unsigned height = 0;
    bool depth_captured = false;
    std::string depth_note;
    std::string upscale_note;
    unsigned upscale_draws = 0;
    unsigned upscale_jobs = 0;
    std::vector<std::string> upscale_inventory;
};

struct State {
    std::recursive_mutex mutex;
    Handle next_handle = 1;
    std::map<Handle, Shader> shaders;
    std::map<Handle, Texture> textures;
    std::map<Handle, Mesh> meshes;
    std::map<Handle, Effect> effects;
    Settings settings;
    Stats stats;
    BridgerFxFrame frame{};

    std::mutex camera_mutex;
    BridgerFxCamera engine_camera{};
    BridgerFxCamera override_camera{};
    bool has_override = false;
    unsigned camera_samples = 0;
    unsigned camera_faults = 0;
    const char* camera_source = "none";
};

State& state();

void save_settings();

namespace detail {
void destroy_shader_objects(Shader& shader);
void destroy_texture_objects(Texture& texture);
void destroy_mesh_objects(Mesh& mesh);
DXGI_FORMAT dxgi_format(BridgerFxFormat format);
const char* camera_source();
unsigned pipeline_count();
ID3D12Resource* texture_resource(Handle texture);
}

}
