/* ppu_fs_xcombine.h: two mods' Deltas (ppu_fs_xmerge.h) on the same game file
 * combined into one, so that both mods work.
 *
 * What two mod tools do to the same game file, and how it is combined:
 *  - each adds its own new bytes (its "region"): both are kept, the first mod's
 *    first. A pointer either mod aimed at the place where both inserted keeps
 *    the first mod's meaning.
 *  - both copy the same game list (or script block) somewhere new with their own
 *    entries added, and aim the game's pointer at the copy: the combined file
 *    gets one more copy, the game's entries plus the first mod's plus the
 *    second's, the pointer (and the list's count or end pointer, and pointers
 *    into it) aimed at that. Entries are compared by content, pointers by what
 *    they point to.
 *  - both add entries at the end of the same table (each insert is "its
 *    entries, then its other data"): the entries are put together, then each
 *    mod's other data.
 *  - both make the same insert (same places, same sizes, same other edits) and
 *    fill its slots: one insert; a slot both filled differently (a record with
 *    a link name) goes to a slot both left empty.
 *  - both raise the same counter: both raises.
 *  - a chunk both left at its size (the game needs some chunks to keep their
 *    size) keeps it: zero padding at the end of new bytes is given back.
 *  - link-table entries: the game's kept, each mod's new ones added, the same
 *    one once.
 * Anything else both changed differently: the mod "prefer" names wins, and it
 * is logged. The result is checked by writing it out and reading it back. */
#pragma once
#include "ppu_fs_xmerge.h"
#include <functional>
#include <stdio.h>
#include <stdlib.h>

namespace xm {

static std::string hx(uint64_t v) { char b[24]; snprintf(b, sizeof b, "%llx", (unsigned long long)v); return b; }

static void for_each_ref(Delta& D, const std::function<void(Ref&)>& f)
{
    for (Blob& b : D.blobs) for (auto& kv : b.ptrs) f(kv.second.to);
    for (auto& kv : D.ptr_set) f(kv.second.to);
    for (auto& kv : D.lists) for (Item& it : kv.second.items) if (it.g < 0) f(it.e.at);
}

/* an entry compared without where its argument words are kept */
static bool same_entry(const Entry& a, const Entry& b)
{
    if (a.kind != b.kind || a.at != b.at || a.sig != b.sig || memcmp(a.key, b.key, 16)) return false;
    const size_t n = a.kind >= 3 ? 2 : 0;
    if (a.w.size() != b.w.size() || a.w.size() < n) return false;
    return std::equal(a.w.begin(), a.w.end() - (long)n, b.w.begin());
}

struct Combine {
    const Arc& G; const Delta& A; Delta B; int prefer;
    Delta M;
    std::vector<std::string>& log;
    std::string err;
    std::vector<Unit> gu;
    std::map<uint32_t, uint32_t> gsec;          /* G pointer word index -> section */
    std::set<uint32_t> gcid;                    /* G class-id addresses */
    std::vector<uint32_t> gtargets;             /* every G pointer value, sorted */
    std::vector<int> bmap;                      /* B blob -> M blob */
    std::vector<int> origin;                    /* M blob -> 0 A, 1 B, 2 both (one insert), 3 combined copies */
    std::set<uint32_t> parallel_units, concat_units;
    std::set<uint32_t> fixed_words;             /* words the list copies set (counts): bytes merge leaves them */
    size_t conflicts = 0;                       /* places both changed differently (the preferred one's kept) */
    bool swap_better = false;                   /* B names more of a same insert's records than A: better to move A's */

    Combine(const Arc& g, const Delta& a, const Delta& b, int pref, std::vector<std::string>& lg)
        : G(g), A(a), B(b), prefer(pref), log(lg) {}

    bool fail(const std::string& e) { err = e; return false; }
    int chunk_of_blob(const Blob& b) const { return b.chunk >= 0 ? b.chunk : gu[b.unit].chunk; }
    uint32_t gword(uint32_t a) const { return rd32(&G.data[a]); }
    bool gptr(uint32_t a) const { return !(a & 3) && gsec.count(a >> 2); }
    /* a word as the mod has it (game word with the mod's byte edits) */
    static uint32_t dword(const Arc& G, const Delta& D, uint32_t a) {
        uint8_t b[4]; memcpy(b, &G.data[a], 4);
        for (int i = 0; i < 4; i++) { auto it = D.bytes.find(a + i); if (it != D.bytes.end()) b[i] = it->second; }
        return rd32(b);
    }
    std::string fname(uint32_t a) const { const int f = G.file_of(a); return f < 0 ? "?" : "#" + std::to_string(f); }

