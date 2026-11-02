What is and is not verified about the Mesa half
===============================================
Written without Mesa's source at hand (the sandbox it was written in could not fetch it) and never built.

- a135f-drm/ is complete against the panfrost uapi (include/uapi/drm/panfrost_drm.h): request numbers
  0x40..0x48 are the low byte of Linux's ioctl codes, which is what the kernel service takes as selectors.
- apply-panfrost-darwin.py makes 8 text edits in Mesa 25.0.7 and says "MISSED" for every one it cannot
  place. If any is missed, make that edit by hand: what each one is for is in the comment above it.
  Likely trouble spots: the exact os_mmap()/os_munmap() text in src/panfrost/lib/kmod/pan_kmod.c, the
  name of osmesa_create_screen() in osmesa_target.c, the dependency list in targets/osmesa/meson.build.
- panfrost_create_screen() is called with an empty pipe_screen_config; if 25.0.7 insists on driconf
  options, copy the driOptionCache set-up from the freedreno version in hakios's freedreno-darwin.patch.
- mk-mesa.sh needs a host mesa_clc (LLVM/libclc); the meson option names (-Dmesa-clc, -Dprecomp-compiler)
  are those of 25.0 as I remember them.
- Nothing here needs Mesa to work to test the kernel: src/mali_test.c talks to the service directly.
