#include "overlay/d3d.h"

#include "core/log.h"

namespace bridger::overlay::d3d {
namespace {

bool g_loaded = false;

template <typename T>
T resolve(HMODULE module, const char* name) {
    return module == nullptr ? nullptr : reinterpret_cast<T>(GetProcAddress(module, name));
}

}

CreateDeviceFn create_device = nullptr;
SerializeRootSignatureFn serialize_root_signature = nullptr;
CreateFactoryFn create_factory = nullptr;
CompileFn compile = nullptr;

bool load() {
    if (g_loaded) {
        return true;
    }

    const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    HMODULE compiler = GetModuleHandleW(L"d3dcompiler_47.dll");
    if (compiler == nullptr) {
        compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    }

    create_device = resolve<CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    serialize_root_signature =
        resolve<SerializeRootSignatureFn>(d3d12, "D3D12SerializeRootSignature");
    create_factory = resolve<CreateFactoryFn>(dxgi, "CreateDXGIFactory1");
    compile = resolve<CompileFn>(compiler, "D3DCompile");

    g_loaded = create_device != nullptr && serialize_root_signature != nullptr
            && create_factory != nullptr && compile != nullptr;
    if (!g_loaded) {
        log::error("overlay: could not resolve the d3d12 entry points");
    }
    return g_loaded;
}

}
