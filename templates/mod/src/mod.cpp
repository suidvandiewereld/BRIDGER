#include "bridger/mod.hpp"

#include <Windows.h>

BRIDGER_MOD("__MOD_ID__", "__MOD_NAME__", "1.0.0", "__MOD_AUTHOR__", "__MOD_DESCRIPTION__")

namespace {

bridger::Setting<bool> s_enabled{"enabled", "Enabled", true};
bridger::Setting<float> s_strength{"strength", "Strength", 0.5f,
                                   "How hard the thing is done."};
bridger::Setting<int> s_hotkey{"key.toggle", "Toggle", VK_F5};

int g_clicks = 0;

void on_hotkey() {
    s_enabled.set(!s_enabled.get());
    bridger::info("toggled {}", s_enabled.get() ? "on" : "off");
}

void draw() {
    namespace ui = bridger::ui;

    if (ui::begin_group("Settings")) {
        ui::row(s_enabled);
        ui::row(s_strength, 0.0f, 1.0f);
        if (ui::key_row(s_hotkey)) {
            bridger::rebind(on_hotkey, static_cast<unsigned>(s_hotkey.get()));
        }
    }
    ui::end_group();

    if (ui::begin_group("Actions")) {
        if (ui::button("Do the thing", 150.0f)) {
            ++g_clicks;
            bridger::info("did the thing ({})", g_clicks);
        }
    }
    ui::end_group();

    if (ui::begin_group("Status", false)) {
        ui::readoutf("image base", "{:#x}", bridger::image_base());
        ui::readoutf("clicks", "{}", g_clicks);
        if (const void* entity = bridger::type("Entity"); entity != nullptr) {
            ui::readoutf("Entity rtti", "{}", entity);
        } else {
            ui::readout("Entity rtti", ui::warn(), "not found, run a scan first");
        }
    }
    ui::end_group();
}

}

bool bridger::on_load() {
    bridger::info("__MOD_ID__ loaded");
    bridger::ui::panel("__MOD_NAME__", draw);
    bridger::hotkey(static_cast<unsigned>(s_hotkey.get()), on_hotkey);
    return true;
}

void bridger::on_unload() {
    bridger::info("__MOD_ID__ unloaded");
}
