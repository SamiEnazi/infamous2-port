/* inFAMOUS 2: the skeletal-animation job (module 218, jobmod_ab654d30fd90_efe70), natively.
 *
 * Track B2 (work/native/TRACK_B2_ANIM.md). The job: per object instance, decode its animation
 * layers at the current time, blend them, turn the pose into bone matrices and a skinning
 * palette. In fights the instances are characters (263-286 bones, up to 9 layer entries), and the
 * host time of the lifted job (melee1 host samples, 3,675 in the job) goes to:
 *   0x16320 the layer blend (slerp of the rotations, lerp of translation and scale) 34.5%,
 *   0x179C0/0x17F1C op 3 (pose -> local matrices) 8.9%, the track decoders (0x18F08 tree) 16.6%,
 *   0x5814 root 2.8%, 0x18B80 palette 1.9%, 0x17780 pose -> 3x4 1.5%, the rest a flat tail.
 * The rewrites here take over whole loops of the job as plain AVX2 code on the job's data: eight
 * bones per host instruction in structure-of-arrays form (the SPU code does four, through shuffles),
 * no SPU register file, no local-store address arithmetic per access, and every float op the SPU's
 * own (natives/spu/spu_avx2.h: round toward zero, +0, extended range redone exactly). Each lane
 * runs the same operations in the same order as the SPU code, so the bits are the SPU's.
 *
 * The functions (spu_native_register; the lift calls spu_native_entry at their heads, natives.txt):
 *   0x16320  blend two poses: per bone, weight bytes a (r6, 0 = all 255) and b (r8, 0 = all 255), the
 *            layer factor t (r9): s = blend factor, w' = the new weight byte (to r4, often r6 itself);
 *            rotation = slerp(A, B, s)
 *            by the module's acos / sin polynomials, translation and scale = A + s(B - A); a zero
 *            weight keeps the other side. Records of 48 bytes (q, t, s), in place over A (r3 = r5).
 *   0x17780  pose (q, t, s) -> 3x4 rows (r4 -> r3, four bones a group).
 *   0x18B80  palette: the bones' 3x4 rows (r4) times the inverse bind rows (r5) into r3 (48 bytes a bone).
 *   0x13FC0  the job's quad copy (blocks of eight quads, then single quads; r3 = the source end).
 *   0x1D8E8, 0x1DAC0  the module's memmove and memset.
 * Each checks the layout it relies on (in-place arrays, alignment, written ranges meeting nothing
 * else it reads, no wrap of the local store) before it writes anything; otherwise it declines and the
 * lifted function runs.
 *
 * Switches (environment or settings.ini):
 *   PS3_NATIVE_ANIM=0        every rewrite here off (the lifted job). 1 (default) on.
 *   PS3_NATIVE_ANIM=2        check mode: the rewrite runs on a copy of the local store, then the lifted
 *                            function on the real one; the first differing byte is logged.
 *                            PS3_NATIVE_ANIM_CHECK=N checks one call in N (default 1), the others run native.
 *   PS3_NATIVE_ANIM_OFF=17780,18B80   single rewrites off (hex addresses).
 *   PS3_NATIVE_STATS=0       no counters line (default: one line every PS3_NATIVE_STATS_SEC = 10 s with
 *                            calls, declines and kcycles per call of each rewrite).
 * PS3_SPU_NATIVE=0 turns off every native rewrite of every job.
 *
 * Validation: tools/jobreplay.ps1 on native/recordings/{ck-rec2,ck-rec3,spu3-p2} (every output byte
 * and the whole local store at the end of each recorded job equal) and the check mode in game. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_jobrec.h"
#include "spu_avx2.h"

#define ANIM_IMAGE 218

int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
extern unsigned long long ps3_ms_now(void);
typedef void (*a218_lifted_fn)(spu_context*);
extern a218_lifted_fn spu_lookup(uint32_t addr, int image_id);

/* ------------------------------------------------------------------ switches, counters, logs */
enum { K_16320, K_17780, K_18B80, K_13FC0, K_1D8E8, K_1DAC0, K_N };
static const uint32_t s_kaddr[K_N] = { 0x16320u, 0x17780u, 0x18B80u, 0x13FC0u, 0x1D8E8u, 0x1DAC0u };
static const char* const s_kname[K_N] = { "blend 16320", "pose3x4 17780", "palette 18B80", "copy 13FC0", "memmove 1D8E8", "memset 1DAC0" };
static int s_mode = -1, s_stats = 1, s_stats_sec = 10, s_check_every = 1;
static uint8_t s_koff[K_N];
static volatile long long s_calls[K_N], s_decl[K_N], s_items[K_N], s_cyc[K_N], s_chk[K_N], s_bad[K_N];
static volatile long long s_exact[K_N];   /* blocks redone on the exact per-op path (a lane at or above FLT_MAX) */
static volatile unsigned long long s_last_ms;
static volatile long s_warned[32];
static _Thread_local int t_bypass;

