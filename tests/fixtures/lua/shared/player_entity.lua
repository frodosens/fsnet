-- Both peers load the same authoritative entity type.
PlayerEntity=fsnet.Entity:extend('Player',{owner='legacy_id'})
PlayerEntity:attr('name',{type='string',default='',sync='observers'})
PlayerEntity:attr('x',{type='int32',default=0,authority='driver',sync='observers'})
PlayerEntity:attr('gold',{type='int64',default=0,sync='owner'})
PlayerEntity:attr('processed_input',{type='int64',default=0,sync='owner'})
function PlayerEntity:advance(payload)
    local delta=assert(tonumber(payload),'invalid movement')
    assert(math.type(delta)=='integer' and math.abs(delta)<=8,'movement limit')
    self.x=self.x+delta
    return 'footstep'
end
PlayerEntity:method('input',{runs_on='server'})
PlayerEntity:rpc('input',{callers={'client'},access='owner'})
function PlayerEntity:input(payload)
    local sequence,delta=payload:match('^(%d+):(-?%d+)$')
    sequence=tonumber(sequence); delta=tonumber(delta)
    if not sequence or sequence~=self.processed_input+1 or not delta or math.abs(delta)>8 then self:reject() end
    self:advance(tostring(delta))
    self.processed_input=sequence
end
PlayerEntity:define()
