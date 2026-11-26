// SPDX-License-Identifier: BSD-2-Clause-Patent
// ruan macOS: memcpy/memset/bzero that are valid on Device memory. The macOS kernel maps uncached and write-combined buffers
// (the GPU pool, ml_io_map_unmappable WIMG 6/7) as Device memory, where the library routines' unaligned or paired accesses fault
// ("Unaligned kernel data abort" at "gpu s2c", full-OS boot 13; NativeUfs had the same in UFS boots 1-2). Aligned 64-bit stores
// where pointers and length allow, byte stores otherwise. Active only with -DRUAN_DEVICE_SAFE_MEM=1 (build-gop-legacy-display-27.py).
#pragma once
#if defined(RUAN_DEVICE_SAFE_MEM) && RUAN_DEVICE_SAFE_MEM
static inline void *ruan_dev_memset(void *d, int c, size_t n)
{
	uint8_t b = (uint8_t)c;
	if (!((uintptr_t)d & 7) && !(n & 7)) {
		uint64_t w = 0x0101010101010101ULL * b;
		volatile uint64_t *q = (volatile uint64_t *)d;
		for (size_t i = 0; i < n / 8; ++i) q[i] = w;
	} else {
		volatile uint8_t *q = (volatile uint8_t *)d;
		for (size_t i = 0; i < n; ++i) q[i] = b;
	}
	return d;
}
static inline void *ruan_dev_memcpy(void *d, const void *s, size_t n)
{
	if (!((uintptr_t)d & 7) && !((uintptr_t)s & 7) && !(n & 7)) {
		volatile uint64_t *q = (volatile uint64_t *)d;
		const volatile uint64_t *r = (const volatile uint64_t *)s;
		for (size_t i = 0; i < n / 8; ++i) q[i] = r[i];
	} else {
		volatile uint8_t *q = (volatile uint8_t *)d;
		const volatile uint8_t *r = (const volatile uint8_t *)s;
		for (size_t i = 0; i < n; ++i) q[i] = r[i];
	}
	return d;
}
#define memcpy(d, s, n) ruan_dev_memcpy((d), (s), (n))
#define memset(d, c, n) ruan_dev_memset((d), (c), (n))
#define bzero(d, n) ruan_dev_memset((d), 0, (n))
#endif
