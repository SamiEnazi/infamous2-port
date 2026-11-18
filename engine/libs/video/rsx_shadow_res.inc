/* ---- [shadow] sun-shadow maps at a higher resolution (PS3RECOMP_SHADOW_RES) --
 * Included by rsx_draw_engine.c, inside the effects-resolution block (before
 * eng_fxres_kq, which asks it first).
 *
 * inFAMOUS 2 draws its sun shadows into a 1024x1024 map: each cascade is a
 * depth-only pass into the zeta at 0x0F2F0000 with a dummy colour target at
 * the SAME address (colour mask 0), and every sun-shadow lookup (the mask
 * 55295a0f6cc1985b, the character masks c70407ba5c029d1c / 25f427a67ef667f7,
 * the terrain passes, ~40 forward materials) samples that map with hardware
 * 2x2 PCF (zfunc GREATER) right after the cascade is drawn. At 1080p and
 * above a shadow texel covers several screen pixels near the camera, so the
 * edges of Cole's shadow, railings and roofs come out as stair steps.
 *
 * RPCS3 gets sharper shadows at 200-300% resolution scale because its scale
 * applies to every render target whose larger side is above "Minimum
 * scalable dimension" (default 16; rsx_utils.h apply_resolution_scale), the
 * shadow maps included. This does the same for the shadow maps alone,
 * independent of the internal resolution:
 *
 *   PS3RECOMP_SHADOW_RES=1|2|4 (live; also 3)  the shadow maps drawn k times
 *       larger on each side: 1 = 1024x1024 as on PS3, 2 = 2048x2048 (the
 *       default), 4 = 4096x4096. "0"/"off" = 1.
 *
 * FIXED (runs sh2-sh4, 2026-11-16): at 2x/4x Cole's shadow first turned to
 * scattered dots. Cause: Edge's "covers no pixel centre" cull on the 1024
 * grid (below); the first matcher missed the shadow viewports (their depth
 * range is not the screen's), the one keyed on scales/offsets hits ~52 a
 * frame and the dots are gone. PS3RECOMP_MESH_TRIM=0 (no culling) gives the
 * same picture for +0.3 ms. Scaling the polygon offset's slope term with the
 * map (PS3RECOMP_SHADOW_BIAS=1) changed nothing visible: off by default.
 *
 * Which targets: a pass whose colour target A sits at the zeta's own address
 * (a depth-only pass: a real colour pass cannot share its depth buffer's
 * memory), with the colour mask off, square, 512..2048 on a side. The first
 * such pass registers its address and size (up to 8, logged once); every
 * later lookup of that target, from any path, gets the shadow scale through
 * eng_fxres_kq. The rest is the effects-resolution machinery, unchanged:
 * the colour target is made larger (eng_surface_get_kq), the depth
 * attachment follows (sink_end_body: clip x tkq), viewport and scissor are
 * scaled (here and in rsx_draw_fast.inc), z->kq records the scale of the
 * last depth write and the readers' crop (eng_zdepth_snapshot_sized) takes
 * the map at that scale. The game's lookups use normalised coordinates and
 * the decompiler's shadow helper reads the map's size with GetDimensions,
 * so its 2x2 PCF and the Lighting page's tent filter (PS3RECOMP_SOFT_SHADOWS)
 * step in the new, smaller texels: the same shaders, byte for byte.
 * The polygon offset needs no change: its constant term is in depth units
 * (the same at any size) and its slope term follows the depth slope per
 * pixel, which shrinks with the texel exactly as the self-shadowing error
 * does. A map the guest reads back through memory goes back to 1x for good.
 *
 * Edge Geometry (the SPU job that skins and culls the characters' meshes,
 * also for the shadow pass) drops every triangle that covers no pixel centre
 * of the viewport it is given -- the map's 1024x1024. Drawn at 2048 those
 * triangles do cover pixels: Cole's shadow came out as dots round the head
 * (run sh2). As the screen's "mesh trimming" fix does (spu_dma.h, edgevp),
 * the job's copy of the map's viewport is scaled with the map
 * (spu_edge_shadow_set); PS3RECOMP_SHADOW_EDGE=0 (also in the toggle file)
 * leaves Edge's viewport as the game gives it (diagnostic).
 *
 * Off (1) costs one load per target lookup; nothing is registered and every
 * path is the old one. */
