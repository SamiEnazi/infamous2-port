"""Turn an RPCS3.log into a structured event recording (oracle side).

Each log line  `·<lvl> <h:mm:ss.us> {<thread> [<where>]} <channel>: <message>`
becomes one JSON object. Then derived views that a diff tool can compare
against the native build's own recording:

  events.jsonl     every parsed line: t (s), lvl, thread, where, chan, msg, fn
  milestones.tsv   first time each (channel, function) appears + total count
  files.tsv        every path the game touched: op, result, first t, count
  threads.tsv      thread names, first/last time, line count

    python -I tools/rpcs3_events.py <RPCS3.log> <out_dir>
"""
import json
import os
import re
import sys
from collections import OrderedDict

LINE = re.compile(
    r"^·(?P<lvl>.) (?P<h>\d+):(?P<m>\d\d):(?P<s>\d\d\.\d+) "
    r"(?:\{(?P<thread>[^}]*)\} )?(?P<chan>[A-Za-z0-9_]+): (?P<msg>.*)$")
THREAD = re.compile(r"^(?P<name>.*?)(?: \[(?P<where>[^\]]*)\])?$")
FN = re.compile(r"^'?(?P<fn>[A-Za-z_][A-Za-z0-9_]*)(?:'|\()")
FAILED = re.compile(r"^'(?P<fn>[A-Za-z_][A-Za-z0-9_]*)' failed with (?P<err>0x[0-9a-f]+)(?: : (?P<name>\w+))?")
PATH = re.compile(r"[“\"](?P<p>/dev_[^”\"]+)[”\"]|path=(?:\*0x[0-9a-f]+ )?[“\"](?P<p2>[^”\"]+)[”\"]")


def parse(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE.match(raw.rstrip("\n"))
            if not m:
                continue
            t = int(m["h"]) * 3600 + int(m["m"]) * 60 + float(m["s"])
            thread, where = None, None
            if m["thread"]:
                tm = THREAD.match(m["thread"])
                thread, where = tm["name"], tm["where"]
            msg = m["msg"]
            ev = {"t": round(t, 6), "lvl": m["lvl"], "thread": thread,
                  "where": where, "chan": m["chan"], "msg": msg}
            fm = FAILED.match(msg)
            if fm:
                ev["fn"], ev["err"] = fm["fn"], fm["name"] or fm["err"]
            else:
                fm = FN.match(msg)
                if fm:
                    ev["fn"] = fm["fn"]
            pm = PATH.search(msg)
            if pm:
                ev["path"] = pm["p"] or pm["p2"]
            yield ev


def main():
    log, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    miles = OrderedDict()
    files = OrderedDict()
    threads = OrderedDict()
    n = 0
    with open(os.path.join(out, "events.jsonl"), "w", encoding="utf-8") as ej:
        for ev in parse(log):
            n += 1
            ej.write(json.dumps(ev, ensure_ascii=False) + "\n")
            if ev.get("fn") and ev["thread"]:  # guest-side calls only
                k = (ev["chan"], ev["fn"])
                if k not in miles:
                    miles[k] = [ev["t"], 0, ev["thread"]]
                miles[k][1] += 1
            if ev.get("path") and ev["chan"] in ("sys_fs", "cellFs", "cellGame", "VFS"):
                k = (ev.get("fn") or "?", ev["path"], ev.get("err") or "ok")
                if k not in files:
                    files[k] = [ev["t"], 0]
                files[k][1] += 1
            if ev["thread"]:
                th = threads.setdefault(ev["thread"], [ev["t"], ev["t"], 0])
                th[1] = ev["t"]
                th[2] += 1
    with open(os.path.join(out, "milestones.tsv"), "w", encoding="utf-8") as f:
        f.write("first_t\tcount\tchannel\tfunction\tfirst_thread\n")
        for (c, fn), (t, cnt, th) in miles.items():
            f.write(f"{t:.6f}\t{cnt}\t{c}\t{fn}\t{th}\n")
    with open(os.path.join(out, "files.tsv"), "w", encoding="utf-8") as f:
        f.write("first_t\tcount\top\tresult\tpath\n")
        for (op, p, r), (t, cnt) in files.items():
            f.write(f"{t:.6f}\t{cnt}\t{op}\t{r}\t{p}\n")
    with open(os.path.join(out, "threads.tsv"), "w", encoding="utf-8") as f:
        f.write("first_t\tlast_t\tlines\tthread\n")
        for name, (a, b, cnt) in threads.items():
            f.write(f"{a:.6f}\t{b:.6f}\t{cnt}\t{name}\n")
    print(f"{n} events, {len(miles)} milestones, {len(files)} file keys, {len(threads)} threads -> {out}")


if __name__ == "__main__":
    main()
