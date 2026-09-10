#pragma once
#ifdef __linux__
#include <pthread.h>
#include <sched.h>

namespace rt {

class RtSchedScope {
public:
    RtSchedScope() noexcept {
        if (pthread_getschedparam(pthread_self(), &prevPolicy_, &prevParam_) != 0) return;
        sched_param p{};
        p.sched_priority = 10;
        const int maxPrio = sched_get_priority_max(SCHED_FIFO);
        if (maxPrio > 0 && p.sched_priority > maxPrio) p.sched_priority = maxPrio;
        ok_ = pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0;
    }

    ~RtSchedScope() noexcept {
        if (ok_) pthread_setschedparam(pthread_self(), prevPolicy_, &prevParam_);
    }

    bool ok() const noexcept { return ok_; }

    RtSchedScope(const RtSchedScope&) = delete;
    RtSchedScope& operator=(const RtSchedScope&) = delete;

private:
    int          prevPolicy_ = 0;
    sched_param  prevParam_{};
    bool         ok_ = false;
};

} // namespace rt
#endif
