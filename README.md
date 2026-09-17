<p align="center">
  <img src="assets/bridger.png" alt="Bridger" width="320">
</p>

# Bridger

**A native modding framework for Death Stranding, built on the Decima engine's own type system.**

Bridger attaches to the running game, reads the engine's runtime type information, and hands it to mods as a language. A mod names a class, a field, a function or a message, and Bridger finds it. Nobody writes an address down.

The whole engine is reachable this way: 9,867 types with exact layouts, 1,943 exported functions with typed signatures, every message handler on every class, and the singletons the engine keeps behind its exports. Mods read and write any live object by field name, call any export by name, hook any function or handler, replace what the engine decides, build engine objects at runtime and hand them back to the systems that consume them, draw on the frame with their own HLSL, and reach into the game's own render pipelines. All of it hot reloads, and all of it reverts when a mod unloads, reloads or faults.

PC, Steam appid `1190460`, build `2022-08-04`. Status: **working.**

---

## Thirty seconds in

Save this as `Bridger/scripts/stronger.lua`. The running game picks it up within two seconds.

```lua
hook("EntitySymbols::Entity_ExportedHeal", function(original, entity, amount)
    return original(entity, amount * 2)
end)

before("DSGazerDamageComponent", "MsgDamage", function(self, message)
    if message.CoreAmount > 50 then return skip end   -- BTs shrug off heavy hits
end)
```

Edit it and it reloads. Break it and the previous version keeps running while the error shows. Delete it and everything it changed is undone.

Then open the console, in the overlay or from a terminal with `python tools/bridger.py`, and ask the engine what anything is:

```
> sam = game.player_entity()
> sam
DSPlayerEntity@0x1f4c2a81040 { Flags = 16, ... }
> sam.Orientation.Position
WorldPosition(-1842.4, 118.9, 2201.6)
> engine.find("time of day")
{ "Game::NodeGraphBindingsGame::sGetTimeOfDay", "Game::NodeGraphBindingsGame::sSetTimeOfDay" }
> engine.Game.NodeGraphBindingsGame.sSetTimeOfDay(6.5, 0)
```

VS Code completes every class, field and function in the engine while you type. The guide is [docs/scripting.md](docs/scripting.md).

## Installing

1. Build (below), or take `winhttp.dll` and the `Bridger` folder from a release.
2. Copy `winhttp.dll` next to `ds.exe` and the `Bridger` folder beside it. In Steam: right-click Death Stranding, *Manage > Browse local files*.
3. Launch the game and press **F1**.

Only the Steam build `2022-08-04` is supported; the type index is tied to that binary. To uninstall, delete `winhttp.dll` and the `Bridger` folder.

Bridger asks Steam, through the game's own signed `steam_api64.dll`, whether your account holds a licence for the game before it loads anything. Family Sharing counts. With a copy that fails that check, the log says why and nothing else happens.

---

## What a mod can do

### Speak to the engine by name

Every lookup a mod makes resolves from the engine's own RTTI or from a static index shipped beside the core, so it works at load time and survives the author forgetting an RVA.

```cpp
auto* rtti   = bridger::type("AIIndividualComponent");                       // a type
auto* fn     = bridger::handler("CameraEntity", "MsgEntityUpdate");           // a message handler
auto  heal   = bridger::symbol<void (*)(void*, float)>("EntitySymbols",
                                                       "Entity_ExportedHeal"); // an export, typed
auto* mgr    = bridger::singleton("DSCatcher", "catcher::sGetActiveCatcherEntity"); // a global
```

`singleton` reads the export's first instructions for the `mov r64, [rip+disp32]` that loads the global, and returns null when the export loads none. Engine globals have no names of their own, but the functions that load them do.

### Read any object as a struct

`include/decima/` holds a generated header for all 8,847 engine classes, laid out from real offsets with a `static_assert` on every field. If a layout is wrong the build fails, which is the correctness check.

```cpp
#include "decima/decima.h"

auto* component = static_cast<decima::EntityComponent*>(pointer);
auto* resource = component->Resource.pointer;
```

Or skip the cast and let the engine describe the object. The overlay's **Inspect** tab, and `bridger::inspect(ptr)` from a mod, show every reflected field of a live object with bases folded in and pointers clickable, so the object graph can be walked from any starting point. From Lua the same object is a view:

```lua
body.Orientation.Position.Z = 200
body:fields()              --> { {name, type, offset, category, property}, ... }
body:messages()            --> every message its class handles
```

### Replace what the engine decides

Everything above calls the game. The content layer replaces it. Messages, functions and virtual methods are answered by the mod, with the engine's own version still reachable, and everything reverts on unload, reload and fault.

