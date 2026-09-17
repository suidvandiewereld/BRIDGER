#include "fx/pipeline.h"
#include "fx/rt.h"

#include <d3dcompiler.h>
#include <MinHook.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <cctype>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "core/guard.h"
#include "core/log.h"
#include "fx/bindings.h"
#include "fx/dxbc.h"
#include "loader/registry.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace bridger::fx::detail {
ID3D12Resource* texture_resource(BridgerFxHandle texture);
}

namespace bridger::fx::pipeline {
namespace {

constexpr int kDeviceCreateHeap = 14;
constexpr int kDeviceCreateRoot = 16;
constexpr int kDeviceCreateGraphics = 10;
constexpr int kDeviceCreateCompute = 11;
constexpr int kDeviceCreateLibrary = 44;
constexpr int kDeviceCreateStream = 47;
constexpr int kListDrawInstanced = 12;
constexpr int kListDrawIndexed = 13;
constexpr int kListDispatch = 14;
constexpr int kListSetTopology = 20;
constexpr int kListSetPipelineState = 25;
constexpr int kListExecuteIndirect = 59;
constexpr int kListSetDescriptorHeaps = 28;
constexpr int kListSetGraphicsRoot = 30;
constexpr int kLibraryLoadGraphics = 9;
constexpr int kLibraryLoadCompute = 10;
constexpr int kLibraryLoadStream = 13;

constexpr unsigned kRetireFrames = 8;
constexpr unsigned kStageCount = 6;
constexpr unsigned kReloadInterval = 30;

using bindings::kBindingTextures;
using bindings::kBindingConstants;

struct Blob {
    std::vector<std::uint8_t> bytes;
    std::uint64_t id = 0;
    dxbc::Stage stage = dxbc::Stage::Unknown;
};

struct GraphicsDesc {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<std::string> element_names;
    std::vector<D3D12_SO_DECLARATION_ENTRY> so_entries;
    std::vector<std::string> so_names;
    std::vector<UINT> so_strides;

    void capture(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& source) {
        desc = source;
        desc.CachedPSO = {};
        if (desc.pRootSignature != nullptr) desc.pRootSignature->AddRef();
        elements.assign(source.InputLayout.pInputElementDescs,
                        source.InputLayout.pInputElementDescs + source.InputLayout.NumElements);
        element_names.reserve(elements.size());
        for (auto& e : elements) {
            element_names.emplace_back(e.SemanticName != nullptr ? e.SemanticName : "");
        }
        so_entries.assign(source.StreamOutput.pSODeclaration,
                          source.StreamOutput.pSODeclaration + source.StreamOutput.NumEntries);
        for (auto& e : so_entries) so_names.emplace_back(e.SemanticName != nullptr ? e.SemanticName : "");
        so_strides.assign(source.StreamOutput.pBufferStrides,
                          source.StreamOutput.pBufferStrides + source.StreamOutput.NumStrides);
        relink();
    }

    void relink() {
        for (std::size_t i = 0; i < elements.size(); ++i) elements[i].SemanticName = element_names[i].c_str();
        for (std::size_t i = 0; i < so_entries.size(); ++i) so_entries[i].SemanticName = so_names[i].c_str();
        desc.InputLayout.pInputElementDescs = elements.empty() ? nullptr : elements.data();
        desc.InputLayout.NumElements = static_cast<UINT>(elements.size());
        desc.StreamOutput.pSODeclaration = so_entries.empty() ? nullptr : so_entries.data();
        desc.StreamOutput.NumEntries = static_cast<UINT>(so_entries.size());
        desc.StreamOutput.pBufferStrides = so_strides.empty() ? nullptr : so_strides.data();
        desc.StreamOutput.NumStrides = static_cast<UINT>(so_strides.size());
    }

    ~GraphicsDesc() {
        if (desc.pRootSignature != nullptr) desc.pRootSignature->Release();
    }
};


bool subobject_shape(UINT type, std::size_t& size, std::size_t& align) {
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: size = sizeof(void*); align = 8; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            size = sizeof(D3D12_SHADER_BYTECODE); align = 8; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: size = sizeof(D3D12_STREAM_OUTPUT_DESC); align = 8; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: size = sizeof(D3D12_BLEND_DESC); align = 4; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: size = sizeof(UINT); align = 4; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: size = sizeof(D3D12_RASTERIZER_DESC); align = 4; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: size = sizeof(D3D12_DEPTH_STENCIL_DESC); align = 4; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: size = sizeof(D3D12_INPUT_LAYOUT_DESC); align = 8; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: size = sizeof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE); align = 4; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: size = sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE); align = 4; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: size = sizeof(D3D12_RT_FORMAT_ARRAY); align = 4; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: size = sizeof(DXGI_FORMAT); align = 4; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: size = sizeof(DXGI_SAMPLE_DESC); align = 4; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: size = sizeof(UINT); align = 4; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: size = sizeof(D3D12_CACHED_PIPELINE_STATE); align = 8; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: size = sizeof(D3D12_PIPELINE_STATE_FLAGS); align = 4; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: size = sizeof(D3D12_DEPTH_STENCIL_DESC1); align = 4; return true;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: size = sizeof(D3D12_VIEW_INSTANCING_DESC); align = 8; return true;
        default: return false;
    }
}


struct StreamDesc {
    std::vector<std::uint8_t> bytes;
    std::size_t shader_offset[kStageCount]{};
    std::size_t input_layout_offset = SIZE_MAX;
    std::size_t stream_output_offset = SIZE_MAX;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<std::string> element_names;
    std::vector<D3D12_SO_DECLARATION_ENTRY> so_entries;
    std::vector<std::string> so_names;
    std::vector<UINT> so_strides;
    ID3D12RootSignature* root = nullptr;
    unsigned render_targets = 0;
    DXGI_FORMAT rtv[8]{};
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    bool compute = false;
    bool complete = false;

    StreamDesc() { for (auto& o : shader_offset) o = SIZE_MAX; }
    StreamDesc(const StreamDesc& other) { *this = other; }
    StreamDesc& operator=(const StreamDesc& other) {
        if (this == &other) return *this;
        if (root != nullptr) root->Release();
        bytes = other.bytes;
        std::memcpy(shader_offset, other.shader_offset, sizeof shader_offset);
        input_layout_offset = other.input_layout_offset;
        stream_output_offset = other.stream_output_offset;
        elements = other.elements;
        element_names = other.element_names;
        so_entries = other.so_entries;
        so_names = other.so_names;
        so_strides = other.so_strides;
        root = other.root;
        if (root != nullptr) root->AddRef();
        render_targets = other.render_targets;
        std::memcpy(rtv, other.rtv, sizeof rtv);
        dsv = other.dsv;
        topology = other.topology;
        compute = other.compute;
        complete = other.complete;
        relink();
        return *this;
    }
    ~StreamDesc() { if (root != nullptr) root->Release(); }

    void capture(const D3D12_PIPELINE_STATE_STREAM_DESC& desc) {
        const auto* base = static_cast<const std::uint8_t*>(desc.pPipelineStateSubobjectStream);
        const std::size_t total = desc.SizeInBytes;
        bytes.assign(base, base + total);
        std::size_t at = 0;
        complete = true;
        while (at + sizeof(UINT) <= total) {
            UINT type;
            std::memcpy(&type, base + at, sizeof type);
            std::size_t size = 0, align = 0;
            if (!subobject_shape(type, size, align)) { complete = false; break; }
            const std::size_t data = (at + sizeof(UINT) + align - 1) & ~(align - 1);
            if (data + size > total) { complete = false; break; }
            switch (type) {
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: shader_offset[BRIDGER_FX_STAGE_VS] = data; break;
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: shader_offset[BRIDGER_FX_STAGE_PS] = data; break;
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: shader_offset[BRIDGER_FX_STAGE_DS] = data; break;
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: shader_offset[BRIDGER_FX_STAGE_HS] = data; break;
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: shader_offset[BRIDGER_FX_STAGE_GS] = data; break;
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: shader_offset[BRIDGER_FX_STAGE_CS] = data; compute = true; break;
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
                    std::memcpy(&root, base + data, sizeof root);
                    if (root != nullptr) root->AddRef();
                    break;
                }
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
                    D3D12_INPUT_LAYOUT_DESC layout;
                    std::memcpy(&layout, base + data, sizeof layout);
                    input_layout_offset = data;
                    if (layout.pInputElementDescs != nullptr && layout.NumElements > 0 && layout.NumElements < 64) {
                        elements.assign(layout.pInputElementDescs, layout.pInputElementDescs + layout.NumElements);
                        for (auto& e : elements) element_names.emplace_back(e.SemanticName != nullptr ? e.SemanticName : "");
                    }
                    break;
                }
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
                    D3D12_STREAM_OUTPUT_DESC so;
                    std::memcpy(&so, base + data, sizeof so);
                    stream_output_offset = data;
                    if (so.pSODeclaration != nullptr && so.NumEntries > 0 && so.NumEntries < 256) {
                        so_entries.assign(so.pSODeclaration, so.pSODeclaration + so.NumEntries);
                        for (auto& e : so_entries) so_names.emplace_back(e.SemanticName != nullptr ? e.SemanticName : "");
                    }
                    if (so.pBufferStrides != nullptr && so.NumStrides > 0 && so.NumStrides <= 4) {
                        so_strides.assign(so.pBufferStrides, so.pBufferStrides + so.NumStrides);
                    }
                    break;
                }
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
                    D3D12_RT_FORMAT_ARRAY formats;
                    std::memcpy(&formats, base + data, sizeof formats);
                    render_targets = std::min<unsigned>(formats.NumRenderTargets, 8);
                    for (unsigned i = 0; i < 8; ++i) rtv[i] = formats.RTFormats[i];
                    break;
                }
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: std::memcpy(&dsv, base + data, sizeof dsv); break;
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: std::memcpy(&topology, base + data, sizeof topology); break;
                case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: {
                    const D3D12_CACHED_PIPELINE_STATE none{};
                    std::memcpy(bytes.data() + data, &none, sizeof none);
                    break;
                }
                default: break;
            }
            at = (data + size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
        }
        relink();
    }

    void relink() {
        for (std::size_t i = 0; i < elements.size(); ++i) elements[i].SemanticName = element_names[i].c_str();
        for (std::size_t i = 0; i < so_entries.size(); ++i) so_entries[i].SemanticName = so_names[i].c_str();
        if (input_layout_offset != SIZE_MAX) {
            D3D12_INPUT_LAYOUT_DESC layout{elements.empty() ? nullptr : elements.data(), static_cast<UINT>(elements.size())};
            std::memcpy(bytes.data() + input_layout_offset, &layout, sizeof layout);
        }
        if (stream_output_offset != SIZE_MAX) {
            D3D12_STREAM_OUTPUT_DESC so{};
            so.pSODeclaration = so_entries.empty() ? nullptr : so_entries.data();
            so.NumEntries = static_cast<UINT>(so_entries.size());
            so.pBufferStrides = so_strides.empty() ? nullptr : so_strides.data();
            so.NumStrides = static_cast<UINT>(so_strides.size());
            std::memcpy(bytes.data() + stream_output_offset, &so, sizeof so);
        }
    }

    void add_stream_output() {
        if (stream_output_offset == SIZE_MAX) {
            const std::size_t at = (bytes.size() + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
            bytes.resize(at + sizeof(void*) + sizeof(D3D12_STREAM_OUTPUT_DESC), 0);
            const UINT type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT;
            std::memcpy(bytes.data() + at, &type, sizeof type);
            stream_output_offset = at + sizeof(void*);
        }
        so_names.assign(1, "SV_Position");
        so_entries.assign(1, D3D12_SO_DECLARATION_ENTRY{0, nullptr, 0, 0, 4, 0});
        so_strides.assign(1, 16u);
        relink();
    }

    void set_shader(unsigned stage, const std::vector<std::uint8_t>& code) {
        if (shader_offset[stage] == SIZE_MAX) return;
        const D3D12_SHADER_BYTECODE value{code.empty() ? nullptr : code.data(), code.size()};
        std::memcpy(bytes.data() + shader_offset[stage], &value, sizeof value);
    }

    [[nodiscard]] D3D12_PIPELINE_STATE_STREAM_DESC desc() {
        return {bytes.size(), bytes.data()};
    }
};

struct HookEntry;
struct Replacement;
struct CountSample {
    ID3D12RootSignature* root;
    UINT total;
};

struct Record {
    std::uint64_t hash = 0;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Device* device = nullptr;
    bool compute = false;
    bool replaceable = false;
    bool stream_created = false;
    bool library = false;
    bool stale = false;
    std::shared_ptr<Blob> stages[kStageCount];
    std::unique_ptr<GraphicsDesc> graphics;
    std::unique_ptr<StreamDesc> stream;
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{};
    unsigned creation_order = 0;

    std::atomic<std::uint32_t> draws_frame{0};
    std::atomic<std::uint32_t> draws_last{0};
    std::atomic<std::uint32_t> draws_total{0};
    std::atomic<std::uint32_t> dispatches_frame{0};
    std::atomic<std::uint32_t> first_use{0};
    std::atomic<std::uint32_t> first_use_last{0};

