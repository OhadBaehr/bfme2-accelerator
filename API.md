# Public initialization API v1

Build with `build.ps1` (or `-Report`). The output directory contains bfme2_accel.dll,
the standalone GUI launcher, inject.exe, and native regression/API tests.
No Python, pySAGE installation, or game patch is required by the standalone launcher.
An external game loader can use the very same DLL through `src/accel_api.h`.

## Startup

LoadLibrary only attaches the DLL and prepares locks. After it returns, resolve the
undecorated `Bfme2AccelInitialize` export and call it with a writable 32-byte request.
The x86 calling convention is WINAPI/stdcall with one pointer argument. Never call
from DllMain. The console injector and GUI launcher share `src/accel_inject.h`:
load in the target process, resolve the export in that target module, then invoke
initialization and read back its result. A LoadLibrary-only legacy injector must
be updated. Load the DLL once; do not unload it while installed hooks exist.

## Request and result

Eight uint32 fields in order: size (32), version (1), requested, reserved (0),
enabled, skipped, failed, status. Input output-mask values are ignored.
Feature bits: 1 heap, 2 CRT, 4 preshader. Both the standalone launcher and sage-patch
request 7. Unsupported bits and reserved fields are rejected without installation.
Version mismatches return 6. The final three feature masks record installed,
unavailable, and failed requested features; inspect them even when status is READY.
CRT enabled means at least one import slot installed; the log records its slot count.

Status/return: 0 invalid request, 1 READY, 2 BUSY, 3 different configuration already
chosen, 4 unsupported host, 5 installation exception, 6 incompatible version.
The return value remains authoritative when the supplied request memory is invalid.
Concurrent calls return BUSY without waiting; repeat after the first call completes.
Completed requests are cached, including failure: equal requests return the same
result; different requests return 3. Use a fresh process to change the feature set.
Requesting zero features is a valid no-op and still establishes that configuration.
Partial installation is not rolled back. READY alone does not certify all features.
The host check is a coarse BFME-family prerequisite; individual installers still
validate their own imports/signatures. It does not certify arbitrary modified engines.

## Current profile and migration

API v1 exposes the portable heap/CRT/preshader profile only. Renderer and engine-address
optimizations are not available through v1. The legacy full installer remains in source
for future per-feature validation, but DllMain no longer starts it. Environment variables
and marker files cannot expand a public API request. This deliberately changes the default
feature set of the updated standalone launcher as well as external integrations.
Do not combine an older auto-start DLL with the new launcher or a second accelerator DLL.
An old DLL may run its installer as soon as it is loaded; detecting a missing export
cannot reverse that. Use matching DLL and launcher releases.

The renderer test harness explicitly uses its test exports to investigate the renderer;
this does not enable it in the public profile. Native D3D9/shim renderer validation and
multiplayer testing remain open. Earlier companion in-game measurements apply to that
companion, not automatically to this new API DLL.

## Validation

`build.ps1` builds DLL, launcher, injector and tests with /W3 /WX, and runs allocator,
effect-cache, busy-state and public API tests. The API test covers version/feature/reserved
rejection, repeated requests, configuration conflicts, and real remote-thread initialization
in a dedicated child process through the same helper used by both launchers. It also
checks unsupported-host rejection. This is not an in-game performance or multiplayer test.
