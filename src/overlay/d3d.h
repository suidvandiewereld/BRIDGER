#pragma once

#include <Windows.h>

#include <d3d12.h>
#include <d3dcommon.h>
#include <dxgi1_4.h>

namespace bridger::overlay::d3d {

using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using SerializeRootSignatureFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*,
                                                 D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                   LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

bool load();

extern CreateDeviceFn create_device;
extern SerializeRootSignatureFn serialize_root_signature;
extern CreateFactoryFn create_factory;
extern CompileFn compile;

}
