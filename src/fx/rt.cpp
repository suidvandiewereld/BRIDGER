#include "fx/rt.h"

#include <d3d12.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <format>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "core/guard.h"
#include "core/log.h"
#include "core/settings.h"
#include "fx/ngx.h"

namespace bridger::fx::rt {
namespace {

using Microsoft::WRL::ComPtr;

constexpr unsigned kParities = 2;
constexpr unsigned kRegionAlignment = 16;
constexpr unsigned kThreadGroup = 64;
constexpr unsigned kRetirePasses = 8;
constexpr UINT64 kCounterBytes = 4;
constexpr std::uint64_t kMaxRegionVertices = 4u << 20;

const char kTransformShader[] = R"hlsl(
struct Region { uint offset_lo; uint offset_hi; uint bound; uint counter; };
cbuffer Params : register(b0) {
    row_major float4x4 inv_view;
    float4 focal;            // 1/P00, 1/P11, jitter x term, jitter y term (clip units per w)
    uint region_count;
    uint total_vertices;
    uint2 pad;
};
RWByteAddressBuffer vertices : register(u0);
ByteAddressBuffer counters : register(t0);
StructuredBuffer<Region> regions : register(t1);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint v = id.x;
    if (v >= total_vertices) return;
    uint lo = 0, hi = region_count;
    while (hi - lo > 1) {
        const uint mid = (lo + hi) / 2;
        if (regions[mid].offset_lo <= v) lo = mid; else hi = mid;
    }
    const Region r = regions[lo];
    const uint local = v - r.offset_lo;
    // Unused vertices collapse to a degenerate triangle at the origin, which no ray can hit. NaN
    // is the specification's inactive-triangle marker, but WARP reports hits with a NaN distance
    // on NaN triangles, so the portable form is used.
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    if (region_count > 0 && local < r.bound) {
        const uint filled = counters.Load(r.counter * 4) / 16;
        if (local < filled) {
            const float4 c = asfloat(vertices.Load4(v * 16));
            const float w = c.w;
            const float3 view = float3((c.x - focal.z * w) * focal.x, (c.y - focal.w * w) * focal.y, -w);
            const float3 world = mul(inv_view, float4(view, 1.0)).xyz;
            if (all(isfinite(world))) result = float4(world, 1.0);
        }
    }
    vertices.Store4(v * 16, asuint(result));
}
)hlsl";

const char kTraceShader[] = R"hlsl(
#define MODE_AO 0
#define MODE_SHADOW 1
#define MODE_DEBUG 2

cbuffer Params : register(b0) {
    row_major float4x4 inv_view_proj;
    float4 camera;           // xyz position, w valid
    float4 size;             // width, height, 1/width, 1/height
    float4 sun;              // direction (towards the sun), strength
    float4 ao;               // radius, bias, rays, frame
    uint4 flags;             // mode, depth reversed, has depth, 0
};
RaytracingAccelerationStructure scene : register(t2);
StructuredBuffer<uint4> regions : register(t1);     // offset_lo, offset_hi, bound, counter
ByteAddressBuffer vertices : register(t0);
Texture2D<float> depth_texture : register(t3);
RWTexture2D<float4> output : register(u1);

float3 world_at(float2 pixel, out bool sky) {
    const float2 uv = pixel * size.zw;
    const float d = depth_texture.Load(int3(int2(pixel), 0));
    sky = flags.y != 0 ? d <= 0.0 : d >= 1.0;
    const float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    const float4 h = mul(inv_view_proj, ndc);
    return abs(h.w) > 1e-20 ? h.xyz / h.w : float3(0, 0, 0);
}

float3 camera_ray(float2 pixel) {
    const float2 uv = pixel * size.zw;
    const float4 a = mul(inv_view_proj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0));
    const float4 b = mul(inv_view_proj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.25, 1.0));
    return normalize(b.xyz / b.w - a.xyz / a.w);
}

// Interleaved gradient noise with a golden-ratio frame offset: cheap and well distributed.
float hash(float2 pixel, float salt) {
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(pixel + salt * 5.588238, magic.xy)));
}

float3 cosine_direction(float3 n, float u1, float u2) {
    const float r = sqrt(u1);
    const float phi = 6.28318530 * u2;
    const float3 t = normalize(abs(n.z) < 0.999 ? cross(n, float3(0, 0, 1)) : cross(n, float3(1, 0, 0)));
    const float3 b = cross(n, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}

bool occluded(float3 origin, float3 direction, float tmax, out float t, out uint geometry, out uint primitive) {
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = tmax;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, ray);
    q.Proceed();
    t = 0.0; geometry = 0; primitive = 0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        t = q.CommittedRayT();
        geometry = q.CommittedGeometryIndex();
        primitive = q.CommittedPrimitiveIndex();
        return true;
    }
    return false;
}

