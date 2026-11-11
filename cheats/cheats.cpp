/*
 * inFAMOUS 2 cheats for the port's in-game menu (F2 or hold L3+R3 -> Cheats).
 *
 * Title knowledge for v1.04 (PPU-41a7d047c573c78c0307506c9d1b52d15a66e4f5): gitignored,
 * never committed. Lives in work\cheats\ and is copied next to game_patches.cpp in the
 * port tree; the port's CMakeLists compiles it when it is there (0008-cheats-source.patch).
 * Needs an executable lifted with
 *     PPU_HOOK_FILE=work\cheats\hooks.txt  PPU_PATCH_FILE=work\cheats\patches.txt
 * (toolkit agent/cheats: tools/ppu_fastpath.py, include/ps3emu/ppu_hooks.h). Without them
 * every cheat shows "needs the cheats lift" and nothing changes.
 *
 * Off = nothing changes: every hook slot empty and every instruction slot 0 (the game's
 * own code runs, one load and a never-taken branch at each place). Cheats start off at
 * every start and are never saved; XP added is written into the save by the game.
 *
 * What the game does (found 2026-11-09; see NOTES.md for how and the community credits):
 *   [0x00A6E9E4]          Cole's character object (null outside play)
 *   character +0x7EC/+0x7F0   max / current health (float); +0x1F68/+0x1F70 energy / max (float)
 *   0x001578F8            Cole's per-frame character update (r3 = character)
 *   0x00363904            SetHealth(r3 character, f1 new health): every health change
 *   0x00363E54            ApplyHealthDelta(r3 character, f1 delta, r5 damage or 0)
 *   0x00364044            TakeDamage(r3 character, r4 damage: +0x7C amount, +0x64 source)
 *   0x00A64E50            XP: available, earned (u32 pair; the save keeps it in its first record)
 *   0x0019BB20            UpdateGravity(r3 jump/fall controller A): A+0x60 gravity; the glide's -350
 *   0x0007E0A4 / 0x00098A00  the free / aim camera's input step (the mouse camera, NOTES.md "Mouse camera")
 *
 * Test knobs (scripted runs; Sami never needs them):
 *   PS3_CHEATS="id=value,..."        at the first Cole update
 *   PS3_CHEATS_AT="t:id=value,..."   t seconds after PS3_CHEATS_AT_WAIT=<file> exists (the fast
 *                                    driver's anchor.flag; unset = after the first Cole update);
 *                                    also t:selftest=1 (Invincibility check through the game's own
 *                                    ApplyHealthDelta, logged)
 *   PS3_CHEATS_PROBE=1               log Cole's numbers every second and the first damage events
 *   PS3_CHEATS_PROBE_XP=<n>          once: list guest words pairs (n, n) (the save's XP)
 *   PS3_CHEATS_XP=<hex>              the XP pair's guest address (overrides the built-in one)
 *   PS3_CHEATS_POKE=<hex>:<n>        with t:poke=1: add n to that u32 pair (the XP search);
 *                                    t:drain=<e> sets Cole's energy to e (tests the refill)
 */
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "ppu_recomp.h"
#include "ps3emu/ppu_hooks.h"
#include "ps3emu/ps3_cheats.h"
#include "ps3emu/ps3_mouse_camera.h"   /* [mouse] the mouse camera's direct mode (cellPad) */

#include <atomic>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <emmintrin.h>   /* _mm_cvttsd_si32: the PPU's fctiwz (the ring fix below) */
#include <string.h>

namespace {

/* ---- inFAMOUS 2 v1.04 --------------------------------------------------------------- */
constexpr uint32_t kPlayerChar = 0x00A6E9E4;
constexpr uint32_t kHpMax = 0x7EC, kHp = 0x7F0, kEnergy = 0x1F68, kEnergyMax = 0x1F70;
constexpr uint32_t kFnColeUpdate = 0x001578F8;
constexpr uint32_t kFnSetHealth = 0x00363904;
constexpr uint32_t kFnTakeDamage = 0x00364044;
constexpr uint32_t kDmgAmount = 0x7C, kDmgSource = 0x64;
constexpr uint32_t kXpDefault = 0x00A64E50; /* XP: two u32 (available, earned), both incremented. G+0x30 of the
                                                progress struct G = 0x00A64E20; the Powers page shows the first.
                                                (G+0x99C holds a snapshot copy of the same record.) */
enum { SLOT_JUMP = 1, SLOT_NPC_SPEED = 2, SLOT_NO_ATTACK = 3 };   /* patches.txt */
/* Karma: a float at G+0x20 (G = the progress struct 0x00A64E20 the save is made from); + = hero
 * (blue), - = infamous (red); ranks at +-1, +-1000, +-2500. Every gameplay change goes through
 * AddKarma (loads/resets call SetKarma 0x0012F6E0 directly). */
constexpr uint32_t kG = 0x00A64E20, kKarma = kG + 0x20;
constexpr uint32_t kFnAddKarma = 0x0012FC6C;      /* AddKarma(r3 G, f1 delta, r5 reason or -1) */
/* Battery: max energy = base (+0x1F6C) + shard level * energy per level (0x0009B780, only called by
 * the recompute 0x00154DB4) + 15 for each of two upgrade bits (0x20, 0x40) in G+0x944. The recompute
 * runs on pickups/upgrades and sets energy = max when max changes; nothing of it is saved (it is
 * derived from the shards at every load). */
constexpr uint32_t kFnBatteryBonus = 0x0009B780;  /* r3 shard progress: [r3+0x14] data: +0x38 energy/level, +0x40 levels */
constexpr uint32_t kGBatteryBits = kG + 0x944;
/* Movement (snapshots, runs cheats5-7): position +0x1560, velocity +0x1570 (x, y, z up; ~cm), copies
 * at +0x1C14/+0x1C20; gravity ~-1500/s^2, Static Thrusters ~-350/s^2. Writing the velocity here does
 * nothing (run cheats7): those are copies; the air movement runs in a jump/fall controller A.
 * Gliding (static, 2026-11-10; NOTES.md "Glide"): every character in the air has a controller A
 * ([A+0x1C] = the character) with fall state A+0x120 (0 Normal, 1 Danger, 2 Float = Static Thrusters,
 * 3 Attack, 4 MeteorDrop, 5 Yank) and gravity vector A+0x60 (x, y, z, w; z = +0x68), the acceleration
 * its air integrator 0x0019BE18 applies. UpdateGravity 0x0019BB20 (each air frame and at every fall
 * state change) sets A+0x60 = the character's gravity (+0x160) and, in Float only, z = max(z, -350):
 * -350 is a constant of the game's movement tuning (0x00747488, read nowhere else). The glide has no
 * timer (the script value FLOAT_REMAINING is a constant FLT_MAX); it lasts while R1 is held. */
constexpr uint32_t kFnUpdateGravity = 0x0019BB20;  /* UpdateGravity(r3 A) */
constexpr uint32_t kAOwner = 0x1C, kAGravityZ = 0x68, kAFallState = 0x120;
constexpr uint32_t kFallFloat = 2;

/* ---- guest helpers (guest thread: inside a hook) ------------------------------------ */
bool gptr(uint32_t a) { return a >= 0x10000u && a < 0xC0000000u && !(a & 3u); }
float rdf(uint32_t a) { uint32_t u = vm_read32(a); float f; memcpy(&f, &u, 4); return f; }
void wrf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); vm_write32(a, u); }
uint32_t player() { const uint32_t c = vm_read32(kPlayerChar); return gptr(c) ? c : 0; }
double now_s()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/* ---- state ------------------------------------------------------------------------- */
std::atomic<int> g_god{0}, g_energy{0}, g_onehit{0}, g_refill{0};
std::atomic<long long> g_xp_pending{0};
uint32_t s_xp = kXpDefault;
bool s_probe, s_timeline, s_env_done;
long long s_probe_xp = -1;
std::atomic<int> s_blocked{0}, s_boosted{0}, s_dmg_logged{0};
std::atomic<long long> s_ticks{0};
/* karma: side 0 off, +1 hero only, -1 infamous only; multiplier; XP per karma point */
std::atomic<int> g_kside{0}, g_kmult{1}, g_kxp{0};
std::atomic<long long> g_karma_add{0};            /* Add karma: pending amount (guest thread applies) */
bool s_karma_bypass;                              /* our own AddKarma call: no lock/multiplier */
std::atomic<int> s_kblocked{0}, s_kscaled{0};
/* battery */
std::atomic<int> g_maxbat{0}, g_recompute{0};
/* glide: choice index (0 = the game's own glide) */
std::atomic<int> g_glide{0};
std::atomic<long long> s_glided{0};
constexpr float kGlideScale[] = { 1.0f, 0.5f, 0.25f, 0.125f };
/* glide research (tests): snapshots of Cole's character object */
int s_snap_left, s_snap_every = 2;
long long s_snap_n;

