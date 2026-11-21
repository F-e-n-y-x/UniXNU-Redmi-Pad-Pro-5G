// SPDX-License-Identifier: BSD-2-Clause-Patent
// ruan: /usr/bin/IOMFB_FDR_Loader in the root overlay (scripts/build-wifi-userspace.py). launchd starts com.apple.iomfb_fdr_loader at load
// as root; its job stays Apple's, in the signed launchd cache, so no cache signature is needed. This program runs the program that held
// the slot before (RUAN_CHAIN: oslogx's wrapper, which runs Apple's loader and the log export; the same arguments) and then keeps the
// WiFi module's supplicant running: started again after it exits, 3 s at first, backing off to 60 s while it keeps exiting at once (no
// en0 before ruan-wifi=5 brings it up, or no WiFi kernel), back to 3 s after a run of a minute or more. It never exits: launchd would end
// the job's process group with it.
// rbu178: the DumpPanic slot used first is under a Protobox syscall filter on iPadOS 17 (posix_spawn and wait4 denied); this slot is not
// (oslogx's wrapper spawns from it).
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef RUAN_CHAIN
#define RUAN_CHAIN "/usr/bin/IOMFB_FDR_Loader.oslogx"
#endif

extern char **environ;

int main(int argc, char **argv)
{
    pid_t pid, supplicant = 0;
    (void)argc;
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
