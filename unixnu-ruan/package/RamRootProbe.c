/* ruan RAM-root diagnostic, ported from UniXNU-fuxi package/RamRootProbe.c (mount and user-entry stages).
 *
 * Boot-arg ram-root-probe=1: hash md0 and /sbin/launchd after the read-only APFS mount, then stop (panic) with PASS/FAIL.
 * Boot-arg ram-root-probe=2: the same checks, then let PID 1 exec /sbin/launchd and stop at its first EL0 SVC, before
 * dispatch. On an exec failure it reports what exec saw instead of the bare "Process 1 exec ... failed".
 * Without the boot-arg every hook returns at once. Every result is a panic(): ruan's -v console does not draw, the panic
 * screen does. The screen's printf knows %s %x %u %d %p (with l/ll and 0/width), nothing else. */
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/mount.h>
#include <sys/mount_internal.h>
#include <sys/proc.h>
#include <kern/thread.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <libkern/crypto/sha2.h>
#include "RamRootProbePins.h"

extern dev_t mdevlookup(int);
extern int mdevgetrange(int, uint64_t *, uint64_t *);
extern unsigned ram_restore_trust_loaded, ram_restore_trust_engineering;

void ram_root_probe_before_mount(dev_t);
void ram_root_probe_mount_failed(int);
void ram_root_probe_after_mount(dev_t, vnode_t);
void ram_root_probe_noexec(const char *, uint64_t, int);
void ram_root_probe_exec_ready(int);
void ram_root_probe_exec_failed(const char *, int);
void ram_root_probe_user_svc(uint64_t, uint32_t, uint32_t, uint64_t) __attribute__((noreturn));

volatile unsigned ram_root_probe_svc_armed;
volatile unsigned ram_root_stage;
static unsigned mode;
static uint8_t file_buffer[4096];
/* What exec_activate_image read when no image activator claimed the file (ENOEXEC). */
static uint32_t noexec_head[8];
static uint64_t noexec_size;
static int noexec_resid;
static unsigned noexec_seen;

static uint64_t
digest_head(const uint8_t *digest)
{
	uint64_t value = 0;
	for (int i = 0; i < 8; i++) {
		value = value << 8 | digest[i];
	}
	return value;
}

void
ram_root_probe_before_mount(dev_t device)
{
	uint64_t base = 0, bytes = 0;
	uint8_t digest[32];
	SHA256_CTX hash;
	if (!PE_parse_boot_argn("ram-root-probe", &mode, sizeof(mode)) || mode == 0) {
		mode = 0;
		return;
	}
	if (mode > 2) {
		panic("RAM_ROOT FAIL\nram-root-probe=%u: only 1 (mount) and 2 (user entry)", mode);
	}
	ram_root_stage = 1;
	if (device == NODEV || device != mdevlookup(0) || mdevgetrange(0, &base, &bytes) ||
	    base < VM_MIN_KERNEL_ADDRESS || bytes < RAM_ROOT_IMAGE_BYTES || base > UINT64_MAX - bytes) {
		panic("RAM_ROOT FAIL stage=1\nroot device is not the expected md0\ndev=%x md0=%x base=%llx bytes=%llu want=%llu",
		    device, mdevlookup(0), base, bytes, RAM_ROOT_IMAGE_BYTES);
	}
	if (PE_parse_boot_argn("ram-restore-trust", &(unsigned){0}, sizeof(unsigned)) && ram_restore_trust_loaded != RAM_RESTORE_ENTRIES) {
		panic("RAM_ROOT FAIL stage=1\nram-restore-trust set but the restore module did not load (%u entries)", ram_restore_trust_loaded);
	}
	ram_root_stage = 2;
	/* Only the image: DarwinBoot may round md0 up. */
	SHA256_Init(&hash);
	SHA256_Update(&hash, (const void *)(uintptr_t)base, (size_t)RAM_ROOT_IMAGE_BYTES);
	SHA256_Final(digest, &hash);
	if (memcmp(digest, ram_root_image_sha256, sizeof(digest))) {
		panic("RAM_ROOT FAIL stage=2\nmedia SHA256 mismatch: got %016llx... want %016llx...\nmd0 base=%llx bytes=%llu",
		    digest_head(digest), digest_head(ram_root_image_sha256), base, bytes);
	}
	ram_root_stage = 3;
}

void
ram_root_probe_mount_failed(int error)
{
	if (mode) {
		panic("RAM_ROOT FAIL stage=%u\nmd0 media SHA256=OK\nvfs_mountroot errno=%d; userland not started", ram_root_stage, error);
	}
}