```cpp
// A message answered by the mod, found through the class's RTTI.
content::Behaviour damage;
damage.replace("DSGazerDamageComponent", "MsgDamage", on_damage);

// An engine function answered by the mod, and a gate made to say yes with no detour to write.
content::Function<bool (*)(void*, const void*)> in_range;
content::Gate allowed;
in_range.replace(range_check, on_range_check);
allowed.force(site_is_allowed, true);

// One entity behaving differently from every other of its type, through a private vtable.
content::Method<bool (*)(void*)> visible;
visible.replace(one_entity, 14, always_visible);

// The other half: a message the mod sends, to the handler the receiver's own class runs.
auto msg = content::Object::create("MsgDamage");
content::deliver(gazer, msg.get());

// The mod's own state, attached to an engine object and keyed on its identity.
content::State<Awareness> awareness;
awareness.of(mule).suspicion += 0.01f;
```

### Author the data it runs on

The RTTI records a constructor for 8,255 types, so a mod can build an engine object with the engine's own constructor, or clone a live one, and register it in the array a system selects from. The engine then consumes it as authored data.

```cpp
const content::Field<float> radius{"DSCatcherTerritoryLocator", "EncounteringRadius"};
auto site = content::Object::clone(nearest_locator);
radius.set(site.get(), 250.0f);

content::Injection sites;
sites.add(catcher_manager, 0x308, 0x310, {site.get()});
```

That exact sequence was the proof: a Catcher territory site cloned at runtime, moved to Sam and injected into the manager's array was selected by `BringCatcher`, and the Catcher staged around it. The real site was 165 m away and the spawn search only looks within 150 m of the chosen site, so the geometry rules out coincidence. Injections survive scene loads, and a restore that would clobber something the engine has since written is skipped and logged.

The guide is [docs/content.md](docs/content.md).

### Draw on the frame

Mods run their own HLSL on the game's frame: post-process passes chained in priority order, world-space geometry in engine metres through the live camera, screen-space geometry in pixels. Shaders live as files in the mod folder and recompile when saved.

```cpp
fx::Shader g_grade;   g_grade.load(BRIDGER_FX_SHADER_FULLSCREEN, "shaders/grade.hlsl");
fx::Pass g_pass;      g_pass.create("grade", g_grade);
fx::DrawList g_world; g_world.create("markers", BRIDGER_FX_SPACE_WORLD, {.depth = BRIDGER_FX_DEPTH_TEST_WRITE});

void on_frame(float) {
    g_world.sphere({x, y, z}, 0.5f, fx::rgba(255, 120, 40));
    g_world.grid({x, y, ground}, 30.0f, 1.0f, fx::rgba(120, 200, 230, 80));
}
```

Every shader gets the frame's matrices, the camera, the game's depth buffer when captured, last frame's result for temporal work, and the NGX motion vectors. A pre-upscale stage runs inside the DLSS evaluate call, so mod geometry drawn there is resolved by the game's own upscaler with the rest of the frame.

### Reach into the game's own rendering

Every pipeline the engine creates is recorded and named by a stable hash. A mod can count its draws, wrap or skip them, swap one of its shaders for its own, or splice a function into the pixel shader of every deferred geometry pass:

```hlsl
float4 bridger_material(float4 color : COLOR0, float4 position : SV_Position) : SV_Target0 {
    return color * tint;   // tint arrives through a constant buffer the mod updates each frame
}
```

The game compiles its shaders with FXC, and D3D12 refuses a DXBC container with a bad checksum, so the core carries its own DXBC reader, writer and checksum. With material bindings on, every root signature the game creates is extended with slots in register space 60, so a slider in a panel moves the game's materials with no recompile.

On top of that sits a raytracing runtime the game never knew it had. Twins of the geometry pipelines stream out every post-transform vertex the game draws, with skinning, wind and instancing already resolved by the game's own vertex shader. A compute pass unprojects them, a BLAS is rebuilt every frame, and DXR 1.1 inline ray queries trace ambient occlusion and sun shadows into the upscaler's input.

The guide is [docs/shaders.md](docs/shaders.md). `examples/fxdemo` is the tour and `mods/photoreal` a finished effect stack.

### Give the player a panel

Every mod gets a page in the overlay's **Panels** tab. Settings persist to `Bridger/config/<mod id>.json`, know the default they were declared with, and revert with one click.

