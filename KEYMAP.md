# FUN60 (custom firmware) — layers, lighting, keybinds

Board: MonsGeek FUN60 Ultra, dev_id 2381 (FUN60_PRO 5088-1049 PCB), ANSI 60%.

## Base layer

```
┌─────┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬────────┐
│ Esc │ 1 │ 2 │ 3 │ 4 │ 5 │ 6 │ 7 │ 8 │ 9 │ 0 │ - │ = │  Bksp  │
├─────┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬──────┤
│  Tab  │ Q │ W │ E │ R │ T │ Y │ U │ I │ O │ P │ [ │ ] │  \   │
├───────┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴┬──┴──────┤
│  Caps  │ A │ S │ D │ F │ G │ H │ J │ K │ L │ ; │ ' │  Enter  │
├────────┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─┬─┴─────────┤
│  LShift  │ Z │ X │ C │ V │ B │ N │ M │ , │ . │ / │   RShift  │
├─────┬────┴┬──┴──┬┴───┴───┴───┴───┴───┴──┬┴───┬┴───┬┴────┬─────┤
│Ctrl │ Win │ Alt │         Space         │Alt │ Fn │Menu │Ctrl │
└─────┴─────┴─────┴───────────────────────┴────┴────┴─────┴─────┘
```

## Fn layer (hold Fn) — works on every profile

