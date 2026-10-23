-- Standalone local-client example: bundle ../shared/magic_circle.lua first.
-- This uses ordinary Lua tables, not an unimplemented fsnet client entity API.
local local_circle = {radius=0, duration=5}
MagicCircleLogic.advance(local_circle, 1)
assert(local_circle.radius == 2 and local_circle.duration == 4)
assert(MagicCircleLogic.contains(local_circle, 1, 0))
assert(not MagicCircleLogic.contains(local_circle, 3, 0))

-- In server-driven mode, the client adapter supplies confirmed property values.
-- Illustrative snapshot only: this is not a network subscription.
local confirmed_circle = {radius=4, duration=3}
assert(MagicCircleLogic.contains(confirmed_circle, 3, 0))
-- Do not advance confirmed state locally; prediction needs a separate model.
MagicCircleLogic.advance(local_circle, 10)
assert(local_circle.radius == 10 and local_circle.duration == 0)
assert(not MagicCircleLogic.contains(local_circle, 0, 0))
