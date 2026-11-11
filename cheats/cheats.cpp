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
 *   0x004AE348            MatchPositionAndRotation(r3 object, r4 -> matrix): the game's own placing of an
 *                         object, with a character's teleport reset (saved spots, NOTES.md "Saved spots")
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
#include <cmath>
#include <mutex>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <emmintrin.h>   /* _mm_cvttsd_si32: the PPU's fctiwz (the ring fix below) */
#include <stdarg.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

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
/* saved spots (the "Saved spots" section below): a Go waiting for Cole's next update; the fall-through
 * guard after one; the saved camera's turn (the free camera hook); the free camera's last angles */
std::atomic<int> g_tp_pending{0};
std::atomic<bool> s_settle_on{false};
std::atomic<int> s_tpc_on{0};
float s_tpc_yaw, s_tpc_pitch;
double s_tpc_t0;
bool s_tpc_cancel;
std::atomic<uint32_t> s_cam_yaw_bits{0}, s_cam_pitch_bits{0};
std::atomic<double> s_cam_t{-1.0};

void hooks_update();
void spot_tick(struct ppu_context* ctx, uint32_t c);

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
    if (g_tp_pending.load(std::memory_order_relaxed) || s_settle_on.load(std::memory_order_relaxed))
        spot_tick(ctx, c);                          /* Go to saved spot (after the game's own update) */
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

/* [spot] Go to saved spot with a saved camera (spot_go sets s_tpc_*): the camera is turned to the saved
 * target yaw/pitch with the mouse turn's own mechanism below, whatever the player plays with. Each free
 * camera frame the turn owed is the saved angle minus the camera's target now (absolute, so nothing is
 * lost on a frame where the game's manual branch does not run), carried into the target within the
 * view's reach as a mouse turn is; the manual branch is kept (the epsilon stick, pointing both ways so the
 * game counts both axes as turned and does not level the pitch) until the view itself has arrived --
 * the idle branch would set the target back to the view. Over when the view is there, after
 * kTpCamSecs, or as soon as the player turns the camera (mouse, stick) or aims. */
constexpr double kTpCamSecs = 3.0;
void tpc_end(const char* why)
{
    s_tpc_on.store(0, std::memory_order_relaxed);
    s_owe_yaw = s_owe_pitch = 0.0f;
    s_free_hold = 0;
    fprintf(stderr, "[cheats] spot: camera turn over (%s) after %.2f s\n", why, now_s() - s_tpc_t0);
}
bool tpc_frame(uint32_t d, float y0, float p0, float vy, float vp, bool moved)   /* true: this frame turns */
{
    if (!s_tpc_on.load(std::memory_order_acquire)) return false;
    if (moved || s_tpc_cancel) { tpc_end("the player turned the camera"); return false; }
    if (now_s() - s_tpc_t0 > kTpCamSecs) { tpc_end("time"); return false; }
    const float gp = clampf(s_tpc_pitch, rdf(kFreePitchLimits), rdf(kFreePitchLimits + 4));
    float oy = wrap_pi(s_tpc_yaw - y0), op = gp - p0;
    if (oy > -1e-4f && oy < 1e-4f) oy = 0.0f;
    if (op > -1e-4f && op < 1e-4f) op = 0.0f;
    const float ey = wrap_pi(s_tpc_yaw - vy), ep = gp - vp;
    if (oy == 0.0f && op == 0.0f && ey > -0.005f && ey < 0.005f && ep > -0.005f && ep < 0.005f) {
        tpc_end("the view is there");
        return false;
    }
    (void)d;
    s_owe_yaw = oy;
    s_owe_pitch = op;
    s_free_hold = kFreeHold;
    s_eps_dx = oy > 0.0f ? -1.0f : 1.0f;   /* as a mouse turn of (dx, dy) owes (-dx, +dy) */
    s_eps_dy = op >= 0.0f ? 1.0f : -1.0f;
    return true;
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
    const bool tp = gptr(d) && tpc_frame(d, y0, p0, vy0, vp0, moved);   /* [spot] (false when no Go is turning it) */
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
    const bool keep = (on || tp) && (s_owe_yaw != 0.0f || s_owe_pitch != 0.0f || s_free_hold > 0);
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
    /* [spot] the camera's angles for Save this spot (host memory only; read on the menu's thread) */
    { const float cy = rdf(d + kDYaw), cp = rdf(d + kDPitch); uint32_t b;
      memcpy(&b, &cy, 4); s_cam_yaw_bits.store(b, std::memory_order_relaxed);
      memcpy(&b, &cp, 4); s_cam_pitch_bits.store(b, std::memory_order_relaxed);
      s_cam_t.store(now_s(), std::memory_order_release); }
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
    if (s_tpc_on.load(std::memory_order_relaxed)) tpc_end("aiming");   /* [spot] the player aims: his camera */
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
        (rdf(px) != 0.0f || rdf(py) != 0.0f) && s_tpc_on.load(std::memory_order_relaxed))
        s_tpc_cancel = true;                    /* [spot] the player's own stick: the saved camera's turn ends */
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

