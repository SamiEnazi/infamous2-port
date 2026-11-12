"""tools/rebuild_win.py PORT_LIFT_REUSE (the updater's fast rebuilds), with stand-in lift steps that write
files the way the real ones do (fresh dirs, the jobs step inside spu/). No game files, no compiler.

    python -I tests/test_lift_reuse.py        exit code = failed checks
"""
import importlib
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(os.path.dirname(HERE), "tools")
fails = checks = 0


def check(ok, what):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
        print("FAIL:", what)


def load(tmp, reuse):
    os.environ["PORT_DIR"] = os.path.join(tmp, "port")
    os.environ["PORT_TK"] = os.path.join(tmp, "tk")
    if reuse:
        os.environ["PORT_LIFT_REUSE"] = "1"
    else:
        os.environ.pop("PORT_LIFT_REUSE", None)
    sys.path.insert(0, TOOLS)
    import rebuild_win
    rw = importlib.reload(rebuild_win)
    rw.ELF = os.path.join(tmp, "EBOOT.elf")
    rw.FW = os.path.join(tmp, "fw")
    rw.CHEATS = os.path.join(tmp, "cheats")
    rw.SPU_NATIVES = os.path.join(tmp, "natives.txt")
    rw.SPU_PATCHES = os.path.join(tmp, "spu_patches.txt")
    return rw


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def make_steps(rw, tmp, ran):
    port = rw.PORT

    def lift_ppu():   # like step_ppu: fresh recompiled/, one chunk per hook-list line (+ a fixed one)
        ran.append("ppu")
        rw.fresh("recompiled")
        hooks = open(os.path.join(tmp, "cheats", "hooks.txt")).read().split()
        write(os.path.join(port, "recompiled", "ppu_recomp_000.cpp"), "// chunk 0, always the same\n")
        for i, h in enumerate(hooks):
            write(os.path.join(port, "recompiled", "ppu_recomp_%03d.cpp" % (i + 1)), "// hook " + h + "\n")

    def lift_spu():   # like step_spu: fresh spu/ (the job outputs inside it go too)
        ran.append("spu")
        rw.fresh("spu")
        write(os.path.join(port, "spu", "spu_0000", "spu_recomp.c"), "// spu image\n")
        write(os.path.join(port, "spu", "spu_workloads.c"), "// workloads\n")

    def lift_jobs():  # like step_jobs: its own outputs inside spu/
        ran.append("jobs")
        write(os.path.join(port, "spu", "jobmod_aa", "spu_recomp.c"), "// job aa\n")
        write(os.path.join(port, "spu", "jobmods_register.c"), "// register\n")

    def lift_fw():
        ran.append("firmware")
        rw.fresh("lle", "lle_spu")
        write(os.path.join(port, "lle", "liblv2", "liblv2.cpp"), "// fw\n")
        write(os.path.join(port, "lle_spu", "x", "spu_recomp.c"), "// fw spu\n")

    rw.step_firmware, rw.step_ppu, rw.step_spu, rw.step_jobs = lift_fw, lift_ppu, lift_spu, lift_jobs


def run(rw, tmp):   # one rebuild_win.py run (its set of steps that lifted starts empty)
    rw._lifted.clear()
    ran = []
    make_steps(rw, tmp, ran)
    for s in ("firmware", "ppu", "spu", "jobs"):
        if not rw.lift_skip(s):
            getattr(rw, "step_" + s)()
            rw.lift_done(s)
    rw.lift_restore_times()
    return ran


def mtimes(rw):
    out = {}
    for dp, _, fn in os.walk(rw.PORT):
        if ".lift" in dp:
            continue
        for f in fn:
            p = os.path.join(dp, f)
            out[os.path.relpath(p, rw.PORT)] = os.stat(p).st_mtime_ns
    return out


def age_all(rw, secs):   # the files as an earlier build left them (older than its objects)
    t = time.time() - secs
    for dp, _, fn in os.walk(rw.PORT):
        for f in fn:
            os.utime(os.path.join(dp, f), (t, t))


