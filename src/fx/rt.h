#pragma once

#include <Windows.h>
#include <d3d12.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "fx/math.h"

namespace bridger::fx::ngx { struct Snapshot; }

namespace bridger::fx::rt {

enum class Mode : int {
    AmbientOcclusion = 0,
    SunShadow = 1,
    DebugPrimary = 2,
};

struct Settings {
    bool enabled = true;
    int mode = 0;
    int rays = 4;
    float radius = 2.0f;
    float strength = 1.0f;
    float bias = 0.02f;
    float sun_azimuth = 135.0f;
    float sun_elevation = 45.0f;
};

void configure(const std::filesystem::path& root);
void save_settings();
[[nodiscard]] bool boot_enabled();
[[nodiscard]] Settings& settings();

void attach(ID3D12Device* device);

struct Region {
    D3D12_GPU_VIRTUAL_ADDRESS buffer = 0;
    UINT64 size = 0;
    D3D12_GPU_VIRTUAL_ADDRESS counter = 0;
};
bool allocate_region(std::uint64_t vertices, std::uint64_t pipeline, Region& out);

void render(ID3D12GraphicsCommandList* commands, const ngx::Snapshot& inputs,
            const math::Mat4& view, const math::Mat4& projection, bool camera_valid, bool depth_reversed);

struct Snapshot {
    bool boot = false;
    bool ready = false;
    const char* problem = "";
    std::string detail;
    unsigned raytracing_tier = 0;
    unsigned regions = 0;
    unsigned regions_skipped = 0;
    std::uint64_t vertices = 0;
    std::uint64_t budget_vertices = 0;
    std::uint64_t blas_bytes = 0;
    std::uint64_t scratch_bytes = 0;
    unsigned passes = 0;
    unsigned faults = 0;
    unsigned width = 0;
    unsigned height = 0;
};
[[nodiscard]] Snapshot snapshot();

struct RegionRecord {
    std::uint64_t offset = 0;
    std::uint64_t vertices = 0;
    unsigned counter = 0;
    std::uint64_t pipeline = 0;
};

struct TraceParams {
    math::Mat4 view;
    math::Mat4 projection;
    float camera[3]{};
    bool camera_valid = false;
    bool depth_reversed = true;
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    unsigned width = 0;
    unsigned height = 0;
    unsigned frame = 0;
    Settings settings;
};

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool initialise(ID3D12Device* device, const std::filesystem::path& dxc_dir, std::uint64_t budget_vertices,
                    unsigned max_regions);
    void shutdown(bool everything);
    [[nodiscard]] bool ready() const;
    [[nodiscard]] const std::string& problem() const;
    [[nodiscard]] unsigned raytracing_tier() const;

    bool allocate(std::uint64_t vertices, std::uint64_t pipeline, Region& out);
    [[nodiscard]] unsigned current_parity() const;
    ID3D12Resource* vertex_buffer(unsigned parity) const;
    ID3D12Resource* counter_buffer(unsigned parity) const;
    [[nodiscard]] std::uint64_t budget_vertices() const;

    struct PassResult {
        unsigned regions = 0;
        std::uint64_t vertices = 0;
        bool traced = false;
        std::string note;
    };
    PassResult record(ID3D12GraphicsCommandList* commands, const TraceParams& params, ID3D12Resource* color,
                      DXGI_FORMAT color_view_format, ID3D12Resource* depth, DXGI_FORMAT depth_format,
                      bool output_only);
    ID3D12Resource* output() const;
    [[nodiscard]] std::uint64_t blas_bytes() const;
    [[nodiscard]] std::uint64_t scratch_bytes() const;
    [[nodiscard]] unsigned regions_skipped_last() const;

private:
    struct Impl;
    Impl* impl_;
};

}
