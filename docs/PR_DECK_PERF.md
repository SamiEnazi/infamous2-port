# Steam Deck / Linux performance (branch `deck-perf`)

The detailed companion of the pull request: every change with its problem, what changed, what it measured, the files,
the switch and its default, and whether Windows builds are affected. The summary, the comparison with upstream, the
installer and the video are in the pull request's description. Hardware: Steam Deck OLED (Zen 2, 4 cores / 8 threads,
one 15 W budget shared by CPU and GPU), SteamOS 3.9.

## Changes, by topic

Each entry: the problem, what changed, what it measured, the files, the switch and its default, and whether Windows
builds are affected. "Linux launcher" means `tools/play.sh` (a `:=` default: any value set from outside wins, `=0`
gives the old path). Older measurements are labelled with their date and setup; the settings then were my own (the
player's), not the Deck preset of the PGO table in the pull request, and most were frame-stepped bench runs (topic 17). Commit hashes are on the fork's `deck-perf` branch.

### 1. Linux write tracking for the draw engine's caches

- **Problem.** `rsx_wtrack.c` built only on Windows. On Linux every cache check of the draw engine (vertex / index
  caches, programs) fell back to hashing the guest memory every frame.
- **Change.** `rsx_wtrack.c` builds on Linux through the engine's Win32 layer (`VirtualProtect` = `mprotect`, the
  vectored handler on SIGSEGV / SIGBUS ahead of the engine's own handlers, which chain). The kernel-write bracketing in
  `ppu_fs.cpp` / `cellFs.c` was already platform neutral.
- **Effect.** 2026-11-18, plaza bench, build t13g (v0.5.7-based, PGO): `PS3RECOMP_WTRACK=0` 33.8 fps vs 41.8 with it
  (-19 %), RSX thread 28.2 vs 21.0 ms a frame. The shadow mode (`=2`) found no missed write.
- **Files.** `engine/libs/video/rsx_wtrack.c` (f0baf8c).
- **Switch.** `PS3RECOMP_WTRACK` (default 1 as on Windows; 0 = hash every frame, 2 = shadow check).
- **Windows.** Unchanged (the `#if` now reads `_WIN32 || __linux__`).

### 2. Linux thread placement (`PS3_CPU_PLACE=rsxcore`)

- **Problem.** `host_place.c` is a stub on Linux, so the RSX thread shares its core with job threads. On a 4-core CPU
  that slows the thread that decides the frame.
- **Change.** On Linux, `rsxcore` puts the RSX thread alone on the first hardware thread of the last core (its SMT
  sibling kept free) and every other thread on the other cores. A helper thread re-applies the split once a second for
  threads created later. The CPU budget is read before any confinement. A second RSX-role thread (`PS3_RSX_SPLIT`,
  opt-in) and the Vulkan recording worker (`PS3_CPU_PLACE_VKW=1`, topic 8) can be put on the free sibling.
  `rsxcpu` (opt-in) pins the RSX thread to one CPU only.
- **Effect.** 2026-11-18, plaza bench, t13g: `PS3_CPU_PLACE=off` 37.8 fps vs 41.8 (-10 %), RSX thread 24.9 vs 21.0
  ms. For comparison, pinning v0.6.2's threads from outside with `taskset` gave it +4.5 %.
- **Files.** `engine/runtime/platform/host_place.c`, `host_place.h` (214308e, 8af80d5, 940f64c, 4cabf54).
- **Switch.** `PS3_CPU_PLACE` (engine default: off; Linux launcher: `rsxcore`), `PS3_CPU_PLACE_VKW` (default 0;
  Linux launcher 1).
- **Windows.** Unchanged (Linux code is in the `#if defined(__linux__)` branch).

### 3. RSX thread: vertex fetch, constant uploads, draw offsets

- **Vertex fetch in registers** (b4eba3e, `engine/libs/video/rsx_vertex_compact.c`). The float / half / unorm8 /
  uint8 / sint16 / snorm16 fetch forms built each vertex in a 16-byte stack array with four word stores and read it
  back with one 16-byte load: a store-forwarding stall per vertex, ~40 % of `rsx_vertex_fetch_all_ex`. The vector is
  now built in registers (SSE4.1). Old vs new on 48 M random vertices (every 16-bit value for the half / int16
  forms): 0 differ. 2026-11-17, plaza bench: RSX thread 22.0 -> 20.85 ms a frame. Switch `PS3RECOMP_VTX_REGS`
  (default 1; 0 = the stack forms). The same commit makes the natives' address check (`spu_native_ea_ok`) inline for
  the common case. All platforms.
- **Constant uploads eight words a step** (eed8799, `rsx_dispatch.c`, `rsx_vertex_compact.c`). With the seen[]
  counting off, a constant run goes eight words at a time on AVX2 builds (`#if defined(__AVX2__)`); the remap range
  pass lost its early exit so it vectorises. RSX thread 21.3 -> 21.15 ms (small). No switch. All platforms whose
  engine is compiled with AVX2.
- **Draw offsets instead of rebinds** (f19120f, `rsx_vulkan_backend.c`, `rsx_draw_engine.c`). A draw from the bound
  vertex or index buffer at a whole number of elements past the bound offset passes that distance in the draw
  (vertexOffset / firstVertex / firstIndex) instead of binding again: RADV re-uploads its vertex-buffer descriptors
  after every bind. Plaza: binds a frame 6,800 -> 574 (vertex) and 6,800 -> 151 (index); 41.0 fps vs 38-39 (three
  runs, 2026-11-17). Switch `PS3RECOMP_VK_DRAW_OFFSETS` (default 1; 0 = the binds). Vulkan backend on all platforms.
  The commit also adds two counters (`PS3RECOMP_VK_CALLSTATS`, `PS3RECOMP_ENG_PASSSTATS`) and job 210 / SSAO
  diagnostics, all off by default.

### 4. RSX thread: the short tail in a wide picture

- **Problem.** With a 16:10 picture (the port sets `PS3_ASPECT_HUD_AFTER`), the short tail for repeated draws
  (`rsx_draw_fast.inc`, ~78 % of the draws at 16:9) was off for every draw.
- **Change.** `eng_fd_hud_note` / `fd_takes` keep the short tail when the HUD is pinned to 16:9 by the composite
  program (rsx_draw_engine.c, rsx_draw_fast.inc; 04ddd13).
- **Effect.** 2026-11-18, plaza bench at 16:10, same binary with the switch off / on (7 rounds): RSX thread 19.63 ->
  18.59 ms, 48.44 -> 50.40 fps, p99 24.2 -> 23.3 ms. Exact: `VERIFY=2` compared 13 M draws, 0 differences. The Deck
  preset of the tables above is 16:9 (`PS3RECOMP_ASPECT` unset), so this change does not enter them; it matters for
  players who pick 16:10 or Auto on the Deck's screen.
- **Switch.** `PS3RECOMP_ENG_FASTDRAW_HUD` (default 1). **Windows:** affected (all platforms), same behaviour.

### 5. RSX thread: descriptor sets kept across frames, fragment program memo (R8)

- **Problem.** On the street ~520-600 descriptor sets a frame were allocated and written again although most had been
  made the frame before, and ~190 fragment programs a frame (the same program uploaded again with other inline
  constants) ran the whole analysis again.
- **Change.** Bit 1: each frames-in-flight slot keeps its sets in a table keyed by the old key plus a serial per
  texture unit's image (a dead view's handle value never meets an old set); a set is written once and bound only by
  its slot's command buffer after the slot's previous submit completed; destroying the uniform arena or a sampler drops
  the tables; the pool is reset only past 8,192 sets. Bit 2: after a content-memo miss, a 1,024-entry table keyed by
  the full instruction blocks gives the structural hash and the unit masks (programs with a branch take the whole
  analysis); the constants are still collected from the program's bytes.
