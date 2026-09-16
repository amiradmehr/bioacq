#pragma once

#include "EmotiBitDiscovery.h"
#include "Retimer.h"
#include "RingBuffer.h"
#include "SignalSpec.h"

#include "brainflow_input_params.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class BoardShim;

struct DeviceConfig
{
    std::string slot;        // "cyton" / "emotibit" (logs)
    std::string displayName; // "OpenBCI Cyton"
    int boardId = -1;
    BrainFlowInputParams params;
    std::vector<ResolvedSignal> signalSpecs; // ("signals" is a Qt keyword)

    // Record from the first sample (file streamers added before start_stream).
    // A recording can also be started / stopped later: requestRecording().
    bool record = false;
    std::string recordDir;
    std::string filePrefix; // "<device>" or "<device>_synthetic"
    std::string stamp;      // yyyyMMdd_HHmmss

    int pollIntervalMs = 10;
    int bufferSize = 60000;    // BrainFlow ring buffer, samples per preset
    double ringSeconds = 130.0; // our display ring: seconds at max(nominal, 250 Hz)

    // EmotiBit only. true: find the device with our own cancellable UDP
    // discovery (EmotiBitDiscovery) BEFORE prepare_session -- for a typed IP
    // as well as for broadcast -- so BrainFlow's process-wide lock is not
    // held while searching. false: let BrainFlow discover (--bf-discovery).
    bool ownDiscovery = true;
    int discoveryPort = emotibit::kAdvertisingPort;
    double discoveryTimeoutSec = -1.0; // <= 0: use params.timeout
    // Own discovery only: when unicast to params.ip_address (a typed IP or the
    // last IP that answered) finds nothing within kUnicastTrySeconds, search
    // again by broadcast on every interface (same subnet): the EmotiBit may
    // have joined another network, e.g. a hotspot, and got a new address.
    bool broadcastFallback = false;

    // Count gaps in the board's package-number row (Cyton: 0..255 per
    // sample) as dropped packets; resolved by start().
    bool countPackageGaps = false;
    int packageNumRow = -1;
    int packageNumPreset = 0;

    // Test only (--selftest): sleep this long right before prepare_session,
    // ignoring stop requests, to simulate a prepare_session that is still
    // blocking when the user disconnects or closes the window.
    int testPrepareDelayMs = 0;
    // Test hooks (command line only, --test-stall-emotibit /
    // --test-rail-offset-uv): stop polling after this many seconds of
    // streaming (the stream stalls, the session stays open), and add a DC
    // offset to the raw value of filterable signals in the DISPLAY ring (the
    // near-rail path). BrainFlow's recordings are unaffected by both.
    double testFreezeAfterSec = -1.0;
    double testRawOffset = 0.0;
    // Test only (--selftest): broadcast targets for the fallback instead of
    // this computer's interface broadcast addresses (loopback fake EmotiBit).
    std::vector<std::string> testBroadcastTargets;
    // Test hook (--test-ppg-bpm): replace the PPG display values (and so the
    // heart-rate input) with HeartRate::syntheticPpg at this rate. 0 = off.
    double testPpgBpm = 0.0;
};

// One plotted signal: its resolved spec and the ring buffer the worker fills.
// Ring channel layout: one channel per trace; filterable signals get one more
// channel at index rawChannel holding the unfiltered value. A derived signal
// (heart rate) has no BrainFlow rows: its ring holds HeartRateRing channels.
struct SignalChannel
{
    ResolvedSignal spec;
    std::shared_ptr<SignalRing> ring;
    int rawChannel = -1;

    // worker-thread bookkeeping (the worker owns its own copy of the vector)
    bool seenData = false;
    double zeroSkipUntil = 0.0;
    bool retime = false; // EmotiBit: spread each packet's samples over time (display only)
    PacketRetimer retimer;
};

