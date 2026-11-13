#!/bin/bash
# Build steps for the A13 iOS port: userland (Mesa + Metal), root image, payload (+ uniLoader blobs).
#
#   ./build.sh MODE... [options]
#
# Modes (one or more; at least one):
#   --userland    Mesa panfrost + the Metal driver + tools -> userland/out          (GL=1 METAL=1 userland/build.sh)
#   --root        iOS root image with userland/out installed -> OUT/ios-root.simg   (needs --ipsw)
#   --payload     ADT + payload + shim -> shim/out/uniloader-blobs/{Image,ramdisk}  (needs --kernel or --build-kernel)
#   --full        --userland --root --payload
#
# Options:
#   --kernel FILE         the XNU kernel to boot. For Metal it must have been built with xnu/patches/0031: without
#                         it the boot still works, on OpenGL ES
#   --build-kernel        build one: dispatch the xnu workflow with gh (repo = origin of this checkout; push first),
#                         wait, download kernel-development-a135f to OUT/kernel.development.a135f
#   --ipsw IPSW_OR_URL    iOS 16 IPSW for the root image
#   --qc-metallib FILE    QuartzCore's shaders, already made (tools/qc-metallib.py)
#   --macos-ipsw IPSW     ... or made here from a macOS IPSW (URL or file; tools/get-qc-metallib.sh)
#   --no-metal            OpenGL ES only (the earlier build): no Metal driver, no metallib, no a135f_metal=1
#   --recipes             also install share/a13m-recipes.txt (iOS 16.5 pipelines; try without first)
#   --cmdline 'ARGS'      added to the kernel's command line (after the defaults)
#   --touch-fw FILE       touchscreen firmware for tools/mkadt.py (default: ./td4375_a13_boe.bin)
#   --out DIR             where root images and downloads go (default: ./out)
#
# Examples:
#   ./build.sh --userland
#   ./build.sh --root --ipsw ~/iPhone10,3_16.7.16_20H392_Restore.ipsw
#   ./build.sh --payload --kernel ~/ios-a135f/kernel.development.a135f
#   ./build.sh --full --kernel K --ipsw I --macos-ipsw M
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
say() { printf '\n==== %s\n' "$*"; }
die() { echo "build.sh: $*" >&2; exit 1; }

kernel="" build_kernel=0 ipsw="" qc="" macos="" metal=1 recipes=0 extra="" touch="$ROOT/td4375_a13_boe.bin"
do_user=0 do_root=0 do_boot=0 out="$ROOT/out"
while [[ $# -gt 0 ]]; do
	case $1 in
	--userland) do_user=1; shift ;;
	--root) do_root=1; shift ;;
	--payload) do_boot=1; shift ;;
	--full) do_user=1 do_root=1 do_boot=1; shift ;;
	--kernel) kernel=$(realpath "$2"); shift 2 ;;
	--build-kernel) build_kernel=1; shift ;;
	--ipsw) ipsw=$2; shift 2 ;;
	--qc-metallib) qc=$(realpath "$2"); shift 2 ;;
	--macos-ipsw) macos=$2; shift 2 ;;
	--no-metal) metal=0; shift ;;
	--recipes) recipes=1; shift ;;
	--cmdline) extra=$2; shift 2 ;;
	--touch-fw) touch=$(realpath "$2"); shift 2 ;;
	--out) mkdir -p "$2"; out=$(realpath "$2"); shift 2 ;;
	-h | --help) sed -n '2,30p' "$0"; exit 0 ;;
	*) die "unknown option $1 (see --help)" ;;
	esac
done
[[ $((do_user + do_root + do_boot)) -gt 0 ]] || { sed -n '2,30p' "$0"; die "give a mode: --userland, --root, --payload or --full"; }
[[ $do_root == 0 || -n $ipsw ]] || die "--root needs --ipsw IPSW_OR_URL"
[[ $do_boot == 0 || -n $kernel || $build_kernel == 1 ]] || die "--payload needs --kernel FILE (or --build-kernel)"
[[ $do_root == 0 || $do_user == 1 || -d userland/out ]] || die "--root needs userland/out: run --userland first"
mkdir -p "$out"

