#pragma once

#include <cstdint>
#include <filesystem>

namespace bridger::patches {

void install(std::uintptr_t game_base, const std::filesystem::path& bridger_root);

void sync_sizes();

int mounted();

}
