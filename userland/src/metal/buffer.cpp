/*
 * A Metal buffer on this port: Mesa's memory when the compiler library is loaded, else a GEM
 * allocation of the kernel's own gpu driver made and mapped exactly as Mesa's shim does it
 * (tools/mesa/a135f-drm/a135f_drm.c): kMsmGemNew for the memory, MSM_INFO_GET_IOVA through
 * kMsmGemInfo for the address the GPU sees, IOConnectMapMemory64 for the address this processor
 * sees, kDrmGemClose to give it back (iokit/Kernel/A135FGPU.cpp has the numbers).
 */
#include "a13m.h"

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>

#include <new>

namespace {

enum : uint32_t {
	kDrmGemClose = 0x09,
	kMsmGemNew = 0x42,
	kMsmGemInfo = 0x43,
};
constexpr uint32_t msm_info_get_iova = 1;

struct GemNew {
	uint64_t size;
	uint32_t flags, handle;
};
struct GemInfo {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};
struct GemClose {
	uint32_t handle, pad;
};

} // namespace

struct a13m_buffer {
	mach_port_t gpu = MACH_PORT_NULL; /* not owned: the device holds it */
	uint32_t handle = 0;              /* the kernel path only */
	uint64_t iova = 0;
	void *contents = nullptr;
	size_t length = 0;
	struct a13a_buffer *mesa = nullptr; /* the Mesa path only */

	~a13m_buffer()
	{
		if (mesa != nullptr) {
			a13m_air()->buffer_destroy(mesa);
			return;
		}
		if (contents != nullptr) {
			IOConnectUnmapMemory64(gpu, handle, mach_task_self(),
					       (mach_vm_address_t)(uintptr_t)contents);
		}
		if (handle != 0) {
			GemClose request = { handle, 0 };
			size_t size = sizeof(request);

			IOConnectCallStructMethod(gpu, kDrmGemClose, &request, sizeof(request), &request,
						  &size);
		}
	}
};

extern "C" struct a13m_buffer *
a13m_buffer_new(struct a13a_device *device, mach_port_t gpu, size_t length, char *why,
	       size_t why_size)
{
	const struct a13m_air *air = a13m_air();
	a13m_buffer *buffer = new (std::nothrow) a13m_buffer;

	if (buffer == nullptr) {
		snprintf(why, why_size, "no memory for the buffer's own record");
		return nullptr;
	}
	buffer->length = length;
	if (a13m_air_available() && device != nullptr && air->buffer_create != nullptr) {
		buffer->mesa = air->buffer_create(device, length, why, why_size);
		if (buffer->mesa == nullptr) {
			delete buffer;
			return nullptr;
		}
		buffer->contents = air->buffer_map(buffer->mesa);
		return buffer;
	}
	if (gpu == MACH_PORT_NULL || length == 0) {
		snprintf(why, why_size, "a buffer of %zu bytes needs a gpu client", length);
		delete buffer;
		return nullptr;
	}

	GemNew request = { length, 0, 0 };
	size_t size = sizeof(request);

	if (IOConnectCallStructMethod(gpu, kMsmGemNew, &request, sizeof(request), &request,
				      &size) != KERN_SUCCESS || request.handle == 0) {
		snprintf(why, why_size, "the kernel would not allocate %zu bytes", length);
		delete buffer;
		return nullptr;
	}
	buffer->gpu = gpu;
	buffer->handle = request.handle;

	/* the address the GPU will see, which is what a command stream carries */
	GemInfo info = { buffer->handle, msm_info_get_iova, 0, 0, 0 };

	size = sizeof(info);
	if (IOConnectCallStructMethod(gpu, kMsmGemInfo, &info, sizeof(info), &info,
				      &size) == KERN_SUCCESS) {
		buffer->iova = info.value;
	}

	/* and the address this processor will see: as the memory is made, write-combined */
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	if (IOConnectMapMemory64(gpu, buffer->handle, mach_task_self(), &address, &mapped,
				 kIOMapAnywhere) == KERN_SUCCESS) {
		buffer->contents = (void *)(uintptr_t)address;
	} else {
		a13m_print("buffer %u is allocated but could not be mapped\n", buffer->handle);
	}
	return buffer;
}

extern "C" void
a13m_buffer_free(struct a13m_buffer *buffer)
{
	delete buffer;
}

extern "C" void *
a13m_buffer_contents(const struct a13m_buffer *buffer)
{
	return buffer->contents;
}

extern "C" size_t
a13m_buffer_length(const struct a13m_buffer *buffer)
{
	return buffer->length;
}

extern "C" uint64_t
a13m_buffer_gpu_address(const struct a13m_buffer *buffer)
{
	return buffer->iova;
}

extern "C" struct a13a_buffer *
a13m_buffer_mesa(const struct a13m_buffer *buffer)
{
	return buffer->mesa;
}
