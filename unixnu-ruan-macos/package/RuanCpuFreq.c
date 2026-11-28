// SPDX-License-Identifier: BSD-2-Clause-Patent
// ruan-cpufreq=<n>: CPU frequency vote for ruan's EPSS domains (SM7435, live DT qcom,cpufreq-hw-epss: freq-domain0 17d91000 =
// cpus 0-3 A55, freq-domain1 17d92000 = cpus 4-7 A78). Layout of qcom-cpufreq-hw's "epss" data, as UniXNU-Dizi-Ruan's
// CpuFreqProbe.cpp: domain_state 0x20, dcvs_ctrl 0xb0, LUT 0x100 (4-byte rows: 31:30 source, 18:16 cores, 7:0 L value x 19.2 MHz),
// perf_state 0x320 (+4*i per core when dcvs_ctrl bit 0 is set). The DT has qcom,skip-enable-check, so reg 0x0 is not checked.
//   1  report each domain's current level and LUT top (printf, so it reaches the boot page)
//   2  also vote the highest non-turbo level in both domains, once each domain's state reads non-zero (cluster up),
//      then run the profile thread below
// RUAN_CPUFREQ_DEFAULT (macOS build: 2) is the mode without the boot-arg; ruan-cpufreq=0 turns it off.
//
// Profiles (mode 2): cpuprof=0|1|2 at boot (values > 2 = 0, the same arg and numbering as RuanGpuPower.c), USB `cpuprof N` at run
// time. ruan_perf_profile holds the live one for both governors.
//   0 performance (default): the fixed top vote above and nothing else, as before this change. The thread sleeps until USB
//     changes the profile; no L3 vote, no thermal cap.
//   1 balanced: every 16 ms a domain's load is its busiest CPU's busy share (1 - recount idle time over the window; the idle
//     thread's spin counts as idle, processor_idle marks the CPU idle before its loop). It gets the lowest level at or above
//     1.25 x load x its current clock (schedutil's margin), the top at 90 % busy; up at once, down after 4 windows asking for
//     less. Floor: d0 >= 1000 MHz (cpu0 takes the USB/UFS interrupts), d1 its lowest level. A touch report holds both domains at
//     their cap for ruan-cpufreq-boost-ms (default 1000, 0 off). The L3 follows the faster domain.
//   2 powersave: the same governor, d0 within 600..1400 MHz, d1 at most 1500 MHz, no touch boost.
// Profiles 1-2: L3 = the live DT's qcom,dcvs/l3 (l3-base 17d90000, LUT 0x100, vote register 0x90 = qcom,shared-offset of "sp");
// big-core thermal cap from tsens0 TM 0xc263000 sensors 3..10 (TM_Sn_STATUS 0xa0 + 4n, bits 11:0 signed deci-C, bit 21 valid):
// the hottest reading is averaged over 4 windows, over 90 C domain 1 is held at or below its middle level, released under 85 C
// and not before 1 s. ruan_screen_off (RuanInput.c, weak: absent = screen on) drops both domains to their floor.
// CPU -> domain from the topology's MPIDR (Aff1 >= 4 = A78), not the XNU cpu number. A CPU XNU has not started counts as idle.
#include <stdint.h>
#ifndef RUAN_CPUFREQ_HOST
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/recount.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/cpu_topology.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
extern wait_result_t assert_wait_timeout(event_t, wait_interrupt_t, uint32_t, uint32_t);
#endif

enum {kRows = 40, kState = 0x20, kDcvs = 0xb0, kLut = 0x100, kPerf = 0x320, kL3Vote = 0x90};