    bool run();
    bool units_and_blobs();
    bool merge_parallel(Blob& a, const Blob& b, size_t bi, int ma);
    void split_tables();
    bool merge_pointers();
    void merge_bytes();
    bool merge_lists();
    bool merge_tables();
    bool keep_fixed_chunks();
    bool place_copies();
    /* combined copies, waiting for their place */
    struct CWord { uint32_t v = 0; bool ptr = false; Ptr p; bool cid = false; int internal = 0; uint32_t ioff = 0; };
    struct CopyOut {
        uint32_t unit = 0, at = 0; uint32_t align_mod = 0;
        std::vector<CWord> words;
        std::vector<std::pair<std::pair<size_t, size_t>, Entry>> entries;   /* (list, entry), entry.at.off = word offset */
        std::vector<std::pair<uint32_t, uint32_t>> set_ptrs;                /* (G location, offset in the copy) */
        std::vector<std::pair<uint32_t, uint32_t>> set_count;               /* (G word, value) */
        std::vector<std::pair<std::pair<int, uint32_t>, uint32_t>> moved;   /* ((blob, off), copy offset): refs into the old copies */
        uint32_t base = 0;                                                   /* its offset in the combined blob */
    };
    std::vector<CopyOut> copies;
};

/* ---- 1. which inserts are the same insert, and the blobs of both ------------------ */

bool Combine::merge_parallel(Blob& a, const Blob& b, size_t bi, int ma)
{
    /* The records of this insert, seen through a window on record boundaries: the
       insert may start inside a record (when a new record begins like the game's
       next one, the bytes they share are counted as the game's), so the window
       takes the game bytes before and after it up to the record edges. */
    const uint32_t n = (uint32_t)a.b.size();
    const Unit& U = gu[a.unit];
    uint32_t r = 0, pre = 0, post = 0;
    {
        auto gcd = [](uint32_t x, uint32_t y) { while (y) { const uint32_t t = x % y; x = y; y = t; } return x; };
        /* the record size: first from where both mods put new names around the insert
           (their positions differ by whole records), else from the game's names in the unit */
        std::vector<int64_t> pos;   /* relative to the insert, in the mod's layout */
        auto add = [&](const Delta& D, int blob) {
            for (const auto& kv : D.lists) for (const Item& it : kv.second.items) {
                if (it.g >= 0 || it.e.kind != 1) continue;
                const Ref& x = it.e.at;
                if (x.blob == blob && x.off < n) pos.push_back(x.off);
                else if (x.blob == -1 && x.off + n >= a.at && x.off < a.at) pos.push_back((int64_t)x.off - (int64_t)a.at);
                else if (x.blob == -1 && x.off >= a.at && x.off < a.at + n) pos.push_back((int64_t)n + (x.off - a.at));
            }
        };
        add(A, ma); add(B, (int)bi);
        std::sort(pos.begin(), pos.end());
        pos.erase(std::unique(pos.begin(), pos.end()), pos.end());
        int64_t ref = 0;
        if (pos.size() >= 2) {
            r = n;
            for (size_t i = 1; i < pos.size(); i++) r = gcd(r, (uint32_t)(pos[i] - pos[0]));
            ref = pos[0];
        }
        if (r < 16 || r == n) {
            r = 0;
            std::vector<uint32_t> ga;
            for (const LinkFile& L : G.links) for (const Block& k : L.blocks) for (const Xlnk& x : k.xlnk)
                if (x.addr >= U.g0 && x.addr < U.g1) ga.push_back(x.addr);
            std::sort(ga.begin(), ga.end());
            for (size_t i = 1; i < ga.size(); i++) if (ga[i] - ga[i - 1] < 0x10000) r = gcd(r, ga[i] - ga[i - 1]);
            if (!ga.empty()) ref = (int64_t)ga[0] - (int64_t)a.at;
        }
        if (r >= 16 && !(n % r)) {
            const uint32_t ph = (uint32_t)(((ref % (int64_t)r) + (int64_t)r) % (int64_t)r);   /* a record edge, from the insert's start */
            pre = ph ? r - ph : 0; post = ph;
            if (a.at < U.g0 + pre || a.at + post > U.g1) r = 0;
        } else r = 0;
    }
    /* a reference as a window offset */
    auto wof = [&](const Ref& x, int blob, uint32_t& o) -> bool {
        if (x.blob == blob) { if (x.off >= n) return false; o = pre + x.off; return true; }
        if (x.blob != -1) return false;
        if (x.off + pre >= a.at && x.off < a.at) { o = x.off + pre - a.at; return true; }
        if (x.off >= a.at && x.off < a.at + post) { o = pre + n + (x.off - a.at); return true; }
        return false;
    };
    /* the link names (XLNK) each mod gave to records of this insert: (window offset, (list, item)) */
    auto xl = [&](const Delta& D, int blob) {
        std::vector<std::pair<uint32_t, std::pair<std::pair<size_t, size_t>, size_t>>> v;
        for (const auto& kv : D.lists) for (size_t i = 0; i < kv.second.items.size(); i++) {
            const Item& it = kv.second.items[i];
            uint32_t o;
            if (it.g < 0 && it.e.kind == 1 && wof(it.e.at, blob, o)) v.push_back({ o, { kv.first, i } });
        }
        return v;
    };
    const auto xa = xl(A, ma), xb = xl(B, (int)bi);
    bool clash = false;
    for (const auto& p : xb) for (const auto& q : xa)
        if (p.first == q.first) {
            const Entry& eb = B.lists.at(p.second.first).items[p.second.second].e;
            const Entry& ea = A.lists.at(q.second.first).items[q.second.second].e;
            if (memcmp(ea.key, eb.key, 16)) clash = true;
        }
    Bytes out = a.b;
    std::vector<bool> b_done(n, false);
    if (clash) {
        if (!r) return fail("both mods filled the same new record in " + fname(a.at) + " and its size is not clear");
        if (getenv("XC_DEBUG")) fprintf(stderr, "[xc] clash in %s: insert at %x, %u bytes, records of %u, window %u+%u+%u, A names %zu, B names %zu\n",
                                        fname(a.at).c_str(), a.at, n, r, pre, n, post, xa.size(), xb.size());
        const uint32_t wn = pre + n + post, nr = wn / r;
        /* the window as each mod has it */
        auto win = [&](const Delta& D, const Bytes& mid) {
            Bytes w(wn);
            for (uint32_t k = 0; k < pre; k++) { const uint32_t g = a.at - pre + k; auto f = D.bytes.find(g); w[k] = f != D.bytes.end() ? f->second : G.data[g]; }
            memcpy(&w[pre], mid.data(), n);
            for (uint32_t k = 0; k < post; k++) { const uint32_t g = a.at + k; auto f = D.bytes.find(g); w[pre + n + k] = f != D.bytes.end() ? f->second : G.data[g]; }
            return w;
        };
        const Bytes wa = win(A, a.b), wb = win(B, b.b);
        /* the pointers in a window record of B: (offset in it, pointer) */
        auto bptrs = [&](uint32_t i) {
            std::vector<std::pair<uint32_t, Ptr>> v;
            for (uint32_t k = 0; k < r; k += 4) {
                const uint32_t o = i * r + k;
                if (o >= pre && o < pre + n) { auto f = b.ptrs.find(o - pre); if (f != b.ptrs.end()) v.push_back({ k, f->second }); continue; }
                const uint32_t g = o < pre ? a.at - pre + o : a.at + (o - pre - n);
                auto f = B.ptr_set.find(g);
                if (f != B.ptr_set.end()) { v.push_back({ k, f->second }); continue; }
                if (gptr(g) && !B.ptr_del.count(g)) v.push_back({ k, Ptr{ gsec.at(g >> 2), Ref{ -1, gword(g) } } });
            }
            return v;
        };
        /* the pointer offsets of a record inside the insert, in A */
        auto aoffs = [&](uint32_t j) {
            std::vector<uint32_t> v;
            for (auto f = a.ptrs.lower_bound(j * r - pre); f != a.ptrs.end() && f->first < (j + 1) * r - pre; ++f) v.push_back(f->first - (j * r - pre));
            return v;
        };
        std::vector<bool> ua(nr, false), ub(nr, false), taken(nr, false);
        for (const auto& q : xa) ua[q.first / r] = true;
        for (const auto& p : xb) ub[p.first / r] = true;
        /* the mod using fewer of these records keeps none in place: the other's records may name each other by place */
        if (std::count(ua.begin(), ua.end(), true) < std::count(ub.begin(), ub.end(), true)) swap_better = true;
        std::map<uint32_t, uint32_t> move;   /* B record -> slot */
        for (uint32_t i = 0; i < nr; i++) {
            if (!ub[i]) continue;
            const bool same = !memcmp(&wa[i * r], &wb[i * r], r);
            bool same_names = true;
            for (const auto& p : xb) if (p.first / r == i) {
                bool f = false;
                for (const auto& q : xa) if (q.first == p.first && !memcmp(A.lists.at(q.second.first).items[q.second.second].e.key,
                                                                         B.lists.at(p.second.first).items[p.second.second].e.key, 16)) f = true;
                if (!f) same_names = false;
            }
            if (!ua[i]) {   /* a record only B named: its bytes (the part in the insert) */
                for (uint32_t k = 0; k < r; k++) { const uint32_t o = i * r + k; if (o >= pre && o < pre + n) out[o - pre] = wb[o]; }
                taken[i] = true;
                continue;
            }
            if (same && same_names) continue;
            const auto bp = bptrs(i);
            std::vector<uint32_t> bo; for (const auto& x : bp) bo.push_back(x.first);
            uint32_t j = 0;
            for (; j < nr; j++) {
                if (ua[j] || ub[j] || taken[j] || memcmp(&wa[j * r], &wb[j * r], r)) continue;
                if (j * r < pre || (j + 1) * r > pre + n) continue;   /* only a record wholly in the insert */
                if (aoffs(j) != bo) continue;                          /* laid out like B's (its pointers in the same places) */
                break;
            }
            if (j == nr) return fail("both mods filled the same new record in " + fname(a.at) + " and no empty one is left");
            const uint32_t so = j * r - pre;
            memcpy(&out[so], &wb[i * r], r);
            for (const auto& x : bp) {
                Ptr p = x.second;
                uint32_t o;
                if (wof(p.to, (int)bi, o) && o / r == i) p.to = Ref{ (int)bi, so + (o - i * r) };   /* into itself: follows it */
                a.ptrs[so + x.first].to = p.to;
                B.blobs[bi].ptrs[so + x.first].to = p.to;
            }
            taken[j] = true;
            move[i] = j;
            log.push_back("record " + std::to_string(i) + " of the new ones in " + fname(a.at) + " (" + B.name + ") moved to " +
                          std::to_string(j) + ": " + A.name + " uses it");
        }
        for (uint32_t i = 0; i < nr; i++) if (ub[i]) for (uint32_t k = 0; k < r; k++) { const uint32_t o = i * r + k; if (o >= pre && o < pre + n) b_done[o - pre] = true; }
        /* the second mod's own entries on a moved record follow it (the ones A has alike stay) */
        for (auto& kv : B.lists) for (Item& it : kv.second.items) {
            uint32_t o;
            if (it.g >= 0 || !wof(it.e.at, (int)bi, o)) continue;
            auto mv = move.find(o / r);
            if (mv == move.end()) continue;
            bool twin = false;
            auto fa = A.lists.find(kv.first);
            if (fa != A.lists.end()) for (const Item& y : fa->second.items) {
                if (y.g >= 0) continue;
                uint32_t oy;
                if (!wof(y.e.at, ma, oy) || oy != o) continue;
                Entry e = it.e; e.at = y.e.at;
                if (same_entry(y.e, e)) { twin = true; break; }
            }
            if (!twin) it.e.at = Ref{ (int)bi, mv->second * r - pre + (o % r) };
        }
    }
    uint32_t nb = 0;
    for (uint32_t k = 0; k < n; k++) {
        if (b_done[k] || a.b[k] == b.b[k]) continue;
        if (!a.b[k]) out[k] = b.b[k];
        else if (b.b[k]) { if (prefer) out[k] = b.b[k]; nb++; }
    }
    conflicts += nb;
    if (nb) log.push_back(std::to_string(nb) + " byte(s) of the same new data in " + fname(a.at) + " differ: " + (prefer ? B.name : A.name) + "'s kept");
    a.b = out;
    if (a.ptrs.size() != b.ptrs.size()) return fail("the same insert in " + fname(a.at) + " with other pointers");
    for (uint32_t c : b.cids) a.cids.insert(c);
    return true;
}

bool Combine::units_and_blobs()
{
    std::string e;
    gu = units_of(G, e);
    if (!e.empty()) return fail(e);
    for (const auto& x : ptr_secs(G)) gsec[x.first] = x.second;
    for (const auto& kv : gsec) gtargets.push_back(rd32(&G.data[kv.first << 2]));
    std::sort(gtargets.begin(), gtargets.end());
    gtargets.erase(std::unique(gtargets.begin(), gtargets.end()), gtargets.end());
    for (const LinkFile& L : G.links) for (const Block& b : L.blocks) if (b.magic == "CID ") for (uint32_t l : b.cid) gcid.insert(l << 2);

    /* per unit: the same insert in both? */
    std::map<uint32_t, std::vector<size_t>> ua, ub;
    for (size_t k = 0; k < A.blobs.size(); k++) if (A.blobs[k].chunk < 0) ua[A.blobs[k].unit].push_back(k);
    for (size_t k = 0; k < B.blobs.size(); k++) if (B.blobs[k].chunk < 0) ub[B.blobs[k].unit].push_back(k);
    std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> ca, cb;
    for (const Cut& c : A.cuts) ca[c.unit].push_back({ c.at, c.len });
    for (const Cut& c : B.cuts) cb[c.unit].push_back({ c.at, c.len });
    std::map<size_t, size_t> pair_b;   /* B blob -> A blob */
    for (const auto& kv : ua) {
        const uint32_t u = kv.first;
        auto it = ub.find(u);
        if (it == ub.end()) continue;
        const auto& va = kv.second; const auto& vb = it->second;
        concat_units.insert(u);
        if (va.size() != vb.size() || va.size() + ca[u].size() < 2) continue;
        bool same = ca[u] == cb[u];
        for (size_t i = 0; same && i < va.size(); i++)
            same = A.blobs[va[i]].at == B.blobs[vb[i]].at && A.blobs[va[i]].b.size() == B.blobs[vb[i]].b.size() &&
                   A.blobs[va[i]].in_file == B.blobs[vb[i]].in_file;
        if (!same) continue;
        /* and the same other edits in the unit */
        const uint32_t g0 = gu[u].g0, g1 = gu[u].g1;
        auto ra = A.bytes.lower_bound(g0); auto rb = B.bytes.cbegin(); rb = B.bytes.lower_bound(g0);
        while (same) {
            const bool ea = ra == A.bytes.end() || ra->first >= g1, eb = rb == B.bytes.end() || rb->first >= g1;
            if (ea || eb) { same = ea && eb; break; }
            if (ra->first != rb->first || ra->second != rb->second) same = false;
            ++ra; ++rb;
        }
        if (!same) continue;
        concat_units.erase(u);
        parallel_units.insert(u);
        for (size_t i = 0; i < va.size(); i++) pair_b[vb[i]] = va[i];
    }
    /* units only one mod inserted into are no concern; units both inserted into
       (not as the same insert) are where both mods' counters add up */
    M = A;
    M.name = A.name + "+" + B.name;
    origin.assign(M.blobs.size(), 0);
    for (const auto& kv : pair_b) {
        if (!merge_parallel(M.blobs[kv.second], B.blobs[kv.first], kv.first, (int)kv.second)) return false;
        origin[kv.second] = 2;
    }
    if (!pair_b.empty()) {
        size_t n = 0;
        for (uint32_t u : parallel_units) (void)u, n++;
        log.push_back(std::to_string(n) + " packed file(s) got the same insert from both: made once");
    }
    bmap.assign(B.blobs.size(), -1);
    for (size_t k = 0; k < B.blobs.size(); k++) {
        auto it = pair_b.find(k);
        if (it != pair_b.end()) { bmap[k] = (int)it->second; continue; }
        bmap[k] = (int)M.blobs.size();
        M.blobs.push_back(B.blobs[k]);
        origin.push_back(1);
    }
    /* B's refs in M's numbering */
    for_each_ref(B, [&](Ref& r) { if (r.blob >= 0) r.blob = bmap[(size_t)r.blob]; });
    for (size_t k = A.blobs.size(); k < M.blobs.size(); k++)
        for (auto& kv : M.blobs[k].ptrs) if (kv.second.to.blob >= 0) kv.second.to.blob = bmap[(size_t)kv.second.to.blob];
    /* the parallel blobs' pointers must agree */
    for (const auto& kv : pair_b) {
        const Blob& bb = B.blobs[kv.first];
        Blob& ab = M.blobs[kv.second];
        for (const auto& p : bb.ptrs) {
            const Ref t = p.second.to;
            auto it = ab.ptrs.find(p.first);
            if (it == ab.ptrs.end() || !(it->second.to == t)) return fail("the same insert in " + fname(ab.at) + " points elsewhere in each mod");
        }
    }
    /* cuts */
    for (const Cut& c : B.cuts) {
        bool dup = false;
        for (const Cut& d : M.cuts) {
            if (d.unit == c.unit && d.at == c.at && d.len == c.len) { dup = true; break; }
            if (d.unit == c.unit && c.at < d.at + d.len && d.at < c.at + c.len) return fail("both mods removed overlapping game bytes differently in " + fname(c.at));
        }
        if (!dup) M.cuts.push_back(c);
        else if (!parallel_units.count(c.unit)) log.push_back("both removed the same game bytes in " + fname(c.at) + " (" + hx(c.len) + " bytes): once");
    }
    return true;
}

/* ---- 2. both added entries at the end of the same table ---------------------------- */

void Combine::split_tables()
{
    std::map<std::pair<uint32_t, uint32_t>, std::vector<int>> at;
    for (size_t k = 0; k < M.blobs.size(); k++)
        if (M.blobs[k].chunk < 0 && origin[k] < 2) at[{ M.blobs[k].unit, M.blobs[k].at }].push_back((int)k);
    for (auto& kv : at) {
        std::vector<int> as, bs;
        for (int k : kv.second) (origin[(size_t)k] ? bs : as).push_back(k);
        if (as.size() != 1 || bs.size() != 1) continue;
        const uint32_t X = kv.first.second, u0 = gu[kv.first.first].g0;
        /* the game's pointer pattern just before X */
        std::vector<uint32_t> locs;
        for (auto it = gsec.lower_bound(X >> 2); it != gsec.begin() && locs.size() < 8;) {
            --it;
            if ((it->first << 2) < u0) break;
            locs.push_back(it->first << 2);
        }
        if (locs.size() < 4) continue;
        const uint32_t s = locs[0] - locs[1];
        bool reg = s >= 4 && s <= 64 && !(s & 3);
        for (size_t i = 1; reg && i + 1 < locs.size(); i++) reg = locs[i] - locs[i + 1] == s;
        if (!reg) continue;
        const uint32_t po = (uint32_t)(((int64_t)locs[0] + s - X) % (int64_t)s + s) % s;
        auto elems = [&](const Blob& b) {
            uint32_t k = 0;
            while ((k + 1) * s <= b.b.size() && b.ptrs.count(po + k * s)) {
                bool only = true;
                for (auto it = b.ptrs.lower_bound(k * s); it != b.ptrs.end() && it->first < (k + 1) * s; ++it) if (it->first != po + k * s) only = false;
                if (!only) break;
                k++;
            }
            return k;
        };
        Blob& a = M.blobs[(size_t)as[0]];
        Blob& b = M.blobs[(size_t)bs[0]];
        const uint32_t ka = elems(a), kb = elems(b);
        if (!ka || !kb || (ka * s == a.b.size() && kb * s == b.b.size())) continue;
        if (a.in_file != a.b.size() || b.in_file != b.b.size()) continue;
        /* a = [a's entries][a's rest] -> a = [a's entries], new blob [a's rest] after b's entries;
           b = [b's entries][b's rest] -> b = [b's entries], new blob [b's rest] last */
        auto cut_off = [&](int bi, uint32_t at_off) {
            Blob& x = M.blobs[(size_t)bi];
            Blob rest;
            rest.unit = x.unit; rest.at = x.at; rest.from = x.from;
            rest.b.assign(x.b.begin() + at_off, x.b.end());
            rest.in_file = (uint32_t)rest.b.size();
            for (auto it = x.ptrs.lower_bound(at_off); it != x.ptrs.end(); ++it) rest.ptrs[it->first - at_off] = it->second;
            x.ptrs.erase(x.ptrs.lower_bound(at_off), x.ptrs.end());
            for (auto it = x.cids.lower_bound(at_off); it != x.cids.end(); ++it) rest.cids.insert(*it - at_off);
            x.cids.erase(x.cids.lower_bound(at_off), x.cids.end());
            x.b.resize(at_off); x.in_file = at_off;
            const int ni = (int)M.blobs.size();
            M.blobs.push_back(std::move(rest));
            origin.push_back(origin[(size_t)bi]);
            for_each_ref(M, [&](Ref& r) { if (r.blob == bi && r.off >= at_off && !(r.off == at_off && at_off == 0)) { r.blob = ni; r.off -= at_off; } });
            for_each_ref(B, [&](Ref& r) { if (r.blob == bi && r.off >= at_off) { r.blob = ni; r.off -= at_off; } });
            return ni;
        };
        /* blob order at X is index order: a, b, a-rest, b-rest */
        cut_off(as[0], ka * s);
        cut_off(bs[0], kb * s);
        log.push_back("both added entries to the table at the end of " + fname(X > 0 ? X - 1 : X) + " (" + std::to_string(ka) + " + " +
                      std::to_string(kb) + " of " + std::to_string(s) + " bytes): put together");
    }
}

/* ---- 3. pointers both aimed elsewhere ------------------------------------------- */

struct Conf { uint32_t loc; Ref ta, tb; };

bool Combine::merge_pointers()
{
    std::vector<Conf> conf;
    for (const auto& kv : B.ptr_set) {
        auto it = M.ptr_set.find(kv.first);
        if (it == M.ptr_set.end()) {
            if (M.ptr_del.count(kv.first)) { if (prefer) { M.ptr_del.erase(kv.first); M.ptr_set[kv.first] = kv.second; } continue; }
            M.ptr_set[kv.first] = kv.second;
            continue;
        }
        if (it->second == kv.second) continue;
        const Ref& ta = it->second.to; const Ref& tb = kv.second.to;
        if (ta.blob >= 0 && tb.blob >= 0 && M.blobs[(size_t)ta.blob].chunk < 0 && M.blobs[(size_t)tb.blob].chunk < 0 &&
            M.blobs[(size_t)ta.blob].unit == M.blobs[(size_t)tb.blob].unit && M.blobs[(size_t)ta.blob].at == M.blobs[(size_t)tb.blob].at &&
            ta.off == tb.off && gptr(kv.first) && gword(kv.first) == M.blobs[(size_t)ta.blob].at + ta.off)
            continue;   /* both: "the place where we inserted": the first mod's */
        conf.push_back({ kv.first, ta, tb });
    }
    for (uint32_t a : B.ptr_del) if (!M.ptr_set.count(a)) M.ptr_del.insert(a); else if (!prefer) {} else { M.ptr_set.erase(a); M.ptr_del.insert(a); }
    std::sort(conf.begin(), conf.end(), [&](const Conf& x, const Conf& y) { return x.loc < y.loc; });

    /* group them: {begin, end} pairs, {pointer, count} pairs, single pointers with pointers into the same copy */
    struct Grp { int type = 0; std::vector<Conf> c; uint32_t tg0 = 0, gend = 0, ta0 = 0, aend = 0, tb0 = 0, bend = 0, cnt[3] = { 0, 0, 0 }; int ba = -1, bb = -1;
                 std::vector<Conf> interior; };
    std::vector<Grp> grp;
    std::set<uint32_t> used;
    auto conf_at = [&](uint32_t loc) -> const Conf* { for (const Conf& c : conf) if (c.loc == loc) return &c; return nullptr; };
    auto blobref = [&](const Ref& r) { return r.blob >= 0; };
    for (const Conf& c : conf) {
        if (used.count(c.loc)) continue;
        if (!gptr(c.loc) || !blobref(c.ta) || !blobref(c.tb)) continue;
        const Conf* e = conf_at(c.loc + 4);
        if (e && gptr(e->loc) && blobref(e->ta) && blobref(e->tb) && e->ta.blob == c.ta.blob && e->tb.blob == c.tb.blob &&
            gword(e->loc) >= gword(c.loc) && e->ta.off >= c.ta.off && e->tb.off >= c.tb.off &&
            gword(e->loc) - gword(c.loc) < 0x100000) {
            Grp g; g.type = 1; g.c = { c, *e };
            g.tg0 = gword(c.loc); g.gend = gword(e->loc); g.ba = c.ta.blob; g.ta0 = c.ta.off; g.aend = e->ta.off; g.bb = c.tb.blob; g.tb0 = c.tb.off; g.bend = e->tb.off;
            grp.push_back(g); used.insert(c.loc); used.insert(e->loc);
            continue;
        }
        const uint32_t w = c.loc + 4;
        if (!gptr(w)) {
            const uint32_t gc = gword(w), ac = dword(G, A, w), bc = dword(G, B, w);
            if (ac != gc && bc != gc && ac > gc && bc > gc && ac - gc < 0x10000 && bc - gc < 0x10000 && gc < 0x1000000) {
                Grp g; g.type = 2; g.c = { c }; g.tg0 = gword(c.loc); g.ba = c.ta.blob; g.ta0 = c.ta.off; g.bb = c.tb.blob; g.tb0 = c.tb.off;
                g.cnt[0] = gc; g.cnt[1] = ac; g.cnt[2] = bc;
                grp.push_back(g); used.insert(c.loc);
                continue;
            }
        }
    }
    /* single pointers: sorted by target; pointers into a copy at the same offset belong to it */
    std::vector<Conf> singles;
    for (const Conf& c : conf) if (!used.count(c.loc) && gptr(c.loc) && blobref(c.ta) && blobref(c.tb)) singles.push_back(c);
    std::sort(singles.begin(), singles.end(), [&](const Conf& x, const Conf& y) { return gword(x.loc) < gword(y.loc); });
    for (size_t i = 0; i < singles.size(); i++) {
        const Conf& c = singles[i];
        if (used.count(c.loc)) continue;
        Grp g; g.type = 3; g.c = { c }; g.tg0 = gword(c.loc); g.ba = c.ta.blob; g.ta0 = c.ta.off; g.bb = c.tb.blob; g.tb0 = c.tb.off;
        used.insert(c.loc);
        std::set<uint32_t> inner_g;
        for (size_t j = i + 1; j < singles.size(); j++) {
            const Conf& d = singles[j];
            if (used.count(d.loc)) continue;
            const uint32_t t = gword(d.loc);
            if (t - g.tg0 >= 0x10000) break;
            if (d.ta.blob == g.ba && d.tb.blob == g.bb && d.ta.off - g.ta0 == t - g.tg0 && d.tb.off - g.tb0 == t - g.tg0) {
                g.interior.push_back(d); inner_g.insert(t);
            }
        }
        /* the game's copy ends at the next place a pointer aims at (not one of these), or its file's end */
        const int f = G.file_of(g.tg0);
        uint32_t fend = f >= 0 ? G.files[(size_t)f][2] + G.files[(size_t)f][1] : g.tg0 + 4;
        auto it = std::upper_bound(gtargets.begin(), gtargets.end(), g.tg0);
        while (it != gtargets.end() && inner_g.count(*it)) ++it;
        g.gend = it == gtargets.end() ? fend : (std::min)(*it, fend);
        std::vector<Conf> keep;
        for (const Conf& d : g.interior) if (gword(d.loc) < g.gend) { keep.push_back(d); used.insert(d.loc); }
        g.interior = keep;
        grp.push_back(g);
    }
    for (const Conf& c : conf)
        if (!used.count(c.loc)) {
            /* not a copy of a game list: the preferred mod's pointer */
            if (prefer) M.ptr_set[c.loc] = B.ptr_set[c.loc];
            conflicts++;
            log.push_back("pointer at " + hx(c.loc) + " (" + fname(c.loc) + ") aimed elsewhere by both: " + (prefer ? B.name : A.name) + "'s kept");
        }

    /* where each mod's own data starts, in its new bytes (any pointer's target) */
    std::map<int, std::set<uint32_t>> anchors;
    {
        Delta ac = A; for_each_ref(ac, [&](Ref& r) { if (r.blob >= 0) anchors[r.blob].insert(r.off); });
        Delta bc = B; for_each_ref(bc, [&](Ref& r) { if (r.blob >= 0) anchors[r.blob].insert(r.off); });
        for (const Conf& c : conf) { if (c.ta.blob >= 0) anchors[c.ta.blob].insert(c.ta.off); if (c.tb.blob >= 0) anchors[c.tb.blob].insert(c.tb.off); }
    }

    /* tokens: a word by its value, a pointer by what it points at (inside the copy: by offset) */
    struct SideV { int blob; uint32_t o0, o1; };
    auto word_at = [&](const SideV& s, uint32_t off, uint32_t& v, const Ptr*& p, bool& cid) -> bool {
        if (s.blob < 0) {
            if (off + 4 > G.data.size()) return false;
            v = gword(off); cid = gcid.count(off) > 0;
            static thread_local Ptr tmp;
            auto it = gsec.find(off >> 2);
            if (!(off & 3) && it != gsec.end()) { tmp.sec = it->second; tmp.to = Ref{ -1, v }; p = &tmp; } else p = nullptr;
            return true;
        }
        const Blob& b = M.blobs[(size_t)s.blob];
        if (off + 4 > b.b.size()) return false;
        v = rd32(&b.b[off]); cid = b.cids.count(off) > 0;
        auto it = b.ptrs.find(off);
        p = it != b.ptrs.end() ? &it->second : nullptr;
        return true;
    };
    auto tok = [&](const SideV& s, uint32_t off) -> uint64_t {
        uint32_t v; const Ptr* p; bool cid;
        if (!word_at(s, off, v, p, cid)) return 0xFFFFFFFFFFFFFFFFull;
        if (!p) return v;
        const Ref& t = p->to;
        if (t.blob == s.blob && t.off >= s.o0 && t.off < s.o1) return (1ull << 62) | (t.off - s.o0);
        return (1ull << 63) | ((uint64_t)(uint32_t)(t.blob + 3) << 32) | t.off;
    };
    auto etok = [&](const SideV& s, uint32_t e, uint32_t sz) -> uint64_t {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t k = 0; k < sz; k += 4) { h ^= tok(s, s.o0 + e * sz + k); h *= 1099511628211ull; }
        return sz == 4 ? tok(s, s.o0 + e * sz) : h;
    };
    auto pmask = [&](const SideV& s, uint32_t e, uint32_t sz) -> uint32_t {
        uint32_t m = 0;
        for (uint32_t k = 0; k < sz; k += 4) { uint32_t v; const Ptr* p; bool cid; if (word_at(s, s.o0 + e * sz + k, v, p, cid) && p) m |= 1u << (k / 4); }
        return m;
    };

