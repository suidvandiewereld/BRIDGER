#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/memory.h"

namespace bridger::debugger::globals {

struct Global {
    std::uintptr_t slot = 0;
    bool indirect = false;
    std::string type;
    std::string source;
    std::size_t references = 0;
};

void scan(const mem::Module& game);

[[nodiscard]] std::vector<Global> snapshot();
[[nodiscard]] bool running();
[[nodiscard]] std::size_t examined();
[[nodiscard]] std::size_t total();

[[nodiscard]] std::uintptr_t resolve(const Global& global);

}
