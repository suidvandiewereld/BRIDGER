#include "core/process.h"

#include <Windows.h>

#include <algorithm>

namespace bridger::proc {
namespace {

constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                          | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE
                          | PAGE_EXECUTE_WRITECOPY;

bool usable(const MEMORY_BASIC_INFORMATION& info) {
    if (info.State != MEM_COMMIT) {
        return false;
    }
    if ((info.Protect & kReadable) == 0) {
        return false;
    }
    return (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
}

}

void AddressSpace::refresh() {
    regions_.clear();

    SYSTEM_INFO system{};
    GetSystemInfo(&system);

    auto cursor = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
    const auto limit = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);

    MEMORY_BASIC_INFORMATION info{};
    while (cursor < limit && VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &info, sizeof(info)) != 0) {
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (usable(info)) {
            Region region;
            region.begin = base;
            region.end = base + info.RegionSize;
            region.writable = (info.Protect & kWritable) != 0;
            region.image = info.Type == MEM_IMAGE;
            if (!regions_.empty() && regions_.back().end == region.begin
                    && regions_.back().writable == region.writable
                    && regions_.back().image == region.image) {
                regions_.back().end = region.end;
            } else {
                regions_.push_back(region);
            }
        }
        cursor = base + info.RegionSize;
    }

    std::sort(regions_.begin(), regions_.end(),
              [](const Region& a, const Region& b) { return a.begin < b.begin; });
}

const Region* AddressSpace::find(std::uintptr_t address) const {
    auto it = std::upper_bound(regions_.begin(), regions_.end(), address,
                               [](std::uintptr_t value, const Region& region) {
                                   return value < region.begin;
                               });
    if (it == regions_.begin()) {
        return nullptr;
    }
    --it;
    return address < it->end ? &*it : nullptr;
}

bool AddressSpace::mapped(std::uintptr_t address, std::size_t bytes) const {
    if (address == 0 || bytes == 0) {
        return false;
    }
    const auto* region = find(address);
    return region != nullptr && address + bytes <= region->end;
}

bool AddressSpace::read(std::uintptr_t address, void* destination, std::size_t bytes) const {
    if (address == 0 || bytes == 0) {
        return false;
    }
    SIZE_T copied = 0;
    const BOOL ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address),
                                      destination, bytes, &copied);
    return ok != FALSE && copied == bytes;
}

std::string AddressSpace::read_string(std::uintptr_t address, std::size_t limit) const {
    if (address == 0) {
        return {};
    }
    std::string out;
    char buffer[64];
    while (out.size() < limit) {
        const auto want = std::min<std::size_t>(sizeof(buffer), limit - out.size());
        SIZE_T copied = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address + out.size()),
                              buffer, want, &copied) == FALSE || copied == 0) {
            return {};
        }
        for (SIZE_T i = 0; i < copied; ++i) {
            if (buffer[i] == '\0') {
                out.append(buffer, i);
                return out;
            }
        }
        out.append(buffer, copied);
    }
    return {};
}

bool AddressSpace::identifier(std::uintptr_t address) const {
    const auto text = read_string(address, 200);
    if (text.empty()) {
        return false;
    }
    const char first = text.front();
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
        return false;
    }
    for (const char c : text) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                     || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}