    std::atomic<bool> hooked{false};
    std::vector<HookEntry*> hooks;

    std::atomic<ID3D12PipelineState*> substitute{nullptr};
    std::atomic<std::uint64_t> built_from{0};
    std::atomic<bool> queued{false};
    bool build_failed = false;
    std::string problem;
    bool material_applied = false;
    bool material_skipped = false;
    std::atomic<bool> uses_bindings{false};
    std::atomic<bool> stream_output{false};
};

using bindings::RootExtension;

struct HeapTail {
    ID3D12Device* device = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    std::atomic<unsigned> written{0};
    std::mutex write;
};

struct Targets {
    unsigned count = 0;
    DXGI_FORMAT rtv[8]{};
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    bool known = false;
};

Targets targets_of(const Record& r) {
    Targets t;
    if (r.graphics != nullptr) {
        const auto& d = r.graphics->desc;
        t.count = d.NumRenderTargets;
        for (unsigned i = 0; i < 8; ++i) t.rtv[i] = d.RTVFormats[i];
        t.dsv = d.DSVFormat;
        t.topology = d.PrimitiveTopologyType;
        t.known = true;
    } else if (r.stream != nullptr) {
        t.count = r.stream->render_targets;
        for (unsigned i = 0; i < 8; ++i) t.rtv[i] = r.stream->rtv[i];
        t.dsv = r.stream->dsv;
        t.topology = r.stream->topology;
        t.known = true;
    }
    return t;
}

struct HookEntry {
    BridgerFxHandle handle = 0;
    std::string owner;
    std::uint64_t pipeline = 0;
    BridgerFxDrawHook before = nullptr;
    BridgerFxDrawHook after = nullptr;
    void* user = nullptr;
    std::atomic<bool> enabled{true};
    std::atomic<unsigned> faults{0};
};

struct Replacement {
    BridgerFxHandle handle = 0;
    std::string owner;
    std::uint64_t pipeline = 0;
    BridgerFxShaderStage stage = BRIDGER_FX_STAGE_PS;
    std::filesystem::path path;
    std::string entry;
    std::vector<std::string> defines;
    std::filesystem::file_time_type stamp{};
    std::vector<std::uint8_t> bytecode;
    std::string problem;
    unsigned generation = 0;
    unsigned compiles = 0;
};

struct MaterialHook {
    BridgerFxHandle handle = 0;
    std::string owner;
    std::filesystem::path path;
    std::string entry;
    std::vector<std::string> defines;
    std::filesystem::file_time_type stamp{};
    std::uint32_t min_targets = 2;
    std::uint32_t max_targets = 0;
    std::uint64_t only = 0;
    std::vector<std::uint8_t> compiled[2];
    std::string problem;
    unsigned generation = 0;
    unsigned compiles = 0;
    std::vector<std::uint8_t> constants;
    bool constants_dirty = false;
    BridgerFxHandle textures[kBindingTextures]{};
};

struct Retired {
    ID3D12PipelineState* pso = nullptr;
    unsigned frame = 0;
};

using CreateGraphicsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateComputeFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateStreamFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using CreateLibraryFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device1*, const void*, SIZE_T, REFIID, void**);
using LoadGraphicsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using LoadComputeFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using LoadStreamFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary1*, LPCWSTR, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using SetPsoFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using ExecuteIndirectFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
using CreateRootFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
using CreateHeapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
using SetHeapsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using SetRootFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using SetTopologyFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);

struct Globals {
    std::shared_mutex mutex;
    std::unordered_map<ID3D12PipelineState*, Record*> by_pso;
    std::unordered_map<std::uint64_t, Record*> by_hash;
    std::vector<std::unique_ptr<Record>> records;
    std::unordered_map<std::uint64_t, std::shared_ptr<Blob>> blobs;
    std::size_t retained_bytes = 0;
    std::vector<std::unique_ptr<HookEntry>> hooks;
    std::vector<std::unique_ptr<Replacement>> replacements;
    std::unique_ptr<MaterialHook> material;
    std::vector<Retired> retired;
    BridgerFxHandle next_handle = 0x4000'0000;
    unsigned frame = 0;
    unsigned creations = 0;
    unsigned stream_created = 0;
    unsigned library_loaded = 0;
    std::filesystem::path dumps;
    std::string problem;
    bool hooks_installed = false;

    std::atomic<bool> enabled{true};
    std::atomic<bool> retain{true};
    std::atomic<bool> work{false};

    std::thread builder;
    std::mutex queue_mutex;
    std::condition_variable queue_signal;
    std::deque<Record*> queue;
    bool stopping = false;
    std::atomic<std::uint32_t> use_counter{0};
    std::atomic<std::uint32_t> draws_last{0};
    std::atomic<std::uint32_t> dispatches_last{0};
    std::atomic<std::uint32_t> draws_frame{0};
    std::atomic<std::uint32_t> dispatches_frame{0};
    std::atomic<std::uint64_t> generation{1};

    CreateGraphicsFn create_graphics = nullptr;
    CreateComputeFn create_compute = nullptr;
    CreateStreamFn create_stream = nullptr;
    CreateLibraryFn create_library = nullptr;
    LoadGraphicsFn load_graphics = nullptr;
    LoadComputeFn load_compute = nullptr;
    LoadStreamFn load_stream = nullptr;
    SetPsoFn set_pso = nullptr;
    DrawInstancedFn draw_instanced = nullptr;
    DrawIndexedFn draw_indexed = nullptr;
    DispatchFn dispatch = nullptr;
    ExecuteIndirectFn execute_indirect = nullptr;
    bool library_hooked = false;

    std::atomic<bool> bindings{false};
    std::atomic<bool> bindings_early{false};
    CreateRootFn create_root = nullptr;
    CreateHeapFn create_heap = nullptr;
    SetHeapsFn set_heaps = nullptr;
    SetRootFn set_root = nullptr;
    SetTopologyFn set_topology = nullptr;
    std::atomic<bool> stream_output_roots{false};
    std::atomic<bool> geometry_capture{false};
    std::atomic<unsigned> stream_twins{0};
    std::atomic<unsigned> stream_draws_frame{0};
    std::atomic<unsigned> stream_draws_last{0};
    std::atomic<unsigned> stream_skipped_frame{0};
    std::atomic<unsigned> stream_skipped_last{0};
    std::atomic<unsigned> serialized{0};
    std::atomic<unsigned> serialized_extended{0};

    std::unordered_map<ID3D12RootSignature*, RootExtension> roots;
    std::unordered_map<ID3D12RootSignature*, bool> plain_roots;
    std::unordered_map<std::uint64_t, UINT> extended_blobs;
    std::vector<CountSample> count_samples;
    int count_offset = -1;
    bool count_uncalibratable = false;
    std::atomic<unsigned> roots_calibrated{0};
    std::unordered_map<ID3D12DescriptorHeap*, std::unique_ptr<HeapTail>> heaps;
    ID3D12Device* binding_device = nullptr;
    ID3D12Resource* constant_buffer = nullptr;
    void* constant_mapped = nullptr;
    std::atomic<std::uint64_t> constant_address{0};
    ID3D12Resource* textures[kBindingTextures]{};
    BridgerFxHandle texture_handles[kBindingTextures]{};
    std::atomic<unsigned> texture_generation{1};
    std::atomic<unsigned> roots_extended{0};
    std::atomic<unsigned> roots_skipped{0};
    std::atomic<unsigned> heaps_reserved{0};
    std::atomic<unsigned> cached_stripped{0};
    std::atomic<unsigned> bind_fallbacks{0};
    std::atomic<unsigned> fallback_root{0};
    std::atomic<unsigned> heaps_unreservable{0};
    std::atomic<unsigned> heaps_borrowed{0};
    std::atomic<unsigned> library_rebuilt{0};
    std::atomic<unsigned> root_calls{0};
    std::atomic<unsigned> roots_foreign{0};
    bool early_patched = false;
    void** patched_vtable = nullptr;
    std::unordered_map<ID3D12DescriptorHeap*, bool> unknown_heaps_logged;
    std::atomic<unsigned> fallback_heap{0};
    std::atomic<unsigned> fallback_constants{0};
    std::atomic<unsigned> root_sets{0};
    std::atomic<unsigned> root_sets_extended{0};
    std::atomic<unsigned> heap_sets{0};
    std::atomic<unsigned> heap_sets_found{0};
    std::atomic<unsigned> bound_draws{0};
    unsigned reported_frame = 0;
};

Globals& g() {
    static Globals instance;
    return instance;
}

thread_local ID3D12GraphicsCommandList* t_list = nullptr;
thread_local Record* t_record = nullptr;
thread_local bool t_twin_bound = false;
thread_local ID3D12GraphicsCommandList* t_root_list = nullptr;
thread_local bool t_root_extended = false;
thread_local RootExtension t_root{};

thread_local ID3D12GraphicsCommandList* t_heap_list = nullptr;
thread_local HeapTail* t_heap = nullptr;
thread_local ID3D12GraphicsCommandList* t_topology_list = nullptr;
thread_local D3D12_PRIMITIVE_TOPOLOGY t_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

struct PendingBlob {
    std::uint64_t id = 0;
    dxbc::Stage stage = dxbc::Stage::Unknown;
    std::vector<std::uint8_t> bytes;
    bool present = false;
};

void read_blob(const D3D12_SHADER_BYTECODE& code, dxbc::Stage stage, bool retain, PendingBlob& out) {
    out = {};
    if (code.pShaderBytecode == nullptr || code.BytecodeLength == 0 || code.BytecodeLength > (64u << 20)) return;
    out.present = true;
    out.stage = stage;
    out.id = dxbc::identity(code.pShaderBytecode, code.BytecodeLength);
    if (retain) {
        const auto* p = static_cast<const std::uint8_t*>(code.pShaderBytecode);
        out.bytes.assign(p, p + code.BytecodeLength);
    }
}

std::shared_ptr<Blob> intern(Globals& s, PendingBlob& pending) {
    if (!pending.present) return nullptr;
    if (auto found = s.blobs.find(pending.id); found != s.blobs.end()) return found->second;
    auto blob = std::make_shared<Blob>();
    blob->id = pending.id;
    blob->stage = pending.stage;
    blob->bytes = std::move(pending.bytes);
    s.retained_bytes += blob->bytes.size();
    s.blobs.emplace(pending.id, blob);
    return blob;
}

std::uint64_t pipeline_hash(const std::shared_ptr<Blob> stages[kStageCount], bool compute) {
    std::uint64_t h = compute ? 0xc0deull : 0x9f0cull;
    for (unsigned i = 0; i < kStageCount; ++i) {
        h = mix(h, stages[i] ? stages[i]->id : 0);
    }
    return h;
}

void register_record(Globals& s, ID3D12PipelineState* pso, ID3D12Device* device, std::unique_ptr<Record> fresh) {
    fresh->pso = pso;
    fresh->device = device;
    if (device != nullptr) device->AddRef();
    fresh->creation_order = ++s.creations;
    fresh->hash = pipeline_hash(fresh->stages, fresh->compute);
    Record* record = fresh.get();
    if (auto old = s.by_pso.find(pso); old != s.by_pso.end()) {
        old->second->stale = true;
    }
    s.by_pso[pso] = record;
    if (s.by_hash.find(record->hash) == s.by_hash.end()) s.by_hash[record->hash] = record;
    for (auto& h : s.hooks) {
        if (h->pipeline == record->hash) record->hooks.push_back(h.get());
    }
    record->hooked.store(!record->hooks.empty(), std::memory_order_release);
    s.records.push_back(std::move(fresh));
}

void record_graphics(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, void* created, bool library) {
    if (desc == nullptr || created == nullptr) return;
    auto& s = g();
    const bool retain = s.retain.load(std::memory_order_relaxed);
    PendingBlob pending[kStageCount];
    read_blob(desc->VS, dxbc::Stage::Vertex, retain, pending[BRIDGER_FX_STAGE_VS]);
    read_blob(desc->PS, dxbc::Stage::Pixel, retain, pending[BRIDGER_FX_STAGE_PS]);
    read_blob(desc->DS, dxbc::Stage::Domain, retain, pending[BRIDGER_FX_STAGE_DS]);
    read_blob(desc->HS, dxbc::Stage::Hull, retain, pending[BRIDGER_FX_STAGE_HS]);
    read_blob(desc->GS, dxbc::Stage::Geometry, retain, pending[BRIDGER_FX_STAGE_GS]);
    std::unique_ptr<GraphicsDesc> graphics;
    if (retain) {
        graphics = std::make_unique<GraphicsDesc>();
        graphics->capture(*desc);
    }
    auto fresh = std::make_unique<Record>();
    fresh->library = library;
    fresh->replaceable = retain;
    fresh->graphics = std::move(graphics);

    std::unique_lock lock(s.mutex);
    for (unsigned i = 0; i < kStageCount; ++i) fresh->stages[i] = intern(s, pending[i]);
    if (library) ++s.library_loaded;
    register_record(s, static_cast<ID3D12PipelineState*>(created), device, std::move(fresh));
}

