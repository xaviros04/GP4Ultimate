// ANSI/Unicode flavours of the DirectInput 8 interfaces, so the proxy and the virtual
// device are written once and work whichever one GP4 (or any other game) asks for.
#pragma once
#include "di.h"

namespace gp4u {

struct TraitsA {
    using DI = IDirectInput8A;
    using Dev = IDirectInputDevice8A;
    using DevInst = DIDEVICEINSTANCEA;
    using ObjInst = DIDEVICEOBJECTINSTANCEA;
    using EffInfo = DIEFFECTINFOA;
    using EnumDevCb = LPDIENUMDEVICESCALLBACKA;
    using EnumObjCb = LPDIENUMDEVICEOBJECTSCALLBACKA;
    using EnumEffCb = LPDIENUMEFFECTSCALLBACKA;
    using SemCb = LPDIENUMDEVICESBYSEMANTICSCBA;
    using ActionFormat = DIACTIONFORMATA;
    using ImageHdr = DIDEVICEIMAGEINFOHEADERA;
    using CfgParams = DICONFIGUREDEVICESPARAMSA;
    using Str = LPCSTR;
    static const IID& iid_di() { return IID_IDirectInput8A; }
    static const IID& iid_dev() { return IID_IDirectInputDevice8A; }
    static constexpr const char* tag = "A";
};

struct TraitsW {
    using DI = IDirectInput8W;
    using Dev = IDirectInputDevice8W;
    using DevInst = DIDEVICEINSTANCEW;
    using ObjInst = DIDEVICEOBJECTINSTANCEW;
    using EffInfo = DIEFFECTINFOW;
    using EnumDevCb = LPDIENUMDEVICESCALLBACKW;
    using EnumObjCb = LPDIENUMDEVICEOBJECTSCALLBACKW;
    using EnumEffCb = LPDIENUMEFFECTSCALLBACKW;
    using SemCb = LPDIENUMDEVICESBYSEMANTICSCBW;
    using ActionFormat = DIACTIONFORMATW;
    using ImageHdr = DIDEVICEIMAGEINFOHEADERW;
    using CfgParams = DICONFIGUREDEVICESPARAMSW;
    using Str = LPCWSTR;
    static const IID& iid_di() { return IID_IDirectInput8W; }
    static const IID& iid_dev() { return IID_IDirectInputDevice8W; }
    static constexpr const char* tag = "W";
};

// Fixed identity of the virtual controller. Product GUID follows DirectInput's HID
// pattern {PPPPVVVV-0000-0000-0000-504944564944} with pid.codes test VID 0x1209, PID 0x4750.
static const GUID kVirtualInstance = {0x6f8a1c20, 0x6d4b, 0x4b8e, {0x9c, 0x1a, 0x47, 0x50, 0x34, 0x4d, 0x4f, 0x44}};
static const GUID kVirtualProduct = {0x47501209, 0x0000, 0x0000, {0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44}};

}  // namespace gp4u
