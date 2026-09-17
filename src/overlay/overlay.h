#pragma once

#include <filesystem>

#include "core/memory.h"

namespace bridger::overlay {

bool initialise(const std::filesystem::path& root, const mem::Module& game);
void shutdown();
bool visible();
void toggle();

bool begin_pick();
void cancel_pick();
[[nodiscard]] bool picking();

}