    for (Grp& g : grp) {
        SideV sg{ -1, g.tg0, g.gend }, sa{ g.ba, g.ta0, g.aend }, sb{ g.bb, g.tb0, g.bend };
        uint32_t s = 4, ng = 0, na = 0, nb = 0;
        const Blob& BA = M.blobs[(size_t)g.ba]; const Blob& BB = M.blobs[(size_t)g.bb];
        if (g.type == 3) {
            /* each mod's copy: up to its next own data, and no further than where the game's copy's end matched */
            auto win = [&](int blob, uint32_t o0, const std::vector<uint32_t>& inner) {
                const auto& an = anchors[blob];
                auto it = an.upper_bound(o0);
                while (it != an.end() && std::find(inner.begin(), inner.end(), *it) != inner.end()) ++it;
                uint32_t e = it == an.end() ? (uint32_t)M.blobs[(size_t)blob].b.size() : *it;
                return (std::min<uint32_t>)(e, (uint32_t)M.blobs[(size_t)blob].b.size());
            };
            std::vector<uint32_t> ia, ib;
            for (const Conf& d : g.interior) { ia.push_back(d.ta.off); ib.push_back(d.tb.off); }
            (void)win;
            const uint32_t glen = g.gend - g.tg0;
            g.aend = (uint32_t)(std::min<uint64_t>)(M.blobs[(size_t)g.ba].b.size(), (uint64_t)g.ta0 + glen + 0x1000);
            g.bend = (uint32_t)(std::min<uint64_t>)(M.blobs[(size_t)g.bb].b.size(), (uint64_t)g.tb0 + glen + 0x1000);
            sa.o1 = g.aend; sb.o1 = g.bend;
            auto fit = [&](SideV& side, uint32_t& end) {
                const uint32_t n0 = (g.gend - g.tg0) / 4, n1 = (end - side.o0) / 4;
                std::vector<uint64_t> X(n0), Y(n1);
                for (uint32_t i = 0; i < n0; i++) X[i] = tok(sg, g.tg0 + 4 * i);
                for (uint32_t i = 0; i < n1; i++) Y[i] = tok(side, side.o0 + 4 * i);
                std::vector<Hunk2> h; pdiff(X, Y, 0, n0, 0, n1, h, 0);
                std::sort(h.begin(), h.end(), [](const Hunk2& x, const Hunk2& y) { return x.a < y.a; });
                /* where the game's last word went */
                if (!n0) return;
                uint32_t ga = 0, mb = 0; int64_t last = -1;
                for (const Hunk2& x : h) {
                    if (x.a > ga && n0 - 1 >= ga && n0 - 1 < x.a) last = mb + (n0 - 1 - ga);
                    ga = x.a + x.an; mb = x.b + x.bn;
                }
                if (n0 - 1 >= ga) last = mb + (n0 - 1 - ga);
                /* the game's last word replaced by a long run (e.g. a new record put before an end
                   marker, with the mod's next data after it): the end is where that word comes back */
                if (last < 0 && X[n0 - 1] != 0)
                    for (const Hunk2& x : h)
                        if (x.a <= n0 - 1 && n0 - 1 < x.a + x.an) {
                            for (uint32_t j = x.b; j < x.b + x.bn; j++) if (Y[j] == X[n0 - 1]) { last = j; break; }
                            break;
                        }
                if (getenv("XC_DEBUG")) {
                    fprintf(stderr, "fit g %x..%x side %d %x.. win %x last %lld hunks:", g.tg0, g.gend, side.blob, side.o0, end, (long long)last);
                    for (const Hunk2& x : h) fprintf(stderr, " [g%u+%u m%u+%u]", x.a, x.an, x.b, x.bn);
                    fprintf(stderr, "\n");
                    if (last < 0) {   /* the game's end lost: show both ends */
                        auto show = [&](const SideV& sv, uint32_t i0, uint32_t i1) {
                            for (uint32_t i = i0; i < i1; i++) {
                                uint32_t v; const Ptr* p; bool cid;
                                if (!word_at(sv, sv.o0 + 4 * i, v, p, cid)) break;
                                const uint64_t t = tok(sv, sv.o0 + 4 * i);
                                fprintf(stderr, "   %s[%u] %08x%s%s", sv.blob < 0 ? "g" : "m", i, v, p ? (t >> 62 == 1 ? " int-ptr" : " ext-ptr") : "", cid ? " cid" : "");
                                if (p) fprintf(stderr, " ->(%d,%x)", p->to.blob, p->to.off);
                                fprintf(stderr, "\n");
                            }
                        };
                        show(sg, n0 > 8 ? n0 - 8 : 0, n0);
                        show(side, n0 > 8 ? n0 - 8 : 0, (std::min<uint32_t>)(n1, n0 + 40));
                    }
                }
                if (last >= 0 && (uint32_t)last < n1) end = side.o0 + 4 * ((uint32_t)last + 1);
                side.o1 = end;
            };
            fit(sa, g.aend); fit(sb, g.bend);
        }
        /* the element size */
        static const uint32_t cand[] = { 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 68, 72, 80, 96, 128 };
        bool ok = false;
        for (uint32_t c : cand) {
            if (g.type == 2) {
                ng = g.cnt[0]; na = g.cnt[1]; nb = g.cnt[2];
                if (g.tg0 + (uint64_t)ng * c > G.data.size() || g.ta0 + (uint64_t)na * c > BA.b.size() || g.tb0 + (uint64_t)nb * c > BB.b.size()) continue;
                sg.o1 = g.tg0 + ng * c; sa.o1 = g.ta0 + na * c; sb.o1 = g.tb0 + nb * c;
            } else {
                const uint32_t lg = g.gend - g.tg0, la = g.aend - g.ta0, lb = g.bend - g.tb0;
                if (lg % c || la % c || lb % c) continue;
                ng = lg / c; na = la / c; nb = lb / c;
                sg.o1 = g.gend; sa.o1 = g.aend; sb.o1 = g.bend;
            }
            if (g.type == 3 && c != 4) continue;   /* a script block or blob: word by word */
            if (g.type == 3) { s = c; ok = true; break; }
            /* every element the same pointer pattern */
            uint32_t m0 = 0; bool first = true, same = true;
            for (auto* sv : { &sg, &sa, &sb }) {
                const uint32_t n = sv == &sg ? ng : sv == &sa ? na : nb;
                for (uint32_t e = 0; same && e < n; e++) { const uint32_t m = pmask(*sv, e, c); if (first) { m0 = m; first = false; } else if (m != m0) same = false; }
            }
            if (!same) continue;
            s = c; ok = true; break;
        }
        if (!ok) { conflicts++; log.push_back("a list both copied at " + hx(g.c[0].loc) + " (" + fname(g.c[0].loc) + ") has no clear entry size: " + (prefer ? B.name : A.name) + "'s kept");
                   for (const Conf& c : g.c) if (prefer) M.ptr_set[c.loc] = B.ptr_set[c.loc];
                   for (const Conf& c : g.interior) if (prefer) M.ptr_set[c.loc] = B.ptr_set[c.loc];
                   continue; }
        /* element diffs against the game's */
        std::vector<uint64_t> eg(ng), ea(na), eb(nb);
        for (uint32_t i = 0; i < ng; i++) eg[i] = etok(sg, i, s);
        for (uint32_t i = 0; i < na; i++) ea[i] = etok(sa, i, s);
        for (uint32_t i = 0; i < nb; i++) eb[i] = etok(sb, i, s);
        /* per side: for each game element, its match (or -1), and the new elements before game element i */
        struct Al { std::vector<int64_t> gm, chg; std::vector<std::vector<uint32_t>> ins; };   /* chg: changed in place to */
        auto align = [&](const std::vector<uint64_t>& Y, Al& al) {
            std::vector<Hunk2> h; pdiff(eg, Y, 0, ng, 0, (uint32_t)Y.size(), h, 0);
            std::sort(h.begin(), h.end(), [](const Hunk2& x, const Hunk2& y) { return x.a < y.a; });
            al.gm.assign(ng, -1); al.chg.assign(ng, -1); al.ins.assign(ng + 1, {});
            uint32_t ga = 0, mb = 0;
            auto same_run = [&](uint32_t g0, uint32_t m0, uint32_t n) {
                for (uint32_t k = 0; k < n; k++) {
                    if (eg[g0 + k] == Y[m0 + k]) al.gm[g0 + k] = m0 + k;
                    else al.chg[g0 + k] = m0 + k;   /* changed in place */
                }
            };
            for (const Hunk2& x : h) {
                same_run(ga, mb, x.a - ga);
                if (x.an == x.bn) for (uint32_t k = 0; k < x.an; k++) al.chg[x.a + k] = x.b + k;   /* replaced one for one */
                else for (uint32_t k = 0; k < x.bn; k++) al.ins[x.a].push_back(x.b + k);
                ga = x.a + x.an; mb = x.b + x.bn;
            }
            same_run(ga, mb, ng - ga);
            for (uint32_t k = mb + (ng - ga); k < Y.size(); k++) al.ins[ng].push_back(k);
        };
        Al aa, ab;
        align(ea, aa); align(eb, ab);
        /* nothing added by one: the other's copy is the combination */
        bool a_adds = false, b_adds = false, a_drops = false, b_drops = false;
        for (uint32_t i = 0; i <= ng; i++) { if (!aa.ins[i].empty()) a_adds = true; if (!ab.ins[i].empty()) b_adds = true; }
        for (uint32_t i = 0; i < ng; i++) {
            if (aa.chg[i] >= 0) a_adds = true; else if (aa.gm[i] < 0) a_drops = true;
            if (ab.chg[i] >= 0) b_adds = true; else if (ab.gm[i] < 0) b_drops = true;
        }
        if (!b_adds && !b_drops) {
            log.push_back("list at " + hx(g.c[0].loc) + " (" + fname(g.c[0].loc) + "): both copied it alike, " + A.name + "'s copy used");
            continue;   /* M already has A's pointers */
        }
        if (!a_adds && !a_drops) {
            for (const Conf& c : g.c) M.ptr_set[c.loc] = B.ptr_set[c.loc];
            for (const Conf& c : g.interior) M.ptr_set[c.loc] = B.ptr_set[c.loc];
            if (g.type == 2) fixed_words.insert(g.c[0].loc + 4), M.bytes.erase(g.c[0].loc + 4);
            if (g.type == 2) for (int q = 0; q < 4; q++) { auto it = B.bytes.find(g.c[0].loc + 4 + q); if (it != B.bytes.end()) M.bytes[g.c[0].loc + 4 + q] = it->second; }
            log.push_back("list at " + hx(g.c[0].loc) + " (" + fname(g.c[0].loc) + "): " + B.name + "'s copy used");
            continue;
        }
        /* combined: game order; at each place A's new ones, then B's; a game element either dropped is dropped */
        std::vector<std::pair<int, uint32_t>> out;   /* (side 0 G, 1 A, 2 B; element) */
        std::vector<int64_t> gpos(ng, -1), apos(na, -1), bpos(nb, -1);
        size_t dropped = 0, changed = 0;
        for (uint32_t i = 0; i <= ng; i++) {
            for (uint32_t k : aa.ins[i]) { apos[k] = (int64_t)out.size(); out.push_back({ 1, k }); }
            /* the very same insert from both: once */
            bool same_ins = !ab.ins[i].empty() && ab.ins[i].size() == aa.ins[i].size();
            for (size_t q = 0; same_ins && q < ab.ins[i].size(); q++) same_ins = ea[aa.ins[i][q]] == eb[ab.ins[i][q]];
            for (size_t q = 0; q < ab.ins[i].size(); q++) {
                const uint32_t k = ab.ins[i][q];
                if (same_ins) { bpos[k] = apos[aa.ins[i][q]]; continue; }
                bpos[k] = (int64_t)out.size(); out.push_back({ 2, k });
            }
            if (i == ng) break;
            if ((aa.gm[i] < 0 && aa.chg[i] < 0) || (ab.gm[i] < 0 && ab.chg[i] < 0)) { dropped++; continue; }
            const int64_t pos = (int64_t)out.size();
            gpos[i] = pos;
            const int64_t ka = aa.chg[i] >= 0 ? aa.chg[i] : aa.gm[i], kb = ab.chg[i] >= 0 ? ab.chg[i] : ab.gm[i];
            apos[(size_t)ka] = pos; bpos[(size_t)kb] = pos;
            if (aa.chg[i] >= 0 && ab.chg[i] >= 0) {
                if (ea[(size_t)ka] != eb[(size_t)kb]) {
                    conflicts++;
                    log.push_back("list at " + hx(g.c[0].loc) + ": game element " + std::to_string(i) + " changed differently by both, " +
                                  (prefer ? B.name : A.name) + "'s kept");
                }
                out.push_back(prefer && ea[(size_t)ka] != eb[(size_t)kb] ? std::make_pair(2, (uint32_t)kb) : std::make_pair(1, (uint32_t)ka));
                changed++;
            } else if (aa.chg[i] >= 0) { out.push_back({ 1, (uint32_t)ka }); changed++; }
            else if (ab.chg[i] >= 0) { out.push_back({ 2, (uint32_t)kb }); changed++; }
            else out.push_back({ 0, i });
        }
        CopyOut co;
        co.unit = BA.chunk < 0 ? BA.unit : 0; co.at = BA.at;
        if (BA.chunk >= 0) { log.push_back("list at " + hx(g.c[0].loc) + ": the first mod's copy is in a new packed file, not combined"); continue; }
        co.align_mod = g.tg0 % 0x40;
        const uint32_t nbytes = (uint32_t)out.size() * s;
        co.words.resize(nbytes / 4);
        auto map_internal = [&](int side, uint32_t ioff, uint32_t& res) -> bool {
            const uint32_t e = ioff / s, r = ioff % s;
            const uint32_t n = side == 0 ? ng : side == 1 ? na : nb;
            if (e == n && !r) { res = nbytes; return true; }
            if (e >= n) return false;
            const int64_t p = side == 0 ? gpos[e] : side == 1 ? apos[e] : bpos[e];
            if (p < 0) return false;
            res = (uint32_t)p * s + r;
            return true;
        };
        for (size_t k = 0; k < out.size(); k++) {
            const int side = out[k].first; const uint32_t e = out[k].second;
            const SideV& sv = side == 0 ? sg : side == 1 ? sa : sb;
            for (uint32_t q = 0; q < s; q += 4) {
                const uint32_t src = sv.o0 + e * s + q;
                uint32_t v; const Ptr* p; bool cid;
                word_at(sv, src, v, p, cid);
                CWord& w = co.words[(k * s + q) / 4];
                w.v = v; w.cid = cid;
                if (p) {
                    w.ptr = true; w.p = *p;
                    const Ref& t = p->to;
                    if (t.blob == sv.blob && t.off >= sv.o0 && (t.off < sv.o1 || (t.off == sv.o1 && g.type != 3))) {   /* a single-pointer list has no end pointer: past it is the mod's own data */
                        uint32_t res;
                        if (map_internal(side, t.off - sv.o0, res)) { w.internal = 1; w.ioff = res; }
                    }
                }
            }
            /* link entries on this element: the game's, or the mod's new ones */
            const uint32_t s0 = sv.o0 + e * s, s1 = s0 + s;
            if (side == 0) {
                for (size_t li = 0; li < G.links.size(); li++)
                    for (size_t bi = 0; bi < G.links[li].blocks.size(); bi++) {
                        const Block& b = G.links[li].blocks[bi];
                        auto add = [&](Entry x, uint32_t addr) { x.at = Ref{ -3, (uint32_t)(k * s) + (addr - s0) }; co.entries.push_back({ { li, bi }, x }); };
                        for (const Otyp& o : b.otyp) if (o.addr >= s0 && o.addr < s1) { Entry x; x.kind = 0; x.w = { o.a, o.b }; add(x, o.addr); }
                        for (const Xlnk& o : b.xlnk) if (o.addr >= s0 && o.addr < s1) { Entry x; x.kind = 1; x.w = { o.v }; memcpy(x.key, o.key, 16); add(x, o.addr); }
                        for (const auto& o : b.s1) if (o[0] >= s0 && o[0] < s1) { Entry x; x.kind = 2; x.w = { o[1], o[2] }; add(x, o[0]); }
                        for (const auto& o : b.s2) if (o[0] >= s0 && o[0] < s1) { Entry x; x.kind = 3; x.w = { o[2], o[3], o[4], o[5], o[6] }; sig_slice(b, o[1], o[6] >> 24, x.sig); add(x, o[0]); }
                        for (const auto& o : b.s3) if (o[0] >= s0 && o[0] < s1) { Entry x; x.kind = 4; x.w = { o[2], o[3], o[4], o[5] }; sig_slice(b, o[1], o[5] >> 24, x.sig); add(x, o[0]); }
                    }
            } else {
                const Delta& D = side == 1 ? (const Delta&)M : (const Delta&)B;
                for (const auto& kv : D.lists) for (const Item& it : kv.second.items)
                    if (it.g < 0 && it.e.at.blob == sv.blob && it.e.at.off >= s0 && it.e.at.off < s1) {
                        Entry x = it.e;
                        if (x.kind >= 3 && x.w.size() >= 2) x.w.resize(x.w.size() - 2);
                        x.at = Ref{ -3, (uint32_t)(k * s) + (it.e.at.off - s0) };
                        co.entries.push_back({ kv.first, x });
                    }
            }
        }
        auto map_g = [&](uint32_t t, uint32_t& res) { return map_internal(0, t - g.tg0, res); };
        uint32_t r0;
        if (!map_g(g.tg0, r0)) r0 = 0;
        co.set_ptrs.push_back({ g.c[0].loc, r0 });
        if (g.type == 1) co.set_ptrs.push_back({ g.c[1].loc, nbytes });
        if (g.type == 2) co.set_count.push_back({ g.c[0].loc + 4, (uint32_t)out.size() });
        for (const Conf& d : g.interior) { uint32_t r; if (map_g(gword(d.loc), r)) co.set_ptrs.push_back({ d.loc, r }); }
        /* other pointers into either mod's old copy now aim at the combined one */
        for (uint32_t e = 0; e < na; e++) if (apos[e] >= 0) for (uint32_t q = 0; q < s; q += 4) co.moved.push_back({ { g.ba, g.ta0 + e * s + q }, (uint32_t)apos[e] * s + q });
        for (uint32_t e = 0; e < nb; e++) if (bpos[e] >= 0) for (uint32_t q = 0; q < s; q += 4) co.moved.push_back({ { g.bb, g.tb0 + e * s + q }, (uint32_t)bpos[e] * s + q });
        size_t nA = 0, nB = 0;
        for (auto& o : out) { if (o.first == 1) nA++; if (o.first == 2) nB++; }
        log.push_back("list at " + hx(g.c[0].loc) + " (" + fname(g.c[0].loc) + "): game's " + std::to_string(ng) + " + " + A.name + " " +
                      std::to_string(nA) + " + " + B.name + " " + std::to_string(nB) + " entries of " + std::to_string(s) + " bytes" +
                      (changed ? ", " + std::to_string(changed) + " changed in place" : "") +
                      (dropped ? ", " + std::to_string(dropped) + " dropped" : ""));
        copies.push_back(std::move(co));
    }
    return true;
}

