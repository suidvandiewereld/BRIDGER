#include "fx/fx.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <tuple>
#include <vector>

#include "core/settings.h"
#include "fx/depth_capture.h"
#include "fx/fx_internal.h"
#include "fx/ngx.h"
#include "fx/pipeline.h"
#include "fx/readback.h"
#include "fx/rt.h"
#include "ui/font.h"
#include "ui/ui.h"

namespace bridger::fx {
namespace {

constexpr float kReloadWidth = 62.0f;
constexpr float kLogWidth = 60.0f;

enum Page : int { kEffects, kShaders, kUpscaler, kPipelines, kSettings, kDiagnostics, kPageCount };
int g_page = kEffects;

Handle g_open_log = 0;

const char* stage_name(BridgerFxStage stage) {
    switch (stage) {
        case BRIDGER_FX_STAGE_PRE_UPSCALE: return "pre upscale";
        case BRIDGER_FX_STAGE_SCENE: return "scene";
        case BRIDGER_FX_STAGE_POST: return "post";
        default: return "overlay";
    }
}

const char* kind_name(BridgerFxShaderKind kind) {
    switch (kind) {
        case BRIDGER_FX_SHADER_WORLD: return "world";
        case BRIDGER_FX_SHADER_SCREEN: return "screen";
        default: return "fullscreen";
    }
}

const char* format_name(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS: return "D32";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "D32S8";
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS: return "D24S8";
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS: return "D16";
        default: return "depth";
    }
}

const char* ngx_format(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8 sRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
        case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
        case DXGI_FORMAT_R32G32_FLOAT: return "RG32F";
        case DXGI_FORMAT_R32_FLOAT: return "R32F";
        case DXGI_FORMAT_UNKNOWN: return "none";
        default: return format_name(format);
    }
}

