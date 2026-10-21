# ios-a135f

**Apple's iOS 16 userland on a Samsung Galaxy A13 (SM-A135F, Exynos850) — port in progress.**

This is a port of the [ios-sweet](https://github.com/dipa-studio/ios-sweet) project (Apple's open-source XNU `xnu-8796`
on a Redmi Note 10 Pro, running the unmodified iPhone X iOS 16.7.16 userland) to the Galaxy A13. All credit for the
original design, the 81 XNU patches, the shim and the tools goes to that project; this tree renames it and replaces the
SoC-specific parts. The original engineering log is kept in `docs/UPSTREAM-sweet-NOTES.md`.

> **Status: written, NOT built, NOT booted.** No part of this port has been compiled on a macOS runner or run on a
> phone yet. Treat it as a careful first draft that will need a few CI/boot iterations. It installs into `userdata`
> and **erases Android/postmarketOS user data**, and booting it means flashing the `boot` partition. Back up first.

## What was ported

| Area | sweet (SM7150) | A13 (Exynos850) | State |
| --- | --- | --- | --- |
| Cores | 6x A55 via PSCI (A76s unused) | 8x Cortex-A55, MPIDR `0x000-3`, `0x100-3`, PSCI | ported (patch 0082) |
| Interrupts | GICv3, timer INTID 19, `ICC_*` sysregs | GIC-400 = **GICv2**, timer INTID 27, MMIO `GICC_IAR/EOIR`, IPIs via `GICD_SGIR` | ported (0082) |
| Timer | 19.2 MHz | 26 MHz (ADT `timebase-frequency`) | ported |
| RAM | upper DDR bank at `0x100000000` | 2 GiB block at `0x880000000` | ported (`mkadt.py`) |
| Display | ABL splash 1080x2400 @ `0x9c000000` | simple-framebuffer 1080x2408 @ `0xfa000000`, same CPU-copy path, shown centered in the 1125x2436 iPhone X canvas | ported (0082) |
| Storage | own UFS host driver | own **DW-MSHC eMMC** driver (polled, IDMAC, reuses the bootloader's trained bus), root `rd=mmcroot0` | written, untested (0083) |
| Boot | ABL + `fastboot boot` | Samsung bootloader: shim is **flashed to `boot`** (EL2->EL1 drop included) | ported (`shim/`) |
| Keys | PMIC via SPMI | volume up/down on GPIO `gpa0-7`/`gpa1-0`; **both together = Power** | written (0084) |
| Battery | PM6150 SPMI | static 100 % fallback (MAX1726x/S2MPU12 drivers not written) | stub |
| Touch | Goodix over GENI I2C | TD4150: polled S3C64xx SPI (USI_CMGP0) + TCM client with zeroflash firmware download, firmware embedded in the ADT by `mkadt.py --touch-fw` | written, untested (0085) |
| USB | Qualcomm DWC3 + QUSB2 | CMU_HSI gates + PMU isolation + Exynos850 UTMI PHY init (from mainline `phy-exynos5-usbdrd.c`), then the same DWC3 CDC-ECM/log gadget | written, untested (0085) |
| Display enable | ABL splash | `DECON_F (0x13060000) + 0x70 = 0x1281`, written by the shim and again by the kernel | added (shim, 0085) |
| GPU, audio, Wi-Fi, BT, modem, camera, PMIC | none | none | not planned |

Generic parts (AMFI/entitlement stubs, HFS+, mockfs, IOSurface/AppleCLCD, HID event services, crash/log tooling,
`mkiosroot.sh`, `a135f-root-tweaks.py`) are unchanged apart from renaming.

## Rebrand

`sweet` -> `a135f`, `SWEET` -> `A135F` everywhere (machine config `A135F`, `config/MASTER.arm64.a135f`,
`pexpert/pexpert/arm64/A135F.h`, `A135F*.cpp` IOKit classes, boot-args `a135f_fb_hz`, `a135f_fb_noconsole`, `a135f_keys`,
`a135f_touch`, `a135f_usb`, ADT nodes `a135f-fbcon` / `a135f-mmc`, payload magic `A135FXNU`, log magic `A135FLOG`).
Change the name once in `tools/` + a `sed` over `xnu/patches` if you prefer another one.

## How to use it (untested path)

You need an A13 with an **unlocked bootloader**, a Linux PC, a GitHub account (kernel build runs on macOS runners) and
a boot.img that your phone already boots (your postmarketOS one is ideal).

1. **Back up** `boot`, `vbmeta`, `userdata` partition table, `efs`, `modem` partitions, etc. Keep them off git.
2. **Kernel:** fork the repo and run the `xnu` workflow (`gh workflow run xnu.yml -R YOU/ios-a135f`), then
   `gh run download RUN_ID -n xnu-kernel -D xnu/out`. Expect compile errors on the first runs (`keep_going` collects them all).
3. **Root image:** as upstream: `tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G`, then
   `tools/a135f-root-tweaks.py`. Needs the iPhone X (iPhone10,3) 16.7.16 IPSW from Apple.
4. **Payload and blobs:**
   ```sh
   python3 tools/mkadt.py shim/out/a135f.adt --touch-fw /path/to/td4375_a13_boe.bin   # vendor TCM image you load on Linux
   python3 tools/mkpayload.py xnu/out/kernel.development.a135f shim/out/a135f.adt shim/out/payload.bin \
       --cmdline 'debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=mmcroot0 mmcrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 a135f_fb_noconsole=1 a135f_fb_hz=30'
   shim/build.sh shim/out/payload.bin      # needs aarch64-linux-gnu-gcc/binutils; -> shim/out/uniloader-blobs/{Image,ramdisk}
   ```
   Start with `cpus=1` to rule out SMP problems. `mkpayload.py` needs the D22AP device tree extracted as in the upstream README.
5. **uniLoader:** copy `shim/out/uniloader-blobs/Image` to `uniLoader/blob/Image`, `.../ramdisk` to `blob/ramdisk`, and your usual mainline A13 DTB to
   `blob/dtb`; build uniLoader exactly as for Linux (same board config, `PAYLOAD_ENTRY`/`RAMDISK_ENTRY` as for your Linux build) and flash the
   result to `boot`. Write the iOS root image to `userdata`. (If you have `mkbootimg`, `shim/build.sh payload.bin [ref-boot.img]` also writes a
   stand-alone `a135f-boot.img` and an Odin `AP_a135f.tar.md5`.)
6. **Logs:** the kernel keeps a 4 MiB console ring at the end of `userdata`; read it from recovery/pmOS with
   `tools/a135f-log-pull.sh`. The shim also paints a status/panic screen on the framebuffer.

## How to boot it

**Use uniLoader, the way you boot Linux/postmarketOS today.** uniLoader embeds `blob/Image`, `blob/dtb` and `blob/ramdisk`
in its own image: the shim is the `Image`, the XNU payload is the `ramdisk`, and your usual mainline A13 DTB is the `dtb`
(the shim only reads `/chosen/linux,initrd-start/-end` from it; XNU gets its own Apple device tree). Package with
`shim/build.sh shim/out/payload.bin` (writes `shim/out/uniloader-blobs/{Image,ramdisk}`), copy them into uniLoader's `blob/` and build/flash uniLoader as for Linux; see `docs/BUILD.md`.
The display write is also in the shim; add it to uniLoader's `a13_init()` too if you like
(`*(volatile uint32_t *)(0x13060000 + 0x70) = 0x1281;`).

Why not the others:
- *Stock s-boot with the stock DTB*: works in principle (same Image + initrd protocol) but gives you no hook for device init
  (display enable, anything else the Linux port needed), and the Samsung boot image checks get in the way. uniLoader is strictly easier.
- *UEFI (Mu-Silicium or your port)*: not for the first boot. It needs the shim built as an EFI application
  (memory map and GOP framebuffer from the firmware, payload read from the ESP, `ExitBootServices`, then caches off, MMU off,
  EL2->EL1) and UEFI usually leaves the GIC, timers and priorities configured its own way, which this kernel assumes are
  as the Linux boot protocol leaves them. Get it booting through uniLoader first; the UEFI route is a good second step and I can
  write the EFI stub once you tell me which EL and GIC state your UEFI hands over.

## Assumptions that must be checked on hardware

- The boot CPU is MPIDR 0 and GIC CPU interface *n* belongs to core *n* (the shim prints MPIDR, EL and CNTFRQ).
- Samsung's EL3 firmware has already routed the timer PPI and SGIs to non-secure Group 1 (Linux relies on this too).
- `CNTFRQ_EL0` may read 0; XNU uses the ADT's 26 MHz instead.
- The bootloader leaves the eMMC controller enabled with the card selected (clock on). If not, `A135F MMC:` prints why and
  the kernel panics at root mount; full controller bring-up (CMU clocks, tuning) would then be needed.
- The touch SPI source clock: the driver forces CMU_CMGP USI0 to the 49.152 MHz RCO / 2 (SCK 6.1 MHz). The supply (LDO28) and the
  panel reset are left to the bootloader; the TD4150 only comes up if it was powered and not reset.
- USB: PHY refclk 26 MHz, VBUS forced valid, CMU_HSI USER muxes as the bootloader left them.
- The 2 MiB DMA window at `0xc8000000` is free RAM at that time (the shim has already copied the payload out).
- The eMMC GPT has a `userdata` partition aligned to 4 KiB.

See `docs/PORTING-NOTES.md` for details and the remaining TODO list.
