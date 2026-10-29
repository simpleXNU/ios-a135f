# ios-a135f

**iOS 16 on a Samsung Galaxy A13 (SM-A135F) **

## Hardware status

| Area | A13 (Exynos850) | State |
| --- | --- | --- |
| Cores | 8x Cortex-A55; MPIDR `0x000-3`, `0x100-3`; PSCI | Working |
| Interrupts | GIC-400 = **GICv2**; timer INTID 27; MMIO `GICC_IAR` / `GICC_EOIR`; IPIs via `GICD_SGIR` | Working |
| Timer | 26 MHz (ADT `timebase-frequency`) | Working |
| RAM | 2 GiB block at `0x880000000` | Working (todo: 3gb/4gb) |
| Display | Simple framebuffer, 1080x2408 at `0xfa000000` | Working |
| Storage | Exynos DWMMC driver; root `rd=mmcroot0` | Working |
| Boot | SBOOT / uniLoader; shim flashed to `boot`, including EL2-to-EL1 transition; preferably via Fastboot | Working |
| Keys | Volume up/down on GPIO `gpa0-7` / `gpa1-0`, both pressed together is power | Working |
| power key | power button on PMIC interrupts | not implemented |
| Fuelgauge | Maxim Max1726x | Not implemented |
| Touch | OVT TD4150 firmware embedded in ADT via `mkadt.py --touch-fw`, seems like theres an issue there though. | WIP |
| USB | DWC3 USB | Working |
| Display Enable | `DECON_F` (`0x13060000`) + `0x70 = 0x1281`; written by shim and kernel | Working |
| GPU | Mali-G52 | Not implemented |
| Audio | ABOX audio | Not implemented |
| Wi-Fi | Samsung S612 | Not implemented |
| Bluetooth | Part of Wi-Fi | Not implemented |
| Modem | Samsung Shannon S5000AP | Not implemented |
| Camera | Samsung Isocell JN1 (S5KJN1) | not implemented |
| Camera | GalaxyCore GC02M1 | Not implemented |
| Camera | GalaxyCore GC08A3 | Not implemented |
| Camera | GalaxyCore GC5035 | Not implemented |
| Camera | GalaxyCore GC02M1_MACRO | Not implemented |
| Main PMIC | Samsung S2MPU12 | Not Implemented |
| Sub PMIC | S2MPB03 | Not implemented |

## How to use it

1. **Kernel:** fork the repo and run the `xnu` workflow (`gh workflow run xnu.yml -R YOU/ios-a135f`), then
   `gh run download RUN_ID -n kernel-development-a135f -D xnu/out`. Expect compile errors on the first runs (`keep_going` collects them all).
3. **Root image:** as upstream: `tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G`, then
   `tools/a135f-root-tweaks.py`. Needs the iPhone X (iPhone10,3) 16.7.16 IPSW from Apple.
for the latest ios 16.7.16 ipsw (iphone x) off ipsw.me:
```
tools/ios-create-root-full.sh <url_to_ipsw or path/to/ipsw>
```


# Todo: REWRITE
4. **Payload and blobs:**
   ```sh
   python3 tools/mkadt.py shim/out/a135f.adt --touch-fw /path/to/td4375_a13_boe.bin   # vendor TCM image you load on Linux
   python3 tools/mkpayload.py xnu/out/kernel.development.a135f shim/out/a135f.adt shim/out/payload.bin \
       --cmdline 'debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=mmcroot0 mmcrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 a135f_fb_noconsole=1 a135f_fb_hz=30'
   shim/build.sh shim/out/payload.bin      # needs aarch64-linux-gnu-gcc/binutils; -> shim/out/uniloader-blobs/{Image,ramdisk}
   ```
5. **uniLoader:** copy `shim/out/uniloader-blobs/Image` to `uniLoader/blob/Image`, `.../ramdisk` to `blob/ramdisk`, and your usual mainline A13 DTB to
   `blob/dtb`; build uniLoader exactly as for Linux (same board config, `PAYLOAD_ENTRY`/`RAMDISK_ENTRY` as for your Linux build) and flash the
   result to `boot`. Write the iOS root image to `userdata`. (If you have `mkbootimg`, `shim/build.sh payload.bin [ref-boot.img]` also writes a
   stand-alone `a135f-boot.img` and an Odin `AP_a135f.tar.md5`.)
6. **Logs:** the kernel keeps a 4 MiB console ring at the end of `userdata`; read it from recovery/pmOS with
   `tools/a135f-log-pull.sh`. The shim also paints a status/panic screen on the framebuffer.

## QEMU Usage
```
export K=/path/to/kernel
mkdir -p out/q
python3 tools/mkadt.py out/q/qemu.adt --qemu
python3 tools/mkpayload.py $K out/q/qemu.adt out/q/payload.bin --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress'
python3 tools/qemu-a135f.py out/q/payload.bin out/q/run --secs 40 --kernel $K
```

## How to boot it

Use uniLoader to boot. Package the Image under blobs/Image, dtb under blobs/dtb and ramdisk under blobs/ramdisk.

See `docs/PORTING-NOTES.md` for details and the remaining TODO list.