std::string upper(std::string_view value) {
    std::string out(value);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

struct Stat {
    std::string label;
    std::string value;
    ui::Color tone;
};

void stat_band(const std::vector<Stat>& stats) {
    const auto& t = ui::theme();
    const ui::Vec2 at = ui::cursor();
    const float width = ui::available_width();
    const float height = 62.0f;
    const float cell_width = width / static_cast<float>(stats.size());
    auto& list = ui::draw();
    for (std::size_t i = 0; i < stats.size(); ++i) {
        const float x = at.x + cell_width * static_cast<float>(i);
        const float inset = i == 0 ? 0.0f : 18.0f;
        if (i > 0) {
            list.rect({x, at.y + 2.0f, x + 1.0f, at.y + height - 16.0f}, t.hairline);
        }
        {
            const ui::font::ScopedFace caption(ui::font::Face::Caption);
            list.text_tracked({x + inset, at.y}, t.text_faint, upper(stats[i].label), t.tracking);
        }
        const ui::font::ScopedFace title(ui::font::Face::Title);
        list.text_clipped({x + inset, at.y + 19.0f}, stats[i].tone, stats[i].value,
                          cell_width - inset - 8.0f);
    }
    list.rect({at.x, at.y + height - 1.0f, at.x + width, at.y + height}, t.hairline);
    ui::set_cursor_y(at.y + height + 16.0f);
}

void page_title(std::string_view title, std::string_view subtitle) {
    const auto& t = ui::theme();
    {
        const ui::font::ScopedFace face(ui::font::Face::Title);
        ui::text_colored(t.text, title);
    }
    if (!subtitle.empty()) {
        const ui::font::ScopedFace micro(ui::font::Face::Micro);
        ui::text_colored(t.text_faint, subtitle);
    }
    ui::spacing(10.0f);
}

void section(std::string_view label) {
    const auto& t = ui::theme();
    ui::spacing(12.0f);
    const ui::font::ScopedFace caption(ui::font::Face::Caption);
    ui::tracked(t.text_faint, upper(label), t.tracking);
}

void cell(float x, float y, float height, ui::Color color, std::string_view value, float width,
          ui::font::Face face = ui::font::Face::Micro) {
    const ui::font::ScopedFace scoped(face);
    ui::draw().text_clipped({x, y + (height - ui::font::line_height()) * 0.5f}, color, value,
                            std::max(0.0f, width));
}

void cell_right(float x1, float y, float height, ui::Color color, std::string_view value,
                ui::font::Face face = ui::font::Face::Mono) {
    const ui::font::ScopedFace scoped(face);
    const float w = ui::font::measure(value);
    ui::draw().text({x1 - w, y + (height - ui::font::line_height()) * 0.5f}, color, value);
}

void row_rule() {
    const ui::Vec2 at = ui::cursor();
    const float y = at.y - ui::theme().spacing * 0.5f;
    ui::draw().rect({at.x, y, at.x + ui::available_width(), y + 1.0f},
                    ui::with_alpha(ui::theme().hairline, 0.9f));
}

void draw_effects(State& s) {
    const auto& t = ui::theme();
    page_title("Effects", "Passes and draw lists from mods, in the order they run.");
    if (s.effects.empty()) {
        ui::note("No effects registered. Mods add passes and draw lists through bridger::fx.");
        return;
    }
    if (s.settings.bypass) {
        ui::text_colored(t.warn, "Bypass is on. Nothing below is drawing.");
    }

    std::vector<Effect*> order;
    order.reserve(s.effects.size());
    for (auto& [handle, effect] : s.effects) {
        (void)handle;
        order.push_back(&effect);
    }
    std::sort(order.begin(), order.end(), [](const Effect* a, const Effect* b) {
        return std::tie(a->stage, a->priority, a->handle) < std::tie(b->stage, b->priority, b->handle);
    });

    bool first = true;
    BridgerFxStage stage = order.front()->stage;
    for (Effect* effect : order) {
        if (first || effect->stage != stage) {
            stage = effect->stage;
            if (!first) {
                ui::spacing(6.0f);
            }
            first = false;
            section(stage_name(stage));
            row_rule();
        }

        const ui::IdScope row(effect->handle);
        const ui::Vec2 at = ui::cursor();
        const float width = ui::available_width();
        if (ui::checkbox(effect->name, effect->enabled)) {
            effect->enabled = !effect->enabled;
        }
        const float height = ui::cursor().y - at.y - t.spacing;

        const char* kind = effect->is_pass ? "full screen pass"
                         : effect->space == BRIDGER_FX_SPACE_WORLD ? "world draws" : "screen draws";
        cell(at.x + width * 0.46f, at.y, height, t.text_faint,
             effect->owner.empty() ? "?" : effect->owner, width * 0.18f);
        cell(at.x + width * 0.64f, at.y, height, t.text_faint, kind, width * 0.2f);

        if (!effect->problem.empty()) {
            cell_right(at.x + width, at.y, height, t.bad, "problem", ui::font::Face::Micro);
        } else if (!effect->enabled) {
            cell_right(at.x + width, at.y, height, t.text_faint, "off", ui::font::Face::Micro);
        } else if (effect->ran) {
            cell_right(at.x + width, at.y, height, t.text_dim, std::format("{:.2f} ms", effect->gpu_ms));
        } else {
            cell_right(at.x + width, at.y, height, t.text_faint, "idle", ui::font::Face::Micro);
        }
        if (!effect->problem.empty()) {
            const ui::font::ScopedFace micro(ui::font::Face::Micro);
            ui::indent(25.0f);
            ui::text_wrapped(t.bad, effect->problem);
            ui::unindent(25.0f);
        }
        row_rule();
    }
}

void draw_shaders(State& s) {
    const auto& t = ui::theme();
    page_title("Shaders", "Mod shaders. File-based ones recompile on save while hot reload is on.");
    bool any = false;
    for (auto& [handle, shader] : s.shaders) {
        if (shader.builtin) {
            continue;
        }
        if (!any) {
            row_rule();
        }
        any = true;
        const ui::IdScope row(handle);

        const ui::Vec2 at = ui::cursor();
        const float width = ui::available_width();
        const float height = t.row_height + 6.0f;

        ui::Color tone = t.good;
        const char* status = "ok";
        if (shader.dirty) {
            tone = t.warn;
            status = "compiling";
        } else if (!shader.ready) {
            tone = t.bad;
            status = "failed";
        } else if (!shader.log.empty()) {
            tone = t.warn;
            status = "warnings";
        }
        const float mid = at.y + height * 0.5f;
        ui::draw().rect({at.x, mid - 3.0f, at.x + 6.0f, mid + 3.0f}, tone);
        cell(at.x + 18.0f, at.y, height, t.text, shader.name, width * 0.38f - 18.0f,
             ui::font::Face::Body);
        cell(at.x + width * 0.38f, at.y, height, t.text_faint,
             shader.owner.empty() ? "?" : shader.owner, width * 0.15f);
        cell(at.x + width * 0.53f, at.y, height, t.text_faint, kind_name(shader.kind), width * 0.1f);
        cell(at.x + width * 0.63f, at.y, height, tone, status, width * 0.1f);

        if (!shader.log.empty()) {
            ui::align_right(kLogWidth + kReloadWidth + t.spacing);
            const bool open = g_open_log == handle;
            if (ui::ghost_button(open ? "hide" : "output", kLogWidth)) {
                g_open_log = open ? 0 : handle;
            }
            ui::same_line(0.0f);
        } else {
            ui::align_right(kReloadWidth);
        }
        if (ui::ghost_button("reload", kReloadWidth)) {
            shader.dirty = true;
        }
        if (g_open_log == handle && !shader.log.empty()) {
            const ui::font::ScopedFace micro(ui::font::Face::Micro);
            ui::indent(18.0f);
            ui::text_wrapped(shader.ready ? t.warn : t.bad, shader.log);
            ui::unindent(18.0f);
        }
        row_rule();
    }
    if (!any) {
        ui::note("No mod shaders loaded.");
    }
}

void draw_upscaler(State& s) {
    const auto& t = ui::theme();
    const auto ngx_state = ngx::snapshot();
    page_title("Upscaler", "DLSS. Pre upscale effects draw into its input.");
    ui::begin_settings();
    if (!ngx_state.hooked) {
        ui::readout_colored("state", t.text_faint, "not loaded");
        ui::note("DLSS is off in the game's graphics options, or there is no NVIDIA runtime.");
        ui::end_settings();
        return;
    }
    if (!ngx_state.seen) {
        ui::readout_colored("state", t.warn, "waiting");
        ui::note("Hooked, but the game has not run the upscaler yet. Turn DLSS on in game.");
        ui::end_settings();
        return;
    }
    const bool live = ngx_state.frames_since < 10;
    ui::readout_colored("state", live ? t.good : t.warn,
                        live ? std::string("live")
                             : std::format("stale, {} frames ago", ngx_state.frames_since));
    ui::readout("resolution", std::format("{} x {}  to  {} x {}", ngx_state.render_width,
                                          ngx_state.render_height, ngx_state.output_width,
                                          ngx_state.output_height));
    if (ngx_state.reset_rate > 0.5f) {
        ui::readout_colored("history", t.bad, std::format("reset on {:.0f}% of frames",
                                                          ngx_state.reset_rate * 100.0f));
        ui::note("With no history there is no temporal resolve, so the image aliases and crawls.");
    }
    if (ngx_state.problem[0] != 0) {
        ui::readout_colored("problem", t.bad, ngx_state.problem);
    }

    section("model");
    auto& settings = s.settings;
    static const char* const kPresets[] = {"game default", "A", "B", "C", "D", "E", "F",
                                           "G", "H", "I", "J  transformer", "K  transformer"};
    int preset = std::clamp(settings.dlss_preset, 0, 11);
    if (ui::setting_combo("Preset", preset, kPresets, 12,
                          "J and K are the transformer models, which hold thin geometry and "
                          "foliage far more stably than the CNN model the game gets by default.")) {
        settings.dlss_preset = preset;
        save_settings();
    }
    if (settings.dlss_preset != 0 && !ngx::preset_applied()) {
        ui::note("Takes effect once DLSS is set up again: toggle the upscaling mode in game, or "
                 "restart.");
    }
    ui::end_settings();
}

void draw_pipelines(State& s) {
    const auto& t = ui::theme();
    const auto snap = pipeline::snapshot();
    page_title("Game pipelines", "The game's own draws, as mods hook and replace them.");

    ui::begin_settings();
    auto& settings = s.settings;
    bool changed = false;
    changed |= ui::setting_bool("Intercept draws", settings.pipeline_hooks,
        "Run mod draw callbacks and substitute replaced pipelines. Off passes every call through.");
    changed |= ui::setting_bool("Retain shader bytecode", settings.pipeline_retain,
        "Needed to replace, rewrite or dump a game shader. Applies to pipelines created after.");
    if (changed) {
        save_settings();
    }
    if (!snap.hooks_installed) {
        ui::readout_colored("hooks", t.bad, snap.problem.empty() ? "not installed" : snap.problem);
    }
    ui::readout("mods", std::format("{} hooked, {} replaced", snap.hooked, snap.replaced));
    if (snap.material_active) {
        ui::readout_colored("material hook", snap.material_problem.empty() ? t.good : t.bad,
                            snap.material_problem.empty()
                                ? std::format("{}  {} rewritten", snap.material_owner,
                                              snap.material_applied)
                                : std::format("{}  {}", snap.material_owner, snap.material_problem));
    }
    ui::end_settings();

    section("busiest last frame");
    const auto rows = pipeline::busiest(16);
    if (rows.empty()) {
        ui::note("Nothing drew through a recorded pipeline last frame.");
        return;
    }
    row_rule();
    for (const auto& row : rows) {
        const ui::IdScope scope(static_cast<std::uint32_t>(row.hash ^ (row.hash >> 32)));
        const ui::Vec2 at = ui::cursor();
        const float width = ui::available_width();
        const float height = t.row_height + 6.0f;
        cell(at.x, at.y, height, t.text_dim, std::format("{:016x}", row.hash), 170.0f,
             ui::font::Face::Mono);
        cell_right(at.x + 240.0f, at.y, height, t.text, std::format("{}", row.draws));
        cell(at.x + 252.0f, at.y, height, t.text_faint, row.compute ? "dispatches" : "draws", 80.0f);
        cell(at.x + 340.0f, at.y, height, t.text_faint,
             row.compute ? std::string("compute")
                         : std::format("{} target{}", row.render_targets,
                                       row.render_targets == 1 ? "" : "s"),
             90.0f);
        std::string flags;
        if (row.replaced) flags += "replaced  ";
        if (row.material) flags += "material  ";
        if (row.hooked) flags += "hooked";
        cell(at.x + 440.0f, at.y, height, t.accent, flags, width - 440.0f - kReloadWidth - 8.0f);
        ui::align_right(kReloadWidth);
        if (ui::ghost_button("dump", kReloadWidth)) {
            pipeline::dump(row.hash);
        }
        if (!row.problem.empty()) {
            const ui::font::ScopedFace micro(ui::font::Face::Micro);
            ui::text_colored(t.warn, row.problem);
        }
        row_rule();
    }
    ui::note("Dumps land in Bridger/dumps/pipelines.");
}

void draw_settings(State& s) {
    auto& settings = s.settings;
    page_title("Settings", "");
    ui::begin_settings();
    bool changed = false;
    changed |= ui::setting_bool("Bypass all effects", settings.bypass,
                                "Skips every pass and draw list. Uploads and compiles still run.");
    changed |= ui::setting_bool("Hot reload", settings.hot_reload,
                                "Recompile shaders when they, or a file they include, change.");
    changed |= ui::setting_bool("GPU timings", settings.timings,
                                "Time every effect with timestamp queries.");
    changed |= ui::setting_bool("Pre upscale effects", settings.pre_upscale,
                                "Experimental. Runs mod work inside DLSS.");

    if (ui::begin_group("Raytracing")) {
        auto& rt_settings = rt::settings();
        const auto rt_snap = rt::snapshot();
        bool boot = rt_snap.boot;
        if (ui::setting_bool("Raytracing (restart)", boot,
                             "Boot-only. Extends the game's root signatures for stream output and turns material bindings on. "
                             "Needs dxcompiler.dll and dxil.dll beside bridger.dll.")) {
            settings::set_bool("bridger", "fx.raytracing", boot);
            settings::flush_now();
        }
        if (rt_snap.boot) {
            changed |= ui::setting_bool("Trace", rt_settings.enabled,
                                        "Capture the game's geometry and trace against it inside DLSS.");
            static const char* const modes[] = {"ambient occlusion", "sun shadow", "debug: primary rays"};
            changed |= ui::setting_combo("Mode", rt_settings.mode, modes, 3,
                                         "The debug view colours each pixel green where the traced distance agrees with the depth buffer.");
            changed |= ui::setting_int("Rays per pixel", rt_settings.rays, 1, 32, "",
                                       "Ambient occlusion samples. DLSS averages the noise over time.");
            changed |= ui::setting_float("Radius", rt_settings.radius, 0.1f, 20.0f, " m", "Ambient occlusion reach.");
            changed |= ui::setting_float("Strength", rt_settings.strength, 0.0f, 1.0f, "", "How much of the result reaches the frame.");
            changed |= ui::setting_float("Bias", rt_settings.bias, 0.0f, 0.2f, "", "Ray origin offset along the normal, per metre of distance.");
            changed |= ui::setting_float("Sun azimuth", rt_settings.sun_azimuth, 0.0f, 360.0f, " deg", "Shadow mode only.");
            changed |= ui::setting_float("Sun elevation", rt_settings.sun_elevation, 0.0f, 90.0f, " deg", "Shadow mode only.");
            if (!rt_snap.ready) ui::readout_colored("status", ui::theme().warn, rt_snap.detail.empty() ? "waiting for the first frame" : rt_snap.detail);
            else ui::readout("status", rt_snap.detail);
        } else {
            ui::note("Off. Turn it on and restart the game.");
        }
    }
    ui::end_group();

    if (ui::begin_group("Camera")) {
        changed |= ui::setting_bool("FOV is horizontal", settings.fov_horizontal,
                                    "Flip this if world geometry drifts against the terrain as "
                                    "the camera turns.");
    }
    ui::end_group();

    if (ui::begin_group("Depth")) {
        changed |= ui::setting_bool("Capture game depth", settings.depth_capture,
                                    "Experimental. Lets shaders read the game's depth at t1.");
        if (settings.depth_capture) {
            changed |= ui::setting_bool("Reverse Z", settings.depth_reversed,
                                        "Decima puts 1 at the near plane. Affects "
                                        "bridger_linear_depth only.");
            const auto candidates = depth::candidates();
            std::vector<std::string> labels;
            labels.emplace_back("automatic");
            for (const auto& candidate : candidates) {
                labels.push_back(std::format("{} x {}  {}", candidate.width, candidate.height,
                                             format_name(candidate.format)));
            }
            std::vector<const char*> items;
            items.reserve(labels.size());
            for (const auto& label : labels) {
                items.push_back(label.c_str());
            }
            int index = std::clamp(settings.depth_candidate + 1, 0,
                                   static_cast<int>(items.size()) - 1);
            if (ui::setting_combo("Buffer", index, items.data(), static_cast<int>(items.size()),
                                  "Only used when the upscaler has not named the depth buffer. "
                                  "The order is not stable between runs.")) {
                settings.depth_candidate = index - 1;
                changed = true;
            }
        }
    }
    ui::end_group();
    ui::end_settings();
    if (changed) {
        save_settings();
    }
}

void draw_diagnostics(State& s) {
    const auto& t = ui::theme();
    const auto& stats = s.stats;
    page_title("Diagnostics", "Internals, for when something looks wrong.");

    if (readback::pending() == 0) {
        if (ui::button("Capture golden frames", 196.0f)) {
            dump_backbuffer();
        }
        ui::same_line(0.0f);
        if (ui::button("Dump upscaler frame", 186.0f)) {
            ngx::dump_frame();
            dump_backbuffer();
        }
        ui::same_line(0.0f);
        if (ui::button("Log evaluate calls", 170.0f)) {
            ngx::capture(3);
        }
    } else {
        ui::text_colored(t.warn, std::format("{} image(s) in flight", readback::pending()));
    }
    const auto message = readback::last_message();
    if (!message.empty()) {
        const ui::font::ScopedFace micro(ui::font::Face::Micro);
        ui::text_colored(t.text_faint, message);
    }

    ui::begin_settings();
    if (ui::begin_group("Frame")) {
        ui::readout_colored("device", s.frame.device_ready ? t.good : t.warn,
                            s.frame.device_ready ? "ready" : "waiting for the swapchain");
        ui::readout("target", std::format("{} x {}", stats.width, stats.height));
        ui::readout("frame", std::format("{}   {:.1f} s", stats.frame, s.frame.time));
        ui::readout("submitted", std::format("{} draws  {} vertices  {} copies", stats.draw_calls,
                                             stats.vertices, stats.copies));
        ui::readout("cpu", std::format("{:.2f} ms", stats.cpu_ms));
        ui::readout("pipelines cached", std::format("{}", detail::pipeline_count()));
        ui::readout_colored("depth", stats.depth_captured ? t.good : t.text_faint,
                            stats.depth_note.empty() ? "off" : stats.depth_note);
    }
    ui::end_group();

    if (ui::begin_group("Camera", false)) {
        const auto& camera = s.frame.camera;
        ui::readout_colored("source", camera.valid ? t.good : t.warn, detail::camera_source());
        if (camera.valid) {
            ui::readout("position", std::format("{:.1f}  {:.1f}  {:.1f}", camera.position[0],
                                                camera.position[1], camera.position[2]));
            ui::readout("forward", std::format("{:.3f}  {:.3f}  {:.3f}", camera.forward[0],
                                               camera.forward[1], camera.forward[2]));
            ui::readout("fov", std::format("{:.1f} deg", camera.fov_degrees));
            ui::readout("clip", std::format("{:.3f} to {}", camera.near_plane,
                                            camera.far_plane > 0.0f
                                                ? std::format("{:.0f} m", camera.far_plane)
                                                : std::string("infinite")));
        }
        ui::readout_colored("samples", s.camera_faults > 0 ? t.warn : t.text_dim,
                            std::format("{} taken, {} faulted", s.camera_samples, s.camera_faults));
    }
    ui::end_group();

    if (ui::begin_group("Upscaler inputs", false)) {
        const auto n = ngx::snapshot();
        ui::readout("color", std::format("{} x {}  {}", n.render_width, n.render_height,
                                         ngx_format(n.color_format)));
        ui::readout("output", std::format("{} x {}  {}", n.output_width, n.output_height,
                                          ngx_format(n.output_format)));
        ui::readout("depth", ngx_format(n.depth_format));
        ui::readout("motion", std::format("{}  scale {:.1f} {:.1f}", ngx_format(n.motion_format),
                                          n.mv_scale_x, n.mv_scale_y));
        ui::readout("jitter", std::format("{:+.4f}  {:+.4f}", n.jitter_x, n.jitter_y));
        ui::readout("calls", std::format("{}", n.calls));
        ui::readout("preset", ngx::preset_status());
        ui::readout("parameters", n.layout);
    }
    ui::end_group();

    if (ui::begin_group("Pre upscale", false)) {
        ui::readout("status", stats.upscale_note);
        ui::readout("leases", std::format("{} jobs, {} draws", stats.upscale_jobs, stats.upscale_draws));
        for (const auto& entry : stats.upscale_inventory) {
            ui::note(entry);
        }
    }
    ui::end_group();

    if (ui::begin_group("Game pipelines", false)) {
        const auto snap = pipeline::snapshot();
        ui::readout("pipelines", std::format("{}  ({} graphics, {} compute)", snap.pipelines,
                                             snap.graphics, snap.compute));
        ui::readout("created", std::format("{} stream, {} library", snap.stream_created,
                                           snap.library_loaded));
        ui::readout("shaders", std::format("{} distinct, {:.1f} MB", snap.blobs,
                                           static_cast<double>(snap.retained_bytes) / (1024.0 * 1024.0)));
        ui::readout("last frame", std::format("{} draws, {} dispatches over {} pipelines",
                                              snap.draws_last_frame, snap.dispatches_last_frame,
                                              snap.used_last_frame));
    }
    ui::end_group();

    if (ui::begin_group("Raytracing", false)) {
        const auto rt_snap = rt::snapshot();
        const auto pipes = pipeline::snapshot();
        ui::readout("runtime", !rt_snap.boot ? "off (fx.raytracing)" : rt_snap.ready ? std::format("ready, tier {}", rt_snap.raytracing_tier == 11 ? "1.1" : rt_snap.raytracing_tier == 10 ? "1.0" : "?") : rt_snap.problem[0] ? rt_snap.problem : "not started");
        ui::readout("status", rt_snap.detail);
        ui::readout("geometry", std::format("{} twins, {} draws captured, {} skipped", pipes.stream_twins,
                                            pipes.stream_draws_last_frame, pipes.stream_skipped_last_frame));
        ui::readout("last pass", std::format("{} regions, {} vertices of {} ({} refused)", rt_snap.regions, rt_snap.vertices,
                                             rt_snap.budget_vertices, rt_snap.regions_skipped));
        ui::readout("memory", std::format("{:.1f} MB blas, {:.1f} MB scratch, {}x{}",
                                          static_cast<double>(rt_snap.blas_bytes) / (1024.0 * 1024.0),
                                          static_cast<double>(rt_snap.scratch_bytes) / (1024.0 * 1024.0),
                                          rt_snap.width, rt_snap.height));
        ui::readout("passes", std::format("{} ({} faults)", rt_snap.passes, rt_snap.faults));
    }
    ui::end_group();

    if (ui::begin_group("Resources", false)) {
        for (const auto& [handle, texture] : s.textures) {
            (void)handle;
            const std::string size =
                texture.color.valid() ? std::format("{} x {}", texture.color.width, texture.color.height)
                : texture.is_target && texture.scale > 0.0f ? std::format("{:.2f} x screen", texture.scale)
                                                            : std::string("pending");
            ui::readout_colored(texture.name, texture.failed ? t.bad : t.text_dim,
                                texture.failed ? texture.error
                                               : std::format("{}  {}  {}",
                                                             texture.is_target ? "target" : "texture",
                                                             size, texture.owner));
        }
        ui::readout("meshes", std::format("{}", s.meshes.size()));
    }
    ui::end_group();
    ui::end_settings();
}

}