#define ENG_SHRES_MAX 8u
static volatile int s_shres_q = -1;   /* quarter steps: 4 = as the game, 8 = 2x, 16 = 4x; -1 = not read */
static struct { u32 loc, off, w, h, kq; } s_shres_t[ENG_SHRES_MAX];
static u32 s_shres_n;
static u32 s_shres_block[ENG_SHRES_MAX];   /* (offset ^ location << 31) + 1 of maps put back to 1x for good */
static u32 s_shres_nblock;
static int s_shres_edge = -1;   /* PS3RECOMP_SHADOW_EDGE: Edge's viewport scaled with the map (default 1) */
void spu_edge_shadow_set(float k, u32 side);   /* runtime/spu/spu_channels.c (spu_dma.h) */
extern u32 g_spu_edge_shadow_hits;              /* ... the Edge viewports it scaled so far */
/* Edge's copy of the map's viewport at the map's scale k (quarter steps; 4 = off) */
static void eng_shres_edge(u32 k, u32 side)
{
    if (s_shres_edge < 0) { const char* e = getenv("PS3RECOMP_SHADOW_EDGE"); s_shres_edge = !(e && *e == '0'); }
    const int on = s_shres_edge && k > 4u;
    spu_edge_shadow_set(on ? (float)k / 4.0f : 1.0f, on ? side : 0u);
}
static struct {
    u64 draws, clears;          /* depth writes / depth clears into scaled maps since the last report */
    u32 frames;                 /* presents since the last report */
    u32 warn_shape, warn_unnorm, warn_wb, warn_cap;
} s_shres_st;
#define ENG_SHRES_DEFAULT_Q 8   /* 2x: +0.1 ms of GPU at 1080p on an RTX 2070 Super (runs sh3/sh4) */
#define ENG_SHRES_MAX_SIDE 4096u   /* the backend's largest target side (vk_eng_*_target_create) */

static int eng_shres_parse(const char* v)
{
    if (!v || !*v) return ENG_SHRES_DEFAULT_Q;
    if (!strcmp(v, "off") || !strcmp(v, "OFF")) return 4;
    const double k = atof(v);
    if (!(k > 1.0)) return 4;
    const int q = (int)(k * 4.0 + 0.5);
    return q > 16 ? 16 : q < 4 ? 4 : q;
}
static int eng_shres_apply(const char* v)
{
    const int q = eng_shres_parse(v);
    const int was = __atomic_load_n(&s_shres_q, __ATOMIC_RELAXED);
    __atomic_store_n(&s_shres_q, q, __ATOMIC_RELAXED);
    if (q <= 4 && s_shres_n) {   /* 1x: Edge back to the game's viewport too; the next scale tells it again */
        for (u32 i = 0; i < s_shres_n; i++) s_shres_t[i].kq = 4u;
        eng_shres_edge(4u, 0u);
    }
    if (q != was && was >= 0)
        fprintf(stderr, "[shadow] shadow maps now x%.2g (PS3RECOMP_SHADOW_RES=%s): %u map(s) known, redrawn at the new size "
                        "from their next pass\n", q / 4.0, v && *v ? v : "(default)", s_shres_n);
    s_dl_ev++;   /* (rnd1) PS3RECOMP_ENG_DELTA: the next draw looks its targets up again */
    return 1;
}
static void eng_shres_init(void)
{
    const char* e = getenv("PS3RECOMP_SHADOW_RES");
    const int q = eng_shres_parse(e);
    __atomic_store_n(&s_shres_q, q, __ATOMIC_RELAXED);
    /* the one startup line */
    fprintf(stderr, "[shadow] shadow maps x%.2g (PS3RECOMP_SHADOW_RES=%s%s): depth-only square passes 512..2048 "
                    "are drawn at %s their size; 1 = as on PS3\n", q / 4.0, e && *e ? e : "unset",
            e && *e ? "" : ", the default", q > 4 ? "this times" : "exactly");
}
static inline u32 eng_shres_id(u32 location, u32 offset) { return (offset ^ (location << 31)) + 1u; }
/* The scale a registered map of w x h gets at quarter steps q: at most the
 * backend's largest side. */
static u32 eng_shres_cap(int q, u32 w, u32 h)
{
    u32 k = (u32)q;
    while (k > 4u && ((k * w + 3u) / 4u > ENG_SHRES_MAX_SIDE || (k * h + 3u) / 4u > ENG_SHRES_MAX_SIDE)) k--;
    if (k != (u32)q && !s_shres_st.warn_cap++)
        fprintf(stderr, "[shadow] WARNING (first occurrence): a %ux%u map at x%.2g would pass %u texels a side: "
                        "drawn at x%.2g\n", w, h, q / 4.0, ENG_SHRES_MAX_SIDE, k / 4.0);
    return k;
}
/* eng_fxres_kq's first question: the shadow scale of a registered map, or 0
 * (not a map, or the feature at 1x). */