// Highest non-turbo row of a LUT (*mhz its clock), as qcom_cpufreq_hw_read_lut: the table ends where a frequency repeats, a row
// with core count 1 past the first is a turbo row. -1 for an empty table.
int ruan_cpufreq_pick(const uint32_t *lut, unsigned rows, unsigned *mhz)
{
	int best = -1;
	unsigned prev = 0;
	for (unsigned i = 0; i < rows; i++) {
		uint32_t d = lut[i];
		unsigned f = (d >> 30) ? (d & 0xff) * 192 / 10 : 300;
		if (!(d & 0xff) || (i && f == prev)) {
			break;
		}
		prev = f;
		if (i && ((d >> 16) & 7) == 1) {
			continue;
		}
		best = (int)i;
		*mhz = f;
	}
	return best;
}

// A LUT's levels in ascending clock (row index, MHz), turbo rows left out when skip_turbo (the L3's core count field means
// nothing). Same end rule as ruan_cpufreq_pick. Returns the count.
unsigned ruan_cpufreq_levels(const uint32_t *lut, unsigned rows, int skip_turbo, uint8_t *row, uint16_t *mhz)
{
	unsigned n = 0, prev = 0;
	for (unsigned i = 0; i < rows; i++) {
		uint32_t d = lut[i];
		unsigned f = (d >> 30) ? (d & 0xff) * 192 / 10 : 300;
		if (!(d & 0xff) || (i && f == prev)) {
			break;
		}
		prev = f;
		if (skip_turbo && i && ((d >> 16) & 7) == 1) {
			continue;
		}
		unsigned at = n++;
		while (at && mhz[at - 1] > f) {
			mhz[at] = mhz[at - 1], row[at] = row[at - 1], at--;
		}
		mhz[at] = (uint16_t)f, row[at] = (uint8_t)i;
	}
	return n;
}

// A profile's level window: *lo the lowest level at or above floor_mhz, *hi the highest at or below cap_mhz (never below *lo).
void ruan_cpufreq_window(const uint16_t *mhz, unsigned n, unsigned floor_mhz, unsigned cap_mhz, unsigned *lo, unsigned *hi)
{
	unsigned l = 0, h = n - 1;
	while (l + 1 < n && mhz[l] < floor_mhz) {
		l++;
	}
	while (h > l && mhz[h] > cap_mhz) {
		h--;
	}
	*lo = l, *hi = h;
}

// A tsens-v2 TM_Sn_STATUS word in deci-C, INT32_MIN when the valid bit (21) is clear.
int32_t ruan_tsens_dc(uint32_t v)
{
	return (v & (1u << 21)) ? (int32_t)(v << 20) >> 20 : INT32_MIN;
}

// The level a domain wants for a window, from its current level and its busiest CPU's busy share (per mille).
unsigned ruan_cpufreq_want(const uint16_t *mhz, unsigned n, unsigned cur, unsigned busy_pm)
{
	if (busy_pm >= 900) {
		return n - 1;
	}
	uint32_t need = (uint32_t)mhz[cur] * busy_pm * 5 / 4000;
	unsigned w = 0;
	while (w + 1 < n && mhz[w] < need) {
		w++;
	}
	return w;
}

