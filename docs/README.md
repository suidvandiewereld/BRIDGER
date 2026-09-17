<p align="center">
  <img src="../assets/bridger.png" alt="Bridger" width="320">
</p>

# Bridger guides

Four guides cover everything a mod can do. Read the first one that matches what you want to build.

| Guide | Read it when you want to | Language |
|---|---|---|
| [Bridger Script](scripting.md) | change the game by saving a file. Hooks, panels, settings, a live console, no compiler. | Lua |
| [Writing a mod](mod-authoring.md) | build a DLL mod: the template, the loader, panels, settings, hooks, hot reload. | C++ |
| [The shader system](shaders.md) | draw on the frame: post passes, world and screen geometry, the game's own pipelines, raytracing. | HLSL, C++, Lua |
| [The content layer](content.md) | replace what the engine decides: messages, functions, vtables, runtime-built objects. | C++, Lua |

## Where things live in the game folder

Everything Bridger writes sits beside `ds.exe` under `Bridger/`.

| Path | What it holds |
|---|---|
| `Bridger/mods/<id>/` | a DLL or folder mod, with its `manifest.json` |
| `Bridger/scripts/<id>.lua` | a single-file script mod |
| `Bridger/config/<id>.json` | that mod's saved settings |
| `Bridger/config/bridger.json` | the core's own settings |
| `Bridger/shaders/` | HLSL shared between mods |
| `Bridger/dumps/` | captures, golden frames, dumped pipelines |
| `Bridger/cache/` | staged copies of loaded DLLs, so a rebuild never hits a locked file |
| `Bridger/bridger.log` | the log, also shown in the overlay's Log tab |

Press **F1** in game to open the overlay.

## A first look

Save this as `Bridger/scripts/stronger.lua` and it runs at once:

```lua
hook("EntitySymbols::Entity_ExportedHeal", function(original, entity, amount)
    return original(entity, amount * 2)
end)
```

The same idea in C++, ready to build with `python tools/new_mod.py my_mod`:

```cpp
#include "bridger/mod.hpp"

BRIDGER_MOD("my_mod", "My Mod", "1.0.0", "you", "What it does.")

bool bridger::on_load() {
    bridger::info("loaded");
    return true;
}

void bridger::on_unload() {}
```
