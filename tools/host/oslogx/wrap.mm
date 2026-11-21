// /usr/bin/IOMFB_FDR_Loader (launchd RunAtLoad, root): starts the real priming tool (IOMFB_FDR_Loader.real, same arguments) and then runs
// UniXNU's unified-log export (NativeOsLogExport.mm) so the host can pull every process's os_log lines (rbu136u: poster kills).
#include <spawn.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>
extern "C" void nativeOsLogExport();
extern char **environ;
int main(int argc, char **argv) {
    pid_t pid;
    argv[0] = (char *)"/usr/bin/IOMFB_FDR_Loader.real";
    posix_spawn(&pid, argv[0], nullptr, nullptr, argv, environ);
    // OSLogStore lives in OSLog.framework; NSClassFromString only finds it once the image is loaded (rbu137u: "OSLogStore missing").
    if (!dlopen("/System/Library/Frameworks/OSLog.framework/OSLog", RTLD_NOW))
        dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_NOW);
    nativeOsLogExport();
    return 0;
}
