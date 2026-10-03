# Device Release without an unconditional queue barrier

Previously every game-thread `IDirect3DDevice9::Release` waited for all recorded work, even a
temporary device reference returned by `GetDevice`. This could repeatedly stop the producer and
worker from running in parallel.

The new hand-written hook retains a temporary reference, consumes the caller's reference under
the execution lock, and keeps the reservation until the next queue drain. Once that drain has
finished, the game thread retires the reservations. The worker never retires these device references.
At most 64 reservations accumulate before the next release forces a drain.

The decision excludes existing reservations from the observed reference count. A final or uncertain
release still waits. Pending queued resource-release records also force the old barrier, because
their destruction could reduce device ownership. Each generated resource-release record now has
flag 2; `rtCommit` tracks the newest such record with the existing wrap-safe sequence comparison.
Incomplete resource-class hooking (per-record object-reference mode), other device pointers,
inactive rendering, foreign threads and existing direct scopes retain their prior behavior.

If other resource destruction makes reservation retirement final, destruction still occurs on the
game thread after the queue drain. The reference count returned by the shortcut excludes reservations
but remains a diagnostic value: it cannot predict native ownership changes in queued commands.
Microsoft documents [COM reference-count returns as test information](https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nf-unknwn-iunknown-release)
and the [special threading requirements around D3D9 final device Release](https://learn.microsoft.com/en-us/windows/win32/direct3d9/multithreading-issues).

Set `AOTR_DEVICE_RELEASE_FAST=0` to retain the original device-release behavior. This optimization
is portable across the accelerator's supported engine builds and does not enable any additional
BFME2 or RotWK address hooks. Diagnostic builds report cumulative shortcut and retirement counts.

## Validation

`src/device_release_test.cpp` includes the production helper and uses an instrumented COM device
and a simulated queue drain. It checks that destruction happens only after pending work finishes
and on the creator thread. Coverage includes:

- Repeated non-final releases followed by final release, with reservation counts excluded correctly.
- Reservation retirement at a later synchronous drain.
- Pending resource destruction, including sequence-number wraparound.
- Per-record reference mode, disabling the shortcut, inactive rendering and direct scopes.
- An actual foreign-thread Release while a reservation exists.
- The 64-reference cap and reservation retirement becoming final after resource destruction.

Build and run in an x86 Visual Studio developer command prompt:

```bat
cd src
cl /O2 /MT /EHsc device_release_test.cpp
device_release_test.exe
```

In the fixture, 64 eligible releases use zero barriers; the 65th forces one and retires all 64
reservations. This corroborates the barrier reduction, not a gameplay speedup. The lifetime tests,
existing resource-index differential test and both DLL builds passed. No in-game FPS benchmark
or driver-level rendering comparison has been completed for this change. The rendering harness
was built and attempted with the shortcut disabled, but native D3D9 `CreateDevice` returned
`0x8876086C` before acceleration was installed. That run provides no rendering or timing evidence.

The rendering harness now performs eight paired effect `GetDevice` / device `Release` calls per
frame and reports device-release mismatches and shortcut counts. Once device creation works, run
the same scene with `AOTR_DEVICE_RELEASE_FAST=0` and `=1` and compare frame hashes and mismatch
counts; timings in this synthetic scene are not a BFME2 gameplay benchmark.

`aotr_rt_gen.inc` is generated: make generator edits in `gen_rt_hooks.py` and regenerate with Python.
