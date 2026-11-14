/*
 * native_mali_test: the first thing to run against NativeMaliGpuService (kernel patches 0029/0030).
 * It speaks the NativeMali ABI v2 directly -- the interface Mali Metal's PanVK backend will use -- and
 * checks every method on the real GPU, including the refusals, and that two connections get address
 * spaces of their own. Prints PASS/FAIL per check; exit status 0 only if everything passed.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <IOKit/IOKitLib.h>

struct nm_q { uint32_t abi, in_bits, out_bits, flags; uint64_t max_bo; };
struct nm_di { uint32_t gpu_id, sh_lo, sh_hi, ti_lo, ti_hi, thr, wg, bar, l2, core, tiler, flags; uint64_t va_start, va_size;
	uint32_t mem, mmu, afbc, tex[4], tasks, regs, tls, pgshift, vmops; };
struct nm_bomap { uint64_t handle, iova; uint32_t perm, reserved; };
struct nm_cpum { uint32_t token, reserved; uint64_t bytes; };
struct nm_cpuu { uint64_t handle; uint32_t token, reserved; };
struct nm_sub { uint64_t chain, wait_fence, wait_timeout; uint32_t flags, reserved; };
struct nm_wait { uint64_t fence, timeout; };
struct nm_stat { uint64_t faults, resets; };
struct nm_stat2 { uint64_t faults, resets; };

enum { QUERY = 1, INFO, ALLOC, MAP, FREE, CPUMAP, CPUUNMAP, SUBMIT, WAITF, STATUS, RESET };

static int failures;
#define CHECK(cond, what) do { if (cond) printf("PASS  %s\n", what); else { printf("FAIL  %s\n", what); failures++; } } while (0)

static kern_return_t
call(io_connect_t c, uint32_t sel, void *in, size_t inSize, void *out, size_t outSize)
{
	size_t o = outSize;

	return IOConnectCallStructMethod(c, sel, in, inSize, out, out ? &o : NULL);
}

static io_connect_t
open_conn(void)
{
	io_service_t s = IOServiceGetMatchingService(0, IOServiceMatching("NativeMaliGpuService"));
	io_connect_t c = 0;

	if (s == 0) {
		return 0;
	}
	if (IOServiceOpen(s, mach_task_self(), 0, &c) != 0) {
		c = 0;
	}
	IOObjectRelease(s);
	return c;
}

/* A write-value job at the start of `mem`: writes `value` to `target`. */
static void
write_job(volatile uint32_t *mem, uint64_t target, uint32_t value)
{
	memset((void *)mem, 0, 0x100);
	mem[4] = (2u << 1) | 1u | (1u << 16);   /* write value, 64-bit pointers, job index 1 */
	mem[8] = (uint32_t)target;
	mem[9] = (uint32_t)(target >> 32);
	mem[10] = 6;                            /* a 32-bit immediate */
	mem[12] = value;
}

struct bo { uint64_t handle; uint64_t iova; volatile uint32_t *cpu; uint32_t token; };

static int
make_bo(io_connect_t c, uint64_t bytes, uint64_t iova, struct bo *b)
{
	uint64_t h = 0, mapped = 0;
	struct nm_bomap m = { 0, iova, 3, 0 };
	struct nm_cpum cm;
	uint64_t hin;
	mach_vm_address_t addr = 0;
	mach_vm_size_t len = 0;

	if (call(c, ALLOC, &bytes, sizeof(bytes), &h, sizeof(h)) != 0) return 1;
	m.handle = h;
	if (call(c, MAP, &m, sizeof(m), &mapped, sizeof(mapped)) != 0 || mapped != iova) return 2;
	hin = h;
	if (call(c, CPUMAP, &hin, sizeof(hin), &cm, sizeof(cm)) != 0) return 3;
	if (IOConnectMapMemory64(c, cm.token, mach_task_self(), &addr, &len, kIOMapAnywhere) != 0) return 4;
	b->handle = h; b->iova = iova; b->cpu = (volatile uint32_t *)(uintptr_t)addr; b->token = cm.token;
	return 0;
}

