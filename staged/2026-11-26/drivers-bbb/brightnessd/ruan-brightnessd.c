// ruan-brightnessd: macOS's own brightness (Control Center slider, brightness keys) to ruan's panel backlight.
// CoreBrightness keeps the user brightness (DisplayServicesGetBrightness) but QuartzCore never hands it to BrightnessControl on
// this port (IOMobileFramebuffer does not pair the GOP framebuffer with it; found by macos-pixel8a, tools/akita-brightnessd), so
// this LaunchAgent watches it and writes {"brightness-nits": 16.16} to the kernel's RuanBacklight (property ruan-backlight).
// Build: xcrun --sdk macosx clang -target arm64-apple-macos26.0 -O2 -Wall -Wextra -Werror ruan-brightnessd.c
//        -framework CoreFoundation -framework CoreGraphics -framework IOKit -o ruan-brightnessd
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

static const double kMaxNits = 500.0;   // = RuanBacklight.cpp kMaxMilliNits

static io_service_t backlight(void) {
  CFMutableDictionaryRef match = IOServiceMatching("IOService");
  CFDictionaryRef prop = CFDictionaryCreate(NULL, (const void *[]){CFSTR("ruan-backlight")}, (const void *[]){kCFBooleanTrue}, 1,
                                            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (!match || !prop) return IO_OBJECT_NULL;
  CFDictionarySetValue(match, CFSTR(kIOPropertyMatchKey), prop);
  CFRelease(prop);
  return IOServiceGetMatchingService(kIOMainPortDefault, match);   // consumes match
}

int main(void) {
  void *ds = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices", RTLD_NOW);
  int (*get)(CGDirectDisplayID, float *) = ds ? (int (*)(CGDirectDisplayID, float *))dlsym(ds, "DisplayServicesGetBrightness") : NULL;
  if (!get) { fprintf(stderr, "ruan-brightnessd: no DisplayServicesGetBrightness\n"); return 1; }
  io_service_t s = IO_OBJECT_NULL;
  float last = -1;
  for (;; usleep(200000)) {
    if (!s) s = backlight();
    float b = -1;
    if (!s || get(CGMainDisplayID(), &b) || b < 0 || b > 1 || fabsf(b - last) < 0.002f) continue;
    // the slider is perceptual: square it onto nits (the SY7758 level is close to linear in light)
    int fixed = (int)lround(kMaxNits * b * b * 65536.0);
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &fixed);
    CFDictionaryRef d = CFDictionaryCreate(NULL, (const void *[]){CFSTR("brightness-nits")}, (const void *[]){n}, 1,
                                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    kern_return_t r = IORegistryEntrySetCFProperties(s, d);
    CFRelease(d);
    CFRelease(n);
    if (r) { IOObjectRelease(s); s = IO_OBJECT_NULL; }
    else last = b;
  }
}
