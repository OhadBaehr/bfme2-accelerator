# BFME2 1.06 engine hooks

This port adds two independently checked optimizations to the executable with `.text` FNV-1a
`32667B9B`, loaded at `0x400000`. Other builds retain their existing behavior. The RotWK engine-hook
flag remains false for BFME2: its skeleton workers, render-list sorting, object-filter memo,
logic spreading, audio limit and mutex replacements still need separate address/ABI validation.

## Equivalence memo

BFME2 `ThingTemplate::isEquivalentTo` at VA `0x73BB04` uses a final-override chain at `+4`,
name at `+0x64`, BuildVariations at `+0x330`, and EquivalentTo at `+0x33c`.
The BFME1 reconstruction confirms the same list-comparison algorithm, but has different addresses
and member offsets. The hook caches the original routine's answer; it does not copy a donor layout.

Entries store the ordered template pair, both full final-override pointers, game-logic pointer and
logic frame. They are reused only in the same logic frame. Changing logic instances or rolling
the frame counter backwards clears the table; frame zero bypasses it. Other threads and calls
before the existing render-thread identification completes use the original function.

The first 512 cache hits are recomputed against stock. Afterwards one in 32 hits is recomputed.
A mismatch permanently bypasses this memo for the session. These checks detect disagreements;
they are not a substitute for gameplay testing, including new-game transitions and mods that
mutate template data during a frame. `AOTR_BFME2_EQ=0` prevents this hook from installing.

## Mesh picking

BFME2 `MeshGeometry::Cast_Ray` at VA `0x56D7B0` tail-jumps to brute-force casting at `0x56CE00`
when the culling tree at `+0x88` is absent. Retail disassembly confirms triangle count at `+0x24`,
index buffer at `+0x2c`, vertex buffer at `+0x30`, shared-buffer data at `+0xc`, 16-bit indices,
and three-float vertices. The collision call receives the ray at test `+0xc` and result at test `+0`.

The port reuses the existing conservative box pretest, computing bounds from the exact indexed
vertices that stock reads, including deformed vertices. The vertex loader now reads exactly three
floats, so the last vertex cannot cause a read past its allocation.

The first 3,000 proposed skips still run stock, requiring a false result and unchanged test/result
bytes. One in 16 remains checked after activation; a violation disables shortcuts for the session.
`AOTR_PICKBOX=0` prevents this BFME2 hook from installing. This port does not enable the RotWK
view-pick answer memo or use its frame counter or scene addresses.

## Evidence and validation

Evidence was read from the Open-BFME-2 workspace without modifying its reconstruction files:

- `Code/GameEngine/Source/Common/Thing/ThingTemplateIsEquivalentTo.cpp`: matched RVA `0x33BB04`, 335 bytes.
- `Code/GameEngine/Source/GameLogic/Object/Update/TurretAI_isWeaponSlotOnTurret.cpp`: logic pointer
  VA `0xDFE78C`, frame `+0x40`; the validation script also checks the retail getter at RVA `0x4D82D6`.
- `Code/Libraries/Source/WWVegas/WW3D2/MeshGeometryReadAABTree.cpp` and retail disassembly of
  `0x56CE00` / `0x56D7B0`: mesh/culling-tree layout and ray-cast ABI.
- `reference/shims/bfme2ray/coltest.h`: ray-test layout, including BFME2's third flag at `+0x42`.
- `reference/open-bfme-1/game/GameEngine/Source/Common/Thing/ThingTemplateIsEquivalentTo.cpp`
  and BFME1 `coltest.h`: algorithm reference; no BFME1 offsets or addresses were installed.

Generate a test from the actual hook code (Python 3; retail checks need `pefile` and `capstone`):

```powershell
& 'C:\Users\vvval\AppData\Local\Programs\Python\Python313\python.exe' src/test_bfme2_hooks.py --output "$env:TEMP\bfme2_hooks_test.cpp" --retail 'path\to\game.dat'
```

From an x86 Visual Studio developer command prompt:

```bat
cl /O2 /MT /EHsc "%TEMP%\bfme2_hooks_test.cpp" /Fe:"%TEMP%\bfme2_hooks_test.exe"
"%TEMP%\bfme2_hooks_test.exe"
```

Tests cover warmup and sampled stock checks, override changes, frame/reset invalidation, deliberate
hash collisions, thread bypass and mismatch shutdown. A protected page immediately after the final
vertex checks that bounds gathering does not overread. Segment tests cover crossing, grazing,
clear misses and non-finite inputs. Retail checks verify the build hash, all four hook signatures,
logic-frame layout, and complete non-relative instructions at both trampoline boundaries.

Both x86 production and diagnostic DLLs built successfully with Visual Studio 2022 Community.
The existing 400,000-operation resource-index differential test also passed. No in-game test or
gameplay speed measurement has been performed; these two hooks should be tested in battle before
distributing a release. Activation and mismatch messages appear in the accelerator log.
