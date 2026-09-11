#pragma once
#include <cstddef>
#include <cstring>
#include <string_view>

namespace rt {

// Copies src into dst (capacity cap, including the NUL) and never splits a
// UTF-8 code point. Returns the number of bytes copied.
inline std::size_t copyUtf8Truncated(char* dst, std::size_t cap, std::string_view src) noexcept {
    if (cap == 0) return 0;
    std::size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    if (n < src.size())
        while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0u) == 0x80u) --n;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
    return n;
}

} // namespace rt
