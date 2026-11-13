# Notice: the Metal / panfrost-perf port

The following in this tree are derived from the HakiOS project's MediaTek port (Mali-G52, Bifrost v7), with
identifiers and paths renamed for this device; the copyright lines in the files are kept as they were:

- `userland/src/a135fmetal.m`, `userland/src/metal/*`, `userland/src/metal_props.c`, `metal_plugin.plist`,
  `metal_test.m`, `metal_drv_test.m`, `fbfetch.metal`, the runtime-hook part of `userland/src/a135f_inject.c`
- `userland/mesa/air/*` (the AIR -> NIR compiler and the Metal device, built into Mesa's src/compiler/air)
- `userland/mesa/a135f-drm/*`, `userland/mesa/patches/*.patch` (except `osmesa-es-profile.patch`, which this
  tree had), `tools/metallib.py`, `tools/qc-metallib.py`, `tools/a13m-recipes.py`, `userland/share/a13m-recipes.txt`
- in the kernel, `bsd/dev/a135f_say.c` and `iokit/Kernel/arm/A135FAccelerator.cpp` (patch 0031) follow that port's
  kern.*.say / bind_cpu sysctls and its IOAcceleratorES service. The files written for Apple's XNU are under the
  Apple Public Source License 2.0 there; everything else under the MIT license below.

Not included, and not to be: any file of iOS or macOS. QuartzCore's `default.metallib` for CoreAnimation's Metal
renderer is Apple's; you make it yourself from a macOS install (`tools/qc-metallib.py`).

The Mesa patches apply to Mesa 25.0.7 (MIT; the tarball is fetched at build time and is not part of this tree).

---

MIT License

Copyright (c) 2026 The HakiOS authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
