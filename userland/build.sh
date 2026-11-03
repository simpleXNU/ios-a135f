#!/bin/bash
# Build the Mali userland for the Galaxy A13's iOS root.   userland/build.sh [GL=1]
#
# Always: out/mali_test and out/liba135f.inject.dylib (small; need only clang and an iOS SDK).
# GL=1 also: Mesa (panfrost + softpipe, mesa/mk-mesa.sh), out/libOSMesa.8.dylib and out/liba135f.gl.dylib
# -- the OpenGL ES library CoreAnimation draws with once the injector has put it behind EAGLContext.
#
#   SDK=/path/iPhoneOS16.5.sdk   (default $THEOS/sdks/iPhoneOS16.5.sdk, or `xcrun --sdk iphoneos --show-sdk-path` on a Mac)
#   CC="clang -target arm64-apple-ios16.0"   (default; on a Mac `xcrun -sdk iphoneos clang -arch arm64` works too)
#   LDID=ldid                    signs the outputs ad hoc when present (the AMFI stand-in only needs a signature to exist)

source /home/schoosh/ios-toolchain/env.sh

set -e
cd "$(dirname "$(readlink -f "$0")")"
if [ -z "$SDK" ]; then
	if command -v xcrun >/dev/null 2>&1; then SDK=$(xcrun --sdk iphoneos --show-sdk-path)
	else SDK=${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk; fi
fi
[ -d "$SDK" ] || { echo "build.sh: no iOS SDK at $SDK (set SDK=)"; exit 1; }
CC=${CC:-clang -target arm64-apple-ios16.0}   # on Linux: source ~/ios-toolchain/env.sh first (BUILD-LINUX.md)
DYN="$CC -isysroot $SDK -O2 -Wall -Wno-unused-function"
LIBDIR=/usr/local/lib/a135f
mkdir -p out obj
sign() { command -v ${LDID:-ldid} >/dev/null 2>&1 && ${LDID:-ldid} -S "$@" || echo "build.sh: no ldid: $* left unsigned"; }

$DYN -Isrc -framework IOKit -framework CoreFoundation -o out/mali_test src/mali_test.c
$DYN -dynamiclib -install_name $LIBDIR/liba135f.inject.dylib -framework IOKit -framework CoreFoundation \
	-o out/liba135f.inject.dylib src/a135f_inject.c
sign out/mali_test; sign out/liba135f.inject.dylib

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
echo "build.sh: done:"; ls -l out