#if defined(RUAN_CPUFREQ_HOST) && defined(RUAN_CPUFREQ_SELFTEST)
// cc -DRUAN_CPUFREQ_HOST -DRUAN_CPUFREQ_SELFTEST RuanCpuFreq.c && ./a.out
#include <assert.h>
int main(void)
{
	// source 0 row (300), 691, 1094, 1958, a turbo row 2400 (cores 1), then a repeat
	uint32_t lut[] = {0x00000001, 0x40030024, 0x40030039, 0x40030066, 0x4001007d, 0x4001007d};
	uint8_t row[8];
	uint16_t mhz[8];
	unsigned lo, hi;
	unsigned n = ruan_cpufreq_levels(lut, 6, 1, row, mhz);
	assert(n == 4 && mhz[0] == 300 && mhz[3] == 1958 && row[3] == 3);
	assert(ruan_cpufreq_levels(lut, 6, 0, row, mhz) == 5 && row[4] == 4);
	n = ruan_cpufreq_levels(lut, 6, 1, row, mhz);
	assert(ruan_cpufreq_want(mhz, n, 3, 950) == 3);         // hispeed jump
	assert(ruan_cpufreq_want(mhz, n, 3, 0) == 0);           // idle: the bottom
	assert(ruan_cpufreq_want(mhz, n, 3, 400) == 2);         // 1958 x 0.4 x 1.25 = 979 -> 1094
	assert(ruan_cpufreq_want(mhz, n, 0, 800) == 0);         // 300 x 0.8 x 1.25 = 300 -> 300
	ruan_cpufreq_window(mhz, n, 1000, 0xffff, &lo, &hi);    // balanced d0: 1094..1958
	assert(lo == 2 && hi == 3);
	ruan_cpufreq_window(mhz, n, 600, 1400, &lo, &hi);       // powersave d0: 691..1094
	assert(lo == 1 && hi == 2);
	ruan_cpufreq_window(mhz, n, 0, 100, &lo, &hi);          // a cap under every level: the bottom, never an empty window
	assert(lo == 0 && hi == 0);
	ruan_cpufreq_window(mhz, n, 5000, 0xffff, &lo, &hi);    // a floor over every level: the top
	assert(lo == 3 && hi == 3);
	assert(ruan_tsens_dc((1u << 21) | 0x384) == 900 && ruan_tsens_dc((1u << 21) | 0xff6) == -10 && ruan_tsens_dc(0x384) == INT32_MIN);
	return 0;
}
#endif

#ifndef RUAN_CPUFREQ_HOST
#ifndef RUAN_CPUFREQ_DEFAULT
#define RUAN_CPUFREQ_DEFAULT 0
#endif
static unsigned mode = RUAN_CPUFREQ_DEFAULT;
static volatile uint32_t *dom[2];       // the domain windows (USB `cpufreq`); NULL for a cluster that never came up
volatile uint32_t ruan_perf_profile;    // cpuprof: 0 performance, 1 balanced, 2 powersave; RuanGpuPower.c may read it too
extern volatile uint32_t ruan_touch_events;     // RuanInput.c: one per touch report
extern volatile uint32_t ruan_screen_off __attribute__((weak));   // RuanInput.c once the GPU governor patch is in
static volatile uint32_t busy_last[2], boosting, l3_row = ~0u;  // last window's busiest CPU per domain (per mille), touch boost, L3 vote
volatile int32_t ruan_cpufreq_temp_dc = INT32_MIN;      // filtered hottest big-core tsens reading, deci-C
volatile uint32_t ruan_cpufreq_capped;                  // windows with domain 1 thermally capped

// Per profile: d0/d1 floor and cap (MHz), touch boost.
// 0: performance (fixed top), 1: schedutil (dynamic balanced), 2: powersave, 3: ondemand
static const struct {
	uint16_t floor[2], cap[2];
	uint8_t boost;
} prof[4] = {
	{{0, 0}, {0xffff, 0xffff}, 0},
	{{1000, 0}, {0xffff, 0xffff}, 1},
	{{600, 0}, {1400, 1500}, 0},
	{{600, 0}, {0xffff, 0xffff}, 1},
};

static void
vote(volatile uint32_t *r, uint32_t row)
{
	unsigned cores = (r[kDcvs / 4] & 1) ? 4 : 1;   // ruan: 4 CPUs per domain
	for (unsigned i = 0; i < cores; i++) {
		r[(kPerf + 4 * i) / 4] = row;
	}
}

