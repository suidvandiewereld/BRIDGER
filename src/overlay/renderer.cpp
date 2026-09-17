#include "overlay/renderer.h"

#include <Windows.h>

#include "overlay/d3d.h"

#include <algorithm>
#include <vector>

#include <cstring>

#include "core/log.h"
#include "fx/fx.h"
#include "ui/draw.h"
#include "ui/font.h"

namespace bridger::overlay {
namespace {

constexpr char kShader[] = R"(
cbuffer Constants : register(b0) {
    float2 scale;
    float2 translate;
};

struct VsInput {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct PsInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

Texture2D atlas : register(t0);
SamplerState atlas_sampler : register(s0);

PsInput vs_main(VsInput input) {
    PsInput output;
    output.position = float4(input.position * scale + translate, 0.0f, 1.0f);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 ps_main(PsInput input) : SV_TARGET {
    float coverage = atlas.Sample(atlas_sampler, input.uv).r;
    return float4(input.color.rgb, input.color.a * coverage);
}
)";

template <typename T>
void release(T*& pointer) {
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

struct Frame {
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12Resource* vertices = nullptr;
    ID3D12Resource* indices = nullptr;
    std::size_t vertex_capacity = 0;
    std::size_t index_capacity = 0;
    ID3D12Resource* target = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    std::uint64_t fence_value = 0;
};

}

struct Renderer::Impl {
    ID3D12Device* device = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    ID3D12GraphicsCommandList* commands = nullptr;
    ID3D12Resource* atlas = nullptr;
    ID3D12Resource* atlas_upload = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    std::uint64_t fence_counter = 0;
    std::vector<Frame> frames;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool atlas_uploaded = false;
};

Renderer::~Renderer() {
    shutdown();
}

void Renderer::release_targets() {
    if (impl_ == nullptr) {
        return;
    }
    for (auto& frame : impl_->frames) {
        release(frame.target);
    }
}

void Renderer::shutdown() {
    if (impl_ == nullptr) {
        return;
    }
    fx::shutdown();
    for (auto& frame : impl_->frames) {
        release(frame.target);
        release(frame.vertices);
        release(frame.indices);
        release(frame.allocator);
    }
    impl_->frames.clear();
    release(impl_->commands);
    release(impl_->pipeline);
    release(impl_->root);
    release(impl_->rtv_heap);
    release(impl_->srv_heap);
    release(impl_->atlas);
    release(impl_->atlas_upload);
    release(impl_->fence);
    if (impl_->fence_event != nullptr) {
        CloseHandle(impl_->fence_event);
        impl_->fence_event = nullptr;
    }
    release(impl_->device);
    delete impl_;
    impl_ = nullptr;
    ready_ = false;
}

bool Renderer::initialise(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue) {
    if (swapchain == nullptr || queue == nullptr) {
        return false;
    }
    shutdown();
    impl_ = new Impl();

    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&impl_->device)))) {
        log::error("overlay: could not get the d3d12 device");
        shutdown();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);
    impl_->format = desc.BufferDesc.Format;
    width_ = desc.BufferDesc.Width;
    height_ = desc.BufferDesc.Height;
    const auto count = std::max<UINT>(desc.BufferCount, 2);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = count;
    if (FAILED(impl_->device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&impl_->rtv_heap)))) {
        shutdown();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = 1;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(impl_->device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&impl_->srv_heap)))) {
        shutdown();
        return false;
    }

    const auto rtv_stride =
        impl_->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtv_handle = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();

    impl_->frames.resize(count);
    for (UINT i = 0; i < count; ++i) {
        auto& frame = impl_->frames[i];
        frame.rtv = rtv_handle;
        rtv_handle.ptr += rtv_stride;
        if (FAILED(impl_->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&frame.allocator)))) {
            shutdown();
            return false;
        }
    }

    if (FAILED(impl_->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               impl_->frames[0].allocator, nullptr,
                                               IID_PPV_ARGS(&impl_->commands)))) {
        shutdown();
        return false;
    }
    impl_->commands->Close();

    if (FAILED(impl_->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl_->fence)))) {
        shutdown();
        return false;
    }
    impl_->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 4;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 2;
    signature.pParameters = parameters;
    signature.NumStaticSamplers = 1;
    signature.pStaticSamplers = &sampler;
    signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* serialized = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(d3d::serialize_root_signature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &serialized, &errors))) {
        release(errors);
        shutdown();
        return false;
    }
    const auto root_result = impl_->device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&impl_->root));
    release(serialized);
    release(errors);
    if (FAILED(root_result)) {
        shutdown();
        return false;
    }

    ID3DBlob* vertex_blob = nullptr;
    ID3DBlob* pixel_blob = nullptr;
    if (FAILED(d3d::compile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "vs_main",
                          "vs_5_0", 0, 0, &vertex_blob, &errors))) {
        if (errors != nullptr) {
            log::error("overlay: vertex shader failed: {}",
                       static_cast<const char*>(errors->GetBufferPointer()));
        }
        release(errors);
        shutdown();
        return false;
    }
    release(errors);
    if (FAILED(d3d::compile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "ps_main",
                          "ps_5_0", 0, 0, &pixel_blob, &errors))) {
        if (errors != nullptr) {
            log::error("overlay: pixel shader failed: {}",
                       static_cast<const char*>(errors->GetBufferPointer()));
        }
        release(errors);
        release(vertex_blob);
        shutdown();
        return false;
    }
    release(errors);

    D3D12_INPUT_ELEMENT_DESC elements[3]{};
    elements[0] = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    elements[1] = {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    elements[2] = {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = impl_->root;
    pso.VS = {vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize()};
    pso.PS = {pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize()};
    pso.InputLayout = {elements, 3};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = impl_->format;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;

    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    const auto pso_result = impl_->device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&impl_->pipeline));
    release(vertex_blob);
    release(pixel_blob);
    if (FAILED(pso_result)) {
        log::error("overlay: pipeline creation failed");
        shutdown();
        return false;
    }

    const int atlas_size = ui::font::atlas_size();
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = static_cast<UINT64>(atlas_size);
    texture.Height = static_cast<UINT>(atlas_size);
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    if (FAILED(impl_->device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&impl_->atlas)))) {
        shutdown();
        return false;
    }

    const UINT row_pitch = (static_cast<UINT>(atlas_size) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                         & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = static_cast<UINT64>(row_pitch) * atlas_size;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.Format = DXGI_FORMAT_UNKNOWN;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(impl_->device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&impl_->atlas_upload)))) {
        shutdown();
        return false;
    }

    void* mapped = nullptr;
    D3D12_RANGE nothing{0, 0};
    if (SUCCEEDED(impl_->atlas_upload->Map(0, &nothing, &mapped))) {
        const auto& pixels = ui::font::atlas();
        auto* destination = static_cast<std::uint8_t*>(mapped);
        for (int row = 0; row < atlas_size; ++row) {
            std::memcpy(destination + static_cast<std::size_t>(row) * row_pitch,
                        pixels.data() + static_cast<std::size_t>(row) * atlas_size,
                        static_cast<std::size_t>(atlas_size));
        }
        impl_->atlas_upload->Unmap(0, nullptr);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    impl_->device->CreateShaderResourceView(impl_->atlas, &view,
                                            impl_->srv_heap->GetCPUDescriptorHandleForHeapStart());

    ready_ = true;
    log::info("overlay: renderer ready ({}x{}, {} buffers)", width_, height_, count);
    if (!fx::initialise(impl_->device, queue, impl_->format, width_, height_, count)) {
        log::warn("overlay: shader system unavailable this session");
    }
    return true;
}

