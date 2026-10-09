#include "effect_proxy.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "device_manager.h"

namespace gp4u {

EffectProxy::EffectProxy(REFGUID type, IDirectInputEffect* real, DWORD real_axis_ofs, double gain)
    : type_(type), real_(real), axis_ofs_(real_axis_ofs), gain_(gain) {}

EffectProxy::~EffectProxy() {
    if (real_) {
        std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
        real_->Release();
    }
}

DIEFFECT EffectProxy::translate(const DIEFFECT& in, DWORD real_axis_ofs, double gain, std::vector<DWORD>& axes,
                                std::vector<LONG>& dirs) {
    // The game may pass a DirectX 5-sized DIEFFECT (no dwStartDelay): copy only what it gave.
    DIEFFECT e;
    std::memset(&e, 0, sizeof e);
    std::memcpy(&e, &in, std::min<size_t>(in.dwSize ? in.dwSize : sizeof(DIEFFECT), sizeof(DIEFFECT)));
    e.dwSize = sizeof(DIEFFECT);
    // A wheel has one FF axis: collapse whatever the game asked for onto it. The game's
    // first axis is (on a steering device) the steering axis, and its first direction
    // component carries the sign for Cartesian effects.
    if (in.cAxes > 0) {
        e.cAxes = 1;
        axes.assign(1, real_axis_ofs);
        e.rgdwAxes = in.rgdwAxes ? axes.data() : nullptr;
        e.dwFlags = (e.dwFlags & ~(DWORD)(DIEFF_OBJECTIDS | DIEFF_OBJECTOFFSETS)) | DIEFF_OBJECTOFFSETS;
        if (in.rglDirection) {
            // Only the sign of the X component survives the projection onto one axis.
            double x;
            if (in.dwFlags & DIEFF_POLAR) {
                x = std::sin(in.rglDirection[0] * 3.14159265358979 / 18000.0);   // 9000 = +X
            } else if (in.dwFlags & DIEFF_SPHERICAL) {
                x = std::cos(in.rglDirection[0] * 3.14159265358979 / 18000.0);   // 0 = +X
            } else {
                x = in.rglDirection[0];
            }
            dirs.assign(1, x < 0 ? -1 : 1);
            e.rglDirection = dirs.data();
            e.dwFlags = (e.dwFlags & ~(DWORD)(DIEFF_POLAR | DIEFF_SPHERICAL)) | DIEFF_CARTESIAN;
        }
    }
    double g = std::clamp((double)in.dwGain * gain, 0.0, (double)DI_FFNOMINALMAX);
    e.dwGain = (DWORD)g;
    return e;
}

STDMETHODIMP EffectProxy::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirectInputEffect) {
        *ppv = static_cast<IDirectInputEffect*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) EffectProxy::AddRef() { return (ULONG)InterlockedIncrement(&ref_); }

STDMETHODIMP_(ULONG) EffectProxy::Release() {
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return (ULONG)r;
}

STDMETHODIMP EffectProxy::Initialize(HINSTANCE, DWORD, REFGUID) { return DI_OK; }

STDMETHODIMP EffectProxy::GetEffectGuid(LPGUID g) {
    if (!g) return E_POINTER;
    *g = type_;
    return DI_OK;
}

STDMETHODIMP EffectProxy::GetParameters(LPDIEFFECT e, DWORD flags) {
    if (!real_) return DI_OK;
    std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
    return real_->GetParameters(e, flags);
}

STDMETHODIMP EffectProxy::SetParameters(LPCDIEFFECT e, DWORD flags) {
    if (!real_ || !e) return DI_OK;
    std::vector<DWORD> axes;
    std::vector<LONG> dirs;
    DIEFFECT t = translate(*e, axis_ofs_, gain_, axes, dirs);
    std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
    return real_->SetParameters(&t, flags);
}

STDMETHODIMP EffectProxy::Start(DWORD n, DWORD flags) {
    playing_ = true;
    if (!real_) return DI_OK;
    std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
    return real_->Start(n, flags);
}

STDMETHODIMP EffectProxy::Stop() {
    playing_ = false;
    if (!real_) return DI_OK;
    std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
    return real_->Stop();
}

STDMETHODIMP EffectProxy::GetEffectStatus(LPDWORD s) {
    if (!s) return E_POINTER;
    if (!real_) { *s = playing_ ? DIEGES_PLAYING : 0; return DI_OK; }
    std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
    return real_->GetEffectStatus(s);
}

STDMETHODIMP EffectProxy::Download() {
    if (!real_) return DI_OK;
    std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
    return real_->Download();
}

STDMETHODIMP EffectProxy::Unload() {
    if (!real_) return DI_OK;
    std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
    return real_->Unload();
}

STDMETHODIMP EffectProxy::Escape(LPDIEFFESCAPE p) {
    if (!real_) return DIERR_UNSUPPORTED;
    std::lock_guard<std::mutex> lk(DeviceManager::get().mutex());
    return real_->Escape(p);
}

}  // namespace gp4u
