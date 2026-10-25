-- Entity IDs are stable strings. Account ownership is separate metadata.
Player=fsnet.Entity:extend('Player',{owner='external'})
Player:attr('name',{type='string',default='',sync='observers'})
Player:attr('position',{type='vec3',authority='driver',sync='observers'})
Player:rpc('move',{callers={'client'},access='owner'})
function Player:move(input)
    local delta=tonumber(input)
    if not delta or math.abs(delta)>8 then self:reject() end
    self.position.x=self.position.x+delta
    return tostring(self.position.x)
end
function Player:on_deactivate() end
Player:define()
