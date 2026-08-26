#pragma once
#ifdef _WIN32
#include <windows.h>
#include <avrt.h>

namespace rt {

// SetThreadPriority alone is NOT enough for audio on Windows. MMCSS ("Pro
// Audio" task) is what actually gets you scheduled ahead of the rest of the
// system. Register on the audio thread itself, from inside that thread.
//
// Note this is priority boosting, not a realtime scheduler -- a badly written
// third-party kernel driver holding a DPC for 3 ms will still stall you, and
// there is nothing you can do about it from user space. Run LatencyMon when
// dropouts have no visible cause in your own code.
class MmcssScope {
public:
    MmcssScope() {
        DWORD taskIndex = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (handle_) AvSetMmThreadPriority(handle_, AVRT_PRIORITY_CRITICAL);
    }
    ~MmcssScope() { if (handle_) AvRevertMmThreadCharacteristics(handle_); }

    bool ok() const noexcept { return handle_ != nullptr; }

    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;

private:
    HANDLE handle_ = nullptr;
};

} // namespace rt
#endif