void
ram_root_probe_after_mount(dev_t device, vnode_t root)
{
	vfs_context_t context = vfs_context_kernel();
	mount_t mount;
	vnode_t file = NULLVP;
	struct vnode_attr va;
	uint32_t head[4] = {0};
	off_t bytes = 0;
	uint8_t digest[32];
	SHA256_CTX hash;
	int error;
	if (!mode) {
		return;
	}
	ram_root_stage = 4;
	mount = root ? vnode_mount(root) : NULL;
	if (device != mdevlookup(0) || !mount || strcmp(vfs_statfs(mount)->f_fstypename, "apfs")) {
		panic("RAM_ROOT FAIL stage=4\nmounted unexpected device or filesystem: dev=%x fs=%s", device,
		    mount ? vfs_statfs(mount)->f_fstypename : "<none>");
	}
	if (!(vfs_flags(mount) & MNT_RDONLY)) {
		panic("RAM_ROOT FAIL stage=4\nmd0 mounted without the read-only flag; stopping before userland");
	}
	error = vnode_lookup("/sbin/launchd", 0, &file, context);
	if (error) {
		panic("RAM_ROOT FAIL stage=4\nmounted md0/APFS; /sbin/launchd lookup errno=%d", error);
	}
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	VATTR_WANTED(&va, va_flags);
	VATTR_WANTED(&va, va_mode);
	error = vnode_getattr(file, &va, context);
	bytes = (off_t)va.va_data_size;
	if (error || bytes != RAM_ROOT_FILE_BYTES) {
		vnode_put(file);
		panic("RAM_ROOT FAIL stage=4\nlaunchd getattr errno=%d size=%llu want=%u flags=%x mode=%x",
		    error, (uint64_t)bytes, RAM_ROOT_FILE_BYTES, va.va_flags, va.va_mode);
	}
	ram_root_stage = 5;
	SHA256_Init(&hash);
	for (off_t offset = 0; offset < bytes;) {
		int count = (int)((bytes - offset) < (off_t)sizeof(file_buffer) ? bytes - offset : (off_t)sizeof(file_buffer));
		int residual = count;
		error = vn_rdwr(UIO_READ, file, (caddr_t)file_buffer, count, offset,
		    UIO_SYSSPACE, 0, vfs_context_ucred(context), &residual, vfs_context_proc(context));
		if (error || residual) {
			vnode_put(file);
			panic("RAM_ROOT FAIL stage=5\nlaunchd read errno=%d residual=%d at=%llu flags=%x (0x20=compressed)",
			    error, residual, (uint64_t)offset, va.va_flags);
		}
		if (offset == 0) {
			memcpy(head, file_buffer, sizeof(head));
		}
		SHA256_Update(&hash, file_buffer, (size_t)count);
		offset += count;
	}
	SHA256_Final(digest, &hash);
	vnode_put(file);
	if (memcmp(digest, ram_root_file_sha256, sizeof(digest))) {
		panic("RAM_ROOT FAIL stage=5\nlaunchd SHA256 mismatch: got %016llx... want %016llx...\n"
		    "head magic=%x cpu=%x sub=%x type=%x flags=%x (0x20=compressed)",
		    digest_head(digest), digest_head(ram_root_file_sha256), head[0], head[1], head[2], head[3], va.va_flags);
	}
	if (mode == 2) {
		ram_root_stage = 7;
		return;
	}
	ram_root_stage = 6;
	panic("RAM_ROOT PASS\nroot=md0 filesystem=apfs readonly=1\nmedia=%lluMiB SHA256=OK\n"
	    "launchd=%llu bytes SHA256=OK flags=%x\nhead magic=%x cpu=%x sub=%x type=%x\nrestore-tc=%u eng=%u\nuserland not started",
	    RAM_ROOT_IMAGE_BYTES >> 20, (uint64_t)bytes, va.va_flags, head[0], head[1], head[2], head[3],
	    ram_restore_trust_loaded, ram_restore_trust_engineering);
}

void
ram_root_probe_noexec(const char *vdata, uint64_t size, int resid)
{
	memcpy(noexec_head, vdata, sizeof(noexec_head));
	noexec_size = size;
	noexec_resid = resid;
	noexec_seen++;
}

/* exec of PID 1 succeeded; its new thread is still held in FINAL_WAIT. */
void
ram_root_probe_exec_ready(int pid)
{
	if (mode == 2 && pid == 1 && ram_root_stage == 7) {
		ram_root_stage = 8;
		ram_root_probe_svc_armed = 1;
	}
}

void
ram_root_probe_exec_failed(const char *path, int error)
{
	vfs_context_t context = vfs_context_kernel();
	vnode_t file = NULLVP;
	uint32_t head[4] = {0};
	off_t bytes = -1;
	int residual = (int)sizeof(head), lookup, read = -1;
	if (!mode) {
		return;
	}
	/* Read the file again here: if this matches the RAM_ROOT pass but differs from what exec saw, exec's read is at fault. */
	lookup = vnode_lookup(path, 0, &file, context);
	if (!lookup) {
		vnode_size(file, &bytes, context);
		read = vn_rdwr(UIO_READ, file, (caddr_t)head, sizeof(head), 0, UIO_SYSSPACE, 0,
		    vfs_context_ucred(context), &residual, vfs_context_proc(context));
		vnode_put(file);
	}
	panic("RAM_USER FAIL stage=%u\nroot=md0 apfs readonly=1, media+launchd SHA256=OK\n%s exec errno=%d (8=ENOEXEC)\n"
	    "exec saw (noexec=%u): %x %x %x %x %x %x size=%llu resid=%d\n"
	    "re-read: lookup=%d read=%d resid=%d size=%llu head %x %x %x %x\nrestore-tc=%u eng=%u; no user syscall dispatched",
	    ram_root_stage, path, error, noexec_seen, noexec_head[0], noexec_head[1], noexec_head[2], noexec_head[3],
	    noexec_head[4], noexec_head[5], noexec_size, noexec_resid,
	    lookup, read, residual, (uint64_t)bytes, head[0], head[1], head[2], head[3],
	    ram_restore_trust_loaded, ram_restore_trust_engineering);
}

