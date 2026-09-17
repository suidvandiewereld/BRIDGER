#pragma once

#include <cstdint>
#include <vector>

#include "ui/draw.h"

namespace bridger::ui::raster {

void render(const DrawList& list, const std::vector<std::uint8_t>& atlas, int atlas_size,
            std::uint32_t* pixels, int width, int height);

void clear(std::uint32_t* pixels, int width, int height, Color color);

}
