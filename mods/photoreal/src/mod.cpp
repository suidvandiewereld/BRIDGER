
#include "bridger/mod.hpp"

#include <Windows.h>

#include <cmath>

BRIDGER_MOD("photoreal", "Photoreal", "1.0.0", "Marquis",
            "Camera pipeline: adaptive sharpening, local contrast, aerial perspective, film "
            "response, lens.")

namespace {

namespace fx = bridger::fx;

bridger::Setting<bool> s_enabled{"enabled", "Enabled", true};
bridger::Setting<int> s_preset{"preset", "Preset", 1,
                               "A whole look. Changing one below leaves the preset showing as "
                               "custom."};

bridger::Setting<float> s_exposure{"camera.exposure", "Exposure", 0.0f, "Stops."};
bridger::Setting<float> s_temperature{"camera.temperature", "White balance", 0.05f,
                                      "Negative cools, positive warms."};
bridger::Setting<float> s_shoulder{"camera.shoulder", "Highlight rolloff", 0.35f};
bridger::Setting<float> s_toe{"camera.toe", "Black lift", 0.008f};
bridger::Setting<float> s_film_contrast{"camera.contrast", "Film contrast", 0.15f};
bridger::Setting<float> s_saturation{"camera.saturation", "Saturation", 1.0f};

bridger::Setting<float> s_sharpness{"detail.sharpness", "Sharpness", 0.5f,
                                    "Contrast adaptive, so it recovers detail softened by the "
                                    "upscaler without haloing edges."};
bridger::Setting<float> s_distance_sharpen{"detail.distance", "Distance sharpen", 0.6f,
                                           "Extra sharpening far away, where texture detail is "
                                           "weakest."};
bridger::Setting<float> s_clarity{"detail.clarity", "Local contrast", 0.6f};

bridger::Setting<float> s_haze_density{"haze.density", "Density", 0.8f};
bridger::Setting<float> s_haze_start{"haze.start", "Start distance", 0.02f,
                                     "As a fraction of a kilometre, so 0.02 is twenty metres."};
bridger::Setting<float> s_haze_brightness{"haze.brightness", "Brightness", 0.85f,
                                          "The haze colour is taken from the sky in the frame, so "
                                          "it follows the weather and the time of day."};
bridger::Setting<float> s_haze_desaturate{"haze.desaturate", "Distance desaturation", 0.5f};

bridger::Setting<float> s_glare{"lens.glare", "Veiling glare", 0.05f};
bridger::Setting<float> s_highlights{"lens.highlights", "Highlight bloom", 0.35f};
bridger::Setting<float> s_bloom_threshold{"lens.bloom_threshold", "Bloom threshold", 0.75f};
bridger::Setting<float> s_vignette{"lens.vignette", "Vignette", 0.5f};
bridger::Setting<float> s_aberration{"lens.aberration", "Chromatic aberration", 0.35f};
bridger::Setting<float> s_grain{"lens.grain", "Grain", 0.025f,
                                "Shot noise, weighted towards the shadows. Re-rolled every frame, "
                                "so anything above a trace of it shimmers by design."};

bridger::Setting<int> s_key_toggle{"key.toggle", "Toggle", VK_F9};

const char* const kPresetNames[] = {"Custom", "Photoreal", "Overcast", "Warm evening",
                                    "Clean (no grain)", "Off the shelf"};

struct alignas(16) PhotorealConstants {
    float exposure;
    float temperature;
    float shoulder;
    float toe;

    float film_contrast;
    float saturation;
    float sharpness;
    float distance_sharpen;

    float clarity;
    float haze_density;
    float haze_start;
    float haze_brightness;

    float haze_desaturate;
    float glare;
    float highlights;
    float bloom_threshold;