void hooks_update();

/* ---- the hooks ----------------------------------------------------------------------- */
void timeline_poll(struct ppu_context* ctx, uint32_t c);
void probe_tick(uint32_t c);
void glide_probe(uint32_t c);

/* Add `n` to the XP pair (available, earned), on the guest thread. */
void xp_add(long long n)
{
    if (!gptr(s_xp) || !n) return;
    for (uint32_t k = 0; k < 2; k++) {
        const long long v = (long long)vm_read32(s_xp + 4 * k) + n;
        vm_write32(s_xp + 4 * k, (uint32_t)(v < 0 ? 0 : v > 99999999 ? 99999999 : v));
    }
}

/* Run one of the game's functions from inside a hook (the guest thread, at a call boundary:
 * the stack below r1 is free), with the registers `setup` sets; ctx is put back after. */
template <class F>
void call_game(struct ppu_context* ctx, ppu_hook_body fn, F setup)
{
    static ppu_context saved;          /* one guest thread runs the hooks that call this */
    memcpy(&saved, ctx, sizeof saved);
    setup(ctx);
    ppu_hook_call(ctx, fn);
    memcpy(ctx, &saved, sizeof saved);
}

void snap_write(uint32_t c);

/* Cole's per-frame update: after the game's own, refill / add what the cheats ask for. */
void hook_cole_update(struct ppu_context* ctx, ppu_hook_body body)
{
    const uint32_t self = (uint32_t)ctx->gpr[3];
    ppu_hook_call(ctx, body);
    const uint32_t c = player();
    if (!c || self != c) {
        static int mis;
        if (s_probe && mis < 5) { mis++; fprintf(stderr, "[cheats] probe: update of %08X, player %08X\n", self, c); }
        return;
    }
    s_ticks.fetch_add(1, std::memory_order_relaxed);
    if (!s_env_done) { s_env_done = true; ps3_cheats_env(); hooks_update(); }
    if (g_energy.load(std::memory_order_relaxed)) {
        const float mx = rdf(c + kEnergyMax);
        if (mx > 0.0f && rdf(c + kEnergy) < mx) wrf(c + kEnergy, mx);
    }
    if (g_refill.exchange(0)) {
        const float mx = rdf(c + kHpMax);
        if (mx > 0.0f) wrf(c + kHp, mx);
    }
    if (long long add = g_xp_pending.exchange(0)) {
        if (gptr(s_xp)) {
            xp_add(add);
            fprintf(stderr, "[cheats] +%lld XP: now %u / %u\n", add, vm_read32(s_xp), vm_read32(s_xp + 4));
        }
        hooks_update();
    }
    if (long long k = g_karma_add.exchange(0)) {     /* Add karma: through the game's own AddKarma */
        const float before = rdf(kKarma);
        s_karma_bypass = true;
        call_game(ctx, func_0012FC6C, [&](struct ppu_context* x) {
            x->gpr[3] = kG; x->gpr[5] = (uint64_t)(int64_t)-1; x->fpr[1] = (double)k; });
        s_karma_bypass = false;
        fprintf(stderr, "[cheats] %+lld karma: %g -> %g\n", k, before, rdf(kKarma));
        hooks_update();
    }
    if (g_recompute.exchange(0)) {                  /* Max battery on/off: the game's own recompute */
        const float before = rdf(c + kEnergyMax);
        call_game(ctx, func_00154DB4, [&](struct ppu_context* x) { x->gpr[3] = c; });
        fprintf(stderr, "[cheats] battery: max energy %g -> %g\n", before, rdf(c + kEnergyMax));
        hooks_update();
    }
    if (s_snap_left > 0) snap_write(c);
    if (s_timeline) timeline_poll(ctx, c);
    if (s_probe) { probe_tick(c); glide_probe(c); }
}

/* AddKarma: the side lock, the multiplier and XP for karma (not for Add karma's own call). */
void hook_add_karma(struct ppu_context* ctx, ppu_hook_body body)
{
    const int side = g_kside.load(std::memory_order_relaxed);
    double d = ctx->fpr[1];
    if (!s_karma_bypass && d != 0.0) {
        if (side && d * side < 0.0) { s_kblocked.fetch_add(1); return; }   /* the other side: no change */
        const int m = g_kmult.load(std::memory_order_relaxed);
        if (m > 1) { d *= m; ctx->fpr[1] = d; s_kscaled.fetch_add(1); }
        const int per = g_kxp.load(std::memory_order_relaxed);
        if (per > 0) xp_add((long long)(fabs(d) * per + 0.5));
    }
    ppu_hook_call(ctx, body);
}

/* The battery bonus with Max battery: every shard level and both upgrade bits. */
void hook_battery_bonus(struct ppu_context* ctx, ppu_hook_body body)
{
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    ppu_hook_call(ctx, body);
    if (!g_maxbat.load(std::memory_order_relaxed) || !gptr(obj)) return;
    const uint32_t data = vm_read32(obj + 0x14);
    if (!gptr(data)) return;
    const int32_t levels = (int32_t)vm_read32(data + 0x40);
    const float per = rdf(data + 0x38);
    if (levels <= 0 || levels > 1000 || !(per > 0.0f && per < 10000.0f)) return;
    double full = (double)levels * per;
    const uint32_t bits = vm_read32(kGBatteryBits);   /* the recompute adds 15 per set bit itself */
    full += 15.0 * (((bits & 0x20) ? 0 : 1) + ((bits & 0x40) ? 0 : 1));
    if (full > ctx->fpr[1]) ctx->fpr[1] = full;
}

/* UpdateGravity: with Glide fall speed, the gravity the game has just set for Cole's glide (fall
 * state Float only) is scaled: he sinks 2x/4x/8x slower while R1 is held. Other fall states and
 * every other character keep the game's value; UpdateGravity sets it again on every air frame and
 * at every fall state change, so nothing is left over when the glide ends or the cheat goes off. */
uint32_t s_cole_a;   /* PS3_CHEATS_PROBE: Cole's jump/fall controller, seen in UpdateGravity */
void hook_update_gravity(struct ppu_context* ctx, ppu_hook_body body)
{
    const uint32_t a = (uint32_t)ctx->gpr[3];
    ppu_hook_call(ctx, body);
    if (!gptr(a)) return;
    const uint32_t c = player();
    if (!c || vm_read32(a + kAOwner) != c) return;
    s_cole_a = a;
    const int k = g_glide.load(std::memory_order_relaxed);
    if (k <= 0 || k >= (int)(sizeof kGlideScale / sizeof kGlideScale[0])) return;
    if (vm_read32(a + kAFallState) != kFallFloat) return;
    const float g = rdf(a + kAGravityZ);
    if (g < 0.0f) {
        wrf(a + kAGravityZ, g * kGlideScale[k]);
        if (s_probe && s_glided.fetch_add(1, std::memory_order_relaxed) % 120 == 0)
            fprintf(stderr, "[cheats] glide: gravity %g -> %g\n", g, rdf(a + kAGravityZ));
    }
}

/* PS3_CHEATS_PROBE (tests), from Cole's update every frame: one line per glide -- how long it
 * lasted, how much height he lost (position +0x1568, ~cm) and the gravity it ran with. */
constexpr uint32_t kPosZ = 0x1568;
void glide_probe(uint32_t c)
{
    static bool was;
    static double t0;
    static float z0, g0;
    const uint32_t a = s_cole_a;
    const bool in_float = gptr(a) && vm_read32(a + kAOwner) == c && vm_read32(a + kAFallState) == kFallFloat;
    if (in_float && !was) { t0 = now_s(); z0 = rdf(c + kPosZ); g0 = rdf(a + kAGravityZ); }
    if (!in_float && was) {
        const double dt = now_s() - t0;
        const float dz = z0 - rdf(c + kPosZ);
        fprintf(stderr, "[cheats] glide probe: %.2f s, lost %.0f height (%.0f/s), gravity %g, setting %d\n",
                dt, (double)dz, dt > 0.0 ? dz / dt : 0.0, (double)g0, g_glide.load());
    }
    was = in_float;
}

/* SetHealth: with Invincibility Cole's health never goes down (healing still works). */
void hook_set_health(struct ppu_context* ctx, ppu_hook_body body)
{
    if (g_god.load(std::memory_order_relaxed)) {
        const uint32_t ch = (uint32_t)ctx->gpr[3];
        if (ch && ch == player()) {
            const float cur = rdf(ch + kHp);
            if (ctx->fpr[1] < (double)cur) { ctx->fpr[1] = (double)cur; s_blocked.fetch_add(1, std::memory_order_relaxed); }
        }
    }
    ppu_hook_call(ctx, body);
}

/* The damage's source is Cole: the character itself, or an object whose +0x1C is it
 * (a pawn/controller -> character link, as at 0x0017AC34). */
bool s_onehit_any;   /* PS3_CHEATS_ONEHIT_ANY=1 (tests): every source counts as Cole */
bool from_player(uint32_t src, uint32_t c)
{
    if (!gptr(src)) return false;
    if (src == c || s_onehit_any) return true;
    return vm_read32(src + 0x1C) == c;
}

/* TakeDamage: with one-hit kills a hit of Cole's on another character is at least that
 * character's health; the damage object is put back afterwards. */
void hook_take_damage(struct ppu_context* ctx, ppu_hook_body body)
{
    const uint32_t victim = (uint32_t)ctx->gpr[3], dmg = (uint32_t)ctx->gpr[4];
    const uint32_t c = player();
    uint32_t old_bits = 0;
    bool boosted = false;
    if (c && gptr(dmg) && gptr(victim) && victim != c) {
        const uint32_t src = vm_read32(dmg + kDmgSource);
        const bool mine = from_player(src, c);
        if (s_probe && s_dmg_logged.fetch_add(1) < 40) {
            char links[160] = ""; size_t n = 0;
            if (gptr(src))
                for (uint32_t o = 0; o < 0x100 && n < sizeof links - 12; o += 4)
                    if (vm_read32(src + o) == c) n += (size_t)snprintf(links + n, sizeof links - n, " +%X", o);
            fprintf(stderr, "[cheats] damage: victim %08X hp %g amount %g src %08X [src+1C] %08X mine %d (src words == Cole at:%s)\n",
                    victim, rdf(victim + kHp), rdf(dmg + kDmgAmount), src, gptr(src) ? vm_read32(src + 0x1C) : 0, mine ? 1 : 0, links);
        }
        if (g_onehit.load(std::memory_order_relaxed) && mine) {
            old_bits = vm_read32(dmg + kDmgAmount);
            float amt; memcpy(&amt, &old_bits, 4);
            const float need = rdf(victim + kHp) + 1.0f;
            if (amt > 0.0f && amt < need) { wrf(dmg + kDmgAmount, need); boosted = true; s_boosted.fetch_add(1); }
        }
    } else if (s_probe && c && victim == c && gptr(dmg) && s_dmg_logged.fetch_add(1) < 40) {
        fprintf(stderr, "[cheats] damage to Cole: hp %g amount %g src %08X\n", rdf(c + kHp), rdf(dmg + kDmgAmount),
                vm_read32(dmg + kDmgSource));
    }
    ppu_hook_call(ctx, body);
    if (boosted) vm_write32(dmg + kDmgAmount, old_bits);
    if (boosted && s_probe)
        fprintf(stderr, "[cheats] one-hit: victim %08X health after the hit %g\n", victim, rdf(victim + kHp));
}

/* ---- fixes (always on) ----------------------------------------------------------------- */
/* func_00196508 (2026-11-11): the largest value in a 30-entry ring of (int time, float value)
 * samples -- obj+0x240 the newest's index, obj+0x244 + 8*i the samples -- among those newer than
 * the newest sample's time minus a window, int(f1 * K1 + K2). It walks back from the newest and
 * stops at the first OLDER sample, with no bound on the walk: when all 30 samples are newer than
 * the cut, it goes round the ring forever. That froze m19-fight for good at 125 s (main thread
 * in here for 55 s, SPUs and RSX idle). The hook runs the game's own body whenever some sample is
 * older than the cut, so the result is the game's, bit for bit; only when none is (where the body
 * would never return) it gives the max over the whole ring, in the body's order and arithmetic
 * (fsubs + fsel), i.e. what one lap of the body computes. The cut is computed as the body does:
 * fmadds then fctiwz, saturating. PS3_FIX_RING_LAP=0 = the game's body always. */
constexpr uint32_t kFnRingMax = 0x00196508;
bool s_fix_ring = true;
bool s_fix_ring_force = false;   /* PS3_FIX_RING_LAP=force (tests): the lap path every call */
std::atomic<int> s_ring_saves{0};
uint32_t fctiwz(double v)   /* the lift's ppu_f2i32(v, 0) */
{
    const int32_t i = _mm_cvttsd_si32(_mm_set_sd(v));
    return (i == INT32_MIN && v > 0.0) ? 0x7FFFFFFFu : (uint32_t)i;
}
void hook_ring_max(struct ppu_context* ctx, ppu_hook_body body)
{
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    const uint32_t head = vm_read32(obj + 0x240);
    if (head > 0x1D) { ppu_hook_call(ctx, body); return; }   /* not the ring we know: the game's code */
    const uint32_t kc = vm_read32((uint32_t)ctx->gpr[2] - 0x7D30);   /* the body's r30: its constants */
    const double w = (double)(float)fma(ctx->fpr[1], (double)rdf(kc - 0x7FE8), (double)rdf(kc - 0x7FE0));
    const uint32_t win = fctiwz(w);
    const uint32_t newest = vm_read32(obj + 0x244 + head * 8u);
    int32_t cut = (int32_t)(newest - win);
    if (cut < 0) cut = 0;
    for (uint32_t i = 0; i <= 0x1D && !s_fix_ring_force; i++)
        if ((int32_t)vm_read32(obj + 0x244 + i * 8u) < cut) { ppu_hook_call(ctx, body); return; }
    float m = rdf(kc - 0x7FF8);
    for (uint32_t n = 0, i = head; n <= 0x1D; n++, i = i ? i - 1u : 0x1Du) {
        const float v = rdf(obj + 0x244 + i * 8u + 4u);
        const float d = (float)((double)m - (double)v);
        m = (d >= 0.0f) ? m : v;
    }
    ctx->fpr[1] = (double)m;
    if (s_ring_saves.fetch_add(1) < 5)
        fprintf(stderr, "[fix] ring max %08X: all 30 samples are newer than %d (newest %u, window %d): "
                "the game's loop would never end; max of the whole ring %g\n", obj, cut, newest, (int32_t)win, (double)m);
}

/* ---- the mouse camera: direct mode (agent/mouse, 2026-11-11; NOTES.md "Mouse camera") -------------
 * PS3_MOUSE_MODE=direct (cellPad, the default): the camera turns by how far the mouse moved -- no stick
 * in between, so none of the game's stick response (its turn-rate cap: free 3.75 / aim 1.65 rad/s at the
 * default camera speed, and its acceleration: the rate may change by at most 15 rad/s per second). What
 * the game does (D1/D2 runs + the code; all in NOTES.md):
 *   free camera: func_00082964 (the camera driver D, once a frame) -> func_0007E0A4 (its input step).
 *     D+0x1C0 yaw, D+0x1C4 pitch (rad; yaw wraps at +-pi, stick right = yaw down, stick up = pitch down),
 *     D+0x1D0/0x1D4 their rates. The manual branch (taken when the camera stick is off centre and the
 *     camera allows it) sets D+0x1A8 = 1, asks func_0008D7C8 (stick -> target rates, aim assist; called
 *     from 0x7E714 only there) and integrates; pitch is held in [-1.4137, 1.4137] (0x007442B0, read-only
 *     tuning data). The rest of func_00082964 makes the view from D+0x1C0 after the input step returns.
 *   aim camera (L1): func_00098A00 (aim camera A, once a frame while aiming): A+0x254 yaw, A+0x258 pitch
 *     (stick up = pitch up), A+0x274/0x278 rates, pitch limits A+0x150 / A+0x154 (-1.5 / 1.4); its right
 *     stick's manual case calls func_0008D7C8 from 0x993E8 every frame it runs.
 *   the camera stick: pad vtable +0x38 = func_0019609C -> 0x19604C: x|x|, y|y| of the right stick, the
 *     game's own invert options G+0x640 (x) / G+0x641 (y).
 * The hooks, each frame the camera updates: take the mouse's motion (cellPad_host_mouse_direct_take:
 * nothing, and 0 back, in stick mode / controller-only input / a menu) and turn the camera by it only
 * when the game runs its manual path (func_0008D7C8 seen from that camera's own call site; for the free
 * camera also D+0x1A8 == 1): the free camera after its step, onto the target yaw and pitch it has just
 * written, carried over frames so the target never leads the game's view by more than its 0.2 rad clamp
 * (hook_free_cam: the view follows the target only so fast, and its view is made after the step
 * returns: same frame); the aim camera before its step, when its manual
 * path ran in its previous frame (it makes the aim inside the step: same frame). Yaw wrapped as the game
 * keeps it, pitch held in the game's limits. A mouse-only frame must
 * still take the free camera's manual path (with the stick centred the game takes its idle one, which
 * also keeps automatic camera states), so the free camera's stick read gets a 1e-6 long stick pointing
 * where the mouse moved instead of 0, in those frames only (and while a carried turn is still being
 * followed): a turn rate of ~4e-6 rad/s, nothing on screen, but the game's own bookkeeping of
 * "the player turns the camera" runs as for a stick.
 * Exact when the mouse is not used: with no motion the hooks only call the game's code and write
 * nothing, so a controller or the keys see the game as before. PS3_MOUSE_CAM_HOOK=0: no hooks at all;
 * PS3_MOUSE_PROBE=1 (tests): one [mouse] line per camera frame with the angles. */
constexpr uint32_t kFnFreeCam = 0x0007E0A4, kFnAimCam = 0x00098A00, kFnCamRate = 0x0008D7C8, kFnCamStick = 0x0019609C;
constexpr uint32_t kLrFreeRate = 0x0007E718, kLrAimRate = 0x000993EC, kLrFreeStick = 0x0007E53C;
constexpr uint32_t kDYaw = 0x1C0, kDPitch = 0x1C4, kDManual = 0x1A8, kDViewYaw = 0x180, kDViewPitch = 0x184;
constexpr uint32_t kAYaw = 0x254, kAPitch = 0x258, kAPitchMin = 0x150, kAPitchMax = 0x154;
constexpr uint32_t kFreePitchLimits = 0x007442B0;          /* (min, max) floats, read-only data */
constexpr uint32_t kGameInvX = kG + 0x640, kGameInvY = kG + 0x641;
/* rad per mouse count at sensitivity 1: 0.0008 (~0.046 deg; 360 deg in ~20 cm at 800 dpi). Aiming:
 * the aim view is 0.5 rad high against the free camera's 0.9 (the render camera's tan(fov/2) 0.2553 vs
 * 0.4831), so aiming turns by 0.2553/0.4831 of that: at the same sensitivity the crosshair crosses the
 * screen as fast as the free view does. */
constexpr float kRadPerCount = 0.0008f, kAimViewRatio = 0.2553f / 0.4831f;
constexpr float kStickEps = 1e-6f;
enum { CAM_NONE, CAM_FREE, CAM_AIM };
bool s_mouse_hook = true;
int s_mouse_probe;               /* 1: a line per camera frame that moved + a summary every 10 s; 2: the summary only */
int s_mt_cam;                    /* the camera step running now (guest main thread) */
float s_mt_dx, s_mt_dy;          /* its mouse motion, counts (the player's and the game's inverts applied) */
bool s_mt_rate, s_mt_eps;        /* its manual path ran / the epsilon stick was given */
bool s_aim_manual;               /* the aim camera's manual path ran in its previous frame */
float s_owe_yaw, s_owe_pitch;    /* free camera: the mouse turn not yet put into the target (rad) */
float s_eps_dx = 1.0f, s_eps_dy; /* free camera: the direction of the last mouse motion (the epsilon stick's) */
int s_free_hold;                 /* free camera: frames left to keep the manual branch after motion */
constexpr int kFreeHold = 45;    /* ~0.3-0.7 s: the view catching up with the last of the turn */
constexpr float kLead = 0.19f;   /* the target's lead on the view we allow (the game clamps at 0.2) */
std::atomic<long long> s_mt_frames{0}, s_mt_turns{0}, s_mt_epsn{0};

float wrap_pi(float a)
{
    const float pi = 3.14159265f, two_pi = 6.28318531f;
    while (a > pi) a -= two_pi;
    while (a < -pi) a += two_pi;
    return a;
}
float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* This camera frame's motion; false = the mouse is not the camera's now (nothing taken). */
bool mouse_take(float* sens, float* aim_sens)
{
    float dx, dy;
    s_mt_dx = s_mt_dy = 0.0f;
    if (!cellPad_host_mouse_direct_take(&dx, &dy, sens, aim_sens)) return false;
    if (vm_read8(kGameInvX)) dx = -dx;     /* the game's own invert options, as for its stick */
    if (vm_read8(kGameInvY)) dy = -dy;
    s_mt_dx = dx; s_mt_dy = dy;
    return true;
}

void mouse_probe(const char* cam, uint32_t o, uint32_t fy, uint32_t fp, float y0, float p0, float y1, float p1,
                 float vy = 0.0f, float vp = 0.0f)
{
    static double last;
    const double t = now_s();
    if (t - last >= 10.0) {
        last = t;
        fprintf(stderr, "[mouse] summary t=%.4f: %lld camera frames, %lld turned by the mouse, %lld with the epsilon stick, mode %s\n",
                t, s_mt_frames.load(), s_mt_turns.load(), s_mt_epsn.load(), cellPad_host_mouse_mode() ? "direct" : "stick");
    }
    if (s_mouse_probe != 1) return;
    const float y2 = rdf(o + fy), p2 = rdf(o + fp);
    if (y0 == y2 && p0 == p2 && !s_mt_dx && !s_mt_dy) return;   /* nothing moved: no line */
    fprintf(stderr, "[mouse] t=%.4f %s %08X d=(%g,%g) manual=%d eps=%d yaw %.6f -> %.6f -> %.6f pitch %.6f -> %.6f -> %.6f view %.6f %.6f\n",
            now_s(), cam, o, (double)s_mt_dx, (double)s_mt_dy, s_mt_rate ? 1 : 0, s_mt_eps ? 1 : 0,
            (double)y0, (double)y1, (double)y2, (double)p0, (double)p1, (double)p2, (double)vy, (double)vp);
}

/* func_0007E0A4, the free camera's input step (r3 = camera driver D). */
void hook_free_cam(struct ppu_context* ctx, ppu_hook_body body)
{
    const uint32_t d = (uint32_t)ctx->gpr[3];
    float sens = 1.0f, aim = 1.0f;
    const bool on = gptr(d) && mouse_take(&sens, &aim);
    const bool moved = on && (s_mt_dx != 0.0f || s_mt_dy != 0.0f);
    const float y0 = gptr(d) ? rdf(d + kDYaw) : 0.0f, p0 = gptr(d) ? rdf(d + kDPitch) : 0.0f;
    const float vy0 = gptr(d) ? rdf(d + kDViewYaw) : 0.0f, vp0 = gptr(d) ? rdf(d + kDViewPitch) : 0.0f;   /* the probe */
    if (!cellPad_host_mouse_direct_ready()) {   /* the menu's Mouse entry: direct mode works here */
        cellPad_host_mouse_direct_provider(1);
        fprintf(stderr, "[mouse] direct mouse camera: the free camera hook runs\n");
    }
    /* The turn owed: the game's view follows the target (D+0x1C0) only so fast, and after the step
     * func_00082964 keeps the target within +-0.2 rad of the view (0x00082FB8: G2+0x1E0/0x1F0), and once
     * the player stops (the idle branch) the target is set back to the view. A turn put into the target
     * all at once came out only partly (runs mouse-t1/t3: 100 counts 50%, 1600 counts 3%; a slow drag
     * 100%). So the turn is carried: each frame as much of it goes into the target as keeps it within
     * 0.19 rad of the view, and the manual branch is kept (the epsilon stick) while some is still owed
     * and for kFreeHold frames after, until the view has caught up. Nothing is lost; the view turns as
     * fast as the game lets its camera follow. Dropped when the free camera stops running (aim). */
    if (moved) {
        const float k = kRadPerCount * sens;
        s_owe_yaw += -s_mt_dx * k;
        s_owe_pitch += s_mt_dy * k;
        s_free_hold = kFreeHold;
        s_eps_dx = s_mt_dx; s_eps_dy = s_mt_dy;
    }
    const bool keep = on && (s_owe_yaw != 0.0f || s_owe_pitch != 0.0f || s_free_hold > 0);
    if (s_free_hold > 0) s_free_hold--;
    s_mt_cam = keep ? CAM_FREE : CAM_NONE;
    s_mt_rate = s_mt_eps = false;
    s_aim_manual = false;            /* the aim camera starts again from its own next frame */
    ppu_hook_call(ctx, body);
    s_mt_cam = CAM_NONE;
    if (!gptr(d)) return;
    const float y1 = rdf(d + kDYaw), p1 = rdf(d + kDPitch);
    const bool manual = keep && s_mt_rate && vm_read32(d + kDManual) == 1;
    if (!manual) {
        s_owe_yaw = s_owe_pitch = 0.0f;             /* no player camera control now: as for a stick, nothing */
        s_free_hold = 0;
    } else if (s_owe_yaw != 0.0f || s_owe_pitch != 0.0f) {
        const float lo = rdf(kFreePitchLimits), hi = rdf(kFreePitchLimits + 4);
        const float vy = rdf(d + kDViewYaw), vp = rdf(d + kDViewPitch);   /* the view of the last frame */
        /* yaw: target + s within view +- kLead (wrapped), towards the owed side only */
        const float ly = wrap_pi(y1 - vy);
        const float sy = s_owe_yaw > 0.0f ? clampf(s_owe_yaw, 0.0f, kLead - ly > 0.0f ? kLead - ly : 0.0f)
                                           : clampf(s_owe_yaw, -kLead - ly < 0.0f ? -kLead - ly : 0.0f, 0.0f);
        const float lp = p1 - vp;
        float sp = s_owe_pitch > 0.0f ? clampf(s_owe_pitch, 0.0f, kLead - lp > 0.0f ? kLead - lp : 0.0f)
                                      : clampf(s_owe_pitch, -kLead - lp < 0.0f ? -kLead - lp : 0.0f, 0.0f);
        const float p2 = clampf(p1 + sp, lo, hi);
        wrf(d + kDYaw, wrap_pi(y1 + sy));
        wrf(d + kDPitch, p2);
        s_owe_yaw -= sy;
        s_owe_pitch -= sp;
        if (p2 != p1 + sp || (s_owe_pitch > 0.0f && p2 >= hi) || (s_owe_pitch < 0.0f && p2 <= lo))
            s_owe_pitch = 0.0f;                     /* at a pitch limit: the rest is gone, as for a stick */
        if (s_owe_yaw > -1e-7f && s_owe_yaw < 1e-7f) s_owe_yaw = 0.0f;
        if (s_owe_pitch > -1e-7f && s_owe_pitch < 1e-7f) s_owe_pitch = 0.0f;
        s_free_hold = kFreeHold;                    /* the view still has the last of it to follow */
        s_mt_turns.fetch_add(1, std::memory_order_relaxed);
    }
    s_mt_frames.fetch_add(1, std::memory_order_relaxed);
    if (s_mt_eps) s_mt_epsn.fetch_add(1, std::memory_order_relaxed);
    if (s_mouse_probe) mouse_probe("free", d, kDYaw, kDPitch, y0, p0, y1, p1, vy0, vp0);
}

/* func_00098A00, the aim camera (r3 = aim camera A), once a frame while aiming. It makes the aim (and
 * Cole's aim direction) from A+0x254/0x258 later in the same call, so the turn goes in BEFORE the game's
 * step (same frame): gated on its manual path having run in its previous frame (s_aim_manual, reset by
 * every free camera frame), as a stick's would be; the game then integrates its stick rate on top and
 * keeps the pitch within its limits (the soft limit in func_0008D7C8). */
void hook_aim_cam(struct ppu_context* ctx, ppu_hook_body body)
{
    const uint32_t a = (uint32_t)ctx->gpr[3];
    float sens = 1.0f, aim = 1.0f;
    const bool on = gptr(a) && mouse_take(&sens, &aim);
    const bool moved = on && (s_mt_dx != 0.0f || s_mt_dy != 0.0f);
    s_owe_yaw = s_owe_pitch = 0.0f;  /* the free camera's carried turn ends with it */
    s_free_hold = 0;
    if (!gptr(a)) { ppu_hook_call(ctx, body); return; }
    const float y0 = rdf(a + kAYaw), p0 = rdf(a + kAPitch);
    if (moved && s_aim_manual) {
        const float k = kRadPerCount * kAimViewRatio * aim;
        wrf(a + kAYaw, wrap_pi(y0 - s_mt_dx * k));
        wrf(a + kAPitch, clampf(p0 - s_mt_dy * k, rdf(a + kAPitchMin), rdf(a + kAPitchMax)));
        s_mt_turns.fetch_add(1, std::memory_order_relaxed);
    }
    const float y1 = rdf(a + kAYaw), p1 = rdf(a + kAPitch);
    s_mt_cam = CAM_AIM;              /* watch for its manual path whether or not the mouse moved */
    s_mt_rate = s_mt_eps = false;
    ppu_hook_call(ctx, body);
    s_mt_cam = CAM_NONE;
    s_aim_manual = s_mt_rate;
    s_mt_frames.fetch_add(1, std::memory_order_relaxed);
    if (s_mouse_probe) mouse_probe("aim", a, kAYaw, kAPitch, y0, p0, y1, p1);
}

/* func_0008D7C8 (stick -> target turn rates): called by a camera's manual path only. */
void hook_cam_rate(struct ppu_context* ctx, ppu_hook_body body)
{
    const uint32_t lr = (uint32_t)ctx->lr;
    ppu_hook_call(ctx, body);
    if ((s_mt_cam == CAM_FREE && lr == kLrFreeRate) || (s_mt_cam == CAM_AIM && lr == kLrAimRate)) s_mt_rate = true;
}

/* func_0019609C, the camera stick (r4 = 1 the right stick; r5, r6 = where x|x|, y|y| go). */
void hook_cam_stick(struct ppu_context* ctx, ppu_hook_body body)
{
    const uint32_t lr = (uint32_t)ctx->lr, which = (uint32_t)ctx->gpr[4];
    const uint32_t px = (uint32_t)ctx->gpr[5], py = (uint32_t)ctx->gpr[6];
    ppu_hook_call(ctx, body);
    if (s_mt_cam == CAM_FREE && lr == kLrFreeStick && which == 1 && gptr(px) && gptr(py) &&
        rdf(px) == 0.0f && rdf(py) == 0.0f) {   /* the stick is centred */
        /* pointing where the mouse moved: the game's stick axis snap (within 15 deg of an axis = that
         * axis only) then marks the axes the player turned (D+0x1E4 yaw / 0x1E5 pitch) as for a stick;
         * with pitch unmarked it levels the pitch back to its default (run mouse-t4) */
        const float n = sqrtf(s_eps_dx * s_eps_dx + s_eps_dy * s_eps_dy);
        wrf(px, n > 0.0f ? kStickEps * s_eps_dx / n : kStickEps);
        wrf(py, n > 0.0f ? kStickEps * s_eps_dy / n : 0.0f);
        s_mt_eps = true;
    }
}

/* Which hooks are needed now: each is set only while a cheat (or a test knob) uses it. */
void hooks_update()
{
    const bool tick = g_god || g_energy || g_xp_pending.load() != 0 || g_refill || s_probe || s_timeline ||
                      !s_env_done || g_karma_add.load() != 0 || g_recompute || s_snap_left > 0;
    ppu_hook_set(kFnColeUpdate, tick ? hook_cole_update : nullptr);
    ppu_hook_set(kFnSetHealth, g_god ? hook_set_health : nullptr);
    ppu_hook_set(kFnTakeDamage, (g_onehit || s_probe) ? hook_take_damage : nullptr);
    ppu_hook_set(kFnAddKarma, (g_kside || g_kmult > 1 || g_kxp > 0) ? hook_add_karma : nullptr);
    ppu_hook_set(kFnBatteryBonus, g_maxbat ? hook_battery_bonus : nullptr);
    ppu_hook_set(kFnUpdateGravity, (g_glide > 0 || s_probe) ? hook_update_gravity : nullptr);
    ppu_hook_set(kFnRingMax, s_fix_ring ? hook_ring_max : nullptr);   /* the fix, always */
    /* [mouse] the mouse camera's direct mode: always (they write nothing while the mouse is still) */
    ppu_hook_set(kFnFreeCam, s_mouse_hook ? hook_free_cam : nullptr);
    ppu_hook_set(kFnAimCam, s_mouse_hook ? hook_aim_cam : nullptr);
    ppu_hook_set(kFnCamRate, s_mouse_hook ? hook_cam_rate : nullptr);
    ppu_hook_set(kFnCamStick, s_mouse_hook ? hook_cam_stick : nullptr);
}

/* ---- the menu's cheats --------------------------------------------------------------- */
int apply_god(ps3_cheat* c)    { g_god = c->value != 0.0; if (g_god) g_refill = 1; hooks_update(); return 1; }
int apply_energy(ps3_cheat* c) { g_energy = c->value != 0.0; hooks_update(); return 1; }
int apply_onehit(ps3_cheat* c) { g_onehit = c->value != 0.0; hooks_update(); return 1; }
int apply_jump(ps3_cheat* c)   { ppu_patch_set(SLOT_JUMP, (int)c->value); return ppu_patch_lifted(SLOT_JUMP); }
int apply_speed(ps3_cheat* c)  { ppu_patch_set(SLOT_NPC_SPEED, (int)c->value); return ppu_patch_lifted(SLOT_NPC_SPEED); }
int apply_noattack(ps3_cheat* c) { ppu_patch_set(SLOT_NO_ATTACK, c->value != 0.0 ? 1 : 0); return ppu_patch_lifted(SLOT_NO_ATTACK); }
int apply_xp(ps3_cheat* c)
{
    if (!gptr(s_xp)) return 0;
    g_xp_pending += (long long)c->value;
    hooks_update();
    return 1;
}

int apply_kside(ps3_cheat* c) { g_kside = (int)c->value == 1 ? 1 : (int)c->value == 2 ? -1 : 0; hooks_update(); return 1; }
int apply_kmult(ps3_cheat* c) { g_kmult = (int)c->value < 1 ? 1 : (int)c->value; hooks_update(); return 1; }
int apply_kxp(ps3_cheat* c)   { g_kxp = (int)c->value < 0 ? 0 : (int)c->value; hooks_update(); return 1; }
int apply_kblue(ps3_cheat* c) { g_karma_add += (long long)c->value; hooks_update(); return 1; }
int apply_kred(ps3_cheat* c)  { g_karma_add -= (long long)c->value; hooks_update(); return 1; }
int apply_glide(ps3_cheat* c)
{
    const int k = (int)c->value;
    g_glide = k > 0 && k < (int)(sizeof kGlideScale / sizeof kGlideScale[0]) ? k : 0;
    hooks_update();
    return ppu_hook_exists(kFnUpdateGravity) ? 1 : 0;
}
int apply_maxbat(ps3_cheat* c)
{
    g_maxbat = c->value != 0.0;
    g_recompute = 1;              /* the game recomputes max energy on Cole's next update */
    hooks_update();
    return 1;
}

const char* const kKarmaSide[] = { "Off", "Hero (blue) only", "Infamous (red) only" };
const char* const kJump[] = { "Normal", "2x higher", "3x higher", "5x higher", "10x higher" };
const char* const kSpeed[] = { "Normal", "Frozen", "Quarter speed", "Half speed", "Double speed" };
const char* const kGlide[] = { "Normal", "Longer", "2x as long", "3x as long" };   /* gravity /2, /4, /8: measured 3.1 s -> 6.4 s / 9.2 s */

ps3_cheat s_god = { "god", "Invincibility",
    "Cole's health never drops (healing still works). If you get stuck where the game expects you to "
    "die, such as deep water, switch it off.", "Cole", PS3_CHEAT_TOGGLE, apply_god };
ps3_cheat s_energy = { "energy", "Infinite energy",
    "Electricity refills every frame: use any power as often as you like.", "Cole", PS3_CHEAT_TOGGLE, apply_energy };
ps3_cheat s_jump = { "jump", "Super jump", "How high Cole jumps.", "Cole", PS3_CHEAT_CHOICE, apply_jump,
    0, 0, 0, NULL, kJump, 5 };
ps3_cheat s_glide = { "glide", "Longer glide",
    "Cole sinks more slowly while gliding with the Static Thrusters (hold R1 in the air), so a glide lasts "
    "longer and goes farther. Only the glide changes: jumps, falls and everyone else are as normal.", "Cole",
    PS3_CHEAT_CHOICE, apply_glide, 0, 0, 0, NULL, kGlide, 4 };
ps3_cheat s_onehit = { "onehit", "One-hit kills",
    "Anyone Cole hurts dies from that hit. Hits from others are unchanged.", "Enemies", PS3_CHEAT_TOGGLE, apply_onehit };
ps3_cheat s_speed = { "speed", "Enemy speed", "How fast enemies and other people move.", "Enemies",
    PS3_CHEAT_CHOICE, apply_speed, 0, 0, 0, NULL, kSpeed, 5 };
ps3_cheat s_noattack = { "noattack", "Enemies hold fire",
    "Most enemies never attack. Turrets and some mission enemies still do.", "Enemies", PS3_CHEAT_TOGGLE, apply_noattack };
ps3_cheat s_xpc = { "xp", "Add XP", "Experience to spend on powers (pause menu, Powers).", "Progress",
    PS3_CHEAT_ACTION, apply_xp, 5000, 500, 50000, "%d XP", NULL, 0, "Add XP", PS3_CHEAT_SAVES };
ps3_cheat s_maxbat = { "maxbat", "Max battery",
    "Cole's battery at its full size (every blast shard level and both battery upgrades). The game "
    "does not save it; Remember cheats switches it on again at the next start.", "Cole", PS3_CHEAT_TOGGLE, apply_maxbat };
