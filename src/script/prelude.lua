
local traceback = debug.traceback
local insert, remove, concat, sort = table.insert, table.remove, table.concat, table.sort
local format, rep = string.format, string.rep
local resume, create, status, running, yield =
    coroutine.resume, coroutine.create, coroutine.status, coroutine.running, coroutine.yield

local frame_callbacks = {}
local timers = {}
local tasks = {}
local unload_callbacks = {}
local key_bindings = {}
local key_state = {}

local game_clock = 0.0
local frame_number = 0
local last_dt = 0.0

local Handle = {}
Handle.__index = Handle
function Handle:remove() self.removed = true end
function Handle:__tostring() return format("%s%s", self.kind, self.removed and " (removed)" or "") end

local function handle(kind, fields)
    fields.kind = kind
    return setmetatable(fields, Handle)
end

function every_frame(fn)
    assert(type(fn) == "function", "every_frame(fn) takes a function")
    local h = handle("every_frame", { fn = fn })
    insert(frame_callbacks, h)
    return h
end

function later(seconds, fn)
    assert(type(fn) == "function", "later(seconds, fn) takes a function")
    local h = handle("later", { at = game_clock + (seconds or 0), fn = fn })
    insert(timers, h)
    return h
end

function every(seconds, fn)
    assert(type(seconds) == "number" and seconds > 0, "every(seconds, fn) needs a positive interval")
    local h = handle("every", { at = game_clock + seconds, period = seconds, fn = fn })
    insert(timers, h)
    return h
end

function ready(fn)
    return later(0, fn)
end

function task(fn, ...)
    local co = create(fn)
    local h = handle("task", { co = co })
    insert(tasks, h)
    local ok, request = resume(co, ...)
    if not ok then
        h.removed = true
        log.error("task failed: " .. traceback(co, tostring(request)))
    elseif type(request) == "table" then
        h.until_time = request.until_time
        h.frames = request.frames
    end
    return h
end

local function in_task(name)
    local co, main = running()
    if main then error(name .. "() only works inside a task(function() ... end)", 3) end
    return co
end

function wait(seconds)
    in_task("wait")
    yield({ until_time = game_clock + (seconds or 0) })
end

function wait_frames(n)
    in_task("wait_frames")
    yield({ frames = n or 1 })
end

function wait_until(predicate, timeout)
    in_task("wait_until")
    local deadline = timeout and (game_clock + timeout)
    while not predicate() do
        if deadline and game_clock >= deadline then return false end
        yield({ frames = 1 })
    end
    return true
end

function on_unload(fn)
    insert(unload_callbacks, fn)
end

function game_time() return game_clock end
function game_frame() return frame_number end

local function report(what, err)
    log.error(format("%s: %s", what, tostring(err)))
end

local function run_list(list, what, call)
    local i = 1
    while i <= #list do
        local h = list[i]
        if h.removed then
            remove(list, i)
        else
            local ok, err = xpcall(call, traceback, h)
            if not ok then
                h.removed = true
                report(what .. " stopped after an error", err)
            end
            i = i + 1
        end
    end
end

local VK = {
    Mouse1 = 0x01, Mouse2 = 0x02, Mouse3 = 0x04, Mouse4 = 0x05, Mouse5 = 0x06,
    Backspace = 0x08, Tab = 0x09, Enter = 0x0D, Shift = 0x10, Ctrl = 0x11, Control = 0x11, Alt = 0x12,
    Pause = 0x13, CapsLock = 0x14, Escape = 0x1B, Esc = 0x1B, Space = 0x20, PageUp = 0x21,
    PageDown = 0x22, End = 0x23, Home = 0x24, Left = 0x25, Up = 0x26, Right = 0x27, Down = 0x28,
    Insert = 0x2D, Delete = 0x2E, LShift = 0xA0, RShift = 0xA1, LCtrl = 0xA2, RCtrl = 0xA3,
    LAlt = 0xA4, RAlt = 0xA5, Multiply = 0x6A, Add = 0x6B, Subtract = 0x6D, Decimal = 0x6E,
    Divide = 0x6F, Semicolon = 0xBA, Plus = 0xBB, Comma = 0xBC, Minus = 0xBD, Period = 0xBE,
    Slash = 0xBF, Tilde = 0xC0, Backquote = 0xC0, LBracket = 0xDB, Backslash = 0xDC,
    RBracket = 0xDD, Quote = 0xDE,
}
for i = 1, 24 do VK["F" .. i] = 0x6F + i end
for i = 0, 9 do
    VK[tostring(i)] = 0x30 + i
    VK["Numpad" .. i] = 0x60 + i
