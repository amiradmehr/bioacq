#include "ProcessSetup.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mmsystem.h> // timeBeginPeriod (winmm)

#include <cstdio>

namespace
{

// Standard handle already usable without a console (redirected to a file or pipe)?
bool redirected (DWORD which)
{
    const HANDLE h = GetStdHandle (which);
    if (h == nullptr || h == INVALID_HANDLE_VALUE)
        return false;
    const DWORD type = GetFileType (h);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

} // namespace

void attachParentConsole ()
{
    const bool outRedirected = redirected (STD_OUTPUT_HANDLE);
    const bool errRedirected = redirected (STD_ERROR_HANDLE);
    if (outRedirected && errRedirected)
        return;
    if (!AttachConsole (ATTACH_PARENT_PROCESS))
        return; // started from Explorer (or the parent has no console): nowhere to print
    FILE *f = nullptr;
    if (!outRedirected)
        freopen_s (&f, "CONOUT$", "w", stdout);
    if (!errRedirected)
        freopen_s (&f, "CONOUT$", "w", stderr);
    std::fputc ('\n', outRedirected ? stderr : stdout); // start below the prompt cmd already printed
}

void raiseTimerResolution ()
{
    timeBeginPeriod (1); // released by the OS when the process exits
}

#else

void attachParentConsole ()
{
}

void raiseTimerResolution ()
{
}

#endif
