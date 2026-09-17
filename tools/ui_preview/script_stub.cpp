
#include "loader/registry.h"
#include "script/runtime.h"

namespace bridger::script {

std::vector<Status> statuses() {
    return {
        {"console", true, "", 0, 412, 0, 1284},
        {"vitals", true, "", 0, 18221, 0, 311},
        {"lodrange", true, "", 0, 52930, 3, 208},
        {"weather_dial", true,
         "mods/weather_dial/main.lua:41: attempt to index a nil value (local 'manager')", 3, 900, 1, 177},
    };
}

std::vector<Line> console_lines(std::size_t) {
    return {
        {0, "> console", "game.player_entity()"},
        {1, "console", "DSPlayerEntity@0x1f4c2a81040 { Flags = 16, Faction = ... }"},
        {0, "> console", "sam = _; sam.Orientation.Position"},
        {1, "console", "WorldPosition(-1842.4, 118.9, 2201.6)"},
        {0, "> console", "engine.find('time of day')"},
        {1, "console", "{ \"Game::NodeGraphBindingsGame::sGetTimeOfDay\", \"Game::NodeGraphBindingsGame::sSetTimeOfDay\" }"},
        {0, "vitals", "healed"},
        {2, "lodrange", "scaled 1204 meshes so far"},
        {0, "> console", "hook('EntitySymbols::Entity_ExportedHeal', function(original, e, amount) return original(e, amount * 2) end)"},
        {1, "console", "hook on EntitySymbols::Entity_ExportedHeal (0 calls)"},
        {4, "weather_dial", "error in frame: mods/weather_dial/main.lua:41: attempt to index a nil value (local 'manager')"},
        {4, "weather_dial", "stack traceback:"},
        {4, "weather_dial", "\tmods/weather_dial/main.lua:41: in function <mods/weather_dial/main.lua:38>"},
        {0, "> console", "sam.Orientaton"},
        {4, "console", "DSPlayerEntity has no field 'Orientaton' (did you mean 'Orientation'?)"},
    };
}

void console_clear() {}
void submit(const std::string&, const std::string&) {}
std::vector<std::string> complete(const std::string&, const std::string&) { return {}; }

}

namespace bridger::loader {

Registry& Registry::instance() {
    static Registry registry;
    return registry;
}

void Registry::request_reload(const std::string&) {}

const std::string& Registry::active_mod() const {
    return active_;
}

}