static __attribute__((noinline, cold)) void a218_init(void)
{
    const char* e = getenv("PS3_NATIVE_ANIM");
    int m = e && *e ? atoi(e) : 1;
    const char* off = getenv("PS3_NATIVE_ANIM_OFF");
    if (off)
        for (const char* p = off; *p; ) {
            const unsigned long a = strtoul(p, (char**)&p, 16);
            for (int k = 0; k < K_N; k++) if (a == s_kaddr[k]) s_koff[k] = 1;
            while (*p == ',' || *p == ' ' || *p == ';') p++;
            if (*p && !(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f') && !(*p >= 'A' && *p <= 'F')) break;
        }
    const char* st = getenv("PS3_NATIVE_STATS");
    s_stats = !(st && st[0] == '0');
    const char* ce = getenv("PS3_NATIVE_ANIM_CHECK"); if (ce && atoi(ce) > 0) s_check_every = atoi(ce);
    const char* ss = getenv("PS3_NATIVE_STATS_SEC");
    if (ss && atoi(ss) > 0) s_stats_sec = atoi(ss);
    fprintf(stderr, "[native-218] animation job rewrites: %s (blend 0x16320%s, pose 0x17780%s, palette 0x18B80%s)\n",
            m == 0 ? "OFF (PS3_NATIVE_ANIM=0)" : m == 2 ? "CHECK mode (PS3_NATIVE_ANIM=2: native on a copy vs lifted)" : "on",
            s_koff[K_16320] ? " off" : "", s_koff[K_17780] ? " off" : "", s_koff[K_18B80] ? " off" : "");
    __atomic_store_n(&s_mode, m, __ATOMIC_RELEASE);
}
static inline int a218_mode(void)
{
    const int m = __atomic_load_n(&s_mode, __ATOMIC_ACQUIRE);
    if (__builtin_expect(m < 0, 0)) { a218_init(); return __atomic_load_n(&s_mode, __ATOMIC_ACQUIRE); }
    return m;
}
/* one loud line the first time each unexpected thing happens */
static __attribute__((noinline, cold)) void a218_warn_once(int id, const char* fmt, uint32_t a, uint32_t b, uint32_t c)
{
    if (id < 0 || id >= 32 || __atomic_exchange_n(&s_warned[id], 1, __ATOMIC_RELAXED)) return;
    fprintf(stderr, "[native-218] WARNING (first time): ");
    fprintf(stderr, fmt, a, b, c);
    fprintf(stderr, "\n");
}
static __attribute__((noinline, cold)) void a218_stats_print(unsigned long long now)
{
    unsigned long long last = __atomic_load_n(&s_last_ms, __ATOMIC_RELAXED);
    if (!last) { __atomic_compare_exchange_n(&s_last_ms, &last, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED); return; }
    if (now - last < (unsigned long long)s_stats_sec * 1000ull) return;
    if (!__atomic_compare_exchange_n(&s_last_ms, &last, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    char buf[1200]; int n = snprintf(buf, sizeof buf, "[native-218] %llus:", (now - last) / 1000ull);
    for (int k = 0; k < K_N; k++) {
        const long long c = __atomic_exchange_n(&s_calls[k], 0, __ATOMIC_RELAXED);
        const long long d = __atomic_exchange_n(&s_decl[k], 0, __ATOMIC_RELAXED);
        const long long it = __atomic_exchange_n(&s_items[k], 0, __ATOMIC_RELAXED);
        const long long cy = __atomic_exchange_n(&s_cyc[k], 0, __ATOMIC_RELAXED);
        const long long ex = __atomic_exchange_n(&s_exact[k], 0, __ATOMIC_RELAXED);
        if (!c && !d) continue;
        n += snprintf(buf + n, sizeof buf - (size_t)n, " %s %lld calls (%lld declined, %.1f items/call, %.1f kcyc/call, %lld exact blocks)",
                      s_kname[k], c, d, c ? (double)it / (double)c : 0.0, c ? (double)cy / 1000.0 / (double)c : 0.0, ex);
        if (n >= (int)sizeof buf - 8) break;
    }
    if (s_mode == 2) {
        long long chk = 0, bad = 0;
        for (int k = 0; k < K_N; k++) { chk += s_chk[k]; bad += s_bad[k]; }
        n += snprintf(buf + n, sizeof buf - (size_t)n, " | check: %lld compared, %lld differ", chk, bad);
    }
    fprintf(stderr, "%s\n", buf);
}

/* ------------------------------------------------------------------ layout checks */
typedef struct { uint32_t lo, hi; int w; } a218_rng;
/* every range inside the local store; a written range meets no other range */
static int a218_ranges_ok(const a218_rng* r, int n)
{
    for (int i = 0; i < n; i++) if (r[i].hi > 0x40000u || r[i].lo > r[i].hi) return 0;
    for (int i = 0; i < n; i++)
        if (r[i].w)
            for (int j = 0; j < n; j++)
                if (j != i && r[i].lo < r[j].hi && r[j].lo < r[i].hi && r[i].hi > r[i].lo && r[j].hi > r[j].lo) return 0;
    return 1;
}
/* the stqd's of the callee-saved registers r80 .. r80+n-1 at sp-0x10, sp-0x20, ... */
static inline void a218_save_regs(uint8_t* ls, const u128* g, uint32_t sp, int n)
{
    for (int i = 0; i < n; i++) sa_stq_reg(ls, sp - 0x10u * (uint32_t)(i + 1), &g[80 + i]);
}

/* ------------------------------------------------------------------ 0x16320: blend two poses */
/* Float ops of one block body: EX = 1 the exact per-op form (every result checked and +0),
 * EX = 0 the raw op with the magnitudes gathered in *acc (any lane at or above FLT_MAX: the block is
 * redone with EX = 1). Raw results that reach anything but float arithmetic are flushed (FL). */
#define OPF(EX, raw, ex) ((EX) ? (ex) : sa_acc(acc, (raw)))
SA_INLINE V8 sa_acc(V8* acc, V8 r) { *acc = _mm256_max_epu32(*acc, _mm256_and_si256(r, v8_set1(0x7FFFFFFFu))); return r; }
SA_INLINE __m256 sa_bar(__m256 x) { __asm__("" : "+x"(x)); return x; }
#define FM(a, b)      OPF(EX, v8_i(_mm256_mul_ps(sa_bar(v8_f(a)), v8_f(b))), v8_fm((a), (b)))
#define FA(a, b)      OPF(EX, v8_i(_mm256_add_ps(sa_bar(v8_f(a)), v8_f(b))), v8_fa((a), (b)))
#define FS(a, b)      OPF(EX, v8_i(_mm256_sub_ps(sa_bar(v8_f(a)), v8_f(b))), v8_fs((a), (b)))
#define FMA(a, b, c)  OPF(EX, v8_i(_mm256_fmadd_ps(sa_bar(v8_f(a)), v8_f(b), v8_f(c))), v8_fma((a), (b), (c)))
#define FNMS(a, b, c) OPF(EX, v8_i(_mm256_fnmadd_ps(sa_bar(v8_f(a)), v8_f(b), v8_f(c))), v8_fnms((a), (b), (c)))
#define FL(x)         ((EX) ? (x) : v8_flush(x))

/* the module's polynomials (its constant quads at 0x1E1xx..0x1E2xx, as the SPU code splats them) */
#define C_ONE   0x3F800000u
#define C_MONE  0xBF800000u
#define C_W255  0x3F808081u     /* 256/255: weight byte * 2^-8 * this */
#define C_Q255  0x3F7F0000u     /* back to a byte: w * 255/256 + 2^-9, * 2^8, truncated */
#define C_QRND  0x3B000000u
#define C_LIN   0x3F7FBE77u     /* |cos| above this: linear blend */
static const uint32_t s_acos[4] = { 0x3FC90FDAu, 0x3E5BBFCAu, 0x3DB63A9Eu, 0x3D4D8392u };
static const uint32_t s_sin[4]  = { 0xBE2AAAABu, 0x3C088887u, 0xB9500C03u, 0x3638B94Fu };

/* sin polynomial: ((((S3 x2 + S2) x2 + S1) x2 + S0) x2 + 1) x */
#define SINP(EX, out, x) do { const V8 x2_ = FM((x), (x)); \
        V8 a_ = FMA(v8_set1(s_sin[3]), x2_, v8_set1(s_sin[2])); a_ = FMA(a_, x2_, v8_set1(s_sin[1])); \
        a_ = FMA(a_, x2_, v8_set1(s_sin[0])); a_ = FMA(a_, x2_, v8_set1(C_ONE)); (out) = FM(a_, (x)); } while (0)

typedef struct {
    __m128i q[8], t[8], s[8];    /* the new A records, bone j = lane j */
    uint8_t w[8];                /* the new weight bytes */
} b16320_out;

/* One block of eight bones (two groups of the SPU loop): A and B are 384-byte records (two groups),
 * wa / wb their weight bytes (wb NULL: 255). Returns the magnitudes gathered (EX = 0). */
SA_INLINE V8 b16320(const int EX, const uint8_t* A, const uint8_t* B, const uint8_t* wa, const uint8_t* wb,
                    V8 T, b16320_out* o)
{
    V8 acc_ = v8_zero(); V8* acc = &acc_;
    const V8 byteA = wa ? _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*)wa)) : v8_set1(255);
    const V8 byteB = wb ? _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*)wb)) : v8_set1(255);
    const V8 zA = _mm256_cmpeq_epi32(byteA, v8_zero()), zB = _mm256_cmpeq_epi32(byteB, v8_zero());
    /* blend factor s and the new weight (v13 .. v49 of the SPU graph) */
    const V8 fa = FM(v8_cuflt_small(byteA, 147), v8_set1(C_W255));
    const V8 fb = FM(v8_cuflt_small(byteB, 147), v8_set1(C_W255));
    const V8 m26 = FM(T, fb);
    const V8 m28 = FS(FMA(T, fa, fb), fa);
    const V8 gt = v8_fcgt(fb, fa);
    const V8 num = v8_selw(m26, m28, gt);
    const V8 den = FL(v8_selw(fa, fb, gt));
    /* Both weights zero (a bone neither layer animates: common in partial-body layers, the game's own stats
     * line showed 30-70% of blocks on the exact path from it): den = 0, so 1/den is at the top of the SPU's
     * range (Inf/NaN to x86) and the SPU's s = 0 * that = +0 (num = T * 0 = +0 there). Those lanes' reciprocal
     * chain is not gathered and their s is +0 directly, which is what the exact path computes; their outputs
     * are B and a zero weight byte. */
    const V8 zAB = _mm256_and_si256(zA, zB);
    V8 acc3_ = v8_zero();
    acc = &acc3_;
    const V8 e0 = v8_fi(den, v8_frest(den));
    const V8 rc = FMA(FNMS(den, e0, v8_set1(C_ONE)), e0, e0);
    const V8 s = v8_selw(FM(num, rc), v8_zero(), zAB);
    acc = &acc_;
    acc_ = _mm256_max_epu32(acc_, _mm256_andnot_si256(zAB, acc3_));
    const V8 w = FNMS(fa, s, FMA(fb, s, fa));
    V8 wi = v8_cflts(FMA(w, v8_set1(C_Q255), v8_set1(C_QRND)), 165);
    wi = v8_selw(wi, v8_set1(255), _mm256_cmpgt_epi32(wi, v8_set1(255)));
    wi = v8_selw(wi, v8_zero(), _mm256_cmpgt_epi32(v8_zero(), wi));
    {
        const __m256i pk = _mm256_shuffle_epi8(wi, _mm256_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                                                    0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1));
        const uint32_t lo = (uint32_t)_mm_cvtsi128_si32(_mm256_castsi256_si128(pk));
        const uint32_t hi = (uint32_t)_mm_cvtsi128_si32(_mm256_extracti128_si256(pk, 1));
        memcpy(o->w, &lo, 4); memcpy(o->w + 4, &hi, 4);
    }
    /* translation and scale: A + s B - s A, per bone with its own s (two bones per op) */
    for (int j = 0; j < 4; j++) {
        const int k = j + 4;
        const V8 sj = _mm256_inserti128_si256(_mm256_castsi128_si256(sa_lane4(s, j)), sa_lane4(s, k), 1);
        const V8 zbj = _mm256_inserti128_si256(_mm256_castsi128_si256(sa_lane4(zB, j)), sa_lane4(zB, k), 1);
        const V8 zaj = _mm256_inserti128_si256(_mm256_castsi128_si256(sa_lane4(zA, j)), sa_lane4(zA, k), 1);
        for (int f = 1; f <= 2; f++) {
            const uint32_t rj = 192u * (uint32_t)(j >> 2) + 48u * (uint32_t)(j & 3) + 16u * (uint32_t)f;
            const uint32_t rk = 192u * (uint32_t)(k >> 2) + 48u * (uint32_t)(k & 3) + 16u * (uint32_t)f;
            const V8 va = _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(A + rj)), SA_BSW128)),
                                                  _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(A + rk)), SA_BSW128), 1);
            const V8 vb = _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(B + rj)), SA_BSW128)),
                                                  _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(B + rk)), SA_BSW128), 1);
            V8 r = FL(FNMS(va, sj, FMA(vb, sj, va)));
            r = v8_selw(r, va, zbj);
            r = v8_selw(r, vb, zaj);
            __m128i* dst = f == 1 ? o->t : o->s;
            dst[j] = _mm256_castsi256_si128(r); dst[k] = _mm256_extracti128_si256(r, 1);
        }
    }
    /* rotation: slerp by the module's acos and sin polynomials */
    V8 Ax, Ay, Az, Aw, Bx, By, Bz, Bw;
    {   /* (loaded here, not at the top: fewer live registers across the weight and lerp parts) */
        __m128i qa[8], qb[8];
        for (int j = 0; j < 8; j++) {
            const uint32_t r = 192u * (uint32_t)(j >> 2) + 48u * (uint32_t)(j & 3);
            qa[j] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(A + r)), SA_BSW128);
            qb[j] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(B + r)), SA_BSW128);
        }
        sa_aos_to_soa(qa, &Ax, &Ay, &Az, &Aw);
        sa_aos_to_soa(qb, &Bx, &By, &Bz, &Bw);
    }
    const V8 d = FMA(Aw, Bw, FMA(Az, Bz, FMA(Ay, By, FM(Ax, Bx))));
    const V8 sg = v8_selw(v8_set1(C_ONE), v8_set1(C_MONE), v8_fcgt(v8_zero(), d));
    const V8 ad = FM(d, sg);
    const V8 p0 = FNMS(ad, FNMS(ad, FNMS(ad, FM(ad, v8_set1(s_acos[3])), v8_set1(s_acos[2])), v8_set1(s_acos[1])), v8_set1(s_acos[0]));
    const V8 om = FS(v8_set1(C_ONE), ad);
    const V8 oms = FS(v8_set1(C_ONE), s);
    const V8 lin = v8_fcgt(ad, v8_set1(C_LIN));
    /* theta, the sines and the division only matter where the blend is not linear: in a linear lane
     * (|cos| near 1: equal rotations give 1 - |cos| = 0, rsqrt and 1/sin at the top of the SPU's
     * range, Inf/NaN to x86) the SPU's values are thrown away by the select, so their magnitudes are
     * gathered apart and that lane's are dropped (the lifted code took the exact path for them) */
    V8 acc2_ = v8_zero();
    acc = &acc2_;
    const V8 rq = v8_fi(om, v8_frsqest(om));
    const V8 th = FM(p0, FNMS(rq, ad, rq));
    V8 sin_st, sin_t, sin_1st;
    SINP(EX, sin_st, FM(s, th));
    SINP(EX, sin_t, th);
    sin_t = FL(sin_t);
    const V8 inv = v8_fi(sin_t, v8_frest(sin_t));
    const V8 cb = v8_selw(FM(sin_st, inv), s, lin);
    SINP(EX, sin_1st, FM(oms, th));
    const V8 ca = v8_selw(FM(sin_1st, inv), oms, lin);
    acc = &acc_;
    acc_ = _mm256_max_epu32(acc_, _mm256_andnot_si256(lin, acc2_));
    V8 rx = FL(FMA(FM(Bx, sg), cb, FM(Ax, ca)));
    V8 ry = FL(FMA(FM(By, sg), cb, FM(Ay, ca)));
    V8 rz = FL(FMA(FM(Bz, sg), cb, FM(Az, ca)));
    V8 rw = FL(FMA(FM(Bw, sg), cb, FM(Aw, ca)));
    rx = v8_selw(v8_selw(rx, Ax, zB), Bx, zA);
    ry = v8_selw(v8_selw(ry, Ay, zB), By, zA);
    rz = v8_selw(v8_selw(rz, Az, zB), Bz, zA);
    rw = v8_selw(v8_selw(rw, Aw, zB), Bw, zA);
    sa_soa_to_aos(rx, ry, rz, rw, o->q);
    return acc_;
}

