
#include <Windows.h>
#include <MinHook.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/guard.h"
#include "script/ffi.h"

using namespace bridger::script::ffi;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "  ok  " : "  FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

bool close_to(double a, double b) {
    return std::fabs(a - b) < 1e-6;
}

struct Big {
    double x, y, z;
};

__declspec(noinline) double mix10(int a, double b, float c, std::int64_t d, float e, int f,
                                  double g, char h, std::uint64_t i, float j) {
    return a * 1.0 + b * 2.0 + c * 3.0 + static_cast<double>(d) * 4.0 + e * 5.0 + f * 6.0
         + g * 7.0 + h * 8.0 + static_cast<double>(i) * 9.0 + j * 10.0;
}

__declspec(noinline) float float_result(float a, int b) {
    return a * static_cast<float>(b) + 0.25f;
}

__declspec(noinline) Big make_big(double x, double y, int scale) {
    return Big{x * scale, y * scale, x + y};
}

__declspec(noinline) double sum_ref(const Big& b, double extra) {
    return b.x + b.y + b.z + extra;
}

__declspec(noinline) double sum_value(Big b, float extra) {
    return b.x * 2.0 + b.y + b.z + extra;
}

__declspec(noinline) int read_through(const int* p) {
    return *p + 1;
}

__declspec(noinline) int add3(int a, int b, int c) {
    volatile int sum = a + b;
    return sum + c;
}

__declspec(noinline) double six(float v, double k, int n, float w, double tail5, int tail6) {
    volatile double r = v * k + n + w * 10.0 + tail5 * 100.0 + tail6 * 1000.0;
    return r;
}

std::uint64_t add3_handler(void* user, Frame& frame) {
    ++*static_cast<int*>(user);
    frame.ints[0] = static_cast<std::uint32_t>(static_cast<int>(frame.ints[0]) * 2);
    Result result;
    frame.call_original(result);
    return static_cast<std::uint32_t>(static_cast<int>(result.rax) + 1000);
}

std::uint64_t six_handler(void*, Frame& frame) {
    frame.xmms[1] = 3.0;
    frame.ints[4] = pack_f64(2.0);
    Result result;
    frame.call_original(result);
    *frame.xmm0_out = result.xmm0 + 0.5;
    return 0;
}

std::uint64_t passthrough_handler(void*, Frame& frame) {
    Result result;
    frame.call_original(result);
    *frame.xmm0_out = result.xmm0;
    return result.rax;
}

std::uint32_t info(std::string_view name, bool& is_enum) {
    is_enum = name == "EDSArea";
    if (is_enum) return 4;
    if (name == "WorldPosition") return 24;
    if (name == "Vec3") return 12;
    if (name == "IVec2") return 8;
    if (name == "String") return 8;
    if (name == "Entity") return 336;
    return 0;
}

