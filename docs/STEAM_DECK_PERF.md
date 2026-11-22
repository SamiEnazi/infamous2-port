# Steam Deck performance work (branch `deck-perf`)

Status and lab notes of the Steam Deck optimisation work: what limits the frame rate, how it is measured,
what was changed, what was tried and rejected, and what is next. The raw chronological log is
`work/bench/NOTES.md` (local, not in git); this file is the curated summary and is kept up to date.
Since 2026-11-19 the work continues on the local branch `deck-v063` = deck-perf + upstream v0.6.3 (section 7: the merge,
what each upstream addition costs, the A/B against the installed hd3; build mg3r = the hd4 candidate).
2026-11-20, roadmap step 6 (agent hd5): today's three pieces combined, defaults decided (play.sh PRIO 1 + R8 3, the store
lift on Linux builds), PGO retrain mg5r; build work/port/hd5c = the hd5 candidate (section 6 "Step 6 result", section 4).
2026-11-22, the upstream PR's video (agent cmp; NOTES.md "PR comparison: upstream v0.6.3 vs deck-perf PGO"): one
real-clock street take per build, recorded (Deck preset, 1280x800 window, 60 fps cap, HUD on, GPU 1100; upstream with its
own launcher env = 2 job threads): route 2-44 s **upstream v0.6.3 29.3 fps vs wide4 (= installed hd7) 47.8 fps
(+63 %)**, p99 44.3 vs 31.1 ms, > 50 ms 6 / 0 a minute, worst 55 / 48 ms; the 15 s of the video (route 2-17 s) 28.0 vs
46.0 (+64.5 %). Single takes: an example, not a statistic. Media: work/pr/compare-v063/ (fork: release v0.6.3-deck4,
branch pr-media). **Caveat found then: the Deck is docked to a 3840x2160 monitor, so the Deck-preset FULLSCREEN benches
(i2-*, inst*, wide-*) ran with a 4K swapchain, not 1280x800**; both sides of every A/B alike, so their percentages
stand. The takes above used `PS3RECOMP_VK_FULLSCREEN=0 PS3RECOMP_VK_WINDOW_SIZE=1280x800`.
2026-11-20, roadmap step 8 (agent sz2): the three big options sized with bench-only measurement modes (section 6 "Step 8
result"): the existing Vulkan recording worker on CPU 7 is +2.1 % on the street today; Edge's math off the CPU is worth
more than thought (contention with the main thread); ThinLTO and the other compiler-level call fixes give nothing.

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

### RSX thread on the street (2026-11-20, agent r2; NOTES.md "R2 graphics thread")

hd4's module + HEAD's engine, `PS3_NATIVE_JOBS_PRIO=1`, quick street (steered), guest 4-20 s: ~7,100-8,150 draws a frame,
the RSX thread 18.9-19.3 ms of CPU a frame (86-89 % busy). perf on `ps3-rsx` alone (17,627 samples, inlining resolved by
llvm-symbolizer) and the engine's own counters (`PS3RECOMP_LIMITS=1`, `PS3RECOMP_ENG_DELTA_STATS=2`,
`PS3RECOMP_VK_MERGE_STATS=1`, `PS3RECOMP_VK_CALLSTATS`):

| Part | Share of the thread | ms a frame | Biggest items |
|---|---|---|---|
| Vertex / index work (pair memo, index / vertex caches, fetch) | 20.6 % | 3.9 | vertex fetch 5.7 % (~810 fetches a frame), index-cache probe 2.9 % (its key compare: the hottest line, 1.95 %), pair memo 2.2 %, vertex-cache probe 1.4 %, index decode 1.3 %, remap 1.1 % |
| FIFO walk + method dispatch | 19.5 % | 3.7 | gcm_rsx_process_fifo 5.7 %, rsx_dispatch_burst 5.1 %, constant uploads 1.8 %, dirty bits 1.8 % |
| RADV (the driver) | 17.9 % | 3.4 | vkUpdateDescriptorSets 1.75 % (~610 a frame), the draw-time flush 1.7 %, vkCmdDrawIndexed 1.6 %, vkCmdBindPipeline 1.2 % (~370 a frame) |
| Our Vulkan backend | 9.2 % | 1.7 | descriptor sets 1.9 %, the body 1.65 %, constant windows 1.2 % |
| libc (memcpy 5.7 %, memcmp 2.0 %) | 8.8 % | 1.7 | constant rows and staged vertices into the arenas |
| Delta engine (dirty tracking, dl_scan) | 4.6 % | 0.9 | |
| Textures / programs / pipeline lookup | 9.3 % | 1.8 | texture slots 1.7 %, FP hash / reads / analysis 2.5 %, pipeline 1.6 % |
| Rest (sink_end_body itself, write tracking, short tail) | 5.4 % | 1.0 | |

By draw path (stage marks, ms a frame at 8,088 draws): all state reused 57 % of draws x 1.26 us = 5.8 ms; textures
changed 17 % x 3.88 us = 5.2 ms (a third of them make a new descriptor set); programs changed 2.7 % x 13.9 us = 3.0 ms
(program lookup / layout 3.4 us, vertices 2.7, backend 4.4 of it); all state + vertices reused 22 % x 0.67 us = 1.2 ms;
whole way 0.6 % x 28 us = 1.4 ms. The Vulkan calls inside the backend's draws: 2.3-2.5 ms a frame (one
vkCmdBindDescriptorSets with two new dynamic offsets + one vkCmdDrawIndexed per draw). No item above ~6 %: the cost is
the per-draw interpreter, spread. What fewer Vulkan draws can save, measured: neighbours with identical state and
constants (26-32 % of draws) as one multi-draw saves ~0.15 ms; ALL neighbours with one pipeline and set (76 % of draws,
the "+constants by draw index" form, which needs a shader change) at most ~0.7 ms (measured with a wrong-picture sizing
mode, section 6). Slack: with the priority on, standing in the street, +1 ms of RSX work costs +0.14 ms of frame (SE
0.30), +1.7 ms costs +0.86 ms (SE 0.24): ~1 ms of slack.

### Texture- and program-changed draws, step by step (2026-11-20, agent r8; NOTES.md "R8 texture/program draws")

Quick street, PRIO 1, stage marks (DELTA_STATS=2, ~16 ns a mark included) of three stats runs + r2's perf by line;
us a draw of the path (all-state draws for comparison):

| Stage | Textures changed (15-19 % of draws) | Programs changed (2.6-3.8 %) | All state (57-62 %) | What it is |
|---|---|---|---|---|
| begin (dl_scan) | 0.30-0.36 | 0.52-0.62 | 0.18-0.20 | the dirty texture / program blocks compared with the snapshot |
| program lookup + layout | -- | 2.8-3.5 | -- | FP memo (8 entries by address) misses ~255 a frame (programs uploaded again with other inline constants); content memo hits ~60, the other ~190 ran the whole analysis: structural hash byte by byte 0.85 % of the thread, four unit walks 1.1 %, size walk 0.4 % |
| vertices | 1.12-1.35 | 2.3-2.5 | 0.45 | a new mesh: vertex / index registers unchanged in only 1 % of texture-changed draws; pair memo 44-49 %, probe 36-38 %, fetch 13-21 % |
| texture units | 0.5-0.93 | 1.0-2.0 | 0 | 2.0 memo + 0.8 probe binds a draw; the probe = a texture's first bind in the frame (index + entry cache misses, 1.0 % of the thread), the 16-unit decode 0.8 % |
| pipeline lookup | 0.25 | 0.7 (5.9 while pipelines are made) | 0 | same pipeline 99 % (key memo), the texture-derived masks recomputed for 16 units |
| backend draw | 1.3-1.5 (set 0.65-0.8: 32 % NEW at ~0.75 us) | 3.9-4.4 (set 1.5-1.75: 67-72 % new; records 1.7-1.8) | 0.42 | new descriptor sets: vkAllocateDescriptorSets + vkUpdateDescriptorSets of 34 descriptors |

The two exact cuts with a size: the descriptor sets made again every frame (~520-600 a frame, R5) and the fragment
programs' whole analysis on every re-upload (~190 a frame); both built as `PS3RECOMP_ENG_R8` (section 4).

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

**The street frame, sized (2026-11-20, agent sz1; NOTES.md "sizing (agent sz1)"). This replaces the "capacity-bound
bursts" model below.** hd4's module, guest 4-20 s of the steered quick street, 46.5 fps, 21.5 ms frames; a timeline of
every job run, wait, kick (claim) and pool sleep (diag engine sz1d, `-DSZ1_DIAG`, 479 frames) and calibrated in-run
slow-downs (PS3_LIVEAB, 12-24 pairs each).

* **The main thread produces the jobs.** The anim manager (218 220 213 201 200 225: 34 ms of CPU a frame) gets ~325 jobs
  a frame, one flush each, from the main thread's game code between 1.3 and 18.6 ms after the flip; the render manager
  (204 216 210 212 208 209: 19 ms) 61 from the main thread at 11-21 ms plus 610 released by barrier cells; Edge (223: 16 ms)
  25 from the Edge kicker thread at the frame start. Every manager's last job is a main-thread kick ~1 ms before it ends
  (no long barrier chain). There are no bursts to make "capacity-bound": the pool is 76 % busy, ~1 of its 4 threads is
  asleep on average all frame after 5 ms (19 % of thread time asleep, 4.5 % spinning).
* **The main thread's 21.5 ms**: own guest code ~15.6 ms, jobs it runs in the world tick 1.7, runner waits 4.2 (1.4 of
  them running the awaited jobs itself, 2.8 pure waiting). The waits: (a) 1.46 ms right after Edge -- Edge's ~15 jobs are
  queued at 0.2 ms and hold all four pool threads to 4.3 ms; the ~60-80 anim jobs the main thread kicks at 1-4 ms queue
  behind them (the ready ring is FIFO across managers; the sticky pick is not the cause) and it waits for them at 4-8 ms;
  (b) ~1.0 ms for the tails of 213s it kicked ~0.3 ms before (36 waits a frame, 28 us each); (c) ~1.4 ms of frame-end render
  drain. The pool is 89 % busy during the waits.
* **Sensitivities** (ms of frame per ms inserted; SE): main thread at the flip 0.88 (0.35) for 1 ms, 0.45 (0.10) for 2 ms;
  main thread after each of its anim kicks 0 for 1.2 ms (all absorbed by its waits), 0.27 (0.08) for 2.1 ms -- the main
  thread has ~1.2-1.5 ms of slack a frame, and a FASTER main thread waits longer for the same jobs (the asymmetry: its code
  savings pay only where no wait follows, or with shorter job latency); Edge +2.4 / +5.4 ms of pool time 0.23 (0.16) / 0.13
  (0.045) (the frame follows Edge's end, CPU / 4, not its CPU); RSX thread +1.05 ms 0.04 (0.40) (~1 ms of slack), +5.2 ms
  0.79; SPU runtime per MFC command (55,000 a frame) +3.0 ms of CPU 0.18 (0.08); GPU 1200 -> 1000 MHz: frame -0.65 ms
  (0.22), fps +3.2 % (1.1) -- GFX -0.35 W, CPU +0.33 W, clock +43 MHz, main -0.59 ms: ~1.9 ms of frame per W moved from the
  GPU. Speed-up check of the Edge mechanism: Edge yielding to the other managers (`PS3_JOBS_PRIO=221`, bench): frame
  -0.49 ms (SE 0.15), fps +2.41 % (SE 0.73), 24 pairs; the post-Edge wait 1.46 -> 0.38 ms, about half of it comes back as
  other waits. **Not confirmed by the s1 round (2026-11-20, section 4 / 6 S1):** the proper priority and the prototype's
  own switches re-run on the s1 binaries give +0.3 % (SE 0.6) in-run (main -0.07 ms, SE 0.12), +1.25 % (SE 0.88) on the
  quick street and +2.06 % (SE 0.63) on the plaza; the traces show why the street gains little: the main thread's waits
  fall 0.5-1.0 ms a frame but its time outside them grows about as much -- it then waits for the RSX thread at the frame
  end (+0.25 ms of usleep polls) and polls more; its own code does not miss more (counters) and it is not kept off the
  CPU (run-queue delay equal).
* **The main thread's code** (perf stat on the main thread, 26 Zen 2 counters): IPC 1.10, only 31 % of uops from the op
  cache, uop queue empty 25 % of the cycles (frontend), ROB-full 5.2 % + store-queue-full 4.9 % + register file 2.7 %
  (backend), L1D refills 7.9 / 1000 instructions of which L3 0.88 and DRAM 0.71 (memory <= ~25 % of the cycles even if
  every one stalled fully), mispredicts 0.96 MPKI. Not mainly memory-bound: instruction count and code footprint are the
  code generator's levers.
* So the frame's critical path alternates the main thread's code and the latency (queue position + run time) of the jobs
  it waits for; job CPU that nobody waits for (most of 218 / 216 / 204) does not count, which is why the 216 and 218 natives
  moved nothing; Edge counts through its hold of the pool at the frame start; power counts everywhere (1 W ~ 2 ms).

**The main thread's code by instruction kind (2026-11-20, agent cg1; NOTES.md "main-thread code generator (agent
cg1)").** perf record of the main thread alone (cycles:u + instructions:u, s1c = HEAD's engine + hd4's module, quick
street, PRIO 1), the 1,050 hottest lifted functions disassembled and every sampled instruction classified
(work/bench/cg1/cgclass.py, stseq.py, ctxpos.py; sequences, not single instructions, because of skid). Whole thread
(instructions / cycles): lifted PPU code 61.5 / 67.5 %, jobs it runs 20.4 / 13.9, the out-of-line store path 7.4 / 5.1
(ppu_lean_st32 3.7 / 2.8, the coherency notify 2.5 / 1.5), other runtime 5.9 / 5.6, waits 4.0 / 7.0. The lifted code's
instructions:

| Pattern (lifter / runtime overhead) | Share of the lifted code's instructions | Share of the thread's | Notes |
|---|---|---|---|
| Store page-map test + re-check (shr, 2 x load / cmp $1 / jne per store) | 11.0 % | ~6.8 % | 40.6 % of it on stores that follow another store off the same register in straight-line code (prologue saves, structure fills), 12.5 % the first of such a run, 46.9 % alone; the store itself 1.0-1.3 % |
| Host stack spills / reloads | 10.0 % | ~6 % | 7.2 in the bodies (15 host registers for ~25 live guest ones), vector spills 2.6; statically 23 % of the code |
| Register-file (ctx) traffic | 9.9 % | ~6 % | GPR loads 3.1 (2.1 at function entry), GPR stores 3.0 (0.6 before returns, 0.5 before calls), VR 2.3, FPR 0.5, CR / LR / CTR 1.0 |
| Host frame of each lifted function (6 push / pop, rsp, call, ret) | 5.4 % | ~3.3 % | entry 2.8, returns 1.2, call sites 1.3 |
| 32-bit address / clrldi zero-extensions (mov r32,r32) + lea | 6.6 + 7.2 % | ~8.5 % | a guest address is a 32-bit sum: 35 % of the loads take their index from a 32-bit lea, 26 % from an add |
| rip-relative globals (vm_base, page map, TOC-ok bytes, icall table) | 3.0 % | ~1.8 % | |
| CR / XER bookkeeping, byte swaps, indirect calls | 0.4 / 0.2 / rare | -- | compares are fused compare + branch; MOVBE does the swap in the access |
| The guest's own work (alu, vector / fp, branches, compares, loads, copies) | ~45 % | | |

The overhead is spread over five patterns of 5-11 % each; the store test is the only one a small, exactness-safe lifter
change can take (section 4: `PPU_ST_CMPB` + `PPU_ST_RUNS`); spills, register-file traffic and the frames come from the
calling convention (every guest register through ctx at each call, ~25 guest registers in 15 host ones) and need a
different ABI between lifted functions.

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

**Main-thread jobs on the street (2026-11-19, agent m13; NOTES.md "main-thread jobs (agent m13)").** gpu2l2 (= hd3),
`PS3_TB_FRAMESTEP_IDLE_US=2000`; perf of the whole game (m13-pall), job recordings of 400 street jobs each of 213 and 216
(D/native/recordings/m13rec-213, -216) and the main1 timeline (m1d-1) re-read by kick index (work/bench/m13-burst.py):

