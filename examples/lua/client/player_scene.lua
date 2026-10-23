-- Bundle shared/player_entity.lua first. Replace model/camera/input operations
-- in these ordinary Lua prototypes with your engine adapter.
local LocalPlayer={}
local RemotePlayer={}
local removed=false
function LocalPlayer:on_spawn()
    print('LocalPlayer',self.id,self.entity.name)
end
function LocalPlayer:on_possess()
    self.prediction=self.entity:prediction({acknowledged=self.entity.processed_input})
    self.camera_target=self.entity -- camera:follow(model) in a graphical client
    print('camera',self.id)
end
function LocalPlayer:on_unpossess()
    self.camera_target=nil
    self.prediction:close()
    print('unpossess',self.id)
end
function LocalPlayer:on_despawn()
    removed=true
    print('despawn',self.id)
end
function RemotePlayer:on_spawn()
    -- Read self.entity.x for interpolation; do not bind local input or camera.
    print('RemotePlayer',self.id,self.entity.name)
end
function RemotePlayer:on_despawn() print('remote-despawn',self.id) end
fsnet.entities:register(PlayerEntity,{owned=LocalPlayer,observed=RemotePlayer})
fsnet.entities:start()
runtime.start(function()
    local id=runtime.server_request('login','Alice')
    local player
    repeat player=fsnet.entities:get('Player',id); if not player or not player.controlled then runtime.sleep(1) end
    until player and player.controlled
    local sequence=player.entity.processed_input+1
    assert(player.prediction:push(sequence,'advance','2')=='footstep')
    PlayerEntity:ref(id):input(tostring(sequence)..':2',{request_id='scene-demo-'..sequence})
    while player.entity.processed_input<sequence do runtime.sleep(1) end
    player.prediction:reconcile(player.entity.processed_input,player.entity.revision)
    assert(player.prediction.state.x==player.entity.x)
    print('position',player.entity.x)
    runtime.server_request('logout','')
    while not removed do runtime.sleep(1) end
    fsnet.entities:stop()
end)
