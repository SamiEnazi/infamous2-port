# Steam Deck performance work (branch `deck-perf`)

Status and lab notes of the Steam Deck optimisation work: what limits the frame rate, how it is measured,
what was changed, what was tried and rejected, and what is next. The raw chronological log is
`work/bench/NOTES.md` (local, not in git); this file is the curated summary and is kept up to date.

Hardware: Steam Deck OLED (Galileo, 90 Hz panel), Zen 2 4 cores / 8 threads, 15 W APU limit shared by CPU
and GPU. The player's GPU clock is set manually (1400 MHz at the time of writing); benchmarks run at 1200 MHz
and put the player's clock back afterwards.

## 1. Where the frame goes (umbrella plaza, spot-2 save, player's settings)

Measured with the deterministic benchmark (section 3), dev build 6b300ea, ~42 fps:

| Part | Busy | Time per frame | Notes |
|---|---|---|---|
| Graphics thread (`ps3-rsx`: GCM FIFO -> Vulkan) | 88-91% of a CPU | ~21.4 ms | co-limits the frame |
| SPU job threads (`ps3-spu0..5`) | ~400% of a CPU | ~96 ms | co-limits the frame |
| Main PPU thread (process thread) | ~78% of a CPU | ~19 ms | caps ~50 fps later |
| GPU (1200 MHz) | ~60% | ~15.3 ms | not limiting |
| APU power | 14.5-15.3 W | | at the 15 W cap: CPU clock ~2.9 GHz |

Both the graphics thread and the SPU side are at the frame limit; speeding up only one of them barely moves
fps (example: "Far shadow detail: Reduced" cuts the graphics thread 12% but fps only +2-3%, the SPU side
then limits). 60 fps needs every part about a third faster at the same 15 W.

SPU time by job (JOBSTAT, player settings): 218 animation 17.6%, 204 render-list builder 17.5%, 223 Edge
geometry 16.0%, 220 BSP/collision 11.9%, 210 culling 11.0%, 213 7.4%, 211 3.6%, 216 3.4%, 201 3.0%.
SPU time by kind (perf, default settings): runtime overhead ~22% (DMA ~12%, indirect branches ~6%,
coherency/lock-line ~2%), WWS job manager ~6.6%, the rest job code (natives and lifted).
~65,000 DMA commands per frame, average ~200-350 bytes, ~690 TSC cycles each (mostly cache misses on the
source data). Job 204: ~500 calls/frame, ~36 us wall each: native body ~10 us, publish ~1.5 us, the 8.6%
declined calls ~6.5 us average, the rest wrapper + job manager.

Graphics thread (perf, player settings): engine 75%, RADV (Mesa) 16.5%, libc 8%; `sink_end_body` 17.6%,
`vk_eng_draw_ex_body` 10.4% (memo / descriptor-set / texture table cache misses), vertex fetch 8.3% (fixed,
see below), dispatch 6.4%, FIFO walk 5.2%. ~2.8 us per draw, ~7,600 draws per frame.

## 2. Results so far (det bench, player settings, plaza)

| Build | fps | Graphics thread | SPU work |
|---|---|---|---|
| Baseline (installed 4cabf54 + measurement only) | ~40.7-41 | 22.1 ms | 99.1 ms |
| Dev 6b300ea | 41.8 | 21.4 ms | 96.1 ms |
| Dev 6b300ea + Far shadow detail: Reduced | 42.7 | 18.6 ms | 93.8 ms |
| j6 (b653037) vs t13d (job 204 types 1/3), 8 clean runs each | 41.70 -> 41.93 | 21.04 -> 21.34 ms (noise?) | 97.38 -> 96.26 ms |
| Upstream v0.6.2 (f746fc6 + measurement, no PGO), `ab62` 4 rounds | **30.10** | 32.2 ms | 60.6 ms |
| ... same batch: t13g (= 89ca87a) / fs3 (installed + measurement) | 41.83 / 41.02 | 21.0 / 21.9 ms | 96.0 / 98.2 ms |
| Merge deck-merge (deck-perf + v0.6.2, no PGO), `ab3` 4 rounds | **42.12** | 22.7 ms | 77.2 ms |
| ... same batch: dpNP (deck-perf, no PGO) / t13g | 38.33 / 42.13 | 23.3 / 21.0 ms | 103.9 / 95.8 ms |
| **Merged + new PGO (mgPr, 1f22354)**, `ab4` 8 rounds | **48.90** | 19.5 ms | 71.1 ms |
| ... same batch: mgP (training-only profile) / mg (no PGO) / t13g | 49.14 / 42.52 / 41.76 | 19.3 / 22.4 / 21.1 ms | 72.4 / 75.0 / 96.9 ms |