```cpp
bridger::Setting<float> s_speed{"speed", "Speed", 8.0f, "Shift multiplies by four."};
bridger::Setting<int>   s_key{"key.toggle", "Toggle", VK_F3};

void draw() {
    if (bridger::ui::begin_group("Movement")) {
        bridger::ui::row(s_speed, 0.25f, 100.0f, "m/s");    // slider, saved, revertible
        if (bridger::ui::key_row(s_key)) {                  // click, then press a key
            bridger::rebind(on_toggle, s_key.get());
        }
    }
    bridger::ui::end_group();
}
```

Rows put the name in a fixed left column and the control in the right one, collapse into groups, show their help on hover and answer the search box at the top of the tab. Hotkeys stay quiet while the overlay is open.

### Change it while it runs

Rebuild a DLL and the running game reloads it, usually within a second. Mods load from a staged copy, so the build never hits a locked file. A reload tears down every panel, tick, hotkey, hook, shader and content entry the mod registered and hands settings back to the new module before its entry point runs.

Scripts reload on save. Shaders recompile on save. Manifests are re-read on reload. The game stays up.

---

## The overlay

Press **F1** in game.

| Tab | Holds |
|---|---|
| **Mods** | every installed mod, DLL or script, with enable, disable and reload |
| **Log** | the session log, tagged by mod |
| **Types** and **Symbols** | the engine's type and export indexes, browsable |
| **Panels** | each mod's settings page |
| **Inspect** | any live object, every field by name, pointers clickable |
| **Shaders** | every effect with its GPU time, the game's pipelines, depth capture, raytracing |
| **Script** | the Lua console, inside any running script's own state |

There is no ImGui. `src/ui/` is a small immediate-mode toolkit: a draw list that batches vertices per clip rect, glyphs rasterised through GDI `GetGlyphOutlineW` into a 1024x1024 R8 atlas, and widgets on top. `src/overlay/renderer.cpp` draws it with a DX12 pipeline of its own: root constants for the ortho transform, one SRV for the atlas, per-frame upload buffers, fenced. `assets/logo.bin` is a coverage bitmap packed into the same atlas, generated offline by `tools/build/gen_logo.py`.

`ui_preview` rasterises the toolkit to a BMP on the CPU, so panel layout can be reviewed without launching the game:

```bash
build/Release/ui_preview.exe preview.bmp
```

Pass `shaders` or `script` as the fourth argument to render those tabs against fabricated state, or `shaders:N` to scroll N wheel notches first.

## The debugger

A second window opens with the game: a dense desktop tool for reading the running engine. It is separate from the overlay, runs on its own thread and draws on the CPU, so it works on another monitor, while the game is paused or loading, and never touches the game's GPU. **F2** shows and hides it.

- **Explore**: every engine global, found by scanning all exported functions for the data they load; the full type database; the exported function index.
- **Object**: a live object as a tree. Nested structs, arrays and pointers expand in place; values refresh ten times a second and flash when they change. Double-click a pointer to open it; Back and Forward (Backspace, Alt+arrows) walk the history. A type name in the address bar shows that type's layout.
- **Memory**: a hex view of the selected field or object, with the selected bytes highlighted and quads that point at engine objects or game code annotated.
- **Watches**: pinned fields, live. **Log**: the session log, filterable.

Set `"debugger": false` in `Bridger/config.json` to keep it closed. `debugger_preview` opens it outside the game and captures screenshots.

---

## Building

Requires MSVC (VS 2022 or newer) and CMake 3.28+.

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DBRIDGER_GAME_DIR="<path to Death Stranding>"
```

```bash
cmake --build build --config Release
```

```bash
cmake --build build --config Release --target deploy
```

`BRIDGER_GAME_DIR` is only needed for `deploy`, which copies `winhttp.dll` into the game directory and `bridger.dll` with its indexes into `<game>/Bridger`. The Python tools find the Steam install themselves, or read the same `BRIDGER_GAME_DIR` environment variable. Runtime output lands in `<game>/Bridger/bridger.log`.

### First run

The repository carries nothing derived from the game: no type database, no engine headers, no Lua types. One script rebuilds all of it from your own install, in two runs.

```bash
python tools/bootstrap.py
```

The first run reads `ds.exe` and writes the static type dump. Deploy, launch the game once and quit; the core writes the export table to `<game>/Bridger/dumps/` on that launch. Run the script again and it folds the dump in, writes the indexes, `include/bridger/lua/engine.d.lua` and `include/decima/`, and asks for one more deploy. Steps already done are skipped, and `deploy` copies whichever of these files exist, so nothing blocks the first build.

### Writing a mod

The quickest mod is a script: create `Bridger/scripts/my_mod.lua`, or scaffold a folder mod straight into the game with `python tools/new_mod.py my_mod --lua`. A DLL mod in C++ is the choice for work that runs per vertex or per frame over thousands of objects, and for the shader system's GPU resources:

```bash
python tools/new_mod.py my_mod --name "My Mod" --author "you"
```

```cpp
#include "bridger/mod.hpp"