/* ---- saved spots: "Save this spot" / "Go to saved spot" (2026-11-11; NOTES.md "Saved spots") --------------
 * How the game itself puts a character somewhere (static, EBOOT v1.04; NOTES.md has the trail):
 *   func_004AE348(r3 object, r4 -> 16-byte aligned 4x4 matrix: rows 0-2 rotation, row 3 position) is the native
 *   of the script command MATCH_POSITION_AND_ROTATION (reflection entry 0x00956A18 -> handler 0x00528944):
 *     1. SetWorldPosition 0x004ABDFC: object +0x90 += target - its world position (0x004AAF28), in its
 *        parent's frame; then vtable +0x5C (the world matrix +0xB0 and the attached children made again).
 *     2. SetWorldRotation 0x004AE158: object +0x80 = the matrix's rotation (normalised: scale ignored); +0x5C.
 *     3. when the object is live (0x001BDB68: no +0x58 flag up its parent chain), vtable +0x60 -- for Cole
 *        0x0015520C = 0x00365060, the character's teleport reset (animation history 0x0032C3D8, facing
 *        state +0x2D0/+0x334, the components +0x520 and +0x3B0 (ground probe, which held a position copy),
 *        linear and angular velocity 0 through vtable +0x78/+0x7C into the physics proxy +0xF4) and
 *        0x0035BA80 (the skill manager +0x378 restarts the skill it runs).
 *   The game runs the same three steps on Cole itself (func_004AE434 = position in v2, rotation matrix in r7)
 *   to put him back where he stood when a UGC test play ends (0x003DCF88 keeps [0x00A6E9E4]'s world matrix
 *   +0xB0 when it starts and puts it back after) -- the closest thing to our feature in the game.
 *   Why it sticks: each frame Cole's movement (0x00367810, from the proxy sync 0x004A63A0 through his vtable
 *   +0x88) starts from his world position (0x004AAF28), and the sync then writes +0x90/+0x80 from the result.
 *   Every other copy (+0xB0 matrix, +0x1F0/+0x250 interpolation history, +0x1560 motion block, +0x3D0/
 *   +0x410) is made from +0x90/+0x80 -- why writing those (Stage B) changed nothing.
 * What this code adds around it:
 *   - The call is made from Cole's own per-frame update (hook 0x001578F8, after the game's body: Cole is
 *     not half-updated) on the game's thread; the matrix goes on the guest stack below r1 (free at a call
 *     boundary), the call gets its own frame below that. Cole is put 5 cm above the spot (never inside
 *     the ground he stood on), upright, facing the saved yaw (or as he faces now).
 *   - Refused (nothing written) while Cole is held by a parent (+0x64 with +0x1C: climbing, riding),
 *     switched off (+0x58: cutscenes) or down (health 0).
 *   - Fall-through guard: a far spot's ground may still be streaming in. For 4 s after the jump, if Cole
 *     has dropped 2.5 m below the spot without moving away (within 2 m), he is placed again (the same call).
 *   - The camera: turned to the saved yaw/pitch through the free camera hook (tpc_frame above).
 * Unused = nothing: Cole's update hook is set only while a Go waits or the guard runs; the free camera
 * hook only reads two floats into host memory for Save this spot.
 * Storage: %APPDATA%\inFAMOUS2-port\spots.ini (PS3_CHEATS_SPOTS=<file> instead, "" = this session only;
 * with PS3_OVERLAY_INI set, e.g. test runs, next to that file), seeded with two places in New Marais (the
 * cathedral spire's top, the cathedral roof below it); read again when it changes on disk. */