- **Effect.** New sets 520-600 -> 40-69 a frame (street). 2026-11-20 (hd4 module, PRIO 1): RSX thread -0.41 ms
  (SE 0.05) on the quick street, fps +0.89 % (SE 0.42, 8 pairs); standing at the plaza fps +1.96 % (SE 0.62), RSX
  -0.46 ms. In the combination of step 6 (no-PGO modules, 6 rounds), taking R8 out was -1.69 % (SE 0.45) on the street
  and -1.42 % (SE 0.92) on the plaza.
- **Exactness.** Check build (`-DR8_CHECK`, `PS3RECOMP_ENG_R8_CHECK=<n>`): every n-th frame a draw bound with a kept
  set is repeated with it and with a twin set written now, into scratch targets, compared byte for byte: 2,870 draws,
  23.7 GB, 0 bytes differ. `PS3RECOMP_ENG_R8_VERIFY=1`: 865,841 memo hits re-analysed, 0 differ.
- **Files.** `rsx_vulkan_backend.c`, `rsx_draw_engine.c` (50b53d0).
- **Switch.** `PS3RECOMP_ENG_R8` bit mask, engine default 0 (the old paths); Linux launcher 3.
- **Windows.** Unchanged unless the variable is set (the code is there, off).

### 6. Vulkan recording worker on Linux

- **Problem.** `vk_threaded_rec.h` / `vk_threaded_be.h` (the RSX thread queues its `vkCmd*` calls, a worker records
  them) built only on Windows.
