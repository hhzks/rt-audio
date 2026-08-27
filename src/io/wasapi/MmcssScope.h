#pragma once
#ifdef _WIN32
#include <windows.h>
#include <avrt.h>

namespace rt {

// SetThreadPriority alone is NOT enough for audio on Windows. MMCSS ("Pro
// Audio" task) is what gets you scheduled ahead of the rest of the system.
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
