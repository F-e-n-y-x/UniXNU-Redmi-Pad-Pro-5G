// SPDX-License-Identifier: BSD-2-Clause-Patent
// ruan: /usr/bin/IOMFB_FDR_Loader in the root overlay (scripts/build-wifi-userspace.py). launchd starts com.apple.iomfb_fdr_loader at load
// as root; its job stays Apple's, in the signed launchd cache, so no cache signature is needed. This program runs the program that held
// the slot before (RUAN_CHAIN: oslogx's wrapper, which runs Apple's loader and the log export; the same arguments) and then keeps the
// WiFi module's supplicant running: started again after it exits, 3 s at first, backing off to 60 s while it keeps exiting at once (no
// en0 before ruan-wifi=5 brings it up, or no WiFi kernel), back to 3 s after a run of a minute or more. It never exits: launchd would end
// the job's process group with it.
// rbu178: the DumpPanic slot used first is under a Protobox syscall filter on iPadOS 17 (posix_spawn and wait4 denied); this slot is not
// (oslogx's wrapper spawns from it).
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef RUAN_CHAIN
#define RUAN_CHAIN "/usr/bin/IOMFB_FDR_Loader.oslogx"
#endif

extern char **environ;

// ruan-ls-rescan=<N> (N > 0) in the boot-args: delete the LaunchServices database on the kept Data volume and kill lsd, so the next lsd
// rebuilds it from installd and registers the system apps of the current root (Spotlight.app, back after the Data was made: SpringBoard's
// FBSSceneClientIdentity assert, docs/research/springboard-trap/plan.md). N >= 2 also deletes installd's LastBuildInfo.plist (its
// OS-update system-app migration). App data and preferences stay. One shot per N (marker /private/var/db/.ruan-ls-rescan-<N>; a new N
// runs again). Here and not in the kernel: done before launchd (rbu184u) it hung the boot right after the Data mount.
static void ls_rescan(void)
{
    char args[1024] = "", marker[64];
    size_t size = sizeof(args) - 1;
    const char *at = sysctlbyname("kern.bootargs", args, &size, NULL, 0) ? NULL : strstr(args, "ruan-ls-rescan=");
    const unsigned n = at ? (unsigned)strtoul(at + 15, NULL, 10) : 0;
    if (!n) return;
    snprintf(marker, sizeof(marker), "/private/var/db/.ruan-ls-rescan-%u", n);
    if (!access(marker, F_OK)) return;
    int removed = 0, failed = 0;
    glob_t found;
    if (!glob("/private/var/mobile/Containers/Data/InternalDaemon/*/Library/Caches/com.apple.LaunchServices-*.csstore", 0, NULL, &found)) {
        for (size_t i = 0; i < found.gl_pathc; i++) unlink(found.gl_pathv[i]) ? failed++ : removed++;
        globfree(&found);
    }
    if (n >= 2 && unlink("/private/var/installd/Library/MobileInstallation/LastBuildInfo.plist")) failed += errno != ENOENT;
    // every lsd (mobile's and root's): launchd starts them again on the next request, without the deleted store
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
    size_t bytes = 0;
    struct kinfo_proc *procs = NULL;
    if (!sysctl(mib, 3, NULL, &bytes, NULL, 0) && (procs = malloc(bytes += 16 * sizeof(*procs))) && !sysctl(mib, 3, procs, &bytes, NULL, 0))
        for (size_t i = 0; i < bytes / sizeof(*procs); i++)
            if (!strcmp(procs[i].kp_proc.p_comm, "lsd")) kill(procs[i].kp_proc.p_pid, SIGKILL);
    free(procs);
    if (!failed) close(open(marker, O_CREAT | O_EXCL | O_WRONLY, 0600));   // a failed run is tried again on the next boot
    FILE *log = fopen("/private/var/log/ruan-ls-rescan.log", "a");
    if (log) {
        fprintf(log, "ls-rescan %u: csstore removed %d, failed %d\n", n, removed, failed);
        fclose(log);
    }
}