/* ---- 4. bytes both changed -------------------------------------------------------- */

void Combine::merge_bytes()
{
    std::set<uint32_t> words;
    for (const auto& kv : B.bytes) words.insert(kv.first & ~3u);
    size_t sums = 0, kept = 0;
    for (uint32_t w : words) {
        if (fixed_words.count(w)) continue;
        bool in_a = false;
        for (int q = 0; q < 4; q++) if (A.bytes.count(w + q)) in_a = true;
        const uint32_t gw = gword(w), bw = dword(G, B, w);
        uint32_t mw = bw;
        if (in_a) {
            const uint32_t aw = dword(G, A, w);
            const int32_t da = (int32_t)(aw - gw), db = (int32_t)(bw - gw);
            auto small = [](int32_t d) { return (d > -0x10000 && d < 0x10000) || (!(d & 0xFFFF) && (d >> 16) > -0x100 && (d >> 16) < 0x100); };
            const int u = G.file_of(w);
            bool concat = false;
            for (size_t k = 0; k < gu.size(); k++) if (gu[k].g0 <= w && w < gu[k].g1) concat = concat_units.count((uint32_t)k) > 0;
            (void)u;
            if (aw == bw) {
                if (concat && da && small(da)) { mw = gw + (uint32_t)da + (uint32_t)db; sums++; }
                else mw = aw;
            } else if (concat && small(da) && small(db)) { mw = gw + (uint32_t)da + (uint32_t)db; sums++; }
            else { mw = prefer ? bw : aw; kept++; conflicts++;
                   log.push_back("word at " + hx(w) + " (" + fname(w) + ") set by both: " + hx(aw) + " / " + hx(bw) + ", " + (prefer ? B.name : A.name) + "'s kept"); }
        }
        uint8_t b[4]; wr32(b, mw);
        for (int q = 0; q < 4; q++) { if (b[q] != G.data[w + q]) M.bytes[w + q] = b[q]; else M.bytes.erase(w + q); }
    }
    if (sums) log.push_back(std::to_string(sums) + " counter(s) both raised: both raises");
    (void)kept;
}