void record_compute(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, void* created, bool library) {
    if (desc == nullptr || created == nullptr) return;
    auto& s = g();
    const bool retain = s.retain.load(std::memory_order_relaxed);
    PendingBlob pending;
    read_blob(desc->CS, dxbc::Stage::Compute, retain, pending);
    auto fresh = std::make_unique<Record>();
    fresh->compute = true;
    fresh->compute_desc = *desc;
    fresh->compute_desc.CachedPSO = {};
    if (fresh->compute_desc.pRootSignature != nullptr) fresh->compute_desc.pRootSignature->AddRef();
    fresh->replaceable = retain;
    fresh->library = library;

    std::unique_lock lock(s.mutex);
    fresh->stages[BRIDGER_FX_STAGE_CS] = intern(s, pending);
    if (library) ++s.library_loaded;
    register_record(s, static_cast<ID3D12PipelineState*>(created), device, std::move(fresh));
}

void record_stream(ID3D12Device* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, void* created, bool library) {
    if (desc == nullptr || desc->pPipelineStateSubobjectStream == nullptr || created == nullptr) return;
    auto& s = g();
    const bool retain = s.retain.load(std::memory_order_relaxed);
    PendingBlob pending[kStageCount];
    bool compute = false;
    const auto* base = static_cast<const std::uint8_t*>(desc->pPipelineStateSubobjectStream);
    const std::size_t total = desc->SizeInBytes;
    std::size_t at = 0;
    while (at + sizeof(UINT) <= total) {
        UINT type;
        std::memcpy(&type, base + at, sizeof type);
        std::size_t size = 0, align = 0;
        if (!subobject_shape(type, size, align)) break;
        const std::size_t data = (at + sizeof(UINT) + align - 1) & ~(align - 1);
        if (data + size > total) break;
        D3D12_SHADER_BYTECODE code{};
        switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: std::memcpy(&code, base + data, sizeof code); read_blob(code, dxbc::Stage::Vertex, retain, pending[BRIDGER_FX_STAGE_VS]); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: std::memcpy(&code, base + data, sizeof code); read_blob(code, dxbc::Stage::Pixel, retain, pending[BRIDGER_FX_STAGE_PS]); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: std::memcpy(&code, base + data, sizeof code); read_blob(code, dxbc::Stage::Domain, retain, pending[BRIDGER_FX_STAGE_DS]); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: std::memcpy(&code, base + data, sizeof code); read_blob(code, dxbc::Stage::Hull, retain, pending[BRIDGER_FX_STAGE_HS]); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: std::memcpy(&code, base + data, sizeof code); read_blob(code, dxbc::Stage::Geometry, retain, pending[BRIDGER_FX_STAGE_GS]); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: std::memcpy(&code, base + data, sizeof code); read_blob(code, dxbc::Stage::Compute, retain, pending[BRIDGER_FX_STAGE_CS]); compute = true; break;
            default: break;
        }
        at = (data + size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    }
    std::unique_ptr<StreamDesc> stream;
    if (retain) {
        stream = std::make_unique<StreamDesc>();
        stream->capture(*desc);
    }
    auto fresh = std::make_unique<Record>();
    fresh->stream_created = true;
    fresh->library = library;
    fresh->compute = compute;
    fresh->replaceable = stream != nullptr && stream->complete;
    fresh->stream = std::move(stream);

    std::unique_lock lock(s.mutex);
    for (unsigned i = 0; i < kStageCount; ++i) fresh->stages[i] = intern(s, pending[i]);
    ++s.stream_created;
    if (library) ++s.library_loaded;
    register_record(s, static_cast<ID3D12PipelineState*>(created), device, std::move(fresh));
}

void hook_library(ID3D12PipelineLibrary* library);
void report_fault(std::uint32_t fault, const char* where);

bool in_game_image(const void* address) {
    static const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    static const std::uintptr_t size = [] {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        return static_cast<std::uintptr_t>(nt->OptionalHeader.SizeOfImage);
    }();
    const auto a = reinterpret_cast<std::uintptr_t>(address);
    return a >= base && a < base + size;
}

std::string module_of(const void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) return "?";
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring w(path);
    const auto slash = w.find_last_of(L"\\/");
    w = slash == std::wstring::npos ? w : w.substr(slash + 1);
    return std::string(w.begin(), w.end());
}

void calibrate(Globals& s);

std::uint64_t blob_hash(const void* bytes, std::size_t size) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto* p = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

HRESULT STDMETHODCALLTYPE create_root_detour(ID3D12Device* device, UINT node, const void* blob, SIZE_T length, REFIID riid, void** out) {
    auto& s = g();
    s.root_calls.fetch_add(1, std::memory_order_relaxed);
    const auto hr = s.create_root(device, node, blob, length, riid, out);
    if (!s.bindings.load(std::memory_order_relaxed) || FAILED(hr) || out == nullptr || *out == nullptr) return hr;
    bool extended = false;
    UINT game_parameters = 0;
    if (blob != nullptr && length > 0) {
        std::shared_lock lock(s.mutex);
        if (auto it = s.extended_blobs.find(blob_hash(blob, length)); it != s.extended_blobs.end()) {
            extended = true;
            game_parameters = it->second;
        }
    }
    ID3D12RootSignature* root = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&root)))) {
        std::unique_lock lock(s.mutex);
        if (extended) {
            RootExtension ext;
            ext.game_parameters = game_parameters;
            ext.constants = game_parameters;
            ext.textures = game_parameters + 1;
            s.roots[root] = ext;
            s.count_samples.push_back({root, game_parameters + bindings::kBindingParameters});
            calibrate(s);
        } else {
            s.roots.erase(root);
        }
        s.plain_roots[root] = !extended;
        if (s.binding_device == nullptr) { s.binding_device = device; device->AddRef(); }
        lock.unlock();
        root->Release();
    }
    (extended ? s.roots_extended : s.roots_skipped).fetch_add(1, std::memory_order_relaxed);
    return hr;
}

HRESULT STDMETHODCALLTYPE create_heap_detour(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out) {
    auto& s = g();
    const auto hr = s.create_heap(device, desc, riid, out);
    if (desc != nullptr && s.bindings.load(std::memory_order_relaxed)) {
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1) < 24) {
            log::info("fx/pipeline: heap created: type {} flags {} descriptors {} -> {:#x}", static_cast<int>(desc->Type),
                      static_cast<int>(desc->Flags), desc->NumDescriptors, static_cast<unsigned>(hr));
        }
        if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
            ID3D12DescriptorHeap* heap = nullptr;
            if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&heap)))) {
                std::unique_lock lock(s.mutex);
                s.heaps.erase(heap);
                s.unknown_heaps_logged.erase(heap);
                lock.unlock();
                heap->Release();
            }
        }
    }
    return hr;
}

bool strip_cached_stream(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, std::vector<std::uint8_t>& copy) {
    if (desc == nullptr || desc->pPipelineStateSubobjectStream == nullptr) return false;
    const auto* base = static_cast<const std::uint8_t*>(desc->pPipelineStateSubobjectStream);
    const std::size_t total = desc->SizeInBytes;
    std::size_t at = 0;
    while (at + sizeof(UINT) <= total) {
        UINT type;
        std::memcpy(&type, base + at, sizeof type);
        std::size_t size = 0, align = 0;
        if (!subobject_shape(type, size, align)) return false;
        const std::size_t data = (at + sizeof(UINT) + align - 1) & ~(align - 1);
        if (data + size > total) return false;
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO) {
            D3D12_CACHED_PIPELINE_STATE cached;
            std::memcpy(&cached, base + data, sizeof cached);
            if (cached.pCachedBlob == nullptr || cached.CachedBlobSizeInBytes == 0) return false;
            copy.assign(base, base + total);
            const D3D12_CACHED_PIPELINE_STATE none{};
            std::memcpy(copy.data() + data, &none, sizeof none);
            return true;
        }
        at = (data + size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    }
    return false;
}

void STDMETHODCALLTYPE set_heaps_detour(ID3D12GraphicsCommandList* list, UINT count, ID3D12DescriptorHeap* const* heaps) {
    auto& s = g();
    if (s.bindings.load(std::memory_order_relaxed)) {
        HeapTail* found = nullptr;
        ID3D12DescriptorHeap* unknown = nullptr;
        {
            std::shared_lock lock(s.mutex);
            for (UINT i = 0; heaps != nullptr && i < count; ++i) {
                if (auto it = s.heaps.find(heaps[i]); it != s.heaps.end()) found = it->second.get();
                else if (heaps[i] != nullptr && !s.unknown_heaps_logged.count(heaps[i])) unknown = heaps[i];
            }
        }
        if (unknown != nullptr) {
            const auto d = unknown->GetDesc();
            std::unique_lock lock(s.mutex);
            if (!s.unknown_heaps_logged.count(unknown)) {
                s.unknown_heaps_logged[unknown] = true;
                const bool borrow = d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
                                 && (d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0
                                 && d.NumDescriptors >= 100000;
                if (borrow) {
                    ID3D12Device* device = nullptr;
                    if (SUCCEEDED(unknown->GetDevice(IID_PPV_ARGS(&device)))) {
                        auto tail = std::make_unique<HeapTail>();
                        tail->device = device;
                        const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                        const UINT base = d.NumDescriptors - kBindingTextures;
                        tail->cpu = unknown->GetCPUDescriptorHandleForHeapStart();
                        tail->cpu.ptr += static_cast<SIZE_T>(base) * step;
                        tail->gpu = unknown->GetGPUDescriptorHandleForHeapStart();
                        tail->gpu.ptr += static_cast<UINT64>(base) * step;
                        found = tail.get();
                        s.heaps[unknown] = std::move(tail);
                        s.heaps_borrowed.fetch_add(1, std::memory_order_relaxed);
                        if (s.binding_device == nullptr) { s.binding_device = device; device->AddRef(); }
                    }
                }
                if (s.unknown_heaps_logged.size() <= 16) {
                    log::info("fx/pipeline: game binds a heap the core did not reserve: {} type {} flags {} descriptors {}{}",
                              static_cast<void*>(unknown), static_cast<int>(d.Type), static_cast<int>(d.Flags), d.NumDescriptors,
                              borrow ? "; borrowing its last 4" : "");
                }
            }
        }
        t_heap_list = list;
        t_heap = found;
        s.heap_sets.fetch_add(1, std::memory_order_relaxed);
        if (found != nullptr) s.heap_sets_found.fetch_add(1, std::memory_order_relaxed);
    }
    s.set_heaps(list, count, heaps);
}

constexpr std::size_t kCalibrationSpan = 512;

bool read_u32(const void* at, std::uint32_t& out) {
    struct Ctx { const void* at; std::uint32_t value; } ctx{at, 0};
    if (guarded_call([](void* raw) { auto* c = static_cast<Ctx*>(raw); std::memcpy(&c->value, c->at, 4); }, &ctx) != 0) return false;
    out = ctx.value;
    return true;
}

void calibrate(Globals& s) {
    if (s.count_offset >= 0 || s.count_samples.size() < 3) return;
    for (std::size_t offset = 0; offset + 4 <= kCalibrationSpan; offset += 4) {
        bool all = true;
        for (const auto& sample : s.count_samples) {
            std::uint32_t value = 0;
            if (!read_u32(reinterpret_cast<const std::uint8_t*>(sample.root) + offset, value) || value != sample.total) { all = false; break; }
        }
        if (all) {
            s.count_offset = static_cast<int>(offset);
            log::info("fx/pipeline: root signature parameter count found at object offset {:#x} ({} samples)", offset, s.count_samples.size());
            return;
        }
    }
    if (s.count_samples.size() >= 8 && !s.count_uncalibratable) {
        s.count_uncalibratable = true;
        log::warn("fx/pipeline: no object offset holds the parameter count on all {} known root signatures; boot-created signatures stay unbound", s.count_samples.size());
    }
}

std::optional<RootExtension> extension_of(Globals& s, ID3D12RootSignature* root) {
    {
        std::shared_lock lock(s.mutex);
        if (auto it = s.roots.find(root); it != s.roots.end()) return it->second;
        if (auto plain = s.plain_roots.find(root); plain != s.plain_roots.end() && plain->second) return std::nullopt;
        if (s.count_offset < 0) return std::nullopt;
    }
    std::uint32_t total = 0;
    if (!read_u32(reinterpret_cast<const std::uint8_t*>(root) + s.count_offset, total)) return std::nullopt;
    if (total < bindings::kBindingParameters + 1 || total > 64 + bindings::kBindingParameters) {
        std::unique_lock lock(s.mutex);
        s.plain_roots[root] = true;
        return std::nullopt;
    }
    RootExtension ext;
    ext.game_parameters = total - bindings::kBindingParameters;
    ext.constants = ext.game_parameters;
    ext.textures = ext.game_parameters + 1;
    std::unique_lock lock(s.mutex);
    s.roots[root] = ext;
    s.roots_calibrated.fetch_add(1, std::memory_order_relaxed);
    return ext;
}

void STDMETHODCALLTYPE set_root_detour(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root) {
    auto& s = g();
    if (s.bindings.load(std::memory_order_relaxed)) {
        t_root_list = list;
        t_root_extended = false;
        if (in_game_image(_ReturnAddress())) {
            if (const auto ext = extension_of(s, root)) {
                t_root = *ext;
                t_root_extended = !ext->pixel_denied;
            }
        }
        s.root_sets.fetch_add(1, std::memory_order_relaxed);
        if (t_root_extended) s.root_sets_extended.fetch_add(1, std::memory_order_relaxed);
    }
    s.set_root(list, root);
}

void report_fault(std::uint32_t fault, const char* where) {
    if (fault == 0) return;
    static std::atomic<unsigned> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
        log::error("fx/pipeline: recording {} faulted ({:#x}) and was skipped: {}", where, fault, describe_last_fault());
    }
}