end
for c = string.byte("A"), string.byte("Z") do VK[string.char(c)] = c end

local key_names = {}
for name, code in pairs(VK) do
    if not key_names[code] or #name < #key_names[code] then key_names[code] = name end
end

function key_code(key)
    if type(key) == "number" then return key, {} end
    if type(key) == "table" and key.value then return key_code(key.value) end
    assert(type(key) == "string", "a key is a name like \"F5\" or \"Ctrl+K\", or a key code")
    local parts = {}
    for part in key:gmatch("[^%+]+") do insert(parts, (part:gsub("^%s+", ""):gsub("%s+$", ""))) end
    local main = parts[#parts]
    local code = VK[main] or VK[main:upper()] or VK[main:sub(1, 1):upper() .. main:sub(2):lower()]
    if not code then error("unknown key '" .. main .. "'", 2) end
    local modifiers = {}
    for i = 1, #parts - 1 do
        local m = VK[parts[i]] or VK[parts[i]:sub(1, 1):upper() .. parts[i]:sub(2):lower()]
        if not m then error("unknown modifier '" .. parts[i] .. "'", 2) end
        insert(modifiers, m)
    end
    return code, modifiers
end

function key_name(code) return key_names[code] or format("0x%02X", code) end

local function track(code)
    local state = key_state[code]
    if not state then
        state = { down = input.raw_down(code), pressed = false }
        key_state[code] = state
    end
    return state
end

function input.down(key)
    local code, modifiers = key_code(key)
    for _, m in ipairs(modifiers) do
        if not input.raw_down(m) then return false end
    end
    return input.raw_down(code)
end

function input.pressed(key)
    local code, modifiers = key_code(key)
    local state = track(code)
    if not state.pressed then return false end
    for _, m in ipairs(modifiers) do
        if not input.raw_down(m) then return false end
    end
    return true
end

function bind(key, fn)
    assert(type(fn) == "function", "bind(key, fn) takes a function")
    local h = handle("bind", { key = key, fn = fn })
    insert(key_bindings, h)
    return h
end

local function poll_keys()
    for code, state in pairs(key_state) do
        local down = input.raw_down(code)
        state.pressed = down and not state.down
        state.down = down
    end
    local i = 1
    while i <= #key_bindings do
        local h = key_bindings[i]
        if h.removed then
            remove(key_bindings, i)
        else
            local ok, code = pcall(key_code, h.key)
            if ok and code and code ~= 0 then
                track(code)
                if input.pressed(h.key) then
                    local fine, err = xpcall(h.fn, traceback)
                    if not fine then report("key binding " .. tostring(key_name(code)), err) end
                end
            end
            i = i + 1
        end
    end
end

function __bridger_tick(dt)
    last_dt = dt
    game_clock = game_clock + dt
    frame_number = frame_number + 1

    poll_keys()
    run_list(frame_callbacks, "every_frame callback", function(h) h.fn(dt) end)

    local i = 1
    while i <= #timers do
        local h = timers[i]
        if h.removed then
            remove(timers, i)
        elseif game_clock >= h.at then
            local ok, err = xpcall(h.fn, traceback)
            if not ok then
                report(h.kind .. " timer", err)
                h.removed = true
            elseif h.period then
                h.at = h.at + h.period
                if h.at < game_clock then h.at = game_clock + h.period end
            else
                h.removed = true
            end
            i = i + 1
        else
            i = i + 1
        end
    end

    i = 1
    while i <= #tasks do
        local h = tasks[i]
        if h.removed or status(h.co) == "dead" then
            remove(tasks, i)
        else
            local due = true
            if h.until_time and game_clock < h.until_time then due = false end
            if h.frames and h.frames > 0 then
                h.frames = h.frames - 1
                due = h.frames <= 0 and due
            end
            if due then
                local ok, request = resume(h.co)
                if not ok then
                    h.removed = true
                    report("task failed", traceback(h.co, tostring(request)))
                elseif type(request) == "table" then
                    h.until_time = request.until_time
                    h.frames = request.frames
                else
                    h.until_time, h.frames = nil, nil
                end
            end
            i = i + 1
        end
    end
end

function game.paused() return last_dt == 0 end

function __bridger_unload()
    for i = #unload_callbacks, 1, -1 do
        local ok, err = xpcall(unload_callbacks[i], traceback)
        if not ok then report("on_unload", err) end
    end
end

local declared = {}
local panel_declared = false
local native_panel = panel

function panel(label, fn)
    panel_declared = true
    return native_panel(label, fn)
end

local Setting = {}
Setting.__index = Setting

local function pretty(key)
    local text = key:gsub("[_%.]", " ")
    return (text:gsub("^%l", string.upper))
end

function setting(key, default, options)
    options = options or {}
    local s = setmetatable({ key = key, options = options, label = options.label or pretty(key),
                             listeners = {} }, Setting)
    if options.key then
        s.default = key_code(default)
    else
        s.default = default
    end
    s.value = __settings.get(key, s.default)
    insert(declared, s)
    return s
end

function Setting:__call() return self.value end
function Setting:__tostring() return format("setting %s = %s", self.key, tostring(self.value)) end
function Setting:get() return self.value end
function Setting:modified() return self.value ~= self.default end
function Setting:reset() self:set(self.default) end
function Setting:on_change(fn) insert(self.listeners, fn) end

function Setting:set(value)
    if value == self.value then return end
    self.value = value
    __settings.set(self.key, value)
    for _, fn in ipairs(self.listeners) do
        local ok, err = xpcall(fn, traceback, value)
        if not ok then report("setting " .. self.key .. " listener", err) end
    end
end

function settings(schema)
    local keys = {}
    for key in pairs(schema) do insert(keys, key) end
    sort(keys, function(a, b)
        local oa = type(schema[a]) == "table" and schema[a].order or 1000
        local ob = type(schema[b]) == "table" and schema[b].order or 1000
        if oa ~= ob then return oa < ob end
        return a < b
    end)
    local byname = {}
    for _, key in ipairs(keys) do
        local spec = schema[key]
        if type(spec) == "table" then
            local default = spec[1]
            if default == nil then default = spec.default end
            byname[key] = setting(key, default, spec)
        else
            byname[key] = setting(key, spec)
        end
    end
    return setmetatable({}, {
        __index = function(_, key)
            local s = byname[key]
            if not s then error("no setting named '" .. tostring(key) .. "'", 2) end
            return s.value
        end,
        __newindex = function(_, key, value)
            local s = byname[key]
            if not s then error("no setting named '" .. tostring(key) .. "'", 2) end
            s:set(value)
        end,
        __pairs = function()
            local i = 0
            return function()
                i = i + 1
                local key = keys[i]
                if key then return key, byname[key].value end
            end
        end,
    })
end

function ui.row(s)
    local options = s.options
    local value, changed = ui.setting(s.label, s.value, options)
    if changed then s:set(value) end
    if ui.revert(s:modified()) then
        s:reset()
        changed = true
    end
    return changed
end

function ui.settings()
    for _, s in ipairs(declared) do ui.row(s) end
end

function __bridger_after_load()
    if #declared > 0 and not panel_declared and not script.console then
        native_panel("Settings", function() ui.settings() end)
    end
end

__bridger_kept = {}

keep = setmetatable({}, {
    __call = function(_, name, default)
        local value
        if __bridger_restore then value = __bridger_restore(name) end
        if value == nil then value = default end
        __bridger_kept[name] = value
        return value
    end,
})

function keep.set(name, value) __bridger_kept[name] = value end

local Vec3 = {}
Vec3.__index = Vec3

function vec3(x, y, z)
    if type(x) ~= "number" and x ~= nil then
        return setmetatable({ X = x.X, Y = x.Y, Z = x.Z }, Vec3)
    end
    return setmetatable({ X = x or 0, Y = y or 0, Z = z or 0 }, Vec3)
end

local function v(a) return type(a) == "number" and vec3(a, a, a) or a end
Vec3.__add = function(a, b) a, b = v(a), v(b); return vec3(a.X + b.X, a.Y + b.Y, a.Z + b.Z) end
Vec3.__sub = function(a, b) a, b = v(a), v(b); return vec3(a.X - b.X, a.Y - b.Y, a.Z - b.Z) end
Vec3.__mul = function(a, b) a, b = v(a), v(b); return vec3(a.X * b.X, a.Y * b.Y, a.Z * b.Z) end
Vec3.__div = function(a, b) a, b = v(a), v(b); return vec3(a.X / b.X, a.Y / b.Y, a.Z / b.Z) end
Vec3.__unm = function(a) return vec3(-a.X, -a.Y, -a.Z) end
Vec3.__eq = function(a, b) return a.X == b.X and a.Y == b.Y and a.Z == b.Z end
Vec3.__tostring = function(a) return format("vec3(%.3f, %.3f, %.3f)", a.X, a.Y, a.Z) end
function Vec3:length() return math.sqrt(self.X * self.X + self.Y * self.Y + self.Z * self.Z) end
function Vec3:dot(o) return self.X * o.X + self.Y * o.Y + self.Z * o.Z end
function Vec3:cross(o)
    return vec3(self.Y * o.Z - self.Z * o.Y, self.Z * o.X - self.X * o.Z, self.X * o.Y - self.Y * o.X)
end
function Vec3:normalized()
    local l = self:length()
    return l > 0 and vec3(self.X / l, self.Y / l, self.Z / l) or vec3()
end
function Vec3:distance(o) return (vec3(o) - self):length() end
function Vec3:lerp(o, t) return self + (vec3(o) - self) * t end

function game.player()
    return engine.PlayerSymbols.Player_sExportedGetLocalPlayer(0)
end

function game.player_entity()
    local player = game.player()
    return player and engine.PlayerSymbols.Player_ExportedGetEntity(player)
end

function game.camera()
    local player = game.player()
    return player and engine.PlayerSymbols.Player_ExportedGetLastActivatedCamera(player)
end

function game.position(entity)
    return vec3(engine.EntitySymbols.Entity_ExportedGetPosition(entity))
end

local function is_identifier(s)
    return type(s) == "string" and s:match("^[%a_][%w_]*$") ~= nil
end

local function repr(value, depth, seen)
    local t = type(value)
    if t == "string" then return format("%q", value) end
    if t ~= "table" then
        if t == "userdata" then
            local mt = getmetatable(value)
            if mt and mt.__name == "bridger.View" and value:type() ~= "memory" and depth > 0 then
                local ok, fields = pcall(value.table, value)
                if ok then
                    local parts, count = {}, 0
                    for _, field in ipairs(value:fields()) do
                        local item = fields[field.name]
                        local kind = type(item)
                        if kind == "number" or kind == "boolean" or kind == "string" then
                            count = count + 1
                            if count > 10 then
                                insert(parts, "...")
                                break
                            end
                            insert(parts, field.name .. " = " .. repr(item, 0, seen))
                        end
                    end
                    if #parts > 0 then return format("%s { %s }", tostring(value), concat(parts, ", ")) end
                end
            end
        end
        return tostring(value)
    end
    if seen[value] then return "<cycle>" end
    if getmetatable(value) == Vec3 or getmetatable(value) == Handle then return tostring(value) end
    if depth <= 0 then return "{...}" end
    seen[value] = true
    local parts, count = {}, 0
    local length = #value
    for i = 1, length do
        count = count + 1
        if count > 40 then insert(parts, "...") break end
        insert(parts, repr(value[i], depth - 1, seen))
    end
    local keys = {}
    for k in pairs(value) do
        if not (math.type(k) == "integer" and k >= 1 and k <= length) then insert(keys, k) end
    end
    sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    for _, k in ipairs(keys) do
        count = count + 1
        if count > 40 then insert(parts, "...") break end
        local label = is_identifier(k) and k or ("[" .. repr(k, 0, seen) .. "]")
        insert(parts, label .. " = " .. repr(value[k], depth - 1, seen))
    end
    seen[value] = nil
    return "{ " .. concat(parts, ", ") .. " }"
end

function __bridger_repr(value) return repr(value, 2, {}) end

function dump(value, depth) return repr(value, depth or 3, {}) end

function inspect(value)
    if type(value) == "userdata" or math.type(value) == "integer" then game.inspect(value) end
    return dump(value, 1)
end

function find(pattern, limit)
    local out = {}
    for _, name in ipairs(engine.find(pattern, limit or 30)) do insert(out, name) end
    for _, name in ipairs(game.types(pattern, limit or 30)) do insert(out, "type " .. name) end
    return out
end

local function sorted_matches(names, partial, prefix, sep)
    local out, seen = {}, {}
    local lower = partial:lower()
    for _, name in ipairs(names) do
        if type(name) == "string" and not seen[name] and name:lower():sub(1, #lower) == lower then
            seen[name] = true
            insert(out, prefix .. sep .. name)
        end
    end
    sort(out)
    return out
end

function __bridger_complete(text)
    local head, sep, partial = text:match("^(.-)([%.:])([%w_]*)$")
    if not head or head == "" then
        local lead, word = text:match("^(.-)([%a_][%w_]*)$")
        if not word then return {} end
        local names = {}
        for k in pairs(_G) do if type(k) == "string" and not k:match("^__") then insert(names, k) end end
        local out = {}
        for _, full in ipairs(sorted_matches(names, word, "", "")) do insert(out, lead .. full) end
        return out
    end
    local path = head:match("[%w_%.%[%]\"']+$")
    if not path or path ~= head:sub(-#path) then return {} end
    local lead = head:sub(1, #head - #path)
    local chunk = load("return " .. path, "=complete", "t")
    if not chunk then return {} end
    local ok, value = pcall(chunk)
    if not ok or value == nil then return {} end

    local names = {}
    if value == engine then
        names = engine.groups()
    elseif type(value) == "table" then
        local mt = getmetatable(value)
        if mt and rawget(value, "list") and type(rawget(value, "list")) == "function" and mt.__name then
            names = value.list()
        else
            for k in pairs(value) do insert(names, k) end
            if mt and type(mt.__index) == "table" then
                for k in pairs(mt.__index) do insert(names, k) end
            end
        end
    elseif type(value) == "userdata" then
        local mt = getmetatable(value)
        if mt and mt.methods then
            for k in pairs(mt.methods) do insert(names, k) end
        end
        if mt and mt.__name == "bridger.View" and sep == "." then
            names = {}
            for _, field in ipairs(value:fields()) do insert(names, field.name) end
        end
    end
    return sorted_matches(names, partial, lead .. path, sep)
end

function help()
    print([[
Bridger Script - quick reference (full guide: docs/scripting.md)

  game.player_entity()                 the entity the player controls
  obj.Field / obj.Field = v            read and write any reflected field by name
  obj:fields()  obj:type()  obj:messages()  obj:inspect()
  engine.Group.Function(args...)       call an exported engine function
  engine.find("position")              search engine functions
  game.types("catcher")  game.type("Entity")
  hook("Group::Name", function(original, ...) return original(...) end)
  before / after (target, fn)          observe or veto; return skip to veto
  on("Class", "MsgName", function(self, msg) end)
  obj:on("MsgName", fn)                one object only
  force("Group::Name", value)          make a function return a constant
  every_frame(fn)  later(s, fn)  every(s, fn)  task(fn) + wait(s)
  bind("F5", fn)   input.down("W")
  settings { speed = { 8.0, min = 0, max = 50 } }   persisted, with a panel
  keep("state", {})                    survives reloads
  dump(value)  inspect(obj)  find("pattern")]])
end
