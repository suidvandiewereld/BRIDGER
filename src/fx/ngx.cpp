#include "fx/ngx.h"

#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

#include "core/guard.h"
#include "core/log.h"
#include "fx/readback.h"
#include "fx/fx.h"

namespace bridger::fx::ngx {
namespace {

constexpr int kSuccess = 0x1;

constexpr unsigned kResetWindow = 600;

constexpr const char* kColor = "Color";
constexpr const char* kDepth = "Depth";
constexpr const char* kMotionVectors = "MotionVectors";
constexpr const char* kOutput = "Output";
constexpr const char* kJitterX = "Jitter.Offset.X";
constexpr const char* kJitterY = "Jitter.Offset.Y";
constexpr const char* kMvScaleX = "MV.Scale.X";
constexpr const char* kMvScaleY = "MV.Scale.Y";
constexpr const char* kSharpness = "Sharpness";
constexpr const char* kReset = "Reset";
constexpr const char* kSubrectWidth = "DLSS.Render.Subrect.Dimensions.Width";
constexpr const char* kSubrectHeight = "DLSS.Render.Subrect.Dimensions.Height";
constexpr const char* kCreateWidth = "Width";
constexpr const char* kCreateHeight = "Height";
constexpr const char* kCreateOutWidth = "OutWidth";
constexpr const char* kCreateOutHeight = "OutHeight";

constexpr const char* kPresetKeys[] = {
    "DLSS.Hint.Render.Preset.DLAA",
    "DLSS.Hint.Render.Preset.Quality",
    "DLSS.Hint.Render.Preset.Balanced",
    "DLSS.Hint.Render.Preset.Performance",
    "DLSS.Hint.Render.Preset.UltraPerformance",
    "DLSS.Hint.Render.Preset.UltraQuality",
};

constexpr const char* kProbeKey = "Bridger.SetterProbe";
constexpr unsigned kProbeValue = 0x5a5au;

using EvaluateFn = int (*)(ID3D12GraphicsCommandList*, const void*, const void*, void*);
using CreateFeatureFn = int (*)(ID3D12GraphicsCommandList*, unsigned, const void*, void**);
using RememberLayoutFn = void (*)(const char*);

struct Hook {
    const wchar_t* module = nullptr;
    const char* name = nullptr;
    void* target = nullptr;
    EvaluateFn original = nullptr;
};

Hook g_hooks[2];
void* g_create_target[2] = {nullptr, nullptr};
CreateFeatureFn g_create_original[2] = {nullptr, nullptr};
bool g_hooked = false;
std::atomic<int> g_preset{0};
std::atomic<bool> g_preset_applied{false};
const char* g_preset_status = "not applied yet";
std::atomic<unsigned> g_capture{0};
std::atomic<bool> g_dump{false};
std::atomic<unsigned> g_dump_index{0};
ID3D12Device* g_device = nullptr;

std::mutex g_mutex;
Snapshot g_snapshot;

using GetPointerFn = int (*)(const void*, const char*, void**);
using GetFloatFn = int (*)(const void*, const char*, float*);
using GetUnsignedFn = int (*)(const void*, const char*, unsigned*);
using GetIntFn = int (*)(const void*, const char*, int*);

struct Layout {
    const char* name;
    int get_resource;
    int get_float;
    int get_unsigned;
    int get_int;
    int set_unsigned;
};

constexpr Layout kDeclared{"declared", 14, 9, 11, 12, 3};
constexpr Layout kReversed{"reversed", 9, 14, 12, 11, 4};

Layout g_layout = kDeclared;
bool g_layout_known = false;
RememberLayoutFn g_remember = nullptr;
unsigned g_layout_attempts = 0;
unsigned g_bad_reads = 0;

enum class Block { Creation, Evaluate };

template <typename Fn>
Fn slot(const void* object, int index) {
    const auto* const* vtable = *reinterpret_cast<const void* const* const*>(object);
    return reinterpret_cast<Fn>(const_cast<void*>(vtable[index]));
}

union ProbeValue {
    void* pointer;
    float number;
    unsigned count;
    int integer;
    std::uint64_t bits;
};

int get_resource(const void* parameters, const char* key, ID3D12Resource** out) {
    ProbeValue value{};
    const int result = slot<GetPointerFn>(parameters, g_layout.get_resource)(parameters, key,
                                                                            &value.pointer);
    *out = result == kSuccess ? static_cast<ID3D12Resource*>(value.pointer) : nullptr;
    return result;
}

int get_float(const void* parameters, const char* key, float* out) {
    ProbeValue value{};
    const int result = slot<GetFloatFn>(parameters, g_layout.get_float)(parameters, key,
                                                                       &value.number);
    *out = result == kSuccess ? value.number : 0.0f;
    return result;
}

int get_unsigned(const void* parameters, const char* key, unsigned* out) {
    ProbeValue value{};
    const int result = slot<GetUnsignedFn>(parameters, g_layout.get_unsigned)(parameters, key,
                                                                             &value.count);
    *out = result == kSuccess ? value.count : 0u;
    return result;
}

using SetUnsignedFn = void (*)(void*, const char*, unsigned);

void set_unsigned(void* parameters, const char* key, unsigned value) {
    slot<SetUnsignedFn>(parameters, g_layout.set_unsigned)(parameters, key, value);
}

int get_int(const void* parameters, const char* key, int* out) {
    ProbeValue value{};
    const int result = slot<GetIntFn>(parameters, g_layout.get_int)(parameters, key, &value.integer);
    *out = result == kSuccess ? value.integer : 0;
    return result;
}

bool plausible_texture(ID3D12Resource* resource, D3D12_RESOURCE_DESC& desc) {
    if (resource == nullptr) {
        return false;
    }
    desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width >= 16
        && desc.Width <= 16384 && desc.Height >= 16 && desc.Height <= 16384;
}

struct ProbeContext {
    const void* parameters = nullptr;
    Layout layout{};
    Block block = Block::Evaluate;
    int score = 0;
};

void probe_body(void* raw) {
    auto* context = static_cast<ProbeContext*>(raw);
    const Layout saved = g_layout;
    g_layout = context->layout;

    auto sane_size = [](unsigned value) { return value >= 64 && value <= 16384; };

    if (context->block == Block::Creation) {
        unsigned width = 0;
        unsigned height = 0;
        if (get_unsigned(context->parameters, kCreateWidth, &width) == kSuccess && sane_size(width)) {
            context->score += 2;
        }
        if (get_unsigned(context->parameters, kCreateHeight, &height) == kSuccess
                && sane_size(height)) {
            context->score += 2;
        }
        unsigned out_width = 0;
        if (get_unsigned(context->parameters, kCreateOutWidth, &out_width) == kSuccess
                && sane_size(out_width)) {
            context->score += 2;
        }
    } else {
        unsigned width = 0;
        if (get_unsigned(context->parameters, kSubrectWidth, &width) == kSuccess
                && sane_size(width)) {
            context->score += 2;
        }
        float jitter = 0.0f;
        if (get_float(context->parameters, kJitterX, &jitter) == kSuccess && jitter > -8.0f
                && jitter < 8.0f) {
            context->score += 1;
        }
        ID3D12Resource* colour = nullptr;
        D3D12_RESOURCE_DESC desc{};
        if (get_resource(context->parameters, kColor, &colour) == kSuccess
                && plausible_texture(colour, desc)) {
            context->score += 3;
        }
    }
    g_layout = saved;
}

void detect_layout(const void* parameters, Block block) {
    if (g_layout_known || parameters == nullptr) {
        return;
    }
    ProbeContext declared{parameters, kDeclared, block, 0};
    ProbeContext reversed{parameters, kReversed, block, 0};
    const auto fault_a = guarded_call(probe_body, &declared);
    const auto fault_b = guarded_call(probe_body, &reversed);

    if (fault_a != 0 || fault_b != 0) {
        log::warn("fx/ngx: parameter probe faulted ({:#x} / {:#x})", fault_a, fault_b);
    }
    if (declared.score == 0 && reversed.score == 0) {
        if (++g_layout_attempts == 400) {
            log::error("fx/ngx: no parameter layout has answered a known key in {} attempts",
                       g_layout_attempts);
            std::scoped_lock lock(g_mutex);
            g_snapshot.problem = "parameter block layout not recognised";
        }
        return;
    }
    g_layout = reversed.score > declared.score ? kReversed : kDeclared;
    g_layout_known = true;
    g_bad_reads = 0;
    log::info("fx/ngx: parameter layout is {} from the {} block (scores: declared {}, reversed {})",
              g_layout.name, block == Block::Creation ? "creation" : "per-frame", declared.score,
              reversed.score);
    {
        std::scoped_lock lock(g_mutex);
        g_snapshot.layout = g_layout.name;
        g_snapshot.problem = "";
    }
    if (g_remember != nullptr) {
        g_remember(g_layout.name);
    }
}

struct DumpContext {
    ID3D12GraphicsCommandList* commands = nullptr;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* output = nullptr;
};

struct ReadContext {
    const void* parameters = nullptr;
    Snapshot out;
};

void read_body(void* raw) {
    auto* context = static_cast<ReadContext*>(raw);
    const void* parameters = context->parameters;
    Snapshot& out = context->out;

    ID3D12Resource* colour = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* output = nullptr;
    get_resource(parameters, kColor, &colour);
    get_resource(parameters, kDepth, &depth);
    get_resource(parameters, kMotionVectors, &motion);
    get_resource(parameters, kOutput, &output);

    D3D12_RESOURCE_DESC desc{};
    if (plausible_texture(colour, desc)) {
        out.color = colour;
        out.color_format = desc.Format;
        out.render_width = static_cast<unsigned>(desc.Width);
        out.render_height = desc.Height;
    }
    if (plausible_texture(depth, desc)) {
        out.depth = depth;
        out.depth_format = desc.Format;
    }
    if (plausible_texture(motion, desc)) {
        out.motion = motion;
        out.motion_format = desc.Format;
    }
    if (plausible_texture(output, desc)) {
        out.output = output;
        out.output_format = desc.Format;
        out.output_width = static_cast<unsigned>(desc.Width);
        out.output_height = desc.Height;
    }

    get_float(parameters, kJitterX, &out.jitter_x);
    get_float(parameters, kJitterY, &out.jitter_y);
    get_float(parameters, kMvScaleX, &out.mv_scale_x);
    get_float(parameters, kMvScaleY, &out.mv_scale_y);
    get_float(parameters, kSharpness, &out.sharpness);
    get_int(parameters, kReset, &out.reset);
    get_unsigned(parameters, kSubrectWidth, &out.subrect_width);
    get_unsigned(parameters, kSubrectHeight, &out.subrect_height);
}

const char* format_name(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
        case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
        case DXGI_FORMAT_R32G32_FLOAT: return "RG32F";
        case DXGI_FORMAT_R32_FLOAT: return "R32F";
        case DXGI_FORMAT_D32_FLOAT: return "D32";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32S8";
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_UNKNOWN: return "none";
        default: return "other";
    }
}

void report(const Snapshot& s) {
    log::info("fx/ngx: evaluate #{} on {} ({} layout)", s.calls, s.module_name, s.layout);
    log::info("fx/ngx:   colour  {} {}x{}   -> output {} {}x{}", format_name(s.color_format),
              s.render_width, s.render_height, format_name(s.output_format), s.output_width,
              s.output_height);
    log::info("fx/ngx:   depth   {}   motion {}   subrect {}x{}", format_name(s.depth_format),
              format_name(s.motion_format), s.subrect_width, s.subrect_height);
    log::info("fx/ngx:   jitter  {:.4f} {:.4f}   mv scale {:.1f} {:.1f}   sharpness {:.2f}   "
              "reset {}", s.jitter_x, s.jitter_y, s.mv_scale_x, s.mv_scale_y, s.sharpness, s.reset);
    log::info("fx/ngx:   resources colour {} depth {} motion {} output {}", s.color, s.depth,
              s.motion, s.output);
}

void dump_body(void* raw) {
    auto* context = static_cast<DumpContext*>(raw);
    if (g_device == nullptr && context->color != nullptr) {
        context->color->GetDevice(IID_PPV_ARGS(&g_device));
    }
    if (g_device == nullptr) {
        return;
    }
    const unsigned index = g_dump_index.fetch_add(1, std::memory_order_relaxed);
    if (context->color != nullptr) {
        readback::request(g_device, context->commands, context->color,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          std::format("{:02}-upscaler-input", index));
    }
    if (context->output != nullptr) {
        readback::request(g_device, context->commands, context->output,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          std::format("{:02}-upscaler-output", index));
    }
}

struct PresetContext {
    void* parameters = nullptr;
    unsigned preset = 0;
    bool setter_ok = false;
};

void apply_preset_body(void* raw) {
    auto* context = static_cast<PresetContext*>(raw);
    void* parameters = context->parameters;

    set_unsigned(parameters, kProbeKey, kProbeValue);
    unsigned echo = 0;
    if (get_unsigned(parameters, kProbeKey, &echo) != kSuccess || echo != kProbeValue) {
        return;
    }
    context->setter_ok = true;
    for (const char* key : kPresetKeys) {
        set_unsigned(parameters, key, context->preset);
    }
}

int create_feature_detour(int index, ID3D12GraphicsCommandList* commands, unsigned feature_id,
                          const void* parameters, void** out_handle) {
    detect_layout(parameters, Block::Creation);
    const int preset = g_preset.load(std::memory_order_relaxed);
    if (preset > 0 && parameters != nullptr && g_layout_known) {
        PresetContext context;
        context.parameters = const_cast<void*>(parameters);
        context.preset = static_cast<unsigned>(preset);
        const auto fault = guarded_call(apply_preset_body, &context);
        if (fault != 0) {
            g_preset_status = "applying the preset faulted";
            log::error("fx/ngx: applying the DLSS preset faulted ({:#x})", fault);
        } else if (!context.setter_ok) {
            g_preset_status = "the parameter setter did not round trip";
            log::warn("fx/ngx: the parameter setter did not round trip; preset not applied");
        } else {
            g_preset_applied.store(true, std::memory_order_relaxed);
            g_preset_status = "applied at feature creation";
            log::info("fx/ngx: created a DLSS feature with preset {} forced on every quality mode",
                      static_cast<char>('A' + preset - 1));
        }
    } else if (preset > 0 && !g_layout_known) {
        g_preset_status = "waiting for the parameter layout";
    }
    return g_create_original[index](commands, feature_id, parameters, out_handle);
}

int create_feature_detour_0(ID3D12GraphicsCommandList* commands, unsigned feature_id,
                            const void* parameters, void** out_handle) {
    return create_feature_detour(0, commands, feature_id, parameters, out_handle);
}

int create_feature_detour_1(ID3D12GraphicsCommandList* commands, unsigned feature_id,
                            const void* parameters, void** out_handle) {
    return create_feature_detour(1, commands, feature_id, parameters, out_handle);
}

int evaluate_detour(int index, ID3D12GraphicsCommandList* commands, const void* feature,
                    const void* parameters, void* callback) {
    static thread_local unsigned nesting = 0;
    struct EvaluateScope { unsigned& depth; explicit EvaluateScope(unsigned& d) : depth(d) { ++depth; } ~EvaluateScope() { --depth; } } scope(nesting);
    if (nesting > 1) return g_hooks[index].original(commands, feature, parameters, callback);
    Snapshot inputs;
    detect_layout(parameters, Block::Evaluate);

    if (g_layout_known && parameters != nullptr) {
        ReadContext context;
        context.parameters = parameters;
        const auto fault = guarded_call(read_body, &context);
        if (fault == 0) inputs = context.out;

        if (context.out.color == nullptr) {
            if (++g_bad_reads == 120) {
                log::warn("fx/ngx: the {} layout has read nothing for {} frames; probing again",
                          g_layout.name, g_bad_reads);
                g_layout_known = false;
                g_bad_reads = 0;
                g_layout_attempts = 0;
            }
        } else {
            g_bad_reads = 0;
        }

        std::scoped_lock lock(g_mutex);
        const unsigned calls = g_snapshot.calls + 1;
        const char* layout = g_snapshot.layout;
        const char* problem = fault != 0 ? "reading the parameter block faulted" : "";
        unsigned window = g_snapshot.window + 1;
        unsigned resets = g_snapshot.window_resets + (context.out.reset != 0 ? 1u : 0u);
        float rate = g_snapshot.reset_rate;
        if (window >= kResetWindow) {
            rate = static_cast<float>(resets) / static_cast<float>(window);
            log::info("fx/ngx: over the last {} evaluate calls the game reset the upscaler's "
                      "history {} times ({:.0f}%)", window, resets, rate * 100.0f);
            window = 0;
            resets = 0;
        }
        g_snapshot = context.out;
        g_snapshot.hooked = true;
        g_snapshot.seen = true;
        g_snapshot.calls = calls;
        g_snapshot.frames_since = 0;
        g_snapshot.window = window;
        g_snapshot.window_resets = resets;
        g_snapshot.reset_rate = rate;
        g_snapshot.module_name = g_hooks[index].module == nullptr ? "?" : "nvngx";
        g_snapshot.layout = layout;
        g_snapshot.problem = problem;

        if (g_capture.load(std::memory_order_relaxed) > 0) {
            g_capture.fetch_sub(1, std::memory_order_relaxed);
            report(g_snapshot);
            log::info("fx/ngx:   command list {}", static_cast<void*>(commands));
        }

        if (g_dump.exchange(false)) {
            DumpContext dump;
            dump.commands = commands;
            dump.color = const_cast<ID3D12Resource*>(
                static_cast<const ID3D12Resource*>(context.out.color));
            dump.output = const_cast<ID3D12Resource*>(
                static_cast<const ID3D12Resource*>(context.out.output));
            const auto dump_fault = guarded_call(dump_body, &dump);
            if (dump_fault != 0) {
                log::error("fx/ngx: dumping the upscaler's buffers faulted ({:#x})", dump_fault);
            } else {
                log::info("fx/ngx: dumping the upscaler's input and output for this frame");
            }
        }
    }

    if (inputs.color != nullptr) {
        render_raytracing(commands, inputs);
        render_pre_upscale(commands, inputs);
    }
    return g_hooks[index].original(commands, feature, parameters, callback);
}

int evaluate_detour_0(ID3D12GraphicsCommandList* commands, const void* feature,
                      const void* parameters, void* callback) {
    return evaluate_detour(0, commands, feature, parameters, callback);
}

int evaluate_detour_1(ID3D12GraphicsCommandList* commands, const void* feature,
                      const void* parameters, void* callback) {
    return evaluate_detour(1, commands, feature, parameters, callback);
}

bool hook_one(int index, const wchar_t* module, void* detour) {
    Hook& hook = g_hooks[index];
    const HMODULE handle = GetModuleHandleW(module);
    if (handle == nullptr) {
        return false;
    }
    void* target = reinterpret_cast<void*>(
        GetProcAddress(handle, "NVSDK_NGX_D3D12_EvaluateFeature"));
    if (target == nullptr) {
        return false;
    }
    for (const Hook& other : g_hooks) {
        if (other.target == target) {
            return false;
        }
    }
    const auto created = MH_CreateHook(target, detour, reinterpret_cast<void**>(&hook.original));
    if (created != MH_OK) {
        if (created != MH_ERROR_ALREADY_CREATED) {
            log::warn("fx/ngx: could not hook the evaluate call ({})", static_cast<int>(created));
        }
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        return false;
    }
    hook.module = module;
    hook.target = target;

    void* create = reinterpret_cast<void*>(GetProcAddress(handle, "NVSDK_NGX_D3D12_CreateFeature"));
    if (create != nullptr && create != g_create_target[0] && create != g_create_target[1]) {
        void* create_detour = index == 0 ? reinterpret_cast<void*>(&create_feature_detour_0)
                                         : reinterpret_cast<void*>(&create_feature_detour_1);
        if (MH_CreateHook(create, create_detour,
                          reinterpret_cast<void**>(&g_create_original[index])) == MH_OK
                && MH_EnableHook(create) == MH_OK) {
            g_create_target[index] = create;
        } else {
            log::warn("fx/ngx: could not hook the feature creation call; the preset override "
                      "will not apply");
        }
    }
    return true;
}

}

