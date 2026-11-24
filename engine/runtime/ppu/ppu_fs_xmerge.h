/* ppu_fs_xmerge.h: a format-aware three-way merge of inFAMOUS 2 PACK files
 * (.xpps / .xpp), for the mod manager (ppu_fs_mods.h).
 *
 * A PACK file is a header, a chunk table, a packed-file table, an object table
 * (the "trail") and one flat data space. Pointers inside the data are plain
 * data-space addresses; the link files (chunks of type 0xFF02) list where they
 * are (ILNK), where class ids are (CID), object types (OTYP), cross links
 * (XLNK) and native-call / asset fix-ups (SPLP). A mod tool that inserts bytes
 * moves everything after them and rewrites every pointer that points past
 * them, so a byte comparison of two such mods against the game's file sees
 * changes everywhere. This merge reads the link tables, so it compares
 * pointers by what they point to and data by where it came from:
 *
 *   1. Arc: the file parsed (and written back byte for byte).
 *   2. Delta: one mod against the game's copy, as "the game's file plus":
 *      new byte ranges (inserted, with their pointers and fix-ups), removed
 *      ranges, changed bytes, pointers aimed elsewhere, fix-ups added or
 *      removed. Addresses are Refs: a place in the game's file, or in one of
 *      the mod's new ranges.
 *   3. Merge: two Deltas on the same game file. Changes of only one mod are
 *      taken; the same change once; each mod's new ranges are kept, the
 *      parts both added alike once; counters both raised get both raises.
 *      Anything else is a conflict, reported, and the merge is not used.
 *   4. Apply: a Delta (merged or not) written onto the game's file.
 *
 * Applying one mod's own Delta must give back its file exactly (tested): the
 * reading is checked on every file before it is merged. */
#pragma once
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdint.h>
#include <string.h>

namespace xm {

typedef std::vector<uint8_t> Bytes;

static inline uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void put32(Bytes& b, uint32_t v) { b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16)); b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
static inline void put16(Bytes& b, uint16_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }

/* ---- ILNK / CID location lists (word indexes), as the game stores them ---- */
static std::vector<uint32_t> loc_decode(const uint16_t* t, size_t n)
{
    std::vector<uint32_t> out;
    uint32_t page = 0, last = 0;
    for (size_t i = 0; i < n; i++) {
        const uint16_t x = t[i];
        if (x >= 0xC000) { page = x & 0x3FFF; continue; }
        if (x >= 0x8000) {
            const uint32_t r = x & 0x3FFF;
            for (uint32_t k = 1; k <= r; k++) out.push_back(last + k);
            last += r;
            continue;
        }
        last = (page << 15) | x;
        out.push_back(last);
    }
    return out;
}
/* sorted, unique locations -> tokens (runs of 2 or more as one token) */
static std::vector<uint16_t> loc_encode(const std::vector<uint32_t>& locs)
{
    std::vector<uint16_t> t;
    int64_t page = -1;
    size_t i = 0;
    const size_t n = locs.size();
    while (i < n) {
        const uint32_t loc = locs[i];
        if ((int64_t)(loc >> 15) != page) { page = loc >> 15; t.push_back((uint16_t)(0xC000 | (loc >> 15))); }
        t.push_back((uint16_t)(loc & 0x7FFF));
        uint32_t last = loc;
        i++;
        size_t j = i;
        while (j < n && locs[j] == last + (uint32_t)(j - i) + 1 && (j - i) < 0x3FFF) j++;
        const size_t r = j - i;
        if (r >= 2) {
            t.push_back((uint16_t)(0x8000 | r));
            last += (uint32_t)r;
            i = j;
            if ((int64_t)(last >> 15) != page) page = -1;   /* a run may end on another page */
        }
    }
    return t;
}

/* ---- the parsed file ---- */
struct IlnkSec { uint32_t kind = 0; std::vector<uint32_t> locs; std::vector<uint16_t> toks; bool same = true; };
struct Otyp { uint32_t addr, a, b; };
struct Xlnk { uint8_t key[16]; uint32_t addr, v; };
struct Block {
    std::string magic;                 /* "ILNK", "CID ", "OTYP", "XLNK", "SPLP", "END ", or another (kept raw) */
    std::vector<IlnkSec> secs;         /* ILNK */
    std::vector<uint32_t> cid;         /* CID: word indexes */
    std::vector<uint16_t> cid_toks; bool cid_same = true;
    std::vector<Otyp> otyp;
    uint32_t x0 = 0, x1 = 0; std::vector<Xlnk> xlnk;
    std::vector<std::array<uint32_t, 3>> s1; std::vector<std::array<uint32_t, 7>> s2; std::vector<std::array<uint32_t, 6>> s3;
    std::vector<uint32_t> sig;         /* SPLP: the argument words s2/s3 entries index (byte offsets into it) */
    Bytes raw;                         /* END / other: the body */
};
struct LinkFile { uint32_t file; std::vector<Block> blocks; };

struct Arc {
    Bytes head;                        /* the file up to the data (header, tables, anything between) */
    uint32_t tab = 0, dof = 0, nc = 0, nf = 0, ntrail = 0, f0 = 0;
    std::vector<std::array<uint32_t, 7>> chunks;   /* type, size, off, ?, ?, first file, files */
    std::vector<std::array<uint32_t, 4>> files;    /* type, size, off, 0 */
    std::vector<std::array<uint32_t, 4>> trail;
    Bytes data, tail;
    std::vector<LinkFile> links;
    std::string err;

    bool load(const Bytes& d);
    Bytes build() const;
    int chunk_of(uint32_t a) const {
        for (size_t i = 0; i < chunks.size(); i++) if (chunks[i][2] <= a && a < chunks[i][2] + chunks[i][1]) return (int)i;
        return -1;
    }
    int file_of(uint32_t a) const;     /* by binary search over the files (sorted by offset, empty ones skipped) */
    uint32_t main_end() const {
        uint32_t e = 0;
        for (const auto& c : chunks) if (c[0] == 0xFF00 && c[2] + c[1] > e) e = c[2] + c[1];
        return e;
    }
    bool is_link_file(uint32_t fi) const {
        for (const auto& c : chunks) if (c[0] == 0xFF02 && c[5] <= fi && fi < c[5] + c[6]) return true;
        return false;
    }
    std::vector<uint32_t> all_ptr_locs() const;   /* every ILNK location (word index), sorted */
};

static bool parse_link(const Arc& A, uint32_t fi, LinkFile& L, std::string& err)
{
    L.file = fi;
    const uint32_t off = A.files[fi][2], sz = A.files[fi][1];
    if ((uint64_t)off + sz > A.data.size()) { err = "a link file is out of range"; return false; }
    const uint8_t* d = A.data.data();
    uint64_t p = off;
    const uint64_t end = (uint64_t)off + sz;
    while (p < end) {
        if (p + 8 > end) { err = "a link block is cut off"; return false; }
        Block b;
        b.magic.assign((const char*)d + p, 4);
        const uint32_t ln = rd32(d + p + 4);
        const uint64_t q0 = p + 8;
        if (q0 + ln > end) { err = "a link block is out of range"; return false; }
        const uint8_t* q = d + q0;
        auto need = [&](uint64_t n) { if (n > ln) { err = "a " + b.magic + " block is too short"; return false; } return true; };
        if (b.magic == "ILNK") {
            if (!need(4)) return false;
            const uint32_t ns = rd32(q);
            if (!need(4 + 8ull * ns)) return false;
            uint64_t t = 4 + 8ull * ns;
            for (uint32_t k = 0; k < ns; k++) {
                IlnkSec s;
                s.kind = rd32(q + 4 + 8 * k);
                const uint32_t nt = rd32(q + 8 + 8 * k);
                if (!need(t + 2ull * nt)) return false;
                s.toks.resize(nt);
                for (uint32_t i = 0; i < nt; i++) s.toks[i] = rd16(q + t + 2 * i);
                t += 2ull * nt;
                s.locs = loc_decode(s.toks.data(), s.toks.size());
                b.secs.push_back(std::move(s));
            }
        } else if (b.magic == "CID ") {
            if (!need(4)) return false;
            const uint32_t n = rd32(q);
            if (!need(4 + 2ull * n)) return false;
            b.cid_toks.resize(n);
            for (uint32_t i = 0; i < n; i++) b.cid_toks[i] = rd16(q + 4 + 2 * i);
            b.cid = loc_decode(b.cid_toks.data(), n);
        } else if (b.magic == "OTYP") {
            if (!need(4)) return false;
            const uint32_t n = rd32(q);
            if (!need(4 + 12ull * n)) return false;
            for (uint32_t i = 0; i < n; i++) b.otyp.push_back({ rd32(q + 4 + 12 * i), rd32(q + 8 + 12 * i), rd32(q + 12 + 12 * i) });
        } else if (b.magic == "XLNK") {
            if (!need(12)) return false;
            b.x0 = rd32(q); b.x1 = rd32(q + 4);
            const uint32_t n = rd32(q + 8);
            if (!need(12 + 24ull * n)) return false;
            for (uint32_t i = 0; i < n; i++) {
                Xlnk x;
                memcpy(x.key, q + 12 + 24 * i, 16);
                x.addr = rd32(q + 28 + 24 * i); x.v = rd32(q + 32 + 24 * i);
                b.xlnk.push_back(x);
            }
        } else if (b.magic == "SPLP") {
            if (!need(12)) return false;
            const uint32_t n1 = rd32(q), n2 = rd32(q + 4), n3 = rd32(q + 8);
            const uint64_t words = (ln - 12) / 4;
            if (3ull * n1 + 7ull * n2 + 6ull * n3 > words) { err = "a SPLP block is too short"; return false; }
            uint64_t k = 0;
            auto w = [&](uint64_t i) { return rd32(q + 12 + 4 * i); };
            for (uint32_t i = 0; i < n1; i++, k += 3) b.s1.push_back({ w(k), w(k + 1), w(k + 2) });
            for (uint32_t i = 0; i < n2; i++, k += 7) { std::array<uint32_t, 7> e; for (int j = 0; j < 7; j++) e[j] = w(k + j); b.s2.push_back(e); }
            for (uint32_t i = 0; i < n3; i++, k += 6) { std::array<uint32_t, 6> e; for (int j = 0; j < 6; j++) e[j] = w(k + j); b.s3.push_back(e); }
            for (; k < words; k++) b.sig.push_back(w(k));
            if ((ln - 12) % 4) { err = "a SPLP block has a broken length"; return false; }
        } else {
            b.raw.assign(q, q + ln);
        }
        const bool stop = b.magic == "END ";
        L.blocks.push_back(std::move(b));
        p = q0 + ln;
        if (stop) break;
    }
    return true;
}

