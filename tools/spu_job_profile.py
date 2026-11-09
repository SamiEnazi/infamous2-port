"""SPU time by job module, from an xperf text dump of a sampled profile (the pf-prof dumps:
`xperf -i prof.etl -o dump.txt -a dumper -stacktimeshifting`, SampledProfile + Stack lines).

    python -I tools/spu_job_profile.py <dump.txt> [<dump2.txt> ...] [--pid N] [--top 30]

For every sample of an SPU host thread (start function spu_exec_thread_proc), two views:
  self  -- the leaf function's module (jobmod_<hash>_<entry>, wwsjob_pm, libsre, runtime, os)
  guest -- the innermost LIFTED SPU function on the stack (leaf first), so runtime helpers
           (mfc_do_transfer, spu_wrch, spu_indirect_branch, float slow paths ...) count for the
           job module that called them; samples with no lifted frame are 'runtime'/'os'.
Shares are of all SPU-thread samples (busy time, not wall time).
"""
import collections
import re
import sys

GUEST = re.compile(r"!((jobmod_[0-9a-f]+_e[0-9a-f]+)|(relocmod_[0-9a-f]+)|(datamod_[0-9a-f]+)|"
                   r"(wwsjob_pm_at_[0-9A-F]+)|(libsre_[a-z]+_[0-9a-f]+)|(spu_000[0-9]_at_[0-9A-F]+))_spu_func_([0-9A-F]+)")


def module_of(fn):
    m = GUEST.search(fn)
    if m:
        return m.group(1), m.group(8)
    if fn.startswith("Infamous2Recomp.exe!"):
        return "runtime:" + fn.split("!", 1)[1].split("+")[0], None
    return "os:" + fn.split("!", 1)[0], None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    pid = None
    top = 30
    for i, a in enumerate(sys.argv):
        if a == "--pid":
            pid = sys.argv[i + 1]
        if a == "--top":
            top = int(sys.argv[i + 1])
    args = [a for a in args if a != pid and a != str(top)]
    self_mod = collections.Counter()
    guest_mod = collections.Counter()
    guest_fn = collections.Counter()
    self_rt = collections.Counter()
    nsamp = 0
    for path in args:
        cur = None      # (ts, tid) of the SPU sample whose stack follows
        frames = []

        def flush():
            nonlocal nsamp
            if cur is None:
                return
            nsamp += 1
            leaf = frames[0] if frames else cur[2]
            sm, _ = module_of(leaf)
            self_mod[sm if not sm.startswith(("runtime:", "os:")) else sm.split(":")[0]] += 1
            if sm.startswith("runtime:"):
                self_rt[sm] += 1
            g = None
            for f in frames or [leaf]:
                gm, addr = module_of(f)
                if addr is not None:
                    g = (gm, addr)
                    break
            if g:
                guest_mod[g[0]] += 1
                guest_fn[g[0] + "_spu_func_" + g[1]] += 1
            else:
                guest_mod["(no lifted frame) " + sm.split(":")[0]] += 1

        with open(path, encoding="utf-8", errors="replace") as f:
            for ln in f:
                if ln.startswith("         SampledProfile,") or ln.lstrip().startswith("SampledProfile,"):
                    flush()
                    cur, frames = None, []
                    p = [x.strip() for x in ln.split(",")]
                    if len(p) < 8 or not p[1].isdigit():
                        continue
                    if "Infamous2Recomp.exe" not in p[2] or (pid and f"({pid})" not in p[2]):
                        continue
                    if "spu_exec_thread_proc" not in p[6]:
                        continue
                    cur = (p[1], p[3], p[7])
                elif cur is not None and ln.lstrip().startswith("Stack,"):
                    p = [x.strip() for x in ln.split(",")]
                    if len(p) >= 6 and p[1] == cur[0] and p[2] == cur[1]:
                        frames.append(p[5])
        flush()
    if not nsamp:
        print("no SPU-thread samples found")
        return
    print(f"{nsamp} SPU-thread samples")
    print("\n== by module, innermost lifted frame (runtime helpers charged to their caller) ==")
    for m, n in guest_mod.most_common(top):
        print(f"  {100.0 * n / nsamp:6.2f}%  {n:7d}  {m}")
    print("\n== by module of the leaf (self) ==")
    for m, n in self_mod.most_common(top):
        print(f"  {100.0 * n / nsamp:6.2f}%  {n:7d}  {m}")
    print("\n== runtime leaf functions (self) ==")
    for m, n in self_rt.most_common(top):
        print(f"  {100.0 * n / nsamp:6.2f}%  {n:7d}  {m}")
    print("\n== lifted functions, innermost (with the runtime they call) ==")
    for m, n in guest_fn.most_common(top):
        print(f"  {100.0 * n / nsamp:6.2f}%  {n:7d}  {m}")


if __name__ == "__main__":
    main()
