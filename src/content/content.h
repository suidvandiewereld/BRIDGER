#pragma once

#include <string_view>

#include "bridger/api.h"

namespace bridger::content {

void tick();

void destroy_owned(std::string_view owner);

void revert_all();

const BridgerContent* api();

}
