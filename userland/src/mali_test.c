/*
 * mali_test: the first thing to run once the kernel says `sysctl kern.a135f_mali` = 6. Talks to the
 * A135FMali service directly (no Mesa): reads GPU_ID, makes a buffer, maps it, puts a "write value"
 * job into it, submits it and looks at the word the GPU wrote. Exit status 0: the Mali ran a job for a
 * user program. Prints everything it learns on stdout.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <IOKit/IOKitLib.h>
#include "a135f.mali.h"

static io_connect_t conn;

static int
req(unsigned sel, void *p, size_t n)
{
	size_t out = n;
	kern_return_t kr = IOConnectCallStructMethod(conn, sel, p, n, p, &out);

	if (kr != 0) {
		printf("request %#x: kern_return %#x\n", sel, kr);
	}
	return kr != 0;
}

int
main(void)
{
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching(A135F_MALI_SERVICE));
	struct a135f_get_param param = { 0 };
	struct a135f_create_bo bo = { .size = 0x4000 };
	struct a135f_submit submit;
	mach_vm_address_t address = 0;
	mach_vm_size_t length = 0;
	volatile uint32_t *page;
	uint32_t handle;

	if (service == 0) {
		printf("mali_test: no %s service (kernel needs boot-arg a135f_mali=1 and kern.a135f_mali = 6)\n", A135F_MALI_SERVICE);
		return 2;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &conn) != 0) {
		printf("mali_test: cannot open the service\n");
		return 3;
	}
	for (uint32_t p = 0; p < 2; p++) {
		param.param = p;
		if (req(A135F_GET_PARAM, &param, sizeof(param)) == 0) {
			printf("param %u = %#llx\n", p, (unsigned long long)param.value);
		}
	}
	if (req(A135F_CREATE_BO, &bo, sizeof(bo))) {
		return 4;
	}
	handle = bo.handle;
	printf("buffer %u at GPU address %#llx\n", handle, (unsigned long long)bo.offset);
	if (IOConnectMapMemory64(conn, handle, mach_task_self(), &address, &length, kIOMapAnywhere) != 0) {
		printf("mali_test: cannot map the buffer\n");
		return 5;
	}
	page = (volatile uint32_t *)(uintptr_t)address;
	memset((void *)page, 0, 0x1000);
	page[4] = (2u << 1) | 1u | (1u << 16);          /* write value, 64-bit pointers, job index 1 */
	page[8] = (uint32_t)(bo.offset + 0x800);
	page[9] = (uint32_t)((bo.offset + 0x800) >> 32);
	page[10] = 6;                                   /* a 32-bit immediate */
	page[12] = 0xa13c0de1u;
	memset(&submit, 0, sizeof(submit));
	submit.jc = bo.offset;
	submit.bo_handles = (uint64_t)(uintptr_t)&handle;
	submit.bo_handle_count = 1;
	if (req(A135F_SUBMIT, &submit, sizeof(submit))) {
		return 6;
	}
	printf("job %llu done; the word: %#x (expected 0xa13c0de1)\n", (unsigned long long)submit.jc, page[0x800 / 4]);
	return page[0x800 / 4] == 0xa13c0de1u ? 0 : 1;
}
