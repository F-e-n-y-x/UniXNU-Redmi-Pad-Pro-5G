#!/usr/bin/env python3
"""Run the actual USB RAM disk receiver (UsbRamdisk.h) against a scripted fastboot host on a fake controller."""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

header = Path('UsbRamdisk.h').resolve()
HARNESS = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
typedef uint8_t UINT8; typedef uint16_t CHAR16; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef size_t UINTN; typedef long INTN; typedef int BOOLEAN; typedef char CHAR8; typedef UINTN EFI_STATUS;
typedef void *EFI_EVENT; typedef struct { UINT32 a; uint16_t b, c; UINT8 d[8]; } EFI_GUID;
#define STATIC static
#define CONST const
#define VOID void
#define EFIAPI
#define TRUE 1
#define FALSE 0
#define SIZE_1MB 0x100000
#define SIZE_2MB 0x200000
#define EFI_PAGE_SIZE 4096
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define EFI_SUCCESS 0
#define EFI_ERR(n) ((EFI_STATUS)1 << 63 | (n))
#define EFI_ERROR(s) ((INTN)(s) < 0)
#define EFI_NOT_FOUND EFI_ERR(14)
#define EFI_TIMEOUT EFI_ERR(18)
#define EFI_ABORTED EFI_ERR(21)
#define EFI_DEVICE_ERROR EFI_ERR(7)
#define EVT_NOTIFY_SIGNAL 0x200
#define TPL_CALLBACK 8
#define EfiResetCold 0
#define CompareMem memcmp
#define CopyMem memcpy
#define ZeroMem(p, n) memset(p, 0, n)
#define AsciiStrCmp strcmp
#define AsciiStrnCmp strncmp
#define AsciiStrLen strlen
static UINT64 AsciiStrHexToUint64(const char *s) { return strtoull(s, NULL, 16); }
static void EfiEventEmptyFunction(EFI_EVENT e, void *c) { (void)e; (void)c; }
static UINTN AsciiSPrint(CHAR8 *out, UINTN n, const CHAR8 *format, ...) {
  char f[64]; size_t j = 0; va_list ap;
  for (size_t i = 0; format[i] && j < sizeof(f) - 2; i++) {      /* %a is EDK2's ASCII string */
    f[j++] = format[i];
    if (format[i] == '%' && format[i + 1] == 'a') { f[j++] = 's'; i++; }
  }
  f[j] = 0; va_start(ap, format); int r = vsnprintf(out, n, f, ap); va_end(ap); return (UINTN)r;
}
static UINTN UnicodeSPrint(CHAR16 *out, UINTN n, const CHAR16 *format, ...) { (void)format; if (n) out[0] = 0; return 0; }
static uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xffffffffu;
  for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & -(c & 1)); }
  return ~c;
}
/* the boot services the receiver calls; the clock only moves when it stalls */
static UINT64 now_us; static int resets, stops, watchdog_off;
static void *usb_protocol;
struct bs {
  void (*Stall)(UINTN);
  EFI_STATUS (*CalculateCrc32)(void *, UINTN, UINT32 *);
  EFI_STATUS (*CreateEventEx)(UINT32, UINTN, void (*)(EFI_EVENT, void *), void *, const EFI_GUID *, EFI_EVENT *);
  EFI_STATUS (*SignalEvent)(EFI_EVENT);
  EFI_STATUS (*CloseEvent)(EFI_EVENT);
  EFI_STATUS (*LocateProtocol)(EFI_GUID *, void *, void **);
  EFI_STATUS (*SetWatchdogTimer)(UINTN, UINT64, UINTN, CHAR16 *);
};
static void stall(UINTN us) { now_us += us; }
static EFI_STATUS crc(void *p, UINTN n, UINT32 *out) { *out = crc32(p, n); return 0; }
static EFI_STATUS create(UINT32 t, UINTN l, void (*f)(EFI_EVENT, void *), void *c, const EFI_GUID *g, EFI_EVENT *e) {
  (void)t; (void)l; (void)f; (void)c; (void)g; *e = (EFI_EVENT)1; return 0; }
static EFI_STATUS signal_close(EFI_EVENT e) { (void)e; return 0; }
static EFI_STATUS locate(EFI_GUID *g, void *r, void **out) {
  (void)r; if (!usb_protocol || g->a != 0xd9d9ce48) return EFI_NOT_FOUND; *out = usb_protocol; return 0; }