HRESULT STDMETHODCALLTYPE create_graphics_detour(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC uncached;
    if (g().bindings.load(std::memory_order_relaxed) && desc != nullptr && desc->CachedPSO.pCachedBlob != nullptr) {
        uncached = *desc;
        uncached.CachedPSO = {};
        desc = &uncached;
        g().cached_stripped.fetch_add(1, std::memory_order_relaxed);
    }
    const auto hr = g().create_graphics(device, desc, riid, out);
    if (FAILED(hr) && g().bindings.load(std::memory_order_relaxed)) {
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1) < 8) log::warn("fx/pipeline: the game's graphics pipeline creation failed ({:#x})", static_cast<unsigned>(hr));
    }
    if (SUCCEEDED(hr) && out != nullptr) {
        struct Ctx { ID3D12Device* device; const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc; void* created; } ctx{device, desc, *out};
        report_fault(guarded_call([](void* raw) { auto* c = static_cast<Ctx*>(raw); record_graphics(c->device, c->desc, c->created, false); }, &ctx), "CreateGraphicsPipelineState");
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE create_compute_detour(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC uncached;
    if (g().bindings.load(std::memory_order_relaxed) && desc != nullptr && desc->CachedPSO.pCachedBlob != nullptr) {
        uncached = *desc;
        uncached.CachedPSO = {};
        desc = &uncached;
        g().cached_stripped.fetch_add(1, std::memory_order_relaxed);
    }
    const auto hr = g().create_compute(device, desc, riid, out);
    if (FAILED(hr) && g().bindings.load(std::memory_order_relaxed)) {
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1) < 8) log::warn("fx/pipeline: the game's compute pipeline creation failed ({:#x})", static_cast<unsigned>(hr));
    }
    if (SUCCEEDED(hr) && out != nullptr) {
        struct Ctx { ID3D12Device* device; const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc; void* created; } ctx{device, desc, *out};
        report_fault(guarded_call([](void* raw) { auto* c = static_cast<Ctx*>(raw); record_compute(c->device, c->desc, c->created, false); }, &ctx), "CreateComputePipelineState");
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE create_stream_detour(ID3D12Device2* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out) {
    std::vector<std::uint8_t> copy;
    D3D12_PIPELINE_STATE_STREAM_DESC uncached;
    if (g().bindings.load(std::memory_order_relaxed)) {
        struct Ctx { const D3D12_PIPELINE_STATE_STREAM_DESC* desc; std::vector<std::uint8_t>* copy; bool stripped; } ctx{desc, &copy, false};
        guarded_call([](void* raw) { auto* c = static_cast<Ctx*>(raw); c->stripped = strip_cached_stream(c->desc, *c->copy); }, &ctx);
        if (ctx.stripped) {
            uncached = {copy.size(), copy.data()};
            desc = &uncached;
            g().cached_stripped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const auto hr = g().create_stream(device, desc, riid, out);
    if (FAILED(hr) && g().bindings.load(std::memory_order_relaxed)) {
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1) < 8) log::warn("fx/pipeline: the game's stream pipeline creation failed ({:#x})", static_cast<unsigned>(hr));
    }
    if (SUCCEEDED(hr) && out != nullptr) {
        struct Ctx { ID3D12Device* device; const D3D12_PIPELINE_STATE_STREAM_DESC* desc; void* created; } ctx{device, desc, *out};
        report_fault(guarded_call([](void* raw) { auto* c = static_cast<Ctx*>(raw); record_stream(c->device, c->desc, c->created, false); }, &ctx), "CreatePipelineState");
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE create_library_detour(ID3D12Device1* device, const void* blob, SIZE_T length, REFIID riid, void** out) {
    const auto hr = g().create_library(device, blob, length, riid, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        ID3D12PipelineLibrary* library = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&library)))) {
            hook_library(library);
            library->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE load_graphics_detour(ID3D12PipelineLibrary* library, LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    auto hr = g().load_graphics(library, name, desc, riid, out);
    if (FAILED(hr) && g().bindings.load(std::memory_order_relaxed) && desc != nullptr) {
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(library->GetDevice(IID_PPV_ARGS(&device)))) {
            hr = create_graphics_detour(device, desc, riid, out);
            device->Release();
            g().library_rebuilt.fetch_add(1, std::memory_order_relaxed);
            return hr;
        }
    }
    if (SUCCEEDED(hr) && out != nullptr) {
        ID3D12Device* device = nullptr;
        library->GetDevice(IID_PPV_ARGS(&device));
        struct Ctx { ID3D12Device* device; const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc; void* created; } ctx{device, desc, *out};
        report_fault(guarded_call([](void* raw) { auto* c = static_cast<Ctx*>(raw); record_graphics(c->device, c->desc, c->created, true); }, &ctx), "LoadGraphicsPipeline");
        if (device != nullptr) device->Release();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE load_compute_detour(ID3D12PipelineLibrary* library, LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    auto hr = g().load_compute(library, name, desc, riid, out);
    if (FAILED(hr) && g().bindings.load(std::memory_order_relaxed) && desc != nullptr) {
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(library->GetDevice(IID_PPV_ARGS(&device)))) {
            hr = create_compute_detour(device, desc, riid, out);
            device->Release();
            g().library_rebuilt.fetch_add(1, std::memory_order_relaxed);
            return hr;
        }
    }
    if (SUCCEEDED(hr) && out != nullptr) {
        ID3D12Device* device = nullptr;
        library->GetDevice(IID_PPV_ARGS(&device));
        struct Ctx { ID3D12Device* device; const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc; void* created; } ctx{device, desc, *out};
        report_fault(guarded_call([](void* raw) { auto* c = static_cast<Ctx*>(raw); record_compute(c->device, c->desc, c->created, true); }, &ctx), "LoadComputePipeline");
        if (device != nullptr) device->Release();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE load_stream_detour(ID3D12PipelineLibrary1* library, LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out) {
    auto hr = g().load_stream(library, name, desc, riid, out);
    if (FAILED(hr) && g().bindings.load(std::memory_order_relaxed) && desc != nullptr) {
        ID3D12Device2* device = nullptr;
        if (SUCCEEDED(library->GetDevice(IID_PPV_ARGS(&device)))) {
            hr = create_stream_detour(device, desc, riid, out);
            device->Release();
            g().library_rebuilt.fetch_add(1, std::memory_order_relaxed);
            return hr;
        }
    }
    if (SUCCEEDED(hr) && out != nullptr) {
        ID3D12Device* device = nullptr;
        library->GetDevice(IID_PPV_ARGS(&device));
        struct Ctx { ID3D12Device* device; const D3D12_PIPELINE_STATE_STREAM_DESC* desc; void* created; } ctx{device, desc, *out};
        report_fault(guarded_call([](void* raw) { auto* c = static_cast<Ctx*>(raw); record_stream(c->device, c->desc, c->created, true); }, &ctx), "LoadPipeline");
        if (device != nullptr) device->Release();
    }
    return hr;
}

bool patch_slot(void** vtable, int slot, void* detour, void** original) {
    DWORD protection = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &protection)) return false;
    *original = InterlockedExchangePointer(&vtable[slot], detour);
    VirtualProtect(&vtable[slot], sizeof(void*), protection, &protection);
    return true;
}

void hook_library(ID3D12PipelineLibrary* library) {
    auto& s = g();
    std::unique_lock lock(s.mutex);
    if (s.library_hooked || library == nullptr) return;
    void** vtable = *reinterpret_cast<void***>(library);
    bool ok = patch_slot(vtable, kLibraryLoadGraphics, reinterpret_cast<void*>(&load_graphics_detour), reinterpret_cast<void**>(&s.load_graphics));
    ok &= patch_slot(vtable, kLibraryLoadCompute, reinterpret_cast<void*>(&load_compute_detour), reinterpret_cast<void**>(&s.load_compute));
    ID3D12PipelineLibrary1* library1 = nullptr;
    if (SUCCEEDED(library->QueryInterface(IID_PPV_ARGS(&library1)))) {
        void** vtable1 = *reinterpret_cast<void***>(library1);
        ok &= patch_slot(vtable1, kLibraryLoadStream, reinterpret_cast<void*>(&load_stream_detour), reinterpret_cast<void**>(&s.load_stream));
        library1->Release();
    }
    s.library_hooked = ok;
    if (ok) {
        log::info("fx/pipeline: the game uses a pipeline library; its loads are recorded too");
    } else {
        log::warn("fx/pipeline: could not hook the pipeline library; pipelines loaded from it are invisible");
    }
}

const char* profile_for(BridgerFxShaderStage stage, unsigned minor) {
    static const char* const names[2][kStageCount] = {
        {"vs_5_0", "ps_5_0", "ds_5_0", "hs_5_0", "gs_5_0", "cs_5_0"},
        {"vs_5_1", "ps_5_1", "ds_5_1", "hs_5_1", "gs_5_1", "cs_5_1"},
    };
    return names[minor == 1 ? 1 : 0][stage];
}

bool compile_file(const std::filesystem::path& path, const std::string& entry, const char* profile,
                  const std::vector<std::string>& defines, std::vector<std::uint8_t>& out, std::string& problem) {
    std::vector<D3D_SHADER_MACRO> macros;
    std::vector<std::pair<std::string, std::string>> storage;
    for (const auto& d : defines) {
        const auto eq = d.find('=');
        storage.emplace_back(d.substr(0, eq), eq == std::string::npos ? "1" : d.substr(eq + 1));
    }
    for (const auto& [name, value] : storage) macros.push_back({name.c_str(), value.c_str()});
    macros.push_back({nullptr, nullptr});
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const auto hr = D3DCompileFromFile(path.wstring().c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                       entry.c_str(), profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors != nullptr) {
        problem.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        errors->Release();
    }
    if (FAILED(hr) || code == nullptr) {
        if (problem.empty()) problem = std::format("D3DCompileFromFile failed ({:#x}) for {}", static_cast<unsigned>(hr), path.string());
        return false;
    }
    problem.clear();
    const auto* p = static_cast<const std::uint8_t*>(code->GetBufferPointer());
    out.assign(p, p + code->GetBufferSize());
    code->Release();
    return true;
}

std::filesystem::file_time_type stamp_of(const std::filesystem::path& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

unsigned minor_of(const Blob* blob) {
    if (blob == nullptr || blob->bytes.empty()) return 0;
    return dxbc::summarise(blob->bytes.data(), blob->bytes.size()).minor;
}

void compile_replacement(Globals& s, Replacement& r) {
    const auto* record = s.by_hash.count(r.pipeline) ? s.by_hash[r.pipeline] : nullptr;
    const unsigned minor = record != nullptr ? minor_of(record->stages[r.stage].get()) : 0;
    std::vector<std::uint8_t> bytes;
    std::string problem;
    ++r.compiles;
    r.stamp = stamp_of(r.path);
    auto extension = r.path.extension().string();
    for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool loaded = false;
    if (extension == ".dxbc") {
        std::ifstream in(r.path, std::ios::binary);
        std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        dxbc::Container container;
        if (raw.empty() || !container.parse(raw.data(), raw.size())) {
            problem = std::format("{} is not a DXBC container", r.path.string());
        } else {
            bytes = container.build();
            loaded = true;
        }
    } else {
        loaded = compile_file(r.path, r.entry, profile_for(r.stage, minor), r.defines, bytes, problem);
    }
    if (loaded) {
        r.bytecode = std::move(bytes);
        r.problem.clear();
        ++r.generation;
        s.generation.fetch_add(1, std::memory_order_acq_rel);
        log::info("fx/pipeline: {} compiled {} for pipeline {:016x} stage {}", r.owner, r.path.filename().string(), r.pipeline, static_cast<int>(r.stage));
    } else {
        r.problem = problem;
        log::warn("fx/pipeline: {} failed to compile {}: {}", r.owner, r.path.filename().string(), problem);
    }
}

void compile_material(Globals& s, MaterialHook& m) {
    ++m.compiles;
    m.stamp = stamp_of(m.path);
    std::string problem;
    bool any = false;
    for (unsigned minor = 0; minor < 2; ++minor) {
        std::vector<std::uint8_t> bytes;
        std::string p;
        if (compile_file(m.path, m.entry, profile_for(BRIDGER_FX_STAGE_PS, minor), m.defines, bytes, p)) {
            m.compiled[minor] = std::move(bytes);
            any = true;
        } else {
            m.compiled[minor].clear();
            if (problem.empty()) problem = p;
        }
    }
    if (any) {
        m.problem.clear();
        ++m.generation;
        s.generation.fetch_add(1, std::memory_order_acq_rel);
        log::info("fx/pipeline: {} compiled material hook {}", m.owner, m.path.filename().string());
    } else {
        m.problem = problem;
        log::warn("fx/pipeline: {} failed to compile material hook {}: {}", m.owner, m.path.filename().string(), problem);
    }
}

void refresh_work(Globals& s) {
    const bool capture = s.stream_output_roots.load(std::memory_order_relaxed) && s.geometry_capture.load(std::memory_order_relaxed);
    s.work.store(!s.replacements.empty() || s.material != nullptr || capture, std::memory_order_release);
}

struct BuildInputs {
    std::uint64_t generation = 0;
    bool compute = false;
    bool replaceable = false;
    bool stream_created = false;
    ID3D12Device* device = nullptr;
    std::shared_ptr<Blob> stages[kStageCount];
    std::vector<std::uint8_t> effective[kStageCount];
    bool material_selected = false;
    bool root_extended = false;
    bool stream_output = false;
    std::vector<std::uint8_t> hook[2];
    std::unique_ptr<GraphicsDesc> graphics;
    std::unique_ptr<StreamDesc> stream;
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{};
};

void gather(Globals& s, const Record& r, BuildInputs& in) {
    std::shared_lock lock(s.mutex);
    in.generation = s.generation.load(std::memory_order_acquire);
    in.compute = r.compute;
    in.replaceable = r.replaceable;
    in.stream_created = r.stream_created;
    in.device = r.device;
    for (unsigned i = 0; i < kStageCount; ++i) in.stages[i] = r.stages[i];
    for (const auto& rep : s.replacements) {
        if (rep->pipeline == r.hash && !rep->bytecode.empty()) in.effective[rep->stage] = rep->bytecode;
    }
    const auto targets_info = targets_of(r);
    if (s.material != nullptr && !r.compute && r.stages[BRIDGER_FX_STAGE_PS] && targets_info.known) {
        const auto& m = *s.material;
        const auto targets = targets_info.count;
        in.material_selected = (m.only == 0 || m.only == r.hash) && targets >= m.min_targets
                            && (m.max_targets == 0 || targets <= m.max_targets);
        if (in.material_selected) {
            in.hook[0] = m.compiled[0];
            in.hook[1] = m.compiled[1];
        }
    }
    ID3D12RootSignature* root = r.graphics != nullptr ? r.graphics->desc.pRootSignature : r.stream != nullptr ? r.stream->root : nullptr;
    {
        const auto plain = s.plain_roots.find(root);
        in.root_extended = root != nullptr && s.bindings.load(std::memory_order_relaxed)
                        && (plain == s.plain_roots.end() || !plain->second);
    }
    in.stream_output = s.stream_output_roots.load(std::memory_order_relaxed)
                    && s.geometry_capture.load(std::memory_order_relaxed)
                    && !r.compute && r.stream != nullptr && r.stream->complete && in.root_extended
                    && r.stages[BRIDGER_FX_STAGE_VS] && r.stages[BRIDGER_FX_STAGE_PS]
                    && !r.stages[BRIDGER_FX_STAGE_HS] && !r.stages[BRIDGER_FX_STAGE_DS] && !r.stages[BRIDGER_FX_STAGE_GS]
                    && targets_info.known && targets_info.count >= 2
                    && targets_info.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (r.graphics != nullptr) {
        in.graphics = std::make_unique<GraphicsDesc>();
        in.graphics->capture(r.graphics->desc);
    }
    if (r.stream != nullptr) in.stream = std::make_unique<StreamDesc>(*r.stream);
    in.compute_desc = r.compute_desc;
}

struct BuildOutput {
    ID3D12PipelineState* twin = nullptr;
    bool changed = false;
    bool failed = false;
    bool material_applied = false;
    bool material_skipped = false;
    bool uses_bindings = false;
    bool stream_output = false;
    std::string problem;
};

void build(Globals& s, BuildInputs& in, BuildOutput& out) {
    const auto* ps_blob = in.stages[BRIDGER_FX_STAGE_PS].get();
    for (unsigned i = 0; i < kStageCount; ++i) if (!in.effective[i].empty()) out.changed = true;
    if (in.material_selected && ps_blob != nullptr) {
        const auto& host = in.effective[BRIDGER_FX_STAGE_PS].empty() ? ps_blob->bytes : in.effective[BRIDGER_FX_STAGE_PS];
        const unsigned minor = host.empty() ? 0 : dxbc::summarise(host.data(), host.size()).minor;
        const auto& hook = in.hook[minor == 1 ? 1 : 0];
        if (host.empty()) {
            out.problem = "shader bytecode was not retained";
            out.material_skipped = true;
        } else if (hook.empty()) {
            out.problem = "the material hook did not compile for this host's profile";
            out.material_skipped = true;
        } else {
            const Blob* upstream = in.stages[BRIDGER_FX_STAGE_DS] ? in.stages[BRIDGER_FX_STAGE_DS].get()
                                 : in.stages[BRIDGER_FX_STAGE_GS] ? in.stages[BRIDGER_FX_STAGE_GS].get()
                                 : in.stages[BRIDGER_FX_STAGE_VS].get();
            const auto spliced = dxbc::splice_pixel_shader(host.data(), host.size(), hook.data(), hook.size(),
                                                           upstream ? upstream->bytes.data() : nullptr,
                                                           upstream ? upstream->bytes.size() : 0);
            if (spliced.error.empty() && spliced.bindings && !in.root_extended) {
                out.problem = "material hook reads space 60 but this pipeline's root signature was not extended";
                out.material_skipped = true;
            } else if (spliced.error.empty()) {
                in.effective[BRIDGER_FX_STAGE_PS] = spliced.bytecode;
                out.material_applied = true;
                out.uses_bindings = spliced.bindings;
                out.changed = true;
            } else {
                out.problem = "material hook: " + spliced.error;
                out.material_skipped = true;
            }
        }
    }
    if (in.stream_output && in.stream != nullptr) {
        in.stream->add_stream_output();
        out.stream_output = true;
        out.changed = true;
    }
    if (!out.changed) return;
    if (!in.replaceable || in.device == nullptr) {
        out.problem = in.stream_created ? "the pipeline stream held a subobject this build does not know" : "description not retained";
        out.failed = true;
        out.changed = false;
        return;
    }
    ID3D12PipelineState* twin = nullptr;
    HRESULT hr = E_FAIL;
    if (in.stream != nullptr) {
        in.stream->relink();
        for (unsigned i = 0; i < kStageCount; ++i) {
            if (!in.effective[i].empty()) in.stream->set_shader(i, in.effective[i]);
            else if (in.stages[i]) in.stream->set_shader(i, in.stages[i]->bytes);
        }
        ID3D12Device2* device2 = nullptr;
        if (g().create_stream == nullptr || FAILED(in.device->QueryInterface(IID_PPV_ARGS(&device2)))) {
            out.failed = true;
            out.material_applied = false;
            out.problem = "the device has no CreatePipelineState";
            return;
        }
        auto desc = in.stream->desc();
        struct Ctx { ID3D12Device2* device; D3D12_PIPELINE_STATE_STREAM_DESC* desc; ID3D12PipelineState** out; HRESULT hr; } ctx{device2, &desc, &twin, E_FAIL};
        guarded_call([](void* raw) { auto* c = static_cast<Ctx*>(raw); c->hr = g().create_stream(c->device, c->desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(c->out)); }, &ctx);
        hr = ctx.hr;
        device2->Release();
    } else if (in.compute) {
        auto desc = in.compute_desc;
        const auto& cs = in.effective[BRIDGER_FX_STAGE_CS].empty() ? in.stages[BRIDGER_FX_STAGE_CS]->bytes : in.effective[BRIDGER_FX_STAGE_CS];
        desc.CS = {cs.data(), cs.size()};
        struct Ctx { ID3D12Device* device; D3D12_COMPUTE_PIPELINE_STATE_DESC* desc; ID3D12PipelineState** out; HRESULT hr; } ctx{in.device, &desc, &twin, E_FAIL};
        guarded_call([](void* raw) { auto* c = static_cast<Ctx*>(raw); c->hr = g().create_compute(c->device, c->desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(c->out)); }, &ctx);
        hr = ctx.hr;
    } else if (in.graphics != nullptr) {
        in.graphics->relink();
        auto desc = in.graphics->desc;
        D3D12_SHADER_BYTECODE* slots[kStageCount] = {&desc.VS, &desc.PS, &desc.DS, &desc.HS, &desc.GS, nullptr};
        for (unsigned i = 0; i < kStageCount - 1; ++i) {
            if (!in.effective[i].empty()) *slots[i] = {in.effective[i].data(), in.effective[i].size()};
            else if (in.stages[i]) *slots[i] = {in.stages[i]->bytes.data(), in.stages[i]->bytes.size()};
            else *slots[i] = {};
        }
        struct Ctx { ID3D12Device* device; D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc; ID3D12PipelineState** out; HRESULT hr; } ctx{in.device, &desc, &twin, E_FAIL};
        guarded_call([](void* raw) { auto* c = static_cast<Ctx*>(raw); c->hr = g().create_graphics(c->device, c->desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(c->out)); }, &ctx);
        hr = ctx.hr;
    }
    if (FAILED(hr) || twin == nullptr) {
        out.failed = true;
        out.material_applied = false;
        out.problem = std::format("the runtime refused the twin pipeline ({:#x}){}", static_cast<unsigned>(hr),
                                  out.problem.empty() ? "" : "; " + out.problem);
        (void)s;
        return;
    }
    out.twin = twin;
}

void publish(Globals& s, Record& r, const BuildInputs& in, BuildOutput& out) {
    std::unique_lock lock(s.mutex);
    if (s.generation.load(std::memory_order_acquire) != in.generation) {
        if (out.twin != nullptr) s.retired.push_back({out.twin, s.frame});
        r.queued.store(false, std::memory_order_release);
        return;
    }
    ID3D12PipelineState* previous = r.substitute.exchange(out.twin, std::memory_order_acq_rel);
    if (previous != nullptr) s.retired.push_back({previous, s.frame});
    r.build_failed = out.failed;
    r.material_applied = out.material_applied;
    r.material_skipped = out.material_skipped;
    r.uses_bindings.store(out.twin != nullptr && out.uses_bindings, std::memory_order_release);
    const bool streams = out.twin != nullptr && out.stream_output;
    if (r.stream_output.exchange(streams, std::memory_order_acq_rel) != streams) {
        if (streams) s.stream_twins.fetch_add(1, std::memory_order_relaxed);
        else s.stream_twins.fetch_sub(1, std::memory_order_relaxed);
    }
    r.problem = out.problem;
    if (out.failed) log::warn("fx/pipeline: pipeline {:016x}: {}", r.hash, out.problem);
    r.built_from.store(in.generation, std::memory_order_release);
    r.queued.store(false, std::memory_order_release);
}

void build_one(Globals& s, Record* record) {
    BuildInputs in;
    gather(s, *record, in);
    BuildOutput out;
    build(s, in, out);
    publish(s, *record, in, out);
}

void builder_main() {
    auto& s = g();
    for (;;) {
        Record* record = nullptr;
        {
            std::unique_lock lock(s.queue_mutex);
            s.queue_signal.wait(lock, [&] { return s.stopping || !s.queue.empty(); });
            if (s.stopping) return;
            record = s.queue.front();
            s.queue.pop_front();
        }
        struct Ctx { Globals* s; Record* record; } ctx{&s, record};
        if (guarded_call([](void* raw) { auto* c = static_cast<Ctx*>(raw); build_one(*c->s, c->record); }, &ctx) != 0) {
            log::error("fx/pipeline: building a twin for pipeline {:016x} faulted: {}", record->hash, describe_last_fault());
            std::unique_lock lock(s.mutex);
            record->build_failed = true;
            record->problem = "building the twin faulted";
            record->built_from.store(s.generation.load(std::memory_order_acquire), std::memory_order_release);
            record->queued.store(false, std::memory_order_release);
        }
    }
}

void enqueue(Globals& s, Record* record) {
    {
        std::scoped_lock lock(s.queue_mutex);
        s.queue.push_back(record);
    }
    s.queue_signal.notify_one();
}

void STDMETHODCALLTYPE set_pso_detour(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso) {
    auto& s = g();
    if (!s.enabled.load(std::memory_order_relaxed) || pso == nullptr) {
        t_list = list;
        t_record = nullptr;
        t_twin_bound = false;
        s.set_pso(list, pso);
        return;
    }
    Record* record = nullptr;
    {
        std::shared_lock lock(s.mutex);
        if (auto found = s.by_pso.find(pso); found != s.by_pso.end()) record = found->second;
    }
    t_list = list;
    t_record = record;
    ID3D12PipelineState* use = pso;
    if (record != nullptr && !record->stale) {
        auto* twin = record->substitute.load(std::memory_order_acquire);
        if (s.work.load(std::memory_order_relaxed) || twin != nullptr) {
            const auto generation = s.generation.load(std::memory_order_acquire);
            if (record->built_from.load(std::memory_order_acquire) != generation
                && !record->queued.exchange(true, std::memory_order_acq_rel)) {
                enqueue(s, record);
            }
        }
        if (twin != nullptr) use = twin;
    }
    t_twin_bound = use != pso;
    s.set_pso(list, use);
}

struct HookCall {
    HookEntry* entry;
    const BridgerFxDraw* draw;
    bool before;
    bool proceed;
};

void run_hook(void* raw) {
    auto* c = static_cast<HookCall*>(raw);
    const auto fn = c->before ? c->entry->before : c->entry->after;
    if (fn != nullptr) {
        const bool r = fn(c->draw, c->entry->user);
        if (c->before) c->proceed = r;
    }
}

template <typename Original>
void dispatch_draw(Record* record, const BridgerFxDraw& draw, Original&& original) {
    auto& s = g();
    if (record == nullptr || record->stale) { original(); return; }
    if (draw.kind == BRIDGER_FX_DRAW_DISPATCH) {
        record->dispatches_frame.fetch_add(1, std::memory_order_relaxed);
        s.dispatches_frame.fetch_add(1, std::memory_order_relaxed);
    } else {
        s.draws_frame.fetch_add(1, std::memory_order_relaxed);
    }
    if (record->draws_frame.fetch_add(1, std::memory_order_relaxed) == 0) {
        record->first_use.store(s.use_counter.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    record->draws_total.fetch_add(1, std::memory_order_relaxed);
    if (!record->hooked.load(std::memory_order_acquire)) { original(); return; }

    HookEntry* entries[8];
    unsigned count = 0;
    {
        std::shared_lock lock(s.mutex);
        for (auto* h : record->hooks) {
            if (count < 8 && h->enabled.load(std::memory_order_relaxed)) entries[count++] = h;
        }
    }
    bool proceed = true;
    for (unsigned i = 0; i < count; ++i) {
        if (entries[i]->before == nullptr) continue;
        HookCall call{entries[i], &draw, true, true};
        if (guarded_call(run_hook, &call) != 0) {
            entries[i]->enabled.store(false, std::memory_order_relaxed);
            entries[i]->faults.fetch_add(1, std::memory_order_relaxed);
            log::error("fx/pipeline: draw hook of {} faulted on pipeline {:016x} and was disabled: {}", entries[i]->owner, record->hash, describe_last_fault());
            continue;
        }
        proceed &= call.proceed;
    }
    if (proceed) original();
    for (unsigned i = 0; i < count; ++i) {
        if (entries[i]->after == nullptr || !entries[i]->enabled.load(std::memory_order_relaxed)) continue;
        HookCall call{entries[i], &draw, false, true};
        if (guarded_call(run_hook, &call) != 0) {
            entries[i]->enabled.store(false, std::memory_order_relaxed);
            entries[i]->faults.fetch_add(1, std::memory_order_relaxed);
            log::error("fx/pipeline: draw hook of {} faulted on pipeline {:016x} and was disabled: {}", entries[i]->owner, record->hash, describe_last_fault());
        }
    }
}

Record* current(ID3D12GraphicsCommandList* list) {
    return t_list == list ? t_record : nullptr;
}

void refresh_tail(Globals& s, HeapTail& tail) {
    const auto generation = s.texture_generation.load(std::memory_order_acquire);
    if (tail.written.load(std::memory_order_acquire) == generation) return;
    std::scoped_lock write(tail.write);
    if (tail.written.load(std::memory_order_acquire) == generation) return;
    ID3D12Resource* resources[kBindingTextures]{};
    {
        std::shared_lock lock(s.mutex);
        for (UINT i = 0; i < kBindingTextures; ++i) {
            resources[i] = s.textures[i];
            if (resources[i] != nullptr) resources[i]->AddRef();
        }
    }
    const UINT step = tail.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT i = 0; i < kBindingTextures; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE at{tail.cpu.ptr + static_cast<SIZE_T>(i) * step};
        if (resources[i] != nullptr) {
            tail.device->CreateShaderResourceView(resources[i], nullptr, at);
            resources[i]->Release();
        } else {
            D3D12_SHADER_RESOURCE_VIEW_DESC none{};
            none.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            none.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            none.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            none.Texture2D.MipLevels = 1;
            tail.device->CreateShaderResourceView(nullptr, &none, at);
        }
    }
    tail.written.store(generation, std::memory_order_release);
}

void prepare_draw(ID3D12GraphicsCommandList* list, Record* record) {
    if (record == nullptr || !t_twin_bound || !record->uses_bindings.load(std::memory_order_relaxed)) return;
    auto& s = g();
    const auto address = s.constant_address.load(std::memory_order_acquire);
    const bool root_ok = t_root_list == list && t_root_extended;
    const bool heap_ok = t_heap_list == list && t_heap != nullptr;
    if (!root_ok || !heap_ok || address == 0) {
        if (!root_ok) s.fallback_root.fetch_add(1, std::memory_order_relaxed);
        if (!heap_ok) s.fallback_heap.fetch_add(1, std::memory_order_relaxed);
        if (address == 0) s.fallback_constants.fetch_add(1, std::memory_order_relaxed);
        s.set_pso(list, record->pso);
        t_twin_bound = false;
        s.bind_fallbacks.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    refresh_tail(s, *t_heap);
    list->SetGraphicsRootConstantBufferView(t_root.constants, address);
    list->SetGraphicsRootDescriptorTable(t_root.textures, t_heap->gpu);
    s.bound_draws.fetch_add(1, std::memory_order_relaxed);
}

void STDMETHODCALLTYPE set_topology_detour(ID3D12GraphicsCommandList* list, D3D12_PRIMITIVE_TOPOLOGY topology) {
    t_topology_list = list;
    t_topology = topology;
    g().set_topology(list, topology);
}

std::uint64_t stream_vertices(ID3D12GraphicsCommandList* list, UINT count, UINT instances) {
    if (t_topology_list != list || instances == 0 || instances > 65536) return 0;
    std::uint64_t triangles = 0;
    switch (t_topology) {
        case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: triangles = count / 3; break;
        case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: triangles = count >= 3 ? count - 2 : 0; break;
        default: return 0;
    }
    return triangles * 3 * instances;
}

void prepare_stream_out(ID3D12GraphicsCommandList* list, Record* record, UINT count, UINT instances) {
    if (record == nullptr || !t_twin_bound || !record->stream_output.load(std::memory_order_relaxed)) return;
    auto& s = g();
    rt::Region region;
    const auto vertices = stream_vertices(list, count, instances);
    if (vertices != 0 && rt::allocate_region(vertices, record->hash, region)) {
        const D3D12_STREAM_OUTPUT_BUFFER_VIEW view{region.buffer, region.size, region.counter};
        list->SOSetTargets(0, 1, &view);
        s.stream_draws_frame.fetch_add(1, std::memory_order_relaxed);
    } else {
        const D3D12_STREAM_OUTPUT_BUFFER_VIEW none{};
        list->SOSetTargets(0, 1, &none);
        s.stream_skipped_frame.fetch_add(1, std::memory_order_relaxed);
    }
}

void STDMETHODCALLTYPE draw_instanced_detour(ID3D12GraphicsCommandList* list, UINT vertices, UINT instances, UINT start_vertex, UINT start_instance) {
    auto& s = g();
    auto* record = s.enabled.load(std::memory_order_relaxed) ? current(list) : nullptr;
    BridgerFxDraw draw{record ? record->hash : 0, list, BRIDGER_FX_DRAW_INSTANCED, vertices, instances, {0, 0, 0}};
    prepare_draw(list, record);
    prepare_stream_out(list, record, vertices, instances);
    dispatch_draw(record, draw, [&] { s.draw_instanced(list, vertices, instances, start_vertex, start_instance); });
}

void STDMETHODCALLTYPE draw_indexed_detour(ID3D12GraphicsCommandList* list, UINT indices, UINT instances, UINT start_index, INT base_vertex, UINT start_instance) {
    auto& s = g();
    auto* record = s.enabled.load(std::memory_order_relaxed) ? current(list) : nullptr;
    BridgerFxDraw draw{record ? record->hash : 0, list, BRIDGER_FX_DRAW_INDEXED, indices, instances, {0, 0, 0}};
    prepare_draw(list, record);
    prepare_stream_out(list, record, indices, instances);
    dispatch_draw(record, draw, [&] { s.draw_indexed(list, indices, instances, start_index, base_vertex, start_instance); });
}

void STDMETHODCALLTYPE dispatch_detour(ID3D12GraphicsCommandList* list, UINT x, UINT y, UINT z) {
    auto& s = g();
    auto* record = s.enabled.load(std::memory_order_relaxed) ? current(list) : nullptr;
    BridgerFxDraw draw{record ? record->hash : 0, list, BRIDGER_FX_DRAW_DISPATCH, 0, 1, {x, y, z}};
    dispatch_draw(record, draw, [&] { s.dispatch(list, x, y, z); });
}

void STDMETHODCALLTYPE execute_indirect_detour(ID3D12GraphicsCommandList* list, ID3D12CommandSignature* signature, UINT max_count, ID3D12Resource* args, UINT64 args_offset, ID3D12Resource* count, UINT64 count_offset) {
    auto& s = g();
    auto* record = s.enabled.load(std::memory_order_relaxed) ? current(list) : nullptr;
    BridgerFxDraw draw{record ? record->hash : 0, list, BRIDGER_FX_DRAW_INDIRECT, 0, max_count, {0, 0, 0}};
    prepare_draw(list, record);
    prepare_stream_out(list, record, 0, 0);
    dispatch_draw(record, draw, [&] { s.execute_indirect(list, signature, max_count, args, args_offset, count, count_offset); });
}

std::string owner_of(const void* caller) {
    auto owner = loader::owner_of_address(caller);
    if (owner.empty()) owner = loader::Registry::instance().active_mod();
    return owner;
}

std::filesystem::path mod_relative(const std::string& owner, const char* path) {
    std::filesystem::path p = path != nullptr ? path : "";
    if (p.is_absolute()) return p;
    for (const auto& mod : loader::Registry::instance().mods()) {
        if (mod.id == owner) return mod.directory / p;
    }
    return p;
}

std::size_t copy_out(const std::string& value, char* buffer, std::size_t capacity) {
    if (buffer != nullptr && capacity > 0) {
        const auto n = std::min(capacity - 1, value.size());
        std::memcpy(buffer, value.data(), n);
        buffer[n] = 0;
    }
    return value.size();
}

void fill_info(const Record& r, BridgerFxPipelineInfo& out) {
    out = {};
    out.hash = r.hash;
    for (unsigned i = 0; i < kStageCount; ++i) out.stage_hash[i] = r.stages[i] ? r.stages[i]->id : 0;
    out.compute = r.compute ? 1 : 0;
    out.draws_last_frame = r.draws_last.load(std::memory_order_relaxed);
    out.draws_total = r.draws_total.load(std::memory_order_relaxed);
    out.first_use = r.first_use_last.load(std::memory_order_relaxed);
    const auto t = targets_of(r);
    out.render_targets = t.count;
    for (unsigned i = 0; i < 8; ++i) out.rtv_format[i] = t.rtv[i];
    out.dsv_format = t.dsv;
    out.topology = t.topology;
    out.replaced = r.substitute.load(std::memory_order_relaxed) != nullptr ? 1 : 0;
    out.hooked = r.hooked.load(std::memory_order_relaxed) ? 1 : 0;
    out.replaceable = r.replaceable ? 1 : 0;
}

void refresh_hooked(Record& r) {
    r.hooked.store(!r.hooks.empty(), std::memory_order_release);
}

}

namespace {

using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
using SerializeVersionedFn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
SerializeFn g_serialize = nullptr;
SerializeVersionedFn g_serialize_versioned = nullptr;
thread_local bool t_in_serialize = false;

void extend_blob(ID3DBlob** blob, const void* caller) {
    auto& s = g();
    if (blob == nullptr || *blob == nullptr || t_in_serialize || !s.bindings.load(std::memory_order_relaxed)) return;
    s.serialized.fetch_add(1, std::memory_order_relaxed);
    if (!in_game_image(caller)) return;
    std::vector<std::uint8_t> extended;
    bindings::RootExtension ext;
    std::string why;
    t_in_serialize = true;
    const bool ok = bindings::extend_root((*blob)->GetBufferPointer(), (*blob)->GetBufferSize(), extended, ext, why,
                                          s.stream_output_roots.load(std::memory_order_relaxed));
    t_in_serialize = false;
    if (!ok) {
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1) < 8) log::warn("fx/pipeline: a root signature of the game's could not be extended ({}); its draws will not bind", why);
        return;
    }
    ID3DBlob* fresh = nullptr;
    if (FAILED(D3DCreateBlob(extended.size(), &fresh)) || fresh == nullptr) return;
    std::memcpy(fresh->GetBufferPointer(), extended.data(), extended.size());
    {
        std::unique_lock lock(s.mutex);
        s.extended_blobs[blob_hash(extended.data(), extended.size())] = ext.game_parameters;
    }
    (*blob)->Release();
    *blob = fresh;
    s.serialized_extended.fetch_add(1, std::memory_order_relaxed);
}

HRESULT WINAPI serialize_detour(const D3D12_ROOT_SIGNATURE_DESC* desc, D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob, ID3DBlob** errors) {
    const auto hr = g_serialize(desc, version, blob, errors);
    if (SUCCEEDED(hr)) extend_blob(blob, _ReturnAddress());
    return hr;
}

HRESULT WINAPI serialize_versioned_detour(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc, ID3DBlob** blob, ID3DBlob** errors) {
    const auto hr = g_serialize_versioned(desc, blob, errors);
    if (SUCCEEDED(hr)) extend_blob(blob, _ReturnAddress());
    return hr;
}

}

void watch_device_creation() {
    auto& s = g();
    if (!s.bindings.load() || !s.bindings_early.load()) return;
    if (GetModuleHandleW(L"d3d12.dll") == nullptr) LoadLibraryW(L"d3d12.dll");
    if (MH_Initialize() == MH_ERROR_NOT_INITIALIZED) return;
    void* a = nullptr;
    void* b = nullptr;
    bool ok = MH_CreateHookApiEx(L"d3d12", "D3D12SerializeRootSignature", reinterpret_cast<void*>(&serialize_detour),
                                 reinterpret_cast<void**>(&g_serialize), &a) == MH_OK && MH_EnableHook(a) == MH_OK;
    ok &= MH_CreateHookApiEx(L"d3d12", "D3D12SerializeVersionedRootSignature", reinterpret_cast<void*>(&serialize_versioned_detour),
                             reinterpret_cast<void**>(&g_serialize_versioned), &b) == MH_OK && MH_EnableHook(b) == MH_OK;
    log::info("fx/pipeline: root signature serialisation {} for material bindings", ok ? "hooked" : "could not be hooked");
}

void wait_for_device(unsigned) {}

bool prepare(void** device_vtable, void** command_list_vtable) {
    auto& s = g();
    if (device_vtable == nullptr || command_list_vtable == nullptr) return false;
    if (!s.enabled.load()) {
        s.problem = "disabled by fx.pipeline_hooks in bridger.json";
        log::info("fx/pipeline: hooks not installed ({})", s.problem);
        return false;
    }
    bool ok = true;
    const bool device_done = s.early_patched;
    if (!device_done) {
        ok &= MH_CreateHook(device_vtable[kDeviceCreateGraphics], reinterpret_cast<void*>(&create_graphics_detour), reinterpret_cast<void**>(&s.create_graphics)) == MH_OK;
        ok &= MH_CreateHook(device_vtable[kDeviceCreateCompute], reinterpret_cast<void*>(&create_compute_detour), reinterpret_cast<void**>(&s.create_compute)) == MH_OK;
    }
    ok &= MH_CreateHook(command_list_vtable[kListSetPipelineState], reinterpret_cast<void*>(&set_pso_detour), reinterpret_cast<void**>(&s.set_pso)) == MH_OK;
    ok &= MH_CreateHook(command_list_vtable[kListDrawInstanced], reinterpret_cast<void*>(&draw_instanced_detour), reinterpret_cast<void**>(&s.draw_instanced)) == MH_OK;
    ok &= MH_CreateHook(command_list_vtable[kListDrawIndexed], reinterpret_cast<void*>(&draw_indexed_detour), reinterpret_cast<void**>(&s.draw_indexed)) == MH_OK;
    ok &= MH_CreateHook(command_list_vtable[kListDispatch], reinterpret_cast<void*>(&dispatch_detour), reinterpret_cast<void**>(&s.dispatch)) == MH_OK;
    ok &= MH_CreateHook(command_list_vtable[kListExecuteIndirect], reinterpret_cast<void*>(&execute_indirect_detour), reinterpret_cast<void**>(&s.execute_indirect)) == MH_OK;
    ok &= MH_CreateHook(command_list_vtable[kListSetTopology], reinterpret_cast<void*>(&set_topology_detour), reinterpret_cast<void**>(&s.set_topology)) == MH_OK;
    if (!device_done && !IsBadReadPtr(device_vtable + kDeviceCreateStream, sizeof(void*))) {
        if (MH_CreateHook(device_vtable[kDeviceCreateStream], reinterpret_cast<void*>(&create_stream_detour), reinterpret_cast<void**>(&s.create_stream)) != MH_OK) {
            log::warn("fx/pipeline: CreatePipelineState could not be hooked; stream-created pipelines are invisible");
        }
        if (MH_CreateHook(device_vtable[kDeviceCreateLibrary], reinterpret_cast<void*>(&create_library_detour), reinterpret_cast<void**>(&s.create_library)) != MH_OK) {
            log::warn("fx/pipeline: CreatePipelineLibrary could not be hooked; library loads are invisible");
        }
    }
    if (ok && s.bindings.load()) {
        bool bound = s.early_patched;
        if (!bound) {
            bound = MH_CreateHook(device_vtable[kDeviceCreateRoot], reinterpret_cast<void*>(&create_root_detour), reinterpret_cast<void**>(&s.create_root)) == MH_OK;
            bound &= MH_CreateHook(device_vtable[kDeviceCreateHeap], reinterpret_cast<void*>(&create_heap_detour), reinterpret_cast<void**>(&s.create_heap)) == MH_OK;
        }
        bound &= MH_CreateHook(command_list_vtable[kListSetDescriptorHeaps], reinterpret_cast<void*>(&set_heaps_detour), reinterpret_cast<void**>(&s.set_heaps)) == MH_OK;
        bound &= MH_CreateHook(command_list_vtable[kListSetGraphicsRoot], reinterpret_cast<void*>(&set_root_detour), reinterpret_cast<void**>(&s.set_root)) == MH_OK;
        if (bound) {
            log::info("fx/pipeline: material bindings on: root signatures gain register space 60, heaps a {}-descriptor tail", kBindingTextures);
        } else {
            s.bindings.store(false);
            log::error("fx/pipeline: material bindings could not be hooked and are off");
        }
    }
    s.hooks_installed = ok;
    if (!ok) {
        s.problem = "the pipeline hooks could not be created";
        log::error("fx/pipeline: {}", s.problem);
    }
    if (ok && !s.builder.joinable()) {
        s.builder = std::thread(builder_main);
    }
    return ok;
}

void configure(const std::filesystem::path& root) {
    g().dumps = root / "dumps" / "pipelines";
}

void end_frame(unsigned frame) {
    auto& s = g();
    ID3D12Resource* resolved[kBindingTextures]{};
    BridgerFxHandle wanted[kBindingTextures]{};
    if (s.bindings.load(std::memory_order_relaxed)) {
        {
            std::shared_lock read(s.mutex);
            if (s.material != nullptr) std::memcpy(wanted, s.material->textures, sizeof wanted);
        }
        for (UINT i = 0; i < kBindingTextures; ++i) {
            if (wanted[i] != 0) resolved[i] = detail::texture_resource(wanted[i]);
        }
    }
    std::unique_lock lock(s.mutex);
    s.frame = frame;
    s.draws_last.store(s.draws_frame.exchange(0), std::memory_order_relaxed);
    s.dispatches_last.store(s.dispatches_frame.exchange(0), std::memory_order_relaxed);
    s.stream_draws_last.store(s.stream_draws_frame.exchange(0), std::memory_order_relaxed);
    s.stream_skipped_last.store(s.stream_skipped_frame.exchange(0), std::memory_order_relaxed);
    s.use_counter.store(0, std::memory_order_relaxed);
    for (auto& r : s.records) {
        r->draws_last.store(r->draws_frame.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
        r->dispatches_frame.store(0, std::memory_order_relaxed);
        r->first_use_last.store(r->first_use.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
    }
    for (auto it = s.retired.begin(); it != s.retired.end();) {
        if (frame - it->frame > kRetireFrames) {
            it->pso->Release();
            it = s.retired.erase(it);
        } else {
            ++it;
        }
    }
    if (s.bindings.load(std::memory_order_relaxed)) {
        if (s.constant_buffer == nullptr && s.binding_device != nullptr) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = kBindingConstants; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
            desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(s.binding_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                                    nullptr, IID_PPV_ARGS(&s.constant_buffer)))) {
                const D3D12_RANGE none{0, 0};
                if (SUCCEEDED(s.constant_buffer->Map(0, &none, &s.constant_mapped))) {
                    std::memset(s.constant_mapped, 0, kBindingConstants);
                    s.constant_address.store(s.constant_buffer->GetGPUVirtualAddress(), std::memory_order_release);
                } else {
                    s.constant_buffer->Release();
                    s.constant_buffer = nullptr;
                }
            }
        }
        if (s.material != nullptr && s.material->constants_dirty && s.constant_mapped != nullptr) {
            std::memcpy(s.constant_mapped, s.material->constants.data(), std::min(s.material->constants.size(), kBindingConstants));
            s.material->constants_dirty = false;
        }
        bool changed = false;
        for (UINT i = 0; i < kBindingTextures; ++i) {
            ID3D12Resource* resource = resolved[i];
            resolved[i] = nullptr;
            if (resource != s.textures[i] || wanted[i] != s.texture_handles[i]) {
                if (s.textures[i] != nullptr) s.textures[i]->Release();
                s.textures[i] = resource;
                s.texture_handles[i] = wanted[i];
                changed = true;
            } else if (resource != nullptr) {
                resource->Release();
            }
        }
        if (changed) s.texture_generation.fetch_add(1, std::memory_order_acq_rel);
        if (frame - s.reported_frame >= 600) {
            s.reported_frame = frame;
            log::info("fx/pipeline: bindings: {} serialised ({} extended), {} root signature calls seen, {} from extended blobs, {} plain, {} not the game's, {} heaps reserved ({} could not be grown, {} borrowed), {} cached blobs dropped, {} library loads rebuilt, "
                      "constant buffer {}; last 600 frames {} bound draws, {} fell back (root {}, heap {}, constants {}); "
                      "root sets {} ({} extended), heap sets {} ({} reserved), count offset {}, {} boot signatures resolved",
                      s.serialized.load(), s.serialized_extended.load(), s.root_calls.load(), s.roots_extended.load(), s.roots_skipped.load(), s.roots_foreign.load(), s.heaps_reserved.load(), s.heaps_unreservable.load(), s.heaps_borrowed.load(), s.cached_stripped.load(), s.library_rebuilt.load(),
                      s.constant_address.load() != 0 ? "ready" : "missing",
                      s.bound_draws.exchange(0), s.bind_fallbacks.exchange(0), s.fallback_root.exchange(0),
                      s.fallback_heap.exchange(0), s.fallback_constants.exchange(0),
                      s.root_sets.exchange(0), s.root_sets_extended.exchange(0), s.heap_sets.exchange(0), s.heap_sets_found.exchange(0), s.count_offset, s.roots_calibrated.load());
        }
    }
    if (frame % kReloadInterval == 0) {
        for (auto& r : s.replacements) {
            if (stamp_of(r->path) != r->stamp) {
                log::info("fx/pipeline: {} changed on disk", r->path.filename().string());
                compile_replacement(s, *r);
            }
        }
        if (s.material != nullptr && stamp_of(s.material->path) != s.material->stamp) {
            log::info("fx/pipeline: material hook {} changed on disk", s.material->path.filename().string());
            compile_material(s, *s.material);
        }
    }
}

void shutdown() {
    auto& s = g();
    {
        std::scoped_lock lock(s.queue_mutex);
        s.stopping = true;
    }
    s.queue_signal.notify_all();
    if (s.builder.joinable()) s.builder.join();
    std::unique_lock lock(s.mutex);
    s.enabled.store(false);
    for (auto& r : s.records) {
        if (auto* twin = r->substitute.exchange(nullptr)) twin->Release();
    }
    for (auto& t : s.retired) t.pso->Release();
    s.retired.clear();
}

void destroy_owned(std::string_view owner) {
    auto& s = g();
    std::unique_lock lock(s.mutex);
    bool changed = false;
    for (auto it = s.hooks.begin(); it != s.hooks.end();) {
        if ((*it)->owner == owner) {
            for (auto& r : s.records) {
                auto& list = r->hooks;
                list.erase(std::remove(list.begin(), list.end(), it->get()), list.end());
                refresh_hooked(*r);
            }
            it = s.hooks.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = s.replacements.begin(); it != s.replacements.end();) {
        if ((*it)->owner == owner) { it = s.replacements.erase(it); changed = true; }
        else ++it;
    }
    if (s.material != nullptr && s.material->owner == owner) {
        s.material.reset();
        changed = true;
    }
    refresh_work(s);
    if (changed) s.generation.fetch_add(1, std::memory_order_acq_rel);
}

void set_enabled(bool enabled) { g().enabled.store(enabled); }
void set_bindings(bool enabled) { g().bindings.store(enabled); }
void set_bindings_early(bool enabled) { g().bindings_early.store(enabled); }
bool bindings() { return g().bindings.load(); }
bool enabled() { return g().enabled.load(); }
void set_retain(bool retain) { g().retain.store(retain); }
void set_stream_output(bool enabled) { g().stream_output_roots.store(enabled); }
void set_geometry_capture(bool enabled) {
    auto& s = g();
    std::unique_lock lock(s.mutex);
    if (s.geometry_capture.exchange(enabled) == enabled) return;
    s.generation.fetch_add(1, std::memory_order_acq_rel);
    refresh_work(s);
}
bool geometry_capture() { return g().geometry_capture.load(); }
bool retain() { return g().retain.load(); }

Snapshot snapshot() {
    auto& s = g();
    std::shared_lock lock(s.mutex);
    Snapshot out;
    out.pipelines = static_cast<unsigned>(s.records.size());
    for (const auto& r : s.records) {
        if (r->compute) ++out.compute; else ++out.graphics;
        if (r->substitute.load(std::memory_order_relaxed) != nullptr) ++out.replaced;
        if (r->hooked.load(std::memory_order_relaxed)) ++out.hooked;
        if (r->material_applied) ++out.material_applied;
        if (r->material_skipped) ++out.material_skipped;
        if (r->build_failed) ++out.material_failed;
        if (r->draws_last.load(std::memory_order_relaxed) > 0) ++out.used_last_frame;
    }
    out.stream_created = s.stream_created;
    out.library_loaded = s.library_loaded;
    out.blobs = static_cast<unsigned>(s.blobs.size());
    out.retained_bytes = s.retained_bytes;
    out.draws_last_frame = s.draws_last.load(std::memory_order_relaxed);
    out.dispatches_last_frame = s.dispatches_last.load(std::memory_order_relaxed);
    out.material_active = s.material != nullptr;
    if (s.material != nullptr) {
        out.material_owner = s.material->owner;
        out.material_problem = s.material->problem;
    }
    out.stream_twins = s.stream_twins.load(std::memory_order_relaxed);
    out.stream_draws_last_frame = s.stream_draws_last.load(std::memory_order_relaxed);
    out.stream_skipped_last_frame = s.stream_skipped_last.load(std::memory_order_relaxed);
    out.problem = s.problem;
    out.hooks_installed = s.hooks_installed;
    return out;
}

std::vector<Row> busiest(unsigned limit) {
    auto& s = g();
    std::shared_lock lock(s.mutex);
    std::unordered_map<std::uint64_t, Row> by_hash;
    for (const auto& r : s.records) {
        const auto draws = r->draws_last.load(std::memory_order_relaxed);
        if (r->stale || draws == 0) continue;
        auto& row = by_hash[r->hash];
        if (row.hash == 0) {
            row.hash = r->hash;
            row.compute = r->compute;
            row.order = r->first_use_last.load(std::memory_order_relaxed);
            const auto t = targets_of(*r);
            row.render_targets = t.count;
            row.rtv0 = t.rtv[0];
            row.dsv = t.dsv;
        }
        row.draws += draws;
        row.order = std::min(row.order, r->first_use_last.load(std::memory_order_relaxed));
        row.replaced |= r->substitute.load(std::memory_order_relaxed) != nullptr;
        row.hooked |= r->hooked.load(std::memory_order_relaxed);
        row.material |= r->material_applied;
        if (row.problem.empty()) row.problem = r->problem;
    }
    std::vector<Row> rows;
    rows.reserve(by_hash.size());
    for (auto& [hash, row] : by_hash) rows.push_back(std::move(row));
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.draws > b.draws; });
    if (rows.size() > limit) rows.resize(limit);
    return rows;
}

