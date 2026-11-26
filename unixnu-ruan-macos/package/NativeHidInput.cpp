// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit offline keyboard/consumer input source. Uses the unchanged original
// HID parser and event dispatch; no user-client entitlement is substituted.
#include "OriginalHidDeviceABI.h"
// Set by the macOS 27.3 ABI header (build-gop-legacy-display-27.py): IOService has 272 slots there, so every IOHIDDevice slot is
// iPadOS 21H461's + 104 (handleReport 285, newReportDescriptor 284), and newPrimaryUsage{,Page}Number (282/283) are declared.
#ifndef NATIVE_HID_MACOS
#define NATIVE_HID_MACOS 0
#endif
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSIterator.h>
extern "C" void native_hid_input_observed(unsigned,int);
extern "C" void native_hid_live_checkpoint(IOService *);
extern "C" bool native_hid_input_requested(void);
extern "C" void native_touch_set_sink(void (*)(void *), void *) __attribute__((weak_import));
// Optional hardware buttons fed by the board button driver: a mask of report 2 bits, sent on every change.
extern "C" void native_buttons_set_sink(void (*)(void *, uint32_t), void *) __attribute__((weak_import));
// Optional touch digitizer fed by the board touch probe (kits/sheng/TouchProbe.cpp): finger set published with a seqlock.
extern "C" {
extern volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
}
// Keyboard Application collection ensures the original generic keyboard
// personality can match. Report 2 carries the Consumer buttons, bit 0 Menu (Home),
// bit 1 Power, bit 2 Volume Increment, bit 3 Volume Decrement. Report 1 is an
// ordinary boot keyboard report.
static constexpr unsigned char descriptor[]={
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,
    0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,
    0x75,0x08,0x95,0x01,0x81,0x03,
    0x19,0x00,0x29,0x65,0x15,0x00,0x25,0x65,
    0x75,0x08,0x95,0x06,0x81,0x00,
    0x85,0x02,0x05,0x0c,0x09,0x40,0x09,0x30,0x09,0xe9,0x09,0xea,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x04,0x81,0x02,
    0x75,0x04,0x95,0x01,0x81,0x03,0xc0
};
// buttons-consumer=1 (always on macOS): report 2 moves out of the keyboard collection into its own Consumer Control collection, as
// UniXNU-xiaomipad8pro PianoInput/HidReports.h kButtonsDescriptor does (Power 0x30, Volume Increment 0xE9, Volume Decrement 0xEA).
// Bit 0 (Menu/Home) becomes padding so buttonsAction's mask (bits 1..3) is sent unchanged.
enum {kKeyboardBytes=44};   // descriptor[] up to its report 2 part (0x85,0x02)
static_assert(descriptor[kKeyboardBytes]==0x85 && descriptor[kKeyboardBytes+1]==0x02,"report 2 starts the consumer part");
static const unsigned char consumerDescriptor[]={
    0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,0x02,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x01,0x81,0x03,
    0x09,0x30,0x09,0xe9,0x09,0xea,0x95,0x03,0x81,0x02,
    0x95,0x04,0x81,0x03,0xc0
};
// touch-pointer=1 (macOS has no touch UI): the touch-only device is an absolute one-button mouse, report-ID-less, 5-byte report
// {buttons, x16, y16}. Same descriptor as refs/macOS-uke/touch/src/package/UkeTouchPointer.cpp (also UniXNU-xiaomipad8pro PianoTouch/TouchHid.h, run on hardware).
static const unsigned char pointerDescriptor[]={
    0x05,0x01,0x09,0x02,0xa1,0x01, 0x09,0x01,0xa1,0x00,
    0x05,0x09,0x19,0x01,0x29,0x01,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x01,0x81,0x02,
    0x75,0x07,0x95,0x01,0x81,0x03,
    0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x02,0x81,0x02,
    0xc0,0xc0
};
static bool bootArgOne(const char *name) {unsigned value=0;return PE_parse_boot_argn(name,&value,sizeof(value)) && value==1;}
// Report 3: ten-finger Touch Screen collection (Tip Switch, Touch Valid, Contact Identifier, X, Y per finger, then Contact Count).
// Touch Valid (0x47) is the finger confidence: IOHIDEventDriver turns a finger transducer whose value is 0 into a hand (palm), which
// multi-finger and system edge gestures ignore, so every finger, down or lifting, sends 1.
// Coordinates are normalised to 0..32767 on both axes by the producer.
#define FINGER_DESC \
    0x09,0x22,0xa1,0x02, 0x09,0x42,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x01,0x81,0x02, 0x09,0x47,0x81,0x02, 0x75,0x06,0x95,0x01,0x81,0x03, \
    0x09,0x51,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02, \
    0x05,0x01,0x09,0x30,0x15,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x01,0x81,0x02, 0x09,0x31,0x81,0x02, 0x05,0x0d, 0xc0,
