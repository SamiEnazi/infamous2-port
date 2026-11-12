# Recomp Achievements

Achievements for recompiled games. Players earn them as they play in [RecompLauncher](https://recomplauncher.com), master whole games, and climb the leaderboards. It's in alpha right now, with testers: [recomplauncher.com/achievements](https://recomplauncher.com/achievements).

This repository is for developers. It has the small C library a recomp uses to report achievements, the list format, and how it all fits together. Adding it is free for any game.

## How it works

A recomp reports achievements to the launcher in one of three ways:

| | What it is | Good for |
| --- | --- | --- |
| **Native** | The game's own achievements. Xbox 360 games already unlock them through the system, and the runtime passes each unlock on. | Xbox 360 recomps (ReXGlue) |
| **Memory** | Rules on the console's memory, checked every frame ("level 3 cleared" = this byte is 3). Written once per game and hosted by RecompLauncher, so a fixed rule reaches players without a recomp update. | N64, SNES and other console recomps |
| **Events** | Your code fires a named event (`beat_final_boss`) and the list maps it to an achievement. | Decompilations and source ports |

The recomp never talks to accounts or servers. It sends small messages to RecompLauncher over a local pipe, and the launcher does the rest: the unlock toast, saving, syncing and the anti-cheat checks. Started without RecompLauncher, the library does nothing, so it's safe to ship in every build.

**Ranked and Casual.** An unlock counts on the leaderboards (Ranked) when it happens live, during a session the launcher saw running, on the player's own save. Unlocks from a copied save, a burst of unlocks right after a save loads, cheats or save states are kept but count as Casual. Memory rules can't fire just from loading a finished save: each rule has to be false for a frame before it can fire.

**Beaten and Mastered.** Finish the story and the game is Beaten. Unlock every achievement and it's Mastered. Both go on the player's profile. Achievements that need online play are left out of a list so Mastered can always be reached offline, and bonus achievements count for points but not for either.

**Leaderboards and rich presence.** A list can carry its own leaderboards (best times, high scores) and a "now playing" line for profiles.

**Rarity.** Every achievement shows how many players have it, and a second leaderboard weights points by rarity.

**Points.** Every achievement is worth 5, 10, 25 or 50 points. Native Xbox 360 gamerscore is converted to the same scale (up to 10 G is 5, up to 30 G is 10, up to 75 G is 25, more is 50), so totals compare across games.

All achievement sets are made for Recomp Achievements. None are copied from other achievement services.

## Add it to your recomp

```c
#include <rl_achievements.h>

rl_ach_set_info("your-recomp-id", "1.2.0");   // optional: your RecompLauncher id and version
rl_ach_init("n64", read_ram, NULL);            // once at start ("" and NULL for native or events only)

rl_ach_frame();                                // once per frame, from your VI or vblank hook
rl_ach_native(id);                             // Xbox 360: from the runtime's unlock hook
rl_ach_event("beat_final_boss");               // decomps: from your own code
rl_ach_save_loaded();                          // whenever a save is loaded
rl_ach_casual("cheats");                       // a cheat menu or save state was used
rl_ach_lboard(1, frames);                      // a leaderboard result from your own code
rl_ach_presence("World 3-2");                  // what the player is doing
```

Step by step for each kind of recomp: [docs/adding-it.md](docs/adding-it.md)
The list format and the messages: [docs/SPEC.md](docs/SPEC.md)
Ready-made hooks for shared runtimes (ReXGlue, N64ModernRuntime): [sdk/bridges](sdk/bridges)

Once it's in, [submit your recomp](https://recomplauncher.com/submit?platform=pc) or tell us on Discord, and we'll write or check the list with you.

## Build and test

```
cmake -S sdk -B build -DRL_ACH_TESTS=ON
cmake --build build
./build/test_rules
python3 sdk/tests/fake_launcher.py ./build/test_channel
```

C99, no dependencies beyond what's in `sdk/third_party`. Builds with GCC, Clang and MinGW on Windows, Linux and macOS.

## Players: want a game added?

Ask its developer. It's free, it's a small change for most recomps, and most developers are happy to hear a game is wanted. Point them here.

## License

Free for any game: use it, change it, ship it, paid or free. Launchers and front-ends need our written permission first. If you'd like Recomp Achievements in yours, open an issue or write to info@recompstore.com. The full terms are in [LICENSE](LICENSE). rcheevos and cJSON are included under their own MIT licenses.

Part of [RecompLauncher](https://recomplauncher.com). See also the [RecompLauncher SDK](https://github.com/elripalda/RecompLauncher-SDK).
