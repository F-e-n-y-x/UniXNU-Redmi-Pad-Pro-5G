// SPDX-License-Identifier: MIT
// ruan macOS: a virtual built-in Ethernet (en0) with a fixed locally administered MAC address (02:72:75:61:00:01), from
// liimagination/UniXNU-tb375fc drivers/net/Tb375fcDummyEth.cpp (MIT). TimeSync takes its clock ID from a built-in interface's
// MAC: without one clocksyncd never answers com.apple.timesync.expositor and WindowServer waits forever (ruan full-OS boots
// 12-22: WindowServer and loginwindow alive, blocked on an XPC reply; the same fix on UniXNU-tb375fc and UniXNU-xiaomipad8pro).
// Outbound packets are dropped. Built by scripts/build-ruan-mac-ufs.py (late registration, IONetworkingFamily bound by address);
// started by package/RuanMacUfs.cpp (ruan-dummy-eth=0 turns it off).

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOOutputQueue.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void ruan_dummy_eth_static_init(void) __asm__("__GLOBAL__sub_I_RuanDummyEth.cpp");
}

class RuanDummyEth : public IOEthernetController {
    OSDeclareDefaultStructors(RuanDummyEth)

private:
    IOEthernetInterface *fNetIf = nullptr;
    IOWorkLoop *fWorkLoop = nullptr;
    OSDictionary *fMediumDict = nullptr;

public:
    bool init(OSDictionary *properties = nullptr) override;
    void free(void) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    bool createWorkLoop(void) override;
    IOWorkLoop *getWorkLoop(void) const override;
    IOReturn enable(IONetworkInterface *netif) override;
    IOReturn disable(IONetworkInterface *netif) override;
    IOReturn getHardwareAddress(IOEthernetAddress *addr) override;
    UInt32 outputPacket(mbuf_t m, void *param) override;
    IOReturn setPromiscuousMode(bool active) override { return kIOReturnSuccess; }
    IOReturn setMulticastMode(bool active) override { return kIOReturnSuccess; }
    IOReturn setMulticastList(IOEthernetAddress *addrs, UInt32 count) override { return kIOReturnSuccess; }
    // Attached under IOResources, start() would wait for IOBSD, which waits for IOResources to finish matching: a deadlock
    // (boot 1 hung here in bsd_init; DroidVM log-2026-10:695, UniXNU-sheng ShengEthernet.cpp:224).
    UInt32 getFeatures() const override { return kIONetworkFeatureNoBSDWait; }
    const OSString *newVendorString(void) const override { return OSString::withCString("ruan"); }
    const OSString *newModelString(void) const override { return OSString::withCString("Virtual Ethernet"); }
};

#define super IOEthernetController
OSDefineMetaClassAndStructors(RuanDummyEth, IOEthernetController)

extern "C" void ruan_dummy_eth_probe(void);

bool RuanDummyEth::init(OSDictionary *properties) {
    if (!super::init(properties)) return false;
    return true;
}

void RuanDummyEth::free(void) {
    if (fMediumDict) { fMediumDict->release(); fMediumDict = nullptr; }
    if (fNetIf) { fNetIf->release(); fNetIf = nullptr; }
    if (fWorkLoop) { fWorkLoop->release(); fWorkLoop = nullptr; }
    super::free();
}

bool RuanDummyEth::createWorkLoop(void) {
    fWorkLoop = IOWorkLoop::workLoop();
    return fWorkLoop != nullptr;
}

IOWorkLoop *RuanDummyEth::getWorkLoop(void) const {
    return fWorkLoop;
}

bool RuanDummyEth::start(IOService *provider) {
    IOLog("RuanDummyEth: step super::start\n");
    if (!super::start(provider)) {
        IOLog("RuanDummyEth: super::start failed\n");
        return false;
    }

    // Publish medium dictionary (1000baseT full-duplex) so link status is valid
    fMediumDict = OSDictionary::withCapacity(2);
    if (fMediumDict) {
        IONetworkMedium *med = IONetworkMedium::medium(
            kIOMediumEthernet1000BaseT | kIOMediumOptionFullDuplex, 1000000000ULL);
        if (med) {
            IONetworkMedium::addMedium(fMediumDict, med);
            publishMediumDictionary(fMediumDict);
            setCurrentMedium(med);
            setSelectedMedium(med);
            med->release();
        }
    }

    IOLog("RuanDummyEth: step attachInterface\n");
    if (!attachInterface((IONetworkInterface **)&fNetIf, true)) {
        IOLog("RuanDummyEth: attachInterface failed\n");
        return false;
    }

    setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, getCurrentMedium(), 1000000000ULL);
    IOLog("RuanDummyEth: step registerService\n");
    registerService();
    IOLog("RuanDummyEth: en0 attached and registered (MAC 02:72:75:61:00:01)\n");
    return true;
}

void RuanDummyEth::stop(IOService *provider) {
    if (fNetIf) {
        detachInterface(fNetIf);
    }
    super::stop(provider);
}

IOReturn RuanDummyEth::enable(IONetworkInterface *netif) {
    setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, getCurrentMedium(), 1000000000ULL);
    return kIOReturnSuccess;
}

IOReturn RuanDummyEth::disable(IONetworkInterface *netif) {
    setLinkStatus(kIONetworkLinkValid, nullptr, 0);
    return kIOReturnSuccess;
}

IOReturn RuanDummyEth::getHardwareAddress(IOEthernetAddress *addr) {
    if (!addr) return kIOReturnBadArgument;
    addr->bytes[0] = 0x02;
    addr->bytes[1] = 0x72;
    addr->bytes[2] = 0x75;
    addr->bytes[3] = 0x61;
    addr->bytes[4] = 0x00;
    addr->bytes[5] = 0x01;
    return kIOReturnSuccess;
}

UInt32 RuanDummyEth::outputPacket(mbuf_t m, void *param) {
    if (m) freePacket(m);
    return kIOReturnOutputSuccess;
}

extern "C" void ruan_dummy_eth_probe(void) {
    static bool probed = false;
    if (probed) return;
    probed = true;

    OSReturn kr_net = OSKextLoadKextWithIdentifier("com.apple.iokit.IONetworkingFamily");
    IOLog("RuanDummyEth: kext load IONetworkingFamily=0x%x\n", kr_net);
    if (kr_net != kOSReturnSuccess) {
        IOLog("RuanDummyEth: IONetworkingFamily load failed\n");
        return;
    }

    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        IOLog("RuanDummyEth: preModLoad failed\n");
        return;
    }
    ruan_dummy_eth_static_init();
    OSReturn post_kr = OSMetaClass::postModLoad(handle);
    IOLog("RuanDummyEth: postModLoad status=0x%x\n", post_kr);
    if (post_kr != kOSReturnSuccess) {
        return;
    }

    IOPlatformExpert *plat = IOService::getPlatform();
    if (!plat) {
        IOLog("RuanDummyEth: no platform expert\n");
        return;
    }
    IOLog("RuanDummyEth: step new\n");
    RuanDummyEth *eth = new RuanDummyEth;
    if (!eth) return;
    if (!eth->init(nullptr)) {
        eth->release();
        return;
    }
    IOLog("RuanDummyEth: step init ok\n");
    eth->setName("RuanDummyEth");
    if (!eth->attach(plat)) {
        eth->release();
        return;
    }
    IOLog("RuanDummyEth: step attach ok\n");
    if (!eth->start(plat)) {
        eth->detach(plat);
        eth->release();
        return;
    }
}