- **Change.** They build on Linux through the Win32 shim (940f64c), off by default in the engine; `PS3_CPU_PLACE_VKW=1`
  puts the worker on the RSX core's free SMT sibling. The Linux launcher turns both on (44f6776).
- **Effect.** It lost on the Deck on 2026-11-17 with the older, slower RSX thread (38.0 vs 40.0 fps). On hd5c
  (2026-11-20, PRIO 1 / R8 3 both sides, quick street, 3 pairs) +2.12 % (SE 0.69), RSX thread 18.14 -> 16.32 ms. hd6c
  (an engine profile retrained with it) vs hd5c, 8 pairs each: street +0.65 % (SE 0.42), RSX 16.7 -> 14.3 ms;
  plaza +0.44 % (SE 0.67), RSX 18.0 -> 16.2 ms. The fps gain is small because the main thread is the limit now; the RSX
  thread's 2.4 ms are headroom. `PS3RECOMP_VK_BT=1` on top still loses (-13.6 %).
- **Files.** `engine/libs/video/vk_threaded_be.h`, `vk_threaded_rec.h`, `host_place.c` / `.h`, `tools/play.sh`.
- **Switch.** `PS3RECOMP_VK_THREADED` (engine default on Linux 0; Linux launcher 1), `PS3_CPU_PLACE_VKW` (0; launcher 1).
- **Windows.** Unchanged.

### 7. GPU: the light pass per 16x16 tile

