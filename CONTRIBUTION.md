# Allocator, effect-cache and build corrections

`realloc(p, 0)` on tagged allocations previously requested the ownership-header
size from rpmalloc rather than freeing the block. It now frees and returns NULL,
matching msvcr71. Regression coverage includes zero fill, preserved data on growth,
overflow rejection without losing the old block, and freeing on a zero-size request.

When the effect-cache epoch wraps, clear every entry rather than `sizeof(pointer)`
bytes. The regression seeds the first, middle and last entries before the wrap.
Renderer startup now checks all required allocations and waits successfully for
worker readiness before installing device hooks or activating the worker queue.

Harness floating-point conversions are explicit at the original conversion points;
double-precision expressions are preserved. Output-file creation uses checked
`fopen_s`. The new PowerShell build discovers MSVC, builds rpmalloc from source,
and treats warnings as errors in production and report configurations.

## Scope and follow-up work

The branch now also provides the explicit initialization contract documented in
[API.md](API.md). The shared DLL supports both the standalone launcher and external
loaders without a pySAGE dependency. DllMain no longer installs optimizations.
The public v1 profile contains heap/CRT/preshader only; renderer and engine hooks
require a later validated extension. Original renderer-only validation below is
historical and must not be read as certification of the new API.

## Validation (2026-10-09)

- Production and report builds pass with VS 2022 Community x86, `/W3 /WX`.
- Allocator and full-table epoch-wrap regression tests pass in both builds.
- Existing CRT differential tests report all replacements identical to the supplied
  RotWK msvcr71 runtime.
- Renderer-off harness: 120 frames, zero query/lock/mirror mismatches. The output
  matches the previously recorded native-D3D9 baseline byte-for-byte (SHA-256
  `102bc01ec7c34cfed067144f2096c587124c52a73b0eb5f5e7145e4ce4699a2c`).
- Allocation-failure and readiness-timeout branches were code-reviewed, not fault-injected.
- `git diff --check` passes. This repository has no pre-commit configuration;
  pySAGE's Python hooks are not applicable to this C++ contribution.