DWORD WINAPI watcher(LPVOID) {
    for (int attempt = 0; attempt < 2400 && !g_hooked; ++attempt) {
        if (initialise()) {
            log::info("fx/ngx: hooked {:.1f} s into the process, before any feature creation",
                      attempt * 0.05);
            return 0;
        }
        Sleep(50);
    }
    return 0;
}

void watch_for_module() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    MH_Initialize();
    const HANDLE thread = CreateThread(nullptr, 0, watcher, nullptr, 0, nullptr);
    if (thread != nullptr) {
        CloseHandle(thread);
    }
}

bool initialise() {
    if (g_hooked) {
        return true;
    }
    bool any = hook_one(0, L"nvngx.dll", reinterpret_cast<void*>(&evaluate_detour_0));
    any |= hook_one(1, L"_nvngx.dll", reinterpret_cast<void*>(&evaluate_detour_1));
    if (!any) {
        return false;
    }
    g_hooked = true;
    {
        std::scoped_lock lock(g_mutex);
        g_snapshot.hooked = true;
    }
    log::info("fx/ngx: hooked NVSDK_NGX_D3D12_EvaluateFeature in {}{}",
              g_hooks[0].target != nullptr ? "nvngx.dll" : "",
              g_hooks[1].target != nullptr ? " _nvngx.dll" : "");
    capture(3);
    return true;
}

