#include "fx/readback.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <cstring>
#include <mutex>
#include <vector>
#include <wrl/client.h>

#include "core/log.h"

namespace bridger::fx::readback {
namespace {

constexpr unsigned kSettleFrames = 6;

struct Pending {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    UINT64 fence_value = 0;
    std::string name;
    unsigned width = 0;
    unsigned height = 0;
    unsigned row_pitch = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned age = 0;
};

std::mutex g_mutex;
std::vector<Pending> g_pending;
std::filesystem::path g_directory;
unsigned g_written = 0;
std::string g_message;

struct Pixel {
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
};

float half_to_float(std::uint16_t value) {
    const std::uint32_t sign = (value & 0x8000u) << 16;
    std::uint32_t exponent = (value >> 10) & 0x1fu;
    std::uint32_t mantissa = value & 0x3ffu;
    if (exponent == 0) {
        if (mantissa == 0) {
            const std::uint32_t bits = sign;
            float out;
            std::memcpy(&out, &bits, sizeof out);
            return out;
        }
        while ((mantissa & 0x400u) == 0) {
            mantissa <<= 1;
            --exponent;
        }
        ++exponent;
        mantissa &= 0x3ffu;
    } else if (exponent == 31) {
        exponent = 255;
    }
    const std::uint32_t bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float out;
    std::memcpy(&out, &bits, sizeof out);
    return out;
}

std::uint8_t to_byte(float value) {
    value = value <= 0.0f ? 0.0f : value >= 1.0f ? 1.0f : value;
    return static_cast<std::uint8_t>(value * 255.0f + 0.5f);
}

bool decode_row(DXGI_FORMAT format, const std::uint8_t* source, unsigned width, Pixel* out) {
    switch (format) {
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
            for (unsigned x = 0; x < width; ++x) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, source + x * 4, sizeof bits);
                out[x].r = static_cast<std::uint8_t>(((bits >> 0) & 0x3ff) >> 2);
                out[x].g = static_cast<std::uint8_t>(((bits >> 10) & 0x3ff) >> 2);
                out[x].b = static_cast<std::uint8_t>(((bits >> 20) & 0x3ff) >> 2);
            }
            return true;
        }
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: {
            for (unsigned x = 0; x < width; ++x) {
                out[x].r = source[x * 4 + 0];
                out[x].g = source[x * 4 + 1];
                out[x].b = source[x * 4 + 2];
            }
            return true;
        }
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
            for (unsigned x = 0; x < width; ++x) {
                out[x].b = source[x * 4 + 0];
                out[x].g = source[x * 4 + 1];
                out[x].r = source[x * 4 + 2];
            }
            return true;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            for (unsigned x = 0; x < width; ++x) {
                std::uint16_t channels[4];
                std::memcpy(channels, source + x * 8, sizeof channels);
                out[x].r = to_byte(half_to_float(channels[0]));
                out[x].g = to_byte(half_to_float(channels[1]));
                out[x].b = to_byte(half_to_float(channels[2]));
            }
            return true;
        }
        case DXGI_FORMAT_R11G11B10_FLOAT: {
            for (unsigned x = 0; x < width; ++x) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, source + x * 4, sizeof bits);
                auto decode = [](std::uint32_t mantissa, std::uint32_t exponent,
                                 int mantissa_bits) {
                    if (exponent == 0) {
                        return static_cast<float>(mantissa) / static_cast<float>(1 << mantissa_bits)
                             / 16384.0f;
                    }
                    const float scale = std::ldexp(1.0f, static_cast<int>(exponent) - 15);
                    return scale * (1.0f + static_cast<float>(mantissa)
                                               / static_cast<float>(1 << mantissa_bits));
                };
                out[x].r = to_byte(decode(bits & 0x3f, (bits >> 6) & 0x1f, 6));
                out[x].g = to_byte(decode((bits >> 11) & 0x3f, (bits >> 17) & 0x1f, 6));
                out[x].b = to_byte(decode((bits >> 22) & 0x1f, (bits >> 27) & 0x1f, 5));
            }
            return true;
        }
        default:
            return false;
    }
}

