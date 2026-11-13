# Metal on the A13 (Mali-G52), the way the MediaTek port does it

Status: ported and checked on a Linux host only (patches applied to pristine Mesa 25.0.7, the AIR compiler built
natively, kernel patch dry-run against the sources of patches 0026-0030). **Nothing here has been built for iOS or run
on the phone.** Expect the first boots to need log reading, as everything in this repo does.

## How it fits together

```
app / CoreAnimation  ->  Metal.framework  ->  A135FMetal.bundle (src/a135fmetal.m over src/metal/*.cpp)
                                                   |  dlopen liba135f.air.dylib  (= libOSMesa.8.dylib)
                              AIR -> NIR (mesa/air)  ->  Mesa panfrost (Bifrost v7)  ->  a135f-drm shim
                                                   |  IOConnectCall (0x40..0x48, 0x7c, 0x7d, 0x7e)
                                           A135FMali (patch 0027/0029/0030/0031)  ->  the GPU
```

- Metal finds a GPU by `IOServiceMatching("IOAcceleratorES")` and the service's `MetalPluginName` /
  `MetalPluginClassName` properties (a bundle in `/System/Library/Extensions`, a class in it).
  **Patch 0031** publishes that service only with boot-arg `a135f_metal=1` and lets a user process set exactly those
  two properties. Nothing changes in a boot without the boot-arg.
- The driver is synchronous in the kernel (a job has finished when submit returns), which is the configuration the
  MediaTek port itself settled on: its asynchronous worker (`a13m_async=1`) made frames draw parts of other frames,
  so it is off by default here too. The selector `0x7c` (surface sequence) is accepted and does nothing for that reason.
- `kern.a135f.say` (also `host_keep`) puts a line into the kernel log; `kern.a135f.bind_cpu` binds a thread to a CPU
  (only asked for with boot-arg `bigcore=1`, which this all-A55 SoC has no use for).
- The Mesa side is the MediaTek port's patch set (`userland/mesa/patches`), which replaces `apply-panfrost-darwin.py`.
  `panfrost-perf.patch` carries the Bifrost fixes and switches its Metal driver relies on.
- `a135f_inject.c` (backboardd): with `a135f_metal=1` it checks that the driver, QuartzCore's shaders and the service
  exist; then it hands QuartzCore `QuartzCore-default.metallib`, has shader functions specialized by our compiler
  (Apple's MTLCompilerService dies on them), reports backboardd's faults into the kernel log (`bbd fault ...` lines) and
  names the driver on the service. If anything is missing it says what and falls back to the OpenGL ES road.
  All of it is found through the Objective-C runtime by name: no address of the dyld shared cache.

## Build

All of it through `./build.sh` at the top of the repo (`./build.sh --help`):

```sh
./build.sh --userland [--qc-metallib FILE | --macos-ipsw MACOS_IPSW]       # Mesa + Metal -> userland/out
./build.sh --root --ipsw IOS_IPSW                                           # root image with userland/out in it
./build.sh --payload --kernel /path/to/kernel.development.a135f             # ADT + payload + shim blobs
./build.sh --full --kernel K --ipsw I --macos-ipsw M                        # all three
```

`--build-kernel` (instead of `--kernel`) runs the xnu workflow with `gh` and downloads the result. Patch 0031 must be in
the kernel for Metal; it adds `iokit/Kernel/arm/A135FAccelerator.cpp` (IOKit file list), `bsd/dev/a135f_say.c` (BSD file
list) and three small edits in `A135FMali.cpp`. The root step also copies the bundle to
`/System/Library/Extensions/A135FMetal.bundle` (a copy, since Metal's discovery opens it by that path) and `out/share/*`
to `/usr/local/share/a135f`. `--no-metal` gives the earlier OpenGL-ES-only build.

## QuartzCore's shaders (you have to supply them)

CoreAnimation's Metal renderer asserts that QuartzCore has a `default.metallib`. The iPhone's firmware has none in AIR,
and Apple's file may not be shipped here, so it is made from a macOS install: `./build.sh --macos-ipsw <macOS IPSW or
URL>` runs `tools/get-qc-metallib.sh`, which unpacks the OS image of the IPSW (several GB), mounts it with apfs-fuse, finds
QuartzCore's `default.metallib`, takes its AIR slice (cputype 0x1000017 of the fat file) and runs `tools/qc-metallib.py`.
That script has not been run on a real macOS IPSW: if it cannot find the file, extract it yourself and pass it with
`--qc-metallib`, or run `python3 tools/qc-metallib.py macos-default.metallib QuartzCore-default.metallib` (it takes the
fat file as it is; a third argument, QuartzCore's strings, is optional).

It marks the library for iOS and AIR 2.5 and adds aliases for functions iOS asks for and macOS lacks (it prints each).
The header says 16.5; a 16.7.16 device accepts a library for an older OS, so nothing to change for that. What can
differ between 16.5 and 16.7.16 is a function name QuartzCore asks for: if the log says a function was not found, add an
alias in `ALIAS` in that script.

## Boot

`... a135f_mali=1 a135f_gl=1 a135f_metal=1` (what `./build.sh --payload` puts on the command line) (the injector is put into backboardd by `a135f_gl=1`, patch 0028).
Without `a135f_metal=1` everything is as before. For first tests, with the boot-arg set, from a shell (adb/ssh):

```sh
metal_props            # shows what the accelerator has; `metal_props on` names the driver by hand, `off` removes it
metal_test             # what iOS believes about Metal, and what CoreImage does without it
metal_drv_test         # the driver's own test: buffers, a draw, a readback
```

The plug-in's switches (boot-args): `a13m_warm=0` (no pipelines built at start), `a13m_async=1`, `a13m_finish=1`,
`a13m_trace=1`, `a13m_inuse=0`, `a13m_drain=0`, `a13m_sched_wait=1`, `a13m_copyin_late=1`, `a13m_xm_blend=0`,
`mali_trace=1`, `sbmtl=1`, `ca_dirty=0` (injector), `bigcore=1`. `RECIPES=1` installs `share/a13m-recipes.txt`: the
pipelines the MediaTek port's CoreAnimation builds at start (iOS 16.5); try without it first.

## What to expect (from the MediaTek port's own notes)

Its Metal output is not finished: CoreAnimation on Metal was drawing, with striping/tearing causes narrowed down, most
frames within 18 ms. The OpenGL ES road stays the safer one; `a135f_metal=1` is for trying Metal and for apps that call
it. Both CPU-side switches and the `bbd fault` lines are there to find out where it dies if it does.

## Left as it was

The native service of patch 0030 (for the PanVK / Mali Metal approach) stays; the two are independent services
(`A135FMaliNative` and `IOAcceleratorES`), and the drm shim uses `A135FMali`.
