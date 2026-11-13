What is and is not verified about the Mesa half
===============================================
Verified (on a Linux machine, no phone, no iOS SDK):
- patches/*.patch, in name order, apply to the pristine mesa-25.0.7 archive (SHA-256 in mk-mesa.sh) without a
  reject, with a135f-drm/ copied to src/a135f-drm and air/ to src/compiler/air as mk-mesa.sh does.
- the AIR frontend (air/, libair) compiles natively; air_metal.c and air_run.c pass a syntax check against
  Mesa's headers (the only two complaints are Apple's PTHREAD_RECURSIVE_MUTEX_INITIALIZER and Linux's
  DRM_FORMAT_MOD_LINEAR, which a135f-drm/xf86drm.h has).
NOT verified: the iOS cross build itself (nothing here had the SDK), and anything on the device.

Where it comes from: the set of patches and the two directories are those of a Bifrost v7 (Mali-G52) port
that runs this Mesa on a phone; the A13's Exynos850 has the same GPU generation. panfrost-darwin.patch replaces
the earlier apply-panfrost-darwin.py (which was written without Mesa's source and had never been built), and
Bifrost v7 has no precompiled shaders, so mk-mesa.sh no longer builds mesa_clc on the host.

- a135f-drm/ stands in for libdrm: the panfrost requests (selectors 0x40..0x48), the surface request 0x7e,
  wait 0x7d and surface sequence 0x7c, all taken by patch 0031's A135FMali.
- panfrost-perf.patch holds the Bifrost fixes and switches the Metal driver relies on; air*.patch and
  panfrost-osmesa-air.patch build the AIR compiler into libOSMesa (liba135f.air.dylib is a link to it).
- freedreno-darwin.patch does nothing for the Mali (the Adreno's driver is not built); panfrost-darwin.patch
  is written on top of it, which is why it is here.
