#pragma once

#include <cstdint>

#include "bridger/mod.hpp"

namespace bridger {

struct InspectApi {
    int version;
    void (*focus)(std::uint64_t address);
};

inline constexpr const char* kInspectService = "bridger.inspect.v1";

inline bool inspect(const void* object) {
    const auto* service = require<InspectApi>(kInspectService);
    if (service == nullptr || service->version < 1 || service->focus == nullptr) {
        return false;
    }
    service->focus(reinterpret_cast<std::uint64_t>(object));
    return true;
}

}
