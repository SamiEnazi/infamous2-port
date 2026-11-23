# Wide and non-16:9 pictures (PS3RECOMP_ASPECT) -- notes

inFAMOUS 2 (EU BCES01143 + 1.04, `PPU-41a7d047c573c78c0307506c9d1b52d15a66e4f5`, the same code as
NPUA80638 1.03 and the other 1.04 discs) draws a 1280x720 picture for a 16:9 screen. There was no
working widescreen patch for it: RPCS3's patch file has an "Extended FOV" (`bef32 0x0096de14`) made for
the demo; in 1.04 that word is the int 4 of a script binding (`END_FORCED_ATTACK`), not a float any
camera reads, so it does nothing useful. This is ours, built on erikomis' Metal-port design
(infamous2-recomp, MIT: `game_patches.cpp` `infamous2_apply_aspect`, `rsx_metal_backend.m`
`PS3_ASPECT`), ported to the Vulkan backend and checked again on this EBOOT.

Rule kept: every address below was read in the EBOOT (capstone over `eboot/EBOOT_upd104.elf`, scripts
in the session's scratchpad `wide/`), with what it is, why it changes and what reads it.

## The switch

| Variable | Values | Read |
|---|---|---|
| `PS3RECOMP_ASPECT` | `16:9` (default = unset: the old path exactly), `auto`, `21:9`, `32:9`, `16:10`, `<w>:<h>`, `<w>x<h>`, a ratio (`2.39`); 6:5 .. 64:9 | once, at start |
| `PS3RECOMP_VK_WINDOW_SIZE` | `<w>x<h>`: the window's client size at start (unset: 1280x720 as before, or the picture's shape at 720 high when it is wide) | at start |
| `PS3_ASPECT_HUD_AFTER` | the title's final composite program(s); the port sets `136d0d1989147bd8` for this game when it widens the cameras | first draw |

