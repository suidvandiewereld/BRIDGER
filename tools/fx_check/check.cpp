
#include <Windows.h>

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "fx/prelude.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace {

class Includes final : public ID3DInclude {
public:
    std::vector<std::filesystem::path> search;
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

bool compile(const std::string& full, const std::string& display, const D3D_SHADER_MACRO* macros,
             Includes& includes, const char* entry, const char* target, std::string& output) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3
                     | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR;
    const HRESULT result = D3DCompile(full.data(), full.size(), display.c_str(), macros, &includes,
                                      entry, target, flags, 0, &blob, &errors);
    if (errors != nullptr) {
        output.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        errors->Release();
    }
    if (blob != nullptr) {
        blob->Release();
    }
    return SUCCEEDED(result);
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: fx_check <fullscreen|world|screen> file.hlsl [...]\n");
        return 64;
    }
    const std::string kind = argv[1];
    const char* vs_entry = "bridger_fullscreen_vs";
    const char* kind_define = "BRIDGER_FULLSCREEN";
    if (kind == "world") {
        vs_entry = "bridger_world_vs";
        kind_define = "BRIDGER_WORLD";
    } else if (kind == "screen") {
        vs_entry = "bridger_screen_vs";
        kind_define = "BRIDGER_SCREEN";
    } else if (kind != "fullscreen") {
        std::fprintf(stderr, "unknown kind %s\n", kind.c_str());
        return 64;
    }

    int failures = 0;
    for (int i = 2; i < argc; ++i) {
        const std::filesystem::path path = argv[i];
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) {
            std::printf("%s: cannot open\n", path.string().c_str());
            ++failures;
            continue;
        }
        std::string source((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        std::string line_name = path.string();
        for (auto& c : line_name) {
            if (c == '\\') {
                c = '/';
            }
        }
        std::string full = bridger::fx::prelude_source();
        full += "\n#line 1 \"" + line_name + "\"\n" + source;

        const D3D_SHADER_MACRO macros[] = {{"BRIDGER_FX", "1"}, {kind_define, "1"}, {nullptr, nullptr}};
        Includes includes;
        includes.prelude = bridger::fx::prelude_source();
        includes.search.push_back(path.parent_path());

        std::string output;
        const bool vs = compile(full, path.string(), macros, includes, vs_entry, "vs_5_0", output);
        const bool ps = compile(full, path.string(), macros, includes, "ps_main", "ps_5_0", output);
        if (vs && ps) {
            std::printf("%s: ok%s\n", path.string().c_str(), output.empty() ? "" : " (warnings)");
        } else {
            std::printf("%s: FAILED\n", path.string().c_str());
            ++failures;
        }
        if (!output.empty()) {
            std::printf("%s\n", output.c_str());
        }
    }
    return failures;
}
