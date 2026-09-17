#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bridger::script::ffi {

enum class Kind : std::uint8_t {
    Void,
    Bool,
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    I64,
    U64,
    F32,
    F64,
    Pointer,
    Enum,
    StructValue,
    StructMemory,
};

struct Type {
    Kind kind = Kind::Void;
    std::uint32_t size = 0;
    std::string name;
    bool reference = false;
    bool is_const = false;
    int depth = 0;

    [[nodiscard]] bool floating() const { return kind == Kind::F32 || kind == Kind::F64; }
};

struct Signature {
    Type result;
    std::vector<Type> params;

    [[nodiscard]] bool hidden_result() const { return result.kind == Kind::StructMemory; }
    [[nodiscard]] std::size_t slot_count() const {
        return params.size() + (hidden_result() ? 1 : 0);
    }
    [[nodiscard]] std::string describe() const;
};

using TypeInfo = std::function<std::uint32_t(std::string_view name, bool& is_enum)>;

Type classify(std::string_view token, const TypeInfo& info);

Signature from_tokens(const std::vector<std::string>& tokens, const TypeInfo& info);

bool parse(std::string_view text, const TypeInfo& info, Signature& out, std::string& error);

struct Result {
    std::uint64_t rax = 0;
    double xmm0 = 0.0;
};

std::uint32_t call(void* fn, const std::uint64_t* ints, const double* xmms, std::size_t count,
                   Result& out);

std::uint64_t pack_f32(float value);
std::uint64_t pack_f64(double value);
float unpack_f32(std::uint64_t bits);
double unpack_f64(std::uint64_t bits);

inline constexpr std::size_t kHookSlots = 256;

struct Frame {
    std::uint64_t* ints = nullptr;
    double* xmms = nullptr;
    double* xmm0_out = nullptr;
    void* trampoline = nullptr;
    std::size_t slots = 0;

    std::uint32_t call_original(Result& out) const;
};

using Handler = std::uint64_t (*)(void* user, Frame& frame);

int create_hook(void* target, std::size_t slots, Handler handler, void* user, std::string& error);
bool enable_hook(int slot);
bool disable_hook(int slot);
bool remove_hook(int slot, unsigned wait_ms);
[[nodiscard]] int in_flight(int slot);
[[nodiscard]] void* hook_target(int slot);

int create_callback(std::size_t slots, Handler handler, void* user, void* fallback,
                    void*& address, std::string& error);
bool release_callback(int slot, unsigned wait_ms);

}
