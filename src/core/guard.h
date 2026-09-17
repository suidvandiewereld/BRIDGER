#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bridger {

using GuardedFn = void (*)(void*);

std::uint32_t guarded_call(GuardedFn function, void* context);

struct FaultInfo {
    std::uint32_t code = 0;
    std::uintptr_t instruction = 0;
    std::uintptr_t accessed = 0;
    std::uintptr_t frames[16]{};
    std::uint32_t frame_count = 0;
};
const FaultInfo& last_fault();

std::string describe_last_fault();

void promote_fault_handler();

class GuardSuspension {
public:
    GuardSuspension();
    ~GuardSuspension();
    GuardSuspension(const GuardSuspension&) = delete;
    GuardSuspension& operator=(const GuardSuspension&) = delete;

private:
    void* saved_;
};

bool safe_read(const void* address, void* destination, std::size_t bytes);

}