static void ser_link(const LinkFile& L, Bytes& out)
{
    out.clear();
    for (const Block& b : L.blocks) {
        Bytes body;
        if (b.magic == "ILNK") {
            put32(body, (uint32_t)b.secs.size());
            std::vector<std::vector<uint16_t>> tk;
            for (const IlnkSec& s : b.secs) {
                tk.push_back(s.same ? s.toks : loc_encode(s.locs));
                put32(body, s.kind); put32(body, (uint32_t)tk.back().size());
            }
            for (const auto& t : tk) for (uint16_t x : t) put16(body, x);
            while (body.size() % 4) body.push_back(0);
        } else if (b.magic == "CID ") {
            const std::vector<uint16_t> t = b.cid_same ? b.cid_toks : loc_encode(b.cid);
            put32(body, (uint32_t)t.size());
            for (uint16_t x : t) put16(body, x);
            while (body.size() % 4) body.push_back(0);
        } else if (b.magic == "OTYP") {
            put32(body, (uint32_t)b.otyp.size());
            for (const Otyp& o : b.otyp) { put32(body, o.addr); put32(body, o.a); put32(body, o.b); }
        } else if (b.magic == "XLNK") {
            put32(body, b.x0); put32(body, b.x1); put32(body, (uint32_t)b.xlnk.size());
            for (const Xlnk& x : b.xlnk) { body.insert(body.end(), x.key, x.key + 16); put32(body, x.addr); put32(body, x.v); }
        } else if (b.magic == "SPLP") {
            put32(body, (uint32_t)b.s1.size()); put32(body, (uint32_t)b.s2.size()); put32(body, (uint32_t)b.s3.size());
            for (const auto& e : b.s1) for (uint32_t v : e) put32(body, v);
            for (const auto& e : b.s2) for (uint32_t v : e) put32(body, v);
            for (const auto& e : b.s3) for (uint32_t v : e) put32(body, v);
            for (uint32_t v : b.sig) put32(body, v);
        } else {
            body = b.raw;
        }
        out.insert(out.end(), b.magic.begin(), b.magic.end());
        put32(out, (uint32_t)body.size());
        out.insert(out.end(), body.begin(), body.end());
    }
}

inline int Arc::file_of(uint32_t a) const
{
    /* files are laid out in order of their offsets within a chunk; a linear
     * search is fine for the few calls made, the merge uses its own maps */
    for (size_t i = 0; i < files.size(); i++) if (files[i][1] && files[i][2] <= a && a < files[i][2] + files[i][1]) return (int)i;
    return -1;
}