Real clock, standing at spot 2 (player's settings with the 60 fps cap, GPU 1400): mgPr 57.84 fps over 30 min (5-min
buckets 57.5-58.3, p99 19.4-19.8 ms, 17 frames > 33 ms, none > 50 ms, worst 38 ms); t13g 48.16 fps over 15 min (p99
24.1 ms, worst 70 ms). Memory grows ~0.8 MB/min in both (pre-existing, not from the merge).

Far shadow detail in normal play (2026-11-18, j6 build, real clock, 60 fps cap, player's settings and GPU clock
1400 MHz, HUD on, one 50 s run each of the plaza walk, `sh60full` / `sh60reduced` recordings): Full 34.9 fps,
8,151 draws per frame; Reduced 37.4 fps, 5,299 draws (+7%; one run each, so noisier than the det bench). The
visual difference is hard to spot at night in this save (side-by-side video sent to the player).

Installed (2026-11-18, the player asked): **mgPr** = deck-perf 1f22354 with the mg1r profiles (engine exe + game
module `libInfamous2Game.so` in `<player>/work/port/build/`). The player's `tools/play.sh` (v0.5.7 + the Deck tuning)
got four lines: GLIBC_TUNABLES static TLS, `PS3_STATIC_TLS_RETRY=1`, `PS3_NATIVE_JOBS_THREADS=4`,
`PS3RECOMP_VK_SHADER_CACHE=<player>/work/shader_cache` (seeded from the merge sandbox's cache; the old build's
`~/.cache/ps3recomp` stays untouched). Settings and saves stay where they were (`~/.config/infamous2-port/settings.ini`,
`game/gamedata`). Smoke test through play.sh: module loaded (PGO), runner 4 threads, wtrack on, rsxcore placement,
the process is `Infamous2Recomp`. Backups / rollback: `work/port/Infamous2Recomp.installed-4cabf54` (old exe; copy it
over `work/port/build/Infamous2Recomp`, the .so is then unused), `tools/play.sh.installed-4cabf54`,
`~/.config/infamous2-port/settings.ini.before-merge-20261118`, `saves-backup/before-merge-20261118/`.

Real play with mgPr installed (2026-11-18, Game Mode, ~30 min over 5 sessions, `<player>/play/20261118-17*`): the
player reports 50s-60 in many places and short dips into the 40s in busy areas. playlog.tsv, seconds in game:
>= 58 fps 11%, >= 55 18%, >= 50 36%, >= 45 57%, >= 40 80%; 48 frames > 33 ms and 2 > 50 ms per minute. fps falls with
the draw count: 1,500-3,000 draws 57.9 fps (p10 53), 3,000-4,500 54.3 (46), 4,500-6,000 50.8 (41), 6,000-7,500 46.9
(37), 7,500-9,000 42.3 (30), 9,000+ 36.7 (30). One crash at the first Game Mode start (17:21:32): SIGABRT "malloc():
unaligned tcache chunk detected" in SDL's malloc during the joystick scan (pad_sdl_scan <- pad_init_backend <- the
game's cellPad init; system libSDL2 = sdl2-compat over libSDL3): heap corruption found there, cause unknown (the
next start ran 18 min). Coredump: `coredumpctl info 756300`.

**Goal set by the player (2026-11-18): stable 60 fps, including busy areas.** Budget at 60 fps: 16.7 ms per frame
for each single thread (the RSX thread is ~2.56 us per draw now, so 9,000 draws need ~1.6 us per draw, -35%), and
all CPU work per frame (now ~110 ms: main 18.7, RSX 19.5, SPU 71) has to fit the 4 cores / 8 threads under 15 W
(roughly -30%). GPU: 15.1 ms/frame at 1200 MHz on the plaza, also close to the 16.7 ms limit. New benchmark zone
wanted by the player: the busy street of their current save, walking to the cathedral (save snapshot
`<player>/work/pgo/bench-saves/gamedata3`, 17:54).

## 3. How to measure

* `work/bench/detbench.sh <tag> [ENV=V ...]` -- the deterministic plaza run: `route-plazafs.pad` replayed on
  the frame-stepped guest clock (`PS3_TB_FRAMESTEP_US=33333`: the guest timebase moves exactly 1/30 s per
  flip once the route's anchor is reached; real time takes over after 100 ms without a flip so mftb
  deadlines cannot hang). Uses a copy of the player's `settings.ini` (`USERSET=0` = engine defaults), GPU
  1200 MHz during the run. Prints fps, per-thread CPU % (`budget.py`), GPU ms/frame
  (`PS3RECOMP_VK_GPU_PROF`), APU power. `EXE=<copy>` picks the binary.
