# Overnight report, 2026-11-18 to 2026-11-19

The player was away for about 8 hours from ~22:30 on 2026-11-18. Their instructions:
- do all the work and test it;
- install the next build without asking if it improves fps, keeping backups;
- keep going through the projects;
- report here.

Full details are in `docs/STEAM_DECK_PERF.md`; raw data is in `work/bench/NOTES.md` (local only).

## Starting point (before the night)

| Build | Plaza det bench | Street det bench | Notes |
|---|---|---|---|
| mgPr (installed) | 48.1 fps | 45.9 fps | merged v0.6.2 + Deck work, PGO mg1r |
| hd1 (dev, not installed) | 50.1 fps | 46.3 fps | + 04ddd13 RSX short tail at 16:10, + 8833293 startup-crash fix |

Street, hd1:
- **Main thread:** 20.4 ms per frame (the limit). A 60 fps frame allows 16.7 ms.
- **RSX thread:** 17.9 ms.
- **Jobs:** 77.7 ms of CPU per frame.
- **GPU:** 17.5 ms at 1200 MHz, over the 16.7 ms budget with the player's settings.

## Plan for the night

1. Main-thread round: breakdown and fixes, targeting 20.4 → 16 ms on the street.
2. PGO retrain on the plaza and the street, with the RSX short tail on and the main-thread changes in.
3. If the result beats the installed build, install it (backups kept), then smoke-test it.
4. Next projects, in order: GPU cost on the street, RSX batching (R2), SPU jobs (223 / 218 / 204).

## Log

(entries are added as results come in)

### 01:40: main-thread round finished (no fps gain; it shows where the time goes)

**Where the main thread's 20.4 ms per frame goes on the street:**

| Item | ms per frame |
|---|---|
| The game's own code (recompiled PPU) | 14.0 |
| Jobs the main thread runs itself | 2.7 |
| Waiting for jobs | 2.5 |
| Guest store slow path | ~1.0 |
| Other | ~0.5 |
| Idle | 1.2 |

- **The game's own code:** 69% of the thread. It is spread across many functions, the top one being only 1.6%. Most of its time is spent waiting for memory (cache misses), not on any one instruction pattern the recompiler could avoid.
- **Waiting for jobs:** about a quarter of the frame. The 4 job threads are fully busy twice per frame:
  - the animation burst: jobs 213, 218 and 200, ~22.5 ms of CPU;
  - the render-list burst: jobs 204, 216, 210, 208 and 201, ~20 ms of CPU.

  The main thread waits at the end of each burst. More job threads did not help, because a 5th thread takes power from the others at 15 W. Neither did letting the main thread run any queued job: it then ran long jobs while the short one it waited for had already finished. **The way forward is faster jobs on those paths, mainly 213, 218, 204 and 216.**

**Kept:**
- 365fd60: the waiting threads check the job queue before taking its lock. That removes ~30,000 lock calls per frame. The output is identical and fps is unchanged (street 46.68 vs 46.66).
- 626ab72: a benchmark-only clock fix, off by default. It removes the fixed ~120 ms frames that the test mode caused on the street.

**Rejected:**

| Variant | Street fps | Note |
|---|---|---|
| Main thread may run any queued job | 45.58 vs 46.66 | |
| 5 job threads | 46.11 | |
| Main thread alone on a core | −3.6% | |

**Next:** retrain the PGO profiles on the plaza and the street with everything above, build the install candidate "hd2", A/B it against the installed build and hd1, then a 30-minute standing run and the job replays. If it passes, I install it.

### 05:30: new build "hd2" installed

hd2 contains:
- the graphics-thread fast path at 16:10 (04ddd13);
- the startup-crash fix (8833293);
- the job-queue lock change (365fd60);
- an optimization profile (PGO) retrained on the plaza and the street with all of the above.

**Benchmarks** (frame-stepped tests, your settings, GPU 1200 MHz):

| | Old installed (mgPr) | hd2 | Change |
|---|---|---|---|
| Plaza | 48.2 fps | **50.7 fps** | +5.2% |
| Street (to the cathedral) | 45.8 fps | **46.1 fps** | +0.5% (within noise) |
| Street, first 16 s (the stretch every run shares) | 39.2 fps | **40.1 fps** | +2.3% |
| Standing at spot 2, real clock, 60 fps cap, 30 min | 57.1 fps (15-min control) | **58.9 fps** | +3% |

**Standing run:** no frame over 50 ms; worst frame 44 ms. Frames over 33 ms: 0.36 per minute vs 0.54 for the old build. No crash or hang.

**Correctness:** the job replays (exact-output checks of the background jobs) give the same results as the old build.

**Why the street barely moves:** the main game thread limits it (20.5 ms per frame), and hd2 speeds up the graphics thread, not the main thread.

