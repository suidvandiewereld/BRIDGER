#include "bridger/mod.hpp"

#include <Windows.h>

#include "freecam_api.h"

BRIDGER_MOD("hello", "Hello Bridger", "2.0.0", "Bridger", "Example mod touring the SDK.")

namespace {

bridger::Setting<bool> s_greet{"greet", "Greet on load", true,
                               "Write a line to the log when the mod loads."};
bridger::Setting<float> s_gain{"gain", "Gain", 0.5f, "Nothing uses this. It is here to drag."};
bridger::Setting<int> s_count{"count", "Count", 3};
bridger::Setting<int> s_mode{"mode", "Mode", 1, "Chooses nothing, demonstrates a dropdown."};
bridger::Setting<int> s_hotkey{"hotkey", "Log a line", VK_F4,
                               "Fires on the game thread while the overlay is closed."};
bridger::Setting<std::string> s_name{"name", "Your name", "", "Used in the greeting below."};

const char* const kModes[] = {"Off", "Balanced", "Aggressive"};

int g_clicks = 0;
unsigned g_game_frames = 0;
float g_game_dt = 0.0f;
unsigned g_hotkey_hits = 0;

void on_game_frame(float dt) {
    ++g_game_frames;
    g_game_dt = dt;
}

void log_game_thread() {
    bridger::info("run_on_game_thread executed on thread {}", GetCurrentThreadId());
}

void on_hotkey() {
    ++g_hotkey_hits;
    bridger::info("hotkey pressed ({} so far)", g_hotkey_hits);
}

void draw() {
    namespace ui = bridger::ui;

    if (ui::begin_group("Configuration")) {
        ui::row(s_greet);
        ui::row(s_gain, 0.0f, 1.0f);
        ui::row(s_count, 0, 10, "items");
        ui::combo_row(s_mode, kModes, 3);
        ui::text_row(s_name, "type something");
        if (ui::key_row(s_hotkey)) {
            bridger::rebind(on_hotkey, static_cast<unsigned>(s_hotkey.get()));
        }
        if (!s_name.get().empty()) {
            ui::note(std::format("Hello, {}.", s_name.get()));
        }
    }
    ui::end_group();

    if (ui::begin_group("Actions")) {
        if (ui::button("Press me", 130.0f)) {
            ++g_clicks;
            bridger::info("button pressed ({})", g_clicks);
        }
        ui::same_line();
        if (ui::ghost_button("Log game thread id", 190.0f)) {
            bridger::run_on_game_thread(log_game_thread);
        }
        ui::progress(static_cast<float>(g_clicks % 10) / 9.0f, 220.0f,
                     ui::color(BRIDGER_COLOR_ACCENT));
    }
    ui::end_group();

    if (ui::begin_group("Engine", false)) {
        ui::readoutf("sizeof Entity", "{} bytes", bridger::type_size("Entity"));
        ui::readoutf("sizeof CameraEntity", "{} bytes", bridger::type_size("CameraEntity"));
        if (const void* fn = bridger::handler("CameraEntity", "MsgEntityUpdate"); fn != nullptr) {
            const auto rva = reinterpret_cast<std::uintptr_t>(fn) - bridger::image_base();
            ui::readoutf("CameraEntity::MsgEntityUpdate", "rva {:#x}", rva);
        } else {
            ui::readout("CameraEntity::MsgEntityUpdate", ui::bad(), "lookup failed");
        }
        ui::readoutf("CameraEntity is an Entity", "{}",
                     bridger::is_a(bridger::type("CameraEntity"), "Entity"));
    }
    ui::end_group();

    if (ui::begin_group("Runtime", false)) {
        if (g_game_frames == 0) {
            ui::readout("game frames", ui::warn(), "none yet, load into the world");
        } else {
            ui::readoutf("game frames", "{}", g_game_frames);
            ui::readoutf("game frame time", "{:.1f} ms", g_game_dt * 1000.0f);
        }
        ui::readoutf("hotkey presses", "{}", g_hotkey_hits);
        ui::readoutf("image base", "{:#x}", bridger::image_base());
    }
    ui::end_group();

    if (ui::begin_group("Services", false)) {
        if (const auto* freecam = bridger::require<FreecamApi>("freecam.v1"); freecam != nullptr) {
            double xyz[3] = {0.0, 0.0, 0.0};
            freecam->position(xyz);
            ui::readoutf("freecam", "{} at {:.1f} {:.1f} {:.1f}",
                         freecam->enabled() ? "on" : "off", xyz[0], xyz[1], xyz[2]);
            if (ui::button(freecam->enabled() ? "Disable freecam" : "Enable freecam", 190.0f)) {
                freecam->set_enabled(!freecam->enabled());
            }
            float speed = freecam->speed();
            if (ui::setting("freecam speed", speed, 0.25f, 100.0f, "m/s")) {
                freecam->set_speed(speed);
            }
        } else {
            ui::readoutf("freecam.v1", "not provided (freecam loaded: {})",
                         bridger::mod_loaded("freecam"));
        }
    }
    ui::end_group();
}

}

bool bridger::on_load() {
    if (s_greet) {
        bridger::info("hello mod loaded");
    }
    bridger::ui::panel("Hello", draw);
    bridger::game_tick(on_game_frame);
    bridger::hotkey(static_cast<unsigned>(s_hotkey.get()), on_hotkey);
    return true;
}

void bridger::on_unload() {
    bridger::info("hello mod unloaded");
}