# ------------------------------------------------------------------ kernel
if [[ $do_boot == 1 && $build_kernel == 1 ]]; then
	say "kernel: xnu workflow"
	command -v gh >/dev/null || die "--build-kernel needs gh (logged in)"
	gh workflow run xnu.yml
	sleep 15
	run=$(gh run list --workflow xnu.yml --limit 1 --json databaseId --jq '.[0].databaseId')
	gh run watch "$run" --exit-status
	rm -rf "$out/kernel-dl"; gh run download "$run" -n kernel-development-a135f -D "$out/kernel-dl"
	cp "$out/kernel-dl/kernel.development.a135f" "$out/kernel.development.a135f"
	kernel="$out/kernel.development.a135f"
fi
[[ $do_boot == 0 || -f $kernel ]] || die "no such kernel: $kernel"
if [[ $do_boot == 1 && $metal == 1 ]] && ! grep -qa "a135f_metal" "$kernel"; then
	echo "warning: no a135f_metal string in $kernel: if it was built without patch 0031 there will be no IOAcceleratorES" >&2
	echo "         and the boot falls back to OpenGL ES (rebuild the kernel, or --build-kernel; ignore this if it is compressed)" >&2
fi

# ------------------------------------------------------------------ metallib
if [[ $metal == 1 && $do_user == 1 && -z $qc ]]; then
	if [[ -s $out/QuartzCore-default.metallib ]]; then
		qc="$out/QuartzCore-default.metallib"
	elif [[ -n $macos ]]; then
		say "QuartzCore shaders from the macOS IPSW"
		tools/get-qc-metallib.sh "$macos" "$out/QuartzCore-default.metallib" "$out/qc-work"
		qc="$out/QuartzCore-default.metallib"
	else
		echo "note: no QuartzCore shaders (--qc-metallib / --macos-ipsw): the Metal driver is built, but backboardd" >&2
		echo "      stays on OpenGL ES until one is installed (docs/METAL.md)" >&2
	fi
fi

# ------------------------------------------------------------------ userland
if [[ $do_user == 1 ]]; then
	say "userland (Mesa panfrost$([[ $metal == 1 ]] && echo ' + Metal'))"
	if [[ -z ${IOS_CC:-} ]]; then
		[[ -f $HOME/ios-toolchain/env.sh ]] || (cd userland && ./setup-linux-toolchain.sh)
		# shellcheck disable=SC1091
		source "$HOME/ios-toolchain/env.sh"
	fi
	(
		cd userland
		export GL=1
		if [[ $metal == 1 ]]; then
			export METAL=1
			[[ -z $qc ]] || export QC_METALLIB=$qc
			[[ $recipes == 0 ]] || export RECIPES=1
		fi
		./build.sh
	)
fi

# ------------------------------------------------------------------ root image
if [[ $do_root == 1 ]]; then
	say "iOS root image"
	tools/ios-create-root-full.sh "$ipsw" --out "$out" --gl userland/out
fi

# ------------------------------------------------------------------ payload + shim
if [[ $do_boot == 1 ]]; then
	say "payload and shim"
	[[ -f $touch ]] || die "touchscreen firmware not found: $touch (--touch-fw)"
	cmdline='debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=mmcroot0 mmcrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 a135f_fb_noconsole=1 a135f_fb_hz=60 a135f_mali=1 a135f_gl=1'
	[[ $metal == 0 ]] || cmdline="$cmdline a135f_metal=1"
	[[ -z $extra ]] || cmdline="$cmdline $extra"
	mkdir -p shim/out
	python3 tools/mkadt.py shim/out/a135f.adt --touch-fw "$touch"
	python3 tools/mkpayload.py "$kernel" shim/out/a135f.adt shim/out/payload.bin --cmdline "$cmdline"
	shim/build.sh out/payload.bin
	echo "cmdline: $cmdline"
fi

say "done"
[[ ! -s $out/ios-root.simg ]] || echo "root:  $out/ios-root.simg   (fastboot -S 256M flash userdata ...)"
[[ ! -d shim/out/uniloader-blobs ]] || echo "boot:  shim/out/uniloader-blobs/{Image,ramdisk}   (package with your dtb for uniLoader)"
