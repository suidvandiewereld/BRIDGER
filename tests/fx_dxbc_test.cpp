
#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "fx/bindings.h"
#include "fx/dxbc.h"

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
namespace dxbc = bridger::fx::dxbc;

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
ID3D12InfoQueue* g_messages = nullptr;
void drain_messages() {
    if (g_messages == nullptr) return;
    const auto count = g_messages->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T length = 0;
        g_messages->GetMessage(i, nullptr, &length);
        std::vector<char> buffer(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (SUCCEEDED(g_messages->GetMessage(i, message, &length))) std::printf("  d3d12: %s\n", message->pDescription);
    }
    g_messages->ClearStoredMessages();
}
void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::printf("%s failed with 0x%08lx\n", what, static_cast<unsigned long>(hr));
        drain_messages();
        throw std::runtime_error(std::string(what) + " failed");
    }
}

std::vector<std::uint8_t> compile(const char* source, const char* entry, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    const auto hr = D3DCompile(source, std::strlen(source), "test", nullptr, nullptr, entry, profile,
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        std::printf("%s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no output");
        throw std::runtime_error("compile failed");
    }
    const auto* p = static_cast<const std::uint8_t*>(code->GetBufferPointer());
    return {p, p + code->GetBufferSize()};
}

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    HANDLE event = nullptr;
    UINT64 serial = 0;

    void start() {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        ComPtr<IDXGIAdapter> adapter;
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "warp");
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        device->QueryInterface(IID_PPV_ARGS(&g_messages));
        D3D12_COMMAND_QUEUE_DESC qd{};
        check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "list");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, errors;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "root serialise");
        check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
    }

    void submit() {
        check(commands->Close(), "close");
        ID3D12CommandList* lists[]{commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), ++serial), "signal");
        check(fence->SetEventOnCompletion(serial, event), "event");
        require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU timeout");
        check(allocator->Reset(), "allocator reset");
        check(commands->Reset(allocator.Get(), nullptr), "list reset");
    }

    ComPtr<ID3D12PipelineState> pipeline(const std::vector<std::uint8_t>& vs, const std::vector<std::uint8_t>& ps,
                                         ID3D12RootSignature* with = nullptr, unsigned targets = 1) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = with != nullptr ? with : root.Get();
        d.VS = {vs.data(), vs.size()};
        d.PS = {ps.data(), ps.size()};
        auto& rt = d.BlendState.RenderTarget[0];
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        rt.SrcBlend = D3D12_BLEND_ONE; rt.DestBlend = D3D12_BLEND_ZERO; rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE; rt.DestBlendAlpha = D3D12_BLEND_ZERO; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        const D3D12_DEPTH_STENCILOP_DESC keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        d.DepthStencilState.FrontFace = keep;
        d.DepthStencilState.BackFace = keep;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = targets;
        for (unsigned i = 0; i < targets; ++i) {
            d.RTVFormats[i] = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.BlendState.RenderTarget[i] = rt;
        }
        d.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> out;
        check(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)), "pipeline");
        return out;
    }

    std::vector<float> render(ID3D12PipelineState* pso, ID3D12RootSignature* with = nullptr,
                              const std::function<void(ID3D12GraphicsCommandList*)>& bind = {}) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 4; desc.Height = 4; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> target;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)), "target");
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)), "rtv heap");
        const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target.Get(), nullptr, rtv);
        D3D12_HEAP_PROPERTIES readback{};
        readback.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256 * 4; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> staging;
        check(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&staging)), "staging");

        const float clear[4]{0, 0, 0, 0};
        commands->ClearRenderTargetView(rtv, clear, 0, nullptr);
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{0, 0, 4, 4, 0, 1};
        D3D12_RECT sc{0, 0, 4, 4};
        commands->RSSetViewports(1, &vp);
        commands->RSSetScissorRects(1, &sc);
        commands->SetGraphicsRootSignature(with != nullptr ? with : root.Get());
        commands->SetPipelineState(pso);
        if (bind) bind(commands.Get());
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {target.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        commands->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = target.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = staging.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 4, 1, 256};
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        submit();
        std::vector<float> out(16 * 4);
        void* mapped = nullptr;
        check(staging->Map(0, nullptr, &mapped), "map");
        for (unsigned y = 0; y < 4; ++y) {
            std::memcpy(out.data() + y * 16, static_cast<const std::uint8_t*>(mapped) + y * 256, 64);
        }
        staging->Unmap(0, nullptr);
        return out;
    }
};

