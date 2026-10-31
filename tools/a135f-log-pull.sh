#!/bin/sh
# Read the A135F console log (kernel patch 0047) from a phone booted into
# recovery with an adb root shell (TWRP/OrangeFox for the A13) or from postmarketOS over ssh/adb.
#   tools/a135f-log-pull.sh OUT.txt
# Only reads: the last 1025 blocks (4 KiB) of the userdata partition.
set -eu
out=${1:?usage: a135f-log-pull.sh OUT.txt}
dev=$(adb shell 'for d in /dev/block/by-name/userdata /dev/block/platform/*/by-name/userdata /dev/disk/by-partlabel/userdata /dev/mmcblk0p*; do [ -e $d ] && readlink -f $d && break; done' | tr -d '\r')
[ -n "$dev" ] || { echo "userdata block device not found" >&2; exit 1; }
bytes=$(adb shell blockdev --getsize64 "$dev" | tr -d '\r')
skip=$((bytes / 4096 - 1025))
adb exec-out "dd if=$dev bs=4096 skip=$skip count=1025 2>/dev/null" > "$out.bin"
echo "$dev: $bytes bytes, read blocks $skip.. ($(stat -c %s "$out.bin") bytes)" >&2
python3 "$(dirname "$0")/a135f-log.py" "$out.bin" "$out"
