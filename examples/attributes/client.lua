local View={}
function View:on_spawn() print('profile',self.id) end
fsnet.entities:register(Profile,{owned=View,observed={}})
fsnet.entities:start()
runtime.start(function()
    local id=runtime.server_request('open_profile','')
    while not fsnet.entities:get('Profile',id) do runtime.sleep(1) end
    local entity=fsnet.entities:get('Profile',id).entity
    entity.position={x=1,y=2,z=3}
    entity.position.x=4
    entity:batch(function(draft)
        draft.position.y=5
        draft.transform[1][4]=6
        draft.pose.origin.z=7
        draft.pose.label='batched'
    end)
    assert(entity.position.x==4 and entity.position.y==5)
    assert(entity.transform[1][4]==6 and entity.pose.origin.z==7)
    assert(not pcall(function() entity.position.x=101 end))
    assert(entity.position.x==4)
    local server=Profile:ref(id)
    server:sample('add')
    assert(entity.items[1].count==1 and entity.tasks['daily.1'].progress==0 and entity.tags:contains('questing'))
    server:sample('finish')
    assert(entity.items[1].count==2 and entity.tasks['daily.1'].done)
    -- Read-only inventory is never granted by client-side assignment.
    assert(not pcall(function() entity.items:add({id='cheat',count=99}) end))
    server:sample('remove')
    assert(#entity.items==0 and not entity.tasks['daily.1'] and not entity.tags:contains('questing'))
    fsnet.entities:stop()
    print('attributes complete')
end)
