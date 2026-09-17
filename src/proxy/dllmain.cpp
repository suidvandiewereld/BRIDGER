#include <Windows.h>

#include <filesystem>

#include "proxy/winhttp_exports.inl"

namespace {

std::filesystem::path module_directory(HMODULE module) {
    wchar_t buffer[MAX_PATH]{};
    if (GetModuleFileNameW(module, buffer, MAX_PATH) == 0) {
        return {};
    }
    return std::filesystem::path(buffer).parent_path();
}

void load_core(HMODULE self) {
    const auto core = module_directory(self) / L"Bridger" / L"bridger.dll";

    std::error_code ec;
    if (!std::filesystem::exists(core, ec)) {
        MessageBoxW(nullptr, core.c_str(), L"Bridger: core not found", MB_ICONERROR | MB_OK);
        return;
    }

    if (LoadLibraryW(core.c_str()) == nullptr) {
        MessageBoxW(nullptr, core.c_str(), L"Bridger: core failed to load", MB_ICONERROR | MB_OK);
    }
}

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        load_core(module);
    }
    return TRUE;
}