/* ---- 5. link-table entries -------------------------------------------------------- */

bool Combine::merge_lists()
{
    for (auto& kv : B.lists) {
        auto it = M.lists.find(kv.first);
        const uint32_t ash = 4 * (uint32_t)(A.sig_extra.count(kv.first) ? A.sig_extra.at(kv.first).size() : 0);
        for (Item& x : kv.second.items) if (x.g < 0 && x.e.kind >= 3 && x.e.w.size() >= 2 && x.e.w.back() == 0xFFFFFFFEu) x.e.w[x.e.w.size() - 2] += ash;
        if (it == M.lists.end()) { M.lists[kv.first] = kv.second; continue; }
        const List& la = it->second; const List& lb = kv.second;
        int ng = -1;
        for (const Item& x : la.items) ng = (std::max)(ng, x.g);
        for (const Item& x : lb.items) ng = (std::max)(ng, x.g);
        const size_t n = (size_t)(ng + 1);
        std::vector<std::vector<const Item*>> ia(n + 1), ib(n + 1);
        std::vector<bool> ka(n, false), kb(n, false);
        std::vector<int> sa, sb;   /* the game entries in the order each mod has them */
        bool oa = true, ob = true; /* in the game's order */
        auto walk = [&](const List& L, std::vector<std::vector<const Item*>>& ins, std::vector<bool>& kept, std::vector<int>& seq, bool& ord) -> bool {
            std::vector<const Item*> pend;
            int last = -1;
            for (const Item& x : L.items) {
                if (x.g < 0) { pend.push_back(&x); continue; }
                if (kept[(size_t)x.g]) return false;   /* twice */
                if (x.g <= last) ord = false;
                last = x.g; kept[(size_t)x.g] = true; seq.push_back(x.g);
                ins[(size_t)x.g] = pend; pend.clear();
            }
            ins[n] = pend;
            return true;
        };
        if (!walk(la, ia, ka, sa, oa) || !walk(lb, ib, kb, sb, ob)) return fail("a link table with a game entry twice");
        /* The order to write them in: the game's, or the one a mod sorted them into
           (builders sort some tables, e.g. by address, and the game's isn't sorted).
           Both mods sorting them differently can't be combined. */
        std::vector<int> order;
        {
            const std::vector<int>* ref = nullptr;
            if (!oa && !ob) {
                std::vector<int> ca, cb;   /* the entries both kept, in each one's order */
                for (int g : sa) if (kb[(size_t)g]) ca.push_back(g);
                for (int g : sb) if (ka[(size_t)g]) cb.push_back(g);
                if (ca != cb) return fail("a link table both mods reordered differently");
                ref = &sa;
            } else if (!oa) ref = &sa;
            else if (!ob) ref = &sb;
            if (!ref) { for (size_t i = 0; i < n; i++) order.push_back((int)i); }
            else {
                /* the reordering one's order; the game entries it left out go after the one before them */
                std::vector<bool> in(n, false);
                for (int g : *ref) in[(size_t)g] = true;
                std::vector<std::vector<int>> after(n + 1);   /* index n: at the front */
                for (size_t i = 0; i < n; i++) if (!in[i]) {
                    size_t p = i;
                    while (p > 0 && !in[p - 1]) p--;
                    (p == 0 ? after[n] : after[p - 1]).push_back((int)i);
                }
                for (int g : after[n]) order.push_back(g);
                for (int g : *ref) { order.push_back(g); for (int h : after[(size_t)g]) order.push_back(h); }
                log.push_back("link table (link " + std::to_string(kv.first.first) + ", block " + std::to_string(kv.first.second) +
                              ") sorted by " + (ref == &sa ? A.name : B.name) + (!oa && !ob ? " and " + B.name + " alike" : "") + ": that order kept");
            }
        }
        /* the game's entries both kept are in either; missing in one = it dropped it */
        List out;
        size_t dup = 0, drop = 0;
        std::map<std::pair<int, uint32_t>, std::array<uint8_t, 16>> xl_at;   /* XLNK by place: two names on one place clash */
        for (size_t oi = 0; oi <= n; oi++) {
            const size_t i = oi == n ? n : (size_t)order[oi];
            const size_t run0 = out.items.size();
            for (const Item* x : ia[i]) { out.items.push_back(*x); if (x->e.kind == 1) { std::array<uint8_t, 16> k; memcpy(k.data(), x->e.key, 16); xl_at[{ x->e.at.blob, x->e.at.off }] = k; } }
            for (const Item* x : ib[i]) {
                bool d = false;
                for (const auto& j : ia) for (const Item* y : j) if (same_entry(y->e, x->e)) d = true;
                if (d) { dup++; continue; }
                if (x->e.kind == 1) {
                    auto f = xl_at.find({ x->e.at.blob, x->e.at.off });
                    if (f != xl_at.end() && memcmp(f->second.data(), x->e.key, 16)) return fail("both mods link the same place to different names");
                }
                out.items.push_back(*x);
            }
            /* new entries both mods keep in place order (e.g. link names by address) stay in that order together */
            {
                auto by_at = [](const Item* x, const Item* y) { return x->e.at < y->e.at; };
                const bool sa = std::is_sorted(ia[i].begin(), ia[i].end(), by_at), sb = std::is_sorted(ib[i].begin(), ib[i].end(), by_at);
                if (sa && sb && !ia[i].empty() && !ib[i].empty())
                    std::stable_sort(out.items.begin() + (long)run0, out.items.end(), [](const Item& x, const Item& y) { return x.e.at < y.e.at; });
            }
            if (oi == n) break;
            if (ka[i] && kb[i]) { Item g; g.g = (int)i; out.items.push_back(g); }
            else if (ka[i] || kb[i]) drop++;
        }
        (void)dup;
        if (drop) log.push_back(std::to_string(drop) + " game link entr(ies) one mod removed: removed");
        it->second = std::move(out);
    }
    for (const auto& kv : B.sig_extra) {
        auto& v = M.sig_extra[kv.first];
        v.insert(v.end(), kv.second.begin(), kv.second.end());
    }
    return true;
}