void set_preset(Preset value) {
    const int next = static_cast<int>(value);
    if (g_preset.exchange(next) == next) {
        return;
    }
    g_preset_applied.store(false, std::memory_order_relaxed);
    g_preset_status = next == 0 ? "left to the game" : "set, waiting for the next feature creation";
    log::info("fx/ngx: DLSS preset override set to {}",
              next == 0 ? std::string("the game's default")
                        : std::string(1, static_cast<char>('A' + next - 1)));
}

Preset preset() {
    return static_cast<Preset>(g_preset.load(std::memory_order_relaxed));
}

bool preset_applied() {
    return g_preset_applied.load(std::memory_order_relaxed);
}

const char* preset_status() {
    return g_preset_status;
}

void shutdown() {
    for (int i = 0; i < 2; ++i) {
        if (g_create_target[i] != nullptr) {
            MH_DisableHook(g_create_target[i]);
            MH_RemoveHook(g_create_target[i]);
            g_create_target[i] = nullptr;
        }
    }
    for (Hook& hook : g_hooks) {
        if (hook.target != nullptr) {
            MH_DisableHook(hook.target);
            MH_RemoveHook(hook.target);
        }
        hook = Hook{};
    }
    g_hooked = false;
    std::scoped_lock lock(g_mutex);
    g_snapshot = Snapshot{};
}

