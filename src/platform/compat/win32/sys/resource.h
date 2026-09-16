#pragma once

// Windows stand-in for POSIX <sys/resource.h>, providing exactly what
// Readouts::processCpuSeconds uses: getrusage (RUSAGE_SELF) with the user and
// system CPU time of all threads (GetProcessTimes). This directory is on the
// include path of Windows builds only (CMakeLists.txt), so the shared code
// keeps a single POSIX spelling. No <windows.h> here: the header is included
// by UI code.

#define RUSAGE_SELF 0

struct rusage
{
    struct
    {
        long long tv_sec;
        long tv_usec;
    } ru_utime, ru_stime;
};

int getrusage (int who, struct rusage *usage);
