---@meta
-- Bridger Script, described for the Lua language server. Point the Lua extension at this folder
-- ("workspace.library": ["<bridger>/include/bridger/lua"]) and every script gets completion and
-- type checking for the API below and, from engine.d.lua, for the whole engine.
--
-- The guide is docs/scripting.md.

-- ---- values ------------------------------------------------------------------------------------

---An engine object, a struct inside one, or a struct value the script owns. Fields read and write by
---their reflected names; the methods below are lower case so they never hide a field.
---@class bridger.View
local View = {}
---The class name, from the object's own RTTI.
---@return string
function View:type() end
---@return integer
function View:address() end
---@param class string
---@return boolean
function View:is_a(class) end
---False once the object has been freed or reused as something else.
---@return boolean
function View:valid() end
---@return { name: string, type: string, offset: integer, category: string, property: boolean }[]
function View:fields() end
---Reads a field by name, for names that collide with a method.
---@param field string
---@return any
function View:get(field) end
---@param field string
---@param value any
---@return self
function View:set(field, value) end
---@param field string
---@return integer
function View:field_address(field) end
---A plain table of every field's current value.
---@return table<string, any>
function View:table() end
---The same memory, read as another class.
---@generic T
---@param class `T`
---@return T
function View:cast(class) end
---A copy owned by the script.
---@return self
function View:copy() end
---Opens the object in the overlay's Inspect tab.
---@return self
function View:inspect() end
---@param offset integer
---@param type string
---@return any
function View:read(offset, type) end
---@param offset integer
---@param type string
---@param value any
function View:write(offset, type, value) end
---Address of the handler this object's class runs for a message, or nil.
---@param message bridger.Message
---@return integer?
function View:handler(message) end
---Every message this object's class and its bases handle.
---@return string[]
function View:messages() end
---Builds the message with the engine's constructor, fills it from `fields`, delivers it to this
---object's own handler and destroys it. True when a handler ran.
---@param message bridger.Message
---@param fields? table<string, any>
---@return boolean
function View:send(message, fields) end
---A byte copy owned by the script, reverted when it unloads.
---@return self
function View:clone() end
---@return integer
function View:vtable() end
---After the engine's handler, for this object only.
---@param message bridger.Message
---@param fn fun(self: bridger.View, message: bridger.View)
---@return bridger.Hook
function View:on(message, fn) end
---Before the engine's handler, for this object only. Return skip to stop the engine's handler.
---@param message bridger.Message
---@param fn fun(self: bridger.View, message: bridger.View): any
---@return bridger.Hook
function View:before(message, fn) end
---Instead of the engine's handler, for this object only.
---@param message bridger.Message
---@param fn fun(original: function, self: bridger.View, message: bridger.View)
---@return bridger.Hook
function View:replace(message, fn) end
---Replaces virtual method `slot` for this object alone.
---@param slot integer
---@param signature string
---@param fn fun(original: function, ...): any
---@return bridger.ContentEntry
function View:override(slot, signature, fn) end

---A Decima Array<T>: #array, array[i] (1-based), ipairs and pairs.
---@class bridger.Array
local Array = {}
---@return integer
function Array:count() end
---@return integer
function Array:data() end
---@return any[]
function Array:table() end
---@return integer
function Array:address() end

---An engine function, callable with its recorded signature.
---@class bridger.Function
---@field address integer
---@field signature string
---@field name string
---@field stub boolean true when the export is one of the build's compiled-out stubs
---@overload fun(...): any
local Function = {}
---@param fn fun(original: function, ...): any
---@return bridger.Hook
function Function:hook(fn) end
---@param fn fun(...): any
---@return bridger.Hook
function Function:before(fn) end
---@param fn fun(result: any, ...): any
---@return bridger.Hook
function Function:after(fn) end
---@param value any
---@return bridger.Hook|bridger.ContentEntry
function Function:force(value) end

---@class bridger.Hook
---@field calls integer
---@field name string
---@field active boolean
local Hook = {}
function Hook:remove() end

