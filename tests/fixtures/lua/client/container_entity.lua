-- Bundle shared/container_entity.lua first; run with fsnet-client.
runtime.start(function()
    local player=ContainerPlayer:legacy_watch('alice')
    local server=ContainerPlayer:legacy_ref('alice')
    local tasks={}
    local function refresh_tasks()
        local next_tasks={}
        for id,task in pairs(player.tasks) do
            if not tasks[id] then print('task_added',id)
            elseif tasks[id].progress~=task.progress or tasks[id].done~=task.done then print('task_changed',id) end
            next_tasks[id]={progress=task.progress,done=task.done}
        end
        for id in pairs(tasks) do if not next_tasks[id] then print('task_removed',id) end end
        tasks=next_tasks
    end
    -- In a game, run this revision check from the UI update loop. Replication
    -- arrives automatically; no extra request is needed to fetch the containers.
    local function step(action)
        local revision=player.revision
        server:demo(action)
        while player.revision==revision do runtime.sleep(1) end
        refresh_tasks()
    end
    step('grant'); assert(#player.items==1 and player.tags:contains('questing'))
    print('item_added',player.items[1].id,player.items[1].count)
    step('finish'); assert(player.tasks['daily.1'].done)
    print('item_changed',player.items[1].id,player.items[1].count)
    step('remove'); assert(#player.items==0 and #player.tasks==0 and #player.tags==0)
    print('items_empty',#player.items)
    player:close()
end)
