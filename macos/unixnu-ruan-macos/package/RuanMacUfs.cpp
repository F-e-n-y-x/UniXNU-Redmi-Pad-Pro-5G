// SPDX-License-Identifier: BSD-2-Clause-Patent
// ruan macOS: start the UFS root driver (ShengUfs NativeUfs, 2f6c7b0 + dizi/ruan edits) on the plain arm64 macOS kernel.
//
// The iPadOS build binds NativeUfsBlock's IOBlockStorageDevice base to the original IOStorageFamily by address
// (build-ufs-original-bindings.py, pinned to the iPadOS 17 kernelcache). On macOS the base is compiled from Apple's public
// IOBlockStorageDevice.cpp instead, with the macOS ABI (TARGET_OS_OSX) and its metaclass registered below the original class,
// as UniXNU sheng's source storage does (package/SourceStorage.cpp there). scripts/build-ruan-mac-ufs.py builds the objects
// with their static constructors removed; this file runs them once IOStorageFamily is loaded, then starts NativeUfs on the
// platform expert device as its second personality would (NativeUfs::probe: IOPlatformExpertDevice).
// Called from bsd_init after vfsinit (patches/xnu-12377/ruan-mac-ufs-start.patch) with native-ufs-root=1.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void ruan_mac_ufs_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void ruan_mac_ufs_driver_init(void) __asm__("__GLOBAL__sub_I_NativeUfs.cpp");
extern "C" int boot_reset_mark_bootloader(char *text, unsigned cap) __attribute__((weak_import));

static const OSMetaClass *storageParent;
static bool attempted;

// Used only by the deferred IOBlockStorageDevice metaclass constructor (build-ruan-mac-ufs.py's renamed registration).
extern "C" const OSMetaClass *native_source_storage_parent(void)
{
    if (!storageParent) panic("RuanMacUfs: metaclass initialized before the original storage family");
    return storageParent;
}

extern "C" void ruan_mac_ufs_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("native-ufs-root", &enabled, sizeof(enabled)) || enabled != 1 || attempted)
        return;
    attempted = true;
    OSReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
    if (status != kOSReturnSuccess)
        panic("RuanMacUfs: IOStorageFamily load failed %x", status);
    const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
    storageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) name->release();
    if (!storageParent || storageParent->getClassSize() != sizeof(IOBlockStorageDevice))
        panic("RuanMacUfs: original IOBlockStorageDevice size %u differs from the source build %lu",
              storageParent ? storageParent->getClassSize() : 0, sizeof(IOBlockStorageDevice));
    status = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
    if (status != kOSReturnSuccess)
        panic("RuanMacUfs: APFS load failed %x", status);

    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("RuanMacUfs: cannot begin class registration");
    ruan_mac_ufs_base_init();
    ruan_mac_ufs_driver_init();
    status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess)
        panic("RuanMacUfs: class registration failed %x", status);

    OSDictionary *matching = IOService::serviceMatching("IOPlatformExpertDevice");
    IOService *provider = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!provider) panic("RuanMacUfs: platform expert device unavailable");
    IOService *ufs = OSDynamicCast(IOService, OSMetaClass::allocClassWithName("NativeUfs"));
    if (!ufs) panic("RuanMacUfs: cannot allocate NativeUfs");
    SInt32 score = 0;   // probe as IOKit would: it records the match (native_ufs_matched) that start relies on
    const char *step = !ufs->init() ? "init" : !ufs->probe(provider, &score) ? "probe" : !ufs->attach(provider) ? "attach" :
                       !ufs->start(provider) ? "start" : nullptr;
    if (step) panic("RuanMacUfs: NativeUfs %s failed", step);
    printf("RuanMacUfs: NativeUfs started on the platform expert device (source IOBlockStorageDevice, macOS ABI)\n");

    // Every later reset goes to fastboot: the PMIC reboot-reason cell gets "bootloader" now (package/BootReset.c), and a panic
    // ends in PSCI SYSTEM_RESET (patches/xnu-12377/ruan-panic-reset.patch). ruan-panic-fastboot=0 leaves the cell alone.
    unsigned to_fastboot = 15;
    PE_parse_boot_argn("ruan-panic-fastboot", &to_fastboot, sizeof(to_fastboot));
    if (to_fastboot && &boot_reset_mark_bootloader) {
        char text[160];
        boot_reset_mark_bootloader(text, sizeof(text));
        printf("RuanMacUfs: %s\n", text);
    }
}
