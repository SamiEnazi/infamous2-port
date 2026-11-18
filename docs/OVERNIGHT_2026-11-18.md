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
