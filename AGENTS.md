# Project direction

- C++ owns the networking/distributed runtime, scene/entity carriers, scheduling,
  storage mechanisms, and script hosting. Do not put game business rules in C++.
- Game business logic belongs in scripts. Target a small common script-host API,
  use Lua for gameplay. See docs/SCRIPTING.md
  for the implemented host contract and adapter limits.
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

See README.md and docs/VALIDATION.md. In the prepared cloud instance, source
`/workspace/fsnet-env/activate-modern`, build with CMake, and run CTest. C++ core,
process crash/restart, and Lua tests should execute in a normal build. Native
sanitizers use a separate build. Do not interpret local ICE/TURN checks as real
cross-NAT production validation.
