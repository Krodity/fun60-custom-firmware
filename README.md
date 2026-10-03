# fun60-custom-firmware

Open, reverse-engineered firmware for the **MonsGeek FUN60 Ultra** magnetic (TMR) keyboard —
RongYuan **RY5088** platform, Artery **AT32F405** MCU — with a full Fn layer, per-profile key
layers, media keys, rapid trigger, per-key RGB, and live/screen-reactive lighting driven from the PC.

> **Built from:** [dot-agi/ry5088-flasher](https://github.com/dot-agi/ry5088-flasher) — the stock-firmware
> flasher and reference custom firmware this project started from (MIT). Its original README is kept at
> [`docs/UPSTREAM-README.md`](docs/UPSTREAM-README.md).

**Status: daily-driver on the author's board.** Typing, layers, media keys, rapid trigger and RGB are
verified on hardware. It targets one specific board revision (below); other RY5088 boards need their own
profile and will likely need the hardware notes re-checked.

| | |
|---|---|
| Keyboard | MonsGeek FUN60 Ultra, wired, TMR switches, ANSI 60% (61 keys) |
| Firmware id | `dev_id 2381` (`ry5088_fun60ultra_8k_dm`), USB `3151:502D` |
| PCB | silkscreen `FUN60_PRO(5088-1049)-3M-RGB V0.3-20250424` |
| MCU | Artery AT32F405 (marked RY5088), 12 MHz crystal, USB **OTGHS** (high-speed) |

---

## Features

- **Analog key scanning** of all 61 TMR sensors at **~1100 Hz**, with a 4-sample moving average and a
  spike-proof baseline (no phantom or stuck keys).
- **Adjustable actuation & rapid trigger** (per key, saved on the keyboard). Current: actuation 0.50 mm,
  release 0.15 mm, rapid trigger 0.06 mm press / 0.15 mm release (held keys stay held; taps re-trigger fast).
  These are also the profile's factory defaults (`[hall]` / `[magnetism]`). **Fn + Left Ctrl** toggles rapid trigger — Space flashes
  **red ×3 (on)** / **blue ×3 (off)**.
- **Three profiles** (Fn + 1 / 2 / 3, or Fn + Tab to cycle), each with its own lighting, colour, speed and
  optional **key layer**:
  - Profile 2: 4/5/6 = ⏮ ⏯ ⏭, 7/8/9 = mute / vol − / vol +, I J K L = arrows.
  - Profile 3: number row = F1 … F12.
- **Fn layer** on every profile: arrows (IJKL), Home/End, PgUp/PgDn, Delete, Print Screen, \`, media
  & volume (Fn + 4 … 9), profile select, lighting controls.
- **Media keys** via a USB consumer-control report (works with any OS media handling).
- **10 lighting modes** (Fn + \\): Wave, Static, Breathing, Rainbow, Random rainbow, Neon, Ripple,
  **Custom** (per-key colours), **Live** (PC-streamed frames), Off. Fn + ; / ' changes the **colour**
  (13-colour palette) in single-colour modes or the **speed** in animated ones.
- **Status guide** on the top rows: number row = profiles, Q…P = lighting modes, A…G = speed. Changes
  flash the selected key purple ×3; holding Fn shows the current state.
- **`fun60-rgb` PC tool**: per-key colours by key name, live effects, and **screen-reactive ambient
  lighting** (15 fps, 720p colour guide, per-key ANSI geometry, dominant-colour detection).
- **Safe flashing**: over USB with no tools; reflash/return-to-stock any time; a phantom-input safety net
  auto-restores stock if a build misbehaves.

The complete key/colour reference is in **[`KEYMAP.md`](KEYMAP.md)**.

---

## Repository layout

```
firmware/
  common/              shared firmware (scan, Hall/TMR engine, RGB, USB HID, vendor protocol, …)
  profiles/2381.toml   THIS BOARD: matrix, keymap, Fn layer, profile layers, LED map, Hall defaults
  profiles/2381.ledmap.json   learnt LED -> key map (copy of the [leds].led_site table)
  profiles/2307.toml   upstream reference profile (FUN60 Ultra, wireless variant) — unverified
  gen/gen_profile.py   profile -> generated board_config.h / board_keymap.h / board_led_map.c
  Makefile             make PROFILE=2381 / make test / make check
flasher/               ry-flash: Rust USB flasher (stock + custom images), JSON CLI + TUI
tools/fun60_rgb.py     fun60-rgb: per-key colours, live frames, screen ambient, Python API
flash-custom-2381.sh   build-independent flash of the custom image with the phantom-input safety net
recover-2381.sh        last-resort recovery via the MCU's ROM DFU (BOOT1 strap)
KEYMAP.md              layers / colours / keybinds reference + where to edit each
docs/                  protocol, hardware, flashing notes (upstream) + UPSTREAM-README.md
```

---

## Requirements

| For | Needs |
|---|---|
| Firmware | `arm-none-eabi-gcc`, `make`, `python3` (3.11+ for `tomllib`) |
| Artery BSP | `git clone https://github.com/ArteryTek/AT32F402_405_Firmware_Library firmware/vendor/AT32F402_405_Firmware_Library` (not redistributed here) |
| Flasher | Rust (`cargo`), `hidapi` |
| RGB tool | `python3`, `numpy`, `pillow`; `grim` + `wf-recorder` for screen ambient (wlroots/Hyprland) |
| Recovery | `dfu-util` (and `sudo`, or a udev rule for `2e3c:df11`) |

Linux needs hidraw access to `3151:*` (e.g. the udev rule shipped by Sharkfin, or
`SUBSYSTEM=="hidraw", ATTRS{idVendor}=="3151", TAG+="uaccess"`).

---

## Build

```sh
cd firmware
make check                         # validate every profile through the generator
rm -rf build/2381 && make PROFILE=2381   # always clean: make does not track -D changes
make PROFILE=2381 test             # host-side unit tests (no ARM toolchain needed)

cd ../flasher && cargo build --release    # -> flasher/target/release/ry-flash
```

The image is `firmware/build/2381/ry5088_2381.bin` (loads at `0x08005200`).

## Flash

```sh
./flash-custom-2381.sh
```

It prepends the 512-byte chip-ID header the RY bootloader expects (`AT32F405 8KMKB` + zeros), puts the
running firmware into its bootloader over USB, flashes, and watches the keyboard for 6 s — if it sees the
phantom-input signature (ErrorRollOver or ≥3 modifiers) it restores stock automatically. Normal typing
during the window is fine.

> Only **one** program may talk to the keyboard's vendor interface at a time (feature-report replies share a
> buffer). Stop `fun60-rgb` (e.g. `screen`) before flashing.

