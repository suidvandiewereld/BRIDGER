#include "fx/image.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace bridger::fx {
namespace {

constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8)
         | (static_cast<std::uint32_t>(c) << 16) | (static_cast<std::uint32_t>(d) << 24);
}

#pragma pack(push, 1)
struct DdsPixelFormat {
    std::uint32_t size;
    std::uint32_t flags;
    std::uint32_t four_cc;
    std::uint32_t rgb_bit_count;
    std::uint32_t r_mask;
    std::uint32_t g_mask;
    std::uint32_t b_mask;
    std::uint32_t a_mask;
};

struct DdsHeader {
    std::uint32_t size;
    std::uint32_t flags;
    std::uint32_t height;
    std::uint32_t width;
    std::uint32_t pitch_or_linear_size;
    std::uint32_t depth;
    std::uint32_t mip_map_count;
    std::uint32_t reserved1[11];
    DdsPixelFormat pixel_format;
    std::uint32_t caps;
    std::uint32_t caps2;
    std::uint32_t caps3;
    std::uint32_t caps4;
    std::uint32_t reserved2;
};

struct DdsHeaderDx10 {
    std::uint32_t dxgi_format;
    std::uint32_t resource_dimension;
    std::uint32_t misc_flag;
    std::uint32_t array_size;
    std::uint32_t misc_flags2;
};

struct TgaHeader {
    std::uint8_t id_length;
    std::uint8_t color_map_type;
    std::uint8_t image_type;
    std::uint16_t color_map_origin;
    std::uint16_t color_map_length;
    std::uint8_t color_map_depth;
    std::uint16_t x_origin;
    std::uint16_t y_origin;
    std::uint16_t width;
    std::uint16_t height;
    std::uint8_t bits_per_pixel;
    std::uint8_t descriptor;
};
#pragma pack(pop)

static_assert(sizeof(DdsHeader) == 124);
static_assert(sizeof(TgaHeader) == 18);

constexpr std::uint32_t kDdpfFourCc = 0x4;
constexpr std::uint32_t kDdpfRgb = 0x40;
constexpr std::uint32_t kDdpfAlphaPixels = 0x1;
constexpr std::uint32_t kDdpfLuminance = 0x20000;

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        error = "could not open " + path.string();
        return false;
    }
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    stream.seekg(0, std::ios::beg);
    if (size <= 0) {
        error = "empty file " + path.string();
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(out.data()), size);
    if (!stream) {
        error = "short read on " + path.string();
        return false;
    }
    return true;
}

