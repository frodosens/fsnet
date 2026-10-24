# Project direction

- C++ owns the networking/distributed runtime, scene/entity carriers, scheduling,
  storage mechanisms, and script hosting. Do not put game business rules in C++.
- Game business logic belongs in scripts. Target a small common script-host API,
  use Lua for gameplay. See docs/SCRIPTING.md
  for the implemented host contract and adapter limits.
- Design new mechanisms for reuse by both client and server. Check existing
  engine code before adding implementations; share algorithms, value models,
  codecs, patch application, and other common runtime mechanisms instead of
  maintaining parallel client/server versions. Keep authentication, authority,
  validation policy, persistence/commit decisions, and platform adapters at the
  appropriate boundary. Verify the shared path from both sides and document any
  necessary differences; do not move gameplay rules into the shared C++ engine.
- Native protocol/transport test handlers and benchmarks are framework fixtures,
  not a reason to add native player/combat/economy services.
- Legacy C code, SDK, examples, and binary archives were removed from
  mainline. The original implementation remains in Git history; do not restore it
  as a runtime dependency.

# Workflow

- Use the existing checkout; cloud tasks are isolated. Do not create a worktree
  unless the user asks for one.
- Work on the refactor branch, not master. After each coherent stage, run relevant
  checks, commit the stage, and push its branch. The user explicitly requested this
  workflow. Do not force-push or rewrite published commits without authorization.
- Do not commit secrets, build outputs, downloaded dependency trees, or local
  environment directories. Application dependencies are pinned in CMake.
- Document what is implemented separately from plans and production claims.

# Validation

See README.md, docs/DEV_ENVIRONMENT.md and docs/VALIDATION.md. Bootstrap with
`bash tools/dev/bootstrap.sh`, source `tools/dev/activate.sh`, then configure/build/test
with the checked-in CMake presets. Local tools and outputs live in ignored `.dev/`.
C++ core, process crash/restart, and Lua tests should execute in a normal build. Native
sanitizers use a separate build. Do not interpret local ICE/TURN checks as real
cross-NAT production validation.
