-- Bundle this class before either host entry point.
PredictedActor=fsnet.Entity:extend('PredictedActor',{owner='legacy_id'})
PredictedActor:attr('x',{type='int32',default=0,authority='driver',sync='owner'})
PredictedActor:attr('processed_input',{type='int64',default=0,sync='owner'})
function PredictedActor:advance(payload)
    local delta=assert(tonumber(payload),'invalid movement')
    assert(math.type(delta)=='integer' and math.abs(delta)<=8,'movement limit')
    self.x=self.x+delta
    return 'footstep'
end
PredictedActor:method('input',{runs_on='server'})
PredictedActor:rpc('input',{callers={'client'},access='owner'})
function PredictedActor:input(payload)
    local sequence,delta=payload:match('^(%d+):(-?%d+)$')
    sequence=tonumber(sequence); delta=tonumber(delta)
    if not sequence or sequence~=self.processed_input+1 or not delta or math.abs(delta)>8 then self:reject() end
    -- A sample authoritative input limit deliberately causes client correction.
    self:advance(tostring(math.max(-4,math.min(4,delta))))
    self.processed_input=sequence
end
PredictedActor:define()
