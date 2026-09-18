#pragma once
#ifdef _WIN32

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace rt {

// A COM single-threaded apartment with a message loop and a hidden window. ASIO drivers expect all
// calls from the thread that created them, and their control panels need a message loop.
class ComHostThread {
public:
    ComHostThread();
    ~ComHostThread();
    ComHostThread(const ComHostThread&) = delete;
    ComHostThread& operator=(const ComHostThread&) = delete;

    void run(const std::function<void()>& f);   // waits; rethrows an exception from f
    void post(std::function<void()> f);          // returns at once; f must not throw
    HWND window() const noexcept { return window_; }
    bool onHostThread() const noexcept;

private:
    void threadMain();
    void drain();

    std::mutex                        mutex_;
    std::condition_variable           ready_;
    std::deque<std::function<void()>> tasks_;
    HANDLE                            wake_ = nullptr;
    HWND                              window_ = nullptr;
    DWORD                             threadId_ = 0;
    bool                              started_ = false;
    bool                              quit_ = false;
    std::string                       startError_;
    std::thread                       thread_;
};

} // namespace rt
#endif
