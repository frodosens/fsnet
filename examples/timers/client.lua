-- The launcher supplies demo_id and wait_for_completion as trusted connection
-- configuration. The client only calls an entity; it never schedules timers.
runtime.start(function()
    local demo=fsnet.ref(demo_id,{timeout_ms=3000})
    local expected='1:3:3:1:0:job-42'
    local previous
    for attempt=1,100 do
        local state=demo:call('status','')
        if state~=previous then
            print('state delayed:periodic:catch_up:absolute:cancelled:job = '..state)
            previous=state
        end
        if not wait_for_completion then
            assert(state=='0:0:0:0:0:pending','tasks fired before the crash demonstration')
            return
        end
        if state==expected then
            -- Stay connected past another period: completed/cancelled tasks
            -- must not change the saved result.
            runtime.sleep(600)
            assert(demo:call('status','')==expected,'completed task ran again')
            print('timers complete')
            return
        end
        runtime.sleep(100)
    end
    error('timer recovery timed out: '..tostring(previous))
end)
