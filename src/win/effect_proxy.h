// Effect objects handed to the game.
//  - pass-through (mode game/mix): wraps a real effect on the FFB wheel, translating the
//    axes the game names (offsets/ids in ITS data format for the virtual device) to the
//    wheel's real axis, and scaling the game's gain.
//  - null (mode telemetry/off): accepts everything and does nothing, so the game keeps
//    its FF code paths alive while our engine owns the wheel.
#pragma once
#include <vector>

#include "di.h"

namespace gp4u {

class EffectProxy : public IDirectInputEffect {
public:
    EffectProxy(REFGUID type, IDirectInputEffect* real, DWORD real_axis_ofs, double gain);

    // Builds a DIEFFECT whose axes point at `real_axis_ofs` (DIEFF_OBJECTOFFSETS),
    // keeping the game's direction vector length and scaling dwGain. Storage lives in
    // `axes`/`dirs` so the returned struct stays valid while they do.
    static DIEFFECT translate(const DIEFFECT& in, DWORD real_axis_ofs, double gain, std::vector<DWORD>& axes,
                              std::vector<LONG>& dirs);

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    // IDirectInputEffect
    STDMETHOD(Initialize)(HINSTANCE, DWORD, REFGUID) override;
    STDMETHOD(GetEffectGuid)(LPGUID) override;
    STDMETHOD(GetParameters)(LPDIEFFECT, DWORD) override;
    STDMETHOD(SetParameters)(LPCDIEFFECT, DWORD) override;
    STDMETHOD(Start)(DWORD, DWORD) override;
    STDMETHOD(Stop)() override;
    STDMETHOD(GetEffectStatus)(LPDWORD) override;
    STDMETHOD(Download)() override;
    STDMETHOD(Unload)() override;
    STDMETHOD(Escape)(LPDIEFFESCAPE) override;

private:
    virtual ~EffectProxy();
    LONG ref_ = 1;
    GUID type_;
    IDirectInputEffect* real_;  // null = null effect
    DWORD axis_ofs_;
    double gain_;
    bool playing_ = false;
};

}  // namespace gp4u
