"""Diff the native build's milestone stream against the RPCS3 oracle recording.

    python -I tools/tracediff.py <oracle_recording_dir> <native_milestones.txt> [--all]

Oracle side : <dir>/RPCS3.log (+ <dir>/parsed/milestones.tsv from rpcs3_events.py)
Native side : the PS3_MILESTONE_OUT file (keys sys:<num>, prx:<nid>, hle:<name>, rsx:...)

Names are made comparable mechanically:
  sys:<num>  -> handler name, from toolkit/runtime/syscalls (lv2_syscall_register
                calls + lv2_syscall_table.h defines)
  prx:<nid>  -> name, from the oracle log's own import listing
                ("**** <module> import: [<name>] (0x<nid>)")
Then every oracle milestone (first use of a syscall or HLE function by a guest
thread, in time order) is looked up in the native stream. The first oracle
milestone the native run never reached is the first divergence to work on.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TK = os.path.join(ROOT, "toolkit")


def norm(name):
    """Comparable spelling: no leading underscores, no toolkit handler suffix
    (sys_spu_initialize_handler == RPCS3's sys_spu_initialize)."""
    n = name.lstrip("_").lower()
    for suf in ("_handler", "_stub"):
        if n.endswith(suf):
            n = n[: -len(suf)]
    return n


def syscall_names():
    defs = {}
    hdr = os.path.join(TK, "runtime", "syscalls", "lv2_syscall_table.h")
    for ln in open(hdr, encoding="utf-8", errors="ignore"):
        m = re.match(r"\s*#\s*define\s+(SYS_\w+)\s+(\d+)\b", ln)
        if m:
            defs[m.group(1)] = int(m.group(2))
    names = {num: d.lower() for d, num in defs.items()}          # fallback spelling
    sdir = os.path.join(TK, "runtime", "syscalls")
    reg = re.compile(r"lv2_syscall_register\s*\(\s*\w+\s*,\s*(\w+)\s*,\s*(\w+)\s*\)")
    for fn in os.listdir(sdir):
        if fn.endswith((".c", ".cpp")):
            for m in reg.finditer(open(os.path.join(sdir, fn), encoding="utf-8", errors="ignore").read()):
                k, h = m.group(1), m.group(2)
                num = defs.get(k) if not k.isdigit() else int(k)
                if num is not None:
                    names[num] = h
    return names


def oracle_imports(log):
    nids = {}
    pat = re.compile(r"\*\*\*\* (\w+) import: \[(\w+)\] \(0x([0-9a-fA-F]+)\)")
    for ln in open(log, encoding="utf-8", errors="replace"):
        m = pat.search(ln)
        if m:
            nids[int(m.group(3), 16)] = (m.group(1), m.group(2))
    return nids


def oracle_milestones(d):
    rows = []
    p = os.path.join(d, "parsed", "milestones.tsv")
    for i, ln in enumerate(open(p, encoding="utf-8")):
        if i == 0:
            continue
        t, cnt, chan, fn, th = ln.rstrip("\n").split("\t")
        rows.append((float(t), int(cnt), chan, fn, th))
    return rows


def native_stream(path, sysn, nids):
    order, counts = [], {}
    in_counts = False
    for ln in open(path, encoding="utf-8", errors="replace"):
        ln = ln.rstrip("\n")
        if ln.startswith("# counts"):
            in_counts = True
            continue
        if ln.startswith("#") or not ln:
            continue
        parts = ln.split("\t")
        if in_counts and parts[0] == "count" and len(parts) == 3:
            counts[parts[1]] = parts[2]   # bucketed ("2-9", "1k-9k")
            continue
        if len(parts) == 2 and parts[0].isdigit():
            in_counts = False
            order.append(parts[1])
    named = []
    for k in order:
        kind, _, v = k.partition(":")
        if kind == "sys" and v.isdigit():
            named.append((k, sysn.get(int(v), f"syscall_{v}")))
        elif kind == "prx" and v.startswith("0x"):
            mod_name = nids.get(int(v, 16))
            named.append((k, mod_name[1] if mod_name else f"nid_{v}"))
        else:
            named.append((k, v))
    return named, counts


def main():
    od, npath = sys.argv[1], sys.argv[2]
    show_all = "--all" in sys.argv
    sysn = syscall_names()
    nids = oracle_imports(os.path.join(od, "RPCS3.log"))
    orc_all = oracle_milestones(od)
    imported = {norm(n) for _, n in nids.values()}
    internal_gcm = ("sys_rsx_",)   # RPCS3's HLE libgcm_sys calls these itself
    orc = [r for r in orc_all
           if (r[3].lstrip("_").startswith("sys_") and not r[3].startswith(internal_gcm))
           or norm(r[3]) in imported]
    dropped = sorted({f"{r[2]} {r[3]}" for r in orc_all if r not in orc})
    nat, counts = native_stream(npath, sysn, nids)
    nat_pos = {}
    for i, (k, n) in enumerate(nat):
        nat_pos.setdefault(norm(n), (i + 1, k))
    print(f"oracle milestones: {len(orc)} guest-visible ({len(orc_all) - len(orc)} RPCS3-internal dropped: "
          f"{', '.join(dropped[:8])}{' ...' if len(dropped) > 8 else ''})")
    print(f"native milestones: {len(nat)}   "
          f"(syscall names known: {len(sysn)}, oracle import NIDs: {len(nids)})")
    first_missing = None
    missing = 0
    print(f"\n{'oracle t':>9}  {'oracle call':44} native")
    for t, cnt, chan, fn, th in orc:
        hit = nat_pos.get(norm(fn))
        if not hit:
            missing += 1
            if first_missing is None:
                first_missing = (t, chan, fn, th)
        if show_all or not hit or first_missing is None:
            mark = f"#{hit[0]} ({hit[1]}, x{counts.get(hit[1], '?')})" if hit else "MISSING"
            print(f"{t:9.3f}  {chan + ' ' + fn:44} {mark}")
    print(f"\n{missing} of {len(orc)} oracle milestones never reached natively.")
    if first_missing:
        t, chan, fn, th = first_missing
        print(f"FIRST DIVERGENCE: oracle t={t:.3f}s  {chan} {fn}  (thread: {th})")
    onames = {norm(fn) for _, _, _, fn, _ in orc}
    extra = [(k, n) for k, n in nat if norm(n) not in onames and k.startswith(("sys:", "hle:"))]
    print(f"\nnative-only sys/hle milestones (not in the oracle's logged subset): {len(extra)}")
    for k, n in extra[:15]:
        print(f"   {k:14} {n}")


if __name__ == "__main__":
    main()