| Job | Live per job (runner stats) | Where its time goes | How the main thread waits for it |
|---|---|---|---|
| 213 (anim, ~140/frame) | 37-42 us | its lifted code only ~10 us (replay, REPEAT=40: 10.7-11.6 us); the rest ~85 small synchronous GET-and-wait DMAs per job (pointer chasing through 0x123F8, the copies miss the cache) and the runner's staging | group 2 (71 jobs, kicked by the main thread after the world tick, 13.1-14.1 ms) token by token, but it runs on the whole pool |
| 216 (render, 6/frame) | 716-744 us | BSP clipper native 0x68D8 ~51 %, the lifted selection pass 0x4D80 ~15 %, other lifted fragments ~30 % (replay perf) | each long 216 job gates the 210/208 entries behind it (a barrier); the token wait ends when it ends (0.15-1.56 ms per wait) |
| 218 (anim, ~67/frame) | 260-282 us | upstream's blend / pose / palette + our FK / clip natives, a flat lifted rest (1.7 % of all samples, top fragment 0.18 %) | the main thread runs ~6 itself (world tick) and waits for the burst's end |
| 204 (render, ~520/frame) | 17-20 us | our native | the drain at the frame's end |

Pool threads (57 % of all samples): the SPU runtime is the largest block -- spu_wrch_cmd_fast 4.1 %, spu_wrch 3.1 % (the
out-of-line path: by its 32-byte copy loop mostly the runner's staging DMAs through wws_native.c WR(), plus atomics; no call graph), spu_indirect_branch 2.0 %, wws_native_run 1.8 %, ready_pop's spin 1.7 % of ALL samples (~12
ms of pool CPU a frame); about a third of the two DMA functions is the data copy's loads (cache misses).

Result of the round: two exact 216 changes (section 4) make 216 10-16 % shorter live (-24 % in the replay) and the main
thread does NOT get faster beyond noise (street, 12 rotated rounds: full window 49.80 vs 50.19 fps (4 / 6 clean), guest 4-20 s
46.80 vs 47.14 (n = 12, 1.5 SE), main 19.41 / 19.22 and 19.53 / 19.32 ms; plaza 51.53 vs 51.26, main 18.71 / 18.72 ms). Reason: both bursts are capacity-bound -- a shorter 216
opens its barrier sooner, but the main thread then waits for the rest of the burst, whose end is set by its total CPU over
~4.5 threads (-0.55 ms of pool CPU a frame = ~0.12 ms, below the 0.25-0.4 ms noise). **Rule of thumb (superseded 2026-11-20 by the sizing above: what counts is the latency of the jobs the main thread waits for, not burst CPU): 1 ms of CPU removed
from a burst's jobs buys ~0.2 ms of main thread; work on the main thread itself (its own 218 runs, the store slow path,
guest code) counts one to one.**

**Main thread round 2 on the street (2026-11-19, agent mt2; NOTES.md "main thread round 2 (agent mt2)").** Three
things, all `PS3_TB_FRAMESTEP_IDLE_US=2000`, rotated rounds:

* **The store slow path (M2) is not a lever.** Counted (`PS3_STSTAT`, section 3): the main thread makes 1.92 M out-of-line
  stores a second (~38,000 a frame), 96 % to a line no reservation ever marked, on the ~55 pages that hold a marked line
  (0x00A6E000 alone half of them: its one marked line is a live job-queue line, ~26,000 reservations a second); 4 %
  (~1,650 a frame) to a marked line, 8 a second on a line another agent held. Taking the 96 % inline (`PPU_ST_LINE`,
  section 5) removes them all and changes nothing: guest 4-20 s, 6 runs each, no-PGO modules: HEAD lift 44.96 fps (sd
  0.87), main 20.37 ms; inline 44.92 (0.66), 20.48; the same binary with `PS3_ST_LINE=0` 45.09 (0.44), 20.48; full window
  and plaza equal too (section 5). So the ~1 ms main1 put on the slow path was sample skid; the store calls cost < ~8 ns.
* **`PS3_CPU_PLACE_MAIN=cpu` (M7) is slower**: same exe (hd3's module + the mt2e engine), 8 rotated rounds: full window
  x0 50.01 fps (n = 7, sd 0.77) vs cpu 48.94 (n = 5, sd 0.74), guest 4-20 s 46.96 (0.48) vs 46.49 (0.62) (n = 8); main's
  own time falls 0.2-0.6 ms (pinned, no migrations) but it waits longer: the pool and SPU threads lose a CPU (6 -> 5).
* **Power: one clock for all four cores, set by the 15 W budget.** The SMU's metrics (gpu_metrics, section 3; street,
  hd3 module, GPU 1200, every run of these batches): socket 14.9-15.0 W (at the cap), CPU 9.1-9.4 W, GFX 2.3-2.7 W, SoC
  0.77 W, the rest ~2.4 W; the four cores always report the SAME clock, 2,930-3,060 MHz (max 3,500), core 3 (the RSX
  core) 1.6-1.8 W, the others 2.3-2.45 W each. perf stat per thread (mt2pw-x0, guest ~17-57 s): main 0.98 CPUs busy at
  2.74 GHz effective (user cycles / task-clock; its kernel time pulls it below the core clock), IPC 1.04; pool 3.28 CPUs at
  2.85 GHz, IPC 1.55; RSX 0.83 CPUs at 2.76 GHz, IPC 2.02; SPU threads 0.41 CPUs. Nothing per core is settable without
  root (cpufreq / amd-pstate files are root's), and per-core settings would not help: the SMU runs the cores at one
  clock; the system-wide governor `performance` (EPP performance, settable through steamosctl) gives no clock at the cap
  either (2,930 vs 2,950 MHz; section 5, with a warning). What moves the main thread's clock is the TOTAL power the
  rest of the APU draws: runs on a lighter path (GFX 2.7 W, CPU 8.9 W) ran at ~3,050 MHz vs ~2,965 on the route, and GPU 1400 vs 1200 MHz costs the main thread 1.1 ms
  (section 2 above). At P ~ f^2.5, 1 W less anywhere is ~+4 % clock, ~0.8 ms of main thread. So the power lever is less
  CPU burnt per frame by the other threads and less GPU draw, on top of the capacity rule (1 ms of burst CPU = ~0.2 ms of
  main). The pool's idle spin is not where the watts are: cutting ready_pop's 3,000 PAUSEs to 300 or 0 left the clock
  where it was (2,941 / 2,955 / 2,962 MHz on the route; section 6, P1). The watts go into real work: pool 3.3 CPUs, main 1,
  RSX 0.8 -- so a faster job (M1 / M3 / M8) helps twice, by capacity and by power.

**Job 218 round (2026-11-19, agent j218; NOTES.md "job 218 round (agent j218)").** hd4, runner stats of the walk
(`PS3_NATIVE_JOBS_STATS=5`): 218 is the biggest job on the street -- 261 us a job, 68 a frame, 17.8 ms/frame = 25.5 % of
all job time (plaza 245 us, 60 a frame, 14.7 ms, 21.4 %; there 223 is bigger). Where a live 218 job's time goes (perf of
the whole game, pool threads): its own code ~78 % (natives 51 %: upstream's blend k16320_ 16 %, our FK 11 %, the clip
sampler ~16 %, copy / root loop ~5 %; the lifted rest 27 %, 176 functions, flat), the SPU runtime ~22 %. The natives are
overhead-bound: in fk_core the FP ops themselves are ~5 % of its samples, the rest per-op flushes and magnitude checks,
spills, the estimate emulation. A replayed job is ~2.5x faster than a live one (SMT sibling, clock, caches).