static const unsigned char touchDescriptor[]={
    0x05,0x0d,0x09,0x04,0xa1,0x01,0x85,0x03,
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    0x09,0x54,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02,0xc0
};
#define TOUCH_FINGERS 10
// Edge contacts (int-163). A contact that starts within ~3 mm of a panel edge carries kIOHIDDigitizerEventFromEdgeTip (0x800) in its
// finger events' event mask for its whole life, as Apple's multitouch stack reports it (MultitouchHID). UIKit derives UITouch's edge
// type from that bit alone (_UIEventHIDTouchEdgeTypeForHIDEvent: 0x800/0x1000/0x2000/0x40000 -> 1..4), and every screen-edge
// recognizer of SpringBoard fails at once on edge type 0: int-162 logged edge=0 for touches starting 2 pt above the bottom edge and
// SBHomeGesturePanGestureRecognizer / SBFluidSwitcherScreenEdgePanGestureRecognizer going 0->5, so no home or switcher gesture.
// The original IOHIDEventDriver (IOHIDFamily-2031.120.4) never sets the bit. Its dispatchEvent (IOHIDEventService slot 198, the slot
// 37 of IOHIDFamily's indirect calls use) is wrapped for this device's driver only, in a copy of its vtable; the wrapper adds the bit
// to the finger children of each digitizer collection and calls the original. IOHIDEvent virtuals of that version: getType 18,
// getIntegerValue 23, setIntegerValue 24, getFixedValue 25 (setIntegerValue/getIntegerValue at +0xc0/+0xb8 in the driver's own
// calls); _children at +24 (IOHIDEvent.h; appendChild stores into it and free releases it). _parent is never set (appendChild only
// stores the child, int-163 skipped every finger by comparing it), so each child is an IOHIDEvent by its class instead.
namespace edge {
enum {kSlots=256,kDispatch=198,kGetType=18,kGetInteger=23,kSetInteger=24,kGetFixed=25,kContacts=256};
enum : uint32_t {kDigitizer=11,kX=0xB0000,kY=0xB0001,kIndex=0xB0005,kMask=0xB0007,kTouch=0xB0009,kMaskTouch=2,kFromEdgeTip=0x800};
// 48 panel pixels of 3048 x 2032 (the sweet reference uses 48 px), in the driver's normalised IOFixed coordinates.
static const int32_t kEdgeX=48*65536/3048,kEdgeY=48*65536/2032;
static uintptr_t originalDispatch;
static bool fromEdge[kContacts];
extern "C" {volatile uint32_t native_touch_edge_stats[4];}   // [0] installed, [1] contacts begun, [2] of them at an edge, [3] skipped events
// [0] reports with two or more fingers down, [1] the most fingers down in one report, [2] tip-up entries sent for vanished fingers
extern "C" {volatile uint32_t native_touch_hid_multi[3];}
template <typename R,typename... A> static R call(void *object,unsigned slot,A... args) {
    return reinterpret_cast<R (*)(void *,A...)>((*reinterpret_cast<uintptr_t **>(object))[slot])(object,args...);
}
// One finger event; true when it now carries the bit.
static bool mark(void *child) {
    if(call<uint32_t,IOOptionBits>(child,kGetType,0)!=kDigitizer)return false;   // the scan-time vendor child
    const uint32_t index=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kIndex,0);
    if(index>=kContacts) {native_touch_edge_stats[3]=native_touch_edge_stats[3]+1;return false;}
    const uint32_t mask=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kMask,0);
    const bool touch=call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kTouch,0)!=0;
    if((mask&kMaskTouch) && touch) {   // the contact begins: its position now decides
        const int32_t x=call<int32_t,uint32_t,IOOptionBits>(child,kGetFixed,kX,0),y=call<int32_t,uint32_t,IOOptionBits>(child,kGetFixed,kY,0);
        fromEdge[index]=x<kEdgeX || x>65536-kEdgeX || y<kEdgeY || y>65536-kEdgeY;
        native_touch_edge_stats[1]=native_touch_edge_stats[1]+1;
        if(fromEdge[index])native_touch_edge_stats[2]=native_touch_edge_stats[2]+1;
    }
    const bool edged=fromEdge[index];
    if(edged)call<void,uint32_t,int32_t,IOOptionBits>(child,kSetInteger,kMask,(int32_t)(mask|kFromEdgeTip),0);
    if(!touch && (mask&kMaskTouch))fromEdge[index]=false;   // the lift keeps the bit; the next contact with this index starts fresh
    return edged;
}
static void dispatch(IOService *self,OSObject *event,IOOptionBits options) {
    if(event && call<uint32_t,IOOptionBits>(event,kGetType,0)==kDigitizer) {
        OSArray *children=*reinterpret_cast<OSArray **>(reinterpret_cast<uint8_t *>(event)+24);
        if(children && OSDynamicCast(OSArray,children) && children->getCount()) {
            // A collection (the driver's touch-screen dispatch): its mask is the union of its fingers', so it takes the bit as well.
            bool any=false;
            for(unsigned i=0;i<children->getCount();++i) {
                OSObject *child=children->getObject(i);
                if(child && child->metaCast("IOHIDEvent"))any|=mark(child);
                else native_touch_edge_stats[3]=native_touch_edge_stats[3]+1;
            }
            if(any) {
                const uint32_t mask=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(event,kGetInteger,kMask,0);
                call<void,uint32_t,int32_t,IOOptionBits>(event,kSetInteger,kMask,(int32_t)(mask|kFromEdgeTip),0);
            }
        } else if(!children) mark(event);   // one transducer dispatched on its own
    }
    reinterpret_cast<void (*)(IOService *,OSObject *,IOOptionBits)>(originalDispatch)(self,event,options);
}
// This device's own event driver gets the wrapped copy; any other driver keeps the original table.
static void install(IORegistryEntry *driver) {
    if(originalDispatch || !driver)return;
    uintptr_t *table=*reinterpret_cast<uintptr_t **>(driver);
    uintptr_t *copy=static_cast<uintptr_t *>(IOMalloc(kSlots*sizeof(uintptr_t)));
    if(!copy)return;
    memcpy(copy,table,kSlots*sizeof(uintptr_t));
    originalDispatch=copy[kDispatch];copy[kDispatch]=(uintptr_t)&dispatch;
    __asm__ volatile("dmb ish":::"memory");
    *reinterpret_cast<uintptr_t **>(driver)=copy;
    native_touch_edge_stats[0]=1;
}
}  // namespace edge
class NativeHidInput : public IOHIDDevice {
    OSDeclareDefaultStructors(NativeHidInput)
    IOWorkLoop *inputLoop=nullptr;
    IOTimerEventSource *timer=nullptr;
    bool down=false;
    uint8_t buttons=0;   // report 2 bits held by the hardware buttons
    bool touchEnabled=false;
    // The digitizer is a separate service from the keyboard: backboardd matches digitizers by primary usage page/usage
    // (Digitizer/Touch Screen) and the Built-In flag, which a combined keyboard-first descriptor would not provide.
    bool touchOnly=false;
    bool pointer=false;          // touch-pointer=1 on the touch-only device
    bool pointerDown=false;
    bool consumerButtons=false;  // buttons-consumer=1 on the keyboard device
    bool eventDriverFound=false;
    uint32_t lastSeq=0;
    // Fingers sent as down in the previous report; a vanished identifier gets one explicit tip-up report.
    int prevIds[TOUCH_FINGERS]={-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    int prevX[TOUCH_FINGERS]={},prevY[TOUCH_FINGERS]={};
    static unsigned transformCoordinate(int value,int maximum) {
        if(value<0)value=0;if(value>=maximum)value=maximum-1;
        return (unsigned)((long long)value*32767/(maximum-1));
    }
    // One finger set to the event driver. The touch thread (kits/sheng/TouchProbe.cpp) calls touchNotify after it publishes a frame; there is no timer
    // watching the seqlock any more (it woke the work loop every 8 ms for nothing).
    static void touchNotify(void *context) {
        NativeHidInput *self=static_cast<NativeHidInput *>(context);
        if(self && self->inputLoop)self->inputLoop->runAction(&NativeHidInput::touchAction,self);
    }
    static IOReturn touchAction(OSObject *owner,void *,void *,void *,void *) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return kIOReturnSuccess;
        self->touchSend();
        return kIOReturnSuccess;
    }
    void touchSend() {
        NativeHidInput *self=this;
        if(!self->hasEventDriverCached())return;
        uint32_t before=native_touch_seq;
        if((before&1) || before==self->lastSeq)return;
        __asm__ volatile("dmb ish":::"memory");
        unsigned count=native_touch_count;if(count>TOUCH_FINGERS)count=TOUCH_FINGERS;
        int ids[TOUCH_FINGERS],xs[TOUCH_FINGERS],ys[TOUCH_FINGERS];
        for(unsigned i=0;i<count;++i){ids[i]=native_touch_ids[i];xs[i]=native_touch_xs[i];ys[i]=native_touch_ys[i];}
        __asm__ volatile("dmb ish":::"memory");
        if(native_touch_seq!=before)return;      // torn read: retry on the next tick
        self->lastSeq=before;
        if(self->pointer){self->pointerSend(count,xs,ys);return;}
        // Sizes follow the axis maxima of the touch core: 30480 long, 20320 short, mapped to the same 0..32767 range.
        unsigned char report[1+TOUCH_FINGERS*6+1]={3};
        unsigned slot=0;
        int nextIds[TOUCH_FINGERS],nextX[TOUCH_FINGERS],nextY[TOUCH_FINGERS];
        for(unsigned i=0;i<count && slot<TOUCH_FINGERS;++i,++slot) {
            unsigned x=transformCoordinate(xs[i],30480),y=transformCoordinate(ys[i],20320);
            unsigned char *f=report+1+slot*6;
            f[0]=3;f[1]=(unsigned char)(ids[i]&0xff);f[2]=(unsigned char)x;f[3]=(unsigned char)(x>>8);f[4]=(unsigned char)y;f[5]=(unsigned char)(y>>8);   // tip, valid
            nextIds[slot]=ids[i];nextX[slot]=(int)x;nextY[slot]=(int)y;
        }
        const unsigned downCount=slot;
        if(downCount>=2)edge::native_touch_hid_multi[0]=edge::native_touch_hid_multi[0]+1;
        if(downCount>edge::native_touch_hid_multi[1])edge::native_touch_hid_multi[1]=downCount;
        // Identifiers that were down and are gone: tip up at the last known position. They are matched by identifier, not by slot:
        // the producer packs its fingers, so when one lifts the others move down a slot and a slot match would lift a finger still
        // down and leave the lifted one stuck.
        for(unsigned j=0;j<TOUCH_FINGERS;++j) {
            const int id=self->prevIds[j];
            if(id<0)continue;
            bool kept=false;
            for(unsigned i=0;i<downCount && !kept;++i)kept=(ids[i]&0xff)==(id&0xff);
            if(kept || slot>=TOUCH_FINGERS)continue;
            unsigned char *f=report+1+slot*6;
            f[0]=2;f[1]=(unsigned char)(id&0xff);   // tip up, valid
            f[2]=(unsigned char)self->prevX[j];f[3]=(unsigned char)(self->prevX[j]>>8);
            f[4]=(unsigned char)self->prevY[j];f[5]=(unsigned char)(self->prevY[j]>>8);
            ++slot;edge::native_touch_hid_multi[2]=edge::native_touch_hid_multi[2]+1;
        }
        for(unsigned j=0;j<TOUCH_FINGERS;++j) {
            self->prevIds[j]=j<downCount?nextIds[j]:-1;
            if(j<downCount){self->prevX[j]=nextX[j];self->prevY[j]=nextY[j];}
        }
        if(!slot)return;
        report[1+TOUCH_FINGERS*6]=(unsigned char)slot;
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?self->handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(6,result);
        native_touch_hid_reports=native_touch_hid_reports+1;
    }
    // Pointer mode (UkeTouchPointer::deliver): finger 0 moves the pointer and holds button 1; the lift resends the last position with
    // button 1 up. shortcut: finger 0 is the producer's first down slot, so lifting it while another stays jumps to that one; track by id if it matters.
    void pointerSend(unsigned count,const int *xs,const int *ys) {
        if(count){prevX[0]=(int)transformCoordinate(xs[0],30480);prevY[0]=(int)transformCoordinate(ys[0],20320);}
        else if(!pointerDown)return;
        pointerDown=count!=0;
        unsigned char report[5]={(unsigned char)(pointerDown?1:0),(unsigned char)prevX[0],(unsigned char)(prevX[0]>>8),
                                 (unsigned char)prevY[0],(unsigned char)(prevY[0]>>8)};
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(6,result);
        native_touch_hid_reports=native_touch_hid_reports+1;
    }
    // The button driver's thread calls buttonsNotify on every change of the pressed set.
    static void buttonsNotify(void *context,uint32_t mask) {
        NativeHidInput *self=static_cast<NativeHidInput *>(context);
        if(self && self->inputLoop)self->inputLoop->runAction(&NativeHidInput::buttonsAction,self,reinterpret_cast<void *>(uintptr_t(mask)));
    }
    static IOReturn buttonsAction(OSObject *owner,void *mask,void *,void *,void *) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return kIOReturnSuccess;
        self->buttons=static_cast<uint8_t>(uintptr_t(mask)&0xe);
        if(!self->hasEventDriverCached())return kIOReturnSuccess;
        unsigned char report[]={2,static_cast<unsigned char>((self->down?1:0)|self->buttons)};
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?self->handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(4,result);
        return kIOReturnSuccess;
    }
    bool hasEventDriverCached() {
        if(!eventDriverFound) {
            IORegistryEntry *driver=eventDriver(this,4);
            eventDriverFound=driver!=nullptr;
            // Not in pointer mode, not on macOS: edge::install patches slot 198 of the iPadOS IOHIDEventDriver vtable (256 slots copied);
            // the macOS 26C5035r IOHIDEventDriver has 359 slots with dispatchEvent at 302.
            if(driver && (touchEnabled || touchOnly) && !pointer && !NATIVE_HID_MACOS)edge::install(driver);
        }
        return eventDriverFound;
    }
    static bool hasEventDriver(IORegistryEntry *parent,unsigned depth) {return eventDriver(parent,depth)!=nullptr;}
    // The IOHIDEventDriver below this device, if one has attached.
    static IORegistryEntry *eventDriver(IORegistryEntry *parent,unsigned depth) {
        if(!depth)return nullptr;
        OSIterator *it=parent->getChildIterator(gIOServicePlane);if(!it)return nullptr;
        IORegistryEntry *found=nullptr;
        while(OSObject *child=it->getNextObject()) {
            IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,child);
            if(!entry)continue;
            found=entry->metaCast("IOHIDEventDriver")?entry:eventDriver(entry,depth-1);
            if(found)break;
        }
        it->release();return found;
    }
    static void tick(OSObject *owner,IOTimerEventSource *source) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return;
        native_hid_live_checkpoint(self);
        // buttons-consumer: no Menu usage any more, so no mailbox/offline-hid-auto-home Home presses either.
        if(self->touchOnly || self->consumerButtons){source->setTimeoutMS(1000);return;}
        if(!self->down && !hasEventDriver(self,4)) {
            native_hid_input_observed(3,0);
            source->setTimeoutMS(1000);
            return;
        }
        native_hid_input_observed(3,1);
        if(!self->down && !native_hid_input_requested()) {source->setTimeoutMS(1000);return;}
        unsigned char report[]={2,static_cast<unsigned char>((self->down?0:1)|self->buttons)};
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?self->handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(self->down?5:4,result);
        // Always attempt release, even if down returned an error: input must
        // never be left pressed by an ambiguous report completion.
        if(!self->down) {self->down=true;source->setTimeoutMS(100);}
        else {self->down=false;source->setTimeoutMS(1000);}
    }