/* ---- 6. tables, header ------------------------------------------------------------ */

bool Combine::merge_tables()
{
    if (B.trail_set) {
        if (!M.trail_set) { M.trail_set = true; M.trail = B.trail; }
        else {
            if (M.trail.size() != B.trail.size() || M.trail.size() != G.trail.size()) return fail("both mods changed the object table's shape");
            for (size_t i = 0; i < M.trail.size(); i++) for (int j = 0; j < 4; j++) {
                const uint32_t g = G.trail[i][(size_t)j], a = M.trail[i][(size_t)j], b = B.trail[i][(size_t)j];
                if (a == b) continue;
                if (a == g) { M.trail[i][(size_t)j] = b; continue; }
                if (b == g) continue;
                M.trail[i][(size_t)j] = g + (a - g) + (b - g);
            }
        }
    }
    for (const auto& kv : B.chunk_w) { auto it = M.chunk_w.find(kv.first); if (it != M.chunk_w.end() && it->second != kv.second) return fail("both changed a chunk's header word"); M.chunk_w[kv.first] = kv.second; }
    for (const auto& kv : B.file_w) { auto it = M.file_w.find(kv.first); if (it != M.file_w.end() && it->second != kv.second) return fail("both changed a packed file's type"); M.file_w[kv.first] = kv.second; }
    if (B.head_set) { if (M.head_set && M.head_extra != B.head_extra) return fail("both changed the file's header"); M.head_set = true; M.head_extra = B.head_extra; }
    if (B.tail_set) { if (M.tail_set && M.tail != B.tail) return fail("both changed the file's end"); M.tail_set = true; M.tail = B.tail; }
    for (uint32_t a : B.cid_add) M.cid_add.insert(a);
    for (uint32_t a : B.cid_del) M.cid_del.insert(a);
    M.new_files = A.new_files + B.new_files;
    return true;
}

