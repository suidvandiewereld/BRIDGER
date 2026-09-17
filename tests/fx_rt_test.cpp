
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "fx/math.h"
#include "fx/rt.h"
#include "overlay/d3d.h"

using Microsoft::WRL::ComPtr;
namespace rt = bridger::fx::rt;
namespace math = bridger::fx::math;

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void check(HRESULT hr, const char* what) { if (FAILED(hr)) throw std::runtime_error(std::string(what) + " failed"); }

float half_to_float(std::uint16_t h) {
    const unsigned sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    float value;
    if (exponent == 0) value = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31) value = mantissa ? NAN : INFINITY;
    else value = std::ldexp(static_cast<float>(mantissa + 1024), static_cast<int>(exponent) - 25);
    return sign ? -value : value;
}

int main() try {
    require(bridger::overlay::d3d::load(), "load DX12 entry points");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter> adapter;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof options);
    if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
        std::printf("fx_rt: skipped, WARP reports raytracing tier %d (1.1 needed)\n", static_cast<int>(options.RaytracingTier));
        return 0;
    }
    ComPtr<ID3D12InfoQueue> messages;
    device.As(&messages);

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    ComPtr<ID3D12GraphicsCommandList> commands;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "command list");
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    UINT64 serial = 0;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "fence event");
    auto submit = [&] {
        check(commands->Close(), "close");
        ID3D12CommandList* lists[]{commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), ++serial), "signal");
        check(fence->SetEventOnCompletion(serial, event), "event");
        require(WaitForSingleObject(event, 20000) == WAIT_OBJECT_0, "GPU timeout");
        check(allocator->Reset(), "allocator reset");
        check(commands->Reset(allocator.Get(), nullptr), "list reset");
    };
    auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        commands->ResourceBarrier(1, &b);
    };
    auto buffer = [&](UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES props{};
        props.Type = heap;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> out;
        check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out)), "buffer");
        return out;
    };
    auto texture = [&](DXGI_FORMAT format, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES props{};
        props.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 64;
        desc.Height = 64;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> out;
        check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out)), "texture");
        return out;
    };

    rt::Runtime runtime;
    const std::filesystem::path dxc_dir = BRIDGER_DXC_DIR;
    if (!runtime.initialise(device.Get(), dxc_dir, 1u << 16, 64)) {
        const auto& problem = runtime.problem();
        if (problem.find("no raytracing") != std::string::npos || problem.find("tier") != std::string::npos) {
            std::printf("fx_rt: skipped, %s\n", problem.c_str());
            return 0;
        }
        throw std::runtime_error("runtime initialisation failed: " + problem);
    }
    std::printf("fx_rt: runtime ready, tier %u\n", runtime.raytracing_tier());

    constexpr unsigned kSize = 64;
    const float jitter_x = 0.25f, jitter_y = -0.3889f;
    const double position[3] = {0.0, 0.0, 0.0};
    const auto view = math::view_matrix({1, 0, 0}, {0, 1, 0}, {0, 0, 1}, position);
    auto projection = math::projection_matrix(1.0f, 1.0f, 0.1f);
    projection = math::jitter_projection(projection, jitter_x, jitter_y, kSize, kSize);
    const auto view_proj = math::multiply(projection, view);
    const math::Vec4 world[3] = {{-1.0f, 5.0f, -1.0f, 1.0f}, {1.0f, 5.0f, -1.0f, 1.0f}, {0.0f, 5.0f, 1.0f, 1.0f}};
    float clip[3][4];
    for (int v = 0; v < 3; ++v) {
        const auto c = math::transform(view_proj, world[v]);
        clip[v][0] = c.x; clip[v][1] = c.y; clip[v][2] = c.z; clip[v][3] = c.w;
    }
    const float garbage[3][4] = {{-100, 0.5f, 0.5f, 1}, {100, 0.5f, 0.5f, 1}, {0, 100, 0.5f, 1}};

    rt::Region region, ignored;
    require(runtime.allocate(3, 1, region), "allocate region");
    require(runtime.allocate(3, 2, ignored), "allocate second region");
    require(region.buffer != ignored.buffer, "regions are distinct");
    const unsigned parity = runtime.current_parity();
    auto* vertices = runtime.vertex_buffer(parity);
    auto* counters = runtime.counter_buffer(parity);
    const UINT64 region_offset = region.buffer - vertices->GetGPUVirtualAddress();
    const UINT64 ignored_offset = ignored.buffer - vertices->GetGPUVirtualAddress();
    const UINT64 counter_offset = region.counter - counters->GetGPUVirtualAddress();

    auto upload = buffer(1024, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    {
        std::uint8_t* mapped = nullptr;
        check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map upload");
        std::memcpy(mapped, clip, sizeof clip);
        std::memcpy(mapped + 64, garbage, sizeof garbage);
        const std::uint32_t filled = 48;
        std::memcpy(mapped + 128, &filled, sizeof filled);
        upload->Unmap(0, nullptr);
    }
    auto depth = texture(DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_STATE_COPY_DEST);
    auto depth_upload = buffer(kSize * kSize * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    {
        float* mapped = nullptr;
        check(depth_upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map depth");
        for (unsigned i = 0; i < kSize * kSize; ++i) mapped[i] = 0.1f / 5.0f;
        depth_upload->Unmap(0, nullptr);
    }
    auto color = texture(DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_STATE_COPY_DEST);

    barrier(vertices, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(counters, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyBufferRegion(vertices, region_offset, upload.Get(), 0, 48);
    commands->CopyBufferRegion(vertices, ignored_offset, upload.Get(), 64, 48);
    commands->CopyBufferRegion(counters, counter_offset, upload.Get(), 128, 4);
    barrier(vertices, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    barrier(counters, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    {
        D3D12_TEXTURE_COPY_LOCATION dst{depth.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        D3D12_TEXTURE_COPY_LOCATION src{depth_upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, kSize, kSize, 1, kSize * 4};
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    barrier(depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    submit();

    rt::TraceParams params;
    params.view = view;
    params.projection = projection;
    params.camera_valid = true;
    params.depth_reversed = true;
    params.jitter_x = jitter_x;
    params.jitter_y = jitter_y;
    params.width = kSize;
    params.height = kSize;
    params.frame = 1;
    params.settings.mode = static_cast<int>(rt::Mode::DebugPrimary);
    params.settings.strength = 1.0f;
    const auto result = runtime.record(commands.Get(), params, color.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, depth.Get(),
                                       DXGI_FORMAT_R32_TYPELESS, false);
    require(result.regions == 2, "two regions consumed");
    require(result.traced, ("pass did not trace: " + result.note).c_str());
    submit();

    auto readback = buffer(kSize * kSize * 8 + kSize * kSize * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(runtime.output(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    {
        D3D12_TEXTURE_COPY_LOCATION src{runtime.output(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_FLOAT, kSize, kSize, 1, kSize * 8};
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src2{color.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        D3D12_TEXTURE_COPY_LOCATION dst2{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst2.PlacedFootprint.Offset = kSize * kSize * 8;
        dst2.PlacedFootprint.Footprint = {DXGI_FORMAT_R10G10B10A2_UNORM, kSize, kSize, 1, kSize * 4};
        commands->CopyTextureRegion(&dst2, 0, 0, 0, &src2, nullptr);
    }
    barrier(runtime.output(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    submit();

    const std::uint8_t* bytes = nullptr;
    check(readback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<std::uint8_t**>(&bytes))), "map readback");
    auto texel = [&](unsigned x, unsigned y, float out[4]) {
        const auto* h = reinterpret_cast<const std::uint16_t*>(bytes + (y * kSize + x) * 8);
        for (int c = 0; c < 4; ++c) out[c] = half_to_float(h[c]);
    };
    auto color_at = [&](unsigned x, unsigned y) {
        std::uint32_t packed;
        std::memcpy(&packed, bytes + kSize * kSize * 8 + (y * kSize + x) * 4, 4);
        return packed;
    };
    float hit[4], miss[4], corner[4];
    texel(32, 32, hit);
    texel(4, 4, miss);
    texel(60, 60, corner);
    std::printf("fx_rt: hit pixel %.3f %.3f %.3f a=%.1f, miss pixel %.3f %.3f %.3f a=%.1f\n",
                hit[0], hit[1], hit[2], hit[3], miss[0], miss[1], miss[2], miss[3]);
    require(hit[3] > 0.0f, "the primary ray through the triangle must hit");
    std::printf("fx_rt: hit distance %.4f (the triangle is 5 m away)\n", hit[3]);
    require(std::fabs(hit[3] - 5.0f) < 0.05f, "the hit distance must be about 5 m");
    require(hit[1] > hit[0] * 2.0f && hit[1] > hit[2] * 2.0f, "the hit must agree with the depth buffer (green)");
    require(miss[3] == 0.0f, "the ray beside the triangle must miss");
    require(corner[3] == 0.0f, "the unfilled region's triangle must be inactive");
    require(miss[0] > 0.5f && miss[2] > 0.5f && miss[1] < 0.2f, "a depth surface with no hit reads magenta");
    const auto packed = color_at(32, 32);
    const unsigned green10 = (packed >> 10) & 0x3ff;
    require(green10 > 300, "the composite must have written the debug view into the colour");
    readback->Unmap(0, nullptr);

    params.frame = 2;
    const auto second = runtime.record(commands.Get(), params, color.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, depth.Get(),
                                       DXGI_FORMAT_R32_TYPELESS, false);
    require(second.regions == 0 && !second.traced, "the second parity starts empty");
    submit();
    rt::Region again;
    require(runtime.allocate(3, 1, again), "allocate on the returned parity");
    require(runtime.current_parity() % 2 == parity % 2, "parity returns after two passes");
    const auto again_offset = again.buffer - vertices->GetGPUVirtualAddress();
    const auto again_counter = again.counter - counters->GetGPUVirtualAddress();
    barrier(vertices, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(counters, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyBufferRegion(vertices, again_offset, upload.Get(), 0, 48);
    commands->CopyBufferRegion(counters, again_counter, upload.Get(), 128, 4);
    barrier(vertices, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    barrier(counters, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    params.frame = 3;
    params.settings.mode = static_cast<int>(rt::Mode::AmbientOcclusion);
    params.settings.rays = 4;
    const auto third = runtime.record(commands.Get(), params, color.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, depth.Get(),
                                      DXGI_FORMAT_R32_TYPELESS, true);
    require(third.regions == 1 && third.traced, "the third pass traces ambient occlusion");
    submit();

    if (messages) {
        unsigned errors = 0;
        const auto count = messages->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T length = 0;
            messages->GetMessage(i, nullptr, &length);
            std::vector<std::uint8_t> storage(length);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(messages->GetMessage(i, message, &length))) continue;
            if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                ++errors;
                std::printf("fx_rt: debug layer: %s\n", message->pDescription);
            }
        }
        require(errors == 0, "the debug layer reported errors");
    }
    runtime.shutdown(true);
    std::printf("fx_rt: ok\n");
    return 0;
} catch (const std::exception& e) {
    std::printf("fx_rt: FAILED: %s\n", e.what());
    return 1;
}
