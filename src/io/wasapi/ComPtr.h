#pragma once
#ifdef _WIN32
#include <utility>

namespace rt {

// Minimal COM smart pointer. Use Microsoft::WRL::ComPtr instead if you already
// depend on WRL; this exists so the skeleton has no external requirements.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr& other) : ptr_(other.ptr_) { if (ptr_) ptr_->AddRef(); }
    ComPtr(ComPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}

    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) { reset(); ptr_ = other.ptr_; if (ptr_) ptr_->AddRef(); }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) { reset(); ptr_ = std::exchange(other.ptr_, nullptr); }
        return *this;
    }

    void reset() { if (ptr_) { ptr_->Release(); ptr_ = nullptr; } }

    T*  get()  const noexcept { return ptr_; }
    T*  operator->() const noexcept { return ptr_; }
    T** put() { reset(); return &ptr_; }               // for out-params
    void** putVoid() { reset(); return reinterpret_cast<void**>(&ptr_); }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

} // namespace rt
#endif
