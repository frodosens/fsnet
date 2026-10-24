Profile:register()
runtime.rpc('open_profile',{callers={'client'},handler=function(ctx)
    assert(ctx.subject~='','authenticated subject required')
    local profile=Profile:spawn_or_restore(ctx.subject)
    profile:show_to(ctx.peer)
    return profile.id
end})
