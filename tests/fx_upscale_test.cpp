#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <filesystem>
#include "fx/fx.h"
#include "fx/fx_internal.h"
#include "fx/ngx.h"
#include "core/guard.h"
#include "overlay/d3d.h"

using Microsoft::WRL::ComPtr;
namespace fx = bridger::fx;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void check(HRESULT hr) { require(SUCCEEDED(hr), "DX12 call failed"); }

int main() try {
    require(bridger::overlay::d3d::load(), "load DX12 compiler entry points");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> messages;
    device.As(&messages);
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> commands;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    UINT64 serial = 0;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "fence event");
    auto submit = [&] {
        check(commands->Close());
        ID3D12CommandList* lists[]{commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        fx::submitted(queue.Get(), 1, lists);
        check(queue->Signal(fence.Get(), ++serial));
        check(fence->SetEventOnCompletion(serial, event));
        require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU timeout");
        check(allocator->Reset());
        check(commands->Reset(allocator.Get(), nullptr));
    };
    auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        commands->ResourceBarrier(1, &b);
    };
    auto texture = [&](DXGI_FORMAT format) {
        ComPtr<ID3D12Resource> out;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 16; desc.Height = 16; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = format; desc.SampleDesc.Count = 1; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&out)));
        return out;
    };
    auto color = texture(DXGI_FORMAT_R8G8B8A8_UNORM);
    auto motion = texture(DXGI_FORMAT_R32G32_FLOAT);
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap_desc.NumDescriptors = 2;
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)));
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    auto motion_rtv = rtv;
    motion_rtv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(color.Get(), nullptr, rtv);
    device->CreateRenderTargetView(motion.Get(), nullptr, motion_rtv);
    const float blue[]{0, 0, 1, 1}, vectors[]{2, -4, 0, 0};
    commands->ClearRenderTargetView(rtv, blue, 0, nullptr);
    commands->ClearRenderTargetView(motion_rtv, vectors, 0, nullptr);
    fx::configure(std::filesystem::temp_directory_path() / "bridger_fx_upscale_test");
    require(fx::initialise(device.Get(), queue.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 2), "fx initialize");
    fx::state().settings.pre_upscale = true;
    fx::state().settings.timings = false;
    BridgerFxShaderDesc shader{};
    shader.kind = BRIDGER_FX_SHADER_FULLSCREEN;
    shader.owner = "upscale_test";
    shader.source = "float4 ps_main(BridgerScreenPixel i):SV_TARGET { float2 m=bridger_scene_motion.Load(int3(0,0,0)); return float4(i.uv.x+m.x*bridger_motion.x,i.uv.y,bridger_motion.z,1); }";
    const auto shader_id = fx::api()->create_shader(&shader);
    BridgerFxPassDesc pass{};
    pass.owner = "upscale_test"; pass.shader = shader_id; pass.stage = BRIDGER_FX_STAGE_PRE_UPSCALE;
    pass.enabled = true; pass.wants_history = true;
    const auto effect = fx::api()->create_pass(&pass);
    require(effect != 0, "create pre upscale pass");
    BridgerFxRenderTargetDesc target_desc{};
    target_desc.width = target_desc.height = 16; target_desc.format = BRIDGER_FX_FORMAT_RGBA8;
    const auto target_id = fx::api()->create_render_target(&target_desc);
    require(target_id != 0, "create named output target");
    fx::api()->set_output(effect, target_id);
    shader.source = "float4 ps_main(BridgerScreenPixel i):SV_TARGET { return bridger_texture0.SampleLevel(bridger_point,i.uv,0); }";
    const auto copy_shader = fx::api()->create_shader(&shader);
    pass.shader = copy_shader; pass.priority = 1;
    const auto combine = fx::api()->create_pass(&pass);
    fx::api()->set_texture(combine, 0, target_id);
    fx::begin_frame(1.0f/60, 16, 16);
    fx::FrameContext frame;
    frame.commands = commands.Get(); frame.backbuffer = color.Get(); frame.backbuffer_rtv = rtv;
    frame.width = frame.height = 16; frame.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    fx::render(frame);
    barrier(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    submit();
    fx::ngx::Snapshot inputs;
    inputs.color = color.Get(); inputs.color_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    inputs.motion = motion.Get(); inputs.motion_format = DXGI_FORMAT_R32G32_FLOAT;
    inputs.render_width = inputs.render_height = 16;
    inputs.mv_scale_x = inputs.mv_scale_y = 1;
    fx::render_pre_upscale(commands.Get(), inputs);
    require(fx::state().stats.upscale_draws == 0, "queue probe must not write color");
    submit();
    fx::render_pre_upscale(commands.Get(), inputs);
    require(fx::state().stats.upscale_draws == 2, "named output and composite passes must draw");
    submit();
    shader.source = "float4 ps_main(BridgerScreenPixel i):SV_TARGET { return bridger_history.SampleLevel(bridger_point,i.uv,0); }";
    const auto history_shader = fx::api()->create_shader(&shader);
    fx::api()->set_shader(effect, history_shader);
    fx::api()->set_output(effect, 0);
    fx::api()->set_enabled(combine, false);
    fx::begin_frame(1.0f/60, 16, 16);
    barrier(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    fx::render(frame);
    barrier(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    submit();
    fx::render_pre_upscale(commands.Get(), inputs);
    require(fx::state().stats.upscale_draws == 1, "history pass must draw");
    fx::destroy_owned("upscale_test");
    submit();
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer_desc.Width = 256 * 16;
    buffer_desc.Height = 1; buffer_desc.DepthOrArraySize = 1; buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1; buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES read_heap{}; read_heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    check(device->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    barrier(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = color.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource = readback.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 256};
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    submit();
    void* bytes = nullptr;
    D3D12_RANGE range{0, 256 * 16}; check(readback->Map(0, &range, &bytes));
    auto* pixel = static_cast<unsigned char*>(bytes) + 8 * 256 + 4 * 4;
    std::printf("pre upscale pixel: %u %u %u %u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    require(pixel[0] >= 135 && pixel[0] <= 137 && pixel[1] >= 71 && pixel[1] <= 73 && pixel[2] == 255,
        "pre upscale history or motion scale is wrong");
    D3D12_RANGE empty{}; readback->Unmap(0, &empty);
    fx::shutdown();
    if (messages) {
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0; messages->GetMessage(i, nullptr, &size);
            std::vector<unsigned char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            check(messages->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::fprintf(stderr, "%s\n", message->pDescription);
                throw std::runtime_error("DX12 validation error");
            }
        }
    }
    CloseHandle(event);
    std::puts("fx_upscale: passed");
    return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "fx_upscale: %s\n", e.what()); return 1; }
