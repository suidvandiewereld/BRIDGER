#pragma once

#include <cstdint>
#include <filesystem>

#include "core/memory.h"

namespace bridger::debugger {

void start(const std::filesystem::path& root, const mem::Module& game);
void toggle();
void stop();
void capture(const std::filesystem::path& file);
void open(std::uintptr_t address);

}