BRIDGER_MOD("my_mod", "My Mod", "1.0.0", "you", "What it does.")

bool bridger::on_load() {
    bridger::info("loaded at {:#x}", bridger::image_base());
    bridger::ui::panel("My Mod", draw);
    return true;
}
```

[include/bridger/mod.hpp](include/bridger/mod.hpp) is a header-only C++ layer over the C ABI (API version 11). It wires up `decima::image_base` when the generated headers are on the include path, so `decima::bind<>()` just works. The guide is [docs/mod-authoring.md](docs/mod-authoring.md), and [docs/README.md](docs/README.md) indexes all four guides.

---

## How it attaches

`ds.exe` imports 14 functions from `WINHTTP.dll` and carries no DRM wrapper, so a forwarding `winhttp.dll` dropped beside the executable is loaded during process init. No injector, no launcher. The proxy forwards all 91 system exports and loads the core.

`bridger.dll` deliberately keeps its static imports to `GDI32`, `KERNEL32`, `USER32` and the shader compiler. It loads during the host's own import resolution, and a static import on `d3d12` or `dxgi` would drag the graphics stack up before the game initialises it and kill the process silently. Those entry points resolve with `LoadLibrary` at overlay init, and so do `wintrust` and `crypt32` for the licence check.

The game tick, the hook every mod's game-thread work hangs off, sits on the world's per-frame update: the main-thread function that advances game time, fans entity updates out to the job workers and waits for them. Everything a mod does there runs after every entity has taken its `MsgEntityUpdate` for the frame.

## The type database

`data/rtti/ds/` holds the Decima RTTI database for this exact build, generated on your machine by `tools/bootstrap.py` and never committed. Two dumpers produce it and agree with each other.

```bash
python tools/rtti/dump_static.py --validate
```

The static dumper scans `.data` in `ds.exe` for RTTI descriptors and writes every class and enum with its descriptor address, size, alignment, constructor and destructor addresses, base offsets and full member layout: 9,867 types, 8,840 of 8,847 classes byte-exact. The runtime dumper waits for the engine to register its types, then scans committed memory and writes `<game>/Bridger/dumps/rtti.json` with live addresses and engine-assigned ids, including all 91 types the static scan is blind to. All scanning goes through `ReadProcessMemory`, because the game frees memory continuously and a direct dereference faults.

Query it from the command line:

```bash
python tools/rtti/rtti.py show EntityComponent
```

```bash
python tools/rtti/rtti.py tree Entity --depth 1
```

```bash
python tools/rtti/disasm.py handlers CameraEntity
```

```bash
python tools/rtti/symbols.py find Weather --kind function
```

`rtti.py` has `stats`, `find`, `show`, `tree` and `msg`; `show` flattens the layout across every base, so the printed offsets are the real ones in memory. `disasm.py` gives annotated disassembly (`fn`), every direct call site (`callers`) and each class's message handler table with addresses (`handlers`); it needs `pip install capstone`. `symbols.py` walks the 540 export groups. `compare.py` diffs the two dumps, and `vtables.py` replays the content layer's vtable resolution offline.

### Generating the headers

```bash
python tools/codegen/gen_headers.py
```

```bash
cmake --build build --config Release --target bridger_headers
```

The generator reads both dumps and writes `include/decima/`: 8,847 structs with explicit padding under `#pragma pack(1)`, 1,020 enums and 1,735 typed function pointers. RTTI properties are getter and setter backed and occupy no storage, so they are left out of the structs and emitted as accessor addresses in `properties.h`.

```cpp
#include "decima/decima.h"
using namespace decima;

decima::image_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

using SetFaction = symbols::AIBehaviorGroupSymbols_AIBehaviorGroup_ExportedSetFaction_t;
auto set_faction = bind<SetFaction>(
    symbols::AIBehaviorGroupSymbols_AIBehaviorGroup_ExportedSetFaction_rva);
set_faction(group, faction);
```

---

## What is here

