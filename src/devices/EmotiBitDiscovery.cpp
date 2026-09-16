#include "EmotiBitDiscovery.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace emotibit
{

namespace
{

std::vector<std::string> splitNonEmpty (const std::string &s, char delim)
{
    // same semantics as Emotibit::split_string: empty fields are skipped
    std::vector<std::string> out;
    std::size_t end = 0;
    std::size_t start;
    while ((start = s.find_first_not_of (delim, end)) != std::string::npos)
    {
        end = s.find (delim, start);
        out.push_back (s.substr (start, end == std::string::npos ? std::string::npos : end - start));
    }
    return out;
}

} // namespace

std::vector<std::string> ipv4BroadcastAddresses ()
{
    std::vector<std::string> out;
    struct ifaddrs *ifap = nullptr;
    if (getifaddrs (&ifap) != 0)
        return out;
    for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        const unsigned flags = ifa->ifa_flags;
        if (!(flags & IFF_UP) || (flags & IFF_LOOPBACK))
            continue;
        // Like BrainFlow (address | ~netmask for every IPv4 interface), minus
        // loopback and host (/32) routes, which cannot reach an EmotiBit.
        struct in_addr bcast;
        if ((flags & IFF_BROADCAST) && ifa->ifa_broadaddr &&
            ifa->ifa_broadaddr->sa_family == AF_INET)
            bcast = reinterpret_cast<struct sockaddr_in *> (ifa->ifa_broadaddr)->sin_addr;
        else if (ifa->ifa_netmask &&
            reinterpret_cast<struct sockaddr_in *> (ifa->ifa_netmask)->sin_addr.s_addr != 0xFFFFFFFFu)
            bcast.s_addr = reinterpret_cast<struct sockaddr_in *> (ifa->ifa_addr)->sin_addr.s_addr |
                ~reinterpret_cast<struct sockaddr_in *> (ifa->ifa_netmask)->sin_addr.s_addr;
        else
            continue;
        char buf[INET_ADDRSTRLEN] = {0};
        if (!inet_ntop (AF_INET, &bcast, buf, sizeof (buf)))
            continue;
        const std::string s = buf;
        if (s != "0.0.0.0" && std::find (out.begin (), out.end (), s) == out.end ())
            out.push_back (s);
    }
    freeifaddrs (ifap);
    return out;
}

std::string helloEmotibitPacket ()
{
    // create_header: timestamp, packet number, data length, type tag,
    // protocol version (1), data reliability (100); then PACKET_DELIMITER_CSV.
    return "0,0,0,HE,1,100\n";
}

bool parseHelloHost (const std::string &datagram, std::string *serial)
{
    for (std::string pkt : splitNonEmpty (datagram, '\n'))
    {
        while (!pkt.empty () && (pkt.back () == '\r' || pkt.back () == '\0'))
            pkt.pop_back ();
        const std::vector<std::string> f = splitNonEmpty (pkt, ',');
        if (f.size () >= 6 && f[3] == "HH")
        {
            if (serial)
                *serial = f.size () > 9 ? f[9] : std::string ();
            return true;
        }
    }
    return false;
}

DiscoveryResult discover (const std::vector<std::string> &targets, int port, double timeoutSec,
    const std::string &wantSerial, const std::function<bool ()> &cancelled, std::atomic<int> *probesSent)
{
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now ();
    auto elapsed = [&t0] { return std::chrono::duration<double> (Clock::now () - t0).count (); };

    DiscoveryResult r;
    r.targets = targets;

    std::vector<struct sockaddr_in> addrs;
    for (const std::string &t : targets)
    {
        struct sockaddr_in a;
        std::memset (&a, 0, sizeof (a));
        a.sin_family = AF_INET;
        a.sin_port = htons (static_cast<uint16_t> (port));
        if (inet_pton (AF_INET, t.c_str (), &a.sin_addr) == 1)
            addrs.push_back (a);
    }
    if (addrs.empty ())
    {
        r.error = targets.empty ()
            ? "no IPv4 network interface with a broadcast address is up (is WiFi on?)"
            : "invalid IPv4 address '" + targets.front () + "'";
        r.seconds = elapsed ();
        return r;
    }

    const int fd = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
    {
        r.lastSendErrno = errno;
        r.error = std::string ("cannot create a UDP socket: ") + std::strerror (errno);
        r.seconds = elapsed ();
        return r;
    }
    int on = 1;
    ::setsockopt (fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof (on));
#ifdef SO_NOSIGPIPE
    ::setsockopt (fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof (on));
#endif
    ::fcntl (fd, F_SETFL, ::fcntl (fd, F_GETFL, 0) | O_NONBLOCK);

    const std::string hello = helloEmotibitPacket ();
    std::vector<char> buf (32768);
    double nextSend = 0.0;

    for (;;)
    {
        if (cancelled && cancelled ())
        {
            r.cancelled = true;
            break;
        }
        const double el = elapsed ();
        if (el >= nextSend)
        {
            for (const struct sockaddr_in &a : addrs)
            {
                const ssize_t n = ::sendto (fd, hello.data (), hello.size (), 0,
                    reinterpret_cast<const struct sockaddr *> (&a), sizeof (a));
                if (n == static_cast<ssize_t> (hello.size ()))
                    r.anySendOk = true;
                else
                    r.lastSendErrno = errno;
            }
            nextSend = el + 1.0; // re-greet once per second (UDP may drop packets)
            if (probesSent)
                probesSent->fetch_add (1);
        }
        if (el > timeoutSec)
            break;

        struct pollfd p;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        const int pr = ::poll (&p, 1, 100);
        if (pr <= 0 || !(p.revents & POLLIN))
            continue;
        for (;;)
        {
            struct sockaddr_in from;
            socklen_t fl = sizeof (from);
            std::memset (&from, 0, sizeof (from));
            const ssize_t n = ::recvfrom (fd, buf.data (), buf.size (), 0,
                reinterpret_cast<struct sockaddr *> (&from), &fl);
            if (n <= 0)
                break;
            std::string serial;
            if (!parseHelloHost (std::string (buf.data (), static_cast<std::size_t> (n)), &serial))
                continue;
            if (!wantSerial.empty () && serial != wantSerial)
                continue;
            char ip[INET_ADDRSTRLEN] = {0};
            if (!inet_ntop (AF_INET, &from.sin_addr, ip, sizeof (ip)))
                continue;
            r.found = true;
            r.ip = ip;
            r.serial = serial;
            break;
        }
        if (r.found)
            break;
    }
    ::close (fd);

    if (!r.found && !r.cancelled)
    {
        if (!r.anySendOk)
            r.error = std::string ("could not send the discovery packet (") +
                std::strerror (r.lastSendErrno) + ")";
        else
            r.error = "no EmotiBit answered";
    }
    r.seconds = elapsed ();
    return r;
}

} // namespace emotibit
