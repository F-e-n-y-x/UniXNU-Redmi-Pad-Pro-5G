/* ruan: load the IPSW's original RestoreTrustCache (v1, embedded unchanged) through AMFI as the first static cache,
 * as a restore boot does. Ported from UniXNU-fuxi package/RamRestoreTrust.c. Boot-arg ram-restore-trust=1 turns it on;
 * without it XNU's loader is unchanged. Entries are never converted, made up or skipped; a rejection panics with AMFI's
 * own error. DarwinBoot's bundle validator only takes v2 modules, hence the embedded copy. */
#include <sys/types.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>
#include <libkern/amfi/amfi.h>
#include <TrustCache/API.h>
#include "RamRootProbePins.h"
#include "RamRestoreTrustPins.h"

unsigned ram_restore_trust_loaded, ram_restore_trust_engineering;
int ram_restore_trust_load(TrustCacheRuntime_t *, TrustCache_t *);

int
ram_restore_trust_load(TrustCacheRuntime_t *runtime, TrustCache_t *cache)
{
	unsigned selector = 0;
	if (!PE_parse_boot_argn("ram-restore-trust", &selector, sizeof(selector)) || selector == 0) {
		return 0;
	}
#if RAM_RESTORE_EMBEDDED
	if (selector != 1 || ram_restore_trust_loaded || !runtime || !cache || !amfi || amfi->TrustCache.version < 2) {
		panic("RAM_RESTORE_TRUST FAIL\nrefused runtime: selector=%u runtime=%p amfi=%p", selector, runtime, amfi);
	}
	/* The engineering policy decides whether the IPSW's v2 Static/SystemOS modules still load after this one. */
	ram_restore_trust_engineering = runtime->allowEngineeringTC;
	TCReturn_t result = amfi->TrustCache.loadModule(runtime, kTCTypeStatic, cache,
	    (uintptr_t)ram_restore_original, sizeof(ram_restore_original));
	if (result.error != kTCReturnSuccess) {
		panic("RAM_RESTORE_TRUST FAIL\noriginal RestoreTrustCache rejected by AMFI: component=%u error=%u unique=%u",
		    result.component, result.error, result.uniqueError);
	}
	for (unsigned i = 0; i < RAM_RESTORE_ENTRIES; i++) {
		TrustCacheQueryToken_t token = {0};
		result = amfi->TrustCache.query(runtime, kTCQueryTypeAll, ram_restore_original + 24 + i * 22, &token);
		if (result.error != kTCReturnSuccess) {
			panic("RAM_RESTORE_TRUST FAIL\nentry query failed: entry=%u error=%u", i, result.error);
		}
	}
	ram_restore_trust_loaded = RAM_RESTORE_ENTRIES;
	return 1;
#else
	(void)runtime;
	(void)cache;
	panic("RAM_RESTORE_TRUST FAIL\nram-restore-trust=%u but this kernel has no RestoreTrustCache built in", selector);
#endif
}
