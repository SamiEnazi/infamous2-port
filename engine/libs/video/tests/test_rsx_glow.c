/*
 * ps3recomp - (fx12) PS3RECOMP_FX_GLOW: the game's glow and blur chain at the
 * guest's size (GitHub issue #12, rsx_draw_engine.c "the glow and blur chain").
 *
 * Driven through rsx_draw_engine_method() like the FIFO walker drives it,
 * against a backend that records what it was asked for. Each frame draws into
 * a 1280x720 target (the lit picture), then into a 320x180 target (on the
 * default list) sampling the 1280x720 one, then into a 160x90 one sampling
 * the 320x180 one, then into a 256x256 target (not on the list) sampling the
 * 1280x720 one. Checks, per mode (live, as the PC menu sets it):
 *   1 (default)  320x180 and 160x90 made through color_target_create_guest,
 *                the 1280x720 and 256x256 ones through color_target_create;
 *                the 320x180 draw samples color_reduce(1280x720 target,
 *                1280, 720) -- asked again every frame (the copy follows the
 *                target) -- the 160x90 draw its kept 320x180 as it is, the
 *                256x256 draw the 1280x720 target as it is;
 *   2            the same targets kept, no color_reduce;
 *   0            nothing kept (the targets made again through
 *                color_target_create), no color_reduce.
 * Run it with PS3RECOMP_ENG_DELTA=0 and =1 (the runner does): the same.
 * Exit 0 = every check passed.
 */
#include "rsx_draw_engine.h"
#include "rsx_primitives.h"
#include "rsx_test_programs.h"
#include "../../include/ps3emu/ps3_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define GUEST_BYTES     (32u << 20)
#define GUEST_LOCAL_EA  (8u << 20)

u8* vm_base;
u32 ppu_vm_size;
volatile u32 g_gcm_xlate_gen = 1;
u32 cellGcmResolveLocated(int local, u32 offset) { return local ? GUEST_LOCAL_EA + offset : offset; }
u32 cellGcmResolveIO(u32 offset) { return offset < GUEST_LOCAL_EA ? offset : 0; }

static int g_failures;
#define CHECK(cond, ...) do { if (cond) { printf("[PASS] " __VA_ARGS__); printf("\n"); } \
                              else { printf("[FAIL] " __VA_ARGS__); printf("   (%s:%d)\n", __FILE__, __LINE__); g_failures++; } } while (0)

/* ---- the recording backend ---------------------------------------------- */
#define MAXH 4096u
static u32 s_next = 1;
static struct { u32 w, h; int guest; int live; } s_tgt[MAXH];   /* colour targets by handle */
static u32 s_view_of[MAXH];                                     /* a view's surface */
static u32 s_reduce_of[MAXH];                                   /* a reduce copy's source */
static u32 s_reduce_calls, s_reduce_w, s_reduce_h, s_reduce_src;
static u32 s_bound_tgt, s_bound_tex0, s_bound_mask;
/* per frame: what each draw drew into and sampled */
static struct { u32 tgt, tex0; } s_draws[16];
static u32 s_ndraws;