void
ram_root_probe_user_svc(uint64_t pc, uint32_t cpsr, uint32_t esr, uint64_t syscall)
{
	uint32_t instruction = 0;
	int pid = proc_getpid(current_proc());
	ram_root_probe_svc_armed = 0;
	if (ram_root_stage != 8 || pid != 1 || (cpsr & 0x1f) || (esr >> 26) != 0x15 || pc < 4 || (pc >> 63)) {
		panic("RAM_USER FAIL\nrefused SVC: stage=%u pid=%d pc=%llx cpsr=%x esr=%x", ram_root_stage, pid, pc, cpsr, esr);
	}
	if (copyin((user_addr_t)(pc - 4), &instruction, sizeof(instruction)) || (instruction & 0xffe0001fU) != 0xd4000001U) {
		panic("RAM_USER FAIL\ninvalid SVC instruction: pc=%llx instruction=%x", pc, instruction);
	}
	ram_root_stage = 9;
	panic("RAM_USER_ENTRY PASS\nroot=md0 apfs readonly=1, media+launchd SHA256=OK\noriginal launchd exec=OK\n"
	    "PID1 executed at EL0; SVC trapped\npc=%llx cpsr=%x esr=%x\nx16=0x%llx instruction=%x\nrestore-tc=%u eng=%u\n"
	    "stopped BEFORE first syscall dispatch",
	    pc, cpsr, esr, syscall, instruction, ram_restore_trust_loaded, ram_restore_trust_engineering);
}

/* patches/xnu/ruan-ram-data-mount.patch: mount the root container's Data volume on /private/var by role. APFS'
 * handle_mount_by_role answers "Can only be called from kernel" (45) to PID 1, which load_init_program runs as, so the
 * mount runs on a kernel thread (kernproc, kernel context) while the caller waits for it. */
static volatile int ram_data_done, ram_data_status;
extern wait_result_t assert_wait_timeout(event_t, wait_interrupt_t, uint32_t, uint32_t);
extern wait_result_t thread_block(thread_continue_t);

static void
ram_data_thread(void *arg __unused, wait_result_t wr __unused)
{
	int error = vnode_get(rootvnode);
	if (!error) {
		error = kernel_mount(rootvnode->v_mount->mnt_vfsstat.f_fstypename, NULLVP, NULLVP, "/private/var",
		    rootvnode->v_mount, 0, MNT_NOSUID | MNT_NODEV, KERNEL_MOUNT_DATAVOL, vfs_context_kernel());
		vnode_put(rootvnode);
	}
	ram_data_status = error;
	ram_data_done = 1;
}

int ram_data_mount_by_role(void);
int
ram_data_mount_by_role(void)
{
	thread_t thread;
	if (kernel_thread_start(ram_data_thread, NULL, &thread) != KERN_SUCCESS) {
		return EAGAIN;
	}
	thread_deallocate(thread);
	while (!ram_data_done) {
		/* polled every 100 ms: the mount takes far longer than that, and no wakeup can be missed */
		assert_wait_timeout((event_t)&ram_data_done, THREAD_UNINT, 100, NSEC_PER_MSEC);
		thread_block(THREAD_CONTINUE_NULL);
	}
	return ram_data_status;
}

/* The boot-progress page's third sticky row, every 5 s once launchd runs: what is mounted on /private/var, and whether
 * launchd's log directory is there (its "fopen(/private/var/log/com.apple.xpc.launchd/launchd.log): 2"). */
void ram_data_describe(char *buf, size_t size);
void
ram_data_describe(char *buf, size_t size)
{
	vnode_t var = NULLVP, logdir = NULLVP;
	int error = vnode_lookup("/private/var", 0, &var, vfs_context_kernel());
	if (error) {
		snprintf(buf, size, "/private/var: lookup error %d", error);
		return;
	}
	mount_t mp = vnode_mount(var);
	struct vfsstatfs *st = vfs_statfs(mp);
	int log_error = vnode_lookup("/private/var/log/com.apple.xpc.launchd", 0, &logdir, vfs_context_kernel());
	snprintf(buf, size, "/private/var now: %s from %s (%s)%s%s, launchd log dir %s %d", st->f_mntonname, st->f_mntfromname,
	    st->f_fstypename, rootvnode && mp == vnode_mount(rootvnode) ? " = root volume" : "",
	    (vfs_flags(mp) & MNT_RDONLY) ? " ro" : " rw", log_error ? "error" : "ok", log_error);
	if (logdir) {
		vnode_put(logdir);
	}
	vnode_put(var);
}