static __attribute__((noinline)) void b16320_exact(const uint8_t* A, const uint8_t* B, const uint8_t* wa, const uint8_t* wb, V8 T, b16320_out* o)
{
    (void)b16320(1, A, B, wa, wb, T, o);
}
static inline void b16320_store(uint8_t* A, uint8_t* wa, const b16320_out* o, int bones)
{
    for (int j = 0; j < bones; j++) {
        uint8_t* r = A + 192u * (uint32_t)(j >> 2) + 48u * (uint32_t)(j & 3);
        _mm_storeu_si128((__m128i*)r, _mm_shuffle_epi8(o->q[j], SA_BSW128));
        _mm_storeu_si128((__m128i*)(r + 16), _mm_shuffle_epi8(o->t[j], SA_BSW128));
        _mm_storeu_si128((__m128i*)(r + 32), _mm_shuffle_epi8(o->s[j], SA_BSW128));
    }
    memcpy(wa, o->w, (size_t)bones);
}
static inline void b16320_run(const uint8_t* A, const uint8_t* B, const uint8_t* wa, const uint8_t* wb, V8 T, b16320_out* o)
{
    const V8 acc = b16320(0, A, B, wa, wb, T, o);
    if (__builtin_expect(_mm256_movemask_epi8(_mm256_cmpgt_epi32(acc, v8_set1(0x7F7FFFFEu))) != 0, 0))
        { __atomic_fetch_add(&s_exact[K_16320], 1, __ATOMIC_RELAXED); b16320_exact(A, B, wa, wb, T, o); }
}

