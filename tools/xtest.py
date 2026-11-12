"""Fake X input through XTest (libXtst; no xdotool needed), for Linux test runs (tools/linux_run.py's
xkey / xmove / xclick / xfocus steps):
    python3 xtest.py key <keysym> [hold_ms]      e.g. key w 1500, key Return, key F2
    python3 xtest.py down <keysym> | up <keysym>
    python3 xtest.py move <dx> <dy> [steps]      relative pointer motion
    python3 xtest.py click <button> [hold_ms]    1 left, 3 right
    python3 xtest.py focus                        name + geometry of the focused window, pointer position
    python3 xtest.py windows                      top-level windows (id, name, geometry)
    python3 xtest.py activate <name substring>    raise + focus that window
DISPLAY / XAUTHORITY from the environment (default :0 and the GDM session's). Input is refused unless the
focused window's name holds XTEST_REQUIRE_FOCUS (default "inFAMOUS"): a desktop's other windows (a system
dialog with a default button) never get a key."""
import ctypes
import ctypes.util
import os
import sys
import time

os.environ.setdefault("DISPLAY", ":0")
xa = "/run/user/%d/gdm/Xauthority" % os.getuid()
if "XAUTHORITY" not in os.environ and os.path.exists(xa):
    os.environ["XAUTHORITY"] = xa
X = ctypes.cdll.LoadLibrary(ctypes.util.find_library("X11") or "libX11.so.6")
T = ctypes.cdll.LoadLibrary(ctypes.util.find_library("Xtst") or "libXtst.so.6")
X.XOpenDisplay.restype = ctypes.c_void_p
X.XOpenDisplay.argtypes = [ctypes.c_char_p]
X.XStringToKeysym.restype = ctypes.c_ulong
X.XStringToKeysym.argtypes = [ctypes.c_char_p]
X.XKeysymToKeycode.restype = ctypes.c_ubyte
X.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
X.XFlush.argtypes = [ctypes.c_void_p]
X.XDefaultRootWindow.restype = ctypes.c_ulong
X.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
T.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
T.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
T.XTestFakeRelativeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]

d = X.XOpenDisplay(None)
if not d:
    sys.exit("cannot open display " + os.environ["DISPLAY"])


def kc(name):
    ks = X.XStringToKeysym(name.encode())
    if not ks:
        sys.exit("unknown keysym " + name)
    return X.XKeysymToKeycode(d, ks)


def key(name, down):
    T.XTestFakeKeyEvent(d, kc(name), 1 if down else 0, 0)
    X.XFlush(d)


class XWindowAttributes(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int), ("height", ctypes.c_int),
                ("border_width", ctypes.c_int), ("depth", ctypes.c_int), ("visual", ctypes.c_void_p),
                ("root", ctypes.c_ulong), ("class", ctypes.c_int), ("bit_gravity", ctypes.c_int),
                ("win_gravity", ctypes.c_int), ("backing_store", ctypes.c_int), ("backing_planes", ctypes.c_ulong),
                ("backing_pixel", ctypes.c_ulong), ("save_under", ctypes.c_int), ("colormap", ctypes.c_ulong),
                ("map_installed", ctypes.c_int), ("map_state", ctypes.c_int), ("all_event_masks", ctypes.c_long),
                ("your_event_mask", ctypes.c_long), ("do_not_propagate_mask", ctypes.c_long),
                ("override_redirect", ctypes.c_int), ("screen", ctypes.c_void_p)]


def wname(w):
    p = ctypes.c_char_p()
    X.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_char_p)]
    if X.XFetchName(d, w, ctypes.byref(p)) and p.value:
        return p.value.decode("utf-8", "replace")
    return ""


def geom(w):
    a = XWindowAttributes()
    X.XGetWindowAttributes.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(XWindowAttributes)]
    X.XGetWindowAttributes(d, w, ctypes.byref(a))
    return a


def toplevels():
    root = X.XDefaultRootWindow(d)
    r, par = ctypes.c_ulong(), ctypes.c_ulong()
    ch, n = ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
    X.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                             ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)), ctypes.POINTER(ctypes.c_uint)]
    out = []

    def walk(w, depth):
        if depth > 3:
            return
        rr, pp = ctypes.c_ulong(), ctypes.c_ulong()
        cc, nn = ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
        if not X.XQueryTree(d, w, ctypes.byref(rr), ctypes.byref(pp), ctypes.byref(cc), ctypes.byref(nn)):
            return
        for i in range(nn.value):
            c = cc[i]
            nm = wname(c)
            if nm:
                out.append((c, nm))
            walk(c, depth + 1)
    walk(root, 0)
    return out


