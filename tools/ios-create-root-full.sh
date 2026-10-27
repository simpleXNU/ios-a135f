#!/bin/bash
# IPSW (URL or local file) -> tweaked iOS root -> raw image + fastboot-ready sparse image.
#
#   tools/ios-create-root-full.sh IPSW_URL_OR_FILE [options]
#
# Steps (each one is skipped if its output already exists, so a failed run can simply be repeated):
#   1. fetch      download the IPSW with curl (resumable) if the argument is a URL
#   2. parse      read BuildManifest.plist: the OS root image and the SystemOS/AppOS cryptex images
#   3. extract    `ipsw extract` those three .dmg files (plain unzip if ipsw is missing or fails)
#   4. build      tools/mkiosroot.sh: HFS+ (journaled, case-sensitive) root image, cryptexes inside
#   5. tweak      mount it and run tools/a135f-root-tweaks.py, then fsck
#   6. sparse     img2simg -> ios-root.simg (this is what you flash: ~5 minutes instead of ~40 for dd)
#
# Options:
#   --out DIR        output directory (default: ./ios-root-out); files: ios-root.img, ios-root.simg, *.sha256
#   --work DIR       scratch directory for the IPSW and extracted DMGs (default: OUT/work)
#   --size SIZE      HFS+ image size (default 16G); must be at least 4 MiB below the userdata partition
#   --copy-to DIR    also copy ios-root.simg (and its checksum) to DIR, e.g. /mnt/c/tmp for Windows fastboot
#   --keep-dmgs      keep the extracted .dmg files and the IPSW after a successful run
#   --no-raw         delete the raw ios-root.img after the sparse image has been made
#   --verify         check that the sparse image expands back to exactly the raw image (slow)
#   --extract-only   stop after step 3 (useful to check the parsing)
#   --no-ipsw        do not use the `ipsw` tool, extract with unzip
#
# Needs: python3, curl (URL only), unzip, sudo, apfs-fuse, mkfs.hfsplus/fsck.hfsplus (hfsprogs), rsync,
#        img2simg (android-sdk-libsparse-utils); `ipsw` (github.com/blacktop/ipsw) is optional but used when present.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
say() { printf '\n== %s\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }

src=""
out="./ios-root-out"
work=""
size="16G"
copy_to=""
keep_dmgs=0 no_raw=0 verify=0 extract_only=0 use_ipsw=1
while [[ $# -gt 0 ]]; do
	case $1 in
	--out) out=$2; shift 2 ;;
	--work) work=$2; shift 2 ;;
	--size) size=$2; shift 2 ;;
	--copy-to) copy_to=$2; shift 2 ;;
	--keep-dmgs) keep_dmgs=1; shift ;;
	--no-raw) no_raw=1; shift ;;
	--verify) verify=1; shift ;;
	--extract-only) extract_only=1; shift ;;
	--no-ipsw) use_ipsw=0; shift ;;
	-h | --help) sed -n '2,29p' "$0"; exit 0 ;;
	-*) die "unknown option $1 (see --help)" ;;
	*) [[ -z $src ]] || die "only one IPSW argument allowed"; src=$1; shift ;;
	esac
done
[[ -n $src ]] || { sed -n '2,29p' "$0"; exit 2; }

mkdir -p "$out"
out=$(realpath "$out")
work=$(realpath -m "${work:-$out/work}")
mkdir -p "$work"
img="$out/ios-root.img"
simg="$out/ios-root.simg"

need() { command -v "$1" >/dev/null 2>&1 || die "missing tool: $1 ($2)"; }
need python3 "apt install python3"
need unzip "apt install unzip"
if [[ $extract_only == 0 ]]; then
	need sudo "needed to mount the images"
	need apfs-fuse "https://github.com/sgan81/apfs-fuse"
	need mkfs.hfsplus "apt install hfsprogs"
	need fsck.hfsplus "apt install hfsprogs"
	need rsync "apt install rsync"
	need img2simg "apt install android-sdk-libsparse-utils"
fi
if [[ $use_ipsw == 1 ]] && ! command -v ipsw >/dev/null 2>&1; then
	echo "note: 'ipsw' not found, extracting with unzip"
	use_ipsw=0
fi

