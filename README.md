# BFME2 Accelerator

**Download: [BFME2-Accelerator-2.0.zip](https://github.com/OhadBaehr/bfme2-accelerator/raw/main/release/BFME2-Accelerator-2.0.zip)**
(2.0, 9 October 2026 - unzip anywhere, run the loader, pick your game)

An in-process accelerator for *The Lord of the Rings: The Battle for Middle-earth II* and *The Rise of the
Witch-king* (EA SAGE engine, 2006), and for the mods built on them. It is a 32-bit DLL that a small loader
injects into `game.dat` at startup.

The engine runs its simulation and its Direct3D calls on a single thread, and draws 30 frames a second because
its logic runs once per frame. This moves the D3D work onto a second thread, replaces several hot engine
routines with versions that return the same bytes, holds the logic at its 30 steps while the picture is drawn 60
times a second, and draws a frame of its own between every two of the game's when the game cannot keep up.

Instructions for playing with it are in [packaging/README.txt](packaging/README.txt), the file that ships
beside the binaries. The rest of this page is about the download and the source.

Nothing is written into the game folder and `game.dat` on disk is never modified, so mod launchers and their
checksums still pass.

## The download

| | |
|---|---|
| File | [`release/BFME2-Accelerator-2.0.zip`](https://github.com/OhadBaehr/bfme2-accelerator/raw/main/release/BFME2-Accelerator-2.0.zip) (1,097,923 bytes) |
| SHA-256 | `14592DE0F4CF4BD10831DBD80DAE4209AA12013217D22A23B919A8FA277CC10E` |
| Contains | `BFME2 Accelerator\` with `bfme2_accel_loader.exe`, `bfme2_accel.dll`, `bfme2_accel.ini`, `README.txt` |
| Built from | this repository at the commit that added the zip (build 72) |

`bfme2_accel_loader.exe` and `bfme2_accel.dll` are signed (SHA-256, timestamped). The certificate was made on
the author's own computer, not issued by a certificate authority: it shows that the two files come
from the same hands as this page and have not been changed since, and nothing more - Windows lists the
signature without vouching for it, and antivirus programs do not trust it. Its public half is
[`release/BFME2-Accelerator-signing.cer`](release/BFME2-Accelerator-signing.cer).

| | |
|---|---|
| Signed by | `CN=OH1A, O=BFME2 Accelerator` |
| Certificate SHA-1 (thumbprint) | `AB5125D43647D3D228CCAD1A19FFA503E0676C50` |
| Certificate SHA-256 | `B3C079778CEBAFCC7767800E315FD514646E948EEA1CB177AF63BF8EE841E2DD` |

To check a download, in PowerShell:

```powershell
(Get-FileHash BFME2-Accelerator-2.0.zip -Algorithm SHA256).Hash
(Get-AuthenticodeSignature "BFME2 Accelerator\bfme2_accel.dll").SignerCertificate.Thumbprint
```

A loader that puts a DLL into another program looks like malware to a scanner, signed or not. If yours removes
a file, that is why; the source of every byte of it is on this page.

## What it changes

One include per piece under `src/`. Each piece is a line in `bfme2_accel.ini` (documented there) or an `AOTR_*`
environment variable, and says in `bfme2_accel.log` what it found and did.

The frame:

- `aotr_rt.inc` and `aotr_rt_gen.inc` hold the render thread. Main-thread D3D9 and D3DX calls are recorded
  into a queue and replayed on a second thread.
- `aotr_fpscap.inc` and `aotr_logicrate.inc` raise the frame cap and hold the simulation at 30 logic frames a
  second through a render-only frame path the engine already has, with animation carried forward on the frames
  in between.
- `aotr_tween.inc`, `aotr_pace.inc` and `aotr_devpass.inc` draw the in-between frames: the render thread runs a
  frame's draw stream a second time with bones, object transforms and the camera halfway to the frame before,
  puts the pictures on a steady beat, and issues the second run from a recording of the device calls D3DX made
  for the first (`aotr_shtab.inc` reads the shaders' constant tables for that).
- `aotr_panwarp.inc`, `aotr_screen.inc` and `aotr_scroll.inc` are the scrolling: the camera is stepped by the
  clock, and while it scrolls one picture goes up for each refresh of the screen (timed by the monitor's own
  vertical blank), shown from where the camera is at that refresh by one full-screen rectangle.
- `aotr_merge.inc` draws runs of the same skinned mesh in one call through the engine's own multi-mesh path.
- `aotr_drawgen.inc` generates the per-draw effect parameter writes rather than walking them.
- `aotr_rlsort.inc` and `aotr_rlsort_algo.h` run the mesh render list sort without the reference-count traffic.
- `aotr_logicspread.inc` leaves the engine's six logic calls per step in their stock order, each running
  exactly once. What moves is where the render frames fall inside that sequence.
- `aotr_posewarm2.inc` updates skeleton trees on worker threads.
- `aotr_pick.inc` and `aotr_rtmirror.inc` cover the mouse pick ray cast and the radar overlay mirrors.

Loading:

- `aotr_texjobs.inc`, `aotr_texmem.inc`, `aotr_texmem_gen.inc` and `aotr_texcache.inc` build and compress
  texture mip chains on worker threads instead of the game thread, and keep the results on disk.
- `aotr_terrain.inc` composes the ground patch textures on several cores.
- `aotr_fdbuf.inc` and `aotr_filewarm.inc` buffer the engine's byte-at-a-time file reads and open the game's
  files ahead of it (the first open of a file is where a virus scanner spends its time).
- `aotr_luasort.inc` and `aotr_streamset.inc` do two wasteful spots of the engine's own loading code without
  the waste (a sort that copied every record at every step, sets that were copied whole for every member added).

The rest:

- `aotr_fastcrt.inc` replaces the hot `msvcr71` imports with SSE versions that return identical bytes.
- `aotr_mutexcs.inc` puts the engine's `MutexClass` on a user-mode critical section instead of a kernel mutex.
- `aotr_equivmemo.inc` memoises `ThingTemplate` equivalence; `aotr_audiolimit.inc` indexes the audio request
  limit check.
- `aotr_factions.inc` gives a computer player whose faction has no skirmish side in the map the side the map
  would have carried (a map holds 20 sides; a mod has more factions than that).
- `aotr_exitfix.inc` fixes a use-after-free of the game's own when a skirmish is left.
- `aotr_shadowmap.inc` and `aotr_shadowpcf.inc` are optional and change the picture: a larger shadow map, and
  a nine-sample weighted shadow lookup patched into the game's pixel shaders in place of four point samples.

`aotr_capture.inc`, `aotr_crashlog.inc`, `aotr_loadprof.inc`, `aotr_modtime.inc`, `aotr_flushtime.inc`,
`aotr_clienttime.inc`, `aotr_posrate.inc` and `aotr_subsys.inc` are diagnostics. `AOTR_PROD` compiles the counters and the report
threads out; everything that changes what the game does stays in.

Hooks are of two kinds. Some are found by wildcarded byte signature; a signature that is missing or ambiguous
installs nothing and logs it. The others are addresses inside one build - *The Rise of the Witch-king* 2.02's
`game.dat`, recognised by a hash of its code section - and install only on that build. On a build it does not
know the accelerator installs the first kind only, and in a program that is not this engine nothing at all.

## Building

Windows, Visual Studio 2022 Build Tools, 32-bit MSVC toolset. Edit the `vcvars32.bat` path in the scripts if
yours differs.

```bat
cd src
build.bat                 :: rpmalloc.obj, aotr_accel.dll, inject.exe, testload.exe
build_prod.bat            :: bfme2_accel.new.dll, the shipped DLL
build_launcher_dist.bat   :: ..\dist\ with bfme2_accel_loader.exe, README.txt and bfme2_accel.ini
```

The release is that DLL renamed to `bfme2_accel.dll`, zipped with the rest of `dist\` as a folder named
`BFME2 Accelerator`. `build_new.bat` builds the same DLL with the counters and report threads left in.

## Tests

None of them needs the game to run; some need files from its install.

```bat
cd src

build_crt_test2.bat        && crt_test2          :: fastcrt vs msvcr71, whole scratch buffer compared
build_rlsort_test.bat      && rlsort_test        :: maps game.dat, runs the stock sort beside the replacement
                                                 :: pass the path if your install is not C:\AgeoftheRing
build_audiolimit_test.bat  && audiolimit_test 0  :: indexed answer vs the walk, random list mutations
build_logicslicer_test.bat && logicslicer_test   :: slicer on vs off, operation sequence must be identical
build_quat_test.bat        && quat_test          :: reads ../quatpairs.txt
build_pace_test.bat        && pace_test          :: the picture beat over uneven frame patterns
build_lerp_test.bat        && lerp_test          :: bones blended four at a time vs one at a time, bit for bit
build_harness.bat          && rt_harness off 600 t600.txt
build_tween_test.bat       && tween_test         :: in-between frames, pan pictures, device resets
build_merge_test.bat       && merge_test         :: merged draws against single draws
build_pcftest.bat          && pcf_test <folder>  :: the shadow lookup patch on every pixel shader of the game
```

`rt_harness` renders a scene shaped like the game's own render pattern, reads every frame back and hashes it.
`off` goes straight to D3D9, `on` runs the same frames through the render thread (it loads
`bfme2_accel.new.dll`). Run both and the hashes must match; add `tween`, `tweenslow`, `texstream texjobs`,
`terrain`, `panid`, `pansim`, `pcf` and the other words in its source for the other features. It, `tween_test`,
`merge_test` and `pcf_test` need `d3d9.dll` and `d3dx9_27.dll` beside the exe: take the first from the game
install, the second from the system.

`pcf_test` wants a folder holding the compiled effects (`*.fxo`) of the game's `Shaders.big`;
`python bigfxo.py <out folder> <game folder>` copies them out.

`rttest-refs/` holds the author's `off` baselines (`dx4_off600.txt` is `off 600`, `dx4_off1500.txt` is
`off 1500`, `dx4_offnf.txt` is `off 600 ... nofpu`, the others carry the option in their name). They come from
one DXVK build and one GPU, so your own `off` against `on` is the real test.

`gd_funcs.inc`, `aotr_rt_gen.inc` and `aotr_texmem_gen.inc` are generated, by `gen_game_funcs.py`,
`gen_rt_hooks.py` and `gen_texmem_stubs.py`.

What 2.0 was not tested on is said in [packaging/README.txt](packaging/README.txt): it was developed on one
machine, with DXVK as the game's Direct3D 9.

## Layout

```
src/          the DLL, the loader, the offline tests, the build scripts
src/art/      the loader's icon and mark, plus placeholder cover images
vendor/       rpmalloc
rttest-refs/  reference frame hashes for rt_harness
packaging/    the README and the settings file that ship beside the binaries
release/      the download, and the public half of the certificate it is signed with
quatpairs.txt captured transforms, quat_test's input
```

## Licence

MIT, in [LICENSE](LICENSE).

`vendor/rpmalloc` is Mattias Jansson's rpmalloc, released into the public domain.

Each game's cover image belongs to Electronic Arts, New Line Cinema and the Age of the Ring team, and none of
them is in this repository - not as a file and not inside the download. The loader here, and the one in
`release/`, is built with the placeholders in `src/art/placeholder/` and shows three plain title cards.

This is an unofficial fan project with no connection to Electronic Arts.
