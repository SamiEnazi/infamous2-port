# Game mods

The port runs the community's inFAMOUS 2 mods: the ones made for tiffsomniac's
**inFAMOUS Mod Manager** (0.2), as they are. You don't need the Manager or its
Unlocker, and your game files are never changed.

## Using mods

1. Add the mod. Either:
   - in the game, **F2 > Mods > Add a mod from a zip...**: the page lists the
     zip files in your Downloads folder and in the mods folder, newest first,
     each already checked ("Sasha's Black Tar 2.0 by sweetasnz -> mods/Sasha's
     Black Tar", or why it cannot be installed; a zip with no `mod.json` in it
     at all is only counted). **Install** puts it in the
     mods folder; a mod whose folder is there already shows **Replace**, which
     keeps the old one until the new one is in place. **Choose another
     file...** opens the Windows file window (over the game; Alt+Tab if it
     stays behind), and the folder you take a zip from is listed from then
     on. After an install the page says what the mod's own checks find, for
     example "15 textures were made for another version of
     cole_gold_amp.xpps: untick Solid Gold Amp";
   - or unzip it yourself into the `mods` folder next to the game (`Play
     inFAMOUS 2` creates it). Each mod is a folder with a `mod.json` in it,
     for example `mods\Red Jacket\mod.json`. A zip that unpacks into a folder
     of its own name one level deeper (`mods\Red Jacket\Red Jacket\mod.json`)
     works too.