constexpr uint32_t kToc = 0x009DB730;              /* the EBOOT's TOC: every function descriptor's r2 */
constexpr uint32_t kOOrigin = 0x14, kOParent = 0x1C, kOOff = 0x58, kOAttached = 0x64, kOPos = 0x90, kOWorld = 0xB0;
constexpr uint32_t kOriginPos = 0x80;
constexpr float kSpotLift = 5.0f;
constexpr double kSpotReqSecs = 2.0;               /* a Go Cole's update has not served by then is dropped */
constexpr double kSettleSecs = 4.0;
constexpr float kSettleDrop = 250.0f, kSettleNear = 200.0f;

struct Spot {
    char name[32];                 /* [31] stays 0 */
    float x, y, z;                 /* Cole's world position (game units, ~cm; z up) */
    float facing;                  /* Cole's yaw (rad) */
    float cyaw, cpitch;            /* the free camera's target yaw / pitch (D+0x1C0 / +0x1C4, rad) */
    bool has_facing, has_cam;
};
constexpr int kSpotsMax = 32;
const Spot kSeeds[] = {
    { "Cathedral spire", -43695.0f, -22816.0f, 7786.0f, 0.0f, 0.0f, 0.0f, false, false },     /* the spire's top */
    { "Cathedral roof", -42024.77f, -24236.1f, 6180.76f, 0.0f, 0.0f, 0.0f, false, false },    /* below the spire */
};
/* The list and its file: the menu's thread (page, refresh) and F7's event thread, under s_spots_lock.
 * Names live in fixed buffers that s_spot_names point at for good (the page's combo reads them). */
std::mutex s_spots_lock;
Spot s_spots[kSpotsMax];
const char* s_spot_names[kSpotsMax];
int s_nspots, s_spot_sel;
char s_spots_path[600];
unsigned long long s_spots_mtime;
double s_spots_check_t = -10.0;
char s_spot_help[800], s_go_help[700];
/* A Go waiting for Cole's update, and the last outcome: any thread, never held over I/O. */
std::mutex s_req_lock;
Spot s_req;
uint32_t s_req_cole;
double s_req_t;
char s_spot_status[240] = "";
/* The guard (guest thread). */
uint32_t s_settle_cole;
double s_settle_t0;
float s_settle_p[3], s_settle_yaw;
int s_settle_n;

void spot_status(bool notice, const char* fmt, ...)
{
    char msg[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    { std::lock_guard<std::mutex> g(s_req_lock); snprintf(s_spot_status, sizeof s_spot_status, "%s", msg); }
    fprintf(stderr, "[cheats] spot: %s\n", msg);
    if (notice) ps3_cheats_notice(msg, 5.0);
}

void spots_path_init()
{
    char* p = s_spots_path;
    const size_t n = sizeof s_spots_path;
    p[0] = 0;
    if (const char* e = getenv("PS3_CHEATS_SPOTS")) { snprintf(p, n, "%s", e); return; }   /* "" = session only */
    if (const char* ini = getenv("PS3_OVERLAY_INI")) {          /* the menu's file elsewhere (tests): next to it */
        if (!*ini) return;
        snprintf(p, n, "%s", ini);
        char* a = strrchr(p, '\\');
        char* b = strrchr(p, '/');
        char* s = a > b ? a : b;
        if (s) snprintf(s + 1, n - (size_t)(s + 1 - p), "spots.ini");
        else snprintf(p, n, "spots.ini");
        return;
    }
#ifdef _WIN32
    if (const char* base = getenv("APPDATA")) if (*base) snprintf(p, n, "%s\\inFAMOUS2-port\\spots.ini", base);
#else
    if (const char* x = getenv("XDG_CONFIG_HOME")) { if (*x) { snprintf(p, n, "%s/infamous2-port/spots.ini", x); return; } }
    if (const char* h = getenv("HOME")) if (*h) snprintf(p, n, "%s/.config/infamous2-port/spots.ini", h);
#endif
}

unsigned long long file_mtime(const char* p)
{
    if (!p[0]) return 0;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &a)) return 0;
    return ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
