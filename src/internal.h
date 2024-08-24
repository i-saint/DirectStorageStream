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


// resize std::vector without zero-clear
// ( https://qiita.com/i_saint/items/59c394a28a5244ec94e1 )
template<class T, template<class> class Allocator>
inline void DirtyResize(std::vector<T, Allocator<T>>& dst, size_t new_size)
{
#ifdef _DEBUG
    dst.resize(new_size);
#else
    struct Proxy
    {
        T value;
#pragma warning(disable:26495)
        // value is uninitialized by intention
        Proxy() {}
#pragma warning(default:26495)
    };
    static_assert(sizeof(T) == sizeof(Proxy));

    if (new_size <= dst.capacity()) {
        reinterpret_cast<std::vector<Proxy, Allocator<Proxy>>&>(dst).resize(new_size);
    }
    else {
        std::vector<T, Allocator<T>> tmp;
        size_t new_capacity = std::max<size_t>(dst.capacity() * 2, new_size);
        tmp.reserve(new_capacity);
        reinterpret_cast<std::vector<Proxy, Allocator<Proxy>>&>(tmp).resize(new_size);
        std::memcpy(tmp.data(), dst.data(), sizeof(T) * dst.size());
        dst.swap(tmp);
    }
#endif
}

} // namespace ist

