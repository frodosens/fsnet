-- Bundle shared/player_entity.lua first. Authentication belongs to the host;
-- ctx.subject is trusted, while payload only supplies a validated display name.
PlayerEntity:register()
local online={}
runtime.rpc('login',{callers={'client'},handler=function(ctx,name)
    assert(ctx.subject~='' and #name>0 and #name<=32,'invalid login')
    local previous=online[ctx.subject]
    assert(not previous or previous.peer==ctx.peer,'account already connected')
    local player=PlayerEntity:spawn_or_restore(ctx.subject,{name=name})
    player:show_to(ctx.peer)
    for subject,session in pairs(online) do
        if subject~=ctx.subject then
            player:show_to(session.peer)
            session.player:show_to(ctx.peer)
        end
    end
    player:possess(ctx.peer)
    online[ctx.subject]={peer=ctx.peer,player=player}
    return player.id
end})
runtime.rpc('logout',{callers={'client'},handler=function(ctx)
    local session=online[ctx.subject]
    if not session or session.peer~=ctx.peer then return '' end
    for _,other in pairs(online) do
        session.player:hide_from(other.peer)
        if other~=session then other.player:hide_from(ctx.peer) end
    end
    online[ctx.subject]=nil
    -- Persistent Player state remains available for the next login.
    return ''
end})
