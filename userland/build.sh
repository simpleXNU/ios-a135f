#!/bin/bash
# Build the Mali userland for the Galaxy A13's iOS root.   userland/build.sh [GL=1]
#
# Always: out/mali_test, out/native_mali_test and out/liba135f.inject.dylib (small; need only clang and an iOS SDK).
# GL=1 also: Mesa (panfrost + softpipe, mesa/mk-mesa.sh), out/libOSMesa.8.dylib and out/liba135f.gl.dylib
# -- the OpenGL ES library CoreAnimation draws with once the injector has put it behind EAGLContext.
#
# METAL=1 (wants GL=1; Mesa then carries the AIR compiler, mesa/air) also: out/A135FMetal.bundle (the Metal driver:
# src/a135fmetal.m over src/metal), out/metal_props (names the bundle on IOAcceleratorES, see docs/METAL.md),
# out/metal_drv_test and out/metal_test, and out/liba135f.air.dylib (a link to libOSMesa: the driver's compiler
# lives inside it on the Mali), out/share/ (QC_METALLIB=FILE, RECIPES=1: see below). Needs the kernel's patch 0031 and boot-arg a135f_metal=1 to be of any use.
#
#   SDK=/path/iPhoneOS16.5.sdk   (default $THEOS/sdks/iPhoneOS16.5.sdk, or `xcrun --sdk iphoneos --show-sdk-path` on a Mac)
#   IOS_CC="clang -target arm64-apple-ios16.0"   (default; on a Mac `xcrun -sdk iphoneos clang -arch arm64` works too)
#   LDID=ldid                    signs the outputs ad hoc when present (the AMFI stand-in only needs a signature to exist)
set -e
cd "$(dirname "$(readlink -f "$0")")"
if [ -z "$SDK" ]; then
	if command -v xcrun >/dev/null 2>&1; then SDK=$(xcrun --sdk iphoneos --show-sdk-path)
	else SDK=${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk; fi
fi
[ -d "$SDK" ] || { echo "build.sh: no iOS SDK at $SDK (set SDK=)"; exit 1; }
CXX=${IOS_CXX:-clang++ -target arm64-apple-ios16.0}
CC=${IOS_CC:-clang -target arm64-apple-ios16.0}   # on Linux: source ~/ios-toolchain/env.sh first (BUILD-LINUX.md)
DYN="$CC -isysroot $SDK -O2 -Wall -Wno-unused-function"
LIBDIR=/usr/local/lib/a135f
mkdir -p out obj
sign() { command -v ${LDID:-ldid} >/dev/null 2>&1 && ${LDID:-ldid} -S "$@" || echo "build.sh: no ldid: $* left unsigned"; }

$DYN -Isrc -framework IOKit -framework CoreFoundation -o out/mali_test src/mali_test.c
$DYN -Isrc -framework IOKit -framework CoreFoundation -o out/native_mali_test src/native_mali_test.c
$DYN -dynamiclib -install_name $LIBDIR/liba135f.inject.dylib -framework IOKit -framework CoreFoundation -lobjc \
	-o out/liba135f.inject.dylib src/a135f_inject.c
sign out/mali_test; sign out/native_mali_test; sign out/liba135f.inject.dylib

if [ "$GL" = 1 ]; then
	M=mesa/out
	[ -f $M/libOSMesa.8.dylib ] || ./mesa/mk-mesa.sh
	./mkeagl.py src/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/eagl.c > obj/eagl_gen.h
	$DYN -dynamiclib -install_name $LIBDIR/liba135f.gl.dylib -Iobj -Isrc -framework CoreFoundation \
		-framework IOKit -framework IOSurface -lobjc $M/libOSMesa.8.dylib -o out/liba135f.gl.dylib \
		src/eagl.c src/gl_osmesa.c
	cp $M/libOSMesa.8.dylib out/
	sign out/liba135f.gl.dylib; sign out/libOSMesa.8.dylib
fi
if [ "$METAL" = 1 ]; then
	[ "$GL" = 1 ] || { echo "build.sh: METAL=1 wants GL=1 (the driver's compiler is inside libOSMesa)"; exit 1; }
	# the driver's core is C++ (src/metal: the compiler library's calls, typed by mesa/air/air_metal.h itself,
	# buffers, what the driver says), the plug-in over it Metal's classes in Objective-C (src/metal/a13m.h is the
	# line between them). Bundle: <MetalPluginName>.bundle, which Metal's discovery opens from /System/Library/Extensions
	DXX="$CXX -isysroot $SDK -Os -std=c++20 -fno-exceptions -fno-rtti -Imesa/air"
	OBJS=
	mkdir -p obj/metal out/A135FMetal.bundle
	for f in air buffer log texture blit draw command recipes import perf plib shape argenc run worker surfaces render compute cmdbuf pipeline drawable; do
		$DXX -c src/metal/$f.cpp -o obj/metal/$f.o
		OBJS="$OBJS obj/metal/$f.o"
	done
	$DYN -Os -bundle -framework Foundation -framework IOKit -framework IOSurface -framework Metal -lobjc -lc++ \
		-Imesa/air -Isrc -o out/A135FMetal.bundle/A135FMetal src/a135fmetal.m $OBJS
	cp src/metal_plugin.plist out/A135FMetal.bundle/Info.plist
	$DYN -framework CoreFoundation -framework IOKit -o out/metal_props src/metal_props.c
	$DYN -framework Foundation -framework IOKit -framework Metal -lobjc -o out/metal_drv_test src/metal_drv_test.m
	$DYN -fobjc-arc -Wno-deprecated-declarations -framework Foundation -framework CoreGraphics -framework CoreImage \
		-framework IOKit -framework Metal -framework OpenGLES -lMobileGestalt -o out/metal_test src/metal_test.m
	ln -sf libOSMesa.8.dylib out/liba135f.air.dylib
	# QC_METALLIB=FILE: QuartzCore's shaders in AIR for CoreAnimation's Metal renderer (src/a135f_inject.c): the AIR
	# slice of a macOS QuartzCore's default.metallib made for iOS by tools/qc-metallib.py. Apple's file: not kept here.
	rm -rf out/share; mkdir -p out/share
	[ -z "$QC_METALLIB" ] || cp "$QC_METALLIB" out/share/QuartzCore-default.metallib
	# RECIPES=1: the pipelines CoreAnimation needs, built by backboardd at start (share/a13m-recipes.txt, made from a
	# run's log by tools/a13m-recipes.py). Taken from iOS 16.5's QuartzCore: try without it on a first boot.
	[ "$RECIPES" != 1 ] || cp share/a13m-recipes.txt out/share/
	sign out/A135FMetal.bundle/A135FMetal; sign out/metal_props; sign out/metal_drv_test; sign out/metal_test
fi
echo "build.sh: done:"; ls -lR out