    float vignette;
    float aberration;
    float grain;
    float depth_available;
};

struct alignas(16) BlurConstants {
    float direction[2];
    float unused[2];
};

fx::Shader g_down_shader;
fx::Shader g_blur_shader;
fx::Shader g_composite_shader;

fx::RenderTarget g_half;
fx::RenderTarget g_temp;
fx::RenderTarget g_blurred;

fx::Pass g_down;
fx::Pass g_blur_h1;
fx::Pass g_blur_v1;
fx::Pass g_blur_h2;
fx::Pass g_blur_v2;
fx::Pass g_composite;

bool g_have_depth = false;

void apply_preset(int preset) {
    switch (preset) {
        case 1:
            s_exposure.set(0.0f); s_temperature.set(0.05f); s_shoulder.set(0.35f);
            s_toe.set(0.008f); s_film_contrast.set(0.15f); s_saturation.set(1.0f);
            s_sharpness.set(0.5f); s_distance_sharpen.set(0.6f); s_clarity.set(0.6f);
            s_haze_density.set(0.8f); s_haze_start.set(0.02f); s_haze_brightness.set(0.85f);
            s_haze_desaturate.set(0.5f); s_glare.set(0.05f); s_highlights.set(0.35f);
            s_bloom_threshold.set(0.75f); s_vignette.set(0.5f); s_aberration.set(0.35f);
            s_grain.set(0.025f);
            break;
        case 2:
            s_exposure.set(0.1f); s_temperature.set(-0.25f); s_shoulder.set(0.45f);
            s_toe.set(0.012f); s_film_contrast.set(0.08f); s_saturation.set(0.88f);
            s_sharpness.set(0.55f); s_distance_sharpen.set(0.7f); s_clarity.set(0.45f);
            s_haze_density.set(1.4f); s_haze_start.set(0.012f); s_haze_brightness.set(0.95f);
            s_haze_desaturate.set(0.7f); s_glare.set(0.08f); s_highlights.set(0.25f);
            s_bloom_threshold.set(0.7f); s_vignette.set(0.45f); s_aberration.set(0.2f);
            s_grain.set(0.02f);
            break;
        case 3:
            s_exposure.set(-0.1f); s_temperature.set(0.45f); s_shoulder.set(0.3f);
            s_toe.set(0.006f); s_film_contrast.set(0.22f); s_saturation.set(1.08f);
            s_sharpness.set(0.5f); s_distance_sharpen.set(0.55f); s_clarity.set(0.7f);
            s_haze_density.set(1.0f); s_haze_start.set(0.02f); s_haze_brightness.set(1.0f);
            s_haze_desaturate.set(0.35f); s_glare.set(0.1f); s_highlights.set(0.6f);
            s_bloom_threshold.set(0.68f); s_vignette.set(0.55f); s_aberration.set(0.45f);
            s_grain.set(0.022f);
            break;
        case 4:
            s_exposure.set(0.0f); s_temperature.set(0.05f); s_shoulder.set(0.35f);
            s_toe.set(0.008f); s_film_contrast.set(0.15f); s_saturation.set(1.0f);
            s_sharpness.set(0.6f); s_distance_sharpen.set(0.6f); s_clarity.set(0.6f);
            s_haze_density.set(0.8f); s_haze_start.set(0.02f); s_haze_brightness.set(0.85f);
            s_haze_desaturate.set(0.5f); s_glare.set(0.0f); s_highlights.set(0.2f);
            s_bloom_threshold.set(0.8f); s_vignette.set(0.25f); s_aberration.set(0.0f);
            s_grain.set(0.0f);
            break;
        case 5:
            s_exposure.set(0.0f); s_temperature.set(0.0f); s_shoulder.set(0.0f);
            s_toe.set(0.0f); s_film_contrast.set(0.0f); s_saturation.set(1.0f);
            s_sharpness.set(0.5f); s_distance_sharpen.set(0.5f); s_clarity.set(0.0f);
            s_haze_density.set(0.0f); s_haze_start.set(0.02f); s_haze_brightness.set(0.85f);
            s_haze_desaturate.set(0.0f); s_glare.set(0.0f); s_highlights.set(0.0f);
            s_bloom_threshold.set(0.8f); s_vignette.set(0.0f); s_aberration.set(0.0f);
            s_grain.set(0.0f);
            break;
        default:
            break;
    }
}

void push_constants() {
    PhotorealConstants c{};
    c.exposure = s_exposure;
    c.temperature = s_temperature;
    c.shoulder = s_shoulder;
    c.toe = s_toe;
    c.film_contrast = s_film_contrast;
    c.saturation = s_saturation;
    c.sharpness = s_sharpness;
    c.distance_sharpen = s_distance_sharpen;
    c.clarity = s_clarity;
    c.haze_density = s_haze_density;
    c.haze_start = s_haze_start;
    c.haze_brightness = s_haze_brightness;
    c.haze_desaturate = s_haze_desaturate;
    c.glare = s_glare;
    c.highlights = s_highlights;
    c.bloom_threshold = s_bloom_threshold;
    c.vignette = s_vignette;
    c.aberration = s_aberration;
    c.grain = s_grain;
    c.depth_available = g_have_depth ? 1.0f : 0.0f;
    g_composite.constants(c);

    BlurConstants blur{};
    blur.direction[0] = 4.0f;
    g_blur_h1.constants(blur);
    blur.direction[0] = 10.0f;
    g_blur_h2.constants(blur);
    blur.direction[0] = 0.0f;
    blur.direction[1] = 4.0f;
    g_blur_v1.constants(blur);
    blur.direction[1] = 10.0f;
    g_blur_v2.constants(blur);
}

void set_enabled(bool on) {
    for (fx::Pass* pass : {&g_down, &g_blur_h1, &g_blur_v1, &g_blur_h2, &g_blur_v2, &g_composite}) {
        pass->enable(on);
    }
}

void on_frame(float) {
    const fx::Frame frame = fx::frame();
    g_have_depth = frame.depth_available;
    push_constants();
    set_enabled(s_enabled);
}

void toggle() {
    s_enabled.set(!s_enabled.get());
    bridger::info("photoreal {}", s_enabled.get() ? "on" : "off");
}

void draw() {
    namespace ui = bridger::ui;

    ui::row(s_enabled);
    if (ui::combo_row(s_preset, kPresetNames, 6)) {
        apply_preset(s_preset.get());
    }

    if (ui::begin_group("Camera")) {
        if (ui::row(s_exposure, -2.0f, 2.0f, "ev")) { s_preset.set(0); }
        if (ui::row(s_temperature, -1.0f, 1.0f)) { s_preset.set(0); }
        if (ui::row(s_shoulder, 0.0f, 1.0f)) { s_preset.set(0); }
        if (ui::row(s_toe, 0.0f, 0.05f)) { s_preset.set(0); }
        if (ui::row(s_film_contrast, 0.0f, 1.0f)) { s_preset.set(0); }
        if (ui::row(s_saturation, 0.0f, 2.0f)) { s_preset.set(0); }
    }
    ui::end_group();

    if (ui::begin_group("Detail")) {
        if (ui::row(s_sharpness, 0.0f, 1.0f)) { s_preset.set(0); }
        if (ui::row(s_distance_sharpen, 0.0f, 1.0f)) { s_preset.set(0); }
        if (ui::row(s_clarity, 0.0f, 2.0f)) { s_preset.set(0); }
    }
    ui::end_group();

    if (ui::begin_group("Aerial perspective")) {
        if (ui::row(s_haze_density, 0.0f, 4.0f)) { s_preset.set(0); }
        if (ui::row(s_haze_start, 0.0f, 0.5f, "km")) { s_preset.set(0); }
        if (ui::row(s_haze_brightness, 0.0f, 2.0f)) { s_preset.set(0); }
        if (ui::row(s_haze_desaturate, 0.0f, 1.0f)) { s_preset.set(0); }
        if (!g_have_depth) {
            ui::note("Needs the game's depth buffer: turn on 'Capture game depth' in the Shaders "
                     "tab. Without it the haze is skipped and everything else still works.");
        }
    }
    ui::end_group();

    if (ui::begin_group("Lens")) {
        if (ui::row(s_glare, 0.0f, 0.3f)) { s_preset.set(0); }
        if (ui::row(s_highlights, 0.0f, 2.0f)) { s_preset.set(0); }
        if (ui::row(s_bloom_threshold, 0.0f, 1.0f)) { s_preset.set(0); }
        if (ui::row(s_vignette, 0.0f, 1.0f)) { s_preset.set(0); }
        if (ui::row(s_aberration, 0.0f, 2.0f)) { s_preset.set(0); }
        if (ui::row(s_grain, 0.0f, 0.1f)) { s_preset.set(0); }
    }
    ui::end_group();

    if (ui::begin_group("Keys", false)) {
        if (ui::key_row(s_key_toggle)) {
            bridger::rebind(toggle, static_cast<unsigned>(s_key_toggle.get()));
        }
    }
    ui::end_group();

    if (ui::begin_group("Status", false)) {
        const auto frame = fx::frame();
        ui::readout("shaders", g_composite_shader.ready() && g_blur_shader.ready()
                                   && g_down_shader.ready()
                                   ? "compiled"
                                   : "failed, see the Shaders tab");
        ui::readoutf("frame", "{} x {}", frame.width, frame.height);
        ui::readout("game depth", g_have_depth ? ui::good() : ui::warn(),
                    g_have_depth ? "available" : "not captured");
    }
    ui::end_group();
}

}

