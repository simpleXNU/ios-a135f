# Building the Mali userland on Linux

Everything here cross-compiles from Linux to iOS (arm64, iOS 16). Tested on paper only for the Mesa part (see
`mesa/README-mesa.txt`); the small programs (`mali_test`, the injector) are plain C.

You build three things, in this order of effort:

| Step | Output | Needs |
|---|---|---|
| A. toolchain (once) | clang + ld64 + iOS SDK + ldid | ~15 min, ~3 GB |
| B. small programs | `out/mali_test`, `out/liba135f.inject.dylib` | step A |
| C. OpenGL ES (optional, CoreAnimation on the GPU) | `out/libOSMesa.8.dylib`, `out/liba135f.gl.dylib` | step A, a host LLVM toolchain, Mesa (unverified) |

Do A and B first; B alone is enough to prove the GPU works (`mali_test`).

## A. The toolchain (once)

Debian 12+ / Ubuntu 22.04+ (other distros: install the equivalents of the packages in the script).

```bash
cd userland
./setup-linux-toolchain.sh          # apt packages, iOS 16.5 SDK (theos/sdks), apple-libtapi, cctools-port (ld64), ldid
source ~/ios-toolchain/env.sh       # sets SDK, CC, CXX, INSTALL_NAME_TOOL, NM, PATH   (every new shell)
```

What it does, if you would rather do it by hand:

1. `sudo apt install clang lld llvm cmake ninja-build make git curl xz-utils patch pkg-config python3 python3-venv python3-mako python3-yaml bison flex libssl-dev libxml2-dev uuid-dev zlib1g-dev libedit-dev ldid`
2. `git clone --depth=1 https://github.com/theos/sdks` and copy `iPhoneOS16.5.sdk` to `~/ios-toolchain/sdk/`
3. build `tpoechtrager/apple-libtapi` (`INSTALLPREFIX=~/ios-toolchain ./build.sh && ./install.sh`), then
   `tpoechtrager/cctools-port` (`cd cctools && ./configure --prefix=~/ios-toolchain --with-libtapi=~/ios-toolchain --target=aarch64-apple-darwin && make -j && make install`)
4. `env.sh` points clang at that linker: `clang -target arm64-apple-ios16.0 -fuse-ld=~/ios-toolchain/bin/aarch64-apple-darwin-ld`

Check it works:

```bash
echo 'int main(void){return 0;}' > /tmp/t.c
$CC -isysroot $SDK /tmp/t.c -o /tmp/t && file /tmp/t     # "Mach-O 64-bit executable arm64"
```

If the linker step fails with `.tbd` / "library not found" errors, the SDK path is wrong or libtapi was not found when
cctools-port was configured. A fallback is lld: `export CC="clang -target arm64-apple-ios16.0 -fuse-ld=lld"`.

## B. The small programs

```bash
cd userland
source ~/ios-toolchain/env.sh
./build.sh                          # -> out/mali_test, out/liba135f.inject.dylib   (signed ad hoc with ldid)
file out/*                          # both Mach-O arm64
```

No Mesa needed. Then put them in the root:

```bash
tools/ios-create-root-full.sh <your usual arguments> --gl userland/out
#   or, on a root that is already mounted:
sudo python3 tools/a135f-root-tweaks.py /mnt/root --gl userland/out
```

They land in `/usr/local/bin/mali_test` and `/usr/local/lib/a135f/liba135f.inject.dylib`.

## C. OpenGL ES with Mesa panfrost (optional)

Extra host packages (for Mesa's own `mesa_clc` tool, which panfrost's built-in shaders need):

```bash
sudo apt install llvm-dev libclang-dev libclang-cpp-dev libclc-dev libllvmspirvlib-dev spirv-tools libspirv-tools-dev glslang-tools
```
(Ubuntu 24.04 versions: LLVM 18; the packages must all be the same LLVM major version.)

```bash
cd userland
source ~/ios-toolchain/env.sh
GL=1 ./build.sh
```

`build.sh GL=1` runs `mesa/mk-mesa.sh`, which:

1. downloads Mesa 25.0.7 (checked against a SHA-256) into `mesa/build/`
2. applies `mesa/patches/*.patch` and `mesa/apply-panfrost-darwin.py`. **If the script prints `MISSED`**, make that edit by hand
   (what each one is for is in the script); re-run: finished edits are recognised
3. builds `mesa_clc` for the host, then cross-builds `libOSMesa.8.dylib` (panfrost + softpipe) for iOS
4. back in `build.sh`: `mkeagl.py` generates the GL entry table, and `src/eagl.c` + `src/gl_osmesa.c` become `liba135f.gl.dylib`

Output in `userland/out/`: add `libOSMesa.8.dylib` and `liba135f.gl.dylib` to the root exactly as in B (`--gl userland/out`).

Common failures:

| Symptom | Fix |
|---|---|
| `MISSED : …pan_kmod.c: bo mmap …` | Mesa's text differs: open the file, replace the `os_mmap(… bo->dev->fd …)` call by `a135f_drm_map(bo->dev->fd, bo->handle, size)` (and `os_munmap` by `a135f_drm_unmap(0, 0, addr)`) under `#ifdef __APPLE__` |
| meson: `mesa_clc not found` | step "3." failed: read `mesa/build/meson-host.log`, `ninja-host.log` (mismatched LLVM / SPIRV packages) |
| link error `_drmIoctl` etc. | `src/a135f-drm` was not picked up: check `mesa/build/mesa-25.0.7/src/a135f-drm/` exists and the meson edit applied |
| `llvm-install-name-tool: not found` | `export INSTALL_NAME_TOOL=$HOME/ios-toolchain/bin/aarch64-apple-darwin-install_name_tool` |

## D. On the phone

Boot args: `a135f_mali=1` (kernel driver) and, for GL, `a135f_gl=1`. Then, from a shell on the device:

```bash
sysctl kern.a135f_mali                 # 6 = the GPU ran a job through its page tables
/usr/local/bin/mali_test               # exit 0, "the word: 0xa13c0de1"
```