// Jobs kept disabled on a kept Data volume too (make-ramroot.py DISABLED_JOBS reaches only a fresh seed): each label is set to true in
// launchd's disabled.plist, the other keys stay. Idempotent; launchd reads the file at its next start (the next boot).
static const char *const ensure_disabled[] = {"com.apple.passd"};   // passd crash-loops in FinanceKit
static void disable_jobs(void)
{
    static const char path[] = "/private/var/db/com.apple.xpc.launchd/disabled.plist", temp[] = "/private/var/db/com.apple.xpc.launchd/.disabled.plist.ruan";
    struct stat st = {.st_uid = 0, .st_gid = 0, .st_mode = 0644};
    CFMutableDictionaryRef jobs = NULL;
    CFPropertyListRef old = NULL;
    int fd = open(path, O_RDONLY), added = 0, error = 0;
    if (fd >= 0) {
        CFMutableDataRef data = CFDataCreateMutable(NULL, 0);
        UInt8 buf[4096];
        ssize_t got;
        while ((got = read(fd, buf, sizeof(buf))) > 0) CFDataAppendBytes(data, buf, got);
        error = got < 0 || fstat(fd, &st);
        close(fd);
        if (!error) old = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
        CFRelease(data);
        if (error || !old || CFGetTypeID(old) != CFDictionaryGetTypeID()) error = 1;   // unreadable: leave it alone
    } else if (errno != ENOENT) error = 1;
    if (!error) {
        jobs = old ? CFDictionaryCreateMutableCopy(NULL, 0, old) :
                     CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        for (size_t i = 0; i < sizeof(ensure_disabled) / sizeof(*ensure_disabled); i++) {
            CFStringRef label = CFStringCreateWithCString(NULL, ensure_disabled[i], kCFStringEncodingUTF8);
            if (CFDictionaryGetValue(jobs, label) != kCFBooleanTrue) {
                CFDictionarySetValue(jobs, label, kCFBooleanTrue);
                added++;
            }
            CFRelease(label);
        }
    }
    if (added) {
        CFDataRef out = CFPropertyListCreateData(NULL, jobs, kCFPropertyListXMLFormat_v1_0, 0, NULL);
        unlink(temp);
        fd = out ? open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600) : -1;
        error = fd < 0 || write(fd, CFDataGetBytePtr(out), CFDataGetLength(out)) != CFDataGetLength(out) ||
                fchown(fd, st.st_uid, st.st_gid) || fchmod(fd, st.st_mode & 07777) || fsync(fd);
        if (fd >= 0) close(fd);
        if (!error) error = rename(temp, path);
        if (error) unlink(temp);
        if (out) CFRelease(out);
    }
    if (jobs) CFRelease(jobs);
    if (old) CFRelease(old);
    FILE *log = fopen("/private/var/log/ruan-ls-rescan.log", "a");
    if (log) {
        fprintf(log, "disabled.plist: %d label(s) added, error %d\n", added, error);
        fclose(log);
    }
}

int main(int argc, char **argv)
{
    pid_t pid, supplicant = 0;
    (void)argc;
    ls_rescan();
    disable_jobs();
    argv[0] = (char *)RUAN_CHAIN;
    posix_spawn(&pid, argv[0], NULL, NULL, argv, environ);   // reaped by the waitpid below
    char *const args[] = {(char *)"/usr/libexec/unixnu-wpa_supplicant", (char *)"-Dsheng", (char *)"-ien0",
                          (char *)"-c/private/etc/wifi/wpa_supplicant.conf", (char *)"-d", (char *)"-t",
                          (char *)"-f/private/var/log/unixnu-wpa_supplicant.log", NULL};   // rbu180: its own messages (no key at -d;
                          // the file grows with every scan and join: drop -d once joins work)
    unsigned pause = 3;
    time_t started = 0;
    for (;;) {
        if (!supplicant) {
            started = time(NULL);
            if (posix_spawn(&supplicant, args[0], NULL, NULL, args, environ)) supplicant = 0;
        }
        const pid_t done = waitpid(-1, NULL, 0);
        if (done < 0 || done == supplicant) {
            supplicant = 0;
            pause = time(NULL) - started >= 60 ? 3 : (pause * 2 > 60 ? 60 : pause * 2);
            sleep(pause);
        }
    }
}