| Path | What it is |
|---|---|
| `src/proxy/` | the `winhttp.dll` proxy: forwards all 91 system exports, loads the core |
| `src/core/` | `bridger.dll`: logging, module resolution, signature scanning, memory walking, per-mod settings |
| `src/decima/` | engine RTTI structures and the in-process type dumper |
| `src/loader/` | mod discovery, manifests, dependency order, the mod API |
| `src/ui/` | the immediate-mode UI: draw list, GDI font atlas, widgets |
| `src/overlay/` | DX12 swapchain hooks and the overlay tabs |
| `src/debugger/` | the debugger window: globals discovery, object model, CPU-drawn panes |
| `src/fx/` | the shader system: passes, draw lists, HLSL hot reload, depth capture, pipeline interception, DXBC splicing, the raytracing runtime |
| `src/content/` | the content layer: message, function and vtable replacement, delivery, per-object state, reflection, runtime objects, injection, revertible writes |
| `src/script/` | Bridger Script: the Lua runtime, RTTI-driven values, the x64 FFI and hook thunks, the console and its pipe |
| `include/bridger/` | `api.h`, the C ABI mods compile against, and the C++ layer over it |
| `include/bridger/lua/` | the API and the whole engine, described for the Lua language server (`engine.d.lua` is generated) |
| `include/decima/` | generated engine headers, never committed |
| `data/rtti/ds/` | the RTTI database for this build, generated locally, never committed |
| `tools/rtti/` | static dumper, database queries, disassembler, vtable resolution |
| `tools/codegen/` | the header generator |
| `tools/script/` | signature export and Lua type generation |
| `tools/bridger.py` | the console in a terminal: eval, run, scripts, reload, log |
| `tools/new_mod.py` | scaffolds a DLL or Lua mod |
| `tools/bootstrap.py` | rebuilds the database, headers and Lua types from your own install |
| `tools/ui_preview/`, `tools/debugger_preview/` | render the overlay and the debugger to a BMP without the game |
| `tools/fx_check/` | compiles mod shaders against the prelude without the game |
| `examples/hello`, `examples/fxdemo` | a minimal mod and the shader system tour |
| `mods/freecam` | a free-flying camera, the worked example of a game-thread hook. Its patched gear shaders are game bytes and never committed; `tools/patch_dither_pipelines.py` regenerates them from a pipeline dump. |
| `mods/photoreal` | photographic post-processing on the shader system |
| `templates/` | the scaffolds `new_mod.py` writes from |
| `tests/` | header compile check, FFI, script runtime, DXBC, upscale and raytracing tests |
| `docs/` | the four guides and the reverse-engineering notes |

## Roadmap

1. ~~Static RTTI dumper.~~ Done: 9,867 types, 8,840 of 8,847 classes byte-exact.
2. ~~Runtime dumper.~~ Done: 9,974 types with live addresses and engine-assigned ids, including all 91 the static scan is blind to.
3. ~~`ExportedSymbols`.~~ Done: 540 groups, 2,888 symbols, 1,943 callable functions.
4. ~~C++ engine headers.~~ Done: 8,847 structs, 1,020 enums, 1,735 typed functions.
5. ~~Plugin loader, manifest format and a stable mod ABI.~~ Done.
6. ~~A content layer.~~ Done: message, function and vtable replacement, message delivery, per-object state, reflection by field name, runtime construction and cloning, and array injection that survives scene loads, all revertible.
7. ~~Mods without a build.~~ Done: Lua mods applied on save, engine objects by field name, every export callable and hookable by name, chained hooks across scripts, a live console in game and over a pipe, and editor types for the whole engine.
8. The game's own rendering. Material hooks with live bindings are proven in game. Pipeline interception, shader replacement and the raytracing runtime are built and pass on the software DX12 device; the in-game check is next.
9. An external mod manager sharing the manifest and config format.
10. Loose-file asset overrides, so mods ship `.core` files in place of repacked archives. The one remaining path to new meshes, animations and audio.

## Credits

Standing on the shoulders of [ShadelessFox/decima-workshop](https://github.com/ShadelessFox/decima), [Nukem9/HZDCoreEditor](https://github.com/Nukem9/HZDCoreEditor), [cauldron-decima/cauldron](https://github.com/cauldronloader/cauldron) and [Wunkolo/DecimaTools](https://github.com/Wunkolo/DecimaTools). The static dumper was validated against Decima Workshop's type database.

## License

Copyright (c) 2026 Marquis. All rights reserved. You may read, build and run Bridger for your own use, with a copy of Death Stranding you own; the core checks that with Steam at startup. Mods that use Bridger through its public API are yours to distribute however you like, as long as they don't bundle Bridger itself or any game file. Redistributing Bridger needs written permission. The full terms are in [LICENSE](LICENSE). MinHook, nlohmann/json and Lua keep their own licenses.

## Legal

Bridger is an unofficial fan project with no affiliation to, and no endorsement from, Kojima Productions, 505 Games or Guerrilla Games. Death Stranding and its assets belong to their owners. This repository ships no game files and nothing derived from them; everything of that kind is generated from your own install.
