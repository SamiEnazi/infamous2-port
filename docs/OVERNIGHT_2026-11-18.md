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