std::string dump(std::uint64_t hash) {
    auto& s = g();
    std::shared_lock lock(s.mutex);
    const auto found = s.by_hash.find(hash);
    if (found == s.by_hash.end()) return {};
    const auto* r = found->second;
    const auto folder = s.dumps / std::format("{:016x}", hash);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    static const char* const names[kStageCount] = {"vs", "ps", "ds", "hs", "gs", "cs"};
    for (unsigned i = 0; i < kStageCount; ++i) {
        const auto& blob = r->stages[i];
        if (!blob || blob->bytes.empty()) continue;
        std::ofstream(folder / (std::string(names[i]) + ".dxbc"), std::ios::binary)
            .write(reinterpret_cast<const char*>(blob->bytes.data()), static_cast<std::streamsize>(blob->bytes.size()));
        ID3DBlob* text = nullptr;
        if (SUCCEEDED(D3DDisassemble(blob->bytes.data(), blob->bytes.size(), 0, nullptr, &text)) && text != nullptr) {
            std::ofstream(folder / (std::string(names[i]) + ".txt"), std::ios::binary)
                .write(static_cast<const char*>(text->GetBufferPointer()), static_cast<std::streamsize>(text->GetBufferSize()));
            text->Release();
        }
    }
    std::ofstream info(folder / "pipeline.txt");
    info << std::format("hash {:016x}\n{} {}\n", hash, r->compute ? "compute" : "graphics",
                        r->stream_created ? "(stream created)" : "");
    const auto t = targets_of(*r);
    if (t.known) {
        info << std::format("render targets {}\n", t.count);
        for (unsigned i = 0; i < t.count; ++i) info << std::format("  rtv{} format {}\n", i, static_cast<int>(t.rtv[i]));
        info << std::format("dsv format {}\ntopology {}\n", static_cast<int>(t.dsv), static_cast<int>(t.topology));
        const auto* elements = r->graphics != nullptr ? &r->graphics->elements : r->stream != nullptr ? &r->stream->elements : nullptr;
        info << std::format("input elements {}\n", elements != nullptr ? elements->size() : 0);
        for (const auto& e : elements != nullptr ? *elements : std::vector<D3D12_INPUT_ELEMENT_DESC>{}) {
            info << std::format("  {}{} format {} slot {} offset {}\n", e.SemanticName, e.SemanticIndex,
                                static_cast<int>(e.Format), e.InputSlot, e.AlignedByteOffset);
        }
    }
    log::info("fx/pipeline: dumped pipeline {:016x} to {}", hash, folder.string());
    return folder.string();
}