---@class bridger.ContentEntry
local ContentEntry = {}
---@return boolean
function ContentEntry:revert() end
---@return boolean
function ContentEntry:intact() end
---@return boolean
function ContentEntry:reapply() end

---Pass as an argument to receive what a pointer parameter is written with, after the result.
---@class bridger.Out
out = {}

---Return from a hook to answer without running the original (zero, false or nil).
---@class bridger.Skip
skip = {}

---@class bridger.Handle
local Handle = {}
function Handle:remove() end

-- ---- the engine --------------------------------------------------------------------------------

---@class game
---@field base integer the game's image base
game = {}

---An engine object at an address, as its own class (or `class` when it has no RTTI).
---@generic T
---@param address integer|bridger.View
---@param class? `T`
---@return T
function game.object(address, class) end
---Memory at an address read as a struct type.
---@generic T
---@param address integer
---@param class `T`
---@return T
function game.struct(address, class) end
---A zeroed struct value owned by the script: game.new("WorldPosition", {X = 1, Y = 2, Z = 3}).
---@generic T
---@param class `T`
---@param fields? table
---@return T
function game.new(class, fields) end
---@param name string
---@return { name: string, kind: string, size: integer, lineage: string[]?, messages: string[]?, fields: table[]?, values: table<string, integer>? }?
function game.type(name) end
---Type names matching every word of `pattern`.
---@param pattern? string
---@param limit? integer
---@return string[]
function game.types(pattern, limit) end
---The RTTI descriptor of a type, for engine functions that take `RTTI const *`.
---@param name string
---@return integer
function game.rtti(name) end
---@param key bridger.Symbol
---@return integer?
function game.symbol(key) end
---@param rva integer
---@return integer
function game.rva(rva) end
---Address of the global an export loads.
---@param key bridger.Symbol
---@param index? integer
---@return integer?
function game.data_ref(key, index) end
---The engine singleton an export loads.
---@param key bridger.Symbol
---@param index? integer
---@param class? string
---@return bridger.View?
function game.singleton(key, index, class) end
---Constructs an engine object the script owns, freed when it unloads.
---@generic T
---@param class `T`
---@param fields? table
---@return T
function game.create(class, fields) end
---@generic T
---@param object T
---@param fields? table
---@return T
function game.clone(object, fields) end
---@param object bridger.View|integer
---@return boolean
function game.destroy(object) end
---Revertible byte patch.
---@param address integer|bridger.View
---@param bytes string
---@return bridger.ContentEntry
function game.patch(address, bytes) end
---Adds `items` to the array a system selects from; see docs/content.md.
---@param owner integer|bridger.View
---@param count_offset integer
---@param data_offset integer
---@param items (bridger.View|integer)[]
---@param persistent? boolean
---@return bridger.ContentEntry
function game.inject(owner, count_offset, data_offset, items, persistent) end
---@param class bridger.HandlerClass
---@param message bridger.Message
---@return integer?
function game.handler(class, message) end
---@param class string
---@return integer?
function game.vtable(class) end
---@param object integer|bridger.View
function game.inspect(object) end
---@return boolean
function game.on_game_thread() end
---@param pattern string IDA style, "48 8B ?? 05"
---@return integer?
function game.scan(pattern) end
---@return boolean
function game.paused() end
---@return Player?
function game.player() end
---@return Entity?
function game.player_entity() end
---@return Entity?
function game.camera() end
---@param entity Entity
---@return bridger.Vec3
function game.position(entity) end

---A function at an address, or a named export with a signature of your own.
---@param target integer|bridger.Symbol|bridger.Function
---@param signature string "float(ptr, float)", "void(Entity*, WorldPosition const&)"
---@return bridger.Function
function fn(target, signature) end