bool write_bmp(const std::filesystem::path& path, const Pending& job, const std::uint8_t* mapped) {
    std::vector<Pixel> row(job.width);
    const int stride = (static_cast<int>(job.width) * 3 + 3) & ~3;
    std::vector<std::uint8_t> line(static_cast<std::size_t>(stride), 0);

    std::FILE* file = nullptr;
    if (fopen_s(&file, path.string().c_str(), "wb") != 0 || file == nullptr) {
        return false;
    }
    const std::uint32_t image_size = static_cast<std::uint32_t>(stride) * job.height;
    const std::uint32_t file_size = 54 + image_size;
    std::uint8_t header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    std::memcpy(header + 2, &file_size, 4);
    const std::uint32_t offset = 54;
    std::memcpy(header + 10, &offset, 4);
    const std::uint32_t info_size = 40;
    std::memcpy(header + 14, &info_size, 4);
    const std::int32_t width = static_cast<std::int32_t>(job.width);
    const std::int32_t height = static_cast<std::int32_t>(job.height);
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &height, 4);
    const std::uint16_t planes = 1;
    const std::uint16_t bits = 24;
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bits, 2);
    std::memcpy(header + 34, &image_size, 4);
    std::fwrite(header, 1, sizeof header, file);

    for (int y = static_cast<int>(job.height) - 1; y >= 0; --y) {
        const std::uint8_t* source = mapped + static_cast<std::size_t>(y) * job.row_pitch;
        if (!decode_row(job.format, source, job.width, row.data())) {
            std::fclose(file);
            return false;
        }
        std::memcpy(line.data(), row.data(), static_cast<std::size_t>(job.width) * 3);
        std::fwrite(line.data(), 1, line.size(), file);
    }
    std::fclose(file);
    return true;
}

void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commands->ResourceBarrier(1, &barrier);
}

}

void configure(const std::filesystem::path& directory) {
    std::scoped_lock lock(g_mutex);
    g_directory = directory;
    std::error_code ec;
    std::filesystem::create_directories(g_directory, ec);
}

bool request(ID3D12Device* device, ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES state, std::string name,
             ID3D12Fence* fence, UINT64 fence_value) {
    if (device == nullptr || commands == nullptr || resource == nullptr) {
        return false;
    }
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.Format = DXGI_FORMAT_UNKNOWN;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* destination = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&destination)))) {
        return false;
    }

    transition(commands, resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION source_location{};
    source_location.pResource = resource;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source_location.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination_location{};
    destination_location.pResource = destination;
    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination_location.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&destination_location, 0, 0, 0, &source_location, nullptr);

    transition(commands, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);

    Pending job;
    job.buffer.Attach(destination);
    job.fence = fence;
    job.fence_value = fence_value;
    job.name = std::move(name);
    job.width = static_cast<unsigned>(desc.Width);
    job.height = desc.Height;
    job.row_pitch = footprint.Footprint.RowPitch;
    job.format = desc.Format;

    std::scoped_lock lock(g_mutex);
    g_pending.push_back(job);
    return true;
}

void poll() {
    std::vector<Pending> ready;
    {
        std::scoped_lock lock(g_mutex);
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            const auto completed = it->fence ? it->fence->GetCompletedValue() : 0;
            if (it->fence && completed == UINT64_MAX) {
                g_message = "capture cancelled: device removed";
                it = g_pending.erase(it);
                continue;
            }
            if (it->fence ? completed >= it->fence_value : ++it->age >= kSettleFrames) {
                ready.push_back(*it);
                it = g_pending.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (Pending& job : ready) {
        void* mapped = nullptr;
        const D3D12_RANGE everything{0, static_cast<SIZE_T>(job.row_pitch) * job.height};
        std::string message;
        if (SUCCEEDED(job.buffer->Map(0, &everything, &mapped)) && mapped != nullptr) {
            const auto path = g_directory / (job.name + ".bmp");
            if (write_bmp(path, job, static_cast<const std::uint8_t*>(mapped))) {
                message = std::format("wrote {} ({}x{})", path.filename().string(), job.width,
                                      job.height);
                log::info("fx/readback: {}", message);
                std::scoped_lock lock(g_mutex);
                ++g_written;
            } else {
                message = std::format("{}: format {} cannot be written", job.name,
                                      static_cast<int>(job.format));
                log::warn("fx/readback: {}", message);
            }
            const D3D12_RANGE nothing{0, 0};
            job.buffer->Unmap(0, &nothing);
        } else {
            message = job.name + ": readback map failed";
            log::warn("fx/readback: {}", message);
        }
        std::scoped_lock lock(g_mutex);
        g_message = message;
    }
}

unsigned written() {
    std::scoped_lock lock(g_mutex);
    return g_written;
}

std::string last_message() {
    std::scoped_lock lock(g_mutex);
    return g_message;
}

unsigned pending() {
    std::scoped_lock lock(g_mutex);
    return static_cast<unsigned>(g_pending.size());
}

}
