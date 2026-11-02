/*
 * The libdrm of xf86drm.h (see there) for Mesa's panfrost on iOS: Linux's DRM requests as calls of the
 * kernel's A135FMali user client. A request's number (the low byte of the ioctl code) is the call's
 * selector, its structure goes in and comes back as it is; a buffer's memory is the client memory of the
 * type that is the buffer's handle. Mesa's file descriptor means nothing (/dev/null): there is one GPU
 * and one connection to it per process.
 *
 * Every job has finished before its submit call returns (kernel), so synchronisation objects are
 * answered "signalled" here and never reach the kernel.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <IOKit/IOKitLib.h>
#include "xf86drm.h"

#define A135F_SERVICE "A135FMali"
#define A135F_SURFACE 0x7e
#define A135F_DESCRIBE 0xffffffffu
#define DRM_SYNCOBJ_FIRST 0xbf          /* DRM_COMMAND numbers of the syncobj ioctls (drm.h): CREATE .. TIMELINE_QUERY */
#define DRM_SYNCOBJ_LAST  0xd0

static io_connect_t connection;
static int connection_fd = -1;
static uint32_t next_syncobj = 1;
#define MAPS 8192
static struct { mach_vm_address_t address; uint32_t handle; } maps[MAPS];
static pthread_mutex_t maps_lock = PTHREAD_MUTEX_INITIALIZER;

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	io_service_t service;

	(void)busid; (void)type;
	if (strcmp(name, "panfrost") != 0) {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	service = IOServiceGetMatchingService(0, IOServiceMatching(A135F_SERVICE));
	if (service == 0) {
		return -1;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
		IOObjectRelease(service);
		return -1;
	}
	IOObjectRelease(service);
	connection_fd = open("/dev/null", O_RDWR);
	return dup(connection_fd);
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);

	switch (result) {
	case 0: return 0;
	case kIOReturnNoMemory: return ENOMEM;
	case kIOReturnBadArgument: return EINVAL;
	case kIOReturnTimeout: return ETIMEDOUT;
	case kIOReturnBusy: return EBUSY;
	case kIOReturnNotFound: return ENOENT;
	case kIOReturnUnsupported: return ENOSYS;
	default: return EIO;
	}
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned nr = (unsigned)(request & 0xff);
	int error;

	(void)fd;
	if (nr >= DRM_SYNCOBJ_FIRST && nr <= DRM_SYNCOBJ_LAST) {
		if (nr == DRM_SYNCOBJ_FIRST && arg != NULL) {   /* CREATE: struct { u32 handle; u32 flags; } */
			((uint32_t *)arg)[0] = next_syncobj++;
		}
		return 0;                       /* everything else about them: signalled, done */
	}
	error = call(nr, arg, IOCPARM_LEN(request));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/* Panfrost's interface version 1.3 (jobs, growable and heap buffers). */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = 3;
		version->name = strdup("panfrost");
		version->name_len = 8;
		version->date = strdup("");
		version->desc = strdup("Mali-G52 on iOS");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name); free(version->date); free(version->desc); free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd; (void)capability;
	*value = 0;
	return 0;
}

void *
a135f_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	(void)fd; (void)size;
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	pthread_mutex_lock(&maps_lock);
	for (unsigned i = 0; i < MAPS; i++) {
		if (maps[i].address == 0) {
			maps[i].address = address;
			maps[i].handle = handle;
			break;
		}
	}
	pthread_mutex_unlock(&maps_lock);
	return (void *)(uintptr_t)address;
}

/* Mesa's munmap() knows the address only: the handle it was mapped by is looked up here. */
void
a135f_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	pthread_mutex_lock(&maps_lock);
	for (unsigned i = 0; i < MAPS; i++) {
		if (maps[i].address == (mach_vm_address_t)(uintptr_t)address && maps[i].address != 0) {
			handle = maps[i].handle;
			maps[i].address = 0;
			break;
		}
	}
	pthread_mutex_unlock(&maps_lock);
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}

int
a135f_drm_surface(int fd, struct a135f_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = A135F_DESCRIBE;
	error = call(A135F_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct a135f_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(A135F_SURFACE, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle) { (void)fd; (void)flags; *handle = next_syncobj++; return 0; }
int drmSyncobjDestroy(int fd, uint32_t handle) { (void)fd; (void)handle; return 0; }
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count) { (void)fd; (void)handles; (void)count; return 0; }
int drmSyncobjWait(int fd, uint32_t *handles, unsigned n, int64_t timeout, unsigned flags, uint32_t *first)
{
	(void)fd; (void)handles; (void)n; (void)timeout; (void)flags;
	if (first != NULL) {
		*first = 0;
	}
	return 0;
}
static int none(void) { errno = ENOSYS; return -1; }
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) { (void)fd; (void)handle; (void)flags; (void)prime_fd; return none(); }
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle) { (void)fd; (void)obj_fd; (void)handle; return none(); }
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd) { (void)fd; (void)handle; (void)obj_fd; return none(); }
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd) { (void)fd; (void)handle; (void)sync_file_fd; return none(); }
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd) { (void)fd; (void)handle; (void)sync_file_fd; return none(); }
