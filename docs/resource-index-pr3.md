# PR 3: Replace hot resource-tracking searches with a hash index

Branch: `optimization/resource-index-pr3`
Base: `optimization/device-release-fastpath` (PR 2)

Replace the two hot 192-entry exact-resource searches with a 512-bucket index keyed by object pointer. Preserve pending-resource checks, both conservative sequence tables, collision updates, diagnostic counters, and the existing oldest-completed-entry eviction policy. Unlink an evicted entry before reusing its array slot so the index cannot retain stale pointers.

The array and its eviction scan remain unchanged; only exact lookup and collision-update searches use the new index.

## Validation

- Passed 400,000 differential operations using production tracking code from this branch and its PR 2 parent. Covered hash collisions, all-pending saturation, eviction, pointer reuse, sequence wraparound, and frame wraparound; compared tracking state and counters and checked index integrity.
- Built reporting and production DLLs with 32-bit MSVC, `/O2 /MT /W3`.
- Synthetic lookup benchmark: 52.80 ns scanning versus 6.44 ns indexed on this machine. Gameplay performance has not been measured.

## Opening the pull request

Push `optimization/resource-index-pr3` and open it against `optimization/device-release-fastpath` to show only this third change. After PR 2 merges into main, retarget this PR to main. GitHub assigns PR numbers; the local branch name does not reserve #3.

GitHub access was blocked in the restoring session, so the branch and this description were prepared locally without creating a remote PR.