#else
    struct stat st;
    return stat(p, &st) ? 0 : (unsigned long long)st.st_mtime;
#endif
}

char* trim(char* s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

/* "name = x y z [facing [camera_yaw camera_pitch]]" */
bool spot_parse(char* line, Spot* s)
{
    char* eq = strchr(line, '=');
    if (!eq) return false;
    *eq = 0;
    const char* nm = trim(line);
    if (!*nm) return false;
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name - 1, "%s", nm);
    float v[6];
    int k = 0;
    const char* q = eq + 1;
    while (k < 6) {
        char* e;
        const float f = strtof(q, &e);
        if (e == q || !std::isfinite(f)) break;
        v[k++] = f;
        q = e;
    }
    if (k < 3) return false;
    for (int i = 0; i < 3; i++) if (v[i] < -1e6f || v[i] > 1e6f) return false;   /* the city is within +-1e5 */
    s->x = v[0]; s->y = v[1]; s->z = v[2];
    /* angles from a hand-edited file: into -pi..pi (wrap_pi's loop on the game's thread must stay short) */
    const float two_pi = 6.28318531f;
    if (k >= 4) { s->facing = remainderf(v[3], two_pi); s->has_facing = true; }
    if (k >= 6) { s->cyaw = remainderf(v[4], two_pi); s->cpitch = clampf(v[5], -1.6f, 1.6f); s->has_cam = true; }
    return true;
}

void spots_names_locked() { for (int i = 0; i < kSpotsMax; i++) s_spot_names[i] = s_spots[i].name; }

/* The file, or the seeds when there is none (the file is written at the first change). */
void spots_load_locked()
{
    FILE* f = s_spots_path[0] ? fopen(s_spots_path, "r") : nullptr;
    spots_names_locked();
    if (!f) {
        s_nspots = 0;
        for (const Spot& s : kSeeds) s_spots[s_nspots++] = s;
        s_spot_sel = 0;
        s_spots_mtime = 0;
        return;
    }
    char line[512], sel[32] = "";
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char* p = trim(line);
        if (!*p || *p == ';' || *p == '#') continue;
        if (!strncmp(p, "selected:", 9)) { snprintf(sel, sizeof sel, "%s", trim(p + 9)); continue; }
        Spot s;
        if (n < kSpotsMax && spot_parse(p, &s)) s_spots[n++] = s;
    }
    fclose(f);
    s_nspots = n;
    s_spot_sel = 0;
    for (int i = 0; i < n; i++) if (!strcmp(s_spots[i].name, sel)) s_spot_sel = i;
    s_spots_mtime = file_mtime(s_spots_path);
}

