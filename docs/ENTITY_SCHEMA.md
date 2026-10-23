# Entity RPC and attribute schemas

This stage implements Lua attribute entities, a native schema carrier, post-commit
attribute events and explicit caller-role policies.
It includes a local owner-node replication service and a native client projection manager. The existing
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

The contextual handler receives a request-scoped identity; do not retain a reference
to it after returning. Copy it if asynchronous work needs it. The RPC payload cannot
set this identity. A gateway must preserve the original client identity when forwarding;
forwarding through a server connection is not sufficient authorization for a client
operation. Distributed identity delegation is not implemented in this stage.

Stateless RPCs can use a declaration with `handler(ctx, payload)`:

```lua
runtime.rpc('ping', {
    callers = {'client'},
    handler = function(ctx, payload) return payload end,
})
```

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
is pending. Persistent methods now support `access='owner'`. For attribute entities, declare `owner='id'` to use the authenticated account
subject as the entity ID entirely from Lua. Otherwise configure
`EntityScriptHost::ownership` with a trusted `(type, id) -> account subject` mapping;
an absent, empty, or throwing mapping denies access. Entity IDs are not implicitly
account IDs without that declaration. External ownership mappings must remain stable
during an operation; serialize mapping changes with entity deactivation/lifecycle.
They are not a second transactional ownership record. The owner check executes before state acquisition and before receipt
lookup. `ctx` is passed as a fourth argument to persistent handlers:
`handler(state, id, payload, ctx)`, containing `caller`, `subject`, and transport-local
`peer`. Each queued task owns its context across awaits; modifying a Lua context table
does not change transport authentication. Owner-only transport routes still reject
registration because a plain transport route has no entity ownership information.

Retry receipts for contextual entities are scoped by caller role and authenticated
subject. Anonymous calls additionally use the connection and process incarnation;
their receipts intentionally do not survive reconnects. Two different principals
cannot reuse a request ID to replay one another's response. Identical subject values
must represent the same security principal within the application's identity domain.
Anonymous receipt keys have a separate domain, so an authenticated subject whose name
resembles an anonymous connection identifier cannot collide with its receipt.


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

## Lua attributes and durable publication

```lua
runtime.entity('Player', {
    attrs = {
        hp = {type='int32', default=100, server='rw', client='r', sync='observers'},
        gold = {type='int64', default=0, server='rw', client='r', sync='owner'},
        temporary = {type='string', default='', persist=false, sync='owner'},
        secret = {type='string', default='', client='none', sync='none'},
    },
    methods = {
        damage = {callers={'server'}, handler=function(self, ctx, amount)
            self.hp = math.max(0, self.hp - tonumber(amount))
            return tostring(self.hp)
        end},
    },
})
```

Attribute handlers receive `(self, ctx, payload)` and return response bytes. They
require explicit caller declarations and cannot also declare opaque `initial` state.
Supported types are `int32`, `uint32`, `int64`, `number`/`double`, `bool`/`boolean`, and
`string`. Integer ranges and strict Lua value types are enforced. The native userdata
checks server read/write access and accounts native carrier memory against the Lua
memory budget. Undeclared properties fail closed. `tick` receives the same attribute
object and runs as an internal server operation.

The FSA1 binary codec stores typed values and a monotonic property revision. Persistent
properties and their revision are serialized into the existing single FER1 record;
transient properties live in a bounded runtime cache. They survive successive operations
while the entity remains owned and resident, and reset on reactivation or failover.
Transient cache growth is reserved before committing and released on failure or eviction.
It has a separate `resident_bytes` budget, reported as `stats().transient_bytes`.

`EntityScriptHost::on_attributes` receives old/new full images only after successful
storage CAS and only when properties changed. Failed scripts, stale ownership, failed
writes, and receipt replays emit no change event. Listeners run on the runtime thread;
their exceptions increment `publication_failures` and cannot undo a committed state.
Events contain private properties for trusted framework filtering and must never be
forwarded wholesale to clients. This callback is currently single-listener.

`encode_attributes`/`decode_attributes` provide a bounded FSA1 wire format, reject
truncation, duplicates, non-finite numbers and trailing bytes. `AttributeReplica` is the
native client projection: the first update must be a snapshot, deltas must bridge the
current revision, late updates cannot rewind it, and replacement snapshots remove
properties no longer visible. A new entity incarnation/subscription must reset its
projection before applying its first snapshot.

## Network replication

Applications wire `AttributeReplication(node, entity_host, observer_hook)` after loading
attribute entities. It owns the host's single change listener. The trusted observer hook
answers whether a subject can observe an entity; ownership is resolved separately by
`EntityScriptHost::ownership`. No observer hook means owner-only subscriptions.
Use authenticated, stable account subjects through `Node::identify`; default TLS
fingerprints are connection identities, not a game login system.

