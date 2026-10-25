-- Bundle shared/predicted_actor.lua first; run with fsnet-client.
runtime.start(function()
    local confirmed=PredictedActor:legacy_watch('alice')
    local first=confirmed.processed_input+1
    local prediction=confirmed:prediction({acknowledged=first-1})
    local effects=0
    for sequence=first,first+1 do
        assert(prediction:push(sequence,'advance','5')=='footstep')
        effects=effects+1 -- Play the effect outside the replayable method.
    end
    local initial_x=confirmed.x
    assert(prediction.state.x==initial_x+10)
    local server=PredictedActor:legacy_ref('alice')
    for sequence=first,first+1 do
        server:input(tostring(sequence)..':5',{request_id='demo-input-'..sequence})
        while confirmed.processed_input<sequence do runtime.sleep(1) end
        prediction:reconcile(confirmed.processed_input,confirmed.revision)
        local expected=sequence==first and initial_x+9 or initial_x+8
        assert(prediction.state.x==expected and effects==2)
    end
    assert(prediction.pending==0 and prediction.state.x==confirmed.x)
    print('prediction',confirmed.x,effects)
    prediction:close(); confirmed:close()
end)
