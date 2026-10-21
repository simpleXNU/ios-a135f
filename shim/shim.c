/*
 * Galaxy A13 (Exynos850) boot shim.
 *
 * The Samsung bootloader loads this image like a Linux kernel (EL1 or EL2,
 * MMU off, x0 = DTB). start.S makes sure we run at EL1; shim_main() puts a
 * status screen on the bootloader's framebuffer, then acts as iBoot for XNU
 * (loader.c). Any unexpected exception paints a blue panic screen and resets
 * through PSCI after a delay so the screen can be read/photographed.
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"

#define PSCI_SYSTEM_RESET	0x84000009UL
#define DECON_F_BASE		0x13060000UL
#define HW_SW_TRIG_CONTROL	0x70
#define DEFAULT_CNTFRQ		26000000UL	/* oscclk; the A13 dts sets the timer to 26 MHz */

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define isb() __asm__ volatile("isb" ::: "memory")

extern uint64_t shim_entry_el;
static const char *stage;	/* set at runtime: no absolute pointers in data */

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t f = rsr("cntfrq_el0");
	if (!f)
		f = DEFAULT_CNTFRQ;	/* firmware did not program CNTFRQ_EL0 */
	uint64_t end = now() + f / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s > 20 ? "in 40s" : "in 20s");
	delay_ms(after_s * 1000);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_clear(BLUE);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	reset(40);
}

/* ---- main ---- */

/* Without this write the panel does not show the framebuffer (A13 DECON_F trigger control). */
static void a13_init(void)
{
	*(volatile uint32_t *)(DECON_F_BASE + HW_SW_TRIG_CONTROL) = 0x1281;
	__asm__ volatile("dsb sy" ::: "memory");
}

void shim_main(uint64_t dtb)
{
	a13_init();
	stage = "loader";
	con_clear(RED);
	con_str("SHIM", "a135f: xnu loader (Exynos850)");
	con_skip();
	con_hex2("BOOT", "el=", (uint32_t)shim_entry_el, "mpidr=", (uint32_t)rsr("mpidr_el1"));
	con_hex2("TIMER", "cntfrq=", (uint32_t)rsr("cntfrq_el0"), "dtb=", (uint32_t)dtb);
	con_skip();
	/* XNU expects the boot CPU to be MPIDR 0 (cpu0, cluster 0); warn loudly if not. */
	if ((rsr("mpidr_el1") & 0xffff) != 0)
		con_str("WARN", "boot CPU is not MPIDR 0");
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	stage = "done";
	reset(40);
}