# Rough space check: IPSW (~6-7 GB) + DMGs (~7 GB) + raw image + sparse image.
need_gb=$(( ${size%G} + 28 ))
avail_gb=$(( $(df --output=avail -B1 "$out" | tail -1) / 1000000000 ))
if (( avail_gb < need_gb )); then
	echo "warning: about ${need_gb} GB free space are recommended in $out, only ${avail_gb} GB available" >&2
fi

# ---------------------------------------------------------------- 1. fetch
say "1/6 IPSW"
if [[ $src =~ ^(https?|ftp|file):// ]]; then
	need curl "apt install curl"
	ipsw_file="$work/$(basename "${src%%\?*}")"
	if [[ -s $ipsw_file && ! -e $ipsw_file.part ]]; then
		echo "already downloaded: $ipsw_file"
	else
		echo "downloading $src"
		curl -L --fail --retry 5 -C - -o "$ipsw_file.part" "$src"
		mv "$ipsw_file.part" "$ipsw_file"
	fi
else
	[[ -f $src ]] || die "no such file: $src"
	ipsw_file=$(realpath "$src")
	echo "using local file $ipsw_file"
fi
unzip -tq "$ipsw_file" BuildManifest.plist >/dev/null 2>&1 || die "$ipsw_file does not look like an IPSW (no BuildManifest.plist)"

# ---------------------------------------------------------------- 2. parse
say "2/6 BuildManifest"
mapfile -t info < <(python3 - "$ipsw_file" <<'PY'
import plistlib, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
b = plistlib.loads(z.read('BuildManifest.plist'))
ids = b['BuildIdentities']
pick = None
for i in ids:
    inf = i.get('Info', {})
    if inf.get('RestoreBehavior') == 'Erase' and str(inf.get('Variant', '')).startswith('Customer Erase'):
        pick = i
        break
pick = pick or ids[0]
m = pick['Manifest']
def path(k):
    return m[k]['Info']['Path'] if k in m and 'Info' in m[k] else ''
print(path('OS'))
print(path('Cryptex1,SystemOS'))
print(path('Cryptex1,AppOS'))
print('%s (%s) for %s, identity %s' % (b.get('ProductVersion', '?'), b.get('ProductBuildVersion', '?'),
      ','.join(b.get('SupportedProductTypes', [])), pick.get('Info', {}).get('Variant', '?')))
PY
)
os_path=${info[0]:-} sysos_path=${info[1]:-} appos_path=${info[2]:-}
echo "iOS ${info[3]:-unknown}"
echo "  OS      : ${os_path:-MISSING}"
echo "  SystemOS: ${sysos_path:-MISSING}"
echo "  AppOS   : ${appos_path:-MISSING}"
[[ -n $os_path && -n $sysos_path && -n $appos_path ]] ||
	die "the manifest has no OS/SystemOS/AppOS images: this IPSW is not an iOS 16 build for the iPhone X (iPhone10,3 / 10,6)"

# ---------------------------------------------------------------- 3. extract
say "3/6 extract"
dmgdir="$work/dmg"
mkdir -p "$dmgdir"
have_all() { local p; for p in "$os_path" "$sysos_path" "$appos_path"; do [[ -s $dmgdir/$(basename "$p") ]] || return 1; done; }
if have_all; then
	echo "already extracted in $dmgdir"
else
	if [[ $use_ipsw == 1 ]]; then
		pat="^($(printf '%s|' "$os_path" "$sysos_path" "$appos_path" | sed 's/|$//; s/\./\\./g'))\$"
		echo "ipsw extract --pattern '$pat'"
		tmp="$work/ipsw-extract"
		rm -rf "$tmp"; mkdir -p "$tmp"
		if ipsw extract "$ipsw_file" --pattern "$pat" --output "$tmp" 2>&1 | tail -n 12; then
			# ipsw puts the files in a subdirectory named after the build; find them wherever they landed.
			for p in "$os_path" "$sysos_path" "$appos_path"; do
				f=$(find "$tmp" -type f -name "$(basename "$p")" -print -quit)
				[[ -n $f ]] && mv -f "$f" "$dmgdir/"
			done
		fi
		rm -rf "$tmp"
	fi
	if ! have_all; then
		[[ $use_ipsw == 0 ]] || echo "ipsw did not produce all three images: falling back to unzip"
		for p in "$os_path" "$sysos_path" "$appos_path"; do
			[[ -s $dmgdir/$(basename "$p") ]] || { echo "unzip $p"; unzip -o -j -q "$ipsw_file" "$p" -d "$dmgdir"; }
		done
	fi
	have_all || die "extraction failed"
fi
os_dmg="$dmgdir/$(basename "$os_path")"
sysos_dmg="$dmgdir/$(basename "$sysos_path")"
appos_dmg="$dmgdir/$(basename "$appos_path")"
ls -lh "$os_dmg" "$sysos_dmg" "$appos_dmg" | awk '{print "  " $5 "  " $9}'
[[ $extract_only == 0 ]] || { echo "--extract-only: stopping here"; exit 0; }

# ---------------------------------------------------------------- 4+5. build and tweak
say "4/6 build HFS+ root ($size)"
sudo -v
if [[ -e $img && -e $img.tweaked ]]; then
	echo "already built and tweaked: $img"
else
	if [[ -e $img ]]; then
		echo "removing the unfinished $img from an earlier run"
		rm -f "$img"
	fi
	"$here/mkiosroot.sh" "$os_dmg" "$sysos_dmg" "$appos_dmg" "$img" "$size"

	say "5/6 root tweaks"
	mnt=$(mktemp -d)
	cleanup_mnt() { mountpoint -q "$mnt" && sudo umount "$mnt" || true; rmdir "$mnt" 2>/dev/null || true; }
	trap cleanup_mnt EXIT
	sudo mount -t hfsplus -o loop,force,rw "$img" "$mnt"
	sudo python3 "$here/a135f-root-tweaks.py" "$mnt"
	sync
	sudo umount "$mnt"
	rmdir "$mnt"
	trap - EXIT
	fsck.hfsplus -f -y "$img" | grep -v 'Illegal name' || true
	fsck.hfsplus -f -n "$img"
	touch "$img.tweaked"
fi

# ---------------------------------------------------------------- 6. sparse image
say "6/6 sparse image for fastboot"
raw_bytes=$(stat -c %s "$img")
echo "raw image: $(numfmt --to=iec "$raw_bytes")  (keep ~4 MiB spare for logring)"
if (( raw_bytes > (111680 - 4) * 1024 * 1024 )); then
	die "the image is larger than the userdata partition minus the 4 MiB log ring: use a smaller --size"
fi
rm -f "$simg"
img2simg "$img" "$simg"
if [[ $verify == 1 ]]; then
	need simg2img "apt install android-sdk-libsparse-utils"
	echo "verifying (expands the sparse image into $work/verify.raw and compares; needs another $size) ..."
	simg2img "$simg" "$work/verify.raw"
	cmp "$work/verify.raw" "$img" && echo "sparse image matches the raw image"
	rm -f "$work/verify.raw"
fi
(cd "$out" && sha256sum ios-root.simg > ios-root.simg.sha256 && { [[ $no_raw == 1 ]] || sha256sum ios-root.img > ios-root.img.sha256; })
[[ $no_raw == 0 ]] || rm -f "$img" "$img.tweaked"
if [[ -n $copy_to ]]; then
	mkdir -p "$copy_to"
	cp -v "$simg" "$simg.sha256" "$copy_to/"
fi
if [[ $keep_dmgs == 0 ]]; then
	rm -f "$os_dmg" "$sysos_dmg" "$appos_dmg"
	[[ $src =~ ^(https?|ftp|file):// ]] && rm -f "$ipsw_file"
fi

# ---------------------------------------------------------------- done
flash_src=${copy_to:+$(realpath "$copy_to")/}${copy_to:-$out/}
say "done"
ls -lh "$simg" | awk '{print $5 "  " $9}'
echo
echo "Flash (bootloader/fastboot mode on the phone, fastboot on the machine that sees the USB port):"
if command -v wslpath >/dev/null 2>&1; then
	win=$(wslpath -w "${flash_src}ios-root.simg" 2>/dev/null || true)
	echo "  Windows (PowerShell):  fastboot -S 256M flash userdata \"$win\""
	echo "  (a path under \\\\wsl.localhost can be slow: use --copy-to /mnt/c/tmp and flash C:\\tmp\\ios-root.simg)"
fi
echo "  Linux:                 fastboot -S 256M flash userdata \"${flash_src}ios-root.simg\""
echo "non sparse images can not be handled by fastboot, so flash the .simg"
