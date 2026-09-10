#pragma once
#include <cerrno>

#ifndef ESTRPIPE
#define ESTRPIPE 86
#endif

namespace rt {

enum class AlsaAction { None, Retry, Recover, Fail };

constexpr AlsaAction alsaActionFor(int err) noexcept {
    if (err >= 0) return AlsaAction::None;
    switch (-err) {
    case EINTR:
    case EAGAIN:   return AlsaAction::Retry;
    case EPIPE:
    case ESTRPIPE: return AlsaAction::Recover;
    default:       return AlsaAction::Fail;
    }
}

} // namespace rt