On the client, construct `AttributeClient(node, server_peer, changed_callback)` and
call `watch(type, id, ready_callback)`. `get(subscription)` provides the local
`AttributeReplica`. Both runtimes execute on their owner threads. Poll the server
replication service and client manager as well as Node, EntityScriptHost and LuaEngine.
The Node and host must outlive the corresponding managers.

The initial control RPC returns a filtered FSA1 snapshot in an FSN1 subscription
envelope. After a successful entity commit, recipient-specific deltas travel in
Node's **one-way notification frames** (kind 3), without request IDs, replies or
pending RPC slots. Business code only assigns `self.hp`. Repeated receipt responses
do not emit another update. Control RPCs watch/unwatch/renew explicitly permit client
callers. All synchronization packets are bounded by the transport frame/queue limits.

The client accepts updates only from its configured server connection. Type, entity
ID, subscription ID, and revision must match. Unrelated peers cannot inject changes.
A revision gap invalidates that subscription and reports rejection; the application
must obtain a fresh snapshot. On backpressure, the server closes the connection so
the application can reconnect and resubscribe instead of silently retaining a gap.

The server rechecks the current connection principal, logical owner and observer
permission before delivery and during bounded polling. Identity rebinding, AOI or
ownership changes, deactivation, disconnection, expiration and schema removal revoke
subscriptions. Permission changes discovered during polling require a fresh watch;
clients clear revoked projections. This cannot erase information a client previously
received legitimately. Entity handles prevent a transient property cache from being
silently reused across deactivation/reactivation.

Defaults are 1,024 server subscriptions, 32 per connection, and 16 MiB of cached
projection images. Pending watches expire after 5 seconds; active leases last 30 seconds
and are renewed by the client manager at one third of the advertised lease. Server
configuration accepts leases of at least 3 seconds. Individual initial projections
are limited to 512 KiB. The native client permits at most 256 subscriptions plus
pending watches and 16 MiB of projection data. Replica updates cannot accumulate more
than 256 named properties or exceed the FSA1 record limit. Subscriber lookup is indexed
by entity, so a commit visits only interested subscribers.

## Client property proposals

Declare `client='rw'` only for properties a client is allowed to propose changing.
`AttributeClient::write(subscription, values, written_callback, options)` submits
those values through an explicitly client-only control RPC. The server verifies the
subscription and current subject, requires logical ownership (observation is not
write permission), then queues the operation in the same entity mailbox. Before
calling Lua it validates names, types, ranges, write permissions and the expected
revision. Server-authoritative properties such as `hp` cannot be overwritten by a
client. After acceptance, normal single-record CAS and filtered notifications apply.
There is no client-side optimistic mutation before acceptance.

An optional Lua `validate_write(self, ctx, proposal)` hook can apply business rules;
it must return true to accept. Treat it as a read-only validator. The framework
checks permissions again when applying the original proposal, so changing its Lua
copy does not widen the accepted write. `runtime.reject()` aborts an RPC with
`Status::rejected`; rejection remains effective even if caught with `pcall`. Failed
validation, scripts or CAS publish no changes. External effects performed by a
validator are not rolled back, so validators should avoid them.

Every proposal contains an expected property revision. Concurrent proposals based
on the same revision cannot silently overwrite one another: after one changes state,
the next is rejected. Owners receive empty revision deltas even when only a
server-private property changes, allowing their next proposal to use the current
revision without exposing private values. Observers may skip invisible revisions.

The SDK generates a request ID when none is supplied. To retry an ambiguous result,
supply the **same** `AttributeWriteOptions.expected_revision`, `request_id` and values.
Successful receipt replay returns without another property event. Receipt retention
remains bounded; this is not unlimited exactly-once delivery. The native client
bounds pending write callbacks to 256 and reports disconnection during shutdown.

A complete script example is [attributes.lua](../examples/lua/attributes.lua).
The `fsnet-script` binary installs AttributeReplication; production embeddings still
provide trusted authentication. Lua client carriers use ClientScriptHost and the
`fsnet-client` binary; see [shared entities](SHARED_ENTITIES.md).

## Remaining integration

Gateway identity delegation, mandatory migration of legacy RPC
registrations, automatic scene/AOI subscription discovery, and subscription migration
between owner nodes remain pending. The current native client connects directly to the
entity owner endpoint; a gateway does not yet proxy these streams. Reconnect requires
application connection establishment followed by a fresh watch. Browser/engine-specific
client adapters are not implemented.
