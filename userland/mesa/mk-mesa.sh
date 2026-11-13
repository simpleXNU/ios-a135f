#!/bin/bash
# Mesa 25.0.7 with panfrost (the Mali) and softpipe (the fallback): the OpenGL ES library behind src/eagl.c
# and, with the AIR compiler in it, what the Metal plug-in (src/a135fmetal.m) draws with.
# Fetches the release, applies mesa/patches/*.patch (in name order), puts a135f-drm (stands in for libdrm)
# and air (the AIR -> NIR compiler and the Metal device, src/compiler/air) into the tree and builds
# libOSMesa.8.dylib (install name /usr/local/lib/a135f/libOSMesa.8.dylib) into mesa/out.
#
# The patches are the set the MediaTek port's Mali-G52 (also Bifrost v7) runs on; the whole set was applied to
# the pristine 25.0.7 archive without a reject. The Mali needs no mesa_clc (Bifrost v7 has no precompiled
# shaders), so there is no host build any more.
#
# Needs: meson, ninja, bison, flex, python3 (venv; Mako, PyYAML, packaging), llvm-ar.
#
#   SDK, IOS_CC, IOS_CXX as for ../build.sh        DRIVERS=softpipe,panfrost (default)
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
B=$HERE/build
if [ -z "$SDK" ]; then
	if command -v xcrun >/dev/null 2>&1; then SDK=$(xcrun --sdk iphoneos --show-sdk-path)
	else SDK=${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk; fi
fi
CC=${IOS_CC:-clang -target arm64-apple-ios16.0}
CXX=${IOS_CXX:-clang++ -target arm64-apple-ios16.0}
DRIVERS=${DRIVERS:-softpipe,panfrost}
mkdir -p "$B/out" "$HERE/out"; cd "$B"

[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
# Patched sources are kept while the patches and our copies are what they were; otherwise from the archive again.
STAMP=$(cat "$HERE"/patches/*.patch | sha256sum | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.a135f-patches 2>/dev/null)" != "$STAMP" ]; then
	rm -rf mesa-$VERSION build-*; tar xJf mesa-$VERSION.tar.xz
	for p in "$HERE"/patches/*.patch; do patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"; done
	echo "$STAMP" > mesa-$VERSION/.a135f-patches
fi
# ours: always the current ones
rm -rf mesa-$VERSION/src/a135f-drm mesa-$VERSION/src/compiler/air
cp -r "$HERE/a135f-drm" mesa-$VERSION/src/a135f-drm
cp -r "$HERE/air" mesa-$VERSION/src/compiler/air

if [ ! -x venv/bin/python3 ]; then python3 -m venv venv; venv/bin/pip -q install mako pyyaml packaging; fi
export PATH=$B/venv/bin:$PATH

# the library for the phone
cat > cross.ini <<INI
[binaries]
objc = [$(printf "'%s', " $CC | sed 's/, $//'), '-isysroot', '$SDK']
c = [$(printf "'%s', " $CC | sed 's/, $//'), '-isysroot', '$SDK']
cpp = [$(printf "'%s', " $CXX | sed 's/, $//'), '-isysroot', '$SDK', '-fuse-cxa-atexit']
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
BUILD=build-${DRIVERS//,/-}
OPT=
case "$DRIVERS" in *panfrost*) OPT=-Doptimization=s ;; esac
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/usr/local --libdir=lib/a135f --buildtype=release $OPT -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C $BUILD > ninja.log
# air_run draws with a translated Metal shader and reads the pixels back: a test program, built only when asked
if [ "${AIR:-}" = 1 ]; then
	ninja -C $BUILD src/gallium/drivers/panfrost/air_run >> ninja.log
	cp $BUILD/src/gallium/drivers/panfrost/air_run $HERE/out/air_run
fi

O=$HERE/out
llvm-strip -x -o $O/libOSMesa.8.dylib $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
${INSTALL_NAME_TOOL:-llvm-install-name-tool} -id /usr/local/lib/a135f/libOSMesa.8.dylib $O/libOSMesa.8.dylib
${NM:-llvm-nm} -gU $O/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > $O/exported-gl.txt
cp mesa-$VERSION/src/mapi/glapi/registry/gl.xml $O/gl.xml
cat "$HERE"/patches/*.patch "$HERE"/a135f-drm/* "$HERE"/air/* | sha256sum > $O/built-from
echo "mk-mesa.sh: Mesa $VERSION ($DRIVERS): $O/libOSMesa.8.dylib, $(stat -c %s $O/libOSMesa.8.dylib) bytes, $(wc -l < $O/exported-gl.txt) gl* functions"