static u32 eng_shres_kq(u32 location, u32 offset, u32 w, u32 h)
{
    const int q = __atomic_load_n(&s_shres_q, __ATOMIC_RELAXED);
    if (__builtin_expect(q <= 4 || !s_shres_n, 1)) return 0;
    for (u32 i = 0; i < s_shres_n; i++) {
        if (s_shres_t[i].off != offset || s_shres_t[i].loc != location || s_shres_t[i].w != w || s_shres_t[i].h != h)
            continue;
        const u32 id = eng_shres_id(location, offset);
        for (u32 b = 0; b < s_shres_nblock; b++) if (s_shres_block[b] == id) return 0;
        for (u32 b = 0; b < s_fxres_nblock; b++) if (s_fxres_block[b] == id) return 0;   /* a colour write-back */
        const u32 k = eng_shres_cap(q, w, h);
        if (k != s_shres_t[i].kq) {
            s_shres_t[i].kq = k;
            fprintf(stderr, "[shadow] map %u:0x%08X %ux%u -> drawn at %ux%u (x%.2g)\n", location, offset, w, h,
                    (w * k + 3u) / 4u, (h * k + 3u) / 4u, k / 4.0);
            if (w == h) eng_shres_edge(k, w);   /* Edge culls on the larger map's pixels */
        }
        return k > 4u ? k : 0u;
    }
    return 0;
}
/* eng_current_target_set, before it looks colour target A up: a depth-only
 * square pass registers its address (see the top). Cheap: a few compares
 * per draw, and only while the feature is above 1x. */
static void eng_shres_detect(const rsx_dsp_surface* sf, u32 sel)
{
    if (__builtin_expect(__atomic_load_n(&s_shres_q, __ATOMIC_RELAXED) <= 4, 1)) return;
    if (sf->color_offset[sel] != sf->zeta_offset || sf->color_location[sel] != sf->zeta_location) return;
    const u32 w = sf->clip_w, h = sf->clip_h;
    for (u32 i = 0; i < s_shres_n; i++)
        if (s_shres_t[i].off == sf->zeta_offset && s_shres_t[i].loc == sf->zeta_location) {
            if ((s_shres_t[i].w != w || s_shres_t[i].h != h) && !s_shres_st.warn_shape++)
                fprintf(stderr, "[shadow] WARNING (first occurrence): map %u:0x%08X (%ux%u) bound as %ux%u: "
                                "that shape stays at its own size\n", sf->zeta_location, sf->zeta_offset,
                        s_shres_t[i].w, s_shres_t[i].h, w, h);
            return;
        }
    if (w != h || w < 512u || w > 2048u) return;
    if (rsx_dsp_reg(&g.rsx, M_COLOR_MASK) != 0) return;   /* the dummy colour target is never written */
    if (s_shres_n >= ENG_SHRES_MAX) {
        if (!s_shres_st.warn_shape++)
            fprintf(stderr, "[shadow] WARNING (first occurrence): more than %u shadow maps: 0x%08X stays at its "
                            "own size\n", ENG_SHRES_MAX, sf->zeta_offset);
        return;
    }
    s_shres_t[s_shres_n].loc = sf->zeta_location; s_shres_t[s_shres_n].off = sf->zeta_offset;
    s_shres_t[s_shres_n].w = w; s_shres_t[s_shres_n].h = h; s_shres_t[s_shres_n].kq = 4u;
    s_shres_n++;
    fprintf(stderr, "[shadow] shadow map found: %u:0x%08X %ux%u (depth-only pass, colour target at the zeta's "
                    "address, frame %u)\n", sf->zeta_location, sf->zeta_offset, w, h, g.frames);
}
/* Whether colour target slot s is a registered map drawn above 1x now. */
static int eng_shres_scaled(u32 s)
{
    if (s >= g.n_surfaces || g.surfaces[s].kq <= 4u) return 0;
    for (u32 i = 0; i < s_shres_n; i++)
        if (s_shres_t[i].off == g.surfaces[s].offset && s_shres_t[i].loc == g.surfaces[s].location &&
            s_shres_t[i].w == g.surfaces[s].w && s_shres_t[i].h == g.surfaces[s].h) return 1;
    return 0;
}
/* PS3RECOMP_SHADOW_BIAS=1 (env, live in the toggle file; default 0): the polygon offset's
 * slope term of a caster drawn into a scaled map x the map's scale. The term is per pixel of
 * the map: at 2x the same factor is half the depth step in world terms (test: does it matter
 * for the characters, the casters drawn with the offset). */
