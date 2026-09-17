#include "fx/fx.h"

#include <Windows.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <tuple>
#include <memory>
#include <wrl/client.h>

#include "core/guard.h"
#include "core/log.h"
#include "core/settings.h"
#include "fx/depth_capture.h"
#include "fx/fx_internal.h"
#include "fx/ngx.h"
#include "fx/pipeline.h"
#include "fx/prelude.h"
#include "fx/readback.h"
#include "fx/rt.h"
#include "overlay/d3d.h"

namespace bridger::fx {
namespace {

constexpr unsigned kStagingDescriptors = 4096;
constexpr unsigned kRtvDescriptors = 512;
constexpr unsigned kDsvDescriptors = 128;
constexpr unsigned kSrvSlots = 9;
constexpr unsigned kMaxEffectsPerFrame = 512;
constexpr unsigned kFrameDescriptors = kSrvSlots * (kMaxEffectsPerFrame + 4);
constexpr std::size_t kConstantRingBytes = 2u << 20;
constexpr unsigned kTimestampsPerFrame = kMaxEffectsPerFrame * 2;
constexpr unsigned kHotReloadInterval = 30;
constexpr std::size_t kVertexStride = sizeof(BridgerFxVertex);
constexpr std::size_t kConstantAlignment = 256;
constexpr float kFarDepth = 0.0f;

constexpr std::uintptr_t kGetLocalPlayer = 0x22075b0;
constexpr std::uintptr_t kGetLastActivatedCamera = 0x2207590;
constexpr std::size_t kEntityOrientation = 200;
constexpr std::size_t kCameraFov = 964;
constexpr std::size_t kCameraNear = 1084;
constexpr std::size_t kCameraFar = 1088;
constexpr unsigned kMaxCameraFaults = 5;

struct FrameConstants {
    float view[16];
    float proj[16];
    float view_proj[16];
    float inv_view_proj[16];
    float inv_proj[16];
    float camera_position[4];
    float camera_forward[4];
    float camera_up[4];
    float camera_right[4];
    float resolution[4];
    float time[4];
    float depth[4];
    float fov[4];
    float motion[4];
};
static_assert(sizeof(FrameConstants) == 464);

struct ObjectConstants {
    float model[16];
    float tint[4];
};

template <typename T>
void release(T*& pointer) {
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

struct RingBuffer {
    ID3D12Resource* resource = nullptr;
    std::uint8_t* mapped = nullptr;
    std::size_t capacity = 0;
    std::size_t used = 0;
};

struct FrameSlot {
    RingBuffer constants;
    RingBuffer vertices;
    RingBuffer indices;
    ID3D12Resource* readback = nullptr;
    unsigned descriptors_used = 0;
    unsigned queries_used = 0;
    std::vector<Handle> timed;
    bool timed_valid = false;
};

struct Retired {
    std::uint64_t frame;
    IUnknown* object;
};

using PsoKey = std::tuple<Handle, unsigned, int, int, int, int, int, int, int>;

struct Device {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned width = 0;
    unsigned height = 0;
    unsigned frame_count = 0;

    ID3D12RootSignature* root = nullptr;
    ID3D12DescriptorHeap* staging = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12DescriptorHeap* dsv_heap = nullptr;
    ID3D12DescriptorHeap* gpu_heap = nullptr;
    ID3D12QueryHeap* queries = nullptr;
    unsigned srv_stride = 0;
    unsigned rtv_stride = 0;
    unsigned dsv_stride = 0;
    std::vector<unsigned> staging_free;
    unsigned staging_next = 0;
    std::vector<unsigned> rtv_free;
    unsigned rtv_next = 0;
    std::vector<unsigned> dsv_free;
    unsigned dsv_next = 0;
    std::uint64_t timestamp_frequency = 0;

    std::vector<FrameSlot> frames;
    std::vector<Retired> retired;
    std::uint64_t frame_counter = 0;

    GpuTexture scene;
    GpuTexture chain[2];
    GpuTexture history;
    GpuTexture resolved_history;
    GpuTexture mod_depth;
    GpuTexture scene_depth;
    GpuTexture white;
    GpuTexture far_depth;
    GpuTexture scene_motion;
    GpuTexture pre_history;
    bool pre_history_valid = false;
    float motion_scale[2]{};
    bool motion_valid = false;
    bool motion_reset = true;
    bool history_valid = false;
    bool scene_depth_valid = false;
    bool mod_depth_cleared = false;

    std::map<PsoKey, ID3D12PipelineState*> pipelines;
    Handle builtin_world = 0;
    Handle builtin_screen = 0;
    Handle builtin_fullscreen = 0;
    Handle builtin_history = 0;
    bool ready = false;

    std::chrono::steady_clock::time_point started;
    double elapsed = 0.0;
    unsigned presented = 0;
};

Device g_gpu;
State g_state;
using Microsoft::WRL::ComPtr;
struct UpscaleJob {
    ~UpscaleJob();
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::vector<ComPtr<IUnknown>> leases;
    FrameSlot slot;
    GpuTexture scene, input, target, depth, mod_depth, history;
    bool detached = false;
    bool submitted = false;
    std::vector<std::string> owners;
};
std::vector<std::unique_ptr<UpscaleJob>> g_upscale_jobs;
ComPtr<ID3D12CommandQueue> g_upscale_queue;
void reap_upscale_jobs();
void detach_upscale_jobs();
unsigned g_dump_backbuffer = 0;
std::string g_dump_batch;
unsigned g_dump_sequence = 0;
std::filesystem::path g_root;
std::uintptr_t g_image_base = 0;
bool g_configured = false;

unsigned take_index(std::vector<unsigned>& free_list, unsigned& next, unsigned capacity) {
    if (!free_list.empty()) {
        const unsigned index = free_list.back();
        free_list.pop_back();
        return index;
    }
    return next < capacity ? next++ : ~0u;
}

D3D12_CPU_DESCRIPTOR_HANDLE staging_handle(unsigned index) {
    auto handle = g_gpu.staging->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * g_gpu.srv_stride;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(unsigned index) {
    auto handle = g_gpu.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * g_gpu.rtv_stride;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle(unsigned index) {
    auto handle = g_gpu.dsv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * g_gpu.dsv_stride;
    return handle;
}

void retire(IUnknown* object) {
    if (object != nullptr) {
        g_gpu.retired.push_back({g_gpu.frame_counter, object});
    }
}

void release_retired(bool everything) {
    for (std::size_t i = 0; i < g_gpu.retired.size();) {
        if (everything || g_gpu.retired[i].frame + g_gpu.frame_count + 1 <= g_gpu.frame_counter) {
            g_gpu.retired[i].object->Release();
            g_gpu.retired.erase(g_gpu.retired.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

DXGI_FORMAT to_dxgi(BridgerFxFormat format) {
    switch (format) {
        case BRIDGER_FX_FORMAT_BACKBUFFER: return g_gpu.format;
        case BRIDGER_FX_FORMAT_RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case BRIDGER_FX_FORMAT_RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case BRIDGER_FX_FORMAT_BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case BRIDGER_FX_FORMAT_R8: return DXGI_FORMAT_R8_UNORM;
        case BRIDGER_FX_FORMAT_RG8: return DXGI_FORMAT_R8G8_UNORM;
        case BRIDGER_FX_FORMAT_R16F: return DXGI_FORMAT_R16_FLOAT;
        case BRIDGER_FX_FORMAT_RG16F: return DXGI_FORMAT_R16G16_FLOAT;
        case BRIDGER_FX_FORMAT_RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case BRIDGER_FX_FORMAT_R32F: return DXGI_FORMAT_R32_FLOAT;
        case BRIDGER_FX_FORMAT_RG32F: return DXGI_FORMAT_R32G32_FLOAT;
        case BRIDGER_FX_FORMAT_RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case BRIDGER_FX_FORMAT_R11G11B10F: return DXGI_FORMAT_R11G11B10_FLOAT;
        case BRIDGER_FX_FORMAT_RGB10A2: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case BRIDGER_FX_FORMAT_BC1: return DXGI_FORMAT_BC1_UNORM;
        case BRIDGER_FX_FORMAT_BC2: return DXGI_FORMAT_BC2_UNORM;
        case BRIDGER_FX_FORMAT_BC3: return DXGI_FORMAT_BC3_UNORM;
        case BRIDGER_FX_FORMAT_BC4: return DXGI_FORMAT_BC4_UNORM;
        case BRIDGER_FX_FORMAT_BC5: return DXGI_FORMAT_BC5_UNORM;
        case BRIDGER_FX_FORMAT_BC6H: return DXGI_FORMAT_BC6H_UF16;
        case BRIDGER_FX_FORMAT_BC7: return DXGI_FORMAT_BC7_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool depth_copy_formats(DXGI_FORMAT source, DXGI_FORMAT& resource, DXGI_FORMAT& view) {
    switch (source) {
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
            resource = DXGI_FORMAT_R32_TYPELESS;
            view = DXGI_FORMAT_R32_FLOAT;
            return true;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            resource = DXGI_FORMAT_R32G8X24_TYPELESS;
            view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            return true;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
            resource = DXGI_FORMAT_R24G8_TYPELESS;
            view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            return true;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
            resource = DXGI_FORMAT_R16_TYPELESS;
            view = DXGI_FORMAT_R16_UNORM;
            return true;
        default:
            return false;
    }
}

void destroy_gpu_texture(GpuTexture& texture) {
    retire(texture.resource);
    texture.resource = nullptr;
    if (texture.srv != ~0u) {
        g_gpu.staging_free.push_back(texture.srv);
    }
    if (texture.rtv != ~0u) {
        g_gpu.rtv_free.push_back(texture.rtv);
    }
    if (texture.dsv != ~0u) {
        g_gpu.dsv_free.push_back(texture.dsv);
    }
    texture = GpuTexture{};
}

bool create_gpu_texture(GpuTexture& texture, unsigned width, unsigned height, unsigned mips,
                        DXGI_FORMAT resource_format, DXGI_FORMAT view_format, bool render_target,
                        bool depth_stencil, D3D12_RESOURCE_STATES initial) {
    destroy_gpu_texture(texture);
    if (width == 0 || height == 0) {
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(std::max(1u, mips));
    desc.Format = resource_format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (render_target) {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (depth_stencil) {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }

    D3D12_CLEAR_VALUE clear{};
    clear.Format = view_format;
    if (depth_stencil) {
        clear.DepthStencil.Depth = kFarDepth;
    }

    const auto result = g_gpu.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, initial,
        (render_target || depth_stencil) ? &clear : nullptr, IID_PPV_ARGS(&texture.resource));
    if (FAILED(result)) {
        log::error("fx: texture {}x{} format {} failed ({:#x})", width, height,
                   static_cast<int>(resource_format), static_cast<unsigned>(result));
        texture.resource = nullptr;
        return false;
    }

    texture.state = initial;
    texture.format = resource_format;
    texture.view_format = view_format;
    texture.width = width;
    texture.height = height;
    texture.mips = std::max(1u, mips);

    if (!depth_stencil) {
        texture.srv = take_index(g_gpu.staging_free, g_gpu.staging_next, kStagingDescriptors);
        if (texture.srv == ~0u) {
            log::error("fx: out of shader resource descriptors");
            destroy_gpu_texture(texture);
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = view_format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = texture.mips;
        g_gpu.device->CreateShaderResourceView(texture.resource, &view, staging_handle(texture.srv));
    }
    if (render_target) {
        texture.rtv = take_index(g_gpu.rtv_free, g_gpu.rtv_next, kRtvDescriptors);
        if (texture.rtv == ~0u) {
            log::error("fx: out of render target descriptors");
            destroy_gpu_texture(texture);
            return false;
        }
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = view_format;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_gpu.device->CreateRenderTargetView(texture.resource, &view, rtv_handle(texture.rtv));
    }
    if (depth_stencil) {
        texture.dsv = take_index(g_gpu.dsv_free, g_gpu.dsv_next, kDsvDescriptors);
        if (texture.dsv == ~0u) {
            log::error("fx: out of depth stencil descriptors");
            destroy_gpu_texture(texture);
            return false;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = view_format;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        g_gpu.device->CreateDepthStencilView(texture.resource, &view, dsv_handle(texture.dsv));
    }
    return true;
}

void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES to) {
    if (state == to || resource == nullptr) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = state;
    barrier.Transition.StateAfter = to;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commands->ResourceBarrier(1, &barrier);
    state = to;
}

void transition(ID3D12GraphicsCommandList* commands, GpuTexture& texture, D3D12_RESOURCE_STATES to) {
    transition(commands, texture.resource, texture.state, to);
}

void copy_into(ID3D12GraphicsCommandList* commands, GpuTexture& destination, ID3D12Resource* source,
               D3D12_RESOURCE_STATES& source_state) {
    transition(commands, source, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(commands, destination, D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyResource(destination.resource, source);
    transition(commands, destination, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ++g_state.stats.copies;
}

bool ensure_ring(RingBuffer& ring, std::size_t needed) {
    if (ring.capacity >= needed && ring.resource != nullptr) {
        return true;
    }
    if (ring.resource != nullptr) {
        ring.resource->Unmap(0, nullptr);
        retire(ring.resource);
        ring.resource = nullptr;
        ring.mapped = nullptr;
    }
    ring.capacity = std::max<std::size_t>(needed + (64u << 10), 256u << 10);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = ring.capacity;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&ring.resource)))) {
        ring.capacity = 0;
        return false;
    }
    D3D12_RANGE nothing{0, 0};
    void* mapped = nullptr;
    if (FAILED(ring.resource->Map(0, &nothing, &mapped))) {
        release(ring.resource);
        ring.capacity = 0;
        return false;
    }
    ring.mapped = static_cast<std::uint8_t*>(mapped);
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS ring_push(RingBuffer& ring, const void* data, std::size_t size,
                                   std::size_t alignment, std::size_t* offset_out = nullptr) {
    const std::size_t offset = (ring.used + alignment - 1) & ~(alignment - 1);
    if (!ensure_ring(ring, offset + size)) {
        return 0;
    }
    if (data != nullptr) {
        std::memcpy(ring.mapped + offset, data, size);
    }
    ring.used = offset + size;
    if (offset_out != nullptr) {
        *offset_out = offset;
    }
    return ring.resource->GetGPUVirtualAddress() + offset;
}

ID3D12Resource* create_upload_buffer(std::size_t size) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<std::size_t>(size, 256);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buffer = nullptr;
    if (FAILED(g_gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&buffer)))) {
        return nullptr;
    }
    return buffer;
}

class IncludeHandler final : public ID3DInclude {
public:
    std::vector<std::filesystem::path> search;
    std::vector<std::filesystem::path> opened;
    std::string prelude;

    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR file, LPCVOID parent, LPCVOID* data,
                                   UINT* bytes) override {
        const std::string name = file != nullptr ? file : "";
        if (name == "bridger.hlsli" || name == "bridger/bridger.hlsli") {
            *data = prelude.data();
            *bytes = static_cast<UINT>(prelude.size());
            return S_OK;
        }
        std::vector<std::filesystem::path> candidates;
        if (const auto found = parents_.find(parent); found != parents_.end()) {
            candidates.push_back(found->second / name);
        }
        for (const auto& directory : search) {
            candidates.push_back(directory / name);
        }
        for (const auto& candidate : candidates) {
            std::ifstream stream(candidate, std::ios::binary);
            if (!stream.is_open()) {
                continue;
            }
            std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            char* buffer = new char[text.size() + 1];
            std::memcpy(buffer, text.data(), text.size());
            buffer[text.size()] = 0;
            parents_[buffer] = candidate.parent_path();
            opened.push_back(candidate);
            *data = buffer;
            *bytes = static_cast<UINT>(text.size());
            return S_OK;
        }
        return E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE Close(LPCVOID data) override {
        if (data != nullptr && data != prelude.data()) {
            parents_.erase(data);
            delete[] static_cast<const char*>(data);
        }
        return S_OK;
    }

private:
    std::map<const void*, std::filesystem::path> parents_;
};

const char* default_vertex_entry(BridgerFxShaderKind kind) {
    switch (kind) {
        case BRIDGER_FX_SHADER_WORLD: return "bridger_world_vs";
        case BRIDGER_FX_SHADER_SCREEN: return "bridger_screen_vs";
        default: return "bridger_fullscreen_vs";
    }
}

std::filesystem::file_time_type newest_stamp(const std::vector<std::filesystem::path>& files) {
    std::filesystem::file_time_type newest{};
    for (const auto& file : files) {
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(file, ec);
        if (!ec && stamp > newest) {
            newest = stamp;
        }
    }
    return newest;
}

bool compile_shader(Shader& shader) {
    shader.dirty = false;
    const auto started = std::chrono::steady_clock::now();
    ++shader.compiles;

    std::string source = shader.source;
    std::string display = shader.name;
    if (!shader.path.empty()) {
        std::ifstream stream(shader.path, std::ios::binary);
        if (!stream.is_open()) {
            shader.log = "could not open " + shader.path.string();
            ++shader.failures;
            log::error("fx: shader {}: {}", shader.name, shader.log);
            return false;
        }
        source.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        display = shader.path.string();
    }

    std::string line_name = display;
    for (auto& c : line_name) {
        if (c == '\\') {
            c = '/';
        }
    }
    std::string full = prelude_source();
    full += "\n#line 1 \"";
    full += line_name;
    full += "\"\n";
    full += source;

    std::vector<std::string> names;
    std::vector<std::string> values;
    for (const auto& define : shader.defines) {
        const auto equals = define.find('=');
        names.push_back(define.substr(0, equals));
        values.push_back(equals == std::string::npos ? "1" : define.substr(equals + 1));
    }
    names.emplace_back("BRIDGER_FX");
    values.emplace_back("1");
    names.emplace_back(shader.kind == BRIDGER_FX_SHADER_WORLD ? "BRIDGER_WORLD"
                       : shader.kind == BRIDGER_FX_SHADER_SCREEN ? "BRIDGER_SCREEN"
                                                                 : "BRIDGER_FULLSCREEN");
    values.emplace_back("1");
    std::vector<D3D_SHADER_MACRO> macros;
    for (std::size_t i = 0; i < names.size(); ++i) {
        macros.push_back({names[i].c_str(), values[i].c_str()});
    }
    macros.push_back({nullptr, nullptr});

    IncludeHandler includes;
    includes.prelude = prelude_source();
    if (!shader.path.empty()) {
        includes.search.push_back(shader.path.parent_path());
    }
    includes.search.push_back(g_root / "shaders");

    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3
                     | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR;
    const std::string vs_entry = shader.vs_entry.empty() ? default_vertex_entry(shader.kind)
                                                         : shader.vs_entry;
    const std::string ps_entry = shader.ps_entry.empty() ? "ps_main" : shader.ps_entry;

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* errors = nullptr;
    std::string output;

    auto collect = [&](ID3DBlob*& blob) {
        if (blob != nullptr) {
            output.append(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
            release(blob);
        }
    };

    HRESULT result = overlay::d3d::compile(full.data(), full.size(), display.c_str(), macros.data(), &includes,
                                  vs_entry.c_str(), "vs_5_0", flags, 0, &vs, &errors);
    collect(errors);
    if (SUCCEEDED(result)) {
        result = overlay::d3d::compile(full.data(), full.size(), display.c_str(), macros.data(), &includes,
                              ps_entry.c_str(), "ps_5_0", flags, 0, &ps, &errors);
        collect(errors);
    }

    shader.watched.clear();
    if (!shader.path.empty()) {
        shader.watched.push_back(shader.path);
    }
    for (const auto& file : includes.opened) {
        shader.watched.push_back(file);
    }
    shader.stamp = newest_stamp(shader.watched);
    shader.last_compile_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();

    if (FAILED(result) || vs == nullptr || ps == nullptr) {
        release(vs);
        release(ps);
        ++shader.failures;
        shader.log = output.empty() ? "compile failed" : output;
        log::error("fx: shader {} failed:\n{}", shader.name, shader.log);
        return false;
    }

    release(shader.vs);
    release(shader.ps);
    shader.vs = vs;
    shader.ps = ps;
    shader.ready = true;
    ++shader.generation;
    shader.log = output;
    log::info("fx: shader {} compiled in {:.1f} ms{}", shader.name, shader.last_compile_ms,
              output.empty() ? "" : " with warnings");
    return true;
}

D3D12_BLEND_DESC blend_desc(BridgerFxBlend blend) {
    D3D12_BLEND_DESC desc{};
    auto& rt = desc.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    switch (blend) {
        case BRIDGER_FX_BLEND_ALPHA:
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case BRIDGER_FX_BLEND_PREMULTIPLIED:
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_ONE;
            rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case BRIDGER_FX_BLEND_ADDITIVE:
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_ONE;
            break;
        case BRIDGER_FX_BLEND_MULTIPLY:
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_DEST_COLOR;
            rt.DestBlend = D3D12_BLEND_ZERO;
            rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
            rt.DestBlendAlpha = D3D12_BLEND_ONE;
            break;
        default:
            rt.BlendEnable = FALSE;
            rt.SrcBlend = D3D12_BLEND_ONE;
            rt.DestBlend = D3D12_BLEND_ZERO;
            break;
    }
    return desc;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type(BridgerFxTopology topology) {
    switch (topology) {
        case BRIDGER_FX_TOPOLOGY_LINES: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case BRIDGER_FX_TOPOLOGY_POINTS: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

D3D12_PRIMITIVE_TOPOLOGY primitive_topology(BridgerFxTopology topology) {
    switch (topology) {
        case BRIDGER_FX_TOPOLOGY_LINES: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case BRIDGER_FX_TOPOLOGY_POINTS: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

ID3D12PipelineState* get_pipeline(const Shader& shader, const Effect& effect, DXGI_FORMAT target,
                                  DXGI_FORMAT depth_format) {
    const bool fullscreen = effect.is_pass;
    const PsoKey key{shader.handle, shader.generation, effect.blend,
                     fullscreen ? 0 : effect.depth, fullscreen ? 0 : effect.cull,
                     fullscreen ? 0 : effect.topology, fullscreen ? 0 : effect.fill,
                     static_cast<int>(target), static_cast<int>(depth_format)};
    if (const auto found = g_gpu.pipelines.find(key); found != g_gpu.pipelines.end()) {
        return found->second;
    }

    D3D12_INPUT_ELEMENT_DESC elements[4]{};
    elements[0] = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    elements[1] = {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    elements[2] = {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    elements[3] = {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 32,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_gpu.root;
    pso.VS = {shader.vs->GetBufferPointer(), shader.vs->GetBufferSize()};
    pso.PS = {shader.ps->GetBufferPointer(), shader.ps->GetBufferSize()};
    if (!fullscreen) {
        pso.InputLayout = {elements, 4};
    }
    pso.PrimitiveTopologyType = fullscreen ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
                                           : topology_type(effect.topology);
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = target;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.BlendState = blend_desc(effect.blend);

    pso.RasterizerState.FillMode = (!fullscreen && effect.fill == BRIDGER_FX_FILL_WIREFRAME)
                                     ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = fullscreen ? D3D12_CULL_MODE_NONE
                                 : effect.cull == BRIDGER_FX_CULL_BACK ? D3D12_CULL_MODE_BACK
                                 : effect.cull == BRIDGER_FX_CULL_FRONT ? D3D12_CULL_MODE_FRONT
                                                                        : D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = TRUE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.AntialiasedLineEnable = !fullscreen && effect.topology == BRIDGER_FX_TOPOLOGY_LINES;

    if (!fullscreen && depth_format != DXGI_FORMAT_UNKNOWN && effect.depth != BRIDGER_FX_DEPTH_NONE) {
        pso.DepthStencilState.DepthEnable = TRUE;
        const bool test = effect.depth == BRIDGER_FX_DEPTH_TEST || effect.depth == BRIDGER_FX_DEPTH_TEST_WRITE;
        const bool write = effect.depth == BRIDGER_FX_DEPTH_WRITE || effect.depth == BRIDGER_FX_DEPTH_TEST_WRITE;
        pso.DepthStencilState.DepthWriteMask = write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.DepthStencilState.DepthFunc = test ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DSVFormat = depth_format;
    }

    ID3D12PipelineState* pipeline = nullptr;
    const auto result = g_gpu.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline));
    if (FAILED(result)) {
        log::error("fx: pipeline for {} / {} failed ({:#x})", shader.name, effect.name,
                   static_cast<unsigned>(result));
        pipeline = nullptr;
    }
    g_gpu.pipelines[key] = pipeline;
    return pipeline;
}

void drop_pipelines_for(Handle shader) {
    for (auto it = g_gpu.pipelines.begin(); it != g_gpu.pipelines.end();) {
        if (std::get<0>(it->first) == shader) {
            retire(it->second);
            it = g_gpu.pipelines.erase(it);
        } else {
            ++it;
        }
    }
}

bool upload_image(ID3D12GraphicsCommandList* commands, GpuTexture& texture, const Image& image) {
    const auto desc = texture.resource->GetDesc();
    const UINT levels = static_cast<UINT>(std::min<std::size_t>(image.levels.size(), texture.mips));
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(levels);
    std::vector<UINT> rows(levels);
    std::vector<UINT64> row_sizes(levels);
    UINT64 total = 0;
    g_gpu.device->GetCopyableFootprints(&desc, 0, levels, 0, footprints.data(), rows.data(),
                                        row_sizes.data(), &total);

    ID3D12Resource* upload = create_upload_buffer(static_cast<std::size_t>(total));
    if (upload == nullptr) {
        return false;
    }
    D3D12_RANGE nothing{0, 0};
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, &nothing, &mapped))) {
        upload->Release();
        return false;
    }
    for (UINT level = 0; level < levels; ++level) {
        const auto& source = image.levels[level];
        const auto& footprint = footprints[level];
        auto* destination = static_cast<std::uint8_t*>(mapped) + footprint.Offset;
        const std::size_t copy_bytes = std::min<std::size_t>(source.row_pitch, row_sizes[level]);
        const UINT copy_rows = std::min<UINT>(rows[level], source.rows);
        for (UINT row = 0; row < copy_rows; ++row) {
            std::memcpy(destination + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                        image.data.data() + source.offset + static_cast<std::size_t>(row) * source.row_pitch,
                        copy_bytes);
        }
    }
    upload->Unmap(0, nullptr);

    transition(commands, texture, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT level = 0; level < levels; ++level) {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture.resource;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = level;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprints[level];
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    transition(commands, texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    retire(upload);
    return true;
}

void materialise_texture(ID3D12GraphicsCommandList* commands, Texture& texture) {
    if (texture.is_target) {
        unsigned width = texture.width;
        unsigned height = texture.height;
        if (texture.scale > 0.0f) {
            width = std::max(1u, static_cast<unsigned>(std::lround(g_gpu.width * texture.scale)));
            height = std::max(1u, static_cast<unsigned>(std::lround(g_gpu.height * texture.scale)));
        }
        const DXGI_FORMAT format = to_dxgi(texture.format);
        if (texture.color.valid() && texture.color.width == width && texture.color.height == height
                && texture.color.format == format) {
            return;
        }
        if (format == DXGI_FORMAT_UNKNOWN || format_is_compressed(format)) {
            texture.failed = true;
            texture.error = "render targets need an uncompressed colour format";
            return;
        }
        if (!create_gpu_texture(texture.color, width, height, 1, format, format, true, false,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
            texture.failed = true;
            texture.error = "could not create the render target";
            return;
        }
        if (texture.with_depth) {
            if (!create_gpu_texture(texture.depth, width, height, 1, DXGI_FORMAT_D32_FLOAT,
                                    DXGI_FORMAT_D32_FLOAT, false, true,
                                    D3D12_RESOURCE_STATE_DEPTH_WRITE)) {
                texture.failed = true;
                texture.error = "could not create the depth attachment";
                return;
            }
        }
        texture.failed = false;
        texture.error.clear();
        return;
    }

    if (!texture.has_pending) {
        return;
    }
    const Image& image = texture.pending;
    if (!texture.color.valid() || texture.color.width != image.width
            || texture.color.height != image.height || texture.color.format != image.format) {
        if (!create_gpu_texture(texture.color, image.width, image.height,
                                static_cast<unsigned>(image.levels.size()), image.format,
                                image.format, false, false, D3D12_RESOURCE_STATE_COPY_DEST)) {
            texture.failed = true;
            texture.error = "could not create the texture";
            texture.has_pending = false;
            return;
        }
    }
    if (!upload_image(commands, texture.color, image)) {
        texture.failed = true;
        texture.error = "could not upload the texture";
    } else {
        texture.failed = false;
        texture.error.clear();
    }
    texture.has_pending = false;
    texture.pending = Image{};
}

void materialise_mesh(Mesh& mesh) {
    if (mesh.uploaded) {
        return;
    }
    const std::size_t vertex_bytes = mesh.vertices.size() * kVertexStride;
    const std::size_t index_bytes = mesh.indices.size() * sizeof(std::uint32_t);
    mesh.vertex_buffer = create_upload_buffer(vertex_bytes);
    mesh.index_buffer = create_upload_buffer(index_bytes);
    if (mesh.vertex_buffer == nullptr || mesh.index_buffer == nullptr) {
        release(mesh.vertex_buffer);
        release(mesh.index_buffer);
        mesh.uploaded = true;
        return;
    }
    D3D12_RANGE nothing{0, 0};
    void* mapped = nullptr;
    if (SUCCEEDED(mesh.vertex_buffer->Map(0, &nothing, &mapped))) {
        std::memcpy(mapped, mesh.vertices.data(), vertex_bytes);
        mesh.vertex_buffer->Unmap(0, nullptr);
    }
    if (SUCCEEDED(mesh.index_buffer->Map(0, &nothing, &mapped))) {
        std::memcpy(mapped, mesh.indices.data(), index_bytes);
        mesh.index_buffer->Unmap(0, nullptr);
    }
    mesh.vertex_count = static_cast<unsigned>(mesh.vertices.size());
    mesh.index_count = static_cast<unsigned>(mesh.indices.size());
    mesh.vertices.clear();
    mesh.vertices.shrink_to_fit();
    mesh.indices.clear();
    mesh.indices.shrink_to_fit();
    mesh.uploaded = true;
}

bool create_internal_targets() {
    const unsigned w = g_gpu.width;
    const unsigned h = g_gpu.height;
    const DXGI_FORMAT format = g_gpu.format;
    bool ok = true;
    ok &= create_gpu_texture(g_gpu.scene, w, h, 1, format, format, false, false,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ok &= create_gpu_texture(g_gpu.chain[0], w, h, 1, format, format, true, false,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ok &= create_gpu_texture(g_gpu.chain[1], w, h, 1, format, format, true, false,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ok &= create_gpu_texture(g_gpu.history, w, h, 1, format, format, false, false,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ok &= create_gpu_texture(g_gpu.resolved_history, w, h, 1, format, format, true, false,
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ok &= create_gpu_texture(g_gpu.mod_depth, w, h, 1, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT,
                             false, true, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    g_gpu.history_valid = false;
    destroy_gpu_texture(g_gpu.scene_depth);
    g_gpu.scene_depth_valid = false;
    return ok;
}

bool create_fallbacks(ID3D12GraphicsCommandList* commands) {
    Image white;
    std::string error;
    const std::uint32_t white_pixel = 0xffffffffu;
    image_from_pixels(white, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, &white_pixel, 0, error);
    if (!create_gpu_texture(g_gpu.white, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                            DXGI_FORMAT_R8G8B8A8_UNORM, false, false, D3D12_RESOURCE_STATE_COPY_DEST)
            || !upload_image(commands, g_gpu.white, white)) {
        return false;
    }
    Image far_image;
    const float far_pixel = kFarDepth;
    image_from_pixels(far_image, 1, 1, DXGI_FORMAT_R32_FLOAT, &far_pixel, 0, error);
    if (!create_gpu_texture(g_gpu.far_depth, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                            false, false, D3D12_RESOURCE_STATE_COPY_DEST)
            || !upload_image(commands, g_gpu.far_depth, far_image)) {
        return false;
    }
    return true;
}

Handle add_builtin(const char* name, BridgerFxShaderKind kind, const char* source) {
    Shader shader;
    shader.handle = g_state.next_handle++;
    shader.name = name;
    shader.owner = "bridger";
    shader.kind = kind;
    shader.source = source;
    shader.builtin = true;
    const Handle handle = shader.handle;
    g_state.shaders[handle] = std::move(shader);
    return handle;
}

bool create_root_signature() {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kSrvSlots;
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER parameters[4]{};
    for (int i = 0; i < 3; ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[i].Descriptor.ShaderRegister = static_cast<UINT>(i);
        parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable.NumDescriptorRanges = 1;
    parameters[3].DescriptorTable.pDescriptorRanges = &range;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[4]{};
    for (UINT i = 0; i < 4; ++i) {
        auto& sampler = samplers[i];
        const bool linear = (i % 2) == 0;
        const bool wrap = i >= 2;
        sampler.Filter = linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = wrap ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = sampler.AddressU;
        sampler.AddressW = sampler.AddressU;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = i;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 4;
    signature.pParameters = parameters;
    signature.NumStaticSamplers = 4;
    signature.pStaticSamplers = samplers;
    signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* serialized = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(overlay::d3d::serialize_root_signature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                             &errors))) {
        if (errors != nullptr) {
            log::error("fx: root signature: {}", static_cast<const char*>(errors->GetBufferPointer()));
        }
        release(errors);
        return false;
    }
    const auto result = g_gpu.device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                          serialized->GetBufferSize(),
                                                          IID_PPV_ARGS(&g_gpu.root));
    release(serialized);
    release(errors);
    return SUCCEEDED(result);
}

struct CameraSample {
    BridgerFxCamera camera{};
    bool ok = false;
};

bool finite3(const float* v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

void sample_camera_body(void* raw) {
    auto* sample = static_cast<CameraSample*>(raw);
    const auto get_player = reinterpret_cast<void* (*)(int)>(g_image_base + kGetLocalPlayer);
    const auto get_camera = reinterpret_cast<void* (*)(void*)>(g_image_base + kGetLastActivatedCamera);
    void* player = get_player(0);
    if (player == nullptr) {
        return;
    }
    void* camera = get_camera(player);
    if (camera == nullptr) {
        return;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(camera);
    double position[3];
    float basis[9];
    std::memcpy(position, bytes + kEntityOrientation, sizeof position);
    std::memcpy(basis, bytes + kEntityOrientation + 24, sizeof basis);
    float fov = 0.0f;
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    std::memcpy(&fov, bytes + kCameraFov, sizeof fov);
    std::memcpy(&near_plane, bytes + kCameraNear, sizeof near_plane);
    std::memcpy(&far_plane, bytes + kCameraFar, sizeof far_plane);

    if (!finite3(basis) || !finite3(basis + 3) || !finite3(basis + 6) || !std::isfinite(fov)
            || !std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2])) {
        return;
    }
    using math::Vec3;
    const Vec3 right{basis[0], basis[1], basis[2]};
    const Vec3 forward{basis[3], basis[4], basis[5]};
    const Vec3 up{basis[6], basis[7], basis[8]};
    if (std::fabs(math::length(right) - 1.0f) > 0.05f || std::fabs(math::length(forward) - 1.0f) > 0.05f
            || std::fabs(math::length(up) - 1.0f) > 0.05f || fov < 1.0f || fov > 179.0f) {
        return;
    }
    auto& out = sample->camera;
    std::memcpy(out.position, position, sizeof position);
    std::memcpy(out.right, &right, sizeof out.right);
    std::memcpy(out.forward, &forward, sizeof out.forward);
    std::memcpy(out.up, &up, sizeof out.up);
    out.fov_degrees = fov;
    out.near_plane = std::isfinite(near_plane) && near_plane > 0.0f ? near_plane : 0.1f;
    out.far_plane = std::isfinite(far_plane) && far_plane > 0.0f ? far_plane : 0.0f;
    out.valid = true;
    sample->ok = true;
}

void store(float* out, const math::Mat4& m) {
    std::memcpy(out, m.m, sizeof m.m);
}

void build_frame(float dt, unsigned width, unsigned height) {
    auto& frame = g_state.frame;
    BridgerFxCamera camera{};
    {
        std::scoped_lock lock(g_state.camera_mutex);
        camera = g_state.has_override ? g_state.override_camera : g_state.engine_camera;
        g_state.camera_source = g_state.has_override ? "mod override"
                              : g_state.engine_camera.valid ? "engine camera entity" : "none";
    }
    frame.width = width;
    frame.height = height;
    frame.frame = g_gpu.presented;
    frame.time = static_cast<float>(g_gpu.elapsed);
    frame.delta = dt;
    frame.camera = camera;
    frame.depth_available = g_gpu.scene_depth_valid;
    frame.device_ready = g_gpu.ready;

    using math::Vec3;
    Vec3 right{camera.right[0], camera.right[1], camera.right[2]};
    Vec3 forward{camera.forward[0], camera.forward[1], camera.forward[2]};
    Vec3 up{camera.up[0], camera.up[1], camera.up[2]};
    double position[3] = {camera.position[0], camera.position[1], camera.position[2]};
    float fov = camera.fov_degrees;
    float near_plane = camera.near_plane;
    if (!camera.valid) {
        right = {1.0f, 0.0f, 0.0f};
        forward = {0.0f, 1.0f, 0.0f};
        up = {0.0f, 0.0f, 1.0f};
        position[0] = position[1] = position[2] = 0.0;
        fov = 70.0f;
        near_plane = 0.1f;
    }
    if (!(near_plane > 0.0f)) {
        near_plane = 0.1f;
    }
    const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    float tan_half_v = 0.0f;
    float tan_half_h = 0.0f;
    if (g_state.settings.fov_horizontal) {
        tan_half_h = std::tan(math::radians(fov) * 0.5f);
        tan_half_v = tan_half_h / aspect;
    } else {
        tan_half_v = std::tan(math::radians(fov) * 0.5f);
        tan_half_h = tan_half_v * aspect;
    }
    tan_half_v = std::max(tan_half_v, 1e-4f);
    tan_half_h = std::max(tan_half_h, 1e-4f);

    const math::Mat4 view = math::view_matrix(right, forward, up, position);
    const math::Mat4 proj = math::projection_matrix(tan_half_v, aspect, near_plane);
    const math::Mat4 view_proj = math::multiply(proj, view);
    store(frame.view, view);
    store(frame.projection, proj);
    store(frame.view_projection, view_proj);
    frame.camera.near_plane = near_plane;
    frame.camera.fov_degrees = fov;
    g_state.stats.width = width;
    g_state.stats.height = height;
    (void)tan_half_h;
}

void write_frame_constants(FrameSlot& slot, D3D12_GPU_VIRTUAL_ADDRESS& address) {
    const auto& frame = g_state.frame;
    FrameConstants constants{};
    std::memcpy(constants.view, frame.view, sizeof constants.view);
    std::memcpy(constants.proj, frame.projection, sizeof constants.proj);
    std::memcpy(constants.view_proj, frame.view_projection, sizeof constants.view_proj);
    math::Mat4 view_proj;
    std::memcpy(view_proj.m, frame.view_projection, sizeof view_proj.m);
    math::Mat4 proj;
    std::memcpy(proj.m, frame.projection, sizeof proj.m);
    store(constants.inv_view_proj, math::inverse(view_proj));
    store(constants.inv_proj, math::inverse(proj));

    const auto& camera = frame.camera;
    constants.camera_position[0] = static_cast<float>(camera.position[0]);
    constants.camera_position[1] = static_cast<float>(camera.position[1]);
    constants.camera_position[2] = static_cast<float>(camera.position[2]);
    constants.camera_position[3] = camera.valid ? 1.0f : 0.0f;
    std::memcpy(constants.camera_forward, camera.forward, sizeof camera.forward);
    std::memcpy(constants.camera_up, camera.up, sizeof camera.up);
    std::memcpy(constants.camera_right, camera.right, sizeof camera.right);
    if (!camera.valid) {
        constants.camera_forward[1] = 1.0f;
        constants.camera_up[2] = 1.0f;
        constants.camera_right[0] = 1.0f;
    }
    constants.resolution[0] = static_cast<float>(frame.width);
    constants.resolution[1] = static_cast<float>(frame.height);
    constants.resolution[2] = frame.width > 0 ? 1.0f / static_cast<float>(frame.width) : 0.0f;
    constants.resolution[3] = frame.height > 0 ? 1.0f / static_cast<float>(frame.height) : 0.0f;
    constants.time[0] = frame.time;
    constants.time[1] = frame.delta;
    constants.time[2] = static_cast<float>(frame.frame);
    constants.time[3] = 0.0f;
    constants.depth[0] = camera.near_plane;
    constants.depth[1] = camera.far_plane;
    constants.depth[2] = g_state.settings.depth_reversed ? 1.0f : 0.0f;
    constants.depth[3] = g_gpu.scene_depth_valid ? 1.0f : 0.0f;

    const float tan_v = proj.m[5] != 0.0f ? 1.0f / proj.m[5] : 1.0f;
    const float tan_h = proj.m[0] != 0.0f ? 1.0f / proj.m[0] : 1.0f;
    constants.fov[0] = 2.0f * std::atan(tan_v);
    constants.fov[1] = 2.0f * std::atan(tan_h);
    constants.fov[2] = tan_v;
    constants.fov[3] = tan_h;
    constants.motion[0] = g_gpu.motion_scale[0];
    constants.motion[1] = g_gpu.motion_scale[1];
    constants.motion[2] = g_gpu.motion_valid ? 1.0f : 0.0f;
    constants.motion[3] = g_gpu.motion_reset ? 1.0f : 0.0f;

    address = ring_push(slot.constants, &constants, sizeof constants, kConstantAlignment);
}

struct Target {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    bool has_dsv = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    unsigned width = 0;
    unsigned height = 0;
    GpuTexture* texture = nullptr;
};

struct Bindings {
    unsigned srv[kSrvSlots]{};
};

D3D12_GPU_DESCRIPTOR_HANDLE bind_srvs(FrameSlot& slot, unsigned frame_index, const Bindings& bindings) {
    const unsigned base = frame_index * kFrameDescriptors + slot.descriptors_used;
    slot.descriptors_used += kSrvSlots;
    D3D12_CPU_DESCRIPTOR_HANDLE sources[kSrvSlots];
    for (unsigned i = 0; i < kSrvSlots; ++i) {
        sources[i] = staging_handle(bindings.srv[i]);
    }
    auto destination = g_gpu.gpu_heap->GetCPUDescriptorHandleForHeapStart();
    destination.ptr += static_cast<SIZE_T>(base) * g_gpu.srv_stride;
    const UINT count = kSrvSlots;
    g_gpu.device->CopyDescriptors(1, &destination, &count, kSrvSlots, sources, nullptr,
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu = g_gpu.gpu_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(base) * g_gpu.srv_stride;
    return gpu;
}

const Shader* shader_for(const Effect& effect) {
    Handle handle = effect.shader;
    if (handle == 0) {
        handle = effect.is_pass ? g_gpu.builtin_fullscreen
               : effect.space == BRIDGER_FX_SPACE_SCREEN ? g_gpu.builtin_screen : g_gpu.builtin_world;
    }
    const auto found = g_state.shaders.find(handle);
    return found == g_state.shaders.end() ? nullptr : &found->second;
}

void set_common_state(ID3D12GraphicsCommandList* commands, const Target& target,
                      D3D12_GPU_VIRTUAL_ADDRESS frame_constants, D3D12_GPU_VIRTUAL_ADDRESS effect_constants,
                      D3D12_GPU_VIRTUAL_ADDRESS object_constants, D3D12_GPU_DESCRIPTOR_HANDLE srvs,
                      bool use_depth) {
    commands->OMSetRenderTargets(1, &target.rtv, FALSE, use_depth ? &target.dsv : nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(target.width),
                                  static_cast<float>(target.height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(target.width), static_cast<LONG>(target.height)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->SetGraphicsRootConstantBufferView(0, frame_constants);
    commands->SetGraphicsRootConstantBufferView(1, effect_constants);
    commands->SetGraphicsRootConstantBufferView(2, object_constants);
    commands->SetGraphicsRootDescriptorTable(3, srvs);
}

D3D12_GPU_VIRTUAL_ADDRESS push_object(FrameSlot& slot, const float* model, const float* tint) {
    ObjectConstants object{};
    if (model != nullptr) {
        std::memcpy(object.model, model, sizeof object.model);
    } else {
        store(object.model, math::Mat4::identity());
    }
    if (tint != nullptr) {
        std::memcpy(object.tint, tint, sizeof object.tint);
    } else {
        object.tint[0] = object.tint[1] = object.tint[2] = object.tint[3] = 1.0f;
    }
    return ring_push(slot.constants, &object, sizeof object, kConstantAlignment);
}

D3D12_GPU_VIRTUAL_ADDRESS push_effect_constants(FrameSlot& slot, const Effect& effect) {
    if (effect.constants.empty()) {
        static const std::uint8_t zeros[256]{};
        return ring_push(slot.constants, zeros, sizeof zeros, kConstantAlignment);
    }
    return ring_push(slot.constants, effect.constants.data(), effect.constants.size(),
                     kConstantAlignment);
}

void execute_pass(ID3D12GraphicsCommandList* commands, FrameSlot& slot, unsigned frame_index,
                  Effect& effect, const Shader& shader, const Target& target,
                  D3D12_GPU_VIRTUAL_ADDRESS frame_constants, const Bindings& bindings) {
    ID3D12PipelineState* pipeline = get_pipeline(shader, effect, target.format, DXGI_FORMAT_UNKNOWN);
    if (pipeline == nullptr) {
        effect.problem = "pipeline creation failed";
        return;
    }
    const auto effect_constants = push_effect_constants(slot, effect);
    const auto object_constants = push_object(slot, nullptr, nullptr);
    if (effect_constants == 0 || object_constants == 0) {
        effect.problem = "constant ring exhausted";
        return;
    }
    commands->SetPipelineState(pipeline);
    set_common_state(commands, target, frame_constants, effect_constants, object_constants,
                     bind_srvs(slot, frame_index, bindings), false);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
    ++effect.draw_calls;
    effect.vertex_total += 3;
    effect.ran = true;
}

bool resolve_history(ID3D12GraphicsCommandList* commands, FrameSlot& slot, unsigned frame_index,
                     D3D12_GPU_VIRTUAL_ADDRESS constants, GpuTexture& history,
                     GpuTexture& current, GpuTexture& output) {
    auto found = g_state.shaders.find(g_gpu.builtin_history);
    if (found == g_state.shaders.end() || !found->second.ready || !output.valid()) return false;
    transition(commands, history, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(commands, output, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Bindings bindings;
    for (auto& srv : bindings.srv) srv = g_gpu.white.srv;
    bindings.srv[0] = current.srv;
    bindings.srv[1] = g_gpu.far_depth.srv;
    bindings.srv[2] = current.srv;
    bindings.srv[3] = history.srv;
    bindings.srv[8] = g_gpu.motion_valid ? g_gpu.scene_motion.srv : g_gpu.far_depth.srv;
    Target target;
    target.rtv = rtv_handle(output.rtv); target.format = output.view_format;
    target.width = output.width; target.height = output.height; target.texture = &output;
    Effect effect; effect.is_pass = true;
    execute_pass(commands, slot, frame_index, effect, found->second, target, constants, bindings);
    transition(commands, output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return effect.ran;
}

void execute_draw_list(ID3D12GraphicsCommandList* commands, FrameSlot& slot, unsigned frame_index,
                       Effect& effect, const Shader& shader, const Target& target,
                       D3D12_GPU_VIRTUAL_ADDRESS frame_constants, const Bindings& bindings) {
    if (effect.vertices.empty() && effect.mesh_draws.empty()) {
        return;
    }
    if (effect.space == BRIDGER_FX_SPACE_WORLD && !g_state.frame.camera.valid) {
        effect.problem = "no camera this frame";
        return;
    }
    const bool use_depth = target.has_dsv && effect.depth != BRIDGER_FX_DEPTH_NONE;
    ID3D12PipelineState* pipeline = get_pipeline(shader, effect, target.format,
                                                 use_depth ? target.depth_format : DXGI_FORMAT_UNKNOWN);
    if (pipeline == nullptr) {
        effect.problem = "pipeline creation failed";
        return;
    }
    const auto effect_constants = push_effect_constants(slot, effect);
    if (effect_constants == 0) {
        effect.problem = "constant ring exhausted";
        return;
    }
    const auto srvs = bind_srvs(slot, frame_index, bindings);
    commands->SetPipelineState(pipeline);
    commands->IASetPrimitiveTopology(primitive_topology(effect.topology));

    if (!effect.vertices.empty()) {
        const std::size_t vertex_bytes = effect.vertices.size() * kVertexStride;
        const std::size_t index_bytes = effect.indices.size() * sizeof(std::uint32_t);
        const auto vertex_address = ring_push(slot.vertices, effect.vertices.data(), vertex_bytes, 16);
        const auto index_address = ring_push(slot.indices, effect.indices.data(), index_bytes, 4);
        const auto object_constants = push_object(slot, nullptr, nullptr);
        if (vertex_address == 0 || index_address == 0 || object_constants == 0) {
            effect.problem = "geometry ring exhausted";
            return;
        }
        set_common_state(commands, target, frame_constants, effect_constants, object_constants, srvs,
                         use_depth);
        D3D12_VERTEX_BUFFER_VIEW vertex_view{vertex_address, static_cast<UINT>(vertex_bytes),
                                            static_cast<UINT>(kVertexStride)};
        D3D12_INDEX_BUFFER_VIEW index_view{index_address, static_cast<UINT>(index_bytes),
                                          DXGI_FORMAT_R32_UINT};
        commands->IASetVertexBuffers(0, 1, &vertex_view);
        commands->IASetIndexBuffer(&index_view);
        commands->DrawIndexedInstanced(static_cast<UINT>(effect.indices.size()), 1, 0, 0, 0);
        ++effect.draw_calls;
        effect.vertex_total += static_cast<unsigned>(effect.vertices.size());
    }

    for (const auto& draw : effect.mesh_draws) {
        const auto found = g_state.meshes.find(draw.mesh);
        if (found == g_state.meshes.end()) {
            effect.problem = "mesh handle is not valid";
            continue;
        }
        Mesh& mesh = found->second;
        materialise_mesh(mesh);
        if (mesh.vertex_buffer == nullptr || mesh.index_count == 0) {
            continue;
        }
        const auto object_constants = push_object(slot, draw.model, draw.tint);
        if (object_constants == 0) {
            effect.problem = "constant ring exhausted";
            break;
        }
        set_common_state(commands, target, frame_constants, effect_constants, object_constants, srvs,
                         use_depth);
        D3D12_VERTEX_BUFFER_VIEW vertex_view{mesh.vertex_buffer->GetGPUVirtualAddress(),
                                            static_cast<UINT>(mesh.vertex_count * kVertexStride),
                                            static_cast<UINT>(kVertexStride)};
        D3D12_INDEX_BUFFER_VIEW index_view{mesh.index_buffer->GetGPUVirtualAddress(),
                                          static_cast<UINT>(mesh.index_count * sizeof(std::uint32_t)),
                                          DXGI_FORMAT_R32_UINT};
        commands->IASetVertexBuffers(0, 1, &vertex_view);
        commands->IASetIndexBuffer(&index_view);
        commands->DrawIndexedInstanced(mesh.index_count, 1, 0, 0, 0);
        ++effect.draw_calls;
        effect.vertex_total += mesh.vertex_count;
    }
    effect.ran = true;
}

void read_timings(FrameSlot& slot) {
    if (!slot.timed_valid || slot.readback == nullptr || slot.queries_used == 0
            || g_gpu.timestamp_frequency == 0) {
        slot.timed_valid = false;
        return;
    }
    const D3D12_RANGE range{0, slot.queries_used * sizeof(std::uint64_t)};
    void* mapped = nullptr;
    if (SUCCEEDED(slot.readback->Map(0, &range, &mapped))) {
        const auto* stamps = static_cast<const std::uint64_t*>(mapped);
        float total = 0.0f;
        for (std::size_t i = 0; i < slot.timed.size(); ++i) {
            const auto begin = stamps[i * 2];
            const auto end = stamps[i * 2 + 1];
            const float ms = end > begin
                ? static_cast<float>(static_cast<double>(end - begin) * 1000.0
                                     / static_cast<double>(g_gpu.timestamp_frequency))
                : 0.0f;
            total += ms;
            if (const auto found = g_state.effects.find(slot.timed[i]); found != g_state.effects.end()) {
                found->second.gpu_ms = ms;
            }
        }
        g_state.stats.gpu_ms = total;
        const D3D12_RANGE nothing{0, 0};
        slot.readback->Unmap(0, &nothing);
    }
    slot.timed_valid = false;
}

void capture_scene_depth(ID3D12GraphicsCommandList* commands) {
    g_gpu.scene_depth_valid = false;
    g_state.stats.depth_captured = false;
    if (!g_state.settings.depth_capture) {
        g_state.stats.depth_note = "off";
        return;
    }
    depth::Candidate candidate;
    bool found = false;
    const auto upscaler = ngx::snapshot();
    if (upscaler.seen && upscaler.frames_since < 10 && upscaler.depth != nullptr) {
        auto* resource =
            const_cast<ID3D12Resource*>(static_cast<const ID3D12Resource*>(upscaler.depth));
        const auto desc = resource->GetDesc();
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1) {
            candidate.resource = resource;
            candidate.width = static_cast<unsigned>(desc.Width);
            candidate.height = desc.Height;
            candidate.format = desc.Format;
            depth::watch(resource);
            found = true;
        }
    }
    if (!found) {
        found = depth::select(g_state.settings.depth_candidate, g_gpu.width, g_gpu.height, candidate);
    }
    if (!found) {
        g_state.stats.depth_note = "no depth buffer seen this frame";
        return;
    }
    DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
    if (!depth_copy_formats(candidate.format, resource_format, view_format)) {
        g_state.stats.depth_note = "unsupported depth format";
        return;
    }
    if (!g_gpu.scene_depth.valid() || g_gpu.scene_depth.width != candidate.width
            || g_gpu.scene_depth.height != candidate.height
            || g_gpu.scene_depth.format != resource_format) {
        if (!create_gpu_texture(g_gpu.scene_depth, candidate.width, candidate.height, 1,
                                resource_format, view_format, false, false,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
            g_state.stats.depth_note = "could not create the depth copy";
            return;
        }
    }
    D3D12_RESOURCE_STATES state = depth::known_state(candidate.resource, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_RESOURCE_STATES original = state;
    copy_into(commands, g_gpu.scene_depth, candidate.resource, state);
    transition(commands, candidate.resource, state, original);
    g_gpu.scene_depth_valid = true;
    g_state.stats.depth_captured = true;
    g_state.stats.depth_note = std::to_string(candidate.width) + "x"
                             + std::to_string(candidate.height)
                             + (upscaler.depth == candidate.resource ? " from the upscaler"
                                                                     : " by guess");
}

bool resolve_target(Effect& effect, Target& target, const FrameContext& context,
                    D3D12_RESOURCE_STATES& backbuffer_state, GpuTexture* chain_target,
                    std::vector<Handle>& touched) {
    if (effect.output == 0) {
        if (chain_target != nullptr) {
            transition(context.commands, *chain_target, D3D12_RESOURCE_STATE_RENDER_TARGET);
            target.rtv = rtv_handle(chain_target->rtv);
            target.format = chain_target->format;
            target.width = chain_target->width;
            target.height = chain_target->height;
            target.texture = chain_target;
        } else {
            transition(context.commands, context.backbuffer, backbuffer_state,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            target.rtv = context.backbuffer_rtv;
            target.format = context.format;
            target.width = context.width;
            target.height = context.height;
            target.texture = nullptr;
        }
        if (!effect.is_pass && g_gpu.mod_depth.valid()) {
            target.dsv = dsv_handle(g_gpu.mod_depth.dsv);
            target.has_dsv = true;
            target.depth_format = g_gpu.mod_depth.view_format;
        }
        return true;
    }

    const auto found = g_state.textures.find(effect.output);
    if (found == g_state.textures.end() || !found->second.is_target) {
        effect.problem = "output is not a render target";
        return false;
    }
    Texture& texture = found->second;
    materialise_texture(context.commands, texture);
    if (!texture.color.valid()) {
        effect.problem = "render target not available";
        return false;
    }
    const bool first_touch = std::find(touched.begin(), touched.end(), texture.handle) == touched.end();
    if (first_touch) {
        touched.push_back(texture.handle);
    }
    transition(context.commands, texture.color, D3D12_RESOURCE_STATE_RENDER_TARGET);
    target.rtv = rtv_handle(texture.color.rtv);
    target.format = texture.color.format;
    target.width = texture.color.width;
    target.height = texture.color.height;
    target.texture = &texture.color;
    if ((effect.is_pass && effect.clear_output) || (!effect.is_pass && first_touch)) {
        const float transparent[4]{};
        context.commands->ClearRenderTargetView(target.rtv,
                                                effect.is_pass ? effect.clear_color : transparent,
                                                0, nullptr);
    }
    if (!effect.is_pass && texture.depth.valid()) {
        target.dsv = dsv_handle(texture.depth.dsv);
        target.has_dsv = true;
        target.depth_format = texture.depth.view_format;
        if (first_touch) {
            context.commands->ClearDepthStencilView(target.dsv, D3D12_CLEAR_FLAG_DEPTH, kFarDepth, 0,
                                                    0, nullptr);
        }
    }
    return true;
}

unsigned srv_of(Handle resource) {
    if (resource == 0) {
        return g_gpu.white.srv;
    }
    const auto found = g_state.textures.find(resource);
    if (found == g_state.textures.end() || !found->second.color.valid()) {
        return g_gpu.white.srv;
    }
    return found->second.color.srv;
}

void prepare_user_textures(ID3D12GraphicsCommandList* commands, const Effect& effect) {
    for (const Handle handle : effect.textures) {
        if (handle == 0) {
            continue;
        }
        const auto found = g_state.textures.find(handle);
        if (found == g_state.textures.end()) {
            continue;
        }
        Texture& texture = found->second;
        materialise_texture(commands, texture);
        if (texture.color.valid()) {
            transition(commands, texture.color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }
}

}

State& state() {
    return g_state;
}

void configure(const std::filesystem::path& root) {
    g_root = root;
    g_image_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    settings::configure(root);
    settings::load("bridger");
    auto& s = g_state.settings;
    s.fov_horizontal = settings::get_bool("bridger", "fx.fov_horizontal", true);
    s.depth_capture = settings::get_bool("bridger", "fx.depth_capture", false);
    s.depth_reversed = settings::get_bool("bridger", "fx.depth_reversed", true);
    s.depth_candidate = static_cast<int>(settings::get_number("bridger", "fx.depth_candidate", -1));
    s.hot_reload = settings::get_bool("bridger", "fx.hot_reload", true);
    s.timings = settings::get_bool("bridger", "fx.timings", true);
    s.pre_upscale = settings::get_bool("bridger", "fx.pre_upscale", false);
    s.dlss_preset = static_cast<int>(settings::get_number("bridger", "fx.dlss_preset", 11));
    s.pipeline_hooks = settings::get_bool("bridger", "fx.pipeline_hooks", true);
    s.pipeline_retain = settings::get_bool("bridger", "fx.pipeline_retain", true);
    pipeline::set_bindings(settings::get_bool("bridger", "fx.material_bindings", false));
    pipeline::set_bindings_early(settings::get_bool("bridger", "fx.material_bindings_early", true));
    rt::configure(root);
    if (rt::boot_enabled()) {
        pipeline::set_bindings(true);
        pipeline::set_stream_output(true);
        pipeline::set_geometry_capture(rt::settings().enabled);
    }
    pipeline::set_enabled(s.pipeline_hooks);
    pipeline::set_retain(s.pipeline_retain);
    pipeline::configure(root);
    ngx::set_preset(static_cast<ngx::Preset>(s.dlss_preset));
    const auto layout = settings::get_text("bridger", "fx.ngx_layout", "");
    ngx::set_known_layout(layout.c_str());
    ngx::on_layout_detected([](const char* name) {
        settings::set_text("bridger", "fx.ngx_layout", name);
    });
    readback::configure(root / "dumps");
    g_configured = true;
}

void save_settings() {
    const auto& s = g_state.settings;
    settings::set_bool("bridger", "fx.fov_horizontal", s.fov_horizontal);
    settings::set_bool("bridger", "fx.depth_capture", s.depth_capture);
    settings::set_bool("bridger", "fx.depth_reversed", s.depth_reversed);
    settings::set_number("bridger", "fx.depth_candidate", s.depth_candidate);
    settings::set_bool("bridger", "fx.hot_reload", s.hot_reload);
    settings::set_bool("bridger", "fx.timings", s.timings);
    settings::set_bool("bridger", "fx.pre_upscale", s.pre_upscale);
    settings::set_number("bridger", "fx.dlss_preset", s.dlss_preset);
    settings::set_bool("bridger", "fx.pipeline_hooks", s.pipeline_hooks);
    settings::set_bool("bridger", "fx.pipeline_retain", s.pipeline_retain);
    pipeline::set_enabled(s.pipeline_hooks);
    pipeline::set_retain(s.pipeline_retain);
    rt::save_settings();
    if (rt::boot_enabled()) pipeline::set_geometry_capture(rt::settings().enabled);
    ngx::set_preset(static_cast<ngx::Preset>(s.dlss_preset));
    depth::set_tracking(s.depth_capture);
}

void prepare_hooks(void** device_vtable, void** command_list_vtable) {
    depth::prepare(device_vtable, command_list_vtable);
    pipeline::prepare(device_vtable, command_list_vtable);
}

bool initialise(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format, unsigned width,
                unsigned height, unsigned frame_count) {
    std::scoped_lock lock(g_state.mutex);
    if (g_gpu.ready) {
        shutdown();
    }
    if (device == nullptr || queue == nullptr) {
        return false;
    }
    g_gpu.device = device;
    device->AddRef();
    g_gpu.queue = queue;
    queue->AddRef();
    rt::attach(device);
    g_gpu.format = format;
    g_gpu.width = width;
    g_gpu.height = height;
    g_gpu.frame_count = std::max(2u, frame_count);
    g_gpu.started = std::chrono::steady_clock::now();
    g_gpu.elapsed = 0.0;

    g_gpu.srv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_gpu.rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_gpu.dsv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kStagingDescriptors;
    bool ok = SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_gpu.staging)));
    heap.NumDescriptors = kFrameDescriptors * g_gpu.frame_count;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ok &= SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_gpu.gpu_heap)));
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = kRtvDescriptors;
    ok &= SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_gpu.rtv_heap)));
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap.NumDescriptors = kDsvDescriptors;
    ok &= SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_gpu.dsv_heap)));
    if (!ok) {
        log::error("fx: descriptor heaps could not be created");
        shutdown();
        return false;
    }

    D3D12_QUERY_HEAP_DESC query_heap{};
    query_heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_heap.Count = kTimestampsPerFrame * g_gpu.frame_count;
    if (FAILED(device->CreateQueryHeap(&query_heap, IID_PPV_ARGS(&g_gpu.queries)))) {
        g_gpu.queries = nullptr;
    }
    if (FAILED(queue->GetTimestampFrequency(&g_gpu.timestamp_frequency))) {
        g_gpu.timestamp_frequency = 0;
    }

    if (!create_root_signature()) {
        log::error("fx: root signature could not be created");
        shutdown();
        return false;
    }

    g_gpu.frames.resize(g_gpu.frame_count);
    for (auto& slot : g_gpu.frames) {
        ensure_ring(slot.constants, kConstantRingBytes);
        D3D12_HEAP_PROPERTIES readback{};
        readback.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = kTimestampsPerFrame * sizeof(std::uint64_t);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&slot.readback)))) {
            slot.readback = nullptr;
        }
    }

    if (!create_internal_targets()) {
        log::error("fx: internal render targets could not be created");
        shutdown();
        return false;
    }

    if (g_gpu.builtin_world == 0) {
        g_gpu.builtin_world = add_builtin("builtin.world", BRIDGER_FX_SHADER_WORLD, builtin_world_source());
        g_gpu.builtin_screen = add_builtin("builtin.screen", BRIDGER_FX_SHADER_SCREEN, builtin_screen_source());
        g_gpu.builtin_fullscreen = add_builtin("builtin.copy", BRIDGER_FX_SHADER_FULLSCREEN,
                                               builtin_fullscreen_source());
    }
    if (g_gpu.builtin_history == 0) g_gpu.builtin_history = add_builtin("builtin.history",
        BRIDGER_FX_SHADER_FULLSCREEN,
        "float4 ps_main(BridgerScreenPixel i):SV_TARGET { float2 uv=bridger_history_uv(i.uv); "
        "if(bridger_motion.w>0.5 || any(uv<0) || any(uv>1)) return bridger_input.SampleLevel(bridger_linear,i.uv,0); "
        "return bridger_history.SampleLevel(bridger_linear,uv,0); }");
    for (auto& [handle, shader] : g_state.shaders) {
        (void)handle;
        shader.dirty = true;
    }
    for (auto& [handle, texture] : g_state.textures) {
        (void)handle;
        texture.color = GpuTexture{};
        texture.depth = GpuTexture{};
    }
    for (auto& [handle, mesh] : g_state.meshes) {
        (void)handle;
        mesh.uploaded = false;
    }

    depth::set_tracking(g_state.settings.depth_capture);
    ngx::initialise();
    g_gpu.ready = true;
    log::info("fx: shader system ready ({}x{}, {} frames in flight, timestamps {})", width, height,
              g_gpu.frame_count, g_gpu.timestamp_frequency != 0 ? "on" : "off");
    return true;
}

void shutdown() {
    std::scoped_lock lock(g_state.mutex);
    g_dump_backbuffer = 0;
    detach_upscale_jobs();
    g_gpu.ready = false;
    for (auto& [key, pipeline] : g_gpu.pipelines) {
        (void)key;
        release(pipeline);
    }
    g_gpu.pipelines.clear();
    for (auto& [handle, texture] : g_state.textures) {
        (void)handle;
        release(texture.color.resource);
        release(texture.depth.resource);
        texture.color = GpuTexture{};
        texture.depth = GpuTexture{};
    }
    for (auto& [handle, mesh] : g_state.meshes) {
        (void)handle;
        release(mesh.vertex_buffer);
        release(mesh.index_buffer);
        mesh.uploaded = false;
    }
    for (GpuTexture* texture : {&g_gpu.scene, &g_gpu.chain[0], &g_gpu.chain[1], &g_gpu.history,
                                &g_gpu.mod_depth, &g_gpu.scene_depth, &g_gpu.white, &g_gpu.far_depth,
                                &g_gpu.scene_motion, &g_gpu.pre_history, &g_gpu.resolved_history}) {
        release(texture->resource);
        *texture = GpuTexture{};
    }
    for (auto& slot : g_gpu.frames) {
        for (RingBuffer* ring : {&slot.constants, &slot.vertices, &slot.indices}) {
            if (ring->resource != nullptr) {
                ring->resource->Unmap(0, nullptr);
                release(ring->resource);
            }
            *ring = RingBuffer{};
        }
        release(slot.readback);
    }
    g_gpu.frames.clear();
    release_retired(true);
    release(g_gpu.queries);
    release(g_gpu.root);
    release(g_gpu.staging);
    release(g_gpu.gpu_heap);
    release(g_gpu.rtv_heap);
    release(g_gpu.dsv_heap);
    release(g_gpu.queue);
    release(g_gpu.device);
    g_gpu.staging_free.clear();
    g_gpu.staging_next = 0;
    g_gpu.rtv_free.clear();
    g_gpu.rtv_next = 0;
    g_gpu.dsv_free.clear();
    g_gpu.dsv_next = 0;
    g_gpu.history_valid = false;
    g_gpu.scene_depth_valid = false;
    g_gpu.motion_valid = false;
    g_gpu.pre_history_valid = false;
}

bool device_ready() {
    return g_gpu.ready;
}

void sample_camera() {
    if (g_image_base == 0 || g_state.camera_faults >= kMaxCameraFaults) {
        return;
    }
    CameraSample sample;
    const auto fault = guarded_call(sample_camera_body, &sample);
    std::scoped_lock lock(g_state.camera_mutex);
    if (fault != 0) {
        ++g_state.camera_faults;
        log::warn("fx: camera sample faulted ({:#x}), {} of {} strikes", fault, g_state.camera_faults,
                  kMaxCameraFaults);
        g_state.engine_camera.valid = false;
        return;
    }
    ++g_state.camera_samples;
    if (sample.ok) {
        g_state.engine_camera = sample.camera;
    } else {
        g_state.engine_camera.valid = false;
    }
}

void begin_frame(float delta_seconds, unsigned width, unsigned height) {
    std::scoped_lock lock(g_state.mutex);
    reap_upscale_jobs();
    g_gpu.elapsed += delta_seconds;
    ngx::end_frame();
    pipeline::end_frame(g_gpu.presented);
    if (!g_state.settings.pre_upscale || ngx::snapshot().frames_since > 1) {
        g_gpu.motion_valid = false;
        g_gpu.motion_reset = !g_gpu.history_valid;
    }
    readback::poll();
    if (!ngx::hooked() && (g_gpu.presented % 120) == 0) {
        ngx::initialise();
    }
    for (auto& [handle, effect] : g_state.effects) {
        (void)handle;
        effect.vertices.clear();
        effect.indices.clear();
        effect.mesh_draws.clear();
        effect.draw_calls = 0;
        effect.vertex_total = 0;
        effect.ran = false;
        effect.problem.clear();
    }
    build_frame(delta_seconds, width > 0 ? width : g_gpu.width, height > 0 ? height : g_gpu.height);

    if (g_state.settings.hot_reload && (g_gpu.presented % kHotReloadInterval) == 0) {
        for (auto& [handle, shader] : g_state.shaders) {
            (void)handle;
            if (shader.watched.empty() || shader.dirty) {
                continue;
            }
            if (newest_stamp(shader.watched) > shader.stamp) {
                shader.dirty = true;
                log::info("fx: shader {} changed on disk, recompiling", shader.name);
            }
        }
    }
}

bool has_work() {
    std::scoped_lock lock(g_state.mutex);
    if (!g_gpu.ready) {
        return false;
    }
    bool pending = g_state.settings.depth_capture || g_dump_backbuffer;
    for (const auto& [handle, shader] : g_state.shaders) {
        (void)handle;
        pending |= shader.dirty;
    }
    for (const auto& [handle, texture] : g_state.textures) {
        (void)handle;
        pending |= texture.has_pending;
    }
    if (g_state.settings.bypass) {
        return pending;
    }
    for (const auto& [handle, effect] : g_state.effects) {
        (void)handle;
        if (!effect.enabled) {
            continue;
        }
        if (effect.is_pass || !effect.vertices.empty() || !effect.mesh_draws.empty()) {
            return true;
        }
    }
    return pending;
}

void render(const FrameContext& context) {
    std::scoped_lock lock(g_state.mutex);
    if (!g_gpu.ready || context.commands == nullptr || context.backbuffer == nullptr) {
        return;
    }
    const auto cpu_started = std::chrono::steady_clock::now();
    auto& stats = g_state.stats;
    stats.effects_run = 0;
    stats.draw_calls = 0;
    stats.vertices = 0;
    stats.copies = 0;
    stats.frame = g_gpu.presented;
    ++g_gpu.frame_counter;
    ++g_gpu.presented;
    release_retired(false);

    if (context.width != g_gpu.width || context.height != g_gpu.height || context.format != g_gpu.format) {
        log::info("fx: target changed to {}x{} format {}", context.width, context.height,
                  static_cast<int>(context.format));
        g_gpu.width = context.width;
        g_gpu.height = context.height;
        g_gpu.format = context.format;
        create_internal_targets();
        for (auto& [key, pipeline] : g_gpu.pipelines) {
            (void)key;
            retire(pipeline);
        }
        g_gpu.pipelines.clear();
        depth::reset();
        build_frame(g_state.frame.delta, context.width, context.height);
    }
    if (context.frame_index >= g_gpu.frames.size()) {
        return;
    }
    ID3D12GraphicsCommandList* commands = context.commands;
    FrameSlot& slot = g_gpu.frames[context.frame_index];
    read_timings(slot);
    slot.constants.used = 0;
    slot.vertices.used = 0;
    slot.indices.used = 0;
    slot.descriptors_used = 0;
    slot.queries_used = 0;
    slot.timed.clear();

    if (!g_gpu.white.valid()) {
        create_fallbacks(commands);
    }
    const bool dump_effects = g_dump_backbuffer != 0 && context.completion_fence != nullptr;
    const auto dump_name = dump_effects
        ? std::format("golden-{}-{}", g_dump_batch, 3u - g_dump_backbuffer) : std::string{};
    if (dump_effects) {
        --g_dump_backbuffer;
        readback::request(g_gpu.device, commands, context.backbuffer,
                          D3D12_RESOURCE_STATE_RENDER_TARGET, dump_name + "-before",
                          context.completion_fence, context.completion_value);
    }
    for (auto& [handle, shader] : g_state.shaders) {
        (void)handle;
        if (shader.dirty) {
            const unsigned before = shader.generation;
            compile_shader(shader);
            if (shader.generation != before) {
                drop_pipelines_for(shader.handle);
            }
        }
    }
    for (auto& [handle, texture] : g_state.textures) {
        (void)handle;
        if (!texture.is_target) {
            materialise_texture(commands, texture);
        }
    }

    depth::end_frame(g_gpu.presented);
    capture_scene_depth(commands);
    g_state.frame.depth_available = g_gpu.scene_depth_valid;

    std::vector<Effect*> order;
    if (!g_state.settings.bypass) {
        for (auto& [handle, effect] : g_state.effects) {
            (void)handle;
            if (!effect.enabled || effect.stage == BRIDGER_FX_STAGE_PRE_UPSCALE) {
                continue;
            }
            if (!effect.is_pass && effect.vertices.empty() && effect.mesh_draws.empty()) {
                continue;
            }
            order.push_back(&effect);
            if (order.size() >= kMaxEffectsPerFrame) break;
        }
    }
    std::sort(order.begin(), order.end(), [](const Effect* a, const Effect* b) {
        return std::tie(a->stage, a->priority, a->handle) < std::tie(b->stage, b->priority, b->handle);
    });
    if (order.empty()) {
        if (dump_effects && context.completion_fence) {
            readback::request(g_gpu.device, commands, context.backbuffer,
                              D3D12_RESOURCE_STATE_RENDER_TARGET, dump_name + "-effects",
                              context.completion_fence, context.completion_value);
        }
        return;
    }

    ID3D12DescriptorHeap* heaps[] = {g_gpu.gpu_heap};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootSignature(g_gpu.root);

    D3D12_GPU_VIRTUAL_ADDRESS frame_constants = 0;
    write_frame_constants(slot, frame_constants);
    if (frame_constants == 0) {
        return;
    }

    D3D12_RESOURCE_STATES backbuffer_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bool wants_history = false;
    bool needs_mod_depth = false;
    for (const Effect* effect : order) {
        wants_history |= effect->wants_history;
        needs_mod_depth |= !effect->is_pass && effect->depth != BRIDGER_FX_DEPTH_NONE && effect->output == 0;
    }
    if (needs_mod_depth && g_gpu.mod_depth.valid()) {
        transition(commands, g_gpu.mod_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        commands->ClearDepthStencilView(dsv_handle(g_gpu.mod_depth.dsv), D3D12_CLEAR_FLAG_DEPTH,
                                        kFarDepth, 0, 0, nullptr);
    }
    if (g_gpu.scene_depth_valid) {
        transition(commands, g_gpu.scene_depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    const bool timing = g_state.settings.timings && g_gpu.queries != nullptr && slot.readback != nullptr
                     && g_gpu.timestamp_frequency != 0;
    const unsigned query_base = context.frame_index * kTimestampsPerFrame;
    std::vector<Handle> touched_targets;

    for (int stage_index = 0; stage_index < 3; ++stage_index) {
        const auto stage = static_cast<BridgerFxStage>(stage_index);
        std::vector<Effect*> in_stage;
        for (Effect* effect : order) {
            if (effect->stage == stage) {
                in_stage.push_back(effect);
            }
        }
        if (in_stage.empty()) {
            continue;
        }

        copy_into(commands, g_gpu.scene, context.backbuffer, backbuffer_state);

        GpuTexture* chain_input = &g_gpu.scene;
        GpuTexture* chain_surface = nullptr;
        unsigned chain_passes = 0;
        for (const Effect* effect : in_stage) {
            chain_passes += (effect->is_pass && effect->output == 0) ? 1u : 0u;
        }
        int ping = 0;
        const bool history_ready = g_gpu.history_valid && resolve_history(commands, slot,
            context.frame_index, frame_constants, g_gpu.history, g_gpu.scene, g_gpu.resolved_history);

        for (Effect* effect : in_stage) {
            const Shader* shader = shader_for(*effect);
            if (shader == nullptr || !shader->ready || shader->vs == nullptr) {
                effect->problem = shader == nullptr ? "shader handle is not valid" : "shader not compiled";
                continue;
            }
            if (effect->is_pass && shader->kind != BRIDGER_FX_SHADER_FULLSCREEN) {
                effect->problem = "a pass needs a FULLSCREEN shader";
                continue;
            }
            if (!effect->is_pass && shader->kind == BRIDGER_FX_SHADER_FULLSCREEN) {
                effect->problem = "a draw list needs a WORLD or SCREEN shader";
                continue;
            }

            prepare_user_textures(commands, *effect);

            Target target;
            GpuTexture* chain_target = nullptr;
            if (effect->output == 0 && stage == BRIDGER_FX_STAGE_POST) {
                if (effect->is_pass) {
                    --chain_passes;
                    if (chain_passes > 0) {
                        chain_target = &g_gpu.chain[ping];
                        ping ^= 1;
                        if (chain_target == chain_input) {
                            chain_target = &g_gpu.chain[ping];
                            ping ^= 1;
                        }
                    }
                } else {
                    chain_target = chain_surface;
                }
            }
            if (!resolve_target(*effect, target, context, backbuffer_state, chain_target, touched_targets)) {
                continue;
            }

            GpuTexture* input = effect->is_pass ? chain_input : &g_gpu.scene;
            if (input == target.texture) {
                input = &g_gpu.scene;
            }
            transition(commands, *input, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(commands, g_gpu.scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (g_gpu.history_valid) {
                transition(commands, g_gpu.history, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }

            Bindings bindings;
            bindings.srv[8] = g_gpu.motion_valid ? g_gpu.scene_motion.srv : g_gpu.far_depth.srv;
            bindings.srv[0] = g_gpu.scene.srv;
            bindings.srv[1] = g_gpu.scene_depth_valid ? g_gpu.scene_depth.srv : g_gpu.far_depth.srv;
            bindings.srv[2] = input->srv;
            bindings.srv[3] = history_ready ? g_gpu.resolved_history.srv : g_gpu.scene.srv;
            for (unsigned i = 0; i < kUserSlots; ++i) {
                const Handle handle = effect->textures[i];
                unsigned srv = srv_of(handle);
                if (handle != 0) {
                    const auto found = g_state.textures.find(handle);
                    if (found != g_state.textures.end() && &found->second.color == target.texture) {
                        srv = g_gpu.white.srv;
                        effect->problem = "texture slot bound to the output target";
                    }
                }
                bindings.srv[4 + i] = srv;
            }

            if (timing && slot.queries_used + 2 <= kTimestampsPerFrame) {
                commands->EndQuery(g_gpu.queries, D3D12_QUERY_TYPE_TIMESTAMP, query_base + slot.queries_used);
            }
            if (effect->is_pass) {
                execute_pass(commands, slot, context.frame_index, *effect, *shader, target,
                             frame_constants, bindings);
            } else {
                execute_draw_list(commands, slot, context.frame_index, *effect, *shader, target,
                                  frame_constants, bindings);
            }
            if (timing && slot.queries_used + 2 <= kTimestampsPerFrame) {
                commands->EndQuery(g_gpu.queries, D3D12_QUERY_TYPE_TIMESTAMP, query_base + slot.queries_used + 1);
                slot.queries_used += 2;
                slot.timed.push_back(effect->handle);
            }

            if (effect->ran) {
                ++stats.effects_run;
                stats.draw_calls += effect->draw_calls;
                stats.vertices += effect->vertex_total;
            }
            if (effect->is_pass && effect->output == 0 && stage == BRIDGER_FX_STAGE_POST) {
                if (chain_target != nullptr) {
                    chain_input = chain_target;
                    chain_surface = chain_target;
                } else {
                    chain_surface = nullptr;
                    chain_input = &g_gpu.scene;
                }
            }
        }

        if (stage == BRIDGER_FX_STAGE_POST && wants_history) {
            copy_into(commands, g_gpu.history, context.backbuffer, backbuffer_state);
            g_gpu.history_valid = true;
        }
    }

    if (timing && slot.queries_used > 0) {
        commands->ResolveQueryData(g_gpu.queries, D3D12_QUERY_TYPE_TIMESTAMP, query_base,
                                   slot.queries_used, slot.readback, 0);
        slot.timed_valid = true;
    }

    for (auto& [handle, texture] : g_state.textures) {
        (void)handle;
        if (texture.color.valid() && texture.color.state == D3D12_RESOURCE_STATE_RENDER_TARGET) {
            transition(commands, texture.color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }
    transition(commands, context.backbuffer, backbuffer_state, D3D12_RESOURCE_STATE_RENDER_TARGET);

    if (dump_effects && context.completion_fence) {
        readback::request(g_gpu.device, commands, context.backbuffer,
                          D3D12_RESOURCE_STATE_RENDER_TARGET, dump_name + "-effects",
                          context.completion_fence, context.completion_value);
    }

    stats.cpu_ms = static_cast<float>(std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - cpu_started).count());
}

void dump_backbuffer() {
    std::scoped_lock lock(g_state.mutex);
    if (g_dump_backbuffer != 0 || readback::pending() != 0) {
        return;
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    g_dump_batch = std::format("{}-{}", stamp, ++g_dump_sequence);
    g_dump_backbuffer = 2;
}

namespace {
void free_upscale_job(UpscaleJob& job) {
    for (auto* texture : {&job.scene, &job.input, &job.target, &job.depth, &job.mod_depth, &job.history}) {
        if (job.detached) release(texture->resource);
        else destroy_gpu_texture(*texture);
    }
    for (auto* ring : {&job.slot.constants, &job.slot.vertices, &job.slot.indices}) {
        if (ring->resource) ring->resource->Unmap(0, nullptr);
        release(ring->resource);
    }
}
UpscaleJob::~UpscaleJob() { free_upscale_job(*this); }

void detach_upscale_jobs() {
    reap_upscale_jobs();
    for (auto& job : g_upscale_jobs) job->detached = true;
    g_upscale_queue.Reset();
}

void reap_upscale_jobs() {
    for (auto it = g_upscale_jobs.begin(); it != g_upscale_jobs.end();) {
        auto& job = **it;
        if (job.submitted && job.fence->GetCompletedValue() >= 1) {
            free_upscale_job(job);
            it = g_upscale_jobs.erase(it);
        } else ++it;
    }
    g_state.stats.upscale_jobs = static_cast<unsigned>(g_upscale_jobs.size());
    auto& inventory = g_state.stats.upscale_inventory;
    inventory.clear();
    for (const auto& job : g_upscale_jobs) {
        std::string owners;
        for (const auto& owner : job->owners) {
            if (!owners.empty()) owners += ", ";
            owners += owner.empty() ? "core" : owner;
        }
        inventory.push_back(std::format("{}: list {}, fence {} {}, {} leases",
            owners.empty() ? "core motion capture" : owners,
            static_cast<void*>(job->commands.Get()), static_cast<void*>(job->fence.Get()),
            job->submitted ? "submitted" : "recorded", job->leases.size()));
        inventory.push_back(std::format("scene {} input {} target {} history {} depth {} mod depth {}",
            static_cast<void*>(job->scene.resource), static_cast<void*>(job->input.resource),
            static_cast<void*>(job->target.resource), static_cast<void*>(job->history.resource),
            static_cast<void*>(job->depth.resource), static_cast<void*>(job->mod_depth.resource)));
        inventory.push_back(std::format("constants {} vertices {} indices {} descriptors {}",
            static_cast<void*>(job->slot.constants.resource), static_cast<void*>(job->slot.vertices.resource),
            static_cast<void*>(job->slot.indices.resource), static_cast<void*>(job->heap.Get())));
    }
}

struct UpscaleRecord {
    UpscaleJob* job;
    const ngx::Snapshot* inputs;
    std::vector<Effect*>* effects;
    ID3D12Resource* color;
    D3D12_RESOURCE_STATES color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* motion;
    D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* depth;
    D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
};

void restore_upscale_inputs(void* raw) {
    auto& r = *static_cast<UpscaleRecord*>(raw);
    auto* commands = r.job->commands.Get();
    transition(commands, r.color, r.color_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (r.motion) transition(commands, r.motion, r.motion_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (r.depth) transition(commands, r.depth, r.depth_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void record_upscale(void* raw) {
    auto& r = *static_cast<UpscaleRecord*>(raw);
    auto& job = *r.job;
    auto* commands = job.commands.Get();
    if (r.motion && g_gpu.scene_motion.valid()) {
        copy_into(commands, g_gpu.scene_motion, r.motion, r.motion_state);
        transition(commands, g_gpu.scene_motion, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_gpu.motion_valid = true;
    }
    if (r.effects->empty()) { restore_upscale_inputs(raw); return; }
    copy_into(commands, job.scene, r.color, r.color_state);
    copy_into(commands, job.input, r.color, r.color_state);
    copy_into(commands, job.target, r.color, r.color_state);
    if (r.depth && job.depth.valid()) {
        copy_into(commands, job.depth, r.depth, r.depth_state);
        transition(commands, job.depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    transition(commands, job.scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(commands, job.input, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[]{job.heap.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootSignature(g_gpu.root);
    D3D12_GPU_VIRTUAL_ADDRESS constants = 0;
    write_frame_constants(job.slot, constants);
    if (!constants) return;
    Target target;
    target.rtv = rtv_handle(job.target.rtv);
    target.format = job.target.view_format;
    target.width = r.inputs->render_width;
    target.height = r.inputs->render_height;
    target.texture = &job.target;
    if (job.mod_depth.valid()) {
        target.dsv = dsv_handle(job.mod_depth.dsv);
        target.has_dsv = true;
        target.depth_format = job.mod_depth.view_format;
        commands->ClearDepthStencilView(target.dsv, D3D12_CLEAR_FLAG_DEPTH, kFarDepth, 0, 0, nullptr);
    }
    Bindings bindings;
    bindings.srv[0] = job.scene.srv;
    bindings.srv[1] = job.depth.valid() ? job.depth.srv : g_gpu.far_depth.srv;
    bindings.srv[2] = job.input.srv;
    const bool history_ready = g_gpu.pre_history_valid && resolve_history(commands, job.slot, 0,
        constants, g_gpu.pre_history, job.scene, job.history);
    bindings.srv[3] = history_ready ? job.history.srv : job.scene.srv;
    bindings.srv[8] = g_gpu.motion_valid ? g_gpu.scene_motion.srv : g_gpu.far_depth.srv;
    std::vector<Handle> touched;
    FrameContext frame_context;
    frame_context.commands = commands;
    bool wrote_color = false;
    for (auto* effect : *r.effects) {
        const auto* shader = shader_for(*effect);
        if (!shader || !shader->ready) { effect->problem = "waiting for shader compilation at Present"; continue; }
        if ((shader->kind == BRIDGER_FX_SHADER_FULLSCREEN) != effect->is_pass) {
            effect->problem = "shader kind does not match the effect"; continue;
        }
        prepare_user_textures(commands, *effect);
        Target draw_target = target;
        if (effect->output != 0 && !resolve_target(*effect, draw_target, frame_context, r.color_state, nullptr, touched)) continue;
        bindings.srv[2] = effect->is_pass ? job.input.srv : job.scene.srv;
        for (unsigned i = 0; i < kUserSlots; ++i) bindings.srv[4+i] = srv_of(effect->textures[i]);
        for (unsigned i = 0; i < kUserSlots; ++i) {
            if (effect->textures[i] && effect->textures[i] == effect->output) {
                bindings.srv[4+i] = g_gpu.white.srv;
                effect->problem = "texture slot bound to output target";
            }
        }
        transition(commands, job.target, D3D12_RESOURCE_STATE_RENDER_TARGET);
        effect->draw_calls = 0;
        if (effect->is_pass) execute_pass(commands, job.slot, 0, *effect, *shader, draw_target, constants, bindings);
        else execute_draw_list(commands, job.slot, 0, *effect, *shader, draw_target, constants, bindings);
        g_state.stats.upscale_draws += effect->draw_calls;
        if (effect->output != 0) {
            transition(commands, *draw_target.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            continue;
        }
        wrote_color |= effect->draw_calls != 0;
        copy_into(commands, job.input, job.target.resource, job.target.state);
        transition(commands, job.input, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (g_gpu.pre_history.valid()) {
        if (std::any_of(r.effects->begin(), r.effects->end(), [](auto* e) { return e->wants_history; })) {
            copy_into(commands, g_gpu.pre_history, job.target.resource, job.target.state);
            g_gpu.pre_history_valid = true;
        }
    }
    if (wrote_color) {
        transition(commands, job.target, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(commands, r.color, r.color_state, D3D12_RESOURCE_STATE_COPY_DEST);
        commands->CopyResource(r.color, job.target.resource);
    }
    restore_upscale_inputs(raw);
}
}

void submitted(ID3D12CommandQueue* queue, unsigned count, ID3D12CommandList* const* lists) {
    std::scoped_lock lock(g_state.mutex);
    if (!queue || !lists || count > 4096) return;
    for (auto& job : g_upscale_jobs) {
        if (job->submitted) continue;
        for (unsigned i = 0; i < count; ++i) {
            ID3D12CommandList* list = nullptr;
            if (!safe_read(lists + i, &list, sizeof list)) return;
            if (list != job->commands.Get()) continue;
            job->queue = queue;
            if (g_upscale_queue.Get() != queue)
                log::info("fx: NGX submission list {} uses queue {}", static_cast<void*>(list), static_cast<void*>(queue));
            g_upscale_queue = queue;
            struct Signal { ID3D12CommandQueue* queue; ID3D12Fence* fence; HRESULT result = E_FAIL; } signal{queue, job->fence.Get()};
            const auto fault = guarded_call([](void* raw) {
                auto& s = *static_cast<Signal*>(raw); s.result = s.queue->Signal(s.fence, 1);
            }, &signal);
            if (!fault && SUCCEEDED(signal.result)) job->submitted = true;
            else {
                g_state.settings.pre_upscale = false;
                g_state.stats.upscale_note = "queue signal failed; stage disabled and GPU leases retained";
            }
            break;
        }
    }
}

void render_raytracing(ID3D12GraphicsCommandList* commands, const ngx::Snapshot& inputs) try {
    std::scoped_lock lock(g_state.mutex);
    if (!g_gpu.ready || !commands || !rt::boot_enabled() || g_state.settings.bypass) return;
    if (!inputs.render_width || !inputs.render_height || !std::isfinite(inputs.jitter_x) || !std::isfinite(inputs.jitter_y)) return;
    const auto old_frame = g_state.frame;
    const auto old_width = g_state.stats.width;
    const auto old_height = g_state.stats.height;
    build_frame(old_frame.delta, inputs.render_width, inputs.render_height);
    math::Mat4 projection, view;
    std::memcpy(projection.m, g_state.frame.projection, sizeof projection.m);
    std::memcpy(view.m, g_state.frame.view, sizeof view.m);
    const bool camera_valid = g_state.frame.camera.valid;
    g_state.frame = old_frame;
    g_state.stats.width = old_width;
    g_state.stats.height = old_height;
    projection = math::jitter_projection(projection, inputs.jitter_x, inputs.jitter_y, inputs.render_width, inputs.render_height);
    rt::render(commands, inputs, view, projection, camera_valid, g_state.settings.depth_reversed);
} catch (...) {
    log::error("fx: raytracing pass preparation failed");
}

void render_pre_upscale(ID3D12GraphicsCommandList* commands, const ngx::Snapshot& inputs) try {
    std::scoped_lock lock(g_state.mutex);
    if (!g_gpu.ready || !commands) return;
    reap_upscale_jobs();
    g_state.stats.upscale_draws = 0;
    if (!g_state.settings.pre_upscale || g_state.settings.bypass) return;
    if (g_upscale_jobs.size() >= 8) {
        g_state.stats.upscale_note = "waiting for GPU submission or completion";
        return;
    }
    auto job = std::make_unique<UpscaleJob>();
    job->commands = commands;
    if (FAILED(g_gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&job->fence)))) return;
    if (g_upscale_queue.Get() != g_gpu.queue) {
        g_state.stats.upscale_note = "probing NGX submission queue; no input writes";
        g_upscale_jobs.push_back(std::move(job));
        return;
    }
    if (!g_gpu.white.valid() || !inputs.render_width || !inputs.render_height
        || !std::isfinite(inputs.jitter_x) || !std::isfinite(inputs.jitter_y)
        || (inputs.subrect_width && inputs.subrect_width != inputs.render_width)
        || (inputs.subrect_height && inputs.subrect_height != inputs.render_height)) {
        g_state.stats.upscale_note = "waiting for full size NGX input and initialized resources";
        return;
    }
    auto* color = const_cast<ID3D12Resource*>(static_cast<const ID3D12Resource*>(inputs.color));
    const auto desc = color->GetDesc();
    if (desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.DepthOrArraySize != 1
        || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        g_state.stats.upscale_note = "unsupported NGX input shape or command list type";
        return;
    }
    std::vector<Effect*> effects;
    std::size_t vertices = 0, indices = 0, meshes = 0;
    for (auto& [handle, effect] : g_state.effects) {
        if (!effect.enabled || effect.stage != BRIDGER_FX_STAGE_PRE_UPSCALE) continue;
        if (effects.size() >= kMaxEffectsPerFrame) break;
        effects.push_back(&effect);
        if (std::any_of(effect.indices.begin(), effect.indices.end(), [&](auto index) {
                return index >= effect.vertices.size(); })) {
            effects.pop_back(); effect.problem = "invalid vertex index"; continue;
        }
        meshes += effect.mesh_draws.size();
        vertices += effect.vertices.size() * sizeof(BridgerFxVertex) + 16;
        indices += effect.indices.size() * sizeof(unsigned) + 16;
        job->owners.push_back(effect.owner);
    }
    if (vertices > (32u << 20) || indices > (16u << 20) || meshes > kMaxEffectsPerFrame) {
        g_state.stats.upscale_note = "pre upscale geometry exceeds the per call budget";
        return;
    }
    std::sort(effects.begin(), effects.end(), [](auto* a, auto* b) {
        return std::tie(a->priority, a->handle) < std::tie(b->priority, b->handle);
    });
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kFrameDescriptors;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    bool ok = true;
    if (!effects.empty()) {
    ok = SUCCEEDED(g_gpu.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&job->heap)));
    ok = ok && ensure_ring(job->slot.constants, kMaxEffectsPerFrame * (kMaxConstants + 768u) + 1024u)
        && ensure_ring(job->slot.vertices, vertices) && ensure_ring(job->slot.indices, indices);
    ok = ok && create_gpu_texture(job->scene, inputs.render_width, inputs.render_height, 1,
        desc.Format, desc.Format, false, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ok = ok && create_gpu_texture(job->input, inputs.render_width, inputs.render_height, 1,
        desc.Format, desc.Format, false, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ok = ok && create_gpu_texture(job->target, inputs.render_width, inputs.render_height, 1,
        desc.Format, desc.Format, true, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ok = ok && create_gpu_texture(job->history, inputs.render_width, inputs.render_height, 1,
        desc.Format, desc.Format, true, false, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (g_gpu.pre_history.valid() && (g_gpu.pre_history.width != inputs.render_width
        || g_gpu.pre_history.height != inputs.render_height || g_gpu.pre_history.format != desc.Format)) {
        destroy_gpu_texture(g_gpu.pre_history);
        g_gpu.pre_history_valid = false;
    }
    if (ok && std::any_of(effects.begin(), effects.end(), [](auto* e) {
        return !e->is_pass && !e->output && e->depth != BRIDGER_FX_DEPTH_NONE;
    })) ok = create_gpu_texture(job->mod_depth, inputs.render_width, inputs.render_height, 1,
        DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT, false, true, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (ok && std::any_of(effects.begin(), effects.end(), [](auto* e) { return e->wants_history; })
        && (!g_gpu.pre_history.valid() || g_gpu.pre_history.width != inputs.render_width
        || g_gpu.pre_history.height != inputs.render_height || g_gpu.pre_history.format != desc.Format)) {
        g_gpu.pre_history_valid = false;
        ok = create_gpu_texture(g_gpu.pre_history, inputs.render_width, inputs.render_height, 1,
            desc.Format, desc.Format, false, false, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    auto* depth_resource = const_cast<ID3D12Resource*>(static_cast<const ID3D12Resource*>(inputs.depth));
    DXGI_FORMAT depth_format, depth_view;
    if (ok && !effects.empty() && depth_resource && depth_copy_formats(inputs.depth_format, depth_format, depth_view)) {
        const auto d = depth_resource->GetDesc();
        if (d.SampleDesc.Count == 1 && d.MipLevels == 1 && d.DepthOrArraySize == 1)
            ok = create_gpu_texture(job->depth, static_cast<unsigned>(d.Width), d.Height, 1,
                depth_format, depth_view, false, false, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    auto* motion = const_cast<ID3D12Resource*>(static_cast<const ID3D12Resource*>(inputs.motion));
    if (!std::isfinite(inputs.mv_scale_x) || !std::isfinite(inputs.mv_scale_y)
        || inputs.mv_scale_x == 0 || inputs.mv_scale_y == 0) motion = nullptr;
    if (ok && motion) {
        const auto m = motion->GetDesc();
        if (m.SampleDesc.Count != 1 || m.MipLevels != 1 || m.DepthOrArraySize != 1) motion = nullptr;
        else if (!g_gpu.scene_motion.valid() || g_gpu.scene_motion.width != m.Width
            || g_gpu.scene_motion.height != m.Height || g_gpu.scene_motion.format != m.Format) {
            ok = create_gpu_texture(g_gpu.scene_motion, static_cast<unsigned>(m.Width), m.Height,
                1, m.Format, m.Format, false, false, D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
    if (!ok) { free_upscale_job(*job); g_state.stats.upscale_note = "pre upscale allocation failed"; return; }
    const auto old_frame = g_state.frame;
    const auto old_stats = g_state.stats;
    build_frame(old_frame.delta, inputs.render_width, inputs.render_height);
    math::Mat4 projection, view;
    std::memcpy(projection.m, g_state.frame.projection, sizeof projection.m);
    std::memcpy(view.m, g_state.frame.view, sizeof view.m);
    projection = math::jitter_projection(projection, inputs.jitter_x, inputs.jitter_y,
        inputs.render_width, inputs.render_height);
    store(g_state.frame.projection, projection);
    store(g_state.frame.view_projection, math::multiply(projection, view));
    const bool old_depth = g_gpu.scene_depth_valid;
    g_gpu.scene_depth_valid = job->depth.valid();
    g_gpu.motion_valid = false;
    g_gpu.motion_reset = inputs.reset != 0 || !g_gpu.pre_history_valid;
    g_gpu.motion_scale[0] = inputs.mv_scale_x / inputs.render_width;
    g_gpu.motion_scale[1] = inputs.mv_scale_y / inputs.render_height;
    auto* old_heap = g_gpu.gpu_heap;
    struct RestoreFrame {
        BridgerFxFrame frame;
        unsigned width, height;
        bool depth;
        ID3D12DescriptorHeap* heap;
        ~RestoreFrame() {
            g_state.frame = frame; g_state.stats.width = width; g_state.stats.height = height;
            g_gpu.scene_depth_valid = depth; g_gpu.gpu_heap = heap;
        }
    } restore{old_frame, old_stats.width, old_stats.height, old_depth, old_heap};
    g_gpu.gpu_heap = job->heap.Get();
    UpscaleRecord record{job.get(), &inputs, &effects, color};
    record.motion = motion;
    record.depth = job->depth.valid() ? depth_resource : nullptr;
    auto* live = job.get();
    live->leases.reserve(g_state.textures.size() * 2 + g_state.meshes.size() * 2
        + g_gpu.pipelines.size() + effects.size() + 16);
    g_upscale_jobs.push_back(std::move(job));
    std::uint32_t fault = 0;
    try { fault = guarded_call(record_upscale, &record); }
    catch (...) { fault = 0xe06d7363; }
    const auto restore_fault = guarded_call(restore_upscale_inputs, &record);
    g_gpu.gpu_heap = old_heap;
    g_state.frame = old_frame;
    g_gpu.scene_depth_valid = old_depth;
    g_gpu.motion_reset = inputs.reset != 0 || !g_gpu.history_valid;
    g_state.stats.width = old_stats.width;
    g_state.stats.height = old_stats.height;
    auto lease = [&](IUnknown* object) { if (object) live->leases.emplace_back(object); };
    lease(g_gpu.root); lease(g_gpu.pre_history.resource); lease(g_gpu.white.resource);
    lease(g_gpu.far_depth.resource); lease(g_gpu.scene_motion.resource);
    for (const auto& [key, pipeline] : g_gpu.pipelines) lease(pipeline);
    for (const auto& [key, texture] : g_state.textures) { lease(texture.color.resource); lease(texture.depth.resource); }
    for (const auto& [key, mesh] : g_state.meshes) { lease(mesh.vertex_buffer); lease(mesh.index_buffer); }
    if (fault || restore_fault) {
        g_state.settings.pre_upscale = false;
        g_state.stats.upscale_note = "pre upscale fault; stage disabled";
        log::error("fx: pre upscale fault {:#x}, restore {:#x}", fault, restore_fault);
        for (const auto& owner : live->owners) if (!owner.empty()) destroy_owned(owner);
    } else g_state.stats.upscale_note = "recorded before NGX; awaiting in game grid check";
    g_state.stats.upscale_jobs = static_cast<unsigned>(g_upscale_jobs.size());
} catch (...) {
    std::scoped_lock lock(g_state.mutex);
    g_state.settings.pre_upscale = false;
    g_gpu.motion_valid = false;
    g_gpu.motion_reset = true;
    log::error("fx: pre upscale preparation failed; disabled");
}

void destroy_owned(std::string_view owner) {
    pipeline::destroy_owned(owner);
    std::scoped_lock lock(g_state.mutex);
    g_gpu.history_valid = false;
    g_gpu.pre_history_valid = false;
    for (auto it = g_state.effects.begin(); it != g_state.effects.end();) {
        it = it->second.owner == owner ? g_state.effects.erase(it) : std::next(it);
    }
    for (auto it = g_state.shaders.begin(); it != g_state.shaders.end();) {
        if (it->second.owner == owner && !it->second.builtin) {
            drop_pipelines_for(it->first);
            release(it->second.vs);
            release(it->second.ps);
            it = g_state.shaders.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_state.textures.begin(); it != g_state.textures.end();) {
        if (it->second.owner == owner) {
            destroy_gpu_texture(it->second.color);
            destroy_gpu_texture(it->second.depth);
            it = g_state.textures.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_state.meshes.begin(); it != g_state.meshes.end();) {
        if (it->second.owner == owner) {
            retire(it->second.vertex_buffer);
            retire(it->second.index_buffer);
            it = g_state.meshes.erase(it);
        } else {
            ++it;
        }
    }
}

namespace detail {

void destroy_shader_objects(Shader& shader) {
    drop_pipelines_for(shader.handle);
    release(shader.vs);
    release(shader.ps);
}

void destroy_texture_objects(Texture& texture) {
    destroy_gpu_texture(texture.color);
    destroy_gpu_texture(texture.depth);
}

void destroy_mesh_objects(Mesh& mesh) {
    retire(mesh.vertex_buffer);
    retire(mesh.index_buffer);
    mesh.vertex_buffer = nullptr;
    mesh.index_buffer = nullptr;
}

DXGI_FORMAT dxgi_format(BridgerFxFormat format) {
    return to_dxgi(format);
}

const char* camera_source() {
    return g_state.camera_source;
}

unsigned pipeline_count() {
    return static_cast<unsigned>(g_gpu.pipelines.size());
}

ID3D12Resource* texture_resource(Handle texture) {
    std::scoped_lock lock(g_state.mutex);
    const auto found = g_state.textures.find(texture);
    if (found == g_state.textures.end() || found->second.is_target || found->second.color.resource == nullptr) return nullptr;
    found->second.color.resource->AddRef();
    return found->second.color.resource;
}

}

}