### Back to stock

```sh
ry-flash --fetch 2381 --stock-dir stock          # download the official image (not included here)
ry-flash --auto --image stock/2381/fw_at32.bin --arm
```

The vendor cloud currently serves **v302** (boards may ship with v304 — restoring is a downgrade).

### Recovery (if the board ever stops enumerating)

The RY bootloader has no key-combo entry, so a firmware that never brings up USB can only be recovered
through the AT32 ROM bootloader:

1. Open the case. On the PCB find the round test pad **`BOOT1`** just below the MCU (U5), next to C32 and
   the 5-hole **J1** header.
2. Bridge **BOOT1** to **3.3 V** — the wide **left tab of U2** (the SOT-223 regulator near the yellow
   tantalum C9) — while plugging the cable in; release after ~2 s. It enumerates as `2e3c:df11`.
3. `./recover-2381.sh` (writes the stock slice at `0x08005000`; the RY bootloader is untouched).

Since the enter-bootloader command now runs from the USB interrupt, a running custom build can always be
reflashed over USB even if its main loop hangs — the BOOT1 route should not be needed again.

---

## Customising

Edit **`firmware/profiles/2381.toml`**, rebuild, flash:

| What | Where |
|---|---|
| Base keymap | `[keymap] codes` — HID usage IDs, index = `col*8 + row` (comments name each key) |
| Fn layer | `[fnmap] codes` — `0x00` falls through to the base/profile layer |
| Per-profile layers | `[layer1]` … `[layer3]` — override the base while that profile is active |
| LED → key map | `[leds] led_site` (re-learn with `fun60-rgb map`) |

