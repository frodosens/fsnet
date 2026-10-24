-- The same Player type runs on the server and supplies client prediction rules.
Player=fsnet.Entity:extend('Player',{owner='id'})
Player:attr('name',{type='string',default='',sync='observers'})
Player:attr('position',{type='vec3',authority='driver',sync='observers'})
Player:attr('processed_input',{type='int64',default=0,sync='owner'})
Player:attr('created',{type='int32',default=0,sync='owner'})
Player:attr('activations',{type='int32',default=0,sync='owner'})
function Player:on_create() self.created=self.created+1 end
function Player:on_activate() self.activations=self.activations+1 end
-- Replayable rule: state only. Rendering/audio are handled outside this method.
function Player:advance(payload)
    local delta=assert(tonumber(payload),'invalid input')
    assert(math.type(delta)=='integer' and math.abs(delta)<=8,'movement limit')
    self.position.x=self.position.x+delta
    return 'footstep'
end
Player:method('input',{runs_on='server'})
Player:rpc('input',{callers={'client'},access='owner'})
function Player:input(payload)
    local sequence,delta=payload:match('^(%d+):(-?%d+)$')
    sequence=tonumber(sequence);delta=tonumber(delta)
    if not sequence or sequence~=self.processed_input+1 or not delta or math.abs(delta)>8 then self:reject() end
    self:advance(tostring(delta));self.processed_input=sequence
end
Player:method('describe',{runs_on='server'})
Player:rpc('describe',{callers={'client'},access='owner'})
function Player:describe() return self.name end
Player:define()