// Owns one BrainFlow session on a dedicated std::thread.
//
// The GUI thread only calls start()/requestStop()/setters, which never block
// on BrainFlow. prepare_session, add_streamer / delete_streamer,
// start_stream, polling, stop_stream and release_session all run on the
// worker thread. Stop requests are honoured as soon as BrainFlow returns
// control: if the user disconnects or closes the window while
// prepare_session is still blocking, the worker releases the session right
// after it returns, so no session is leaked.
//
// BrainFlow guards every session call of every board with ONE process-wide
// mutex, so a long prepare_session in one worker stalls the other worker's
// polling. For the EmotiBit the worker therefore finds the device itself
// first (EmotiBitDiscovery, cancellable, lock-free) and only then calls
// prepare_session with the found IP, which keeps the lock held ~1-7 s.
//
// Recording: BrainFlow's file streamers can be added to and removed from a
// RUNNING session (Board::add_streamer / delete_streamer take the board's
// spinlock that push_package holds while streaming; deleting a FileStreamer
// fcloses the file), so requestRecording() starts / stops a recording at any
// time; the worker applies it within one poll interval.
class DeviceWorker : public QObject
{
    Q_OBJECT

public:
    enum State
    {
        Idle = 0,
        Connecting = 1,
        Streaming = 2,
        Stopping = 3
    };
    Q_ENUM (State)

    // Finer-grained connect progress for the UI.
    enum Phase
    {
        PhaseIdle = 0,
        PhaseDiscovering = 1, // EmotiBit: our UDP discovery (lock-free, cancellable)
        PhaseOpening = 2,     // inside BrainFlow's prepare_session
        PhaseStreaming = 3,
        PhaseStopping = 4
    };

    enum FailureKind
    {
        FailNone = 0,
        FailNotFound = 1, // discovery ran and nothing answered / the serial port does not exist
        FailNoSend = 2,   // discovery could not send (no interface / permission)
        FailOther = 3,
        FailPortBusy = 4  // Cyton: the port exists but cannot be opened or the board does not answer
    };

    explicit DeviceWorker (QObject *parent = nullptr);
    ~DeviceWorker () override;

    // Starts the worker thread. Returns false if a session is still active.
    bool start (const DeviceConfig &cfg);
    // Non-blocking; the thread winds down and emits disconnected()+finished().
    void requestStop ();
    // True from start() until the finished thread has been joined.
    bool isActive () const
    {
        return thread_.joinable ();
    }
    // Waits for the thread to finish and joins it. Returns false on timeout.
    bool waitForFinished (int timeoutMs);

    State state () const
    {
        return static_cast<State> (state_.load ());
    }
    Phase phase () const
    {
        return static_cast<Phase> (phase_.load ());
    }
    // steadyNow() time at which the current phase began.
    double phaseSince () const
    {
        return phaseSince_.load ();
    }
    // HELLO_EMOTIBIT rounds sent by the current / last discovery.
    int probesSent () const
    {
        return probes_.load ();
    }
    // Discovery timeout of the current / last session (s).
    double discoveryTimeout () const
    {
        return discTimeout_;
    }
    FailureKind failureKind () const
    {
        return static_cast<FailureKind> (failKind_.load ());
    }
    // Packets missing from the package-number sequence since start()
    // (valid only if countsDrops()).
    std::uint64_t droppedPackets () const
    {
        return dropped_.load ();
    }
    bool countsDrops () const
    {
        return countDrops_.load ();
    }

    const std::vector<SignalChannel> &channels () const
    {
        return channels_;
    }
    const DeviceConfig &config () const
    {
        return cfg_;
    }

    // Filter settings (applied to filterable signals, state reset on change).
    // Chain order: high-pass, notch, low-pass.
    void setHighPass (bool on, double hz = 0.5);
    void setNotch (bool on, double hz = 60.0);
    void setLowPass (bool on, double hz = 40.0);

    // Start (on) or stop (off) recording every preset to
    // <dir>/<prefix>_<preset>_<stamp>.csv. Applied by the worker thread within
    // one poll interval while streaming, or as soon as streaming starts.
    void requestRecording (bool on, const std::string &dir, const std::string &stamp);
    bool isRecording () const
    {
        return recording_.load ();
    }
    QStringList recordingFiles () const;