bool spots_write_locked()
{
    if (!s_spots_path[0]) return true;                        /* this session only */
    char dir[600], tmp[620];
    snprintf(dir, sizeof dir, "%s", s_spots_path);
    char* a = strrchr(dir, '\\');
    char* b = strrchr(dir, '/');
    if (char* s = a > b ? a : b) {
        *s = 0;
#ifdef _WIN32
        CreateDirectoryA(dir, NULL);                          /* there already: fails, fine */
#endif
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", s_spots_path);
    FILE* f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "; inFAMOUS 2 port: saved spots for F2 > Cheats > Saved spots (F7 goes to the selected one).\n"
               "; One spot a line:  name = x y z [facing [camera_yaw camera_pitch]]\n"
               ";   x y z: Cole's position (game units, about 1 cm; z is up). facing: the way Cole looks;\n"
               ";   camera_yaw camera_pitch: the camera's angles (radians). A spot without them leaves Cole's\n"
               ";   facing and the camera as they are. Rename, edit or delete lines freely: the game reads\n"
               ";   the file again when it changes (while the Cheats page is open).\n");
    if (s_nspots) fprintf(f, "selected: %s\n", s_spots[s_spot_sel].name);
    for (int i = 0; i < s_nspots; i++) {
        const Spot& s = s_spots[i];
        fprintf(f, "%s = %.2f %.2f %.2f", s.name, (double)s.x, (double)s.y, (double)s.z);
        if (s.has_facing || s.has_cam) fprintf(f, " %.4f", (double)s.facing);
        if (s.has_cam) fprintf(f, " %.4f %.4f", (double)s.cyaw, (double)s.cpitch);
        fprintf(f, "\n");
    }
    const bool ok = !ferror(f);
    if (fclose(f) != 0 || !ok) { remove(tmp); return false; }
#ifdef _WIN32
    if (!MoveFileExA(tmp, s_spots_path, MOVEFILE_REPLACE_EXISTING)) { remove(tmp); return false; }
#else
    if (rename(tmp, s_spots_path)) { remove(tmp); return false; }
#endif
    s_spots_mtime = file_mtime(s_spots_path);
    return true;
}

/* Cole's world position as the game computes it (0x004AAF28 for an object no parent holds: +0x90 plus its
 * origin's +0x80) and his facing (the yaw of the world matrix +0xB0's first row: (cos, sin) x scale).
 * false: a parent holds him (+0x64 with +0x1C, as 0x004ABDFC tests): climbing, riding -- not a free spot. */
bool cole_world(uint32_t c, float p[3], float* yaw)
{
    if (vm_read8(c + kOAttached) && vm_read32(c + kOParent)) return false;
    const uint32_t org = vm_read32(c + kOOrigin);
    for (uint32_t k = 0; k < 3; k++)
        p[k] = rdf(c + kOPos + 4u * k) + (gptr(org) ? rdf(org + kOriginPos + 4u * k) : 0.0f);
    *yaw = atan2f(rdf(c + kOWorld + 4u), rdf(c + kOWorld));
    return true;
}

/* The game's own placing (func_004AE348): Cole at p (world), upright, facing yaw. Guest thread, from Cole's
 * update after the game's body (a call boundary: the guest stack below r1 is free). Layout below r1:
 * [frame .. +0x30) the call's header (it saves LR at frame+0x10), [+0x30 .. +0x70) the parameter area,
 * [+0x80 .. +0xC0) the matrix (16-byte aligned), up to sp; the call's own frame goes below `frame`. */
void place(struct ppu_context* ctx, uint32_t c, const float p[3], float yaw)
{
    const uint32_t sp = (uint32_t)ctx->gpr[1] & ~0xFu;
    const uint32_t frame = sp - 0x100u, m = frame + 0x80u;
    const float cs = cosf(yaw), sn = sinf(yaw);
    const float rows[16] = { cs, sn, 0.0f, 0.0f,   -sn, cs, 0.0f, 0.0f,   0.0f, 0.0f, 1.0f, 0.0f,
                             p[0], p[1], p[2], 1.0f };   /* w = 1: SetWorldPosition adds target - current to all 4 lanes */
    for (uint32_t i = 0; i < 16; i++) wrf(m + 4u * i, rows[i]);
    call_game(ctx, func_004AE348, [&](struct ppu_context* x) {
        x->gpr[1] = frame; x->gpr[2] = kToc; x->gpr[3] = c; x->gpr[4] = m; });
}

