# Porting notes: sweet (SM7150) -> A13 (Exynos850)

## Verified (on a Linux box, no hardware)
- xnu-8796.141.3 + the blacktop build-script edits + staged libpthread/HFS sources: all 81 original patches apply.
- The mechanically renamed series applies identically; patches 0082-0084 apply on top and reproduce the working tree.
- `tools/mkadt.py` produces an ADT (without the D22AP `/product` node, which needs Apple's IPSW).
- BearSSL staging (`0020-stage-bearssl.sh`) was not run locally (bearssl.org unreachable); the CI runner fetches it.

## Not verified
Everything that needs compiling XNU (macOS) or a phone: all of 0082-0084, `shim/`, boot, eMMC reads, SMP, display.

## Design decisions
- **GICv2:** `HAS_GIC_V3` is not set for A135F; `pe_fiq.c` has a separate `#elif defined(A135F)` block. Each CPU programs its
  banked PPI/SGI state and CPU interface; the boot CPU enables the distributor. IPIs use `GICD_SGIR` with the target's CPU
  interface bit, learned from `GICD_ITARGETSR0` (fallback `1 << logical cpu`).
- **One scheduler cluster:** both physical A55 clusters appear as a single cluster in the ADT (`MAX_CPU_CLUSTERS` stays 1);
  only the MPIDRs (`0x000-0x003`, `0x100-0x103`) differ from sweet.
- **eMMC DMA:** IDMAC (32- or 64-bit descriptors chosen from `HCON`), 1 MiB bounce buffer + descriptors in a device-mapped
  window below 4 GiB (`/a135f-mmc dma-base`), no cache maintenance needed, 64-bit aligned copies only. No clock/tuning changes.
- **Power key:** no S2MPU12 driver yet; Vol Up + Vol Down together = Power.

## TODO (in rough order of value)
1. Compile on CI and fix errors (expect some in 0082-0084).
2. First boot with `cpus=1`; confirm framebuffer marks, GIC timer tick, then eMMC root.
3. Touch (0085, polled): verify on hardware that the SPI clock/pinmux assumptions hold; use an IRQ instead of 4 ms polling later.
4. USB (0085): verify PHY/clock bring-up; try `a135f_usb=1` (report only) first if the kernel hangs.
5. Fuel gauge (MAX1726x over I2C) and S2MPU12 power key / PMIC (ACPM mailbox).
6. Optionally bring up the A55 DVFS/idle, and a real power-off path.
