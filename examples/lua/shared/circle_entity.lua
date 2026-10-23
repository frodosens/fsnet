-- Bundle shared/magic_circle.lua before this class. Both peers load this file.
MagicCircle = fsnet.Entity:extend('MagicCircle', {owner='id'})
MagicCircle:attr('radius', {type='number', default=0, authority='driver', client='r', sync='observers', persist=false})
MagicCircle:attr('duration', {type='number', default=5, authority='driver', client='r', sync='observers', persist=false})

function MagicCircle:advance(dt)
    MagicCircleLogic.advance(self, dt)
end
function MagicCircle:contains(x, y)
    return MagicCircleLogic.contains(self, x, y)
end
MagicCircle:method('on_tick', {runs_on='server'})
function MagicCircle:on_tick(step_ms)
    self:advance(step_ms / 1000)
end
MagicCircle:rpc('cancel', {callers={'client'}, access='owner'})
function MagicCircle:cancel()
    self.duration = 0
end
MagicCircle:define() -- define the shared model without exporting server routes
