#pragma once
#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

// UTF-8 console output for the lifetime of the object; the previous code page comes back after.
struct Utf8Console {
#ifdef _WIN32
    UINT previous = GetConsoleOutputCP();
    Utf8Console() { SetConsoleOutputCP(CP_UTF8); }
    ~Utf8Console() { SetConsoleOutputCP(previous); }
#else
    Utf8Console() = default;
#endif
    Utf8Console(const Utf8Console&) = delete;
    Utf8Console& operator=(const Utf8Console&) = delete;
};

} // namespace rt