static int s_shres_bias = -1;
static float eng_shres_bias_k(u32 tkq, u32 target)
{
    if (__builtin_expect(tkq <= 4u, 1)) return 1.0f;
    if (s_shres_bias < 0) { const char* e = getenv("PS3RECOMP_SHADOW_BIAS"); s_shres_bias = e && *e && *e != '0'; }
    return (s_shres_bias && eng_shres_scaled(target)) ? (float)tkq * 0.25f : 1.0f;
}
/* A depth write into a scaled map (sink_end_body and the fast path). */
static inline void eng_shres_note_draw(u32 tkq, u32 target)
{
    if (__builtin_expect(tkq <= 4u, 1) || !eng_shres_scaled(target)) return;
    s_shres_st.draws++;
}
/* The guest reads a scaled map back through memory: 1x for good. */
static void eng_shres_block_zeta(u32 location, u32 offset)
{
    const u32 id = eng_shres_id(location, offset);
    int known = 0;
    for (u32 i = 0; i < s_shres_n; i++) if (eng_shres_id(s_shres_t[i].loc, s_shres_t[i].off) == id) known = 1;
    if (!known) return;
    for (u32 b = 0; b < s_shres_nblock; b++) if (s_shres_block[b] == id) return;
    if (s_shres_nblock < ENG_SHRES_MAX) s_shres_block[s_shres_nblock++] = id;
    if (!s_shres_st.warn_wb++)
        fprintf(stderr, "[shadow] WARNING (first occurrence): the guest reads shadow map %u:0x%08X back through "
                        "memory: drawn at its own size from now on\n", location, offset);
}
/* A depth-format lookup of a map last written above 1x: unnormalised
 * coordinates count the guest's texels and would read the top-left part
 * (none seen in inFAMOUS 2: the decompiler's PS3_RES_SHADOW_FIX scales them
 * for the internal resolution only). Logged once, with the program. */
static void eng_shres_check_lookup(const eng_zdepth* z, const rsx_dsp_texture* t, u32 unit)
{
    if (__builtin_expect(z->kq <= 4u, 1)) return;
    if ((t->format & RSX_TEX_FMT_UNNORM) && !s_shres_st.warn_unnorm++)
        fprintf(stderr, "[shadow] WARNING (first occurrence): unit %u reads scaled map 0x%08X (%ux%u at x%.2g) with "
                        "unnormalised coordinates: that lookup lands in the map's top-left part\n",
                unit, t->offset, t->width, t->height, z->kq / 4.0);
}
/* Once a present: every 600 frames, what went into the scaled maps. */
static void eng_shres_frame(void)
{
    if (__builtin_expect(!s_shres_n, 1)) return;
    const int q = __atomic_load_n(&s_shres_q, __ATOMIC_RELAXED);
    if (q <= 4) { s_shres_st.frames = 0; s_shres_st.draws = s_shres_st.clears = 0; return; }   /* 1x: the old path */
    if (++s_shres_st.frames < 600u) return;
    const double f = (double)s_shres_st.frames;
    fprintf(stderr, "[shadow] %u frames at x%.2g: %.1f caster draws and %.1f depth clears a frame into %u map(s)",
            s_shres_st.frames, q / 4.0, (double)s_shres_st.draws / f, (double)s_shres_st.clears / f, s_shres_n);
    for (u32 i = 0; i < s_shres_n; i++)
        fprintf(stderr, "%s0x%08X %ux%u->%ux%u", i ? ", " : " (", s_shres_t[i].off, s_shres_t[i].w, s_shres_t[i].h,
                (s_shres_t[i].w * s_shres_t[i].kq + 3u) / 4u, (s_shres_t[i].h * s_shres_t[i].kq + 3u) / 4u);
    { static u32 last_hits; const u32 h = __atomic_load_n(&g_spu_edge_shadow_hits, __ATOMIC_RELAXED);
      fprintf(stderr, "), %u Edge viewports scaled with the map\n", h - last_hits); last_hits = h; }
    s_shres_st.frames = 0; s_shres_st.draws = 0; s_shres_st.clears = 0;
}
/* PS3RECOMP_SHADOW_RES: the engine's value for the PC menu (vk_overlay_shadow.inc). */
int rsx_draw_engine_shadow_res_q(void)
{
    const int q = __atomic_load_n(&s_shres_q, __ATOMIC_RELAXED);
    return q < 0 ? eng_shres_parse(getenv("PS3RECOMP_SHADOW_RES")) : q;
}
/* ---- [shadow] end (the rest is marked [shadow] where it is used) ---- */