std::uint32_t api_count() {
    auto& s = g();
    std::shared_lock lock(s.mutex);
    return static_cast<std::uint32_t>(s.records.size());
}

bool api_at(std::uint32_t index, BridgerFxPipelineInfo* out) {
    auto& s = g();
    std::shared_lock lock(s.mutex);
    if (out == nullptr || index >= s.records.size()) return false;
    fill_info(*s.records[index], *out);
    return true;
}

bool api_find(std::uint64_t hash, BridgerFxPipelineInfo* out) {
    auto& s = g();
    std::shared_lock lock(s.mutex);
    const auto found = s.by_hash.find(hash);
    if (out == nullptr || found == s.by_hash.end()) return false;
    fill_info(*found->second, *out);
    return true;
}

std::size_t api_dump(std::uint64_t hash, char* buffer, std::size_t capacity) {
    return copy_out(dump(hash), buffer, capacity);
}

BridgerFxHandle api_hook(std::uint64_t hash, BridgerFxDrawHook before, BridgerFxDrawHook after, void* user, const void* caller) {
    if (before == nullptr && after == nullptr) return 0;
    auto& s = g();
    std::unique_lock lock(s.mutex);
    auto entry = std::make_unique<HookEntry>();
    entry->handle = ++s.next_handle;
    entry->owner = owner_of(caller);
    entry->pipeline = hash;
    entry->before = before;
    entry->after = after;
    entry->user = user;
    unsigned attached = 0;
    for (auto& r : s.records) {
        if (r->hash == hash) {
            r->hooks.push_back(entry.get());
            refresh_hooked(*r);
            ++attached;
        }
    }
    log::info("fx/pipeline: {} hooked draws of pipeline {:016x} ({} record{})", entry->owner, hash, attached, attached == 1 ? "" : "s");
    const auto handle = entry->handle;
    s.hooks.push_back(std::move(entry));
    return handle;
}