static u32 newh(void) { const u32 h = s_next++; if (h >= MAXH) { printf("[FAIL] handle space\n"); exit(2); } return h; }
static int  r_init(void* u, u32 w, u32 h) { (void)u; (void)w; (void)h; return 0; }
static void r_shutdown(void* u) { (void)u; }
static void r_submit(void* u, u32 reason) { (void)u; (void)reason; }
static u32  r_tex_create(void* u, rsx_be_format f, u32 w, u32 h, u32 mips, u32 faces, u32 remap, u32 rf)
{ (void)u; (void)f; (void)w; (void)h; (void)mips; (void)faces; (void)remap; (void)rf; return newh(); }
static void r_tex_upload(void* u, u32 t, u32 face, u32 mip, u32 w, u32 h, const void* src, u32 row, u32 rows)
{ (void)u; (void)t; (void)face; (void)mip; (void)w; (void)h; (void)src; (void)row; (void)rows; }
static void r_release(void* u, u32 hd) { (void)u; if (hd < MAXH) s_tgt[hd].live = 0; }
static u32 make_target(u32 w, u32 h, int guest)
{ const u32 hd = newh(); s_tgt[hd].w = w; s_tgt[hd].h = h; s_tgt[hd].guest = guest; s_tgt[hd].live = 1; return hd; }
static u32  r_color_create(void* u, rsx_be_format f, u32 w, u32 h, const void* seed, u32 row)
{ (void)u; (void)f; (void)seed; (void)row; return make_target(w, h, 0); }
static u32  r_color_create_guest(void* u, rsx_be_format f, u32 w, u32 h, const void* seed, u32 row)
{ (void)u; (void)f; (void)seed; (void)row; return make_target(w, h, 1); }
static u32  r_reduce(void* u, u32 s, u32 w, u32 h)
{
    (void)u;
    s_reduce_calls++; s_reduce_w = w; s_reduce_h = h; s_reduce_src = s;
    for (u32 i = 1; i < s_next; i++) if (s_reduce_of[i] == s) return i;   /* one copy per source, as the backend */
    const u32 hd = newh(); s_reduce_of[hd] = s; return hd;
}
static u32  r_surface_view(void* u, u32 s, u32 remap, u32 f)
{
    (void)u; (void)remap; (void)f;
    for (u32 i = 1; i < s_next; i++) if (s_view_of[i] == s) return i;
    const u32 hd = newh(); s_view_of[hd] = s; return hd;
}
static u32  r_depth_create(void* u, u32 w, u32 h) { (void)u; (void)w; (void)h; return newh(); }
static u32  r_depth_snapshot(void* u, u32 d, u32 w, u32 h) { (void)u; (void)d; (void)w; (void)h; return newh(); }
static u32  r_color_snapshot(void* u, u32 s) { (void)u; (void)s; return newh(); }
static u32  r_depth_rgba8(void* u, u32 d, u32 w, u32 h) { (void)u; (void)d; (void)w; (void)h; return newh(); }
static u32  r_pipeline_create(void* u, const char* vs, const char* ps, const rsx_be_render_state* rs,
                              const rsx_vertex_layout_plan* layout, u32 stride, rsx_be_format f, u32 count)
{ (void)u; (void)vs; (void)ps; (void)rs; (void)layout; (void)stride; (void)f; (void)count; return newh(); }
static void r_bind_targets(void* u, const u32* s, u32 n, u32 d) { (void)u; (void)d; s_bound_tgt = (n && s) ? s[0] : 0u; }
static void r_bind_pipeline(void* u, u32 p) { (void)u; (void)p; }
static void r_bind_vs(void* u, const void* d, u32 n) { (void)u; (void)d; (void)n; }
static void r_bind_ps(void* u, const void* d, u32 n) { (void)u; (void)d; (void)n; }
static void r_bind_vs_range(void* u, const void* rows, u32 first, u32 count, const void* tail, u32 tb, u32 total)
{ (void)u; (void)rows; (void)first; (void)count; (void)tail; (void)tb; (void)total; }
static void r_bind_textures(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 mask)
{ (void)u; (void)s; s_bound_mask = mask; s_bound_tex0 = (mask & 1u) ? t[0] : 0u; }
static void r_bind_vtextures(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 mask) { (void)u; (void)t; (void)s; (void)mask; }
static void r_viewport(void* u, float x, float y, float w, float h) { (void)u; (void)x; (void)y; (void)w; (void)h; }
static void r_scissor(void* u, u32 x, u32 y, u32 w, u32 h) { (void)u; (void)x; (void)y; (void)w; (void)h; }
static void r_stencil(void* u, u32 r) { (void)u; (void)r; }
static void note_draw(void) { if (s_ndraws < 16) { s_draws[s_ndraws].tgt = s_bound_tgt; s_draws[s_ndraws].tex0 = s_bound_tex0; } s_ndraws++; }
static void r_draw(void* u, rsx_topology t, const void* v, u32 nv, u32 stride, const u32* idx, u32 ni)
{ (void)u; (void)t; (void)v; (void)nv; (void)stride; (void)idx; (void)ni; note_draw(); }
static u32  r_vbuf_create(void* u, const void* d, u32 n) { (void)u; (void)d; (void)n; return newh(); }
static void r_draw_vbuf(void* u, rsx_topology t, u32 vb, u32 nv, u32 stride, const u32* idx, u32 ni)
{ (void)u; (void)t; (void)vb; (void)nv; (void)stride; (void)idx; (void)ni; note_draw(); }
static void r_draw_vbuf_ib(void* u, rsx_topology t, u32 vb, u32 nv, u32 stride, u32 ib, u32 ni)
{ (void)u; (void)t; (void)vb; (void)nv; (void)stride; (void)ib; (void)ni; note_draw(); }
static void r_prefetch(void* u, u32 vb) { (void)u; (void)vb; }
static void r_clear_color(void* u, u32 s, const float c[4]) { (void)u; (void)s; (void)c; }
static void r_clear_ds(void* u, u32 d, u32 f, float z, u8 s) { (void)u; (void)d; (void)f; (void)z; (void)s; }
static void r_present(void* u, u32 s) { (void)u; (void)s; }
static void r_readback(void* u, u32 s, u32 x, u32 y, u32 w, u32 h, void* out, u32 pitch)
{ (void)u; (void)s; (void)x; (void)y; if (out) for (u32 r = 0; r < h; r++) memset((u8*)out + (size_t)r * pitch, 0, (size_t)w * 4u); }
static void r_vs_gen(void* u, u32 gen) { (void)u; (void)gen; }

