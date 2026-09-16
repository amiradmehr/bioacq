#pragma once

#include <QString>

#include <csignal>

// Set from a SIGINT/SIGTERM handler; headless loops stop early (cleanly).
extern volatile std::sig_atomic_t g_interruptRequested;

struct ProbeOptions
{
    QString cytonPort; // empty = auto-detect the OpenBCI dongle (findCytonDongle)
    QString emotibitIp = QStringLiteral ("192.168.1.12"); // blank = broadcast discovery
    int emotibitTimeoutSec = 5;
    bool bfDiscovery = false; // let BrainFlow discover the EmotiBit (holds its global lock)
    double seconds = 8.0;
    bool record = false;
    QString recordDir;
    bool skipCyton = false;
    bool skipEmotibit = false;
};

// --selftest: both device slots on BrainFlow's SYNTHETIC_BOARD through the
// same DeviceWorker / SignalRing code the GUI uses, plus the EmotiBit
// discovery against a local fake device. Returns 0 on success.
int runSelftest (double seconds);

// --probe: the real devices; reports per-signal sample counts and rates.
// Never crashes when a device is absent. Returns 0 if every connected device
// delivered data on all its signals, 1 if nothing connected, 2 otherwise.
int runProbe (const ProbeOptions &options);

// --emotibit-wifi-list: the WiFi networks saved on the EmotiBit, read over its
// USB serial port (port empty = the likeliest candidate). Restarts the
// EmotiBit; prints SSIDs only. Returns 0 on success.
int runEmotibitWifiList (const QString &port);
