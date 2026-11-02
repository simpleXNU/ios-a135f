#!/usr/bin/env python3
"""Make Mesa's panfrost build and run on iOS over the A135FMali service (userland/mesa/a135f-drm).

  apply-panfrost-darwin.py MESA_SOURCE_DIR

Written against Mesa 25.0.x WITHOUT the source at hand, so every edit is anchored on a regular
expression and checked: a miss is reported by name and the exit status is 1 (nothing half-silently
skipped). If one misses, open the file it names, make the same change by hand (what it is is in the
comment above each edit), and re-run -- finished edits are recognised and left alone.
"""
import os, re, shutil, sys

root = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
failed = []
MARK = 'A135F:'

def edit(path, what, pattern, repl, flags=re.S):
    p = os.path.join(root, path)
    s = open(p).read()
    if MARK in s and ('/* %s %s */' % (MARK, what)) in s:
        print('already: %s: %s' % (path, what)); return
    new, n = re.subn(pattern, lambda m: repl(m) if callable(repl) else repl, s, count=1, flags=flags)
    if n != 1:
        print('MISSED : %s: %s' % (path, what)); failed.append((path, what)); return
    open(p, 'w').write(new); print('done   : %s: %s' % (path, what))

# 1. The libdrm stand-in: Mesa's headers and our two files.
dst = os.path.join(root, 'src/a135f-drm')
shutil.rmtree(dst, ignore_errors=True)
shutil.copytree(os.path.join(here, 'a135f-drm'), dst)

# 2. meson.build (top): on Darwin, panfrost's "libdrm" is the stand-in.
edit('meson.build', 'darwin libdrm for panfrost',
     r'(if dep_libdrm\.found\(\))',
     lambda m: ("if host_machine.system() == 'darwin' and with_gallium_panfrost\n"
                "  # /* A135F: darwin libdrm for panfrost */ no DRM here: src/a135f-drm stands in for libdrm\n"
                "  dep_libdrm = declare_dependency(\n"
                "    include_directories : include_directories('src/a135f-drm'),\n"
                "    link_args : ['-framework', 'IOKit', '-framework', 'CoreFoundation'],\n"
                "  )\nelif dep_libdrm.found()"))

# 3. pan_kmod.c: a buffer's CPU mapping is the user client's memory, not mmap() on the device.
edit('src/panfrost/lib/kmod/pan_kmod.c', 'bo mmap through the user client',
     r'os_mmap\s*\(\s*host_addr\s*,\s*size\s*,\s*prot\s*,\s*flags\s*,\s*bo->dev->fd\s*,[^;]*?\)\s*;',
     lambda m: ('/* A135F: bo mmap through the user client */\n'
                '#ifdef __APPLE__\n   a135f_drm_map(bo->dev->fd, bo->handle, size);\n#else\n   ' + m.group(0) + '\n#endif\n'))
edit('src/panfrost/lib/kmod/pan_kmod.c', 'bo munmap through the user client',
     r'os_munmap\s*\(\s*([^,;]+?)\s*,\s*([^;]+?)\)\s*;',
     lambda m: ('/* A135F: bo munmap through the user client */\n#ifdef __APPLE__\n   a135f_drm_unmap(0, 0, %s);\n#else\n   %s\n#endif\n' % (m.group(1), m.group(0))))

# 4. OSMesa's target: the Mali is the screen when there is one.
tgt = 'src/gallium/targets/osmesa/osmesa_target.c'
inc = open(os.path.join(here, 'a135f_osmesa_gpu.inc')).read()
edit(tgt, 'gpu screen code',
     r'(struct pipe_screen \*\s*\n?osmesa_create_screen\s*\(void\))',
     lambda m: '/* A135F: gpu screen code */\n#ifdef __APPLE__\n' + inc + '#endif\n\n' + m.group(1))
edit(tgt, 'gpu screen first',
     r'(osmesa_create_screen\s*\(void\)\s*\{)',
     lambda m: m.group(1) + '\n   /* A135F: gpu screen first */\n#ifdef __APPLE__\n   { struct pipe_screen *gpu = osmesa_create_gpu_screen(); if (gpu) return gpu; }\n#endif')
edit('src/gallium/targets/osmesa/meson.build', 'link panfrost',
     r'(driver_swrast\s*,)',
     lambda m: m.group(1) + " driver_panfrost, dep_libdrm,  # /* A135F: link panfrost */\n    ")

# 5. OSMesa's frontend: glEGLImageTargetTexture2DOES takes an IOSurface ID.
fe = 'src/gallium/frontends/osmesa/osmesa.c'
edit(fe, 'egl image from iosurface',
     r'(global_fscreen->get_egl_image\s*=\s*NULL\s*;)',
     lambda m: '/* A135F: egl image from iosurface */\n      global_fscreen->get_egl_image = osmesa_st_get_egl_image;\n      global_fscreen->validate_egl_image = osmesa_st_validate_egl_image;')
edit(fe, 'egl image helpers',
     r'(^static\s+\w[^\n]*\n[^\n]*\n\{)',   # placed before the first static function
     lambda m: ('/* A135F: egl image helpers */\nextern struct pipe_resource *osmesa_surface_resource(struct pipe_screen *screen, uint32_t surface);\n'
                'static bool osmesa_st_get_egl_image(struct pipe_frontend_screen *fscreen, void *image, struct st_egl_image *out)\n'
                '{ out->texture = osmesa_surface_resource(fscreen->screen, (uint32_t)(uintptr_t)image);\n'
                '  if (!out->texture) return false; out->format = out->texture->format; return true; }\n'
                'static bool osmesa_st_validate_egl_image(struct pipe_frontend_screen *fscreen, void *image) { return true; }\n') + m.group(1), flags=re.S | re.M)

print('\n%d edit(s) missed' % len(failed))
sys.exit(1 if failed else 0)
