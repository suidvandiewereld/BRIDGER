#pragma once

#include <cstdint>

namespace bridger::overlay::inspect {

void draw();

void focus(std::uintptr_t address);

bool pending();

}
