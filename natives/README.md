# natives/ -- native versions of the game's heaviest SPU jobs

The PS3 game runs most of its heavy work as "jobs" on the console's SPU cores. The port translates that
SPU code instruction by instruction, which is exact but slow. The files in `spu/` are the same jobs
written as ordinary C with AVX2, checked bit for bit against the translated code (the job replay,
`tools/jobreplay.ps1`: every output byte of 1,753 recorded jobs is equal).

| File | Job | What it does | Off switch (settings.ini or environment) |
|---|---|---|---|
| `spu/edge223_native.c` | 223, Edge geometry | skinning, cull transform, culling, vertex decompression | `PS3_NATIVE_EDGE=0` (per kernel: `PS3_NATIVE_EDGE_SKIN`, `_XF`, `_CULL`, `_DEC` = 0) |
| `spu/job204_native.c` | 204, render list | builds the draw command blocks of a frame | `PS3RECOMP_NATIVE_J204=0` |
| `spu/job216_bsp_native.c` | 216, BSP | the polygon clipper | `PS3_NATIVE_J220=0` |

`PS3_SPU_NATIVE=0` turns all of them off at once. A build without them: `PORT_SPU_NATIVES=0` before Setup.
A native that meets an input it does not know declines, and the translated job runs instead.

Measured on a PC cut down to 4 cores / 8 threads (the cathedral spire, 2026-11-13): 45 -> 54 fps in the
heaviest view, 57 -> 70 fps in a light one.

`spu/natives.txt` tells the build which translated functions each file may take over. They are for the
game's 1.04 code (EU and US discs with the 1.04 update: the same EBOOT); the build stops on another one.

## Credits and licenses

All three files are ports of [ThiagoFraga761/infamous2-pc](https://github.com/ThiagoFraga761/infamous2-pc)
(`edge_kernels_native.c`, `job220_native.c`, `ec5f_native.c`; MIT, Copyright (c) 2026 the infamous2-recomp
contributors) to this game version, with hand-written AVX2 paths added; his MIT notice is at the top of
each file. `spu/simde-0.8.2/` holds the 68 headers of [SIMDe](https://github.com/simd-everywhere/simde)
0.8.2 that the Edge kernels include (MIT, `spu/simde-0.8.2/COPYING`).
