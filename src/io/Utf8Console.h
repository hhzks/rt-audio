#pragma once
#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

// UTF-8 console output for the lifetime of the object; the previous code page comes back after,
// also when Ctrl+C or a closed window ends the process. One object per process.
struct Utf8Console {
#ifdef _WIN32
    Utf8Console() {
        previous_ = GetConsoleOutputCP();
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCtrlHandler(&restoreOnSignal, TRUE);
    }
    ~Utf8Console() {
        SetConsoleCtrlHandler(&restoreOnSignal, FALSE);
        SetConsoleOutputCP(previous_);
    }
    // Returns FALSE, so the default handler still ends the process.
    static BOOL WINAPI restoreOnSignal(DWORD) {
        SetConsoleOutputCP(previous_);
        return FALSE;
    }
#else
    Utf8Console() = default;
#endif
    Utf8Console(const Utf8Console&) = delete;
    Utf8Console& operator=(const Utf8Console&) = delete;

#ifdef _WIN32
private:
    static inline UINT previous_ = 0;
#endif
};

} // namespace rt