    // True while this worker is inside BrainFlow's prepare_session (holding,
    // or queued on, BrainFlow's global lock). The other device's polling is
    // stalled meanwhile.
    bool inBrainFlowSetup () const
    {
        return inSetup_.load ();
    }
    // Upper bound (s) for how long a stop request can take to be honoured
    // with the current configuration (BrainFlow calls cannot be interrupted).
    double worstCaseStopSeconds () const
    {
        return worstCaseStop_;
    }

    static double steadyNow ();
    static QString describeError (int exitCode, int boardId);
    static QString emotibitNetworkHint ();
    // "Broadcast discovery only works on the same subnet ... enter the IP".
    static QString emotibitSubnetHint ();
    // User-facing text for a discovery that found nothing.
    static QString discoveryFailureText (
        const emotibit::DiscoveryResult &r, const std::string &typedIp, double timeoutSec);

signals:
    void stateChanged (int state);
    void progress (const QString &text); // connecting-phase status for the device panel
    void discovered (const QString &ip, const QString &serial); // EmotiBit answered
    void connected (const QString &info);
    void failed (const QString &error);
    void disconnected (const QString &info);
    void finished ();
    void message (const QString &text);
    void dataStall (bool stalled);
    void recordingStarted (const QStringList &files);
    void recordingStopped (const QStringList &files); // files are closed

private:
    void run (DeviceConfig cfg, std::vector<SignalChannel> chans);
    // Our own EmotiBit discovery. Returns false if cancelled, throws if no
    // EmotiBit answered; on success cfg.params.ip_address is the device.
    bool discoverEmotibit (DeviceConfig &cfg);
    void pollLoop (BoardShim &board, const DeviceConfig &cfg, std::vector<SignalChannel> &chans);
    void startRecording (BoardShim &board, const DeviceConfig &cfg, const std::string &dir, const std::string &stamp);
    void stopRecording (BoardShim &board);
    void applyRecordingRequest (BoardShim &board, const DeviceConfig &cfg);
    void setState (State s);
    void setPhase (Phase p);
    bool waitStop (int ms); // sleep up to ms; true if a stop was requested

    std::thread thread_;
    std::atomic<bool> stop_ {false};
    std::atomic<bool> done_ {true};
    std::atomic<int> state_ {Idle};
    std::atomic<int> phase_ {PhaseIdle};
    std::atomic<double> phaseSince_ {0.0};
    std::atomic<int> probes_ {0};
    std::atomic<int> failKind_ {FailNone};
    double discTimeout_ = 0.0; // GUI thread only (set in start())
    std::atomic<std::uint64_t> dropped_ {0};
    std::atomic<bool> countDrops_ {false};
    std::atomic<bool> inSetup_ {false};
    double worstCaseStop_ = 10.0; // GUI thread only (set in start())
    std::mutex waitMutex_;
    std::condition_variable waitCv_;

    std::atomic<bool> hpOn_ {false};
    std::atomic<bool> notchOn_ {false};
    std::atomic<bool> lpOn_ {false};
    std::atomic<double> hpHz_ {0.5};
    std::atomic<double> notchHz_ {60.0};
    std::atomic<double> lpHz_ {40.0};
    std::atomic<unsigned> filterGen_ {0};

    // recording requests (GUI -> worker) and state
    std::mutex recMutex_;
    bool recWant_ = false;
    std::string recDir_, recStamp_;
    std::atomic<unsigned> recGen_ {0};
    unsigned startRecGen_ = 0; // recGen_ when the session was started
    std::atomic<bool> recording_ {false};
    std::vector<std::pair<std::string, int>> streamers_; // worker thread only

    DeviceConfig cfg_;
    std::vector<SignalChannel> channels_;

    mutable std::mutex filesMutex_;
    QStringList files_;
};