* `work/bench/detab.sh <prefix> <runs> "label|ENV=V" ...` -- interleaved A/B; `EXE_<label>` per config.
  Prints ms/frame of the RSX and SPU sides (CPU% x 10 / fps) -- more precise than fps (+-1% within a
  batch). Flags `SUSPECT(cpu)` (SPU < 380%: something else used the CPU -- discard) and `SUSPECT(route)`
  (draws outside 7,300-8,100: the walk diverged, ~1 run in 7 -- discard; expected when a change cuts draws).
* `work/bench/detprof.sh <tag>` -- the same with `perf record` of the whole game (thread names in samples).
  Profile only fixed exe copies (`work/port/Infamous2Recomp.<tag>`): perf resolves symbols from the file.
* Job replays (exactness of SPU natives): `tools/jobreplay.sh <jobrec dir> <exe> 1 [K=V]`, recordings in
  `<player>/native/recordings/rec-210-218`, `rec-220-211-208`, `rec-223`. `PS3_JOBREPLAY_REPEAT=N` for
  timing (replay times are dominated by the harness: compare natives on/off, not absolute).
* Live checks: `J218_CHECK=1` (blend vs lifted), `J218CLIP_CHECK`, `J210_CHECK`, `J220_CHECK`.
* Job 204 switches: `PS3RECOMP_NATIVE_J204=0` (all off), `J204_TYPES=<mask>` (types the native takes, default
  0xF; 0x5 = only 0 and 2 as before), `PS3RECOMP_J204_BREAK=1/2` (checker self-test), `PS3_JOBREPLAY_FULLPUT=1`
  (replay compares whole 4096-byte command blocks).