/* r3 = A records (= r5), r6 = A's weight bytes (0: all 255), r4 = where the new weight bytes go (often r6 itself),
 * r7 = B records, r8 = B's weight bytes (0: 255), r9 = the layer factor, r10 = bones; the loop runs (N + 3) / 4 whole
 * groups of four. (The SPU code reads the weights through r6 and merges the new ones into r4's quads.) */
static int k16320(const u128* g, uint8_t* ls, long long* items)
{
    const uint32_t A = g[3]._u32[0], WO = g[4]._u32[0], WA = g[6]._u32[0], B = g[7]._u32[0], WB = g[8]._u32[0];
    const uint32_t N = g[10]._u32[0], SP = g[1]._u32[0];
    if (g[5]._u32[0] != A) {
        a218_warn_once(0, "0x16320 called with r5 0x%X != r3 0x%X (r4 0x%X): lifted", g[5]._u32[0], A, WO);
        return 0;
    }
    uint32_t NG = 0;
    if (N) {
        if (N > 0x10000u) { a218_warn_once(1, "0x16320 bone count 0x%X: lifted%s%s", N, 0, 0); return 0; }
        NG = (N + 3u) >> 2;
        if ((A & 15u) || (B & 15u) || (WO & 3u) || (WA & 3u) || (WB & 3u) || WO == 0) {
            a218_warn_once(2, "0x16320 unaligned or null arrays (A 0x%X B 0x%X weights out 0x%X): lifted", A, B, WO);
            return 0;
        }
        /* written: A, the new weights (r4), the saved registers; read: A, B, the two weight arrays (r4 may be r6) */
        const a218_rng r[6] = { { A, A + 192u * NG, 1 }, { WO, WO + 4u * NG, 1 }, { B, B + 192u * NG, 0 },
                                { WA && WA != WO ? WA : 0, WA && WA != WO ? WA + 4u * NG : 0, 0 },
                                { WB ? WB : 0, WB ? WB + 4u * NG : 0, 0 },
                                { (SP - 0x1E0u) & 0x3FFFFu, ((SP - 0x1E0u) & 0x3FFFFu) + 0x1E0u, 1 } };
        if (!a218_ranges_ok(r, 6)) {
            a218_warn_once(3, "0x16320 arrays overlap or wrap (A 0x%X B 0x%X weights out 0x%X): lifted", A, B, WO);
            return 0;
        }
    }
    a218_save_regs(ls, g, SP, 30);
    if (!N) return 1;
    const V8 T = v8_set1(g[9]._u32[0]);
    b16320_out o;
    uint32_t gi = 0;
    for (; gi + 2 <= NG; gi += 2) {
        uint8_t* pa = ls + A + 192u * gi;
        b16320_run(pa, ls + B + 192u * gi, WA ? ls + WA + 4u * gi : NULL, WB ? ls + WB + 4u * gi : NULL, T, &o);
        b16320_store(pa, ls + WO + 4u * gi, &o, 8);
    }
    if (gi < NG) {   /* the last group alone: its inputs staged beside a dummy group */
        uint8_t sa[384], sb[384], swa[8], swb[8];   /* (weights 255 in the dummy group: no zero divide there) */
        memset(sa + 192, 0, 192); memset(sb + 192, 0, 192); memset(swa + 4, 0xFF, 4); memset(swb + 4, 0xFF, 4);
        memcpy(sa, ls + A + 192u * gi, 192); memcpy(sb, ls + B + 192u * gi, 192);
        if (WA) memcpy(swa, ls + WA + 4u * gi, 4);
        if (WB) memcpy(swb, ls + WB + 4u * gi, 4);
        b16320_run(sa, sb, WA ? swa : NULL, WB ? swb : NULL, T, &o);
        b16320_store(ls + A + 192u * gi, ls + WO + 4u * gi, &o, 4);
    }
    *items += N;
    return 1;
}

