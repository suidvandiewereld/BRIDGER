#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bridger::mem {

struct Module {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    std::uintptr_t text_begin = 0;
    std::uintptr_t text_end = 0;

    [[nodiscard]] bool valid() const { return base != 0; }
    [[nodiscard]] std::uintptr_t from_rva(std::uintptr_t rva) const { return base + rva; }
    [[nodiscard]] std::uintptr_t to_rva(std::uintptr_t address) const { return address - base; }
};

Module module_of(const wchar_t* name = nullptr);

class Pattern {
public:
    explicit Pattern(std::string_view signature);

    [[nodiscard]] bool empty() const { return bytes_.empty(); }
    [[nodiscard]] std::size_t size() const { return bytes_.size(); }
    [[nodiscard]] std::uintptr_t scan(std::uintptr_t begin, std::uintptr_t end) const;
    [[nodiscard]] std::uintptr_t scan(const Module& module) const;
    [[nodiscard]] std::vector<std::uintptr_t> scan_all(const Module& module) const;

private:
    std::vector<std::uint8_t> bytes_;
    std::vector<bool> mask_;
};

std::uintptr_t resolve_relative(std::uintptr_t address, std::size_t instruction_length = 4);

}