const char* kVertex = R"(
struct Out { float4 color : COLOR0; float4 position : SV_Position; };
Out main(uint id : SV_VertexID) {
    Out o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.color = float4(0.5, 0.25, 0.125, 1);
    return o;
})";

const char* kHostNoPosition = R"(
float4 main(float4 c : COLOR0) : SV_Target {
    float4 r;
    r.xy = c.xy * 2.0;
    r.zw = c.zw;
    return r;
})";

const char* kHostPosition = R"(
float4 main(float4 c : COLOR0, float4 p : SV_Position) : SV_Target {
    if (p.x > 100.0) return float4(9, 9, 9, 9);
    return float4(c.rgb, p.x);
})";

const char* kHostUnusedPosition = R"(
float4 main(float4 c : COLOR0, float4 p : SV_Position) : SV_Target {
    return c;
})";

const char* kHookInvert = R"(
float4 bridger_material(float4 color : COLOR0, float4 position : SV_Position) : SV_Target0 {
    return float4(1.0 - color.rgb, color.a);
})";

const char* kHookPosition = R"(
float4 bridger_material(float4 color : COLOR0, float4 position : SV_Position) : SV_Target0 {
    return float4(color.rg, position.x, position.y);
})";

const char* kHookLoop = R"(
float4 bridger_material(float4 color : COLOR0, float4 position : SV_Position) : SV_Target0 {
    float3 acc = color.rgb;
    [loop] for (int i = 0; i < 3; ++i) { acc = acc * 0.5 + 0.1; }
    if (acc.x > 0.9) acc = 0;
    return float4(acc, color.a);
})";

const char* kHostNotEqual = R"(
float4 main(float4 c : COLOR0) : SV_Target {
    if (c.x != 0.75) return float4(c.rgb, 1.0);
    return float4(0, 0, 0, 0);
})";

const char* kHookNotEqual = R"(
float4 bridger_material(float4 color : COLOR0, float4 position : SV_Position) : SV_Target0 {
    return color.g != 0.25 ? float4(1, 1, 1, 1) : float4(color.b, color.b, color.b, 1);
})";

const char* kHost51 = R"(
cbuffer HostConstants : register(b0) { float4 host_tint; };
Texture2D host_texture : register(t0);
float4 main(float4 c : COLOR0) : SV_Target {
    return float4(c.rgb * host_tint.rgb + host_texture.Load(int3(0, 0, 0)).rgb * 0.0, 1);
})";

const char* kHookBindings = R"(
cbuffer BridgerMaterial : register(b0, space60) { float4 hook_scale; float4 hook_add; };
Texture2D hook_texture : register(t0, space60);
SamplerState hook_sampler : register(s0, space60);
float4 bridger_material(float4 color : COLOR0, float4 position : SV_Position) : SV_Target0 {
    float4 t = hook_texture.SampleLevel(hook_sampler, float2(0.5, 0.5), 0);
    return float4(color.rgb * hook_scale.rgb + hook_add.rgb + t.rgb, color.a);
})";

const char* kHostTwo = R"(
struct Out { float4 a : SV_Target0; float4 b : SV_Target1; };
Out main(float4 c : COLOR0) {
    Out o;
    o.a = c;
    o.b = float4(c.b, c.g, c.r, 0.75);
    return o;
})";

const char* kHookTwo = R"(
struct Out { float4 a : SV_Target0; float4 b : SV_Target1; };
Out bridger_material(float4 albedo : COLOR0, float4 material : COLOR1) {
    Out o;
    o.a = float4(material.rgb, albedo.a);
    o.b = float4(0.9, material.gba);
    return o;
})";