void spot_go(struct ppu_context* ctx, uint32_t c, const Spot& s)
{
    float p0[3], yaw0;
    if (vm_read8(c + kOOff)) { spot_status(true, "Go to %s: not now (Cole is in a scripted moment).", s.name); return; }
    if (!cole_world(c, p0, &yaw0)) { spot_status(true, "Go to %s: Cole is holding on to something; let go first.", s.name); return; }
    if (!(rdf(c + kHp) > 0.0f)) { spot_status(true, "Go to %s: not now (Cole is down).", s.name); return; }
    const float p[3] = { s.x, s.y, s.z + kSpotLift };
    const float yaw = s.has_facing ? s.facing : std::isfinite(yaw0) ? yaw0 : 0.0f;
    place(ctx, c, p, yaw);
    float p1[3] = { 0, 0, 0 }, yaw1 = 0.0f;
    cole_world(c, p1, &yaw1);
    fprintf(stderr, "[cheats] spot: %s: from (%.1f %.1f %.1f) facing %.3f to (%.1f %.1f %.1f) facing %.3f: now (%.1f %.1f %.1f) "
            "facing %.3f, velocity copy (%.1f %.1f %.1f)\n", s.name, (double)p0[0], (double)p0[1], (double)p0[2], (double)yaw0,
            (double)p[0], (double)p[1], (double)p[2], (double)yaw, (double)p1[0], (double)p1[1], (double)p1[2], (double)yaw1,
            (double)rdf(c + 0x1570), (double)rdf(c + 0x1574), (double)rdf(c + 0x1578));
    s_settle_cole = c;
    s_settle_t0 = now_s();
    memcpy(s_settle_p, p, sizeof s_settle_p);
    s_settle_yaw = yaw;
    s_settle_n = 0;
    s_settle_on.store(true);
    if (s.has_cam && s_mouse_hook && ppu_hook_exists(kFnFreeCam)) {   /* the free camera hook turns it (tpc_frame) */
        s_tpc_yaw = s.cyaw;
        s_tpc_pitch = s.cpitch;
        s_tpc_t0 = now_s();
        s_tpc_cancel = false;
        s_tpc_on.store(1, std::memory_order_release);
    }
    spot_status(true, "Went to %s.", s.name);
}

/* The fall-through guard, each of Cole's updates for kSettleSecs after a Go. */
void spot_settle(struct ppu_context* ctx, uint32_t c)
{
    const double t = now_s() - s_settle_t0;
    float p[3], yaw;
    if (c != s_settle_cole || t > kSettleSecs || !cole_world(c, p, &yaw)) {
        s_settle_on.store(false);
        fprintf(stderr, "[cheats] spot: guard over after %.2f s, %d re-placement(s)\n", t, s_settle_n);
        return;
    }
    if (fabsf(p[0] - s_settle_p[0]) < kSettleNear && fabsf(p[1] - s_settle_p[1]) < kSettleNear &&
        p[2] < s_settle_p[2] - kSettleDrop && !vm_read8(c + kOOff) && rdf(c + kHp) > 0.0f) {
        place(ctx, c, s_settle_p, s_settle_yaw);
        s_settle_n++;
        fprintf(stderr, "[cheats] spot: Cole fell to z %.1f (spot %.1f) %.2f s after the jump: placed again (%d)\n",
                (double)p[2], (double)s_settle_p[2], t, s_settle_n);
    }
}

void spot_tick(struct ppu_context* ctx, uint32_t c)
{
    if (g_tp_pending.exchange(0, std::memory_order_acquire)) {
        Spot s;
        uint32_t want;
        double t;
        { std::lock_guard<std::mutex> g(s_req_lock); s = s_req; want = s_req_cole; t = s_req_t; }
        if (c != want || now_s() - t > kSpotReqSecs)
            spot_status(true, "Go to %s: not done (the game was paused or loading); press it again in play.", s.name);
        else
            spot_go(ctx, c, s);
    }
    if (s_settle_on.load()) spot_settle(ctx, c);
    if (!g_tp_pending.load() && !s_settle_on.load()) hooks_update();   /* Cole's update hook off again if unused */
}