`auto` = the shape of the screen in fullscreen (`PS3RECOMP_VK_FULLSCREEN=1`, the primary display's mode,
EnumDisplaySettingsW), else of `PS3RECOMP_VK_WINDOW_SIZE`; a plain windowed start (1280x720) is 16:9.
`PS3_ASPECT` (the Metal port's name) is read when `PS3RECOMP_ASPECT` is unset.
Menu: F2 > Display > Aspect ratio (16:9 / Auto / 21:9 / 32:9 / 16:10), "on restart": the cameras and
every render target are made at start. Any shape within 0.5% of 16:9 (1366x768, 1920x1080) is 16:9.

Hor+: the vertical field of view is the game's, the horizontal one follows the shape: wider at 21:9 and
32:9, a little narrower at 16:10 (90% of 16:9's width: Vert+ there would need a code change in SetFov,
see below, not done).

## The game's half (port `patches/port/0010-aspect-hor-plus-follows-the-toolkit.patch`, game_patches.cpp)

Data only, written after the EBOOT is loaded and before the game runs, after checking the PPU hash and
the five words below; another EBOOT is left alone (and the backend then stays 16:9).

| Address | What it is | Change | Who reads it |
|---|---|---|---|
| `0x0047F5F8` | camera constructor | none | -- |
| `0x0047F7B0` / `0x0047F7B8` | `lwz r11,-0x7F04(r30)` / `lwz r10,-0x7F00(r30)`, r30 = `0x009B72F0` (TOC slot `r2-0x79A0`, r2 = `0x009DB730`) | none | loads the two table entries |
| `0x0047F858` / `0x0047F85C` | `fdivs f13 = (float)*w / (float)*h`; `stfs f13, 0x2A4(r28)` = cam+0x2A4, the camera's aspect | none | -- |
| `0x009AF3EC` | the camera module's width entry, a pointer to `0x009D7218` | repointed to `0x0092FFF0` | only the constructor above: a scan of every D-form access through every TOC-loaded base register, and of lis/addi pairs, finds no other reader |
| `0x009AF3F0` | its height entry, pointer to `0x009D7214` | none (checked) | only the constructor |
| `0x009D7218` / `0x009D7214` | the ints 1280 / 720 | none (checked) | 13 other module tables point at them; 30 more reads in 16 other functions, see below |
| `0x0092FFF0` | unused: between the text segment (ends `0x00928168`) and the data segment (`0x00930000`), zero | `round(720 * aspect)` (1680 at 21:9, 2560 at 32:9, 1152 at 16:10) | the constructor, through the repointed entry |
| `0x0047D820` | SetFov: `tany = tan(fov * 0.5)` -> cam+0x2AC, `tanx = tany * cam+0x2A4` -> cam+0x2A8 | none | -- |

27 functions load a float from +0x2A8 / +0x2AC of some object (not all of them the camera; among them the projection, the culling frustum, the renderer's camera copy
`func_00355290` -> `0x00A948B8`, ...): every reader of the camera's tanx gets the wider one, so the view and
the PPU-side culling widen together. No 16/9 float (`0x3FE38E39`, `0x3FE38E38`), 9/16 (`0x3F100000`) or
16/9 double is anywhere in the EBOOT's segments, so no code rebuilds a 16:9 aspect by itself.

The other readers of the 1280 / 720 ints keep 16:9 (they read their own table entries). Read from the
disassembly (constants and the operations), not traced at run time:
`func_000EB9F0` (integer 1280 / a target size), `func_002B51C8` + `func_002B587C` (1280.0 / 720.0,
halves: screen centre), `func_002D7960`, `func_002D92F0`, `func_002D9818` (w x K / h with 720.0: 2D
layout scale), `func_002FE8F8` (16h / 9w: the letterbox rectangle of a 2D full-screen quad drawn with
the depth test off), `func_00302BC4` (w / h with +-1, 0.5: NDC for 2D), `func_0046486C` (720.0),
`func_0049D390`, `func_0049E820`, `func_006F0AB0` (1280 x obj+0x344 / obj+0x33C: a 2D object's pixel
scale), and plain size reads in `func_0004E190`, `func_0013AD5C`, `func_001C5870`, `func_00373858`.
They are 2D (the HUD, menus, movies) or render-target sizes; the 2D is kept 16:9 below, so they stay
right.

Also set by the port: `PS3_ASPECT_HUD_AFTER=136d0d1989147bd8`, the game's final composite into the
display buffer (erikomis found it; the same program in this port's `[hudlog]` runs hud1..hudg2, drawing
1280x720 into `0x00734000` / `0x003B0000`).

## The SPU side: nothing changes

The Edge geometry job (image 223, `jobmod_d0d15ec83992_ea2a0`) culls in its transform-for-cull
`0xA3C8`: positions through the world matrix (S+0xB0) and the viewport info's 4x4 matrix (S+0xF0),
divided by w, mapped to pixels by S+0x130 (scales) / S+0x140 (offsets), outcodes against the scissor at
S+0xE0. The PPU builds that matrix from the camera, so it is the wide view-projection; the scale /
offset (640, -360 / 640, 360) and the scissor stay in the guest's 1280x720 pixels, which is still the
guest's viewport -- the clip space just holds a wider view. A point at the new screen edge has NDC x = 1,
pixel 1280: inside. Mesh trimming's patch (`PS3RECOMP_MESH_TRIM`, LS 0xA7C0) is independent of this.
No SPU code, data or float path is touched, so the job-replay gate is not affected.

## The renderer's half (toolkit, `[wide]` in the sources)

- `vk_postfx.c`: `ps3_aspect_wanted` (the switch above, decided once, logged as `[aspect]`),
  `ps3_aspect_game_set` / `ps3_aspect_game` (what the title said), `vk_postfx_aspect_k` (the menu).
- `rsx_vulkan_backend.c`:
  - `vk_wide_decide` at init: wide only when asked AND the title reported its cameras AND the draw
    engine runs (guest programs) AND the title's height is the guest's. Else 16:9, never stretched.
  - `[res]` scaling with x apart: `s_res.xn / xd` (= q / 4 at 16:9, q x camera width / (4 x 1280)
    wide), `vk_res_px_x` for every width and x (colour and depth targets, seeds, readback point
    sampling, depth snapshots, the light pass's size, the SSAO output and its `rs.x`, the display image,
    the bounce arena's size), `rsx_fp_set_wpos_scale_xy(sx, s)` (the decompiler divides WPOS and DDX
    per axis). At 720p a wide picture still scales x (1680x720 at 21:9). Targets that do not fit
    1280x720 (shadow cascades) and guest textures keep their own size, as with `[res]`.
  - HUD: `set_hud` is installed only while wide. The draw engine flags the display-sized draws after
    the composite, and those of frames without one (menus, motion comics, movies, loading screens), as
    the HUD (`PS3_ASPECT_HUD_AFTER` logic, `rsx_draw_engine.c`); their viewport and scissor go into the
    target's 16:9 rectangle at one scale (`vk_wide_pin_rect`: y's scale centred across when wider,
    x's scale centred top to bottom when narrower). No stretch.
  - Bars: a frame of overlay alone (`set_hud 3`) gets black beside its 16:9 in the display image
    (`vk_wide_bars`: blits of an 8x8 black image after the frame's blit / tone pass): movies and menus
    are pillarboxed (letterboxed at 16:10).
  - Present: the frame goes into the window by its own shape, centred, the rest cleared black
    (`vk_wide_fit`, `vk_wide_window_black`, both present paths); AA / sharpening / FSR run at the
    fitted size. At 16:9 the whole window, as before.
  - Window: `PS3RECOMP_VK_WINDOW_SIZE`, or the picture's shape at 720 high (shrunk to 90% of the
    screen's width when larger).
- `rsx_draw_engine.c`: the HUD logic also starts for `PS3RECOMP_ASPECT` (it was `PS3_ASPECT` only),
  and logs the composite list it uses.
- `vk_overlay.cpp`: Display > Aspect ratio; the "now" sizes count the wider image.

16:9 (unset) runs none of it: no `set_hud`, x scales exactly as y (the same integer expressions), the
present fills the window as before, the window is 1280x720 unless `PS3RECOMP_VK_WINDOW_SIZE` is set.

## Known limits

- HUD elements the game places from the 3D world (markers over people and objects, if drawn after the
  composite) are pinned with the HUD: near the sides they sit inward of their spot, by
  (x - 640) x (kx - k) pixels (up to ~300 px at the edge at 21:9 1080p). Centre ones are right.
- (Fixed 2026-11-13, PS3RECOMP_WIDE_FILL, below.) Full-screen 2D effects drawn after the composite used to be
  pinned too: the hurt overlay covered the 16:9 middle only, with a hard vertical edge at both sides.
- Full-screen effects drawn with other programs than the two kinds `PS3RECOMP_WIDE_FILL` knows (below) stay
  pinned; `PS3RECOMP_WIDE_LOG=1` lists what is pinned and what fills, once a second.
- A HUD piece that slides in from a side (partly past the screen edge) fills while it is past the edge and
  is pinned once it is inside: a small jump in its last frame of the slide. Not seen in the opening.
- The SSAO keeps the game's half-res job sizes: horizontally coarser by the shape factor.
- 16:10 is hor+, i.e. 90% of 16:9's width; Vert+ would need SetFov (`0x0047D820`) to scale tany.
- With the option at 16:9, a non-16:9 fullscreen still stretches the 16:9 picture (today's
  behaviour); Auto fixes that.
- Every SPU readback of a display target is point-sampled per axis (as `[res]`); at 16:10 and 720p the
  x scale is 0.9 (a few columns are dropped in readbacks; the default GPU light and SSAO read none).

## Checked (2026-11-11, non-PGO build of agent/wide on m17, internal 1080p, city dock from the first save)

Route: a fast.ps1 steps file (the city intro movie, the dock view at anchor+3 / +8, a camera turn at +12),
runs wd1-* and wd2-* in native\recordings, shots shot_mov / shot_a3 / shot_a8 / shot_a12.

| Run | Shape, window | Display image | Dock fps (2-12 s) | RSX drain ms/frame |
|---|---|---|---|---|
| wd2-A | 16:9, 1280x720 | 1920x1080 | 119.7 (clean) | 8.07 |
| wd2-B | 21:9, 1920x823 | 2520x1080 | 109.5 (0.5 cores other) | 8.90 |
| wd1-C | auto from 1440x900 = 16:10 | 1728x1080 | 118.3 (one run) | 8.10 |
| wd2-D | 32:9, 1920x540 | 3840x1080 | 96.2 (contaminated) | 10.02 |

The cost is the wider view's extra geometry on the RSX thread (+0.8 ms/frame at 21:9 here), not the
pixels. Pictures: more pier and river at both sides at 21:9 and 32:9, the same vertical view, nothing
missing at the edges, Cole and props not stretched; the energy meter, minimap (round), call box and
subtitles in the middle 16:9; the placeholder movie pillarboxed (letterboxed at 16:10). The movie's
2-px green edge columns are the same at 16:9 (x = 0 and 1279), not this change.
Not run: fullscreen Auto on a real ultrawide screen, menus / pause / loading screens / in-engine
cutscenes / fights at a wide shape, FSR upscaling of a wide 720p picture, the job-replay gate (no SPU
change).

## Full-screen effects after the composite: PS3RECOMP_WIDE_FILL (2026-11-13, agent dofov)

A YouTube review at 21:9 showed the hurt overlay (red veins and a dark vignette) covering only the middle
16:9 with a hard vertical edge at both sides. What the game draws there (`PS3RECOMP_WIDE_LOG=1`, runs
`dofov-w1` old / `dofov3-w2` / `dofov4-w3` in native/recordings, route work/dofov/wide.steps: the opening, a
hit of 0.8 x max health at w95 through the game's own ApplyHealthDelta (`PS3_CHEATS_AT=95:hurt=0.8`), the
pause menu at w100-104):
- every HUD piece and the hurt overlay use the same UI programs (fp `2921adcb9c915692`, vp `cd700ba66a07de3c`
  in this build's hashes): one textured quad per element, placed by two matrices in the vertex constants
  -- clip = ((pos.x c[256] + pos.y c[257] + pos.z c[258] + c[259]).xyz) . rows c[260..263];
- the overlay is an 8-vertex frame (positions +-874 x +-531, clip +-1.37 x +-1.48: past every edge, the dark
  vignette, multiplied) and 4-vertex blood splats (+-256, placed half off the screen, e.g. clip x -1.58..-0.59);
- the pause menu is a frame of overlay alone (no composite): pillarboxed as before.

The rule (rsx_draw_engine.c, the `PS3_ASPECT_HUD_AFTER` block; default on, `PS3RECOMP_WIDE_FILL=0` = every draw
after the composite pinned, as before). A display draw after the composite fills the wide picture (stretched
across like the 3D frame, `set_hud 2`) instead of being pinned when:
1. it uses the composite's own vertex program (the post passes' screen mesh), or
2. its vertex program is the UI's two-matrix form -- recognised once per program by its decompiled text (the
   eight products `v[0].x * c[256]` ... `+ c[263]`) -- and its position box through those two matrices either
   covers the whole guest screen (|x|, |y| past 0.98 of w) or reaches past the left or right edge (x < -1 or
   > 1). Everything inside the screen (meters, minimap, prompts, subtitles, text) stays pinned.
The position box comes from the vertices read for the draw, or, when they came from the vertex cache, from the
box kept with the cache entry when it was made (`eng_vc_entry.wbox`, only while a wide picture pins a HUD). A UI
quad with neither stays pinned. Draws after a wide frame's composite skip the fast path for repeated draws
(rsx_draw_fast.inc): their decision depends on vertices and constants it does not see.

Seen at 21:9 (720p, shots `h956` / `h965` / `h98` = 0.6 / 1.5 / 3 s after the hit): the dark vignette and the
splats reach the sides of the wide picture, no vertical edge; the splats are 1.31x wider than at 16:9 (the
16:9 overlay stretched across, as the 3D frame is) -- organic shapes, it does not read as stretched. The HUD
(prompt text, reticle) stays where it was; the pause menu keeps its bars. 16:9 runs none of this (`set_hud` is
not installed there).

## Two frames in one present: PS3RECOMP_WIDE_NEXTFRAME (2026-11-22, agent wide; GitHub #47 / #105)

The flicker of #47 (32:9) and #105 (21:9), also seen at 16:10 on the Deck: now and then one frame of gameplay showed
the 3D picture squeezed into the 16:9 rectangle (letterboxed at 16:10, pillarboxed when wider), the HUD in place.
v0.6.0's hysteresis for the bars verdict did not change it, because the bars verdict never flips in those frames
(`[widebars]` logs no change). What happens:
- inFAMOUS 2 flips through the `cellGcmSetFlipCommand` import, so its command stream has no flip word
  (`GCM_FLIP_STATS=1`: 0 FIFO flips, ~30 import flips a second); the frame clock presents a pending flip between
  drains. Now and then a drain runs on past the end of a frame into the next one: one present (`g.frames`) holds two
  of the title's frames (caught on the Deck: 16,438 draws in one present, ~8,000 a frame).
- The HUD logic counted frames by presents only, so the second frame's draws came after the first frame's composite
  with `s_hud_after` still set. Its G-buffer and post passes are display-sized (1280x720 targets), so they were pinned
  as the HUD: each pass squeezed again the pass before it (the frame caught at 16:10: 0.9^4 of the height, centred),
  and the second frame's composite showed that picture. Its 3D vertex programs (world matrix in c[256..259], then
  view-projection in c[260..263]) also went through the UI-quad check as "UI quads" (`[engine] aspect: vertex program
  f8ca6f6e65e1c9aa ... 8e3a9796b3a5dbcb is a UI quad`): the same five programs in #47's v0.6.0 log, where 56 of 74
  seconds pin 3D meshes after the composite. The decompiles are also that frame's hitch.
