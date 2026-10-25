-- Example business stays entirely in Lua. In-memory only, not a durable player model.
local visits = {}

runtime.legacy_rpc('player.visit', function(player_id)
    assert(#player_id > 0 and #player_id <= 128, 'invalid player id')
    visits[player_id] = (visits[player_id] or 0) + 1
    return tostring(visits[player_id])
end)

runtime.legacy_rpc('player.visits', function(player_id)
    return tostring(visits[player_id] or 0)
end)