BridgerFxHandle api_replace(const BridgerFxReplaceDesc* desc, const void* caller) {
    if (desc == nullptr || desc->path == nullptr || desc->stage > BRIDGER_FX_STAGE_CS) return 0;
    auto& s = g();
    std::unique_lock lock(s.mutex);
    auto r = std::make_unique<Replacement>();
    r->handle = ++s.next_handle;
    r->owner = owner_of(caller);
    r->pipeline = desc->pipeline;
    r->stage = desc->stage;
    r->path = mod_relative(r->owner, desc->path);
    r->entry = desc->entry != nullptr ? desc->entry : "main";
    for (std::uint32_t i = 0; desc->defines != nullptr && i < desc->define_count; ++i) {
        if (desc->defines[i] != nullptr) r->defines.emplace_back(desc->defines[i]);
    }
    compile_replacement(s, *r);
    const auto handle = r->handle;
    s.replacements.push_back(std::move(r));
    refresh_work(s);
    s.generation.fetch_add(1, std::memory_order_acq_rel);
    return handle;
}

BridgerFxHandle api_material_hook(const BridgerFxMaterialHookDesc* desc, const void* caller) {
    if (desc == nullptr || desc->path == nullptr) return 0;
    auto& s = g();
    std::unique_lock lock(s.mutex);
    const auto owner = owner_of(caller);
    if (s.material != nullptr && s.material->owner != owner) {
        log::warn("fx/pipeline: {} asked for the material hook but {} holds it", owner, s.material->owner);
        return 0;
    }
    auto m = std::make_unique<MaterialHook>();
    m->handle = s.material != nullptr ? s.material->handle : ++s.next_handle;
    m->owner = owner;
    m->path = mod_relative(owner, desc->path);
    m->entry = desc->entry != nullptr ? desc->entry : "bridger_material";
    for (std::uint32_t i = 0; desc->defines != nullptr && i < desc->define_count; ++i) {
        if (desc->defines[i] != nullptr) m->defines.emplace_back(desc->defines[i]);
    }
    m->min_targets = desc->min_render_targets;
    m->max_targets = desc->max_render_targets;
    m->only = desc->only_pipeline;
    if (s.material != nullptr) {
        m->constants = s.material->constants;
        m->constants_dirty = !m->constants.empty();
        std::memcpy(m->textures, s.material->textures, sizeof m->textures);
    }
    compile_material(s, *m);
    s.material = std::move(m);
    refresh_work(s);
    s.generation.fetch_add(1, std::memory_order_acq_rel);
    return s.material->handle;
}