def main():
    tmp = tempfile.mkdtemp(prefix="liftreuse-")
    try:
        write(os.path.join(tmp, "EBOOT.elf"), "elf v1.04")
        for m in ("liblv2", "libsysmodule", "libsre"):
            write(os.path.join(tmp, "fw", m + ".prx"), "prx " + m)
        write(os.path.join(tmp, "cheats", "hooks.txt"), "00196508\n001578F8\n")
        write(os.path.join(tmp, "tk", "tools", "ppu_lifter.py"), "# lifter v1\n")
        write(os.path.join(tmp, "port", "tools", "lift_datamods.py"), "# port tool\n")

        rw = load(tmp, True)
        ran = run(rw, tmp)
        check(ran == ["firmware", "ppu", "spu", "jobs"], "first build lifts everything: %r" % ran)
        check(all(os.path.isfile(os.path.join(rw.LIFT, s + ".fp")) for s in ran), "fingerprints written")
        age_all(rw, 3600)
        before = mtimes(rw)

        ran = run(rw, tmp)
        check(ran == [], "nothing changed: every lift kept: %r" % ran)
        check(mtimes(rw) == before, "nothing changed: no file touched")

        # a new cheat hook (as v0.4.2): only the PPU lift runs; the unchanged chunks keep their times
        write(os.path.join(tmp, "cheats", "hooks.txt"), "00196508\n001578F8\n002BB874\n")
        ran = run(rw, tmp)
        now = mtimes(rw)
        check(ran == ["ppu"], "hooks changed: only the PPU lift: %r" % ran)
        check(now[os.path.join("recompiled", "ppu_recomp_000.cpp")] ==
              before[os.path.join("recompiled", "ppu_recomp_000.cpp")], "unchanged chunk 0 kept its time")
        check(now[os.path.join("recompiled", "ppu_recomp_001.cpp")] == before[os.path.join("recompiled", "ppu_recomp_001.cpp")],
              "unchanged chunk 1 kept its time")
        new3 = os.path.join("recompiled", "ppu_recomp_003.cpp")
        check(new3 in now and new3 not in before, "the new chunk is there")
        check(not os.path.isdir(rw.HELD), "the held old outputs are gone afterwards")
        check(all(now[k] == before[k] for k in before if not k.startswith("recompiled")), "the other lifts' files untouched")

        # the toolkit's lifter changed: every step lifts again, and jobs with spu; identical outputs keep their times
        age_all(rw, 3600)
        before = mtimes(rw)
        write(os.path.join(tmp, "tk", "tools", "ppu_lifter.py"), "# lifter v2\n")
        ran = run(rw, tmp)
        check(ran == ["firmware", "ppu", "spu", "jobs"], "toolkit tools changed: all lift again: %r" % ran)
        check(mtimes(rw) == before, "same bytes out: every file kept its time (nothing to compile)")

        # a lift switch in the environment counts; one for the compile does not
        os.environ["PORT_JOBS"] = "3"
        check(run(rw, tmp) == [], "PORT_JOBS (compile only) does not lift again")
        os.environ["SPU_LIFT_MERGE_CHUNKS"] = "0"
        ran = run(rw, tmp)
        check(ran == ["firmware", "ppu", "spu", "jobs"], "SPU_LIFT_MERGE_CHUNKS=0: lifts again: %r" % ran)
        del os.environ["SPU_LIFT_MERGE_CHUNKS"], os.environ["PORT_JOBS"]
        run(rw, tmp)

        # only the jobs' inputs (the port's tools) changed: spu stays, jobs lifts again inside spu/
        before = mtimes(rw)
        write(os.path.join(tmp, "port", "tools", "lift_datamods.py"), "# port tool v2\n")
        ran = run(rw, tmp)
        check(ran == ["jobs"], "port tools changed: only jobs: %r" % ran)
        check(os.path.isfile(os.path.join(rw.PORT, "spu", "spu_0000", "spu_recomp.c")), "spu's own files still there")
        check(mtimes(rw).get(os.path.join("spu", "jobmod_aa", "spu_recomp.c")) == before[os.path.join("spu", "jobmod_aa", "spu_recomp.c")],
              "the identical job output kept its time")

        # a stopped run: fingerprint gone, old outputs held; the next run lifts again and still restores times
        age_all(rw, 3600)
        before = mtimes(rw)
        write(os.path.join(tmp, "cheats", "hooks.txt"), "00196508\n")
        make_steps(rw, tmp, [])
        check(not rw.lift_skip("ppu"), "ppu will run")
        check(not os.path.isfile(os.path.join(rw.LIFT, "ppu.fp")), "its fingerprint is dropped before it runs")
        write(os.path.join(rw.PORT, "recompiled", "ppu_recomp_000.cpp"), "// half written")   # then the run stops
        rw = load(tmp, True)
        ran = run(rw, tmp)
        check(ran == ["ppu"], "after a stop: ppu lifts again: %r" % ran)
        check(mtimes(rw)[os.path.join("recompiled", "ppu_recomp_000.cpp")] == before[os.path.join("recompiled", "ppu_recomp_000.cpp")],
              "after a stop: the unchanged chunk still gets the time of the last good build")

        # a run without PORT_LIFT_REUSE (the dev way) drops the fingerprints of what it lifts
        rw = load(tmp, False)
        ran = run(rw, tmp)
        check(ran == ["firmware", "ppu", "spu", "jobs"], "without PORT_LIFT_REUSE everything lifts: %r" % ran)
        check(not any(os.path.isfile(os.path.join(rw.LIFT, s + ".fp")) for s in ran), "... and no fingerprint is left")
        rw = load(tmp, True)
        check(run(rw, tmp) == ["firmware", "ppu", "spu", "jobs"], "so the next PORT_LIFT_REUSE run lifts again")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d checks, %d failed" % (checks, fails))
    return fails


if __name__ == "__main__":
    sys.exit(main())
