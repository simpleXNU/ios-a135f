/*
 * Host test for bsd/dev/a135f_touch.c: compiles the driver with A135F_TOUCH_TEST (register
 * level replaced) against a model of a TD4150 TCM controller (ROM bootloader -> firmware
 * download -> application, continued reads, report bytecode) and runs bring-up scenarios.
 *   gcc -Wall -Wextra -Wshorten-64-to-32 -o t test-touch-tcm.c && ./t
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#define ARM64_BOARD_CONFIG_A135F 1
#define A135F_TOUCH_TEST 1
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define __unused __attribute__((unused))
#define NSEC_PER_MSEC 1000000ull
#define Z_WAITOK 0
#define Z_ZERO 0
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef int kern_return_t;
typedef void *thread_t;
typedef int wait_result_t;
#define THREAD_UNINT 0
#define THREAD_CONTINUE_NULL 0
#define bzero(p, n) memset((p), 0, (n))
#define kalloc_data(n, f) calloc(1, (n))
#define kfree_data(p, n) free(p)

static uint64_t vtime_ns;
static void delay(int us) { vtime_ns += (uint64_t)us * 1000; }
static uint64_t mach_absolute_time(void) { return vtime_ns; }
static void clock_interval_to_absolutetime_interval(unsigned i, uint64_t scale, uint64_t *r) { *r = i * scale; }
static void clock_interval_to_deadline(unsigned i, uint64_t scale, uint64_t *r) { *r = vtime_ns + i * scale; }
static int assert_wait_deadline(void *c, int m, uint64_t d) { (void)c; (void)m; vtime_ns = d > vtime_ns ? d : vtime_ns; return 0; }
static int thread_block(int x) { (void)x; return 0; }

/* ---- recorded HID events ---- */
static int hid_n, hid_last[4];
void a135f_hid_touch(int f, int d, int x, int y) { hid_n++; hid_last[0] = f; hid_last[1] = d; hid_last[2] = x; hid_last[3] = y; }

/* ---- TD4150 model ---- */
#define FWLEN 102400
static uint8_t fw[FWLEN];
static struct {
	int mode; bool fw_ok, attn_works, ok_for_identify, spi_dead, flaky_first_read;
	uint8_t q[16][4200]; unsigned qlen[16]; int qn;
	uint8_t cont[4200]; unsigned cont_len; bool cont_pending;
	uint8_t wbuf[110000]; unsigned wlen; bool in_write;
	unsigned max_write; int rd_in_txn;
	unsigned reads, writes;
} dev;
static bool cs_on;
static unsigned read_length;

static void push(uint8_t code, const uint8_t *p, unsigned n)
{
	uint8_t *m = dev.q[dev.qn];
	m[0] = 0xa5; m[1] = code; m[2] = n & 0xff; m[3] = n >> 8;
	if (n) memcpy(m + 4, p, n);
	m[4 + n] = 0x5a;
	dev.qlen[dev.qn++] = n + 5;
}
static void push_identify(void)
{
	uint8_t id[24] = { 0 };
	id[1] = (uint8_t)dev.mode; memcpy(id + 2, "TD4150", 6);
	id[18] = 7; id[22] = dev.max_write & 0xff; id[23] = dev.max_write >> 8;
	if (dev.ok_for_identify) push(0x01, id, 24); else push(0x10, id, 24);
}
static void push_touch(unsigned x, unsigned y, int n)
{
	/* bytecode below: num_active(4) then per object index(4) class(4) x(16) y(16) z(8) */
	uint8_t r[16] = { 0 }; unsigned bit = 0;
#define PUT(v, b) do { for (unsigned i_ = 0; i_ < (b); i_++, bit++) if (((v) >> i_) & 1) r[bit / 8] |= (uint8_t)(1u << (bit % 8)); } while (0)
	PUT((unsigned)n, 4);
	for (int o = 0; o < n; o++) { PUT((unsigned)o, 4); PUT(1u, 4); PUT(x + (unsigned)o * 10, 16); PUT(y, 16); PUT(40u, 8); }
	push(0x11, r, (bit + 7) / 8);
}
static void dev_command(const uint8_t *w, unsigned len)
{
	uint8_t c = w[0]; unsigned plen = w[1] | (w[2] << 8);
	dev.writes++;
	switch (c) {
	case 0x02: push_identify(); break;
	case 0x04: dev.mode = 4; dev.fw_ok = false; push_identify(); break;
	case 0x45:
		if (dev.mode != 4) { push(0x0f, NULL, 0); break; }
		/* length field is the low 16 bits of 14 + fwlen; first payload byte = fwlen >> 16 */
		if (plen != ((14 + FWLEN) & 0xffff) || len != 3 + 14 + FWLEN || w[3] != (FWLEN >> 16) ||
		    memcmp(w + 3 + 14, fw, FWLEN)) { push(0x0f, NULL, 0); break; }
		dev.fw_ok = true; push(0x01, NULL, 0); break;
	case 0x42:
		if (!dev.fw_ok) { push(0x0f, NULL, 0); break; }
		dev.mode = 2; push_identify(); break;
	case 0x20: {
		uint8_t ai[40] = { 0 }; ai[32] = 0x37; ai[33] = 0x04; ai[34] = 0x67; ai[35] = 0x09; ai[36] = 10;
		push(0x01, ai, 38); break; }
	case 0x25: {
		static const uint8_t cfg[] = { 0x18, 4, 0x01, 0x06, 4, 0x07, 4, 0x08, 16, 0x09, 16, 0x0a, 8, 0x03, 0x00 };
		push(0x01, cfg, sizeof(cfg)); break; }
	default: push(0x0e, NULL, 0);
	}
}
static bool attn(void) { return dev.attn_works && (dev.qn > 0 || dev.cont_pending); }
static void cs_set(bool a)
{
	if (a) { dev.wlen = 0; dev.in_write = false; dev.rd_in_txn = 0; }
	else if (dev.in_write && dev.wlen) dev_command(dev.wbuf, dev.wlen);
	cs_on = a;
}
static int spi_hw_init(void) { return 0; }
static int spi_chunk(const uint8_t *tx, uint8_t *rx, unsigned n)
{
	if (dev.spi_dead) return ETIMEDOUT;
	if (tx) { dev.in_write = true; memcpy(dev.wbuf + dev.wlen, tx, n); dev.wlen += n; return 0; }
	/* read: first chunk of a read transaction decides content (whole txn length = sum of chunks) */
	static uint8_t out[4300]; static unsigned outlen, outpos;
	if (dev.rd_in_txn == 0) {
		dev.reads++;
		memset(out, 0xff, sizeof(out)); outpos = 0;
		if (dev.cont_pending) {
			out[0] = 0xa5; out[1] = 0x03; memcpy(out + 2, dev.cont, dev.cont_len);
			outlen = dev.cont_len + 2; dev.cont_pending = false;
		} else if (dev.qn) {
			/* length of this read is unknown until the txn ends: model fixed predictive sizes */
			unsigned L = read_length, tot = dev.qlen[0];
			memcpy(out, dev.q[0], MIN(tot, L));
			if (tot > L + 1) { dev.cont_len = tot - L; memcpy(dev.cont, dev.q[0] + L, dev.cont_len); dev.cont_pending = true; }
			memmove(dev.q[0], dev.q[1], sizeof(dev.q) - sizeof(dev.q[0]));
			memmove(dev.qlen, dev.qlen + 1, sizeof(dev.qlen) - sizeof(dev.qlen[0]));
			dev.qn--; outlen = L;
		} else {
			out[0] = 0xa5; out[1] = 0x00; out[2] = 0; out[3] = 0; out[4] = 0x5a; outlen = 9;
		}
	}
	dev.rd_in_txn++;
	memcpy(rx, out + outpos, n); outpos += n; (void)outlen;
	return 0;
}