static void
domain(unsigned d, uint64_t phys)
{
	volatile uint32_t *r = (volatile uint32_t *)ml_io_map(phys, 0x1000);
	uint32_t lut[kRows];
	unsigned waited = 0, mhz = 0;
	if (!r) {
		printf("ruan-cpufreq: d%u map failed\n", d);
		return;
	}
	while (!(r[kState / 4] & 0xff) && waited++ < 300) {   // a cluster whose cores are all off reads 0
		assert_wait_timeout((event_t)&mode, THREAD_UNINT, 100, NSEC_PER_MSEC);
		thread_block(THREAD_CONTINUE_NULL);
	}
	for (unsigned i = 0; i < kRows; i++) {
		lut[i] = r[(kLut + 4 * i) / 4];
	}
	int top = ruan_cpufreq_pick(lut, kRows, &mhz);
	printf("ruan-cpufreq: d%u state %u MHz, top row %d = %u MHz, dcvs 0x%x\n", d, (r[kState / 4] & 0xff) * 192 / 10, top, mhz,
	    r[kDcvs / 4]);
	dom[d] = r;
	if (mode < 2 || top < 0 || !(r[kState / 4] & 0xff)) {   // no vote for a cluster that never came up (cpus=1): the governor skips it too
		return;
	}
	vote(r, (uint32_t)top);
	assert_wait_timeout((event_t)&mode, THREAD_UNINT, 200, NSEC_PER_MSEC);
	thread_block(THREAD_CONTINUE_NULL);
	printf("ruan-cpufreq: d%u voted row %d, state now %u MHz\n", d, top, (r[kState / 4] & 0xff) * 192 / 10);
}