ps3_cheat s_kside = { "karmaside", "Karma lock",
    "Karma moves only towards the chosen side; the game ignores changes towards the other one.",
    "Karma", PS3_CHEAT_CHOICE, apply_kside, 0, 0, 0, NULL, kKarmaSide, 3, NULL, PS3_CHEAT_SAVES };
ps3_cheat s_kmult = { "karmamult", "Karma multiplier",
    "Karma gains of the locked side (of both sides when the lock is off) count this many times.",
    "Karma", PS3_CHEAT_INT, apply_kmult, 1, 1, 10, "%dx", NULL, 0, NULL, PS3_CHEAT_SAVES };
ps3_cheat s_kxp = { "karmaxp", "XP for karma",
    "Each karma point gained on the locked side (either side when the lock is off) also gives this much XP.",
    "Karma", PS3_CHEAT_INT, apply_kxp, 0, 0, 20, "%d XP per point", NULL, 0, NULL, PS3_CHEAT_SAVES };
ps3_cheat s_kblue = { "karmablue", "hero karma", NULL, "Karma", PS3_CHEAT_ACTION, apply_kblue,
    500, 50, 2500, "%d", NULL, 0, "Add hero karma (blue)", PS3_CHEAT_SAVES };
ps3_cheat s_kred = { "karmared", "infamous karma", NULL, "Karma", PS3_CHEAT_ACTION, apply_kred,
    500, 50, 2500, "%d", NULL, 0, "Add infamous karma (red)", PS3_CHEAT_SAVES };

void refresh()
{
    const char* lift = "needs the cheats lift";
    s_god.unavailable    = ppu_hook_exists(kFnSetHealth) && ppu_hook_exists(kFnColeUpdate) ? NULL : lift;
    s_energy.unavailable = ppu_hook_exists(kFnColeUpdate) ? NULL : lift;
    s_onehit.unavailable = ppu_hook_exists(kFnTakeDamage) ? NULL : lift;
    s_jump.unavailable   = ppu_patch_lifted(SLOT_JUMP) ? NULL : lift;
    s_glide.unavailable  = ppu_hook_exists(kFnUpdateGravity) ? NULL : lift;
    s_speed.unavailable  = ppu_patch_lifted(SLOT_NPC_SPEED) ? NULL : lift;
    s_noattack.unavailable = ppu_patch_lifted(SLOT_NO_ATTACK) ? NULL : lift;
    s_xpc.unavailable    = !ppu_hook_exists(kFnColeUpdate) ? lift : gptr(s_xp) ? NULL : "XP not located";
    s_maxbat.unavailable = ppu_hook_exists(kFnBatteryBonus) && ppu_hook_exists(kFnColeUpdate) ? NULL : lift;
    const char* k = ppu_hook_exists(kFnAddKarma) ? NULL : lift;
    s_kside.unavailable = s_kmult.unavailable = s_kxp.unavailable = k;
    s_kblue.unavailable = s_kred.unavailable = ppu_hook_exists(kFnColeUpdate) ? NULL : lift;
}

/* ---- test knobs -------------------------------------------------------------------- */
struct TimedSet { double t; char id[24]; double v; bool done; };
TimedSet s_at[32];
int s_n_at;
double s_t0 = -1.0, s_poll_t;
const char* s_at_wait;

void selftest(struct ppu_context* ctx, uint32_t c)
{
    static ppu_context saved;
    memcpy(&saved, ctx, sizeof saved);
    const float hp0 = rdf(c + kHp), mx = rdf(c + kHpMax);
    const int god_was = g_god;
    auto hit = [&](float d) {
        ctx->gpr[3] = c; ctx->gpr[4] = 0; ctx->gpr[5] = 0; ctx->fpr[1] = d;
        ppu_hook_call(ctx, func_00363E54);          /* the game's ApplyHealthDelta (and its tail jumps) */
        memcpy(ctx, &saved, sizeof saved);
    };
    ps3_cheat_set(&s_god, 1);
    hit(-0.25f * mx);
    const float hp1 = rdf(c + kHp);
    ps3_cheat_set(&s_god, 0);
    hit(-0.25f * mx);
    const float hp2 = rdf(c + kHp);
    wrf(c + kHp, hp0);
    ps3_cheat_set(&s_god, god_was);
    g_refill = 0;
    fprintf(stderr, "[cheats] selftest: health %g of %g; a hit of %g with Invincibility: %g (%s); without: %g (%s)\n",
            hp0, mx, 0.25f * mx, hp1, hp1 == hp0 ? "PASS" : "FAIL", hp2,
            hp2 < hp0 ? "PASS" : "FAIL");
}

void timeline_poll(struct ppu_context* ctx, uint32_t c)
{
    const double t = now_s();
    if (s_t0 < 0.0) {
        if (t - s_poll_t < 0.2) return;
        s_poll_t = t;
#ifdef _WIN32
        if (s_at_wait && GetFileAttributesA(s_at_wait) == INVALID_FILE_ATTRIBUTES) return;
#endif
        s_t0 = t;
        fprintf(stderr, "[cheats] timeline starts (t=%.4f)\n", t);
    }
    bool left = false;
    for (int i = 0; i < s_n_at; i++) {
        TimedSet& e = s_at[i];
        if (e.done) continue;
        if (t - s_t0 < e.t) { left = true; continue; }
        e.done = true;
        fprintf(stderr, "[cheats] t=%.1f: %s=%g\n", t - s_t0, e.id, e.v);
        if (!strcmp(e.id, "selftest")) selftest(ctx, c);
        else if (!strcmp(e.id, "snap")) {           /* PS3_CHEATS_SNAP=<file>: Cole's object, every 2nd frame */
            s_snap_left = (int)e.v;
            hooks_update();
        }
        else if (!strcmp(e.id, "karmatest")) {      /* a karma change through the hooked AddKarma (lock/multiplier) */
            const float before = rdf(kKarma);
            const long long xp0 = gptr(s_xp) ? vm_read32(s_xp) : 0;
            call_game(ctx, func_0012FC6C, [&](struct ppu_context* x) {
                x->gpr[3] = kG; x->gpr[5] = (uint64_t)(int64_t)-1; x->fpr[1] = e.v; });
            fprintf(stderr, "[cheats] karmatest %+g: karma %g -> %g, xp %lld -> %u\n", e.v, before, rdf(kKarma),
                    xp0, gptr(s_xp) ? vm_read32(s_xp) : 0);
        }
        else if (!strcmp(e.id, "mousemode")) {      /* [mouse] tests: direct (1) / stick (0) mode */
            cellPad_host_set_mouse_mode(e.v != 0.0);
            fprintf(stderr, "[mouse] mode %s (turns so far %lld in %lld camera frames)\n", e.v != 0.0 ? "direct" : "stick",
                    s_mt_turns.load(), s_mt_frames.load());
        }
        else if (!strcmp(e.id, "drain")) {          /* energy as if spent (tests the refill) */
            wrf(c + kEnergy, (float)e.v);
            fprintf(stderr, "[cheats] drain: energy set to %g\n", rdf(c + kEnergy));
        }
        else if (!strcmp(e.id, "dump")) {           /* PS3_CHEATS_DUMP=addr,addr: words around each */
            if (const char* p = getenv("PS3_CHEATS_DUMP")) {
                char buf[128]; snprintf(buf, sizeof buf, "%s", p);
                for (char* tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) {
                    const uint32_t a = (uint32_t)strtoul(tok, nullptr, 16) & ~3u;
                    if (!gptr(a) || a < 0x30) continue;
                    char line[400]; size_t n = 0;
                    for (uint32_t o = 0; o < 0x40; o += 4)
                        n += (size_t)snprintf(line + n, sizeof line - n, " %08X", vm_read32(a - 0x18 + o));
                    fprintf(stderr, "[cheats] dump %08X-0x18:%s\n", a, line);
                }
            }
        }
        else if (!strcmp(e.id, "poke")) {           /* PS3_CHEATS_POKE: +delta to a u32 pair (XP search) */
            if (const char* p = getenv("PS3_CHEATS_POKE")) {
                const uint32_t a = (uint32_t)strtoul(p, nullptr, 16);
                const char* d = strchr(p, ':');
                if (gptr(a) && d)
                    for (uint32_t k = 0; k < 2; k++) vm_write32(a + 4 * k, vm_read32(a + 4 * k) + (uint32_t)atoi(d + 1));
                fprintf(stderr, "[cheats] poke: %08X now %u / %u\n", a, vm_read32(a), vm_read32(a + 4));
            }
        }
        else if (ps3_cheat* ch = ps3_cheat_find(e.id)) ps3_cheat_set(ch, e.v);
    }
    if (!left) { s_timeline = false; hooks_update(); }
}

