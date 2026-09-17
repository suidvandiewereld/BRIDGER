# Writing a mod

*A mod is a folder in `<game>/Bridger/mods/` with a `manifest.json` and a DLL. The loader reads the manifest, sorts out dependencies, loads the DLL and calls into it.*

Bridger also runs mods written in Lua: one `.lua` file in `Bridger/scripts/`, live the moment you save it, with the engine reachable by field and function name. Start there unless you need C++. The Lua guide is [scripting.md](scripting.md). This guide covers DLL mods.

**Contents**

1. [Start from the template](#start-from-the-template)
2. [The shape of a mod](#the-shape-of-a-mod)
3. [Logging](#logging)
4. [Finding engine code](#finding-engine-code)
5. [Hooks](#hooks)
6. [Panels](#panels)
7. [Settings](#settings)
8. [Configuration rows](#configuration-rows)
9. [Hotkeys and keys](#hotkeys-and-keys)
10. [Talking to other mods](#talking-to-other-mods)
11. [Running every frame](#running-every-frame)
12. [Drawing with shaders](#drawing-with-shaders)
13. [Replacing engine behaviour](#replacing-engine-behaviour)
14. [Reading engine memory](#reading-engine-memory)
15. [Inspecting live objects](#inspecting-live-objects)
16. [Hot reload](#hot-reload)
17. [The manifest](#the-manifest)
18. [Quick reference](#quick-reference)

---

## Start from the template

```bash
python tools/new_mod.py my_mod --name "My Mod" --author "you"
```

That writes `mods/my_mod/` with a build file, a manifest and a working panel. Build and install it:

```bash
cmake -S mods/my_mod -B mods/my_mod/build -A x64 -DBRIDGER_DIR=<bridger> -DBRIDGER_GAME_DIR=<game>
```

```bash
cmake --build mods/my_mod/build --config Release --target install_mod
```

Launch the game. The mod appears under the **Mods** tab and its panel under **Panels**.

## The shape of a mod

```cpp
#include "bridger/mod.hpp"

BRIDGER_MOD("my_mod", "My Mod", "1.0.0", "you", "What it does.")

bool bridger::on_load() {
    bridger::info("loaded");
    return true;
}

void bridger::on_unload() {}
```

`BRIDGER_MOD` emits the three exported entry points, captures the loader API and, when the generated Decima headers are on the include path, wires up `decima::image_base` so the engine bindings resolve.

Return `false` from `on_load` to mark the mod failed. The overlay shows the reason.

## Logging

`bridger::info`, `warn`, `error` and `trace` take `std::format` arguments. Every line carries the mod id, in `bridger.log` and in the overlay's **Log** tab.

```cpp
bridger::info("spawned {} at {:.1f} m", count, distance);
```

## Finding engine code

Hard-coded addresses pin a mod to one build. Four lookups avoid them, and all four work from `on_load`, because the loader ships static indexes next to `bridger.dll`.

### Types

`bridger::type("Entity")` returns the live RTTI descriptor. It resolves from `Bridger/types.json`, so it needs no memory scan and works at load time.

```cpp
const auto* rtti = bridger::type("AIIndividualComponent");
std::size_t size = bridger::type_size("AIIndividualComponent");
const auto* live = bridger::rtti_of(object);       // the descriptor of a live object
bool yes = bridger::is_a(rtti, "Entity");           // walks the base tables
bool also = bridger::object_is_a(object, "Entity");
```

### Exported functions

`Bridger/symbols.json` names every exported engine function as `Group::name`, with the full parameter type list. That list is the quickest way to work out a signature.

```cpp
using SetFaction = void (*)(decima::AIBehaviorGroup*, decima::AIFaction*);
auto set_faction = bridger::symbol<SetFaction>("AIBehaviorGroupSymbols",
                                               "AIBehaviorGroup_ExportedSetFaction");

auto position = bridger::symbol<Position (*)(void*)>("EntitySymbols",
                                                     "Entity_ExportedGetPosition");
```

`bridger::at_rva<Fn>(rva)` takes an address from the generated `decima/symbols.h` when you have one. Browse types and symbols in the overlay's **Types** and **Symbols** tabs, and regenerate the shipped indexes with `python tools/rtti/export_index.py`.

### Message handlers

Every engine class registers its message handlers in its RTTI descriptor. `bridger::handler` reads that table, so a hook can name the class and the message and never touch an address.

```cpp
auto* fn = bridger::handler("ThirdPersonPlayerCameraComponent", "MsgUpdateBaseTransform");
```

### Globals

Engine singletons have no name of their own, but most are loaded by an export that does. Name the export and Bridger reads the address out of its code.

```cpp
auto* registry = bridger::singleton("DsGameActorCommandSymbols",
                                    "DsGameActorCommand_sExportedGetGameActorEntity");
auto* catcher  = bridger::singleton("DSCatcher", "catcher::sGetActiveCatcherEntity");
```

`data_ref` returns the address of the global and `singleton` dereferences it once. Both scan the export's first bytes for `mov r64, [rip+disp32]`, skip `__security_cookie`, and return null when the export loads no global in its prologue.

> [!TIP]
> Treat null as "wrong build". Pick an export that touches the singleton for real. Retrying with another index only finds a different wrong answer.

### Byte patterns

`bridger::scan<Sig>("48 8B ?? 05")` finds a byte pattern in the game's code. Reach for it last.

## Hooks

`bridger::Hook` owns the trampoline and removes itself on destruction.

```cpp
bridger::Hook<void (*)(decima::Entity*)> g_update;

void update_detour(decima::Entity* entity) {
    g_update.call(entity);
}

g_update.install("EntitySymbols", "Entity_Update", update_detour);
```

A handler hook needs no address at all:

```cpp
bridger::Hook<void (*)(void*, void*)> g_update;
g_update.install_handler("CameraEntity", "MsgEntityUpdate", update_detour);
```

Hooks are tracked per mod. One you forget to remove is removed for you on unload and logged as a warning. Fix the warning anyway; it means `on_unload` is incomplete.

> [!NOTE]
> `Hook` is right for watching. For anything that changes what the game decides, use `content::Function` from the [content layer](content.md). It is reverted together with the rest of the mod's work, on unload, reload and fault.

## Panels

Register a panel once in `on_load`. The callback runs inside the overlay each frame. Each mod gets its own page in the **Panels** tab, picked from the rail on the left.

```cpp
void draw() {
    bridger::ui::header("Tuning");
    bridger::ui::textf("health {}", value);
    if (bridger::ui::button("Reset")) { value = 0; }
    bridger::ui::checkbox("Enabled", g_enabled);
    bridger::ui::slider("strength", g_strength, 0.0f, 1.0f);   // float or int
    bridger::ui::input_text("name", g_name, "placeholder");
}

bridger::ui::panel("My Mod", draw);
```

Panels draw on the render thread. To call the engine from a button, queue the call for the next game tick:

```cpp
if (bridger::ui::button("Teleport")) {
    bridger::run_on_game_thread(do_teleport);
}
```

## Settings

A `Setting<T>` declares a storage key, the label a panel shows, a default and optional help text. Bridger reads the value from `Bridger/config/<mod id>.json` the first time you use it and writes it back whenever it changes. Writes are coalesced and flushed at most once a second, and again on shutdown.

```cpp
bridger::Setting<float> s_speed{"speed", "Speed", 8.0f, "Shift multiplies by four."};
bridger::Setting<bool>  s_wasd{"use_wasd", "Accept WASD", false};
bridger::Setting<int>   s_key{"key.toggle", "Toggle", VK_F3};

float current = s_speed;         // reads, loading on first use
s_speed.set(12.0f);              // writes and schedules a save
s_speed.modified();              // true when it differs from the default
s_speed.reset();                 // back to the default
```

Settings are in memory before `on_load` runs, so you can read them while setting your mod up. `bridger::config::get` and `set` cover values that belong to no row.

## Configuration rows

A row puts the name in a fixed left column and the control in the right one, so a panel reads as a form. Rows honour the search box at the top of the **Panels** tab, show their help text as a tooltip, and grow a revert arrow in the trailing gutter once the value differs from its default.

```cpp
void draw() {
    if (bridger::ui::begin_group("Movement")) {          // collapsible, remembers its state
        bridger::ui::row(s_speed, 0.25f, 100.0f, "m/s"); // slider, saved, revertible
        bridger::ui::row(s_wasd);                        // switch
        if (bridger::ui::key_row(s_key)) {               // click, then press a key
            bridger::rebind(on_toggle, s_key.get());     // takes effect at once
        }
        bridger::ui::combo_row(s_mode, kModes, 3);       // dropdown over a Setting<int>
        bridger::ui::text_row(s_name, "placeholder");
        bridger::ui::note("An explanatory line under the rows above.");
    }
    bridger::ui::end_group();

    if (bridger::ui::begin_group("Diagnostics", false)) { // starts collapsed
        bridger::ui::readoutf("frame", "{:.1f} ms", dt * 1000.0f);
        bridger::ui::readout("state", bridger::ui::good(), "running");
    }
    bridger::ui::end_group();
}
```

A few habits keep panels readable:

- **Use `readout` for anything the panel reports.** It is styled apart from the controls on purpose. Telemetry mixed into a column of controls is the most common reason a panel becomes hard to read.
- **Sliders take a typed value on double click.** Hold Shift while dragging for fine control.
- **`bridger::ui::setting(label, value, ...)`** gives the same row over a plain variable when you do not want it saved.

### Widget identity

Widget ids hash from the label. A list of rows that all draw a "reload" button would share one hover and press state, so wrap each row in an id scope:

```cpp
for (const auto& entry : entries) {
    bridger::ui::IdScope row(entry.id);      // any string unique within the panel
    bridger::ui::text(entry.name);
    bridger::ui::same_line();                // continue where the text ended
    if (bridger::ui::ghost_button("reload", 62.0f)) { reload(entry); }
}
```

`same_line()` with no argument continues on the line the previous widget ended. A positive offset is an absolute x from the left edge, which is how you start a column. `align_right(w)` reserves `w` pixels at the right for what follows.

## Hotkeys and keys

```cpp
bridger::hotkey(VK_F3, on_f3);                          // edge triggered, game thread
bridger::hotkey(VK_F3, on_f3, BRIDGER_MOD_CONTROL);     // needs a modifier
```

Hotkey callbacks run on the game thread and stay quiet while the overlay is open. For held keys use `bridger::key_down(VK_W)`. It reads false while the overlay is open, so your mod stops fighting the UI; `bridger::overlay_visible()` tells you why.

## Talking to other mods

Publish a table of function pointers under a versioned name and let other mods fetch it. The table must outlive `on_unload`. Consumers copy the header.

```cpp
// provider
static const FreecamApi g_service{1, is_enabled, set_enabled, ...};
bridger::provide("freecam.v1", &g_service);

// consumer, each frame or once after load
if (const auto* freecam = bridger::require<FreecamApi>("freecam.v1")) {
    freecam->set_enabled(true);
}
bridger::mod_loaded("freecam");   // true once its on_load has returned
```

Declare the provider in `dependencies` if you need it at load time. Otherwise check `require` lazily and degrade.

## Running every frame

There are two ticks, and the difference matters.

```cpp
bridger::tick(on_render_frame);     // render thread, from the overlay's Present hook
bridger::game_tick(on_game_frame);  // game thread, once per engine frame
```

| | `tick` | `game_tick` |
|---|---|---|
| Thread | render, inside Present | game, after every entity's `MsgEntityUpdate` |
| Safe to call the engine | no | yes |
| Delta passed | wall-clock seconds since the last tick | the engine's frame delta, scaled by the game's time scale, 0 while paused |
| Clamp | 100 ms | 100 ms |

> [!WARNING]
> Calling engine functions from `bridger::tick` crashes the game sooner or later. The engine is mid-update on another thread. Keep the render tick to input polling and bookkeeping.

Under the hood, the game tick hooks the world's per-frame update (RVA `0x21bbb70`): the main-thread function that advances game time, fans entity updates out to the job workers and waits for them. Entities update in parallel on worker threads, so a hook on an individual message handler may run on any of them.

If you need a more specific point in the frame, hook a message handler yourself. `tools/rtti/disasm.py handlers <Class>` lists them with addresses. `mods/freecam` is the worked example, and `docs/research/camera-pipeline.md` explains why it hooks where it does.

## Drawing with shaders

`bridger::fx` runs your HLSL on the frame: post-process passes, world-space geometry through the live camera, screen-space geometry in pixels. Shaders live as files in the mod folder and recompile when saved. The full guide is [shaders.md](shaders.md).

```cpp
bridger::fx::Shader g_grade;
bridger::fx::Pass g_pass;
bridger::fx::DrawList g_world;

bool bridger::on_load() {
    g_grade.load(BRIDGER_FX_SHADER_FULLSCREEN, "shaders/grade.hlsl");
    g_pass.create("grade", g_grade);
    g_world.create("markers", BRIDGER_FX_SPACE_WORLD, {.depth = BRIDGER_FX_DEPTH_TEST_WRITE});
    bridger::tick(on_frame);
    return true;
}

void on_frame(float) {
    g_world.sphere({x, y, z}, 0.5f, bridger::fx::rgba(255, 120, 40));
}
```

## Replacing engine behaviour

Everything above calls the game. `bridger::content` replaces it: messages, functions and virtual methods the mod answers in place of the engine, messages the mod sends, state attached to an engine object, and the data all of that runs on. Everything it makes is reverted on unload, reload and fault. The full guide is [content.md](content.md).

```cpp
namespace content = bridger::content;

content::Behaviour damage;                         // a message, per class
content::Function<bool (*)(void*)> noticing;       // an engine function
content::Gate allowed;                             // a gate made to say yes
content::State<Awareness> awareness;               // the mod's state, per engine object

// No offsets written down: the field comes from the engine's own RTTI.
const content::Field<float> radius{"DSCatcherTerritoryLocator", "EncounteringRadius"};
content::Object site;
content::Injection sites;

bool bridger::on_load() {
    if (!radius.valid()) {
        bridger::error("not the build this mod was written against");
        return false;
    }
    damage.replace("DSGazerDamageComponent", "MsgDamage", on_damage);
    allowed.force(site_is_allowed, true);
    return true;
}

void on_damage(void* self, void* message) {
    awareness.of(self).hits += 1;
    damage.call_original(self, message);
}

void plant() {
    site = content::Object::clone(nearest_locator);
    radius.set(site.get(), 250.0f);
    sites.add(catcher_manager, 0x308, 0x310, {site.get()});
}
```

## Reading engine memory

`include/decima/` holds a generated struct for all 8,847 engine classes, laid out from real offsets with a `static_assert` on every field. Cast a pointer and read it:

```cpp
#include "decima/decima.h"

auto* component = static_cast<decima::EntityComponent*>(pointer);
auto* resource = component->Resource.pointer;
```

Regenerate the headers with `python tools/codegen/gen_headers.py` when the dumps change.

## Inspecting live objects

The overlay's **Inspect** tab reads a live engine object and shows every reflected field by name with its current value. It stands in for a debugger: point it at the object and read, with no diagnostic to add, no rebuild and no restart.

Open one from a mod with a single call, or paste a hex address into the tab:

```cpp
#include "bridger/inspect.h"

if (ui::button("Inspect")) {
    bridger::inspect(resource);
}
```

Any pointer is safe. One without RTTI in vtable slot 0 is reported and left alone, and every read goes through the same checked path the dumper uses, so a bad address shows `??` and never crashes.

What the inspector shows:

- **The type comes from the object's own RTTI.** You never tell it what it is looking at. Base classes are walked and their fields folded in at the right offsets.
- **Values are formatted per type.** Integers, floats, `bool`, `WorldPosition`, `Vec3`, `GGUUID`, `String`, and `Array<T>` as a count and data pointer. Enum fields show the member name beside the number.
- **Pointer fields are clickable.** `Ref<T>`, `cptr<T>`, `WeakPtr<T>` and raw pointers that resolve to a real object show the target's type and open it on click, so you can walk the object graph from any starting point. **Back** returns along the path.
- **Values are sampled when a view loads.** **Refresh** reads them again.

This is also the fastest way to answer "what sits at `+0x30` on this thing", which is the question that costs the most time when reverse engineering a system.

## Hot reload

Rebuild a mod and the running game picks it up, usually within a second. No restart.

```
[00:58:16] photoreal changed on disk, reloading
[00:58:17] reloaded mod photoreal 0.2.0
```

Mods load from a staged copy under `Bridger/cache`, so the built DLL is never locked and a build can overwrite it at any time. The watcher polls each loaded mod's timestamp twice a second and waits until the file has been still for 400 ms, so it never stages a half-written DLL. Turn it off with **Auto reload on rebuild** in the **Mods** tab, or reload by hand with the per-mod **Reload** button.

Editing `manifest.json` works too. The manifest is re-read on reload, so a changed entry filename or version is picked up without a restart.

> [!TIP]
> Ship a stable entry filename. Versioned names like `mymod_0_4.dll` were only ever a workaround for the file lock, and the lock is gone.

### What a reload tears down

Everything the loader knows the mod registered: panels, ticks, game ticks, hotkeys, services, fx draw lists and shaders, every content entry it authored, and any queued `run_on_game_thread` callbacks whose code lives in the module. Hooks installed through `bridger::Hook` come out as well.

A reload runs at the top of an overlay frame with mod dispatch held exclusively, so no tick, panel or game tick is executing when the module is freed. You can request one from anywhere, including a panel button.

### What a reload keeps

Settings. They are flushed before the module is freed and reloaded before the entry point runs.

Nothing else. Engine pointers, handles and observed state are gone, exactly as after a restart. If a capture is expensive to redo, key it on something stable like a `GGUUID` and save it in settings, and never hold the raw pointer across a reload.

## The manifest

```json
{
  "id": "my_mod",
  "name": "My Mod",
  "version": "1.0.0",
  "author": "you",
  "description": "What it does.",
  "entry": "my_mod.dll",
  "dependencies": ["some_other_mod"]
}
```

`id` must match the folder name. Dependencies load first. A missing or failed dependency fails the mod, with the reason shown in the overlay.

## Quick reference

| You want to | Call |
|---|---|
| log a line | `bridger::info("...")`, `warn`, `error`, `trace` |
| find a type | `bridger::type("Entity")` |
| call an exported function | `bridger::symbol<Fn>("Group", "name")` |
| find a message handler | `bridger::handler("Class", "MsgX")` |
| read an engine global | `bridger::singleton("Group", "export")` |
| watch a function | `bridger::Hook<Fn>::install` or `install_handler` |
| change what a function decides | `content::Function`, `content::Gate` |
| draw a panel | `bridger::ui::panel("Name", draw)` |
| keep a value between runs | `bridger::Setting<T>` |
| bind a key | `bridger::hotkey(VK_F3, fn)` |
| run once on the game thread | `bridger::run_on_game_thread(fn)` |
| run every engine frame | `bridger::game_tick(fn)` |
| share an API with other mods | `bridger::provide` and `bridger::require<T>` |
| open an object in the inspector | `bridger::inspect(ptr)` |
| draw on the frame | `bridger::fx::Pass`, `bridger::fx::DrawList` |
