#!/bin/bash
# Build the shim and wrap it in an Android boot.img for the Samsung bootloader.
#
#   shim/build.sh PAYLOAD [REF_BOOT_IMG]        (boot.img for a stock/Odin flash)
#   Always writes out/uniloader-blobs/{Image,ramdisk} for uniLoader; the boot.img/Odin files only if mkbootimg exists.
#
# PAYLOAD      xnu payload from tools/mkpayload.py (becomes the ramdisk)
# REF_BOOT_IMG a boot.img that your bootloader already boots (e.g. the postmarketOS
#              or stock boot.img of YOUR phone). Its header version, base, offsets,
#              page size and DTB are copied; only kernel and ramdisk are replaced.
#              Needs mkbootimg's unpack_bootimg.py (AOSP, or `pip install mkbootimg`).
#              Without it the BOOT_* defaults below are used: check them against your phone.
set -euo pipefail
cd "$(dirname "$0")"
CC=${CC:-aarch64-linux-gnu-gcc}
OBJCOPY=${OBJCOPY:-aarch64-linux-gnu-objcopy}
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c fdt.c loader.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever the bootloader puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/fdt.o out/loader.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/fdt.o out/loader.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# The bootloader copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
# Ramdisk: the xnu payload if given as $1, else an empty newc cpio (diagnostics screen only).
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi

# uniLoader: it embeds blob/Image, blob/dtb and blob/ramdisk at its own build time.
mkdir -p out/uniloader-blobs
cp out/Image out/uniloader-blobs/Image
cp out/ramdisk.bin out/uniloader-blobs/ramdisk
echo "uniLoader blobs: out/uniloader-blobs/{Image,ramdisk} (use your mainline A13 DTB as blob/dtb)"

# Optional: a stand-alone Android boot.img (only if mkbootimg exists), for bootloaders that load boot.img directly.
#ref=${2:-}
#if command -v mkbootimg >/dev/null 2>&1; then
#	if [ -n "$ref" ]; then
#		rm -rf out/ref && mkdir out/ref
#		args=$(python3 "$(command -v unpack_bootimg.py || echo unpack_bootimg.py)" --boot_img "$ref" --out out/ref --format=mkbootimg)
#		args=$(echo "$args" | sed -E 's#--kernel [^ ]+##; s#--ramdisk [^ ]+##')
#		eval mkbootimg $args --kernel out/Image --ramdisk out/ramdisk.bin -o out/a135f-boot.img
#	else
#		: "${BOOT_HEADER:=2}" "${BOOT_BASE:=0x10000000}" "${BOOT_KERNEL_OFFSET:=0x8000}" \
#		  "${BOOT_RAMDISK_OFFSET:=0x1000000}" "${BOOT_TAGS_OFFSET:=0x100}" "${BOOT_PAGESIZE:=2048}"
#		mkbootimg --header_version "$BOOT_HEADER" --kernel out/Image --ramdisk out/ramdisk.bin \
#			--base "$BOOT_BASE" --kernel_offset "$BOOT_KERNEL_OFFSET" \
#			--ramdisk_offset "$BOOT_RAMDISK_OFFSET" --tags_offset "$BOOT_TAGS_OFFSET" \
#			--pagesize "$BOOT_PAGESIZE" --cmdline "console=null" -o out/a135f-boot.img
#	fi
#	cp out/a135f-boot.img out/boot.img
#	(cd out && tar -H ustar -c boot.img > AP_a135f.tar && md5sum -t AP_a135f.tar >> AP_a135f.tar && mv AP_a135f.tar AP_a135f.tar.md5)
#fi

# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/ramdisk.bin out/uniloader-blobs/*