/* ------------------------------------------------------------------ 0x17780: pose -> 3x4 rows */
/* Per bone (q, t, s) at P (48 bytes) -> rows at X (48 bytes):
 *   row0 = ((0.5 - y2 - z2) 2sx, (xy - wz) 2sy, (xz + wy) 2sz, tx)
 *   row1 = ((xy + wz) 2sx, (0.5 - x2 - z2) 2sy, (yz - wx) 2sz, ty)
 *   row2 = ((xz - wy) 2sx, (yz + wx) 2sy, (0.5 - x2 - y2) 2sz, tz)
 * with every product and difference in the SPU code's order (one fused op where it fuses). */
SA_INLINE V8 b17780(const int EX, const uint8_t* P, __m128i rows[24])
{
    V8 acc_ = v8_zero(); V8* acc = &acc_;
    __m128i q[8], t[8], s[8];
    for (int j = 0; j < 8; j++) {
        const uint8_t* r = P + 192u * (uint32_t)(j >> 2) + 48u * (uint32_t)(j & 3);
        q[j] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)r), SA_BSW128);
        t[j] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(r + 16)), SA_BSW128);
        s[j] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(r + 32)), SA_BSW128);
    }
    V8 X, Y, Z, W, Tx, Ty, Tz, Tw, Sx, Sy, Sz, Sw;
    sa_aos_to_soa(q, &X, &Y, &Z, &W);
    sa_aos_to_soa(t, &Tx, &Ty, &Tz, &Tw);
    sa_aos_to_soa(s, &Sx, &Sy, &Sz, &Sw);
    (void)Tw; (void)Sw;
    const V8 two = v8_set1(0x40000000u), half = v8_set1(0x3F000000u);
    const V8 sx2 = FM(Sx, two), sy2 = FM(Sy, two), sz2 = FM(Sz, two);
    const V8 xz = FM(X, Z), yz = FM(Y, Z), xy = FM(X, Y);
    const V8 hx = FNMS(X, X, half), hy = FNMS(Y, Y, half);
    const V8 r00 = FL(FM(FNMS(Z, Z, hy), sx2)), r01 = FL(FM(FNMS(W, Z, xy), sy2)), r02 = FL(FM(FMA(W, Y, xz), sz2));
    const V8 r10 = FL(FM(FMA(W, Z, xy), sx2)), r11 = FL(FM(FNMS(Z, Z, hx), sy2)), r12 = FL(FM(FNMS(W, X, yz), sz2));
    const V8 r20 = FL(FM(FNMS(W, Y, xz), sx2)), r21 = FL(FM(FMA(W, X, yz), sy2)), r22 = FL(FM(FNMS(Y, Y, hx), sz2));
    __m128i o0[8], o1[8], o2[8];
    sa_soa_to_aos(r00, r01, r02, Tx, o0);
    sa_soa_to_aos(r10, r11, r12, Ty, o1);
    sa_soa_to_aos(r20, r21, r22, Tz, o2);
    for (int j = 0; j < 8; j++) { rows[3 * j] = o0[j]; rows[3 * j + 1] = o1[j]; rows[3 * j + 2] = o2[j]; }
    return acc_;
}
static __attribute__((noinline)) void b17780_exact(const uint8_t* P, __m128i rows[24]) { (void)b17780(1, P, rows); }
static inline void b17780_run(const uint8_t* P, __m128i rows[24])
{
    const V8 acc = b17780(0, P, rows);
    if (__builtin_expect(_mm256_movemask_epi8(_mm256_cmpgt_epi32(acc, v8_set1(0x7F7FFFFEu))) != 0, 0)) { __atomic_fetch_add(&s_exact[K_17780], 1, __ATOMIC_RELAXED); b17780_exact(P, rows); }
}
/* r3 = X out, r4 = P in (192 bytes a group of four, the same array or apart), r5 = bones */
static int k17780(const u128* g, uint8_t* ls, long long* items)
{
    const uint32_t X = g[3]._u32[0], P = g[4]._u32[0], N = g[5]._u32[0];
    if (!N) return 1;
    if (N > 0x10000u || (X & 15u) || (P & 15u)) { a218_warn_once(4, "0x17780 bones 0x%X X 0x%X P 0x%X: lifted", N, X, P); return 0; }
    const uint32_t NG = (N + 3u) >> 2;
    if (X != P) {
        const a218_rng r[2] = { { X, X + 192u * NG, 1 }, { P, P + 192u * NG, 0 } };
        if (!a218_ranges_ok(r, 2)) { a218_warn_once(5, "0x17780 X 0x%X and P 0x%X overlap (%u groups): lifted", X, P, NG); return 0; }
    } else if (X + 192u * NG > 0x40000u) return 0;
    __m128i rows[24];
    uint32_t gi = 0;
    for (; gi + 2 <= NG; gi += 2) {
        b17780_run(ls + P + 192u * gi, rows);
        uint8_t* o = ls + X + 192u * gi;
        for (int k = 0; k < 24; k++) _mm_storeu_si128((__m128i*)(o + 16 * k), _mm_shuffle_epi8(rows[k], SA_BSW128));
    }
    if (gi < NG) {
        uint8_t sp[384];
        memcpy(sp, ls + P + 192u * gi, 192); memset(sp + 192, 0, 192);
        b17780_run(sp, rows);
        uint8_t* o = ls + X + 192u * gi;
        for (int k = 0; k < 12; k++) _mm_storeu_si128((__m128i*)(o + 16 * k), _mm_shuffle_epi8(rows[k], SA_BSW128));
    }
    *items += N;
    return 1;
}

