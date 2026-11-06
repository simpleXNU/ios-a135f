#!/bin/bash
# One-time: an iOS cross toolchain on Linux.  Fast path (default, ~1 minute): clang + lld + the iOS SDK.
#   ./setup-linux-toolchain.sh               -> installs into $HOME/ios-toolchain  (IOS_TOOLCHAIN=... to change)
#   USE_CCTOOLS=1 ./setup-linux-toolchain.sh -> also builds tpoechtrager's cctools-port ld64 (~15 min), used instead of lld
#   SKIP_APT=1 ...                           -> do not run apt
#   LDID=/path/to/ldid                       -> your ldid (default $HOME/ios-a135f/binaries/ldid)
# Then:  source $HOME/ios-toolchain/env.sh   and run ./build.sh   (see BUILD-LINUX.md)
set -e
T=${IOS_TOOLCHAIN:-$HOME/ios-toolchain}
mkdir -p "$T"/{src,bin,sdk}
cd "$T/src"

if [ "${SKIP_APT:-0}" != 1 ]; then
	PKGS="clang lld llvm git make curl xz-utils patch pkg-config python3 python3-venv python3-mako python3-yaml bison flex"
	[ "${USE_CCTOOLS:-0}" = 1 ] && PKGS="$PKGS cmake ninja-build libssl-dev libxml2-dev uuid-dev zlib1g-dev libedit-dev libblocksruntime-dev libdispatch-dev"
	sudo apt-get update
	sudo apt-get install -y $PKGS
fi

# the iOS SDK the HakiOS userland is built against
[ -d sdks ] || git clone --depth=1 https://github.com/theos/sdks
[ -d sdks/iPhoneOS16.5.sdk ] || { echo "setup: iPhoneOS16.5.sdk not in theos/sdks; have: $(ls sdks)"; exit 1; }
rm -rf "$T/sdk/iPhoneOS16.5.sdk"; cp -r sdks/iPhoneOS16.5.sdk "$T/sdk/"

LD="-fuse-ld=lld"
INT=llvm-install-name-tool
if [ "${USE_CCTOOLS:-0}" = 1 ]; then
	[ -d apple-libtapi ] || git clone --depth=1 https://github.com/tpoechtrager/apple-libtapi
	( cd apple-libtapi && INSTALLPREFIX="$T" ./build.sh && ./install.sh )
	[ -d cctools-port ] || git clone --depth=1 https://github.com/tpoechtrager/cctools-port
	( cd cctools-port/cctools && ./configure --prefix="$T" --with-libtapi="$T" --target=aarch64-apple-darwin && make -j"$(nproc)" && make install )
	LD="-fuse-ld=$T/bin/aarch64-apple-darwin-ld"
	INT="$T/bin/aarch64-apple-darwin-install_name_tool"
fi

cat > "$T/env.sh" <<ENV
export IOS_TOOLCHAIN="$T"
export SDK="$T/sdk/iPhoneOS16.5.sdk"
export PATH="$T/bin:\$PATH"
export IOS_CC="clang -target arm64-apple-ios16.0 $LD"
export IOS_CXX="clang++ -target arm64-apple-ios16.0 $LD"
export INSTALL_NAME_TOOL="$INT"
export NM=llvm-nm
export LDID="${LDID:-\$HOME/ios-a135f/binaries/ldid}"
ENV
echo "setup: done. Next:  source $T/env.sh"
