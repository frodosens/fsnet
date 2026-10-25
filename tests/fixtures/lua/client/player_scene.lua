-- Bundle shared/player_entity.lua first; run with fsnet-client.
-- No graphical engine is required: camera/model integration is shown with fields.
local LocalPlayer = {}
local RemotePlayer = {}
local removed = false

-- The framework calls this after receiving the server's initial attributes.
function LocalPlayer:on_spawn()
    self.server = PlayerEntity:legacy_ref(self.id)
    print('LocalPlayer', self.id, self.entity.name)
end

function LocalPlayer:on_possess()
    self.prediction = self.entity:prediction({
        acknowledged = self.entity.processed_input,
    })
    self.next_input = self.entity.processed_input
    self.camera_target = self.entity -- Replace with your engine's camera binding.
    print('camera', self.id)
end

-- Application method, called from an async task. This demo waits for each move
-- before submitting the next one; it does not implement a real-time input loop.
function LocalPlayer:move(delta)
    assert(self.controlled, 'player is not controlled')
    assert(math.type(delta) == 'integer' and math.abs(delta) <= 8, 'movement limit')
    local sequence = self.next_input + 1
    local payload = tostring(delta)

    -- 1. Run the shared movement rule immediately on independent local state.
    local effect = self.prediction:push(sequence, 'advance', payload)
    self.next_input = sequence
    -- Play effect here, outside the replayable movement rule.
    assert(effect == 'footstep')
    print('predicted', self.prediction.state.x, 'confirmed', self.entity.x)

    -- 2. Send the input, not the predicted position, to the authoritative entity.
    -- On an ambiguous failure, retry this exact input and request ID rather than
    -- treating it as a new move. This small demo exits on RPC errors.
    self.server:input(tostring(sequence) .. ':' .. payload, {
        request_id = 'scene-demo-' .. sequence,
    })

    -- 3. The server publishes state and its cumulative input acknowledgement.
    while self.entity.processed_input < sequence do runtime.sleep(1) end
    self.prediction:reconcile(self.entity.processed_input, self.entity.revision)
    assert(self.prediction.state.x == self.entity.x)
    print('position', self.entity.x)
end

function LocalPlayer:on_unpossess()
    self.camera_target = nil
    self.prediction:close()
    print('unpossess', self.id)
end

function LocalPlayer:on_despawn()
    removed = true
    print('despawn', self.id)
end

function RemotePlayer:on_spawn()
    -- Use self.entity.x for remote rendering/interpolation, without local input.
    print('RemotePlayer', self.id, self.entity.name)
end

function RemotePlayer:on_despawn() print('remote-despawn', self.id) end

-- Register before login so server-created replicas can be handled immediately.
fsnet.entities:register(PlayerEntity, {
    owned = LocalPlayer,
    observed = RemotePlayer,
})
fsnet.entities:start()

runtime.start(function()
    -- The host already authenticated the connection. 'Alice' is a display name;
    -- the server derives the account/entity ID from its trusted login context.
    local id = runtime.legacy_server_request('login', 'Alice')

    -- No watch/spawn on the client: the server's presence stream creates it.
    local player
    repeat
        player = fsnet.entities:get('Player', id)
        if not player or not player.controlled then runtime.sleep(1) end
    until player and player.controlled

    -- Simulate three local movement inputs: x goes 0 -> 2 -> 5 -> 4.
    player:move(2)
    player:move(3)
    player:move(-1)

    runtime.legacy_server_request('logout', '')
    while not removed do runtime.sleep(1) end
    fsnet.entities:stop()
end)