| Fn + | Does | | Fn + | Does |
|---|---|---|---|---|
| Esc | \` ~ | | I / J / K / L | ↑ ← ↓ → |
| **1 / 2 / 3** | **profile 1 / 2 / 3** | | U / O | Home / End |
| **4 / 5 / 6** | **⏮ prev / ⏯ play-pause / ⏭ next** | | Y / H | Page Up / Page Down |
| **7 / 8 / 9** | **mute / vol − / vol +** | | P | Print Screen |
| Backspace | Delete | | Tab | next profile (1→2→3) |
| \\ | next lighting mode (this profile) | | Left Ctrl | rapid trigger on/off |
| ; | colour ← / slower | | ' | colour → / faster |

Unlisted keys type normally while Fn is held. F1–F12 live on profile 3.

## Profile 2 layer (active only on profile 2, no Fn needed)

| 4 | 5 | 6 | 7 | 8 | 9 | I | J | K | L |
|---|---|---|---|---|---|---|---|---|---|
| ⏮ Prev | ⏯ Play/Pause | ⏭ Next | Mute | Vol − | Vol + | ↑ | ← | ↓ | → |

(So on profile 2 those keys don't type 4–9 / i j k l — use profile 1 for typing. Hold Fn for the normal Fn layer.)

## Profile 3 layer (active only on profile 3, no Fn needed)

| 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 0 | - | = |
|---|---|---|---|---|---|---|---|---|---|---|---|
| F1 | F2 | F3 | F4 | F5 | F6 | F7 | F8 | F9 | F10 | F11 | F12 |

## Profiles (Fn + 1/2/3, or Fn + Tab to cycle) — default look of each

| Profile | Guide key | Default lighting |
|---|---|---|
| 1 | `1` | Rainbow wave |
| 2 | `2` | Solid red |
| 3 | `3` | Solid blue |

Each profile remembers its own lighting mode (changed with Fn + \\).

## Lighting modes (Fn + \\) — guide keys on the Q row

| Q | W | E | R | T | Y | U | I | O | P |
|---|---|---|---|---|---|---|---|---|---|
| Wave | Static | Breathing | Rainbow | Random rainbow | Neon | Ripple | Custom | Live | Off |

- **Custom** = your per-key colours (`fun60-rgb set …`).
- **Live** = colours streamed from the PC (`fun60-rgb screen`, `fun60-rgb live wave|sparkle`). Holds the last frame when the stream stops.
- Static / Breathing / Neon / Ripple use the profile's colour (white on profile 1).

## Status guide

- **On Fn + Tab / Fn + \\**: that row lights as a guide — the new choice **flashes purple ×3**, other options dim white, unused slots dark.
- **Fn + ; / Fn + '**: in single-colour modes (Static, Breathing, Neon, Ripple) step through 13 colours (red, orange, yellow, lime, green, mint, cyan, sky, blue, violet, purple, magenta, white); in animated modes change speed, shown on **A S D F G** (slowest → fastest, new level flashes purple ×3). Colour and speed are remembered per profile.
- **Fn + Left Ctrl**: **Space flashes red ×3 = rapid trigger ON, blue ×3 = OFF**.
- **Hold Fn**: both rows show the guide, current profile + mode **solid purple**.

## Switch feel (current)

Actuation **0.50 mm** · release **0.15 mm** · rapid trigger **0.06 mm press / 0.15 mm release**, on for all keys.
The 0.15 mm releases keep held keys (Win, Backspace, Shift) down while your finger relaxes; quick taps still re-trigger after 0.06 mm.

## Per-key colours & live lighting (PC)

```
fun60-rgb set esc red   w,a,s,d '#00ffaa'   # per key (names: esc 1..0 - = bksp tab q.. \ caps a.. ' enter
fun60-rgb row 1 orange                      #           lshift z.. / rshift lctrl lgui lalt space ralt fn menu rctrl)
fun60-rgb fill blue
fun60-rgb show                              # print saved colours
fun60-rgb screen [--fps 15] [--output DP-2] [--raw]
fun60-rgb live wave | sparkle
```
Only run one `fun60-rgb` at a time, and stop it before flashing.

---

## Where to edit (all under `~/Projects/ry5088-flasher/`)

| What | File | Where in it |
|---|---|---|
| Base keymap | `firmware/profiles/2381.toml` | `[keymap] codes` — HID usage IDs, index = col×8 + row; each line's comment names the keys |
| Fn layer | `firmware/profiles/2381.toml` | `[fnmap] codes` — same layout; `0x00` = fall through to base |
| Per-profile layers | `firmware/profiles/2381.toml` | `[layer1]`…`[layer3]` `codes` — override the base while that profile is active (`[layer2]` exists); `0x00` = same as base |
| Special Fn actions | (codes you can put in `[fnmap]`/`[layerN]`) | `0xF1` next profile · `0xF6`/`0xF7`/`0xF8` profile 1/2/3 · `0xF2` next lighting · `0xF3` rapid-trigger toggle · `0xF4` / `0xF5` colour or speed down / up · media `0xE8` prev `0xE9` play/pause `0xEA` next `0xEB` mute `0xEC` vol− `0xED` vol+ · `0xF0` = the Fn key itself (in `[keymap]`) |
| Colour palette for Fn+;/' | `firmware/common/main.c` | `colour_palette[]` |
| Profile default colours | `firmware/common/main.c` | `profile_led[]` — `{mode, speed 0-4, brightness 0-4, dir, flag 7=colour/8=rainbow, R, G, B}` |
| Lighting mode order (Q…P) | `firmware/common/main.c` | `light_cycle[]` |
| Guide colour / flash speed | `firmware/common/main.c` | `guide_purple[]`, `GUIDE_FLASH_MS`, `GUIDE_FLASHES` |
| Rapid-trigger/actuation defaults | `firmware/profiles/2381.toml` | `[hall]` (live values are stored on the keyboard — change those with the vendor `0x65` command, see below) |
| Per-key custom colours | `~/.config/fun60/colors.json` | then `fun60-rgb apply` |
| LED → key map | `firmware/profiles/2381.toml` `[leds] led_site` (copy in `~/.config/fun60/ledmap.json`) | re-learn with `fun60-rgb map` |
| PC lighting tool | `tools/fun60_rgb.py` | `screen()`, `dominant_clumps()`, `demo()` |

HID usage IDs: a–z `0x04–0x1D`, 1–0 `0x1E–0x27`, Enter `0x28`, Esc `0x29`, Bksp `0x2A`, Tab `0x2B`, Space `0x2C`,
F1–F12 `0x3A–0x45`, arrows → `0x4F` ← `0x50` ↓ `0x51` ↑ `0x52`, LCtrl `0xE0` LShift `0xE1` LAlt `0xE2` LGui `0xE3`
(right side `0xE4–0xE7`). Full list: USB HID Usage Tables, Keyboard page (0x07).

### Apply changes

```
cd ~/Projects/ry5088-flasher/firmware
make check                                   # validate the profile
rm -rf build/2381 && make PROFILE=2381       # build (always clean: make misses define changes)
make PROFILE=2381 test                       # host tests
cd .. && ./flash-custom-2381.sh              # flash over USB; restores stock on phantom input
```

Change actuation / rapid trigger without reflashing (values in 0.01 mm, all 64 entries):
```
python3 -c "
import sys; sys.path.insert(0,'tools'); from fun60_rgb import Fun60
kb=Fun60()
for k in range(64):
    kb.q([0x65,0x00,0,k,0,0,0,0, 50,0])   # actuation 0.50 mm
    kb.q([0x65,0x01,0,k,0,0,0,0, 15,0])   # release   0.15 mm
    kb.q([0x65,0x02,0,k,0,0,0,0,  6,0])   # RT press  0.06 mm
    kb.q([0x65,0x03,0,k,0,0,0,0, 15,0])   # RT release 0.15 mm
"
```
Below ~0.04 mm rapid trigger, and below ~0.3 mm actuation, the keys start chattering / firing from resting fingers.

### Get back to stock / recover

- Stock firmware: `ry-flash --auto --image stock/2381/fw_at32.bin --arm`
- If it ever won't show up on USB: wire **BOOT1** pad (below the MCU, by the J1 holes) to the **left tab of U2**, plug in, then `./recover-2381.sh`.
