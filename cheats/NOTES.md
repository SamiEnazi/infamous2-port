# inFAMOUS 2 cheats -- notes and credits (cheats agent, 2026-11-09)

Title knowledge for v1.04 = PPU-41a7d047c573c78c0307506c9d1b52d15a66e4f5 (BCES01143/BCES01144/
BCUS98125/BCES01229 1.04, NPUA80638/NPEA00318 1.03). Gitignored: never commit or publish.

## Community sources (credit)

- **bungholio** -- Artemis code list "inFamous 2 BCES01443 01.04.ncl" (sic, = BCES01143 1.04) in
  bucanero/ArtemisPS3 `docs/codes/` (https://github.com/bucanero/ArtemisPS3). Every 1.04 site
  matched our EBOOT byte for byte. Used directly (as run-time instruction alternatives,
  patches.txt): Jump Height Multiplier (0x22A104), AI Movement Speed Modifier (0x1D99BC), Most
  Enemies Never Attack (0x1F4764). Used as leads: "Cole Never Dies" (0x17AC50: health at
  character+0x7F0, max +0x7EC), "Electrical Attacks Do Not Decrease Electricity" (0x154DAC:
  energy at +0x1F68).
- **PS3UserCheat / CodeUnique** -- the US 1.00 list "inFamous 2 BCAS20181 BCUS98125 01.00
  BCES01143 01.01.ncl" in the same repo: Exp 99999999 (a static pair of u32 at 0xA18C60/64),
  Infinite HP ([0x9E8D20]+0x7BC/0x7C0), Infinite Energy (+0x1F34..0x1F3C). Not our build
  (that list's sites do not match even our EU 1.01 disc ELF), but the layout guided the 1.04
  search: same fields, +0x30 later in 1.04.
- No NetCheat/CCAPI/Cheat Engine tables for inFAMOUS 2 were found (searched 2026-11-09).

## Own findings (static, capstone on eboot\EBOOT_upd104.elf; tools in this folder)

- `[0x00A6E9E4]` = Cole's character object (the 1.00 code's 0x9E8D20). Found from the HUD-ish
  check at 0x0C1694 (`r3 = [[r30-0x7FE4]]`, then +0x7EC/+0x7F0/+0x1F68/+0x1F70); 58 module
  tables point at it, the character's constructor 0x158744 stores it (0x158B8C).
- character +0x7EC max health, +0x7F0 health, +0x1F68 energy, +0x1F70 max energy (floats).
- 0x001578F8 Cole's per-frame character update (via thunk 0x0066FECC, this -= 0x314); its
  0x1579D4 branch is health regen (min(hp + rate*dt, max)).
- 0x00363904 SetHealth(r3 character, f1 new health): every health change goes through it
  (heal: 0x363E54; damage: TakeDamage -> 0x363E54 -> here).
- 0x00363E54 ApplyHealthDelta(r3 character, f1 delta, r5 damage or 0): new = min(hp + delta, max).
- 0x00364044 TakeDamage(r3 character, r4 damage object): ApplyHealthDelta(-[r4+0x7C]);
  [r4+0x64] is told "you hit something" (event 0x7BF) = the source.
- 0x0017ABE0 the player's hit reaction (state 0x11 when health <= 0, else 0x10).
- XP: the save's STATE holds the pair (725, 725) at file offset 0x14/0x18 in work\saves\
  first-save (the 1.00 static pair); its 1.04 guest address is found by the run-time probe
  (PS3_CHEATS_PROBE_XP=725) -- see the results below.

Tools: ppcdis.py (disassemble), funcs.py (function of an address), scanimm.py (D-form
displacement scan), xref.py / gref.py (who uses a data address), gmap.py (static map of .bss
globals reached through module tables), callers.py, anchor.py.

## How the cheats work (cheats.cpp)

| Cheat | How |
|---|---|
| Invincibility (was "God mode") | hook SetHealth: Cole's new health is never below his current one; health filled once when switched on |
| Infinite energy | hook Cole's update: energy = max energy after every frame |
| One-hit kills | hook TakeDamage: a hit whose source is Cole (the character, or an object whose +0x1C is it) carries at least the victim's health; the damage object is restored after |
| Super jump | instruction alternative at 0x22A104 (bungholio): 2x/3x/5x/10x |
| Enemy speed | instruction alternative at 0x1D99BC (bungholio): frozen/0.25/0.5/2x |
| Enemies hold fire | instruction alternative at 0x1F4764 (bungholio) |
| Add XP | hook Cole's update: adds the amount to both words of the XP pair (game thread) |

## Results

Run cheats1 (2026-11-09 07:02, native\recordings\cheats1, city from work\saves\first-save, exe
work\port-cheats\build lifted with hooks.txt + patches.txt, toolkit agent/cheats cdd3181):
- Lift: 3 hook slots + 3 instruction slots registered (slot 2 in 4 lifted copies: mid-function
  entries); register-locals pass: 31952 functions rewritten, 0 kept plain (same as without).
- Cole's update hook runs every frame (Cole = 0x496FFE90, health 400/400, energy 180/180).
- Invincibility self-test through the game's own ApplyHealthDelta: hit of 100 with Invincibility ->
  health stays 400 (PASS); without -> 300 (PASS); health put back.
- Super jump 5x: shot j_on (Cole at the top of the screen) vs j_off (normal hop). PASS.
- Menu: the Cheats tab (6th) with groups, ON marks, choices; save note shown. PASS.
- No VUID / exception / fatal / assert lines.
- Damage events seen were NPC vs NPC (amount 0): src = the attacking character object, so a hit
  of Cole's should carry src == Cole's character.
- XP: (725, 725) pairs at 0x0098D4F8 (a constant in the ELF file: not XP), 0x00A64E50 (G+0x30 of
  the progress struct G = 0x00A64E20, read by getter 0x4E694 / 0x3D8AF8 like a counter),
  0x00A657BC (G+0x99C, inside the sub-object at G+0x968 that the save serializer 0x3DD7E0 writes),
  0x40354290 (heap). Settled in run cheats2.
- The scripted bolts did not fire during the Wolfe call (energy never moved): energy re-checked in
  run cheats2.

Run cheats2 (07:35, native\recordings\cheats2, same exe + test knobs drain/poke, toolkit edf97fd):
- Infinite energy with real powers: cheat off, 7 aimed bolts -> energy 180 -> 173 (1 per bolt);
  cheat on (t=21), 8 more bolts -> stays 180 (no change logged). drain to 10 with the cheat on ->
  180 again within one frame (t=34.00 -> 34.26 log); cheat off, drain to 10 -> stays 10. PASS.
- XP: +5000 at A 0x00A657BC -> 5725/5725, +1000 at B 0x00A64E50 -> 1725/1725 (logged). The pause
  menu's first page shows no XP (Powers needs Cross): which one is shown checked in cheats3.
- One-hit kills: the bolts hit the planks (the man stands above-right of the reticle): no hit of
  Cole's yet; NPC-vs-NPC damage events again (src = the attacker's character).
