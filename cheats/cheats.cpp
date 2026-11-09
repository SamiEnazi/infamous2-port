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

#include <atomic>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
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
/* Movement (snapshots, runs cheats5-7; for a later glide cheat): position +0x1560, velocity +0x1570
 * (x, y, z up; ~cm), copies at +0x1C14/+0x1C20; gravity ~-1500/s^2, Static Thrusters ~-350/s^2.
 * Writing the velocity here does nothing (run cheats7): the physics state lives elsewhere. */

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
/* glide research (tests): snapshots of Cole's character object */
int s_snap_left, s_snap_every = 2;
long long s_snap_n;

void hooks_update();

/* ---- the hooks ----------------------------------------------------------------------- */
void timeline_poll(struct ppu_context* ctx, uint32_t c);
void probe_tick(uint32_t c);

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
    if (s_probe) probe_tick(c);
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

ps3_cheat s_god = { "god", "Invincibility",
    "Cole's health never drops (healing still works). If you get stuck where the game expects you to "
    "die, such as deep water, switch it off.", "Cole", PS3_CHEAT_TOGGLE, apply_god };
ps3_cheat s_energy = { "energy", "Infinite energy",
    "Electricity refills every frame: use any power as often as you like.", "Cole", PS3_CHEAT_TOGGLE, apply_energy };
ps3_cheat s_jump = { "jump", "Super jump", "How high Cole jumps.", "Cole", PS3_CHEAT_CHOICE, apply_jump,
    0, 0, 0, NULL, kJump, 5 };
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
        fprintf(stderr, "[cheats] timeline starts\n");
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
            "boosted %d kblocked %d kscaled %d\n",
            c, rdf(c + kHp), rdf(c + kHpMax), rdf(c + kEnergy), rdf(c + kEnergyMax),
            gptr(s_xp) ? vm_read32(s_xp) : 0, gptr(s_xp) ? vm_read32(s_xp + 4) : 0, rdf(kKarma),
            s_ticks.load(), s_blocked.load(), s_boosted.load(), s_kblocked.load(), s_kscaled.load());
}

struct Init {
    Init()
    {
        ps3_cheat* all[] = { &s_god, &s_energy, &s_maxbat, &s_jump, &s_onehit, &s_speed, &s_noattack, &s_xpc,
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
        if (!getenv("PS3_CHEATS")) s_env_done = true;
        hooks_update();
        if (s_probe || s_timeline || !s_env_done)
            fprintf(stderr, "[cheats] test knobs: probe %d, timeline %d entries, PS3_CHEATS %s\n",
                    s_probe ? 1 : 0, s_n_at, s_env_done ? "-" : getenv("PS3_CHEATS"));
    }
};
Init s_init;

} // namespace
