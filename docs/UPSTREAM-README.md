# ry5088-flasher

Open-source tools and firmware for **RongYuan RY5088** magnetic keyboards — an **Artery AT32F405** main MCU with a
**Panchip PAN1080** wireless co-processor, shipped by MonsGeek, Akko, and others. The project has two parts:

- **`flasher/`** — `ry-flash`, a single-binary **stock-firmware flasher** with a terminal UI and a scriptable
  JSON CLI. It restores official firmware over USB (no case-opening, no special tools) and is *variant-safe*:
  it reads the connected keyboard's `dev_id` and only flashes that model's matching image.
- **`firmware/`** — buildable **custom firmware** for the AT32F405: one shared codebase in `common/`
  (key-scan, Hall rapid-trigger, WS2812 RGB, flash-config persistence, the vendor protocol, optional
  wireless), specialised per keyboard by a small **profile** at `firmware/profiles/<dev_id>.toml`.
  `make PROFILE=<dev_id>` builds that model's image; adding a board is one profile — see
  [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Supported models

The **flasher** works with any Hall-effect (**HE**) / TMR magnetic keyboard on the RY5088 platform: **FUN60 /
Pro / Max / Ultra**, **FUN68**, **FUN75**, **M1 / M2 / M3 V5** (HE and TMR), **Akko 5075B / 5087B HE**,
**Shine68**, and other `ry5088_*` boards — identified by `dev_id` (the USB PID is shared across many models).
Full table: [`docs/models.md`](docs/models.md).

The **custom firmware** ships a profile for the **Fun60 Ultra** (`dev_id 2307`) as the reference; other models
are added by contributing a profile (`make -C firmware models` lists those present) — see
[`CONTRIBUTING.md`](CONTRIBUTING.md).

> Not RY5088 (do not use the flasher on these): **MG75S HE**, **M1W HE**, **M1 V5 VIA** (YiChip YC3121) and
> **ICE75** (YiChip YC500). The flasher refuses any image that isn't a keyboard `AT32F405 8KMKB` build.

## Quick start

**Flash stock firmware** (plug the keyboard in via USB, close the vendor app):
```bash
cd flasher
cargo build --release            # needs libhidapi (brew install hidapi / libhidapi-dev)
./target/release/ry-flash        # interactive TUI — detect, confirm, flash
# or headless / scriptable:
./target/release/ry-flash --auto --arm        # detect + flash the matching stock image (auto-downloads if needed)
./target/release/ry-flash --detect --json     # machine-readable device info
```

**Build the custom firmware** (needs `arm-none-eabi-gcc` and the Artery BSP — see [`firmware/README.md`](firmware/README.md)):
```bash
cd firmware
make PROFILE=2307                         # build the Fun60 Ultra image -> build/2307/ry5088_2307.bin
make test                                 # host-side unit tests (no ARM toolchain / BSP needed)
make check                                # validate every board profile
```

## Add your keyboard
The platform and vendor protocol are shared across all these boards, so supporting a new model is mostly
*data*: its `dev_id`, matrix geometry, keymap, LED map, and switch params — one `profiles/<dev_id>.toml`.
With Claude Code, the **`/re-tmr`** skill drives the whole flow (detect → fetch stock firmware → analyse →
draft the profile → build). Flashing is recoverable (app-slot only; the flasher always restores stock), so
it is safe to experiment. Full playbook: [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Documentation
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — **add your keyboard**: the profile schema and the contributor workflow
- [`docs/models.md`](docs/models.md) — supported models and the dev_id scheme
- [`docs/flashing.md`](docs/flashing.md) — flasher usage, the JSON/agent contract, recovery
- [`docs/protocol.md`](docs/protocol.md) — the HID-DFU flash protocol and firmware API
- [`docs/cloud-api.md`](docs/cloud-api.md) — the vendor firmware-fetch API (`--fetch`) and driver transport
- [`docs/firmware.md`](docs/firmware.md) — custom firmware architecture and building
- [`docs/hardware.md`](docs/hardware.md) — the board (MCU, flash map, key matrix, RGB, wireless)

## Status & safety
The flasher is the mature, validated component. The custom firmware builds and passes host tests but has **not**
completed on-hardware bring-up — treat it as experimental. Flashing is recoverable: a failed transfer leaves the
board in its bootloader, and the flasher can always restore stock firmware. Entering the bootloader clears the app
config (re-settable in the vendor app); per-key Hall calibration is preserved.

## Related projects
Open firmware for these magnetic boards **is** being built — credit where it's due:

- **[libhmk](https://github.com/peppapighs/libhmk)** — a mature open-source **Hall-effect keyboard firmware** that already runs on the **AT32F405** and implements the advanced modes this repo's firmware only stubs: Dynamic Keystroke, Null Bind (SOCD / Snap-Tap), Tap-Hold, Toggle, Rapid Trigger, 8 kHz, plus a web configurator. **If you want a feature-complete *replacement* firmware, start here.**
- **[QMK PR #25215](https://github.com/qmk/qmk_firmware/pull/25215)** + the **[qmk-arterytek](https://github.com/qmk-arterytek)** org — bringing the Artery **AT32F402/F405** MCU into QMK (ChibiOS port, UF2 bootloader, KiCad libs).
- [echtzeit-solutions/monsgeek-akko-linux](https://github.com/echtzeit-solutions/monsgeek-akko-linux) — a Linux **driver** for the stock firmware.
- [RieGan/rongyuan-kb-software](https://github.com/RieGan/rongyuan-kb-software) · [noaione/rongyuan-software](https://github.com/noaione/rongyuan-software) — reverse-engineered RongYuan config **software**.

**Where this project fits.** Those *replace* the firmware (and bring their own configurator). This one is the
complement: it **flashes and recovers the official stock firmware over USB** — no vendor app, variant-safe across
446 boards, fully recoverable — plus the cloud-API / `dev_id` / board-catalog reverse-engineering behind it. The
bundled custom firmware is a **stock-protocol-compatible reference** (still configurable from the vendor app); for
a feature-complete open firmware on the same MCU, use **libhmk**.

## License
MIT — see [`LICENSE`](LICENSE). The Artery AT32F402_405 Firmware Library (fetched separately to build the firmware)
is covered by its own license; see `firmware/THIRD-PARTY-NOTICES.md`.
