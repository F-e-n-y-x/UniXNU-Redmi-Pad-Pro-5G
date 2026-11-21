// SPDX-License-Identifier: BSD-2-Clause-Patent
// ruan: /usr/libexec/DumpPanic in the root overlay (scripts/build-wifi-userspace.py). launchd starts com.apple.DumpPanic at load as root
// (RunAtLoad, Interactive, KeepAlive on crash); its job stays Apple's, in the signed launchd cache, so no cache signature is needed. This
// program runs the real DumpPanic (DumpPanic.real, the same arguments) and then keeps the WiFi module's supplicant running: started
// again after it exits, 3 s at first, backing off to 60 s while it keeps exiting at once (no en0 before ruan-wifi=5 brings it up, or no
// WiFi kernel at all), back to 3 s after a run of a minute or more. The same trick as oslogx's IOMFB_FDR_Loader wrapper.
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv)
{
    pid_t pid, supplicant = 0;
    (void)argc;
    argv[0] = (char *)"/usr/libexec/DumpPanic.real";
    posix_spawn(&pid, argv[0], NULL, NULL, argv, environ);   // reaped by the waitpid below
    char *const args[] = {(char *)"/usr/libexec/unixnu-wpa_supplicant", (char *)"-Dsheng", (char *)"-ien0",
                          (char *)"-c/private/etc/wifi/wpa_supplicant.conf", (char *)"-s", NULL};
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