/* ------------------------------------------------------------------ 0x18B80: the palette */
/* Per bone b (48 bytes each): out = r3, the bone's 3x4 rows X = r4, the inverse bind rows B = r5, r6 bones:
 *   out_i = fma((0,0,0,1), X_i, fma(B2, X_i.z, fma(B1, X_i.y, fm(B0, X_i.x))))   (i = 0, 1, 2)
 * The SPU loop loads bone b+1 before it stores bone b, so out may be X or B itself (in place). */
SA_INLINE V8 b18B80(const int EX, const uint8_t* B, const uint8_t* Xr, __m128i out[6])
{
    V8 acc_ = v8_zero(); V8* acc = &acc_;
#define LD2(p, o) _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)((p) + (o))), SA_BSW128)), \
                                          _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)((p) + 48 + (o))), SA_BSW128), 1)
    const V8 B0 = LD2(B, 0), B1 = LD2(B, 16), B2 = LD2(B, 32);
    const V8 w = _mm256_setr_epi32(0, 0, 0, (int)0x3F800000u, 0, 0, 0, (int)0x3F800000u);
    for (int i = 0; i < 3; i++) {
        const V8 x = LD2(Xr, 16 * i);
        const V8 r = FL(FMA(w, x, FMA(B2, _mm256_shuffle_epi32(x, 0xAA), FMA(B1, _mm256_shuffle_epi32(x, 0x55), FM(B0, _mm256_shuffle_epi32(x, 0x00))))));
        out[i] = _mm256_castsi256_si128(r); out[3 + i] = _mm256_extracti128_si256(r, 1);
    }