2. **F2 > Mods** lists every mod with its preview picture (if its `mod.json`
   names one: `"image": "red_jacket.png"`; click it for a large view, hover
   for a bigger one), name, version, author and what it changes. From there
   you can:
   - turn each mod on or off,
   - pick its options, if the mod has any (`modular.json`: toggles and
     drop-downs), shown under their group's name. A drop-down group, and a
     toggle group whose options all change the same file (one T-shirt
     texture in seven designs), is one at a time: radio buttons. Other
     toggle groups (black tar on the sword, the amp and the outfit) are
     check boxes,
   - choose which mod wins when two clash: **Use this one instead**, on a mod
     left out by a clash, says first what the others lose ("Climbing Gloves
     and Karmic Logo Tshirt will be left out"),
   - switch every mod off at once with **Use mods**,
   - use **Open the mods folder** and **Look for new mods**.
3. Every change is saved at once, and the page shows at once what the next
   start will do with it: "Used from the next start: changes ...", "Left out
   from the next start: it changes the same files as ...". The game uses the
   changes when it starts. **Restart to apply** appears when they differ from
   what this session uses: it saves, closes the game and starts it again. The
   game comes back at its title screen; choose **Continue** there to go on
   from your last save (progress since that save is lost, as when quitting).
   New mods are on by default, with their default options: every toggle, and
   each drop-down's first option by name, as in the Manager.

The page also says why a mod is not used:

- it was made for inFAMOUS or Festival of Blood,
- its `mod.json` doesn't name the game,
- one of its files is broken (for example, a texture whose header doesn't
  match its name),
- it changes the same file as a mod that comes before it, and the two can't
  be merged,
- it is a merge patch and not every mod it names is used.

When two mods change the same thing, the Manager refuses to turn on the second
one. Here the port first tries to merge them (see **Merging** below); only
when that can't be done is the one that comes first used and the other left
out whole.
First means: the mods you put first with **Use this one instead** (each goes
above every mod it clashes with), then the rest by folder name. "The same thing" means the same
whole file, a texture at the same offset, an overlapping packed-file patch, or
a patch onto a file another mod replaces. If a file cannot be built, only the
mod at fault is left out: each mod on that file is tried alone, and if each
works alone, the last one in that order is left out. The page's view of the
next start uses the clash rule only (nothing is built while you choose); the
start itself builds everything, and a mod whose file could not be built this
time stays shown as not used until it or the mods it failed with change.

### Merging

Two mods that change the same file are both used when the port can merge them:

- **Different packed files, merged by themselves.** An `.xpps` file holds
  packed files. When mods replace the same whole file (or one replaces it and
  another patches packed files in it with `xpps_patches`), each mod's copy is
  compared with the game's, packed file by packed file. If they changed
  different packed files, the game gets its own file with each mod's changed
  packed files put in (as `file_<i>_...` patches would) and the other mods'
  packed-file patches on top. A copy that adds packed files at the end (a
  new power usually does) is fine too: then the others' changed packed files
  go into that copy instead of the game's. Two copies that both add packed
  files, or a texture patch onto such a file, can't be combined. The comparison reads both files once; it is cached in
  `mods\.port_cache\merge` (by the mod file's size and time) for the next
  starts. Log lines start with `[mods] merge:`.
- **A merge patch, for mods that change the same parts.** A third mod can
  carry the file with both mods' changes and say which mods it merges:

  ```
  {"game": "inf2", "name": "Lightning Bolt + Electric Dash patch", "patch_for": ["Lightning Bolt", "Electric Dash"], ...}
  ```

  Each name is a mod's folder or name. The patch is used only while every
  mod it names is used. Then its files replace theirs for the files it has,
  those files are no longer a clash between them (or with the patch), and
  their other files are used as always. With one of them off, the page says
  "a merge patch, used only while ... are used" and the other is used alone;
  with the patch off, they clash as before. The Manager ignores the key.
- **The same packed file, combined.** When two mods change the same packed
  file in different places, the port combines it: the game's copy with both
  mods' changes (a three-way merge). Bytes a mod inserted or removed are
  fine: the copies are matched on 16-byte runs that occur once in both, so
  everything after an insertion moves with it. Changes closer together than
  about 16 bytes count as one place.
- **Conflicts: the same place.** When both change the very same place of a
  packed file (or something that can't be combined: a texture at the same
  offset, a texture patch onto a file the other replaces, two copies that
  both add packed files), turning the second mod on asks **Combine these
  mods?**. **Combine** combines everything as above, and where both change
  the same place the mod you just turned on is used there (a checkbox gives
  those places to the other mod instead); a part that can't be combined uses
  that mod's version. **Turn ... off** keeps the other one; **Later** leaves
  the mod out until you press **Combine** on its row. The choice is saved in
  `mods.ini` in both mods' sections (`win = <other folder>|<file>|<part>`,
  `lose = ...`); **Change** on the mod's **Combined** row changes it.
- **Restart, and taking a mod back out.** Regular mods are built at start,
  so after **Combine** the page offers **Restart now** or **Later** (a mod
  that combines on its own when you turn it on gets the same question). Live
  mods take it at once. Nothing is ever written into the game's or the mods'
  files: every start builds the combined files again from the game's own and
  the mods that are on, so turning a mod off takes its changes back out.
  What each file got from which mod is written to `merge_log.txt` in the
  mods folder at every start (and to the session log, `[mods] merge:`).
  Live mods are never combined byte by byte and never asked about with a
  regular mod: the regular mod's file is used whole and the live mod's
  bytes are written on top in memory wherever its own windows are found
  there (where the regular mod changed those very bytes, they are not).
  Between two live mods, the mod you turned on keeps a part both change.

Downloaded mods are treated as untrusted:

- every patch is bounds-checked, and a packed-file patch that would replace
  part of a file's own tables is refused;
- a texture patch is written only where this game's copy of the file has a
  texture of exactly that size, format and mip count (found as
  xpps-texture-tools' exporter finds them). A mod made from another version of
  a file is refused rather than written beside the texture: the US digital
  game's `cole_gold_amp.xpps` has its textures 0x110 bytes further on than the
  EU disc's, so Sasha's Black Tar's Solid Gold Amp option does not fit the EU
  disc (untick that option and the rest of the mod works);
- a mod file over 512 MB, or a `mod.json`/`modular.json` over 1 MB, is
  refused;
- junctions and symbolic links inside a mod folder are ignored;
- a mod's files are only ever handed to the game for reading;
- a preview picture is shown only when `"image"` is a relative path inside
  the mod's folder (no `..`, absolute path, drive letter, network share,
  `:` stream or device name, through no junction or link) to a PNG or JPEG of
  at most 8 MB and 64 M pixels; it is decoded on its own thread and shrunk to
  512 pixels at most. A picture that fails is simply not shown.

A zip is checked whole before anything is written (`runtime/ppu/ppu_fs_mods_zip.h`):

- every name in it must stay inside the mod's folder: no absolute path, `..`,
  drive letter, `:` stream, device name (`CON`, `NUL`, `COM1`...), name
  ending in a dot or space, or character outside plain ASCII; `\` is read as
  `/` and checked the same; no two entries may be the same file;
- a zip with a program anywhere in it (`exe`, `dll`, `bat`, `cmd`, `ps1`,
  `vbs`, `js`, `msi`, `scr`, `com`, `lnk`, `py`, `jar`, `sys`, ...) is refused
  whole: "contains a program, not a mod" (the inFAMOUS 2 Ability Manager zip,
  for one);
- at most 20,000 entries, 512 MB per file and 2 GB in all; ZIP64, zips split
  into parts, password-protected zips and compression other than stored or
  deflate are refused;
- one `mod.json`, at the top or one folder down, that names inFAMOUS 2;
- only mod files are taken out (`json`, `xpps`, `xpp`, `dds`, `bin`, `png`,
  `jpg`, `jpeg`, `txt`, `md`); others are left out and the page says so;
- every file's CRC is checked as it is unpacked. The mod is unpacked into a
  hidden folder inside the mods folder (`.installing-...`) and only renamed
  into place when all of it is there; nothing outside the mods folder is ever
  written or deleted.

With a text language chosen (F2 > Audio > Language, `PS3_TEXT_LANG`), every
`lang_*.xpps` the game asks for is the chosen language's, so a text mod counts
for the language it replaces: a mod's `lang_italian.xpps` is used while the
text is Italian, and a mod's other `lang_*.xpps` is not.

The choices are saved in `mods.ini` beside the menu's `settings.ini`
(`%APPDATA%\inFAMOUS2-port`). **Use mods** is `PS3_GAME_MODS` in
`settings.ini`. `mods.ini` has a section for each mod that is not at its
defaults, and the folders the zip list also looks in:

```
[Sasha's Black Tar]
pick = default,jbintro      the options picked
priority = 2                put first (Use this one instead): a mod with a priority
                            comes before every mod without one, a higher one first
[Young Kessler]
on = 0                      turned off
[.zips]
folder = D:/Downloads       a folder "Choose another file" took a zip from
```

Older builds read the same file: they skip `priority` and `[.zips]`.

**Restart to apply** goes the way **Quit game** does (the game is asked to
exit, a save being written is waited for), then: started by `Play inFAMOUS 2`
(`tools\play.ps1`, which sets `PS3_LAUNCHER_RESTART=1`), the game ends with
exit code `0x52455354` and the launcher starts it again with the same
settings, in the same session folder; started any other way, the game starts
its own exe again with the command line, environment and working directory it
started with (the environment as it was before the menu's settings were
applied), and the new one waits for the old one to end.

## The mod format (the Manager's)

```
<mod>/mod.json        {"game": "inf2", "name": ..., "author": ..., "version": ...}
                      port only: "patch_for": [mods] (a merge patch, see Merging),
                      "discover_powers": [power numbers], "file_alias": {missing: stand-in}
                      (see New powers), "electric_dash" (see Electric Dash),
                      "karma_patches" (see Karma patches)
<mod>/files/          .xpps files that replace the game's (by file name, any subfolder;
                      a mod with no files/ folder keeps them in the mod folder itself)
<files>/texture_patches/<target>.xpps/[data]<OFFSET>_..._<W>x<H>_<DXTn>_m<mips>.dds
                      a texture written into the target, OFFSET bytes past its data start
                      (the big-endian u32 at +40); the DDS rows are flipped back, as the
                      xpps-texture-tools exporter flips them
<files>/xpps_patches/<target>.xpps/file_<i>_<abs>.bin
                      packed file i of the target replaced whole: its size, the later
                      files' offsets, its chunk's size, later chunks' offsets and the data
                      size are fixed up
<files>/xpps_patches/<target>.xpps/file_<i>_<abs>_offset_<o>.bin
                      these bytes written at offset o inside packed file i
<mod>/modular.json    {"modules": [{"id", "name", "paths": [...]}],
                       "groups":  [{"id", "name", "type": "toggle"|"dropdown",
                                    "options": [{"id", "name", "paths": [...]}]}]}
                      the chosen options' paths (folders of the mod) are the parts used
```

The game's file names are flat (`cole_default.xpps`, `zeke.xpps`), so a file is
matched by its name.

## Live mods (port only)

A mod whose `mod.json` has `"live": true` is switched on and off at once, with
no restart (the Mods page marks it **Instant**). Such a mod may only patch
bytes in place: texture patches and `xpps_patches` with `_offset_` (no whole
files, no whole packed files, nothing that changes a file's size). The
Manager and older ports ignore the key and apply the patches at load as usual.

The game loads its own files; a worker thread
(`runtime/ppu/ppu_fs_mods.h`, `live_*`) diffs each patch against the game's
original file (out of the archives) into windows of at least 32 bytes, finds
them in guest main memory (`0x30000000`-`0x50000000`) and RSX local memory
(`0xC0000000`-`0xD0000000`, where the textures are), and writes the mod's bytes
over them; turned off, it writes the originals back. Writes are bracketed by
`g_vm_host_io_hook`, so the draw engine's write tracking sees the new texture
bytes. Every live mod's windows are made at start (a few seconds, on or off),
then a pass every 4 s (`PS3_GAME_MODS_LIVE_SECS`; ~0.4 s over ~380 MB) also
patches what the game loads later. The status line counts the places changed.
Log lines start `[mods] live:`.

### Karma levels

A live mod can change with Cole's karma level. `karma_levels` names a folder
for any of the seven levels (`Infamous`, `Outlaw`, `Thug`, `Neutral`,
`Guardian`, `Champion`, `Hero`); each folder holds `files/` with patches, as
the mod's own:

```json
{
  "game": "inf2", "name": "Karma Lightning", "live": true,
  "karma_levels": { "Infamous": "levels/infamous", "Hero": "levels/hero" }
}
```

The worker reads the karma level every half second the way the game finds it
(the karma float at `0x00A64E40`, the first of the seven ranges at
`0x00745EC8` that holds it, else Neutral) and puts the level's bytes in when
it changes. A level the mod does not name uses the mod's own `files/`, which
is also what the Manager and older ports apply at load.

### Karma patches

A mod (live or not) can make some of its own bytes depend on Cole's karma
level with `karma_patches`: at the listed levels the place in guest memory
that holds `find` gets `replace`'s bytes, at the other levels it gets
`find`'s bytes back.

```json
"karma_patches": [
  { "levels": ["Thug", "Neutral", "Guardian", "Champion", "Hero"],
    "find":    "1e070000 28000030 1f010000 01040400 00000009 ...",
    "replace": "???????? ???????? ???????? ???????? 00000251 ..." }
]
```

- `find` and `replace` are hex of the same length (16-4096 bytes, a multiple
  of 4; spaces are ignored). `??` in `find` is any byte: use it for bytes the
  game's loader fills in (pointers, native call and class words, which differ
  from the file). `??` in `replace` leaves the byte as it is; a byte
  `replace` sets must be known in `find`.
- `find` must hold 8 known bytes in a row (at a multiple of 4) that `replace`
  leaves alone; they are what is searched for. It must be unique to the mod's
  bytes, or every place that matches is changed.
- While a save is being played (Cole's character object at `0x00A6E9E4` is
  set), the live-mods worker looks for the place in either form in guest main
  memory and RSX local memory (again at most every 5 s while it is not
  found), remembers it and checks the karma level every half second.

Custom Powers uses it for Lightning Bolt's head explosion (Skull Bolt's
headshot call, copied into its bolt's script): below Outlaw the call's
character class check is pointed at a class no character has, so the call is
skipped. Log lines start `[mods] karma_patches:`. The Manager and older ports
ignore the key (the mod's bytes stay as its files have them).

### Black bolts

`"dark_bolts": true` (or a list of karma levels, e.g. `["Infamous", "Outlaw"]`)
draws Cole's electric arcs with the game's own dark (evil) lightning shader
while the mod is on: a black core with the outside colour of the evil bolt
textures. It is the same draw swap as Cheats > Black lightning
(`cheats/cheats.cpp`, `hook_arc_draw`), switched by the mod. Additive
glows cannot be black (black adds no light), so a mod made for black
bolts should give the glows' cores the outside colour instead.

## New powers (port only)

A mod that adds a power (a new slot in the Powers menu) can list it in
`discover_powers`, so it shows as found in any save, not only in a new game:

```json
{ "game": "inf2", "name": "My Power", "version": "1.0", "discover_powers": [0, 9] }
```

A power is found at a story moment, which a save made before the mod never
reaches again, so the menu would show it as "Undiscovered power". While the
mod is on and a save is being played (Cole's character object at
`0x00A6E9E4` is set), the live-mods worker checks every listed power's word
(`0x00A64E60` + 4 x power: 1 found, 2 its requirement met, 4 bought, 0x20 new)
every few seconds and gives one without the found bit `0x23`: found, ready to
buy and marked new. The game saves it with the rest, so the power stays found
if the mod is turned off later. The mod does not need `"live": true`. Log
lines start `[mods] discover_powers:`.
### Electric Dash

`"electric_dash": true` (or an object with settings) turns on the engine side
of a mod that adds a Misc power as power `0x30` (slot kind `0xB`, after
Firebird Strike). The mod itself adds the power with `xpps_patches` (its
definition, icon, menu entry); the engine side is in `cheats/cheats.cpp`
(`ed_*`) and runs while that power is equipped and its button (R2) is held,
Cole has energy and is not climbing:

```json
{
  "game": "inf2", "name": "Electric Dash",
  "electric_dash": { "speed": 2, "anim": 1.3, "drain": 15, "glow": true, "glowskip": 11000 }
}
```

- `speed` (1-20, default 2): Cole moves that many times as far each frame, on
  the ground and in the air (through the game's own SetWorldPosition and the
  jump/fall controller's air speed). Jumps on the move keep their take-off
  speed; the landing roll keeps the speed he landed with.
- `anim` (0.1-10, default 1.3): his animations run that much faster on the
  ground (1 leaves the animation update alone).
- `drain` (0-1000, default 15): energy taken each second (the bar is 330);
  none comes back while the button is held, and at 0 the dash stays off until
  the button is let go.
- `glow` (default true): the charged-body look of Overload while dashing; it
  fades out once the button is let go (`glowskip` ticks are skipped, default
  11000). While the game's own Overload runs (Bio Leech, Ionic Drain) it is
  left alone, so that still refills the bar.

The power is marked discovered in any save while the mod is used. The setting
is read once at start, so turning the mod on or off needs a restart. Nothing
is hooked unless a used mod asks for it. `PS3_ED_LOG=1` logs the steps.

The Powers menu plays `movies/ui/powerk_<name>.bik` for the power under the
cursor, where `<name>` is the power's entry in the game's power-name table
(lower case), e.g. `powerk_bolt9.bik` for power 9 and `powerk_r2_8.bik` for
power 0x30. A new power has no such movie, and one failed open leaves the
menu's movie player blank for every power until the game restarts. List a
stand-in in `file_alias` (paths under `USRDIR`; any game file can stand in):

```json
"file_alias": { "movies/ui/powerk_bolt9.bik": "movies/ui/powerk_fatbolt.bik" }
```

While the mod is on, a read of the first path that the game does not have
reads the second instead (a file the game has is never replaced this way).
The first read logs `[mods] movies/ui/powerk_bolt9.bik -> ... (file_alias of <mod>)`.

## How it works

The game reads a file from its PSARC archives when one of them has it, and
otherwise reads the loose file under `USRDIR`. The Unlocker relies on this:
it unpacks the five big archives (`infamous1/2`, `patch1/3/4.psarc_s`) into
loose files and moves the archives away. The port does neither
(`runtime/ppu/ppu_fs_mods.h`):

- **Each changed file is hidden from the archives.** The game looks a file up
  by the MD5 of its upper-case name. When an archive is opened, that entry's
  MD5 in the table of contents is made one larger. This happens in the read
  buffer, never on disk. The table isn't sorted, so no other lookup is
  affected.
- **The game then asks for the loose file** (for example
  `/dev_hdd0/game/BCES01143_INSTALL/USRDIR/cole_default.xpps`). The port
  answers with one of:
  - the mod's own file, or
  - for patches, a build of the original with the patches written in. The
    original is read out of the highest archive that has it, in the order
    patch4, patch3, patch1, infamous2, infamous1 (the order in which the
    Unlocker's copies overwrite each other). Builds are cached in
    `mods\.port_cache` and only rebuilt when something changes.

Builds were compared byte for byte with the Manager's own patching (texture
flips, packed-file resizes and chunk patches) and are identical.

| Variable | |
|---|---|
| `PS3_GAME_MODS_DIR` | the mods folder; unset means no mods (tests, the harness) |
| `PS3_GAME_MODS=0` | use none of them |
| `PS3_GAME_MODS_STATE` | the saved choices (default: `mods.ini` beside `settings.ini`) |
| `PS3_GAME_MODS_CACHE` | where builds go (default: `<mods>/.port_cache`) |
| `PS3_GAME_MODS_ZIP_DIRS` | more folders the zip list looks in (`;`-separated) |
| `PS3_LAUNCHER_RESTART=1` | a launcher answers **Restart to apply** (exit code `0x52455354`) |
| `PS3_GAME_MODS_TEST` | tests: the page's actions as steps, once, `PS3_GAME_MODS_TEST_AT` s (15) after start: `install=<zip>`, `replace=<zip>`, `pick=<folder>=<id,id>`, `prefer=<folder>`, `plan`, `zips`, `restart`, separated by `\|` |
| `PS3_RESTARTED` | set by a restart in the new game (the count); `PS3_RESTART_WAIT_PID` makes it wait for the old one |

Log lines start with `[mods]`: what is used and why something is not, what
was built, what was hidden in which archive, each file served, each install
(`[mods] zip: ...`) and each mod put first. A restart logs `[restart] ...`.

Tests: `runtime/ppu/tests/test_game_mods.cpp` (the zip reader and every
refusal on crafted zips, install and Replace, the clash plan with priorities,
`mods.ini` old and new, the picture path rules and decoding; with a downloads
folder and the game, the real community zips and the version check).

Texture packs (PNG/DDS replacements by content, `PS3_MODS`) are a separate,
port-only feature, also on the Mods page.
