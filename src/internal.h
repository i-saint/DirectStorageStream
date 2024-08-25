#pragma once
#define NOMINMAX

#include <cstring>
#include <cassert>
#include <cstdarg>
#include <memory>
#include <vector>
#include <windows.h>

// VTune
#if __has_include(<ittnotify.h>)

#include <ittnotify.h>
#pragma comment(lib, "libittnotify.lib")

namespace ist {
class ProfileScope
{
public:
    ProfileScope(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), format, args);
        event_ = __itt_event_create(buf, (int)std::strlen(buf));
        __itt_event_start(event_);
    }

    ~ProfileScope()
    {
        __itt_event_end(event_);
    }

private:
    __itt_event event_{};
};
} // namespace ist

#define DS_PROFILE_SCOPE(...) ::ist::ProfileScope profile_scope_##__LINE__(__VA_ARGS__)

#else

#define DS_PROFILE_SCOPE(...) 

#endif


namespace ist {

class ScopedHandle
{
public:
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& v) noexcept { *this = std::move(v); }
    ScopedHandle& operator=(ScopedHandle&& v) noexcept { swap(v); return *this; }

    ScopedHandle() {}
    ScopedHandle(HANDLE v) { reset(v); }

    ~ScopedHandle() { reset(); }

    void swap(ScopedHandle& v) noexcept
    {
        std::swap(value_, v.value_);
    }

    void reset(HANDLE v = INVALID_HANDLE_VALUE)
    {
        if (value_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(value_);
        }
        value_ = v;
    }

    HANDLE get() const { return value_; }

    bool operator==(const ScopedHandle& v) const { return value_ == v.value_; }
    bool operator!=(const ScopedHandle& v) const { return value_ != v.value_; }
    operator bool() const { return value_ != INVALID_HANDLE_VALUE; }

    operator HANDLE() const { return value_; }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

} // namespace ist