float3 face_normal(uint geometry, uint primitive) {
    const uint4 r = regions[geometry];
    const uint base = (r.x + primitive * 3) * 16;
    const float3 a = asfloat(vertices.Load3(base));
    const float3 b = asfloat(vertices.Load3(base + 16));
    const float3 c = asfloat(vertices.Load3(base + 32));
    const float3 n = cross(b - a, c - a);
    return dot(n, n) > 0.0 ? normalize(n) : float3(0, 0, 1);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(size.x) || id.y >= uint(size.y)) return;
    const float2 pixel = float2(id.xy) + 0.5;
    const uint mode = flags.x;
    const bool has_depth = flags.z != 0;

    if (mode == MODE_DEBUG) {
        const float3 dir = camera_ray(pixel);
        float t; uint geometry, primitive;
        const bool hit = occluded(camera.xyz, dir, 1.0e6, t, geometry, primitive);
        float3 color = float3(0.05, 0.05, 0.08);          // miss: near black
        float alpha = 0.0;                                  // hit distance, 0 on a miss
        if (hit) {
            const float3 n = face_normal(geometry, primitive);
            const float shade = 0.35 + 0.65 * abs(dot(n, dir));
            float3 tint = float3(0.7, 0.7, 0.7);          // no depth to compare with: grey
            if (has_depth) {
                bool sky;
                const float3 p = world_at(pixel, sky);
                const float reference = length(p - camera.xyz);
                const float tolerance = max(0.05, 0.03 * reference);
                if (sky) tint = float3(0.3, 0.5, 1.0);        // traced geometry where depth says sky
                else if (abs(t - reference) < tolerance) tint = float3(0.2, 1.0, 0.2);   // agrees
                else if (t < reference) tint = float3(1.0, 0.2, 0.2);                   // traced hit in front of the depth
                else tint = float3(1.0, 0.8, 0.2);                                      // traced hit behind the depth
            }
            color = tint * shade;
            alpha = t;
        } else if (has_depth) {
            bool sky;
            world_at(pixel, sky);
            if (!sky) color = float3(0.6, 0.1, 0.6);       // depth has a surface the trace missed
        }
        output[id.xy] = float4(color, alpha);
        return;
    }

    if (!has_depth) { output[id.xy] = float4(1, 1, 1, 0); return; }
    bool sky;
    const float3 p = world_at(pixel, sky);
    if (sky) { output[id.xy] = float4(1, 1, 1, 0); return; }
    // Normal from the depth buffer: the closer neighbour on each axis avoids bleeding across edges.
    bool s1, s2;
    const float3 px = world_at(pixel + float2(1, 0), s1);
    const float3 nx = world_at(pixel - float2(1, 0), s2);
    const float3 py = world_at(pixel + float2(0, 1), s1);
    const float3 ny = world_at(pixel - float2(0, 1), s2);
    const float3 dx = (s1 || length(px - p) > length(nx - p)) ? p - nx : px - p;
    const float3 dy = (s2 || length(py - p) > length(ny - p)) ? p - ny : py - p;
    float3 n = cross(dx, dy);
    n = dot(n, n) > 0.0 ? normalize(n) : float3(0, 0, 1);
    const float3 to_camera = camera.xyz - p;
    if (dot(n, to_camera) < 0.0) n = -n;
    const float distance = length(to_camera);
    const float3 origin = p + n * (ao.y * max(1.0, distance));

    if (mode == MODE_SHADOW) {
        float t; uint g, pr;
        const bool hit = occluded(origin, normalize(sun.xyz), 1.0e5, t, g, pr);
        output[id.xy] = float4(hit ? 0.0 : 1.0, 0, 0, 1);
        return;
    }

    const uint rays = max(1u, uint(ao.z));
    uint hits = 0;
    for (uint i = 0; i < rays; ++i) {
        const float u1 = hash(pixel, ao.w + float(i) * 1.61803);
        const float u2 = hash(pixel.yx, ao.w * 0.7 + float(i) * 2.23606);
        float t; uint g, pr;
        if (occluded(origin, cosine_direction(n, u1, u2), ao.x, t, g, pr)) ++hits;
    }
    const float visibility = 1.0 - float(hits) / float(rays);
    output[id.xy] = float4(visibility, 0, 0, 1);
}
)hlsl";

const char kCompositeShader[] = R"hlsl(
cbuffer Params : register(b0) { float4 p; };   // mode, strength, 0, 0
Texture2D<float4> scene_texture : register(t0);
Texture2D<float4> trace_texture : register(t1);

struct Vertex { float4 position : SV_Position; };

Vertex vs_main(uint id : SV_VertexID) {
    Vertex v;
    const float2 uv = float2((id << 1) & 2, id & 2);
    v.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return v;
}

float4 ps_main(Vertex v) : SV_Target0 {
    const int3 at = int3(int2(v.position.xy), 0);
    const float4 c = scene_texture.Load(at);
    const float4 r = trace_texture.Load(at);
    if (uint(p.x) == 2) return float4(lerp(c.rgb, r.rgb, p.y), c.a);
    return float4(c.rgb * lerp(1.0, r.x, p.y * r.a), c.a);
}
)hlsl";

struct TransformConstants {
    float inv_view[16];
    float focal[4];
    std::uint32_t region_count;
    std::uint32_t total_vertices;
    std::uint32_t pad[2];
};
static_assert(sizeof(TransformConstants) == 96);

struct TraceConstants {
    float inv_view_proj[16];
    float camera[4];
    float size[4];
    float sun[4];
    float ao[4];
    std::uint32_t flags[4];
};
static_assert(sizeof(TraceConstants) == 144);

struct GpuRegion {
    std::uint32_t offset_lo;
    std::uint32_t offset_hi;
    std::uint32_t bound;
    std::uint32_t counter;
};

void barrier(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to) {
    if (resource == nullptr || from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    commands->ResourceBarrier(1, &b);
}

void uav_barrier(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = resource;
    commands->ResourceBarrier(1, &b);
}

ComPtr<ID3D12Resource> create_buffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                                     D3D12_RESOURCE_STATES state, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<UINT64>(size, 256);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> out;
    if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out)))) return nullptr;
    if (name != nullptr) out->SetName(name);
    return out;
}

ComPtr<ID3D12Resource> create_texture(ID3D12Device* device, unsigned width, unsigned height, DXGI_FORMAT format,
                                      D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> out;
    if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out)))) return nullptr;
    if (name != nullptr) out->SetName(name);
    return out;
}

std::string narrow(const wchar_t* text) {
    std::string out;
    for (; text != nullptr && *text != 0; ++text) out.push_back(*text < 128 ? static_cast<char>(*text) : '?');
    return out;
}

bool depth_view_format(DXGI_FORMAT source, DXGI_FORMAT& resource, DXGI_FORMAT& view) {
    switch (source) {
        case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT:
            resource = DXGI_FORMAT_R32_TYPELESS; view = DXGI_FORMAT_R32_FLOAT; return true;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS:
            resource = DXGI_FORMAT_R32G8X24_TYPELESS; view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; return true;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS:
            resource = DXGI_FORMAT_R24G8_TYPELESS; view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return true;
        case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS:
            resource = DXGI_FORMAT_R16_TYPELESS; view = DXGI_FORMAT_R16_UNORM; return true;
        default: return false;
    }
}

using SerializeVersionedFn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);

ComPtr<ID3D12RootSignature> create_root(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC1& desc, std::string& problem) {
    static const auto serialize = reinterpret_cast<SerializeVersionedFn>(
        GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeVersionedRootSignature"));
    if (serialize == nullptr) { problem = "d3d12.dll has no D3D12SerializeVersionedRootSignature"; return nullptr; }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versioned.Desc_1_1 = desc;
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(serialize(&versioned, &blob, &errors))) {
        problem = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "root signature serialisation failed";
        return nullptr;
    }
    ComPtr<ID3D12RootSignature> root;
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)))) {
        problem = "root signature creation failed";
        return nullptr;
    }
    return root;
}

}

