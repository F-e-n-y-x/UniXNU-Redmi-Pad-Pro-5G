/* ruan macOS 27.3 (xnu-12377) boot-progress hooks for USB debugging, without the iPadOS boot-progress page.
 *
 * The OS profile's zzz-boot-progress.patch calls these (patches/xnu-12377/ruan-usbdbg-early.patch makes the header declare
 * them always and tees the console into boot_progress_putc). boot_progress_active() stays 0, so the verbose console is
 * unchanged. With ruan-ramoops=1 the console text, "[ev]" lines (kernel stages, exec stages, faults, failed exits) and a
 * once-a-second "[hb]" heartbeat go to the console zone of ruan's stock ramoops region in Linux persistent_ram format
 * (same zone and format as the iPadOS package/BootProgress.c); package/UsbDebugCdc.cpp streams that zone over USB and
 * replays it after a reset (DarwinBoot "upload" or Android pstore give it back as well). */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <kern/boot_progress.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern int snprintf(char *, size_t, const char *, ...) __printflike(3, 4);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern wait_result_t assert_wait_timeout(event_t, wait_interrupt_t, uint32_t, uint32_t);
extern wait_result_t thread_block(thread_continue_t);
extern unsigned int vm_page_free_count;
extern char ruan_usb_short[48];   /* package/UsbDebugCdc.cpp: "usb e.. r.. c.." or why it stopped */

#define ZONE_BASE  0x80D80000UL
#define ZONE_SIZE  0x80000UL
#define ZONE_SIG   0x43474244U          /* "DBGC" */
#define ZONE_HEAD  12U
#define ZONE_DATA  (ZONE_SIZE - ZONE_HEAD)

static unsigned ramoops;
static volatile uint8_t *zone;
static char zone_early[16384];
static unsigned zone_early_len;
static volatile int zone_lock;
static uint64_t freq;
static volatile uint64_t irqs, aborts, execs, exits, last_event_ms;
static char last_event[96];

