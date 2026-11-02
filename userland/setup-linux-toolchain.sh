#!/bin/bash
# One-time: an iOS cross toolchain on Linux (clang + cctools-port's ld64 + the iOS SDK + ldid).
#   ./setup-linux-toolchain.sh            -> installs into $HOME/ios-toolchain  (IOS_TOOLCHAIN=... to change)
# Then:  source $HOME/ios-toolchain/env.sh   and run ./build.sh   (see BUILD-LINUX.md)
# Debian 12+/Ubuntu 22.04+; needs sudo only for the apt line. Takes ~15 minutes, ~3 GB.
set -e
T=${IOS_TOOLCHAIN:-$HOME/ios-toolchain}
mkdir -p "$T"/{src,bin}
cd "$T/src"

if [ "${SKIP_APT:-0}" != 1 ]; then
	sudo apt-get update
	sudo apt-get install -y clang lld llvm cmake ninja-build make git curl xz-utils patch pkg-config python3 python3-venv \
		python3-mako python3-yaml bison flex libssl-dev libxml2-dev uuid-dev zlib1g-dev libedit-dev ldid
fi

# 1. iPhoneOS16.5 sdk
[ -d sdks ] || git clone --depth=1 https://github.com/theos/sdks
mkdir -p "$T/sdk"
ls sdks | grep -q '^iPhoneOS16.5.sdk$' || { echo "setup: iPhoneOS16.5.sdk not in theos/sdks; list: $(ls sdks)"; exit 1; }
rm -rf "$T/sdk/iPhoneOS16.5.sdk"; cp -r sdks/iPhoneOS16.5.sdk "$T/sdk/"

# 2. ld64 and friends for Mach-O (tpoechtrager's cctools-port), with libtapi to read the SDK's .tbd stubs
[ -d apple-libtapi ] || git clone --depth=1 https://github.com/tpoechtrager/apple-libtapi
( cd apple-libtapi && INSTALLPREFIX="$T" ./build.sh && ./install.sh )
[ -d cctools-port ] || git clone --depth=1 https://github.com/tpoechtrager/cctools-port
( cd cctools-port/cctools && ./configure --prefix="$T" --with-libtapi="$T" --target=aarch64-apple-darwin && make -j"$(nproc)" && make install )

cat > "$T/env.sh" <<ENV
export IOS_TOOLCHAIN="$T"
export SDK="$T/sdk/iPhoneOS16.5.sdk"
export PATH="$T/bin:\$PATH"
export CC="clang -target arm64-apple-ios16.0 -fuse-ld=$T/bin/aarch64-apple-darwin-ld"
export CXX="clang++ -target arm64-apple-ios16.0 -fuse-ld=$T/bin/aarch64-apple-darwin-ld"
export INSTALL_NAME_TOOL="$T/bin/aarch64-apple-darwin-install_name_tool"
export NM=llvm-nm
ENV
echo "setup: done. Next:  source $T/env.sh"