---@class mem
mem = {}
---@param address integer|bridger.View
---@param type string "float", "int32", "ptr", "WorldPosition", ...
---@return any
function mem.read(address, type) end
---@param address integer|bridger.View
---@param type string
---@param value any
function mem.write(address, type, value) end
---@param address integer|bridger.View
---@param count integer
---@return string
function mem.bytes(address, count) end
---Unrevertible raw write; prefer game.patch.
---@param address integer|bridger.View
---@param bytes string
---@return boolean
function mem.poke(address, bytes) end
---@param address integer
---@param limit? integer
---@return string
function mem.string(address, limit) end
---@param address integer
---@param limit? integer
---@return string
function mem.wstring(address, limit) end
---@param size integer
---@param class? string
---@return bridger.View
function mem.alloc(size, class) end
---@param address integer
---@param size? integer
---@return boolean
function mem.readable(address, size) end
---@param pattern string
---@return integer?
function mem.scan(pattern) end
---@param address integer|bridger.View
---@param bytes string
---@return bridger.ContentEntry
function mem.patch(address, bytes) end

---@param type string
---@param value integer
---@return string?
function enum.name(type, value) end

-- ---- hooks ---------------------------------------------------------------------------------------

---Replaces an engine function. `original` runs the rest of the chain and the engine's code, with new
---arguments or the ones the call came with. Return a value to answer, nothing for the original's
---answer, or skip to answer zero without running it.
---@overload fun(class: bridger.HandlerClass, message: bridger.Message, fn: fun(original: function, self: bridger.View, message: bridger.View)): bridger.Hook
---@overload fun(address: integer, signature: string, fn: fun(original: function, ...): any): bridger.Hook
---@param target bridger.Symbol|bridger.Function
---@param fn fun(original: function, ...): any
---@return bridger.Hook
function hook(target, fn) end

---Runs first. Return nothing to let the call through, a value to answer instead, skip to answer zero.
---@overload fun(class: bridger.HandlerClass, message: bridger.Message, fn: fun(self: bridger.View, message: bridger.View): any): bridger.Hook
---@overload fun(address: integer, signature: string, fn: fun(...): any): bridger.Hook
---@param target bridger.Symbol|bridger.Function
---@param fn fun(...): any
---@return bridger.Hook
function before(target, fn) end

---Runs after, with the result first (for functions that return one). Return a value to replace it.
---@overload fun(class: bridger.HandlerClass, message: bridger.Message, fn: fun(self: bridger.View, message: bridger.View)): bridger.Hook
---@overload fun(address: integer, signature: string, fn: fun(result: any, ...): any): bridger.Hook
---@param target bridger.Symbol|bridger.Function
---@param fn fun(result: any, ...): any
---@return bridger.Hook
function after(target, fn) end

---After the engine's handler for a message, for every object of the class.
---@param class bridger.HandlerClass
---@param message bridger.Message
---@param fn fun(self: bridger.View, message: bridger.View)
---@return bridger.Hook
function on(class, message, fn) end

---The function returns `value` and none of it runs.
---@param target bridger.Symbol|bridger.Function
---@param value any
---@return bridger.Hook|bridger.ContentEntry
function force(target, value) end

---A native function pointer that runs `fn`, for handing script code to the engine.
---@param signature string
---@param fn fun(...): any
---@return bridger.Function
function callback(signature, fn) end

-- ---- time -----------------------------------------------------------------------------------------

---@param fn fun(dt: number)
---@return bridger.Handle
function every_frame(fn) end
---@param seconds number
---@param fn fun()
---@return bridger.Handle
function later(seconds, fn) end
---@param seconds number
---@param fn fun()
---@return bridger.Handle
function every(seconds, fn) end
---@param fn fun()
---@return bridger.Handle
function ready(fn) end
---@param fn fun(...)
---@return bridger.Handle
function task(fn, ...) end
---@param seconds number
function wait(seconds) end
---@param frames? integer
function wait_frames(frames) end
---@param predicate fun(): boolean
---@param timeout? number
---@return boolean
function wait_until(predicate, timeout) end
---@param fn fun()
function on_unload(fn) end
---@return number
function game_time() end
---@return integer
function game_frame() end
---Seconds of real time since the runtime started.
---@return number
function now() end

