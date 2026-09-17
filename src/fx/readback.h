#pragma once

#include <Windows.h>
#include <d3d12.h>

#include <filesystem>
#include <string>

namespace bridger::fx::readback {

void configure(const std::filesystem::path& directory);

bool request(ID3D12Device* device, ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES state, std::string name,
             ID3D12Fence* fence = nullptr, UINT64 fence_value = 0);

void poll();

[[nodiscard]] unsigned written();
[[nodiscard]] std::string last_message();
[[nodiscard]] unsigned pending();

}
