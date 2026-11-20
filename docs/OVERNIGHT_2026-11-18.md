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

You approved (09:10) installing hd3 once the standing test passes, and setting the GPU clock to manual 1100 MHz after it.

### 09:30: hd3 installed, GPU clock set to 1100 MHz

**30-minute standing test at spot 2** (real clock, 60 fps cap, your settings, 1400 MHz like hd2's test):

| | hd2 | hd3 |
|---|---|---|
| fps over 30 min | 58.86 | **59.31** |
| fps, minutes 5–30 | 58.81 | **59.36** |
| Frames over 50 ms, minutes 5–30 | 0 | 0 |
| Worst frame, minutes 5–30 | 44 ms | 42 ms |

- **First 5 minutes:** 12 frames over 50 ms (worst 89 ms). They came at the exact seconds I was committing and publishing and the jobs agent was starting, so the cause was activity on the Deck, not the game.
- **Check run** (10 min, the installed files, 1100 MHz, nothing else running): **59.72 fps, 0 frames over 50 ms, worst 41 ms**, 5 frames over 33 ms. Clean start and exit, same log lines as before.
- **Lesson:** during a measurement run I now do nothing else either, including commits and publishing.

**Installed:** `work/port/build/Infamous2Recomp` = hd3 (deck-perf 130be5d, sha 4f660ac1…). The game module is unchanged, the same file as hd2's (sha 510aee90…).

**GPU clock:** manual 1100 MHz (`steamosctl set-manual-gpu-clock 1100`). If Steam's per-game performance profile is on for the game, set 1100 there too.

**To go back to hd2:**
- copy both files from `work/port/installed-hd2-20261119/` into `work/port/build/`;
- the launcher only gained a comment line; its backup is `tools/play.sh.installed-hd2-20261119`.

### 09:40: jobs round running

Faster jobs 213 / 218 / 204 / 216 (main thread, M1 / M3); estimated −0.8 to −2.3 ms on the main thread.

### 11:20: jobs round finished (job 216 faster, main thread not; parked)

- **Job 216:** two exact changes made it 10–16% shorter (exact by job replay and a live check over 660,000 calls). But the main thread didn't get faster beyond noise:
  - street 46.80 → 47.14 fps over the first 16 s (+0.7%, not significant);
  - plaza 51.53 → 51.26 fps.
- **Why:** both job bursts per frame are limited by total CPU across ~4.5 threads. A shorter job lets its successors start sooner, but the burst still ends when all its work is done. Rule of thumb: **1 ms less CPU in a burst = only ~0.2 ms less main thread.** Work done on the main thread itself counts one for one.
- **Kept as a patch only** (`work/bench/m13-216.patch`, local), not committed: no fps gain, more code to carry.
- **Job 213 native: rejected.** Its own code is only about a quarter of a 213 job. The rest is waiting for many small memory transfers, which a native would still have to do.
- **What this changes in the plan:** single-job speedups are worth much less than estimated. The levers now are work on the main thread itself, and less CPU and GPU work overall, which leaves more of the shared 15 W for the main thread (the light tiles made the main thread 0.7 ms faster this way).

### 11:40: upstream v0.6.3 is out (merge planned after the current round)

v0.6.3 (1f407ee, 19 Nov) plus 6e7addd: 55 files, about 1,840 lines added since v0.6.2. Our branch has 66 commits on top of v0.6.2.

**What matters for us:**
- **Freeze fix in "Powering Up Flood Town"** (#87, #88), in the job runner (`wws_runner.c`), which we use. A job manager re-created at the same address kept the old state, and the game waited forever. Upstream also adds a stall repair (`PS3_NATIVE_JOBS_HEAL`, on by default). Our job-queue change (365fd60) touches the same file.
- **Controller thread** (`cellPad.c`, "controller freezes"): SDL controller polling moves to its own thread (`PS3_PAD_THREAD`), so a slow controller driver no longer stalls the game's main thread. This is the same code our startup-crash fix (8833293) protects. The merge must check again that the SDL race stays fixed, with the replica test, because there is now a third thread calling SDL.
- **Linux setup on stock SteamOS** (6e7addd, `tools/libc_headers.sh`): relevant to the final PR and to the Linux PGO script.
- **"Optimise for this PC" fixes** (#90, Windows setup), Esc pauses, smaller fixes.

**Expected conflicts:** `cellPad.c`, `rsx_vulkan_backend.c` (the window pump), `vk_overlay.cpp`, `wws_runner.c`, `tools/play.sh`.

**Plan:**
1. After main thread round 2: merge v0.6.3 into deck-perf.
2. Re-verify the SDL race fix with the replica.
3. Retrain PGO (new code), A/B against hd3, then a standing run and the job replays.
4. Install as hd4, with your OK.

This should be done before you reach Flood Town.

**Final goal (your instruction):** a branch with all our improvements on top of the latest upstream, ready for the maintainer to merge:
- our changes as clean commits on upstream main, split into topic PRs, each with its measurements;
- bench-only and local tooling left out;
- Windows paths left untouched. We can't build Windows here, so Linux-only code stays behind `#if` and the maintainer's Windows build is the check.

### 11:25: main thread round 2 started

1. The guest store slow path (~1 ms on the main thread itself).
2. A real A/B of pinning the main thread to a CPU of its own.
3. Measuring the clock the main thread really runs at vs the job threads, to see how much power is the limit.

### Roadmap addition (09:15, your decision)

**At the very end, when the implementation is final:** a one-step PGO training for Linux and Deck players, like upstream's Windows "Optimise for this PC" (`tools/setup_build.ps1 -Pgo`, `tools/pgo_train.ps1`):
1. build the instrumented game;
2. let it play scripted routes by itself (plaza, street, standing, the videos);
3. merge the counts;
4. rebuild with the profile.

Every player trains their own: upstream never publishes profiles. Our PGO gain was about 15% (plaza 42.1 → 48.9 fps). This is also a candidate pull request for upstream.

### 14:20: main thread round 2 finished, nothing kept

| Idea | Result |
|---|---|
| **M2:** the guest store slow path | **Rejected.** Counted: the main thread makes ~38,000 slow-path stores a frame, 96 % of them to lines no reservation uses. Moving those inline removed them all and changed nothing: street 44.96 → 44.92 fps (guest 4–20 s, 6 runs each, no PGO on either side). The earlier ~1 ms estimate came from profiler skid; each call costs under ~8 ns. |
| **M7:** pin the main thread to a CPU of its own | **Rejected:** −2.1 % full window (48.94 vs 50.01 fps), −1.0 % guest 4–20 s (8 rounds). The main thread's own work is 0.2–0.6 ms faster, but the job threads lose a CPU and it waits longer. |
| **Power** | All four cores always run at **one shared clock** (~2,930–3,050 MHz of 3,500), set by the 15 W cap: CPU 9.1–9.4 W, GPU 2.3–2.7 W, SoC 0.8 W. Nothing per core is settable without root. Governor `performance`: no gain. Cutting the job threads' idle spin: no measurable watts. |

**What this means:** the watts go into real work: job threads 3.3 CPUs, main 1, RSX 0.8. So a faster job helps the main thread twice: it frees burst capacity (the 0.2 rule) and it frees power for the shared clock. 1 W less anywhere is roughly +4 % clock, about 0.8 ms of main thread. Less GPU draw helps the same way; GPU 1400 vs 1200 MHz costs the main thread 1.1 ms.

**Next main-thread candidate:** job 218. The main thread runs ~6 of these jobs a frame itself (~1.8 ms, one to one), so 10 % faster gives roughly 0.35–0.5 ms. After that: M8 (runtime per DMA command) and M3.

**One slip:** the governor test left the CPU's energy preference at `performance` instead of `balance_performance`. Setting the governor back worked, but the driver keeps the energy preference, and only root can write it. No fps effect at the 15 W cap; slightly more battery use at light load. A reboot restores it. From now on, agents don't touch the CPU governor or any other system-wide CPU setting. The GPU clock is back at 1100 MHz.

**Next:** the upstream v0.6.3 merge round, with each upstream change measured on its own against hd3.

### 18:40: upstream v0.6.3 merged (deck-perf 7c3c1e8, pushed); hd4 candidate ready, not installed

**What's in it:** all our changes plus everything in v0.6.3: the Flood Town freeze fix (upstream's job-runner test hangs on hd3's runner; the merged one passes all 8 cases), the controller thread, Esc = START, the Linux setup on stock SteamOS. Our optimisation files (placement, light tiles, short tail, the runtime) have no code change; the one runtime diff is upstream's comment wording.

**Conflicts:** wws_runner.c (both kept: our help-peek next to upstream's checks), cellPad.c (both kept, our SDL lock reworked for the controller thread), vk_overlay.cpp (upstream's: the menu no longer calls SDL). Commits: 2335d44 (merge), 64574f3 (play.sh), 7c3c1e8 (docs).

**The startup-crash fix with the controller thread** (stand-alone test program, work/bench/mg3-sdlrace.c):

| Version | Runs that crash |
|---|---|
| v0.6.3 as released | 10 of 10 |
| our old fix merged as is | 4 of 20 (ASan: double free in 2 of 3) |
| the merge's lock | **0 of 20**, 0 of 10 with ~1,000 hot-plugs, ASan clean in 5 |

**What upstream's additions cost** (same binary, street, guest 4–20 s, real controller on):

| Setting | fps (SE) | main ms |
|---|---|---|
| controller thread every 1 ms (upstream's default) | 46.46 (0.26) | 19.41 |
| no thread (the old way) | 46.58 (0.37) | 19.35 |
| **thread every 4 ms (our Linux default now)** | **47.12** (0.22) | 19.14 |
| Flood Town checks + stall repair off vs on (n = 8) | 46.78 vs 46.74 | 19.34 vs 19.31 |

So the controller thread at upstream's 1 ms did cost us; at 4 ms it is the fastest option. The Flood Town checks cost nothing measurable. Windows keeps upstream's 1 ms.

**Final A/B, hd4 candidate (work/port/mg3r, PGO retrained) vs hd3** (EPP performance in every run, replayed input):

| | hd3 | hd4 candidate | Δ (SE) |
|---|---|---|---|
| Street, full window (clean runs) | 49.47 | 49.67 | +0.20 (0.55) |
| Street, guest 4–20 s (8 runs) | 46.78 | 46.74 | −0.04 (0.20) |
| Plaza, full window | 51.45 | 51.57 | +0.12 (0.53) |
| Plaza, guest 4–20 s | 47.55 | 47.80 | +0.25 (0.29) |
| **Standing, real controller, 60 cap, 2 × 10 min** | 59.45 / 59.52 fps, p99 19.5 ms | **59.65 / 59.69 fps, p99 18.3 ms** | faster |

Equal within noise in the replayed benches. Faster with the real controller, as in play: SDL's update of the Deck's controller takes ~8 ms about once a second; hd3 does it inside the game's pad call, hd4 on its own thread. One 58 ms frame in hd4's first standing round didn't come back in the second; hd3 had none. Job replays: 213 and 216 pass 400 of 400. All 64 runs of the round exited cleanly.

**Install note:** v0.6.3 rewrites an old input.toml once at first start (Esc becomes START), so back it up with the install.

### 18:50: hd4 installed (your OK)

- Files: engine 5fecd563…, module 508d9e5c… (work/port/mg3r, PGO mg3r) in work/port/build/.
- Your launcher is kept as it was, plus the one new setting: the controller thread every 4 ms.
- Backups: work/port/installed-hd3-20261119/ (both files), tools/play.sh.installed-hd3-20261119, input.toml.installed-hd3-20261119.
- Smoke test of the installed files (street route, real controller, a copy of your input.toml): exited cleanly by itself, no errors.
- Your input.toml changes once at the first start: keyboard START becomes Esc instead of Enter (`start = "Escape"`, `layout = 2`). The controller's buttons don't change. F2 > Controls changes it back.
- **Rollback:** copy the two files from installed-hd3-20261119/ back to work/port/build/, and the two .installed-hd3 files back.

### 19:15: full install of our latest version (your request)

Until now each install replaced only the two game files and patched your old launcher by hand. The rest of your folder was still upstream **v0.5.7** source from 15 Nov, with the v0.5.7 launcher. Now:
- **Your folder is one version:** tag `hd4` (deck-perf 22a90db, pushed to the fork), on a local branch `installed`. The launcher is our branch's `tools/play.sh`: v0.6.3's launcher plus all our Deck settings (4 job threads, thread placement, the TLS fix, controller every 4 ms).
- **Unchanged:** the hd4 game files (already installed), saves, game data, shader cache, DLC.
- **What the new launcher did at its first start:**
  - copied your settings.ini into the game folder (the original stays in ~/.config/infamous2-port);
  - backed up your saves to saves-backup/;
  - found your 26 DLC unlock files;
  - made the dlc/ and mods/ folders.
- **input.toml:** updated once: keyboard START = Esc (was Enter). The backup is input.toml.installed-hd3-20261119.
- **Start test with the real launcher** (sound off, ~1 min, then closed): window up, settings applied from the folder, PGO module loaded, placement and controller thread on, no errors.
- **Rollback:** the old launcher is committed on branch `backup/v057-launcher-20261119`. The hd3 files are in work/port/installed-hd3-20261119/.
- **Next installs:** switch the branch `installed` to the new tag and copy in the two game files.

### 22:45: faster testing round finished (agent ft1; commits dc67c5b, b2c9763, 483d0a6)

| Tool | How long | Validated against known results | Good for |
|---|---|---|---|
| **Builds** (incremental, byte-identical to a clean build) | 1–2 s for one native or engine file (was ~15 min); a re-lift ~7 min | identical objects | everything |
| **Quick street run** (route to guest ~30 s) | 68 s per run (was 142 s), every run usable | — | every A/B |
| **Route steering** (closed loop past the tram gate) | — | 11 of 11 runs on the old path (was ~50 %); draws within 2 % | full-window street runs |
| **Early stopping** (min 3, max 8 pairs, ±3 SE) | big effects decided at pair 3: ~7 min | light tiles decided at pair 3; old neutral results stay inconclusive, as they were | effects of ~2 % or more |
| **A/B inside one run** | one 3–4 min run | light tiles: right sign and mechanism (GPU −5 ms, clock +125 MHz); controller 4 ms vs 1 ms: agrees; main-thread pinning: **not** reproduced | power, clock, GPU and thread questions; fps effects of 2–3 % or more |
| **Work counters** (reject-only filter) | 4+4 runs, ~9 min | noise ~1 % per thread group | ruling out code changes that change no work |
| **Slow-it-down test** (job delay, low-power wait) | one run | the 0.2 rule reproduced: 0.19 ms per ms of pool job time | sizing a candidate before building it |

**The honest limit:** fps effects of 0.5–1 % still need many runs with any method, because the crowd makes each run's fps vary by ~1 %. The gains are fast builds, every run usable, quick runs, and fast answers for anything ≥ 2 % or about mechanisms.

**New finding for the roadmap:** the main thread's own 218 jobs don't count one for one. A 0.78 ms delay there cost only +0.12 ms of frame time, because those jobs run inside its waits for the bursts. So job 218 ~10 % faster is worth about +0.3–0.4 ms of frame (~+2 % fps), through capacity and power, not the 0.35–0.5 ms one for one.

**HUD:** on in every test run from 19:30 (your request). Its cost is below noise (fps +0.5 %, SE 1.2).

### 20 Nov 01:10: step 1, job 218: faster and exact, but no fps; parked

- **Per job:** −8.6 % on the street (269.9 → 246.6 µs), −8.2 % on the plaza. About 1.5 ms less job CPU a frame.
- **Exact:** 800 of 800 recorded jobs, and 0 of 7 M live calls differ.
- **Changes:** a zero-weight shortcut in the blend, the FK first pass without per-op flushes, and a native for 0x17280. The model for that native comes from the Switch fork (MIT, credited).
- **fps:** no change. Quick street −0.01 % (SE 0.61), plaza +0.38 % (SE 0.53). In-run, the main thread doesn't move.
- **What it shows:** the freed job-thread time just turns into idle spin. This is the second job speed-up after 216 that doesn't reach the main thread. Making jobs faster is not the lever by itself. What the main thread actually waits for is the open question, and that's what step 2, the sizing, now has to answer before anything else gets built.
- **Parked** as work/bench/j218-natives.patch, so it can still be a candidate in the combined round (step 6).
- **Side note:** in replays, HEAD's engine runs 218 about 3 % slower than hd4's. To check before the next engine install.

### 20 Nov 02:40: step 2, sizing: what really limits the frame

**The old model was wrong.** The job threads aren't overloaded: over a frame they are busy 76 %, spinning 4.5 % and asleep 19 %. The main thread itself produces the jobs as its game code runs. What costs it time is **waiting for specific jobs it kicked**, and the biggest single cause is Edge (job 223):
- at the start of each frame, Edge's ~15 jobs fill all 4 job threads for ~4 ms;
- the main thread's animation jobs, kicked 1–4 ms into the frame, queue behind them (2.2 ms on average, in 346 of 479 frames). Then the main thread waits for them, ~1.5 ms a frame;
- that's why the faster 216/218 natives did nothing: they removed CPU from jobs nobody was waiting for.

**Frame sensitivities** (slow-it-down tests, ms of frame per ms inserted):

| Part | Sensitivity |
|---|---|
| main thread's own code | 0.3–0.9, depending on where |
| Edge on the job threads | 0.13–0.23 |
| SPU memory transfers | 0.18 |
| graphics thread, small delays | ~0 (it has ~1 ms of slack), large delays 0.8 |
| GPU watts | ~1.9 ms of frame per W |

**Tried in the speed-up direction:** letting Edge's jobs yield to the main thread's in the job queue (prototype): **frame −0.49 ms (SE 0.15), fps +2.4 % (SE 0.7)**. The wait after Edge falls from 1.46 to 0.38 ms.

**The main thread's own code** (15.6 of 21.4 ms) is limited more by instruction count and code size (the CPU front end) than by memory. That's the code generator's lever.

**New order:**
- **Step 3:** the job priority (Edge yields), built properly.
- **Step 4:** the main-thread code generator, sized first with one change.
- **Step 5:** R2 (graphics thread, the next limit) together with M8.
- **Edge on the GPU** stays later; S1 takes most of its measurable effect for far less work.
- **GPU clock:** 1000 vs 1200 MHz is +3 %, but the 19 Nov sweep found 1000 and 1100 equal, so your 1100 already gets most of it.

### 20 Nov 06:10: step 3, job priority: built, safe, a modest gain; the graphics thread is next

- **The design:** Edge's jobs now give way to the jobs the main thread waits for. A general rule, not a hard-coded job number: a long-job manager that the main thread didn't kick goes in a second, lower queue. A job that waits 8 ms goes first anyway, so nothing can starve. Switch `PS3_NATIVE_JOBS_PRIO`, off by default until the combined round.
- **Results** (same build, switch on vs off):

| Test | fps | main thread |
|---|---|---|
| plaza | **+2.06 %** (SE 0.63, clear) | −0.66 ms |
| quick street | +1.25 % (SE 0.88, not clear) | −0.40 ms |
| standing at spot 2 | equal | |

- **Safety:** upstream's job-runner tests pass, plus 7 new cases for the priority. Edge's results are never late for the graphics thread. 39 runs, all clean.
- **Why the street gains less than the prototype suggested:** the main thread's wait behind Edge dropped from 1.44 to 0.59 ms a frame, but it now waits for the **graphics thread** at the end of the frame instead. So the graphics thread (R2: the cost per draw) now caps the street.
- **Watch item for the final check:** standing frames over 33 ms were 7 with the priority vs 3 without (20 min each), within the usual range.

**Next (step 4): R2, the graphics thread's per-draw cost**, moved ahead of the main-thread code generator, because a faster main thread would now just wait longer for the graphics thread.

### 20 Nov 08:20: step 4, R2 (graphics thread): profiled; multi-draw exact but no gain, parked

- **Where the graphics thread's ~19 ms a frame go:**

  | Part | ms a frame |
  |---|---|
  | vertex and index work | 3.9 |
  | walking the PS3 command list | 3.7 |
  | the Vulkan driver | 3.4 |
  | our Vulkan code | 1.7 |
  | memory copies and compares | 1.7 |
  | texture / program / pipeline lookups | 1.8 |
  | dirty tracking | 0.9 |

  The Vulkan draw calls themselves are only ~2.4 ms. The expensive draws are the ones where the **textures changed** (17 % of draws, 5.2 ms) or the **shader program changed** (2.7 %, 3.0 ms).
- **What was built:** joining neighbouring draws into one Vulkan call (multi-draw) and skipping repeated binds. The picture was byte-identical over 8.1 GB of checked images, but there's no measurable gain (street +0.43 %, SE 0.47; plaza −0.44 %, SE 0.60). It saves only ~0.15 ms. Parked as a patch.
- **Where the graphics thread's real savings are:**
  - **R8:** the texture-changed and program-changed draws, ~3.4 ms of possible cuts;
  - **R5:** persistent descriptor sets, ~0.5 ms.
- **The order stays:** with the job priority on, the main thread is the first wall and the graphics thread is ~1 ms behind it. **Next, step 5: the main thread's code generator**, then R8 for the graphics thread. For 60 fps the graphics thread needs about −2.5 ms in the end.

### 20 Nov 09:30: step 5, the main thread's code generator: a real saving, capped by the graphics thread

- **Where the main thread's instructions go:**
  - the game's own work: ~45 % of the generated code;
  - checking each memory store for SPU coherency: 11 %;
  - saving and restoring registers: ~10 % (host stack) + ~10 % (the PS3 register file);
  - per-function call frames: 5 %;
  - address computation: ~14 %.
  There's no hotspot.
- **What was built:** cheaper store checks. One check per run of 3 or more stores instead of one per store, and a shorter check instruction. Two switches set when the code is generated, off by default. Verified exact: 35,039 store runs checked by an independent checker with 0 differences, and a self-checking build with 0 reports.
- **Results** (PRIO on, both sides without PGO):

  | | change |
  |---|---|
  | main thread instructions | **−6.2 %** (SE 1.1) |
  | main thread's own time | **−0.42 ms** (SE 0.07) |
  | street fps | +0.96 % (not clear) |
  | plaza fps | −0.22 % (no change) |

  The saving is real, but the **graphics thread is right behind the main thread** and caps the frame.
- **The code generator's ceiling:** bigger gains need a new calling convention between generated functions. That would be worth ~0.5–1 ms but take weeks, so it's out of reach for now. Committed with the switches off; step 6 decides.

**Next: R8**, cheaper texture-changed and program-changed draws on the graphics thread (~3.4 ms of possible cuts). That is now what holds both the job priority's and this change's gains back.

### 20 Nov 11:25: step 5b, R8 (graphics thread): −0.4 to −0.5 ms, byte-identical

- **What was built:**
  - descriptor sets kept across frames: 520–600 new sets a frame → 40–69;
  - a memo for the shader program analysis: 95 % of those analyses skipped.
  Switch `PS3RECOMP_ENG_R8`, off until step 6.
- **Exactness:** 23.7 GB of replayed draws compared, 0 bytes differ; 865,841 memo hits re-checked, 0 differ.
- **Results** (job priority on, both sides):
  - graphics thread **−0.41 ms** on the quick street (SE 0.05) and **−0.46 ms** standing at the plaza;
  - fps: **+1.96 %** standing at the plaza (SE 0.62), +0.89 % on the quick street (not clear).
- **Why only ~0.5 of the ~3.4 ms r2 hoped for:** most of the cost of a texture- or program-changed draw is real new work (a new mesh, a first bind), which nothing exact can skip.
- **Where things stand now:** on the walking street the graphics thread is ~17.9 ms and the main thread ~19.2 ms, so the main thread is the first wall again, ~1.3 ms ahead. For 60 fps: the main thread needs ~−2.5 ms, the graphics thread ~−1.3 ms.

**Next: step 6, the combined final check**, with everything kept on: the job priority, the cheaper store checks and R8. Each one is measured alone and all together, PGO is retrained, and the full check runs against hd4. If it qualifies, it becomes **hd5**, installed only with your OK.

### 20 Nov 16:20: step 6 finished, hd5 candidate ready (not installed)

**All three improvements together** (no PGO, same conditions; removing any one is slower on both scenes, so all three stay on):

| | street | plaza |
|---|---|---|
| all three vs none | **+2.6 %** (SE 0.51) | **+2.1 %** (SE 1.07) |

**hd5c** (all three + PGO retrained, mg5r) **vs your hd4**, same batch:

| Test | Result |
|---|---|
| Street, full walk | **+2.63 %** (50.44 vs 49.12 fps) |
| Street, busiest part | **+1.65 %** (SE 0.46); main −0.40 ms, graphics thread −0.43 ms; slowest 1 % of frames −1.4 ms |
| Plaza | **+1.70 %** (SE 0.47) |
| Standing at spot 2, real controller, 2 × 10 min each | 59.77 / 59.82 vs 59.73 / 59.77 fps; no frame over 50 ms |
| Job replays, upstream's job-runner tests | identical / 15 of 15 |

All 40 runs were clean. Watch item: slightly more short graphics-thread waits on the street (< 0.001 ms a frame).

Launcher defaults (tools/play.sh): the job priority and R8 on. Linux builds use the cheaper store checks.

### 20 Nov 16:45: hd5 installed (your OK), full install

- **Tag `hd5`** = deck-perf e9e05ea, pushed to the fork. Your folder is on branch `installed` = hd5, so everything is one version, launcher included. The launcher now switches on the job priority and R8. The game files are hd5c's (PGO mg5r).
- **Backups:** work/port/installed-hd4-20261120/ (the two hd4 files), input.toml.installed-hd4-20261120, settings.ini.installed-hd4-20261120. Rollback: `git switch -C installed hd4` in the folder, then copy the two files back.
- **Start test with the real launcher:** the PGO module loaded, the window came up, the controller thread, placement, and the settings (job priority 1, R8 3, controller every 4 ms, 4 job threads) were active, with no errors. Saves backed up by the launcher as usual.

### 20 Nov 18:45: step 8, sizing the big options

| Option | Measured | Realistic gain | Effort |
|---|---|---|---|
| **The Vulkan recording worker** (already in the engine: `PS3RECOMP_VK_THREADED=1 PS3_CPU_PLACE_VKW=1`, records the Vulkan calls on the graphics core's idle twin) | street **+2.12 %** (SE 0.69, decided after 3 pairs), graphics thread 18.1 → 16.3 ms; plaza +2.4 to +3.4 % | +2 to +3 % | hours (full check + PGO) |
| **Edge on the GPU** | Edge's math part removed (measurement mode): **+3.4 %** (SE 0.85), main thread −0.75 ms at the same clock (less fighting over the shared cores, not power); all of Edge extrapolated +5 to +12 % | +4 to +10 % | weeks |
| **A native for Edge's biggest lifted function (0x9C20, ~2.4 ms a frame)** | same lever | +2 to +4 % | days |
| A new graphics-thread split design | ~0 beyond the worker until the main thread is faster | ~0 now | weeks |
| New calling convention | 3.3–3.5 M instructions a frame | +1 to +2.5 % | weeks |
| ThinLTO / call / frame flags | nothing to gain (no PLT, no frame pointer; ThinLTO +0.10 %) | 0 | — |

The recording worker lost in the 17 Nov test with the old graphics thread; with today's it wins, and it alone meets the graphics thread's −1.3 ms target.

**Recommended order:**
1. the recording worker on by default (full check + PGO);
2. Edge: first the 0x9C20 native plus a placement check, then Edge on the GPU if 60 fps still needs it;
3. the calling convention last.

### 20 Nov 20:30: hd6 built and checked in about 1.5 hours (your request)

- **What hd6 is:** hd5 + the Vulkan recording worker on by default (launcher) + an engine PGO retrained with it on (mg6r). The game module is hd5's.
- **Build time:** training 11 min, merge 1 min, build 38 s, thanks to the faster builds.
- **Quick check against hd5,** same batch, alternating:

| Test | hd6 vs hd5 | Graphics thread |
|---|---|---|
| Street, busiest part, 8 pairs | **+0.65 %** (SE 0.42, not clear) | **16.7 → 14.3 ms** |
| Plaza, 8 pairs | **+0.44 %** (SE 0.67, not clear) | **18.0 → 16.2 ms** |
| Standing at spot 2, real controller, 5 min each | 59.87 vs 59.81 fps; 1 frame > 33 ms each; none > 50 ms | |

- **Reading:** the worker does its job, taking 1.8–2.4 ms off the graphics thread, but the main thread (~18.5–19 ms) is now the only limit on both scenes, so fps barely moves. The sizing round's +2.1 % came from a test that stopped after 3 pairs, which tends to overestimate. hd6 isn't slower anywhere and gives the graphics thread the headroom it needs once the main thread gets faster (Edge, next).
