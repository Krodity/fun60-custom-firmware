#!/usr/bin/env python3
"""fun60-rgb - per-key colours and live frames for the custom FUN60 (dev_id 2381) firmware.

    fun60-rgb map                     light each LED in turn; press the key under it (61 presses)
    fun60-rgb set KEY COLOR [...]     per-key colour, e.g.  set esc red  w,a,s,d '#00ffaa'
    fun60-rgb fill COLOR              every key one colour
    fun60-rgb row N COLOR             a physical row (1 = number row .. 5 = bottom row)
    fun60-rgb show                    print the saved per-key layout
    fun60-rgb apply                   re-send the saved layout to the keyboard
    fun60-rgb live [DEMO]             stream frames (needs lighting set to "Live" via Fn+\\)
                                      demos: wave (default), sparkle
    fun60-rgb screen [--fps N] [--output NAME] [--raw]
                                      ambient: each key = the AVERAGE colour of its patch of
                                      the screen (sampled from a 1280x720 guide), N frames/s
                                      (default 15); output defaults to the focused monitor.
                                      Each key samples its real ANSI 60% rectangle; colours are
                                      saturation-weighted averages, brightness-normalised, gamma-
                                      corrected and smoothed; --raw sends the plain averages

Per-key colours show when the active profile's lighting is "Custom" (Fn+\\ cycle).
Live frames show when it is "Live". The LED->key map is learnt once with `map`.

Python API (for your own effects):
    from fun60_rgb import Fun60
    kb = Fun60(); kb.live({"esc": (255, 0, 0), "space": (0, 0, 255)})
"""
import colorsys, fcntl, glob, json, math, os, random, re, sys, time

try:
    import tomllib
except ImportError:  # python < 3.11
    tomllib = None

HERE = os.path.dirname(os.path.realpath(__file__))
PROFILE = os.path.join(HERE, "..", "firmware", "profiles", "2381.toml")
CONF = os.path.expanduser("~/.config/fun60")
MAP_FILE = os.path.join(CONF, "ledmap.json")       # LED index -> scan site
COLORS_FILE = os.path.join(CONF, "colors.json")    # key name -> "#rrggbb"
NUM_LEDS = 61
ROWS = 8                                           # site = col*8 + row

NAMED = {"red": (255, 0, 0), "green": (0, 255, 0), "blue": (0, 64, 255), "white": (255, 255, 255),
         "off": (0, 0, 0), "black": (0, 0, 0), "yellow": (255, 200, 0), "orange": (255, 80, 0),
         "purple": (160, 0, 255), "pink": (255, 40, 120), "cyan": (0, 220, 255), "magenta": (255, 0, 200)}

# HID usage -> short key name (what you type on the command line)
USAGE = {0x29: "esc", 0x2A: "bksp", 0x2B: "tab", 0x39: "caps", 0x28: "enter", 0x2C: "space",
         0xE0: "lctrl", 0xE1: "lshift", 0xE2: "lalt", 0xE3: "lgui", 0xE4: "rctrl", 0xE5: "rshift",
         0xE6: "ralt", 0x65: "menu", 0xF0: "fn", 0x2D: "-", 0x2E: "=", 0x2F: "[", 0x30: "]",
         0x31: "\\", 0x33: ";", 0x34: "'", 0x36: ",", 0x37: ".", 0x38: "/"}
USAGE.update({0x04 + i: chr(ord("a") + i) for i in range(26)})
USAGE.update({0x1E + i: str((i + 1) % 10) for i in range(10)})


def _ioc(nr, n):
    return (3 << 30) | (n << 16) | (ord("H") << 8) | nr


def parse_color(s):
    s = s.strip().lower()
    if s in NAMED:
        return NAMED[s]
    m = re.fullmatch(r"#?([0-9a-f]{6})", s)
    if not m:
        raise SystemExit(f"bad colour {s!r} (use a name like red, or #rrggbb)")
    v = int(m.group(1), 16)
    return (v >> 16 & 255, v >> 8 & 255, v & 255)


