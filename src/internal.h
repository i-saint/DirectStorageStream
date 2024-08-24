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

struct handle_closer
{
    void operator()(HANDLE h) const noexcept
    {
        assert(h != INVALID_HANDLE_VALUE);
        if (h) {
            ::CloseHandle(h);
        }
    }
};
using ScopedHandle = std::unique_ptr<void, handle_closer>;

} // namespace ist

