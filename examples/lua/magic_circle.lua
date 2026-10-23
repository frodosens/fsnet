-- Server adapter. Bundle shared/magic_circle.lua first (see ../README.md).
local Circle = fsnet.Entity:extend('MagicCircle', {owner='id'})
Circle:attr('radius', {type='number', default=0, server='rw', client='r', sync='observers', persist=false})
Circle:attr('duration', {type='number', default=5, server='rw', client='r', sync='observers', persist=false})

function Circle:contains(x, y)
    return MagicCircleLogic.contains(self, x, y)
end

function Circle:on_tick(step_ms)
    MagicCircleLogic.advance(self, step_ms / 1000)
end

Circle:rpc('cancel', {callers={'client'}, access='owner'})
function Circle:cancel()
    self.duration = 0
end

-- Tick and cancel use the same entity mailbox. Sync is published only after CAS.
-- contains is a local helper, not a network entry point.
Circle:register()