def keymap_sites():
    """key name -> scan site, from the profile's measured keymap."""
    text = open(PROFILE, "rb").read()
    if tomllib:
        codes = tomllib.loads(text.decode())["keymap"]["codes"]
    else:
        body = text.decode().split("[keymap]")[1].split("codes = [")[1].split("]")[0]
        codes = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    out = {}
    for site, code in enumerate(codes):
        name = USAGE.get(code)
        if name and name not in out:
            out[name] = site
    return out


class Fun60:
    def __init__(self):
        nodes = []
        for d in glob.glob("/sys/class/hidraw/*"):
            ev = open(d + "/device/uevent").read()
            if "00003151:0000502D" in ev:
                nodes.append("/dev/" + os.path.basename(d))
        if not nodes:
            raise SystemExit("FUN60 (3151:502D) not found")
        self.node = sorted(nodes, key=lambda n: int(n.rsplit("hidraw", 1)[1]))[-1]  # vendor IF
        self.fd = os.open(self.node, os.O_RDWR)
        self._map = None

    def q(self, payload):
        r = bytearray(64)
        r[:len(payload)] = bytes(payload)
        r[7] = (0xFF - sum(r[:7])) & 0xFF                  # header checksum (bytes 0..7 sum to 0xFF)
        fcntl.ioctl(self.fd, _ioc(6, 65), bytes([0]) + bytes(r))
        b = bytearray(65)
        fcntl.ioctl(self.fd, _ioc(7, 65), b)
        return b[1:]

    # ---- LED <-> key map --------------------------------------------------
    def led_map(self):
        """LED index -> key name (from the learnt map + the profile keymap)."""
        if self._map is None:
            if not os.path.exists(MAP_FILE):
                raise SystemExit("no LED map yet: run  fun60-rgb map")
            led_site = json.load(open(MAP_FILE))
            site_key = {s: k for k, s in keymap_sites().items()}
            self._map = {i: site_key.get(s) for i, s in enumerate(led_site)}
        return self._map

    def key_leds(self):
        return {k: i for i, k in self.led_map().items() if k}

    def frame_from(self, colors):
        """dict key->rgb or list[61] -> list[61] of rgb"""
        if isinstance(colors, dict):
            kl = self.key_leds()
            f = [(0, 0, 0)] * NUM_LEDS
            for k, c in colors.items():
                if k in kl:
                    f[kl[k]] = c
            return f
        return list(colors)

    # ---- live frames (shown when lighting = "Live") ------------------------
    def live(self, colors):
        f = self.frame_from(colors)
        for start in range(0, NUM_LEDS, 18):
            chunk = f[start:start + 18]
            p = [0xF5, start, len(chunk), 0, 0, 0, 0, 0]
            for r, g, b in chunk:
                p += [r, g, b]
            self.q(p)

    # ---- saved per-key colours (shown when lighting = "Custom") -------------
    def set_userpic(self, colors):
        f = self.frame_from(colors)
        for page in range(math.ceil(NUM_LEDS / 18)):
            chunk = f[page * 18:page * 18 + 18]
            p = [0x0C, 0, 0xFF, page, 0, 0, 0, 0]          # always bulk pages (see firmware note)
            for r, g, b in chunk:
                p += [r, g, b]
            self.q(p)


def load_colors():
    return json.load(open(COLORS_FILE)) if os.path.exists(COLORS_FILE) else {}


def save_and_apply(kb, colors):
    os.makedirs(CONF, exist_ok=True)
    json.dump(colors, open(COLORS_FILE, "w"), indent=1, sort_keys=True)
    kb.set_userpic({k: parse_color(v) for k, v in colors.items()})


