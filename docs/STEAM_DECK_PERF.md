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

The installed build is still 4cabf54 (the player's safe version; install only when asked).

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
        PORT_PGO="use:$E/work/pgo/deck3.profdata" PORT_JOBS=3 PORT_LIFT_JOBS=2 \
        PORT_BUILD="$D/work/port/build-engavx2" PORT_C_EXTRA="-march=znver2 -ffp-contract=off" \
        bash -c "cd '$D' && '$D/work/venv/bin/python' -I tools/rebuild.py --only compile"

  Then copy the binary to `work/port/Infamous2Recomp.<tag>` before benchmarking or profiling it.
* Shell pitfall: never `pkill -f` / `pgrep -f` a pattern that also appears in the waiting command itself
  (it matches its own shell). Wait on a file's line count instead.

## 4. Changes on deck-perf (newest first)

| Commit | Change | Measured |
|---|---|---|
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
* `SPU_LIFT_XFN_BI` (cross-function const `bid/bie` as tail calls): -35..40% fps measured upstream.
* Huge pages: guest memory already mostly THP; TLB misses < 1% of cycles.
* Vectorising frest/frsqest: the compiler already uses gathers.
* GPU clock (2026-11-18, player settings, plaza): 1000 MHz 41.8 fps > 1100 41.2 > 1200 40.1 > 1400 38.2.
  Higher clocks take power from the CPU (at 1400 the RSX thread needs 23.2 ms and the SPUs 105 ms per frame,
  at 1000 20.7 and 95.6). Recommended: manual 1000-1100 MHz. Benchmarks stay at 1200 for comparability.
* Lower scheduling priority for the SPU threads (renice +3): no effect.

## 6. Next candidates (expected gain, effort)

1. Job 204: model the declined object types: FB_TYPE = job types 1, 3, 4 (about equal shares, ~6% of all
   calls together; the native handles types 0 and 2), plus FB_FPUPLOAD 1.7%, FB_STALE 1.3%, FB_OP11B 1.3%
   of calls. ~1.5-2% SPU; needs new job 204 recordings with those types for the replay check.
   Dispatch: type at IN+0x384, jump table LS 0x139A4 (module blob loads at LS 0x4000): type 1 -> 0x13B68,
   3 -> 0x13C78 (item loop 0xBB60), 4 -> 0x13CB0 (item loop 0xC368); type 2 = 0x13C4C (loop 0xBCD0).
2. Graphics thread: per-draw constant offsets as push constants (descriptor set only rebound when textures
   change): estimate revised to ~3% RSX (RADV already passes dynamic UBO offsets through its push-constant
   area, so the saving is only the bind/flush difference). Needs the HLSL generators (rsx_vp_decompiler.c
   emits `vp_c[(N + aK.c) & 511u]` / `vp_c[N]`, rsx_fp_decompiler.c `fp_constants[N]`, plus vp_posscale,
   vp_posoffset, fp_alpha; the same HLSL feeds D3D12/Metal, so Vulkan-only), the guest set layout
   (rsx_vulkan_backend.c ~3410, bindings VPCONST/PSCONST UNIFORM_BUFFER_DYNAMIC), 7 bind sites and a
   shader-cache key bump. Validate with det-bench screenshots. Low priority for the gain.
3. Job 218 FK / clip sampler widened like the blend: ~0.5-1% SPU.
4. Large, risky: WWS job manager natively (~7% SPU); Edge geometry on the GPU (~16% SPU).