| Change (parked as work/bench/j218-natives.patch, not in the tree; switch) | What | Replay us/job, street / plaza (best) |
|---|---|---|
| (hd4) | | 103.9 / 124.3 |
| blend zero-weight shortcut (`PS3_NATIVE_ANIM_ZSKIP`) | a block of eight bones where every bone has a zero weight in A or B copies records (B's where A's weight is zero, else A's stay) and computes only the new weights, with the blend's own statements; 33 % of the street's blocks, 48 % of the plaza's | 99.8 / 118.5 |
| FK first pass raw (`J218_FKRAW`) | 0x179C0's first pass without the per-op flush (raw ops + gathered magnitudes, flush at the stores and the frsqest input), as upstream's blend does | 97.8 / 115.0 |
| native 0x17280 (`PS3_NATIVE_ANIM_OFF=17280`) | the float-channel blend after every 0x16320 (the Switch fork's model, MIT; the blend's arithmetic) | 96.9 / 113.9 |

Live (counters batch, quick street, 4 + 4 runs): 218 per job 269.9 -> 246.6 us (**-8.6 %**), i.e. ~1.5 ms/frame less
job CPU. Exact: replays FULLPUT 400 + 400 new recordings and the whole valP2 set (table identical to hd4's), check modes in
the replays (0 of 132,864 + 26,000 calls differ) and one live check run on the street (0 of 7.08 M native_218.c calls
and 1.3 M FK calls differ). fps: **no change beyond noise** -- quick street (abseq, 8 pairs) -0.01 % (SE 0.61), plaza +0.38 % (SE 0.53, 8 clean pairs), in-run street-still (the three switches flipped together) main +0.06 ms (SE 0.22): the freed pool time becomes ready_pop spin. By rule 2 not a commit on its own (kept uncommitted behind its switches); not an hd5 candidate by itself.

**Installed 2026-11-20 21:20: hd6** (tag `hd6` = deck-perf 9abaf38, pushed; exe dir work/port/hd6c: engine 2a400630..., module af1bbc72...), full install: the player's folder on branch `installed` = hd6 (launcher with the recording worker on). Backups: `<player>/work/port/installed-hd5-20261120/` (both files), input.toml / settings.ini `.installed-hd5-20261120`; rollback = `git -C <player> switch -C installed hd5` + the two files back. Start test with the real launcher: PGO module, threaded recording on, the worker on the RSX core's other CPU, PRIO=1 / R8=3 / VK_THREADED=1 / VKW=1 in the game's environment, no errors.

**hd6 candidate (2026-11-20 20:30, orchestrator):** work/port/hd6c = hd5c's module + an engine retrained with the recording worker on (PGO mg6r engine, mg5r module; build 38 s), launcher default `PS3RECOMP_VK_THREADED=1 PS3_CPU_PLACE_VKW=1` (commit above). vs hd5c, same batch: street guest 4-20 s +0.65 % (SE 0.42, 8 pairs, inconclusive), RSX 16.7 -> 14.3 ms; plaza +0.44 % (SE 0.67, 8 pairs), RSX 18.0 -> 16.2 ms; standing 5 min each 59.87 vs 59.81 fps, none > 50 ms. The main thread is the limit now. Lesson: an abseq ACCEPT after 3 pairs (sz2's +2.12 %) overestimates; re-measure early stops before relying on their size. Outputs: work/bench/hd6-check.out.

**Installed 2026-11-20 16:45: hd5** (tag `hd5` = deck-perf e9e05ea, pushed; exe dir work/port/hd5c: engine 0ad34587..., module 04bd9e54..., PGO mg5r) = hd4 + job priority + cheaper store checks + R8, full install: the player's folder on branch `installed` = tag hd5, so its tools/play.sh sets PS3_NATIVE_JOBS_PRIO=1 and PS3RECOMP_ENG_R8=3. Backups: `<player>/work/port/installed-hd4-20261120/` (both files), input.toml / settings.ini `.installed-hd4-20261120`; rollback = `git -C <player> switch -C installed hd4` + the two files back. Start test with the real launcher: PGO module, window, controller thread, placement, PRIO=1 / R8=3 / pad 4 ms / 4 job threads in the game's environment, no errors.

**Installed 2026-11-19 18:50: hd4** (deck-perf 7c3c1e8 = hd3 + upstream v0.6.3, PGO mg3r; exe dir work/port/mg3r: engine 5fecd563..., module 508d9e5c...) replaces hd3 in the player's folder. 19:10 full install: the player's folder (the main checkout, was v0.5.7 source + hand-edited launcher) now has branch `installed` = tag `hd4` (22a90db) checked out, so its launcher is the branch's tools/play.sh (v0.6.3's + the Deck settings) and the whole folder is one version; the old launcher is on branch `backup/v057-launcher-20261119`. The new launcher copied settings.ini into the folder once (the original stays in ~/.config/infamous2-port). Next installs: `git -C <player> switch -C installed hdN` + the two binaries into work/port/build. Backups: `<player>/work/port/installed-hd3-20261119/` (both files), `tools/play.sh.installed-hd3-20261119`, `input.toml.installed-hd3-20261119` (v0.6.3 rewrites input.toml once: START = Esc instead of Enter, `layout = 2`). Smoke test of the installed files (street route, real controller, a copy of the player's input.toml): exited cleanly by itself, no errors, controller thread on.

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
  * **mg5r** (2026-11-20, agent hd5; the hd5 candidate hd5c) = mg5 training x5 + `deck.profdata` x2, as mg3r. mg5 = mg3's
    six runs (work/bench/pgotrain-mg5.sh: RUNENV = `PS3_NATIVE_JOBS_PRIO=1 PS3RECOMP_ENG_R8=3` in every run) on the
    instrumented build pgogen5 with the store lift (HEAD 1ffe5c6 + rebuild_win.py's Linux default); raws train-mg5. With
    the store lift every lifted function with a store has a new CFG hash: deck.profdata's records for them match nothing
    (the training alone trains them). mg3r = hd4's (2026-11-19, agent mg3: the same recipe on 2335d44).
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
* Job internals (2026-11-19, m13): `PS3_NATIVE_JOBS_STATS=5` (cheap, runner lines every 5 s: per-module jobs/s and us/job
  live) and `work/bench/m13-jobstats.py <prefix> <labels>` (their means per A/B label over the walk); recordings of 400
  street jobs each of 213 / 216 in D/native/recordings/m13rec-213, m13rec-216 (`work/bench/m13-rec.sh`: PS3_JOBREC_MOD=213,216
  MAX=400 EVERY=11 GAP_MS=20 AFTER=40); `work/bench/m13-repprof.sh <jrec dir> <exe> <out.data>` = perf of a 1-thread replay
  with REPEAT (the job's functions without the live noise; jr_* = the harness, ~45 % of the samples);
  `work/bench/m13-burst.py <rec> <frame> <n>` = one frame's jobs by manager slot and kick index with the main thread's
  waits and their targets (from main1-ftana.py's ftana.pkl); `work/bench/m13-symmod.py` sums a `perf report --sort comm,sym`
  by thread group and job module. Replay timing: interleave the configs and take the second pass (the first run after a
  build is cold: 362 vs 317 us on the same binary).
* Light pass check (gpu2): `PS3RECOMP_VK_LIGHT_CULL=2` renders the original light shader too every 60th light pass, reads
  both outputs back and compares them byte for byte (`[light-cull] check` lines; the run is slow, only for exactness).

* Slow guest stores (2026-11-19, mt2): `PS3_STSTAT=<s>` in an engine built with work/bench/m2-diag.patch (diagnostic, not in
  the tree; exe work/port/mt2d = HEAD + it) counts every out-of-line lifted store (ppu_lean_st<N>) of the main thread by page
  and line (marked or not, held by another agent at that moment or not), other threads in total, and every reservation per
  line by kind (PPU lwarx / SPU getllar); a dump every <s> s in stderr.log (`[ststat]` lines: totals, top 30 pages with
  their page byte and marked lines, top 30 marked lines with their reservation rates).
* Power and clocks without root (2026-11-19, mt2): the SMU's metrics table `/sys/class/drm/card0/device/gpu_metrics` (v2.4,
  168 bytes, world-readable) has socket / CPU / SoC / GFX power (u16 mW at 0x28), the four cores' power (0x30) and current
  clocks (0x58, MHz), GFX clock (0x4C). `work/bench/mt2-power.py <rec> [delay] [dur]` samples it at 20 Hz from anchor+delay
  for dur s into <rec>/power.tsv and (unless PERF=0) runs `perf stat --per-thread -e cycles:u,instructions:u,task-clock`
  on the game for the same window (perf_event_paranoid 2 allows it for our own process); `mt2-powwatch.sh <prefix>` starts
  it (PERF=0) for every new recording of a batch; `mt2-powsum.py <rec>...` prints the means and, per thread group, the
  effective clock (user cycles / task-clock) and IPC. Settable without root: nothing per core (cpufreq / amd-pstate files
  are root-only); system-wide through steamosctl (D-Bus): CPU governor (powersave / performance = EPP balance_performance /
  performance), boost, TDP limit (3-15 W, 15 = max), CPU scheduler (none / lavd), GPU clock / performance level.
* v0.6.3 merge round (2026-11-19, mg3; section 7): `work/bench/mg3batches.sh F|A|P|S` (the round's batches; mg3rot.sh =
  gpu2rot2.sh with the EPP logged per run, pause file `<scratchpad>/mg3.pause`), `mg3-watch.sh <prefix>` (per run:
  mt2-power.py PERF=0 + `mg3-padcpu.py` = CPU ms/s and context switches per thread name, anchor+12..52 s),
  `mg3-labelsum.py <prefix> <labels>` (their means per label), `mg3table.py` (gpu2table.py + SE, and p99 / > 33 ms for
  guest 4-20 s), `mg3build.sh <name> <module pgo> <engine pgo> <rebuild args>` (the build command above, waits for the
  game lock), `pgotrain-mg3.sh`. SDL pads in a bench: `PS3_PAD_SDL=1` (linux_run.py sets 0); on this Deck SDL then opens
  the physical controller through HIDAPI ("Steam Deck Controller", 28de:1205) and the game's rumble really vibrates it.
  The sdl2-compat race outside the game: `mg3-sdlrace.c` (LOCK=0/1/2, HOTPLUG=n) with `mg3-sdlrace.sh <out> <bin> <runs>`.
  Upstream's own tests that run here: `tests/jobrunner/test_jobrunner.py` (distrobox, PATH with E's LLVM; `--runner
  <file> --expect-broken` for an old runner), engine/libs/input/tests/test_pad_thread.c / test_pad_hotplug.c (need stubs
  for SDL_GetVersion and our frame-step clock hooks on Linux).

### How to measure (fast) (2026-11-19, agent ft1; NOTES.md "faster testing (agent ft1)")

Triage only: a change that passes these still gets the full gate (street + plaza A/B with >= 4 clean rounds, the standing
run, the job replays) before an install. All tools in work/bench; the engine side is bench only and off by default.
What to use: a switch-only question -> one in-run run (`liveab.sh`, 3-4 min) for its mechanism (clock, power, GPU, thread
ms: precise) and big fps effects (>= 2-3 %), or `abseq.sh` on quick street runs (3 pairs = 7 min for >= 2.5-3 %, up to 8
pairs = 18 min); a code change -> `ft1build.sh` (seconds when no lift input changed), the counters' reject-only filter
(4 + 4 quick counter runs, 9 min), then `abseq.sh`; the full A/B -> street runs with the steering (every run on the route:
no discarded rounds) in rotated pairs with the early-stopping rules. Small effects (0.5-1 % fps) stay expensive: the
content noise (the crowd) is ~1 % per run whichever way it is measured.

**The HUD is on in every test run since 2026-11-19 19:30** (the player's request: linux_run.py's default, zrun.sh /
stand2.sh; HUD=0 turns it off). Both sides of an A/B always share the HUD state; never compare a batch of before with one
of after. Measured: HUD off vs on, in-run (street-still, 12 pairs): fps +0.5 % (SE 1.2), RSX thread -0.25 ms (SE 0.16),
GPU 0.0 ms, power equal: below the noise. Its CPU line has no sampler on Linux (nothing to switch off).

* **The A/B inside one game run** (`PS3_LIVEAB`, runtime/diag/ps3_liveab.c): `EXE=<exe dir> work/bench/liveab.sh <tag>
  <scene> "<switch>=<a>|<b>;block=300f;settle=60f;blocks=24" [ENV=V ...]` -- one run (~3-4 min) flips the switch in blocks
  (order abba) and prints per block fps, CPU ms per frame per thread group, GPU ms, the SMU's power and clock; then
  `work/bench/liveab.py <rec>... [--cross]` = paired differences with SE. Scenes: `spot2-still` (plaza save, standing,
  ~64 fps; use start=1050f), `street-still` (street save, walk 16 s, stand), `street-pan`, `street-walk` / `street-walk2`
  (the det route / route v2; content changes along the walk, so pair two runs of opposite phase: `first=b` + `--cross`).
  Switches: PS3RECOMP_VK_LIGHT_CULL, PS3_PAD_THREAD_US, PS3_CPU_PLACE_MAIN (re-pins at the flip), PS3_DBG_FLIP_DELAY_US,
  PS3_DBG_PRESENT_DELAY_US, PS3_JOBDELAY_US, PS3_OVERLAY_HUD, `null` (A/A). New switches: read through g_ps3_lab[] (a site
  that caches its env value keeps the cache and takes the override when it is >= 0). Settle: power split < 0.5 s, clock
  < 1 s, fps ~1.5 s after a flip (trace=1, liveabtrace.py): settle=60f is enough.
  * Validation: light tiles on vs off +4.7..+6.0 % on the still street (GPU -5.2 ms, clock +120-130 MHz, main -1.0..-1.3 ms;
    SE 1.1-1.2 % per run), +5.7 % at spot 2 (SE 0.65, one 2.9 min run); reference +3.2 % on the street WALK (GPU -4.3 ms):
    same sign and mechanism, larger in the standing views (their light pass costs more). PS3_CPU_PLACE_MAIN=cpu: NOT
    reproduced (spot 2 -0.02 % SE 0.41, still street +0.48 % SE 0.60, two runs each; reference -1.0 % walk guest 4-20 s at
    1.7 SE, -2.1 % full window). Controller thread 4 ms vs 1 ms (PS3_PAD_SDL=1): +2.2 % (SE 1.0), ps3-pad's group -0.47
    ms/frame; reference +1.4 % (1.9 SE): agrees. Each of these: one run of 3-4.4 min (the reference A/Bs: 20-40 min). On
    the WALK (street-walk, 4 runs crossover, blocks of 8 guest s): light tiles +1.25 % (SE ~1) over guest 4-20 s (the
    unsteered route split after that), reference +3.2 %: same sign, ~2 SE low.
  * Noise (A/A): pair sd 3.1 % fps at spot 2, 4.9 % on the still street (300-frame blocks): the content (the crowd) differs
    from block to block, so the fps SE per run is ~0.9 % (spot 2) / ~1.4 % (street) -- no better per minute than between
    runs. The mechanism metrics are precise in one run: clock SE 5-8 MHz, power 0.03-0.05 W, GPU 0.05-0.1 ms, thread ms
    0.1-0.27 ms. Use it for (a) big effects (>= 2-3 %: one run), (b) power / clock / GPU / thread-time questions (one run),
    (c) causal delays (below). It cannot settle 0.5-1 % fps questions in 10 minutes; nothing here can (see the counters).
  * Limits: the effect is the scene's (standing views differ from the walk); a switch must be safe to flip at any frame;
    effects that need seconds to build up (caches, pools) need longer blocks and settles.
* **Causal delay per job** (`PS3_JOBDELAY=<module>:<us>[:all|main|pool][:mwait|spin]`, wws_runner.c; default off): every job
  of that module takes <us> longer before its completion is published; `mwait` = MONITORX/MWAITX with a TSC timeout (user
  mode on Zen 2: the thread waits in C0 without drawing power), `spin` = PAUSE. Flip it in-run with
  `PS3_LIVEAB="PS3_JOBDELAY_US=0|<us>;..."`; the block lines carry `ev` = delays per frame. Validation on job 218: pool
  only, 100 us (spot 2, ~16 delays a frame = +1.6 ms of burst time): frame +0.30 ms (fps -1.9 %, SE 0.7) = 0.19 ms per ms,
  the rule of thumb's 0.2 (here without the power part). Main only, 200 us x 3.9 a frame (= 0.78 ms of the main thread's
  own 218 time, street-still): frame +0.12 ms (SE 0.18, mwait) / -0.08 ms (SE 0.27, spin): NOT one to one -- the main
  thread's 218 runs sit inside its waits for the anim burst (it helps while it waits), so they cost like burst work.
* **More causal delays and the job timeline** (2026-11-20, agent sz1; uncommitted, patch work/bench/sz1-hooks.patch; NOTES.md
  "sizing (agent sz1)"): `PS3_DBG_FLIP_DELAY_KIND=mwait` (the flip delay as MWAITX; it runs on the main thread),
  `PS3_DBG_DRAW_DELAY_NS` (per guest draw, RSX thread), `PS3_DBG_DMA_DELAY_NS` (per SPU MFC command, every thread),
  `PS3_KICKDELAY=<mgr>:<us>:after` (the main thread after each flush of one manager), `PS3_LAB_GPUCLK` (the GPU clock flipped
  in-run through work/bench/sz1-gpuclk.sh), `PS3_NATIVE_JOBS_STICKY` / `PS3_JOBS_PRIO=221` live (since s1: `PS3_NATIVE_JOBS_PRIO=0|1|2`, the runner's priority classes; the prototype is gone); work/bench/sz1-fms.py = the
  paired frame-ms difference and the sensitivity. MWAITX wakes ~0.4 us late (a 25-100 ns request takes ~420 ns; use
  `_KIND=spin` below ~1 us) and leaves the core to the SMT sibling without drawing power, so MWAITX slow-downs look cheaper
  than real work. The diag engine (`CEXTRA=-DSZ1_DIAG work/bench/sz1build.sh`) adds ftrace kinds 21 (every job run with its
  queue latency), 20 (waits), 24 (every claim: who kicked which jobs), 25 (pool sleeps); work/bench/sz1-tl.py / sz1-tl2.py /
  sz1-chain.py read them (the typical frame in 1 ms bins, the waits by the job that ended them, chains).
* **Quick street run** (`ZONE=streetq detstreet.sh`, bench-street-quick.steps): the street route only to real 30 s after
  the mark, measured over guest 4-20 s (the stretch every run walks: no run leaves the route): ~68 s a run instead of
  ~142 s. Per-run sd ~0.9 % fps.
* **The street route's split (item 5)**: at guest ~17-19 s Cole passes the cathedral gate's round stone pillar with a
  margin of a few degrees of heading; he moves relative to the follow camera, whose heading differs a little from run to
  run (the guest's own run-to-run variation; no long frames before the gate), so he either passes it (on to the cathedral
  balustrade, ~8,200 draws) or runs into it / onto other walls. Open-loop nudges (route v2, v3) did not reproduce the path
  (v2: 9 of 10 runs on ONE path, but into a wall by the steps: 5,600-6,250 draws). **Fix: closed-loop steering**
  (`PS3_PAD_STEER=00A93440:<absolute path of work/bench/zone2/steer-ref2.txt>:160:6000:19500`, cellPad.c, bench only; the
  game opens the file from its recording folder, so the path must be absolute): the street's copy of
  the player position (guest 0x00A93440 = (x, y, height, 1) in cm on this save; live until guest ~20 s, when Cole leaves
  the street) is steered toward the recorded path of an on-route run (steer-ref2.txt, made with `PS3_PAD_STEER_REC=
  00A93440:<file>`) 600 ms ahead: **11 of 11 runs on the old route's path** (ft1-st1..11: draws per segment guest 20-30 /
  30-40 s 8,114-8,287 / 8,071-8,282 vs the route's reference 8,241 / 8,218; shots at real 30 s vs the reference end shot
  7.6-8.6 % of pixels differ, on-route runs of the old route 9.9-15.2 %; the unsteered route: 3 of 4 here, ~50 % over the
  earlier batches). Timing A/B with it over the full window (light tiles off vs on, 8 rotated pairs, 16 of 16 runs on the
  route, 38 min): -3.79 % (SE 1.27; INCONCLUSIVE by 0.02 SE), reference +3.7 % for tiles; the full window's pair sd 3.6 %
  (the crowd at the balustrade) vs 1.4-2.5 % over guest 4-20 s. Pass it to every street run (`detstreet.sh <tag> PS3_PAD_STEER=...`;
  both sides of an A/B). Another save, another route or a game build that moves the heap needs the address found again
  (PS3RECOMP_ENG_MEMSNAP dumps + work/bench/ft1-posfind.py, NOTES) and a new reference run.
* **Early stopping** (`work/bench/abseq.sh "a=<exe>[|ENV..]" "b=<exe>[|ENV..]"`, rules in abseq.py, fixed): rotated pairs
  of quick street runs (AB, BA, ...), d = fps B/A - 1 over guest 4-20 s; from pair 3: ACCEPT at >= +3 SE, REJECT at <= -3
  SE, NO EFFECT when the 95 % interval sits inside +-0.5 %, at 8 pairs INCONCLUSIVE; SE = max(sd, 1.2 %)/sqrt(n) (the
  floor: two lucky pairs cannot fake a small SE). Live: light tiles off vs on REJECT at pair 3 (-2.09 %, SE 0.69) in 6.8
  min (reference +3.2 %). Replayed on old batches (`abseq.py replay <batch .out> <a> <b>`): light tiles street (g2f) ACCEPT at
  pair 3, plaza (g2p) at pair 4; PLACE_MAIN (m7s, -1.0 %), mg3r vs hd3 (mg3s), the controller period (mg3f, +1.4 %), M2
  (m2s), 216 (m13s): all INCONCLUSIVE at 8 pairs (or their 4-6), as their verdicts were "within noise". With between-run
  noise (pair sd 1.4-2.5 %) NO EFFECT within +-0.5 % is out of reach (it needs SE <= 0.2 %: ~40-150 pairs).
* **Work counters** (`work/bench/ctrstreet.sh <tag>` = quick street run + ctr.py: perf stat per thread, cycles:u /
  instructions:u / task-clock in 0.5 s intervals, and the SMU's power; `ctrsum.py <rec>... [-g g1:g2] [--noise]` per frame
  and thread group over the guest window). Noise (4 runs of one binary, guest 4-20 s; sd per run): cycles per frame main
  1.0 %, RSX 0.6 %, pool 1.3 %, SPU threads 0.6 %, other 1.0 %; instructions 0.8-1.4 %; socket power 0.12 W (0.8 %), CPU
  0.04 W, GFX 0.08 W, clock 27 MHz; fps 0.9 %. **Rule (reject-only):** with n runs a side, a group "moves" when its
  cycles per frame (or a power figure) differ by more than 2 x sd x sqrt(2/n) (n = 4: 1.4 x sd); reject a code change only
  when NO group's cycles and no power figure move. Changes to scheduling, placement or waiting skip the filter (they
  change fps without changing work). perf costs ~1-2 % fps while attached: compare counter runs only with counter runs. **Limit found by j218 (2026-11-19):** a job-side saving does not show in the pool's cycles: below saturation the pool threads spin (ready_pop) when out of work and cycles:u counts the spin (218 -8.6 % per job = ~1.5 ms/frame less job CPU, pool cycles -0.2 %); for job natives read the runner's us per job (`PS3_NATIVE_JOBS_STATS=5`) instead.