inline std::vector<uint32_t> Arc::all_ptr_locs() const
{
    std::vector<uint32_t> v;
    for (const LinkFile& L : links)
        for (const Block& b : L.blocks)
            if (b.magic == "ILNK") for (const IlnkSec& s : b.secs) v.insert(v.end(), s.locs.begin(), s.locs.end());
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

inline bool Arc::load(const Bytes& d)
{
    err.clear();
    if (d.size() < 48 || memcmp(d.data(), "PACK", 4)) { err = "not a PACK file"; return false; }
    tab = rd32(&d[24]); dof = rd32(&d[40]);
    const uint32_t dsize = rd32(&d[44]);
    if ((uint64_t)tab + 24 > d.size() || (uint64_t)dof + dsize > d.size() || dof < tab) { err = "its header is out of range"; return false; }
    nc = rd32(&d[tab + 4]); nf = rd32(&d[tab + 12]); ntrail = rd32(&d[tab + 20]);
    f0 = tab + 24 + 28 * nc;
    const uint64_t te = (uint64_t)f0 + 16ull * nf + 16ull * ntrail;
    if (te > dof) { err = "its tables run into the data"; return false; }
    chunks.resize(nc); files.resize(nf); trail.resize(ntrail);
    for (uint32_t i = 0; i < nc; i++) for (int j = 0; j < 7; j++) chunks[i][j] = rd32(&d[tab + 24 + 28 * i + 4 * j]);
    for (uint32_t i = 0; i < nf; i++) for (int j = 0; j < 4; j++) files[i][j] = rd32(&d[f0 + 16 * i + 4 * j]);
    const uint32_t e = f0 + 16 * nf;
    for (uint32_t i = 0; i < ntrail; i++) for (int j = 0; j < 4; j++) trail[i][j] = rd32(&d[e + 16 * i + 4 * j]);
    head.assign(d.begin(), d.begin() + dof);
    data.assign(d.begin() + dof, d.begin() + dof + dsize);
    tail.assign(d.begin() + dof + dsize, d.end());
    for (const auto& f : files) if ((uint64_t)f[2] + f[1] > data.size()) { err = "a packed file is out of range"; return false; }
    for (const auto& c : chunks) {
        if (c[0] != 0xFF02) continue;
        if ((uint64_t)c[5] + c[6] > nf) { err = "a link chunk names files that aren't there"; return false; }
        for (uint32_t fi = c[5]; fi < c[5] + c[6]; fi++) {
            LinkFile L;
            if (!parse_link(*this, fi, L, err)) return false;
            links.push_back(std::move(L));
        }
    }
    return true;
}

/* The file written back: link files serialized again (in place; they are the
 * last thing in their chunk), the tables from chunks/files/trail. With nothing
 * changed this gives back the bytes it was loaded from. Files added beyond nf,
 * or trail entries beyond ntrail, grow the tables (the data moves on). */
inline Bytes Arc::build() const
{
    Arc A = *this;   /* (copy: link sizes move offsets) */
    for (const LinkFile& L : links) {
        Bytes blob;
        ser_link(L, blob);
        auto& f = A.files[L.file];
        const uint32_t off = f[2], sz = f[1];
        const int64_t delta = (int64_t)blob.size() - (int64_t)sz;
        A.data.erase(A.data.begin() + off, A.data.begin() + off + sz);
        A.data.insert(A.data.begin() + off, blob.begin(), blob.end());
        f[1] = (uint32_t)blob.size();
        if (!delta) continue;
        int ci = -1;
        for (size_t i = 0; i < A.chunks.size(); i++) if (A.chunks[i][5] <= L.file && L.file < A.chunks[i][5] + A.chunks[i][6]) ci = (int)i;
        if (ci >= 0) A.chunks[(size_t)ci][1] = (uint32_t)((int64_t)A.chunks[(size_t)ci][1] + delta);
        for (auto& c : A.chunks) if (c[2] > off) c[2] = (uint32_t)((int64_t)c[2] + delta);
        for (size_t i = 0; i < A.files.size(); i++) if (i != L.file && A.files[i][2] > off) A.files[i][2] = (uint32_t)((int64_t)A.files[i][2] + delta);
    }
    Bytes h = head;
    const uint32_t addf = (uint32_t)(A.files.size() - nf), addt = (uint32_t)(A.trail.size() - ntrail);
    const uint32_t grow = 16 * (addf + addt);
    if (grow) {
        const uint32_t end_old = f0 + 16 * nf + 16 * ntrail;
        h.insert(h.begin() + end_old, grow, 0);
        wr32(&h[tab + 12], (uint32_t)A.files.size());
        wr32(&h[tab + 20], (uint32_t)A.trail.size());
        wr32(&h[28], rd32(&h[28]) + grow);
        wr32(&h[40], dof + grow);
    }
    wr32(&h[44], (uint32_t)A.data.size());
    for (size_t i = 0; i < A.chunks.size(); i++) for (int j = 0; j < 7; j++) wr32(&h[tab + 24 + 28 * i + 4 * j], A.chunks[i][j]);
    for (size_t i = 0; i < A.files.size(); i++) for (int j = 0; j < 4; j++) wr32(&h[f0 + 16 * i + 4 * j], A.files[i][j]);
    const uint32_t e = f0 + 16 * (uint32_t)A.files.size();
    for (size_t i = 0; i < A.trail.size(); i++) for (int j = 0; j < 4; j++) wr32(&h[e + 16 * i + 4 * j], A.trail[i][j]);
    Bytes out;
    out.reserve(h.size() + A.data.size() + tail.size());
    out.insert(out.end(), h.begin(), h.end());
    out.insert(out.end(), A.data.begin(), A.data.end());
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}


/* ---- one mod against the game's file ------------------------------------------ */

/* A place: in the game's file (blob -1), in new range `blob` of the delta, or a
 * value that is no data address at all (blob -2, kept as it is). */
struct Ref {
    int blob = -1; uint32_t off = 0;
    bool operator==(const Ref& o) const { return blob == o.blob && off == o.off; }
    bool operator!=(const Ref& o) const { return !(*this == o); }
    bool operator<(const Ref& o) const { return blob != o.blob ? blob < o.blob : off < o.off; }
};
struct Ptr { uint32_t sec = 0; Ref to; bool operator==(const Ptr& o) const { return sec == o.sec && to == o.to; } };

/* New bytes: inserted into the game's unit `unit` (a packed file's span) before
 * its byte `at` (an address in the game's data), or a new packed file of
 * chunk `chunk` (newfile: its type). */
struct Blob {
    uint32_t unit = 0, at = 0;
    int chunk = -1; uint32_t ftype = 0;   /* a new packed file: chunk >= 0 */
    uint32_t in_file = 0;                 /* bytes of it inside the packed file (the rest in the gap after it) */
    Bytes b;
    std::map<uint32_t, Ptr> ptrs;         /* offset -> pointer */
    std::set<uint32_t> cids;              /* offsets */
    std::string from;                     /* the mod's name, for messages */
};
struct Cut { uint32_t unit, at, len; };   /* game bytes [at, at+len) left out */

/* A link-table entry, its addresses as Refs; sig: an SPLP entry's argument words. */
struct Entry {
    int kind = 0;                          /* 0 OTYP, 1 XLNK, 2 SPLP s1, 3 s2, 4 s3 */
    Ref at;
    std::vector<uint32_t> w;               /* the other words (s2/s3 without the sig offset) */
    std::vector<uint32_t> sig;
    uint8_t key[16] = {};
    bool operator<(const Entry& o) const {
        if (kind != o.kind) return kind < o.kind;
        if (at != o.at) return at < o.at;
        if (w != o.w) return w < o.w;
        if (sig != o.sig) return sig < o.sig;
        return memcmp(key, o.key, 16) < 0;
    }
    bool operator==(const Entry& o) const { return !(*this < o) && !(o < *this); }
};
/* A list as the mod has it: each item a game entry (g >= 0, by index) or a new one. */
struct Item { int g = -1; Entry e; };
struct List { std::vector<Item> items; };   /* per (link file, block) of a list kind */

struct Delta {
    std::string name, err;
    std::vector<Blob> blobs;
    std::vector<Cut> cuts;
    std::map<uint32_t, uint8_t> bytes;        /* game address -> the mod's byte */
    std::map<uint32_t, Ptr> ptr_set;          /* game address (word) -> pointer there */
    std::set<uint32_t> ptr_del;               /* game pointers that are no pointer any more */
    std::set<uint32_t> cid_add, cid_del;      /* game addresses */
    std::map<std::pair<size_t, size_t>, List> lists;   /* (link file, block) -> its entries */
    std::map<std::pair<size_t, size_t>, std::vector<uint32_t>> sig_extra;   /* SPLP sig words after the game's */
    std::map<std::pair<size_t, int>, uint32_t> chunk_w;   /* (chunk, word 0/3/4) -> value, where it differs */
    std::map<std::pair<size_t, int>, uint32_t> file_w;    /* (game file, word 0/3) -> value */
    bool trail_set = false; std::vector<std::array<uint32_t, 4>> trail;
    bool head_set = false; Bytes head_extra;  /* the bytes between the tables and the data */
    bool tail_set = false; Bytes tail;
    size_t new_files = 0;
};

/* A packed file that lies outside its own chunk (the game's i2.xpps lists, as the
 * last file of chunk 0, a buffer that sits at the start of the texture chunk): it
 * owns no bytes of its chunk, and keeps its place relative to the chunk it lies in. */
static bool is_alias(const Arc& A, size_t j, uint32_t fi)
{
    const auto& c = A.chunks[j];
    const uint32_t o = A.files[fi][2], e = o + A.files[fi][1];
    if (o >= c[2] && e <= c[2] + c[1]) return false;
    return o >= c[2] + c[1];
}
struct Alias { uint32_t file; size_t in_chunk; uint32_t rel; };
static std::vector<Alias> aliases_of(const Arc& A)
{
    std::vector<Alias> v;
    for (size_t j = 0; j < A.chunks.size(); j++) {
        const auto& c = A.chunks[j];
        if (c[0] == 0xFF02) continue;
        for (uint32_t t = 0; t < c[6]; t++) {
            const uint32_t fi = c[5] + t;
            if (!is_alias(A, j, fi)) continue;
            const uint32_t o = A.files[fi][2];
            size_t k = SIZE_MAX;
            for (size_t i = 0; i < A.chunks.size(); i++)
                if (i != j && A.chunks[i][0] != 0xFF02 && o >= A.chunks[i][2] && o + A.files[fi][1] <= A.chunks[i][2] + A.chunks[i][1]) k = i;
            if (k == SIZE_MAX) continue;
            v.push_back({ fi, k, o - A.chunks[k][2] });
        }
    }
    return v;
}

/* the game's units: each packed file's span (to the next file, or the chunk's end) */
struct Unit { uint32_t g0, g1, file; int chunk; };
static std::vector<Unit> units_of(const Arc& A, std::string& err)
{
    std::vector<Unit> u;
    for (size_t j = 0; j < A.chunks.size(); j++) {
        const auto& c = A.chunks[j];
        if (c[0] == 0xFF02) continue;
        if (!c[6]) { if (c[1]) { err = "a chunk with data and no packed file"; return {}; } continue; }
        std::vector<uint32_t> own;
        for (uint32_t t = 0; t < c[6]; t++) if (!is_alias(A, j, c[5] + t)) own.push_back(c[5] + t);
        if (own.empty() && c[1]) { err = "a chunk with data and no packed file"; return {}; }
        for (size_t t = 0; t < own.size(); t++) {
            const uint32_t fi = own[t];
            const uint32_t s0 = t ? A.files[fi][2] : c[2];
            const uint32_t s1 = t + 1 < own.size() ? A.files[own[t + 1]][2] : c[2] + c[1];
            if (A.files[fi][2] < s0 || A.files[fi][2] + A.files[fi][1] > s1 || s1 < s0) { err = "packed files out of order"; return {}; }
            u.push_back({ s0, s1, fi, (int)j });
        }
    }
    {   /* every alias must sit inside some other chunk */
        size_t n = 0;
        for (size_t j = 0; j < A.chunks.size(); j++) if (A.chunks[j][0] != 0xFF02)
            for (uint32_t t = 0; t < A.chunks[j][6]; t++) if (is_alias(A, j, A.chunks[j][5] + t)) n++;
        if (n != aliases_of(A).size()) { err = "a packed file outside every chunk"; return {}; }
    }
    return u;
}

/* the section index (over every ILNK section of every link file, in order) of each pointer */
static std::vector<std::pair<uint32_t, uint32_t>> ptr_secs(const Arc& A)   /* (word index, section), sorted */
{
    std::vector<std::pair<uint32_t, uint32_t>> v;
    uint32_t si = 0;
    for (const LinkFile& L : A.links)
        for (const Block& b : L.blocks)
            if (b.magic == "ILNK") for (const IlnkSec& s : b.secs) { for (uint32_t l : s.locs) v.push_back({ l, si }); si++; }
    std::sort(v.begin(), v.end());
    return v;
}
static std::string link_shape(const Arc& A)
{
    std::string s;
    for (const LinkFile& L : A.links) {
        s += "|";
        for (const Block& b : L.blocks) {
            s += b.magic;
            if (b.magic == "ILNK") for (const IlnkSec& x : b.secs) s += "." + std::to_string(x.kind);
        }
    }
    return s;
}

/* M's segments: [m0, m1) is game bytes from g0 (blob -1) or blob `blob` from offset g0 */
struct Seg { uint32_t m0, m1; int blob; uint32_t g0; };

struct Aligner {
    const Arc& G; const Arc& M;
    std::vector<Seg> segs;                     /* sorted by m0 */
    std::vector<uint32_t> gp, mp;              /* pointer word indexes, sorted */
    struct GM { uint32_t g0, g1, m0; };
    std::vector<GM> gmap;                      /* the game's bytes -> the mod's, as far as known (sorted) */
    uint32_t g2m(uint32_t v) const {
        size_t lo = 0, hi = gmap.size();
        while (lo < hi) { const size_t mid = (lo + hi) / 2; if (gmap[mid].g1 <= v) lo = mid + 1; else hi = mid; }
        if (lo < gmap.size() && gmap[lo].g0 <= v) return gmap[lo].m0 + (v - gmap[lo].g0);
        if (lo > 0 && gmap[lo - 1].g1 == v) return gmap[lo - 1].m0 + (v - gmap[lo - 1].g0);
        return 0xFFFFFFFFu;
    }
    /* a word as compared: a pointer by where it points (the game's mapped into the mod's), else its value */
    uint64_t gtok(uint32_t a) const {
        const uint32_t v = rd32(&G.data[a]);
        if (gptr(a)) { const uint32_t m = g2m(v); return (1ull << 63) | (m == 0xFFFFFFFFu ? (1ull << 40) | v : m); }
        return v;
    }
    uint64_t mtok(uint32_t a) const {
        const uint32_t v = rd32(&M.data[a]);
        return mptr(a) ? ((1ull << 63) | v) : v;
    }
    std::string err;
    Aligner(const Arc& g, const Arc& m) : G(g), M(m) {}
    bool gptr(uint32_t a) const { return !(a & 3) && std::binary_search(gp.begin(), gp.end(), a >> 2); }
    bool mptr(uint32_t a) const { return !(a & 3) && std::binary_search(mp.begin(), mp.end(), a >> 2); }
    /* words that match: equal bytes, or a pointer in both */
    bool wmatch(uint32_t ga, uint32_t ma) const {
        if (!memcmp(&G.data[ga], &M.data[ma], 4)) return true;
        return gptr(ga) && mptr(ma);
    }
    Ref mref(uint32_t v) const {
        size_t lo = 0, hi = segs.size();
        while (lo < hi) { const size_t mid = (lo + hi) / 2; if (segs[mid].m1 <= v) lo = mid + 1; else hi = mid; }
        if (lo < segs.size() && segs[lo].m0 <= v && v < segs[lo].m1) {
            const Seg& s = segs[lo];
            return { s.blob, s.g0 + (v - s.m0) };
        }
        return { -2, v };
    }
};

/* Where M changed game unit [g0,g1) (now [m0,m1)): the edit window, as a cut
 * of the game's bytes and/or an insertion, the rest mapped. */
static void align_unit(Aligner& al, Delta& D, uint32_t ui, const Unit& gu, uint32_t m0, uint32_t m1, uint32_t file_end_g)
{
    const Arc& G = al.G; const Arc& M = al.M;
    const uint32_t g0 = gu.g0, g1 = gu.g1, lg = g1 - g0, lm = m1 - m0, mn = (std::min)(lg, lm);
    if (lg == lm) {
        al.segs.push_back({ m0, m1, -1, g0 });
        return;
    }
    uint32_t p = 0;
    while (p + 4 <= mn && al.wmatch(g0 + p, m0 + p)) p += 4;
    uint32_t s = 0;
    while (s + 4 <= mn && al.wmatch(g1 - s - 4, m1 - s - 4)) s += 4;
    if (p + s > mn) {   /* an insertion or a cut somewhere in an equal stretch: place it */
        const bool ins = lm > lg;
        const uint32_t n = ins ? lm - lg : lg - lm;
        uint32_t lo = g0 + (mn - s), hi = g0 + p;   /* the game address it can go before */
        if (lo > hi) lo = hi;
        if (ins) {   /* on the 16-byte step of the tools that insert, the latest such place */
            uint32_t X = hi;
            for (uint32_t c = hi + 1; c-- > lo;) if (((c - g0) & 15) == 0) { X = c; break; }
            const uint32_t mX = m0 + (X - g0);
            if (X > g0) al.segs.push_back({ m0, mX, -1, g0 });
            Blob b;
            b.unit = ui; b.at = X; b.from = D.name;
            b.in_file = X <= file_end_g ? n : 0;
            b.b.assign(M.data.begin() + mX, M.data.begin() + mX + n);
            al.segs.push_back({ mX, mX + n, (int)D.blobs.size(), 0 });
            D.blobs.push_back(std::move(b));
            if (mX + n < m1) al.segs.push_back({ mX + n, m1, -1, X });
        } else {
            uint32_t X = hi;
            for (uint32_t c = hi + 1; c-- > lo;) if (((c - g0) & 15) == 0) { X = c; break; }
            const uint32_t mX = m0 + (X - g0);
            if (X > g0) al.segs.push_back({ m0, mX, -1, g0 });
            D.cuts.push_back({ ui, X, n });
            if (mX < m1) al.segs.push_back({ mX, m1, -1, X + n });
        }
        return;
    }
    /* a stretch replaced: the game's middle cut, the mod's middle new */
    if (p) al.segs.push_back({ m0, m0 + p, -1, g0 });
    const uint32_t gc = g0 + p, gl = lg - p - s, ml = lm - p - s;
    if (gl) D.cuts.push_back({ ui, gc, gl });
    if (ml) {
        Blob b;
        b.unit = ui; b.at = gc + gl; b.from = D.name;
        /* how much of it is inside the packed file: the file's end moved by the difference */
        b.in_file = gc + gl <= file_end_g ? ml : 0;
        b.b.assign(M.data.begin() + m0 + p, M.data.begin() + m0 + p + ml);
        al.segs.push_back({ m0 + p, m0 + p + ml, (int)D.blobs.size(), 0 });
        D.blobs.push_back(std::move(b));
    }
    if (s) al.segs.push_back({ m1 - s, m1, -1, g1 - s });
}

/* Word-level alignment of a changed unit: equal runs (pointers compared by
 * target) are the game's bytes; between them, the game's words are cut and the
 * mod's are new. Unique words anchor the match (patience), recursively; a stretch
 * without anchors of the same length is kept in place (its changes are edits). */
struct Hunk2 { uint32_t a, an, b, bn; };   /* word indexes: game a.., mod b.. */
static void pdiff(const std::vector<uint64_t>& A, const std::vector<uint64_t>& B, uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1,
                  std::vector<Hunk2>& out, int depth)
{
    while (a0 < a1 && b0 < b1 && A[a0] == B[b0]) { a0++; b0++; }
    while (a0 < a1 && b0 < b1 && A[a1 - 1] == B[b1 - 1]) { a1--; b1--; }
    if (a0 == a1 && b0 == b1) return;
    if (a0 == a1 || b0 == b1 || depth > 64) { out.push_back({ a0, a1 - a0, b0, b1 - b0 }); return; }
    std::unordered_map<uint64_t, std::pair<int64_t, int64_t>> cnt;   /* token -> (pos in A or -2 if repeated, pos in B ...) */
    cnt.reserve((a1 - a0) + (b1 - b0));
    for (uint32_t i = a0; i < a1; i++) { auto& c = cnt[A[i]]; c.first = c.first == 0 ? (int64_t)i + 1 : -1; }
    for (uint32_t i = b0; i < b1; i++) { auto it = cnt.find(B[i]); if (it == cnt.end()) continue; auto& c = it->second; c.second = c.second == 0 ? (int64_t)i + 1 : -1; }
    std::vector<std::pair<uint32_t, uint32_t>> pairs;   /* (a, b) of words unique in both */
    for (uint32_t i = b0; i < b1; i++) {
        auto it = cnt.find(B[i]);
        if (it == cnt.end() || it->second.first <= 0 || it->second.second <= 0) continue;
        pairs.push_back({ (uint32_t)(it->second.first - 1), i });
    }
    if (pairs.empty()) {
        if (a1 - a0 == b1 - b0) return;   /* same length: in place, compared word by word later */
        out.push_back({ a0, a1 - a0, b0, b1 - b0 });
        return;
    }
    /* longest increasing run of a over b order (patience sorting) */
    std::vector<size_t> tails, prev(pairs.size(), (size_t)-1);
    std::vector<uint32_t> tv;
    for (size_t k = 0; k < pairs.size(); k++) {
        const uint32_t a = pairs[k].first;
        size_t pos = std::lower_bound(tv.begin(), tv.end(), a) - tv.begin();
        if (pos == tv.size()) { tv.push_back(a); tails.push_back(k); } else { tv[pos] = a; tails[pos] = k; }
        prev[k] = pos ? tails[pos - 1] : (size_t)-1;
    }
    std::vector<std::pair<uint32_t, uint32_t>> lis;
    for (size_t k = tails.empty() ? (size_t)-1 : tails.back(); k != (size_t)-1; k = prev[k]) lis.push_back(pairs[k]);
    std::reverse(lis.begin(), lis.end());
    uint32_t pa = a0, pb = b0;
    for (const auto& m : lis) {
        pdiff(A, B, pa, m.first, pb, m.second, out, depth + 1);
        pa = m.first + 1; pb = m.second + 1;
    }
    pdiff(A, B, pa, a1, pb, b1, out, depth + 1);
}

static bool align_words(Aligner& al, Delta& D, uint32_t ui, const Unit& gu, uint32_t m0, uint32_t m1, uint32_t file_end_g)
{
    const uint32_t g0 = gu.g0, g1 = gu.g1;
    if ((g0 | g1 | m0 | m1) & 3) return false;
    const uint32_t na = (g1 - g0) / 4, nb = (m1 - m0) / 4;
    std::vector<uint64_t> A(na), B(nb);
    for (uint32_t i = 0; i < na; i++) A[i] = al.gtok(g0 + 4 * i);
    for (uint32_t i = 0; i < nb; i++) B[i] = al.mtok(m0 + 4 * i);
    std::vector<Hunk2> h;
    pdiff(A, B, 0, na, 0, nb, h, 0);
    std::sort(h.begin(), h.end(), [](const Hunk2& x, const Hunk2& y) { return x.a < y.a; });
    uint32_t ga = 0, mb = 0;
    for (const Hunk2& x : h) {
        if (x.a - ga != x.b - mb) return false;
        if (x.a > ga) al.segs.push_back({ m0 + 4 * mb, m0 + 4 * x.b, -1, g0 + 4 * ga });
        const uint32_t cut_at = g0 + 4 * x.a;
        if (x.an) D.cuts.push_back({ ui, cut_at, 4 * x.an });
        if (x.bn) {
            Blob b;
            b.unit = ui; b.at = cut_at + 4 * x.an; b.from = D.name;
            b.in_file = b.at <= file_end_g ? 4 * x.bn : 0;
            b.b.assign(al.M.data.begin() + m0 + 4 * x.b, al.M.data.begin() + m0 + 4 * (x.b + x.bn));
            al.segs.push_back({ m0 + 4 * x.b, m0 + 4 * (x.b + x.bn), (int)D.blobs.size(), 0 });
            D.blobs.push_back(std::move(b));
        }
        ga = x.a + x.an; mb = x.b + x.bn;
    }
    if (na - ga != nb - mb) return false;
    if (na > ga) al.segs.push_back({ m0 + 4 * mb, m1, -1, g0 + 4 * ga });
    return true;
}

static bool sig_slice(const Block& b, uint32_t off, uint32_t nargs, std::vector<uint32_t>& out)
{
    if (off & 3) return false;
    const size_t k = off / 4, n = 3ull * nargs;
    if (k + n > b.sig.size()) return false;
    out.assign(b.sig.begin() + k, b.sig.begin() + k + n);
    return true;
}

/* The mod's file M against the game's G. */
static bool make_delta(const Arc& G, const Arc& M, const std::string& name, Delta& D)
{
    D = Delta();
    D.name = name;
    auto fail = [&](const std::string& e) { D.err = e; return false; };
    if (G.nc != M.nc) return fail("another number of chunks");
    for (size_t j = 0; j < G.nc; j++) {
        if (G.chunks[j][0] != M.chunks[j][0]) return fail("other chunk types");
        if (M.chunks[j][6] < G.chunks[j][6]) return fail("a chunk with fewer packed files");
        if (G.chunks[j][0] == 0xFF02 && M.chunks[j][6] != G.chunks[j][6]) return fail("other link files");
        for (int w : { 3, 4 }) if (G.chunks[j][w] != M.chunks[j][w]) D.chunk_w[{ j, w }] = M.chunks[j][w];
    }
    if (link_shape(G) != link_shape(M)) return fail("link tables of another shape");
    std::string e;
    const std::vector<Unit> gu = units_of(G, e);
    if (!e.empty()) return fail(e);
    const std::vector<Unit> mu = units_of(M, e);
    if (!e.empty()) return fail(e);
    Aligner al(G, M);
    for (const auto& x : ptr_secs(G)) al.gp.push_back(x.first);
    for (const auto& x : ptr_secs(M)) al.mp.push_back(x.first);
    /* game file index -> mod file index */
    std::vector<uint32_t> fmap(G.nf, 0);
    for (size_t j = 0; j < G.nc; j++)
        for (uint32_t t = 0; t < G.chunks[j][6]; t++) fmap[G.chunks[j][5] + t] = M.chunks[j][5] + t;
    std::map<uint32_t, const Unit*> mu_by_file;
    for (const Unit& u : mu) mu_by_file[u.file] = &u;
    /* first, the moves the packed-file table shows: each unit's start (exact for units
       that kept their size); then once more with the moves found inside units */
    for (uint32_t ui = 0; ui < gu.size(); ui++) {
        auto it = mu_by_file.find(fmap[gu[ui].file]);
        if (it == mu_by_file.end()) return fail("a packed file is missing");
        al.gmap.push_back({ gu[ui].g0, gu[ui].g1, it->second->g0 });
    }
    std::sort(al.gmap.begin(), al.gmap.end(), [](const Aligner::GM& a, const Aligner::GM& b) { return a.g0 < b.g0; });
    for (int pass = 0; pass < 2; pass++) {
    if (pass) {
        al.gmap.clear();
        for (const Seg& x : al.segs) if (x.blob < 0) al.gmap.push_back({ x.g0, x.g0 + (x.m1 - x.m0), x.m0 });
        std::sort(al.gmap.begin(), al.gmap.end(), [](const Aligner::GM& a, const Aligner::GM& b) { return a.g0 < b.g0; });
        al.segs.clear(); D.blobs.clear(); D.cuts.clear();
    }
    for (uint32_t ui = 0; ui < gu.size(); ui++) {
        const Unit& g = gu[ui];
        const uint32_t mf = fmap[g.file];
        auto it = mu_by_file.find(mf);
        if (it == mu_by_file.end()) return fail("a packed file is missing");
        const Unit& m = *it->second;
        /* the mod's unit ends where its first new packed file of the chunk starts */
        uint32_t m1 = m.g1;
        const auto& mc = M.chunks[(size_t)g.chunk];
        const auto& gc = G.chunks[(size_t)g.chunk];
        const bool last = ui + 1 == gu.size() || gu[ui + 1].chunk != g.chunk;
        if (last && mc[6] > gc[6]) m1 = M.files[mc[5] + gc[6]][2];
        if (last && mc[6] == gc[6]) m1 = mc[2] + mc[1];
        const uint32_t fend = G.files[g.file][2] + G.files[g.file][1];
        const size_t nb = D.blobs.size(), ncut = D.cuts.size();
        if (m1 - m.g0 == g.g1 - g.g0) al.segs.push_back({ m.g0, m1, -1, g.g0 });
        else {
            const size_t sb = D.blobs.size(), sc = D.cuts.size(), ss = al.segs.size();
            if (!align_words(al, D, ui, g, m.g0, m1, fend)) {
                D.blobs.resize(sb); D.cuts.resize(sc); al.segs.resize(ss);
                align_unit(al, D, ui, g, m.g0, m1, fend);
            }
        }
        /* check the packed file's own offset and size come out as the mod has them */
        int64_t before = 0, inside = 0;   /* bytes added before the file's start / inside it */
        for (size_t k = nb; k < D.blobs.size(); k++) {
            const Blob& b = D.blobs[k];
            if (b.at < G.files[g.file][2]) before += (int64_t)b.b.size();
            else inside += b.in_file;
        }
        for (size_t k = ncut; k < D.cuts.size(); k++) {
            const Cut& c = D.cuts[k];
            if (c.at < G.files[g.file][2]) before -= c.len;
            else inside -= (int64_t)(std::min<uint64_t>)(c.len, fend > c.at ? fend - c.at : 0);
        }
        const int64_t moff = (int64_t)m.g0 + ((int64_t)G.files[g.file][2] - g.g0) + before;
        const int64_t msz = (int64_t)G.files[g.file][1] + inside;
        if (moff != M.files[mf][2] || msz != M.files[mf][1]) {
            /* an edit at the file's end: give the last such new range's bytes to the file as the mod did */
            int last_end = -1;
            for (size_t k = nb; k < D.blobs.size(); k++) if (D.blobs[k].at == fend) last_end = (int)k;
            const int64_t diff = (int64_t)M.files[mf][1] - msz;
            if (last_end >= 0 && moff == M.files[mf][2] && (int64_t)D.blobs[(size_t)last_end].in_file + diff >= 0 &&
                (int64_t)D.blobs[(size_t)last_end].in_file + diff <= (int64_t)D.blobs[(size_t)last_end].b.size())
                D.blobs[(size_t)last_end].in_file = (uint32_t)((int64_t)D.blobs[(size_t)last_end].in_file + diff);
            else
                return fail("packed file " + std::to_string(g.file) + " changed in a way not understood");
        }
        if (pass) for (int w : { 0, 3 }) if (G.files[g.file][w] != M.files[mf][w]) D.file_w[{ g.file, w }] = M.files[mf][w];
    }
    }
    {   /* packed files that lie in another chunk keep their place there */
        const auto ga = aliases_of(G), ma = aliases_of(M);
        if (ga.size() != ma.size()) return fail("a packed file moved out of its chunk");
        for (size_t i = 0; i < ga.size(); i++)
            if (fmap[ga[i].file] != ma[i].file || ga[i].in_chunk != ma[i].in_chunk || ga[i].rel != ma[i].rel ||
                G.files[ga[i].file] [1] != M.files[ma[i].file][1] || G.files[ga[i].file][0] != M.files[ma[i].file][0])
                return fail("packed file " + std::to_string(ga[i].file) + " (in another chunk) changed");
    }
    /* new packed files */
    for (size_t j = 0; j < G.nc; j++) {
        const auto& gc = G.chunks[j]; const auto& mc = M.chunks[j];
        for (uint32_t t = gc[6]; t < mc[6]; t++) {
            const uint32_t mf = mc[5] + t;
            const uint32_t s0 = M.files[mf][2], s1 = t + 1 < mc[6] ? M.files[mf + 1][2] : mc[2] + mc[1];
            Blob b;
            b.chunk = (int)j; b.ftype = M.files[mf][0]; b.from = D.name;
            b.in_file = M.files[mf][1];
            if (M.files[mf][3]) return fail("a new packed file with an unknown word");
            b.b.assign(M.data.begin() + s0, M.data.begin() + s1);
            al.segs.push_back({ s0, s1, (int)D.blobs.size(), 0 });
            D.blobs.push_back(std::move(b));
            D.new_files++;
        }
    }
    std::sort(al.segs.begin(), al.segs.end(), [](const Seg& a, const Seg& b) { return a.m0 < b.m0; });
    /* pointers and bytes */
    const auto gs = ptr_secs(G), ms = ptr_secs(M);
    std::map<uint32_t, uint32_t> gsec(gs.begin(), gs.end()), msec(ms.begin(), ms.end());
    std::set<uint32_t> g_seen;
    for (const Seg& s : al.segs) {
        if (s.blob >= 0) {
            Blob& b = D.blobs[(size_t)s.blob];
            for (auto it = msec.lower_bound(s.m0 >> 2); it != msec.end() && (it->first << 2) < s.m1; ++it) {
                const uint32_t ma = it->first << 2;
                b.ptrs[ma - s.m0 + s.g0] = { it->second, al.mref(rd32(&M.data[ma])) };
            }
            continue;
        }
        for (uint32_t k = 0; k < s.m1 - s.m0; k++) {
            const uint32_t ga = s.g0 + k, ma = s.m0 + k;
            if (!(ga & 3) && !(ma & 3)) {
                auto gi = gsec.find(ga >> 2);
                auto mi = msec.find(ma >> 2);
                const bool gpt = gi != gsec.end(), mpt = mi != msec.end();
                if (gpt) g_seen.insert(ga);
                if (gpt || mpt) {
                    if (mpt) {
                        const Ptr P{ mi->second, al.mref(rd32(&M.data[ma])) };
                        const Ptr Q{ gpt ? gi->second : 0, Ref{ -1, gpt ? rd32(&G.data[ga]) : 0 } };
                        /* the game's own pointer: its value mapped the way the game's data moved */
                        if (!gpt || !(P == Q)) D.ptr_set[ga] = P;
                    } else {
                        D.ptr_del.insert(ga);
                        for (int q = 0; q < 4; q++) if (G.data[ga + q] != M.data[ma + q]) D.bytes[ga + q] = M.data[ma + q];
                    }
                    k += 3;
                    continue;
                }
            }
            if (G.data[ga] != M.data[ma]) D.bytes[ga] = M.data[ma];
        }
    }
    /* a game pointer whose word was cut: nothing to do (gone with it). The value of
       a kept game pointer is the game's value mapped through the game's moves; check
       below that it is what the mod has (else it was set explicitly). */
    /* CID */
    std::set<uint32_t> gcid, mcid_g;
    for (const LinkFile& L : G.links) for (const Block& b : L.blocks) if (b.magic == "CID ") for (uint32_t l : b.cid) gcid.insert(l << 2);
    for (const LinkFile& L : M.links)
        for (const Block& b : L.blocks)
            if (b.magic == "CID ")
                for (uint32_t l : b.cid) {
                    const Ref r = al.mref(l << 2);
                    if (r.blob >= 0) D.blobs[(size_t)r.blob].cids.insert(r.off);
                    else if (r.blob == -1) { mcid_g.insert(r.off); if (!gcid.count(r.off)) D.cid_add.insert(r.off); }
                    else return fail("a class id outside the data");
                }
    for (uint32_t a : gcid) if (!mcid_g.count(a)) D.cid_del.insert(a);
    /* lists */
    for (size_t li = 0; li < G.links.size(); li++)
        for (size_t bi = 0; bi < G.links[li].blocks.size(); bi++) {
            const Block& gb = G.links[li].blocks[bi];
            const Block& mb = M.links[li].blocks[bi];
            if (gb.magic != "OTYP" && gb.magic != "XLNK" && gb.magic != "SPLP") {
                if (gb.magic != "ILNK" && gb.magic != "CID " && gb.raw != mb.raw) return fail("a link block " + gb.magic + " changed");
                continue;
            }
            if (gb.magic == "XLNK" && (gb.x0 != mb.x0 || gb.x1 != mb.x1)) return fail("XLNK header changed");
            if (gb.magic == "SPLP") {
                if (mb.sig.size() < gb.sig.size() || !std::equal(gb.sig.begin(), gb.sig.end(), mb.sig.begin()))
                    return fail("the native-call arguments were rewritten, not added to");
                D.sig_extra[{ li, bi }].assign(mb.sig.begin() + gb.sig.size(), mb.sig.end());
            }
            auto entries = [&](const Arc& A, const Block& b, bool game, std::vector<Entry>& out, std::vector<uint32_t>& sigoff) -> bool {
                auto ref = [&](uint32_t a) { return game ? Ref{ -1, a } : al.mref(a); };
                for (const Otyp& o : b.otyp) { Entry x; x.kind = 0; x.at = ref(o.addr); x.w = { o.a, o.b }; out.push_back(x); sigoff.push_back(0); }
                for (const Xlnk& o : b.xlnk) { Entry x; x.kind = 1; x.at = ref(o.addr); x.w = { o.v }; memcpy(x.key, o.key, 16); out.push_back(x); sigoff.push_back(0); }
                for (const auto& o : b.s1) { Entry x; x.kind = 2; x.at = ref(o[0]); x.w = { o[1], o[2] }; out.push_back(x); sigoff.push_back(0); }
                for (const auto& o : b.s2) {
                    Entry x; x.kind = 3; x.at = ref(o[0]); x.w = { o[2], o[3], o[4], o[5], o[6] };
                    if (!sig_slice(b, o[1], o[6] >> 24, x.sig)) return false;
                    out.push_back(x); sigoff.push_back(o[1]);
                }
                for (const auto& o : b.s3) {
                    Entry x; x.kind = 4; x.at = ref(o[0]); x.w = { o[2], o[3], o[4], o[5] };
                    if (!sig_slice(b, o[1], o[5] >> 24, x.sig)) return false;
                    out.push_back(x); sigoff.push_back(o[1]);
                }
                (void)A;
                return true;
            };
            std::vector<Entry> ge, me;
            std::vector<uint32_t> gso, mso;
            if (!entries(G, gb, true, ge, gso) || !entries(M, mb, false, me, mso)) return fail("a native call's arguments are out of range");
            for (const Entry& x : me) if (x.at.blob == -2) return fail("a link entry outside the data");
            std::map<Entry, std::vector<int>> by;
            for (int i = (int)ge.size(); i-- > 0;) by[ge[(size_t)i]].push_back(i);
            List Lst;
            for (size_t i = 0; i < me.size(); i++) {
                Item it;
                auto f = by.find(me[i]);
                if (f != by.end() && !f->second.empty() && (me[i].kind < 3 || gso[(size_t)f->second.back()] == mso[i])) {
                    it.g = f->second.back(); f->second.pop_back();
                } else {
                    it.e = me[i];
                    if (me[i].kind >= 3) {   /* a new entry: its arguments must be among the new words */
                        if (mso[i] < 4 * gb.sig.size()) {
                            /* it reuses the game's words: keep the offset as it is (a word index into the game's part) */
                            it.e.w.push_back(mso[i]);
                            it.e.w.push_back(0xFFFFFFFFu);
                        } else {
                            it.e.w.push_back(mso[i] - 4 * (uint32_t)gb.sig.size());
                            it.e.w.push_back(0xFFFFFFFEu);
                        }
                    }
                }
                Lst.items.push_back(std::move(it));
            }
            D.lists[{ li, bi }] = std::move(Lst);
        }
    /* tables */
    if (G.trail != M.trail) { D.trail_set = true; D.trail = M.trail; }
    {
        const uint32_t ge0 = G.f0 + 16 * G.nf + 16 * G.ntrail, me0 = M.f0 + 16 * M.nf + 16 * M.ntrail;
        const Bytes gx(G.head.begin() + ge0, G.head.end()), mx(M.head.begin() + me0, M.head.end());
        Bytes gh(G.head.begin(), G.head.begin() + G.tab + 24), mh(M.head.begin(), M.head.begin() + M.tab + 24);
        for (int w : { 28, 40, 44 }) { memset(&gh[(size_t)w], 0, 4); memset(&mh[(size_t)w], 0, 4); }
        memset(&gh[G.tab + 12], 0, 4); memset(&mh[M.tab + 12], 0, 4);
        memset(&gh[G.tab + 20], 0, 4); memset(&mh[M.tab + 20], 0, 4);
        if (gh != mh) return fail("the file's header changed");
        if (gx != mx) { D.head_set = true; D.head_extra = mx; }
    }
    if (G.tail != M.tail) { D.tail_set = true; D.tail = M.tail; }
    return true;
}


/* ---- a delta written onto the game's file ---------------------------------------- */

static bool apply_delta(const Arc& G, const Delta& D, Bytes& out, std::string& err, std::vector<uint32_t>* bpos_out = nullptr)
{
    std::string e;
    const std::vector<Unit> gu = units_of(G, e);
    if (!e.empty()) { err = e; return false; }
    /* link chunks after everything else */
    {
        uint32_t last_data = 0, first_link = 0xFFFFFFFFu;
        for (const auto& c : G.chunks) {
            if (c[0] == 0xFF02) first_link = (std::min)(first_link, c[2]);
            else if (c[1]) last_data = (std::max)(last_data, c[2] + c[1]);
        }
        if (first_link < last_data) { err = "link tables before the data"; return false; }
        for (size_t j = 1; j < G.chunks.size(); j++)
            if (G.chunks[j][2] < G.chunks[j - 1][2] + G.chunks[j - 1][1] && G.chunks[j][1]) { err = "chunks out of order"; return false; }
    }
    /* events per unit */
    std::vector<std::vector<std::pair<uint32_t, int>>> ev(gu.size());   /* (at, blob index or -1-cut index) */
    for (size_t k = 0; k < D.blobs.size(); k++) if (D.blobs[k].chunk < 0) {
        if (D.blobs[k].unit >= gu.size()) { err = "a new range in no packed file"; return false; }
        ev[D.blobs[k].unit].push_back({ D.blobs[k].at, (int)k });
    }
    for (size_t k = 0; k < D.cuts.size(); k++) {
        if (D.cuts[k].unit >= gu.size()) { err = "a cut in no packed file"; return false; }
        ev[D.cuts[k].unit].push_back({ D.cuts[k].at, -1 - (int)k });
    }
    for (auto& v : ev) std::stable_sort(v.begin(), v.end(), [](const std::pair<uint32_t, int>& a, const std::pair<uint32_t, int>& b) {
        if (a.first != b.first) return a.first < b.first;
        return (a.second < 0) > (b.second < 0);   /* a cut before new bytes at the same place */
    });
    Bytes O;
    O.reserve(G.data.size() + 0x100000);
    struct GSeg { uint32_t g0, g1, o0; };
    std::vector<GSeg> gm;                       /* game bytes -> output */
    std::vector<uint32_t> bpos(D.blobs.size(), 0xFFFFFFFFu);
    std::vector<std::array<uint32_t, 7>> chunks = G.chunks;
    std::vector<std::array<uint32_t, 4>> files;
    for (const auto& kv : D.chunk_w) chunks[kv.first.first][(size_t)kv.first.second] = kv.second;
    auto copy_g = [&](uint32_t a, uint32_t b) {
        if (b <= a) return;
        gm.push_back({ a, b, (uint32_t)O.size() });
        O.insert(O.end(), G.data.begin() + a, G.data.begin() + b);
    };
    std::vector<size_t> unit_of_file(G.nf, (size_t)-1);
    for (size_t u = 0; u < gu.size(); u++) unit_of_file[gu[u].file] = u;
    std::vector<uint32_t> link_files;
    for (size_t j = 0; j < G.chunks.size(); j++) {
        const auto& gc = G.chunks[j];
        chunks[j][5] = (uint32_t)files.size();
        if (gc[0] == 0xFF02) {   /* written below */
            for (uint32_t t = 0; t < gc[6]; t++) { files.push_back(G.files[gc[5] + t]); link_files.push_back((uint32_t)files.size() - 1); }
            continue;
        }
        const uint32_t c0 = (uint32_t)O.size();
        if (!gc[6] && gc[1]) { err = "a chunk with data and no packed file"; return false; }
        for (uint32_t t = 0; t < gc[6]; t++) {
            const uint32_t fi = gc[5] + t;
            if (unit_of_file[fi] == (size_t)-1) { files.push_back(G.files[fi]); continue; }   /* an alias: placed below */
            const size_t ui = unit_of_file[fi];
            const Unit& u = gu[ui];
            const uint32_t F = G.files[fi][2], Fe = F + G.files[fi][1];
            uint32_t cur = u.g0, o_file = 0xFFFFFFFFu;
            int64_t fsize = G.files[fi][1];
            for (const auto& x : ev[ui]) {
                if (x.first < cur) { err = "overlapping changes in packed file " + std::to_string(fi); return false; }
                if (o_file == 0xFFFFFFFFu && F >= cur && F <= x.first) { copy_g(cur, F); cur = F; o_file = (uint32_t)O.size(); }
                copy_g(cur, x.first);
                cur = x.first;
                if (x.second >= 0) {
                    const Blob& b = D.blobs[(size_t)x.second];
                    bpos[(size_t)x.second] = (uint32_t)O.size();
                    O.insert(O.end(), b.b.begin(), b.b.end());
                    if (x.first >= F && x.first <= Fe) fsize += b.in_file;
                } else {
                    const Cut& c = D.cuts[(size_t)(-1 - x.second)];
                    if (c.at + c.len > u.g1) { err = "a cut past its packed file"; return false; }
                    if (c.at < Fe && c.at + c.len > F) fsize -= (int64_t)(std::min)(c.at + c.len, Fe) - (std::max)(c.at, F);
                    cur = c.at + c.len;
                }
            }
            if (o_file == 0xFFFFFFFFu) {
                if (F < cur) { err = "packed file " + std::to_string(fi) + " starts inside a change"; return false; }
                copy_g(cur, F); cur = F; o_file = (uint32_t)O.size();
            }
            copy_g(cur, u.g1);
            std::array<uint32_t, 4> f = G.files[fi];
            f[2] = o_file; f[1] = (uint32_t)fsize;
            for (int w : { 0, 3 }) { auto it = D.file_w.find({ fi, w }); if (it != D.file_w.end()) f[(size_t)w] = it->second; }
            files.push_back(f);
        }
        for (size_t k = 0; k < D.blobs.size(); k++) {
            const Blob& b = D.blobs[k];
            if (b.chunk != (int)j) continue;
            bpos[k] = (uint32_t)O.size();
            files.push_back({ b.ftype, b.in_file, (uint32_t)O.size(), 0 });
            O.insert(O.end(), b.b.begin(), b.b.end());
        }
        chunks[j][2] = c0;
        chunks[j][1] = (uint32_t)O.size() - c0;
        chunks[j][6] = (uint32_t)files.size() - chunks[j][5];
    }
    for (size_t k = 0; k < D.blobs.size(); k++) if (bpos[k] == 0xFFFFFFFFu) { err = "a new range with no place"; return false; }
    for (const Alias& a : aliases_of(G)) {
        uint32_t ofi = 0;   /* its index in the output: same position in its chunk */
        size_t j = 0;
        while (j < G.chunks.size() && !(a.file >= G.chunks[j][5] && a.file < G.chunks[j][5] + G.chunks[j][6])) j++;
        if (j == G.chunks.size()) { err = "an alias in no chunk"; return false; }
        ofi = chunks[j][5] + (a.file - G.chunks[j][5]);
        files[ofi][2] = chunks[a.in_chunk][2] + a.rel;
    }
    const uint32_t data_end = (uint32_t)O.size();
    /* a game byte's place in the output; end_ok: a pointer value may be one past a stretch */
    auto g2o = [&](uint32_t a, bool& ok, bool end_ok = false) -> uint32_t {
        size_t lo = 0, hi = gm.size();
        while (lo < hi) { const size_t mid = (lo + hi) / 2; if (gm[mid].g1 <= a) lo = mid + 1; else hi = mid; }
        if (lo < gm.size() && gm[lo].g0 <= a) return gm[lo].o0 + (a - gm[lo].g0);
        if (end_ok && lo > 0 && gm[lo - 1].g1 == a) return gm[lo - 1].o0 + (gm[lo - 1].g1 - gm[lo - 1].g0);
        ok = false;
        return 0;
    };
    auto r2o = [&](const Ref& r, bool& ok) -> uint32_t {
        if (r.blob == -2) return r.off;
        if (r.blob == -1) return g2o(r.off, ok, true);
        if ((size_t)r.blob >= D.blobs.size() || r.off > D.blobs[(size_t)r.blob].b.size()) { ok = false; return 0; }
        return bpos[(size_t)r.blob] + r.off;
    };
    bool ok = true;
    for (const auto& kv : D.bytes) {
        const uint32_t o = g2o(kv.first, ok);
        if (!ok) { err = "a changed byte in a part that was cut"; return false; }
        O[o] = kv.second;
    }
    /* pointers */
    std::map<uint32_t, std::pair<uint32_t, Ref>> P;   /* output address -> (section, target) */
    {
        const auto gs = ptr_secs(G);
        for (const auto& x : gs) {
            const uint32_t ga = x.first << 2;
            if (D.ptr_del.count(ga) || D.ptr_set.count(ga)) continue;
            bool k = true;
            const uint32_t o = g2o(ga, k);
            if (!k || o + 4 > data_end) continue;   /* cut with its bytes */
            P[o] = { x.second, Ref{ -1, rd32(&G.data[ga]) } };
        }
        for (const auto& kv : D.ptr_set) {
            const uint32_t o = g2o(kv.first, ok);
            if (!ok) { err = "a pointer in a part that was cut"; return false; }
            P[o] = { kv.second.sec, kv.second.to };
        }
        for (size_t k = 0; k < D.blobs.size(); k++)
            for (const auto& kv : D.blobs[k].ptrs) P[bpos[k] + kv.first] = { kv.second.sec, kv.second.to };
    }
    for (const auto& kv : P) {
        const uint32_t v = r2o(kv.second.second, ok);
        if (!ok) { err = "a pointer to a part that was cut"; return false; }
        wr32(&O[kv.first], v);
    }
    /* link tables */
    std::vector<LinkFile> links = G.links;
    {
        std::vector<std::vector<uint32_t>> secs;
        size_t nsec = 0;
        for (const LinkFile& L : G.links) for (const Block& b : L.blocks) if (b.magic == "ILNK") nsec += b.secs.size();
        secs.resize(nsec);
        for (const auto& kv : P) { if (kv.second.first >= nsec) { err = "a pointer in no table"; return false; } secs[kv.second.first].push_back(kv.first >> 2); }
        std::set<uint32_t> cid;
        for (const LinkFile& L : G.links) for (const Block& b : L.blocks) if (b.magic == "CID ")
            for (uint32_t l : b.cid) {
                const uint32_t ga = l << 2;
                if (D.cid_del.count(ga)) continue;
                bool k = true;
                const uint32_t o = g2o(ga, k);
                if (k) cid.insert(o >> 2);
            }
        for (uint32_t ga : D.cid_add) { const uint32_t o = g2o(ga, ok); if (!ok) { err = "a class id in a cut"; return false; } cid.insert(o >> 2); }
        for (size_t k = 0; k < D.blobs.size(); k++) for (uint32_t off : D.blobs[k].cids) cid.insert((bpos[k] + off) >> 2);
        size_t si = 0;
        bool cid_done = false;
        for (size_t li = 0; li < links.size(); li++)
            for (size_t bi = 0; bi < links[li].blocks.size(); bi++) {
                Block& b = links[li].blocks[bi];
                const Block& g = G.links[li].blocks[bi];
                if (b.magic == "ILNK") {
                    for (IlnkSec& x : b.secs) {
                        x.locs = secs[si++];
                        x.same = x.locs == g.secs[(size_t)(&x - &b.secs[0])].locs;
                    }
                } else if (b.magic == "CID ") {
                    if (cid_done) { err = "two class id tables"; return false; }
                    cid_done = true;
                    b.cid.assign(cid.begin(), cid.end());
                    b.cid_same = b.cid == g.cid;
                } else if (b.magic == "OTYP" || b.magic == "XLNK" || b.magic == "SPLP") {
                    auto lit = D.lists.find({ li, bi });
                    if (lit == D.lists.end()) continue;   /* (unchanged lists aren't in a delta made by hand) */
                    std::vector<uint32_t> extra;
                    auto sx = D.sig_extra.find({ li, bi });
                    if (sx != D.sig_extra.end()) extra = sx->second;
                    if (b.magic == "SPLP") { b.sig = g.sig; b.sig.insert(b.sig.end(), extra.begin(), extra.end()); }
                    b.otyp.clear(); b.xlnk.clear(); b.s1.clear(); b.s2.clear(); b.s3.clear();
                    for (const Item& it : lit->second.items) {
                        if (it.g >= 0) {
                            size_t gi = (size_t)it.g;
                            /* the game's entry, its address moved */
                            auto mv = [&](uint32_t a) { bool k2 = true; const uint32_t o = g2o(a, k2); if (!k2) ok = false; return o; };
                            if (b.magic == "OTYP") { if (gi >= g.otyp.size()) { err = "bad entry"; return false; } Otyp o = g.otyp[gi]; o.addr = mv(o.addr); b.otyp.push_back(o); }
                            else if (b.magic == "XLNK") { if (gi >= g.xlnk.size()) { err = "bad entry"; return false; } Xlnk o = g.xlnk[gi]; o.addr = mv(o.addr); b.xlnk.push_back(o); }
                            else {
                                if (gi < g.s1.size()) { auto o = g.s1[gi]; o[0] = mv(o[0]); b.s1.push_back(o); }
                                else if ((gi -= g.s1.size()) < g.s2.size()) { auto o = g.s2[gi]; o[0] = mv(o[0]); b.s2.push_back(o); }
                                else if ((gi -= g.s2.size()) < g.s3.size()) { auto o = g.s3[gi]; o[0] = mv(o[0]); b.s3.push_back(o); }
                                else { err = "bad entry"; return false; }
                            }
                            if (!ok) { err = "a link entry on a part that was cut"; return false; }
                            continue;
                        }
                        const Entry& x = it.e;
                        const uint32_t a = r2o(x.at, ok);
                        if (!ok) { err = "a new link entry on nothing"; return false; }
                        auto sigoff = [&](size_t i) -> uint32_t {
                            const uint32_t v = x.w[i], mark = x.w[i + 1];
                            return mark == 0xFFFFFFFFu ? v : 4 * (uint32_t)g.sig.size() + v;
                        };
                        switch (x.kind) {
                        case 0: b.otyp.push_back({ a, x.w[0], x.w[1] }); break;
                        case 1: { Xlnk o; memcpy(o.key, x.key, 16); o.addr = a; o.v = x.w[0]; b.xlnk.push_back(o); break; }
                        case 2: b.s1.push_back({ a, x.w[0], x.w[1] }); break;
                        case 3: b.s2.push_back({ a, sigoff(5), x.w[0], x.w[1], x.w[2], x.w[3], x.w[4] }); break;
                        case 4: b.s3.push_back({ a, sigoff(4), x.w[0], x.w[1], x.w[2], x.w[3] }); break;
                        }
                    }
                }
            }
    }
    /* the link files, in their chunks after the data */
    for (size_t j = 0; j < chunks.size(); j++) {
        if (G.chunks[j][0] != 0xFF02) continue;
        const uint32_t c0 = (uint32_t)O.size();
        for (uint32_t t = 0; t < chunks[j][6]; t++) {
            const uint32_t gfi = G.chunks[j][5] + t, ofi = chunks[j][5] + t;
            const LinkFile* L = nullptr;
            for (size_t li = 0; li < links.size(); li++) if (G.links[li].file == gfi) L = &links[li];
            if (!L) { err = "a link file not read"; return false; }
            Bytes blob;
            ser_link(*L, blob);
            files[ofi][2] = (uint32_t)O.size(); files[ofi][1] = (uint32_t)blob.size();
            O.insert(O.end(), blob.begin(), blob.end());
            /* the gap the game had after it */
            const uint32_t gend = t + 1 < G.chunks[j][6] ? G.files[gfi + 1][2] : G.chunks[j][2] + G.chunks[j][1];
            const uint32_t gap = gend - (G.files[gfi][2] + G.files[gfi][1]);
            O.insert(O.end(), G.data.begin() + G.files[gfi][2] + G.files[gfi][1], G.data.begin() + G.files[gfi][2] + G.files[gfi][1] + gap);
        }
        chunks[j][2] = c0; chunks[j][1] = (uint32_t)O.size() - c0;
    }
    /* the header and tables */
    const std::vector<std::array<uint32_t, 4>>& trail = D.trail_set ? D.trail : G.trail;
    const Bytes& hx = D.head_set ? D.head_extra : Bytes(G.head.begin() + G.f0 + 16 * G.nf + 16 * G.ntrail, G.head.end());
    Bytes h(G.head.begin(), G.head.begin() + G.f0);
    const uint32_t grow = 16 * (uint32_t)((files.size() - G.nf) + (trail.size() - G.ntrail));
    wr32(&h[G.tab + 12], (uint32_t)files.size());
    wr32(&h[G.tab + 20], (uint32_t)trail.size());
    wr32(&h[28], rd32(&G.head[28]) + grow);
    for (size_t i = 0; i < chunks.size(); i++) for (int j = 0; j < 7; j++) wr32(&h[G.tab + 24 + 28 * i + 4 * j], chunks[i][(size_t)j]);
    for (const auto& f : files) for (int j = 0; j < 4; j++) put32(h, f[(size_t)j]);
    for (const auto& t : trail) for (int j = 0; j < 4; j++) put32(h, t[(size_t)j]);
    h.insert(h.end(), hx.begin(), hx.end());
    wr32(&h[40], (uint32_t)h.size());
    wr32(&h[44], (uint32_t)O.size());
    out.clear();
    out.reserve(h.size() + O.size() + G.tail.size());
    out.insert(out.end(), h.begin(), h.end());
    out.insert(out.end(), O.begin(), O.end());
    const Bytes& tl = D.tail_set ? D.tail : G.tail;
    out.insert(out.end(), tl.begin(), tl.end());
    if (bpos_out) *bpos_out = bpos;
    return true;
}

}   /* namespace xm */
