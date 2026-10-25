-- Bundle shared/structured_entity.lua first; run with fsnet-client.
runtime.start(function()
    local entity=StructuredEntity:legacy_watch('alice')

    -- Both forms automatically submit a proposal and await server validation.
    -- The server commits before broadcasting to other authorized observers.
    entity.position={x=1,y=2,z=3}
    entity.position.x=10
    print('position',entity.position.x,entity.position.y,entity.position.z)

    -- Structured fields and fixed arrays can nest.
    entity.matrix[1]={1,0,0,0}
    entity.matrix[1][4]=20
    entity.pose.position.y=5
    print('matrix',entity.matrix[1][4])
    print('pose',entity.pose.position.y)

    local ok=pcall(function() entity.position.x=101 end)
    assert(not ok and entity.position.x==10) -- rejected by server validation
    print('rejected',entity.position.x)
    -- Four setters, one proposal/validation/commit. Confirmed values stay
    -- unchanged until the batch callback finishes and the server accepts it.
    entity:batch(function(draft)
        draft.position.x=11
        draft.position.y=4
        draft.matrix[1][4]=21
        draft.pose.position.y=6
    end)
    print('batch',entity.position.x,entity.position.y,entity.matrix[1][4],entity.pose.position.y)
    entity:close()
end)