- No VUID / exception / fatal / assert lines.

Run cheats3 (08:09, native\recordings\cheats3): XP decided.
- Both XP candidates sit in a copy of the save's first record: [-0x14] 0x52 (A only), floats
  -492.5 / -500.5, 0, 0, then the pair (725, 725) -- the same bytes as STATE +0x00..+0x18.
- +5000 at A (0x00A657BC) and +1000 at B (0x00A64E50); the pause menu's Powers page shows
  "1725 AVAILABLE XP" (shot w95): **XP = 0x00A64E50** (G+0x30; getter 0x0004E694 returns it).
  A = G+0x99C is a snapshot of the record (left alone by the cheat).
- No damage event in this run (the camera nudge put the reticle ~80 px from the man; bolts missed).

Run cheats4 (08:29, native\recordings\cheats4, cheats.cpp with the built-in XP address):
- Add XP 5000 with no override: "+5000 XP: now 5725 / 5725" and the Powers page shows
  "5725 AVAILABLE XP" (shot w95). PASS.
- Invincibility/one-hit/speed=frozen/hold fire switched on at once: no crash, no VUID/exception/fatal/assert.
- R2 alone threw no grenade and the camera landed elsewhere: still no hit of Cole's on anyone.

## Status

| Cheat | Checked |
|---|---|
| Invincibility | PASS: game's own ApplyHealthDelta, 100 damage -> health kept (off: -100) |
| Infinite energy | PASS: bolts cost 1 each with it off (180 -> 173), none with it on; drain refilled in a frame |
| Add XP | PASS: Powers page 725 -> 5725 |
| Super jump | PASS: 5x jump screenshot |
| Cheats page | PASS: sixth tab, groups, ON marks, save note |
| One-hit kills | NOT SEEN on a real hit (no target hit in 4 scripted runs). Hook runs on every damage event; observed events carry src = the attacking character, which the check compares with Cole |
| Enemy speed / Enemies hold fire | NOT CHECKED (no enemies at the test spot). Community codes for this EBOOT, lifted as switchable alternatives; switching them on did not crash |

