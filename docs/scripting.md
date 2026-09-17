# Bridger Script

*A mod is a `.lua` file. Save it and the running game has it.*

```lua
-- Bridger/scripts/stronger.lua
hook("EntitySymbols::Entity_ExportedHeal", function(original, entity, amount)
    return original(entity, amount * 2)
end)
```

No compiler, no CMake, no restart, no addresses. Scripts read and write engine objects by their reflected field names, call the engine's 1,943 exported functions by name, and hook any of them or any class's message handler. Everything a DLL mod can do is here too: panels, settings, keys, the content layer. Whatever a script changes is undone when it unloads, reloads or fails.

The console runs the same Lua inside the game while it plays, from the overlay or from a terminal. It is the fastest way to find out what anything is:

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

**Contents**

1. [Where scripts live](#where-scripts-live)
2. [The console](#the-console)
3. [Engine objects](#engine-objects)
4. [Calling the engine](#calling-the-engine)
5. [Changing behaviour](#changing-behaviour)
6. [Time](#time)
7. [Input](#input)
8. [Settings and panels](#settings-and-panels)
9. [State across reloads](#state-across-reloads)
10. [Authoring engine content](#authoring-engine-content)
11. [Memory](#memory)
12. [Threads, budgets and safety](#threads-budgets-and-safety)
13. [Editor support](#editor-support)
14. [How it works](#how-it-works)
15. [Quick reference](#quick-reference)

---

## Where scripts live

| Path | What it is |
|---|---|
| `Bridger/scripts/name.lua` | one file, one mod, id `name`. Nothing else to write. |
| `Bridger/mods/name/main.lua` | a folder mod. Its other `.lua` files are `require`-able and watched. |
| `Bridger/mods/name/manifest.json` | optional for a folder: name, version, author, dependencies, or an `entry` other than `main.lua` |

A new file is picked up within two seconds. Saving any file a script uses reloads it: its entry, anything it `require`s, anything in its folder. A save that fails to parse keeps the running version and reports the syntax error. A script that raises an error while loading is marked failed with the reason, and the next save tries again.

Files starting with `_` in `Bridger/scripts` are shared code, never mods, so helpers can live there as `require("_helpers")`.

Two scaffolds:

```bash
python tools/new_mod.py my_mod --lua
```

```bash
python tools/bridger.py new my_script
```

The first writes a folder mod straight into the game, the second a single file. Script mods sit in the **Mods** tab beside the DLL mods, can be disabled the same way, and can depend on other mods.

## The console

**In game:** press F1, then open the **Script** tab. The list on the left shows every running script. Pick one and the console runs inside that script's own state, with its locals' upvalues and globals. Enter runs, Tab completes, Up and Down walk the history.

**In a terminal:**

```bash
python tools/bridger.py
```

The same console with colour, multi-line input (an unfinished `function ... end` keeps reading) and, with `pyreadline3` installed, Tab completion. `:target my_mod` switches state, `:scripts` lists them, `:reload my_mod` reloads one.

For one-offs:

```bash
python tools/bridger.py eval "game.player_entity().Orientation.Position"
```

```bash
python tools/bridger.py run experiment.lua
```

```bash
python tools/bridger.py scripts
```

```bash
python tools/bridger.py console -f
```

An expression prints its value; a statement runs for effect. `_` holds the last value. `print` output from every script lands here too, and so do their errors with tracebacks.

The console runs on the game thread when the game is ticking, so engine calls typed there are as safe as they are from `every_frame`. In menus and on loading screens the world does not update, so the console runs on the calling thread instead. Reading is fine there; calling the engine is at your own risk.

> [!NOTE]
> The terminal talks to `\\.\pipe\bridger`: one JSON request per line, one response per line, local to the machine and to the user running the game. Requests are `ping`, `eval {target, code}`, `complete {target, prefix}`, `scripts`, `reload {id}`, `log {since}` and `console {since}`. Anything that can open a pipe can drive the game: an editor extension, a test harness, an assistant.

## Engine objects

A pointer to an engine object becomes a **view**. Fields read and write by name, resolved from the object's own RTTI with its bases flattened, so a `DSPlayerEntity` has every field `Entity` declares.

```lua
local body = game.player_entity()
body:type()                      --> "DSPlayerEntity"
body.Orientation.Position.Z      --> 118.9
body.Orientation.Position.Z = 200
body:fields()                    --> { {name, type, offset, category, property}, ... }
body:is_a("Entity")              --> true
body:messages()                  --> every message its class handles
body:inspect()                   --> opens it in the overlay's Inspect tab
```

Properties are the 172 fields the engine computes through accessors instead of storing, such as `MsgDamage.CoreAmount` and `Entity.Flags`. They read and write the same way; the engine's getter or setter runs underneath.

A misspelt field is an error that says what you meant: `DSPlayerEntity has no field 'Orientaton' (did you mean 'Orientation'?)`.

### How each field type comes through

| Field | Reads as | Writes |
|---|---|---|
| `bool`, integers, `float`, `double` | boolean, integer, number | the same |
| enums | integer | an integer or a value's name: `obj.State = "Idle"` |
| `String`, `WString` | Lua string | read only: the engine owns the memory |
| `GGUUID`, `UUIDRef<T>` | `"0a1b2c3d-..."` | a string in that form, or a GGUUID view |
| `Ref<T>`, `cptr<T>`, `WeakPtr<T>` | the object's view, or nil | `cptr` takes a view or address; `Ref` refuses (see `mem.write`) |
| a struct (`Vec3`, `WorldTransform`, ...) | a view into the same memory | a view to copy from, or a table of fields |
| `Array<T>` | an array view: `#a`, `a[i]` from 1, `ipairs` | elements: `a[2] = v` |
| `StreamingRef<T>` | integer handle | integer |
| anything with no known shape | its address, for `mem.read` | nothing |
| a property (accessor backed) | whatever its getter returns | through its setter; read only without one |

A struct takes a table by name or in order: `pos = {X = 1, Y = 2, Z = 3}` or `pos = {1, 2, 3}`. `vec3(x, y, z)` makes a vector with the engine's field names and arithmetic, so it assigns straight into `Vec3` and `WorldPosition` fields:

```lua
body.Orientation.Position = vec3(body.Orientation.Position) + vec3(0, 0, 2)
```

### Views as keys

Views compare equal when they are the same object, and the same object is the same Lua value, so they work as table keys. That is how a script keeps state per object:

```lua
local seen = setmetatable({}, { __mode = "k" })
on("DSMuleComponent", "MsgEntityUpdate", function(self) seen[self] = (seen[self] or 0) + 1 end)
```

### View methods

View methods are lower case and engine fields are capitalised, so they never collide.

| Method | Does |
|---|---|
| `obj:get("name")`, `obj:set("name", v)` | reach a field whatever it is called |
| `obj:cast("Type")` | read the same memory as another class |
| `obj:copy()` | make a struct value the script owns |
| `obj:valid()` | whether the object is still what it was |

`game.object(address, "Type")`, `game.struct(address, "Type")` and `game.new("Type", {fields})` make views from addresses and from nothing. `game.type("Entity")` describes a type; `game.types("mule component")` searches type names.

## Calling the engine

Every exported function sits under `engine`, by group and name, with its parameter types from the engine's own symbol table:

```lua
local Entity = engine.EntitySymbols
Entity.Entity_ExportedGetHealth(body)          --> 100.0
Entity.Entity_ExportedGetPosition(body)        --> WorldPosition(...), a value the script owns
Entity.Entity_ExportedSetInvulnerable(body, true)
engine["EntitySymbols::Entity_ExportedKill"]   --> the same function, by its full name
engine.find("entity heal")                     --> names matching every word
engine.EntitySymbols.list()                    --> every function in the group
```

Arguments convert from their declared types: numbers and booleans as you would expect, enums from integers or names, pointers from views, addresses or nil, `const &` structs from views or tables, `char const *` from strings. A struct result comes back as a view the script owns; a `String` result as text.

### Out parameters

Pointer parameters the function writes through are out parameters. Pass `out` and the value comes back after the result:

```lua
local velocity = Entity.Entity_ExportedGetVelocity(body, out)        -- void(Entity const*, Vec3*)
local ok, count = engine.X.Y(thing, out)                              -- result first, then outs
```

### Functions with no signature

A function the symbol table has no signature for, or one that is never exported, takes a signature written by hand:

```lua
local get_player = fn(game.rva(0x22075b0), "Player*(int)")
local update = fn(address, "void(Entity* self, float dt)")          -- names are decoration
```

Types in a signature are the primitives (`bool int8 uint8 int16 uint16 int uint32 int64 uint64 float double ptr`) and any engine type by name, with `*`, `&` and `const` as in C++.

A call that faults, from wrong arguments or a stale object, is a Lua error naming the function. The game keeps running.

> [!IMPORTANT]
> Engine functions belong on the game thread. `every_frame`, timers, tasks, message hooks and the console all run there. A panel does not; hop with `ready(fn)`.

### Addresses

`game.singleton("Group::Name")` reads the global an export loads, the way `bridger::singleton` does in C++. `game.symbol`, `game.rtti("Type")` (for functions taking `RTTI const *`), `game.handler(class, message)`, `game.vtable(class)` and `game.scan("48 8B ?? 05")` give addresses.

## Changing behaviour

### Functions

```lua
-- instead: original runs the rest, with these arguments or the call's own
hook("EntitySymbols::Entity_ExportedHeal", function(original, entity, amount)
    return original(entity, amount * 2)
end)

-- first: return nothing to let the call through, a value to answer instead
local config = settings { stealth = false }
before("AIManagerGameSymbols::AIManagerGame_sExportedGetPlayerIsBeingSeen", function(player)
    if config.stealth then return false end
end)

-- after: the result comes first; return a value to replace it
after("EntitySymbols::Entity_ExportedGetLinearSpeed", function(speed, entity)
    return speed * 1.5
end)

-- a constant, with no Lua on the call path for integer, bool and pointer results
force("AIManagerGameSymbols::AIManagerGame_sExportedGetPlayerHasBeenReported", false)
```

`hook` returning nothing means "the original's answer", and the original is called if the script did not call it. Returning `skip` answers zero without running it, which is how you suppress a `void` function.

A target is a `"Group::Name"` string (the editor completes them), an `engine.Group.Name` function, which also has `:hook`, `:before`, `:after` and `:force`, or an address with a signature.

Several scripts can hook the same function. Subscribers run in the order they subscribed, and `original` in one is the next one, then the engine. A DLL mod that already hooks the function with MinHook makes the script's hook fail with that reason.

### Messages

Most engine logic runs as message handlers on classes. Find them by class and message name:

```lua
on("DSMuleComponent", "MsgEntityUpdate", function(self, message) end)      -- after the engine's
before("DSGazerDamageComponent", "MsgDamage", function(self, message)       -- first; skip vetoes
    if message.CoreAmount > 100 then return skip end
end)
hook("CameraEntity", "MsgEntityUpdate", function(original, self, message)  -- instead
    original(self, message)
end)
```

The message is a view of the message struct, so its fields read and write by name before the engine sees them.

One object instead of every object of its class:

```lua
mule:on("MsgDamage", function(self, message) print("that one got hit") end)
mule:send("MsgDamage", { ImpactSeverity = 1 })   -- build a message, hand it to the object's handler
```

A handler a class inherits without redeclaring it is shared with every other class that inherits it, so hooking it through one class affects them all, exactly as in C++.

### Virtual methods and callbacks

```lua
-- one object's vtable slot, with the displaced method as original
obj:override(14, "bool(ptr)", function(original, self) return true end)

-- a script function as a native function pointer, for anything that takes one
local compare = callback("int(ptr, ptr)", function(a, b) return 0 end)
compare.address
```

### When a hook goes wrong

An error in a hook is reported once. Repeats are counted, so a hook that fails every frame logs one line and a count. The call then continues as if the hook were absent: the rest of the chain, then the engine. The same happens when the script has stopped, and when it is too busy on another thread to answer within two seconds. The engine always gets an answer.

## Time

```lua
every_frame(function(dt) end)       -- game thread, every engine frame; dt is 0 while paused
later(2.5, function() end)          -- once, in game seconds
every(10, function() end)
ready(function() end)               -- next game tick: the way to reach the game thread
on_unload(function() end)           -- before the script's hooks and panels are taken away

task(function()                     -- reads like a sequence of events
    wait(2)
    wait_frames(1)
    wait_until(function() return game.player_entity() ~= nil end, 30)
end)
```

Each returns a handle with `:remove()`. A callback that errors stops and says so. Save the fix and the script reloads with it running again.

## Input

```lua
bind("F5", function() end)                 -- game thread, on press; quiet while the overlay is open
bind("Ctrl+K", fn)
input.down("W")  input.pressed("Mouse4")
```

Keys are names (`F1` to `F24`, `A` to `Z`, `0` to `9`, `Numpad0`, `Space`, `Shift`, `Mouse4`, ...) with `+` modifiers, or virtual key codes.

## Settings and panels

```lua
local config = settings {
    enabled  = true,
    distance = { 3.0, min = 1, max = 12, suffix = "x", help = "How far." },
    mode     = { 1, choices = { "Off", "Balanced", "Aggressive" } },
}
local toggle = setting("toggle", "F6", { key = true, label = "Toggle" })

if config.enabled then end
config.distance = 4
bind(toggle, function() config.enabled = not config.enabled end)   -- follows rebinding
```

Values persist in `Bridger/config/<id>.json`, like a DLL mod's. A script that declares settings and no panel gets one for free: every setting as a row, with revert markers, in the **Panels** tab. A script with more to show declares its own:

```lua
panel("Vitals", function()
    ui.settings()                                  -- the declared settings, as rows
    ui.header("state")
    ui.readout("health", vitals.health, "good")
    if ui.button("Heal") then ready(heal) end      -- panels draw on the render thread
end)
```

Panels are declared while the script loads. `ui` has text, headers, buttons, checkboxes, sliders, inputs, readouts, notes, badges, groups, progress bars and every settings row type (`ui.setting(label, value, options)`). See `include/bridger/lua/bridger.d.lua` for the full list.

## State across reloads

A reload starts the script from scratch. Put what should survive in `keep`:

```lua
local state = keep("state", { visits = 0, marked = {} })
state.visits = state.visits + 1
```

Kept tables are copied into the new state: numbers, strings, booleans, nested tables, and engine objects (by address). Functions are code, so they are never kept.

## Authoring engine content

The content layer is reachable from scripts, and everything it makes belongs to the script:

```lua
local site = game.clone(nearest_locator, { EncounteringRadius = 250 })
local entry = game.inject(catcher_manager, 0x308, 0x310, { site }, true)
local patch = game.patch(address, "\x90\x90")
entry:revert()  patch:intact()
game.create("DSCatcherTerritoryLocator")
```

See [content.md](content.md) for what each of these does to the engine and why.

## Memory

For what reflection does not describe:

```lua
mem.read(address, "float")    mem.read(address, "ptr")    mem.read(address, "WorldPosition")
mem.write(address, "int32", 5)
mem.bytes(address, 16)        mem.string(address)          mem.readable(address, 64)
mem.alloc(64)                 -- zeroed bytes owned by the script
```

Every read and write is guarded. A bad address is an error, never a crash.

## Threads, budgets and safety

Each script has its own Lua state and a lock around it, so a script's callbacks never run at the same time as each other, whichever thread they arrive on.

| Runs on | |
|---|---|
| the game thread | `every_frame`, timers, tasks, `ready`, `bind`, the console |
| any thread | hooks and message handlers: entities update on worker threads |
| the render thread | panels, changes made through settings rows |
| the loading thread | the script's top level, during load and reload |

Engine calls belong on the game thread or inside hooks the engine calls. The top level of a script runs while it loads, which is a different thread. Register there, and do engine work in `ready`, `every_frame` or hooks.

A script that runs for more than a second without returning, such as an endless loop, has an error raised inside it. The console allows fifteen seconds. `script.budget(ms)` changes the limit for the state it runs in.

`os.exit` is removed. C modules cannot be loaded, because a DLL would outlive the script. Everything else in the standard library is there, `io` included.

When a script unloads, reloads or fails to load: its `on_unload` callbacks run, its hooks come out (waiting for calls in flight), its panels, settings rows, content entries and fx resources go, and its state is closed.

## Editor support

`include/bridger/lua/` describes the whole API and the whole engine for the Lua language server (the Lua extension for VS Code): every class with typed fields, every enum, every exported function with typed parameters, and the strings `hook`, `on` and friends take complete to real names.

The deploy target copies it into `Bridger/scripts/.types` with a `.luarc.json`, so opening the game's `scripts` folder in VS Code just works. `tools/new_mod.py --lua` writes a `.luarc.json` pointing at the repository copy.

Regenerate the engine half after a new dump:

```bash
python tools/script/export_signatures.py
```

```bash
python tools/script/gen_luals.py
```

## How it works

Everything lives in `src/script/`. Lua 5.4 is compiled as C++, so a script error unwinds through native frames with their destructors.

Field access goes through `reflect.cpp`, which resolves RTTI type names to value codecs and classes to flattened, cached layouts. Calls and hooks go through `ffi.cpp` and `ffi_x64.asm`: one generic x64 call that loads both register sets, and a pool of 256 assembly entry thunks that capture the caller's register and stack arguments into a frame a handler can rewrite and pass on. `tests/script_ffi_test.cpp` proves that against plain C++, and `tests/script_runtime_test.cpp` drives the runtime end to end without the game.

## Quick reference

| You want to | Write |
|---|---|
| read a field | `obj.Field`, `obj.Nested.Field` |
| write a field | `obj.Field = value` |
| call an exported function | `engine.Group.Name(args)` |
| search for a function | `engine.find("words")` |
| replace a function | `hook("Group::Name", function(original, ...) end)` |
| answer before the engine | `before(...)`, return a value or `skip` |
| adjust a result | `after(...)`, return a value |
| pin a result | `force("Group::Name", value)` |
| watch a message | `on("Class", "MsgX", function(self, message) end)` |
| send a message | `obj:send("MsgX", { Field = v })` |
| run every frame | `every_frame(function(dt) end)` |
| reach the game thread | `ready(fn)` |
| bind a key | `bind("F5", fn)` |
| keep a value between runs | `settings { name = default }` |
| keep state across reloads | `keep("name", { ... })` |
| build an engine object | `game.clone(obj, { Field = v })`, `game.create("Type")` |