static const rsx_draw_backend g_rec = {
    .init = r_init, .shutdown = r_shutdown, .submit_and_wait = r_submit,
    .texture_create = r_tex_create, .texture_upload = r_tex_upload, .texture_release = r_release,
    .color_target_create = r_color_create, .color_target_release = r_release, .surface_view = r_surface_view,
    .depth_target_create = r_depth_create, .depth_target_release = r_release, .depth_snapshot = r_depth_snapshot,
    .color_snapshot = r_color_snapshot, .depth_snapshot_rgba8 = r_depth_rgba8,
    .color_target_create_guest = r_color_create_guest, .color_reduce = r_reduce,
    .pipeline_create = r_pipeline_create, .pipeline_release = r_release,
    .bind_targets = r_bind_targets, .bind_pipeline = r_bind_pipeline, .bind_vs_constants = r_bind_vs,
    .bind_ps_constants = r_bind_ps, .bind_textures = r_bind_textures, .bind_vertex_textures = r_bind_vtextures,
    .set_viewport = r_viewport, .set_scissor = r_scissor, .set_stencil_ref = r_stencil, .draw = r_draw,
    .clear_color = r_clear_color, .clear_depth_stencil = r_clear_ds, .present = r_present, .readback = r_readback,
    .bind_vs_constants_range = r_bind_vs_range,
    .vbuf_create = r_vbuf_create, .vbuf_release = r_release, .draw_vbuf = r_draw_vbuf,
    .draw_vbuf_ib = r_draw_vbuf_ib, .vbuf_prefetch = r_prefetch, .vs_constants_gen = r_vs_gen,
    .draw_vbuf_ib_again = r_draw_vbuf_ib,
};

/* ---- the stream ----------------------------------------------------------- */
static void m(u32 method, u32 arg) { rsx_draw_engine_method(method, arg); }
static void put_be32(u8* p, u32 w) { p[0] = (u8)(w >> 24); p[1] = (u8)(w >> 16); p[2] = (u8)(w >> 8); p[3] = (u8)w; }
static u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }

