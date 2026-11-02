/* What user space and the kernel's A135FMali service (iokit/Kernel/arm/A135FMali.cpp) agree on:
 * Linux's panfrost DRM requests as IOKit user-client selectors (the low byte of the ioctl code). */
#ifndef A135F_MALI_H
#define A135F_MALI_H
#include <stdint.h>

#define A135F_MALI_SERVICE "A135FMali"
enum {
	A135F_GEM_CLOSE = 0x09, A135F_SUBMIT = 0x40, A135F_WAIT_BO = 0x41, A135F_CREATE_BO = 0x42,
	A135F_MMAP_BO = 0x43, A135F_GET_PARAM = 0x44, A135F_GET_BO_OFFSET = 0x45, A135F_MADVISE = 0x48,
	A135F_SURFACE = 0x7e,
};
struct a135f_create_bo { uint32_t size, flags, handle, pad; uint64_t offset; };
struct a135f_get_param { uint32_t param, pad; uint64_t value; };
struct a135f_get_bo_offset { uint32_t handle, pad; uint64_t offset; };
struct a135f_submit {
	uint64_t jc, in_syncs; uint32_t in_sync_count, out_sync;
	uint64_t bo_handles; uint32_t bo_handle_count, requirements;
};
struct a135f_gem_close { uint32_t handle, pad; };
#endif
