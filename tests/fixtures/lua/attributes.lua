-- Embed with EntityScriptHost + AttributeReplication (docs/OBJECT_API.md).
local Entity = fsnet.Entity
local Player = Entity:extend('Player', {owner='legacy_id'})

Player:attr('hp', {type='int32', default=100, server='rw', client='r', sync='observers'})
Player:attr('gold', {type='int64', default=0, server='rw', client='r', sync='owner'})
Player:attr('display_name', {type='string', default='Player', server='rw', client='rw', sync='owner'})
Player:attr('internal_flags', {type='uint32', default=0, server='rw', client='none', sync='none'})
Player:attr('animation', {type='string', default='idle', server='rw', client='r', sync='observers', persist=false})

-- Ordinary methods are local; they are never exported implicitly.
function Player:is_alive()
    return self.hp > 0
end

Player:rpc('damage', {callers={'server'}})
function Player:damage(payload)
    local damage = tonumber(payload)
    if not damage or damage < 0 or damage % 1 ~= 0 then self:reject() end
    self.hp = math.max(0, self.hp - damage)
    -- No business RPC for property synchronization; the framework publishes after CAS.
end

Player:rpc('request_respawn', {callers={'client'}, access='owner'})
function Player:request_respawn()
    if self:is_alive() then self:reject() end
    self.hp = 100
end

function Player:validate_write(proposal)
    return not proposal.display_name or (#proposal.display_name > 0 and #proposal.display_name <= 32)
end

Player:register()