* **Builds**: `work/bench/ft1build.sh <name> <module pgo> <engine pgo> --from port` = mg3build.sh + PORT_LIFT_REUSE=1 (a lift
  step whose inputs did not change is skipped; byte-identical lift outputs keep their old times), `SEED=<build dir>` starts
  a new module dir as a copy (only what changed compiles). Two fixes in tools/rebuild_win.py: on Linux the lifted files'
  old times were given back only AFTER the module compile (so every re-lift compiled all 132 module TUs, twice); the five
  patched port files were rewritten on every build (now only when their bytes change). Measured (PGO use, 4 jobs): nothing
  changed 1 s; one native changed 2 s (1 TU); an engine file changed 2 s; a fresh engine dir 32 s; a re-lift (lifter /
  natives.txt / rebuild_win.py change) ppu 381 s + SPU/jobs 40 s, then only the changed files compile; the whole module
  from scratch 411 s. Byte checks: a reverted incremental build == a full compile of the same tree (module .so identical;
  engine identical); a fresh engine dir differs from an incremental one only in .debug_* (the build dir's path). No
  compiler cache in the build container (none installed; not added).

* **RSX thread tools** (2026-11-20, agent r2; NOTES.md "R2 graphics thread"): `work/bench/r2-stats.sh <tag> <exe dir>
  [ENV=V ...]` = one quick street run (steered, PRIO 1) with the RSX counters on ([delta] / [delta-cost] per draw path,
  [merge], [vk calls], [track-c]; add `PS3RECOMP_ENG_DELTA_STATS=2` for the per-stage split, ~0.3 us a draw of its own);
  `PTHREAD=rsx PSTART=3 PSECS=10 ZONE=streetq work/bench/zprof.sh` + `work/bench/r2-inl.py <perf.data> <exe> [n]` (samples
  symbolised with their inline chains by E's llvm-symbolizer: by file, innermost function, outer -> first inlined, line)
  + `work/bench/r2-cat.py` (categories); RADV / libc by FDE range: work/bench/r2-fde/fdegroup.py with rsx1's FDE maps (radv-fde.txt, libc-fde.txt, vkep.txt = entry points)
  (same RADV .so since Oct 15). `PS3RECOMP_VK_MULTIDRAW=3` = the multi-draw replay check (section 4); `=9` = sizing of
  "constants by draw index" (wrong picture).

* **R8 tools** (2026-11-20, agent r8; NOTES.md "R8 texture/program draws"): `PS3RECOMP_ENG_R8=<mask>` (section 4; flips
  live: `PS3_LIVEAB="PS3RECOMP_ENG_R8=0|3;..."`), `PS3RECOMP_LIMITS=1` adds [r8-dset] (kept sets: table hits, new sets,
  tables given up, pool resets, destroyed views / buffers per frame) and [r8-fp] (structural memo hits / misses, branch
  programs, verify) lines; `PS3RECOMP_ENG_R8_VERIFY=1` = every structural-memo hit analysed the whole way and compared;
  the check build (`CEXTRA=-DR8_CHECK work/bench/sz1build.sh <name> ... --only engine`) + `PS3RECOMP_ENG_R8_CHECK=<n>`
  = every n-th frame a draw bound with a kept set from an earlier frame is repeated with it and with a twin set written
  now, into scratch targets, compared byte for byte ([r8-check]); work/bench/r8-check.sh (street + plaza check runs),
  r8-stats1.sh (stats runs), r8-chain1.sh (the A/B chain), r8-sema.py (RSX semaphore waits per run from ftrace.txt), r8-stats3.sh + r8-cost.py (the per-path stage split of stats runs, street proper),
  r8-diag-delta.patch (the part:textures counters: offsets-only share, vertex registers clean), r8-stage.py `<perf.data>
  <exe> <function regex>` = r2-inl.py by inline chain (every sample whose chain names a function, by chain and by line).

* **Main-thread code tools** (2026-11-20, agent cg1; NOTES.md "main-thread code generator (agent cg1)"):
  `EXE=<exe dir> ZONE=streetq PSTART=3 PSECS=12 work/bench/cg1-prof.sh <tag> [ENV=V ...]` = perf record of the main thread
  with cycles:u AND instructions:u (one perf.data); `perf script -F period,event,ip,sym,symoff,dso` + work/bench/cg1/cat.py
  (by kind of code), cgclass.py <objdump of the hot functions> <script> (the lifted code's machine instructions by kind,
  weighted by samples; objfilter.py keeps the hot functions of one `objdump -d` pass, ~25 s), stseq.py (the store
  page-map test sequences and their straight-line runs), ctxpos.py (register-file / spill / frame traffic by position:
  entry, around calls, before returns). work/bench/cg1build.sh = ft1build.sh + `LIFTENV="PPU_...=1 ..."` (lift switches
  into the build container); cg1-ctr.sh = rotated counter pairs; cg1/abpair.py = paired b - a with SEs of an abseq
  output section (fps, main, RSX, GPU, p99, max, > 33 / > 50, draws); cg1/runcheck.py = the independent check of a
  `PPU_ST_RUNS` lift's text. Note: PORT_LIFT_JOBS is in the lift fingerprint (cg1build.sh uses 4, ft1build.sh 2): changing
  it re-lifts once (byte-identical).

* **Sizing modes** (2026-11-20, agent sz2; uncommitted, work/bench/sz2-hooks.patch; NOTES.md "sizing the big options (agent
  sz2)"; all default off, flip live with PS3_LIVEAB, a WRONG or frozen picture in their on state): `PS3_DBG_EDGE_SKIP=1`
  (natives/spu/edge223_native.c: Edge's skinning and vertex decompression copy each segment's outputs from a memo of the
  first time it was met -- the same geometry, frozen poses; the `[edge] (sz2) ... memo` exit line counts hits),
  `PS3_DBG_VK_SKIP=1` (rsx_vulkan_backend.c: the draw engine's four draw entries swapped for stubs once a frame: no Vulkan
  draw recorded; only when the variable is named does the engine get the mutable table), `PS3_DBG_SMT_SPIN=<duty %>`
  (ps3_liveab.c: a busy thread on CPU 7). Tools: work/bench/sz2-chain.sh / sz2-chain2.sh (the round's runs),
  sz2-jobs223.py (job 223 from a PS3_NATIVE_JOBS_STATS=5 run), sz2-jrec223.py (Edge's segments / vertices / output from
  job recordings). `PS3RECOMP_EDGE_TSC=1` (existing) = cycles per Edge kernel. ThinLTO for the module: `PORT_LIFT_LTO=thin`
  (work/bench/sz2-thinlto.patch, tools/build_module.py; measured: nothing).

* **Combination / gate tools** (2026-11-20, agent hd5; NOTES.md "combined check and hd5 (agent hd5)"):
  `work/bench/hd5rot.sh <rounds> "<label>=<exe dir>[|ENV=V ...]" ...` = mg3rot.sh's rotated rounds for any number of labels
  with abseq.sh's output line (`ZONE`, `G`, `COMMON`, `PREFIX`, `FIRST`; `REV=1`: even rounds backwards, so with 3+ labels
  each pair runs in both orders; pause file `<scratchpad>/hd5.pause`); `work/bench/hd5pair.py <out>... --base <label>
  [--win g1:g2] [--keep-suspect]` = per label means and every label against the base paired by round (fps %, main / RSX
  / GPU / W / p99 / max / > 33 / > 50 / draws, SE over the rounds; `--win` recomputes with zstat.py over another guest
  window, e.g. the street's 4:20 next to its full window); `work/bench/hd5-longf.py <rec>... [-g g1:g2] [-t ms]` = the
  long frames with their guest time (the street's guest-16.4 s frame is in every run: the frame-stepped clock's
  artifact). Gate driver `work/bench/hd5-gate.sh "<steps>"` (st / pl / sd / rp / jr; HD5ENV = the candidate's switches).
  NOTE: hd4's engine (mg3r) predates PS3_PAD_STEER, so in a gate against hd4 its street runs are unsteered (about half
  leave the route after the junction): plan ~2x the rounds for its full window, or use guest 4-20 s.

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
| (uncommitted in D: tools/play.sh, tools/rebuild_win.py; exe dir **work/port/hd5c** = HEAD 1ffe5c6 + the store lift + mg5r profiles; hd5, 2026-11-20; section 6 "Step 6 result") | **hd5 candidate, the step-6 defaults**: tools/play.sh sets `PS3_NATIVE_JOBS_PRIO=1` and `PS3RECOMP_ENG_R8=3` (Linux launcher; `:=` defaults, =0 from outside = the old paths); tools/rebuild_win.py lifts Linux builds with `PPU_ST_CMPB=1 PPU_ST_RUNS=1` (step_ppu, `sys.platform` linux; Windows builds keep the old lift; =0 in the environment = the old lift; the change is in the lift fingerprint: one re-lift); PGO retrain mg5r (mg3's training with the two runtime switches, x5 + deck.profdata x2) | in combination (no-PGO modules, 6 rotated rounds of 5 configs): all on vs none street +2.6 % (SE 0.51), plaza +2.1 % (SE 1.07); all on minus PRIO / store lift / R8: street -0.76 (0.48) / -2.39 (0.73) / -1.69 % (0.45), plaza -0.56 (1.01) / -0.22 (0.84) / -1.42 % (0.92); gate vs hd4: street full window +2.63 % (SE 1.11, 6 pairs), guest 4-20 s +1.65 % (SE 0.46, 13 pairs, main -0.40 ms, RSX -0.43, p99 -1.4), plaza +1.70 % (SE 0.47, 4 pairs), standing 2 x 10 min 59.80 vs 59.75 fps (p99 17.8 / 17.8, > 33 ms 3 / 2, > 50 0 / 0); replays identical, jobrunner 15/15, stale profiles: module 0, engine `main` only |
| (uncommitted in D; work/bench/r8-cuts.patch, exe dirs work/port/r8 = this engine + hd4's PGO module (measured), r8f = the final tree (r8 + two cold lines), r8c / r8cf = their check builds (`CEXTRA=-DR8_CHECK`); r8, 2026-11-20) | the texture- and program-changed draws (R8) + R5 (section 1 "Texture- and program-changed draws"), `PS3RECOMP_ENG_R8` = bit mask (**default 0** = HEAD's code paths; flips live with PS3_LIVEAB): **bit 1 = R5, descriptor sets kept across frames** (rsx_vulkan_backend.c: a table per frame-in-flight slot of sets from that slot's pool, the pool reset only past 8,192 sets; key = the old key + each unit's image serial, so a dead view's handle value never meets an old set; the uniform arena's destruction gives the tables up) -- new sets ~520-600 -> 40-69 a frame on the street, 67 on the plaza; **bit 2 = the fragment program's structural memo** (rsx_draw_engine.c: after a content-memo miss, a program whose instruction blocks were analysed before -- the same program uploaded again with other inline constants -- takes the structural hash and the four unit masks from a 1,024-entry table; the constants are still collected from its bytes; programs with a branch the whole way) -- 95-96 % of the ~190 whole analyses a frame; `PS3RECOMP_ENG_R8_VERIFY=1` (memo hits analysed and compared), `PS3RECOMP_ENG_R8_CHECK=<n>` (check build: every n-th frame a draw with a kept set from an earlier frame repeated with it and with a twin written now, compared) | in-run (PRIO 1): street-still **RSX -0.51 ms (SE 0.13)**, fps +0.21 % (0.93), main -0.03 (0.17); spot 2 standing **fps +1.96 % (SE 0.62)**, RSX -0.46 (0.10), main -0.24 (0.09); walking crossover RSX -0.33 (0.19), fps +0.13 % (0.81); counters (4 + 4 quick street) **RSX cycles -1.52 % (SE 0.39)**, instructions -1.31 % (0.36), per draw -2.5 % / -2.3 %, other groups and power equal; quick street abseq (8 pairs) INCONCLUSIVE **+0.89 %** (SE 0.42; pairs 0.34), **RSX -0.41 ms (SE 0.05)**, main -0.03 (0.08), GPU -0.06 (0.05), p99 +0.2 (1.8), > 33 ms 6.1 -> 5.4, > 50 ms 1.1 -> 1.0 a run; plaza abseq (8 pairs, 7 on route) INCONCLUSIVE -0.21 % (SE 0.85), on-route -0.73 % (0.77) with +0.9 % draws, RSX -0.08 (0.10; per draw -1.3 %, 0.5), main +0.43 (0.18), p99 -0.14 (0.24), max 40.8 -> 36.7 ms, > 33 ms 1.0 / 1.0; by the stage marks R5 ~-0.17 ms (set work -0.14 / -0.36 us a texture- / program-changed draw, the set bind +0.07-0.09: RADV reads a cold kept set), the memo ~-0.15 ms (layout -0.65 us a program-changed draw); exact: R5 check 2,870 draws (street, plaza, final tree) repeated both ways, 23.7 GB compared, 0 bytes differ; memo verify 865,841 hits, 0 differ; RSX semaphore waits equal (street 17 / 14, plaza 35 / 21 in 8 runs, the same labels), no Vulkan error; PGO: vk_eng_batch_set, eng_guest_programs, vk_batch_completed (+ cold ones) lose their profile, sink_end_body and vk_eng_draw_ex_body keep theirs |
| (uncommitted in D; work/bench/cg1-store.patch, exe dirs work/port/cg1a = HEAD's lift / cg1b = both switches, no-PGO modules, s1c's engine (= HEAD's) in both; cg1, 2026-11-20) | the store page-map test in fewer instructions, two lift-time switches in engine/tools/ppu_fastpath.py (**default off**: HEAD's lift is unchanged; set them in the lift's environment): `PPU_ST_CMPB=1` the test and its re-check as one `cmpb $1, (map, page)` each (asm goto: the same byte read at the same point); `PPU_ST_RUNS=1` three or more stores off one guest register in straight-line code (prologue saves, structure fills) take one test of the run's two end pages, plain byte-swapped stores and one re-check (if a page went slow meanwhile, ppu_store_pm_recheck for every store of the run); the stretch is emitted twice, the careful copy (the original lines) for a run whose pages are not fast; `PPU_ST_RUN_MIN`, `PPU_ST_RUNS_VERIFY=1` = the check lift. 210,309 of 417,714 store sites grouped (35,039 runs), module .text +4.5 % (the cold copies) | counters (4 + 4 quick street, PRIO 1): **main instructions -6.2 % (SE 1.1), cycles -2.4 % (SE 0.85)**, CPU ms -1.4 %, IPC 0.99 -> 0.95, other groups and power equal; quick street abseq (PRIO 1, 8 pairs) INCONCLUSIVE **+0.96 %** (SE 0.42; pairs 0.28), **main -0.42 ms (SE 0.07)**, RSX +0.18 (0.06), p99 -0.25 (0.27), > 33 ms 6.1 / 6.0, > 50 ms 1.25 / 1.0 a run; plaza abseq (PRIO 1, 8 pairs) -0.22 % (SE 0.55), main -0.14 (0.14), RSX +0.29 (0.12), max 45 -> 37 ms, > 33 ms 1.6 -> 0.5; street PRIO 0 (8 pairs) -0.05 % (SE 0.42), main -0.14 (0.05), RSX +0.28 (0.07), p99 +1.1 (0.4); plaza counters (on-path runs, A 2 / B 4): main instructions -8.0 %, cycles -3.3 %; exact: an independent check of the lift text (all 35,039 run blocks: same statements, same stores, offsets = the original addresses) 0 differences, the verify lift (every grouped store checks its own address: 0 reports on the quick street and the plaza), the steered full street route on the path with the end shot at 6.3 % (A 7.2 %), 69 game runs without a crash, hang or error, draws equal; PGO: every lifted function with a store changes its CFG (a retrain with the lift; both sides here no PGO) |
| (parked, not in the tree: work/bench/r2-multidraw.patch, exe work/port/r2 = hd4's module + this engine; r2, 2026-11-20) | Vulkan entry-point wrappers, `PS3RECOMP_VK_MULTIDRAW` (**default: unset** = HEAD's code path, nothing installed; flips live with PS3_LIVEAB): 1 = a vkCmdBindDescriptorSets equal to the bound one (set and dynamic offsets) with only draws / buffer binds / dynamic state since is left out; 2 = 1 + neighbouring indexed draws with nothing recorded between them recorded as one vkCmdDrawMultiIndexedEXT (VK_EXT_multi_draw, enabled at device creation only with the switch); 3 = 2 + a replay check (captured runs drawn both ways into scratch targets, read back, compared); 9 = sizing only (wrong picture: offsets ignored, the "+constants by draw index" tier); `PS3RECOMP_VK_MULTIDRAW_STATS=1` = [multidraw] lines. No recording function changes (PGO: only rsx_vulkan_backend_init loses its profile) | mode 2 joins 24-32 % of draws (street 2,000-2,650 a frame, plaza 2,300-2,500), leaves out as many binds; Vulkan-call time in the backend's draws -0.13..-0.17 ms a frame; in-run street-still PRIO 1 (2 runs) RSX -0.07 ms (SE 0.14), frame -0.02 (0.15), fps +0.18 % (0.74); PRIO 0 RSX -0.44 (0.19), fps +1.31 % (1.04); walking crossover (unsteered, off-route) RSX -0.78 (0.63); quick street abseq (PRIO 1 both, 8 pairs) INCONCLUSIVE +0.43 % (SE 0.47), RSX -0.03 ms (SE 0.10), main -0.06 (0.10), p99 -0.75 (0.64), > 33 ms 6.1 -> 4.6 a run (SE 0.95), > 50 ms 1.0 / 1.0; plaza abseq (PRIO 1 both) over the 6 on-route pairs of 8 -0.44 % (SE 0.60), RSX +0.03 ms (SE 0.11), main +0.14 (0.16), p99 -0.37 (0.25), > 33 ms 0.50 -> 0.33, > 50 ms 0 / 0; counters (4 + 4 quick street runs) RSX cycles a frame -0.32 % (SE 0.65), instructions -1.14 % (SE 0.58): no group moves; exact: 420 runs / 2,884 draws replayed both ways, 8.1 GB of colour / depth / stencil, 0 bytes differ; RSX semaphore waits: quick street equal (in the window 8 vs 9 in 8 runs each), plaza more with the switch (in the window 10 vs 26 over 6 on-route pairs, 4.0 vs 12.8 ms in total, < 0.001 ms a frame; labels 0x870 / 0x890 as without it: read as the RSX reaching the light jobs' acquire a little earlier, a watch item). Sizing mode 9: 76 % of draws join, RSX -0.69 ms (SE 0.15) = the most the shader-side form could save. By rule 2 not worth turning on alone |
| (uncommitted in D; work/bench/s1-prio.patch, exe work/port/s1c = hd4's module + this engine; s1, 2026-11-20) | runner priority classes, `PS3_NATIVE_JOBS_PRIO` (**default 0** = upstream's single FIFO; flips live with PS3_LIVEAB): two ticket rings; with 1 a manager goes to the low class when its last flush came from a thread other than the main PPU thread (guest id 1) AND its jobs are long (mean of all its jobs >= `PS3_NATIVE_JOBS_PRIO_LONG_US`, 250 us) -- on the street and the plaza that is Edge (221, ~600 us a job) alone; FIFO inside a class, jobs never interrupted, the sticky pick yields to the high class, a low job older than `PS3_NATIVE_JOBS_PRIO_AGE_US` (8 ms) goes first anyway; bench modes 2 (static list `PS3_NATIVE_JOBS_PRIO_LOW`, default 221), 3 / 4 (awaited manager first), 5 (by kicker alone); stats in the `[jobs] prio` line; replaces sz1's PS3_JOBS_PRIO prototype; tests/jobrunner: + scenario `prio` (7 cases), 15 of 15 | mechanism (trace): first anim job 4.38 -> 1.52 ms after the flip, post-Edge anim waits 1.44 -> 0.59 ms a frame, main's runner waits -0.5..-1.0 ms but its time outside them +0.85..+0.97 ms (frame-end wait for the RSX +0.25); Edge end median 4.3 -> 9.1 ms (p99 15.4, none past the frame); RSX semaphore waits unchanged (1 a 479 frames). fps: in-run street-still (Edge low, 4 runs) +0.33 % (SE 0.62), main -0.07 ms (SE 0.12); plaza abseq (7 on-route pairs) **+2.06 % (SE 0.63)**, main -0.66 ms, RSX -0.23, p99 -0.5 ms; quick street abseq +1.25 % (SE 0.88, 8 pairs, main -0.40 ms; with s1b's flapping rule -0.32 %, SE 0.44); standing 10 min x 2 pairs (real clock, spot 2, 60 cap, real controller): on 59.68 / 59.65 fps, p99 18.0 / 17.9, > 33 ms 4 / 3, worst 41 / 44 ms vs off 59.72 / 59.59, p99 18.0 / 18.3, > 33 ms 0 / 3, worst 33 / 40, > 50 ms 0 in all four; full street route on / off 49.42 / 49.82 fps, both on route, one > 50 ms each (the world start); 39 runs exit 0; no `[jobs] WARN`, no new frames > 50 ms; no PGO retrain (pool_main, hook_token_test, ready_add_n lost their profile) |
| (parked, not in the tree: work/bench/j218-natives.patch, exe work/port/j218c2 = hd4's engine + the new module) | job 218: blend zero-weight shortcut (`PS3_NATIVE_ANIM_ZSKIP`), FK 0x179C0 first pass raw (`J218_FKRAW`), native 0x17280 (`PS3_NATIVE_ANIM_OFF=17280`; model from the Switch fork, MIT); natives.txt +0x17280 (SPU_NATIVE_MAX now full, 32) | 218 per job live -8.6 % (269.9 -> 246.6 us, counters batch), replay -7.2 % / -8.4 % (street / plaza best); fps: quick street INCONCLUSIVE at 8 pairs, -0.01 % (SE 0.61); plaza +0.38 % (SE 0.53, 8 clean pairs); in-run main +0.06 ms (SE 0.22): neutral by rule 2; exact (FULLPUT 800/800, valP2 table identical, check modes 0 of 7.08 M + 1.3 M live calls differ); no PGO retrain (coordinator: one combined retrain later) |
| (deck-v063, local) play.sh `PS3_PAD_THREAD_US=4000` | v0.6.3's controller thread every 4 ms instead of 1 ms on Linux (section 7) | ps3-pad 32 -> 13 ms of CPU a second; street guest 4-20 s 46.46 -> 47.12 fps (n = 4, 1.9 SE), SDL pads on |
| 2335d44 (deck-v063, local) | merge of upstream v0.6.3: Flood Town runner fix + stall repair, controller thread, Esc = START, tests; our SDL lock reworked into a lock of its own for sdl2-compat's joystick list, taken by the controller thread too (section 7) | replica 0 / 20 aborts (v0.6.3 as released 10 / 10, the old lock merged as is 4 / 20); A/B vs hd3: section 7 |
| (nothing kept; mt2, 2026-11-19) | main thread round 2: store slow path inline (`PPU_ST_LINE`, work/bench/m2-st-line.patch) and `PS3_CPU_PLACE_MAIN=cpu` (work/bench/main1-place-main.patch) measured and rejected (section 5); the tree is back at HEAD. Tools only (work/bench, not in git): m2-diag.patch (`PS3_STSTAT`), mt2-power.py / mt2-powsum.py / mt2-powwatch.sh (SMU power and clocks, perf stat per thread; section 3) | street M2 guest 4-20 s 44.96 / 44.92 / 45.09 fps (HEAD lift / inline / inline off), M7 46.96 -> 46.49 |
| (uncommitted; work/bench/m13-216.patch, exe work/port/m13) | job 216: native selection pass 0x4D80 (new natives/spu/job216_sel_native.c, `PS3_NATIVE_J216SEL`, default 1; 0 off, 2 check mode, `J216SEL_STATS=N`): O(n^3/6) -> O(n^2) dot products (each candidate keeps its running maximum), four candidates per exact SPU helper op; BSP clipper split(): vertex sides four at a time + the crossing slots from bit masks (`PS3_NATIVE_J220_VEC`, default 1; 0 = the scalar loop, 2 = check against it) | **neutral**: 216 per job live 661 -> 595 us street, 568 -> 510 plaza (replay 415 -> 317 us); street m13s + m13t (12 rotated rounds) full window 49.80 vs 50.19 fps (4 / 6 clean of 12, main 19.41 -> 19.22 ms), guest 4-20 s 46.80 vs 47.14 (n = 12, +0.7 %, 1.5 SE; main 19.53 -> 19.32), plaza m13p 51.53 vs 51.26 (4 / 4, main 18.71 / 18.72); p99 27.1 / 27.3 (street), 23.7 / 24.0 (plaza); exact (replay 400 street jobs FULLPUT 400/400; live check 660k 0x4D80 calls and 329 M vertex sides, 0 differ). By rule 2 not a commit on its own merit; PGO: bsp / split lost their profile (changed), new TU unprofiled |
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

* Texture- and program-changed draws, the cuts NOT built (2026-11-20, r8; NOTES.md "R8 texture/program draws"; sized
  with DELTA_STATS=2 counters, work/bench/r8-diag-delta.patch): taking the last draw's vertex / index work into a
  texture-changed draw -- its vertex / index registers are unchanged in only 1 % of them (a texture change comes with
  a new mesh); a texture-mask memo of the pipeline lookup keyed on the units' registers without their offsets --
  offset-only changes are 15-37 % of texture-changed draws, ~0.05 ms a frame; decoding only the changed texture units
  (the 16-unit decode is 0.8 % of the thread, ~0.09 ms to win) -- moves sink_end_body off its PGO profile. First form of
  R5 (kept descriptor sets given up whenever any view is destroyed): one view a frame dies on the street, so the sets
  never lived past a frame (new sets 498-565 a frame as before); fixed with per-image serials in the key (section 4).

* Variants of the store-test change (2026-11-20, cg1; NOTES.md "main-thread code generator (agent cg1)"): the page byte
  read with a volatile load -- clang folds neither an atomic nor a volatile load into the compare (movzbl + cmp + jne
  either way); a plain load folds but is a data race (the asm goto of `PPU_ST_CMPB` reads the same byte, same point).
  Store runs across conditional exits (`if (c) goto` inside a run, re-check at the exit): +1.4 % more grouped stores
  (static, 8 chunks): not built. Runs of two stores: the run test checks two pages, so no test is saved: not built.
  Keeping the per-store test out of the careful copy's inlining (clang does not inline into the cold copy): &_mm then
  lives in memory, four stores at every function entry -- the accessors are always_inline under `PPU_ST_RUNS`.

* Fewer Vulkan draws without a shader change (2026-11-20, r2; NOTES.md "R2 graphics thread"; kept as the default-off
  tool `PS3RECOMP_VK_MULTIDRAW`, section 4): neighbouring draws with identical pipeline, set, constant windows, buffers and
  dynamic state (24-32 % of draws on the street and the plaza) recorded as one vkCmdDrawMultiIndexedEXT, their
  repeated set binds left out. Exact (replay check: 8.1 GB compared, 0 bytes differ), but RADV charges only ~70 ns for
  such a draw: the Vulkan-call time falls 0.13-0.17 ms a frame and no in-run or between-run A/B shows it (quick street abseq +0.43 %, SE 0.47, RSX -0.03 ms; plaza -0.44 %, SE 0.60, RSX +0.03 ms).
  RADV's per-draw cost is the two NEW dynamic offsets of every other draw (descriptor / push-constant re-upload), which
  only the shader-side "constants by draw index" form removes -- sized with a wrong-picture mode (9): 76 % of draws join,
  RSX -0.69 ms (SE 0.15) at most, before the cost of its per-draw constant table. Not worth the shader-generator change
  on its own (days of work for <= 0.7 of 19 ms); the RSX thread's time is the per-draw interpreter (section 1).

* Runner priority variants (2026-11-20, s1; NOTES.md "job priority (agent s1)"; the kept form is section 4's
  `PS3_NATIVE_JOBS_PRIO`): **"by kicker" alone** (every manager another PPU thread flushes goes last: Edge 221 AND the SPU
  light jobs 219) -- the RSX thread waits for the light jobs' label (0x870): in-run vs Edge-only-low RSX +0.53 ms (SE
  0.14), fps -1.9 % (SE 0.68); the trace shows 4 RSX semaphore waits (4.8 ms) vs 1 (0.4-0.7 ms). **The manager the main
  thread is waiting on goes first** (sizing option b, dynamic): +0.04 % (SE 0.74) alone, -0.59 % (SE 1.39) on top of the
  classes -- the main thread starts waiting on anim at ~4 ms, when Edge's first wave ends anyway, and runs the awaited
  jobs itself (help). The "any PPU waiter" form cannot separate Edge from anim at all (the Edge kicker polls Edge's token
  test from its kick to its end). **Turning the sticky pick off** with the priority: 0.0 % (SE 1.06), main +0.06 ms -- the
  prototype needed STICKY=0 only because its check sat behind the sticky pick. **A running mean (1/8 per job) for "long
  jobs"**: Edge's mean hovered around the 250 us limit and its class flapped (use the mean over the manager's life).
  Not tried: reserving one pool thread for the high class (Edge would end later still: p90 13.9 ms of a 21 ms frame).

* Job 218 (2026-11-19, j218; NOTES.md "job 218 round"): **FK (0x179C0) two groups per pass on AVX2** -- only 1.5 % (street)
  / 1.7 % (plaza) of group pairs are independent (a parent of group g+1 is a child of group g in all the others: the bone
  list is a hierarchy walked in order), so the 8-lane pass would almost never run; the 4-lane pass went raw instead
  (kept). **The Switch fork's "raw" form** (no per-op checks at all, under a precondition on the inputs: every float
  < 2^56, |t| <= 8) not taken: upstream's gathered magnitudes cost one op per op and need no per-kernel proof of the
  bounds. **A native of 0x4F60** (3x4 -> 4x4 world matrices, 0.8 % of a live 218 job) written but not built: the engine's
  SPU_NATIVE_MAX (32) is full with 0x17280 (work/bench/j218-4f60-kernel.c.txt). **The clip sampler (0x18EC0) raw** not
  done: ~1 % of 218 by estimate (its FP ops are a minority of its work; mostly scalar bit-stream code).
* v0.6.3 merge (2026-11-19, mg3; section 7): **8833293's lock merged as it was** (the poll lock; v0.6.3's controller thread
  does not take it) is unsafe: the replica aborts in 4 of 20 runs (v0.6.3 as released: 10 of 10). Replaced by a lock of its
  own for the joystick list that every controller pass takes. **The controller thread under the poll lock** (the other
  obvious fix) was not built: a pass that blocks in a driver (8 ms SDL updates of the Deck controller, a 0.5-0.8 s Bluetooth
  open) would make the title's pad calls wait again, which is the freeze upstream's thread removes. **Upstream's 1 ms
  controller period on Linux**: 32 ms of CPU a second on the main thread's and the pool's CPUs; 4 ms in tools/play.sh
  (13 ms/s, +1.4 % at 1.9 SE). **Placing ps3-pad** (e.g. on the RSX core's idle sibling): not done, 13 ms/s is not worth
  it. **`PS3_NATIVE_JOBS_NODETECT=1 HEAL=0`**: no gain (46.78 vs 46.74 fps), and they are the Flood Town fix: kept on.

* Store slow path, inline line-bit test (M2, 2026-11-19, agent mt2; work/bench/m2-st-line.patch, lift switch `PPU_ST_LINE`,
  run-time `PS3_ST_LINE`; exe dirs work/port/mt2a / mt2b, no-PGO modules on both sides): counted first (`PS3_STSTAT`,
  work/bench/m2-diag.patch): the main thread made **1.92 M out-of-line stores a second** on the street (~38,000 a frame),
  96 % of them to a line no reservation ever marked, on ~55 pages that hold a marked line (one data page, 0x00A6E000, took
  half: its one marked line is a live job-queue line, reserved ~26,000 times a second by SPUs and PPU), 4 % (~1,650 a
  frame) to a marked line, and only 8 a second hit a line another agent held at that moment. The inline test (page byte 3
  = fast page with a reserved line: test the line's bit, store inline with ppu_lean_st's own re-check) removed all the
  unmarked-line calls (1.92 M -> 72 k/s, checked with the counter) and **bought nothing**: street 6 rotated rounds, guest
  4-20 s (n = 6 each) a0 44.96 / b 44.92 / boff 45.09 fps, main 20.37 / 20.48 / 20.48 ms; full window b 46.42 / boff (same binary, switch off) 47.82 / HEAD-lift a0 47.10 fps (n = 3 / 2 / 3), main 20.82 / 20.22 /
  20.44 ms; same off-route path a0 55.73, b 54.58 and 55.81, boff 54.69; plaza a0 49.99, b 50.03, boff 50.88 (n = 3 / 2 /
  1), main 19.42 / 19.41 / 19.03. The calls cost less than the noise (< ~8 ns each): main1's "~1 ms" for the slow path
  was sample skid (the samples sit on the call after a cache-missing load). And the module grows 17 % (111 -> 130 MB, the
  cold branch at ~405,000 store sites) and loses its PGO profile. Unmarking unheld lines (the other M2 idea) cannot help
  either: the hot pages' marked lines are reserved thousands of times a second (a re-mark = a process barrier).
* `PS3_CPU_PLACE_MAIN=cpu` (M7, 2026-11-19, agent mt2; work/bench/main1-place-main.patch, engine build-mt2e-eng + hd3's
  module = exe work/port/mt2m, the same binary on both sides): street, 8 rotated rounds: full window 48.94 fps (n = 5, sd
  0.74) vs 50.01 (n = 7, sd 0.77) without it, guest 4-20 s 46.49 (0.62) vs 46.96 (0.48) (n = 8, -1.0 %, 1.7 SE); main's own
  ms/frame 18.66 vs 19.26 (full window) but more of the frame spent waiting: the rest of the process gets 5 CPUs instead of
  6. With `=core` (main1, section 5 above) it was -3.6 %. Placement of the main thread is not a lever.
* CPU governor `performance` (EPP performance) via steamosctl (2026-11-19, mt2): street at the 15 W cap, the cores ran
  2,930 MHz vs 2,940-2,960 with the default EPP balance_performance; fps within noise. Not a lever (the power limit sets
  the clock). **Do not toggle it in benches:** on this kernel the switch back to powersave keeps EPP = performance (the
  driver's cached value), and the EPP file is root's -- only `sudo tee .../energy_performance_preference` or a reboot
  restores balance_performance (mt2 left it at performance, NOTES.md).
* Native job 213 (2026-11-19, m13; not built): a live 213 job is 37-42 us but its lifted code only ~10 us (replay of 400
  recorded street jobs, REPEAT=40); the rest is ~85 small synchronous GET-and-wait DMAs a job (pointer chasing, cache
  misses on the copies) and the runner's staging, which a native of its game logic (16k SPU instructions in 769 lifted
  fragments, a flat call tree, no leaf kernel) would still do. Faster 213 = a cheaper DMA command (section 6, M8).
* Faster 216 alone for the main thread (2026-11-19, m13): exact and -10..-16 % per 216 job live, but main / fps unchanged
  within noise (section 2): the render burst is capacity-bound. Kept uncommitted behind switches (section 4).
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
* `PS3RECOMP_RSX_SPLIT=1` (walker + exec threads, exec on the RSX core's SMT sibling): -19% fps. Again on hd5c (sz2,
  2026-11-20, quick street abseq): -2.53 % (SE 0.69, REJECT; walker 11.5 ms + exec ~17 ms = ~28 ms of work for the
  single thread's 18).
* Threaded Vulkan recording / VK_BT, `PS3_CPU_PLACE=rsxcpu`: slower on the Deck. **Re-measured on hd5c (sz2, 2026-11-20):
  the plain recording worker (`PS3RECOMP_VK_THREADED=1 PS3_CPU_PLACE_VKW=1`) now WINS, +2.12 % (SE 0.69, ACCEPT) -- see
  section 6 "Step 8 result"; `+ PS3RECOMP_VK_BT=1` still loses: -13.6 % (SE 0.69, REJECT).**
* ThinLTO over the module's lifted PPU units (`PORT_LIFT_LTO=thin` (parked: work/bench/sz2-thinlto.patch, not in the tree), sz2, 2026-11-20; patch
  work/bench/sz2-thinlto.patch): main cycles +0.3 %, quick street abseq +0.10 % (SE 0.60, 8 pairs): nothing. Not retried
  without a new reason (e.g. a lifter that makes small lifted functions).
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

**Ranked by measured expected gain (2026-11-20, agent sz1; street, hd4's module, 46.7 fps = 21.4 ms; section 2 "The
street frame, sized"; NOTES.md "sizing (agent sz1)").** 60 fps needs the frame -4.7 ms: the main thread's chain (own code
15.6 ms + 2.8 ms of waits + 3.1 ms of jobs it runs) AND the RSX thread (18.3 ms, ~1 ms under the main thread now) both
below 16.7 ms; the GPU (13.2 ms at 1200 MHz, ~15 at 1000) fits.

| # | Candidate | What limits it | Measured sensitivity (ms of frame per ms) | Realistic speed-up | Expected fps on the street | Confidence |
|---|---|---|---|---|---|---|
| S1 | Runner priority -- **built (s1, 2026-11-20): `PS3_NATIVE_JOBS_PRIO=1`, default 0** (section 4; NOTES.md "job priority (agent s1)"): other threads' long-job managers (Edge) go behind the main thread's | Edge's ~15 jobs queued at 0.2 ms held all 4 pool threads to 4.3 ms; with the priority the first anim job starts at 1.5 ms instead of 4.4 and the post-Edge waits fall 1.44 -> 0.59 ms a frame, BUT the main thread's time outside its waits grows about as much (its frame-end wait for the RSX thread +0.25 ms; no extra cache misses, no run-queue delay) | measured: in-run street-still +0.33 % (SE 0.62, 4 runs); quick street abseq +1.25 % (SE 0.88, 8 pairs; main -0.40 ms, SE 0.20); plaza abseq +2.06 % (SE 0.63, 7 on-route pairs; main -0.66 ms, RSX -0.23, p99 -0.5); sz1's +2.4 % on the street not reproduced | the remaining frame gain is capped by the RSX thread finishing the frame last | **street ~+1 % (not proven), plaza +2 %** | medium: plaza 3.3 SE, street 1.4 SE; standing and full routes clean; pays more once R2 shortens the RSX thread; the default is step 6's decision |
| S2 | GPU clock 1000 instead of 1100-1200 MHz (a setting: the player's) | the 15 W cap: GFX watts are taken from the CPU's clock | 1200 -> 1000: frame -0.65 ms (SE 0.22), GFX -0.35 W -> ~1.9 ms per W | GPU busy 13.2 -> 15 ms (still < 16.7) | **+3.2 %** (SE 1.1) vs 1200; ~+1.5 % vs 1100 (interpolated, not measured) | medium (one in-run test; check the plaza / GPU-heavy views before advising). Note (orchestrator): the 19 Nov clock sweep with light tiles (section 4, "GPU clock, light tiles") measured 1000 and 1100 MHz as equal on the street (50.39 / 50.51 on route, 46.71 / 47.16 guest 4-20 s, small n), so the player's 1100 already takes most of this; 1000 leaves less GPU headroom for busy views. No change advised without a 1000 vs 1100 A/B on street + plaza. |
| S3 | Main-thread code generator -- **step 5 done (cg1, 2026-11-20)**: breakdown in section 2 ("The main thread's code by instruction kind"); built `PPU_ST_CMPB` + `PPU_ST_RUNS` (section 4, default off) | the lifter's overhead is spread: store tests 11 %, spills 10 %, register-file traffic 10 %, host frames 5 %, address arithmetic ~14 % of the lifted code; a removed instruction was worth ~0.4 cycles (they are the cheap ones) | measured: main -0.42 ms (SE 0.07) for -6.2 % instructions / -2.4 % cycles; the frame got ~half of it on the street (+0.96 %, SE 0.42), nothing on the plaza (RSX-bound) or with PRIO 0 (the saving becomes job waiting) | the rest of the store test (single stores): ~2.3 % of the thread's instructions, ~0.2 ms, only with a protocol change; the calling-convention items (spills + ctx + frames ~25 % of the lifted code) need a new ABI between lifted functions: ~0.5-1 ms at the measured exchange rate | street +1 % (measured, 2.3 SE), plaza 0 | measured; the next main-thread ms are in the ABI (large) -- and the RSX thread is now right behind (+0.2-0.3 ms) |
| S4 | Edge (223) on the GPU | as S1, plus 16 ms of pool CPU a frame (power) | Edge slow-downs 0.23 (0.16) / 0.13 (0.045) per ms of Edge CPU (follows Edge's end; MWAITX hides the power part) | after S1: the remaining 0.38 ms of post-Edge wait + ~0.5-1 W of CPU (minus the GPU's new work) | +1..+4 % beyond S1 | low (GPU-side cost unknown; very large effort) |
| M8 | SPU runtime per MFC command (55,000 a frame) | every job's DMA path, incl. the awaited 213s (85 DMAs a job) | 0.18 (SE 0.08) (spin delay 3.0 ms of CPU; large-signal 0.15) | -15..-30 ns a command = -0.8..-1.6 ms of CPU | +0.7..+1.4 % (speed-up likely weaker) | low-medium |
| G2-G4 | GPU passes (shadow copies, box/tile, depth decodes: 0.7-1.2 ms of GPU) | not the limit: only through power at a fixed clock | ~1.9 ms of frame per W of GFX (S2) | ~0.07-0.1 W | +0.5..+1 %, or room for a lower clock | low |
| R2 | RSX per-draw cost -- **profiled and built (r2, 2026-11-20)**: breakdown in section 1 ("RSX thread on the street"); `PS3RECOMP_VK_MULTIDRAW` (default off; section 4): neighbouring draws with identical state and constants as one vkCmdDrawMultiIndexedEXT + repeated set binds left out | the per-draw interpreter, spread over vertex / index caches 3.9 ms, FIFO walk 3.7, RADV 3.4, backend 1.7, libc 1.7, state tracking ~2.6; the Vulkan calls themselves 2.3-2.5 ms | with PRIO 1, street-still: +1.0 ms of RSX -> frame +0.14 (SE 0.30); +1.7 ms -> +0.86 (SE 0.24): ~1 ms of slack | built form ~0.15 ms of RSX (Vulkan-call time; not measurable in-run); the full "constants by draw index" form at most ~0.7 ms (sizing mode 9: RSX -0.69, SE 0.15) | 0 measured (section 4) | high: the Vulkan side is not where the RSX thread's time is |
| -- | More single-job natives (216 / 218 / 204 style) | the CPU of jobs nobody waits for | 216 -10 %, 218 -8 %: 0 measured | -- | 0 | high (measured twice) |

**Step 3 result (s1, 2026-11-20):** S1 is built as a default-off switch (section 4). Its mechanism works (the post-Edge wait is gone), but on the street the main thread then waits for the RSX thread at the frame end, so the frame gains ~+1 % (not significant) on the street and +2 % on the plaza. That moves R2 up: after S1 the RSX thread (18.4 ms, drained last) is what ends the street frame.

**Step 4 result (r2, 2026-11-20):** the RSX thread's ~19 ms on the street (with the priority on) is the per-draw
interpreter, spread: vertex / index caches 3.9 ms, FIFO walk + dispatch 3.7, RADV 3.4, our backend 1.7, libc 1.7, state
tracking / textures / programs ~2.6 (section 1). Fewer Vulkan draws is a small lever: the built form (identical
neighbours as one multi-draw, `PS3RECOMP_VK_MULTIDRAW=2`, exact) saves ~0.15 ms and measures as nothing (quick street
abseq +0.43 %, SE 0.47; RSX -0.03 ms, SE 0.10; plaza -0.44 %, SE 0.60, 6 on-route pairs); even the shader-side "constants by draw index" form is worth at
most ~0.7 ms (sizing mode 9). With PRIO 1 the standing street still leaves the RSX thread ~1 ms of slack (+1 ms of RSX
work: +0.14 ms of frame, SE 0.30). The RSX-side lever with a size is R8 (the texture-changed and program-changed draw paths:
8.2 ms for a fifth of the draws), then R5 (~0.5 ms), R3 / R4 (~0.5 together) and the step-6 PGO retrain; all are
engine-side (sink_end_body's profile), so they belong before step 6's retrain. Order unchanged: step 5 = the main-thread
code generator (S3), which the frame waits on first.

**Step 5 result (cg1, 2026-11-20):** the main thread's lifted code (61.5 % of its instructions) has no dominant
lifter overhead: store page-map tests 11 %, host spills 10 %, register-file traffic 10 %, host frames 5 %, 32-bit address
arithmetic ~14 % (section 2). The store test was the one cheap and exact target: `PPU_ST_CMPB` + `PPU_ST_RUNS` (section 4)
remove 6.2 % of the main thread's instructions (SE 1.1) but only 2.4 % of its cycles (the removed instructions ran in
parallel with the rest: ~0.4 cycles each), main -0.42 ms on the quick street with the priority on, and the frame gets
about half (+0.96 %, 2.3 SE) because the RSX thread is right behind (+0.2-0.3 ms busier in every A/B, the same work in
the counters: clock / pacing); plaza (RSX-bound) and PRIO 0 (the saving becomes job waiting) show nothing. What is left
of this kind: the single stores' tests (~2.3 % of the thread's instructions, needs a protocol without the per-store
re-check: ~0.2 ms), the load addresses (a lea / zero-extension per load: ~2 % of the lifted code, needs 64-bit addressing
with guard pages for the wraps), and the calling convention (every guest register through ctx at each call, the six
host pushes / pops, the spills of ~25 guest registers in 15 host ones: ~25 % of the lifted code, ~15 % of the thread's
instructions; at the measured exchange rate ~0.5-1 ms, a new ABI between lifted functions = weeks). So S3's -1.6..-3.1 ms
is out of reach with lifter patches; the S3 change is worth keeping for the PGO retrain of step 6 (with its lift), and
the next frame milliseconds on the street are again on the RSX thread (R8) and in the job latency the main thread waits for.

**Step 5b result (r8, 2026-11-20):** the texture- and program-changed draws (section 1, "Texture- and program-changed
draws") cost what they cost mostly because they are NEW work: a texture change comes with a new mesh (vertex /
index registers unchanged in 1 %: vertex work 1.1-1.35 us), a texture's first bind in a frame pays its index probe and
hash check, a program change pays a new pipeline bind (~0.8 us in RADV) and layout. What repeated from frame to frame
was the descriptor sets (~520-600 made and written again a frame) and the fragment programs' whole analysis on every
re-upload with other constants (~190 a frame): both are cut exactly by `PS3RECOMP_ENG_R8=3` (section 4: R5 + the
structural memo). Measured: the RSX thread -0.41 ms on the quick street (abseq, SE 0.05) / -0.5 ms in-run (street-still,
spot 2), counters -1.5 % cycles (-2.5 % per draw); the frame: spot 2 standing +1.96 % (SE 0.62), quick street +0.89 %
(SE 0.42, 2.1 SE), street-still and the plaza route within noise. The RSX thread now runs ~17.9-18.0 ms a frame on the
quick street against the main thread's ~19.2: on the walking street the main thread is the first wall again (the RSX
~1.3 ms behind), on the standing plaza view the RSX still paces the frame. The next RSX levers: R3 / R4 (index-cache
successors, dl_scan in AVX2: ~0.5 ms together), a per-(index entry, vertex signature) table in front of the vertex-cache
probe for draws whose index lists serve several vertex buffers (pair-memo signature misses ~1,150 a frame: <= ~0.2 ms),
the FIFO walk (3.7 ms); all in sink_end_body, i.e. before step 6's PGO retrain.

**Step 6 result (hd5, 2026-11-20; NOTES.md "combined check and hd5 (agent hd5)"): the combined check.** The three kept
pieces in one build, each measured inside the combination (all on minus that piece), no PGO module on either side (one
engine binary, HEAD's with mg3r's engine profile; modules: HEAD's lift = cg1a's byte for byte, and the store lift = cg1b's),
6 rotated rounds of 5 configurations per scene (work/bench/hd5rot.sh, REV=1; pairs by round: hd5pair.py), EPP performance,
HUD on in every run:

| Piece | All on minus it: quick street (steered, guest 4-20 s) | Plaza (G 16-100 s; 5-6 on-route pairs) | Slow frames | Decision |
|---|---|---|---|---|
| `PS3_NATIVE_JOBS_PRIO=1` (runner priority) | -0.76 % (SE 0.48), main +0.21 ms | -0.56 % (SE 1.01), main +0.18 | > 33 ms: street 34 (on) vs 37 (without), plaza 7 vs 2 (1.4 SE: the watch item, all 41-46 ms frames); none > 50 ms | **on** |
| store lift `PPU_ST_CMPB=1 PPU_ST_RUNS=1` | **-2.39 % (SE 0.73)**, main +0.56 ms, RSX +0.25 | -0.22 % (SE 0.84) | equal | **on** |
| `PS3RECOMP_ENG_R8=3` | **-1.69 % (SE 0.45)**, RSX +0.60 ms | -1.42 % (SE 0.92), RSX +0.45, p99 +1.06 ms | equal | **on** |
| all three vs none | **+2.6 % (SE 0.51)**: 46.27 vs 45.09 fps, main -0.56 ms, RSX -0.66 | **+2.1 % (SE 1.07)**: 50.81 vs 49.74, main -0.41, RSX -0.63, p99 -0.67 | standing 10 min each (real controller, 60 cap): 59.59 / 59.61 fps, p99 18.0 / 17.9, > 33 ms 1 / 1, worst 35 / 39 ms, > 50 ms 0 / 0 | |

The pieces add up on the street (alone today: +0.96 / +1.25 / +0.89 %; together +2.6 %): with R8's RSX saving the store
lift's main-thread saving reaches the frame (main -0.56 ms in the combination vs -0.42 alone, and the RSX no longer gets
busier). Defaults (uncommitted in D): tools/play.sh sets `PS3_NATIVE_JOBS_PRIO=1` and `PS3RECOMP_ENG_R8=3` (Linux launcher;
each can be set from outside, =0 = the old path); tools/rebuild_win.py lifts with `PPU_ST_CMPB=1 PPU_ST_RUNS=1` on Linux
builds only (`sys.platform` linux; Windows builds keep the old lift; `PPU_ST_CMPB=0` / `PPU_ST_RUNS=0` in the environment =
the old lift).

PGO retrain **mg5r** (E/work/pgo, new files): instrumented build pgogen5 made with NO lift variable in the environment (the
new Linux default produced the store lift: 35,039 runs = cg1b's), mg3's six training runs with `PS3_NATIVE_JOBS_PRIO=1
PS3RECOMP_ENG_R8=3` in every run (pgotrain-mg5.sh; raws E/work/pgo/train-mg5), mg5 = the training, mg5r = training x5 +
deck.profdata x2 (as mg3r). Use build **hd5c** (work/port/hd5c): stale-profile report module 0 ("every compiled function
matched its profile"), engine 1 = `main` (the gen build's dump code, as always); vk_eng_batch_set, pool_main,
eng_guest_programs, hook_token_test are warm again.

**The full gate, hd5c vs hd4 (work/port/mg3r = installed), one batch, label order rotated** (hd5c with PRIO 1 / R8 3 =
the launcher's new defaults; hd4's engine has neither and predates the steering, so its street runs are unsteered: 6 of
13 on the route, hd5c 13 of 13):

| Test | hd4 | hd5c | hd5c vs hd4 (paired by round) |
|---|---|---|---|
| Street, full window (guest 10-85 s, clean runs) | 49.12 fps (n 6, sd 1.53), main 19.67, RSX 17.31, p99 28.8 | 50.44 (n 13, sd 1.02), main 19.15, RSX 16.84, p99 28.5 | **+2.63 % (SE 1.11, 6 pairs)**; unpaired +2.7 %; main -0.50 ms, RSX -0.55; > 33 ms 5.7 / 5.2 a run, > 50 1.17 / 1.15 |
| Street, guest 4-20 s (all 26 runs) | 46.61 (sd 0.58), main 19.45, RSX 18.47 | 47.37 (sd 0.51), main 19.05, RSX 18.04 | **+1.65 % (SE 0.46, 13 pairs)**; p99 -1.38 ms (0.92); > 33 equal; > 50 1.08 / 1.15 (the frame-stepped clock's guest-16.4 s frame in every run) |
| Plaza (G 16-100 s) | 51.29 (n 5, sd 0.47), main 18.63, RSX 18.36 | 52.12 (n 4, sd 0.75), main 18.26, RSX 18.05 | **+1.70 % (SE 0.47, 4 pairs)**; p99 -0.30; > 33 ms 2 vs 3 in all; > 50 0 / 0 |
| Standing, 2 x 10 min each (real clock, spot 2, 60 cap, real controller; order hd5c hd4 hd4 hd5c) | 59.73 / 59.77 fps, p99 17.9 / 17.7, > 33 ms 1 / 1, worst 35 / 34 | 59.77 / 59.82, p99 17.9 / 17.7, > 33 ms 3 / 0, worst 38 / 33 | equal; > 50 ms 0 in all four; RSS growth equal (+4-6 MB after 5 min) |
| RSX semaphore waits (whole runs) | street 1.6 a run (1.2 ms), plaza 2.8 (1.2 ms) | street 3.5 (2.8 ms), plaza 1.0 (0.4 ms) | the runner priority's (Phase A: with PRIO 2.2-4.8 a run, without 0.5-1.2); < 0.001 ms a frame; watch item |
| Job replays (valP2 set + m13's 213 / 216 FULLPUT) | | | pass / fail table and native lines identical to hd4's; 218 timing replay (M9) best 103.3-103.9 vs 103.5-103.8 us: equal |
| tests/jobrunner | | | 15 of 15 (upstream's 8 + 7 prio cases) |

Every run of the round exited by itself (62 Phase A, 6 training, 40 gate), no `[jobs] WARN`. **Verdict: hd5c is the hd5
candidate** (not slower anywhere, faster on the street and the plaza by 3.6 SE each, no new stutter); it needs tools/play.sh
with the new defaults (without them it runs PRIO 0 / R8 0). Not installed: the player decides.

**Step 8 result: the three big options sized (sz2, 2026-11-20; NOTES.md "sizing the big options (agent sz2)").** Base hd5c
with the launcher's defaults (PRIO 1, R8 3), the street, HUD on, EPP performance; bench-only modes, default off, uncommitted
(work/bench/sz2-hooks.patch): `PS3_DBG_EDGE_SKIP` (Edge's skinning + decompression replaced by a memo of each segment's
outputs: the same geometry, frozen poses), `PS3_DBG_VK_SKIP` (the RSX thread records no Vulkan draw), `PS3_DBG_SMT_SPIN`
(a busy thread on CPU 7, the RSX core's sibling); all flip live with PS3_LIVEAB.

| Option | Measured upper bound (frame ms / fps) | Realistic gain | Effort | Risk | Confidence |
|---|---|---|---|---|---|
| 2a. RSX: the EXISTING recording worker, `PS3RECOMP_VK_THREADED=1 PS3_CPU_PLACE_VKW=1` (vkCmd* calls replayed on CPU 7) | street abseq **ACCEPT +2.12 % (SE 0.69, 3 pairs)**, RSX 18.14 -> 16.32 ms, main equal, the worker ~4.2 ms on CPU 7; plaza abseq +3.41 % (SE 1.14, 8 pairs; +2.35 % without one light-content pair), RSX -1.63 ms, p99 -0.96 ms | **+2 %** street, +2..+3 % plaza (the RSX's -1.3 ms target met: -1.6..-1.8 ms) | **hours** (a launcher default + the full gate + a PGO retrain) | low: existing code, the same commands in the same order; watch the ring's sync points, stalls | high (3.1 SE; lost on 11-17 with the old RSX thread, wins now) |
| 1. Edge (job 223, 13.8 ms of pool CPU a frame) off the CPU | measured for the skinning + decompression math (the memo removes ~2.1 ms of Edge's job time, ~4.1 ms of the pool group): **+3.43 % (SE 0.85, 24 pairs) still, +2.97 % (SE 1.28) walk; main -0.75 ms at the same clock** (contention on the shared cores, not power); all of Edge, extrapolated: main -1.5..-3 ms, frame **+5..+12 %** | GPU port **+4..+10 %** (its GPU cost ~0.2-0.4 ms of 13 + ~0.1 W, ~95 k vertices / 50 k triangles a frame); a CPU native for Edge's hottest lifted kernel 0x9C20 (~2.4 ms a frame) **+2..+4 %** | GPU: **weeks** (hand-over of the kernels, decompression / skinning / packing as compute, the RSX thread binding GPU-made buffers for the Edge ring, a check mode); CPU native: **days** | GPU: high (SPU float rules: truncation, FTZ, the estimate tables -- exactness needs float_controls RTZ or the user's OK); native: low (replay-exact, the known way) | sign high (4.1 SE + the slow-down mirror); size low (x6 extrapolation) |
| 2b. RSX: a new two-thread design (walker / recorder) | the Vulkan side = **5.4 ms** of the RSX thread (VK skip, by cycles at the same clock); SMT: **0.22 ms of RSX per ms on CPU 7** (30 % / 100 % spinner); the old designs: `PS3_RSX_SPLIT=1` **-2.5 %** (REJECT), `+PS3RECOMP_VK_BT=1` **-13.6 %** (REJECT) | beyond 2a: RSX -1..-2 ms more, **~0 %** on the street until the main thread is faster (it is the wall at ~19 ms) | weeks | high (every backend call that returns something is a sync point; labels / 0x870 / write tracking) | medium |
| 3. Cheaper calls (new calling convention) | ceiling 8.9 M instructions a frame (25 % of the lifted code); compiler-level: no PLT (hidden + -fno-plt, direct calls), no frame pointer, **ThinLTO built and measured: main cycles +0.3 %, abseq +0.10 % (SE 0.60, 8 pairs) = nothing** | a realistic convention: -3.3..-3.5 M instructions (6 % of main), main -0.45..-0.7 ms, **+1..+2.5 %** | weeks | high (every lifted function, the engine's entry points, PGO) | medium (cg1's cycles-per-instruction rate) |

**Order for steps 9-11 (sz2, by measured gain per effort):** step 9 = 2a, the recording worker on CPU 7 -- a switch that
exists, +2 % on the street and the RSX target met, needs only the gate (street, plaza, standing, replays) and a PGO retrain
with it on. Step 10 = Edge off the CPU, in two stages: first a CPU native for its hottest lifted kernel (0x9C20, ~2.4 ms a
frame) and a placement check (Edge's jobs kept off the main thread's SMT sibling) -- days, exact, they test the mechanism
the memo found (the main thread runs 0.75 ms faster without Edge's math beside it); then Edge on the GPU if 60 fps still
needs the main thread's -2.5 ms. Step 11 = the calling convention only after those (weeks for +1..+2.5 %); a new RSX split
design is not worth building now (2a takes its cheap part; the rest pays only once the main thread is faster), and the
compiler-level call fixes are done (nothing to gain).

**Order for roadmap steps 3-5 (sz1):** step 3 = S1 done properly (small: the ready ring gets a priority class for the
main thread's managers; gate it) and put S2 to the player (free). Step 4 = the main-thread code generator (S3), sized
first by a prototype (one lifter change, perf stat IPC / instructions on the main thread + an in-run A/B), because it is
the biggest ceiling (15.6 of 21.4 ms) and Edge's measurable effect (the post-Edge wait) is mostly taken by S1 at a
fraction of the cost. Step 5 = R2 (the RSX thread is the next wall: 18.3 ms) together with M8 (short job latency makes the
main thread's code savings count). Edge on the GPU (S4) moves behind S3: after S1 its remaining value is power, which
S2 shows can be bought more cheaply. The earlier plan's job-side track (M1 / M3: shorter 213 / 216 / 218 jobs for burst
capacity) is wrong: CPU removed from jobs nobody waits for buys nothing.

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
| M1 | Shorter anim burst (213 + 218 + 200, ~22.5 ms of CPU): 2026-11-19 (m13) measured -- 213's lifted code is only ~1/4 of a live 213 job (the rest = DMA latency, see M8); 218 is the lever: ~18 ms/frame of pool CPU. **2026-11-19 (j218): 218 -8.6 % per job live (zero-weight shortcut, FK raw, native 0x17280; section 2 / 4) and the fps did NOT move beyond noise** (quick street -0.01 %, SE 0.61; the freed pool time becomes ready_pop spin). Left in a live 218 job: lifted rest ~27 % (flat), runtime ~22 %, clip sampler ~16 %, blend ~11 %, FK ~9 % | measured ~0 for -8.6 % (the 0.19 ms-per-ms rule of thumb predicted ~+1.3 %) | large | medium | the remaining 218 natives are overhead-bound (per-op checks, spills, estimate emulation); 0x4F60 native written, blocked by SPU_NATIVE_MAX |
| M2 | TRIED 2026-11-19 (mt2), rejected (section 5): the inline line-bit test removes 96 % of the main thread's out-of-line stores (1.92 M/s -> 72 k/s) and buys nothing measurable (b vs the same binary with `PS3_ST_LINE=0`: street main 20.82 / 20.22 ms, plaza 19.41 / 19.03); the module grows 17 %. Unmarking unheld lines: the hot pages' lines are live (thousands of reservations a second). Left: the ~1,650 marked-line stores a frame (lock + fence + two scans; 8 a second find a holder) | <= 0.2 (unproven: main1's slow-path estimate was sample skid) | medium (a per-line "held" summary the reservers keep, Dekker with the writer's fence) | medium-high (reservation protocol) | measure first: a causal switch that adds a busy wait per marked-line store, or skip notify_spus on lines no SPU ever reserved |
| M3 | Render-manager drain (204 x 520 at 18 us, 216 x 6 at 600-700 us behind barriers): 2026-11-19 (m13) DONE for 216 (0x4D80 native + vector BSP sides, -10..-16 % per 216 job live, exact) -- main / fps unchanged within noise (capacity-bound burst, section 2); left in 216: split's polygon loop, lifted 0x7C40 / 0x9DF8 / 0x6CB8 / 0x55A0 (~26 % of the job) | 216 rest: ~0.1; 204 -20 %: ~0.4 | medium-large | low (exact by replay) | SPU_NATIVE_MAX (32, spu_channels.c) is full since job216_sel_native.c: the next native needs it raised (engine) |
| M8 | SPU runtime per DMA command: the runner's staging copies (spu_wrch, 3.1 % of all samples: the out-of-line path, by its copy loop mostly wws_native.c WR()'s staging DMAs -- inferred, no call graph) and the jobs' commands (spu_wrch_cmd_fast 4.1 %), ~1/3 of both = the copy's cache misses: prefetch the next queued job's input buffers (the runner has its command list) while the current job runs; a leaner per-command path (overlay / coherency checks, bcmp) | 0.2-0.4 (1-2 ms of pool CPU in both bursts; helps 213, 204, 218, 220 at once) | medium (engine; hot functions lose their PGO profile -> retrain) | medium (DMA ordering is unchanged; exact by construction, replays) | the biggest single block of pool time (~12 ms/frame with indirect branches and wws_native_run) |
| M4 | Codegen: ctx traffic around calls (~6 % of hot guest samples) | 0.3-0.5 | large (lifter) | medium | host-stack spills (12 %) are register pressure; v20-v31 kept = no gain on SysV (section 5) |
| M5 | Codegen: VMX special-value tests | <= 0.3 | large | high (NJ / NaN exactness) | part of the 8.5 % vector/fp alu |
| M6 | DONE 2026-11-19 (pgo2, with R1): PGO retrain mg2r, build hd2 | measured 0 on the street (main 20.56 vs hd1 20.43 ms/frame, guest 4-20 s 20.51 vs 20.50), plaza -0.4 (18.98 vs 19.38) | -- | -- | the 9 changed engine functions (help_one, usleep, ppu_timebase_now, pump_messages, the crash fix's pad lock ...) have profiles again; stale after the use build: module 0, engine 1 (`main`, cold) |
| M7 | TRIED 2026-11-19 (mt2), rejected (section 5): `PS3_CPU_PLACE_MAIN=cpu` -2.1 % full window (48.94 vs 50.01 fps, n = 5 / 7), -1.0 % guest 4-20 s (n = 8) | -- | -- | -- | main's own time -0.2..-0.6 ms, but the rest loses a CPU |
| P1 | Power: the other threads' spins burn the budget the main thread's clock comes from (one clock for all four cores at the 15 W cap, section 2). First test 2026-11-19 (mt2, work/bench/mt2-poolspin.patch, `PS3_NATIVE_JOBS_POOL_SPIN`, street, 2 rounds): the pool's idle spin 3,000 -> 300 / 0 PAUSEs gives NO measurable clock (on the route 2,941 / 2,955 / 2,962 MHz, CPU 9.10 / 9.05 / 9.17 W) and guest 4-20 s 48.02 / 46.70 / 46.62 fps (n = 2 each, ~1-1.5 SE: the main thread runs more jobs itself when the pool sleeps). So a PAUSE spin costs little power here; MONITORX / MWAITX (user mode on Zen 2) would save less still | <= 0.2 (unproven; the spin's watts are small) | small-medium | low-medium | the bigger power items are the GPU's draw (clock, light tiles) and the total CPU work of the bursts |
| M2b | The ~1,650 marked-line stores a frame (stripe lock + seq_cst fence + scans of the PPU slots and SPU contexts; 8 a second find a holder): skip notify_spus on lines no SPU ever reserved (a second bitmap set before the barrier), or a per-line holder summary | <= 0.2 (unproven) | medium | medium-high (reservation protocol) | causal test first (a busy wait per marked-line store); M2's lesson: the slow-path samples were skid |
| M9 | (j218) Engine housekeeping before more SPU natives: SPU_NATIVE_MAX (32, spu_channels.c) is full with 0x17280 -- raise it (then 0x4F60, work/bench/j218-4f60-kernel.c.txt, ~0.8 % of a 218 job); and check why HEAD's engine (ft1's bench tools in, all off) replays 218 ~3 % slower than hd4's (best 107.0 vs 103.7 us; spu_jobrec_dma / spu_indirect_branch samples) before the next engine install | 0-0.1 | small | low | the replay difference may be replay-only (spu_jobrec_dma); ft1's live A/B of the tools: -0.20 % (SE 0.76) |
| -- | done / rejected: M2 inline line test, M7 PLACE_MAIN=cpu (section 5, mt2), help-peek (neutral, section 4), 216 natives (neutral, section 4), native 213 (section 5), HELP=2, THREADS=5, PLACE_MAIN=core, cross-manager priority, v20-v31 locals (section 5) | | | | |

Order after mt2 (2026-11-19): M2 and M7 measured and rejected (section 5). The main thread's work is guest code (14 ms, cache-miss bound, IPC ~1.0) and the jobs it runs or waits for; what is left with a known size: **M1 via 218** (the main thread runs ~6 of them itself: one to one, plus 0.2 per burst-ms; every ms of pool CPU saved also frees power for the one all-core clock), then M8 / M3; P1 (the spins) measured small. A PGO retrain is not needed for anything of this round (nothing kept).

Order after m13 (2026-11-19): **M2 first** (it is time ON the main thread: counts one to one, ~1 ms/frame at stake), then
M8 (runtime per DMA command, helps every job of both bursts at once), then M1 via 218; more single-job natives on the
render or anim burst buy ~0.2 ms of main thread per ms of CPU they remove (section 2).

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
| R2 | Constants by draw index + multi-draw (no per-draw set bind; consecutive draws with one pipeline and set as one vkCmdDrawMultiIndexedEXT) | **measured 2026-11-20 (r2)**: the plain tier (same constants too, 26-32 % of draws) ~0.02 us a draw (0.15 ms, built: `PS3RECOMP_VK_MULTIDRAW=2`, exact, not measurable); the full form (76 % of draws join) at most ~0.09 us a draw (RSX -0.69 ms, sizing mode 9, wrong picture) before the cost of its per-draw constant table | large (VS / FS read constants at a draw-indexed base; Vulkan-only HLSL path; shader cache key) | RADV's cost of a draw whose state did not change is ~70 ns; what it charges per draw is the two new dynamic offsets (descriptor / push-constant re-upload), which only the shader change removes -- and that is <= 0.7 ms of 19 |
| R3 | Index-cache successor prediction with two successors per entry (or a per-frame sequence) | 0.02-0.04 us | small (noinline helper, PGO-safe; 56 B of padding free in eng_ic_entry) | probe misses 2.0% + 0.6%: only 57.5% of links are right |
| R4 | dl_scan with AVX2 compares (work/bench/rsx1-dlscan-simd.patch, `PS3RECOMP_ENG_DELTA_SIMD`) | ~0.04 us | small, but changes sink_end_body's PGO hash: needs R1 | rsx_draw_delta.inc:293-298 ~2% of the thread |
| R5 | **BUILT 2026-11-20 (r8): `PS3RECOMP_ENG_R8` bit 1, default off** (section 4): descriptor sets kept across frames, a table per frame-in-flight slot, key + image serials | measured with bit 2: street RSX -0.41 ms (abseq, SE 0.05); new sets ~520-600 -> 40-69 a frame (street), 67 (plaza) | -- | by the stage marks ~-0.17 ms of the pair's ~-0.4..-0.5: the set work -0.14 / -0.36 us a texture- / program-changed draw, but the bind of a kept set +0.07-0.09 us (RADV reads the set's own memory, cold for an old set); next: keep the recently bound sets warm or make fewer distinct sets |
| R6 | Fetch vertices straight into the arena (no staging memcpy) | ~0.03 us | medium | memcpy of staged vertices / indices 1.4% |
| R7 | Push constants for the per-draw offsets (item 4) | < 0.06 us | large | bounded by NOBIND (section 5) |
| R8 | **BUILT 2026-11-20 (r8): `PS3RECOMP_ENG_R8=3`, default off** (section 4; profile in section 1 "Texture- and program-changed draws"): R5 (bit 1) + the fragment program's structural memo (bit 2: 96 % of the ~190 whole analyses a frame skipped) | measured: street RSX -0.41 ms (abseq, SE 0.05), -0.51 (in-run still, SE 0.13); spot 2 standing fps +1.96 % (SE 0.62), RSX -0.46; quick street abseq +0.89 % (SE 0.42) | -- | of r2's ~3.4 ms estimate ~0.45 ms was exact and buildable: the rest of the texture-changed draws' cost is a new mesh's vertex work (1.1-1.35 us) and the first bind of each texture in the frame, and the program-changed draws' a new pipeline bind and new layout |

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

## 7. Upstream v0.6.3 merge (2026-11-19, agent mg3; branch `deck-v063`, NOTES.md "v0.6.3 merge (agent mg3)")

v0.6.3 (1f407ee, with 6e7addd) on top of deck-perf 4a30160: the "Powering Up Flood Town" freeze fix in the native job
runner, cellPad's controller thread, Esc = START, the Windows "Optimise for this PC", the Linux setup on stock SteamOS,
upstream tests. The rule of the round: keep every upstream fix and every Deck optimisation, and measure whatever upstream
adds to a hot path on the same binary with its switch (so PGO does not enter the per-feature numbers). EPP was
`performance` (left by mt2) in every run of the round.

**Conflicts and resolutions (merge commit 2335d44).**

| File | Ours | Theirs | Resolution |
|---|---|---|---|
| engine/libs/spurs/wws_runner.c | help peek 365fd60 (`PS3_NATIVE_JOBS_HELP_PEEK`) | Flood Town fix: the claim path also checks the active array / table version / finished-prefix word (a manager re-created at the same address), `PS3_NATIVE_JOBS_HEAL` stall repair, `PS3_NATIVE_JOBS_NODETECT` | both. Upstream's tests/jobrunner passes on the merged runner (8 of 8, the repair included); on hd3's runner it shows the hang (`--expect-broken`: 3 HANG + 1 FAIL as expected) |
| engine/libs/input/cellPad.c | 8833293: the window pump drains SDL's queue under cellPad's poll lock (try-lock) | the controller thread (SDL work every 1 ms on its own thread, `SDL_HINT_AUTO_UPDATE_JOYSTICKS=0`), the slow-call log | both, and our lock reworked: a lock of its own for sdl2-compat's joystick list, taken by every controller pass too (below) |
| engine/libs/video/vk_overlay.cpp (pads_refresh) | SDL_NumJoysticks under the try-lock | the menu's controller list from cellPad's snapshot | theirs: no SDL call left there |
| rsx_vulkan_backend.c, tools/play.sh | batched pump drain; `PS3_NATIVE_JOBS_THREADS=4`, static TLS | Esc = START; the container note | merged cleanly |

Everything else upstream changed is comments (the lifted code, the natives and spu_patches.txt are unchanged: the
module's sources are the same), Windows-only tools / libc table, the input map's layout 2 (an old input.toml is moved
to Esc = START once and written back at the first start: the player's file will change at install) and new tests.

**The SDL race with the controller thread.** v0.6.3 moves every SDL controller call (SDL_GameControllerUpdate = SDL's
device detection, the hot-plug scan with SDL_NumJoysticks and the opens, rumble, the state reads) onto a thread of its
own, every 1 ms, so a slow driver no longer stalls the game. That is one more thread in sdl2-compat's unlocked joystick
list (section 2, the startup crash), and 8833293's lock was cellPad's poll lock, which the new thread never takes.
Replica outside the game (work/bench/mg3-sdlrace.c: the window pump, cellPadInit / End 200 times a run, the controller
thread; the system's sdl2-compat 2.32.70 over SDL3 3.4.12, Steam's virtual pad):

| Variant | Runs that abort |
|---|---|
| v0.6.3 as released (no lock anywhere) | 10 of 10 |
| 8833293 merged as is (init and pump locked, the controller thread not) | 4 of 20 |
| the merge's fix (every controller pass under the same lock) | **0 of 20** (4,000 init cycles, ~81,000 passes) |
| the merge's fix with a pad plugged in / out every 10 passes (virtual pad, ~1,000 hot-plugs) | **0 of 10**; under ASan 0 memory errors in 5 |
| 8833293 as is, with hot-plug / under ASan | 1 of 5 abort / 2 of 3 "attempting double-free" (the thread's SDL_NumJoysticks vs the pump) |

The fix (in the merge commit): a lock of its own for the joystick list (cellPad.c `s_sdl_list_lock`, behind
`cellPad_host_sdl_trylock`) around every controller pass, the thread's device list and closes, cellPadInit's SDL work,
and the `PS3_PAD_THREAD=0` in-call pass and its rumble; the RSX pump keeps its try-lock. Not the poll lock, so a pad
call of the title never waits for a pass that blocks in a driver (upstream's test_pad_thread.c, slow fake SDL: slowest
pad call 0.13 ms with the lock). Even with joystick events off, the pump still takes some out of the queue (145 in the 20
fixed runs, 2,372 in the 10 hot-plug runs), and sdl2-compat converts each with SDL_NumJoysticks, so the lock matters in
play, not only at start. In
steady state it skips 2 % of the pump's passes (those events wait 4 ms more).

**What each upstream addition costs (same binary mg3r, switches; street, guest 4-20 s, n = 4 per label, batch mg3f).**

| Switch (all with SDL pads on, `PS3_PAD_SDL=1`) | fps (se) | main ms | ps3-pad CPU | Verdict |
|---|---|---|---|---|
| controller thread, upstream's 1 ms (`PS3_PAD_THREAD_US=1000`) | 46.46 (0.26) | 19.41 | 32 ms/s (755 passes of 42 us) | costs 3.2 % of a CPU on the CPUs main and the pool share |
| no thread, SDL inside the pad call (`PS3_PAD_THREAD=0`, = v0.6.2) | 46.58 (0.37) | 19.35 | -- | = the 1 ms thread within noise |
| thread every 4 ms (`PS3_PAD_THREAD_US=4000`) | **47.12** (0.22) | 19.14 | 13 ms/s (234 passes of 55 us) | +1.4 % vs 1 ms (1.9 SE); **now the Linux launcher's default** |
| (hd3 with SDL pads on, its in-call reads) | 45.82 (0.41) | 19.80 | -- | reference |

The bench itself runs without SDL pads (linux_run.py `PS3_PAD_SDL=0`), so the thread is absent from every other table;
these runs had the Deck's own controller open through SDL's HIDAPI driver. Power and clock did not move between the
variants (14.93 W, 2,940-2,970 MHz). `PS3_PAD_THREAD_US=4000` (tools/play.sh, Linux only; Windows keeps upstream's 1 ms)
makes the snapshot a pad call reads at most ~4 ms old, the bound set for this round, against v0.6.2's at most 1 ms. The
thread stays unplaced (it inherits CPUs 0-5; the rsxcore sweeper keeps it off the RSX core): at 13 ms of CPU a second
there is nothing to place.

**The runner's new checks (same binary).** `PS3_NATIVE_JOBS_NODETECT=1 PS3_NATIVE_JOBS_HEAL=0` (= the v0.6.2 claim path, no
repair) vs the defaults, street batch mg3s, 8 rounds: guest 4-20 s 46.78 vs 46.74 fps (se 0.17 / 0.12), full window
49.78 (n = 3) vs 49.67 (n = 4), main 19.34 vs 19.31 ms: nothing measurable (a few guest loads per claim). `PS3_NATIVE_JOBS_HEAL`
alone runs the same instructions until a wait has made no progress for 1 s; no run of the round logged a `[jobs] WARN` or
a repair. Kept as upstream wrote it (it is the Flood Town fix).

**PGO retrain (mg3r).** As NOTES.md describes for mg2r: instrumented build of 2335d44, the same six training runs (spot 2
spin, spot 1, video A / B, plaza, street; this time the street run left the route at the junction), training x5 +
deck.profdata x2; profiles E/work/pgo/mg3{,r}-{module,engine}.profdata. Use build: module 0 stale functions, engine 1
(`main`). The runner's changed functions (wait_until, hook_token_test, claim) are warm again; with mg2r they compiled cold.

**Final A/B vs hd3 (exe work/port/gpu2l2, installed; mg3r = work/port/mg3r), rotated rounds, `PS3_TB_FRAMESTEP_IDLE_US=2000`,
GPU 1200 MHz, EPP performance, SDL pads off (replayed input).**

| | hd3 | mg3r | difference (SE) |
|---|---|---|---|
| Street (mg3s, 8 rounds), full window, clean runs: fps (n) | 49.47 (5) | 49.67 (4) | +0.20 (0.55) |
| ... main / RSX / SPU ms, p99, frames > 33 ms per run | 19.44 / 16.80 / 73.4, 28.2, 3.4 | 19.34 / 17.07 / 73.2, 28.9, 4.5 | |
| Street, guest 4-20 s, all 8 runs: fps | 46.78 | 46.74 | -0.04 (0.20) |
| ... main / RSX ms, p99, frames > 33 ms per run | 19.38 / 18.14, 34.5, 5.5 | 19.31 / 18.27, 33.5, 5.0 | |
| Plaza (mg3p, 6 rounds), full window, clean runs: fps (n) | 51.45 (6) | 51.57 (5) | +0.12 (0.53) |
| ... main / RSX / SPU ms, p99, frames > 33 ms per run | 18.58 / 18.09 / 70.7, 24.4, 1.0 | 18.52 / 18.12 / 69.7, 24.4, 0.8 | |
| Plaza, guest 4-20 s, all 6 runs: fps; main / RSX ms; p99, frames > 33 ms per run | 47.55 (se 0.11); 19.87 / 19.29; 30.4, 2.5 | 47.80 (se 0.27); 19.63 / 19.20; 30.3, 2.7 | +0.25 (0.29) |
| Power, all-core clock (street, anchor+12..52 s) | 14.93 W, 2,949 MHz | 14.93 W, 2,943 MHz | |
| With SDL pads on (mg3f, street, guest 4-20 s, n = 4): hd3 in-call vs mg3r with the 4 ms thread | 45.82 (se 0.41), main 19.80 | 47.12 (se 0.22), main 19.14 | +1.30 (0.47) |

Off the route at the tram junction: hd3 3, mg3r 4 of 8 (street); plaza 0 / 1 of 6 (all paths replayed as usual).

**Standing at spot 2 (stand2.sh, real clock, the player's settings with the 60 fps cap, GPU 1100 MHz, EPP performance,
SDL pads ON for both = real play: hd3 reads the Deck's controller inside the pad call, mg3r on the controller thread at
the launcher's 4 ms; both builds back to back, two rounds, order rotated, 10 min each; the earlier 59.72 fps of hd3 was
EPP balance_performance and SDL off, not comparable).**

| Run (order) | fps | p99 mean | frames > 33 / > 50 ms | worst | socket W, all-core MHz | RSS growth after 5 min |
|---|---|---|---|---|---|---|
| mg3r round 1 (first) | 59.65 | 18.4 | 2 / 1 | 58 ms (one frame, t = 371 s; nothing in the log there) | 14.95, 3,000 | +5.7 MB |
| hd3 round 1 | 59.45 | 19.6 | 3 / 0 | 40 ms | 14.95, 2,990 | +6.0 MB |
| hd3 round 2 (first) | 59.52 | 19.5 | 3 / 0 | 41 ms | 14.97, 3,000 | +4.1 MB |
| mg3r round 2 | 59.69 | 18.3 | 3 / 0 | 38 ms | 14.94, 3,010 | +4.3 MB |
| **20 min each** | **mg3r 59.67, hd3 59.49** | **18.35 vs 19.55** | **5 / 1 vs 6 / 0** | | | |

Even capped at 60 fps the socket sits at the 15 W limit. mg3r holds 60 a little better and its p99 is 1.2 ms lower in
both rounds. A likely reason, seen with `PS3_PAD_SLOW_LOG=1` in mg3r's round 2: about 1.2 times a second, SDL's update of
the Deck controller (HIDAPI) takes 7.6-8.2 ms (730 passes over 10 min, all in "SDL update + device detection"). hd3 makes
that call inside the game's pad call, so the polling thread waits for it; mg3r makes it on the controller thread, and no
pad call waits (upstream's "controller freezes"). mg3r's single 58 ms frame (round 1) did not come back in round 2.
Every run exited cleanly.

**Exactness.** Job replays on mg3r (valP2.sh): the pass / fail table and every native line are the same as hd2's (204
1960/1960 FULLPUT, 210 / 218, 211 / 220, 223, mgrec; 208's known nx_208 replay limit unchanged), and m13's street jobs
recorded by the runner pass too (213 400/400, 216 400/400, FULLPUT). Upstream changed no native and no job code. The
runner's claim logic is covered by tests/jobrunner (above).

**Verdict.** mg3r (work/port/mg3r: deck-v063 2335d44 + the mg3r profiles; with tools/play.sh of 64574f3) is as fast as
hd3 everywhere within noise, with its SE stated: street guest 4-20 s -0.04 fps (SE 0.20), full window +0.20 (SE 0.55);
plaza full window +0.12 (SE 0.53), guest 4-20 s +0.25 (SE 0.29). With SDL pads on (real play) it is faster: street guest
4-20 s +1.30 fps (SE 0.47), standing p99 1.2 ms lower. There is no new stutter in the det benches (p99 / slow frames
equal) and no crash or hang in any of the round's 64 game runs (44 of the merged build: smoke, training, F, A, P, standing), all exit 0 by themselves. The one 58 ms frame in 20 min of mg3r standing (hd3: worst 41 ms)
did not repeat in the second round. Upstream's additions cost nothing measurable except the controller thread's 1 ms
period, which the Linux launcher now sets to 4 ms. It brings the Flood Town fix (hd3 hangs in upstream's re-creation
test) and the SDL race fix for the controller thread. **hd4 candidate: work/port/mg3r** (engine + libInfamous2Game.so,
with tools/play.sh of 64574f3). At install: v0.6.3 migrates the player's input.toml to Esc = START once and writes it
back, and the in-game rumble now runs on the controller thread.
