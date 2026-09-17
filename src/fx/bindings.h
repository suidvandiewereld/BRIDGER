#pragma once

#include <Windows.h>
#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bridger::fx::bindings {

constexpr UINT kBindingSpace = 60;
constexpr UINT kBindingTextures = 4;
constexpr UINT kBindingSamplers = 4;
constexpr std::size_t kBindingConstants = 4096;

struct RootExtension {
    UINT game_parameters = 0;
    UINT constants = 0;
    UINT textures = 1;
    bool pixel_denied = false;
};
constexpr UINT kBindingParameters = 2;

bool extend_root(const void* blob, std::size_t length, std::vector<std::uint8_t>& out, RootExtension& ext,
                 std::string& why, bool allow_stream_output = false);

}
