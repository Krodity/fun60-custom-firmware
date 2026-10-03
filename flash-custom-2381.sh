#!/usr/bin/env bash
# Build-independent flash of the custom 2381 image with a phantom-input safety net:
# after flashing, watch the boot-keyboard interface for 6 s and restore stock if a
# report carries ErrorRollOver (0x01) or 3+ modifier bits (the bring-up failure
# signature). Normal typing during the window does not trip it.
set -uo pipefail
cd "$(dirname "$0")"
B=firmware/build/2381
{ dd if=stock/2381/fw_at32.bin bs=512 skip=$((0x5000/512)) count=1 status=none; cat $B/ry5088_2381.bin; } > $B/ry5088_2381_rydfu.bin
ry-flash --auto --image $B/ry5088_2381_rydfu.bin --arm --json 2>/dev/null | grep -o '"match":[a-z]*' || { echo "flash failed"; exit 1; }
sleep 2
node=$(for d in /sys/class/hidraw/hidraw*; do grep -q 0000502D $d/device/uevent && echo /dev/$(basename $d); done | head -1)
python3 - "$node" <<'EOF'
import os, select, sys, time
fd = os.open(sys.argv[1], os.O_RDONLY); t0 = time.time(); bad = 0; n = 0
while time.time() - t0 < 6:
    r, _, _ = select.select([fd], [], [], 0.5)
    if not r: continue
    b = os.read(fd, 64); n += 1
    if 0x01 in b[2:8] or bin(b[0]).count('1') >= 3: bad += 1
print(f"safety window: {n} reports, {bad} phantom-signature")
sys.exit(1 if bad else 0)
EOF
if [ $? -ne 0 ]; then
  echo ">>> phantom signature seen - restoring stock"
  ry-flash --auto --image stock/2381/fw_at32.bin --arm --json 2>/dev/null | grep -o '"match":[a-z]*'
  exit 2
fi
echo ">>> custom firmware running"