/* interrupts masked while held, so the holder is never preempted on one CPU */
static void
zone_put(const char *text, unsigned n)
{
	uint64_t daif;
	if (!ramoops || !n) {
		return;
	}
	__asm__ volatile ("mrs %0, daif\n\tmsr daifset, #3" : "=r"(daif) :: "memory");
	while (__atomic_exchange_n(&zone_lock, 1, __ATOMIC_ACQUIRE)) {
	}
	if (!zone) {
		for (unsigned i = 0; i < n && zone_early_len < sizeof(zone_early); i++) {
			zone_early[zone_early_len++] = text[i];
		}
	} else {
		volatile uint32_t *head = (volatile uint32_t *)zone;
		uint32_t start = head[1], size = head[2];
		for (unsigned i = 0; i < n; i++) {
			zone[ZONE_HEAD + start] = (uint8_t)text[i];
			start = start + 1 == ZONE_DATA ? 0 : start + 1;
		}
		head[1] = start;
		head[2] = size + n > ZONE_DATA ? ZONE_DATA : size + n;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	__atomic_store_n(&zone_lock, 0, __ATOMIC_RELEASE);
	__asm__ volatile ("msr daif, %0" :: "r"(daif) : "memory");
}

static uint64_t
ms(void)
{
	if (!freq) {
		freq = __builtin_arm_rsr64("CNTFRQ_EL0");
	}
	return freq ? __builtin_arm_rsr64("CNTVCT_EL0") / (freq / 1000) : 0;
}

static void
event(const char *s)
{
	char line[160];
	uint64_t t = ms();
	last_event_ms = t;
	strlcpy(last_event, s, sizeof(last_event));
	int n = snprintf(line, sizeof(line), "\n[ev] %5llu.%03llu %s\n", t / 1000, t % 1000, s);
	zone_put(line, n > 0 ? ((unsigned)n < sizeof(line) ? (unsigned)n : sizeof(line) - 1) : 0);
}

void
boot_progress_init(void)
{
	PE_parse_boot_argn("ruan-ramoops", &ramoops, sizeof(ramoops));
	event("kernel entry (macOS 27.3, xnu-12377 arm64)");
}

int
boot_progress_active(void)
{
	return 0;   /* never divert the console: the hooks only record */
}

void
boot_progress_putc(char c)
{
	if (c != '\r') {
		zone_put(&c, 1);
	}
}

void
boot_progress_bind(uintptr_t base, unsigned width, unsigned height, unsigned rowbytes, unsigned depth)
{
	(void)base; (void)width; (void)height; (void)rowbytes; (void)depth;
}

void
boot_progress_stage(const char *s)
{
	event(s ? s : "?");
}

void
boot_progress_irq(uint64_t pc, uint64_t cpsr, uint64_t sp)
{
	(void)pc; (void)cpsr; (void)sp;
	irqs++;
}

void
boot_progress_poll(void)
{
}

void
boot_progress_user_breakpoint(void *state, unsigned esr)
{
	char s[64];
	(void)state;
	snprintf(s, sizeof(s), "user breakpoint esr 0x%x", esr);
	event(s);
}

void
boot_progress_user_abort(void *state, unsigned esr, uint64_t far)
{
	(void)state;
	if (aborts++ < 64) {   /* user faults can repeat without bound; the first ones tell the story */
		char s[80];
		snprintf(s, sizeof(s), "user abort esr 0x%x far 0x%llx", esr, far);
		event(s);
	}
}

void
boot_progress_critical_exit(int pid, char *text, unsigned long size)
{
	char s[128];
	snprintf(s, sizeof(s), "critical exit pid %d: %.96s", pid, text && size ? text : "");
	event(s);
}

void
boot_progress_process_stage(int pid, const char *name, const char *stage)
{
	char s[128];
	if (stage && !strcmp(stage, "exec load_machfile enter")) {
		execs++;
	}
	snprintf(s, sizeof(s), "pid %d %.24s: %.80s", pid, name ? name : "?", stage ? stage : "?");
	event(s);
}

void
boot_progress_exit(int pid, const char *name, int rv, unsigned ns, uint64_t code)
{
	exits++;
	if (rv) {
		char s[128];
		snprintf(s, sizeof(s), "exit pid %d %.24s rv 0x%x reason ns %u code 0x%llx", pid, name ? name : "?", rv, ns, code);
		event(s);
	}
}

/* The heartbeat: proves the scheduler still runs a kernel thread; the USB thread answers `cmd info`/`dmesg` independently. */
static void
heartbeat(void *arg __unused, wait_result_t wr __unused)
{
	for (;;) {
		char line[192];
		uint64_t t = ms();
		int n = snprintf(line, sizeof(line), "\n[hb] %llu.%03llu free=%u irq=%llu exec=%llu exit=%llu idle=%llus %.47s | %.60s\n",
		    t / 1000, t % 1000, vm_page_free_count, irqs, execs, exits, (t - last_event_ms) / 1000, ruan_usb_short, last_event);
		zone_put(line, n > 0 ? ((unsigned)n < sizeof(line) ? (unsigned)n : sizeof(line) - 1) : 0);
		assert_wait_timeout((event_t)&heartbeat, THREAD_UNINT, 1000, 1000 * 1000);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

/* kernel_bootstrap_thread after clock_service_create: the VM is up, map the zone and start the heartbeat */
void
boot_progress_start(void)
{
	if (ramoops && !zone) {
		volatile uint8_t *z = (volatile uint8_t *)ml_io_map(ZONE_BASE, ZONE_SIZE);
		if (z) {
			volatile uint32_t *head = (volatile uint32_t *)z;
			head[1] = 0;
			head[2] = 0;
			head[0] = ZONE_SIG;
			zone = z;
			zone_put(zone_early, zone_early_len);
		}
	}
	event("boot_progress_start");
	if (ramoops) {
		thread_t th = THREAD_NULL;
		if (kernel_thread_start(heartbeat, NULL, &th) == KERN_SUCCESS) {
			thread_deallocate(th);
		}
	}
}
