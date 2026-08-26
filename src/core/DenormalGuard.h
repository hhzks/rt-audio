#pragma once

// Denormal floats can cost 100x on some CPUs. They show up in IIR filter and
// reverb tails AFTER the signal stops -- which is why "it only glitches when
// I stop talking" is such a common and baffling bug report.
//
// Construct one of these at the top of the audio callback. RAII restores the
// previous MXCSR on exit so we do not leak FTZ/DAZ into host or UI threads.

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  #define RT_HAS_SSE_DENORMAL_CONTROL 1
  #include <xmmintrin.h>
  #include <pmmintrin.h>
#else
  #define RT_HAS_SSE_DENORMAL_CONTROL 0
#endif

namespace rt {

class DenormalGuard {
public:
    DenormalGuard() noexcept {
#if RT_HAS_SSE_DENORMAL_CONTROL
        savedCsr_ = _mm_getcsr();
        _mm_setcsr(savedCsr_ | 0x8040u); // FTZ (bit 15) | DAZ (bit 6)
#endif
    }
    ~DenormalGuard() noexcept {
#if RT_HAS_SSE_DENORMAL_CONTROL
        _mm_setcsr(savedCsr_);
#endif
    }
    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;

private:
#if RT_HAS_SSE_DENORMAL_CONTROL
    unsigned savedCsr_ = 0;
#endif
};

} // namespace rt
