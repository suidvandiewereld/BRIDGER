
local config = settings {
    enabled = { true, order = 1, help = "Turns the mod on and off." },
    strength = { 1.0, min = 0, max = 10, order = 2 },
}

local toggle = setting("toggle", "F6", { key = true, label = "Toggle" })

bind(toggle, function()
    config.enabled = not config.enabled
    print("__MOD_NAME__ " .. (config.enabled and "on" or "off"))
end)

every_frame(function(dt)
    if not config.enabled then return end
    local body = game.player_entity()
    if not body then return end
end)