/* ---- the Saved spots entries (menu thread; Go also from F7's event thread) ---- */
int apply_spot(ps3_cheat* c)
{
    std::lock_guard<std::mutex> g(s_spots_lock);
    if (!s_nspots) return 0;
    const int i = (int)c->value;
    s_spot_sel = i < 0 ? 0 : i >= s_nspots ? s_nspots - 1 : i;
    spots_write_locked();                                     /* the pick is kept (the file's "selected:") */
    return 1;
}

int apply_spot_go(ps3_cheat*)
{
    const uint32_t c = player();
    if (!c) { spot_status(true, "Go to saved spot: Cole is not in play."); return 0; }
    Spot s;
    bool have = false;
    {
        std::lock_guard<std::mutex> g(s_spots_lock);
        if (s_nspots) { s = s_spots[s_spot_sel]; have = true; }
    }
    if (!have) { spot_status(true, "Go to saved spot: no saved spots (Save this spot adds one)."); return 0; }
    { std::lock_guard<std::mutex> g(s_req_lock); s_req = s; s_req_cole = c; s_req_t = now_s(); }
    g_tp_pending.store(1, std::memory_order_release);
    hooks_update();                                           /* Cole's update hook: it jumps there next frame */
    return 1;
}

int apply_spot_save(ps3_cheat*)
{
    const uint32_t c = player();
    if (!c) { spot_status(true, "Save this spot: Cole is not in play."); return 0; }
    float p[3], yaw;
    if (!cole_world(c, p, &yaw)) { spot_status(true, "Save this spot: Cole is holding on to something; let go first."); return 0; }
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
        spot_status(true, "Save this spot: Cole's position does not read right now; try again.");
        return 0;
    }
    Spot s;
    memset(&s, 0, sizeof s);
    s.x = p[0]; s.y = p[1]; s.z = p[2];
    s.facing = yaw; s.has_facing = std::isfinite(yaw);
    const double ct = s_cam_t.load(std::memory_order_acquire);
    if (ct >= 0.0 && now_s() - ct < 1.0) {                    /* the free camera ran within the last second */
        uint32_t b = s_cam_yaw_bits.load(std::memory_order_relaxed); memcpy(&s.cyaw, &b, 4);
        b = s_cam_pitch_bits.load(std::memory_order_relaxed); memcpy(&s.cpitch, &b, 4);
        s.has_cam = std::isfinite(s.cyaw) && std::isfinite(s.cpitch);
    }
    bool full = false, wrote = false;
    {
        std::lock_guard<std::mutex> g(s_spots_lock);
        if (s_nspots >= kSpotsMax) full = true;
        else {
            int k = 1;                                        /* "Spot <k> (MM-DD HH:MM)", k not used yet */
            for (;; k++) {
                char pre[16];
                snprintf(pre, sizeof pre, "Spot %d", k);
                const size_t l = strlen(pre);
                bool used = false;
                for (int i = 0; i < s_nspots; i++)
                    if (!strncmp(s_spots[i].name, pre, l) && (s_spots[i].name[l] == 0 || s_spots[i].name[l] == ' ')) used = true;
                if (!used) break;
            }
            const time_t now = time(nullptr);
            struct tm tmv;
#ifdef _WIN32
            localtime_s(&tmv, &now);
#else
            localtime_r(&now, &tmv);
#endif
            snprintf(s.name, sizeof s.name - 1, "Spot %d (%02d-%02d %02d:%02d)", k, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
            s_spots[s_nspots++] = s;
            s_spot_sel = s_nspots - 1;
            wrote = spots_write_locked();
        }
    }
    if (full) { spot_status(true, "Save this spot: the list is full (%d); delete some lines in %s.", kSpotsMax, s_spots_path); return 0; }
    spot_status(true, "Saved %s: x %.0f y %.0f z %.0f%s%s", s.name, (double)s.x, (double)s.y, (double)s.z,
                s.has_cam ? "" : " (without the camera: the free camera was not running -- aiming or paused?)",
                wrote ? "" : " -- the file could not be written, kept for this session");
    return 1;
}

