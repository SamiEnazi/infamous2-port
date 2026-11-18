/*
 * [aa] rsx_fp_apply_alpha_aa / rsx_fp_alpha_aa_lod: the rewritten alpha test
 * compiles (glslang HLSL -> SPIR-V, as the Vulkan backend compiles it) for the
 * decompiler's three program shapes -- one target with the texture array, one
 * target with separate units, an MRT struct -- in each mode, and a program
 * with the mode off stays byte-identical. No GPU, no game.
 *     python -I test_alpha_aa_win.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"
#include "rsx_shader_spirv.h"

static int s_fail, s_pass;
static void ok(const char* what, int cond, const char* hlsl)
{
    if (cond) { s_pass++; printf("[PASS] %s\n", what); return; }
    s_fail++;
    printf("[FAIL] %s\n", what);
    if (hlsl) printf("---- HLSL ----\n%s\n--------------\n", hlsl);
}

static const char kHead[] =
    "struct PSInput {\n"
    "    float4 position : SV_POSITION;\n"
    "    float4 col0 : COLOR0;\n"
    "    float4 tc0 : TEXCOORD0;\n"
    "};\n";
static const char kArr[] =
    "Texture2D    rsx_tex[16] : register(t0);\n"
    "SamplerState rsx_samp[16] : register(s0);\n";
static const char kSep[] =
    "Texture2D   rsx_tex0 : register(t0);\n"
    "TextureCube rsx_tex1 : register(t1);\n"
    "Texture2D   rsx_tex2 : register(t2);\n"
    "SamplerState rsx_samp[16] : register(s0);\n";
static const char kCb[] =
    "cbuffer PSConstants : register(b1) {\n"
    "    float4 fp_constants[2];\n"
    "    float4 fp_alpha;\n"
    "};\n";

static void build(char* out, size_t cap, int shape)
{
    /* shape 0: one target, array; 1: one target, separate units; 2: MRT */
    snprintf(out, cap, "%s%s%s%s", kHead, shape == 1 ? kSep : kArr, kCb,
        shape == 2
        ? "struct PSOutput {\n    float4 t0 : SV_TARGET0;\n    float4 t1 : SV_TARGET1;\n};\n"
          "PSOutput main(PSInput input) {\n"
          "    float4 r[4];\n"
          "    r[0] = rsx_tex[0].Sample(rsx_samp[0], (input.tc0).xy);\n"
          "    r[1] = rsx_tex[3].Sample(rsx_samp[3], float2((input.tc0).x, 1.0 - (input.tc0).y));\n"
          "    r[0].w = r[0].w * input.col0.w;\n"
          "    PSOutput _out;\n"
          "    float4 _o0 = r[0];\n"
          "    _out.t0 = (_o0 == _o0) ? _o0 : (float4)0;\n"
          "    float4 _o1 = r[1];\n"
          "    _out.t1 = (_o1 == _o1) ? _o1 : (float4)0;\n"
          "    return _out;\n}\n"
        : shape == 1
        ? "float4 main(PSInput input) : SV_TARGET {\n"
          "    float4 r[4];\n"
          "    r[0] = rsx_tex0.Sample(rsx_samp[0], (input.tc0).xy);\n"
          "    r[1] = rsx_tex1.Sample(rsx_samp[1], (input.tc0).xyz);\n"
          "    r[2] = rsx_tex2.SampleLevel(rsx_samp[2], (input.tc0).xy, 0.0);\n"
          "    r[0] = r[0] * r[1] + r[2];\n"
          "    float4 _o = r[0];\n"
          "    return (_o == _o) ? _o : (float4)0;\n}\n"
        : "float4 main(PSInput input) : SV_TARGET {\n"
          "    float4 r[4];\n"
          "    r[0] = rsx_tex[0].Sample(rsx_samp[0], (input.tc0).xy);\n"
          "    if (r[0].x < 0.0) discard;\n"
          "    float4 _o = r[0];\n"
          "    return (_o == _o) ? _o : (float4)0;\n}\n");
}

static int compiles(const char* hlsl, const char* what)
{
    static u32 spv[1 << 18];
    u32 n = 0;
    char log[4096] = { 0 };
    const int r = rsx_hlsl_to_spirv(hlsl, RSX_SHADER_STAGE_FRAGMENT, spv, (u32)(sizeof spv / sizeof spv[0]), &n,
                                    log, sizeof log);
    if (r || !n) printf("  %s: glslang: %s\n", what, log);
    return !r && n > 0;
}