#include "a135f_touch.c"

/* The model needs the driver's read length to know how many bytes the host clocks: peek at it. */
static int run(const char *name, bool ok_ident, bool attn_ok, unsigned max_write, bool fw_present, int expect)
{
	memset(&dev, 0, sizeof(dev)); dev.mode = 4; dev.ok_for_identify = ok_ident; dev.attn_works = attn_ok; dev.max_write = max_write;
	blind_mode = attn_seen = false; read_length = TCM_MIN_READ_LEN; wr_chunk_size = 0; reports_seen = 0; hid_n = 0;
	memset(objects_down, 0, sizeof(objects_down)); memset(&diag, 0, sizeof(diag));
	printf("--- %s\n", name);
	int r = touch_bringup(fw_present ? fw : NULL, fw_present ? FWLEN : 0);
	int pass = (r == 0) == (expect == 0);
	if (!r) {
		push_touch(100, 200, 2);
		for (int i = 0; i < 4; i++) { ovt_tcm_poll(); }
		printf("hid events %d last f%d d%d %d,%d\n", hid_n, hid_last[0], hid_last[1], hid_last[2], hid_last[3]);
		pass &= hid_n >= 2 && hid_last[1] == 1;
		dev.qn = 0; push_touch(0, 0, 0);
		for (int i = 0; i < 3; i++) ovt_tcm_poll();
		printf("after lift: last f%d d%d\n", hid_last[0], hid_last[1]);
		pass &= hid_last[1] == 0;
	}
	printf("%s: ret %d -> %s\n", name, r, pass ? "PASS" : "FAIL");
	return !pass;
}

int main(void)
{
	for (unsigned i = 0; i < FWLEN; i++) fw[i] = (uint8_t)(i * 7 + (i >> 8));
	fw[0] = 0x55; fw[1] = 0xaa;
	int bad = 0;
	bad += run("identify as report, attn ok", false, true, 512, true, 0);
	bad += run("identify as STATUS_OK payload", true, true, 512, true, 0);
	bad += run("small max write size", false, true, 3, true, 0);
	bad += run("attention pin dead -> blind polling", false, false, 512, true, 0);
	bad += run("no firmware in ADT", false, true, 512, false, -1);
	memset(&dev, 0, sizeof(dev)); dev.spi_dead = true; dev.attn_works = true;
	printf("--- spi dead\n"); int r = touch_bringup(fw, FWLEN); printf("ret %d stage %s\n", r, diag.stage); bad += r == 0;
	printf("\n%s\n", bad ? "SOME TESTS FAILED" : "ALL PASSED");
	return bad;
}
