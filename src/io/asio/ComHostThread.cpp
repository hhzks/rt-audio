#ifdef _WIN32
#include "io/asio/ComHostThread.h"

#include <objbase.h>

#include <future>
#include <stdexcept>

namespace rt {

namespace {

constexpr wchar_t kClassName[] = L"rt_audio_com_host";

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

ComHostThread::ComHostThread() {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_) throw std::runtime_error("CreateEvent failed");
    thread_ = std::thread([this] { threadMain(); });
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] { return started_; });
    if (!startError_.empty()) {
        const std::string error = startError_;
        lock.unlock();
        thread_.join();
        CloseHandle(wake_);
        throw std::runtime_error(error);
    }
}

ComHostThread::~ComHostThread() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
}

bool ComHostThread::onHostThread() const noexcept {
    return GetCurrentThreadId() == threadId_;
}

void ComHostThread::run(const std::function<void()>& f) {
    if (onHostThread()) {
        f();
        return;
    }
    std::packaged_task<void()> task(f);
    std::future<void> done = task.get_future();
    post([&task] { task(); });
    done.get();
}

void ComHostThread::post(std::function<void()> f) {
    {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(f));
    }
    SetEvent(wake_);
}

void ComHostThread::drain() {
    for (;;) {
        std::function<void()> f;
        {
            std::lock_guard lock(mutex_);
            if (tasks_.empty()) return;
            f = std::move(tasks_.front());
            tasks_.pop_front();
        }
        f();
    }
}

void ComHostThread::threadMain() {
    threadId_ = GetCurrentThreadId();
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = windowProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);   // a second instance finds the class already registered
    window_ = CreateWindowExW(0, kClassName, L"rt-audio", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                              nullptr, wc.hInstance, nullptr);
    {
        std::lock_guard lock(mutex_);
        if (FAILED(co))
            startError_ = "CoInitializeEx failed";
        else if (!window_)
            startError_ = "could not create the host window";
        started_ = true;
    }
    ready_.notify_all();
    if (FAILED(co)) return;
    if (!window_) {
        CoUninitialize();
        return;
    }

    for (;;) {
        const DWORD r = MsgWaitForMultipleObjects(1, &wake_, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) {
            drain();
            std::lock_guard lock(mutex_);
            if (quit_) break;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(window_);
    CoUninitialize();
}

} // namespace rt
#endif