// The profile loop (see the header). Level sets 0, 1 = the domains, 2 = the L3.
static void
governor(void)
{
	static uint8_t row[3][kRows];
	static uint16_t mhz[3][kRows];
	volatile uint32_t *win[3] = {dom[0], dom[1], (volatile uint32_t *)ml_io_map(0x17d90000, 0x1000)};
	volatile uint32_t *tm = (volatile uint32_t *)ml_io_map(0x0c263000, 0x1000);   // tsens0 TM (live DT thermal-sensor@c263000)
	const ml_topology_info_t *topo = ml_get_topology_info();
	unsigned n[3] = {0}, cur[3] = {0}, down[3] = {0}, boost_ms = 1000, was = 0, cap_hold = 0;
	uint64_t idle[MAX_CPUS] = {0}, seen = 0, last = 0, boost_abs = 0, boost_end = 0;
	uint32_t lut[kRows], touches = ruan_touch_events;
	int32_t temp = INT32_MIN;

	PE_parse_boot_argn("ruan-cpufreq-boost-ms", &boost_ms, sizeof(boost_ms));
	nanoseconds_to_absolutetime((uint64_t)boost_ms * NSEC_PER_MSEC, &boost_abs);
	for (unsigned d = 0; d < 3; d++) {
		if (!win[d]) {
			continue;
		}
		for (unsigned i = 0; i < kRows; i++) {
			lut[i] = win[d][(kLut + 4 * i) / 4];
		}
		n[d] = ruan_cpufreq_levels(lut, kRows, d < 2, row[d], mhz[d]);
		cur[d] = n[d] ? n[d] - 1 : 0;   // domain() left the domains at the top
	}
	printf("ruan-cpufreq: profile %u, d0 %u levels, d1 %u, l3 %u (top %u MHz)\n", ruan_perf_profile, n[0], n[1], n[2],
	    n[2] ? mhz[2][n[2] - 1] : 0);
	for (;;) {
		const unsigned p = ruan_perf_profile <= 3 ? ruan_perf_profile : 0;
		if (p == 0) {
			if (was != 0) {         // back from a governed profile: today's fixed top, the L3 at its top too
				for (unsigned d = 0; d < 3; d++) {
					if (n[d] && win[d]) {
						cur[d] = n[d] - 1;
						if (d < 2) {
							vote(win[d], row[d][cur[d]]);
						} else if (l3_row != ~0u) {
							win[2][kL3Vote / 4] = l3_row = row[2][cur[2]];
						}
					}
				}
				boosting = 0;
			}
			was = 0, last = 0, seen = 0;
			assert_wait((event_t)&ruan_perf_profile, THREAD_UNINT);   // no wakeups in performance: USB `cpuprof` wakes it
			if (ruan_perf_profile != 0) {
				clear_wait(current_thread(), THREAD_AWAKENED);
				continue;
			}
			thread_block(THREAD_CONTINUE_NULL);
			continue;
		}
		uint64_t t = mach_absolute_time(), dt = t - last;
		unsigned busy[2] = {0, 0}, frac = 0, lo[2], hi[2];

		for (unsigned i = 0; topo && i < topo->num_cpus && i < MAX_CPUS; i++) {
			const ml_topology_cpu_t *c = &topo->cpus[i];
			processor_t pr = cpu_to_processor((int)c->cpu_id);
			struct recount_usage usage = {};
			uint64_t now_idle = 0;
			if (pr == PROCESSOR_NULL || (pr->state != PROCESSOR_RUNNING && pr->state != PROCESSOR_IDLE && pr->state != PROCESSOR_DISPATCHING)) {
				seen &= ~(1ull << i);   // not started (cpus=N) or offline: idle, and a fresh baseline once it runs
				continue;
			}
			recount_processor_usage(&pr->pr_recount, &usage, &now_idle);
			uint64_t di = now_idle - idle[i];
			idle[i] = now_idle;
			unsigned b = (!last || !(seen & (1ull << i)) || di >= dt) ? 0 : (unsigned)((dt - di) * 1000 / dt);
			seen |= 1ull << i;
			unsigned d = ((c->phys_id >> 8) & 0xff) >= 4;   // MPIDR Aff1: cores 4-7 are the A78s (freq-domain1)
			busy[d] = b > busy[d] ? b : busy[d];
		}
		if (!last) {    // first window after boot or a switch from 0: only the idle baselines, so no drop to the floor on no data
			last = t, was = p;
			assert_wait_timeout((event_t)&mode, THREAD_UNINT, 16, NSEC_PER_MSEC);
			thread_block(THREAD_CONTINUE_NULL);
			continue;
		}
		int32_t dc = INT32_MIN;
		for (unsigned s = 3; tm && s <= 10; s++) {
			int32_t v = INT32_MIN;
			for (unsigned i = 0; i < 3 && v == INT32_MIN; i++) {     // tsens.c: up to three reads for the valid bit
				v = ruan_tsens_dc(tm[(0xa0 + 4 * s) / 4]);
			}
			dc = v > dc ? v : dc;
		}
		// The hottest of 8 sensors swings 83-93 C between 16 ms windows (iPadOS log: the cap flipped every window), so average it.
		temp = dc == INT32_MIN ? INT32_MIN : temp == INT32_MIN ? dc : (3 * temp + dc) / 4;
		ruan_cpufreq_temp_dc = temp;
		const int screen_off = &ruan_screen_off && ruan_screen_off;
		for (unsigned d = 0; d < 2; d++) {
			if (n[d]) {
				ruan_cpufreq_window(mhz[d], n[d], prof[p].floor[d], prof[p].cap[d], &lo[d], &hi[d]);
			}
		}
		if (n[1]) {
			const unsigned mid = (n[1] - 1) / 2;
			unsigned capped = cap_hold != 0;
			if (temp > 900 || (capped && (temp > 850 || cap_hold > 1))) {
				cap_hold = temp > 900 ? 64 : cap_hold - (cap_hold > 1);   // 64 windows, about 1 s, after the last reading over 90 C
				hi[1] = hi[1] < mid ? hi[1] : mid;
				lo[1] = lo[1] < hi[1] ? lo[1] : hi[1];
				ruan_cpufreq_capped++;
			} else {
				cap_hold = 0;
			}
			if (capped != (cap_hold != 0)) {
				printf("ruan-cpufreq: d1 thermal cap %s at %d dC\n", capped ? "off" : "on", temp);
			}
		}
		busy_last[0] = busy[0], busy_last[1] = busy[1];
		if (last && boost_abs && ruan_touch_events != touches) {
			touches = ruan_touch_events;
			boost_end = t + boost_abs;
		}
		boosting = prof[p].boost && t < boost_end && !screen_off;
		for (unsigned d = 0; d < 3; d++) {
			if (!n[d] || !win[d]) {
				continue;
			}
			unsigned want, l = d < 2 ? hi[d] : n[2] - 1;
			if (d < 2) {
				if (screen_off) {
					want = lo[d];
				} else if (boosting || (p == 3 && busy[d] >= 800)) {
					want = hi[d];
				} else {
					want = ruan_cpufreq_want(mhz[d], n[d], cur[d], busy[d]);
				}
				want = want > lo[d] ? want : lo[d];
			} else {
				for (want = 0; want + 1 < n[2] && (uint32_t)mhz[2][want] * 1000 < (uint32_t)mhz[2][n[2] - 1] * frac; want++) {
				}
			}
			want = want < l ? want : l;
			if (want > cur[d] || (want < cur[d] && (cur[d] > l || was != p || ++down[d] >= 4))) {
				cur[d] = want;
				down[d] = 0;
				if (d < 2) {
					vote(win[d], row[d][want]);
				} else {
					win[2][kL3Vote / 4] = l3_row = row[2][want];
				}
			} else if (want >= cur[d]) {
				down[d] = 0;
			}
			if (d < 2) {
				unsigned f = (uint32_t)mhz[d][cur[d]] * 1000 / mhz[d][n[d] - 1];
				frac = f > frac ? f : frac;
			}
		}
		was = p;
		last = t;
		assert_wait_timeout((event_t)&mode, THREAD_UNINT, 16, NSEC_PER_MSEC);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

static void
run(void *arg __unused, wait_result_t wr __unused)
{
	domain(0, 0x17d91000);
	domain(1, 0x17d92000);
	if (mode >= 2 && (dom[0] || dom[1])) {
		governor();
	}
}

static volatile uint32_t started;   // BootProgress.c and RuanMacBootProgress.c both call ruan_cpufreq_start: one thread only

// USB `cpuprof N` (UsbDebugCdc.cpp weak import): switch the profile at run time. Returns 0, or -1 for a bad N or no governor.
int
ruan_cpufreq_set_profile(unsigned p)
{
	if (p > 3 || mode < 2 || !started || (!dom[0] && !dom[1])) {
		return -1;
	}
	ruan_perf_profile = p;
	thread_wakeup((event_t)&ruan_perf_profile);
	return 0;
}

// USB `cpufreq` (UsbDebugCdc.cpp weak import): mode, profile, both domains' live clock (0 = not read yet), the L3 vote row
// (- = never voted), last window's busiest CPU per domain (per mille), filtered big-core temperature, capped windows, touch boost.
void
ruan_cpufreq_report(char *out, unsigned len)
{
	unsigned s0 = dom[0] ? (dom[0][kState / 4] & 0xff) * 192 / 10 : 0, s1 = dom[1] ? (dom[1][kState / 4] & 0xff) * 192 / 10 : 0;
	snprintf(out, len, "mode %u prof %u d0 %u MHz d1 %u MHz l3 row %d busy %u/%u temp %d dC capped %u boost %u", mode,
	    ruan_perf_profile, s0, s1, (int)l3_row, busy_last[0], busy_last[1], ruan_cpufreq_temp_dc, ruan_cpufreq_capped, boosting);
}

void
ruan_cpufreq_start(void)
{
	thread_t thread;
	unsigned p = 1;
	if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) {
		return;
	}
	PE_parse_boot_argn("ruan-cpufreq", &mode, sizeof(mode));
	PE_parse_boot_argn("cpuprof", &p, sizeof(p));
	ruan_perf_profile = p <= 3 ? p : 1;
	if (!mode) {
		return;
	}
	if (kernel_thread_start(run, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif
