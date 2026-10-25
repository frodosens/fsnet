local LocalPlayer,RemotePlayer={},{}
function LocalPlayer:on_spawn() print('LocalPlayer',self.id) end
function LocalPlayer:on_possess() print('camera follow',self.id) end
function RemotePlayer:on_spawn() print('RemotePlayer',self.id) end
fsnet.entities:register(Player,{owned=LocalPlayer,observed=RemotePlayer})
fsnet.entities:start()
-- Trusted connection configuration supplies manager_id (a persisted EntityId).
function login(manager_id,name)
    local login_id=fsnet.ref(manager_id):call('discover','')
    local player_id=fsnet.ref(login_id):call('login',name,{request_id='login-1'})
    -- Store player_id in a profile, task or another system, then reconstruct
    -- the reference later with fsnet.ref(player_id). No physical Node is saved.
    return player_id
end
