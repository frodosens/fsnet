# Entity RPC and attribute schemas

This stage implements a native schema carrier and explicit caller-role policies.
It does **not** yet provide an end-to-end client replication service. The existing
opaque-state interface and legacy RPC registrations still exist for migration.
Do not expose legacy registrations as an authorization boundary.

## RPC policies

Native applications use `Node::bind_rpc(route, policy, contextual_handler)`.
Both caller flags default to false: the application must explicitly permit
clients, servers, or both. `Node::identify` is a trusted identity resolver receiving
the connection handle and verified TLS certificate fingerprint. Without a resolver,
connections have the client role. Only trusted application code can assign a server
role. Resolver rejection and exceptions fail closed. Route authorization configured
with `Node::authorize` remains an additional check.

The contextual handler receives an owned request identity; do not retain a reference
to it after returning. Copy it if asynchronous work needs it. The RPC payload cannot
set this identity. A gateway must preserve the original client identity when forwarding;
forwarding through a server connection is not sufficient authorization for a client
operation. Distributed identity delegation is not implemented in this stage.

Lua persistent methods can declare caller roles:

```lua
runtime.entity('counter', {
    initial = '0',
    methods = {
        read = {
            callers = {'client'},
            handler = function(state, id, payload) return state, state end,
        },
        increment = {
            callers = {'server'},
            handler = function(state, id, payload)
                local next = tostring(tonumber(state) + 1)
                return next, next
            end,
        },
    },
})
```

These policies reach Node through both persistent and routed script-host decorators.
Hosts without policy support reject registration. Empty or unknown caller lists fail
script loading. The original function-only methods and `runtime.rpc` are legacy
registrations, not protected client entry points. Migration to mandatory declarations
is pending. `access='owner'` is explicitly rejected until entity contextual dispatch
is integrated; transport-level policies also reject owner-only declarations rather
than silently ignoring them.

## Native attribute carrier

`EntitySchema` declares named typed attributes, server/client read and write access,
client visibility (`none`, `owner`, `observers`), persistence, and RPC policies.
`EntityAttributes` owns values and monotonic revisions on the runtime thread.

* Integer values are signed 64-bit; numbers are finite doubles; booleans and strings
  are supported. Schemas contain at most 256 properties and 256 methods. Each string
  is bounded to 64 KiB. Unknown names and type mismatches fail closed.
* Client writes require both explicit write permission and the authenticated entity
  owner's subject. Business validation remains the script's responsibility. A write
  request must enter the same entity mailbox as other mutations.
* `stage` validates without changing committed state. Persist the candidate with
  the entity's single-record fenced CAS, then call `publish` only on CAS success.
  `publish` rejects stale revisions and applies a whole update atomically in memory.
* `snapshot(view)` filters all readable properties; `changes(since, view)` coalesces
  changes per property. Private fields never appear in observer updates. A future
  revision requests a full snapshot. A new subscription or changed visibility needs
  a fresh snapshot, not a delta from an unrelated subscription.
* `persistent()` excludes transient fields. `restore()` validates the saved image;
  new carriers retain schema defaults for fields omitted from the saved image.

Native callers are trusted framework code: `Mutation`, `publish`, and `restore` must
not be exposed directly to clients. Client updates must pass `stage` plus script
validation. Values and property revisions must be serialized in the same entity
record before publication; no database transaction is required.

## Remaining integration

The Lua `attrs` declaration, property serialization into persistent entity records,
commit-triggered publication, contextual owner authorization, gateway delegation,
subscription/AOI lifecycle, wire snapshots/deltas, and client SDK application are
not implemented yet. In particular, changing `self.hp` does not currently generate
network traffic. They must be connected before claiming automatic synchronization.
