"""What SDL (the game's own SDL2.dll) sees: every joystick, whether it is a game controller,
its type, GUID and mapping, and -- with --watch N -- N seconds of live input (buttons pressed,
sticks past a dead zone), so a controller can be checked before playing.

    python tools/pad_probe.py [--watch 10] [--positional]

--positional sets SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0 (what play.ps1 uses: buttons by
position, so a Nintendo-layout pad's bottom button is Cross).
"""
import ctypes, os, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dll = os.path.join(ROOT, "work", "port", "build", "SDL2.dll")
os.environ.setdefault("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1")   # as play.ps1: input without window focus
if "--positional" in sys.argv:
    os.environ["SDL_GAMECONTROLLER_USE_BUTTON_LABELS"] = "0"
sdl = ctypes.CDLL(dll)
sdl.SDL_GetError.restype = ctypes.c_char_p
sdl.SDL_JoystickNameForIndex.restype = ctypes.c_char_p
sdl.SDL_GameControllerName.restype = ctypes.c_char_p
sdl.SDL_GameControllerMapping.restype = ctypes.c_void_p
sdl.SDL_GameControllerOpen.restype = ctypes.c_void_p
sdl.SDL_GameControllerGetButton.argtypes = [ctypes.c_void_p, ctypes.c_int]
sdl.SDL_GameControllerGetAxis.argtypes = [ctypes.c_void_p, ctypes.c_int]
sdl.SDL_GameControllerGetAxis.restype = ctypes.c_int16
sdl.SDL_GameControllerGetType.argtypes = [ctypes.c_void_p]
sdl.SDL_GameControllerMapping.argtypes = [ctypes.c_void_p]
sdl.SDL_GameControllerName.argtypes = [ctypes.c_void_p]
sdl.SDL_free.argtypes = [ctypes.c_void_p]


class GUID(ctypes.Structure):
    _fields_ = [("data", ctypes.c_uint8 * 16)]


sdl.SDL_JoystickGetDeviceGUID.restype = GUID
TYPES = ["unknown", "Xbox 360", "Xbox One", "PS3", "PS4", "Switch Pro", "virtual", "PS5", "Amazon Luna",
         "Google Stadia", "NVIDIA Shield", "Switch Joy-Con L", "Switch Joy-Con R", "Switch Joy-Con pair"]
BUTTONS = ["A(south)", "B(east)", "X(west)", "Y(north)", "Back/Select", "Guide/Home", "Start", "L3", "R3",
           "LB/L1", "RB/R1", "Up", "Down", "Left", "Right", "Misc/Capture", "Paddle1", "Paddle2", "Paddle3",
           "Paddle4", "Touchpad"]
AXES = ["LX", "LY", "RX", "RY", "LT", "RT"]

if sdl.SDL_Init(0x2000 | 0x200) != 0:   # GAMECONTROLLER | JOYSTICK
    sys.exit("SDL_Init failed: %s" % sdl.SDL_GetError())
time.sleep(1.0)                          # let HIDAPI finish talking to the pads
sdl.SDL_PumpEvents()
n = sdl.SDL_NumJoysticks()
print(f"SDL {dll}: {n} joystick(s); button labels {'OFF (positional)' if '--positional' in sys.argv else 'default'}")
opened = []
for i in range(n):
    name = sdl.SDL_JoystickNameForIndex(i)
    g = sdl.SDL_JoystickGetDeviceGUID(i)
    guid = "".join(f"{b:02x}" for b in g.data)
    gc = sdl.SDL_IsGameController(i)
    print(f"  [{i}] {name.decode(errors='replace') if name else '?'}  guid {guid}  game controller: {bool(gc)}")
    if gc:
        c = sdl.SDL_GameControllerOpen(i)
        if not c:
            print("      open failed:", sdl.SDL_GetError())
            continue
        t = sdl.SDL_GameControllerGetType(c)
        m = sdl.SDL_GameControllerMapping(c)
        print(f"      type {TYPES[t] if 0 <= t < len(TYPES) else t}")
        if m:
            print("      mapping", ctypes.cast(m, ctypes.c_char_p).value.decode(errors="replace")[:220])
            sdl.SDL_free(m)
        opened.append((i, c))

if "--watch" in sys.argv and opened:
    secs = float(sys.argv[sys.argv.index("--watch") + 1])
    print(f"watching {secs:.0f} s -- press buttons, move sticks")
    last = {}
    end = time.time() + secs
    while time.time() < end:
        sdl.SDL_GameControllerUpdate()
        for i, c in opened:
            pressed = [BUTTONS[b] for b in range(len(BUTTONS)) if sdl.SDL_GameControllerGetButton(c, b)]
            axes = [f"{AXES[a]}={sdl.SDL_GameControllerGetAxis(c, a)}" for a in range(6)
                    if abs(sdl.SDL_GameControllerGetAxis(c, a)) > (8000 if a < 4 else 3000)]
            state = (tuple(pressed), tuple(axes))
            if state != last.get(i):
                last[i] = state
                print(f"  [{i}] {' '.join(pressed) or '-'}  {' '.join(axes)}")
        time.sleep(0.02)
sdl.SDL_Quit()
