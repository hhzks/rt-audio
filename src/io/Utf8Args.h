#pragma once
#ifdef _WIN32
#include "io/WinString.h"
#include <windows.h>
#include <shellapi.h>
#endif

#include <string>
#include <vector>

namespace rt {

#ifdef _WIN32
inline std::vector<std::string> utf8ArgsFrom(const wchar_t* commandLine) {
    int n = 0;
    LPWSTR* wide = CommandLineToArgvW(commandLine, &n);
    std::vector<std::string> out;
    if (!wide) return out;
    for (int i = 0; i < n; ++i) out.push_back(wideToUtf8(wide[i]));
    LocalFree(wide);
    return out;
}
#endif

// The program arguments in UTF-8. Windows gives argv in the ANSI code page, so read the wide command line.
inline std::vector<std::string> utf8Args([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
#ifdef _WIN32
    return utf8ArgsFrom(GetCommandLineW());
#else
    return {argv, argv + argc};
#endif
}

} // namespace rt