/* ---- 7. chunks both left at their size ------------------------------------------- */

bool Combine::keep_fixed_chunks()
{
    auto growth = [&](const Delta& D, size_t j) {
        int64_t g = 0;
        for (const Blob& b : D.blobs) if (chunk_of_blob(b) == (int)j) g += (int64_t)b.b.size();
        for (const Cut& c : D.cuts) if (gu[c.unit].chunk == (int)j) g -= c.len;
        return g;
    };
    for (size_t j = 0; j < G.chunks.size(); j++) {
        if (G.chunks[j][0] == 0xFF02) continue;
        if (growth(A, j) || growth(B, j)) continue;
        int64_t g = growth(M, j);
        if (!g) continue;
        if (g < 0) return fail("chunk " + std::to_string(j) + " would shrink");
        /* the end of each new range nothing uses */
        std::map<int, uint32_t> need;
        for_each_ref(M, [&](Ref& r) { if (r.blob >= 0) need[r.blob] = (std::max)(need[r.blob], r.off); });
        for (int k = (int)M.blobs.size() - 1; k >= 0 && g > 0; k--) {
            Blob& b = M.blobs[(size_t)k];
            if (chunk_of_blob(b) != (int)j || b.chunk >= 0) continue;
            uint32_t used = 0;
            for (uint32_t i = 0; i < b.b.size(); i++) if (b.b[i]) used = i + 1;
            if (!b.ptrs.empty()) used = (std::max)(used, b.ptrs.rbegin()->first + 4);
            if (!b.cids.empty()) used = (std::max)(used, *b.cids.rbegin() + 4);
            auto it = need.find(k);
            if (it != need.end()) used = (std::max)(used, it->second);
            used = (used + 3) & ~3u;
            if (used >= b.b.size()) continue;
            const uint32_t cut = (uint32_t)(std::min<int64_t>)(g, b.b.size() - used) & ~3u;
            if (!cut) continue;
            const bool whole = b.in_file == b.b.size();
            b.b.resize(b.b.size() - cut);
            if (whole) b.in_file = (uint32_t)b.b.size(); else if (b.in_file > b.b.size()) b.in_file = (uint32_t)b.b.size();
            g -= cut;
        }
        if (g) return fail("chunk " + std::to_string(j) + " must keep its size and the new data doesn't fit");
        log.push_back("chunk " + std::to_string(j) + " kept at its size (padding given back)");
    }
    return true;
}

