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

## Glide (2026-11-10, static only; cheats agent on branch glide) -- built, NOT yet run

How it was found: bungholio's US 1.00 "Hold L2 + X To Fly" code (ArtemisPS3 list "inFamous 2
BCUS98125 v01.00 ...") writes a z speed into the physics proxy's velocity; its AoB matched our
EBOOT once (0x004A63FC, the proxy sync 0x004A63A0, where the air movement's velocity arrives). From
the game's script reflection tables (names FALL_STATE, JUMP_STATE, SET_FLOAT_PARAMS, gravity,
INITIATE_GLIDE, FLOAT_REMAINING at 0x009723E0..) and the enum name lists at 0x009ECBA0 / 0x009ECBB8
back to the native code. The Stage B heap search could not find it: the value is a constant in
the ELF, not on the heap.

- Every character in the air has a jump/fall controller A ([A+0x1C] = the character; for the
  player the object at [0x00A6E9E4]). A+0x118 jump state (0 Launch, 1 PreFall, 2 Fall, 3 Assist,
  4 Attach, 5 Glide), A+0x120 fall state (0 Normal, 1 Danger, 2 Float, 3 Attack, 4 MeteorDrop,
  5 Yank), A+0x124 frame of the last fall state change, A+0x60 gravity vector (x, y, z, w; z up),
  the acceleration the air integrator 0x0019BE18 adds to the velocity.