void Renderer::render(const ui::DrawList* list, IDXGISwapChain3* swapchain,
                      ID3D12CommandQueue* queue) {
    if (!ready_ || impl_ == nullptr) {
        return;
    }
    const bool draw_ui = list != nullptr && !list->indices().empty();
    const bool draw_fx = fx::has_work();
    if (!draw_ui && !draw_fx) {
        return;
    }

    const auto index = swapchain->GetCurrentBackBufferIndex();
    if (index >= impl_->frames.size()) {
        return;
    }
    auto& frame = impl_->frames[index];

    if (frame.fence_value != 0 && impl_->fence->GetCompletedValue() < frame.fence_value) {
        impl_->fence->SetEventOnCompletion(frame.fence_value, impl_->fence_event);
        WaitForSingleObject(impl_->fence_event, 200);
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);

    if (frame.target == nullptr) {
        if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&frame.target)))) {
            return;
        }
        impl_->device->CreateRenderTargetView(frame.target, nullptr, frame.rtv);
    }

    std::size_t vertex_bytes = 0;
    std::size_t index_bytes = 0;
    if (draw_ui) {
        vertex_bytes = list->vertices().size() * sizeof(ui::Vertex);
        index_bytes = list->indices().size() * sizeof(std::uint32_t);

        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.Format = DXGI_FORMAT_UNKNOWN;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (frame.vertex_capacity < vertex_bytes) {
            release(frame.vertices);
            frame.vertex_capacity = vertex_bytes + 8192;
            buffer.Width = frame.vertex_capacity;
            if (FAILED(impl_->device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&frame.vertices)))) {
                return;
            }
        }
        if (frame.index_capacity < index_bytes) {
            release(frame.indices);
            frame.index_capacity = index_bytes + 8192;
            buffer.Width = frame.index_capacity;
            if (FAILED(impl_->device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&frame.indices)))) {
                return;
            }
        }

        D3D12_RANGE nothing{0, 0};
        void* mapped = nullptr;
        if (SUCCEEDED(frame.vertices->Map(0, &nothing, &mapped))) {
            std::memcpy(mapped, list->vertices().data(), vertex_bytes);
            frame.vertices->Unmap(0, nullptr);
        }
        if (SUCCEEDED(frame.indices->Map(0, &nothing, &mapped))) {
            std::memcpy(mapped, list->indices().data(), index_bytes);
            frame.indices->Unmap(0, nullptr);
        }
    }

    frame.allocator->Reset();
    impl_->commands->Reset(frame.allocator, impl_->pipeline);

    if (!impl_->atlas_uploaded) {
        const int atlas_size = ui::font::atlas_size();
        const UINT row_pitch = (static_cast<UINT>(atlas_size) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                             & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = impl_->atlas;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = impl_->atlas_upload;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
        source.PlacedFootprint.Footprint.Width = static_cast<UINT>(atlas_size);
        source.PlacedFootprint.Footprint.Height = static_cast<UINT>(atlas_size);
        source.PlacedFootprint.Footprint.Depth = 1;
        source.PlacedFootprint.Footprint.RowPitch = row_pitch;

        impl_->commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        D3D12_RESOURCE_BARRIER to_shader{};
        to_shader.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_shader.Transition.pResource = impl_->atlas;
        to_shader.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        to_shader.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        to_shader.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        impl_->commands->ResourceBarrier(1, &to_shader);
        impl_->atlas_uploaded = true;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.target;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    impl_->commands->ResourceBarrier(1, &barrier);

    if (draw_fx) {
        fx::FrameContext context;
        context.commands = impl_->commands;
        context.backbuffer = frame.target;
        context.backbuffer_rtv = frame.rtv;
        context.frame_index = index;
        context.width = desc.BufferDesc.Width;
        context.height = desc.BufferDesc.Height;
        context.format = desc.BufferDesc.Format;
        context.completion_fence = impl_->fence;
        context.completion_value = impl_->fence_counter + 1;
        fx::render(context);
    }

    if (draw_ui) {
        impl_->commands->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
        impl_->commands->SetGraphicsRootSignature(impl_->root);
        impl_->commands->SetPipelineState(impl_->pipeline);
        impl_->commands->SetDescriptorHeaps(1, &impl_->srv_heap);
        impl_->commands->SetGraphicsRootDescriptorTable(
            1, impl_->srv_heap->GetGPUDescriptorHandleForHeapStart());

        const float display_width = list->display().x;
        const float display_height = list->display().y;
        const float constants[4] = {2.0f / display_width, -2.0f / display_height, -1.0f, 1.0f};
        impl_->commands->SetGraphicsRoot32BitConstants(0, 4, constants, 0);

        D3D12_VIEWPORT viewport{0.0f, 0.0f, display_width, display_height, 0.0f, 1.0f};
        impl_->commands->RSSetViewports(1, &viewport);

        D3D12_VERTEX_BUFFER_VIEW vertex_view{};
        vertex_view.BufferLocation = frame.vertices->GetGPUVirtualAddress();
        vertex_view.SizeInBytes = static_cast<UINT>(vertex_bytes);
        vertex_view.StrideInBytes = sizeof(ui::Vertex);
        impl_->commands->IASetVertexBuffers(0, 1, &vertex_view);

        D3D12_INDEX_BUFFER_VIEW index_view{};
        index_view.BufferLocation = frame.indices->GetGPUVirtualAddress();
        index_view.SizeInBytes = static_cast<UINT>(index_bytes);
        index_view.Format = DXGI_FORMAT_R32_UINT;
        impl_->commands->IASetIndexBuffer(&index_view);
        impl_->commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (const auto& command : list->commands()) {
            if (command.index_count == 0) {
                continue;
            }
            const D3D12_RECT scissor{
                static_cast<LONG>(std::max(0.0f, command.clip.x0)),
                static_cast<LONG>(std::max(0.0f, command.clip.y0)),
                static_cast<LONG>(std::min(display_width, command.clip.x1)),
                static_cast<LONG>(std::min(display_height, command.clip.y1)),
            };
            if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) {
                continue;
            }
            impl_->commands->RSSetScissorRects(1, &scissor);
            impl_->commands->DrawIndexedInstanced(command.index_count, 1, command.index_offset, 0, 0);
        }
    }

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    impl_->commands->ResourceBarrier(1, &barrier);
    impl_->commands->Close();

    ID3D12CommandList* lists[] = {impl_->commands};
    queue->ExecuteCommandLists(1, lists);

    frame.fence_value = ++impl_->fence_counter;
    queue->Signal(impl_->fence, frame.fence_value);
}

}
