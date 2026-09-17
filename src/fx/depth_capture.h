#pragma once

#include <Windows.h>
#include <d3d12.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bridger::fx::depth {

struct Candidate {
    ID3D12Resource* resource = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned clears = 0;
    unsigned last_seen_frame = 0;
    bool msaa = false;
};

bool prepare(void** device_vtable, void** command_list_vtable);

void set_tracking(bool enabled);
bool tracking();

void end_frame(unsigned frame);
bool select(int preferred_index, unsigned swapchain_width, unsigned swapchain_height,
            Candidate& out);

void watch(ID3D12Resource* resource);

D3D12_RESOURCE_STATES known_state(ID3D12Resource* resource, D3D12_RESOURCE_STATES fallback);

std::vector<Candidate> candidates();
unsigned known_views();
void reset();

}