bool layout_levels(Image& image, std::uint32_t mips) {
    const auto unit = format_unit_bytes(image.format);
    if (unit == 0) {
        return false;
    }
    const bool compressed = format_is_compressed(image.format);
    image.levels.clear();
    std::size_t offset = 0;
    std::uint32_t width = image.width;
    std::uint32_t height = image.height;
    for (std::uint32_t mip = 0; mip < std::max(1u, mips); ++mip) {
        Image::Level level;
        level.offset = offset;
        level.width = width;
        level.height = height;
        if (compressed) {
            level.row_pitch = std::max(1u, (width + 3) / 4) * unit;
            level.rows = std::max(1u, (height + 3) / 4);
        } else {
            level.row_pitch = width * unit;
            level.rows = height;
        }
        offset += static_cast<std::size_t>(level.row_pitch) * level.rows;
        image.levels.push_back(level);
        if (width == 1 && height == 1) {
            break;
        }
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    image.data.assign(offset, 0);
    return true;
}

DXGI_FORMAT dds_format(const DdsPixelFormat& pf, const DdsHeaderDx10* dx10, bool& needs_bgr_expand) {
    needs_bgr_expand = false;
    if ((pf.flags & kDdpfFourCc) != 0) {
        switch (pf.four_cc) {
            case fourcc('D', 'X', 'T', '1'): return DXGI_FORMAT_BC1_UNORM;
            case fourcc('D', 'X', 'T', '2'):
            case fourcc('D', 'X', 'T', '3'): return DXGI_FORMAT_BC2_UNORM;
            case fourcc('D', 'X', 'T', '4'):
            case fourcc('D', 'X', 'T', '5'): return DXGI_FORMAT_BC3_UNORM;
            case fourcc('A', 'T', 'I', '1'):
            case fourcc('B', 'C', '4', 'U'): return DXGI_FORMAT_BC4_UNORM;
            case fourcc('B', 'C', '4', 'S'): return DXGI_FORMAT_BC4_SNORM;
            case fourcc('A', 'T', 'I', '2'):
            case fourcc('B', 'C', '5', 'U'): return DXGI_FORMAT_BC5_UNORM;
            case fourcc('B', 'C', '5', 'S'): return DXGI_FORMAT_BC5_SNORM;
            case 36: return DXGI_FORMAT_R16G16B16A16_UNORM;
            case 113: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case 114: return DXGI_FORMAT_R32_FLOAT;
            case 116: return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case fourcc('D', 'X', '1', '0'):
                if (dx10 == nullptr) {
                    return DXGI_FORMAT_UNKNOWN;
                }
                switch (static_cast<DXGI_FORMAT>(dx10->dxgi_format)) {
                    case DXGI_FORMAT_BC1_UNORM:
                    case DXGI_FORMAT_BC1_UNORM_SRGB:
                    case DXGI_FORMAT_BC2_UNORM:
                    case DXGI_FORMAT_BC2_UNORM_SRGB:
                    case DXGI_FORMAT_BC3_UNORM:
                    case DXGI_FORMAT_BC3_UNORM_SRGB:
                    case DXGI_FORMAT_BC4_UNORM:
                    case DXGI_FORMAT_BC4_SNORM:
                    case DXGI_FORMAT_BC5_UNORM:
                    case DXGI_FORMAT_BC5_SNORM:
                    case DXGI_FORMAT_BC6H_UF16:
                    case DXGI_FORMAT_BC6H_SF16:
                    case DXGI_FORMAT_BC7_UNORM:
                    case DXGI_FORMAT_BC7_UNORM_SRGB:
                    case DXGI_FORMAT_R8G8B8A8_UNORM:
                    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                    case DXGI_FORMAT_B8G8R8A8_UNORM:
                    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                    case DXGI_FORMAT_R8_UNORM:
                    case DXGI_FORMAT_R8G8_UNORM:
                    case DXGI_FORMAT_R16_FLOAT:
                    case DXGI_FORMAT_R16G16_FLOAT:
                    case DXGI_FORMAT_R16G16B16A16_FLOAT:
                    case DXGI_FORMAT_R16G16B16A16_UNORM:
                    case DXGI_FORMAT_R32_FLOAT:
                    case DXGI_FORMAT_R32G32_FLOAT:
                    case DXGI_FORMAT_R32G32B32A32_FLOAT:
                    case DXGI_FORMAT_R11G11B10_FLOAT:
                    case DXGI_FORMAT_R10G10B10A2_UNORM:
                        return static_cast<DXGI_FORMAT>(dx10->dxgi_format);
                    default:
                        return DXGI_FORMAT_UNKNOWN;
                }
            default:
                return DXGI_FORMAT_UNKNOWN;
        }
    }
    if ((pf.flags & kDdpfRgb) != 0) {
        if (pf.rgb_bit_count == 32) {
            if (pf.r_mask == 0x000000ff && pf.g_mask == 0x0000ff00 && pf.b_mask == 0x00ff0000) {
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            }
            if (pf.r_mask == 0x00ff0000 && pf.g_mask == 0x0000ff00 && pf.b_mask == 0x000000ff) {
                return DXGI_FORMAT_B8G8R8A8_UNORM;
            }
            if (pf.r_mask == 0x3ff && pf.g_mask == 0xffc00 && pf.b_mask == 0x3ff00000) {
                return DXGI_FORMAT_R10G10B10A2_UNORM;
            }
        }
        if (pf.rgb_bit_count == 24 && pf.r_mask == 0x00ff0000 && pf.g_mask == 0x0000ff00
                && pf.b_mask == 0x000000ff) {
            needs_bgr_expand = true;
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }
    if ((pf.flags & kDdpfLuminance) != 0 && pf.rgb_bit_count == 8) {
        return DXGI_FORMAT_R8_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

bool load_dds(const std::vector<std::uint8_t>& bytes, Image& out, std::string& error) {
    if (bytes.size() < 4 + sizeof(DdsHeader)) {
        error = "dds: file too small";
        return false;
    }
    std::uint32_t magic = 0;
    std::memcpy(&magic, bytes.data(), 4);
    if (magic != fourcc('D', 'D', 'S', ' ')) {
        error = "dds: bad magic";
        return false;
    }
    DdsHeader header{};
    std::memcpy(&header, bytes.data() + 4, sizeof header);
    if (header.size != 124 || header.pixel_format.size != 32) {
        error = "dds: malformed header";
        return false;
    }
    std::size_t offset = 4 + sizeof(DdsHeader);
    DdsHeaderDx10 dx10{};
    const bool has_dx10 = (header.pixel_format.flags & kDdpfFourCc) != 0
                       && header.pixel_format.four_cc == fourcc('D', 'X', '1', '0');
    if (has_dx10) {
        if (bytes.size() < offset + sizeof dx10) {
            error = "dds: truncated DX10 header";
            return false;
        }
        std::memcpy(&dx10, bytes.data() + offset, sizeof dx10);
        offset += sizeof dx10;
        if (dx10.resource_dimension != 3 && dx10.resource_dimension != 0) {
            error = "dds: only 2D textures are supported";
            return false;
        }
    }

    bool expand_bgr = false;
    const DXGI_FORMAT format = dds_format(header.pixel_format, has_dx10 ? &dx10 : nullptr, expand_bgr);
    if (format == DXGI_FORMAT_UNKNOWN) {
        error = "dds: unsupported pixel format";
        return false;
    }
    if (header.width == 0 || header.height == 0 || header.width > 16384 || header.height > 16384) {
        error = "dds: bad dimensions";
        return false;
    }

    out.width = header.width;
    out.height = header.height;
    out.format = format;
    const std::uint32_t mips = std::max(1u, header.mip_map_count);
    if (!layout_levels(out, mips)) {
        error = "dds: unsupported format layout";
        return false;
    }

    for (const auto& level : out.levels) {
        const std::size_t source_pitch = expand_bgr ? static_cast<std::size_t>(level.width) * 3
                                                    : level.row_pitch;
        const std::size_t needed = source_pitch * level.rows;
        if (bytes.size() < offset + needed) {
            error = "dds: truncated pixel data";
            return false;
        }
        for (std::uint32_t row = 0; row < level.rows; ++row) {
            const std::uint8_t* source = bytes.data() + offset + row * source_pitch;
            std::uint8_t* destination = out.data.data() + level.offset
                                      + static_cast<std::size_t>(row) * level.row_pitch;
            if (expand_bgr) {
                for (std::uint32_t x = 0; x < level.width; ++x) {
                    destination[x * 4 + 0] = source[x * 3 + 2];
                    destination[x * 4 + 1] = source[x * 3 + 1];
                    destination[x * 4 + 2] = source[x * 3 + 0];
                    destination[x * 4 + 3] = 255;
                }
            } else {
                std::memcpy(destination, source, level.row_pitch);
            }
        }
        offset += needed;
    }
    return true;
}

bool load_tga(const std::vector<std::uint8_t>& bytes, Image& out, std::string& error) {
    if (bytes.size() < sizeof(TgaHeader)) {
        error = "tga: file too small";
        return false;
    }
    TgaHeader header{};
    std::memcpy(&header, bytes.data(), sizeof header);
    const bool rle = header.image_type == 10 || header.image_type == 11;
    const bool grey = header.image_type == 3 || header.image_type == 11;
    if (header.image_type != 2 && header.image_type != 3 && !rle) {
        error = "tga: only uncompressed or RLE truecolor/greyscale images are supported";
        return false;
    }
    if (header.color_map_type != 0) {
        error = "tga: colour-mapped images are not supported";
        return false;
    }
    const std::uint32_t bpp = header.bits_per_pixel;
    if ((grey && bpp != 8) || (!grey && bpp != 24 && bpp != 32)) {
        error = "tga: unsupported bit depth";
        return false;
    }
    if (header.width == 0 || header.height == 0) {
        error = "tga: bad dimensions";
        return false;
    }

    out.width = header.width;
    out.height = header.height;
    out.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    layout_levels(out, 1);

    const std::size_t pixel_count = static_cast<std::size_t>(out.width) * out.height;
    const std::uint32_t bytes_per_pixel = bpp / 8;
    std::size_t offset = sizeof(TgaHeader) + header.id_length;
    const bool top_left = (header.descriptor & 0x20) != 0;

    auto store = [&](std::size_t index, const std::uint8_t* pixel) {
        const std::size_t x = index % out.width;
        const std::size_t y = index / out.width;
        const std::size_t row = top_left ? y : (out.height - 1 - y);
        std::uint8_t* destination = out.data.data() + (row * out.width + x) * 4;
        if (grey) {
            destination[0] = destination[1] = destination[2] = pixel[0];
            destination[3] = 255;
        } else {
            destination[0] = pixel[2];
            destination[1] = pixel[1];
            destination[2] = pixel[0];
            destination[3] = bytes_per_pixel == 4 ? pixel[3] : 255;
        }
    };

    if (!rle) {
        if (bytes.size() < offset + pixel_count * bytes_per_pixel) {
            error = "tga: truncated pixel data";
            return false;
        }
        for (std::size_t i = 0; i < pixel_count; ++i) {
            store(i, bytes.data() + offset + i * bytes_per_pixel);
        }
        return true;
    }

    std::size_t written = 0;
    while (written < pixel_count) {
        if (offset >= bytes.size()) {
            error = "tga: truncated RLE data";
            return false;
        }
        const std::uint8_t packet = bytes[offset++];
        const std::size_t count = static_cast<std::size_t>(packet & 0x7f) + 1;
        if ((packet & 0x80) != 0) {
            if (offset + bytes_per_pixel > bytes.size()) {
                error = "tga: truncated RLE run";
                return false;
            }
            for (std::size_t i = 0; i < count && written < pixel_count; ++i) {
                store(written++, bytes.data() + offset);
            }
            offset += bytes_per_pixel;
        } else {
            if (offset + count * bytes_per_pixel > bytes.size()) {
                error = "tga: truncated RLE literal";
                return false;
            }
            for (std::size_t i = 0; i < count && written < pixel_count; ++i) {
                store(written++, bytes.data() + offset + i * bytes_per_pixel);
            }
            offset += count * bytes_per_pixel;
        }
    }
    return true;
}

}

std::uint32_t format_unit_bytes(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
            return 1;
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
            return 2;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return 4;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 16;
        default:
            return 0;
    }
}

bool format_is_compressed(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

bool image_from_pixels(Image& out, std::uint32_t width, std::uint32_t height, DXGI_FORMAT format,
                       const void* pixels, std::uint32_t row_pitch, std::string& error) {
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        error = "texture dimensions must be 1..16384";
        return false;
    }
    if (format_is_compressed(format)) {
        error = "block compressed textures can only be loaded from a .dds file";
        return false;
    }
    out = Image{};
    out.width = width;
    out.height = height;
    out.format = format;
    if (!layout_levels(out, 1)) {
        error = "unsupported texture format";
        return false;
    }
    if (pixels == nullptr) {
        return true;
    }
    const auto& level = out.levels[0];
    const std::uint32_t source_pitch = row_pitch == 0 ? level.row_pitch : row_pitch;
    if (source_pitch < level.row_pitch) {
        error = "row pitch is smaller than a row of pixels";
        return false;
    }
    const auto* source = static_cast<const std::uint8_t*>(pixels);
    for (std::uint32_t row = 0; row < level.rows; ++row) {
        std::memcpy(out.data.data() + level.offset + static_cast<std::size_t>(row) * level.row_pitch,
                    source + static_cast<std::size_t>(row) * source_pitch, level.row_pitch);
    }
    return true;
}

bool load_image(const std::filesystem::path& path, Image& out, std::string& error) {
    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes, error)) {
        return false;
    }
    out = Image{};
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".dds") {
        return load_dds(bytes, out, error);
    }
    if (extension == ".tga") {
        return load_tga(bytes, out, error);
    }
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0) {
        return load_dds(bytes, out, error);
    }
    error = "unsupported image type (use .dds or .tga)";
    return false;
}

}
