#include <Windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstring>

void detour() {}

int main() {
    const unsigned char prefix[] = {
        0x49, 0x89, 0xe3, 0x53, 0x48, 0x81, 0xec, 0xb0, 0, 0, 0,
        0xc4, 0xc1, 0x78, 0x29, 0x7b, 0xc8
    };
    auto* code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code || MH_Initialize() != MH_OK) return 1;
    std::memcpy(code, prefix, sizeof(prefix));
    void* original = nullptr;
    const auto status = MH_CreateHook(code, reinterpret_cast<void*>(&detour), &original);
    std::printf("Player update hook: %s\n", MH_StatusToString(status));
    bool ok = status == MH_OK;
    if (ok) {
        ok = std::memcmp(original, prefix, 11) == 0;
        ok = MH_EnableHook(code) == MH_OK && ok;
        ok = MH_RemoveHook(code) == MH_OK && ok;
        ok = std::memcmp(code, prefix, sizeof(prefix)) == 0 && ok;
    }
    std::memcpy(code, prefix + 11, 6);
    const auto unsupported = MH_CreateHook(code, reinterpret_cast<void*>(&detour), &original);
    ok = unsupported == MH_ERROR_UNSUPPORTED_FUNCTION && ok;
    std::printf("AVX inside patch rejected: %s\n", MH_StatusToString(unsupported));
    const unsigned char range[] = {0x48,0x83,0xec,0x18,0xc5,0xfb,0x10,0x41,0x30};
    const unsigned char exclusion[] = {0xc5,0xfb,0x10,0x52,0x10};
    auto check_prefix = [&](const unsigned char* bytes, size_t length, const char* name) {
        std::memset(code, 0xcc, 64);
        std::memcpy(code, bytes, length);
        const auto result = MH_CreateHook(code, reinterpret_cast<void*>(&detour), &original);
        std::printf("%s: %s\n", name, MH_StatusToString(result));
        bool passed = result == MH_OK;
        if (passed) {
            passed = std::memcmp(original, bytes, length) == 0;
            passed = MH_EnableHook(code) == MH_OK && passed;
            passed = MH_RemoveHook(code) == MH_OK && passed;
            passed = std::memcmp(code, bytes, length) == 0 && passed;
        }
        return passed;
    };
    ok = check_prefix(range, sizeof(range), "Catcher range prefix") && ok;
    ok = check_prefix(exclusion, sizeof(exclusion), "Catcher exclusion prefix") && ok;
    const unsigned char rip[] = {0xc5,0xfb,0x10,0x05,0,0,0,0};
    const unsigned char sib[] = {0xc5,0xfb,0x10,0x44,0x24,0x10};
    const unsigned char* unsupported_forms[] = {rip, sib};
    for (const auto* bytes : unsupported_forms) {
        std::memcpy(code, bytes, 6);
        const auto rejected = MH_CreateHook(code, reinterpret_cast<void*>(&detour), &original);
        ok = rejected == MH_ERROR_UNSUPPORTED_FUNCTION && ok;
        if (rejected == MH_OK) MH_RemoveHook(code);
    }
    MH_Uninitialize();
    VirtualFree(code, 0, MEM_RELEASE);
    return ok ? 0 : 1;
}
