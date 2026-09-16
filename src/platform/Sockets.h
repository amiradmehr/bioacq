#pragma once

// Minimal portable layer over BSD sockets for the plain-UDP code: the EmotiBit
// discovery and the selftest's fake EmotiBit. Winsock 2 on Windows, POSIX
// sockets elsewhere. Header-only; include it from .cpp files only (on Windows
// it pulls in <winsock2.h>, and with it <windows.h>).

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <cstddef>
#include <cstring>
#include <string>

namespace netsock
{

#ifdef _WIN32
using Socket = SOCKET;
using SockLen = int;
inline const Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
using SockLen = socklen_t;
inline const Socket kInvalidSocket = -1;
#endif

// Winsock must be initialised per process before any socket call; the calls
// are reference counted, so every user simply holds one Session. No-op on POSIX.
class Session
{
public:
    Session ()
    {
#ifdef _WIN32
        WSADATA data;
        ok_ = WSAStartup (MAKEWORD (2, 2), &data) == 0;
#endif
    }
    ~Session ()
    {
#ifdef _WIN32
        if (ok_)
            WSACleanup ();
#endif
    }
    Session (const Session &) = delete;
    Session &operator= (const Session &) = delete;

    bool ok () const
    {
        return ok_;
    }

private:
    bool ok_ = true;
};

// Error code of the last failed socket call: errno / WSAGetLastError ().
inline int lastError ()
{
#ifdef _WIN32
    return WSAGetLastError ();
#else
    return errno;
#endif
}

// Human-readable text for a lastError () code (system locale encoding).
inline std::string errorText (int code)
{
#ifdef _WIN32
    char buf[512] = {0};
    const DWORD n = FormatMessageA (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD> (code), 0, buf, sizeof (buf), nullptr);
    std::string s (buf, n);
    while (!s.empty () && (s.back () == '\n' || s.back () == '\r' || s.back () == ' ' || s.back () == '.'))
        s.pop_back ();
    if (s.empty ())
        s = "socket error " + std::to_string (code);
    return s;
#else
    return std::strerror (code);
#endif
}

inline void closeSocket (Socket s)
{
    if (s == kInvalidSocket)
        return;
#ifdef _WIN32
    ::closesocket (s);
#else
    ::close (s);
#endif
}

// UDP socket, IPv4. On Windows an ICMP "port unreachable" for an earlier
// datagram would otherwise make the next recvfrom() fail with WSAECONNRESET.
inline Socket openUdp ()
{
    const Socket s = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s != kInvalidSocket)
    {
        BOOL off = FALSE;
        DWORD ret = 0;
        const DWORD kSioUdpConnReset = static_cast<DWORD> (IOC_IN | IOC_VENDOR | 12); // SIO_UDP_CONNRESET
        WSAIoctl (s, kSioUdpConnReset, &off, sizeof (off), nullptr, 0, &ret, nullptr, nullptr);
    }
#endif
    return s;
}

inline bool setNonBlocking (Socket s)
{
#ifdef _WIN32
    u_long on = 1;
    return ::ioctlsocket (s, FIONBIO, &on) == 0;
#else
    const int flags = ::fcntl (s, F_GETFL, 0);
    return flags >= 0 && ::fcntl (s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

inline bool setBroadcast (Socket s)
{
    int on = 1;
    return ::setsockopt (s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *> (&on), sizeof (on)) == 0;
}

inline bool setReceiveTimeoutMs (Socket s, int ms)
{
#ifdef _WIN32
    const DWORD tv = static_cast<DWORD> (ms);
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
#endif
    return ::setsockopt (s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *> (&tv), sizeof (tv)) == 0;
}

// Waits up to timeoutMs for s to become readable: > 0 readable, 0 timeout, < 0 error.
inline int waitReadable (Socket s, int timeoutMs)
{
#ifdef _WIN32
    WSAPOLLFD p;
    p.fd = s;
    p.events = POLLRDNORM;
    p.revents = 0;
    const int r = ::WSAPoll (&p, 1, timeoutMs);
    return r > 0 && (p.revents & (POLLRDNORM | POLLERR | POLLHUP)) ? 1 : (r < 0 ? -1 : 0);
#else
    struct pollfd p;
    p.fd = s;
    p.events = POLLIN;
    p.revents = 0;
    const int r = ::poll (&p, 1, timeoutMs);
    return r > 0 && (p.revents & POLLIN) ? 1 : (r < 0 ? -1 : 0);
#endif
}

// sendto / recvfrom with size_t lengths; return bytes, or < 0 on error.
inline long sendTo (Socket s, const char *data, std::size_t len, const struct sockaddr_in &to)
{
    return static_cast<long> (::sendto (s, data, static_cast<int> (len), 0,
        reinterpret_cast<const struct sockaddr *> (&to), static_cast<SockLen> (sizeof (to))));
}

inline long recvFrom (Socket s, char *data, std::size_t len, struct sockaddr_in *from)
{
    SockLen fl = sizeof (*from);
    std::memset (from, 0, sizeof (*from));
    return static_cast<long> (::recvfrom (s, data, static_cast<int> (len), 0,
        reinterpret_cast<struct sockaddr *> (from), &fl));
}

} // namespace netsock
