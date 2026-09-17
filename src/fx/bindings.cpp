#include "fx/bindings.h"

#include <d3dcommon.h>

#include <vector>

namespace bridger::fx::bindings {
namespace {

bool uses_space(const D3D12_ROOT_SIGNATURE_DESC1& d, UINT space) {
    for (UINT i = 0; i < d.NumParameters; ++i) {
        const auto& p = d.pParameters[i];
        if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r) {
                if (p.DescriptorTable.pDescriptorRanges[r].RegisterSpace == space) return true;
            }
        } else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            if (p.Constants.RegisterSpace == space) return true;
        } else if (p.Descriptor.RegisterSpace == space) {
            return true;
        }
    }
    for (UINT i = 0; i < d.NumStaticSamplers; ++i) {
        if (d.pStaticSamplers[i].RegisterSpace == space) return true;
    }
    return false;
}

using CreateDeserializerFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
using SerializeVersionedFn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);

HRESULT create_deserializer(LPCVOID blob, SIZE_T length, ID3D12VersionedRootSignatureDeserializer** out) {
    static const auto fn = reinterpret_cast<CreateDeserializerFn>(
        GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12CreateVersionedRootSignatureDeserializer"));
    return fn != nullptr ? fn(blob, length, __uuidof(ID3D12VersionedRootSignatureDeserializer), reinterpret_cast<void**>(out)) : E_NOTIMPL;
}

HRESULT serialize_versioned(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc, ID3DBlob** blob, ID3DBlob** errors) {
    static const auto fn = reinterpret_cast<SerializeVersionedFn>(
        GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeVersionedRootSignature"));
    return fn != nullptr ? fn(desc, blob, errors) : E_NOTIMPL;
}

}

bool extend_root(const void* blob, SIZE_T length, std::vector<std::uint8_t>& out, RootExtension& ext, std::string& why,
                 bool allow_stream_output) {
    ID3D12VersionedRootSignatureDeserializer* reader = nullptr;
    if (FAILED(create_deserializer(blob, length, &reader))) {
        why = "not a root signature blob";
        return false;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* versioned = nullptr;
    const HRESULT hr = reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &versioned);
    bool ok = SUCCEEDED(hr) && versioned != nullptr;
    if (!ok) why = "could not read it at version 1.1";
    if (ok) {
        const auto& d = versioned->Desc_1_1;
        if ((d.Flags & D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE) != 0) { ok = false; why = "a local root signature"; }
        else if (uses_space(d, kBindingSpace)) { ok = false; why = "already uses register space 60"; }
    }
    if (ok) {
        const auto& d = versioned->Desc_1_1;
        std::vector<D3D12_ROOT_PARAMETER1> params(d.pParameters, d.pParameters + d.NumParameters);
        std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(d.pStaticSamplers, d.pStaticSamplers + d.NumStaticSamplers);
        ext.game_parameters = d.NumParameters;
        ext.constants = d.NumParameters;
        D3D12_ROOT_PARAMETER1 cb{};
        cb.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        cb.Descriptor = {0, kBindingSpace, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        cb.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(cb);
        ext.textures = d.NumParameters + 1;
        static const D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kBindingTextures, 0, kBindingSpace,
                                                   D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
        D3D12_ROOT_PARAMETER1 table{};
        table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        table.DescriptorTable = {1, &range};
        table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(table);
        ext.pixel_denied = (d.Flags & D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS) != 0;
        const D3D12_FILTER filters[kBindingSamplers] = {D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_FILTER_MIN_MAG_MIP_POINT,
                                                        D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_FILTER_MIN_MAG_MIP_POINT};
        for (UINT i = 0; i < kBindingSamplers; ++i) {
            D3D12_STATIC_SAMPLER_DESC sd{};
            sd.Filter = filters[i];
            const auto mode = i < 2 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.AddressU = sd.AddressV = sd.AddressW = mode;
            sd.MaxAnisotropy = 1;
            sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            sd.ShaderRegister = i;
            sd.RegisterSpace = kBindingSpace;
            sd.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            samplers.push_back(sd);
        }
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC extended{};
        extended.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        extended.Desc_1_1 = d;
        if (allow_stream_output) extended.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
        extended.Desc_1_1.NumParameters = static_cast<UINT>(params.size());
        extended.Desc_1_1.pParameters = params.data();
        extended.Desc_1_1.NumStaticSamplers = static_cast<UINT>(samplers.size());
        extended.Desc_1_1.pStaticSamplers = samplers.data();
        ID3DBlob* serialized = nullptr;
        ID3DBlob* errors = nullptr;
        if (SUCCEEDED(serialize_versioned(&extended, &serialized, &errors)) && serialized != nullptr) {
            const auto* bytes = static_cast<const std::uint8_t*>(serialized->GetBufferPointer());
            out.assign(bytes, bytes + serialized->GetBufferSize());
        } else {
            ok = false;
            why = errors != nullptr ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                                    : "serialising the extension failed";
        }
        if (serialized != nullptr) serialized->Release();
        if (errors != nullptr) errors->Release();
    }
    reader->Release();
    return ok;
}

}