bool hooked() {
    return g_hooked;
}

void end_frame() {
    std::scoped_lock lock(g_mutex);
    if (g_snapshot.seen && g_snapshot.frames_since < 1000) {
        ++g_snapshot.frames_since;
    }
}

Snapshot snapshot() {
    std::scoped_lock lock(g_mutex);
    return g_snapshot;
}

void set_known_layout(const char* name) {
    if (name == nullptr || g_layout_known) {
        return;
    }
    if (std::strcmp(name, kReversed.name) == 0) {
        g_layout = kReversed;
        g_layout_known = true;
    } else if (std::strcmp(name, kDeclared.name) == 0) {
        g_layout = kDeclared;
        g_layout_known = true;
    }
    if (g_layout_known) {
        log::info("fx/ngx: using the {} parameter layout remembered from a previous session",
                  g_layout.name);
        std::scoped_lock lock(g_mutex);
        g_snapshot.layout = g_layout.name;
    }
}

const char* known_layout() {
    return g_layout_known ? g_layout.name : "";
}

void on_layout_detected(RememberLayoutFn callback) {
    g_remember = callback;
}

void capture(unsigned frames) {
    g_capture.store(frames, std::memory_order_relaxed);
}

void dump_frame() {
    g_dump.store(true, std::memory_order_relaxed);
}

bool dump_pending() {
    return g_dump.load(std::memory_order_relaxed);
}

ID3D12Device* device() {
    return g_device;
}

}
