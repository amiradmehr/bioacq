#include "EmotiBitDiscovery.h"

#include "Sockets.h"

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
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

namespace
{

void addUnique (std::vector<std::string> &out, const struct in_addr &bcast)
{
    char buf[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop (AF_INET, &bcast, buf, sizeof (buf)))
        return;
    const std::string s = buf;
    if (s != "0.0.0.0" && std::find (out.begin (), out.end (), s) == out.end ())
        out.push_back (s);
}

} // namespace

#ifdef _WIN32
std::vector<std::string> ipv4BroadcastAddresses ()
{
    // Same rule as below: address | ~netmask of every IPv4 interface that is up,
    // minus loopback and host (/32) routes.
    std::vector<std::string> out;
    ULONG size = 16384;
    std::vector<unsigned char> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4 && rc == ERROR_BUFFER_OVERFLOW; ++attempt)
    {
        buf.resize (size);
        rc = GetAdaptersAddresses (AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
            reinterpret_cast<PIP_ADAPTER_ADDRESSES> (buf.data ()), &size);
    }
    if (rc != NO_ERROR)
        return out;
    for (auto *ad = reinterpret_cast<PIP_ADAPTER_ADDRESSES> (buf.data ()); ad; ad = ad->Next)
    {
        if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (auto *ua = ad->FirstUnicastAddress; ua; ua = ua->Next)
        {
            const struct sockaddr *sa = ua->Address.lpSockaddr;
            if (!sa || sa->sa_family != AF_INET || ua->OnLinkPrefixLength >= 32)
                continue;
            const std::uint32_t mask =
                ua->OnLinkPrefixLength == 0 ? 0u : (0xFFFFFFFFu << (32 - ua->OnLinkPrefixLength));
            struct in_addr bcast;
            bcast.s_addr = reinterpret_cast<const struct sockaddr_in *> (sa)->sin_addr.s_addr | htonl (~mask);
            addUnique (out, bcast);
        }
    }
    return out;
}
#else
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
        addUnique (out, bcast);
    }
    freeifaddrs (ifap);
    return out;
}
#endif

std::string socketErrorText (int code)
{
    return netsock::errorText (code);
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

    const netsock::Session session;
    const netsock::Socket fd = session.ok () ? netsock::openUdp () : netsock::kInvalidSocket;
    if (fd == netsock::kInvalidSocket)
    {
        r.lastSendErrno = netsock::lastError ();
        r.error = "cannot create a UDP socket: " + netsock::errorText (r.lastSendErrno);
        r.seconds = elapsed ();
        return r;
    }
    netsock::setBroadcast (fd);
#ifdef SO_NOSIGPIPE
    int on = 1;
    ::setsockopt (fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof (on));
#endif
    netsock::setNonBlocking (fd);

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
                const long n = netsock::sendTo (fd, hello.data (), hello.size (), a);
                if (n == static_cast<long> (hello.size ()))
                    r.anySendOk = true;
                else
                    r.lastSendErrno = netsock::lastError ();
            }
            nextSend = el + 1.0; // re-greet once per second (UDP may drop packets)
            if (probesSent)
                probesSent->fetch_add (1);
        }
        if (el > timeoutSec)
            break;

        if (netsock::waitReadable (fd, 100) <= 0)
            continue;
        for (;;)
        {
            struct sockaddr_in from;
            const long n = netsock::recvFrom (fd, buf.data (), buf.size (), &from);
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
    netsock::closeSocket (fd);

    if (!r.found && !r.cancelled)
    {
        if (!r.anySendOk)
            r.error = "could not send the discovery packet (" + netsock::errorText (r.lastSendErrno) + ")";
        else
            r.error = "no EmotiBit answered";
    }
    r.seconds = elapsed ();
    return r;
}

} // namespace emotibit