def cmd_map(kb):
    print("Mapping LEDs: one LED lights at a time - press the key under it.")
    print("(Keys don't type while mapping. Ctrl+C to cancel.)")
    kb.q([0xF6, 1])
    last = -1
    try:
        while True:
            st = kb.q([0xF6, 2])
            state, led = st[1], st[2]
            if led != last:
                print(f"\r  LED {led + 1 if state == 1 else led}/{NUM_LEDS}", end="", flush=True)
                last = led
            if state == 2:
                break
            time.sleep(0.05)
    except KeyboardInterrupt:
        kb.q([0xF6, 0])
        raise SystemExit("\ncancelled")
    sites = []
    for start in range(0, NUM_LEDS, 56):
        b = kb.q([0xF7, start])
        sites += list(b[8:8 + b[2]])
    os.makedirs(CONF, exist_ok=True)
    json.dump(sites, open(MAP_FILE, "w"))
    site_key = {s: k for k, s in keymap_sites().items()}
    unknown = [s for s in sites if s not in site_key]
    print(f"\nsaved {MAP_FILE}  ({NUM_LEDS - len(unknown)} LEDs matched to keys"
          + (f", {len(unknown)} unmatched sites {unknown}" if unknown else "") + ")")


def demo(kb, name):
    leds = kb.led_map()
    sites = json.load(open(MAP_FILE))
    col = [s // ROWS for s in sites]
    row = [s % ROWS for s in sites]
    print(f"streaming '{name}' - Ctrl+C to stop (set lighting to Live with Fn+\\)")
    heat = [0.0] * NUM_LEDS
    t0 = time.time()
    try:
        while True:
            t = time.time() - t0
            if name == "sparkle":
                heat = [h * 0.85 for h in heat]
                for _ in range(3):
                    heat[random.randrange(NUM_LEDS)] = 1.0
                f = [tuple(int(255 * c * h) for c in colorsys.hsv_to_rgb((t * 0.1 + i / 61) % 1, 0.8, 1))
                     for i, h in enumerate(heat)]
            else:  # wave
                f = [tuple(int(255 * c) for c in colorsys.hsv_to_rgb((col[i] / 14 + row[i] / 20 - t * 0.4) % 1, 1, 0.6))
                     for i in range(NUM_LEDS)]
            kb.live(f)
            time.sleep(1 / 60)
    except KeyboardInterrupt:
        print()


# Physical ANSI 60% layout (key name, width in key units); every row is 15u wide.
LAYOUT = [
    [("esc", 1)] + [(c, 1) for c in "1234567890-="] + [("bksp", 2)],
    [("tab", 1.5)] + [(c, 1) for c in "qwertyuiop[]"] + [("\\", 1.5)],
    [("caps", 1.75)] + [(c, 1) for c in "asdfghjkl;'"] + [("enter", 2.25)],
    [("lshift", 2.25)] + [(c, 1) for c in "zxcvbnm,./"] + [("rshift", 2.75)],
    [("lctrl", 1.25), ("lgui", 1.25), ("lalt", 1.25), ("space", 6.25),
     ("ralt", 1.25), ("fn", 1.25), ("menu", 1.25), ("rctrl", 1.25)],
]


def key_rects(w, h):
    """key name -> (x0, y0, x1, y1) pixel rectangle on a w x h image"""
    out = {}
    for r, row in enumerate(LAYOUT):
        x = 0.0
        for name, units in row:
            out[name] = (int(round(x / 15 * w)), int(round(r / 5 * h)),
                         max(int(round((x + units) / 15 * w)), int(round(x / 15 * w)) + 1),
                         max(int(round((r + 1) / 5 * h)), int(round(r / 5 * h)) + 1))
            x += units
    return out


def focused_output():
    import subprocess
    mons = json.loads(subprocess.run(["hyprctl", "monitors", "-j"], capture_output=True, text=True).stdout)
    return next((m["name"] for m in mons if m.get("focused")), mons[0]["name"])


def boost(cells):  # (legacy grid mode helper)
    """Keep each averaged cell's hue, but normalise brightness across the grid and
    lift saturation - dark, low-contrast desktops otherwise light the keys a
    near-uniform dim grey."""
    hsv = [colorsys.rgb_to_hsv(*(c / 255 for c in px)) for px in cells]
    vmax = max((v for _, _, v in hsv), default=0) or 1.0
    out = []
    for h, sat, v in hsv:
        v2 = min(1.0, 0.15 + 0.85 * v / vmax)         # brightest cell -> full, keep relative shading
        s2 = min(1.0, sat * 1.8)
        out.append(tuple(int(255 * c) for c in colorsys.hsv_to_rgb(h, s2, v2)))
    return out


HUE_BINS = 12                      # colour clumps per key: 12 hues + 1 neutral bin


_LUT = None


def _clump_lut():
    """15-bit colour -> (clump bin, vividness weight), built once. Moves all the
    per-pixel hue/saturation maths out of the 15 fps loop."""
    global _LUT
    if _LUT is None:
        import numpy as np
        q = np.arange(32768)
        rgb = np.stack([(q >> 10) & 31, (q >> 5) & 31, q & 31], 1).astype(np.float32) * (255 / 31) / 255
        r, g, b = rgb[:, 0], rgb[:, 1], rgb[:, 2]
        mx, mn = rgb.max(1), rgb.min(1)
        sat = (mx - mn) / (mx + 1e-4)
        d = (mx - mn) + 1e-6
        hue = np.where(mx == r, ((g - b) / d) % 6, np.where(mx == g, (b - r) / d + 2, (r - g) / d + 4)) / 6.0
        colourful = (sat > 0.25) & (mx > 0.12)
        hbin = np.where(colourful, np.minimum((hue * HUE_BINS).astype(np.int32), HUE_BINS - 1), HUE_BINS)
        _LUT = (hbin.astype(np.int32), (sat * mx).astype(np.float32))
    return _LUT


def dominant_clumps(px8, labels, n):
    """Per key, pick the dominant colour CLUMP instead of averaging everything.

    px8: uint8 (N, 3) pixels. Every pixel goes into one of 12 hue bins if it is
    clearly coloured (sat > 0.25, value > 0.12), else the neutral bin (via a
    15-bit lookup table). For each key, colourful clumps are scored by pixel
    count x mean vividness; the winner's own mean colour lights the key, so a
    small saturated region (an icon, a logo, a video's subject) survives instead
    of being diluted into grey. If coloured pixels are under 4% of the key's
    region, the neutral clump's mean is used (whites/greys/blacks)."""
    import numpy as np
    lut_bin, lut_viv = _clump_lut()
    p = px8.astype(np.int32)
    q = ((p[:, 0] >> 3) << 10) | ((p[:, 1] >> 3) << 5) | (p[:, 2] >> 3)
    nb = HUE_BINS + 1
    idx = labels * nb + lut_bin[q]                      # (key, bin) flat index
    tot = n * nb
    cnt = np.bincount(idx, None, tot).reshape(n, nb)[:NUM_LEDS]
    vivid = np.bincount(idx, lut_viv[q], tot).reshape(n, nb)[:NUM_LEDS]
    sums = np.stack([np.bincount(idx, p[:, c], tot).reshape(n, nb)[:NUM_LEDS] for c in range(3)], 2)
    best = vivid[:, :HUE_BINS].argmax(1)                 # score = count x mean vividness
    keys = np.arange(NUM_LEDS)
    use_colour = cnt[keys, best] / np.maximum(cnt.sum(1), 1) > 0.04
    pick = np.where(use_colour, best, HUE_BINS)
    return (sums[keys, pick] / np.maximum(cnt[keys, pick], 1)[:, None] / 255.0).astype(np.float32)


def screen(kb, fps=15.0, output=None, raw=False):
    """Screen-ambient lighting at up to ~15 fps.

    Capture: one persistent wf-recorder stream (Hyprland screencopy). ffmpeg
    drops to the target fps FIRST, then area-averages down to a 1280x720 colour
    guide (~85 px per key unit) - no per-frame process spawn. Per-key means are
    one vectorised bincount over a precomputed pixel->LED label map. Geometry: every key samples its real ANSI 60% rectangle
    (Space = the wide bottom-middle strip, staggered rows, wide modifiers).
    Colour: per key, the dominant colour clump of its region (see
    dominant_clumps) - small vivid regions survive; then, unless --raw, brightness is
    normalised across the board, saturation lifted, LED gamma applied, and a
    light exponential smoothing removes flicker."""
    import subprocess
    import numpy as np
    output = output or focused_output()
    W, H = 1280, 720                                   # the colour guide (720p)
    rate = int(max(1, min(fps, 30)))
    kl = kb.key_leds()
    labels = np.full((H, W), NUM_LEDS, np.int32)       # NUM_LEDS = "not a key"
    for k, (x0, y0, x1, y1) in key_rects(W, H).items():
        if k in kl:
            labels[y0:y1, x0:x1] = kl[k]
    labels = labels.ravel()
    cmd = ["wf-recorder", "-y", "-o", output, "-c", "rawvideo", "-m", "rawvideo", "-x", "rgb24",
           "-F", f"fps={rate},scale={W}:{H}:flags=area", "-r", str(rate), "-f", "/dev/stdout"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    print(f"screen ambient from {output} at {rate} fps, 720p colour guide, per-key ANSI geometry"
          f"{' (raw averages)' if raw else ''} - Ctrl+C to stop (lighting must be Live: Fn+\\\\)")
    fb = W * H * 3
    smooth = None
    try:
        while True:
            buf = proc.stdout.read(fb)
            if len(buf) < fb:
                raise SystemExit("capture stream ended")
            px8 = np.frombuffer(buf, np.uint8).reshape(-1, 3)
            n = NUM_LEDS + 1
            if raw:                                         # plain per-key mean
                cnt = np.bincount(labels, None, n)[:NUM_LEDS]
                cols = np.stack([np.bincount(labels, px8[:, c], n)[:NUM_LEDS] for c in range(3)], 1)
                cols = (cols / np.maximum(cnt, 1)[:, None] / 255.0).astype(np.float32)
            else:
                cols = dominant_clumps(px8, labels, n)
            if not raw:
                v = cols.max(1, keepdims=True)
                vmax = float(v.max()) or 1.0
                grey = cols.mean(1, keepdims=True)
                cols = grey + (cols - grey) * 1.6             # saturation lift around each key's grey
                cols = np.clip(cols, 0, None)
                cols *= (0.12 + 0.88 * v / vmax) / np.maximum(cols.max(1, keepdims=True), 1e-4)
                cols = np.clip(cols, 0, 1) ** 1.8             # LED gamma: dim tones stay dim, not grey
                smooth = cols if smooth is None else smooth * 0.4 + cols * 0.6
                out = smooth
            else:
                out = cols
            kb.live([tuple(int(c * 255) for c in rgb) for rgb in out])
    except KeyboardInterrupt:
        print()
    finally:
        proc.terminate()


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return
    kb = Fun60()
    cmd, args = argv[0], argv[1:]
    if cmd == "map":
        cmd_map(kb)
    elif cmd == "set":
        if len(args) % 2:
            raise SystemExit("usage: set KEY[,KEY..] COLOR [KEY COLOR ...]")
        colors, known = load_colors(), set(kb.key_leds())
        for keys, c in zip(args[::2], args[1::2]):
            parse_color(c)
            for k in keys.lower().split(","):
                if k not in known:
                    raise SystemExit(f"unknown key {k!r}; keys: {' '.join(sorted(known))}")
                colors[k] = c
        save_and_apply(kb, colors)
        print("saved; shows when lighting is 'Custom' (Fn+\\)")
    elif cmd == "fill":
        save_and_apply(kb, {k: args[0] for k in kb.key_leds()})
        print("saved")
    elif cmd == "row":
        n, c = int(args[0]), args[1]
        colors = load_colors()
        for k, s in keymap_sites().items():
            if s % ROWS == n and k in kb.key_leds():
                colors[k] = c
        save_and_apply(kb, colors)
        print("saved")
    elif cmd == "show":
        for k, v in sorted(load_colors().items()):
            print(f"  {k:7s} {v}")
    elif cmd == "apply":
        save_and_apply(kb, load_colors())
        print("applied")
    elif cmd == "live":
        demo(kb, args[0] if args else "wave")
    elif cmd == "screen":
        fps, output, raw = 15.0, None, False
        it = iter(args)
        for a in it:
            if a == "--fps": fps = float(next(it))
            elif a == "--output": output = next(it)
            elif a == "--raw": raw = True
            else: raise SystemExit(f"unknown option {a!r}")
        screen(kb, fps, output, raw)
    else:
        raise SystemExit(f"unknown command {cmd!r} (see --help)")


if __name__ == "__main__":
    main(sys.argv[1:])