void test_calls() {
    std::puts("calls");
    {
        std::uint64_t ints[10] = {
            static_cast<std::uint64_t>(-3), pack_f64(1.5), pack_f32(2.25f),
            static_cast<std::uint64_t>(-40000000000ll), pack_f32(-1.5f), 7,
            pack_f64(0.125), static_cast<std::uint64_t>(static_cast<unsigned char>('A')), 9000000000ull,
            pack_f32(3.5f)};
        double xmms[4] = {unpack_f64(ints[0]), unpack_f64(ints[1]), unpack_f64(ints[2]),
                          unpack_f64(ints[3])};
        Result result;
        const auto fault = call(reinterpret_cast<void*>(&mix10), ints, xmms, 10, result);
        const double expected = mix10(-3, 1.5, 2.25f, -40000000000ll, -1.5f, 7, 0.125, 'A',
                                      9000000000ull, 3.5f);
        check(fault == 0 && close_to(result.xmm0, expected), "ten mixed arguments, six on the stack");
    }
    {
        std::uint64_t ints[4] = {pack_f32(1.5f), 4, 0, 0};
        double xmms[4] = {unpack_f64(ints[0]), unpack_f64(ints[1]), 0, 0};
        Result result;
        call(reinterpret_cast<void*>(&float_result), ints, xmms, 2, result);
        check(close_to(unpack_f32(pack_f64(result.xmm0)), 6.25), "float result from xmm0");
    }
    {
        Big out{};
        std::uint64_t ints[4] = {reinterpret_cast<std::uint64_t>(&out), pack_f64(2.0),
                                 pack_f64(3.0), 10};
        double xmms[4] = {0, 2.0, 3.0, 0};
        Result result;
        call(reinterpret_cast<void*>(&make_big), ints, xmms, 4, result);
        check(result.rax == reinterpret_cast<std::uint64_t>(&out) && close_to(out.x, 20.0)
                  && close_to(out.y, 30.0) && close_to(out.z, 5.0),
              "hidden struct result");
    }
    {
        Big in{1, 2, 3};
        std::uint64_t ints[4] = {reinterpret_cast<std::uint64_t>(&in), pack_f64(0.5), 0, 0};
        double xmms[4] = {0, 0.5, 0, 0};
        Result result;
        call(reinterpret_cast<void*>(&sum_ref), ints, xmms, 2, result);
        check(close_to(result.xmm0, 6.5), "struct by reference");
    }
    {
        Big copy{1, 2, 3};
        std::uint64_t ints[4] = {reinterpret_cast<std::uint64_t>(&copy), pack_f32(0.25f), 0, 0};
        double xmms[4] = {0, unpack_f64(ints[1]), 0, 0};
        Result result;
        call(reinterpret_cast<void*>(&sum_value), ints, xmms, 2, result);
        check(close_to(result.xmm0, 7.25), "large struct by value, through the caller's copy");
    }
    {
        std::uint64_t ints[4] = {0, 0, 0, 0};
        double xmms[4] = {};
        Result result;
        const auto fault = call(reinterpret_cast<void*>(&read_through), ints, xmms, 1, result);
        check(fault == EXCEPTION_ACCESS_VIOLATION, "a fault unwinds through the call frame");
        int value = 41;
        ints[0] = reinterpret_cast<std::uint64_t>(&value);
        const auto clean = call(reinterpret_cast<void*>(&read_through), ints, xmms, 1, result);
        check(clean == 0 && result.rax == 42, "and the next call is unaffected");
    }
}

void test_hooks() {
    std::puts("hooks");
    std::string error;
    int calls = 0;
    const int slot = create_hook(reinterpret_cast<void*>(&add3), 3, add3_handler, &calls, error);
    check(slot >= 0, "create an integer hook");
    check(add3(1, 2, 3) == 6, "not live until enabled");
    check(enable_hook(slot), "enable it");
    check(add3(1, 2, 3) == 1007 && calls == 1, "handler rewrites an argument and the result");
    check(enable_hook(slot) && add3(1, 2, 3) == 1007, "enabling a live hook again keeps it live");

    const int second = create_hook(reinterpret_cast<void*>(&add3), 3, add3_handler, &calls, error);
    check(second < 0 && !error.empty(), "a second hook on the same target is refused");

    check(remove_hook(slot, 100), "remove it");
    check(add3(1, 2, 3) == 6 && calls == 2, "the function is its own again");

    const int floating = create_hook(reinterpret_cast<void*>(&six), 6, six_handler, nullptr, error);
    check(floating >= 0 && enable_hook(floating), "hook a function with stack arguments");
    const double hooked = six(1.0f, 9.0, 2, 0.5f, 7.0, 3);
    const double expected = 1.0 * 3.0 + 2 + 0.5 * 10.0 + 2.0 * 100.0 + 3 * 1000.0 + 0.5;
    check(close_to(hooked, expected), "xmm and stack arguments rewritten, xmm0 result replaced");
    remove_hook(floating, 100);

    const int through =
        create_hook(reinterpret_cast<void*>(&six), 6, passthrough_handler, nullptr, error);
    check(through >= 0 && enable_hook(through), "rehook the freed target");
    check(close_to(six(1.0f, 9.0, 2, 0.5f, 7.0, 3), 1.0 * 9.0 + 2 + 5.0 + 700.0 + 3000.0),
          "passthrough is exact");
    check(disable_hook(through), "disable");
    check(close_to(six(2.0f, 1.0, 0, 0.0f, 0.0, 0), 2.0), "disabled hook is inert");
    remove_hook(through, 100);
}