-- ---- input ----------------------------------------------------------------------------------------

---@alias bridger.Key string|integer "F5", "Ctrl+K", "Mouse4", or a virtual key code

---@class input
input = {}
---@param key bridger.Key
---@return boolean
function input.down(key) end
---@param key bridger.Key
---@return boolean
function input.pressed(key) end
---@return boolean
function input.overlay() end
---@param key bridger.Key|bridger.Setting
---@param fn fun()
---@return bridger.Handle
function bind(key, fn) end
---@param key bridger.Key
---@return integer code, integer[] modifiers
function key_code(key) end
---@param code integer
---@return string
function key_name(code) end

-- ---- settings and state -----------------------------------------------------------------------------

---@class bridger.Setting
---@field key string
---@field value any
---@field default any
---@field label string
---@overload fun(): any
local Setting = {}
---@return any
function Setting:get() end
---@param value any
function Setting:set(value) end
function Setting:reset() end
---@return boolean
function Setting:modified() end
---@param fn fun(value: any)
function Setting:on_change(fn) end

---@class bridger.SettingOptions
---@field label? string
---@field help? string
---@field min? number
---@field max? number
---@field suffix? string
---@field int? boolean
---@field key? boolean the default is a key name and the value a key code
---@field choices? string[] the value is an index into these
---@field placeholder? string
---@field order? integer

---A persisted value with a panel row.
---@param key string
---@param default boolean|number|string
---@param options? bridger.SettingOptions
---@return bridger.Setting
function setting(key, default, options) end

---Several settings at once; the returned table reads and writes their values.
---@param schema table<string, boolean|number|string|table>
---@return table<string, any>
function settings(schema) end

---@class bridger.Keep
---@overload fun(name: string, default: any): any
keep = {}
---@param name string
---@param value any
function keep.set(name, value) end

-- ---- panels and ui ------------------------------------------------------------------------------------

---Declares a page in the overlay's Panels tab. Call it while the script loads.
---@param label string
---@param draw fun()
function panel(label, draw) end

---@alias bridger.Colour "text"|"dim"|"faint"|"accent"|"good"|"warn"|"bad"|integer

---@class ui
ui = {}
---@param text any
---@param colour? bridger.Colour
function ui.text(text, colour) end
---@param text any
---@param colour? bridger.Colour
function ui.wrapped(text, colour) end
---@param text string
function ui.header(text) end
function ui.separator() end
---@param amount? number
function ui.spacing(amount) end
---@param offset? number
function ui.same_line(offset) end
function ui.newline() end
---@param amount? number
function ui.indent(amount) end
---@param amount? number
function ui.unindent(amount) end
---@param label string
---@param width? number
---@return boolean clicked
function ui.button(label, width) end
---@param label string
---@param width? number
---@return boolean clicked
function ui.ghost_button(label, width) end
---@param label string
---@param value boolean
---@return boolean value, boolean changed
function ui.checkbox(label, value) end
---@param label string
---@param value number
---@param min number
---@param max number
---@param width? number
---@return number value, boolean changed
function ui.slider(label, value, min, max, width) end
---@param id string
---@param value string
---@param placeholder? string
---@param width? number
---@return string value, boolean changed
function ui.input(id, value, placeholder, width) end
---@param label string
---@param value any
---@param colour? bridger.Colour
function ui.readout(label, value, colour) end
---@param text string
function ui.note(text) end
---@param label string
---@param colour? bridger.Colour
function ui.badge(label, colour) end
---@param label string
function ui.keycap(label) end
---@param text string
function ui.tooltip(text) end
---@param label string
---@param open? boolean
---@return boolean expanded
function ui.group(label, open) end
function ui.end_group() end
---@param fraction number
---@param width? number
---@param colour? bridger.Colour
function ui.progress(fraction, width, colour) end
---@param id string
function ui.push_id(id) end
function ui.pop_id() end
---@param id string
---@param height number
function ui.scroll(id, height) end
function ui.end_scroll() end
---@return number
function ui.width() end
---@param modified boolean
---@return boolean clicked
function ui.revert(modified) end
---@param label string
---@param value any
---@param options? bridger.SettingOptions
---@return any value, boolean changed
function ui.setting(label, value, options) end
---@param setting bridger.Setting
---@return boolean changed
function ui.row(setting) end
function ui.settings() end

