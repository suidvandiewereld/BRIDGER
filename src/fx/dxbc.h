#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bridger::fx::dxbc {

constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a))
         | static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8
         | static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16
         | static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24;
}

constexpr std::uint32_t kDXBC = fourcc('D', 'X', 'B', 'C');
constexpr std::uint32_t kSHEX = fourcc('S', 'H', 'E', 'X');
constexpr std::uint32_t kSHDR = fourcc('S', 'H', 'D', 'R');
constexpr std::uint32_t kISGN = fourcc('I', 'S', 'G', 'N');
constexpr std::uint32_t kISG1 = fourcc('I', 'S', 'G', '1');
constexpr std::uint32_t kOSGN = fourcc('O', 'S', 'G', 'N');
constexpr std::uint32_t kRDEF = fourcc('R', 'D', 'E', 'F');
constexpr std::uint32_t kDXIL = fourcc('D', 'X', 'I', 'L');

struct Part {
    std::uint32_t tag = 0;
    std::vector<std::uint8_t> data;
};

struct Container {
    std::uint32_t version = 1;
    std::vector<Part> parts;

    bool parse(const void* bytes, std::size_t size);
    [[nodiscard]] std::vector<std::uint8_t> build() const;
    [[nodiscard]] Part* find(std::uint32_t tag);
    [[nodiscard]] const Part* find(std::uint32_t tag) const;
};

bool is_container(const void* bytes, std::size_t size);
bool is_dxil(const void* bytes, std::size_t size);

void checksum(const void* bytes, std::size_t size, std::uint8_t out[16]);
bool verify(const void* bytes, std::size_t size);
std::uint64_t identity(const void* bytes, std::size_t size);

enum class Stage : std::uint8_t { Pixel = 0, Vertex = 1, Geometry = 2, Hull = 3, Domain = 4, Compute = 5, Unknown = 0xff };

struct Summary {
    Stage stage = Stage::Unknown;
    unsigned major = 0;
    unsigned minor = 0;
    unsigned temps = 0;
    unsigned instructions = 0;
    unsigned outputs = 0;
    bool writes_target0 = false;
    bool has_position = false;
    std::string problem;
};

Summary summarise(const void* bytes, std::size_t size);

struct SpliceResult {
    std::vector<std::uint8_t> bytecode;
    std::string error;
    unsigned insertions = 0;
    bool bindings = false;
};

SpliceResult splice_pixel_shader(const void* host, std::size_t host_size, const void* hook,
                                 std::size_t hook_size, const void* upstream = nullptr,
                                 std::size_t upstream_size = 0);

std::uint32_t output_register(const void* bytes, std::size_t size, std::uint32_t system_value);

}
