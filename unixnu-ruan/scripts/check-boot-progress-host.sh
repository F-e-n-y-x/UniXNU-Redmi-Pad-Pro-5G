#!/bin/bash
# Render package/BootProgress.c's page on the host (no tablet): a scripted boot (kernel stages, launchd, daemons,
# backboardd crash, console lines) into a 1600x2560 buffer, written as $OUT/boot-progress.ppm (+ .png with Pillow).
# Needs clang and the patched XNU work tree (for iso_font.c).  Usage: scripts/check-boot-progress-host.sh [out-dir]
set -euo pipefail
D=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$(mktemp -d)}; mkdir -p "$OUT"
XNU=${XNU:-${UNIXNU_WORK:-/root/unixnu-work}/UniXNU/out/ruan/ipados-17.7.11-21H461/ruan/tree/.cache/xnu-work}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/kern" "$T/pexpert"
sed -n '/^void boot_progress_init/,/^void boot_progress_exit/p' "$XNU/osfmk/kern/boot_progress.h" > "$T/kern/boot_progress.h"
sed -i '1i #include <stdint.h>' "$T/kern/boot_progress.h"
cat > "$T/kern/thread.h" <<'EOF'
#include <stddef.h>
typedef void *thread_t, *event_t; typedef int wait_result_t, wait_interrupt_t, kern_return_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
#define THREAD_UNINT 0
#define THREAD_CONTINUE_NULL ((thread_continue_t)0)
#define NSEC_PER_MSEC 1000000ull
#define MAXPRI_KERNEL 95
#define KERN_SUCCESS 0
#define __printflike(a, b)
#define __unused __attribute__((unused))
static inline kern_return_t kernel_thread_start_priority(thread_continue_t f, void *a, int p, thread_t *t) { (void)f, (void)a, (void)p, (void)t; return 1; }
static inline void thread_set_thread_name(thread_t t, const char *n) { (void)t, (void)n; }
static inline void thread_deallocate(thread_t t) { (void)t; }
EOF
cat > "$T/pexpert/pexpert.h" <<'EOF'
#include <string.h>
static inline int PE_parse_boot_argn(const char *n, void *v, int s) { (void)s; *(unsigned *)v = 1; return !strcmp(n, "boot-progress"); }
static inline int PE_parse_boot_arg_str(const char *n, char *v, int s) { (void)n; strlcpy(v, "host", s); return 1; }
#define PAGE_SIZE 4096
#define strnstr(s, f, n) strstr(s, f)
typedef unsigned long vm_size_t;
static inline int copyin(uint64_t a, void *p, size_t n) { memcpy(p, (const void *)(uintptr_t)a, n); return 0; }
static inline int copyinstr(uint64_t a, char *p, size_t n, vm_size_t *d) { (void)a, (void)n, (void)d; p[0] = 0; return 0; }
EOF
cat > "$T/main.c" <<'EOF'
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <kern/boot_progress.h>
#include <kern/thread.h>
int nprocs = 42;
uint64_t max_mem = 5561ull << 20, arm_vm_outside_base, arm_vm_outside_end;
unsigned long gPhysBase, real_phys_size;
uint32_t ml_outside_memory_pages, ml_outside_memory_refused;
int host_event_has(const char *s);
int host_pin_has(unsigned i, const char *s);
void proc_selfname(char *b, int n) { snprintf(b, n, "mount"); }
void ram_data_describe(char *b, size_t n) { snprintf(b, n, "/private/var now: host"); }
void boot_progress_early_write(int, const uint64_t *);
uint64_t cnt = 1;
uint64_t host_rsr(const char *r) { return r[3] == 'F' ? 19200000 : cnt; }
wait_result_t assert_wait_timeout(event_t e, wait_interrupt_t i, uint32_t a, uint32_t b) { (void)e, (void)i, (void)a, (void)b; return 0; }
wait_result_t thread_block(thread_continue_t c) { (void)c; return 0; }
static void out(const char *s) { while (*s) boot_progress_putc(*s++); }
int main(int argc, char **argv) {
	enum { W = 1600, H = 2560 };
	uint32_t *fb = calloc(W * H, 4);
	boot_progress_init();
	assert(boot_progress_active());
	boot_progress_stage("arm_vm_init enter"); cnt += 19200000; boot_progress_stage("arm_vm_init returned");
	out("kprintf line before the framebuffer\n");
	boot_progress_bind((uintptr_t)fb, W, H, W * 4, 32);
	const char *k[] = {"vm_mem_bootstrap", "sched_startup", "PE_init_iokit", "bsd_init enter", "bsd_init: vfs_mountroot\n",
	                   "vfs_mountroot enter", "vfs_mountroot mounted; authentication pending", "load_init_program enter"};
	for (unsigned i = 0; i < sizeof(k) / sizeof(*k); i++) { cnt += 19200000; boot_progress_stage(k[i]); }
	boot_progress_process_stage(1, "launchd", "exec process signature accepted");
	const char *d[] = {"logd", "notifyd", "configd", "UserEventAgent", "keybagd", "xpcproxy", "backboardd", "SpringBoard"};
	for (unsigned i = 0; i < sizeof(d) / sizeof(*d); i++) { cnt += 9600000; boot_progress_process_stage(30 + i, d[i], "exec process signature accepted"); }
	boot_progress_process_stage(99, "refused", "exec process signature REFUSED");
	boot_progress_exit(35, "xpcproxy", 0, 0, 0);
	boot_progress_exit(36, "backboardd", 0x6, 1, 0x1234);
	for (int i = 0; i < 40; i++) { char b[64]; snprintf(b, sizeof(b), "console line %d\n", i); out(b); }
	/* launchd lines about backboardd / bootstrap mode reach the events panel without the launchd|date prefix */
	out("com.apple.xpc.launchd|2026-11-13 03:25:13.234448 (user/501/com.apple.backboardd) <Error>: Service could not "
	    "initialize: access(/usr/libexec/backboardd, X_OK) failed with errno 2 - No such file or directory\n");
	out("com.apple.xpc.launchd|2026-11-13 03:25:13.1 (system) <Notice>: entering bootstrap mode\n");
	out("com.apple.xpc.launchd|2026-11-13 03:25:13.2 (user/501) <Notice>: pending spawn: com.apple.other\n");
	assert(host_event_has("L (user/501/com.apple.backboardd) <Error>: Service could not"));
	assert(host_event_has("  ..")); /* the long line's second row */
	assert(host_event_has("L (system) <Notice>: entering bootstrap mode"));
	assert(!host_event_has("com.apple.other"));
	/* a boot task's stderr, one event per line */
	static const char msg[] = "failed to get boot-manifest-hash\nmount: second line\n";
	uint64_t wa[3] = {2, (uint64_t)(uintptr_t)msg, sizeof(msg) - 1};
	boot_progress_early_write(3, wa);
	assert(host_event_has("W mount[3] failed to get boot-manifest-hash"));
	assert(host_event_has("W mount[3] mount: second line"));
	boot_progress_stage("ram-data: role Data of / on /private/var rw: error 45, mounted from -");
	assert(host_pin_has(0, "error 45"));
	out("MOUNT-BY-ROLE (64) failed! (45)load_init_program: next\n");
	assert(host_pin_has(1, "MOUNT-BY-ROLE (64) failed! (45)"));
	FILE *f = fopen(argv[argc - 1], "wb");
	fprintf(f, "P6 %d %d 255\n", W, H);
	for (int i = 0; i < W * H; i++) { fputc(fb[i] >> 16 & 255, f); fputc(fb[i] >> 8 & 255, f); fputc(fb[i] & 255, f); }
	fclose(f);
	assert(fb[0] == 0xc0c0c0);   /* title bar painted */
	return 0;
}
EOF
{ echo '#include <stdint.h>'; echo 'uint64_t host_rsr(const char *);'
  sed 's/__builtin_arm_rsr64(\("[A-Z_0-9]*"\))/host_rsr(\1)/g' "$D/package/BootProgress.c"
  echo 'int host_event_has(const char *s) { for (unsigned i = 0; i < RING; i++) if (strstr(ev[i], s)) return 1; return 0; }'
  echo 'int host_pin_has(unsigned i, const char *s) { return strstr(pin[i], s) != NULL; }'; } > "$T/BootProgress.c"
clang -w -I"$T" -o "$T/page" "$T/main.c" "$T/BootProgress.c" "$XNU/osfmk/console/iso_font.c"
"$T/page" "$OUT/boot-progress.ppm"
python3 -c "from PIL import Image; Image.open('$OUT/boot-progress.ppm').save('$OUT/boot-progress.png')" 2>/dev/null || true
echo "PASS: $OUT/boot-progress.ppm"