Re-run a check: work\cheats\turn<N>.ps1 under tools\with_game_lock.ps1 (build_cheats.ps1 builds
work\port-cheats\build; test_cheats.ps1 runs a steps file with the [cheats] probe log).

## Batch 2 (2026-11-09 morning): Invincibility rename, karma, max battery, glide, remembered cheats

Findings (static + snapshots of Cole's object):
- Karma = float at G+0x20 (G = progress struct 0x00A64E20; -492.5 in work\saves\first-save), + = hero
  (blue), - = infamous (red). Rank thresholds at 0x00745CF0+0x1DC: +-1, +-1000, +-2500.
  AddKarma 0x0012FC6C (r3 G, f1 delta, r5 reason/-1) is every gameplay change (bungholio's karma
  codes sit inside it); loads/resets call SetKarma 0x0012F6E0 directly. Crossing a rank snaps karma
  (e.g. -492.5 + 500 -> 49) and the game shows its rank screen ("YOUR KARMA RANK IS NOW: GUARDIAN").
- Battery: RecomputeMaxEnergy 0x00154DB4 (r3 Cole): max = +0x1F6C base + shard level x energy/level
  (0x0009B780; level from a cumulative shard table, data+0x3C, levels at data+0x40) + 15 per set bit
  0x20/0x40 of G+0x944; sets energy = max when max changes. Derived at every load (not saved).
- Movement: position +0x1560 (x, y, z up; ~cm), velocity +0x1570 (copy +0x1C20). Gravity ~-1500/s^2;
  Static Thrusters (R1 in the air) ~-350/s^2, and the R1 press cuts a rising jump's speed.
- Protect targets: the game has HEALTH_METER / CHealthMeter (class 88 of a 584-class table at
  0x009E9CFC), DAMAGE_METER, ESCORT/CEscortSkill (427), HERO_INVULNERABLE; the link from a mission's
  health meter to its target needs a save at such a mission (not available) -- not built.

| Cheat | How |
|---|---|
| Karma lock / multiplier / XP for karma | hook AddKarma: a change towards the other side is dropped; gains of the side x multiplier; XP += points x setting |
| Add hero / infamous karma | the game's AddKarma, called from Cole's update (bypassing the lock) |
| Max battery | hook 0x0009B780: every shard level (+ the two upgrade bits' 30); the game's recompute run once on switching |
| Glide strength | Cole's update: on frames with the thrusters' gravity (-1000..-100 /s^2, in the air) keep 1/2, 1/4, 1/8 of it; Fly settles at ~-30/s |
| Remember cheats | the menu's settings.ini: PS3_CHEAT_<ID>=<value> for every toggle/choice/slider that is on (never the Add ... actions), set again at window open + a 8 s notice "Cheats on: ..." |

Results (runs cheats5-7, city from work\saves\first-save; exe work\port-cheats\build, toolkit f7dec86):
- Karma (cheats5): Add hero 500 -> -492.5 to 49 (rank snap) + the game's "YOUR KARMA RANK IS NOW:
  GUARDIAN" screen; lock hero + 3x + 2 XP: -100 blocked, +100 -> +300 karma and +600 XP; Add
  infamous 500 -> 349 to -151 + "Karma Rank changed to THUG"; lock infamous: +50 blocked, -50 applied. PASS.
- Max battery (cheats6): max energy 180 -> 420 (bar refilled), off -> 180. PASS.
- Remembered cheats (cheats7): a settings file with Invincibility, Infinite energy, Max battery,
  Glide Fly, Karma lock Hero -> all set again at window open, notice "Cheats on: ..." shown (shot
  toast), Max battery already 420 at Cole's first frame (the game's own load-time recompute went
  through the hook); a menu change wrote the file (log "remembered: god=1 energy=1 maxbat=1 jump=2
  karmaside=1 ..."); Remember off -> the file keeps PS3_CHEATS_REMEMBER=0 only. PASS. (Since then a
  slider at its bottom, e.g. karma multiplier 1x, counts as off and is not written.)
- Glide strength (cheats7): the velocity writes changed nothing (Fly and Normal jumps identical,
  1.36 s vs 1.39 s airtime): the physics state is not in the character's +0x1560 block. Removed
  from the menu; next step = find the character controller / physics body Cole's update reads.
- Protect targets: not built (see the findings above).
- No VUID / exception / fatal / assert lines in runs 5-7. Final dev build compiled (run 8, no game).

Promoted to work\cheats (cheats.cpp, hooks.txt with 5 hooks, patches.txt) on 2026-11-09 09:2x.

## Stage B: glide strength (2026-11-09 09:30-10:10) -- NOT achieved, nothing promoted

Every PPU-side copy of Cole's motion is an output the physics engine rewrites each frame:
- writes to the velocity copies (+0x1578 / +0x1C28), run cheats7: no effect;
- [Cole+0x138] -> an entry of a scene/transform list in the 0x401Axxxx heap (entry+0 = Cole+0x130,
  entry+0x10 = velocity, a frame ahead of the copies; 88 functions use +0x138, 0x7ABE4 = set scale):
  writes, run cheats9: no effect (the Fly "airtime" there was Cole running up a ramp);
- Cole's motion block at +0x1520 (the AI-speed code's region 0x1D9xxx..0x1DCxxx): 0x001DBFA4 syncs
  it from the physics object [motion+0x74] = [Cole+0x1594] = Cole himself; position from 0x004AAF28,
  velocity from Cole's vtable 0x0093E1E0 slot +0x64 = 0x0032D84C, which interpolates two timestamped
  transforms in Cole's object (+0x1F0 / +0x250, positions +0x220 / +0x280, buffer index byte +0x11E):
  velocity is derived from positions. Lifting both positions during a glide, run cheats12: no effect.