def focused():
    """(window, name) of the focused window, walking up to the first named ancestor."""
    w, rev = ctypes.c_ulong(), ctypes.c_int()
    X.XGetInputFocus.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int)]
    X.XGetInputFocus(d, ctypes.byref(w), ctypes.byref(rev))
    cur = w.value
    X.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                             ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)), ctypes.POINTER(ctypes.c_uint)]
    for _ in range(8):
        if cur <= 1:
            break
        nm = wname(cur)
        if nm:
            return cur, nm
        rr, pp = ctypes.c_ulong(), ctypes.c_ulong()
        cc, nn = ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
        if not X.XQueryTree(d, cur, ctypes.byref(rr), ctypes.byref(pp), ctypes.byref(cc), ctypes.byref(nn)):
            break
        cur = pp.value
    return w.value, ""


# Safety: input goes only to the game. The server's desktop may show other dialogs (an Ubuntu
# upgrade prompt whose default button must never get a Return): every key/click/move is refused
# unless the focused window's name holds XTEST_REQUIRE_FOCUS (default "inFAMOUS").
cmd = sys.argv[1] if len(sys.argv) > 1 else "focus"
if cmd in ("key", "down", "up", "move", "click"):
    need = os.environ.get("XTEST_REQUIRE_FOCUS", "inFAMOUS")
    fw, fn = focused()
    if need and need.lower() not in fn.lower():
        sys.exit("refused: the focused window is 0x%x %r, not %r" % (fw, fn, need))
if cmd == "key":
    key(sys.argv[2], True)
    time.sleep((int(sys.argv[3]) if len(sys.argv) > 3 else 80) / 1000.0)
    key(sys.argv[2], False)
elif cmd in ("down", "up"):
    key(sys.argv[2], cmd == "down")
elif cmd == "move":
    dx, dy = int(sys.argv[2]), int(sys.argv[3])
    steps = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    for i in range(steps):
        T.XTestFakeRelativeMotionEvent(d, dx // steps, dy // steps, 0)
        X.XFlush(d)
        time.sleep(0.016)
elif cmd == "click":
    # a click lands where the pointer is: first put it in the middle of the focused game window, and
    # click only when the window under it is that window (or inside it) -- never on the desktop
    b = int(sys.argv[2])
    fw, fn = focused()
    a = geom(fw)
    X.XWarpPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                               ctypes.c_uint, ctypes.c_uint, ctypes.c_int, ctypes.c_int]
    X.XWarpPointer(d, 0, fw, 0, 0, 0, 0, a.width // 2, a.height // 2)
    X.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    X.XSync(d, 0)
    X.XQueryPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong] + [ctypes.POINTER(ctypes.c_ulong)] * 2 + \
        [ctypes.POINTER(ctypes.c_int)] * 4 + [ctypes.POINTER(ctypes.c_uint)]
    cur, ok = X.XDefaultRootWindow(d), False
    for _ in range(10):   # down the tree to the deepest window under the pointer
        r, ch = ctypes.c_ulong(), ctypes.c_ulong()
        rx, ry, wx, wy, m = ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_uint()
        X.XQueryPointer(d, cur, ctypes.byref(r), ctypes.byref(ch), ctypes.byref(rx), ctypes.byref(ry),
                        ctypes.byref(wx), ctypes.byref(wy), ctypes.byref(m))
        if cur == fw:
            ok = True
        if not ch.value:
            break
        cur = ch.value
    ok = ok or cur == fw
    if not ok:
        sys.exit("refused: the pointer is not over the game window 0x%x %r (under it: 0x%x)" % (fw, fn, cur))
    T.XTestFakeButtonEvent(d, b, 1, 0); X.XFlush(d)
    time.sleep((int(sys.argv[3]) if len(sys.argv) > 3 else 80) / 1000.0)
    T.XTestFakeButtonEvent(d, b, 0, 0); X.XFlush(d)
elif cmd == "focus":
    w, nm = focused()
    a = geom(w) if w > 1 else None
    print("focus 0x%x %r %s" % (w, nm, "%dx%d+%d+%d" % (a.width, a.height, a.x, a.y) if a else ""))
elif cmd == "windows":
    for w, nm in toplevels():
        a = geom(w)
        print("0x%x\t%dx%d+%d+%d\tmap=%d\t%s" % (w, a.width, a.height, a.x, a.y, a.map_state, nm))
elif cmd == "activate":
    sub = sys.argv[2].lower()
    hits = [(w, nm) for w, nm in toplevels() if sub in nm.lower()]
    if not hits:
        sys.exit("no window named like " + sub)
    w = hits[0][0]
    X.XRaiseWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    X.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    X.XRaiseWindow(d, w)
    X.XSetInputFocus(d, w, 1, 0)   # RevertToPointerRoot... 1 = RevertToPointerRoot
    X.XFlush(d)
    print("activated 0x%x %s" % (w, hits[0][1]))
else:
    sys.exit(__doc__)
