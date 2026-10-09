#include "di.h"

#include <vector>

namespace gp4u {

const GUID& axis_guid(int axis) {
    switch (axis) {
        case 0: return GUID_XAxis;
        case 1: return GUID_YAxis;
        case 2: return GUID_ZAxis;
        case 3: return GUID_RxAxis;
        case 4: return GUID_RyAxis;
        case 5: return GUID_RzAxis;
        default: return GUID_Slider;
    }
}

namespace {
struct Joystate2Format {
    std::vector<DIOBJECTDATAFORMAT> objs;
    DIDATAFORMAT fmt{};
    Joystate2Format();
};
}  // namespace

const DIDATAFORMAT* joystate2_format() {
    static const Joystate2Format f;  // thread-safe one-time init
    return &f.fmt;
}

Joystate2Format::Joystate2Format() {
    const DWORD axis_type = DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL;
    for (int a = 0; a < 8; ++a)
        objs.push_back({&axis_guid(a), joystate2_axis_offset(a), axis_type, DIDOI_ASPECTPOSITION});
    for (int p = 0; p < 4; ++p)
        objs.push_back({&GUID_POV, (DWORD)(FIELD_OFFSET(DIJOYSTATE2, rgdwPOV) + p * 4),
                        DIDFT_POV | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0});
    for (int b = 0; b < 128; ++b)
        objs.push_back({nullptr, (DWORD)(FIELD_OFFSET(DIJOYSTATE2, rgbButtons) + b),
                        DIDFT_BUTTON | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0});
    fmt.dwSize = sizeof(DIDATAFORMAT);
    fmt.dwObjSize = sizeof(DIOBJECTDATAFORMAT);
    fmt.dwFlags = DIDF_ABSAXIS;
    fmt.dwDataSize = sizeof(DIJOYSTATE2);
    fmt.dwNumObjs = (DWORD)objs.size();
    fmt.rgodf = objs.data();
}

}  // namespace gp4u
