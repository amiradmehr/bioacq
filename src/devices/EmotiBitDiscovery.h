#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

// EmotiBit discovery that runs OUTSIDE BrainFlow.
//
// BrainFlow serialises every session call (prepare_session, get_board_data,
// stop_stream, ...) behind one process-wide mutex, and its EmotiBit
// prepare_session performs the whole broadcast discovery while holding it:
// up to (timeout + 5 s) per broadcast address, during which the Cyton worker
// cannot even poll. We therefore find the EmotiBit ourselves first, using the
// same advertising protocol BrainFlow uses (emotibit.cpp create_adv_connection):
//
//   host  -> <addr>:3131   "0,0,0,HE,1,100\n"          (HELLO_EMOTIBIT)
//   EmotiBit -> host port  "...,<n>,<len>,HH,...\n"    (HELLO_HOST, payload[3] = serial)
//
// and hand the answering address to BrainFlow as params.ip_address. BrainFlow
// then only re-greets that one address, so its lock is held for ~1-7 s instead
// of up to a minute, and our wait can be cancelled at any time.
namespace emotibit
{

constexpr int kAdvertisingPort = 3131; // WIFI_ADVERTISING_PORT in BrainFlow

struct DiscoveryResult
{
    bool found = false;
    bool cancelled = false;
    std::string ip;                   // sender address of the HELLO_HOST
    std::string serial;               // serial number from the HELLO_HOST (may be empty)
    std::vector<std::string> targets; // addresses the HELLO_EMOTIBIT was sent to
    bool anySendOk = false;           // at least one sendto() succeeded
    int lastSendErrno = 0;            // error code of the last failed sendto() (errno / WSAGetLastError)
    std::string error;                // reason when !found && !cancelled
    double seconds = 0.0;             // time spent
};

// IPv4 broadcast addresses (address | ~netmask) of every interface that is up
// and not loopback -- BrainFlow's own list, minus 127.255.255.255 and /32s.
std::vector<std::string> ipv4BroadcastAddresses ();

// Largest subnet scanned host by host (a /24).
constexpr int kMaxScanHosts = 254;

// Host addresses of address/prefix without the network and broadcast
// addresses and the address itself; empty when the subnet has more than
// maxHosts hosts or the prefix is outside 1-30. Pure (for the selftest).
std::vector<std::string> subnetHosts (const std::string &address, int prefix, int maxHosts = kMaxScanHosts);

// Every host of the small subnets (at most kMaxScanHosts hosts) of the
// interfaces that are up, minus this computer's own addresses. HELLO_EMOTIBIT
// sent to each by unicast finds an EmotiBit on networks that drop broadcasts
// (iPhone hotspots, some routers). *subnets receives "a.b.c.0/p" for each
// scanned subnet.
std::vector<std::string> ipv4ScanHosts (std::vector<std::string> *subnets = nullptr);

// Text for DiscoveryResult::lastSendErrno (strerror / FormatMessage).
std::string socketErrorText (int code);

// The HELLO_EMOTIBIT datagram, byte-identical to BrainFlow's
// create_package (HELLO_EMOTIBIT, 0, "", 0).
std::string helloEmotibitPacket ();

// True if the datagram contains a HELLO_HOST packet; *serial receives the
// serial-number field (BrainFlow reads field 9) or "" if absent.
bool parseHelloHost (const std::string &datagram, std::string *serial);

// Sends HELLO_EMOTIBIT to every target (re-sent once per second) and waits up
// to timeoutSec for a HELLO_HOST (matching wantSerial if non-empty). Polls
// in 100 ms slices and returns promptly once cancelled() is true.
// *probesSent (optional) counts the HELLO rounds sent so far.
DiscoveryResult discover (const std::vector<std::string> &targets, int port, double timeoutSec,
    const std::string &wantSerial, const std::function<bool ()> &cancelled,
    std::atomic<int> *probesSent = nullptr);

} // namespace emotibit