- More often at a high frame rate (more frames, more chances for the drain to run into the next one); upstream v0.6.3
  has the same logic (frames by presents); the Deck branch's short tail is refused after the composite, so it is not
  involved.

The rule (`rsx_draw_engine.c` `eng_hud_next_frame`, called from the HUD block; default on, `PS3RECOMP_WIDE_NEXTFRAME=0` =
frames by presents only, as before): the HUD is drawn into the composite's own target (the display buffer). A
display-sized draw after the composite into ANOTHER target is the next frame's, so it starts the next slice
(`s_hud_after = 0`): it and the rest of that frame fill the wide picture, as its draws after the present do. A later
draw into the composite's target in the same present takes the break back (the next frame cannot draw into the buffer
still waiting to be shown; that would be this frame's HUD after an off-screen pass, pinned as before). Not seen in
normal frames: 0 such draws in ~6,000 frames of street and standing play before the change. Menus, the pause menu,
movies and loading screens (frames without a composite) are not touched and keep their bars; 16:9 does not run it
(`set_hud` is not installed). `PS3RECOMP_WIDE_LOG=1` prints a `[widenext]` line for each present that held the next
frame (how many of its draws were still pinned) and a 10 s count. A call of its own: `sink_end_body` and `eng_present`
keep their PGO profile (same "changed since the profile" list as HEAD with mg6r).

Checked on the Deck (16:10, Deck preset + `PS3RECOMP_ASPECT=16:10`, street route from zone2/gamedata3 and the spot-2
save, real clock, window recorded with ffmpeg, work/bench/NOTES.md "2026-11-22 aspect flicker"): before, one squeezed
frame in the installed hd6c's recording (with the five 3D programs taken for UI quads at that moment) and one in the
new build's with `PS3RECOMP_WIDE_NEXTFRAME=0` (its `[widenext]` line at the same time: 3,133 draws of the second frame
pinned); after (default), two presents that held two frames in two runs, 0 draws pinned, no squeezed frame, the picture
wide. Pause menu and loading screen keep their bars; 16:9 unchanged (no wide line in the log, same picture). Merges are
rare at the Deck's ~30 fps (about one every 1-2 minutes on the street route); neither a delay at the present nor a busy
loop on the RSX core made them more frequent.
