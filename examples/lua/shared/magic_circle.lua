-- Shared gameplay module: no fsnet, networking, rendering or storage dependency.
-- Bundle this file before either ../magic_circle.lua or ../client/magic_circle.lua.
MagicCircleLogic = {}

function MagicCircleLogic.advance(circle, dt)
    assert(type(dt) == 'number' and dt >= 0 and dt < math.huge, 'invalid dt')
    local elapsed = math.min(dt, circle.duration)
    circle.radius = math.min(10, circle.radius + elapsed * 2)
    circle.duration = math.max(0, circle.duration - elapsed)
end

function MagicCircleLogic.contains(circle, x, y)
    return circle.duration > 0 and x * x + y * y <= circle.radius * circle.radius
end
