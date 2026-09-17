# The content layer

*Everything else in Bridger lets a mod call the game. This lets it replace the game.*

The difference is concrete. Before this layer a mod could find a MULE, alert it, damage it, move it and read every field on it, and every one of those was the engine's own verb, called by the mod. When the engine had no verb for what you wanted, you were stuck. The dead ends in the MULE and BT work all had the same shape: no Catcher site here, no "follow" order, no `CPLocator` for `sRequestMuleReinforce`. The logic belonged to the engine, and the data it ran on was missing.

This layer is about behaviour and logic: what the game decides, and what it decides it with.

**Contents**

1. [The primitives](#the-primitives)
2. [Everything undoes itself](#everything-undoes-itself)
3. [Replacing a message](#replacing-a-message)
4. [Replacing a function](#replacing-a-function)
5. [Sending a message](#sending-a-message)
6. [Replacing a virtual method](#replacing-a-virtual-method)
7. [State about an engine object](#state-about-an-engine-object)
8. [Reflection](#reflection)
9. [Authoring an object](#authoring-an-object)
10. [Registering it](#registering-it)
11. [Revertible writes](#revertible-writes)
12. [Faults](#faults)
13. [Threading](#threading)
14. [What is written down and what is not](#what-is-written-down-and-what-is-not)
15. [Limits](#limits)

---

## The primitives

**Replacing what the engine does**

| | |
|---|---|
| `Behaviour` | a message the mod answers instead of, before, or after the engine's handler |
| `Function` | an engine function the mod answers, with the original still reachable |
| `Gate` | a function made to return a constant, with no detour to write |
| `Method` | a virtual method replaced for a class, or for one object alone |

**Driving what the engine does**

| | |
|---|---|
| `deliver` | a message handed to an object's own handler, the other half of `Behaviour` |
| `State<T>` | the mod's own state, attached to an engine object and keyed on its identity |

**Authoring what it runs on**

| | |
|---|---|
| `Field<T>` | a field by name from RTTI, so no mod writes an offset down |
| `Object` | an engine object built at runtime, constructed or cloned |
| `Injection` | mod-owned elements registered in the array a system selects from |
| `Patch` | a write to engine memory that puts the originals back |

From C++ it is `bridger::content` in [include/bridger/content.hpp](../include/bridger/content.hpp), which `mod.hpp` pulls in. From any language it is the `BridgerContent` table in [include/bridger/api.h](../include/bridger/api.h). Scripts reach it through `game.clone`, `game.inject`, `game.patch` and `game.create`; see [scripting.md](scripting.md#authoring-engine-content).

> [!NOTE]
> This layer stops at streamed assets. Meshes, animations, audio and textures live in the archives as bytes, with no reflected object to replace. That is a separate tool with its own lifetime, written up in `docs/research/content-layer-scope.md`.

## Everything undoes itself

Every entry a mod makes belongs to that mod and is reverted when it unloads, reloads or faults. That is what separates a framework from a pile of pokes. Writing engine memory by hand means remembering the undo on the unload path, the hot-reload path, the fault path and the world-change path. Three of those are easy to forget, and one of them only bites someone else.

Restores are checked. If the engine has written over the same slot since, because a scene load rebuilt an array or another mod took the same handler, the stale value is **left alone** and the skip is logged. Nothing clobbers whatever replaced it.

## Replacing a message

Every Decima class carries a message handler table on its RTTI descriptor: pairs of `{message type, handler function}`. `Behaviour` finds the handler a class runs for a message there, walking the class and then its bases, and replaces that function.

```cpp
content::Behaviour damage;

void on_damage(void* self, void* message) {
    rewrite_damage_type(message);
    damage.call_original(self, message);
}

damage.replace("DSGazerDamageComponent", "MsgDamage", on_damage);
```

Three orderings:

| | The engine's handler |
|---|---|
| `replace()` | never runs, unless the override calls `call_original()` |
| `before()` | runs after the override |
| `after()` | runs before the override |

### Why it replaces the function and never the table

Writing the `{message, handler}` pair on the descriptor would be the tidier edit: exactly per class, no trampoline, no reach into anything that shares a function. That was this layer's original design, and **it does nothing**.

The engine builds a sorted, id-keyed dispatch structure when a type is registered at startup (the binary search at `0x21ae8b0`, over 12-byte entries keyed by a `uint16` message id) and dispatches from that. The descriptor's table is only the source it is built from, read once and never consulted again. An override written afterwards is installed, intact, and completely inert, with nothing in the log to say so.

Measured in game, riding a trike with both mechanisms installed at once:

```
Update calls           3797     the trampoline
Table-override calls      0     the descriptor table
```

Two consequences follow from replacing the function:

- **A handler several classes share is replaced for all of them.** An inherited handler that no subclass overrides is one function. Trampolining it catches every class that inherits it.
- **A class cannot be given a message it never handled.** There is no function to replace, so `replace()` returns false. Adding one would mean building the engine's registration-time structure, which this layer leaves alone.

Overrides come from a fixed pool of 256 slots. The pointer the hook jumps to is an ordinary function in `bridger.dll`, never allocated executable memory, and it is cleared last on revert, so a thread already inside it falls through to the trampoline and never into a module being unloaded.

## Replacing a function

Most engine logic is a plain function: `IsGazerNoticingPlayer`, `IsSiteInRange`, `BringCatcher`. `Function` replaces one.

```cpp
content::Function<bool (*)(void*, const void*)> in_range;

bool on_range_check(void* site, const void* where) {
    return anywhere || in_range.call(site, where);
}

in_range.replace(bridger::symbol<void*>("DSCatcherSymbols", "..."), on_range_check);
```

It is the same machinery as `bridger::Hook`, owned by the mod: reverted on unload, on reload and on the mod's fault. Prefer it over `Hook` for anything that changes what the game decides. `Hook` stays right for pure observation.

### Gates

A large share of engine logic is yes or no, and a large share of what a mod wants from it is "say yes". A detour for that is ceremony:

```cpp
content::Gate allowed;
allowed.force(site_is_allowed, true);
```

No detour, no signature to match, no original to hold. The stub ignores every argument and returns the value in `rax`. On x64 the caller cleans its own stack and the first four arguments never reach memory, so arity does not matter. It covers integers, bools and pointers. A float or a large struct comes back some other way, so use `Function` there. 64 slots.

## Sending a message

Replacing behaviour is half of it. A mod that can answer a message and never send one can only react.

```cpp
auto damage = content::Object::create("MsgDamage");
amount.set(damage.get(), 40.0f);
content::deliver(gazer, damage.get());
```

`deliver` takes the message's own RTTI, finds the handler the receiver's class runs for it, walking the class and then its bases exactly as the engine's dispatch does, and calls it.

> [!IMPORTANT]
> This is **direct delivery to one object's handler**. There is no queue, no listener registry and no scene involved, and it runs on the calling thread, so send from the game thread. Driving the engine's own broadcast path is a research job of its own: the dispatcher at `0x21ae8b0` is a binary search over a sorted listener table keyed by a message id, and nothing here pretends to drive it yet.

`content::handler_for(object, "MsgX")` answers the prior question, whether this object would do anything at all with a message, without sending one.

## Replacing a virtual method

```cpp
content::Method<bool (*)(void*)> visible;

bool always_visible(void* self) { return forced || visible.original()(self); }

visible.replace("DSMuleComponent", 14, always_visible);          // every instance of the class
visible.replace_shared(content::vtable_of(some_mule), 14, ...);  // the same reach, via an instance
visible.replace(one_entity, 14, always_visible);                 // this object alone
```

### The class form

The class form patches the vtable the constructor writes. The RTTI descriptor records no vtable, so it is read out of the constructor's own `lea rax, [rip + vtable]` / `mov [this], rax`, the same trade `bridger::singleton()` makes for engine globals: derive the address from named code instead of writing it down.

That is the one byte pattern in the layer, and it **confirms** its answer. Slot 0 of a Decima vtable is `GetRTTI`, emitted as `lea rax, [rip + descriptor]; ret`, so reading it back says which class the candidate belongs to, and a candidate is only accepted when that is this very class. `tools/rtti/vtables.py` replays the scan offline against `ds.exe`:

```
classes with a constructor        8255
  confirmed by GetRTTI            7437
  slot 0 is a code pointer only    329
  unresolved                       489

polymorphic classes               6859
  unresolved                        14
```

The 489 unresolved are almost all classes with no vtable to find: `AssetPath`, `ActivityFeedEntry` and the rest are plain data structs. Of the classes that are polymorphic at all, 14 resolve to nothing. For those, `replace_shared(content::vtable_of(instance), ...)` has the same reach, and the vtable of a live object is never in doubt.

### The instance form

The instance form gives that object a **private copy of its vtable** and points the object at the copy. The slot at index -1 comes along, so MSVC's complete-object locator travels with it. Nothing else of its class changes.

This is how one entity behaves differently from every other of its type. It is the closest thing the layer has to a new class, and in practice the substitute for one: clone a type, override its handlers and methods, and you have an object the engine treats as its own and the mod drives entirely.

## State about an engine object

Logic needs somewhere to remember. An engine pointer is a poor key: the engine frees and recycles continuously, so an address that still maps may be a different object.

```cpp
struct Awareness { float suspicion = 0.0f; double last_seen = 0.0; };
content::State<Awareness> awareness;

void on_mule_update(void* self, void*) {
    awareness.of(self).suspicion += 0.01f;
}

void tick(float) { awareness.reap(); }
```

Entries are keyed on the `ObjectUUID` every `RTTIRefObject` carries at offset 8 and checked on every lookup, so a recycled address reads as a fresh entry and never as another object's state. `reap()` drops everything whose object no longer reads back the UUID it was filed under, which is what a free looks like from here.

This is the mod's own memory. Nothing is written to the engine, so nothing needs reverting. For state that must outlive the process, `bridger::Handle` remembers a UUID in the mod's settings.

## Reflection

```cpp
// Resolved once, on first use, from the engine's own RTTI. Flattened across every base, so the
// offset is the real one in memory, whichever class declares the field.
const content::Field<float> radius{"DSCatcherTerritoryLocator", "EncounteringRadius"};
const content::Field<Position> where{"DSCatcherTerritoryLocator", "Position"};

radius.set(site, 250.0f);
const auto p = where.get(site);
```

This replaces the `field<Position>(site, 0x20)` idiom. A field the build lacks leaves the `Field` invalid and every access a no-op, so a wrong build reads as "nothing happened" instead of a wild write. Check `valid()` in `on_load` and fail the load there, where it can be reported:

```cpp
bool bridger::on_load() {
    if (!radius.valid() || !where.valid()) {
        bridger::error("this is not the build the mod was written against");
        return false;
    }
    ...
}
```

`content::fields("SomeType")` lists every field of a type with its offset and size, and for the accessor-backed properties, which occupy no storage, the getter and setter to call instead. Resolution is cached per type, so a `Field` in a hot loop costs a pointer add.

## Authoring an object

The RTTI records a constructor and a destructor for 8,255 of 9,867 types, plus exact size, alignment and every field offset. So an engine object can simply be built:

```cpp
content::Object config = content::Object::create("DSMuleConfig");
```

The memory is the mod's, aligned to the type's alignment, with **the engine's own constructor** run over it, so the vtable and every default come from the engine. The 592 abstract classes have no constructor and fail loudly.

Cloning is usually the better move when a live instance exists:

```cpp
content::Object site = content::Object::clone(nearest_locator);
```

Every field the consumer reads is already valid, so a failure can only come from what the mod changed. That is what made the original spike conclusive.

> [!WARNING]
> Two things to hold on to.
>
> **The memory is the mod's, never the engine allocator's.** Never hand one to something that will free it. Systems that read from an array are the use case.
>
> **A clone carries the original's pointers.** The unit resource at `+0x70`, the nav mesh area at `+0x1e0`. A clone lives only as long as the region those point into. Leaving the region can leave them dangling.

## Registering it

A constructed object is inert until the system that consumes it knows about it. Every Decima system selects from an array, a count and a pointer to the elements, and that array is the registration point:

```cpp
content::Injection sites;
sites.add(catcher_manager, 0x308, 0x310, {site.get()});
```

`inject` copies the engine's elements into a mod-owned array, appends the mod's, and points the system at it. **The engine's array is never written to and never freed.** It goes back on revert.

The two writes are ordered so a reader can never see a half-applied injection. The pointer is published first; the new array's leading elements are copies of the engine's, so a racing reader still sees the engine's own data. The count follows. Withdrawing is the reverse.

These arrays are **rebuilt on scene load**, which silently drops the injection. A persistent injection, the default, re-applies itself on the game tick against whatever the engine has just built. Otherwise `intact()` reports the drop and `reapply()` is the fix.

That was the spike, and it ran: a `DSCatcherTerritoryLocator` cloned at runtime, moved to Sam and swapped into the manager's array was selected by `BringCatcher`, and the Catcher staged around it instead of around the authored site. The geometry rules out coincidence. The real site was 165 m away, and the spawn search only looks within 150 m of the selected site. Objects a mod builds are consumed as authored data. The general rule is "find the array each system selects from", and `Injection` is that rule made into a type.

## Revertible writes

```cpp
content::Patch truce;
truce.write(&faction_matrix[index], std::uint8_t{0});
```

The originals are kept and go back on revert, on unload and on fault. This is what a mod reaches for in place of writing engine memory by hand and maintaining four undo paths.

## Faults

`bridger::safely()` latches the mod on an engine access fault, runs `on_fault()`, and then **reverts everything that mod authored**. A mod that has stopped no longer answers the engine's messages, holds its descriptors or stands in for its functions. Register `on_fault()` for anything outside the content layer that must never be left half applied.

## Threading

| | |
|---|---|
| **Writing the engine** | `create`, `inject`, a `Behaviour` or `Function` install, a `Patch`: do it on the game thread. From a panel button, hop first with `bridger::on_game_thread([] { ... })`. |
| **Handlers and replacements** | run on whatever thread the engine calls them on. For entity updates that is the game thread; for some AI work it is a job worker. Keep them to what that call site can afford. |
| **`deliver`** | runs the handler on the calling thread. Send from the game thread. |
| **Reflection** | `Field`, `fields` and `offset_of` are read-only and safe from anywhere. |

## What is written down and what is not

Nothing in this layer needs an RVA. Types, fields, constructors, destructors, message handler tables and vtables all come from the engine's own RTTI or from named code.

Two things a mod still supplies by hand: the **address of the system** holding an array, which `bridger::singleton()` gets from a named export in most cases, and the **offsets of the count and data slots** inside it, because a Decima `Array` is itself unreflected.

## Limits

- 256 live message handler overrides across all mods; 64 forced results.
- 4,096 elements per injection. An engine array over 65,536 is rejected as implausible.
- A patch is capped at 1 MB, an authored object at 16 MB.
- `Gate` covers returns that come back in `rax`. Floats and large structs need `Function`.
- `deliver` reaches one object's handler, never the engine's broadcast.
- Nothing persists. Objects are rebuilt on each load, which for a mod is normal and pairs well with hot reload.