#undef LD2
    return acc_;
}
static __attribute__((noinline)) void b18B80_exact(const uint8_t* B, const uint8_t* Xr, __m128i out[6]) { (void)b18B80(1, B, Xr, out); }
static int k18B80(const u128* g, uint8_t* ls, long long* items)
{
    const uint32_t O = g[3]._u32[0], X = g[4]._u32[0], B = g[5]._u32[0], N = g[6]._u32[0];
    if (!N) return 1;
    if (N > 0x10000u || ((O | X | B) & 15u)) { a218_warn_once(7, "0x18B80 bones 0x%X out 0x%X X 0x%X: lifted", N, O, X); return 0; }
    const uint32_t L = 48u * N;
    if (O + L > 0x40000u || X + L > 0x40000u || B + L > 0x40000u) return 0;
    /* out may be X or B exactly; any other overlap: lifted */
    if ((O != X && O < X + L && X < O + L) || (O != B && O < B + L && B < O + L)) {
        a218_warn_once(8, "0x18B80 out 0x%X overlaps X 0x%X / B 0x%X: lifted", O, X, B);
        return 0;
    }
    __m128i out[6];
    uint32_t b = 0;
    for (; b + 2 <= N; b += 2) {
        const uint8_t* pb = ls + B + 48u * b;
        const uint8_t* px = ls + X + 48u * b;
        const V8 acc = b18B80(0, pb, px, out);
        if (__builtin_expect(_mm256_movemask_epi8(_mm256_cmpgt_epi32(acc, v8_set1(0x7F7FFFFEu))) != 0, 0)) { __atomic_fetch_add(&s_exact[K_18B80], 1, __ATOMIC_RELAXED); b18B80_exact(pb, px, out); }
        uint8_t* po = ls + O + 48u * b;
        for (int i = 0; i < 6; i++) _mm_storeu_si128((__m128i*)(po + 16 * i), _mm_shuffle_epi8(out[i], SA_BSW128));
    }
    if (b < N) {
        uint8_t sb[96], sx[96];
        memcpy(sb, ls + B + 48u * b, 48); memset(sb + 48, 0, 48);
        memcpy(sx, ls + X + 48u * b, 48); memset(sx + 48, 0, 48);
        const V8 acc = b18B80(0, sb, sx, out);
        if (__builtin_expect(_mm256_movemask_epi8(_mm256_cmpgt_epi32(acc, v8_set1(0x7F7FFFFEu))) != 0, 0)) { __atomic_fetch_add(&s_exact[K_18B80], 1, __ATOMIC_RELAXED); b18B80_exact(sb, sx, out); }
        uint8_t* po = ls + O + 48u * b;
        for (int i = 0; i < 3; i++) _mm_storeu_si128((__m128i*)(po + 16 * i), _mm_shuffle_epi8(out[i], SA_BSW128));
    }
    *items += N;
    return 1;
}

/* ------------------------------------------------------------------ 0x13FC0: quad copy */
/* r3 = dst, r4 = src, r5 = quads: (r5 >> 3) blocks of eight quads (all eight loads before the eight stores),
 * then r5 & 7 single quads; leaves r3 = src past the copy (all four words, as the SPU code computes it).
 * Addresses as the SPU's lqd/stqd take them: the quad, the local store wrapping at 256 KB. */
static int k13FC0(u128* g, uint8_t* ls, long long* items)
{
    const uint32_t D = g[3]._u32[0], S = g[4]._u32[0], Q = g[5]._u32[0];
    const uint32_t nb = Q >> 3, rem = Q & 7u;
    uint32_t d = D, s = S;
    for (uint32_t b = 0; b < nb; b++) {
        __m128i t[8];
        for (int k = 0; k < 8; k++) t[k] = _mm_loadu_si128((const __m128i*)(ls + ((s + 16u * (uint32_t)k) & 0x3FFF0u)));
        for (int k = 0; k < 8; k++) _mm_storeu_si128((__m128i*)(ls + ((d + 16u * (uint32_t)k) & 0x3FFF0u)), t[k]);
        s += 128u; d += 128u;
    }
    for (uint32_t k = 0; k < rem; k++) {
        const __m128i t = _mm_loadu_si128((const __m128i*)(ls + (s & 0x3FFF0u)));
        _mm_storeu_si128((__m128i*)(ls + (d & 0x3FFF0u)), t);
        s += 16u; d += 16u;
    }
    for (int i = 0; i < 4; i++)   /* the remainder loop runs on word 0's count and steps every word */
        g[3]._u32[i] = g[4]._u32[i] + (nb ? ((g[5]._u32[i] >> 3) << 7) : 0u) + 16u * rem;   /* the block loop ran: (r5 >> 3) << 7 per word */
    *items += Q;
    return 1;
}

/* ------------------------------------------------------------------ 0x1D8E8 / 0x1DAC0: memmove, memset */
/* The module's memmove(r3 = dst, r4 = src, r5 = bytes) and memset(r3 = dst, byte 3 of r4, r5 = bytes): whatever the
 * alignment they write exactly the bytes [dst, dst + n) (merged quads at the ends) and leave r3 = dst. */
static int k1D8E8(u128* g, uint8_t* ls, long long* items)
{
    const uint32_t D = g[3]._u32[0], S = g[4]._u32[0], n = g[5]._u32[0];
    if (!n) return 1;
    if (n > 0x40000u || D > 0x40000u - n || S > 0x40000u - n) { a218_warn_once(9, "0x1D8E8 memmove dst 0x%X src 0x%X n 0x%X wraps: lifted", D, S, n); return 0; }
    memmove(ls + D, ls + S, n);
    *items += n;
    return 1;
}
static int k1DAC0(u128* g, uint8_t* ls, long long* items)
{
    const uint32_t D = g[3]._u32[0], n = g[5]._u32[0];
    if (!n) return 1;
    if (n > 0x40000u || D > 0x40000u - n) { a218_warn_once(10, "0x1DAC0 memset dst 0x%X n 0x%X wraps%s: lifted", D, n, 0); return 0; }
    memset(ls + D, (int)(g[4]._u32[0] & 0xFFu), n);
    *items += n;
    return 1;
}

