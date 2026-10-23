-- Embed with EntityScriptHost + AttributeReplication (docs/ENTITY_SCHEMA.md).
-- Authentication supplies an account subject; this example uses that subject as
-- the entity ID. No account/entity mapping or game rules need to be written in C++.
runtime.entity('Player', {
    owner = 'id',
    attrs = {
        hp = {type='int32', default=100, server='rw', client='r', sync='observers'},
        gold = {type='int64', default=0, server='rw', client='r', sync='owner'},
        display_name = {type='string', default='Player', server='rw', client='rw', sync='owner'},
        internal_flags = {type='uint32', default=0, server='rw', client='none', sync='none'},
        animation = {type='string', default='idle', server='rw', client='r', sync='observers', persist=false},
    },
    validate_write = function(self, ctx, proposal)
        if proposal.display_name then
            return #proposal.display_name > 0 and #proposal.display_name <= 32
        end
        return true
    end,
    methods = {
        damage = {
            callers = {'server'},
            handler = function(self, ctx, payload)
                local damage = tonumber(payload)
                if not damage or damage < 0 or damage % 1 ~= 0 then runtime.reject() end
                self.hp = math.max(0, self.hp - damage)
                return '' -- property changes are automatically replicated after CAS
            end,
        },
        request_respawn = {
            callers = {'client'}, access = 'owner',
            handler = function(self, ctx, payload)
                if self.hp ~= 0 then runtime.reject() end
                self.hp = 100
                return ''
            end,
        },
    },
})
