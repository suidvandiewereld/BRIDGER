#include "loader/patches.h"

#include <Windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/guard.h"
#include "core/log.h"

namespace bridger::patches {
void sync_sizes();
namespace {

constexpr std::uintptr_t kMountAll = 0x1774be0;
constexpr std::uintptr_t kMount = 0x1778e30;
constexpr std::uintptr_t kStringFromCString = 0x175b570;
constexpr std::uintptr_t kStringRelease = 0x175b890;
constexpr std::uintptr_t kReadAheadAddFile = 0x1747cf0;

using MountAllFn = void (*)(void*, void*);
using MountFn = bool (*)(void*, void*, int);
using StringFromFn = void (*)(void*, const char*);
using StringReleaseFn = void (*)(void*);
using AddFileFn = void* (*)(void*, void**, std::uint64_t);

std::uintptr_t g_base = 0;
MountAllFn g_mount_all_original = nullptr;
MountFn g_mount_original = nullptr;
std::filesystem::path g_dir;
std::atomic<int> g_official = 0;
std::atomic<int> g_mounted = 0;
std::atomic<bool> g_done = false;
std::atomic<void*> g_device = nullptr;
AddFileFn g_add_file_original = nullptr;
std::mutex g_sizes_mutex;
std::unordered_map<std::uint64_t, std::uint32_t> g_patched_sizes;

std::uint64_t rotl64(std::uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

std::uint64_t fmix64(std::uint64_t k) {
    k ^= k >> 33;
    k *= 0xFF51AFD7ED558CCDull;
    k ^= k >> 33;
    k *= 0xC4CEB9FE1A85EC53ull;
    k ^= k >> 33;
    return k;
}

std::uint64_t path_hash(std::string path) {
    for (auto& c : path) {
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    path.push_back('\0');
    const std::uint64_t c1 = 0x87C37B91114253D5ull;
    const std::uint64_t c2 = 0x4CF5AD432745937Full;
    std::uint64_t h1 = 42;
    std::uint64_t h2 = 42;
    const auto* data = reinterpret_cast<const std::uint8_t*>(path.data());
    const std::size_t len = path.size();
    const std::size_t blocks = len / 16;
    for (std::size_t i = 0; i < blocks; ++i) {
        std::uint64_t k1 = 0;
        std::uint64_t k2 = 0;
        std::memcpy(&k1, data + i * 16, 8);
        std::memcpy(&k2, data + i * 16 + 8, 8);
        k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1;
        h1 = rotl64(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52DCE729;
        k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2;
        h2 = rotl64(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495AB5;
    }
    const std::uint8_t* tail = data + blocks * 16;
    const std::size_t rest = len & 15;
    std::uint64_t k1 = 0;
    std::uint64_t k2 = 0;
    for (std::size_t i = rest; i-- > 0;) {
        if (i >= 8) {
            k2 ^= static_cast<std::uint64_t>(tail[i]) << ((i - 8) * 8);
        } else {
            k1 ^= static_cast<std::uint64_t>(tail[i]) << (i * 8);
        }
    }
    if (rest > 8) {
        k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2;
    }
    if (rest > 0) {
        k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1;
    }
    h1 ^= len; h2 ^= len;
    h1 += h2; h2 += h1;
    h1 = fmix64(h1); h2 = fmix64(h2);
    h1 += h2;
    return h1;
}

const char* engine_string(void* slot) {
    const char* chars = nullptr;
    safe_read(slot, &chars, sizeof chars);
    return chars != nullptr ? chars : "";
}

bool mount_detour(void* device, void* path, int priority) {
    const bool ok = g_mount_original(device, path, priority);
    if (!g_done) {
        ++g_official;
    }
    return ok;
}

void mount_patches(void* device) {
    std::error_code error;
    std::vector<std::filesystem::path> archives;
    for (const auto& entry : std::filesystem::directory_iterator(g_dir, error)) {
        if (entry.path().extension() == L".bin") {
            archives.push_back(entry.path());
        }
    }
    std::sort(archives.begin(), archives.end());
    const auto make = reinterpret_cast<StringFromFn>(g_base + kStringFromCString);
    const auto release = reinterpret_cast<StringReleaseFn>(g_base + kStringRelease);
    for (const auto& archive : archives) {
        const std::string path = "source:Bridger/patches/" + archive.filename().string();
        const auto* bytes = static_cast<const std::uint8_t*>(device);
        int count = 0;
        int bias = 0;
        safe_read(bytes + 0x30, &count, sizeof count);
        safe_read(bytes + 0x40, &bias, sizeof bias);
        struct Work {
            MountFn mount;
            StringFromFn make;
            StringReleaseFn release;
            void* device;
            const char* path;
            int priority;
            bool ok;
        } work{g_mount_original, make, release, device, path.c_str(), count - bias, false};
        const auto fault = guarded_call([](void* user) {
            auto& w = *static_cast<Work*>(user);
            void* string = nullptr;
            w.make(&string, w.path);
            w.ok = w.mount(w.device, &string, w.priority);
            w.release(&string);
        }, &work);
        if (fault != 0) {
            log::error("patches: mounting {} faulted {:#x} {}", path, fault, describe_last_fault());
            continue;
        }
        log::info("patches: mounted {} at priority {}: {}", path, work.priority, work.ok ? "ok" : "FAILED");
        g_mounted += work.ok ? 1 : 0;
    }
}

struct ArchiveTables {
    char pad0[0x08];
    const char* path;
    char pad1[0x10];
    std::int32_t file_count;
    std::int32_t file_capacity;
    std::uint8_t* files;
};

void sync_overridden_sizes(void* device, int patch_count) {
    const auto* bytes = static_cast<const std::uint8_t*>(device);
    int count = 0;
    std::uint8_t** list = nullptr;
    safe_read(bytes + 0x30, &count, sizeof count);
    safe_read(bytes + 0x38, &list, sizeof list);
    log::info("patches: device {} archive count {} list {} patches {}", device, count,
              static_cast<void*>(list), patch_count);
    if (list == nullptr || count <= patch_count) {
        return;
    }
    std::vector<ArchiveTables*> archives;
    for (int i = 0; i < count; ++i) {
        ArchiveTables* archive = nullptr;
        safe_read(list + i, &archive, sizeof archive);
        archives.push_back(archive);
    }
    std::unordered_map<std::uint64_t, std::uint32_t> patched;
    auto is_patch = [](const ArchiveTables* a) {
        return a != nullptr && a->path != nullptr && std::strncmp(a->path, "source:Bridger/patches/", 23) == 0;
    };
    for (int i = 0; i < count; ++i) {
        const auto* a = archives[i];
        if (!is_patch(a)) {
            continue;
        }
        for (int f = 0; a != nullptr && f < a->file_count; ++f) {
            const auto* e = a->files + f * 0x20;
            std::uint64_t hash = 0;
            std::uint32_t size = 0;
            std::memcpy(&hash, e + 0x08, 8);
            std::memcpy(&size, e + 0x18, 4);
            patched[hash] = size;
        }
    }
    {
        const std::lock_guard<std::mutex> lock(g_sizes_mutex);
        g_patched_sizes = patched;
    }
    int synced = 0;
    for (int i = 0; i < count; ++i) {
        auto* a = archives[i];
        if (a == nullptr || is_patch(a)) {
            continue;
        }
        for (int f = 0; f < a->file_count;) {
            auto* e = a->files + f * 0x20;
            std::uint64_t hash = 0;
            std::memcpy(&hash, e + 0x08, 8);
            const auto it = patched.find(hash);
            if (it == patched.end()) {
                ++f;
                continue;
            }
            std::uint32_t old = 0;
            std::memcpy(&old, e + 0x18, 4);
            const int after = a->file_count - f - 1;
            if (after > 0) {
                std::memmove(e, e + 0x20, static_cast<std::size_t>(after) * 0x20);
            }
            std::memset(a->files + (a->file_count - 1) * 0x20, 0, 0x20);
            --a->file_count;
            log::info("patches: path {:016x} removed from archive {} (size {}, patch has {})", hash, i, old, it->second);
            ++synced;
        }
    }
    log::info("patches: {} overridden entries removed from the game's archives", synced);
}

void* add_file_detour(void* stream, void** name, std::uint64_t size) {
    const char* chars = name != nullptr ? static_cast<const char*>(*name) : nullptr;
    if (chars != nullptr && size != ~0ull) {
        std::int32_t length = 0;
        std::memcpy(&length, chars - 8, 4);
        if (length > 0 && length < 1024) {
            std::string path(chars, static_cast<std::size_t>(length));
            if (const auto colon = path.find(':'); colon != std::string::npos) {
                path.erase(0, colon + 1);
            }
            const std::uint64_t hash = path_hash(path);
            std::uint32_t patched = 0;
            bool found = false;
            {
                const std::lock_guard<std::mutex> lock(g_sizes_mutex);
                if (const auto it = g_patched_sizes.find(hash); it != g_patched_sizes.end()) {
                    patched = it->second;
                    found = true;
                }
            }
            if (found && patched != size) {
                log::info("patches: read-ahead size for {} corrected {} -> {}", path, size, patched);
                size = patched;
            }
        }
    }
    return g_add_file_original(stream, name, size);
}

void mount_all_detour(void* device, void* dir) {
    g_mount_all_original(device, dir);
    log::info("patches: the game mounted {} archives from {} (device {})", g_official.load(), engine_string(dir), device);
    if (!g_done.exchange(true)) {
        mount_patches(device);
        g_device = device;
        const auto* bytes = static_cast<const std::uint8_t*>(device);
        for (int waited = 0; waited < 2000; waited += 5) {
            int count = 0;
            safe_read(bytes + 0x30, &count, sizeof count);
            if (count >= g_official.load() + g_mounted.load()) {
                break;
            }
            Sleep(5);
        }
        sync_sizes();
    }
}

}

void install(std::uintptr_t base, const std::filesystem::path& root) {
    g_base = base;
    g_dir = root / L"patches";
    std::error_code error;
    if (!std::filesystem::exists(g_dir, error)) {
        return;
    }
    if (const auto status = MH_Initialize(); status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log::error("patches: MinHook init failed ({})", static_cast<int>(status));
        return;
    }
    auto* mount_all = reinterpret_cast<void*>(base + kMountAll);
    auto* mount = reinterpret_cast<void*>(base + kMount);
    auto* add_file = reinterpret_cast<void*>(base + kReadAheadAddFile);
    const bool ok = MH_CreateHook(mount_all, reinterpret_cast<void*>(&mount_all_detour),
                                  reinterpret_cast<void**>(&g_mount_all_original)) == MH_OK
                 && MH_CreateHook(mount, reinterpret_cast<void*>(&mount_detour),
                                  reinterpret_cast<void**>(&g_mount_original)) == MH_OK
                 && MH_CreateHook(add_file, reinterpret_cast<void*>(&add_file_detour),
                                  reinterpret_cast<void**>(&g_add_file_original)) == MH_OK
                 && MH_EnableHook(mount_all) == MH_OK && MH_EnableHook(mount) == MH_OK
                 && MH_EnableHook(add_file) == MH_OK;
    log::info("patches: {} for {}", ok ? "armed" : "could not hook the archive mount", g_dir.string());
}

void sync_sizes() {
    void* device = g_device.load();
    if (device == nullptr || g_mounted.load() == 0) {
        return;
    }
    if (const auto fault = guarded_call([](void* d) { sync_overridden_sizes(d, g_mounted.load()); }, device)) {
        log::error("patches: size sync faulted {:#x} {}", fault, describe_last_fault());
    }
}

int mounted() {
    return g_mounted.load();
}

}