- Physics: free fall -1478/s^2 (median), Static Thrusters -350/s^2 (runs cheats8/11).
- Parameter search, runs cheats10/11: heap floats of +-350, +-3.5, +-14.8 (36 found; the first 24
  scaled to 10% one glide each): no glide changed (all 1.35-1.38 s, -344..-356/s^2). Untested:
  candidates 24-35 of run cheats11 (-350 x3, -14.8 x3, 14.8 x6).
Next step (bigger): find the physics engine's own character state or the controller's request to
it -- the writer of the +0x1F0/+0x250 transforms (likely an SPU physics job, its gravity/thruster
constants in the job's data), then hook that request on the PPU side.
Dev code (gsearch/air_tick/posscan/regions knobs, glide v3) stays in work\cheats\dev only.
- Last try (2026-11-09 10:05-10:13, runs cheats13/14): the heap layout repeats from boot to boot
  (same addresses in cheats11/13/14). cheats13: 16 more heap floats of 3.5 / 350 at 0x41C600B8..
  0x41CBD6D8; cheats14: the 12 left from cheats11 (-350 at 0x41D980B0, 0x41E658F0, 0x41F46854;
  -14.8 at 0x42658290, 0x42DC99B0, 0x43390F68; 14.8 at 0x42F71068, 0x42F712B8, 0x42FC79D0,
  0x42FC7A14, 0x430D8C38, 0x4314CEB8) + 4 more -350 (0x41F6A2C0, 0x41FCF9D4, 0x41FD5114, 0x41FE54B4),
  each scaled to 10% for one glide: none changed it (1.35-1.38 s, -343..-353/s^2). GLIDE PARKED.
  (Not tested: the rest of cheats13's list, e.g. -3.5 at 0x41D4F7E0.. and -14.8 at 0x43392AF4..)
