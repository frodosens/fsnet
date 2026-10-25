Profile:register()
local ProfileHub=fsnet.Entity:extend('ProfileHub')
ProfileHub:rpc('open',{callers={'client'}})
function ProfileHub:open()
    local ctx=self.context
    assert(ctx.subject~='','authenticated subject required')
    local profile=Profile:spawn({},{creation_key='profile:'..ctx.subject})
    profile:bind_owner(ctx.connection)
    profile:show_to(ctx.connection)
    return profile.id
end
ProfileHub:register()
runtime.start(function()
    local hub=ProfileHub:spawn({},{creation_key='profiles',bootstrap_scope='examples'})
    print('entry '..hub.id)
end)
