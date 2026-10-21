# XNU for the Galaxy A13 (A135F)

Built on GitHub's macOS arm64 runners by `.github/workflows/xnu.yml`, using
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) (MIT) at a pinned commit.

- `MACOS_VERSION=13.5` -> xnu-8796.141.3 (same generation as iOS 16.6/16.7).
- `MACHINE_CONFIG=A135F` selects `config/MASTER.arm64.a135f` and `pexpert/pexpert/arm64/A135F.h`.
- `patches/*.sh` stage third-party sources (libpthread, HFS, BearSSL), then `patches/*.patch` are applied in order with `git apply`.
- Patches 0001-0081 are the upstream ios-sweet series (Redmi Note 10 Pro, SM7150), mechanically renamed. They still contain
  Qualcomm-specific code that 0082-0084 replace. 0082-0084 are the Exynos850 port:
  - 0082 SoC core: GIC-400 (GICv2) interrupts and IPIs, eight Cortex-A55 with Exynos MPIDRs, framebuffer at 0xfa000000
  - 0083 eMMC (DW-MSHC) root device replacing the UFS driver (`rd=mmcroot0`)
  - 0084 removes the Qualcomm RPMh/SPMI/GENI/USB code: GPIO volume keys, placeholders for touch and USB, battery fallback
- Output: `kernel.<config>.a135f` as a workflow artifact.

Fetch the latest build: `gh run download -R YOU/ios-a135f -n xnu-kernel -D xnu/out`