ps3_cheat s_spot = { "spot", "Spot", s_spot_help, "Saved spots", PS3_CHEAT_CHOICE, apply_spot,
    0, 0, 0, NULL, s_spot_names, 0, NULL, PS3_CHEAT_NO_REMEMBER };
ps3_cheat s_spotgo = { "spotgo", "Go to saved spot (F7)", s_go_help, "Saved spots", PS3_CHEAT_ACTION, apply_spot_go,
    0, 0, 0, NULL, NULL, 0, NULL, PS3_CHEAT_HOTKEY };
ps3_cheat s_spotsave = { "spotsave", "Save this spot",
    "Adds where Cole stands now, the way he faces and the camera's angles as a new spot (Spot 1, Spot 2, ...) "
    "and picks it.", "Saved spots", PS3_CHEAT_ACTION, apply_spot_save };

/* The page's texts and the combo (menu thread; also at start). spots.ini changed on disk: read again. */
void spots_texts_locked()
{
    s_spot.choices = s_spot_names;
    s_spot.n_choices = s_nspots;
    s_spot.value = s_spot_sel;
    const char* where = s_spots_path[0] ? s_spots_path : "this session only (PS3_CHEATS_SPOTS is empty)";
    if (s_nspots) {
        const Spot& s = s_spots[s_spot_sel];
        snprintf(s_spot_help, sizeof s_spot_help, "x %.0f  y %.0f  z %.0f, %s, %s. Spots are kept in %s: rename or delete "
                 "them there.", (double)s.x, (double)s.y, (double)s.z, s.has_facing ? "facing as saved" : "facing as Cole is",
                 s.has_cam ? "camera as saved" : "camera as it is", where);
    } else
        snprintf(s_spot_help, sizeof s_spot_help, "No saved spots: Save this spot adds one. Spots are kept in %s.", where);
    char st[240];
    { std::lock_guard<std::mutex> g(s_req_lock); snprintf(st, sizeof st, "%s", s_spot_status); }
    snprintf(s_go_help, sizeof s_go_help, "Puts Cole at the picked spot, standing still and facing the saved way, and turns "
             "the camera to the saved view (the game's own placing, as when it puts Cole back after a test play). Use it "
             "in free play with Cole on his feet. F7 does it too, with the menu open or closed.%s%s",
             st[0] ? "\nLast: " : "", st);
}

void spots_refresh()
{
    std::lock_guard<std::mutex> g(s_spots_lock);
    const double t = now_s();
    if (t - s_spots_check_t >= 1.0) {
        s_spots_check_t = t;
        const unsigned long long mt = file_mtime(s_spots_path);
        if (mt && mt != s_spots_mtime) {
            spots_load_locked();
            fprintf(stderr, "[cheats] spot: %s changed: %d spot(s)\n", s_spots_path, s_nspots);
        }
    }
    spots_texts_locked();
}

/* Which hooks are needed now: each is set only while a cheat (or a test knob) uses it. */
void hooks_update()
{
    const bool tick = g_god || g_energy || g_xp_pending.load() != 0 || g_refill || s_probe || s_timeline ||
                      !s_env_done || g_karma_add.load() != 0 || g_recompute || s_snap_left > 0 ||
                      g_tp_pending.load() != 0 || s_settle_on.load();
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
    s_spotgo.unavailable = ppu_hook_exists(kFnColeUpdate) ? NULL : lift;   /* Save this spot only reads memory */
    spots_refresh();
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
                             &s_kside, &s_kmult, &s_kxp, &s_kblue, &s_kred, &s_spot, &s_spotgo, &s_spotsave };
        for (ps3_cheat* c : all) ps3_cheat_register(c);
        spots_path_init();                    /* saved spots: the file (or the seeds), read once here */
        {
            std::lock_guard<std::mutex> g(s_spots_lock);
            spots_load_locked();
            spots_texts_locked();
        }
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
