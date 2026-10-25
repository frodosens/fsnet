-- Account-to-player mapping is Lua policy; IDs are allocated by fsnet.
Player:register()
local online={}
local SceneHub=fsnet.Entity:extend('SceneHub')
SceneHub:rpc('login',{callers={'client'}})
function SceneHub:login(name)
    local ctx=self.context
    assert(ctx.subject~='' and #name>0 and #name<=32,'invalid login')
    local player=Player:spawn({},{creation_key='player:'..ctx.subject})
    player:bind_owner(ctx.connection)
    player:configure(name)
    player:show_to(ctx.connection)
    for subject,session in pairs(online) do
        if subject~=ctx.subject then
            local live=pcall(function()
                player:show_to(session.connection)
                session.player:show_to(ctx.connection)
            end)
            if not live then online[subject]=nil end
        end
    end
    player:possess(ctx.connection)
    online[ctx.subject]={connection=ctx.connection,player=player}
    return player.id
end
SceneHub:rpc('logout',{callers={'client'}})
function SceneHub:logout()
    local ctx=self.context
    local session=online[ctx.subject]
    if not session or session.connection.id~=ctx.connection.id or session.connection.generation~=ctx.connection.generation then return '' end
    for subject,other in pairs(online) do
        pcall(function() session.player:hide_from(other.connection) end)
        if subject~=ctx.subject then other.player:hide_from(ctx.connection) end
    end
    session.player:bind_owner(nil)
    online[ctx.subject]=nil
    return ''
end
SceneHub:register()
runtime.start(function()
    local hub=SceneHub:spawn({},{creation_key='scene',bootstrap_scope='examples'})
    print('entry '..hub.id)
end)