void scan_pair(uint32_t want)
{
#ifdef _WIN32
    const uint32_t be = __builtin_bswap32(want);
    int hits = 0, in_elf = 0;
    uint32_t elf_hit = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t* lo = vm_base + 0x10000u;
    uint8_t* hi = vm_base + 0xC0000000ull;
    for (uint8_t* p = lo; p < hi && hits < 40; ) {
        if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
        uint8_t* end = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        const DWORD pr = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
            (pr == PAGE_READWRITE || pr == PAGE_READONLY || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_WRITECOPY)) {
            const uint32_t* w = (const uint32_t*)(p > (uint8_t*)mbi.BaseAddress ? p : (uint8_t*)mbi.BaseAddress);
            const uint32_t* we = (const uint32_t*)(end < hi ? end : hi) - 3;
            for (; w < we && hits < 40; w++)
                if (w[0] == be && w[1] == be) {
                    const uint32_t ea = (uint32_t)((const uint8_t*)w - vm_base);
                    fprintf(stderr, "[cheats] probe: (%u, %u) at %08X  (+8: %08X %08X)\n", want, want, ea,
                            __builtin_bswap32(w[2]), __builtin_bswap32(w[3]));
                    hits++;
                    if (ea >= 0x00930000u && ea < 0x00BA4408u) { in_elf++; elf_hit = ea; }   /* the EBOOT's data/bss */
                }
        }
        p = end;
    }
    fprintf(stderr, "[cheats] probe: %d pair(s) of %u, %d in the EBOOT's data\n", hits, want, in_elf);
    if (in_elf == 1 && !gptr(s_xp) && getenv("PS3_CHEATS_PROBE_XP_USE")) {
        s_xp = elf_hit;
        fprintf(stderr, "[cheats] probe: XP pair taken at %08X for this run\n", s_xp);
    }
#else
    (void)want;
#endif
}

void snap_write(uint32_t c)
{
    static FILE* f;
    static long long frame;
    frame++;
    if (frame % s_snap_every) return;
    if (!f) { const char* p = getenv("PS3_CHEATS_SNAP"); if (!p || !(f = fopen(p, "ab"))) { s_snap_left = 0; return; } }
    static uint8_t buf[0x2800];
    for (uint32_t o = 0; o < sizeof buf; o += 4) { const uint32_t w = vm_read32(c + o); memcpy(buf + o, &w, 4); }
    const double t = s_t0 >= 0 ? now_s() - s_t0 : -1.0;
    fwrite(&t, 8, 1, f); fwrite(&c, 4, 1, f); fwrite(buf, 1, sizeof buf, f);
    s_snap_n++;
    if (--s_snap_left == 0) { fflush(f); fprintf(stderr, "[cheats] snap: %lld snapshots written\n", s_snap_n); hooks_update(); }
}

void probe_tick(uint32_t c)
{
    static double last, last_e;
    static float e_was = -1.0f;
    const double t = now_s();
    const float e = rdf(c + kEnergy);
    if (e != e_was && t - last_e >= 0.25) {         /* energy changes, at most 4 lines a second */
        fprintf(stderr, "[cheats] probe: energy %g/%g (t=%.2f)\n", e, rdf(c + kEnergyMax), s_t0 >= 0 ? t - s_t0 : -1.0);
        e_was = e; last_e = t;
    }
    if (t - last < 1.0) return;
    last = t;
    if (s_probe_xp >= 0 && s_ticks.load() > 120) { scan_pair((uint32_t)s_probe_xp); s_probe_xp = -1; }
    fprintf(stderr, "[cheats] probe: Cole %08X health %g/%g energy %g/%g xp %u/%u karma %g ticks %lld blocked %d "
            "boosted %d kblocked %d kscaled %d glided %lld\n",
            c, rdf(c + kHp), rdf(c + kHpMax), rdf(c + kEnergy), rdf(c + kEnergyMax),
            gptr(s_xp) ? vm_read32(s_xp) : 0, gptr(s_xp) ? vm_read32(s_xp + 4) : 0, rdf(kKarma),
            s_ticks.load(), s_blocked.load(), s_boosted.load(), s_kblocked.load(), s_kscaled.load(),
            s_glided.load());
}

struct Init {
    Init()
    {
        ps3_cheat* all[] = { &s_god, &s_energy, &s_maxbat, &s_jump, &s_glide, &s_onehit, &s_speed, &s_noattack, &s_xpc,
                             &s_kside, &s_kmult, &s_kxp, &s_kblue, &s_kred };
        for (ps3_cheat* c : all) ps3_cheat_register(c);
        ps3_cheats_set_note("XP and karma changed here are real: the game writes them into your save the next "
                            "time it saves. Copy your saves first if you want to keep a clean game.");
        ps3_cheats_set_refresh(refresh);
        if (const char* e = getenv("PS3_CHEATS_XP")) s_xp = (uint32_t)strtoul(e, nullptr, 16);
        s_onehit_any = getenv("PS3_CHEATS_ONEHIT_ANY") && atoi(getenv("PS3_CHEATS_ONEHIT_ANY")) != 0;
        s_probe = getenv("PS3_CHEATS_PROBE") && atoi(getenv("PS3_CHEATS_PROBE")) != 0;
        if (const char* e = getenv("PS3_CHEATS_PROBE_XP")) { s_probe_xp = atoll(e); s_probe = true; }
        if (const char* e = getenv("PS3_CHEATS_AT")) {
            char buf[1024];
            snprintf(buf, sizeof buf, "%s", e);
            for (char* tok = strtok(buf, ", "); tok && s_n_at < 32; tok = strtok(nullptr, ", ")) {
                char* colon = strchr(tok, ':');
                char* eq = colon ? strchr(colon, '=') : nullptr;
                if (!eq) continue;
                *eq = 0;
                TimedSet& a = s_at[s_n_at++];
                a.t = atof(tok);
                snprintf(a.id, sizeof a.id, "%s", colon + 1);
                a.v = atof(eq + 1);
            }
            s_at_wait = getenv("PS3_CHEATS_AT_WAIT");
            s_timeline = s_n_at > 0;
        }
        /* The first Cole update applies PS3_CHEATS (the hook slots exist by then); the
         * update hook stays only while something needs it. Plain play: no env, and after
         * that first update every slot is empty again. */
        if (const char* e = getenv("PS3_FIX_RING_LAP")) { s_fix_ring = e[0] != '0'; s_fix_ring_force = !strcmp(e, "force"); }
        if (const char* e = getenv("PS3_MOUSE_CAM_HOOK")) s_mouse_hook = e[0] != '0';   /* [mouse] */
        s_mouse_probe = getenv("PS3_MOUSE_PROBE") ? atoi(getenv("PS3_MOUSE_PROBE")) : 0;
        if (!getenv("PS3_CHEATS")) s_env_done = true;
        hooks_update();
        if (s_probe || s_timeline || !s_env_done)
            fprintf(stderr, "[cheats] test knobs: probe %d, timeline %d entries, PS3_CHEATS %s\n",
                    s_probe ? 1 : 0, s_n_at, s_env_done ? "-" : getenv("PS3_CHEATS"));
    }
};
Init s_init;

} // namespace
