# Build, package and test (ios-a135f)

Everything below is **untested end to end**: it follows the scripts as written, but nothing here has been run on macOS CI
or on a phone yet. Commands assume the repo root as the working directory.

## 0. Host prerequisites (Linux)

`python3`, `curl`, `gcc`, `aarch64-linux-gnu-gcc` + `binutils-aarch64-linux-gnu` (shim), `cpio`, `gzip`, the GitHub CLI `gh`
(for the kernel build) and [`ipsw`](https://github.com/blacktop/ipsw) (to extract the iPhone X device tree).
For the QEMU test also `qemu-system-aarch64` (7.x or newer) and `llvm-nm`.

## 1. Kernel (GitHub Actions, macOS runner)

```sh
git init && git add -A && git commit -m ios-a135f && gh repo create ios-a135f --public --source . --push
gh workflow run xnu.yml                       # inputs: machine_config=A135F (default), kernel_config=DEVELOPMENT
gh run watch                                  # first runs will probably fail to compile: read the log, send me the errors
gh run download <RUN_ID> -n xnu-kernel -D xnu/out
ls xnu/out/kernel.development.a135f           # the Mach-O the next steps use as $K
```
(Public repo = free macOS minutes. The 85 patches in `xnu/patches/` are applied by the workflow.)

## 2. Device tree (Apple ADT)

The iPhone X identity (`/product` node) comes from Apple's own DeviceTree; it is optional for a first boot (mkadt warns) but
userland expects it:

```sh
python3 tools/remotezip.py IPSW_URL DeviceTree.d22ap.im4p research/ipsw-x/dt      # iPhone10,3 16.7.x IPSW URL
ipsw img4 im4p extract research/ipsw-x/dt/Firmware/all_flash/DeviceTree.d22ap.im4p -o research/ipsw-x/d22ap.adt
python3 tools/mkadt.py shim/out/a135f.adt --touch-fw /path/to/td4375_a13_boe.bin   # mkdir -p shim/out first
```

## 3. Payload and shim

```sh
K=xnu/out/kernel.development.a135f
python3 tools/mkpayload.py $K shim/out/a135f.adt shim/out/payload.bin \
  --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=mmcroot0 mmcrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1'
```
Start with `cpus=1`; raise to `cpus=8` once it boots. Add `a135f_usb=0` / `a135f_touch=0` to isolate hangs.

## 4. Package for uniLoader

uniLoader embeds `blob/Image`, `blob/dtb` and `blob/ramdisk` into its own image, so the shim is the "kernel" and the XNU payload is the "ramdisk":

```sh
shim/build.sh shim/out/payload.bin          # -> shim/out/uniloader-blobs/{Image,ramdisk}
cp shim/out/uniloader-blobs/Image   /path/to/uniLoader/blob/Image
cp shim/out/uniloader-blobs/ramdisk /path/to/uniLoader/blob/ramdisk
cp /path/to/your/a13-linux.dtb      /path/to/uniLoader/blob/dtb       # the DTB you already boot Linux with
```
`Image` is the shim and `ramdisk` is the payload; then configure and build uniLoader the
way you do for Linux (arm64 board config for the A13, `PAYLOAD_ENTRY`/`RAMDISK_ENTRY` as for Linux, libfdt on) and flash
the resulting boot image exactly as you flash your Linux one. The shim needs `/chosen/linux,initrd-start/-end` in the DTB
uniLoader hands over (the same mechanism Linux uses for its initramfs). The payload must fit your `boot` partition.

(Stock Samsung bootloader instead, if `mkbootimg` is installed: `shim/build.sh shim/out/payload.bin /path/to/working/boot.img`
also makes `shim/out/a135f-boot.img` + `AP_a135f.tar.md5` for Odin/Heimdall.)

## 5. Root filesystem and flashing

```sh
tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G     # from the iPhone X 16.7.x IPSW, needs apfs-fuse + hfsprogs
python3 tools/a135f-root-tweaks.py ...                                  # as in the upstream README
```
Write `ios16-root.img` into the `userdata` partition (from recovery/postmarketOS, `dd`). **This erases userdata.**
Logs: `tools/a135f-log-pull.sh OUT.txt` reads the 4 MiB console ring the kernel keeps at the end of `userdata`.

## 6. QEMU test run (kernel only, no phone)

QEMU `virt` with a GIC-400 (`gic-version=2`) and a Cortex-A55 exercises the kernel's boot path, interrupt controller, timer
and framebuffer console. It cannot test the eMMC, touch, USB or the shim (those are replaced by the host script).

```sh
mkdir -p out/q
python3 tools/mkadt.py out/q/qemu.adt --qemu                 # QEMU addresses: GIC 0x08000000, 62.5 MHz timer, 1 GiB RAM at 0xb0000000
python3 tools/mkpayload.py $K out/q/qemu.adt out/q/payload.bin \
  --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress'
python3 tools/qemu-a135f.py out/q/payload.bin out/q/run --secs 40 --kernel $K
```
Outputs in `out/q/run/`: `fb.png` (the 1080x2408 screen), `fb.txt` (decoded console text), `regs.txt`, `qemu.log`
(exceptions with ESR/FAR/ELR), `result.json`. Without a root filesystem the expected end state is a panic about the missing root
device (`rd=` not set) *after* the kernel banner and VM/IOKit startup: that already proves the GIC-400 timer interrupt works.
Add `--fail-on-panic` only once you have a root, since a panic is then a failure. `--secs 40` is generous; use `--gdb` to attach.

Optional: boot to a user-space `init` with a RAM-disk root (needs Apple clang or `clang -target arm64-apple-ios16.0`, and `hfsprogs`):

```sh
clang -target arm64-apple-ios16.0 -c tools/a135f-init.S -o out/q/init.o
python3 tools/mkinit.py out/q/init.o out/q/launchd
truncate -s 64M out/q/root.img && mkfs.hfsplus -s -J -v qemuroot out/q/root.img
sudo mkdir -p /mnt/qroot && sudo mount -t hfsplus -o loop,force out/q/root.img /mnt/qroot
sudo mkdir -p /mnt/qroot/sbin && sudo cp out/q/launchd /mnt/qroot/sbin/launchd && sudo umount /mnt/qroot
python3 tools/mkpayload.py $K out/q/qemu.adt out/q/payload-root.bin --ramdisk out/q/root.img \
  --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=md0 rootrw=1'
python3 tools/qemu-a135f.py out/q/payload-root.bin out/q/run2 --secs 40 --kernel $K --fail-on-panic
```
The test init is a tiny static arm64 program; `fb.txt` should show its output instead of a panic.

## 7. First boot on the phone

1. Boot with `cpus=1 a135f_usb=0 a135f_touch=0` first. Photograph the screen: the shim prints entry EL, MPIDR, CNTFRQ and any panic.
2. Then enable USB (default) and touch (default), then `cpus=8`.
3. If the shim shows a blue panic screen it prints ESR/ELR/FAR: send me those lines.