**Smoke test of the installed files:** the street route, run from your install folder against a separate test copy of the game data: clean start and exit, no errors.

**To go back to the previous build:**
- copy both files from `work/port/installed-mgPr-20261119/` into `work/port/build/`;
- the launcher backup is `tools/play.sh.installed-mgPr-20261119`; the launcher itself only gained a comment line.

### 05:45: GPU round started

On the street the GPU takes 17.5 ms per frame with your settings (at 1200 MHz), which is over the 16.7 ms a 60 fps frame allows. This round:
- measures where that GPU time goes;
- measures what each of your graphics settings costs (texture filtering, soft shadows, SSAO and the others), with screenshots, so you can choose;
- re-checks the best GPU clock on the street;
- looks for GPU savings that leave the picture identical.

If you want to play in the meantime: the agent stops starting test runs while the game lock is held. When you're back, tell me in the session and I'll hold it for you.

### 08:50: GPU round finished (GPU −4 ms per frame, picture identical)

**Where the GPU time goes on the street** (hd2, your settings, 1200 MHz): 17.3 ms per frame.

| Step | ms per frame |
|---|---|
| Lighting (adds up all 83–129 lights for every pixel) | 5.05 |
| Geometry into the hidden buffers | 2.93 |
| Sun shadow maps (2048², 3 cascades) and their copies | 2.15 |
| Transparent objects and composite | 1.77 |
| Post-processing buffers (depth of field, edge blur, glow) | 1.35 |
| SSAO "Full, high" | 1.22 |
| FXAA + sharpen + present | 0.69 |
| Depth decodes | 0.56 |
| Small passes (water reflection, glow chain, ...) | ~1.4 |

**Kept: light list per screen tile** (commit 130be5d; switch `PS3RECOMP_VK_LIGHT_CULL`, on by default).
- **How:** a small pass works out, for each 16×16-pixel tile, which lights can reach it. The lighting step then adds only those lights, in the same order and with the same math.
- **Exactness:** a check mode also renders the original every 60th frame and compares the two. 0 bytes differed over 113 million pixels (street and plaza).

| | hd2 | Light tiles |
|---|---|---|
| Street, first 16 s (4 runs each): GPU | 17.4 ms | **13.2 ms** |
| … fps | 45.3 | **46.7 (+3.2%)** |
| … main thread | 20.3 ms | 19.5 ms |
| Plaza (3 clean rounds): GPU | 17.6 ms | **13.5 ms** |
| … fps | 50.2 | **51.8 (+3.2%)** |
| Plaza, a stretch where the GPU limited: fps | 56.7 | **60.8 (+7.4%)** |

Every light-tile run was faster than every hd2 run. The main thread got faster too: the GPU now uses less of the shared 15 W, which leaves more for the CPU.

**Rejected: skipping duplicate GPU waits** (`PS3_RSX_BARBATCH`): 0 ms on the Deck (17.49 vs 17.32). The first −2.4 ms reading compared two different walks; I had passed that number on before it was checked.

**What each graphics setting costs** (street, GPU ms saved per frame, one change at a time):

| Setting change | GPU ms saved |
|---|---|
| Shadow quality 2048 → 1024 | **1.1** |
| SSAO → PS3 | 0.5 |
| Effects resolution Full → As game | 0.5 |
| Texture filtering 16× → Off | 0.4 |
| SSAO "Full, high" → "Full" | 0.2 |
| Soft shadows → off | 0.2 |
| Texture filtering 16× → 8× | 0 |

None of them changes fps on the street, because the main thread is the limit there. Screenshots for each are in `work/bench/gpu2/shots/`. The resolution is already 720p, the lowest.

**GPU clock:** best is **manual 1100 MHz** (1000–1200 are about equal; 1100 keeps headroom for busier views). At 1400 the GPU takes power from the CPU and the street loses 5–7% fps.

| Clock (hd2, street) | fps | GPU ms | main thread ms |
|---|---|---|---|
| 1000 | 47.96 | 20.27 | 19.48 |
| 1100 | 49.47 | 18.57 | 19.91 |
| 1200 | 48.64 | 17.27 | 20.14 |
| 1400 | 45.15 | 15.39 | 21.19 |
| 1600 | 44.03 | 14.09 | 22.17 |

With light tiles: 1000 → 50.4, 1100 → 50.5, 1200 → 49.7, 1400 → 47.3 fps.

**Next GPU candidates (picture-identical):**
- reading the shadow map directly instead of copying it first: 0.6–0.7 ms;
- cheaper tile passes: 0.1–0.2 ms;
- fewer repeated depth decodes: up to 0.2 ms.

### 09:00: install candidate "hd3" = hd2 + light tiles

The 30-minute standing run at spot 2 (same test as hd2's 58.9 fps) is running. Since you are back, I'll ask before installing.
