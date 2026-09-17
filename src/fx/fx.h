#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "bridger/api.h"

namespace bridger::fx {
namespace ngx { struct Snapshot; }

struct FrameContext {
    ID3D12GraphicsCommandList* commands = nullptr;
    ID3D12Resource* backbuffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE backbuffer_rtv{};
    unsigned frame_index = 0;
    unsigned width = 0;
    unsigned height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ID3D12Fence* completion_fence = nullptr;
    UINT64 completion_value = 0;
};

void configure(const std::filesystem::path& root);

bool initialise(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format,
                unsigned width, unsigned height, unsigned frame_count);
void shutdown();
[[nodiscard]] bool device_ready();

void prepare_hooks(void** device_vtable, void** command_list_vtable);

void begin_frame(float delta_seconds, unsigned width, unsigned height);
[[nodiscard]] bool has_work();
void render(const FrameContext& context);
void render_pre_upscale(ID3D12GraphicsCommandList* commands, const ngx::Snapshot& inputs);
void render_raytracing(ID3D12GraphicsCommandList* commands, const ngx::Snapshot& inputs);
void submitted(ID3D12CommandQueue* queue, unsigned count, ID3D12CommandList* const* lists);

void sample_camera();

void dump_backbuffer();

void destroy_owned(std::string_view owner);

const BridgerFx* api();

void draw_panel();
void select_panel_page(int page);

}