-- ---- the script -----------------------------------------------------------------------------------------

---@class script
---@field id string
---@field dir string
---@field root string
---@field console boolean
script = {}
function script.reload() end
---@param id string
function script.reload_mod(id) end
---@return { id: string, alive: boolean, error: string, errors: integer, hooks: integer, memory_kb: integer }[]
function script.list() end
---@param ms? integer
---@return integer previous
function script.budget(ms) end

---@class log
log = {}
---@param ... any
function log.info(...) end
---@param ... any
function log.warn(...) end
---@param ... any
function log.error(...) end

-- ---- helpers ----------------------------------------------------------------------------------------------

---@class bridger.Vec3
---@field X number
---@field Y number
---@field Z number
---@operator add(bridger.Vec3|number): bridger.Vec3
---@operator sub(bridger.Vec3|number): bridger.Vec3
---@operator mul(bridger.Vec3|number): bridger.Vec3
---@operator div(bridger.Vec3|number): bridger.Vec3
---@operator unm: bridger.Vec3
local Vec3 = {}
---@return number
function Vec3:length() end
---@param other bridger.Vec3
---@return number
function Vec3:dot(other) end
---@param other bridger.Vec3
---@return bridger.Vec3
function Vec3:cross(other) end
---@return bridger.Vec3
function Vec3:normalized() end
---@param other bridger.Vec3|bridger.View
---@return number
function Vec3:distance(other) end
---@param other bridger.Vec3|bridger.View
---@param t number
---@return bridger.Vec3
function Vec3:lerp(other, t) end

---@param x? number|bridger.View|bridger.Vec3
---@param y? number
---@param z? number
---@return bridger.Vec3
function vec3(x, y, z) end

---@param value any
---@param depth? integer
---@return string
function dump(value, depth) end
---@param value any
---@return string
function inspect(value) end
---@param pattern string
---@param limit? integer
---@return string[]
function find(pattern, limit) end
function help() end

---@class BridgerUpscaleInfo
---@field available boolean
---@field width integer
---@field height integer
---@field jitter_x number
---@field jitter_y number
fx = {}
---@return BridgerUpscaleInfo
function fx.upscale_info() end
---@param source string Fullscreen HLSL source with ps_main
---@param options? {history?: boolean, priority?: integer}
---@return integer effect
function fx.pre_upscale_pass(source, options) end
function fx.revert() end

---@class BridgerPipeline
---@field hash string 16 hex digits, stable across runs
---@field compute boolean
---@field draws integer draws or dispatches last frame
---@field draws_total integer
---@field first_use integer order of first draw last frame, 0 if it did not draw
---@field render_targets integer colour targets; several for geometry passes, one for post
---@field replaced boolean a mod replacement or the material hook is in effect
---@field hooked boolean

--- Every pipeline the game has created, in creation order.
---@return BridgerPipeline[]
function fx.pipelines() end
---@param hash string
---@return BridgerPipeline?
function fx.pipeline(hash) end
--- Writes the pipeline's shaders and disassembly under Bridger/dumps/pipelines.
---@param hash string
---@return string? folder
function fx.pipeline_dump(hash) end
--- Splices `float4 bridger_material(float4 color : COLOR0, float4 position : SV_Position)`
--- from an HLSL file (relative to the script's mod folder) into every matching pixel shader.
---@param path string
---@param options? {entry?: string, defines?: string[], min_targets?: integer, max_targets?: integer, only?: string}
---@return integer handle 0 on failure
function fx.material_hook(path, options) end
---@param handle integer
---@return string
function fx.pipeline_problem(handle) end
---@param handle integer
function fx.pipeline_revert(handle) end