- Fall state Float = the Static Thrusters. SetFallState 0x0019FB90; the fall state machine
  0x001A0378 enters Float when the thrusters button (mask at [0x00A2C8C8]) is pressed in the air and
  0x00154728 allows it (power 0x49 owned in the progress struct, character +0x2648 == 0), or when
  the character goes airborne with it held (0x001A6844). It stays in Float while the button is held
  (or A+0x138 forces it), power 0x49 is usable (0x00153860: energy > 0, or the power's energy rate
  is 0) and no drop/meteor drop is asked; each frame it charges the power's energy rate
  (0x00155EE0) and adds the frame time to A+0x12C (float time this jump, used only to give the
  upgraded thrusters' lift once per jump).
- UpdateGravity 0x0019BB20 (each air frame from 0x001A696C, and on every fall state change):
  A+0x60 = the character's gravity (character +0x160, ~(0, 0, -1500, 0)); in Float only,
  z = max(z, -350). -350 is the float at 0x00747488 (z of a vector in the game's movement tuning
  block 0x00747460, read-only data; its only reader is UpdateGravity). This is the measured
  -350/s^2 of the glide (Stage B), and on entering Float while rising the game scales the upward
  speed by sqrt(new gravity / old gravity) so the top of the jump stays as high (the "R1 cuts a
  rising jump" seen in Stage B).
- Also in Float (read, not checked in a run): the air-steering factor A+0x94 is 0.5 instead of
  1.0; with the powers 0x4A..0x4C (likely the thruster upgrades; bits A+0x114..0x116, set from the
  progress struct each frame) the thrusters add a lift that decays (SET_FLOAT_PARAMS values
  A+0xF0/0x100/0x110/0x130), and with 0x4C a fall faster than -1000 is eased back towards -1000.
- No glide timer: the script value FLOAT_REMAINING returns a constant FLT_MAX; the glide lasts while
  the button is held. The only possible limit is energy: in runs cheats8-14 (2.8 s glides, probe
  logging every energy change) energy stayed 180/180, so the thrusters' rate is 0 at that point of
  the game. Infinite energy covers it anyway. No "unlimited glide" cheat: nothing to remove.

| Cheat | How |
|---|---|
| Glide fall speed (Normal / 2x / 4x / 8x slower fall) | hook UpdateGravity 0x0019BB20: after the game's body, when A+0x120 == 2 (Float) and [A+0x1C] is Cole, A+0x68 (gravity z, normally -350) *= 1/2, 1/4, 1/8. Off = empty slot. |

Side effects checked (static): the hooked function sets A+0x60 for every character, but the cheat
only touches Cole's controller and only in Float; every other fall state and character keeps the
game's value. UpdateGravity runs again on every air frame and on every fall state change (states
0,1,3,4,5 through 0x0019FEEC, Float through 0x001A0120), so the scaled value never outlives the
glide, and switching the cheat off restores -350 on the next air frame. The tuning constant itself
is not written (it is shared read-only data). Readers of A+0x60 checked: the air integrator and the
fall state machine (Danger check, Normal state only; the rise scaling on Float entry, which keeps
the jump's top height); ~14 more vector loads of +0x60 in the movement code were not traced. All
of them read the glide gravity the game itself sets there, now scaled.
Untested: a scripted glide (A+0x138) gets the same scaling.

Test probe: with PS3_CHEATS_PROBE=1 the UpdateGravity hook is set too (cheat off or on) and Cole's
update logs one "[cheats] glide probe: <s> s, lost <h> height (<h/s>), gravity <g>, setting <n>"
line per glide, plus "[cheats] glide: gravity -350 -> <scaled>" while the cheat scales it.

Build: toolkit agent/glide (= agent/next, no toolkit change), project branch glide, lifted with
PORT_CHEATS_DIR=wt-proj\glide\cheats into work\port-glide, exe work\port\build-glide (not run).

## Mouse camera, direct mode (2026-11-11, inf2-cheats mouse agent; GitHub issue #5)

A player: "the mouse feels slow when aiming; it seems the game is emulating the movement of a controller's
analog stick". It was: cellPad turned mouse speed into right-stick deflection (capped at full deflection) and
the game then applied its own stick response. Direct mode (PS3_MOUSE_MODE=direct, the default; stick = the
old path) turns the camera by how far the mouse moved. Everything below was read in the EBOOT (IDA 9.3 batch
database + capstone, scripts in work\mouse) and checked in two diagnosis runs with x86 debug registers on the
running exe (work\mouse\drtrace.py, drsnap.py; runs native\recordings\mouse-d1, mouse-d2).

How the game turns its camera:
- Pads: array 0x00A73130, 0x690 bytes each (TOC r2-0x7D30 -> 0x009ACC8C, -0x7FCC). The poll 0x001991D8
  (cellPadGetData) normalises each stick (0x00198808: dead zone/gate, outputs) into pad+0x10 right x,
  +0x14 right y, +0x18 its magnitude (+0x04/+0x08/+0x0C the left stick). The 30-sample ring at pad+0x240
  (0x001965A8) is the right stick's magnitude history (the ring-loop fix 0x00196508 reads it).
- D1 (data breakpoints on pad 0 +0x10/+0x14/+0x18, free camera and aiming, still and turning): the only
  reader of the right stick x/y is 0x0019604C (the pad's "camera stick": x|x|, y|y|), reached through pad
  vtable +0x38 = 0x0019609C (the game's invert options G+0x640 x / G+0x641 y, G = 0x00A64E20) or +0x3C =
  0x001960C0 (G+0x642/0x643). Callers of +0x38 with r4 = 1 (right stick): 0x0007E538 (free camera),
  0x00098CAC (aim camera, a loop over both sticks), 0x00097B70/0x00097BE4 (a third camera, not seen running).
- Free camera: 0x00082964 = the camera driver's frame (r3 = driver D, a heap object) -> 0x0007DC2C, then
  0x0007E0A4 (the input step; an IDA function 0x7E0A4-0x800E4 inside the lift's 0x7D46C), then the view
  (0x00081CE0 ...). D2 (execute breakpoints on these entries, the driver copied each frame):
  D+0x1C0 yaw, D+0x1C4 pitch (rad; yaw kept in -pi..pi; stick right = yaw falls, stick up = pitch falls),
  D+0x1D0/0x1D4 their rates; D+0x180/0x184/0x188 = yaw/pitch/distance of the view the game settles on
  (follows 0x1C0; the distance shrinks from 390 to 120 when looking up), D+0x1A8 = 1 while the stick
  turns (0x0007AD94 sets it), D+0x1E0 camera state (4 = player control), D+0x1E8 last player input
  (ticks of 1/1440 s, timer [0x00A2C500]+4). The manual branch (0x7E504..0x7E980; needs the stick off
  centre, threshold 0.0, and the camera volume to allow it): axis snap (stick within 15 deg of an axis
  = that axis only), 0x000898A8 = the game's camera speed option (G+0x648 index into a table: 1.5),
  0x0008D7C8 = target rates (x * speed * -2.5, y * -speed; aim assist; a soft pitch limit), then the rate
  moves towards the target by at most 15 rad/s per second (dt * 15.0), angle += rate * dt, pitch clamped
  to [-1.4137, 1.4137] (0x007442B0 = 0x00744210+0xA0, read-only tuning data), stored at 0x7E97C.
  Measured in D2: yaw rate -3.75 rad/s at full right stick, pitch -1.5 rad/s up; the soft limit stopped
  the stick at pitch -1.3575.
- The free camera's view follows the target (runs mouse-t1/t3/t4, a [mouse] probe line per frame): the solver
  0x00081CE0 (after the input step) moves the camera towards the target at its own speed, ~0.04-0.05 rad a
  frame here; then func_00082964 keeps the target within +-0.2 rad of the new view (0x00082FB8..0x00082FFC:
  wrap, clamp to G2+0x1E0 / G2+0x1F0 = -0.2 / 0.2); when the player stops, the idle branch sets the target back
  to the view (whatever the view has not reached is gone). A stick's steady rate stays under that speed. A whole
  mouse flick written into the target in one frame came out 50% (100 counts) to 3% (1600 counts); writing the
  view block D+0x180 too did nothing (the solver keeps its own state and rewrites it). The axis snap of the
  manual branch (atan2 of the stick: within 15 deg of an axis = that axis only) sets D+0x1E4 / D+0x1E5 for
  the axes the player turned; with D+0x1E5 unset the idle branch levels the pitch back to 0.12 (~0.3 rad/s).
- Aim camera (L1): 0x00098A00 (via 0x006524C8), once a frame while aiming, r3 = aim camera A: A+0x254
  yaw, A+0x258 pitch (stick up = pitch rises), A+0x274/0x278 their rates (at most 1.65 rad/s at full
  stick), pitch limits A+0x150 / A+0x154 = -1.5 / 1.4 (passed to 0x0008D7C8: a spring back past them).
  Per-stick roles at [[A+0x1D0] + 4 * stick] pick the case (0x00098CEC table: 0/1 = manual aim at
  0x00099360 -> 0x0008D7C8 from 0x000993E8, 4 = turn towards a target at 0x00098E34). The aim, Cole's
  aim direction and the view are made from A+0x254/0x258 later in the same call (0x0009912C ->
  0x004EA9AC). The render camera (driver +0x108 and A+0x108, 0x49509800 in D2): +0x29C vertical fov
  0.9 rad free, 0.5 aiming (tan of half: +0x2AC 0.4831 / 0.2553).

The hooks (cheats.cpp, hooks.txt):
| Address | Hook |
|---|---|
| 0x0007E0A4 | take the mouse motion (cellPad_host_mouse_direct_take; nothing in stick mode, controller-only input or a menu) into an owed turn (yaw -dx, pitch +dy, x 0.0008 x sens); run the step; if its manual branch ran (0x0008D7C8 seen from 0x7E714, D+0x1A8 == 1) put as much of the owed turn into the target D+0x1C0/0x1C4 as keeps it within 0.19 rad of the view D+0x180/0x184 (pitch held in the limits at 0x007442B0); keep the manual branch (epsilon stick) while some is owed and 45 frames after, so the view catches up; drop it when there is no player control (no manual branch) |
| 0x00098A00 | take the motion; if it moved and the aim's manual case ran in its previous frame: A+0x254 -= dx * k, A+0x258 -= dy * k (k = 0.0008 * 0.2553/0.4831 * aim sens: the crosshair crosses the screen as fast as the free view at the same setting; held in A+0x150..0x154), before the step (the aim is made inside it: same frame) |
| 0x0008D7C8 | marks "this camera's manual path ran" (ctx->lr = 0x7E718 free, 0x993EC aim) |
| 0x0019609C | free camera, while the mouse turn is being given and the stick is centred: a 1e-6 long stick pointing where the mouse last moved instead of 0 (call site 0x7E538 only), so the game takes its manual branch as for a stick (state 4, last-input time, D+0x1A8, and its axis snap marks D+0x1E4 yaw / D+0x1E5 pitch as player-turned) -- a turn rate of ~4e-6 rad/s |

Exact when the mouse is not used: without motion every hook only calls the game's code and writes nothing,
so a controller and the keys (I/J/K/L move the PS3 stick) see the game as before. The game's camera speed
option does not scale the mouse (the port's sensitivities do); the game's invert options do apply.
PS3_MOUSE_CAM_HOOK=0: no hooks; PS3_MOUSE_PROBE=1 a [mouse] line per moving camera frame (2: summary only);
PS3_CHEATS_AT=t:mousemode=0|1 switches the mode (tests).

## Saved spots: Save this spot / Go to saved spot (2026-11-11 night, inf2-cheats; static only, NOT run)

Sami: load a save and jump to an exact place (the cathedral spire slow view, x -43695 y -22816 z 7786) to
compare performance there. Everything below was read in the EBOOT (IDA 9.3 batch database copied to
work\teleport, scripts xr.py / dec.py / ids.py / lst.py / bl.py / imm.py / snap.py there); no game run.

How the game places a character (the "one safe way"):
- Script reflection entries are 0x34 bytes: +0 name, +4 parameter descriptor, +8 id, +0x10 kind (1 = command),
  +0x14 the native's function descriptor. TELEPORT (0x0096286C) -> 0x005210C4 -> 0x003145B4 starts an AI skill
  (CTeleportSkill: intro/outro animations, for NPCs), not a placing; SET_POS (0x0095D384) sets 4 bytes of a UI
  item. MATCH_POSITION_AND_ROTATION (0x00956A18) -> 0x00528944 -> **func_004AE348(r3 object, r4 -> 16-byte
  aligned 4x4: rows 0-2 rotation, row 3 position)**:
  1. SetWorldPosition 0x004ABDFC (target in v2): object +0x90 += target - world position (0x004AAF28), the
     difference turned into the parent's frame when +0x64 and a parent +0x1C; then vtable +0x5C (Cole:
     0x00325514 -> +0x98 0x0032771C: world matrix +0xB0 made again, attached children, scene node).
  2. SetWorldRotation 0x004AE158: object +0x80 (quaternion) from the matrix (normalised by 0x004EB51C: scale
     ignored; position untouched); then +0x5C.
  3. if 0x001BDB68 (no +0x58 "off" flag on the object or a parent): vtable +0x60. Cole's (vtable 0x0093E1E0)
     = 0x0015520C: 0x00365060 = the character's teleport reset (0x0032C3D8 animation history; +0x2D0 facing
     matrix and +0x334 facing yaw (0x00364BF8); components +0x520 (0x0028F590) and +0x3B0 (0x0014A2BC, the
     ground probe that holds position copies +0x3D0/+0x410); vtable +0x78 / +0x7C with v2 = 0 = linear and
     angular velocity 0 (0x004AC47C / 0x004AD114 -> physics proxy [+0xF4] +0x10 / +0x20; a proxy marked
     stale (+8) is made again first)) + 0x0035BA80 (the skill manager [+0x378] restarts its skill).
- The same three steps exist as func_004AE434 (position in v2, rotation matrix in r7) and, for the hero,
  0x00154628 (SetWorldPosition + 0x004AE968 rotation + 0x00365060, from a script native 0x0052447C).
- **The game does exactly our job itself**: 0x003DCF88 (mode changes of the UGC test play, callers 0x003DD278 /
  0x003DD40C) copies [0x00A6E9E4]'s world matrix +0xB0 when a test play starts and, when it ends, splits it
  (0x004EB51C) and calls func_004AE434(Cole, position, rotation): Cole back where he stood.
- Why it sticks (and why Stage B's writes did nothing): the physics proxy [Cole+0xF4] (128 bytes, vtable
  0x00955B68; +4 owner, +0x10/+0x20 velocities, +0x40/+0x50 result velocities, +0x60 position, +0x70 rotation)
  is filled each frame by Cole's movement (vtable +0x88 0x00368804 -> skill manager / 0x00367810, which reads
  his world position with 0x004AAF28), then its sync 0x004A63A0 writes Cole +0x90 = proxy +0x60 - origin
  [+0x14]+0x80 and +0x80 = proxy +0x70. So +0x90/+0x80 are the state; +0xB0 (world matrix), +0x1F0/+0x250
  (interpolation history), +0x1560/+0x1C10 (motion block copies), +0x3D0/+0x410 are made from them.
- Cole's object (snapshots cheats7): +0x14 origin (its +0x80 = offset, 0 at the docks), +0x1C parent (0),
  +0x58 off flag (0), +0x64 attached flag (1), +0x70 scale (1.11), +0x80 quaternion (yaw about z), +0x90
  position, +0xB0 world matrix (row 0 = (cos, sin, 0) x 1.11, row 3 = position), +0xF4 proxy, +0x334 facing
  yaw, +0x378 skill manager, +0x1594 = Cole.

What the cheat writes (cheats.cpp "saved spots"):
- Go: from Cole's update hook 0x001578F8 AFTER the game's body (Cole not half-updated; same game thread as
  scripts), 64 bytes on the guest stack 0x80..0xC0 below a frame 0x100 under r1 (free at a call boundary):
  the matrix rows (cos y, sin y, 0, 0), (-sin y, cos y, 0, 0), (0, 0, 1, 0), (x, y, z + 5, 1) -- w = 1 because
  SetWorldPosition adds target - current to all four lanes -- then call_game(func_004AE348, r1 = that frame,
  r2 = TOC 0x009DB730, r3 = Cole, r4 = matrix). Every write to Cole is the game's own code. Upright (yaw only),
  5 cm above the spot (never inside the ground it was saved on).
- Refused (nothing written): Cole held by a parent (+0x64 and +0x1C), +0x58 set (cutscene/scripted), health 0,
  a different Cole than when pressed, or not served within 2 s (a load in between).
- Fall-through guard (conservative choice: a far spot's ground may still be streaming): for 4 s after the
  jump, if Cole is within 2 m of the spot horizontally and 2.5 m below it, the same call places him again
  (logged). A spot saved in mid-air bounces for those 4 s, then he falls normally.
- Camera: the saved target yaw/pitch of the free camera (D+0x1C0 / +0x1C4) are put into the target with the
  mouse turn's carry (within 0.19 rad of the view per frame, the manual branch kept by the 1e-6 epsilon stick,
  now pointing both ways so both axes count as turned and the pitch is not levelled), recomputed each frame
  from the absolute goal, until the view (D+0x180 / +0x184) is within 0.005 rad, 3 s, or the player turns the
  camera / aims. Works with a controller too (the turn does not need the mouse). Without a saved camera (the
  seeded spots) the camera keeps its angles and follows Cole as it always does.
- Save: read on the menu thread (no hook): world position as 0x004AAF28 computes it (+0x90 + origin +0x80),
  facing = atan2 of +0xB0 row 0, camera angles from the free camera hook's last frame (host copies, < 1 s old;
  none while aiming).
- Unused = the game's code only: Cole's update hook is set only while a Go waits or the guard runs; the free
  camera hook (always on for the mouse) only copies two floats to host memory each frame.

Menu: F2 > Cheats > "Saved spots": Spot (pick), "Go to saved spot (F7)", "Save this spot" (adds "Spot N
(MM-DD HH:MM)" and picks it). F7 (toolkit agent/teleport: PS3_CHEAT_HOTKEY, kept from the controls) does Go
with the menu open or closed. File %APPDATA%\inFAMOUS2-port\spots.ini (PS3_CHEATS_SPOTS=<file>, "" = session
only; with PS3_OVERLAY_INI set = next to it, so test runs never touch Sami's), one spot a line
"name = x y z [facing [camera_yaw camera_pitch]]" + "selected: <name>", read again when it changes on disk.
Seeds (until the first change writes the file): "Cathedral spire" -43695 -22816 7786 (the spire top)
and "Cathedral roof" -42024.77 -24236.1 6180.76 (below it); no facing/camera.
Log lines: "[cheats] spot: ..." (from / to / now, re-placements, camera turn end).
Not checked in a game: everything above (Sami tests). Things to watch: Cole snapping back (would mean
another state holds the position), a camera that does not turn (camera not in player control), a fall
through the city at a far spot (the guard's lines).
