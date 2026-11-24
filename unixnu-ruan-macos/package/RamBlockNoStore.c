/* ruan: NativeRamBlock's optional persistent store and loader-memory release (weak imports in the driver), absent on
 * ruan: the static kernel link needs definitions. Mode 0 = no store, so the driver never calls the others; they fail. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

unsigned native_ufs_store_mode(void) { return 0; }
unsigned native_ufs_store_gpt_check(void) { return 0xff; }
int native_ufs_store_open(uint64_t blocks, uint64_t *first, const char **why) { (void)blocks; (void)first; if (why) *why = "no store on ruan"; return 0; }
int native_ufs_store_read(uint64_t lba, uint32_t count, void *buffer) { (void)lba; (void)count; (void)buffer; return 0; }
int native_ufs_store_write(uint64_t lba, uint32_t count, const void *buffer) { (void)lba; (void)count; (void)buffer; return 0; }
int native_ufs_store_write_cached(uint64_t lba, uint32_t count, const void *buffer) { (void)lba; (void)count; (void)buffer; return 0; }
int native_ufs_store_sync(void) { return 0; }
bool ml_static_mfree_prepare(uintptr_t address, size_t size) { (void)address; (void)size; return false; }