- **Problem.** The light pass (the game's SPU light job, done on the GPU over the whole screen) walked every light of
  the frame (83-129 on the street) for every pixel: 5.05 ms of the street's 17.3 ms GPU frame at 1200 MHz
  (2026-11-19, the player's settings).
- **Change.** A pass over 4x4-pixel position boxes and a tile pass (0.3 ms together) keep, per 16x16 tile, the lights
  whose sphere can reach the tile's (slightly widened) box; the light shader walks only those, in ascending order,
  with the original loop body. A light left out has an outer radius smaller than its distance to every pixel of the
  tile, so the original's own `if (w <= 0) continue` skipped it there too: same lights, same order, same arithmetic.
- **Effect.** 2026-11-19, street, 1200 MHz: GPU 17.43 -> 13.17 ms a frame (-24 %), fps +3.2 %, main thread -0.7 ms
  (the GPU draws less of the shared 15 W); plaza GPU 17.58 -> 13.51 ms, fps +3.2 %; a stretch where the GPU limited
  +7.4 %.
- **Exactness.** `PS3RECOMP_VK_LIGHT_CULL=2`: every 60th light pass is also rendered with the original shader, both are
  read back and compared byte for byte: 0 bytes differ in 113 M pixels (street + plaza, up to 129 lights).
- **Files.** `rsx_vulkan_backend.c` (`vk_eng_light_pass`), `vulkan/rsx_vk_passes_glsl.h` (130be5d).
- **Switch.** `PS3RECOMP_VK_LIGHT_CULL` (default 1; 0 = the original shader, 2 = check mode).
- **Windows.** Affected (Vulkan backend, all platforms; same picture).

### 8. Job runner: priority class for other threads' long jobs

- **Problem.** On the street, Edge (manager 221, ~15 jobs of ~600 us kicked by the Edge thread at the frame start)
  held all four pool threads to ~4.3 ms after the flip; the 60-80 animation jobs the main thread kicks at 1-4 ms
  queued behind them in the FIFO ready ring, and the main thread waited for them (1.46 ms a frame).
- **Change.** Two ready rings. A manager whose last flush came from a thread other than the main PPU thread AND whose
  jobs are long (mean over its life >= `PS3_NATIVE_JOBS_PRIO_LONG_US`, 250 us) is low class: on the street and the
  plaza that is Edge alone. FIFO inside a class, jobs never interrupted, the sticky pick yields to the high class, a
  low job older than `PS3_NATIVE_JOBS_PRIO_AGE_US` (8 ms) goes first anyway. Off = the single FIFO as before.
- **Effect.** 2026-11-20, same binary on / off: plaza +2.06 % (SE 0.63, 7 pairs), quick street +1.25 % (SE 0.88,
  8 pairs); the post-Edge wait 1.44 -> 0.59 ms a frame. In the step-6 combination, taking it out: street -0.76 %
  (SE 0.48), plaza -0.56 % (SE 1.01). Standing 10 min x 2 with the real controller: equal (59.68 / 59.65 vs 59.72 /
  59.59 fps), no frame > 50 ms.
- **Tests.** `tests/jobrunner`: a new `prio` scenario (7 cases); with upstream's 8, 15 of 15 pass.
- **Files.** `engine/libs/spurs/wws_runner.c`, `tests/jobrunner/jobrunner_harness.c`, `test_jobrunner.py` (7bc12f8).
- **Switch.** `PS3_NATIVE_JOBS_PRIO` (engine default 0; Linux launcher 1); bench modes 2-5.
- **Windows.** Unchanged unless the variable is set.

### 9. Job runner: help-peek

- **Problem.** `wait_until`'s spin called `help_one` on every turn and `help_one` took the manager's lock each time:
  ~15,000 lock / unlock pairs a frame from the main thread, against the pool threads that finish and claim the same
  manager's jobs (3.4 % of the main thread's samples were in `pthread_rwlock_*`).
- **Change.** `help_one` reads head / tail (and the head job's slot / index) without the lock first and returns when
  there is nothing it may run; the locked test still decides.
- **Effect.** Neutral on fps (street 46.68 vs 46.66, 2026-11-18); ~30,000 fewer lock calls a frame.
- **Files.** `wws_runner.c` (365fd60). **Switch.** `PS3_NATIVE_JOBS_HELP_PEEK` (default 1). **Windows.** Affected
  (same behaviour).

### 10. Job 204 native: object types 1 and 3, slot 12 exact

- **Problem / change.** The job 204 native (render lists, ~520 jobs a frame) handled object types 0 and 2 only and
  wrote slot 12's y..w from the wrong stack quad (type 0): 231 of 517 recorded slot-12 calls differed from the lifted
  code (reported separately with the analysis). Now: types 1 and 3 natively, `j->r112` tracks main's r112 per range
  with the item loop's look-ahead, and the native declines (falls back to the lifted code) in the three cases it does
  not model.
- **Effect.** SPU time -1.1 %, fps +0.55 % (2026-11-18); all 8,000 recorded calls match with the whole-block compare.
- **Files.** `natives/spu/job204_native.c`, `engine/runtime/spu/spu_jobrec.c` / `.h` (6b300ea, b653037, 0183e2a,
  56427dc). **Switches.** `PS3RECOMP_NATIVE_J204`, `J204_TYPES` (default 0xF), `PS3_JOBREPLAY_FULLPUT=1` (the replay
  compares whole 4096-byte blocks). **Windows.** Affected (game module), same output as the lifted code.

### 11. Other SPU job natives (from before v0.6)

Kept from the branch's v0.5.7 base, all exact by job replay (topic "Correctness"):

- job 218: the bone hierarchy (0x179C0), the finalize kernels (0x17780, 0x18B80, the 0x5814 loop) and the clip sampler
  (0x18EC0, four tracks in SIMD lanes); `J218_PICK=<blend><pose><palette>` chooses this branch's or upstream's native
  per function, default `ttt` = upstream's blend / pose / palette (13 % faster per job here); the check-mode scratch
  lives on the heap (the module has no static TLS for it). Files: `natives/spu/job218_native.c`, `job218_clip_native.c`,
  `native_218.c`, `natives.txt` (c65d8ab, d474d52, 9bd07d5, dee8e27, a77dd5a, f75d36e, 865ef66, 5dfb0b2).
- natives for jobs 211, 208, 209, 201, 212 from the Switch fork (MIT; 208 / 209 / 212 are upstream's whole-job natives
  under the native runner), job 223's index decompression (0xC7B8), job 220's BSP walk (0x7470, off by default: no gain
  live), the SPU helpers' generate-controls without a store-forwarding stall, job 210's culling walker (superseded by
  upstream's whole-job 210 under the runner) (1722499, efb5338, a1bfffd, 7872648, e3ec209, 1297e1a, 79d9c81).
- "Far shadow detail" (Graphics page, `PS3RECOMP_SHADOW_LOD`, default Full = the game's): Reduced lets small far
  objects cast no distant shadow (~2,400 fewer shadow draws, RSX thread ~-12 %), also inside upstream's whole-job 210
  (`engine/libs/spurs/native_jobs/job210.c`; 89e52ae, b529db1). A picture choice, off by default.

**Windows:** the natives are in the game module, so they apply on every platform. Not tested here: whether the Windows
build of the module has the import slots for the natives' extra engine calls.

### 12. Main thread: cheaper store checks (lift-time, Linux default)

- **Problem.** Every lifted store tests the guest page map and re-checks it (shr, two loads / compares / branches):
  11 % of the lifted code's instructions on the main thread, 40 % of them on stores that follow another store off the
  same register in straight-line code (prologue saves, structure fills).
- **Change.** Two lift-time switches in `engine/tools/ppu_fastpath.py`: `PPU_ST_CMPB=1` does the test and its re-check
  as one `cmpb $1, (map, page)` each (asm goto: the same byte read at the same point); `PPU_ST_RUNS=1` gives three or
  more stores off one guest register in straight-line code one test of the run's two end pages, plain byte-swapped
  stores and one re-check (if a page went slow meanwhile, the per-store slow path for every store of the run); the
  stretch is emitted twice, the careful copy taking runs whose pages are not fast. 210,309 of 417,714 store sites
  grouped (35,039 runs); the module's .text grows 4.5 % (the cold copies). `tools/rebuild_win.py` turns both on for
  Linux builds only.
- **Effect.** 2026-11-20, quick street, no-PGO modules, counters: main-thread instructions -6.2 % (SE 1.1), cycles
  -2.4 % (SE 0.85), main thread -0.42 ms a frame (SE 0.07), fps +0.96 % (SE 0.42). In the step-6 combination, taking
  it out: street -2.39 % (SE 0.73), main +0.56 ms.
- **Exactness.** An independent check of the lift text (all 35,039 run blocks: same statements, same stores, offsets =
  the original addresses): 0 differences; the verify lift (`PPU_ST_RUNS_VERIFY=1`, every grouped store checks its own
  address): 0 reports on the street and the plaza; 69 game runs without a crash, hang or error.
- **Files.** `engine/tools/ppu_fastpath.py` (833f677), `tools/rebuild_win.py` (6df5ed0).
- **Switch.** Lift-time `PPU_ST_CMPB` / `PPU_ST_RUNS` (default off in the lifter; on in Linux builds; `=0` in the build's
  environment = the old lift, a different lift fingerprint: one re-lift).
- **Windows.** Unchanged (Windows builds keep the old lift). Every lifted function with a store gets a new CFG hash, so
  a PGO profile made before it matches none of them.

### 13. SDL joystick-list crash on Linux, and the fix reworked for the controller thread

- **Problem.** sdl2-compat (SteamOS's SDL2 over SDL3, also Arch, Fedora 42+) keeps its joystick list in globals with
  no lock, and `SDL_NumJoysticks` frees and rebuilds it, also while converting every joystick ADDED / REMOVED event.
  `cellPadInit`'s `SDL_InitSubSystem` on a game thread queues those events while the RSX thread's window pump
  (`SDL_PollEvent` every 4 ms) converts them: a double free, then "malloc(): unaligned tcache chunk detected"
  (1 crash in 33 Game Mode sessions; v0.6.2 and v0.6.3 have the same code). v0.6.3's controller thread is one more
  thread in that list.
- **Change.** 8833293 drained the pump's queue under cellPad's poll lock (try-lock). For v0.6.3 that was reworked in the
  merge (2335d44): a lock of its own for the joystick list (`s_sdl_list_lock`, `cellPad_host_sdl_trylock`) taken around
  every controller pass, the thread's device list and closes, `cellPadInit`'s SDL work and the `PS3_PAD_THREAD=0`
  in-call pass; the RSX pump keeps its try-lock (a busy pass is skipped, 4 ms later the next). Not the poll lock, so a
  pad call of the title never waits for a pass that blocks in a driver (upstream's `test_pad_thread.c` with a slow fake
  SDL: slowest pad call 0.13 ms with the lock).
- **Evidence.** A replica outside the game (`mg3-sdlrace.c`: the pump, cellPadInit / End 200 times a run, the controller
  thread; system sdl2-compat 2.32.70 over SDL3 3.4.12): v0.6.3 as released aborts in 10 of 10 runs; 8833293's lock as
  it was, 4 of 20; the reworked lock 0 of 20 (4,000 init cycles, ~81,000 passes), 0 of 10 with a pad plugged in / out
  every 10 passes, 0 memory errors under ASan in 5.
- **Files.** `engine/libs/input/cellPad.c`, `rsx_vulkan_backend.c`, `vk_overlay.cpp`.
- **Switch.** None (a fix). **Windows.** Unchanged (`#if !defined(_WIN32)`: real SDL2 has its own lock).

### 14. Controller thread every 4 ms on Linux

- **Problem.** v0.6.3's controller thread polls every 1 ms. On the Deck's controller (SDL HIDAPI) a pass costs 40-55 us:
  32 ms of CPU a second (755 passes) on the CPUs the main thread and the job pool share.
- **Change.** Linux launcher `PS3_PAD_THREAD_US=4000` (13 ms a second, 234 passes). A pad call then copies a snapshot at
  most ~4 ms old (1 ms with the upstream default), against a 17-25 ms frame.
- **Effect.** 2026-11-19, street, SDL pads on, n = 4 each: 46.46 -> 47.12 fps (+1.4 %, 1.9 SE), main thread 19.41 ->
  19.14 ms. In-run check later: +2.2 % (SE 1.0).
- **Files.** `tools/play.sh` (64574f3). **Windows.** Unchanged (keeps 1 ms).

### 15. Linux launcher defaults (`tools/play.sh`)

| Variable | Value | Why, measured |
|---|---|---|
| `SDL_VIDEODRIVER` | `x11` when `WAYLAND_DISPLAY` is set | KDE Plasma 6 reports more than 64 surface formats, the swapchain code asks for 64, gets VK_INCOMPLETE and runs without a window (0f4f273, 2026-11-16). v0.6.3 now retries with x11 by itself; this line skips the failed first start. |
| `PS3RECOMP_GCM_SPIN_US`, `PS3RECOMP_VK_TQ_SPIN_US` | 0 | no busy-waiting in the GCM wait and the Vulkan worker on 4 cores (0f4f273) |
| `PS3RECOMP_VK_PIPE_THREADS` | 1 | one pipeline worker (0f4f273) |
| `PS3_FRAMECLOCK_US` | 250 | on Linux the frame clock sleeps its whole period (0f4f273). These four: 28.5 -> 30.9 fps at one street (2026-11-16, PGO build) |
| `PS3_CPU_PLACE` | `rsxcore` | topic 2 |
| `PS3_NATIVE_JOBS_THREADS` | 4, **on a Steam Deck only** (`board_vendor` = Valve; 04d709b) | the engine default (physical cores - 2) is 2 on a 4-core CPU. 2026-11-18, plaza bench, merged no-PGO build, 2 runs each: 2 / 3 / 4 pool threads 37.8 / 40.8 / 41.0 fps (with 2 the main thread ran jobs in its waits for ~26 ms a frame) (60870c0) |
| `PS3_PAD_THREAD_US` | 4000 | topic 14 |
| `PS3_NATIVE_JOBS_PRIO` | 1 | topic 8 |
| `PS3RECOMP_ENG_R8` | 3 | topic 5 |
| `PS3RECOMP_VK_THREADED`, `PS3_CPU_PLACE_VKW` | 1, 1 | topic 6 |
| `GLIBC_TUNABLES` + `PS3_STATIC_TLS_RETRY=1` | `glibc.rtld.optional_static_tls=1048576` | the game module has initial-exec thread-locals; without the tunable the engine restarts itself through `/proc/self/exe` and the process is then named `exe` (36b24fb) |

Every other line applies to every Linux PC (measured on the Deck only; see "Known limits").

### 16. Linux build

- **Incremental module builds** (dc67c5b, `tools/rebuild_win.py`). On Linux `lift_restore_times()` ran only after the
  module compile, so every re-lift compiled the whole module twice; it now runs before, as on Windows. The patched port
  files are made in a scratch folder and copied over only when their bytes differ (they keep their mtime). Measured:
  nothing changed 1 s, one native changed 2 s (one unit), one engine file 2 s, the whole module from scratch 411 s; an
  incremental module is byte-identical to a full compile. Windows: the patched-files part applies there too.
- **`PORT_C_EXTRA`** (8e496e8): flags added to the engine / runtime C and C++ on Linux (not the lifted code), e.g.
  `-march=znver2 -ffp-contract=off`. Opt-in.
- **Linux PGO training module** (0381235): the instrumented module failed to load ("undefined symbol: atexit"); a
  hidden `atexit` over `__cxa_atexit` with the module's own `__dso_handle`, only in `PORT_PGO=gen` builds of the Linux
  module. This is what a Linux "Optimise for this PC" needs first; `tools/pgo_train.py` and patch 0014 (topic 19) build on it.

### 17. Bench and diagnostic code in the branch (all opt-in, off by default)

- Frame-stepped guest clock `PS3_TB_FRAMESTEP_US` (the guest timebase moves exactly 1/30 s per flip once a recorded
  pad route starts: the same work in every run), `PS3_TB_FRAMESTEP_IDLE_US` (the flipping thread's clock-polling waits
  see real time after a limit: removes the stepped clock's fixed ~120 ms frames), `PS3_SPU_DMASTAT`, Linux thread names
  (`ps3-main`, `ps3-rsx`, `ps3-spu<n>`, `ps3-spuj<n>`, `ps3-pad`) (cb44e50, 626ab72, f9bbfd8).
- Causal-test delays per flip / present (`PS3_DBG_FLIP_DELAY_US`, `PS3_DBG_PRESENT_DELAY_US`; 4cabf54), the in-run
  A/B (`PS3_LIVEAB`: flips a switch in blocks of frames inside one run), per-job delays (`PS3_JOBDELAY`), closed-loop
  route steering for the street bench (`PS3_PAD_STEER`) (b2c9763), low-power delays per draw / MFC command / kick and a
  job timeline (630bc6e), and three sizing modes that give a wrong picture on purpose (Edge segment memo, Vulkan
  recording skip, an SMT spinner; 25c0a1a).
- Measured cost when off: the in-run A/B / steering / job-delay engine (b2c9763) vs the engine before it, same module,
  4 + 4 runs: fps -0.20 % (SE 0.76), counters equal; the Edge hook of 25c0a1a (one call and one branch per kernel
  call, ~650 a frame): job 223 589 vs 586 us a job, kernel cycles equal within the run noise.

These are what the numbers in this description were measured with. If you would rather not carry them, they are in
separate commits and the rest does not depend on them (except the thread names, which tools use to group threads).

### 18. The aspect-ratio flicker (#47, #105)

- **Problem.** At an aspect ratio other than 16:9 (16:10 on the Deck, 21:9, 32:9) the 3D picture was squeezed into the
  16:9 rectangle for one frame now and then. The title flips through the `cellGcmSetFlipCommand` import, and now and then
  a drain runs on into the next frame: one present holds two of the title's frames (caught: 16,438 draws in one present,
  ~8,000 a frame). The HUD logic counted frames by presents, so the second frame's display-sized G-buffer and post passes
  came after the composite and were pinned as the HUD. More frequent at a high frame rate; v0.6.3 has the same logic.
- **Change.** `eng_hud_next_frame` (`rsx_draw_engine.c`): a display-sized draw after the composite into another target
  is the next frame's and starts the next slice; a later draw into the composite's own target takes the break back.
  Design note: `engine/docs/WIDESCREEN.md`, "Two frames in one present".
- **Checked.** At 16:10 on the Deck, short street and standing runs recorded before / after: the old behaviour (also
  switched back on in the new build) gave a squeezed frame, logged with 3,133 squeezed draws; the fix none. 16:9 does
  not run the code; menus, movies and loading screens keep their bars.
- **Switch.** `PS3RECOMP_WIDE_NEXTFRAME` (default 1; 0 = frames by presents only). **Windows.** Affected (all
  platforms), same fix.

### 19. Linux installer and Optimise for this PC

The double-click installer (`tools/install_gui.sh`, the launchers `tools/launcher/launcher.c`), the build environment
that works the same on every distribution (`tools/buildenv.sh` / `.py`), Optimise for this PC on Linux with the engine
trained too (`tools/pgo_train.py`, patch 0014 `PS3_PGO_ENGINE_FILE`), Add to Steam (`tools/steam_shortcut.py`) and the
Linux release zip (`tools/make_release_zip_linux.py`): all in [`PR_INSTALLER_PGO.md`](PR_INSTALLER_PGO.md), with the
measurements.

## Correctness

The rule I worked to: an SPU native has to match the lifted job bit for bit, a graphics change has to give the same
picture, and every change can be turned off by a variable (or, for the store lift, by the build's environment).

- **Job replays** (`tools/jobreplay.sh` on recorded jobs; `PS3_JOBREPLAY_FULLPUT=1` compares whole 4096-byte output
  blocks): job 204 1,960 / 1,960 (and the full 8,000-call recording), 210 / 218 100 / 100 and 102 / 102 with both
  `J218_PICK` settings, 211 / 220 100 / 100, 223 150 / 150, a 1,800-job recording made by the native runner (204 / 209 /
  210 / 212 / 218 300 / 300 each, and your 208 / 209 / 210 whole-job natives 300 / 300), 400 street jobs each of 213
  and 216. The pass / fail table and every native's line have been identical from build to build since the v0.6.2
  merge. One known limit of the replay tool: this branch's old `nx_208` hook fails every replay (it is not
  used under the native runner; with `PS3RECOMP_NATIVE_J207=0` the lifted 208 passes).
- **Live checks.** Job 218: the two-group blend 0 of 850 k live calls differ; your 218 natives 0 of 3.8 M; your
  verify modes 210 0 of 89 k, 208 0 of 80 k, 209 0 of 26 k.
- **Upstream's tests.** `tests/jobrunner/test_jobrunner.py`: 15 of 15 (your 8 + the 7 `prio` cases). On the
  runner before your Flood Town fix it shows the hang (`--expect-broken`), as intended. `engine/libs/input/tests/
  test_pad_thread.c` with a slow fake SDL: the slowest pad call 0.13 ms with the reworked lock.
- **Pictures.** Light tiles: check mode, 0 bytes differ in 113 M pixels (street + plaza). R8: 2,870 draws repeated with
  kept and fresh descriptor sets, 23.7 GB compared, 0 bytes differ; 865,841 memo hits re-analysed, 0 differ. Short tail
  at 16:10: 13 M draws checked (`VERIFY=2`), 0 differences. Vertex fetch: 48 M random vertices, old vs new, 0 differ.
  Draw offsets read the same bytes by construction. End shots of the benches at the same spot differ only where people
  walk.
- **Store lift.** An independent check of the lifted text (all 35,039 run blocks) and a verify lift (every grouped store
  checks its own address): 0 differences, 0 reports.
- **SDL race.** The replica program (`mg3-sdlrace.c`, see topic 13): v0.6.3 10 of 10 runs abort, with the fix 0 of 20,
  0 of 10 with hot-plugs, 0 ASan errors.
- **Stability.** Every bench run of this round exited by itself; standing runs of 10-30 min on every
  build I installed since 2026-11-18: no crash or hang; one frame over 50 ms (58 ms) in the 20 min of the v0.6.3-merge
  gate, none in the others. Memory grows
  ~0.75 MB a minute while standing in all builds, upstream's included (not from these changes).

## Not in this PR

- **The bench tooling outside the engine** (`work/bench/` in my tree: the run, rotation, statistics, power, counter,
  timeline and PGO-training scripts): it depends on my paths, the Deck's sysfs files and the Distrobox; it is not
  something to ship. I can share any of it if it helps.
- **Measured and parked as patches (not in the branch):** job 218's zero-weight blend shortcut, raw FK first pass and a
  native of 0x17280 (exact, -8.6 % per 218 job, no fps change: the pool time freed turns into idle spin); job 216's
  selection pass and vectorised BSP sides (exact, -10..-16 % per 216 job, no fps change); `PS3RECOMP_VK_MULTIDRAW`
  (neighbouring draws as one `vkCmdDrawMultiIndexedEXT`, exact, ~0.15 ms of RSX thread: not measurable); barrier
  batching on RADV (0 ms of GPU); an inline line-bit test for the store slow path (no gain, module +17 %); the main
  thread pinned to a core (slower); ThinLTO over the module (nothing). They follow the rule "within noise and no other
  benefit = not committed".
- **No Windows build was made or run** for this work.

## Known limits and watch items

- **The launcher defaults were measured only on the Steam Deck** (4 cores / 8 threads, 15 W shared by CPU and GPU).
  The job-thread count is therefore set on a Deck only (`board_vendor` = Valve, as `play.sh` already does for the render
  scale); every other PC keeps the engine's rule (physical cores - 2, at most 8). The other defaults apply to every
  Linux PC.
- **PGO on Linux.** My installed build uses profiles trained on my own Deck runs. A Linux player gets the normal build
  from `./setup.sh`, or the optimised one with the installer's "Optimise for this PC" (topic 19: +8.7 % on the quick
  street, +7.3 % on the plaza at the Deck's fresh-install settings).
- **Engine compiler flags.** My own builds compile the engine with `PORT_C_EXTRA="-march=znver2 -ffp-contract=off"`;
  `setup.sh` and the installer build it at upstream's flags. The difference was not measured on its own.
- **Job priority:** RSX semaphore waits are a little more frequent with it (2.2-4.8 a run vs 0.5-1.2, < 0.001 ms a
  frame); on the plaza the step-6 combination had 7 frames > 33 ms with it vs 2 without (1.4 SE; all 41-46 ms). Not
  seen in the standing runs.
- **The frame-stepped bench** removes timing noise but has its own artifacts (one ~60 ms frame at guest 16.4 s of the
  street in every run, the 100 ms stall rule): its p99 / > 50 ms numbers are not real-play numbers; the standing runs
  are.
- **Untested here:** Windows (see above), other Linux GPUs (only RADV / Van Gogh), Game Mode's gamescope for the
  benches (they run in Desktop Mode on X11; I play in Game Mode with the same builds).
- The x11 line in `play.sh` (topic 15) predates v0.6.3's own Wayland-to-x11 retry and may now be redundant.

## How to try it on a Steam Deck

1. Desktop Mode: the fork's release zip (v0.6.3-deck4 or later), unpacked into a new folder; double-click **Install
   inFAMOUS 2** (with Optimise for this PC), then **Play inFAMOUS 2**. Or from a clone of the branch: `./setup.sh`
   (the normal build) or `bash tools/install_gui.sh`.
2. GPU clock: Quick Access (`...`) > Performance > Manual GPU clock **1100 MHz** (see "GPU clock" above). If Steam's
   per-game profile is on for the game, set it there.
3. Play through `./setup.sh` / `tools/play.sh` as usual: the launcher sets the defaults of topic 15. To compare, any of
   them can be set from outside, e.g. `PS3_NATIVE_JOBS_PRIO=0 PS3RECOMP_ENG_R8=0 PS3RECOMP_VK_THREADED=0
   PS3_CPU_PLACE=0 tools/play.sh`; `PS3RECOMP_VK_LIGHT_CULL=0` (original light shader), `PS3RECOMP_WTRACK=0` (no
   write tracking).
4. F1 shows the fps counter (the HUD that is on in the video).

## Video

The side-by-side street video (upstream v0.6.3 vs this branch with PGO, both 1280x800, GPU 1100 MHz, HUD on) is in the
pull request's description and attached to the fork's v0.6.3-deck4 release.