static EFI_STATUS watchdog(UINTN t, UINT64 c, UINTN n, CHAR16 *d) { (void)c; (void)n; (void)d; watchdog_off = t == 0; return 0; }
static struct bs boot_services = { stall, crc, create, signal_close, signal_close, locate, watchdog };
static struct bs *gBS = &boot_services;
struct rt { void (*ResetSystem)(int, EFI_STATUS, UINTN, void *); };
static void reset(int k, EFI_STATUS s, UINTN n, void *d) { (void)k; (void)s; (void)n; (void)d; resets++; }
static struct rt runtime = { reset };
static struct { struct rt *RuntimeServices; } system_table = { &runtime }, *gST = &system_table;
static void DiagDraw(const CHAR16 *t, UINTN row) { (void)t; (void)row; }
#include "UsbRamdisk.h"

/* The fake controller: host packets go to OUT receives in order (a long one is a stream, taken in pieces), every IN
   send is a reply the host reads. Completions come back one per HandleEvent, after the first "connected". */
static const uint8_t *host; static size_t host_len, host_at;
static char replies[8192]; static size_t replies_len;
static int connected_sent; static struct { int pending; UINT8 ep; UINTN bytes; } done;
static EFI_STATUS u_send(UINT8 ep, UINTN size, void *buf) {
  if (done.pending) return EFI_DEVICE_ERROR;               /* one transfer at a time, as the receiver uses it */
  if (ep == USBRD_EP_IN) {
    replies_len += (size_t)snprintf(replies + replies_len, sizeof(replies) - replies_len, "%.*s|", (int)size, (char *)buf);
    done.pending = 1; done.ep = ep; done.bytes = size; return 0;
  }
  if (host_at + 4 > host_len) return 0;                    /* nothing more from the host: the receive never ends */
  uint32_t n; memcpy(&n, host + host_at, 4);
  uint32_t take = n < size ? n : (uint32_t)size;
  memcpy(buf, host + host_at + 4, take);
  if (take == n) host_at += 4 + n;
  else { host_at += take; n -= take; memcpy((uint8_t *)host + host_at, &n, 4); } /* the rest stays queued */
  done.pending = 1; done.ep = ep; done.bytes = take; return 0;
}
static EFI_STATUS u_event(UINT32 *ev, UINTN *size, USBRD_EVENT_DATA *d) {
  (void)size;
  if (!connected_sent) { connected_sent = 1; *ev = UsbRdEventState; d->DeviceState = UsbRdStateConnected; return 0; }
  if (done.pending) {
    done.pending = 0; *ev = UsbRdEventTransfer;
    d->Transfer.Status = UsbRdTransferOk; d->Transfer.Endpoint = done.ep; d->Transfer.Bytes = done.bytes; return 0;
  }
  *ev = UsbRdEventNone; return 0;
}
static EFI_STATUS u_alloc(UINTN n, void **b) { *b = malloc(n); return *b ? 0 : EFI_DEVICE_ERROR; }
static EFI_STATUS u_free(void *b) { free(b); return 0; }
static EFI_STATUS u_stop(void) { stops++; return 0; }
static EFI_STATUS u_abort(UINT8 ep) { (void)ep; done.pending = 0; return 0; }
static EFI_STATUS u_start(USBRD_DESCRIPTOR_SET *s) { return s->StringCount == 5 && s->Descriptors[0] == mUsbRdConfig ? 0 : EFI_DEVICE_ERROR; }
static USBRD_PROTOCOL fake = { 0, 0, u_send, u_event, u_alloc, u_free, u_stop, u_abort, 0, u_start };

