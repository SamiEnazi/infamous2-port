# Steam Deck performance work (branch `deck-perf`)

Status and lab notes of the Steam Deck optimisation work: what limits the frame rate, how it is measured,
what was changed, what was tried and rejected, and what is next. The raw chronological log is
`work/bench/NOTES.md` (local, not in git); this file is the curated summary and is kept up to date.

Hardware: Steam Deck OLED (Galileo, 90 Hz panel), Zen 2 4 cores / 8 threads, 15 W APU limit shared by CPU
and GPU. The player's GPU clock is set manually (1400 MHz at the time of writing); benchmarks run at 1200 MHz
and put the player's clock back afterwards.

## 1. Where the frame goes (umbrella plaza, spot-2 save, player's settings)

Measured with the deterministic benchmark (section 3) on the merged build mgPr (1f22354 + mg1r PGO), 48.9 fps
(ab4), 2026-11-18 (agent rsx1; raw data in work/bench/NOTES.md):

| Part | Busy | Time per frame | Notes |
|---|---|---|---|
| Graphics thread (`ps3-rsx`: GCM FIFO -> Vulkan) | ~95% of a CPU | 19.5 ms | the limit; 2.56 us per draw at ~7,600 draws |
| Main PPU thread (process thread; runs jobs in its waits) | ~91% of a CPU | 18.7 ms | next limit (95% once the RSX thread is faster) |
| Job pool (`ps3-spuj0..3`, 4 threads) + SPU threads | ~350% of a CPU | 71.1 ms | pool 75-84% busy: not limiting |
| GPU (1200 MHz) | 66-79% | 15.1 ms | not limiting (17.4 ms with the player's 17:42 settings: aniso 16, soft shadows 2) |
| APU power | 14.6 W | | at the 15 W cap |

Job time per frame (runner stats, 70.5 ms): 223 Edge 15.6, 218 animation 14.2, 204 render lists 11.6 (504 jobs x
23 us), 220 BSP 8.5, 213 5.0, 216 3.3, 210 2.9, 201 2.6, the rest < 1.4 each.

Graphics thread (perf, 19,703 samples on `ps3-rsx`): engine 74.3%, RADV 17.3%, libc 8.1%. Per draw: FIFO walk +
method dispatch ~0.49 us (rsx_dispatch.c 13.6%, cellGcmSys.c 5.7%), draw engine ~0.97 us (sink_end_body 24.3% with
dc_fetch / index-cache probe / textures / delta engine inlined, vertex fetch 4.75%, delta 5.6%), our Vulkan backend
~0.39 us (vk_eng_draw_ex_body + set memo 10.9%), RADV ~0.44 us (draw-time state flush 2.0% + vkCmdDrawIndexed 1.5% +
vkUpdateDescriptorSets 1.8% (~780 new sets a frame) + vkCmdBindPipeline 0.9% + small helpers), libc ~0.21 us (memcpy
5.2%: VS constant rows into the uniform arena 1.5%, staged vertices / indices 1.4%; memcmp 2.3%: delta and set keys).
Hottest single line: the index-cache probe's key compare (2.0%, a miss: the successor prefetch is right for only 57%
of probes, because several passes draw the same index lists). Vulkan calls per frame: one vkCmdBindDescriptorSets
per draw (two dynamic offsets), ~700 vertex / 150 index buffer binds, ~415 pipeline binds, ~770 descriptor sets made.

Found with this breakdown: under the player's 16:10 picture (PS3_ASPECT_HUD_AFTER set by the port) the short tail
for repeated draws (rsx_draw_fast.inc, ~78% of draws at 16:9) was off for every draw; mgPr and every det bench so far
ran the long tail. Fixed behind `PS3RECOMP_ENG_FASTDRAW_HUD` (section 4 / 6).

(Old build 6b300ea, ~42 fps: graphics thread 21.4 ms, SPU threads ~96 ms, main ~19 ms, GPU 15.3 ms; both the
graphics thread and the SPU side were at the limit then.)

### GPU on the street (2026-11-19, agent gpu2; NOTES.md "GPU cost on the street")

hd2 with the player's 17:42 settings snapshot, GPU 1200 MHz, `PS3_TB_FRAMESTEP_IDLE_US=2000`, measured with the
measurement build gpu2m (`PS3RECOMP_VK_GPU_PROF=2`: every timestamp interval labelled with the pass it belongs to -- an
engine pass by its targets and their guest addresses, a backend pass by its kind -- with its draws; patch
work/bench/gpu2-prof.patch, summed by work/bench/gpu2pass.py) over guest 4-20 s of the street walk (the stretch every run
shares): **17.3 ms of GPU per frame**.

