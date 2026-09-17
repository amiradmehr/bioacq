#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

class QThread;

// How BioAcq reaches the EmotiBit (--emotibit-link).
enum class EmotiBitLink
{
    Auto, // a Bluetooth scan alongside the Wi-Fi discovery; the first to find the EmotiBit wins
    WiFi,
    Bluetooth
};

// Streams an EmotiBit that runs the bioacq BLE firmware
// (third_party/emotibit-firmware) into BrainFlow's unchanged EmotiBit driver.
//
// The EmotiBit's data packets arrive as BLE notifications (a Nordic-UART-style
// service). The bridge presents them to BrainFlow as an EmotiBit on Wi-Fi at
// 127.0.0.1: it answers HELLO_EMOTIBIT ("HE") with HELLO_HOST ("HH") on UDP
// 127.0.0.1:3131, connects to BrainFlow's TCP control port after its
// EMOTIBIT_CONNECT ("EC", which also names the UDP data port), and forwards
// every complete data packet to that data port. BrainFlow's parsing, presets,
// timestamps and file streamers apply unchanged, so a Bluetooth recording has
// exactly the Wi-Fi format. If the BLE link drops, the bridge reconnects on its
// own; the worker meanwhile sees a stalled stream.
//
// All Qt objects live on the bridge's own QThread (the BLE backends need an
// event loop). The public methods are thread-safe; stop () blocks until the
// bridge has shut down and must not be called from the bridge thread.
class EmotiBitBleBridge
{
public:
    // Synthetic: the selftest's packet generator (EmotiBit timing and packet
    // format) instead of a radio, so the loopback handshake is tested anywhere.
    enum class Source
    {
        Bluetooth,
        Synthetic
    };
    enum State
    {
        Idle,
        Scanning,
        Connecting,
        Ready, // subscribed: packets flow (the link may still drop and reconnect)
        Failed,
        Stopped
    };

    static constexpr quint16 kLoopbackPort = 3131; // BrainFlow's WIFI_ADVERTISING_PORT
    static constexpr const char *kServiceUuid = "{6E400001-B5A3-F393-E0A9-E50E24DCCA9E}";
    static constexpr const char *kTxUuid = "{6E400003-B5A3-F393-E0A9-E50E24DCCA9E}"; // notify: data packets

    EmotiBitBleBridge ();
    ~EmotiBitBleBridge ();
    EmotiBitBleBridge (const EmotiBitBleBridge &) = delete;
    EmotiBitBleBridge &operator= (const EmotiBitBleBridge &) = delete;

    // Returns at once. Bluetooth: scans up to scanTimeoutSec for a device whose
    // name starts with "EmotiBit" (preferredName, if seen, wins), connects and
    // enables notifications.
    void start (Source source, const QString &preferredName, double scanTimeoutSec);
    void stop ();

    State state () const
    {
        return static_cast<State> (shared_->state.load ());
    }
    bool linkUp () const
    {
        return shared_->linkUp.load ();
    }
    std::uint64_t packetsForwarded () const
    {
        return shared_->forwarded.load ();
    }
    // Failed because the scan found no EmotiBit (Bluetooth itself worked).
    bool failedNotFound () const
    {
        return shared_->notFound.load ();
    }
    QString deviceName () const;
    QString error () const;
    QString status () const;

    // Complete packets ('\n'-terminated) taken from the front of buf; a partial
    // last packet stays in buf. Pure (selftest).
    static QByteArray takeCompletePackets (QByteArray &buf);
    // The EmotiBit id in an advertised name ("EmotiBit: MD-V7-0001421" ->
    // "MD-V7-0001421"), or the name itself. Pure.
    static QString deviceIdFromName (const QString &name);

    struct Shared
    {
        std::atomic<int> state {Idle};
        std::atomic<bool> linkUp {false};
        std::atomic<std::uint64_t> forwarded {0};
        std::atomic<bool> notFound {false};
        mutable std::mutex mutex;
        QString name, error, status;
    };

private:
    class Impl;
    std::shared_ptr<Shared> shared_;
    std::unique_ptr<QThread> thread_;
    Impl *impl_ = nullptr;
};