#define VTX_MAIN   0x00010000u
#define IDX_MAIN   0x00030000u
#define FP_COL     0x00001000u     /* local: colour out */
#define FP_TEX     0x00001100u     /* local: sample unit 0 */
#define RT_SCENE   0x00040000u     /* local 1280x720 */
#define RT_Q       0x00400000u     /* local 320x180 */
#define RT_E       0x00440000u     /* local 160x90 */
#define RT_W       0x00460000u     /* local 256x256 */
#define ZETA       0x00500000u
#define NVERT 8u

static void surface_wh(u32 off, u32 w, u32 h)
{
    m(0x0194, 0xFEED0000u); m(0x0198, 0xFEED0000u);
    m(0x0200, w << 16); m(0x0204, h << 16);
    m(0x0208, 5u | (2u << 5) | (1u << 8));
    m(0x020C, w * 4u); m(0x022C, 1280u * 4u);
    m(0x0210, off); m(0x0214, ZETA);
    m(0x0220, 1u);
    m(0x0A00, w << 16); m(0x0A04, h << 16);
    m(0x0A20, f2u(w / 2.0f)); m(0x0A24, f2u(h / 2.0f)); m(0x0A28, f2u(0.5f)); m(0x0A2C, 0);
    m(0x0A30, f2u(w / 2.0f)); m(0x0A34, f2u(-(h / 2.0f))); m(0x0A38, f2u(0.5f)); m(0x0A3C, 0);
}
static void tex0_rt(u32 off, u32 w, u32 h)
{
    m(0x1A00, off);
    m(0x1A04, 1u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));   /* local, 2D, A8R8G8B8 linear, 1 level */
    m(0x1A08, 0x00030303u);
    m(0x1A0C, 0x80000000u);
    m(0x1A10, 0xAAE4u);
    m(0x1A14, (1u << 24) | (1u << 16));                                 /* point, as the bloom's downsample */
    m(0x1A18, (w << 16) | h);
    m(0x1840, w * 4u);
}
static void draw(void)
{
    m(0x1808, RSX_PRIMITIVE_TRIANGLES);
    m(0x1824, 0u | (5u << 24));
    m(0x1808, 0);
}

static void frame(void)
{
    s_ndraws = 0;
    m(0x0A74, 0u); m(0x0A70, 0u);                     /* no depth test, as the chain */
    surface_wh(RT_SCENE, 1280, 720); m(0x1A0C, 0); m(0x08E4, FP_COL | 1u); draw();
    surface_wh(RT_Q, 320, 180); tex0_rt(RT_SCENE, 1280, 720); m(0x08E4, FP_TEX | 1u); draw();
    surface_wh(RT_E, 160, 90); tex0_rt(RT_Q, 320, 180); draw();
    surface_wh(RT_W, 256, 256); tex0_rt(RT_SCENE, 1280, 720); draw();
    rsx_draw_engine_present();
}

static u32 base_of(u32 h)
{
    if (h < MAXH && s_view_of[h]) h = s_view_of[h];
    return h;
}