Internal action codes usable in any layer:

| Code | Action | Code | Action |
|---|---|---|---|
| `0xF0` | Fn key (base keymap only) | `0xF5` | colour / speed + |
| `0xF1` | next profile | `0xF6`/`F7`/`F8` | select profile 1 / 2 / 3 |
| `0xF2` | next lighting mode | `0xE8` `0xE9` `0xEA` | ⏮ prev / ⏯ play-pause / ⏭ next |
| `0xF3` | rapid trigger on/off | `0xEB` `0xEC` `0xED` | mute / vol − / vol + |
| `0xF4` | colour / speed − | | |

Lighting presets, palette, mode order and guide colour live in `firmware/common/main.c`
(`profile_led[]`, `colour_palette[]`, `light_cycle[]`, `guide_purple[]`).

Actuation / rapid trigger can be changed live without reflashing — see `KEYMAP.md`
(vendor command `0x65`, values in 0.01 mm, written per key). Measured limits on this board: below
~0.04 mm rapid trigger keys chatter; below ~0.3 mm actuation resting fingers trigger keys; a
rapid-trigger *release* as small as 0.06 mm drops held keys (Win/Backspace) — 0.15 mm keeps them held.

---

## `fun60-rgb`

```sh
fun60-rgb map                         # learn the LED -> key map (press the key under the lit LED, 61x)
fun60-rgb set esc red  w,a,s,d '#00ffaa'
fun60-rgb row 1 orange | fill blue | show | apply
fun60-rgb live wave | sparkle         # needs lighting = Live (Fn + \)
fun60-rgb screen [--fps 15] [--output DP-2] [--raw]
```

`screen` captures with one persistent `wf-recorder` stream (frames dropped to the target fps **before**
scaling), area-averages to a 1280×720 colour guide, gives every key its real ANSI-60% rectangle, picks the
**dominant colour clump** per key (12 hue bins + neutral, scored by count × vividness — small vivid regions
survive instead of averaging to grey), then normalises brightness, lifts saturation, applies LED gamma and
light smoothing. ~22 ms of processing per frame.

Python API:

```python
from fun60_rgb import Fun60
kb = Fun60()
kb.live({"esc": (255, 0, 0), "space": (0, 0, 255)})   # by key name, or a list of 61 RGB tuples
```

---

## How the hardware works (reverse-engineering notes)

Everything below was confirmed against the stock 2381 firmware (disassembly of the vendor image) and
then on hardware.

| Subsystem | Finding |
|---|---|
| **USB** | Stock bootloader **and** app use **OTGHS** (`0x40040000`, enumerates high-speed); OTGFS1 (PA11/PA12) is not wired. The upstream build targeted OTGFS1, so it never enumerated. → `USB_CORE ?= USB_OTG_HS`. |
| **Vendor HID** | The config interface needs an interrupt-IN endpoint or Linux `usbhid` won't bind it (no hidraw node). Added EP `0x83`. |
| **Sensor front-end** | PA2 = ADC1 ch2 (mux output). Column counter on PA4 (reset) / PA5 (clock) / PA6 (strobe); 3-bit row mux PC6/PC7/PC8. **14 columns × mux rows 1–5** (rows 0/6/7 unused). |
| **Sensor power/enable** | **PF7 must be LOW** (HIGH → every site reads 0). **PC4, PC5, PC10, PA7, PB9 driven HIGH** (stock board init) — without them sites sit at a fixed ~810 that ignores magnets. |
| **ADC** | Needs `adc_ordinary_conversion_trigger_set(…SOFTWARE, TRUE)` or the software trigger is ignored and the first conversion waits forever. Clock AHB/8, sample time 28.5, calibration. |
| **Column counter** | Clock pulses must be ≥1 µs (≈90 ns pulses miscounted → random columns). Settle 5 µs after column select, 1 µs after the mux (stock SysTick delays). |
| **Signal** | Idle ~2000–2220 counts (mid-rail), full press ~900–1100 (**pressing lowers the reading**), idle noise ±4–8 counts. Response is non-linear (little change near the top of travel). |
| **LEDs** | 61× WS2812 on SPI2 MOSI = **PA10** (AF5), DMA1 ch1 via DMAMUX req 13, one SPI byte per bit (`0xC0`/`0xF0`). **PB13 = LED power enable.** Chain is a row serpentine (Esc…Bksp, \\…Tab, Caps…Enter, RShift…LShift, LCtrl…RCtrl). |
| **Bootloader** | RY DFU at `3151:502A`; stays in DFU only if the mailbox `0x08004800 == 0x55AA55AA` or the chip-ID at `0x08005000` is wrong (no key/pin entry). Image = 512-byte header (`AT32F405 8KMKB`) + app at `0x08005200`. |

