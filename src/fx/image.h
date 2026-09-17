#pragma once

#include <dxgiformat.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bridger::fx {

struct Image {
    struct Level {
        std::size_t offset = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t row_pitch = 0;
        std::uint32_t rows = 0;
    };

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<Level> levels;
    std::vector<std::uint8_t> data;

    [[nodiscard]] bool valid() const { return width > 0 && height > 0 && !levels.empty(); }
};

std::uint32_t format_unit_bytes(DXGI_FORMAT format);
bool format_is_compressed(DXGI_FORMAT format);

bool image_from_pixels(Image& out, std::uint32_t width, std::uint32_t height, DXGI_FORMAT format,
                       const void* pixels, std::uint32_t row_pitch, std::string& error);

bool load_image(const std::filesystem::path& path, Image& out, std::string& error);

}