struct Runtime::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Device5> device5;
    HMODULE dxil = nullptr;
    HMODULE dxc = nullptr;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcUtils> utils;
    std::string problem;
    bool ready = false;
    unsigned tier = 0;

    ComPtr<ID3D12RootSignature> transform_root;
    ComPtr<ID3D12RootSignature> trace_root;
    ComPtr<ID3D12RootSignature> composite_root;
    ComPtr<ID3D12PipelineState> transform_pso;
    ComPtr<ID3D12PipelineState> trace_pso;
    ComPtr<ID3D12PipelineState> composite_pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    UINT descriptor_stride = 0;
    UINT rtv_stride = 0;

    struct Frame {
        ComPtr<ID3D12Resource> vertices;
        ComPtr<ID3D12Resource> counters;
        ComPtr<ID3D12Resource> regions_upload;
        GpuRegion* regions_mapped = nullptr;
        ComPtr<ID3D12Resource> constants;
        std::uint8_t* constants_mapped = nullptr;
        ComPtr<ID3D12Resource> instance;
        D3D12_RAYTRACING_INSTANCE_DESC* instance_mapped = nullptr;
        std::atomic<std::uint64_t> used{0};
        std::atomic<unsigned> count{0};
        unsigned last_count = 0;
        std::vector<RegionRecord> records;
        D3D12_RESOURCE_STATES vertices_state = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES counters_state = D3D12_RESOURCE_STATE_COMMON;
    };
    Frame frames[kParities];
    std::atomic<unsigned> parity{0};
    std::uint64_t budget = 0;
    unsigned max_regions = 0;
    std::atomic<unsigned> skipped{0};
    unsigned skipped_last = 0;
    ComPtr<ID3D12Resource> zeros;
    std::vector<GpuRegion> sorted;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
    bool accepting = false;

    ComPtr<ID3D12Resource> blas;
    ComPtr<ID3D12Resource> tlas;
    ComPtr<ID3D12Resource> scratch;
    UINT64 blas_size = 0, tlas_size = 0, scratch_size = 0;
    ComPtr<ID3D12Resource> depth_copy;
    DXGI_FORMAT depth_copy_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depth_copy_view = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12Resource> target;
    DXGI_FORMAT target_format = DXGI_FORMAT_UNKNOWN;
    unsigned width = 0, height = 0;
    bool tlas_valid = false;
    struct Retired { ComPtr<ID3D12Resource> resource; unsigned pass; };
    std::vector<Retired> retired;
    unsigned passes = 0;

    bool compile(const char* source, const wchar_t* entry, const wchar_t* profile, ComPtr<IDxcBlob>& out);
    bool load_compiler(const std::filesystem::path& dir);
    bool create_pipelines();
    bool ensure_size(unsigned width, unsigned height, DXGI_FORMAT color_format, DXGI_FORMAT color_resource_format,
                     DXGI_FORMAT depth_format);
    bool ensure_acceleration(UINT64 blas_result, UINT64 tlas_result, UINT64 scratch_needed);
    void retire(ComPtr<ID3D12Resource>& resource);
    void reap();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned parity, unsigned slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned parity, unsigned slot) const;
};

bool Runtime::Impl::load_compiler(const std::filesystem::path& dir) {
    if (compiler) return true;
    if (!dir.empty()) {
        dxil = LoadLibraryW((dir / "dxil.dll").c_str());
        dxc = LoadLibraryW((dir / "dxcompiler.dll").c_str());
    }
    if (dxil == nullptr) dxil = LoadLibraryW(L"dxil.dll");
    if (dxc == nullptr) dxc = LoadLibraryW(L"dxcompiler.dll");
    if (dxc == nullptr) { problem = "dxcompiler.dll was not found; deploy it beside bridger.dll"; return false; }
    const auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxc, "DxcCreateInstance"));
    if (create == nullptr) { problem = "dxcompiler.dll has no DxcCreateInstance"; return false; }
    if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) {
        problem = "the DXC compiler could not be created";
        compiler = nullptr;
        return false;
    }
    if (dxil == nullptr) log::warn("fx/rt: dxil.dll not found; DXIL will be unsigned and the runtime may refuse it");
    return true;
}

bool Runtime::Impl::compile(const char* source, const wchar_t* entry, const wchar_t* profile, ComPtr<IDxcBlob>& out) {
    DxcBuffer buffer{source, std::strlen(source), DXC_CP_UTF8};
    LPCWSTR args[] = {L"-E", entry, L"-T", profile, L"-O3", L"-Qstrip_debug", L"-Qstrip_reflect", L"-Wno-ignored-attributes"};
    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(&buffer, args, static_cast<UINT32>(std::size(args)), nullptr, IID_PPV_ARGS(&result))) || !result) {
        problem = "DXC compile call failed";
        return false;
    }
    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (FAILED(status)) {
        problem = std::format("shader {} failed to compile: {}", narrow(entry),
                              errors && errors->GetStringLength() ? errors->GetStringPointer() : "no output");
        return false;
    }
    if (errors && errors->GetStringLength() > 0) log::info("fx/rt: compiler output for {}: {}", narrow(entry), errors->GetStringPointer());
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&out), nullptr)) || !out) {
        problem = "DXC produced no object";
        return false;
    }
    return true;
}

