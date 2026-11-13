#!/bin/bash
# macOS IPSW (URL or file) -> QuartzCore's default.metallib for CoreAnimation's Metal renderer (see docs/METAL.md).
#
#   tools/get-qc-metallib.sh MACOS_IPSW_OR_URL OUT.metallib [WORKDIR]
#
# Pulls the OS disk image out of the IPSW (unzip; several GB), mounts its APFS volumes with apfs-fuse, takes
# QuartzCore.framework's default.metallib from the first volume that has one, and runs tools/qc-metallib.py on it
# (AIR slice, iOS header, aliases). Apple's file: it is written to OUT, never into the repository.
# NOT run by the author on a real macOS IPSW: if the library is not where it looks, it says so and you can point
# build.sh at one you found yourself (--qc-metallib).
# Needs: python3, unzip, curl (URL only), apfs-fuse, sudo.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
die() { echo "error: $*" >&2; exit 1; }
src=${1:-}; dst=${2:-}; work=${3:-${TMPDIR:-/tmp}/qc-metallib-work}
[[ -n $src && -n $dst ]] || { sed -n '2,12p' "$0"; exit 2; }
command -v apfs-fuse >/dev/null || die "missing apfs-fuse (https://github.com/sgan81/apfs-fuse)"
mkdir -p "$work"; work=$(realpath "$work"); dst=$(realpath -m "$dst")
if [[ -s $dst ]]; then echo "already there: $dst"; exit 0; fi
if [[ $src =~ ^(https?|ftp|file):// ]]; then
	ipsw="$work/$(basename "${src%%\?*}")"
	[[ -s $ipsw && ! -e $ipsw.part ]] || { curl -L --fail --retry 5 -C - -o "$ipsw.part" "$src"; mv "$ipsw.part" "$ipsw"; }
else
	[[ -f $src ]] || die "no such file: $src"; ipsw=$(realpath "$src")
fi
dmg_path=$(python3 - "$ipsw" <<'PY'
import plistlib, sys, zipfile
b = plistlib.loads(zipfile.ZipFile(sys.argv[1]).read('BuildManifest.plist'))
for i in b['BuildIdentities']:
    m = i.get('Manifest', {})
    if 'OS' in m and 'Info' in m['OS']:
        print(m['OS']['Info']['Path']); break
PY
)
[[ -n $dmg_path ]] || die "no OS image in the manifest: is this a macOS IPSW?"
dmg="$work/$(basename "$dmg_path")"
[[ -s $dmg ]] || { echo "extracting $dmg_path"; unzip -o -j -q "$ipsw" "$dmg_path" -d "$work"; }
mnt="$work/mnt"; mkdir -p "$mnt"
cleanup() { mountpoint -q "$mnt" && sudo umount "$mnt" 2>/dev/null || true; }
trap cleanup EXIT
found=""
for vol in 0 1 2 3 4 5; do
	cleanup
	sudo "$(command -v apfs-fuse)" -v "$vol" -o allow_other "$dmg" "$mnt" 2>/dev/null || continue
	found=$(sudo find "$mnt/root/System/Library/Frameworks/QuartzCore.framework" "$mnt/System/Library/Frameworks/QuartzCore.framework" \
		-name default.metallib -print -quit 2>/dev/null || true)
	[[ -z $found ]] || break
done
[[ -n $found ]] || die "no QuartzCore default.metallib in $dmg_path (volumes 0-5); extract one yourself and use --qc-metallib"
echo "found $found"
sudo cp "$found" "$work/macos-default.metallib"; sudo chown "$USER" "$work/macos-default.metallib"
cleanup
python3 "$here/qc-metallib.py" "$work/macos-default.metallib" "$dst"
echo "wrote $dst"