LONG CALLBACK host_crash_handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        std::puts("  FAIL the host's crash handler saw a guarded fault");
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 99);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void test_host_handler() {
    std::puts("faults under a host crash handler");
    bridger::promote_fault_handler();
    const PVOID host = AddVectoredExceptionHandler(1, host_crash_handler);
    bridger::promote_fault_handler();

    int buffer = 0;
    check(!bridger::safe_read(reinterpret_cast<const void*>(0x10), &buffer, sizeof buffer),
          "a bad read fails without raising");

    std::uint64_t ints[4] = {0, 0, 0, 0};
    double xmms[4] = {};
    Result result;
    const auto fault = call(reinterpret_cast<void*>(&read_through), ints, xmms, 1, result);
    check(fault == EXCEPTION_ACCESS_VIOLATION, "a faulting call is recovered ahead of the host");
    int value = 6;
    ints[0] = reinterpret_cast<std::uint64_t>(&value);
    check(call(reinterpret_cast<void*>(&read_through), ints, xmms, 1, result) == 0 && result.rax == 7,
          "and the thread carries on normally");

    struct Nest {
        std::uint32_t inner = 0;
    } nest;
    const auto outer = bridger::guarded_call(
        [](void* user) {
            static_cast<Nest*>(user)->inner = bridger::guarded_call(
                [](void*) { volatile int* p = nullptr; *p = 1; }, nullptr);
        },
        &nest);
    check(outer == 0 && nest.inner == EXCEPTION_ACCESS_VIOLATION, "nested guards recover innermost");
    RemoveVectoredExceptionHandler(host);
}

void test_signatures() {
    std::puts("signatures");
    const TypeInfo types = info;
    check(classify("WorldPosition const &", types).kind == Kind::Pointer, "reference is a pointer");
    check(classify("AIHTNPlannerDaemon * *", types).depth == 2, "pointer depth");
    check(classify("EDSArea", types).kind == Kind::Enum, "enum by value");
    check(classify("IVec2", types).kind == Kind::StructValue, "8 byte struct in a register");
    check(classify("WorldPosition", types).kind == Kind::StructMemory, "24 byte struct by copy");

    const auto position = from_tokens({"WorldPosition", "Entity *"}, types);
    check(position.hidden_result() && position.slot_count() == 2, "large result is hidden");
    const auto text = from_tokens({"String", "uint32"}, types);
    check(text.hidden_result(), "String result is non-trivial, so hidden");
    const auto small = from_tokens({"IVec2"}, types);
    check(!small.hidden_result(), "trivial 8 byte result in rax");

    Signature parsed;
    std::string error;
    check(parse("float get(Entity* entity, f32 scale, unsigned int count)", types, parsed, error)
              && parsed.result.kind == Kind::F32 && parsed.params.size() == 3
              && parsed.params[0].kind == Kind::Pointer && parsed.params[0].name == "Entity"
              && parsed.params[2].kind == Kind::U32,
          "hand written signature with names");
    check(parse("void()", types, parsed, error) && parsed.params.empty(), "empty parameter list");
    check(!parse("void(Nonsense)", types, parsed, error), "unknown by-value type is rejected");
    std::printf("         %s\n", error.c_str());
    check(parse("Vec3(WorldPosition const&, bool)", types, parsed, error)
              && parsed.describe() == "Vec3(WorldPosition&, bool)",
          "describe round trip");
}

}

int main() {
    if (MH_Initialize() != MH_OK) {
        std::puts("MinHook failed to initialise");
        return 1;
    }
    test_calls();
    test_host_handler();
    test_hooks();
    test_signatures();
    MH_Uninitialize();
    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "passed" : "FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
