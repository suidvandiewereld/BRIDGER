#include "core/memory.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>

namespace bridger::mem {

Module module_of(const wchar_t* name) {
    Module out;
    const auto handle = GetModuleHandleW(name);
    if (handle == nullptr) {
        return out;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(handle);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return out;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return out;
    }

    out.base = base;
    out.size = nt->OptionalHeader.SizeOfImage;

    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        out.text_begin = base + section->VirtualAddress;
        out.text_end = out.text_begin + section->Misc.VirtualSize;
        break;
    }
    return out;
}

Pattern::Pattern(std::string_view signature) {
    for (std::size_t i = 0; i < signature.size();) {
        if (signature[i] == ' ') {
            ++i;
            continue;
        }
        if (signature[i] == '?') {
            bytes_.push_back(0);
            mask_.push_back(false);
            while (i < signature.size() && signature[i] == '?') {
                ++i;
            }
            continue;
        }

        unsigned value = 0;
        const char* first = signature.data() + i;
        const char* last = signature.data() + std::min(i + 2, signature.size());
        const auto result = std::from_chars(first, last, value, 16);
        if (result.ec != std::errc{}) {
            bytes_.clear();
            mask_.clear();
            return;
        }
        bytes_.push_back(static_cast<std::uint8_t>(value));
        mask_.push_back(true);
        i += static_cast<std::size_t>(result.ptr - first);
    }
}

std::uintptr_t Pattern::scan(std::uintptr_t begin, std::uintptr_t end) const {
    if (bytes_.empty() || begin == 0 || end <= begin || end - begin < bytes_.size()) {
        return 0;
    }

    const auto* data = reinterpret_cast<const std::uint8_t*>(begin);
    const std::size_t span = (end - begin) - bytes_.size();
    for (std::size_t i = 0; i <= span; ++i) {
        bool hit = true;
        for (std::size_t j = 0; j < bytes_.size(); ++j) {
            if (mask_[j] && data[i + j] != bytes_[j]) {
                hit = false;
                break;
            }
        }
        if (hit) {
            return begin + i;
        }
    }
    return 0;
}

std::uintptr_t Pattern::scan(const Module& module) const {
    return scan(module.text_begin, module.text_end);
}

std::vector<std::uintptr_t> Pattern::scan_all(const Module& module) const {
    std::vector<std::uintptr_t> hits;
    std::uintptr_t cursor = module.text_begin;
    while (cursor != 0 && cursor < module.text_end) {
        const auto hit = scan(cursor, module.text_end);
        if (hit == 0) {
            break;
        }
        hits.push_back(hit);
        cursor = hit + 1;
    }
    return hits;
}

std::uintptr_t resolve_relative(std::uintptr_t address, std::size_t instruction_length) {
    if (address == 0) {
        return 0;
    }
    const auto displacement = *reinterpret_cast<const std::int32_t*>(address);
    return address + instruction_length + static_cast<std::intptr_t>(displacement);
}

}
