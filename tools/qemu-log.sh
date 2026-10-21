#!/bin/bash
# Dump the whole kernel message buffer from a QEMU run.
#   tools/qemu-log.sh PAYLOAD KERNEL [SECS] [OUTDIR]   -> OUTDIR/msgbuf.txt (default out/q/log)
set -euo pipefail
payload=$1 kernel=$2 secs=${3:-12} out=${4:-out/q/log}
here=$(cd "$(dirname "$0")" && pwd)
run() { python3 "$here/qemu-a135f.py" "$payload" "$out" --secs "$secs" --kernel "$kernel" "$@" >/dev/null 2>&1 || true; }
sym=$(llvm-nm "$kernel" | awk '$3=="_msgbufp"{print $1}')
[ -n "$sym" ] || { echo "no _msgbufp symbol in $kernel" >&2; exit 1; }
quads() { grep -o '0x[0-9a-f]\{16\}' "$out/regs.txt" | tail -n "$1"; }
run --hmp "x /1gx 0x$sym"
msgbuf=$(quads 1)
run --hmp "x /3gx $msgbuf"
bufc=$(quads 3 | sed -n 3p)
echo "msgbuf struct $msgbuf, buffer $bufc" >&2
run --hmp "memsave $bufc 0x20000 \"$PWD/$out/msgbuf.bin\""
tr -d '\0' < "$out/msgbuf.bin" > "$out/msgbuf.txt"
rm -f "$out/msgbuf.bin"
wc -l "$out/msgbuf.txt" >&2