bool Runtime::Impl::create_pipelines() {
    {
        D3D12_ROOT_PARAMETER1 params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[3].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC1 desc{};
        desc.NumParameters = 4;
        desc.pParameters = params;
        transform_root = create_root(device.Get(), desc, problem);
        if (!transform_root) return false;
    }
    {
        D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 1};
        D3D12_ROOT_PARAMETER1 params[5]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[3].Descriptor = {2, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[4].DescriptorTable = {2, ranges};
        for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC1 desc{};
        desc.NumParameters = 5;
        desc.pParameters = params;
        trace_root = create_root(device.Get(), desc, problem);
        if (!trace_root) return false;
    }
    {
        D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0};
        D3D12_ROOT_PARAMETER1 params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC1 desc{};
        desc.NumParameters = 2;
        desc.pParameters = params;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        composite_root = create_root(device.Get(), desc, problem);
        if (!composite_root) return false;
    }
    ComPtr<IDxcBlob> transform, trace, vs, ps;
    if (!compile(kTransformShader, L"main", L"cs_6_0", transform)) return false;
    if (!compile(kTraceShader, L"main", L"cs_6_5", trace)) return false;
    if (!compile(kCompositeShader, L"vs_main", L"vs_6_0", vs)) return false;
    if (!compile(kCompositeShader, L"ps_main", L"ps_6_0", ps)) return false;
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = transform_root.Get();
        desc.CS = {transform->GetBufferPointer(), transform->GetBufferSize()};
        if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&transform_pso)))) {
            problem = dxil ? "the transform pipeline was refused" : "the transform pipeline was refused: DXIL unsigned (dxil.dll missing)";
            return false;
        }
        desc.pRootSignature = trace_root.Get();
        desc.CS = {trace->GetBufferPointer(), trace->GetBufferSize()};
        if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&trace_pso)))) {
            problem = "the trace pipeline was refused (inline ray queries need raytracing tier 1.1)";
            return false;
        }
    }
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE Runtime::Impl::cpu(unsigned p, unsigned slot) const {
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(p * 4 + slot) * descriptor_stride;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Runtime::Impl::gpu(unsigned p, unsigned slot) const {
    auto h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(p * 4 + slot) * descriptor_stride;
    return h;
}

void Runtime::Impl::retire(ComPtr<ID3D12Resource>& resource) {
    if (resource) retired.push_back({std::move(resource), passes});
    resource = nullptr;
}

void Runtime::Impl::reap() {
    std::erase_if(retired, [&](const Retired& r) { return passes - r.pass > kRetirePasses; });
}

bool Runtime::Impl::ensure_size(unsigned w, unsigned h, DXGI_FORMAT color_view, DXGI_FORMAT color_resource, DXGI_FORMAT depth_format) {
    DXGI_FORMAT depth_resource = DXGI_FORMAT_UNKNOWN, depth_view = DXGI_FORMAT_UNKNOWN;
    const bool has_depth = depth_view_format(depth_format, depth_resource, depth_view);
    const bool same = width == w && height == h && target_format == color_resource && output
                   && (!has_depth || (depth_copy && depth_copy_format == depth_resource));
    if (same) return true;
    retire(output);
    retire(target);
    retire(depth_copy);
    composite_pso = nullptr;
    width = w;
    height = h;
    target_format = color_resource;
    output = create_texture(device.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"bridger rt output");
    target = create_texture(device.Get(), w, h, color_resource, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_COPY_SOURCE, L"bridger rt target");
    if (has_depth) {
        depth_copy = create_texture(device.Get(), w, h, depth_resource, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_COPY_DEST, L"bridger rt depth");
        depth_copy_format = depth_resource;
        depth_copy_view = depth_view;
    }
    if (!output || !target || (has_depth && !depth_copy)) { problem = "per size textures could not be created"; return false; }

    for (unsigned p = 0; p < kParities; ++p) {
        if (depth_copy) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = depth_copy_view;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(depth_copy.Get(), &srv, cpu(p, 0));
        } else {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R32_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(nullptr, &srv, cpu(p, 0));
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, cpu(p, 1));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(output.Get(), &srv, cpu(p, 3));
    }
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = color_view;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(target.Get(), &rtv, rtv_heap->GetCPUDescriptorHandleForHeapStart());

    ComPtr<IDxcBlob> vs, ps;
    if (!compile(kCompositeShader, L"vs_main", L"vs_6_0", vs) || !compile(kCompositeShader, L"ps_main", L"ps_6_0", ps)) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = composite_root.Get();
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = color_view;
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&composite_pso)))) {
        problem = std::format("the composite pipeline was refused for format {}", static_cast<int>(color_view));
        return false;
    }
    return true;
}

bool Runtime::Impl::ensure_acceleration(UINT64 blas_result, UINT64 tlas_result, UINT64 scratch_needed) {
    auto grow = [&](ComPtr<ID3D12Resource>& r, UINT64& have, UINT64 need, D3D12_RESOURCE_STATES state, const wchar_t* name) {
        if (r && have >= need) return true;
        retire(r);
        have = std::max<UINT64>(need + need / 4, 1u << 20);
        r = create_buffer(device.Get(), have, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, state, name);
        if (!r) { problem = "acceleration structure memory could not be allocated"; have = 0; return false; }
        return true;
    };
    const bool blas_grew = !(blas && blas_size >= blas_result);
    if (!grow(blas, blas_size, blas_result, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"bridger rt blas")) return false;
    if (!grow(tlas, tlas_size, tlas_result, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"bridger rt tlas")) return false;
    if (!grow(scratch, scratch_size, scratch_needed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"bridger rt scratch")) return false;
    if (blas_grew) tlas_valid = false;
    return true;
}

Runtime::Runtime() : impl_(new Impl) {}
Runtime::~Runtime() { shutdown(false); delete impl_; }

bool Runtime::ready() const { return impl_->ready; }
const std::string& Runtime::problem() const { return impl_->problem; }
unsigned Runtime::raytracing_tier() const { return impl_->tier; }
unsigned Runtime::current_parity() const { return impl_->parity.load(std::memory_order_acquire); }
ID3D12Resource* Runtime::vertex_buffer(unsigned parity) const { return impl_->frames[parity % kParities].vertices.Get(); }
ID3D12Resource* Runtime::counter_buffer(unsigned parity) const { return impl_->frames[parity % kParities].counters.Get(); }
std::uint64_t Runtime::budget_vertices() const { return impl_->budget; }
ID3D12Resource* Runtime::output() const { return impl_->output.Get(); }
std::uint64_t Runtime::blas_bytes() const { return impl_->blas_size; }
std::uint64_t Runtime::scratch_bytes() const { return impl_->scratch_size; }
unsigned Runtime::regions_skipped_last() const { return impl_->skipped_last; }