int main(void)
{
    static char h[65536], orig[65536];
    static const char* const shape_name[3] = { "array", "separate", "mrt" };
    static const u32 funcs[4] = { 0x0206u /* GEQUAL */, 0x0204u /* GREATER */, 0x0201u /* LESS */, 0x0202u /* EQUAL */ };
    for (int shape = 0; shape < 3; shape++) {
        /* off: byte-identical */
        rsx_fp_alpha_aa_set(0, 0);
        build(h, sizeof h, shape);
        rsx_fp_apply_alpha_test_buffered(h, sizeof h, 0x0206u);
        memcpy(orig, h, sizeof h);
        { char t[64]; snprintf(t, sizeof t, "%s off: no rewrite", shape_name[shape]);
          ok(t, rsx_fp_apply_alpha_aa(h, sizeof h, 0x0206u, 0) == 0 && rsx_fp_alpha_aa_lod(h, sizeof h) == 0 &&
                !strcmp(h, orig), h); }
        { char t[64]; snprintf(t, sizeof t, "%s plain test compiles", shape_name[shape]);
          ok(t, compiles(h, t), h); }
        for (int mode = 1; mode <= 3; mode++) {          /* 1 mip, 2 cov, 3 both */
            for (int f = 0; f < 4; f++) {
                for (int dex = 0; dex < 2; dex++) {
                    rsx_fp_alpha_aa_set(mode & 1, (mode & 2) != 0);
                    build(h, sizeof h, shape);
                    rsx_fp_apply_alpha_test_buffered(h, sizeof h, funcs[f]);
                    const int r = rsx_fp_apply_alpha_aa(h, sizeof h, funcs[f], dex);
                    const int want = (mode & 1) && (f < 2) ? 1 : ((mode & 2) && !dex && f < 3) ? 1 : 0;
                    char t[160];
                    snprintf(t, sizeof t, "%s mode %d func 0x%04X%s: rewrite %d (want %d)", shape_name[shape], mode,
                             funcs[f], dex ? " depth-export" : "", r, want);
                    ok(t, r == want, r < 0 ? h : NULL);
                    if (dex && r > 0) {
                        if (rsx_fp_apply_depth_export(h, sizeof h) < 0) { ok("depth export after it", 0, h); continue; }
                    }
                    const int l = rsx_fp_alpha_aa_lod(h, sizeof h);
                    snprintf(t, sizeof t, "%s mode %d func 0x%04X%s: lod %d, compiles", shape_name[shape], mode,
                             funcs[f], dex ? " depth-export" : "", l);
                    ok(t, l >= 0 && compiles(h, t), h);
                    if (shape == 0 && mode == 3 && f == 0 && !dex) printf("---- sample (array, both, GEQUAL) ----\n%s\n", h);
                }
            }
        }
    }
    /* [aa] PS3RECOMP_VP_PRECISE: a vertex program decompiled both ways compiles, and only the precise one
     * carries NoContraction (OpDecorate 71, decoration 42) */
    {
        static unsigned char uc[64];
        static char vs0[200000], vs1[200000];
        memset(uc, 0, sizeof uc);
        uc[15] = 1;                                   /* one instruction, the end bit (d3 bit 0) */
        rsx_vp_set_precise(0);
        const int r0 = rsx_vp_decompile(uc, 16, vs0, sizeof vs0);
        rsx_vp_set_precise(1);
        const int r1 = rsx_vp_decompile(uc, 16, vs1, sizeof vs1);
        rsx_vp_set_precise(0);
        ok("vp decompiles both ways", r0 >= 0 && r1 >= 0, r1 < 0 ? vs1 : NULL);
        ok("vp precise text differs only where asked", strstr(vs1, "precise float4 r[32]") && !strstr(vs0, "precise"), vs1);
        static u32 spv[1 << 18];
        u32 n0 = 0, n1 = 0; char log[4096];
        const int c0 = rsx_hlsl_to_spirv(vs0, RSX_SHADER_STAGE_VERTEX, spv, 1u << 18, &n0, log, sizeof log);
        int nc0 = 0;
        for (u32 i = 5; i < n0; ) { const u32 op = spv[i] & 0xFFFFu, wc = spv[i] >> 16; if (!wc) break;
            if (op == 71u && wc >= 3 && spv[i + 2] == 42u) nc0++; i += wc; }
        const int c1 = rsx_hlsl_to_spirv(vs1, RSX_SHADER_STAGE_VERTEX, spv, 1u << 18, &n1, log, sizeof log);
        if (c1) printf("  glslang: %s\n", log);
        int nc1 = 0;
        for (u32 i = 5; i < n1; ) { const u32 op = spv[i] & 0xFFFFu, wc = spv[i] >> 16; if (!wc) break;
            if (op == 71u && wc >= 3 && spv[i + 2] == 42u) nc1++; i += wc; }
        printf("  vp NoContraction decorations: plain %d, precise %d\n", nc0, nc1);
        ok("vp plain compiles, no NoContraction", !c0 && nc0 == 0, NULL);
        ok("vp precise compiles with NoContraction", !c1 && nc1 > 0, vs1);
    }
    printf("test_alpha_aa: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
