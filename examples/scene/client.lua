-- Replace the camera/effect functions with your engine bindings. The SDK owns
-- networking and confirmed state; this script owns presentation and input rules.
local camera={target=nil}
local function play(effect) assert(effect=='footstep');print('effect',effect) end
local LocalPlayer,RemotePlayer={},{}
local removed=false
function LocalPlayer:on_spawn()
    self.server=Player:ref(self.id)
    print('LocalPlayer',self.id,self.entity.name)
end
function LocalPlayer:on_possess()
    self.prediction=self.entity:prediction({acknowledged=self.entity.processed_input})
    self.next_input=self.entity.processed_input
    camera.target=self.prediction.state
    print('camera',self.id)
end
function LocalPlayer:move(delta)
    assert(self.controlled,'input requires possession')
    local sequence=self.next_input+1
    local payload=tostring(delta)
    -- Initial push returns an effect; reconciliation does not deliver it again.
    play(self.prediction:push(sequence,'advance',payload))
    self.next_input=sequence
    print('predicted',self.prediction.state.position.x,'confirmed',self.entity.position.x)
    self.server:input(sequence..':'..payload,{request_id='scene-input-'..sequence})
    while self.entity.processed_input<sequence do runtime.sleep(1) end
    self.prediction:reconcile(self.entity.processed_input,self.entity.revision)
    assert(self.prediction.state.position.x==self.entity.position.x)
    print('position',self.entity.position.x)
end
function LocalPlayer:on_unpossess()
    camera.target=nil;self.prediction:close();print('unpossess',self.id)
end
function LocalPlayer:on_despawn() removed=true;print('despawn',self.id) end
function RemotePlayer:on_spawn() print('RemotePlayer',self.id,self.entity.name) end
function RemotePlayer:on_despawn() print('remote-despawn',self.id) end
fsnet.entities:register(Player,{owned=LocalPlayer,observed=RemotePlayer})
fsnet.entities:start()
runtime.start(function()
    local id=fsnet.ref(entry_id):call('login','Alice')
    local player
    repeat
        player=fsnet.entities:get('Player',id)
        if not player or not player.controlled then runtime.sleep(1) end
    until player and player.controlled
    assert(player.entity.created==1)
    for _,delta in ipairs({2,3,-1}) do player:move(delta) end
    fsnet.ref(entry_id):call('logout','')
    while not removed do runtime.sleep(1) end
    fsnet.entities:stop()
    print('scene complete')
end)