| Pass (what it is) | GPU ms per frame | Share | Notes |
|---|---|---|---|
| Light pass (backend: the game's SPU light job, done on the GPU over the whole screen) | 5.05 | 29 % | every light of the frame (83-129 on the street) for every pixel; **~0.75 ms with the light tiles (section 2)** |
| G-buffer: 3-target passes at 0x00AB8000 (~1,960 draws, 2.24 ms), 4-target decal passes (~455 draws, 0.46), rest | 2.93 | 17 % | |
| Sun shadow maps 2048x2048 (Shadow quality High; 3 cascades, ~4,500 depth-only draws) 1.40 + their R32F copies for the shadow receivers (3 x 0.23) + gaps | 2.15 | 12 % | the copies exist because the zeta is a 1280-wide buffer and the shadow texture its 1024x1024 corner |
| Forward / composite into 0x011C0000 (~800 draws, 6.8 passes) | 1.77 | 10 % | |
| Post buffers 0x003B0000 / 0x00734000 (~50 draws each) | 1.35 | 8 % | the game's post chain (depth of field / edge blur / glow composite) |
| SSAO "Full, high" (full-res depth 0.08, 24-sample AO 0.86, blur h 0.14, blur v 0.14) | 1.22 | 7 % | |
| FXAA + CAS sharpening at present 0.54, window blit 0.07, present gap 0.08 | 0.69 | 4 % | |
| Depth-as-RGBA8 decodes (4.9 a frame) + an R32F depth copy | 0.56 | 3 % | |
| Small passes: half-res depth export 0.23, 0x00B9E000 0.17, 0x00E3C000 0.17, a 1280x180 pass 0.15, water reflection 256x256 0.24, glow / blur chain 320x180 .. 20x11 0.37, gaps | ~1.4 | 8 % | |

Barriers: ~100 full + ~115 image barriers a frame; leaving the redundant image ones out (`PS3_RSX_BARBATCH=1`) saves no GPU
time (section 5). The street's draws per frame (PS3RECOMP_ENG_PASSSTATS): ~7,800, of which ~4,700 depth-only into the sun
shadow zeta 0x0F2F0000 (cascade runs ~4 / 195 / 950 / 3,510), G-buffer 1,340-1,550 + 200-270 (4 targets), forward ~1,050.

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
| mgPr + short tail at 16:10 (`PS3RECOMP_ENG_FASTDRAW_HUD`, 04ddd13, rsx1fdf), `rb4`+`rb5` (player's settings of 17:42: aniso 16, soft shadows 2; GPU 17.4 ms) | **49.75** | 18.87 ms | 74 ms |
| ... same batches: mgPr | 48.03 | 19.61 ms | 73 ms |
| hd1 = HEAD e0c7e3d (04ddd13 + 8833293 SDL fix, mg1r), new tooling `ZONE=plaza detstreet.sh` (guest 16-100 s), `zp`+`zp2` 3 clean of 7 | **50.12** | 18.69 ms | 73.1 ms |
| ... same batches: mgPr, 3 clean of 7 | 48.08 | 19.78 ms | 72.5 ms |
| **hd2** = 08b19a7 (code of 3d974be: hd1 + 365fd60 help peek + 626ab72 bench clock switch) + **mg2r** profiles (R1 / M6, short tail trained), `p2z`+`p2z2` 8 rounds, label order rotated, 6 clean of 8 | **50.65** (sd 0.61) | 18.42 ms (2.40 us/draw) | 72.4 ms |
| ... same batches: hd1 8 clean of 8 / mgPr 4 clean of 8 | 49.73 (0.84) / 48.16 (0.16) | 18.81 (2.47 us) / 19.71 (2.56 us) ms | 74.2 / 73.4 ms |

**Street (zone 2, the player's autosave of 17:54: busy street by the pharmacy, toward the cathedral; 2026-11-18,
agent zone2; section 3).** Guest 10-85 s of the walk, player's settings, GPU 1200, ~8,300 draws:

| Build (batches `zs`+`zs2`) | fps (sd) | main / RSX / jobs+SPU ms per frame | busy main / RSX | GPU ms | W | p99 |
|---|---|---|---|---|---|---|
| mgPr, 5 clean of 9 | 45.90 (0.42) | 20.46 / 19.03 / 77.6 | 94 / 87 % | 17.49 | 14.7 | 30.3 |
| hd1, 3 clean of 9 | **46.34** (0.45) | 20.40 / 17.86 / 77.7 | 95 / 83 % | 17.48 | 14.7 | 29.6 |

PGO retrain (2026-11-19, agent pgo2): hd2 = HEAD with the mg2r profiles (section 3), batches `p2s`+`p2s2`, 12 rounds,
label order rotated, same tooling and settings snapshot:

| Build (batches `p2s`+`p2s2`) | fps (sd) | main / RSX / jobs+SPU ms per frame | busy main / RSX | GPU ms | W | p99 |
|---|---|---|---|---|---|---|
| mgPr, 5 clean of 12 | 45.84 (0.73) | 20.58 / 19.08 / 78.3 | 94 / 88 % | 17.43 | 14.7 | 29.2 |
| hd1, 8 clean of 12 | 46.34 (0.68) | 20.43 / 17.71 / 76.5 | 95 / 82 % | 17.43 | 14.7 | 29.3 |
| hd2, 5 clean of 12 | 46.08 (0.51) | 20.56 / 17.50 / 78.9 | 95 / 81 % | 17.52 | 14.7 | 29.0 |

Guest 4-20 s, the stretch EVERY run walks (before the junction split; `main1-shared.sh`, n = 10 per build of `p2s`):
mgPr 39.18 fps (sd 0.27), hd1 39.63 (0.94; 39.90 without one outlier), hd2 **40.07** (0.24); RSX 20.05 / 19.09 /
18.63 ms, main 20.52 / 20.50 / 20.51 ms. So on the street hd2 = hd1 within noise (main-bound: the RSX saving of
0.2-0.5 ms does not reach fps), better than mgPr; off the route at the tram junction: mgPr 7, hd1 4, hd2 7 of 12 (this
time hd1 stayed on it most: the split is chaotic, not a property of a build).

The street is **main-thread-bound**: a 2 ms busy wait per frame on the main thread costs 2.2 ms of frame time, on the
RSX thread 0.7 ms (causal test over guest 4-20 s), so 04ddd13's RSX saving (-1.2 ms, 2.16 us per draw) gives only +1 %
here. The GPU needs 17.5 ms per frame (81 % of the frame): also over a 60 fps budget. Jobs 75.6 ms/frame (223 Edge
17.9, 218 17.4, 204 10.4, 220 8.5; pool 85 % busy). Main thread profile: guest PPU code 69 % (flat), jobs run in its
waits ~14 % + the runner's wait spin 4 % (~3.6 ms/frame of job waiting), guest stores 2.7 %, pthread rwlocks 3.4 %
(caller not yet found). The street bench has fixed clusters of ~120 ms frames while walking (an artifact of the
frame-stepped clock, absent on the real clock): its p99 / > 50 ms counts are not real-play numbers.

**Main thread on the street (2026-11-18, agent main1; NOTES.md "main thread on the street").** hd1, guest 10-85 s,
21.6 ms frames, main 20.4 ms busy (94.5 %). From the main-thread profile (zb-pmain, 56,743 samples) and a timeline run
with the job runner's new ftrace kinds (m1d-1: every job run, every wait of the main thread, 2,250 frames):

| Item | ms/frame | Notes |
|---|---|---|
| Guest PPU code (lifted EBOOT) | 14.0 | 69 %, flat (top 30 functions = 27 % of it); mostly data-cache-miss bound (the hot samples sit on the instruction after a guest load); no single codegen pattern stands out (below) |
| Jobs the main thread runs itself | 2.7 | 218 anim 1.8 (6/frame x 300 us, from the world tick's re-polls), 204 0.5 (30/frame, array-wait help), 220 0.2, rest 0.2 |
| Waiting for jobs (spin, lock, yields) | 2.5 | waits total 3.6 ms incl. the help above; by the job a wait ends on: 213 1.2, 204 0.8, 216 0.8, 220 / 218 polls 0.45, rest 0.2; inside: wait_until's spin 0.8, its pthread_rwlock calls 0.7, token tests 0.2 |
| Guest store slow path | ~1.0 | ppu_lean_st* 0.6 (stores to pages where any line was ever reserved: out-of-line call through the GOT), SPU coherency notify from PPU stores 0.3, reservations / stwcx 0.1 |
| Other runtime | ~0.5 | world-tick native 0.2, HLE / indirect calls 0.1, gcm pump 0.1 |
| Idle | 1.2 | |

Waiting for jobs (union of waits, jobs run and the world tick's re-polls): 5.2-5.7 ms/frame, a quarter of the frame. The
timeline shows two bursts per frame in which the pool (4 threads, 78 % busy on average) is saturated: the anim manager
(213 x 142 + 218 x 62 + 200; ~22.5 ms of CPU) kicked at ~7 ms and consumed by the world tick from ~10.5 ms, and the
render manager (204 x 513 + 210 + 216 x 6 at 800 us + 208 + 201; ~20 ms) kicked at ~15.5 ms and drained at ~20.4 ms;
between them (~14-15 ms) the pool is idle. Neither job order (each burst is one manager's jobs in the guest's kick
order, nothing else competes) nor the runner's pick (the main thread takes only jobs it needs) is the cause: it is
capacity in the bursts -- and more threads do not buy it at 15 W (a 5th pool thread, or waiters running any queued job,
section 5), so the lever is shorter jobs on that path (213, 218, 204, 216; section 6). The 3.4 % of pthread_rwlock calls are the spin of these waits: help_one took the manager's lock
on every spin turn (33,000 rwlock calls a frame), i.e. wait time, plus lock traffic against the pool.

The frame-stepped bench's ~120 ms frames (guest 8 / 10 / 16-17 s) are the main thread in the game's timed wait
func_000B1388 (deadline = mftb + f/30 s, usleep(3) polls): on the stepped clock its deadline only comes from the 100 ms
stall rule. `PS3_TB_FRAMESTEP_IDLE_US=2000` (new, bench only) lets the flipping thread's clock-polling sleeps see real
time after 2 ms, as on the real clock (section 3).

**Installed 2026-11-19 05:30: hd2** (code 3d974be, PGO mg2r) replaces mgPr in the player's folder. Backups: `<player>/work/port/installed-mgPr-20261119/` (both files) and `tools/play.sh.installed-mgPr-20261119`; older: `work/port/Infamous2Recomp.installed-4cabf54`. Smoke test of the installed files (sandbox, street route): ran and exited cleanly, no errors.

Real clock, standing at spot 2 (player's settings with the 60 fps cap, GPU 1400): mgPr 57.84 fps over 30 min (5-min
buckets 57.5-58.3, p99 19.4-19.8 ms, 17 frames > 33 ms, none > 50 ms, worst 38 ms); t13g 48.16 fps over 15 min (p99
24.1 ms, worst 70 ms). Memory grows ~0.8 MB/min in both (pre-existing, not from the merge).
hd2 (2026-11-19, `work/bench/stand2.sh`, the player's CURRENT settings of 17:42 -- aniso 16, soft shadows 2, SSAO 2,
immediate present -- heavier than mgPr's run above, which had aniso 8, soft shadows 0, mailbox): 58.86 fps over 30 min
(5-min buckets 58.3-59.1, p99 18.6-19.4 ms, 11 frames > 33 ms, none > 50 ms, worst 44 ms), exit 0, no crash / hang /
error, RSS 680 -> 762 MB (+0.75 MB/min after 5 min), threads 55 -> 56. Control on the same settings, mgPr 15 min (standmgPr2): 57.06 fps, p99 19.9 ms, 8 frames > 33 ms
(0.54 per minute vs hd2's 0.36), none > 50 ms, worst 39 ms. So hd2 is not less stable than mgPr (rule 3) and +3 % at the cap.

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
game's cellPad init; system libSDL2 = sdl2-compat over libSDL3). Cause found (2026-11-18, NOTES.md "startup crash in
the SDL joystick scan"): sdl2-compat's joystick list has no lock, and SDL_NumJoysticks frees and rebuilds it -- also
inside every joystick ADDED/REMOVED event it converts. cellPadInit's SDL_InitSubSystem on the game thread queues those
events while the RSX thread's window pump (SDL_PollEvent every 4 ms) converts them: two threads free the same list (a
double free), the next small malloc aborts. Rare (1 in 33 sessions), at start or a controller hot-plug; old builds and
upstream v0.6.2 have it too; the benches never reach it (PS3_PAD_SDL=0). Reproduced outside the game (20/20 runs abort,
ASan: double free, pump thread vs SDL_InitSubSystem) and traced in the game. Fix in commit 8833293:
the pump and the menu's controller list take cellPad's lock (try-lock; a busy pass is skipped).

**What each graphics setting costs on the street (2026-11-19, agent gpu2).** hd2, GPU 1200 MHz,
`PS3_TB_FRAMESTEP_IDLE_US=2000`, one setting changed at a time from the player's 17:42 snapshot (aniso 16, shadow softness
2 "Softer", SSAO 2 "Full, high", shadow quality High (2048), effects resolution Full, 720p, FXAA + CAS 0.10, DOF and edge
blur on). About half the street runs leave the route at the tram junction (onto other, fixed paths), so every setting is
measured over guest 4-20 s, the stretch every run walks (all runs), and over the full window (guest 10-85 s) on the runs
that stayed on the route. Batches g2a / g2b / g2c / g2f, 3-7 runs per setting (work/bench/gpu2/settings-table.txt).

| Setting (from the player's) | GPU ms/frame, guest 4-20 s (runs) | GPU saved | fps, guest 4-20 s | main / RSX ms | full window, on-route runs: n, fps, GPU ms |
|---|---|---|---|---|---|
| none (the player's settings) | 17.32 (7, sd 0.32) | -- | 45.43 | 20.14 / 18.53 | 4: 48.64, 17.27 |
| Texture filtering 16x -> 8x | 17.33 (3) | 0 | 45.61 | 20.03 / 18.67 | 1: 47.44, 17.31 |
| Texture filtering -> Off | 16.90 (3) | 0.4 | 45.30 | 20.15 / 18.74 | 1: 47.24, 17.09 |
| Shadow softness Softer (2) -> Soft (1) | 17.10 (4) | 0.2 | 45.92 | 20.00 / 18.39 | 0 |
| Shadow softness -> PS3 (0) | 17.10 (3) | 0.2 | 45.86 | 19.94 / 18.53 | 3: 47.89, 17.15 |
| Ambient occlusion Full, high (2) -> Full (1) | 17.15 (3) | 0.2 | 45.23 | 20.19 / 18.55 | 3: 47.72, 17.03 |
| Ambient occlusion -> PS3 (0) | 16.80 (2) | 0.5 | 45.61 | 19.88 / 18.50 | 0 |
| Shadow quality High (2048) -> PS3 (1024) | 16.18 (3) | **1.1** | 46.15 | 19.79 / 18.52 | 1: 47.51, 16.21 |
| Effects resolution Full -> As game | 16.86 (3) | 0.5 | 45.73 | 19.97 / 18.72 | 0 |

* No setting moves fps on the street: the main thread limits (~20 ms of a ~22 ms frame in the shared stretch; fps sd
  0.3-0.8 between runs) and the GPU is ~75-80 % busy at 1200 MHz. A GPU saving here only buys headroom (busier views, a
  lower GPU clock), not fps. Power is at the 15 W cap in every run (15.0-15.1 W).
* The only setting with a sizeable GPU cost is **Shadow quality** (High -> PS3 saves ~1.1 ms: the maps and their copies
  are 4x smaller). Everything else is worth 0.5 ms or less each; texture filtering 16x vs 8x costs nothing measurable,
  shadow softness and SSAO "Full, high" vs "Full" ~0.2 each. Differences under ~0.3 ms are within the base's run-to-run
  spread.
* Not run separately (small in the breakdown above): anti-aliasing (FXAA) + sharpening (CAS) 0.54 ms together, the glow /
  blur chain ~0.4 ms, depth of field and edge blur (terms of the game's post pass, 1.35 ms for the whole pass). Internal
  resolution is already the lowest (720p). Upscaling (FSR) does not run: the window is the game image's size (the
  player's swapchain is 1152x720 in Game Mode, the bench's window 1152x718).
* End shots (cathedral balustrade, real 100 s after the start; people walk, compare the lighting and the stone):
  work/bench/gpu2/shots/street-end-base-player-settings.png, -aniso-8, -aniso-off, -soft-shadows-0-PS3, -ssao-1-full,
  -ssao-0-PS3 (that run had the HUD on: the menu was opened during it, numbers discarded), -shadow-quality-PS3-1024.
* Side note: the player's sessions load the lsfg-vk layer (Lossless Scaling frame generation; decky plugin, its config has
  multiplier 1); the bench runs without it, so whatever it costs is not in these numbers.

**GPU clock on the street (2026-11-19, gpu2; hd2, player's settings, same batches; detstreet.sh sets the clock per run and
puts the player's back).**

| GPU clock | fps, on-route runs (n) | fps, guest 4-20 s (n) | GPU ms/frame (busy) | main ms, 4-20 s | frames > 33 ms per run | W |
|---|---|---|---|---|---|---|
| 1000 MHz | 47.96 (2) | 45.93 (4) | 20.27 (94 %) | 19.48 | 5.0 | 15.1 |
| 1100 MHz | 49.47 (1) | 45.78 (3) | 18.57 (84 %) | 19.91 | 4.0 | 15.1 |
| 1200 MHz | 48.64 (4) | 45.43 (7) | 17.27 (78 %) | 20.14 | 4.2 | 15.0 |
| 1400 MHz (the player's) | 45.15 (2) | 43.17 (4) | 15.39 (61 %) | 21.19 | 6.5 | 15.0 |
| 1600 MHz | 44.03 (1) | 41.42 (3) | 14.09 (62 %) | 22.17 | 5.0 | 15.1 |

At 1400 and 1600 MHz the GPU takes power the CPU needs (main thread +1.1 / +2.0 ms per frame): -7 % / -10 % fps vs 1200.
At 1000 MHz the GPU becomes the limit on the full walk (94 % busy, 20.3 ms). With the light tiles below (exe
work/port/gpu2l, batch g2g + g2f, same settings) the GPU needs ~4 ms less and the optimum moves down:

| GPU clock, light tiles | fps, on-route runs (n) | fps, guest 4-20 s (n) | GPU ms/frame (busy) | main ms, on route |
|---|---|---|---|---|
| 1000 MHz | 50.39 (2) | 46.71 (2) | 13.90 (66 %) | 19.22 |
| 1100 MHz | 50.51 (1) | 47.16 (2) | 12.93 (58 %) | 19.18 |
| 1200 MHz | 49.74 (1) | 46.73 (4) | 12.15 (54 %) | 19.45 |
| 1400 MHz | 47.33 (1) | 44.79 (2) | 10.96 (49 %) | 20.49 |

**Recommendation: manual GPU clock 1100 MHz** (hd2: 1100-1200 are equal and best; with the light tiles 1000-1200 are
equal, 1100 keeps headroom for busier views). The player's 1400 MHz costs 5-7 % on the street now.

**Light pass per 16x16 tile (2026-11-19, gpu2; uncommitted, work/bench/gpu2-lightcull.patch, exe work/port/gpu2l).**
The light pass (the game's SPU light job, done on the GPU) walked every light of the frame (83-129 on the street) for every
pixel: 5.05 ms. Now a pass over 4x4-pixel position boxes (288x180, 0.21 ms) and a tile pass (four fragments per 16x16
tile, 0.1 ms) keep, per tile, the lights whose sphere can reach the tile's (slightly widened) box, and the light shader
walks only those, in ascending order, with the original loop body; a light left out has outer radius < its distance to
every pixel of the tile, so the original's own `if (w <= 0) continue` skipped it there too. Same lights, same order, same
arithmetic: the same bytes. Light work per frame 5.05 -> ~0.75 ms (guest 4-20 s, backend passes at 1152x720 6.75 -> 2.13
ms). Check (`PS3RECOMP_VK_LIGHT_CULL=2`: every 60th light pass also rendered with the original shader, both read back and
compared byte for byte): street 81 checks, up to 129 lights, 67 M pixels, 0 bytes differ (the first, single-stage version:
96 checks, 80 M pixels, 0); plaza (exe work/port/gpu2l2 = gpu2l + target cleanup at shutdown) 55 checks, up to 102 lights,
46 M pixels, 0. End shots at noise level (street, on the route: light tiles vs hd2 3.5 / 4.3 / 9.3 % of pixels differ by >
24, hd2 vs hd2 8.5 / 8.8 % -- people walk; plaza, same spot: 0.7 / 1.4 % vs hd2 vs hd2 0.7 / 2.9 %; g2p-lt-4 stood a step
away from the others at the end, 28 % against every run, lighting the same).

| A/B, rotated rounds, `PS3_TB_FRAMESTEP_IDLE_US=2000`, 1200 MHz | hd2 | light tiles | change |
|---|---|---|---|
| Street, guest 4-20 s, 4 runs each (g2f): fps | 45.28 (sd 0.35) | 46.73 (sd 0.40) | +3.2 % |
| ... GPU ms/frame | 17.43 | 13.17 | -4.3 ms (-24 %) |
| ... main / RSX ms/frame | 20.27 / 18.52 | 19.54 / 18.27 | -0.7 / -0.3 |
| Street, full window, on-route runs: fps (n) | 47.97 (2) | 49.74 (1) | +3.7 % |
| ... GPU ms (busy), p99, frames > 33 ms per run | 17.27 (77 %), 28.2, 4.5 | 12.15 (54 %), 27.0, 3.0 | -5.1 ms |
| Plaza, on-route runs, 3 of 4 rounds (g2p): fps | 50.21 (sd 0.68) | 51.83 (sd 0.36) | +3.2 % |
| ... GPU ms (busy), main / RSX ms, p99, frames > 33 ms per run | 17.58 (80 %), 19.01 / 18.69, 24.1, 0.7 | 13.51 (60 %), 18.36 / 18.20, 23.1, 0 | -4.1 ms |
| Plaza, round 2 (both left the route onto the same path, 6,582 draws) | 56.65 fps, GPU 16.43 (89 %) | 60.82 fps, GPU 12.42 | +7.4 % |

Where the main thread limits, fps rises ~3 % (the CPU gets the power the GPU no longer draws: main -0.65..-0.7 ms); where
the GPU limited (plaza round 2) +7 %. The street's GPU frame at 1200 MHz is now ~12-13 ms, inside the 16.7 ms budget.
Off-route runs on matching paths agree (street path B 11.4 vs ~13.3 ms, path C 11.6 vs 13.8). Rule 1 (exact) by
construction and check; rule 2 GPU -24 % and fps up; rule 3 p99 and slow frames not worse (street 27.0 vs 28.2, plaza 23.1
vs 24.1); rule 4 `PS3RECOMP_VK_LIGHT_CULL=0` restores the original shader. Not committed (orchestrator); PGO: only
vk_eng_light_pass (cold) and vk_eng_shutdown changed.

**Goal set by the player (2026-11-18): stable 60 fps, including busy areas.** Budget at 60 fps: 16.7 ms per frame
for each single thread (the RSX thread is ~2.56 us per draw now, so 9,000 draws need ~1.6 us per draw, -35%), and
all CPU work per frame (now ~110 ms: main 18.7, RSX 19.5, SPU 71) has to fit the 4 cores / 8 threads under 15 W
(roughly -30%). GPU: 15.1 ms/frame at 1200 MHz on the plaza, also close to the 16.7 ms limit. New benchmark zone
wanted by the player: the busy street of their current save, walking to the cathedral (save snapshot
`<player>/work/pgo/bench-saves/gamedata3`, 17:54) -- built 2026-11-18 (`work/bench/detstreet.sh`, section 3; results
above: main-thread-bound at ~46 fps / 8,300 draws).

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
        PORT_PGO="use:$E/work/pgo/mg2r-module.profdata" PORT_ENGINE_PGO="use:$E/work/pgo/mg2r-engine.profdata" \
        PORT_JOBS=3 PORT_LIFT_JOBS=2 \
        PORT_BUILD="$D/work/port/build-<name>" PORT_ENGINE_BUILD="$D/work/port/build-<name>-eng" \
        PORT_C_EXTRA="-march=znver2 -ffp-contract=off" \
        bash -c "cd '$D' && '$D/work/venv/bin/python' -I tools/rebuild.py --from port --only compile"

  (PORT_ENGINE_BUILD: the compile step builds the engine there, default work/port/build -- never leave it unset.
  The current profiles are mg2r (2026-11-19, below); mg1r = the installed mgPr's.)

  Then copy the engine exe AND `libInfamous2Game.so` (side by side) to `work/port/<tag>/` before benchmarking or
  profiling. Since the merge (1f22354): `PORT_PGO` = the game module, `PORT_ENGINE_PGO` = the engine (deck3.profdata
  is for the old single exe; before 1f22354 the build dir was build-engavx2 with `PORT_PGO=use:deck3.profdata`).
  `--from port` skips the inputs step, which would copy onto `work/EBOOT.elf` (a symlink into the player's tree).
  Profile warnings: engine `work/logs/compile-<builddir>.log`, module `<build>/module-obj/compile.log`
  ("PGO: N function(s) changed since the profile"; `work/bench/pgowarn.py <log>` lists them with their discarded
  counts).
* PGO profiles (`E/work/pgo`; module + engine file per name; never overwrite one, a retrain gets a new name):
  * **mg2r** (2026-11-19, agent pgo2; hd2) = mg2 training x5 + `deck.profdata` (old real play) x2. Training mix (6 runs,
    one each, instrumented HEAD 3d974be, the player's 17:42 settings snapshot `zone2/settings-player-20261118.ini` =
    16:10 so the short tail runs, sandbox env + 4 pool threads + play.sh's Deck env): spot 2 spin (bench-move.steps,
    gamedata2), spot 1 standing (bench.steps, gamedata), video A and video B routes (video.steps, gamedata2, HUD on,
    60 fps cap as vidrun.sh), the plaza walk (bench-plaza.steps, gamedata2; these 5 on the real clock = mg1's routes)
    and **the street** (zone2/bench-street.steps + route-street.pad, zone2/gamedata3, frame-stepped clock as
    detstreet.sh). The 5:2 weights keep the training's share of mg1r's merged counts (module 9.8 %, engine 17.9 %)
    with the 24 % larger training (llvm-profdata weights are integers). mg2 = the training alone (weight 1).
  * mg1r (2026-11-18; mgPr, hd1) = mg1 training (the first 5 routes, short tail off) x3 + deck.profdata x1.
  * Retrain: instrumented build (`PORT_PGO=gen PORT_ENGINE_PGO=gen`, its own PORT_BUILD / PORT_ENGINE_BUILD, e.g.
    work/port/build-pgogen2{,-eng}), then `work/bench/pgotrain-mg2.sh` (runs through zrun.sh = the dev tree's
    linux_run.py: recordings in D/native/recordings, only the raws go to `E/work/pgo/train-<name>`;
    `LLVM_PROFILE_FILE=<dir>/<run>-%p-%m.profraw` = one raw per binary per run, written at the clean exit of the
    steps' close; ONLY=<runs> for a subset), merge with `REAL=5 REALW=2 work/bench/pgomerge-mg2.sh <name> <train dir>`
    (training weight / real-play weight; without REAL = training only). Checks: `work/bench/pgoana.py counts
    <profdata> <name substr>...` (a function's records and counts), `pgoana.py gain <train> <merged>` (what real play
    adds), `pgoana.py cold <old> <new>` (what the new profile warms up); after the use build pgowarn.py on both logs
    (expected: module 0, engine 1 = `main`, the gen build's dump code).
* Running a merged build (engine + module): `GLIBC_TUNABLES=glibc.rtld.optional_static_tls=1048576
  PS3_STATIC_TLS_RETRY=1` (else it re-execs and shows up as "exe", invisible to `pgrep -x Infamous2Recomp`, budget.py
  and the lock checks), `PS3_NATIVE_JOBS_THREADS=4` (play.sh sets both; the exe default is 2), and for benchmarks the
  sandbox env (`<merge tree>/work/bench/mgenv.txt`: `PS3RECOMP_VK_SHADER_CACHE`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`)
  so dev builds never write the player's config or the shared shader cache. Bench copies that do all this:
  `<merge tree>/work/bench/detab-m.sh`, `ab4.sh`, `stand.sh` (real-clock standing run), `valP.sh` (all replays).
  Dev-tree copies that write nothing into the player's tree (2026-11-19, pgo2): `work/bench/stand2.sh <tag> <s>`
  (`EXE=<exe dir>`; the standing run on the real clock, the player's settings snapshot with its 60 fps cap, spot 2,
  recording in D/native/recordings, 5-minute buckets by standbuckets.py) and `work/bench/valP2.sh <exe> <tag>` (the
  replay set on D/native/recordings/rp-* = symlinks to the player's .jrec files, so replay_report / stderr logs stay in
  D; PS3_GAMEDATA_ROOT pointed at the sandbox).
  Fast builds leave the det route more often (5 of 14 PGO runs at the house wall near Hank's at anchor+5-7 s; the
  > 50 ms frames of those runs come after the divergence): run more rounds and discard by draws.
* Frame-stepped clock, timed waits (2026-11-18, main1): `PS3_TB_FRAMESTEP_IDLE_US=<us>` (default 0 = off; only with
  `PS3_TB_FRAMESTEP_US`) -- the flipping thread's clock-polling sleeps (usleep calls < 1 ms apart in one frame, with
  mftb reads right after them: 8+ polls) see real time once they last longer than <us>, from the start of the polls. It
  removes the street bench's fixed ~120 ms frames (the game's timed wait 0xB1388 could only end by the 100 ms stall
  rule). A wait on another thread (0x13ACE8's wait for the RSX sleeps without reading the clock) and every other thread
  keep the stepped clock. Not used by detstreet.sh yet (results would not compare with older batches): pass it as an
  env (`... PS3_TB_FRAMESTEP_IDLE_US=2000`) to both sides of an A/B, on builds that have it (m1p and later).
* Main-thread job timeline (2026-11-18, main1; patch work/bench/main1-diag.patch + main1-diag-dlogic.patch, not in the
  tree): PS3_FTRACE kinds 20 (every wait of a waiter: hook, slot, the needed jobs queued / running / unclaimed at its
  start, the manager's module, the target), 21 (every job run: pool thread or helping waiter, module, source), 22 (the
  world tick's re-poll passes), 23 (the 100 ms stall rule fired: thread, guest lr). Run with
  `PS3_FTRACE=28:85:<rec>/ftrace.txt PS3_FTRACE_KINDS=1,2,5,7,8,9,10,12,16,17,20,21,22,23 PS3_FTRACE_CAP=16000000`
  (overrides linux_run.py's), then `work/bench/main1-ftana.py <rec> <t_lo_ms> <t_hi_ms>` (per-frame waits, runs, pool),
  `main1-ftmatch.py` (each wait -> the module of the job it ended on), `main1-ftframe.py <rec> <frame>` (one frame).
* rwlock callers: `LD_PRELOAD=work/bench/main1-rwshim.so RWSHIM_OUT=<rec>/rwshim` (build main1-rwshim.c in the
  distrobox: `gcc -O2 -fPIC -shared -o main1-rwshim.so main1-rwshim.c -ldl -lpthread`), then `main1-rwana.py <rec>
  <k_lo> <k_hi> <frames>` (dump k = 5 s steps since start).
* Codegen: `main1-cgana.py <perf.data> <module .so> [n]` (hot instructions of the top n lifted functions by class),
  `main1-cghot.py <perf.data> <so> <guest addr>` (one function's hottest instructions in context).
* Shell pitfall: never `pkill -f` / `pgrep -f` a pattern that also appears in the waiting command itself
  (it matches its own shell). Wait on a file's line count instead.
* Merged builds in the dev tree (2026-11-18, rsx1): `work/bench/abx.sh <rounds> "<label>=<exe dir>[|ENV=V ...]" ...`
  (sandbox env work/bench/mgenv.txt -> D/work/sandbox, 4 pool threads, per-label env; recordings `$PREFIX-<label>-<n>`;
  waits while the orchestrator's game.lock exists), `detprof-mg.sh` (PMODE=all whole game, PMODE=rsxcg the RSX thread
  with DWARF stacks; perf 7.1.4's libdw unwinder fails on most samples here: use inline chains via llvm-symbolizer),
  `abtable.py` with `DROPCPU=""`. Always pass EXE / an exe dir: detbench / abrun default to the player's install.
  `--only engine` builds into PORT_ENGINE_BUILD (default work/port/build, not PORT_BUILD): set it per build dir.
  The det bench copies the player's current settings.ini: when the player changes settings, compare only within a batch.
* Street det bench (zone 2, 2026-11-18; merged builds only): `work/bench/detstreet.sh <tag> [ENV=V ...]` with
  `EXE=<exe dir>` (required). Save `work/bench/zone2/gamedata3` (copy of `<player>/work/pgo/bench-saves/gamedata3`):
  at boot the game's ListAutoLoad takes the slot with the newest PARAM.SFO (cellSaveData.c save_mtime), here `_0`
  "(Autosave) 8. Bomb Scare" = the player's street; to load another slot, `touch` its PARAM.SFO in a copy. Route
  `zone2/route-street.pad` (the player picked it from test z2t2): stand 4 s, left stick half forward to guest 84 s.
  At normal speed Cole jogs down the tram tracks, crosses the junction to the cathedral grounds and ends against the
  stone balustrade by the cathedral steps (~8,300 draws; z2t2's turn to the pub only happened under build load).
  Measured over GUEST 10..85 s (`zstat.py`: frames = the ftrace PRESENT spans after the route start, so every build is
  measured over the same frames of the walk; a real-time window would cover a different stretch per build), CPU per
  thread from `zsample.py` (0.5 s samples), GPU from `PS3RECOMP_VK_GPU_PROF`, power from sysfs. Recordings in
  `D/native/recordings` (`zrun.sh` runs the dev tree's linux_run.py: nothing is written in the player's tree).
  Route check (SUSPECT(route)): 10 guest-s draw segments vs `zone2/ref-segments-street.txt` (5 %), end shot vs
  `zone2/ref-end-street.png` (30 % of pixels), median 7,600-9,400. All runs agree to guest ~20 s; at the tram junction
  hd1 left the route in 5 of 9 runs, mgPr in 2 of 9: plan ~2x the rounds and discard. SUSPECT(input) = someone opened
  the in-game menu during the run (the HUD gets turned on): discard. The ~120 ms frames at guest 8 / 10 / 16-17 s are a
  frame-stepped-clock artifact (a guest wait for time inside a frame ends only by the 100 ms stall rule; the real
  clock shows none). Interleaved A/B: `PREFIX=<p> work/bench/abstreet.sh <rounds> "a=<exe dir>[|ENV=V ...]" "b=..."`,
  table `python3 work/bench/ztable.py <output>` (`work/bench/abrot.sh` = the same with the label order rotated every
  round); `ZONE=plaza` = the old det plaza on the same tooling (guest 16..100 s),
  `ZONE=street2` = a steadier variant (walk to guest 16 s, then stand + camera pans; ~41.7 fps, 1 of 6 off-route; not
  the player's pick). `work/bench/zprof.sh` = the same + perf of one thread (`PTHREAD=main|rsx|all`). Never build,
  profile-analyse or run anything heavy while a game run is going: it skews the run.
* PGO-safe edits: a change to a function's control flow (a branch, a loop, a select) drops its profile; small static
  functions are inlined into their callers before instrumentation, so e.g. any edit of dl_scan / eng_ic_probe changes
  sink_end_body's hash (the build prints "PGO: N function(s) changed"). New work in a `noinline` function called from
  the hot code keeps the caller's hash (a direct call adds no edge).

* GPU per pass (2026-11-19, gpu2): `PS3RECOMP_VK_GPU_PROF=2` in a build with work/bench/gpu2-prof.patch (measurement only:
  it touches sink_end_body, so its PGO profile goes stale) labels every timestamp interval: an engine pass by colour target
  0 / target count / depth (backend handles, formats and the guest addresses the engine bound them at), a backend pass by
  its kind (light, lfx_* = SSAO, zr32 / zrgba_d = depth copies / decodes, the light tiles), the postfx (`post`) apart from
  the window blit, with draws per frame. `work/bench/gpu2pass.py <rec> <g1> <g2>` sums the summaries over a guest window
  (also works on `=1` runs, by size only). `PS3RECOMP_VK_GPU_PROF_DUMP=1` dumps one batch's raw intervals with the commands
  in each (copies, clears, barriers, draws). `PS3_RSX_STATS=10` prints the barrier counts.
* Settings / clock batches (gpu2): `work/bench/gpu2rot.sh` = abrot.sh + `GPUCLK=<MHz>` per label (to detstreet.sh, which
  sets and restores the manual clock) + `COMMON="ENV=V ..."` for every label; `gpu2rot2.sh` also waits between runs while
  `<scratchpad>/gpu2.pause` exists (pause for builds / analysis without stopping the batch). `work/bench/gpu2table.py
  <batch .out> ... -s [-l labels]`: per label, the full window on the clean runs and (with -s) guest 4-20 s on every run
  (SUSPECT(input) runs dropped from both) -- the street's shared stretch, usable when half the runs leave the route.
  Killing a batch script mid-run kills its run (bash runs the TERM trap at once): use the pause file instead.
* Light pass check (gpu2): `PS3RECOMP_VK_LIGHT_CULL=2` renders the original light shader too every 60th light pass, reads
  both outputs back and compares them byte for byte (`[light-cull] check` lines; the run is slow, only for exactness).

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
| (uncommitted; work/bench/gpu2-lightcull.patch, exe work/port/gpu2l) | light pass per 16x16 tile (`PS3RECOMP_VK_LIGHT_CULL`, default 1; 0 = the original shader, 2 = check): a pass over 4x4-pixel position boxes and a tile pass (0.3 ms together) keep, per tile, only the lights whose sphere can reach the tile; the light shader walks those bits in order with the original loop body (rsx_vk_passes_glsl.h VK_LIGHT_GLSL, rsx_vulkan_backend.c vk_eng_light_pass) | street GPU 17.43 -> 13.17 ms (guest 4-20 s, 4 runs each), fps +3.2 %; plaza 17.58 -> 13.51 ms, 50.21 -> 51.83 fps (3 on-route rounds), +7.4 % where the GPU limited; exact (check mode: 0 bytes differ in 113 M pixels, street + plaza; end shots at noise level) |
| 626ab72 | `PS3_TB_FRAMESTEP_IDLE_US` (bench only, default off, work/bench/main1-fsidle.patch): the frame-stepped clock lets the flipping thread's clock-polling sleeps see real time after <us> (the game's timed wait 0xB1388 ended only by the 100 ms stall rule) | (tools) street: frames > 50 ms ~15 -> 1 per run, max ~130 -> ~60 ms; guest 4-20 s 39.5 -> ~45 fps (the artifact cost ~13 % there, ~3-8 % over guest 10-85 s); route replays (on-route runs m1s-fs-2 and m1x-xu-2: segments within 3 % of ZREF, end shots 13.7 / 13.9 %; all 8 runs agree to guest 20 s); off-route rate still high (6 of 8) |
| 365fd60 | `PS3_NATIVE_JOBS_HELP_PEEK` (default 1, work/bench/main1-runner-peek.patch): help_one looks at the queue without the lock before taking it -- the waiters' spin took the manager lock ~15,000 times a frame | neutral: street `m1a` 46.68 vs hd1 46.66 fps (2 / 4 clean of 8), guest 4-20 s 39.76 vs 39.49 (8 runs each, sd 0.48 / 0.26); plaza `m1z`+`m1z2` 50.96 (49.72 without one outlier, 4 / 3 clean of 7) vs hd1 49.28 (2 clean of 7); exact by construction (the locked test still decides). By rule 2 not a commit on its own merit (cleanup of ~30,000 lock calls a frame) |
| 04ddd13 | short tail for repeated draws also in a wide picture with the HUD pinned (`PS3RECOMP_ENG_FASTDRAW_HUD`, default 1): under the player's 16:10 it was off for every draw (rsx_draw_engine.c eng_fd_hud_note, rsx_draw_fast.inc fd_takes) | `rb3` 7 rounds (switch off / on, same binary): RSX 19.63 -> 18.59 ms/frame (2.57 -> 2.45 us per draw), **fps 48.44 -> 50.40**, p99 24.2 -> 23.3; final binary vs mgPr (`rb4`+`rb5`, 7 / 6 clean): RSX 19.61 -> 18.87, fps 48.03 -> 49.75, p99 24.6 -> 23.5, > 33 ms 2.7 -> 1.0 per run; exact (VERIFY=2: 13 M draws, 0 differences); main thread now co-limits; the short tail is still cold in the mg1r profile (retrain) |
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

* Main thread, street (2026-11-18, main1; NOTES.md; street A/B `m1a` 8 interleaved rounds, zone2/m1a.out):
  `PS3_NATIVE_JOBS_HELP=2` (waiters run any queued job of their manager): 45.58 vs hd1 46.66 fps (5 / 4 clean), main
  +0.5 ms/frame -- the main thread takes long jobs (218 at 300 us, 216 at 800 us) while the job it waits for ends within
  ~13 us. `PS3_NATIVE_JOBS_THREADS=5`: main -0.3 ms (busy 92 %) but SPU-side CPU +5.6 ms/frame and fps 46.11 (3 clean):
  no gain at 15 W. Over guest 4-20 s (the stretch all 8 runs of each share): hd1 39.49 / HELP=2 39.62 / THREADS=5
  39.65 fps (sd 0.26-0.32): equal. `PS3_CPU_PLACE_MAIN=core` (experiment, work/bench/main1-place-main.patch: the main
  thread alone on a core, everything else on the other 4 CPUs): guest 4-20 s 43.28 vs 44.91 fps (both with
  `PS3_TB_FRAMESTEP_IDLE_US=2000`, 2 runs each), main +1.5 ms (it waits longer for the jobs). `=cpu` (main pinned to
  its own CPU, the sibling shared): 45.64 vs 44.91 (2 runs each): inconclusive, not pursued.
* Job priority across managers for the main thread's waits (2026-11-18, main1, not built): the timeline shows no
  competition -- in both job bursts of a street frame only one manager's jobs are queued, in the guest's kick order.
* Keeping v20-v31 in locals across calls (lifter, not built): on x86-64 SysV every xmm register is caller-saved, so a
  vector local live across a call is reloaded from the host stack instead of ctx: no saving.
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
* GPU clock, street (2026-11-19, gpu2; section 2 table): with hd2 1100-1200 MHz are best (48.6-49.5 fps on the route),
  1400 MHz (the player's) -7 %, 1600 -10 % (the main thread loses power: +1.1 / +2.0 ms), 1000 GPU-bound (20.3 ms, 94 %
  busy). With the light tiles 1000-1200 MHz are equal (50.4 / 50.5 / 49.7 fps on the route), 1400 -5..-6 %.
  **New recommendation: manual 1100 MHz** (was 1000-1100 on the plaza, the player plays at 1400).
* `PS3_RSX_BARBATCH=1` on the Deck (2026-11-19, gpu2; the track-C switch that leaves out each GENERAL -> GENERAL image
  barrier right behind a full barrier; off on AMD by default): ~115 of ~215 barriers a frame left out, but no GPU time
  saved -- street guest 4-20 s 17.49 / 17.57 ms (bb / lcbb, one run each after discards) vs 17.32 for hd2; light tiles
  with and without it 13.23 vs 13.14 ms. The first pair measured (17.65 -> 15.23 ms) were two different off-route paths
  with the same draw count (the end shots showed it). Patch kept: work/bench/gpu2-barbatch.patch (turns it on under RADV).
* Light tiles in one stage (2026-11-19, gpu2, exe work/port/gpu2t1: one fragment per half tile reading all 256 depth
  texels): exact, but the tile pass took 0.70 ms (6,480 fragments, too few waves for 256 serial fetches each); replaced by
  the 4x4-box stage (0.30 ms for both small passes).
* GPU clock (2026-11-18, player settings, plaza): 1000 MHz 41.8 fps > 1100 41.2 > 1200 40.1 > 1400 38.2.
  Higher clocks take power from the CPU (at 1400 the RSX thread needs 23.2 ms and the SPUs 105 ms per frame,
  at 1000 20.7 and 95.6). Recommended: manual 1000-1100 MHz. Benchmarks stay at 1200 for comparability.
* Lower scheduling priority for the SPU threads (renice +3): no effect.
* `PS3RECOMP_ENG_PREFETCH_PATH` (2026-11-18, rsx1; patch work/bench/rsx1-engine-all.patch): prefetching the index /
  vertex cache probe's slots in front of the predicted entry. Det A/B `rb3` with the short tail on: RSX 18.77 vs 18.59
  ms/frame, fps 49.80 vs 50.40 (not better). Why: only ~10% of probes find their entry off its home slot, and the
  successor link is right for only 57.5% of probes (DBG_ICSTATS: several passes draw the same index lists, so an entry's
  successor flips). A better predictor (two successors per entry, or per-frame sequence) is the open idea, not this.
* Per-draw descriptor-set bind / push constants bound (2026-11-18, rsx1): leaving the fast record's
  vkCmdBindDescriptorSets out entirely (`PS3RECOMP_DBG_FD_NOBIND`, measurement only, wrong pictures) saved ~0.7 ms of
  19 ms (one clean pair). Push constants (item 4) would keep a per-draw constant flush, so < ~0.5 ms: not worth the
  shader-generator change on its own.

## 6. Next candidates (expected gain, effort)

**GPU candidates (2026-11-19, gpu2; street, 1200 MHz, after the light tiles: ~13 ms of GPU per frame on the shared
stretch, ~12.2 on the route).** Ranked by the GPU time at stake; "exact" = the same picture by construction / by a check.

| # | Candidate | GPU ms/frame at stake | Effort | Exact? | Notes |
|---|---|---|---|---|---|
| G1 | DONE (uncommitted): light pass per 16x16 tile | measured street -4.3 (shared stretch), -5.1 (route), plaza -4.1 | -- | yes (check: 0 bytes differ in 113 M pixels, street + plaza, final code) | section 2 / 4 |
| G2 | Sun shadow copies: sample the shadow zeta itself instead of its R32F copy (3 x 2048^2 a frame) | 0.6-0.7 | medium-high | needs proof | the copy exists because the zeta is a 1280-wide buffer and the texture its 1024x1024 corner: sampling the zeta directly needs its own allocation at the map's size (or a coordinate change, not exact) |
| G3 | Box + tile passes of G1 (0.30 ms): fewer, wider fragments or one pass with shared memory (a compute pass) | 0.1-0.2 | small-medium | yes (same check) | the box pass at 288x180 is 0.2 ms |
| G4 | Depth-as-RGBA8 decodes (4.9 a frame, 0.49 ms with gaps): skip one whose depth had no write since the last decode of the same size | 0-0.2 | small | yes | every depth-writing draw invalidates them now; whether any of the five repeats is unknown |
| G5 | Shadow quality PS3 (1024) | 1.1 | setting | no (lower-resolution shadows) | section 2 table; the player's choice |
| G6 | SSAO "PS3", effects resolution "As game", texture filtering Off | 0.4-0.5 each | setting | no | section 2 table |
| -- | rejected: BARBATCH on RADV (0 ms), single-stage tile pass (section 5) | | | | |

Barriers, render-target formats (RGBA8 everywhere but the SSAO / depth scratch) and the post chain showed nothing
picture-identical to take. Per-material shader patterns were not profiled (the timestamps are per pass, not per draw: the
G-buffer and forward passes, 4.7 ms together, are where they would show); the AMD NaN rules the FP decompiler adds
(`PS3RECOMP_VK_NAN_PRESERVE`, `RSX_FP_NAN_MODE=rsx`, ...) are correctness fixes (GitHub #27) and were left alone. With the light tiles the GPU is no longer the street's second limit at 1200 MHz (~55-60 %
busy); the main thread (20 ms) is.

**Main-thread candidates (2026-11-18, main1; street, hd1: main 20.4 ms/frame busy, target <= 16 ms, i.e. -4.4 ms).**
Breakdown in section 2. Ranked by the expected saving on the main thread per frame:

| # | Candidate | Expected ms/frame | Effort | Risk | Notes |
|---|---|---|---|---|---|
| M1 | Shorter job bursts on the main thread's critical path: faster 213 (142 jobs x 39 us, lifted; the main thread waits for them token by token, 1.2 ms) and 218 (62 x 270 us; the main thread runs 6 itself, 1.8 ms) | 0.5-1.5 | large (natives, exactness by job replay) | medium | the anim burst (213 + 218 + 200, ~22.5 ms of CPU) is saturated on 4 pool threads + main for ~5-7 ms of every frame |
| M2 | Store slow path: an inline line-bit test where the page has a reserved line (no out-of-line call through the GOT), and/or unmarking lines no reservation holds | 0.3-0.8 | medium (lifter header + module rebuild, no-PGO A/B; unmarking needs a holder count per line and the process barrier on re-mark) | medium (SPU/PPU coherency) | ppu_lean_st* 0.6 + coherency notify 0.3 + call sites ~0.3 ms/frame; lines and pages are marked forever (spu_coherency.h); next step: count slow stores by page (which pages, PPU lwarx or SPU getllar marks) |
| M3 | Render-manager drain at the frame's end (204 x 513 at 20 us, 216 x 6 at 800 us behind a barrier): faster 204 / 216 | 0.3-0.8 | large | medium | 204 drain 0.8 + 216 barrier 0.8 ms of waiting |
| M4 | Codegen: ctx traffic around calls (~6 % of hot guest samples) | 0.3-0.5 | large (lifter) | medium | host-stack spills (12 %) are register pressure; v20-v31 kept = no gain on SysV (section 5) |
| M5 | Codegen: VMX special-value tests | <= 0.3 | large | high (NJ / NaN exactness) | part of the 8.5 % vector/fp alu |
| M6 | DONE 2026-11-19 (pgo2, with R1): PGO retrain mg2r, build hd2 | measured 0 on the street (main 20.56 vs hd1 20.43 ms/frame, guest 4-20 s 20.51 vs 20.50), plaza -0.4 (18.98 vs 19.38) | -- | -- | the 9 changed engine functions (help_one, usleep, ppu_timebase_now, pump_messages, the crash fix's pad lock ...) have profiles again; stale after the use build: module 0, engine 1 (`main`, cold) |
| M7 | `PS3_CPU_PLACE_MAIN=cpu` (main pinned to a CPU of its own, the sibling shared) | 0-0.5? | small (patch exists) | low | +1.6 % over guest 4-20 s in 2 runs: needs a real A/B |
| -- | done / rejected: help-peek (neutral, section 4), HELP=2, THREADS=5, PLACE_MAIN=core, cross-manager priority, v20-v31 locals (section 5) | | | | |

The street's 60 fps also needs the GPU (17.5 ms/frame at 1200 MHz) and the RSX thread (17.9 ms) below 16.7 ms; the main
thread's remaining 14 ms of guest code is flat and data-cache-miss bound, so the job side (M1, M3) and the store path
(M2) are the levers with a known size.

Order after the RSX breakdown (2026-11-18, rsx1; target: a stable 60 fps, i.e. ~1.6-1.7 us of RSX thread per draw at
9,000 draws, now 2.45 us at the plaza). With the short tail working at 16:10 (section 4) the RSX thread and the main
thread are both at ~95%: RSX work alone no longer moves fps much; the main thread (PPU + jobs run in its waits) needs
its own breakdown. RSX candidates ranked by per-draw saving (plaza, ~7,600 draws; 1% of the thread = ~0.025 us):

| # | Candidate | Expected per draw | Effort | Notes |
|---|---|---|---|---|
| R1 | DONE 2026-11-19 (pgo2): mg2r profiles (mg1's 5 routes + the street, 16:10 snapshot), build hd2 | measured 0.04-0.07 us (street 2.14 -> 2.10, plaza 2.47 -> 2.40 us per draw vs hd1) | -- | fd_draw 1.0e8 / vk_fd_record 8.0e7 entries now (zero in mg1r); plaza **50.65 vs 49.73 fps (+1.8 %)**, street 46.08 vs 46.34 (= hd1, main-bound); R4 can now be built on mg2r |
| R2 | Constants by draw index + multi-draw (no per-draw set bind; consecutive draws with one pipeline and set as one vkCmdDrawMultiIndexedEXT) | 0.1-0.25 us | large (VS / FS read constants at a draw-indexed base; Vulkan-only HLSL path; shader cache key) | RADV is 0.44 us per draw: draw-time flush 2.0% + DrawIndexed 1.5% + set bind; the NOBIND bound alone is ~0.09 us |
| R3 | Index-cache successor prediction with two successors per entry (or a per-frame sequence) | 0.02-0.04 us | small (noinline helper, PGO-safe; 56 B of padding free in eng_ic_entry) | probe misses 2.0% + 0.6%: only 57.5% of links are right |
| R4 | dl_scan with AVX2 compares (work/bench/rsx1-dlscan-simd.patch, `PS3RECOMP_ENG_DELTA_SIMD`) | ~0.04 us | small, but changes sink_end_body's PGO hash: needs R1 | rsx_draw_delta.inc:293-298 ~2% of the thread |
| R5 | Persistent descriptor sets across frames (or vkUpdateDescriptorSetWithTemplate) | 0.03-0.08 us | medium (lifetime of views / samplers / arena buffers) | ~770 sets made and written per frame: vkUpdateDescriptorSets 1.8% + alloc 0.2% + our set building ~1% |
| R6 | Fetch vertices straight into the arena (no staging memcpy) | ~0.03 us | medium | memcpy of staged vertices / indices 1.4% |
| R7 | Push constants for the per-draw offsets (item 4) | < 0.06 us | large | bounded by NOBIND (section 5) |

Item 6 (graphics thread +1.4% with t13d / `J204_TYPES`): the rb0 A/B for it was cut short (mgPr install) and 5 of 8
runs left the route; still open (cheap: mgPr vs mgPr `J204_TYPES=0x5`, >= 4 clean rounds). Then (c) SPU leftovers:
job 223 Edge (15.6 ms/frame, the biggest job), 218, 204 declines (item 2), 220 / 213 / 216; (d) Edge on GPU only with
a new idea (section 5); (e) main thread (now co-limiting), GPU / power. Items 0, 1, 1b and 3 are done.

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
4. (bounded 2026-11-18 by NOBIND, section 5: < ~0.5 ms of 19; see R2 / R7 above) Graphics thread: per-draw constant offsets as push constants (descriptor set only rebound when textures
   change): estimate revised to ~3% RSX (RADV already passes dynamic UBO offsets through its push-constant
   area, so the saving is only the bind/flush difference). Needs the HLSL generators (rsx_vp_decompiler.c
   emits `vp_c[(N + aK.c) & 511u]` / `vp_c[N]`, rsx_fp_decompiler.c `fp_constants[N]`, plus vp_posscale,
   vp_posoffset, fp_alpha; the same HLSL feeds D3D12/Metal, so Vulkan-only), the guest set layout
   (rsx_vulkan_backend.c ~3410, bindings VPCONST/PSCONST UNIFORM_BUFFER_DYNAMIC), 7 bind sites and a
   shader-cache key bump. Validate with det-bench screenshots. Low priority for the gain.
5. Job 218 FK / clip sampler widened like the blend: ~0.5-1% SPU.
6. Graphics thread +1.4% with t13d (not consistent between batches): check with a longer A/B (2026-11-18: rb0 cut
   short, inconclusive; see the table above).
7. Large, risky: WWS job manager natively (~7% SPU); Edge geometry on the GPU (~16% SPU).
