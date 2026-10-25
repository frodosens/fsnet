-- Bundle the same two shared files as the server; run with fsnet-client.
local local_circle = MagicCircle:spawn_local({duration=2})
local_circle:advance(1)
assert(local_circle.radius == 2 and local_circle:contains(1, 0))
local_circle:close()

runtime.start(function()
    local circle = MagicCircle:legacy_watch('alice')
    while circle.duration > 0 do
        -- Same query method reads current confirmed properties automatically.
        print('circle', circle.radius, circle.duration, circle:contains(0, 0))
        runtime.sleep(100)
    end
    circle:close()
end)
