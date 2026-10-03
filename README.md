# BFME2 Accelerator

An in-process accelerator for *The Lord of the Rings: The Battle for Middle-earth II* and *The Rise of the
Witch-king* (EA SAGE engine, 2006), and for the mods built on them. It is a 32-bit DLL that a small loader
injects into `game.dat` at startup.

The engine runs its simulation and its Direct3D calls on a single thread. This moves the D3D work onto a
second thread and replaces several hot engine routines with versions that return the same bytes.

Instructions for playing with it are in [packaging/README.txt](packaging/README.txt), the file that ships
beside the binaries. The rest of this page is about the source.

Nothing is written into the game folder and `game.dat` on disk is never modified, so mod launchers and their
checksums still pass. Portable hooks use signatures, vtables and named imports. Engine hooks use addresses
for an identified executable build and verify instruction bytes before patching. Unrecognised engine
builds receive only the portable hooks.

## What it changes

One include per piece under `src/`. Most can be switched off at runtime with an `AOTR_*` environment
variable.

- `aotr_rt.inc` and `aotr_rt_gen.inc` hold the render thread. Main-thread D3D9 and D3DX calls are recorded
  into a queue and replayed on a second thread.
- `aotr_rt_release.inc` avoids device-release queue barriers where lifetime tracking permits it;
  see [ownership rules and tests](docs/device-release.md).
- `aotr_fastcrt.inc` replaces the hot `msvcr71` imports with SSE versions that return identical bytes.
- `aotr_rlsort.inc` and `aotr_rlsort_algo.h` run the mesh render list sort without the reference-count traffic.
- `aotr_logicspread.inc` leaves the engine's six logic calls per step in their stock order, each running
  exactly once. What moves is where the render frames fall inside that sequence, so one frame in six stops
  being the hitch.
- `aotr_mutexcs.inc` puts the engine's `MutexClass` on a user-mode critical section instead of a kernel mutex.
- `aotr_drawgen.inc` generates the per-draw effect parameter writes rather than walking them.
- `aotr_posewarm2.inc` updates skeleton trees on worker threads.
- `aotr_equivmemo.inc` memoises `ThingTemplate` equivalence.
- `aotr_bfme2.inc` independently ports equivalence caching and the mesh-picking box pretest to verified
  BFME2 1.06. See [supported hooks, validation and switches](docs/bfme2-engine-hooks.md).
- `aotr_audiolimit.inc` indexes the audio request limit check.
- `aotr_pick.inc` and `aotr_rtmirror.inc` cover the mouse pick ray cast and the radar overlay mirrors.

`aotr_capture.inc`, `aotr_crashlog.inc`, `aotr_modtime.inc`, `aotr_flushtime.inc`, `aotr_clienttime.inc` and
`aotr_subsys.inc` are diagnostics. `AOTR_PROD` compiles the counters and the report threads out; everything
that changes what the game does stays in.

## Building

Windows, Visual Studio 2022 Build Tools, 32-bit MSVC toolset. Edit the `vcvars32.bat` path in the scripts if
yours differs.

```bat
cd src
build.bat                 :: rpmalloc.obj, aotr_accel.dll, inject.exe, testload.exe
build_prod.bat            :: bfme2_accel.new.dll, the shipped DLL
build_launcher_dist.bat   :: ..\dist\bfme2_accel_loader.exe and README.txt
```

The release is that DLL renamed to `bfme2_accel.dll`, zipped with the rest of `dist\` as a folder named
`BFME2 Accelerator`. `build_new.bat` builds the same DLL with the counters and report threads left in.

## Tests

```bat
cd src

build_crt_test2.bat        && crt_test2          :: fastcrt vs msvcr71, whole scratch buffer compared
build_rlsort_test.bat      && rlsort_test        :: maps game.dat, runs the stock sort beside the replacement
                                                 :: pass the path if your install is not C:\AgeoftheRing
build_audiolimit_test.bat  && audiolimit_test 0  :: indexed answer vs the walk, random list mutations
build_production_hotpath_test.bat             :: reporting vs production counters, cache checks and allocation
build_logicslicer_test.bat && logicslicer_test   :: slicer on vs off, operation sequence must be identical
build_quat_test.bat        && quat_test          :: reads ../quatpairs.txt
build_harness.bat          && rt_harness off 600 t600.txt
```

`build_production_hotpath_test.bat` runs both modes using the shipping preshader/allocator functions
and freshly compiled rpmalloc. Run it from an x86 Native Tools prompt, or set `AOTR_VCVARS32` to your
`vcvars32.bat` path. Production removes report-only work in those paths while retaining cache checks.

`rt_harness` renders a scene shaped like the game's own render pattern, reads every frame back and hashes it.
`off` goes straight to D3D9, `on` runs the same frames through the render thread. Run both and the hashes
must match. It needs `d3d9.dll` and `d3dx9_27.dll` beside the exe: take the first from the game install, the
second from the system.

`rttest-refs/` holds the author's `off` baselines (`dx4_off600.txt` is `off 600`, `dx4_off1500.txt` is
`off 1500`, `dx4_offnf.txt` is `off 600 ... nofpu`). They come from one DXVK build and one GPU, so your own
`off` against `on` is the real test.

`gd_funcs.inc` and `aotr_rt_gen.inc` are generated, by `gen_game_funcs.py` and `gen_rt_hooks.py`.

## Layout

```
src/          the DLL, the loader, the offline tests, the build scripts
src/art/      the loader's icon and mark, plus placeholder cover images
vendor/       rpmalloc
rttest-refs/  reference frame hashes for rt_harness
packaging/    the README that ships beside the binaries
quatpairs.txt captured transforms, quat_test's input
```

## Licence

MIT, in [LICENSE](LICENSE).

`vendor/rpmalloc` is Mattias Jansson's rpmalloc, released into the public domain.

The published loader embeds each game's cover image, which belongs to Electronic Arts, New Line Cinema and
the Age of the Ring team. Those files are left out of this repository. A build here picks up the placeholders
in `src/art/placeholder/` instead, so the loader you get shows three plain title cards.

This is an unofficial fan project with no connection to Electronic Arts.