static void check_mode(const char* mode, int keep, int reduce, int frames)
{
    ps3_setting_apply("PS3RECOMP_FX_GLOW", mode);
    for (int f = 0; f < frames; f++) {
        const u32 calls0 = s_reduce_calls;
        frame();
        CHECK(s_ndraws == 4, "mode %s frame %d: 4 draws (%u)", mode, f, s_ndraws);
        if (s_ndraws != 4) continue;
        const u32 scene = s_draws[0].tgt, q = s_draws[1].tgt, e = s_draws[2].tgt, w = s_draws[3].tgt;
        CHECK(s_tgt[scene].w == 1280 && !s_tgt[scene].guest, "mode %s: the 1280x720 target is made the usual way", mode);
        CHECK(s_tgt[q].w == 320 && s_tgt[q].h == 180 && s_tgt[q].guest == keep && s_tgt[q].live,
              "mode %s: the 320x180 target %s", mode, keep ? "kept at the guest's size" : "made the usual way");
        CHECK(s_tgt[e].w == 160 && s_tgt[e].guest == keep, "mode %s: the 160x90 target %s", mode, keep ? "kept" : "usual");
        CHECK(s_tgt[w].w == 256 && !s_tgt[w].guest, "mode %s: the 256x256 target (not on the list) usual", mode);
        const u32 t1 = base_of(s_draws[1].tex0);
        if (reduce) {
            CHECK(s_reduce_calls == calls0 + 1, "mode %s frame %d: one color_reduce (%u)", mode, f, s_reduce_calls - calls0);
            CHECK(s_reduce_src == scene && s_reduce_w == 1280 && s_reduce_h == 720,
                  "mode %s: color_reduce(the 1280x720 target, 1280, 720) (h%u %ux%u)", mode, s_reduce_src, s_reduce_w, s_reduce_h);
            CHECK(t1 < MAXH && s_reduce_of[t1] == scene, "mode %s: the 320x180 draw samples the averaged copy", mode);
        } else {
            CHECK(s_reduce_calls == calls0, "mode %s frame %d: no color_reduce", mode, f);
            CHECK(t1 == scene, "mode %s: the 320x180 draw samples the 1280x720 target itself", mode);
        }
        CHECK(base_of(s_draws[2].tex0) == q, "mode %s: the 160x90 draw samples the 320x180 target itself", mode);
        CHECK(base_of(s_draws[3].tex0) == scene, "mode %s: the 256x256 draw samples the 1280x720 target itself", mode);
    }
}

int main(void)
{
#ifdef _WIN32
    vm_base = (u8*)VirtualAlloc(NULL, GUEST_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    vm_base = (u8*)calloc(1, GUEST_BYTES);
#endif
    if (!vm_base) { printf("[FAIL] guest memory\n"); return 2; }
    ppu_vm_size = GUEST_BYTES;
    rsx_draw_engine_set_backend(&g_rec);
    if (rsx_draw_engine_init(1280, 720) != 0) { printf("[FAIL] engine init\n"); return 2; }

    u8 vp[32];
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 8u; i++) { u32 w; memcpy(&w, vp + i * 4u, 4); m(0x0B80 + (i & 3u) * 4u, w); }
    m(0x1EA0, 0);
    rsx_test_fp_mov_r0_col0(vm_base + GUEST_LOCAL_EA + FP_COL, RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_tex_r0_tc0(vm_base + GUEST_LOCAL_EA + FP_TEX, 0);
    for (u32 i = 0; i < NVERT; i++)
        for (u32 c = 0; c < 4; c++) {
            put_be32(vm_base + VTX_MAIN + i * 16u + c * 4u, f2u((float)((i * 7u + c * 3u) % 17u) / 8.0f - 1.0f));
            put_be32(vm_base + VTX_MAIN + NVERT * 16u + i * 16u + c * 4u, f2u((float)((i + c) % 5u) / 4.0f));
        }
    static const u8 tri2[6] = { 0, 1, 2, 2, 1, 3 };
    for (u32 i = 0; i < 6u; i++) { vm_base[IDX_MAIN + i * 2u] = 0; vm_base[IDX_MAIN + i * 2u + 1u] = tri2[i]; }
    m(0x1740 + 0 * 4u, 2u | (4u << 4) | (16u << 8));
    m(0x1740 + 3 * 4u, 2u | (4u << 4) | (16u << 8));
    m(0x1680 + 0 * 4u, 0x80000000u | VTX_MAIN);
    m(0x1680 + 3 * 4u, 0x80000000u | (VTX_MAIN + NVERT * 16u));
    m(0x181C, IDX_MAIN);
    m(0x1820, 1u | (1u << 4));

    check_mode("1", 1, 1, 3);   /* the default */
    check_mode("2", 1, 0, 2);
    check_mode("0", 0, 0, 2);
    check_mode("1", 1, 1, 2);   /* and back, live */
    rsx_draw_engine_shutdown();
    printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
