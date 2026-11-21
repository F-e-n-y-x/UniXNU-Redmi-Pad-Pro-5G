// IOMFB_bics_daemon slot: runs UniXNU's unified-log export (NativeOsLogExport.mm) as root at boot, so the host can pull os_log lines.
extern "C" void nativeOsLogExport();
int main() { nativeOsLogExport(); return 0; }