### Fixes vs. upstream

Firmware:
- USB on OTGHS; vendor interface endpoint; enter-bootloader writes the DFU mailbox (stock-style) and runs
  from the USB IRQ (works even if the main loop is wedged).
- ADC software trigger + calibration wait; bounded spin waits everywhere (no more hangs).
- Board power/enable pins, PF7 polarity, 1 µs counter clock, 8-row indexing, column-sweep scan
  (5.0 ms → 0.87 ms per pass).
- Hall engine: rate-limited, self-healing baseline (a single noise spike used to hold a key "pressed").
- HID: IN-endpoint busy tracking — reports are never overwritten in flight, releases are retried (fixed
  stuck/held keys).
- Per-profile layers, Fn actions, media (consumer) reports, profiles/lighting/colour/speed/RT controls,
  status guide, live frames, LED-map learning, random-rainbow effect.
- Host tests made layout-independent; new tests for the Fn layer, random rainbow and baseline spikes.

`ry-flash`:
- Accepts normal-mode PID `0x502D` (the board's real PID; the tool only knew `0x5030`).
- 10 ms DFU chunk pacing (2 ms silently dropped chunks on Linux hidraw, so nothing ever flashed).

### Bring-up / diagnostic vendor commands (not in stock)

| Cmd | Use |
|---|---|
| `0x8F` GET_INFOR | bytes 3–6 = boot stage + main-loop pass count |
| `0xF0` | raw ADC samples (28 per report) |
| `0xF1` / `0xF2` | on-device first-press log / hall-engine pressed bitmap |
| `0xF3` / `0xF4` | GPIO poke / hardware snapshot (LED frames, DMA, SPI, GPIO outputs) |
| `0xF5` | live LED frame data |
| `0xF6` / `0xF7` | LED → key mapping control / read |

A `DIAG_NO_HID` build (`make PROFILE=2381 EXTRA_DEFS=-DDIAG_NO_HID`) scans and serves these without ever
sending a keystroke — use it when bringing up a new board.

---

## Safety

- Flashing writes only the application region; the RY bootloader is never touched.
- Entering the bootloader from **stock** wipes the stock app config (re-settable in the vendor app); the
  custom firmware keeps its own config at `0x08028000`.
- This is unofficial firmware for one PCB revision. Use at your own risk; keep the recovery steps handy.

## Credits & license

- **[dot-agi/ry5088-flasher](https://github.com/dot-agi/ry5088-flasher)** — the original flasher, protocol
  documentation and reference firmware this project is built on (MIT). Upstream README:
  [`docs/UPSTREAM-README.md`](docs/UPSTREAM-README.md).
- Artery AT32F402_405 Firmware Library — fetched separately, under its own license
  (`firmware/THIRD-PARTY-NOTICES.md`).
- Related: [libhmk](https://github.com/peppapighs/libhmk) (feature-complete open HE firmware on the AT32F405),
  [qmk-arterytek](https://github.com/qmk-arterytek).

MIT — see [`LICENSE`](LICENSE). Stock vendor firmware is **not** included or redistributed; `ry-flash`
downloads it on demand.