/* ------------------------------------------------------------------ the entry points */
typedef int (*a218_kernel)(u128* g, uint8_t* ls, long long* items);
static int k16320_(u128* g, uint8_t* ls, long long* it) { return k16320(g, ls, it); }
static int k17780_(u128* g, uint8_t* ls, long long* it) { return k17780(g, ls, it); }
static int k18B80_(u128* g, uint8_t* ls, long long* it) { return k18B80(g, ls, it); }
static const a218_kernel s_kfn[K_N] = { k16320_, k17780_, k18B80_, k13FC0, k1D8E8, k1DAC0 };

static __attribute__((noinline, cold)) int a218_check(spu_context* ctx, int k)
{
    /* the rewrite on a copy, then the lifted function on the real state; compare */
    static _Thread_local uint8_t* t_ls;
    if (!t_ls && !(t_ls = (uint8_t*)malloc(SPU_LS_SIZE))) return 0;
    memcpy(t_ls, ctx->ls, SPU_LS_SIZE);
    u128 t_g[128];
    memcpy(t_g, ctx->gpr, sizeof t_g);
    long long it = 0;
    const int ran = s_kfn[k](t_g, t_ls, &it);
    a218_lifted_fn fn = spu_lookup(s_kaddr[k], ctx->resident_ovl);
    if (!fn) return 0;
    const int32_t si = (int32_t)ctx->image_id;
    const uint32_t r0 = ctx->gpr[0]._u32[0];
    t_bypass = 1;
    ctx->host_depth++; fn(ctx); spu_drain_call(ctx, r0); ctx->host_depth--;
    spu_img_restore(ctx, si);
    t_bypass = 0;
    if (ran) {
        __atomic_fetch_add(&s_chk[k], 1, __ATOMIC_RELAXED);
        if (memcmp(&t_g[3], &ctx->gpr[3], 16) && k == K_13FC0) {
            const long long nb = __atomic_add_fetch(&s_bad[k], 1, __ATOMIC_RELAXED);
            if (nb <= 16) fprintf(stderr, "[native-218] CHECK %s: r3 native %08X lifted %08X\n", s_kname[k], t_g[3]._u32[0], ctx->gpr[3]._u32[0]);
        } else if (memcmp(t_ls, ctx->ls, SPU_LS_SIZE)) {
            uint32_t a = 0, lines = 0, first = 0;
            for (a = 0; a < SPU_LS_SIZE; a += 16)
                if (memcmp(t_ls + a, ctx->ls + a, 16)) { if (!lines) first = a; lines++; }
            uint32_t b = first; while (t_ls[b] == ctx->ls[b]) b++;
            const long long nb = __atomic_add_fetch(&s_bad[k], 1, __ATOMIC_RELAXED);
            if (nb <= 16)
                fprintf(stderr, "[native-218] CHECK %s differs: %u line(s), first byte LS 0x%05X native %02X lifted %02X (r3 %05X r4 %05X r7 %05X r8 %05X N %u)\n",
                        s_kname[k], lines, b, t_ls[b], ctx->ls[b], ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
                        ctx->gpr[7]._u32[0], ctx->gpr[8]._u32[0], ctx->gpr[10]._u32[0]);
        }
    }
    ctx->pc = ctx->gpr[0]._u32[0];
    return 1;
}

static inline int a218_entry(spu_context* ctx, int k)
{
    const int m = a218_mode();
    if (!m || t_bypass || s_koff[k]) return 0;
    if (__builtin_expect(m == 2, 0)) {   /* check one call in PS3_NATIVE_ANIM_CHECK (default every call); the others run native */
        static _Thread_local unsigned t_cn;
        if (++t_cn >= (unsigned)s_check_every) {
            t_cn = 0;
            const int r = a218_check(ctx, k);
            static _Thread_local unsigned t_nc;
            if (s_stats && (++t_nc & 255) == 0) a218_stats_print(ps3_ms_now());
            return r;
        }
    }
    const unsigned long long c0 = __rdtsc();
    long long it = 0;
    const int r = s_kfn[k](ctx->gpr, ctx->ls, &it);
    const unsigned long long c1 = __rdtsc();
    if (s_stats) {
        if (r) {
            __atomic_fetch_add(&s_calls[k], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&s_items[k], it, __ATOMIC_RELAXED);
            __atomic_fetch_add(&s_cyc[k], (long long)(c1 - c0), __ATOMIC_RELAXED);
        } else __atomic_fetch_add(&s_decl[k], 1, __ATOMIC_RELAXED);
        static _Thread_local unsigned t_n;
        if ((++t_n & 255) == 0) a218_stats_print(ps3_ms_now());
    }
    return r;
}
static int e16320(spu_context* ctx) { return a218_entry(ctx, K_16320); }
static int e17780(spu_context* ctx) { return a218_entry(ctx, K_17780); }
static int e18B80(spu_context* ctx) { return a218_entry(ctx, K_18B80); }
static int e13FC0(spu_context* ctx) { return a218_entry(ctx, K_13FC0); }
static int e1D8E8(spu_context* ctx) { return a218_entry(ctx, K_1D8E8); }
static int e1DAC0(spu_context* ctx) { return a218_entry(ctx, K_1DAC0); }

__attribute__((constructor)) static void a218_register(void)
{
    spu_native_register(ANIM_IMAGE, 0x16320u, e16320);
    spu_native_register(ANIM_IMAGE, 0x17780u, e17780);
    spu_native_register(ANIM_IMAGE, 0x18B80u, e18B80);
    spu_native_register(ANIM_IMAGE, 0x13FC0u, e13FC0);
    spu_native_register(ANIM_IMAGE, 0x1D8E8u, e1D8E8);
    spu_native_register(ANIM_IMAGE, 0x1DAC0u, e1DAC0);
}