void select_panel_page(int page) {
    g_page = std::clamp(page, 0, kPageCount - 1);
}

void draw_panel() {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const auto& t = ui::theme();

    int effects_on = 0;
    int effect_problems = 0;
    for (const auto& [handle, effect] : s.effects) {
        (void)handle;
        effects_on += effect.enabled ? 1 : 0;
        effect_problems += effect.problem.empty() ? 0 : 1;
    }
    int shader_count = 0;
    int shader_problems = 0;
    for (const auto& [handle, shader] : s.shaders) {
        (void)handle;
        if (!shader.builtin) {
            ++shader_count;
            shader_problems += (!shader.ready && !shader.dirty) ? 1 : 0;
        }
    }
    const auto ngx_state = ngx::snapshot();
    const auto snap = pipeline::snapshot();

    const float height = ui::remaining_height();
    const ui::Rect body = ui::window_body();
    ui::draw().rect({body.x0, body.y0, body.x0 + t.rail_width, body.y1}, t.window_low);
    ui::draw().rect({body.x0 + t.rail_width, body.y0, body.x0 + t.rail_width + 1.0f, body.y1},
                    t.hairline);
    const float rail = t.rail_width - t.padding * 2.0f;
    ui::begin_column(rail, height);
    const auto heading = [&](const char* label, bool gap) {
        if (gap) {
            ui::spacing(14.0f);
        }
        const ui::font::ScopedFace caption(ui::font::Face::Caption);
        ui::tracked(t.text_faint, label, t.tracking);
    };
    const auto entry = [&](Page page, const char* label, const std::string& count, bool problem) {
        const ui::Vec2 at = ui::cursor();
        if (ui::selectable(label, g_page == page, rail)) {
            g_page = page;
        }
        if (!count.empty()) {
            cell_right(at.x + rail - 10.0f, at.y, t.row_height, problem ? t.bad : t.text_faint,
                       count, ui::font::Face::Micro);
        }
    };
    heading("MODS", false);
    entry(kEffects, "Effects",
          effect_problems > 0 ? std::format("{} failing", effect_problems)
                              : std::format("{}", s.effects.size()),
          effect_problems > 0);
    entry(kShaders, "Shaders",
          shader_problems > 0 ? std::format("{} failing", shader_problems)
                              : std::format("{}", shader_count),
          shader_problems > 0);
    heading("GAME", true);
    entry(kUpscaler, "Upscaler", "", false);
    entry(kPipelines, "Game pipelines", snap.hooks_installed ? "" : "off", !snap.hooks_installed);
    heading("BRIDGER", true);
    entry(kSettings, "Settings", "", false);
    entry(kDiagnostics, "Diagnostics", "", false);
    ui::end_column();
    ui::set_cursor_x(t.rail_width);

    ui::begin_column(ui::available_width(), height);

    std::string upscaler = "off";
    ui::Color upscaler_tone = t.text_faint;
    if (ngx_state.hooked && ngx_state.seen) {
        const bool live = ngx_state.frames_since < 10;
        const bool resetting = ngx_state.reset_rate > 0.5f;
        upscaler = !live ? "stale" : resetting ? "resetting" : "live";
        upscaler_tone = live && !resetting ? t.good : t.warn;
    }
    stat_band({
        {"gpu", s.settings.timings ? std::format("{:.2f} ms", s.stats.gpu_ms) : std::string("--"),
         t.text},
        {"effects", s.settings.bypass ? std::string("bypassed")
                                      : std::format("{} / {}", s.stats.effects_run, effects_on),
         s.settings.bypass ? t.warn : t.text},
        {"upscaler", upscaler, upscaler_tone},
        {"depth", s.stats.depth_captured ? std::string("captured") : std::string("off"),
         s.stats.depth_captured ? t.good : t.text_faint},
        {"game draws", std::format("{}", snap.draws_last_frame), t.text},
    });

    ui::begin_scroll("fx_panel", ui::remaining_height());
    switch (g_page) {
        case kEffects: draw_effects(s); break;
        case kShaders: draw_shaders(s); break;
        case kUpscaler: draw_upscaler(s); break;
        case kPipelines: draw_pipelines(s); break;
        case kSettings: draw_settings(s); break;
        default: draw_diagnostics(s); break;
    }
    ui::end_scroll();
    ui::end_column();
    ui::newline();
}

}
