"""Apply a git-style unified diff without git (the build must not need git).

Strict, like `git apply`: every hunk's context and removed lines must match the file exactly
(a hunk may sit a few lines away from where the header says, as with git). Line ends: the
file's own (LF or CRLF) are kept. Handles changed, new and deleted files; no binary patches.

    from apply_patch import apply_patch; apply_patch("x.patch", root, strip=1)
    python tools/apply_patch.py <x.patch> [<root>]          (exit 1 + a message if it does not apply)
"""
import os
import re
import sys

HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


class PatchError(Exception):
    pass


def _strip(path, n):
    if path == "/dev/null":
        return None
    path = path.split("\t")[0].strip()
    parts = path.split("/")
    return "/".join(parts[n:]) if len(parts) > n else None


def parse(text, strip=1):
    """[(old_path, new_path, [(old_start, old_lines, new_lines, no_eol_old, no_eol_new)])]"""
    files, lines, i = [], text.split("\n"), 0
    if lines and lines[-1] == "":
        lines.pop()
    while i < len(lines):
        if not lines[i].startswith("--- ") or i + 1 >= len(lines) or not lines[i + 1].startswith("+++ "):
            i += 1
            continue
        old, new = _strip(lines[i][4:], strip), _strip(lines[i + 1][4:], strip)
        i += 2
        hunks = []
        while i < len(lines) and lines[i].startswith("@@"):
            m = HUNK.match(lines[i])
            if not m:
                raise PatchError("bad hunk header: " + lines[i])
            ostart, ocount = int(m.group(1)), int(m.group(2) or 1)
            ncount = int(m.group(4) or 1)
            i += 1
            o, n, last = [], [], None
            no_eol_o = no_eol_n = False
            while (len(o) < ocount or len(n) < ncount) or (i < len(lines) and lines[i].startswith("\\")):
                if i >= len(lines):
                    raise PatchError("patch ends inside a hunk")
                ln = lines[i]
                i += 1
                if ln.startswith("\\"):            # "\ No newline at end of file" for the line before
                    if last in (" ", "-"):
                        no_eol_o = True
                    if last in (" ", "+"):
                        no_eol_n = True
                    continue
                tag, body = (ln[:1], ln[1:]) if ln else (" ", "")
                if tag == " ":
                    o.append(body); n.append(body)
                elif tag == "-":
                    o.append(body)
                elif tag == "+":
                    n.append(body)
                else:
                    raise PatchError("unexpected line in a hunk: " + ln)
                last = tag
            hunks.append((ostart, o, n, no_eol_o, no_eol_n))
        files.append((old, new, hunks))
    if not files:
        raise PatchError("no file changes in the patch")
    return files


def _find(lines, want, at):
    """Index of `want` in `lines`, the match nearest to `at` (git allows an offset)."""
    if not want:
        return min(max(at, 0), len(lines))
    best = None
    for start in range(0, len(lines) - len(want) + 1):
        if lines[start:start + len(want)] == want:
            if best is None or abs(start - at) < abs(best - at):
                best = start
    return best


def apply_patch(patch, root, strip=1):
    """Apply every file change of `patch` under `root`; raises PatchError (nothing half-written)."""
    with open(patch, encoding="utf-8", newline="") as f:
        text = f.read().replace("\r\n", "\n")
    out = {}
    for old, new, hunks in parse(text, strip):
        path = new or old
        full = os.path.join(root, path)
        if old is None:
            lines, eol, final_nl = [], "\n", True
        else:
            if not os.path.isfile(full):
                raise PatchError(f"{path}: no such file")
            with open(full, "rb") as f:
                raw = f.read().decode("utf-8", "surrogateescape")
            eol = "\r\n" if raw.count("\r\n") > raw.count("\n") / 2 else "\n"
            final_nl = raw.endswith("\n")
            lines = raw.replace("\r\n", "\n").split("\n")
            if final_nl:
                lines.pop()
        delta = 0
        for k, (ostart, o, n, no_eol_o, no_eol_n) in enumerate(hunks, 1):
            at = _find(lines, o, ostart - 1 + delta if o else ostart + delta)
            if at is None:
                raise PatchError(f"{path}: hunk {k} does not match the file")
            lines[at:at + len(o)] = n
            delta += len(n) - len(o)
            if no_eol_n:
                final_nl = False
            elif no_eol_o:
                final_nl = True
        out[full] = None if new is None else (eol.join(lines) + (eol if final_nl and lines else ""))
    for full, data in out.items():
        if data is None:
            os.remove(full)
            continue
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data.encode("utf-8", "surrogateescape"))
    return sorted(out)


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit("usage: apply_patch.py <patch> [<root>]")
    try:
        for p in apply_patch(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else "."):
            print("patched", p)
    except PatchError as e:
        sys.exit(f"{os.path.basename(sys.argv[1])}: {e}")