bool api_material_constants(BridgerFxHandle handle, const void* data, std::size_t size) {
    auto& s = g();
    std::unique_lock lock(s.mutex);
    if (s.material == nullptr || s.material->handle != handle) return false;
    auto& c = s.material->constants;
    c.assign(kBindingConstants, 0);
    if (data != nullptr) std::memcpy(c.data(), data, std::min(size, kBindingConstants));
    s.material->constants_dirty = true;
    return s.bindings.load();
}

bool api_material_texture(BridgerFxHandle handle, std::uint32_t slot, BridgerFxHandle texture) {
    auto& s = g();
    std::unique_lock lock(s.mutex);
    if (s.material == nullptr || s.material->handle != handle || slot >= kBindingTextures) return false;
    s.material->textures[slot] = texture;
    return s.bindings.load();
}

bool api_reload(BridgerFxHandle handle) {
    auto& s = g();
    std::unique_lock lock(s.mutex);
    for (auto& r : s.replacements) {
        if (r->handle == handle) { compile_replacement(s, *r); return r->problem.empty(); }
    }
    if (s.material != nullptr && s.material->handle == handle) {
        compile_material(s, *s.material);
        return s.material->problem.empty();
    }
    return false;
}

std::size_t api_problem(BridgerFxHandle handle, char* buffer, std::size_t capacity) {
    auto& s = g();
    std::shared_lock lock(s.mutex);
    for (const auto& r : s.replacements) {
        if (r->handle == handle) return copy_out(r->problem, buffer, capacity);
    }
    if (s.material != nullptr && s.material->handle == handle) {
        std::string problem = s.material->problem;
        if (problem.empty()) {
            unsigned shown = 0;
            for (const auto& r : s.records) {
                if (!r->problem.empty() && shown < 8) {
                    problem += std::format("{:016x}: {}\n", r->hash, r->problem);
                    ++shown;
                }
            }
        }
        return copy_out(problem, buffer, capacity);
    }
    return copy_out(std::string(), buffer, capacity);
}

void api_revert(BridgerFxHandle handle) {
    auto& s = g();
    std::unique_lock lock(s.mutex);
    for (auto it = s.hooks.begin(); it != s.hooks.end(); ++it) {
        if ((*it)->handle == handle) {
            for (auto& r : s.records) {
                auto& list = r->hooks;
                list.erase(std::remove(list.begin(), list.end(), it->get()), list.end());
                refresh_hooked(*r);
            }
            s.hooks.erase(it);
            return;
        }
    }
    for (auto it = s.replacements.begin(); it != s.replacements.end(); ++it) {
        if ((*it)->handle == handle) {
            s.replacements.erase(it);
            refresh_work(s);
            s.generation.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
    }
    if (s.material != nullptr && s.material->handle == handle) {
        s.material.reset();
        refresh_work(s);
        s.generation.fetch_add(1, std::memory_order_acq_rel);
    }
}

}