bool Runtime::initialise(ID3D12Device* device, const std::filesystem::path& dxc_dir, std::uint64_t budget_vertices, unsigned max_regions) {
    auto& i = *impl_;
    if (i.ready) return true;
    if (device == nullptr) { i.problem = "no device"; return false; }
    i.device = device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&i.device5)))) { i.problem = "the device has no ID3D12Device5 (raytracing needs a newer runtime)"; return false; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof options))) { i.problem = "feature query failed"; return false; }
    i.tier = static_cast<unsigned>(options.RaytracingTier);
    if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
        i.problem = options.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED ? "this device has no raytracing support" : "inline ray queries need raytracing tier 1.1";
        return false;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof model)) || model.HighestShaderModel < D3D_SHADER_MODEL_6_5) {
        i.problem = "shader model 6.5 is not supported";
        return false;
    }
    if (!i.load_compiler(dxc_dir)) return false;
    if (!i.create_pipelines()) return false;

    i.descriptor_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    i.rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kParities * 4;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&i.heap)))) { i.problem = "descriptor heap creation failed"; return false; }
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = 1;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&i.rtv_heap)))) { i.problem = "RTV heap creation failed"; return false; }

    i.budget = std::max<std::uint64_t>(budget_vertices, 4096);
    i.max_regions = std::clamp(max_regions, 16u, 16384u);
    const UINT64 counter_bytes = std::max<UINT64>(kCounterBytes * i.max_regions, 4096);
    i.zeros = create_buffer(device, counter_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, L"bridger rt zeros");
    if (!i.zeros) { i.problem = "upload memory could not be allocated"; return false; }
    {
        void* mapped = nullptr;
        if (FAILED(i.zeros->Map(0, nullptr, &mapped))) { i.problem = "upload memory could not be mapped"; return false; }
        std::memset(mapped, 0, static_cast<std::size_t>(counter_bytes));
        i.zeros->Unmap(0, nullptr);
    }
    for (unsigned p = 0; p < kParities; ++p) {
        auto& f = i.frames[p];
        f.vertices = create_buffer(device, i.budget * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON, p == 0 ? L"bridger rt stream 0" : L"bridger rt stream 1");
        f.counters = create_buffer(device, counter_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                                   D3D12_RESOURCE_STATE_COMMON, p == 0 ? L"bridger rt counters 0" : L"bridger rt counters 1");
        f.regions_upload = create_buffer(device, sizeof(GpuRegion) * i.max_regions, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, L"bridger rt regions");
        f.constants = create_buffer(device, 3 * 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, L"bridger rt constants");
        f.instance = create_buffer(device, sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                   D3D12_RESOURCE_STATE_GENERIC_READ, L"bridger rt instance");
        if (!f.vertices || !f.counters || !f.regions_upload || !f.constants || !f.instance) {
            i.problem = std::format("stream-out memory could not be allocated ({} MB per frame)", (i.budget * 16) >> 20);
            return false;
        }
        void* mapped = nullptr;
        if (FAILED(f.regions_upload->Map(0, nullptr, &mapped))) { i.problem = "region upload could not be mapped"; return false; }
        f.regions_mapped = static_cast<GpuRegion*>(mapped);
        if (FAILED(f.constants->Map(0, nullptr, &mapped))) { i.problem = "constant upload could not be mapped"; return false; }
        f.constants_mapped = static_cast<std::uint8_t*>(mapped);
        if (FAILED(f.instance->Map(0, nullptr, &mapped))) { i.problem = "instance upload could not be mapped"; return false; }
        f.instance_mapped = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(mapped);
        f.records.resize(i.max_regions);
    }
    i.sorted.reserve(i.max_regions);
    i.geometries.reserve(i.max_regions);
    i.ready = true;
    i.accepting = true;
    i.problem.clear();
    log::info("fx/rt: runtime ready: raytracing tier {}, {} vertices per frame ({} MB), {} regions",
              i.tier == 10 ? "1.0" : i.tier == 11 ? "1.1" : "?", i.budget, (i.budget * 16) >> 20, i.max_regions);
    return true;
}

void Runtime::shutdown(bool everything) {
    auto& i = *impl_;
    i.accepting = false;
    i.ready = false;
    i.retired.clear();
    i.blas = nullptr; i.tlas = nullptr; i.scratch = nullptr;
    i.blas_size = i.tlas_size = i.scratch_size = 0;
    i.output = nullptr; i.target = nullptr; i.depth_copy = nullptr;
    i.width = i.height = 0;
    i.transform_pso = nullptr; i.trace_pso = nullptr; i.composite_pso = nullptr;
    i.transform_root = nullptr; i.trace_root = nullptr; i.composite_root = nullptr;
    i.heap = nullptr; i.rtv_heap = nullptr;
    if (everything) {
        for (auto& f : i.frames) {
            f.vertices = nullptr; f.counters = nullptr; f.regions_upload = nullptr; f.constants = nullptr; f.instance = nullptr;
            f.regions_mapped = nullptr; f.constants_mapped = nullptr; f.instance_mapped = nullptr;
        }
        i.zeros = nullptr;
    } else {
        for (auto& f : i.frames) { f.vertices.Detach(); f.counters.Detach(); }
        for (auto& f : i.frames) { f.regions_upload = nullptr; f.constants = nullptr; f.instance = nullptr; }
        i.zeros = nullptr;
    }
    i.compiler = nullptr;
    i.utils = nullptr;
    i.device5 = nullptr;
    i.device = nullptr;
}

