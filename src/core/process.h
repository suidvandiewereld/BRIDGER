#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bridger::proc {

struct Region {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    bool writable = false;
    bool image = false;

    [[nodiscard]] std::size_t size() const { return end - begin; }
};

class AddressSpace {
public:
    void refresh();

    [[nodiscard]] const std::vector<Region>& regions() const { return regions_; }
    [[nodiscard]] const Region* find(std::uintptr_t address) const;
    [[nodiscard]] bool mapped(std::uintptr_t address, std::size_t bytes) const;

    [[nodiscard]] bool read(std::uintptr_t address, void* destination, std::size_t bytes) const;

    template <typename T>
    [[nodiscard]] bool read(std::uintptr_t address, T& destination) const {
        return read(address, &destination, sizeof(T));
    }

    [[nodiscard]] std::string read_string(std::uintptr_t address, std::size_t limit = 256) const;
    [[nodiscard]] bool identifier(std::uintptr_t address) const;

private:
    std::vector<Region> regions_;
};

}
