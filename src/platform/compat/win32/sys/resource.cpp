// Windows implementation of the getrusage () stand-in (see resource.h). Built on Windows only.

#include "sys/resource.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{

// FILETIME counts 100 ns intervals.
void toTimeval (const FILETIME &ft, long long *sec, long *usec)
{
    const unsigned long long t = (static_cast<unsigned long long> (ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    *sec = static_cast<long long> (t / 10000000ull);
    *usec = static_cast<long> ((t % 10000000ull) / 10ull);
}

} // namespace

int getrusage (int who, struct rusage *usage)
{
    if (who != RUSAGE_SELF || !usage)
        return -1;
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes (GetCurrentProcess (), &created, &exited, &kernel, &user))
        return -1;
    toTimeval (user, &usage->ru_utime.tv_sec, &usage->ru_utime.tv_usec);
    toTimeval (kernel, &usage->ru_stime.tv_sec, &usage->ru_stime.tv_usec);
    return 0;
}
