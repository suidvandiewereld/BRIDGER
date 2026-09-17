#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/memory.h"

namespace bridger::debugger::picker {

bool install(const mem::Module& game);

void set_collecting(bool collecting);

struct Candidate {
    std::uintptr_t entity = 0;
    std::string type;
    float x = 0.0f;
    float y = 0.0f;
    float radius = 0.0f;
    double distance = 0.0;
};

std::vector<Candidate> update(float cursor_x, float cursor_y, int& hovered);

}
