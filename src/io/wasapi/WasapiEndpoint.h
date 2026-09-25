#pragma once
#ifdef _WIN32
#include "core/SampleConvert.h"
#include "io/WinString.h"
#include "io/wasapi/ComPtr.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>

#include <optional>
#include <string>

namespace rt {

inline std::optional<SampleFormat> detectFormat(const WAVEFORMATEX* fmt) noexcept {
    const bool ext = fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE;
    GUID sub{};
    if (ext) sub = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat;

    const bool isFloat = ext ? (sub == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
                             : (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    const bool isPcm   = ext ? (sub == KSDATAFORMAT_SUBTYPE_PCM)
                             : (fmt->wFormatTag == WAVE_FORMAT_PCM);

    if (isFloat && fmt->wBitsPerSample == 32) return SampleFormat::Float32;
    if (isPcm) {
        switch (fmt->wBitsPerSample) {
            case 16: return SampleFormat::Int16;
            case 24: return SampleFormat::Int24;
            case 32: return SampleFormat::Int32;
            default: break;
        }
    }
    return std::nullopt;
}

inline std::string endpointId(IMMDevice* dev) {
    LPWSTR id = nullptr;
    if (FAILED(dev->GetId(&id)) || id == nullptr) return {};
    std::string s = wideToUtf8(id);
    CoTaskMemFree(id);
    return s;
}

inline std::string friendlyName(IMMDevice* dev) {
    std::string name = "(unnamed)";
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            name = wideToUtf8(pv.pwszVal);
        PropVariantClear(&pv);
    }
    return name;
}

} // namespace rt
#endif