bool close_to(float a, float b) { return std::fabs(a - b) < 1e-4f; }

std::vector<std::uint8_t> compile_file(const wchar_t* path, const char* entry, const char* profile, int mode) {
    const std::string mode_text = std::to_string(mode);
    const D3D_SHADER_MACRO macros[]{{"MODE", mode_text.c_str()}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> code, errors;
    const auto hr = D3DCompileFromFile(path, macros, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile,
                                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        std::printf("%s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no output");
        throw std::runtime_error("compile failed");
    }
    const auto* p = static_cast<const std::uint8_t*>(code->GetBufferPointer());
    return {p, p + code->GetBufferSize()};
}

int check_material_file(const char* path) {
    const std::wstring wide(path, path + std::strlen(path));
    const auto vs = compile(kVertex, "main", "vs_5_0");
    const auto host = compile(kHostNoPosition, "main", "ps_5_0");
    Gpu gpu;
    gpu.start();
    for (int mode = 0; mode <= 3; ++mode) {
        const auto hook = compile_file(wide.c_str(), "bridger_material", "ps_5_0", mode);
        const auto r = dxbc::splice_pixel_shader(host.data(), host.size(), hook.data(), hook.size(), vs.data(), vs.size());
        if (!r.error.empty()) {
            std::printf("fx_dxbc: %s MODE=%d: %s\n", path, mode, r.error.c_str());
            return 1;
        }
        auto pso = gpu.pipeline(vs, r.bytecode);
        const auto px = gpu.render(pso.Get());
        std::printf("%s MODE=%d: pixel 0 = %.3f %.3f %.3f %.3f\n", path, mode, px[0], px[1], px[2], px[3]);
    }
    std::printf("fx_dxbc: %s splices and runs in every mode\n", path);
    return 0;
}

int main(int argc, char** argv) try {
    if (argc > 1) return check_material_file(argv[1]);
    const auto vs = compile(kVertex, "main", "vs_5_0");
    const auto host_plain = compile(kHostNoPosition, "main", "ps_5_0");
    const auto host_pos = compile(kHostPosition, "main", "ps_5_0");
    const auto hook_invert = compile(kHookInvert, "bridger_material", "ps_5_0");
    const auto hook_position = compile(kHookPosition, "bridger_material", "ps_5_0");
    const auto hook_loop = compile(kHookLoop, "bridger_material", "ps_5_0");

    require(dxbc::verify(vs.data(), vs.size()), "checksum of a compiled vertex shader");
    require(dxbc::verify(host_plain.data(), host_plain.size()), "checksum of a compiled pixel shader");
    {
        auto copy = host_plain;
        copy[copy.size() - 1] ^= 1;
        require(!dxbc::verify(copy.data(), copy.size()), "a flipped bit must fail verification");
        dxbc::Container c;
        require(c.parse(host_plain.data(), host_plain.size()), "parse");
        const auto rebuilt = c.build();
        require(rebuilt == host_plain, "a parse and rebuild is byte identical");
    }

    {
        const auto s = dxbc::summarise(host_plain.data(), host_plain.size());
        require(s.problem.empty(), s.problem.c_str());
        require(s.stage == dxbc::Stage::Pixel && s.major == 5, "stage and model");
        require(s.writes_target0 && !s.has_position, "plain host shape");
        const auto t = dxbc::summarise(host_pos.data(), host_pos.size());
        require(t.problem.empty() && t.has_position, "position host shape");
        const auto v = dxbc::summarise(vs.data(), vs.size());
        require(v.problem.empty() && v.stage == dxbc::Stage::Vertex, "vertex shader walks");
    }

    Gpu gpu;
    gpu.start();
    {
        auto pso = gpu.pipeline(vs, host_plain);
        const auto px = gpu.render(pso.Get());
        require(close_to(px[0], 1.0f) && close_to(px[1], 0.5f) && close_to(px[2], 0.125f) && close_to(px[3], 1.0f), "baseline pixel");
    }

    {
        const auto r = dxbc::splice_pixel_shader(host_plain.data(), host_plain.size(), hook_invert.data(), hook_invert.size());
        require(r.error.empty(), r.error.c_str());
        require(r.insertions == 1, "one return in the plain host");
        require(dxbc::verify(r.bytecode.data(), r.bytecode.size()), "spliced shader verifies");
        ComPtr<ID3DBlob> text;
        require(SUCCEEDED(D3DDisassemble(r.bytecode.data(), r.bytecode.size(), 0, nullptr, &text)), "spliced shader disassembles");
        auto pso = gpu.pipeline(vs, r.bytecode);
        const auto px = gpu.render(pso.Get());
        std::printf("invert: %.3f %.3f %.3f %.3f\n", px[0], px[1], px[2], px[3]);
        require(close_to(px[0], 0.0f) && close_to(px[1], 0.5f) && close_to(px[2], 0.875f) && close_to(px[3], 1.0f), "inverted pixel");
    }

    {
        const auto r = dxbc::splice_pixel_shader(host_plain.data(), host_plain.size(), hook_position.data(), hook_position.size(), vs.data(), vs.size());
        require(r.error.empty(), r.error.c_str());
        auto pso = gpu.pipeline(vs, r.bytecode);
        const auto px = gpu.render(pso.Get());
        const float* p = px.data() + (1 * 4 + 2) * 4;
        std::printf("position added: %.3f %.3f %.3f %.3f\n", p[0], p[1], p[2], p[3]);
        require(close_to(p[0], 1.0f) && close_to(p[1], 0.5f) && close_to(p[2], 2.5f) && close_to(p[3], 1.5f), "position pixel");
    }

    {
        const auto r = dxbc::splice_pixel_shader(host_pos.data(), host_pos.size(), hook_position.data(), hook_position.size(), vs.data(), vs.size());
        require(r.error.empty(), r.error.c_str());
        require(r.insertions >= 1, "hook placed before every return");
        auto pso = gpu.pipeline(vs, r.bytecode);
        const auto px = gpu.render(pso.Get());
        const float* p = px.data() + (3 * 4 + 1) * 4;
        std::printf("position shared: %.3f %.3f %.3f %.3f\n", p[0], p[1], p[2], p[3]);
        require(close_to(p[0], 0.5f) && close_to(p[1], 0.25f) && close_to(p[2], 1.5f) && close_to(p[3], 3.5f), "shared position pixel");
    }

    {
        const auto host_unused = compile(kHostUnusedPosition, "main", "ps_5_0");
        const auto r = dxbc::splice_pixel_shader(host_unused.data(), host_unused.size(), hook_position.data(), hook_position.size(), vs.data(), vs.size());
        require(r.error.empty(), r.error.c_str());
        auto pso = gpu.pipeline(vs, r.bytecode);
        const auto px = gpu.render(pso.Get());
        const float* p = px.data() + (1 * 4 + 2) * 4;
        std::printf("unread position reused: %.3f %.3f %.3f %.3f\n", p[0], p[1], p[2], p[3]);
        require(close_to(p[0], 0.5f) && close_to(p[1], 0.25f) && close_to(p[2], 2.5f) && close_to(p[3], 1.5f), "unread position pixel");
    }

    {
        const auto host51 = compile(kHost51, "main", "ps_5_1");
        const auto hook51 = compile(kHookBindings, "bridger_material", "ps_5_1");
        const auto r = dxbc::splice_pixel_shader(host51.data(), host51.size(), hook51.data(), hook51.size(), vs.data(), vs.size());
        require(r.error.empty(), r.error.c_str());
        require(dxbc::verify(r.bytecode.data(), r.bytecode.size()), "bindings splice verifies");

        D3D12_DESCRIPTOR_RANGE host_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_DESCRIPTOR_RANGE hook_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 60, 0};
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor = {0, 0};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &host_range};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[2].Descriptor = {0, 60};
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable = {1, &hook_range};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.RegisterSpace = 60;
        D3D12_ROOT_SIGNATURE_DESC rd{4, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ComPtr<ID3DBlob> blob, errors;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "bindings root serialise");
        ComPtr<ID3D12RootSignature> root2;
        check(gpu.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root2)), "bindings root");

        D3D12_HEAP_PROPERTIES upload{};
        upload.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 512; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> constants;
        check(gpu.device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constants)), "constants");
        {
            void* m = nullptr;
            check(constants->Map(0, nullptr, &m), "map constants");
            const float host_tint[4]{2.0f, 2.0f, 2.0f, 1.0f};
            const float hook[8]{0.5f, 0.5f, 0.5f, 0, 0.125f, 0.0f, 0.0f, 0};
            std::memcpy(m, host_tint, sizeof host_tint);
            std::memcpy(static_cast<std::uint8_t*>(m) + 256, hook, sizeof hook);
            constants->Unmap(0, nullptr);
        }
        auto texture = [&](float value) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = 1; td.Height = 1; td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1;
            ComPtr<ID3D12Resource> t;
            check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t)), "texture");
            const float texel[4]{value, value, value, 1};
            ComPtr<ID3D12Resource> src;
            check(gpu.device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&src)), "texture upload");
            void* m = nullptr;
            check(src->Map(0, nullptr, &m), "map texture upload");
            std::memcpy(m, texel, sizeof texel);
            src->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = t.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = src.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1, 16};
            gpu.commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            gpu.submit();
            return t;
        };
        const auto host_tex = texture(0.75f);
        const auto hook_tex = texture(0.0625f);
        ComPtr<ID3D12DescriptorHeap> srvs;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        check(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvs)), "srv heap");
        const UINT step = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = srvs->GetCPUDescriptorHandleForHeapStart();
        gpu.device->CreateShaderResourceView(host_tex.Get(), nullptr, cpu);
        cpu.ptr += step;
        gpu.device->CreateShaderResourceView(hook_tex.Get(), nullptr, cpu);

        auto pso = gpu.pipeline(vs, r.bytecode, root2.Get());
        const auto px = gpu.render(pso.Get(), root2.Get(), [&](ID3D12GraphicsCommandList* list) {
            ID3D12DescriptorHeap* heaps[]{srvs.Get()};
            list->SetDescriptorHeaps(1, heaps);
            auto gpu_start = srvs->GetGPUDescriptorHandleForHeapStart();
            list->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
            list->SetGraphicsRootDescriptorTable(1, gpu_start);
            list->SetGraphicsRootConstantBufferView(2, constants->GetGPUVirtualAddress() + 256);
            gpu_start.ptr += step;
            list->SetGraphicsRootDescriptorTable(3, gpu_start);
        });
        std::printf("hook bindings: %.4f %.4f %.4f %.4f\n", px[0], px[1], px[2], px[3]);
        drain_messages();
        require(close_to(px[0], 0.6875f) && close_to(px[1], 0.3125f) && close_to(px[2], 0.1875f) && close_to(px[3], 1.0f), "hook bindings pixel");

        D3D12_ROOT_SIGNATURE_DESC plain{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ComPtr<ID3DBlob> blob2;
        check(D3D12SerializeRootSignature(&plain, D3D_ROOT_SIGNATURE_VERSION_1, &blob2, &errors), "plain root serialise");
        ComPtr<ID3D12RootSignature> root3;
        check(gpu.device->CreateRootSignature(0, blob2->GetBufferPointer(), blob2->GetBufferSize(), IID_PPV_ARGS(&root3)), "plain root");
        bool created = true;
        try { auto refused = gpu.pipeline(vs, r.bytecode, root3.Get()); } catch (const std::exception&) { created = false; }
        std::printf("hook bindings without space 60 slots: %s\n", created ? "created" : "refused");
        drain_messages();
        require(!created, "a space 60 hook must be refused without the slots");

        std::vector<std::uint8_t> extended;
        bridger::fx::bindings::RootExtension ext;
        std::string why;
        require(bridger::fx::bindings::extend_root(blob2->GetBufferPointer(), blob2->GetBufferSize(), extended, ext, why), why.c_str());
        require(ext.constants == 2 && ext.textures == 3, "appended after the game's two parameters");
        ComPtr<ID3D12RootSignature> root4;
        check(gpu.device->CreateRootSignature(0, extended.data(), extended.size(), IID_PPV_ARGS(&root4)), "extended root");
        auto pso4 = gpu.pipeline(vs, r.bytecode, root4.Get());
        ComPtr<ID3D12DescriptorHeap> table;
        D3D12_DESCRIPTOR_HEAP_DESC td{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        check(gpu.device->CreateDescriptorHeap(&td, IID_PPV_ARGS(&table)), "table heap");
        auto at = table->GetCPUDescriptorHandleForHeapStart();
        gpu.device->CreateShaderResourceView(host_tex.Get(), nullptr, at);
        at.ptr += step;
        gpu.device->CreateShaderResourceView(hook_tex.Get(), nullptr, at);
        for (int i = 0; i < 3; ++i) {
            at.ptr += step;
            D3D12_SHADER_RESOURCE_VIEW_DESC none{};
            none.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            none.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            none.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            none.Texture2D.MipLevels = 1;
            gpu.device->CreateShaderResourceView(nullptr, &none, at);
        }
        const auto px4 = gpu.render(pso4.Get(), root4.Get(), [&](ID3D12GraphicsCommandList* list) {
            ID3D12DescriptorHeap* heaps[]{table.Get()};
            list->SetDescriptorHeaps(1, heaps);
            auto start = table->GetGPUDescriptorHandleForHeapStart();
            list->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
            list->SetGraphicsRootDescriptorTable(1, start);
            list->SetGraphicsRootConstantBufferView(ext.constants, constants->GetGPUVirtualAddress() + 256);
            start.ptr += step;
            list->SetGraphicsRootDescriptorTable(ext.textures, start);
        });
        std::printf("extended root signature: %.4f %.4f %.4f %.4f\n", px4[0], px4[1], px4[2], px4[3]);
        drain_messages();
        require(close_to(px4[0], 0.6875f) && close_to(px4[1], 0.3125f) && close_to(px4[2], 0.1875f) && close_to(px4[3], 1.0f), "extended root pixel");

        auto plain_pso = gpu.pipeline(vs, host51, root4.Get());
        const auto px5 = gpu.render(plain_pso.Get(), root4.Get(), [&](ID3D12GraphicsCommandList* list) {
            ID3D12DescriptorHeap* heaps[]{table.Get()};
            list->SetDescriptorHeaps(1, heaps);
            list->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
            list->SetGraphicsRootDescriptorTable(1, table->GetGPUDescriptorHandleForHeapStart());
        });
        std::printf("host under the extended signature: %.4f %.4f %.4f %.4f\n", px5[0], px5[1], px5[2], px5[3]);
        require(close_to(px5[0], 1.0f) && close_to(px5[1], 0.5f) && close_to(px5[2], 0.25f), "host pixel under extension");

        std::vector<std::uint8_t> again;
        bridger::fx::bindings::RootExtension ext2;
        require(!bridger::fx::bindings::extend_root(blob->GetBufferPointer(), blob->GetBufferSize(), again, ext2, why), "space 60 signature left alone");
        std::printf("left alone: %s\n", why.c_str());
    }

    {
        const auto host2 = compile(kHostTwo, "main", "ps_5_0");
        const auto hook2 = compile(kHookTwo, "bridger_material", "ps_5_0");
        const auto r = dxbc::splice_pixel_shader(host2.data(), host2.size(), hook2.data(), hook2.size());
        require(r.error.empty(), r.error.c_str());
        require(dxbc::verify(r.bytecode.data(), r.bytecode.size()), "two-target splice verifies");
        auto pso = gpu.pipeline(vs, r.bytecode, nullptr, 2);
        const auto px = gpu.render(pso.Get());
        std::printf("two targets: %.4f %.4f %.4f %.4f\n", px[0], px[1], px[2], px[3]);
        require(close_to(px[0], 0.125f) && close_to(px[1], 0.25f) && close_to(px[2], 0.5f) && close_to(px[3], 1.0f), "two-target pixel");
        const auto q = dxbc::splice_pixel_shader(host_plain.data(), host_plain.size(), hook2.data(), hook2.size());
        std::printf("missing target reads: %s\n", q.error.c_str());
        require(q.error.find("does not write") != std::string::npos, "undeclared target refused");
    }

    {
        const auto r = dxbc::splice_pixel_shader(host_plain.data(), host_plain.size(), hook_loop.data(), hook_loop.size());
        require(r.error.empty(), r.error.c_str());
        auto pso = gpu.pipeline(vs, r.bytecode);
        const auto px = gpu.render(pso.Get());
        auto f = [](float c) { for (int i = 0; i < 3; ++i) c = c * 0.5f + 0.1f; return c; };
        std::printf("loop: %.4f %.4f %.4f %.4f\n", px[0], px[1], px[2], px[3]);
        require(close_to(px[0], f(1.0f)) && close_to(px[1], f(0.5f)) && close_to(px[2], f(0.125f)) && close_to(px[3], 1.0f), "loop pixel");
    }

    {
        const auto host_ne = compile(kHostNotEqual, "main", "ps_5_0");
        const auto hook_ne = compile(kHookNotEqual, "bridger_material", "ps_5_0");
        const auto s = dxbc::summarise(host_ne.data(), host_ne.size());
        require(s.problem.empty(), s.problem.c_str());
        const auto r = dxbc::splice_pixel_shader(host_ne.data(), host_ne.size(), hook_ne.data(), hook_ne.size());
        require(r.error.empty(), r.error.c_str());
        auto pso = gpu.pipeline(vs, r.bytecode);
        const auto px = gpu.render(pso.Get());
        std::printf("not equal: %.3f %.3f %.3f %.3f\n", px[0], px[1], px[2], px[3]);
        require(close_to(px[0], 0.125f) && close_to(px[1], 0.125f) && close_to(px[2], 0.125f) && close_to(px[3], 1.0f), "not equal pixel");
    }

    {
        const char* icb = "static const float k[4] = {0.1, 0.2, 0.3, 0.4}; float4 bridger_material(float4 color : COLOR0) : SV_Target0 { return color * k[(uint)(color.a * 3.0)]; }";
        const auto hook_icb = compile(icb, "bridger_material", "ps_5_0");
        const auto q = dxbc::splice_pixel_shader(host_plain.data(), host_plain.size(), hook_icb.data(), hook_icb.size());
        std::printf("static array reads: %s\n", q.error.c_str());
        require(q.error.find("immediate constant buffer") != std::string::npos, "static array hook rejected as custom data");
    }

    {
        const auto r = dxbc::splice_pixel_shader(vs.data(), vs.size(), hook_invert.data(), hook_invert.size());
        require(!r.error.empty() && r.bytecode.empty(), "vertex host rejected");
        const char* bad = "cbuffer C { float k; }; float4 bridger_material(float4 color : COLOR0) : SV_Target0 { return color * k; }";
        const auto hook_cb = compile(bad, "bridger_material", "ps_5_0");
        const auto q = dxbc::splice_pixel_shader(host_plain.data(), host_plain.size(), hook_cb.data(), hook_cb.size());
        require(!q.error.empty(), "constant buffer hook rejected");
        std::printf("rejection reads: %s\n", q.error.c_str());
    }

    require(dxbc::output_register(vs.data(), vs.size(), 1) == 1, "SV_Position register from the vertex shader");
    require(dxbc::output_register(host_plain.data(), host_plain.size(), 1) == ~0u, "no position output in a pixel shader");

    std::printf("fx_dxbc: all checks passed\n");
    return 0;
} catch (const std::exception& e) {
    std::printf("fx_dxbc: FAILED: %s\n", e.what());
    return 1;
}