int
main(void)
{
	io_connect_t c = open_conn(), c2;
	struct nm_q q = { 2, 0, 0, 0, 0 };
	struct nm_di d;
	struct nm_stat2 st0, st1;
	struct bo b, b2;
	uint64_t fence = 0, bytes = 16384;
	struct nm_sub s;
	struct nm_wait w;
	int r;

	if (c == 0) {
		printf("FAIL  no NativeMaliGpuService (kernel needs patches 0029/0030 and boot-arg a135f_mali=1)\n");
		return 2;
	}
	CHECK(call(c, QUERY, &q, sizeof(q), &q, sizeof(q)) == 0 && q.abi == 2 && q.max_bo == (1u << 27), "Query v2");
	printf("      address bits in %u out %u, max buffer %llu\n", q.in_bits, q.out_bits, (unsigned long long)q.max_bo);
	memset(&d, 0, sizeof(d));
	CHECK(call(c, INFO, NULL, 0, &d, sizeof(d)) == 0 && d.gpu_id != 0, "GetDeviceInfo (112 bytes)");
	printf("      GPU_ID %#x shaders %#x tiler %#x threads %u wg %u l2 %#x mem %#x mmu %#x\n", d.gpu_id, d.sh_lo, d.ti_lo, d.thr, d.wg, d.l2, d.mem, d.mmu);
	printf("      va %#llx + %#llx, tasks/core %u regs/core %u tls/core %u flags %#x\n", (unsigned long long)d.va_start,
	    (unsigned long long)d.va_size, d.tasks, d.regs, d.tls, d.flags);
	CHECK(d.va_start != 0 && d.va_size != 0 && d.pgshift == 12, "device info: VA window and page size");

	CHECK(call(c, STATUS, NULL, 0, &st0, sizeof(st0)) == 0, "GetStatus");

	r = make_bo(c, bytes, 0x04000000, &b);
	CHECK(r == 0, "BoAlloc + BoMap + BoCpuMap + map into this process");
	if (r == 0) {
		write_job(b.cpu, b.iova + 0x800, 0xa13c0de2u);
		b.cpu[0x800 / 4] = 0;
		memset(&s, 0, sizeof(s));
		s.chain = b.iova;
		CHECK(call(c, SUBMIT, &s, sizeof(s), &fence, sizeof(fence)) == 0 && fence != 0, "Submit a write-value job");
		w.fence = fence; w.timeout = 1000000000ull;
		CHECK(call(c, WAITF, &w, sizeof(w), NULL, 0) == 0, "WaitFence on that fence");
		CHECK(b.cpu[0x800 / 4] == 0xa13c0de2u, "the GPU wrote the word (through this connection's page tables)");
		printf("      word = %#x\n", b.cpu[0x800 / 4]);

		/* refusals */
		s.chain = b.iova + 8;
		CHECK(call(c, SUBMIT, &s, sizeof(s), &fence, sizeof(fence)) != 0, "Submit refuses a chain that is not 64-byte aligned");
		s.chain = 0x08000000;
		CHECK(call(c, SUBMIT, &s, sizeof(s), &fence, sizeof(fence)) != 0, "Submit refuses a chain outside any mapped buffer");
		s.chain = b.iova; s.flags = 1;
		CHECK(call(c, SUBMIT, &s, sizeof(s), &fence, sizeof(fence)) != 0, "Submit refuses unknown flags");
		w.fence = 0x7777777777ull;
		CHECK(call(c, WAITF, &w, sizeof(w), NULL, 0) != 0, "WaitFence refuses a fence never issued");
		{
			struct nm_bomap again = { b.handle, 0x05000000, 3, 0 };
			uint64_t out;
			CHECK(call(c, MAP, &again, sizeof(again), &out, sizeof(out)) != 0, "BoMap refuses a buffer already mapped");
		}
		{
			uint64_t h2 = 0, bytes2 = 4096, out;
			struct nm_bomap over;
			call(c, ALLOC, &bytes2, sizeof(bytes2), &h2, sizeof(h2));
			over.handle = h2; over.iova = b.iova + 4096; over.perm = 3; over.reserved = 0;
			CHECK(call(c, MAP, &over, sizeof(over), &out, sizeof(out)) != 0, "BoMap refuses an overlap");
			over.iova = 0x00001000;
			CHECK(call(c, MAP, &over, sizeof(over), &out, sizeof(out)) != 0, "BoMap refuses an address below the window");
			call(c, FREE, &h2, sizeof(h2), NULL, 0);
		}
		{
			uint64_t big = (1ull << 27) + 4096, h;
			CHECK(call(c, ALLOC, &big, sizeof(big), &h, sizeof(h)) != 0, "BoAlloc refuses more than 128 MiB");
			big = 100;
			CHECK(call(c, ALLOC, &big, sizeof(big), &h, sizeof(h)) != 0, "BoAlloc refuses a size that is not a page multiple");
		}
	}

	/* a second connection: its own address space, the same addresses, no interference */
	c2 = open_conn();
	CHECK(c2 != 0, "a second connection opens");
	if (c2 != 0 && r == 0) {
		int r2 = make_bo(c2, bytes, 0x04000000, &b2);
		CHECK(r2 == 0, "the second connection maps its own buffer at the same GPU address");
		if (r2 == 0) {
			write_job(b2.cpu, b2.iova + 0x800, 0xb0b0b0b0u);
			b2.cpu[0x800 / 4] = 0;
			memset(&s, 0, sizeof(s));
			s.chain = b2.iova;
			CHECK(call(c2, SUBMIT, &s, sizeof(s), &fence, sizeof(fence)) == 0, "Submit on the second connection");
			CHECK(b2.cpu[0x800 / 4] == 0xb0b0b0b0u && b.cpu[0x800 / 4] == 0xa13c0de2u, "each connection's job wrote only its own buffer");
		}
		IOServiceClose(c2);
	}

	CHECK(call(c, STATUS, NULL, 0, &st1, sizeof(st1)) == 0 && st1.faults == st0.faults, "no GPU faults during the run");
	printf("      faults %llu resets %llu\n", (unsigned long long)st1.faults, (unsigned long long)st1.resets);
	if (r == 0) {
		struct nm_cpuu cu = { b.handle, b.token, 0 };

		CHECK(call(c, CPUUNMAP, &cu, sizeof(cu), NULL, 0) == 0, "BoCpuUnmap");
		CHECK(call(c, FREE, &b.handle, sizeof(b.handle), NULL, 0) == 0, "BoFree");
		CHECK(call(c, FREE, &b.handle, sizeof(b.handle), NULL, 0) != 0, "BoFree refuses a stale handle");
	}
	printf("%s: %d check(s) failed\n", failures ? "RESULT FAIL" : "RESULT PASS", failures);
	return failures != 0;
}