* Statistics switches: `PS3_JOBSTAT=1` (per-job time), `PS3_SPU_DMASTAT=<s>` (DMA per job overlay) with
  `PS3_SPU_DMASTAT_OVL=<id>` (that job's DMA sites), `J204_STATS=<n>` (declines by reason + native cycles),
  `J218_STATS`, `J210_STATS`, `PS3RECOMP_ENG_PASSSTATS`, `PS3RECOMP_VK_CALLSTATS`.
* Causal test: `PS3_DBG_FLIP_DELAY_US` / `PS3_DBG_PRESENT_DELAY_US` add a busy wait per frame to the main /
  graphics thread; fps unchanged = that thread has slack.
* Build (dev tree D, player tree E for the toolchain; ~4-5 min; `--only compile` for engine changes,
  `--only spu,jobs,compile` after natives / natives.txt; errors: `python3 -I tools/build_errors.py
  work/logs/compile-build-engavx2.log`):

      distrobox enter infamous2-build -- env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        PORT_LLVM="$E/tools-cache/llvm" PORT_CMAKE="$E/tools-cache/cmake/bin/cmake" \
        PORT_NINJA="$E/tools-cache/ninja/ninja" PORT_SDL2_DIR="$E/deps/sdl2-cmake" \
        PORT_PGO="use:$E/work/pgo/mg1r-module.profdata" PORT_ENGINE_PGO="use:$E/work/pgo/mg1r-engine.profdata" \
        PORT_JOBS=3 PORT_LIFT_JOBS=2 \
        PORT_BUILD="$D/work/port/build-<name>" PORT_C_EXTRA="-march=znver2 -ffp-contract=off" \
        bash -c "cd '$D' && '$D/work/venv/bin/python' -I tools/rebuild.py --from port --only compile"

  Then copy the engine exe AND `libInfamous2Game.so` (side by side) to `work/port/<tag>/` before benchmarking or
  profiling. Since the merge (1f22354): `PORT_PGO` = the game module, `PORT_ENGINE_PGO` = the engine (deck3.profdata
  is for the old single exe; before 1f22354 the build dir was build-engavx2 with `PORT_PGO=use:deck3.profdata`).
  `--from port` skips the inputs step, which would copy onto `work/EBOOT.elf` (a symlink into the player's tree).
  Profile warnings: engine `work/logs/compile-<builddir>.log`, module `<build>/module-obj/compile.log`
  ("PGO: N function(s) changed since the profile"). Retrain: `<merge tree>/work/bench/pgotrain-mg.sh` (instrumented
  build with `PORT_PGO=gen PORT_ENGINE_PGO=gen`, `LLVM_PROFILE_FILE=<dir>/<run>-%p-%m.profraw`, clean exit per run)
  and `pgomerge-mg.sh` (training x3 + deck.profdata real play).
* Running a merged build (engine + module): `GLIBC_TUNABLES=glibc.rtld.optional_static_tls=1048576
  PS3_STATIC_TLS_RETRY=1` (else it re-execs and shows up as "exe", invisible to `pgrep -x Infamous2Recomp`, budget.py
  and the lock checks), `PS3_NATIVE_JOBS_THREADS=4` (play.sh sets both; the exe default is 2), and for benchmarks the
  sandbox env (`<merge tree>/work/bench/mgenv.txt`: `PS3RECOMP_VK_SHADER_CACHE`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`)
  so dev builds never write the player's config or the shared shader cache. Bench copies that do all this:
  `<merge tree>/work/bench/detab-m.sh`, `ab4.sh`, `stand.sh` (real-clock standing run), `valP.sh` (all replays).
  Fast builds leave the det route more often (5 of 14 PGO runs at the house wall near Hank's at anchor+5-7 s; the
  > 50 ms frames of those runs come after the divergence): run more rounds and discard by draws.
* Shell pitfall: never `pkill -f` / `pgrep -f` a pattern that also appears in the waiting command itself
  (it matches its own shell). Wait on a file's line count instead.

### Acceptance rules (every change, agreed with the player 2026-11-18)

1. Exact: SPU natives match the lifted job bit for bit (job replay exit 0, live checks 0 differences); graphics
   changes give the same image (det-bench screenshots).
2. Faster: interleaved det A/B (>= 3 clean runs per side) shows the targeted side (SPU or RSX ms/frame) lower
   and fps not lower. Within noise and no other benefit = not committed.
3. Not less stable: frame-time p99 and the number of frames over 33 ms (each run's playlog.tsv) not worse, no
   crash or hang in the runs; anything that touches threads or timing also gets a long standing run.
4. Behind a switch: each change can be turned off by an environment variable / menu setting (rollback without
   a rebuild), and the commit can be reverted alone.
5. A change that fails 1-3 is reverted and listed in section 5 with its numbers, so it is not retried blindly.
6. The installed build only changes when the player asks; the previous exe is kept as a backup.

## 4. Changes on deck-perf (newest first)

| Commit | Change | Measured |
|---|---|---|
| 1f22354 | merge of deck-merge: upstream v0.6.2 (native WWS job runner, whole-job 208/209/210, their 218) + 0381235 Linux PGO-gen module atexit, 60870c0 4 pool threads, 865ef66 `J218_PICK=ttt`, 36b24fb static TLS, f9bbfd8 pool names, b529db1 Far shadow in their 210, 5dfb0b2 TLS scratch; new profiles mg1r | **48.90 vs 41.76 fps (+17.1%)**, RSX -1.6, SPU -26 ms/frame, p99 23.8 vs 27.4; exact |
| 56427dc | job 204 type 0 fix: SLOT(12) lanes y..w were wrong in 231 of 517 slot-12 calls of rec-204b (also in the installed build); block buffers compared for every type; FB_RAND / FB_STALE guards | exact; replay time unchanged |
| 0183e2a | job 204 native: types 1 and 3 (sub-draw lists 0x69B0, type 1 slots 5/7), `J204_TYPES` switch, `PS3_JOBREPLAY_FULLPUT` | SPU -1.1%, fps +0.55% |
| b653037 | J204_STATS by job type | (tools) |
| 6b300ea | job 204 output PUTs via the lean DMA path; J204_STATS cycles; DMASTAT_OVL | small |
| 89e52ae | in-game "Far shadow detail" (`PS3RECOMP_SHADOW_LOD`, job 210 far cascade K.y x20) | RSX -12%, fps +2-3% |
| eed8799 | constant upload 8 words per step (AVX2); vertex remap pass vectorises | RSX -0.7% |
| b4eba3e | vertex fetch builds vertices in registers (store-forwarding stall); natives' ea_ok inline | RSX -5.3% |
| a77dd5a | job 218 blend: two bone groups per pass on AVX2 (exact, 850k live calls 0 differ) | SPU -2.2% |
| cb44e50 | deterministic clock, thread names, DMA / job 204 statistics | (tools) |
| 4cabf54 and older | see git log (natives from the Switch fork, job 210/218/220/223 natives, draw offsets...) | |

## 5. Tried and rejected (do not retry without a new reason)

* `PS3RECOMP_EDGE_GPU=1/2` (GPU culls instead of Edge): SPU -1%, RSX +8 points. No.
* `PS3RECOMP_RSX_SPLIT=1` (walker + exec threads, exec on the RSX core's SMT sibling): -19% fps.
* Threaded Vulkan recording / VK_BT, `PS3_CPU_PLACE=rsxcpu`: slower on the Deck.
* `J210_KZ_SCALE` / `J210_KX_SCALE` (camera-view detail knobs): no gain (K.z does not cut the camera view).
* `PS3RECOMP_SSAO=1` vs 2: GPU -0.6 ms only; GPU is not the limit.
* Packed key arrays for the index / vertex caches: RSX +1 ms (an unprefetched miss in front of the
  prefetched entry). Tag arrays for the descriptor-set memo / table: no gain.
* `PS3RECOMP_NATIVE_J220=1` (exact BSP native): no gain live.
* Job 204 SLOT(12) root cause (for the record): main (0x12C60) calls 0x7C58 per passing object, whose prologue
  saves main's r112 at main sp-0x210 = the type 0 handler 0x11580's sp+0x30, which slot 12 loads (only x
  inserted). r112 = rotqbyi(E,12) of the last fetched material; the item loop is pipelined, so item k sees item
  k+1's quad. The native had read entry sp-0x970 / r10.
* Job 204 type 4 natively: ~6,000 instructions of handler for 1.2% of job 204 calls -- not worth it (2026-11-18).
* `SPU_LIFT_XFN_BI` (cross-function const `bid/bie` as tail calls): -35..40% fps measured upstream.
* Huge pages: guest memory already mostly THP; TLB misses < 1% of cycles.
* Vectorising frest/frsqest: the compiler already uses gathers.
* GPU clock (2026-11-18, player settings, plaza): 1000 MHz 41.8 fps > 1100 41.2 > 1200 40.1 > 1400 38.2.
  Higher clocks take power from the CPU (at 1400 the RSX thread needs 23.2 ms and the SPUs 105 ms per frame,
  at 1000 20.7 and 95.6). Recommended: manual 1000-1100 MHz. Benchmarks stay at 1200 for comparability.
* Lower scheduling priority for the SPU threads (renice +3): no effect.

## 6. Next candidates (expected gain, effort)

Order after the merge (2026-11-18): the merged build is RSX-thread bound (~95% of a CPU, pool threads 75-78%), so
(a) a fresh "where the frame goes" breakdown on mgPr (section 1 is from the old build), then (b) RSX-thread work
(item 4 push constants, item 6, new hot spots), then (c) SPU leftovers: job 223 Edge (~19 ms/frame, now the biggest
job), 218, 204 declines (item 2), 220 / 213 / 216; (d) Edge on GPU only with a new idea (section 5); (e) main thread,
GPU / power. Items 0, 1, 1b and 3 are done.

0. (player's request, 2026-11-18, next agent after the job 204 fix) Test upstream v0.6.x separately: upstream
   (origin = SamiEnazi/infamous2-port) went v0.5.7 (our base, a9716880) -> v0.6.0/0.6.1/0.6.2 (f746fc6): 171 files,
   +21.9k lines; claims +30% fps at 4 cores / 8 threads and +75% at 4c/4t on an i5-13600K (desktop, Windows), slowest
   1% not better. It includes a native WWS job manager (engine/libs/spurs/wws_runner.c, wws_native.c, wws_njob.c),
   native jobs 208/209/210 + cull_core.h, native_218.c, spu_avx2.h, inf2_dlogic.c, vm_guard.c, sharper shadows
   (rsx_shadow_res.inc), MSAA (off by default). Plan: separate worktree + build dir; add only our measurement commit
   (cb44e50's engine part: frame-stepped clock, thread names, DMASTAT) so the det bench applies; same plaza, same
   player settings, GPU 1200; compare against the dev build (and the installed 4cabf54). If upstream is clearly
   faster on the Deck, move the Deck work onto upstream (our natives 204/210/218/220/223 vs theirs: keep the faster
   exact one per job).

   **Result (2026-11-18): upstream v0.6.2 is 28% slower on the Deck (30.1 vs 41.8 fps), but its job side is much
   cheaper.** Tree `infamous2-port-upstream`, branch `upstream-test` (no upstream tracking): f746fc6 + e88e22c (our
   measurement engine part, Linux thread names only, no placement) + d4450b2 (`PORT_C_EXTRA`), no PGO, znver2;
   copy in `work/port/up62/` (engine exe + `libInfamous2Game.so`). v0.6 needs
   `GLIBC_TUNABLES=glibc.rtld.optional_static_tls=1048576 PS3_STATIC_TLS_RETRY=1`, else it re-execs and shows up
   as "exe" (invisible to `pgrep -x Infamous2Recomp`, budget.py, the lock checks). Isolated with
   `PS3RECOMP_VK_SHADER_CACHE` / `XDG_CONFIG_HOME` / `XDG_CACHE_HOME` in `U/work/sandbox`. Upstream picks
   `PS3RECOMP_SHADOW_RES=1` on the Deck by itself (1024 maps, = ours); its FBF1X / BARBATCH are off on AMD.

   | ab62 (4 rounds) | fps | main / rsx / spu ms/frame | GPU | W | p99 | >33 ms per run |
   |---|---|---|---|---|---|---|
   | upstream | 30.10 (sd 0.41) | 28.6 / 32.2 / 60.6 | 16.0 | 15.9 | 38.2 | ~810 |
   | t13g | 41.83 (sd 0.84, run 1 off-route) | 18.7 / 21.0 / 96.0 | 15.2 | 15.0 | 27.9 | 4 |
   | fs3 | 41.02 (sd 1.71) | 19.0 / 21.9 / 98.2 | 15.2 | 14.9 | 28.3 | 3.8 |

   * Upstream's limit is the RSX thread (97% of a CPU). Decomposition (`abx62`, 2 runs each): t13g with
     `PS3RECOMP_WTRACK=0` 33.75 fps / RSX 28.2 ms (upstream's `rsx_wtrack.c` is Windows-only); t13g with
     `PS3_CPU_PLACE=off` 37.8 fps / RSX 24.9 ms (upstream has no Linux placement); upstream pinned with taskset
     31.45 fps / RSX 30.6 ms; the rest (~2 ms) = PGO + our RSX commits. Upstream with 4 pool threads 30.05 fps.
   * Job side: SPU CPU 60.6 vs 96.0 ms/frame. Upstream runs 2 pool threads (87-100% busy) plus jobs on the main
     thread in its waits (7.5-10 of its 28.6 ms). Per job (`js62`, ours -> upstream, ms/frame): 218 20.5 -> 15.2,
     223 19.1 -> 15.5, 204 16.6 -> 11.2 (theirs = v0.5.7's native), 220 13.1 -> 9.1, 210 11.6 -> 2.4 (whole-job
     native), 213 8.3 -> 5.7, 208 2.1 -> 0.4, 212 2.0 -> 1.0, 209 0.85 -> 0.2; all 109 -> 68-74. Caution: same-code
     jobs being 25-40% shorter there is partly the setting (2 threads vs 4, so less SMT / cache sharing, and 30 fps
     instead of 42); only a merged build tells how much is real.
   * Merge dry-run (deck-perf + f746fc6): text conflicts in 2 files only, `vk_overlay.cpp` (our Far shadow row vs
     their MSAA rows; their menu moved to `vk_overlay_menu.inc`) and `vk_threaded_rec.h` (1 include hunk). Overlaps
     git does not see: job 218 entries 0x16320 / 0x17780 / 0x18B80 registered twice (our job218_native.c, their
     native_218.c); with their runner on, 208/209/210 run as whole-job natives (our nx_job208/209 and the j210
     walker unused; their job212.c is a stub, 212 stays lifted + our nx hook); their 204 = v0.5.7 (ours kept);
     201/211/220 ours only. The module split needs PGO
     retrained (`PORT_PGO` = module, `PORT_ENGINE_PGO` = engine) and the GLIBC_TUNABLES line in play.sh / bench.
   * Decision: do not install v0.6.2. Try moving the Deck work onto upstream in a test branch (item 1).

1. Merge test (next agent): deck-perf + upstream f746fc6 in a separate worktree / branch, keep our Linux RSX
   pieces (write tracking, placement, RSX commits), place their pool threads, per-job A/B ours vs theirs (exact
   only), pool thread count 2/3/4. Accept only if it beats t13g by the acceptance rules; then PGO retrain on the
   winner (item 3).

   **Result (2026-11-18): the merge wins against its fair baseline; it ties t13g without PGO.** Branch `deck-merge`
   (tree `infamous2-port-merge`, pushed to fork): 93a4593 merge, 5dfb0b2 job 218 check scratch on the heap (module
   static TLS), f75d36e + 865ef66 `J218_PICK=<blend><pose><palette>` (o = ours, t = theirs, default `ttt`: -13% per
   job 218, replay 134.5 vs 150 us), b529db1 Far shadow detail in their whole-job 210 (`g_j210_shadow_lod_k`),
   f9bbfd8 runner pool threads named `ps3-spuj<n>` (SPU role: CPUs 0-5, bench group "spu"), 36b24fb + 60870c0
   play.sh (GLIBC_TUNABLES line; `PS3_NATIVE_JOBS_THREADS=4` unless set, the exe default is still 2). Exe copies
   `work/port/mg/` (engine + module) and `work/port/Infamous2Recomp.dpNP` (deck-perf 306f6b1, no PGO).
   * Native registry: `spu_native_entry` runs the first registered native for an address (module link order), a
     declining one falls to the lifted code; a second native is never tried. Their whole-job natives:
     `PS3_NATIVE_CULL=0|1|2|<list>` (default 1). Their runner runs jobs on host SPU contexts through the same
     native hooks and MFC / atomic paths, so our per-function natives (204, 218 extras, 201/211/212, 216, 220,
     223) run under it; job 204 accepts 94.5% live as before. Our wtrack, rsxcore placement, frame clock live.
   * Choices (2 runs each): pool threads 2 / 3 / 4 = 37.75 / 40.75 / 41.00 fps (4); main thread running jobs in its
     waits on vs off 41.7 / 41.9 (kept on, as upstream); 210 their whole-job vs our walker: SPU 74.9 vs 83.9 ms
     (theirs); 208/209 theirs vs our nx: equal (theirs, replay-exact).
   * Exact: replays rec-204b/sel (FULLPUT) 1960/1960, rec-210-218 210 100/100 and 218 102/102 (ooo and ttt),
     rec-223 150/150, rec-220-211-208 211 / 220 100/100; new recording `mgrec` (1,800 jobs by the merged runner,
     natives off): 204 / 209 / 210 / 212 / 218 300/300 each, their 208 / 209 / 210 natives 300/300. Live checks:
     their 218 0 of 3.8 M; their verify 210 0 of 89k, 208 0 of 80k, 209 0 of 26k (209 verify only with our nx_209
     off: it compares an LS area nx_209 does not write). Our nx_208 fails every replay in t13g / dpNP too (replay
     tool limit, not new; unused under the runner). Screens: same spot, view, shadows, lighting as t13g; the window
     is 1152x718 (upstream's), so no pixel diff. 32 merged-build runs, no crash or hang.

   | ab3 (4 rounds) | fps (sd) | main / rsx / spu ms/frame | GPU | W | p99 | >33 ms per run |
   |---|---|---|---|---|---|---|
   | mg | 42.12 (0.54), 4 clean | 20.48 / 22.68 / 77.2 | 15.36 | 14.4 | 27.4 | 2.2 |
   | dpNP | 38.33 (0.74), 3 clean | 20.94 / 23.34 / 103.9 | 15.23 | 14.7 | 30.3 | 16.0 |
   | t13g | 42.13 (0.86), 3 clean | 18.60 / 20.99 / 95.8 | 15.19 | 15.0 | 27.7 | 6.7 |

   (I recomputed p99 / >33 ms from the playlogs: identical.) mg vs dpNP (both without PGO): fps +9.9%, SPU -25.7%,
   total CPU -18.5%, p99 30.3 -> 27.4 ms, frames > 33 ms 16 -> 2.2 per run. mg is now RSX-bound (95.5% of a CPU,
   pool 75-78% busy); its RSX +1.7 ms vs t13g is the PGO gap (dpNP 23.34 vs t13g 20.99). Per job (js1, mg vs
   dpNP): real savings are the whole-job natives (210 11.9 -> 3.2, 208 2.2 -> 0.5, 209 0.9 -> 0.2 ms/frame), their
   218 (19.5 -> 18.0) and the runner on short jobs with the same code (204 34 -> 25 us per job, 213 -20%, 201 -25%,
   212 -36%); 223 / 216 / 220 / 211 savings seen in the upstream build were its 2-thread, 30 fps setting.
   Total job time 112.5 -> ~92 ms/frame. Risks: Windows build of the module lacks import slots for our natives'
   extra engine calls (untested); v0.6's play.sh moves settings / mods / spots / shader cache into the port folder
   (matters at install).
   * Decision: continue on deck-merge. Next (item 1b): PGO retrain on it, long standing run (rule 3), final A/B vs
     t13g; if it wins, deck-merge is merged into deck-perf.

1b. PGO on deck-merge (next agent): train module (`PORT_PGO`) and engine (`PORT_ENGINE_PGO`) profiles on the
   merged build (work/bench/pgotrain3.sh adapted, new profile names, the 4-thread runner), rebuild, check the
   stale-profile warnings are gone, det A/B mg-PGO vs mg vs t13g (>= 4 rounds), a long standing run (30+ min,
   real clock, player's settings), the replays again on the PGO build.

   **Result (2026-11-18): accepted, merged into deck-perf (1f22354).** Profiles E/work/pgo/mg1-{module,engine}.profdata
   (training: spot 2 spin, spot 1 standing, video A/B, plaza) and mg1r-* (training x3 + deck.profdata real play; only
   529 module / 188 engine functions gain from real play, most lifted functions changed their hash in v0.6). Stale
   warnings: old t13g 13 functions (11 hot); mg1 / mg1r: module 0, engine 1 (`main`, cold). Replays on mgP and mgPr:
   the same pass table as mg (208 0/100 = the known nx_208 replay limit, 100/100 with `PS3RECOMP_NATIVE_J207=0`).

   | ab4 (8 rounds) | clean | fps (sd) | main / rsx / spu ms/frame | GPU | W | p99 | >33 / >50 ms per run |
   |---|---|---|---|---|---|---|---|
   | mgPr | 4 | 48.90 (0.35) | 18.70 / 19.47 / 71.1 | 15.10 | 14.6 | 23.8 | 1.0 / 0 |
   | mgP | 5 | 49.14 (0.32) | 18.89 / 19.33 / 72.4 | 15.19 | 14.9 | 24.2 | 1.8 / 0.6 |
   | mg | 6 | 42.52 (0.55) | 20.27 / 22.44 / 75.0 | 15.46 | 14.5 | 26.7 | 1.8 / 0.2 |
   | t13g | 5 | 41.76 (0.71) | 18.78 / 21.11 / 96.9 | 15.26 | 15.0 | 27.4 | 2.4 / 0 |

   (I recomputed every run's fps / p99 / >33 ms from the playlogs: consistent.) Discarded (off the route at the
   Hank's wall): mgP-3, mgPr-3/5/6/8, mg-7. mgPr and mgP are equal within noise; mgPr chosen (real-play coverage,
   long run done on it). Still RSX-bound (~95%). Risks: profile covers 5 city routes (combat / missions / cutscenes
   cold: a real-play session with the instrumented build would fix it); Windows module build untested; v0.6's
   play.sh moves settings.ini / mods.ini / spots.ini / the shader cache into the port folder (install step).

2. Job 204 remaining declines (5.5% of calls, t13d): type 4 (18.7k per run, handler ~3,650 instructions + ~2,600
   in callees: poor value), type 1 slots 6/8/9 (5.9k; 0x11080 sRGB->linear pow 2.4 and 0xA4A8), FB_STALE (material
   reads past the fetched material, 14.7k type 0 + 10.9k type 1 + 6k type 2), FB_FPUPLOAD 26.6k, FB_OP11B 20.7k.
   Recording: `<player>/native/recordings/rec-204b/jobrec` (8,000 calls), subsets `sel` (1,960), `sel0..sel3`.
   Details of the handlers and of 0x69B0 / 0x4740 / 0x5F38: work/bench/NOTES.md (2026-11-18 entries).
3. (superseded by 1b on deck-merge) PGO profile is stale: 13 hot functions now compile without profile data (job 218 blend_group, job 204 run /
   publish / emit_vec, j210_walker, f_7C18). Retrain (work/bench/pgotrain3.sh, new profile file name) and A/B.
4. Graphics thread: per-draw constant offsets as push constants (descriptor set only rebound when textures
   change): estimate revised to ~3% RSX (RADV already passes dynamic UBO offsets through its push-constant
   area, so the saving is only the bind/flush difference). Needs the HLSL generators (rsx_vp_decompiler.c
   emits `vp_c[(N + aK.c) & 511u]` / `vp_c[N]`, rsx_fp_decompiler.c `fp_constants[N]`, plus vp_posscale,
   vp_posoffset, fp_alpha; the same HLSL feeds D3D12/Metal, so Vulkan-only), the guest set layout
   (rsx_vulkan_backend.c ~3410, bindings VPCONST/PSCONST UNIFORM_BUFFER_DYNAMIC), 7 bind sites and a
   shader-cache key bump. Validate with det-bench screenshots. Low priority for the gain.
5. Job 218 FK / clip sampler widened like the blend: ~0.5-1% SPU.
6. Graphics thread +1.4% with t13d (not consistent between batches): check with a longer A/B.
7. Large, risky: WWS job manager natively (~7% SPU); Edge geometry on the GPU (~16% SPU).
