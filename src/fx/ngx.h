#pragma once

#include <Windows.h>
#include <d3d12.h>

#include <cstdint>

namespace bridger::fx::ngx {

struct Snapshot {
    bool hooked = false;
    bool seen = false;
    unsigned calls = 0;
    unsigned frames_since = 0;

    unsigned render_width = 0;
    unsigned render_height = 0;
    unsigned output_width = 0;
    unsigned output_height = 0;
    unsigned subrect_width = 0;
    unsigned subrect_height = 0;

    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float mv_scale_x = 0.0f;
    float mv_scale_y = 0.0f;
    float sharpness = 0.0f;
    int reset = 0;

    unsigned window = 0;
    unsigned window_resets = 0;
    float reset_rate = 0.0f;

    DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT motion_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT output_format = DXGI_FORMAT_UNKNOWN;

    const void* color = nullptr;
    const void* depth = nullptr;
    const void* motion = nullptr;
    const void* output = nullptr;

    const char* module_name = "";
    const char* layout = "unknown";
    const char* problem = "";
};

enum class Preset {
    Default = 0,
    A = 1, B = 2, C = 3, D = 4, E = 5, F = 6, G = 7, H = 8, I = 9,
    J = 10,
    K = 11,
};

void set_preset(Preset preset);
[[nodiscard]] Preset preset();
[[nodiscard]] bool preset_applied();
[[nodiscard]] const char* preset_status();

bool initialise();

void set_known_layout(const char* name);
[[nodiscard]] const char* known_layout();
using RememberLayoutFn = void (*)(const char*);
void on_layout_detected(RememberLayoutFn callback);

void watch_for_module();

void shutdown();
[[nodiscard]] bool hooked();

void end_frame();

[[nodiscard]] Snapshot snapshot();

void capture(unsigned frames);

void dump_frame();
[[nodiscard]] bool dump_pending();

[[nodiscard]] ID3D12Device* device();

}
