-- Loaded on both node-a and node-b. Authentication supplies context.subject;
-- this example demonstrates entity orchestration, not a credential database.
Player:register()
local LoginEntity=fsnet.Entity:extend('LoginEntity')
LoginEntity:attr('home_node',{type='string',default=''})
LoginEntity:rpc('login',{callers={'client'}})
function LoginEntity:login(name)
    local ctx=self.context
    if ctx.subject=='' or #name==0 or #name>32 then self:reject() end
    local player=Player:spawn({name=name},{
        creation_key='player:'..ctx.subject,
        owner=ctx.connection,
    })
    -- Announce first so Gateway can preserve this projection during handoff.
    player:show_to(ctx.connection)
    player:possess(ctx.connection)
    if self.home_node~='node-b' then
        local status=Player:transfer(player.id,'node-b')
        assert(status==runtime.status.ok,'retry handoff to the same destination')
    end
    return player.id
end
LoginEntity:register()
Manager=fsnet.Entity:extend('Manager')
Manager:attr('logins',{type=fsnet.list('string',16),default={}})
function Manager:on_create()
    for i,node in ipairs({'node-a','node-b'}) do
        local entry=LoginEntity:spawn({home_node=node},{
            target_node=node,creation_key='login-'..i,persistence='rebuildable',
        })
        self.logins:add(entry.id)
    end
end
Manager:rpc('discover',{callers={'client'}})
function Manager:discover()
    assert(#self.logins>0,'login pool is empty')
    return self.logins[math.random(#self.logins)]
end
Manager:register()
-- A trusted bootstrapper creates/retrieves Manager with a stable creation_key,
-- then supplies its ID as connection configuration. All subsequent business
-- requests are entity RPCs. An Account actor can coordinate one-player-per-
-- account across login actors; this example does not impose that game policy.