bool bridger::on_load() {
    if (fx::table() == nullptr) {
        bridger::error("this loader has no shader system");
        return false;
    }

    g_down_shader.load(BRIDGER_FX_SHADER_FULLSCREEN, "shaders/blur_down.hlsl");
    g_blur_shader.load(BRIDGER_FX_SHADER_FULLSCREEN, "shaders/blur.hlsl");
    g_composite_shader.load(BRIDGER_FX_SHADER_FULLSCREEN, "shaders/photoreal.hlsl");

    g_half.create(0.25f, BRIDGER_FX_FORMAT_RGBA16F, false, "photoreal half");
    g_temp.create(0.25f, BRIDGER_FX_FORMAT_RGBA16F, false, "photoreal scratch");
    g_blurred.create(0.25f, BRIDGER_FX_FORMAT_RGBA16F, false, "photoreal blurred");

    fx::PassOptions options;
    options.stage = BRIDGER_FX_STAGE_POST;

    options.priority = 100;
    options.output = g_half.handle();
    g_down.create("photoreal downsample", g_down_shader, options);

    options.priority = 101;
    options.output = g_temp.handle();
    g_blur_h1.create("photoreal blur h", g_blur_shader, options);
    g_blur_h1.texture(0, g_half);

    options.priority = 102;
    options.output = g_blurred.handle();
    g_blur_v1.create("photoreal blur v", g_blur_shader, options);
    g_blur_v1.texture(0, g_temp);

    options.priority = 103;
    options.output = g_temp.handle();
    g_blur_h2.create("photoreal blur h wide", g_blur_shader, options);
    g_blur_h2.texture(0, g_blurred);

    options.priority = 104;
    options.output = g_blurred.handle();
    g_blur_v2.create("photoreal blur v wide", g_blur_shader, options);
    g_blur_v2.texture(0, g_temp);

    options.priority = 110;
    options.output = 0;
    g_composite.create("photoreal", g_composite_shader, options);
    g_composite.texture(0, g_blurred);

    bridger::hotkey(static_cast<unsigned>(s_key_toggle.get()), toggle);
    bridger::tick(on_frame);
    bridger::ui::panel("Photoreal", draw);
    bridger::info("photoreal loaded, preset {}", kPresetNames[std::clamp(s_preset.get(), 0, 5)]);
    return true;
}

void bridger::on_unload() {}
