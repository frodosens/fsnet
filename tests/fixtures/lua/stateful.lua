-- All state interpretation and business rules live in Lua. The runtime stores
-- opaque bytes, serializes each ID and commits before acknowledging a request.
runtime.entity('counter', {
    initial = '0',
    methods = {
        add = function(state, id, payload)
            local next_state = tostring(tonumber(state) + assert(tonumber(payload)))
            return next_state, next_state
        end,
        get = function(state) return state, state end,
    },
    -- Optional serialized scene/entity update. Elapsed is a millisecond string.
    -- tick = function(state, id, elapsed) return state, '' end,
})