/* ---- 8. the combined copies: placed after both mods' new bytes -------------------- */

bool Combine::place_copies()
{
    if (copies.empty()) return true;
    /* one new range per place, after everything else there */
    std::map<std::pair<uint32_t, uint32_t>, std::vector<size_t>> by;
    for (size_t i = 0; i < copies.size(); i++) by[{ copies[i].unit, copies[i].at }].push_back(i);
    std::map<std::pair<uint32_t, uint32_t>, int> nb;
    for (auto& kv : by) {
        Blob b; b.unit = kv.first.first; b.at = kv.first.second; b.from = "combined";
        nb[kv.first] = (int)M.blobs.size();
        M.blobs.push_back(b);
        origin.push_back(3);
    }
    /* where they land: write it out once */
    std::vector<uint32_t> bpos;
    {
        Bytes tmp; std::string e;
        if (!apply_delta(G, M, tmp, e, &bpos)) return fail("trial: " + e);
    }
    for (auto& kv : by) {
        const int bi = nb[kv.first];
        Blob& b = M.blobs[(size_t)bi];
        /* granule: the largest power of two (<= 0x4000) all other new ranges here are a multiple of */
        uint32_t gran = 0x4000;
        for (size_t k = 0; k < M.blobs.size(); k++)
            if ((int)k != bi && M.blobs[k].chunk < 0 && M.blobs[k].unit == b.unit && M.blobs[k].at == b.at)
                while (gran > 16 && M.blobs[k].b.size() % gran) gran >>= 1;
        uint32_t off = 0;
        const uint32_t p0 = bpos[(size_t)bi];
        for (size_t ci : kv.second) {
            CopyOut& c = copies[ci];
            while ((p0 + off) % 0x40 != c.align_mod) off += 4;
            c.base = off;
            off += 4 * (uint32_t)c.words.size();
        }
        off = (off + gran - 1) / gran * gran;
        b.b.assign(off, 0);
        b.in_file = off;
        for (size_t ci : kv.second) {
            CopyOut& c = copies[ci];
            for (size_t w = 0; w < c.words.size(); w++) {
                const CWord& x = c.words[w];
                const uint32_t o = c.base + 4 * (uint32_t)w;
                wr32(&b.b[o], x.v);
                if (x.cid) b.cids.insert(o);
                if (x.ptr) {
                    Ptr p = x.p;
                    if (x.internal) p.to = Ref{ bi, c.base + x.ioff };
                    b.ptrs[o] = p;
                }
            }
            for (auto& e : c.entries) {
                Entry x = e.second;
                x.at = Ref{ bi, c.base + x.at.off };
                if (x.kind >= 3) {
                    auto& extra = M.sig_extra[e.first];
                    x.w.push_back(4 * (uint32_t)extra.size());
                    x.w.push_back(0xFFFFFFFEu);
                    extra.insert(extra.end(), x.sig.begin(), x.sig.end());
                }
                Item it; it.e = x;
                M.lists[e.first].items.push_back(it);
            }
            for (auto& sp : c.set_ptrs) {
                auto it = M.ptr_set.find(sp.first);
                Ptr p; p.sec = it != M.ptr_set.end() ? it->second.sec : gsec[sp.first >> 2];
                p.to = Ref{ bi, c.base + sp.second };
                M.ptr_set[sp.first] = p;
            }
            for (auto& sc : c.set_count) {
                fixed_words.insert(sc.first);
                uint8_t w4[4]; wr32(w4, sc.second);
                for (int q = 0; q < 4; q++) { if (w4[q] != G.data[sc.first + q]) M.bytes[sc.first + q] = w4[q]; else M.bytes.erase(sc.first + q); }
            }
            /* other references into the old copies */
            std::map<std::pair<int, uint32_t>, uint32_t> mv(c.moved.begin(), c.moved.end());
            std::set<uint32_t> own;
            for (auto& sp : c.set_ptrs) own.insert(sp.first);
            size_t n = 0;
            for (auto& kv2 : M.ptr_set) {
                if (own.count(kv2.first)) continue;
                auto f = mv.find({ kv2.second.to.blob, kv2.second.to.off });
                if (f != mv.end() && kv2.second.to.blob != bi) { kv2.second.to = Ref{ bi, c.base + f->second }; n++; }
            }
            for (size_t k = 0; k < M.blobs.size(); k++) {
                if ((int)k == bi) continue;
                for (auto& pp : M.blobs[k].ptrs) {
                    /* (pointers inside the old copies themselves stay: those copies are no longer used) */
                    bool inside_old = false;
                    for (auto& m : c.moved) if (m.first.first == (int)k && m.first.second == pp.first) { inside_old = true; break; }
                    if (inside_old) continue;
                    auto f = mv.find({ pp.second.to.blob, pp.second.to.off });
                    if (f != mv.end()) { pp.second.to = Ref{ bi, c.base + f->second }; n++; }
                }
            }
            if (n) log.push_back(std::to_string(n) + " other pointer(s) into the old copies moved to the combined list");
        }
    }
    return true;
}

bool Combine::run()
{
    if (!units_and_blobs()) return false;
    split_tables();
    if (!merge_pointers()) return false;
    merge_bytes();
    if (!merge_lists()) return false;
    if (!merge_tables()) return false;
    if (!keep_fixed_chunks()) return false;
    if (!place_copies()) return false;
    return true;
}

/* The combination of two mods' changes to game file G (A first; prefer: 1 = B wins
 * bytes both set differently). out: the combined file. */
static bool combine_files(const Arc& G, const Delta& A, const Delta& B, int prefer, Bytes& out, std::vector<std::string>& log, std::string& err,
                          Delta* merged = nullptr, bool allow_conflicts = false)
{
    std::vector<std::string> l1;
    Combine c(G, A, B, prefer, l1);
    if (!c.run()) { err = c.err; return false; }
    if (c.swap_better) {   /* the second mod's record moves would break its own records: combine the other way round */
        std::vector<std::string> l2;
        Combine d(G, B, A, 1 - prefer, l2);
        if (d.run() && !d.swap_better) {
            log.insert(log.end(), l2.begin(), l2.end());
            if (d.conflicts && !allow_conflicts) { err = std::to_string(d.conflicts) + " place(s) both mods change differently"; return false; }
            if (!apply_delta(G, d.M, out, err)) return false;
            Arc chk;
            if (!chk.load(out)) { err = "the combined file doesn't read back: " + chk.err; return false; }
            if (merged) *merged = std::move(d.M);
            return true;
        }
    }
    log.insert(log.end(), l1.begin(), l1.end());
    if (c.conflicts && !allow_conflicts) { err = std::to_string(c.conflicts) + " place(s) both mods change differently"; return false; }
    if (!apply_delta(G, c.M, out, err)) return false;
    Arc chk;
    if (!chk.load(out)) { err = "the combined file doesn't read back: " + chk.err; return false; }
    if (merged) *merged = std::move(c.M);
    return true;
}

}   /* namespace xm */
