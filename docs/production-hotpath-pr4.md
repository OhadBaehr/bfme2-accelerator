# PR 4: Remove report-only overhead from production hot paths

With `AOTR_PROD`, allocator hooks and the preshader cache still executed report-only atomic updates, and cache misses/verification calls still timed the interpreter. Their reporting threads are absent in that build. Compile out those updates and timer calls using the existing production switch.

Preshader cache hits avoid two report-only atomic increments. Timed interpreter calls avoid two QueryPerformanceCounter calls and a timing accumulation. Allocation/free/foreign-forwarding paths avoid their report counter increments. Reporting builds retain these measurements.

The atomic counter that schedules every 32nd cache-hit verification remains active, as does mismatch-triggered cache shutdown. Cache outputs, interpreter calls/return values, allocation tags, overflow checks, zero-filling, realloc behavior, and foreign-pointer forwarding retain their existing behavior. This change does not resolve the separate dependency/lifetime risks in audit item A5, or the allocator semantics issues in A9.

This implements the allocator/preshader portion of audit item A8. Queue, picking, pose, and other report-only overhead are follow-up candidates; clocks used for watchdogs or scheduling budgets must remain functional. Expected benefit is less CPU overhead in call-heavy scenes. Gameplay FPS impact has not been measured.

## Validation

Synthetic before/after comparison on this machine, using production code, a fixed CPU affinity, and seven one-million-operation samples per helper: the first run's median allocation/free pair fell from 18.98 ns to 12.59 ns, and a one-double cached preshader call fell from 32.33 ns to 28.60 ns. A repeat produced different absolute timings (21.23 to 9.94 ns and 44.58 to 22.92 ns respectively), so these are workload-specific measurements, not stable percentage promises. The test interpreter is trivial; this does not measure real D3DX programs or gameplay. Cache verification remained active in both versions.

For scale, the first run's differences sum to about 0.10 ms if a frame performs 10,000 allocation/free pairs and 10,000 of those cached calls. Actual battle call counts were not measured.

- Built reporting and production DLLs with fresh rpmalloc using x86 MSVC `/O2 /MT /W3`.
- Ran `src/build_production_hotpath_test.bat` in both modes. It includes the shipping implementation and calls the actual allocator/preshader helpers without installing game hooks.
- Verified cache miss, replay, every-32nd-hit recomputation, mismatch fallback, disabled-cache execution, oversized bank bypass, faulting cache input, missing cache, and the existing no-op probe.
- In that workload, the reporting mode makes eight interpreter-timing clock calls; production makes zero. Report counters remain untouched in production and record the expected values in reporting.
- Verified tagged allocation/free, calloc zeroes, realloc data retention, null inputs, arithmetic-overflow rejection, and stock forwarding of known foreign storage.

## Publishing

Intended branch: `optimization/production-hotpath-pr4`, based on local PR 3 revision `f92495f37e6a9c6d68471236697feb8fd6583a15`. Use `main` as the PR base if that revision has merged; otherwise use `optimization/resource-index-pr3` to show only this fourth change. GitHub assigns the actual PR number.

The destination repository is explicitly the user's fork: `vvvalenntinnn-collab/bfme2-accelerator`. PR creation must set that repository as its base repository.

In the implementation session, creating the branch failed because this workspace's `.git` was read-only, and GitHub port 443 access failed with `Bad access`. Code and validation were completed in the existing working tree. A publishing script and standalone patch were prepared in the user's Temp folder; no remote PR was created by this session.