long descriptor(const void *p, size_t n, UINT64 *size, UINT32 *crc) { return UsbRamdiskDescriptor(p, n, size, crc); }
long receive(uint8_t *script, size_t len, int with_usb, uint8_t *dest, UINT64 size, UINT32 want, char *out, size_t cap) {
  host = script; host_len = len; host_at = 0; replies_len = 0; replies[0] = 0; connected_sent = 0; done.pending = 0;
  stops = resets = watchdog_off = 0; now_us = 0; usb_protocol = with_usb ? &fake : NULL;
  EFI_STATUS s = UsbRamdiskReceive(dest, size, want);
  snprintf(out, cap, "%s#stops=%d#watchdog_off=%d#resets=%d", replies, stops, watchdog_off, resets);
  return s == 0 ? 0 : (long)(s & 0xff);
}
'''


def packets(*items):
    return b''.join(struct.pack('<I', len(item)) + item for item in items)


with tempfile.TemporaryDirectory(prefix='darwinboot-usb-') as temporary:
    work = Path(temporary)
    (work / 'test.c').write_text(HARNESS)
    library = work / 'usb.so'
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fshort-wchar', '-shared', '-fPIC',
                    '-I', str(header.parent), str(work / 'test.c'), '-o', str(library)], check=True)
    lib = ctypes.CDLL(str(library))
    lib.descriptor.restype = ctypes.c_long
    lib.descriptor.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint32)]
    lib.receive.restype = ctypes.c_long
    lib.receive.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint64,
                            ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]

    def describe(blob):
        size, crc = ctypes.c_uint64(), ctypes.c_uint32()
        return lib.descriptor(blob, len(blob), ctypes.byref(size), ctypes.byref(crc)), size.value, crc.value

    image = bytes((i * 7 + (i >> 12)) & 0xff for i in range(3 * 1024 * 1024 + 8192))
    crc = zlib.crc32(image)
    good = struct.pack('<8sIIQQ', b'DBUSBRD1', 1, crc, len(image), 0)
    assert describe(good) == (1, len(image), crc)
    assert describe(b'DBRDISK1' + bytes(24))[0] == 0
    assert describe(b'not one')[0] == 0
    assert describe(struct.pack('<8sIIQQ', b'DBUSBRD1', 2, crc, len(image), 0))[0] == -1
    assert describe(struct.pack('<8sIIQQ', b'DBUSBRD1', 1, crc, len(image) + 1, 0))[0] == -1
    assert describe(struct.pack('<8sIIQQ', b'DBUSBRD1', 1, crc, 1 << 32, 0))[0] == -1
    assert describe(good + b'\0')[0] == -1

    def run(script, image_size=len(image), want=crc, with_usb=1):
        dest = ctypes.create_string_buffer(image_size)
        out = ctypes.create_string_buffer(8192)
        status = lib.receive(script, len(script), with_usb, dest, image_size, want, out, len(out))
        return status, dest.raw, out.value.decode()

    # fastboot getvar, stage (the image in uneven bulk packets, one longer than a receive), continue
    pieces = [image[:5000], image[5000:5000 + 1536 * 1024], image[5000 + 1536 * 1024:]]
    script = packets(b'getvar:product', b'getvar:max-download-size', b'download:%08x' % len(image), *pieces, b'continue')
    status, dest, out = run(script)
    assert status == 0 and dest == image, out
    assert out == ('OKAYunixnu-ramdisk|OKAY0x%08x|DATA%08x|OKAY|OKAY|#stops=1#watchdog_off=1#resets=0'
                   % (len(image), len(image))), out

    # truncated download: only used prefix sent, tail is zero-filled by receiver
    used_size = 1024 * 1024
    sparse_image = image[:used_size] + bytes(len(image) - used_size)
    sparse_crc = zlib.crc32(sparse_image)
    script = packets(b'download:%08x' % used_size, sparse_image[:used_size], b'continue')
    status, dest, out = run(script, want=sparse_crc)
    assert status == 0 and dest == sparse_image, out
    assert out == ('DATA%08x|OKAY|OKAY|#stops=1#watchdog_off=1#resets=0' % used_size), out

    # size exceeding descriptor, zero size, or premature continue are refused
    status, _, out = run(packets(b'download:%08x' % (len(image) + 4096), b'download:00000000', b'continue', b'flash:boot', b'erase:userdata'))
    assert status == 18 and out.startswith('FAILsize exceeds descriptor|FAILsize exceeds descriptor|FAILno image yet|FAILunsupported|FAILunsupported|'), out
    assert '#stops=1' in out

    # a corrupted byte: the CRC32 refuses the image and "continue" still finds none
    bad = bytearray(image)
    bad[12345] ^= 1
    status, _, out = run(packets(b'download:%08x' % len(image), bytes(bad), b'continue'))
    assert status == 18 and out.startswith('DATA%08x|FAILcrc32 mismatch|FAILno image yet|' % len(image)), out

    # the host stops half-way: the download gives up after its data wait
    status, _, out = run(packets(b'download:%08x' % len(image), image[:4096]))
    assert status == 18 and 'FAILshort download|' in out, out

    # reboot is the only other command that does anything
    status, _, out = run(packets(b'reboot'))
    assert '#resets=1' in out, out

    # no USB device protocol: refused before anything is touched
    status, _, out = run(b'', with_usb=0)
    assert status == 14 and '#stops=0' in out, out
print('usb ramdisk: descriptor and fastboot receiver checks passed')
