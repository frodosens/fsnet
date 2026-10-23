-- Bundle shared/structured_entity.lua first; run with fsnet-client.
runtime.start(function()
    local entity=StructuredEntity:watch('alice')

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
    entity:close()
end)
