# Adding Recomp Achievements

Add `sdk/` to your build (CMake: `add_subdirectory(rl-achievements/sdk)` and link `rl_achievements`, or just compile `sdk/src/rl_achievements.c`, the rcheevos sources and cJSON into your project). Then pick the section that fits your recomp.

Every call is safe outside RecompLauncher: with no launcher to talk to, they return straight away.

## Xbox 360 (ReXGlue)

The game already unlocks its own achievements through the system, and ReXGlue catches them. You only pass them on.

- If your ReXGlue build is from late June 2026 or newer, you may not need to do anything: RecompLauncher watches the unlock file ReXGlue writes (`achievements/<TITLE ID>.toml` in your user data folder) and picks up new unlocks by itself.
- For the best result (instant toasts, ReXGlue's own toast hidden while the launcher shows one, saves flagged as they load), use the bridge in [sdk/bridges/rexglue.cpp](../sdk/bridges/rexglue.cpp). It's about twenty lines in `KernelState`.

RecompLauncher reads the achievement names and gamerscore from the player's own copy, so you don't write a list.

## N64 (N64Recomp and N64ModernRuntime)

Memory achievements: rules on RDRAM, checked once per frame.

1. Copy [sdk/bridges/n64modernruntime.cpp](../sdk/bridges/n64modernruntime.cpp) into your project.
2. In your init callback (it receives `rdram`), set the pointer and call `rl_ach_init("n64", read_rdram, nullptr)`.
3. Call `rl_ach_frame()` from `vi_callback`.
4. Call `rl_ach_save_loaded()` where the game loads a save, if you have a hook there.

Addresses in N64 lists are RDRAM offsets (`0x000000` to `0x7FFFFF`), the same ones emulators and decomp symbol maps use. The bridge handles N64Recomp's byte order.

## SNES, GB, GBA, PS1 and other memory-based recomps

The same as N64: give `rl_ach_init` the console name and a function that reads console memory at the console's own addresses, and call `rl_ach_frame()` once per frame.

```c
static uint32_t read_ram(uint32_t address, uint8_t* buf, uint32_t n, void* ud) {
  if (address + n > sizeof(g_wram)) return 0;   // an address the console doesn't have
  memcpy(buf, g_wram + address, n);
  return n;
}

rl_ach_init("snes", read_ram, NULL);
```

## Decompilations and source ports

Events: you fire them from your own code, and the list maps each name to an achievement.

```c
rl_ach_init("", NULL, NULL);
...
if (boss_defeated) rl_ach_event("beat_final_boss");
```

Use short, stable names. Firing an event twice is fine; an achievement unlocks once.

## Leaderboards and "now playing"

Lists can have their own leaderboards (fastest boss, most coins) and a rich presence line ("World 3-2, 47 stars") that shows on the player's profile.

- **Memory lists** do both by themselves: the list holds the rules (see [SPEC.md](SPEC.md)).
- **Ports** send them from their own code:

```c
rl_ach_lboard(1, frames_taken);          // leaderboard 1 from the list, in its format
rl_ach_presence("World 3-2, 47 stars");  // sent only when it changes
```

## Keeping it fair

These make an unlock count as Casual instead of Ranked. Call them where they apply:

- `rl_ach_save_loaded()` whenever a save is loaded.
- `rl_ach_casual("cheats")` when a cheat menu, debug menu or trainer is used. Any short word works: `"save_state"`, `"debug"`, `"speedhack"`.

## The list

Memory and events lists are JSON files in the format in [SPEC.md](SPEC.md). RecompLauncher hosts them, so you can fix a rule without shipping an update. Send us your recomp and we'll write or check the list with you. Examples are in [examples/](../examples).

## Testing

Run the tests in `sdk/tests`. `fake_launcher.py` plays the launcher's side of the channel and prints every message your build sends:

```
python3 sdk/tests/fake_launcher.py ./your-recomp
```

In RecompLauncher, testers can turn on the dev tools to see the live message log, a memory viewer and a rule tester for each session.