bool Runtime::allocate(std::uint64_t vertices, std::uint64_t pipeline, Region& out) {
    auto& i = *impl_;
    if (!i.accepting || vertices == 0 || vertices > kMaxRegionVertices) {
        if (i.accepting) i.skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const std::uint64_t rounded = (vertices + kRegionAlignment - 1) & ~static_cast<std::uint64_t>(kRegionAlignment - 1);
    auto& f = i.frames[i.parity.load(std::memory_order_acquire) % kParities];
    const unsigned index = f.count.fetch_add(1, std::memory_order_acq_rel);
    if (index >= i.max_regions) { f.count.fetch_sub(1, std::memory_order_acq_rel); i.skipped.fetch_add(1, std::memory_order_relaxed); return false; }
    const std::uint64_t offset = f.used.fetch_add(rounded, std::memory_order_acq_rel);
    if (offset + rounded > i.budget) {
        f.used.fetch_sub(rounded, std::memory_order_acq_rel);
        f.records[index] = {};
        i.skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    f.records[index] = {offset, vertices, index, pipeline};
    out.buffer = f.vertices->GetGPUVirtualAddress() + offset * 16;
    out.size = vertices * 16;
    out.counter = f.counters->GetGPUVirtualAddress() + static_cast<UINT64>(index) * kCounterBytes;
    return true;
}

Runtime::PassResult Runtime::record(ID3D12GraphicsCommandList* commands, const TraceParams& params, ID3D12Resource* color,
                                    DXGI_FORMAT color_view_format, ID3D12Resource* depth, DXGI_FORMAT depth_format, bool output_only) {
    auto& i = *impl_;
    PassResult result;
    if (!i.ready || commands == nullptr) { result.note = "runtime not ready"; return result; }
    ++i.passes;
    i.reap();
    i.skipped_last = i.skipped.exchange(0, std::memory_order_acq_rel);

    const unsigned consumed = i.parity.load(std::memory_order_acquire) % kParities;
    auto& f = i.frames[consumed];
    const unsigned count = std::min(f.count.load(std::memory_order_acquire), i.max_regions);
    auto& next = i.frames[(consumed + 1) % kParities];
    std::fill_n(next.records.begin(), std::min(next.last_count, i.max_regions), RegionRecord{});
    next.count.store(0, std::memory_order_release);
    next.used.store(0, std::memory_order_release);
    f.last_count = count;
    i.parity.store(consumed + 1, std::memory_order_release);

    i.sorted.clear();
    for (unsigned r = 0; r < count; ++r) {
        const auto& rec = f.records[r];
        if (rec.vertices == 0) continue;
        i.sorted.push_back({static_cast<std::uint32_t>(rec.offset), static_cast<std::uint32_t>(rec.offset >> 32),
                            static_cast<std::uint32_t>(rec.vertices), rec.counter});
    }
    std::sort(i.sorted.begin(), i.sorted.end(), [](const GpuRegion& a, const GpuRegion& b) { return a.offset_lo < b.offset_lo; });
    std::uint64_t total = 0;
    for (const auto& r : i.sorted) total = std::max<std::uint64_t>(total, r.offset_lo + r.bound);
    result.regions = static_cast<unsigned>(i.sorted.size());
    result.vertices = total;
    if (!i.sorted.empty()) std::memcpy(f.regions_mapped, i.sorted.data(), i.sorted.size() * sizeof(GpuRegion));

    ID3D12DescriptorHeap* heaps[] = {i.heap.Get()};
    commands->SetDescriptorHeaps(1, heaps);

    ComPtr<ID3D12GraphicsCommandList4> commands4;
    if (FAILED(commands->QueryInterface(IID_PPV_ARGS(&commands4)))) { result.note = "the command list has no raytracing interface"; return result; }

    barrier(commands, f.vertices.Get(), f.vertices_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(commands, f.counters.Get(), f.counters_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (total > 0) {
        TransformConstants tc{};
        const auto inv_view = math::inverse(params.view);
        std::memcpy(tc.inv_view, inv_view.m, sizeof tc.inv_view);
        const float p00 = params.projection.m[0], p11 = params.projection.m[5];
        tc.focal[0] = p00 != 0.0f ? 1.0f / p00 : 0.0f;
        tc.focal[1] = p11 != 0.0f ? 1.0f / p11 : 0.0f;
        tc.focal[2] = -params.projection.m[2];
        tc.focal[3] = -params.projection.m[6];
        tc.region_count = static_cast<std::uint32_t>(i.sorted.size());
        tc.total_vertices = static_cast<std::uint32_t>(total);
        std::memcpy(f.constants_mapped, &tc, sizeof tc);
        commands->SetComputeRootSignature(i.transform_root.Get());
        commands->SetPipelineState(i.transform_pso.Get());
        commands->SetComputeRootConstantBufferView(0, f.constants->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(1, f.vertices->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(2, f.counters->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(3, f.regions_upload->GetGPUVirtualAddress());
        commands->Dispatch(static_cast<UINT>((total + kThreadGroup - 1) / kThreadGroup), 1, 1);
    }
    barrier(commands, f.vertices.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    bool built = false;
    if (!i.sorted.empty()) {
        i.geometries.clear();
        const auto base = f.vertices->GetGPUVirtualAddress();
        for (const auto& r : i.sorted) {
            D3D12_RAYTRACING_GEOMETRY_DESC g{};
            g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            g.Triangles.VertexBuffer = {base + static_cast<UINT64>(r.offset_lo) * 16, 16};
            g.Triangles.VertexCount = r.bound - r.bound % 3;
            i.geometries.push_back(g);
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs{};
        blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blas_inputs.NumDescs = static_cast<UINT>(i.geometries.size());
        blas_inputs.pGeometryDescs = i.geometries.data();
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs{};
        tlas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        tlas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlas_inputs.NumDescs = 1;
        tlas_inputs.InstanceDescs = f.instance->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info{}, tlas_info{};
        i.device5->GetRaytracingAccelerationStructurePrebuildInfo(&blas_inputs, &blas_info);
        i.device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlas_inputs, &tlas_info);
        if (blas_info.ResultDataMaxSizeInBytes == 0) {
            result.note = "the runtime refused the geometry";
        } else if (i.ensure_acceleration(blas_info.ResultDataMaxSizeInBytes, tlas_info.ResultDataMaxSizeInBytes,
                                         std::max(blas_info.ScratchDataSizeInBytes, tlas_info.ScratchDataSizeInBytes))) {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.DestAccelerationStructureData = i.blas->GetGPUVirtualAddress();
            build.Inputs = blas_inputs;
            build.ScratchAccelerationStructureData = i.scratch->GetGPUVirtualAddress();
            commands4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
            uav_barrier(commands, i.blas.Get());
            uav_barrier(commands, i.scratch.Get());
            auto& instance = *f.instance_mapped;
            instance = {};
            instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1.0f;
            instance.InstanceMask = 0xff;
            instance.AccelerationStructure = i.blas->GetGPUVirtualAddress();
            build.DestAccelerationStructureData = i.tlas->GetGPUVirtualAddress();
            build.Inputs = tlas_inputs;
            commands4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
            uav_barrier(commands, i.tlas.Get());
            i.tlas_valid = true;
            built = true;
        } else {
            result.note = i.problem;
        }
    } else {
        result.note = "no geometry regions this frame";
    }

    const auto color_desc = color != nullptr ? color->GetDesc() : D3D12_RESOURCE_DESC{};
    const bool sized = params.width > 0 && params.height > 0
        && i.ensure_size(params.width, params.height, color_view_format, color != nullptr ? color_desc.Format : DXGI_FORMAT_R16G16B16A16_FLOAT, depth_format);
    if (built && i.tlas_valid && sized) {
        const bool has_depth = depth != nullptr && i.depth_copy;
        if (has_depth) {
            barrier(commands, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->CopyResource(i.depth_copy.Get(), depth);
            barrier(commands, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barrier(commands, i.depth_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        TraceConstants tc{};
        const auto inv_vp = math::inverse(math::multiply(params.projection, params.view));
        std::memcpy(tc.inv_view_proj, inv_vp.m, sizeof tc.inv_view_proj);
        tc.camera[0] = params.camera[0]; tc.camera[1] = params.camera[1]; tc.camera[2] = params.camera[2];
        tc.camera[3] = params.camera_valid ? 1.0f : 0.0f;
        tc.size[0] = static_cast<float>(params.width); tc.size[1] = static_cast<float>(params.height);
        tc.size[2] = 1.0f / params.width; tc.size[3] = 1.0f / params.height;
        const float az = params.settings.sun_azimuth * 0.017453292f, el = params.settings.sun_elevation * 0.017453292f;
        tc.sun[0] = std::cos(el) * std::cos(az); tc.sun[1] = std::cos(el) * std::sin(az); tc.sun[2] = std::sin(el);
        tc.sun[3] = params.settings.strength;
        tc.ao[0] = std::max(0.05f, params.settings.radius);
        tc.ao[1] = std::max(0.0f, params.settings.bias);
        tc.ao[2] = static_cast<float>(std::clamp(params.settings.rays, 1, 64));
        tc.ao[3] = static_cast<float>(params.frame % 1024);
        tc.flags[0] = static_cast<std::uint32_t>(std::clamp(params.settings.mode, 0, 2));
        tc.flags[1] = params.depth_reversed ? 1u : 0u;
        tc.flags[2] = has_depth ? 1u : 0u;
        std::memcpy(f.constants_mapped + 256, &tc, sizeof tc);
        commands->SetComputeRootSignature(i.trace_root.Get());
        commands->SetPipelineState(i.trace_pso.Get());
        commands->SetComputeRootConstantBufferView(0, f.constants->GetGPUVirtualAddress() + 256);
        commands->SetComputeRootShaderResourceView(1, f.vertices->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(2, f.regions_upload->GetGPUVirtualAddress());
        commands->SetComputeRootShaderResourceView(3, i.tlas->GetGPUVirtualAddress());
        commands->SetComputeRootDescriptorTable(4, i.gpu(consumed, 0));
        commands->Dispatch((params.width + 7) / 8, (params.height + 7) / 8, 1);
        if (has_depth) barrier(commands, i.depth_copy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        result.traced = true;

        if (!output_only && color != nullptr && i.composite_pso) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = color_view_format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            i.device->CreateShaderResourceView(color, &srv, i.cpu(consumed, 2));
            float* cc = reinterpret_cast<float*>(f.constants_mapped + 512);
            cc[0] = static_cast<float>(tc.flags[0]);
            cc[1] = std::clamp(params.settings.strength, 0.0f, 1.0f);
            cc[2] = cc[3] = 0.0f;
            barrier(commands, i.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            barrier(commands, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            barrier(commands, i.target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const auto rtv = i.rtv_heap->GetCPUDescriptorHandleForHeapStart();
            commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(params.width), static_cast<float>(params.height), 0.0f, 1.0f};
            const D3D12_RECT scissor{0, 0, static_cast<LONG>(params.width), static_cast<LONG>(params.height)};
            commands->RSSetViewports(1, &viewport);
            commands->RSSetScissorRects(1, &scissor);
            commands->SetGraphicsRootSignature(i.composite_root.Get());
            commands->SetPipelineState(i.composite_pso.Get());
            commands->SetGraphicsRootConstantBufferView(0, f.constants->GetGPUVirtualAddress() + 512);
            commands->SetGraphicsRootDescriptorTable(1, i.gpu(consumed, 2));
            commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commands->DrawInstanced(3, 1, 0, 0);
            barrier(commands, i.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barrier(commands, color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            commands->CopyResource(color, i.target.Get());
            barrier(commands, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barrier(commands, i.output.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    } else if (result.note.empty()) {
        result.note = !sized ? i.problem : "nothing to trace";
    }

    barrier(commands, f.vertices.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);
    barrier(commands, f.counters.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(commands, next.counters.Get(), next.counters_state, D3D12_RESOURCE_STATE_COPY_DEST);
    const UINT64 counter_bytes = kCounterBytes * i.max_regions;
    commands->CopyBufferRegion(f.counters.Get(), 0, i.zeros.Get(), 0, counter_bytes);
    commands->CopyBufferRegion(next.counters.Get(), 0, i.zeros.Get(), 0, counter_bytes);
    barrier(commands, f.counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    barrier(commands, next.counters.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    f.vertices_state = f.counters_state = next.counters_state = D3D12_RESOURCE_STATE_STREAM_OUT;
    return result;
}

namespace {

struct Globals {
    std::mutex mutex;
    bool boot = false;
    Settings settings;
    std::uint64_t budget_vertices = 6u << 20;
    unsigned max_regions = 4096;
    std::filesystem::path root;
    Runtime* runtime = nullptr;
    std::atomic<Runtime*> live{nullptr};
    ID3D12Device* device = nullptr;
    bool init_failed = false;
    std::string problem;
    std::string note;
    unsigned regions = 0;
    std::uint64_t vertices = 0;
    unsigned passes = 0;
    unsigned faults = 0;
    unsigned width = 0, height = 0;
};

Globals& g() {
    static Globals instance;
    return instance;
}

}

void configure(const std::filesystem::path& root) {
    auto& s = g();
    std::scoped_lock lock(s.mutex);
    s.root = root;
    s.boot = settings::get_bool("bridger", "fx.raytracing", false);
    s.settings.enabled = settings::get_bool("bridger", "fx.rt_enabled", true);
    s.settings.mode = static_cast<int>(settings::get_number("bridger", "fx.rt_mode", 0));
    s.settings.rays = static_cast<int>(settings::get_number("bridger", "fx.rt_rays", 4));
    s.settings.radius = static_cast<float>(settings::get_number("bridger", "fx.rt_radius", 2.0));
    s.settings.strength = static_cast<float>(settings::get_number("bridger", "fx.rt_strength", 1.0));
    s.settings.bias = static_cast<float>(settings::get_number("bridger", "fx.rt_bias", 0.02));
    s.settings.sun_azimuth = static_cast<float>(settings::get_number("bridger", "fx.rt_sun_azimuth", 135.0));
    s.settings.sun_elevation = static_cast<float>(settings::get_number("bridger", "fx.rt_sun_elevation", 45.0));
    const double budget_mb = settings::get_number("bridger", "fx.rt_budget_mb", 96.0);
    s.budget_vertices = static_cast<std::uint64_t>(std::clamp(budget_mb, 8.0, 1024.0) * (1u << 20) / 16);
    s.max_regions = static_cast<unsigned>(std::clamp(settings::get_number("bridger", "fx.rt_max_regions", 4096), 64.0, 16384.0));
}

void save_settings() {
    auto& s = g();
    std::scoped_lock lock(s.mutex);
    settings::set_bool("bridger", "fx.raytracing", s.boot);
    settings::set_bool("bridger", "fx.rt_enabled", s.settings.enabled);
    settings::set_number("bridger", "fx.rt_mode", s.settings.mode);
    settings::set_number("bridger", "fx.rt_rays", s.settings.rays);
    settings::set_number("bridger", "fx.rt_radius", s.settings.radius);
    settings::set_number("bridger", "fx.rt_strength", s.settings.strength);
    settings::set_number("bridger", "fx.rt_bias", s.settings.bias);
    settings::set_number("bridger", "fx.rt_sun_azimuth", s.settings.sun_azimuth);
    settings::set_number("bridger", "fx.rt_sun_elevation", s.settings.sun_elevation);
}

bool boot_enabled() { return g().boot; }
Settings& settings() { return g().settings; }

void attach(ID3D12Device* device) {
    auto& s = g();
    std::scoped_lock lock(s.mutex);
    if (device == nullptr || device == s.device) return;
    if (s.device != nullptr) {
        s.live.store(nullptr, std::memory_order_release);
        s.runtime = nullptr;
        s.device->Release();
        s.init_failed = false;
    }
    s.device = device;
    device->AddRef();
}

bool allocate_region(std::uint64_t vertices, std::uint64_t pipeline, Region& out) {
    auto* runtime = g().live.load(std::memory_order_acquire);
    return runtime != nullptr && runtime->allocate(vertices, pipeline, out);
}

namespace {

struct RenderCall {
    ID3D12GraphicsCommandList* commands;
    const ngx::Snapshot* inputs;
    TraceParams params;
    Runtime::PassResult result;
};

void render_body(void* raw) {
    auto& c = *static_cast<RenderCall*>(raw);
    auto& s = g();
    auto* color = const_cast<ID3D12Resource*>(static_cast<const ID3D12Resource*>(c.inputs->color));
    auto* depth = const_cast<ID3D12Resource*>(static_cast<const ID3D12Resource*>(c.inputs->depth));
    DXGI_FORMAT color_view = c.inputs->color_format;
    const auto desc = color->GetDesc();
    if (color_view == DXGI_FORMAT_UNKNOWN || color_view == DXGI_FORMAT_R10G10B10A2_TYPELESS) color_view = DXGI_FORMAT_R10G10B10A2_UNORM;
    if (desc.Format != DXGI_FORMAT_R10G10B10A2_TYPELESS && desc.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS
        && desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && desc.Format != DXGI_FORMAT_R11G11B10_FLOAT) color_view = desc.Format;
    if (desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS) color_view = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) color_view = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    if (depth != nullptr) {
        const auto d = depth->GetDesc();
        depth_format = d.Format;
        if (d.SampleDesc.Count != 1 || d.MipLevels != 1 || d.DepthOrArraySize != 1
            || d.Width != c.params.width || d.Height != c.params.height) depth = nullptr;
    }
    c.result = s.runtime->record(c.commands, c.params, color, color_view, depth, depth_format, false);
}

}

void render(ID3D12GraphicsCommandList* commands, const ngx::Snapshot& inputs, const math::Mat4& view,
            const math::Mat4& projection, bool camera_valid, bool depth_reversed) {
    auto& s = g();
    std::scoped_lock lock(s.mutex);
    if (!s.boot || commands == nullptr || inputs.color == nullptr) return;
    if (s.runtime == nullptr && !s.init_failed) {
        if (s.device == nullptr) { s.problem = "no device yet"; return; }
        s.runtime = new Runtime;
        if (!s.runtime->initialise(s.device, s.root, s.budget_vertices, s.max_regions)) {
            s.problem = s.runtime->problem();
            s.init_failed = true;
            log::error("fx/rt: raytracing unavailable: {}", s.problem);
            return;
        }
        s.problem.clear();
        s.live.store(s.runtime, std::memory_order_release);
    }
    if (s.runtime == nullptr || !s.settings.enabled) return;
    if (!inputs.render_width || !inputs.render_height || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        s.note = "waiting for a direct command list and NGX input sizes";
        return;
    }
    RenderCall call{commands, &inputs, {}, {}};
    call.params.view = view;
    call.params.projection = projection;
    const auto inv_view = math::inverse(view);
    call.params.camera[0] = inv_view.m[3];
    call.params.camera[1] = inv_view.m[7];
    call.params.camera[2] = inv_view.m[11];
    call.params.camera_valid = camera_valid;
    call.params.depth_reversed = depth_reversed;
    call.params.jitter_x = inputs.jitter_x;
    call.params.jitter_y = inputs.jitter_y;
    call.params.width = inputs.render_width;
    call.params.height = inputs.render_height;
    call.params.frame = ++s.passes;
    call.params.settings = s.settings;
    const auto fault = guarded_call(render_body, &call);
    if (fault != 0) {
        ++s.faults;
        s.settings.enabled = false;
        s.note = std::format("pass faulted ({:#x}); raytracing disabled", fault);
        log::error("fx/rt: pass faulted {:#x}: {}", fault, describe_last_fault());
        return;
    }
    s.regions = call.result.regions;
    s.vertices = call.result.vertices;
    s.width = inputs.render_width;
    s.height = inputs.render_height;
    s.note = call.result.traced ? "traced and composited before NGX" : call.result.note;
}

Snapshot snapshot() {
    auto& s = g();
    std::scoped_lock lock(s.mutex);
    Snapshot out;
    out.boot = s.boot;
    out.ready = s.runtime != nullptr && s.runtime->ready();
    out.detail = s.problem.empty() ? s.note : s.problem;
    out.problem = s.init_failed ? "unavailable" : "";
    out.raytracing_tier = s.runtime != nullptr ? s.runtime->raytracing_tier() : 0;
    out.regions = s.regions;
    out.vertices = s.vertices;
    out.passes = s.passes;
    out.faults = s.faults;
    out.width = s.width;
    out.height = s.height;
    if (s.runtime != nullptr) {
        out.budget_vertices = s.runtime->budget_vertices();
        out.blas_bytes = s.runtime->blas_bytes();
        out.scratch_bytes = s.runtime->scratch_bytes();
        out.regions_skipped = s.runtime->regions_skipped_last();
    }
    return out;
}

}