public:
    void setTouchOnly() {touchOnly=true;}
    IOWorkLoop *getWorkLoop() const override {return inputLoop;}
    OSString *newTransportString() const override {return OSString::withCString(touchOnly && !pointer?"SPI":"Virtual");}
    OSString *newProductString() const override {
        return OSString::withCString(pointer?"Offline touch pointer":touchOnly?"Offline touch digitizer":"Offline HID input probe");
    }
#if NATIVE_HID_MACOS
    // The pointer's primary usage, Generic Desktop/Mouse, through the original publishProperties; other devices keep the original.
    OSNumber *newPrimaryUsageNumber() const override {return pointer?OSNumber::withNumber(2ULL,32):IOHIDDevice::newPrimaryUsageNumber();}
    OSNumber *newPrimaryUsagePageNumber() const override {return pointer?OSNumber::withNumber(1ULL,32):IOHIDDevice::newPrimaryUsagePageNumber();}
#endif
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        if(!out)return kIOReturnBadArgument;
        unsigned char combined[sizeof(descriptor)+sizeof(consumerDescriptor)+sizeof(touchDescriptor)];
        size_t size;
        if(pointer){size=sizeof(pointerDescriptor);memcpy(combined,pointerDescriptor,size);}
        else if(touchOnly){size=sizeof(touchDescriptor);memcpy(combined,touchDescriptor,size);}
        else{
            if(consumerButtons) {   // keyboard collection closed before report 2, then the consumer collection
                size=kKeyboardBytes;memcpy(combined,descriptor,size);combined[size++]=0xc0;
                memcpy(combined+size,consumerDescriptor,sizeof(consumerDescriptor));size+=sizeof(consumerDescriptor);
            } else {size=sizeof(descriptor);memcpy(combined,descriptor,size);}
            // Default: one device, keyboard collection first, digitizer second (the arrangement whose event driver
            // and user client attach on hardware); the separate digitizer device is the opt-in sheng-touch-split.
            if(touchEnabled){memcpy(combined+size,touchDescriptor,sizeof(touchDescriptor));size+=sizeof(touchDescriptor);}
        }
        *out=IOBufferMemoryDescriptor::withBytes(combined,size,kIODirectionNone);
        return *out?kIOReturnSuccess:kIOReturnNoMemory;
    }
    bool start(IOService *provider) override {
        inputLoop=IOWorkLoop::workLoop();if(!inputLoop)return false;
        // Request the normal generic HID event personality for this owned
        // keyboard. This is a matching property, not a client entitlement.
        if(!setProperty("HIDDefaultBehavior",true))return false;
        // touch-pointer=1: touch goes only to the separate pointer device, never into the combined one.
        pointer=touchOnly && bootArgOne("touch-pointer");
        // macOS: always (no Menu/Home usage there, and no boot-arg room); iPadOS keeps the opt-in.
        consumerButtons=!touchOnly && (NATIVE_HID_MACOS || bootArgOne("buttons-consumer"));
        touchEnabled=!touchOnly && !bootArgOne("touch-pointer") && bootArgOne("sheng-touch-hid");
        // DigitizerCollectionDispatch: IOHIDEventDriver (setDigitizerProperties) reads it up the service plane and then sends each
        // report's fingers as one digitizer collection with a child per finger. Without it a touch screen gets one event per finger,
        // and backboardd takes each as the whole touch set, so no two fingers are ever down together for UIKit (no pinch, no
        // multi-finger or system gestures).
        // Pointer mode publishes none of them: without Built-In, native_hid_live_checkpoint also skips its 0x0d/4 rewrite.
        // Its primary usage is Generic Desktop/Mouse: newPrimaryUsage*Number on macOS, a property where the iPadOS ABI header names no slot.
        if((touchOnly||touchEnabled) && !pointer){setProperty("Built-In",true);setProperty("DisplayIntegrated",true);setProperty("DigitizerCollectionDispatch",true);}
        if(pointer && !NATIVE_HID_MACOS){setProperty("PrimaryUsagePage",(unsigned long long)1,32);setProperty("PrimaryUsage",(unsigned long long)2,32);}
        if(!IOHIDDevice::start(provider))return false;
        timer=IOTimerEventSource::timerEventSource(this,tick);
        if(!timer || inputLoop->addEventSource(timer)!=kIOReturnSuccess) {
            OSSafeReleaseNULL(timer);IOHIDDevice::stop(provider);return false;
        }
        if(touchOnly||touchEnabled) {
            if(&native_touch_set_sink)native_touch_set_sink(&NativeHidInput::touchNotify,this);
        }
        if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(&NativeHidInput::buttonsNotify,this);
        setProperty("OfflineInputProbe",true);
        native_hid_input_observed(2,0);
        timer->setTimeoutMS(1000);return true;
    }
    void stop(IOService *provider) override {
        if((touchOnly||touchEnabled) && &native_touch_set_sink)native_touch_set_sink(nullptr,nullptr);
        if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(nullptr,nullptr);
        if(timer){timer->cancelTimeout();inputLoop->removeEventSource(timer);OSSafeReleaseNULL(timer);}
        IOHIDDevice::stop(provider);
    }
    void free() override {OSSafeReleaseNULL(inputLoop);IOHIDDevice::free();}
};
OSDefineMetaClassAndStructors(NativeHidInput,IOHIDDevice)
extern "C" IOService *native_hid_input_allocate(void) {return new NativeHidInput;}
extern "C" IOService *native_hid_touch_allocate(void) {NativeHidInput *device=new NativeHidInput;if(device)device->setTouchOnly();return device;}
