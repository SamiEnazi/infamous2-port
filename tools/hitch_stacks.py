r"""What every guest thread was doing during the frames that froze (hitches.tsv, written by a build with
the toolkit's hitch catcher: runtime/diag/ps3_hitchcatch.c, on whenever the play log is).

    python -I tools/hitch_stacks.py <session dir | hitches.tsv> [--exe <exe>] [--top 6] [--hitch N]
                                    [--min-ms 0] [--json <out.json>]

Per hitch: how long the frame lasted, the nearest F9 MARK in playlog.tsv, and for every sampled thread
(guest PPU threads, SPU workers, the presenting thread) how often it ran, its top leaf functions, its
innermost guest functions (func_XXXXXXXX / <module>_spu_func_XXXX), its top inclusive frames, the HLE call
it was inside (PPU) and the SPU job modules resident (SPU: ovl ids, with the job module named from the
stack). Names come from dbghelp against the exe's own PDB (the exe path and PDB GUID are in the file's
header; --exe when the exe has moved). Nothing is guessed: a sample whose stack could not be read says so.

summary_lines(dir) is what tools/playlog_report.py puts in summary.txt.
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as W
import json
import os
import re
import sys

GUEST = re.compile(r"^(func_[0-9A-Fa-f]{8}|.*_spu_func_[0-9A-Fa-f]+)$")
SPUMOD = re.compile(r"^((?:jobmod|relocmod|datamod)_[0-9a-f]+(?:_e[0-9a-f]+)?|wwsjob_pm_at_[0-9A-F]+|"
                    r"libsre_[a-z]+_[0-9a-f]+|spu_[0-9a-f]+_at_[0-9A-F]+)_spu_func_[0-9A-Fa-f]+$")
STATE_NAMES = {"r": "ran", "q": "first sample", "w": "did not run", "x": "not sampled"}


# ---- reading hitches.tsv ----------------------------------------------------------------------------
class Hitch:
    def __init__(self, n, start, end, dur, first, passes, cut):
        self.n, self.start, self.end, self.dur, self.first, self.passes, self.cut = n, start, end, dur, first, passes, cut
        self.nodes = {}            # id -> (parent, mod, rva) ; mod None = absolute address in rva
        self.samples = []          # (t_ms, serial, st, node, rep, extra dict)
        self.tcpu = {}             # serial -> (cpu_ms, ran, seen)
        self.end_stats = None
        self.complete = False


def read_hitches(path):
    header, mods, threads, hitches = {}, {}, {}, []
    cur = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            r = line.rstrip("\n").split("\t")
            k = r[0]
            if k == "#hitchcatch":
                header = dict(x.split("=", 1) for x in r[2:] if "=" in x)
                header["version"] = r[1] if len(r) > 1 else "?"
            elif k == "CAL":   # the watchdog's measurements at start (TSC rate, a suspend's cost)
                header.update(dict(x.split("=", 1) for x in r[1:] if "=" in x))
            elif k == "MOD" and len(r) >= 5:
                mods[int(r[1])] = (int(r[2], 16), int(r[3], 16), r[4])
            elif k == "THREAD" and len(r) >= 6:
                threads[int(r[1])] = dict(kind=r[2], id=int(r[3]), tid=int(r[4]), name=r[5])
            elif k == "HITCH" and len(r) >= 8:
                cur = Hitch(int(r[1]), float(r[2]), float(r[3]), float(r[4]), float(r[5]), int(r[6]), r[7] != "-")
                hitches.append(cur)
            elif cur is None:
                continue
            elif k == "N" and len(r) >= 5:
                m, a = r[4].split(":", 1)
                cur.nodes[int(r[2])] = (int(r[3]), None if m == "?" else int(m), int(a, 16))
            elif k == "S" and len(r) >= 8:
                extra = {}
                if r[7] != "-":
                    extra = dict(x.split("=", 1) for x in r[7].split(",") if "=" in x)
                cur.samples.append((float(r[2]), int(r[3]), r[4], int(r[5]), int(r[6]), extra))
            elif k == "TCPU" and len(r) >= 6:
                cur.tcpu[int(r[2])] = (float(r[3]), int(r[4]), int(r[5]))
            elif k == "END" and len(r) >= 11:
                cur.end_stats = dict(passes=int(r[2]), samples=int(r[3]), waits=int(r[4]), susp_max_us=float(r[5]),
                                     susp_avg_us=float(r[6]), pass_avg_us=float(r[7]), sampler_ms=float(r[8]),
                                     write_ms=float(r[9]), nodes=int(r[10]), nodes_full=len(r) > 11)
                cur.complete = True
    return header, mods, threads, hitches


def read_marks(d):
    p = os.path.join(d, "playlog.tsv")
    marks = []
    if os.path.exists(p):
        with open(p, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("MARK\t"):
                    try:
                        marks.append(float(line.split("\t")[1]))
                    except ValueError:
                        pass
    return marks


# ---- symbols (dbghelp, offline: no process needed) ----------------------------------------------------
class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", W.ULONG), ("TypeIndex", W.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", W.ULONG), ("Size", W.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", W.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", W.ULONG),
                ("Scope", W.ULONG), ("Tag", W.ULONG), ("NameLen", W.ULONG), ("MaxNameLen", W.ULONG),
                ("Name", ctypes.c_wchar * 512)]


class IMAGEHLP_MODULEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", W.DWORD), ("BaseOfImage", ctypes.c_uint64), ("ImageSize", W.DWORD),
                ("TimeDateStamp", W.DWORD), ("CheckSum", W.DWORD), ("NumSyms", W.DWORD), ("SymType", W.DWORD),
                ("ModuleName", ctypes.c_wchar * 32), ("ImageName", ctypes.c_wchar * 256),
                ("LoadedImageName", ctypes.c_wchar * 256), ("LoadedPdbName", ctypes.c_wchar * 256),
                ("CVSig", W.DWORD), ("CVData", ctypes.c_wchar * 780), ("PdbSig", W.DWORD),
                ("PdbSig70", ctypes.c_ubyte * 16), ("PdbAge", W.DWORD), ("PdbUnmatched", W.BOOL),
                ("DbgUnmatched", W.BOOL), ("LineNumbers", W.BOOL), ("GlobalSymbols", W.BOOL),
                ("TypeInfo", W.BOOL), ("SourceIndexed", W.BOOL), ("Publics", W.BOOL),
                ("MachineType", W.DWORD), ("Reserved", W.DWORD)]


def guid_hex(b):
    d1 = int.from_bytes(bytes(b[0:4]), "little")
    d2 = int.from_bytes(bytes(b[4:6]), "little")
    d3 = int.from_bytes(bytes(b[6:8]), "little")
    return "%08X%04X%04X" % (d1, d2, d3) + "".join("%02X" % x for x in b[8:16])


class Symbols:
    _next_handle = 0x48430000

    def __init__(self, header, mods, exe_override=None):
        self.ok = False
        self.notes = []
        self.mods = mods
        self.exe = header.get("exe", "")
        self.exe_path = exe_override or self.exe
        self.loaded = {}
        self.cache = {}
        try:
            self.dbg = ctypes.WinDLL("dbghelp", use_last_error=True)
        except OSError:
            self.notes.append("dbghelp.dll not found: addresses only")
            return
        d = self.dbg
        d.SymInitializeW.argtypes = [W.HANDLE, ctypes.c_wchar_p, W.BOOL]
        d.SymLoadModuleExW.restype = ctypes.c_uint64
        d.SymLoadModuleExW.argtypes = [W.HANDLE, W.HANDLE, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint64,
                                       W.DWORD, ctypes.c_void_p, W.DWORD]
        d.SymFromAddrW.argtypes = [W.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                   ctypes.POINTER(SYMBOL_INFOW)]
        d.SymGetModuleInfoW64.argtypes = [W.HANDLE, ctypes.c_uint64, ctypes.POINTER(IMAGEHLP_MODULEW64)]
        d.SymCleanup.argtypes = [W.HANDLE]
        Symbols._next_handle += 1
        self.h = W.HANDLE(Symbols._next_handle)
        d.SymSetOptions(0x2 | 0x200 | 0x80000)   # UNDNAME | FAIL_CRITICAL_ERRORS | NO_PROMPTS
        search = [os.path.dirname(self.exe_path)] if self.exe_path else []
        pdb = header.get("pdb", "")
        if pdb and os.path.dirname(pdb):
            search.append(os.path.dirname(pdb))
        if not d.SymInitializeW(self.h, ";".join(search) or None, False):
            self.notes.append("dbghelp SymInitialize failed: addresses only")
            return
        self.ok = True
        self.want_guid = header.get("pdbguid", "").upper()
        self.want_age = header.get("pdbage", "")
        self.si = SYMBOL_INFOW()
        self.si.SizeOfStruct = 88   # the fixed part (dbghelp's own size of the struct, without Name)
        self.si.MaxNameLen = 500
        self.disp = ctypes.c_uint64(0)

    def _module(self, m):
        if m in self.loaded:
            return self.loaded[m]
        base, size, path = self.mods.get(m, (0, 0, "?"))
        is_exe = bool(self.exe) and os.path.normcase(path) == os.path.normcase(self.exe)
        load = self.exe_path if is_exe else path
        short = os.path.splitext(os.path.basename(path))[0] if path else "?"
        ok = False
        if self.ok and load and os.path.exists(load):
            got = self.dbg.SymLoadModuleExW(self.h, None, load, None, base, size, None, 0)
            ok = bool(got) or ctypes.get_last_error() == 0
            if is_exe:
                mi = IMAGEHLP_MODULEW64()
                mi.SizeOfStruct = ctypes.sizeof(mi)
                if self.dbg.SymGetModuleInfoW64(self.h, base, ctypes.byref(mi)):
                    g, a = guid_hex(mi.PdbSig70), str(mi.PdbAge)
                    if mi.SymType != 3:      # SymPdb
                        self.notes.append("the exe's PDB was not found next to %s: its functions are named by "
                                          "exports only" % load)
                    elif self.want_guid and (g != self.want_guid or (self.want_age and a != self.want_age)):
                        self.notes.append("PDB MISMATCH: %s is %s/%s, the game ran %s/%s -- names are wrong"
                                          % (mi.LoadedPdbName, g, a, self.want_guid, self.want_age))
                    else:
                        self.notes.append("symbols: %s (matches the exe that ran)" % mi.LoadedPdbName)
        elif is_exe:
            self.notes.append("the exe that ran is not here (%s): addresses only (--exe <path>)" % load)
        self.loaded[m] = (ok, short, is_exe)
        return self.loaded[m]

    def name(self, m, rva, caller):
        """caller: a return address (look up the call instruction, rva - 1)"""
        key = (m, rva, caller)
        if key in self.cache:
            return self.cache[key]
        if m is None:
            r = "0x%x" % rva
        else:
            ok, short, is_exe = self._module(m)
            r = "%s+0x%x" % (short, rva)
            if ok:
                base = self.mods[m][0]
                if self.dbg.SymFromAddrW(self.h, base + rva - (1 if caller else 0), ctypes.byref(self.disp),
                                         ctypes.byref(self.si)):
                    nm = self.si.Name
                    r = nm if is_exe else "%s!%s" % (short, nm)
        self.cache[key] = r
        return r


# ---- analysis ----------------------------------------------------------------------------------------
def stack_of(h, node, sym, memo):
    """names, leaf first (index 0 = where the thread was; the rest are return addresses)"""
    if node in memo:
        return memo[node]
    chain = []
    n = node
    while n and n in h.nodes and len(chain) < 64:
        chain.append(n)
        n = h.nodes[n][0]
    # the root-most node is the last; leaf = chain[0]
    names = []
    for i, nid in enumerate(chain):
        _, m, rva = h.nodes[nid]
        names.append(sym.name(m, rva, i > 0))
    memo[node] = names
    return names


def pct(c, total):
    return 100.0 * c / total if total else 0.0


def top(counter, total, k):
    return [(n, round(pct(c, total), 1)) for n, c in counter.most_common(k)]


def analyse(h, threads, sym, k=6):
    memo = {}
    per = collections.OrderedDict()
    ovl_names = collections.defaultdict(collections.Counter)    # ovl -> jobmod names seen with it
    for t_ms, serial, st, node, rep, extra in h.samples:
        th = per.get(serial)
        if th is None:
            info = threads.get(serial, dict(kind="?", id=0, tid=0, name="?"))
            th = per[serial] = dict(serial=serial, kind=info["kind"], id=info["id"], tid=info["tid"], name=info["name"],
                                    total=0, states=collections.Counter(), leaf=collections.Counter(),
                                    guest=collections.Counter(), incl=collections.Counter(), hle=collections.Counter(),
                                    ovl=collections.Counter(), jobmod=collections.Counter(), stacks=collections.Counter(),
                                    run_guest=collections.Counter(), run_n=0, exe=collections.Counter())
        th["total"] += rep
        th["states"][st] += rep
        if st == "x" or not node:
            th["leaf"]["(stack not read)" if st != "x" else "(not sampled)"] += rep
            continue
        names = stack_of(h, node, sym, memo)
        if not names:
            continue
        th["leaf"][names[0]] += rep
        g = next((x for x in names if GUEST.match(x)), None)
        th["guest"][g or "(no guest frame)"] += rep
        th["exe"][next((x for x in names if "!" not in x and "+0x" not in x), names[0])] += rep   # innermost frame in the exe
        if st == "r":
            th["run_n"] += rep
            th["run_guest"][g or names[0]] += rep
        for x in set(names):
            th["incl"][x] += rep
        th["stacks"][tuple(names[:8])] += rep
        if extra.get("hle", "-") != "-":
            th["hle"][extra["hle"]] += rep
        if "ovl" in extra:
            o = extra["ovl"]
            th["ovl"][o] += rep
            jm = next((SPUMOD.match(x).group(1) for x in names if SPUMOD.match(x)), None)
            if jm:
                th["jobmod"][jm] += rep
                ovl_names[o][jm] += rep
    out = []
    for th in per.values():
        tot = th["total"]
        known = th["states"]["r"] + th["states"]["w"]
        cpu = h.tcpu.get(th["serial"])
        out.append(dict(
            serial=th["serial"], kind=th["kind"], id=th["id"], tid=th["tid"], name=th["name"], samples=tot,
            ran_pct=round(pct(th["states"]["r"], known), 1) if known else None,
            states={STATE_NAMES.get(s, s): c for s, c in th["states"].items()},
            cpu_ms=cpu[0] if cpu else None,
            leaf=top(th["leaf"], tot, k), guest=top(th["guest"], tot, k), inclusive=top(th["incl"], tot, k + 4),
            exe=top(th["exe"], tot, k),
            running_in=top(th["run_guest"], th["run_n"], k) if th["run_n"] else [],
            hle=top(th["hle"], tot, 3) if th["hle"] else [],
            jobs=top(th["ovl"], tot, k) if th["ovl"] else [],
            jobmods=top(th["jobmod"], tot, k) if th["jobmod"] else [],
            top_stack=list(th["stacks"].most_common(1)[0][0]) if th["stacks"] else [],
            top_stack_pct=round(pct(th["stacks"].most_common(1)[0][1], tot), 1) if th["stacks"] else 0.0))
    ovl_map = {o: c.most_common(1)[0][0] for o, c in ovl_names.items() if c}
    return out, ovl_map


def busy_score(t):
    return (t["cpu_ms"] if t["cpu_ms"] is not None and t["cpu_ms"] >= 0 else 0.0)


def verdict(h, ths, ovl_map):
    """the facts that name the freeze, short: what the main PPU thread did, and what ran meanwhile"""
    lines = []
    main = next((t for t in ths if t["kind"] == "PPU" and t["id"] == 1), None)
    if main:
        if main["ran_pct"] is not None and main["ran_pct"] >= 50.0:
            where = main["running_in"][0] if main["running_in"] else main["guest"][0]
            chain = [x for x in main["top_stack"] if GUEST.match(x)][:4]
            lines.append("main PPU thread RAN %.0f%% of the frame: %.0f%% of its running samples in %s%s" % (
                main["ran_pct"], where[1], where[0], (" (stack " + " < ".join(chain) + ")") if len(chain) > 1 else ""))
        else:
            g = main["guest"][0] if main["guest"] else ("?", 0)
            hle = (" in HLE " + main["hle"][0][0]) if main["hle"] and main["hle"][0][0] not in ("-", "?") else ""
            lf = main["leaf"][0] if main["leaf"] else ("?", 0)
            ex = main["exe"][0] if main["exe"] else ("?", 0)
            via = (" -> %s (%.0f%%)" % ex) if ex[0] not in (g[0], lf[0]) else ""
            lf = (lf[0] + " (%.0f%%)" % lf[1], lf[1])
            lines.append("main PPU thread WAITED (ran %s%% of the frame): %.0f%% in %s%s -> %s%s" % (
                "%.0f" % main["ran_pct"] if main["ran_pct"] is not None else "?", g[1], g[0], via, lf[0], hle))
    spus = [t for t in ths if t["kind"] == "SPU"]
    if spus:
        jobs = collections.Counter()
        tot = 0
        for t in spus:
            for o, p in t["jobs"]:
                jobs[o] += p * t["samples"] / 100.0
            tot += t["samples"]
        busy = [t for t in spus if t["ran_pct"] and t["ran_pct"] >= 50.0]
        js = ", ".join("%s%s %.0f%%" % (o, "=" + ovl_map[o] if o in ovl_map else "", pct(c, tot))
                       for o, c in jobs.most_common(3) if o not in ("0",))
        lines.append("SPU workers: %d of %d ran most of the frame; resident jobs: %s" % (len(busy), len(spus), js or "none"))
        for t in sorted(busy, key=lambda t: -(t["ran_pct"] or 0))[:2]:
            ri = t["running_in"][0] if t["running_in"] else ("?", 0)
            lines.append("  %s ran %.0f%%: %.0f%% in %s" % (t["name"], t["ran_pct"], ri[1], ri[0]))
    rsx = next((t for t in ths if t["kind"] == "RSX"), None)
    if rsx and rsx["exe"]:
        ex, lf = rsx["exe"][0], rsx["leaf"][0] if rsx["leaf"] else ("?", 0)
        lines.append("presenting thread ran %s%%: %.0f%% in %s%s" % (
            "?" if rsx["ran_pct"] is None else "%.0f" % rsx["ran_pct"], ex[1], ex[0],
            (" -> " + lf[0]) if lf[0] != ex[0] else ""))
    others = sorted((t for t in ths if t is not main and t["kind"] == "PPU" and (t["ran_pct"] or 0) >= 50.0),
                    key=lambda t: -busy_score(t))
    for t in others[:2]:
        ri = t["running_in"][0] if t["running_in"] else ("?", 0)
        lines.append("other busy PPU thread %s (id %d) ran %.0f%%: %.0f%% in %s" % (t["name"], t["id"], t["ran_pct"], ri[1], ri[0]))
    return lines


def nearest_mark(h, marks):
    if not marks:
        return None
    m = min(marks, key=lambda x: abs(x - h.end))
    return m, m - h.end


def load(path, exe=None):
    d = path if os.path.isdir(path) else os.path.dirname(path)
    f = os.path.join(path, "hitches.tsv") if os.path.isdir(path) else path
    if not os.path.exists(f):
        return None
    header, mods, threads, hitches = read_hitches(f)
    sym = Symbols(header, mods, exe)
    return dict(dir=d, file=f, header=header, mods=mods, threads=threads, hitches=hitches, sym=sym,
                marks=read_marks(d))


def report(s, k=6, only=None, min_ms=0.0):
    out = []
    say = out.append
    hd = s["header"]
    say("hitch catcher: %s, %d hitches over %s ms (sampling from %s ms, every %s us, %s frames deep)" % (
        s["file"], len(s["hitches"]), hd.get("keep_ms", "?"), hd.get("start_ms", "?"), hd.get("period_us", "?"),
        hd.get("depth", "?")))
    if "suspend_cycles_max" in hd:
        say("  one suspend costs a sleeping thread up to %s cycles of its own (%.1f us): right after a sample, a "
            "thread counts as running only above %s" % (hd["suspend_cycles_max"], int(hd["suspend_cycles_max"]) * 1e6 /
                                                        max(1.0, float(hd.get("tsc_hz", "0") or 0)), hd.get("ran_threshold_after_sample")))
    for h in sorted(s["hitches"], key=lambda h: -h.dur):
        if (only and h.n != only) or h.dur < min_ms:
            continue
        ths, ovl_map = analyse(h, s["threads"], s["sym"], k)
        s.setdefault("_an", {})[h.n] = (ths, ovl_map)
        for n in s["sym"].notes:
            if n not in out:
                say("  " + n)
        mk = nearest_mark(h, s["marks"])
        say("")
        say("hitch #%d: %.1f ms, frame %.2f -> %.2f s on the play log clock%s%s%s" % (
            h.n, h.dur, h.start, h.end, (", F9 mark at %.2f s (%+.1f s from the frame's end)" % mk) if mk else "",
            ", CUT (buffer or 8 s limit)" if h.cut else "", "" if h.complete else ", INCOMPLETE (no END line)"))
        e = h.end_stats
        if e:
            say("  sampled from %.0f ms: %d passes, %d stacks, %d sleeping threads skipped; a thread was stopped %.0f us "
                "at most (%.1f avg); sampler CPU %.1f ms" % (h.first, e["passes"], e["samples"], e["waits"],
                                                             e["susp_max_us"], e["susp_avg_us"], e["sampler_ms"]))
        for line in verdict(h, ths, ovl_map):
            say("  => " + line)
        for t in sorted(ths, key=lambda t: (t["kind"] != "PPU" or t["id"] != 1, t["kind"] != "SPU", -busy_score(t))):
            rp = "?" if t["ran_pct"] is None else "%.0f%%" % t["ran_pct"]
            cpu = "" if t["cpu_ms"] is None or t["cpu_ms"] < 0 else ", cpu %.0f ms" % t["cpu_ms"]
            say("  %s %s (id %s, host tid %d): %d samples, ran %s%s" % (t["kind"], t["name"], t["id"], t["tid"],
                                                                         t["samples"], rp, cpu))
            if (t["ran_pct"] or 0) < 5.0 and t["kind"] == "PPU" and t["id"] != 1 and t["leaf"]:
                say("      asleep in %s (%s)" % (t["guest"][0][0], t["leaf"][0][0]))
                continue
            if t["running_in"]:
                say("      running in: " + ", ".join("%.0f%% %s" % (p, n) for n, p in t["running_in"]))
            say("      leaf:       " + ", ".join("%.0f%% %s" % (p, n) for n, p in t["leaf"]))
            say("      guest:      " + ", ".join("%.0f%% %s" % (p, n) for n, p in t["guest"]))
            say("      in the exe: " + ", ".join("%.0f%% %s" % (p, n) for n, p in t["exe"]))
            say("      inclusive:  " + ", ".join("%.0f%% %s" % (p, n) for n, p in t["inclusive"]))
            if t["hle"]:
                say("      hle:        " + ", ".join("%.0f%% %s" % (p, n) for n, p in t["hle"]))
            if t["jobs"]:
                say("      jobs (ovl): " + ", ".join("%.0f%% %s%s" % (p, n, "=" + ovl_map[n] if n in ovl_map else "")
                                                     for n, p in t["jobs"]))
    return out


def summary_lines(d, worst=5):
    """playlog_report.py: the worst freezes with the facts that name them (no hitches.tsv -> [])"""
    s = load(d)
    if not s:
        return []
    out = ["worst freezes and their cause (hitch catcher; full report: python -I tools\\hitch_stacks.py %s):" % d]
    if not s["hitches"]:
        out.append("  none caught (no frame over %s ms while it was on)" % s["header"].get("keep_ms", "?"))
        return out
    for h in sorted(s["hitches"], key=lambda h: -h.dur)[:worst]:
        ths, ovl_map = analyse(h, s["threads"], s["sym"], 4)
        mk = nearest_mark(h, s["marks"])
        out.append("  %7.1f s  %6.1f ms%s" % (h.end, h.dur, ("  (F9 mark %+.1f s)" % mk[1]) if mk and abs(mk[1]) <= 10 else ""))
        for line in verdict(h, ths, ovl_map):
            out.append("      " + line)
    for n in s["sym"].notes:
        out.append("  " + n)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--exe")
    ap.add_argument("--top", type=int, default=6)
    ap.add_argument("--hitch", type=int)
    ap.add_argument("--min-ms", type=float, default=0.0)
    ap.add_argument("--json")
    a = ap.parse_args()
    s = load(a.path, a.exe)
    if not s:
        sys.exit("no hitches.tsv in %s" % a.path)
    for line in report(s, a.top, a.hitch, a.min_ms):
        print(line)
    if a.json:
        res = []
        for h in s["hitches"]:
            ths, ovl_map = s.get("_an", {}).get(h.n) or analyse(h, s["threads"], s["sym"], a.top)
            mk = nearest_mark(h, s["marks"])
            res.append(dict(n=h.n, start=h.start, end=h.end, dur_ms=h.dur, first_ms=h.first, cut=h.cut, mark=mk, verdict=verdict(h, ths, ovl_map),
                            threads=ths, ovl_map=ovl_map, stats=h.end_stats))
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(dict(header=s["header"], notes=s["sym"].notes, hitches=res), f, indent=1)


if __name__ == "__main__":
    main()
