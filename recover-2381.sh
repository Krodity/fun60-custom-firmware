#!/usr/bin/env bash
# Recover the FUN60 Ultra (dev_id 2381) via the AT32F405 ROM bootloader (BOOT0 held at power-on).
# Writes the stock v302 app slice (chip-ID header + app) at 0x08005000; the RY bootloader below it is untouched.
set -uo pipefail   # the :leave step always ends with a harmless "get_status" error as the chip resets
cd "$(dirname "$0")"
echo "waiting for ROM DFU device 2e3c:df11 (hold BOOT0 to 3.3V while plugging in)..."
until lsusb -d 2e3c:df11 >/dev/null 2>&1; do sleep 0.5; done
sudo dfu-util -l | grep -i 2e3c
sudo dfu-util -a 0 -d 2e3c:df11 -s 0x08005000:leave -D stock/2381/stock_slice_0x08005000.bin
sleep 3; lsusb | grep -i 3151 && ry-flash --detect --json
