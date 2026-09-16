#include "DeviceWorker.h"

#include "AppPaths.h"
#include "Biquad.h"
#include "HeartRate.h"
#include "RateMeter.h"
#include "Readouts.h"
#include "SerialPorts.h"

#include "board_shim.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace
{

// params.timeout handed to BrainFlow once our own discovery has found the
// EmotiBit. BrainFlow then only re-greets that one address; one HELLO
// exchange is all it needs (values < 2 would be forced to 4 by BrainFlow).
constexpr int kBrainFlowEmotiTimeout = 2;
// With a broadcast fallback, the unicast try at the typed / remembered IP is
// short: a live EmotiBit answers HELLO_EMOTIBIT within ~0.1 s.
constexpr double kUnicastTrySeconds = 2.0;

QString cleanWhat (const BrainFlowException &e)
{
    std::string w = e.what ();
    const std::string code = std::to_string (e.exit_code);
    if (w.size () >= code.size () && w.compare (w.size () - code.size (), code.size (), code) == 0)
        w.erase (w.size () - code.size ());
    while (!w.empty () && (w.back () == ' ' || w.back () == ':'))
        w.pop_back ();
    return QString::fromStdString (w);
}

std::string homeFallbackDir ()
{
    return fallbackRecordDir ().toStdString ();
}

QString joinTargets (const std::vector<std::string> &targets)
{
    // at most 4 addresses: a subnet scan adds up to 253 per interface
    const std::size_t shown = std::min<std::size_t> (targets.size (), 4);
    QStringList l;
    for (std::size_t i = 0; i < shown; ++i)
        l << QString::fromStdString (targets[i]);
    QString s = l.join (QStringLiteral (", "));
    if (targets.size () > shown)
        s += QStringLiteral (" and %1 more").arg (targets.size () - shown);
    return s;
}

} // namespace

DeviceWorker::DeviceWorker (QObject *parent) : QObject (parent)
{
}

DeviceWorker::~DeviceWorker ()
{
    requestStop ();
    if (thread_.joinable ())
        thread_.join ();
}

double DeviceWorker::steadyNow ()
{
    return std::chrono::duration<double> (std::chrono::steady_clock::now ().time_since_epoch ()).count ();
}

bool DeviceWorker::start (const DeviceConfig &cfg)
{
    if (thread_.joinable ())
    {
        if (!done_.load ())
            return false; // still running
        thread_.join ();
    }

    cfg_ = cfg;
    channels_.clear ();
    for (const ResolvedSignal &s : cfg.signalSpecs)
    {
        SignalChannel ch;
        ch.spec = s;
        int nch = static_cast<int> (s.rows.size ());
        if (s.filterable)
        {
            ch.rawChannel = nch;
            nch += 1;
        }
        const double rate = std::max (s.nominalRate, 250.0);
        if (s.derived)
            // heart rate: one sample per beat (<= 200 bpm) or status second
            ch.ring = std::make_shared<SignalRing> (
                HeartRateRing::Count, static_cast<std::size_t> (4.0 * cfg.ringSeconds) + 16);
        else
            ch.ring = std::make_shared<SignalRing> (
                nch, static_cast<std::size_t> (rate * cfg.ringSeconds) + 16);
        // EmotiBit stamps every sample of a packet with one host time; spread
        // them for display (never for the filterable EXG path / other boards).
        ch.retime = cfg.boardId == static_cast<int> (BoardIds::EMOTIBIT_BOARD) && !s.filterable && !s.derived;
        ch.retimer.reset (s.nominalRate);
        channels_.push_back (ch);
    }

    // Package-number row of the first filterable signal's preset (dropped
    // packet counter). Static getter: does not take BrainFlow's session lock.
    cfg_.packageNumRow = -1;
    if (cfg_.countPackageGaps)
        for (const ResolvedSignal &s : cfg_.signalSpecs)
            if (s.filterable)
            {
                try
                {
                    cfg_.packageNumRow = BoardShim::get_package_num_channel (cfg_.boardId, s.preset);
                    cfg_.packageNumPreset = s.preset;
                }
                catch (const std::exception &)
                {
                    cfg_.packageNumRow = -1;
                }
                break;
            }
    countDrops_.store (cfg_.packageNumRow >= 0);
    dropped_.store (0);

    // How long a stop may take once requested, given that BrainFlow calls
    // cannot be interrupted (feeds the close-window watchdog).
    if (cfg.boardId == static_cast<int> (BoardIds::EMOTIBIT_BOARD))
    {
        if (cfg.ownDiscovery)
            // our discovery: cancellable within ~0.1 s. Then prepare_session with
            // the found IP: one HELLO exchange (<= 5 s recv if the reply is lost)
            // + BrainFlow's fixed 1 s sleep + TCP accept wait (20 x 300 ms);
            // then stop_stream / release_session.
            worstCaseStop_ = 0.2 + 5.0 + 1.0 + 6.0 + 8.0;
        else
        {
            // BrainFlow's discovery: (timeout + one 5 s recv) per broadcast address.
            const double nAddr = cfg.params.ip_address.empty ()
                ? static_cast<double> (emotibit::ipv4BroadcastAddresses ().size () + 1)
                : 1.0;
            const double to = cfg.params.timeout < 2 ? 4.0 : static_cast<double> (cfg.params.timeout);
            worstCaseStop_ = nAddr * (to + 5.0) + 1.0 + 6.0 + 8.0;
        }
    }
    else if (cfg.boardId == static_cast<int> (BoardIds::CYTON_BOARD))
        worstCaseStop_ = 15.0; // serial open + soft reset + stop/release
    else
        worstCaseStop_ = 5.0;
    worstCaseStop_ += cfg.testPrepareDelayMs / 1000.0;
    discTimeout_ = cfg.discoveryTimeoutSec > 0.0 ? cfg.discoveryTimeoutSec
                                                 : std::max (2.0, static_cast<double> (cfg.params.timeout));
    {
        std::lock_guard<std::mutex> lock (filesMutex_);
        files_.clear ();
    }

    stop_.store (false);
    done_.store (false);
    inSetup_.store (false);
    recording_.store (false);
    probes_.store (0);
    failKind_.store (FailNone);
    startRecGen_ = recGen_.load ();
    filterGen_.fetch_add (1);
    setState (Connecting);
    setPhase (PhaseIdle);
    thread_ = std::thread (&DeviceWorker::run, this, cfg_, channels_);
    return true;
}

void DeviceWorker::requestStop ()
{
    stop_.store (true);
    {
        std::lock_guard<std::mutex> lock (waitMutex_);
    }
    waitCv_.notify_all ();
}

bool DeviceWorker::waitForFinished (int timeoutMs)
{
    if (!thread_.joinable ())
        return true;
    {
        std::unique_lock<std::mutex> lock (waitMutex_);
        if (!waitCv_.wait_for (lock, std::chrono::milliseconds (timeoutMs),
                [this] { return done_.load (); }))
            return false;
    }
    thread_.join ();
    return true;
}

void DeviceWorker::setHighPass (bool on, double hz)
{
    hpHz_.store (hz);
    hpOn_.store (on);
    filterGen_.fetch_add (1);
}

void DeviceWorker::setNotch (bool on, double hz)
{
    notchHz_.store (hz);
    notchOn_.store (on);
    filterGen_.fetch_add (1);
}

void DeviceWorker::setLowPass (bool on, double hz)
{
    lpHz_.store (hz);
    lpOn_.store (on);
    filterGen_.fetch_add (1);
}

void DeviceWorker::requestRecording (bool on, const std::string &dir, const std::string &stamp)
{
    {
        std::lock_guard<std::mutex> lock (recMutex_);
        recWant_ = on;
        recDir_ = dir;
        recStamp_ = stamp;
    }
    recGen_.fetch_add (1);
}

QStringList DeviceWorker::recordingFiles () const
{
    std::lock_guard<std::mutex> lock (filesMutex_);
    return files_;
}

void DeviceWorker::setState (State s)
{
    state_.store (s);
    emit stateChanged (s);
}

void DeviceWorker::setPhase (Phase p)
{
    phaseSince_.store (steadyNow ());
    phase_.store (p);
}

bool DeviceWorker::waitStop (int ms)
{
    std::unique_lock<std::mutex> lock (waitMutex_);
    return waitCv_.wait_for (
        lock, std::chrono::milliseconds (ms), [this] { return stop_.load (); });
}

QString DeviceWorker::describeError (int code, int boardId)
{
    const char *name = "UNKNOWN_ERROR";
    switch (static_cast<BrainFlowExitCodes> (code))
    {
        case BrainFlowExitCodes::PORT_ALREADY_OPEN_ERROR:
            name = "PORT_ALREADY_OPEN_ERROR";
            break;
        case BrainFlowExitCodes::UNABLE_TO_OPEN_PORT_ERROR:
            name = "UNABLE_TO_OPEN_PORT_ERROR";
            break;
        case BrainFlowExitCodes::SET_PORT_ERROR:
            name = "SET_PORT_ERROR";
            break;
        case BrainFlowExitCodes::BOARD_WRITE_ERROR:
            name = "BOARD_WRITE_ERROR";
            break;
        case BrainFlowExitCodes::INCOMMING_MSG_ERROR:
            name = "INCOMMING_MSG_ERROR";
            break;
        case BrainFlowExitCodes::INITIAL_MSG_ERROR:
            name = "INITIAL_MSG_ERROR";
            break;
        case BrainFlowExitCodes::BOARD_NOT_READY_ERROR:
            name = "BOARD_NOT_READY_ERROR";
            break;
        case BrainFlowExitCodes::STREAM_ALREADY_RUN_ERROR:
            name = "STREAM_ALREADY_RUN_ERROR";
            break;
        case BrainFlowExitCodes::INVALID_BUFFER_SIZE_ERROR:
            name = "INVALID_BUFFER_SIZE_ERROR";
            break;
        case BrainFlowExitCodes::STREAM_THREAD_ERROR:
            name = "STREAM_THREAD_ERROR";
            break;
        case BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR:
            name = "INVALID_ARGUMENTS_ERROR";
            break;
        case BrainFlowExitCodes::UNSUPPORTED_BOARD_ERROR:
            name = "UNSUPPORTED_BOARD_ERROR";
            break;
        case BrainFlowExitCodes::BOARD_NOT_CREATED_ERROR:
            name = "BOARD_NOT_CREATED_ERROR";
            break;
        case BrainFlowExitCodes::ANOTHER_BOARD_IS_CREATED_ERROR:
            name = "ANOTHER_BOARD_IS_CREATED_ERROR";
            break;
        case BrainFlowExitCodes::GENERAL_ERROR:
            name = "GENERAL_ERROR";
            break;
        case BrainFlowExitCodes::SYNC_TIMEOUT_ERROR:
            name = "SYNC_TIMEOUT_ERROR";
            break;
        default:
            break;
    }

    const bool cyton = boardId == static_cast<int> (BoardIds::CYTON_BOARD);
    const bool emotibit = boardId == static_cast<int> (BoardIds::EMOTIBIT_BOARD);
    const QString verbose = QStringLiteral ("Run with --verbose to see BrainFlow's log.");
    QString hint;
    switch (static_cast<BrainFlowExitCodes> (code))
    {
        case BrainFlowExitCodes::UNABLE_TO_OPEN_PORT_ERROR:
        case BrainFlowExitCodes::SET_PORT_ERROR:
            if (cyton)
                hint = "Cannot open the serial port. Check the dongle is plugged in and that no "
                       "other program (OpenBCI GUI, a serial monitor) has it open.";
            else if (emotibit)
                hint = emotibitNetworkHint () + "\n" + verbose;
            else
                hint = "Cannot open/configure the port or network socket.";
            break;
        case BrainFlowExitCodes::PORT_ALREADY_OPEN_ERROR:
            hint = "The port is already open.";
            break;
        case BrainFlowExitCodes::BOARD_NOT_READY_ERROR:
            if (cyton)
                // macOS lets several processes open the same cu.* device, so a
                // busy dongle shows up here (missing welcome bytes), not as
                // UNABLE_TO_OPEN_PORT.
                hint = "Close any other program using the dongle (OpenBCI GUI, a serial "
                       "monitor), then: switch the board to PC, check its battery, set the "
                       "dongle switch to GPIO6, and power-cycle board + dongle.";
            else if (emotibit)
                hint = emotibitNetworkHint ();
            break;
        case BrainFlowExitCodes::INITIAL_MSG_ERROR:
        case BrainFlowExitCodes::INCOMMING_MSG_ERROR:
            hint = "Unexpected data from the board: power-cycle it and reconnect.";
            break;
        case BrainFlowExitCodes::ANOTHER_BOARD_IS_CREATED_ERROR:
            hint = "A session with identical settings is already open in this process.";
            break;
        case BrainFlowExitCodes::GENERAL_ERROR:
        case BrainFlowExitCodes::SYNC_TIMEOUT_ERROR:
            // EmotiBit: socket init / sendto failures (e.g. Local Network access
            // denied, no route) are reported as GENERAL_ERROR.
            hint = emotibit ? emotibitNetworkHint () + "\n" + verbose : verbose;
            break;
        default:
            break;
    }
    QString out = QString ("%1 (code %2)").arg (QString::fromLatin1 (name)).arg (code);
    if (!hint.isEmpty ())
        out += "\n" + hint;
    return out;
}

namespace
{

// What the OS needs to allow before UDP to the EmotiBit gets through.
QString networkPermissionHint ()
{
#ifdef _WIN32
    return QStringLiteral (
        "Windows Defender Firewall allows BioAcq on this network (accept the firewall prompt, or allow it "
        "under Windows Security > Firewall & network protection > Allow an app through firewall)");
#else
    return QStringLiteral (
        "macOS allows Local Network access for the app that launched this program (System Settings > "
        "Privacy & Security > Local Network)");
#endif
}

} // namespace

QString DeviceWorker::emotibitNetworkHint ()
{
    return QStringLiteral (
               "Check: the EmotiBit is on and has joined the WiFi; EmotiBit Oscilloscope (and any other "
               "program streaming from it) is CLOSED, because only one host can own the stream; the IP "
               "address is right (EmotiBit serial boot log or EmotiBit Oscilloscope); ") +
        networkPermissionHint () + QStringLiteral (".");
}

QString DeviceWorker::emotibitSubnetHint ()
{
    return QStringLiteral (
        "Broadcast discovery only works when this computer and the EmotiBit are on the same subnet. "
        "Being on the same WiFi network (SSID) is not enough: the computer can end up on e.g. "
        "172.31.x.x while the EmotiBit is on 192.168.1.x. Enter the EmotiBit's IP address "
        "(printed in its serial boot log, or shown in EmotiBit Oscilloscope) and connect again; "
        "a typed IP is reached by unicast across subnets.");
}

QString DeviceWorker::discoveryFailureText (
    const emotibit::DiscoveryResult &r, const std::string &typedIp, double timeoutSec)
{
    const QString where = joinTargets (r.targets);
    const QString secs = QString::number (timeoutSec, 'g', 3);
    if (!r.anySendOk && r.lastSendErrno == 0)
    {
        QString s = QStringLiteral ("EmotiBit discovery could not start: %1.")
                        .arg (QString::fromStdString (r.error));
        if (typedIp.empty ())
            s += "\n" + emotibitSubnetHint ();
        return s;
    }
    if (!r.anySendOk)
        return QStringLiteral ("could not send the EmotiBit discovery packet to %1 (%2).\n"
                               "Check that %3, that WiFi is on and that the IP address is right.")
            .arg (where, QString::fromLocal8Bit (emotibit::socketErrorText (r.lastSendErrno).c_str ()),
                networkPermissionHint ());
    if (typedIp.empty ())
        return QStringLiteral ("no EmotiBit answered the broadcast discovery (sent to %1) within %2 s.\n")
                   .arg (where.isEmpty () ? QStringLiteral ("no interface") : where, secs) +
            emotibitSubnetHint () +
            QStringLiteral ("\nAlso make sure EmotiBit Oscilloscope is closed.");
    return QStringLiteral (
        "no EmotiBit answered at %1 within %2 s.\n"
        "Check the IP address (printed in the EmotiBit's serial boot log, or shown in EmotiBit "
        "Oscilloscope), that the EmotiBit is powered on and has joined the WiFi, and that EmotiBit "
        "Oscilloscope is closed. Leave the IP blank to try broadcast discovery (same subnet only).")
        .arg (QString::fromStdString (typedIp), secs);
}

bool DeviceWorker::discoverEmotibit (DeviceConfig &cfg)
{
    const std::string typed = cfg.params.ip_address;
    // A search of this computer's networks: every interface's broadcast
    // address plus a unicast scan of its small subnets (/24 or narrower),
    // because some hotspots (an iPhone's) drop broadcasts.
    QString searchText;
    auto networkTargets = [&cfg, &searchText] {
        if (!cfg.testBroadcastTargets.empty ())
        {
            searchText = joinTargets (cfg.testBroadcastTargets);
            return cfg.testBroadcastTargets;
        }
        std::vector<std::string> t = emotibit::ipv4BroadcastAddresses ();
        searchText = t.empty () ? QStringLiteral ("no interface") : joinTargets (t);
        std::vector<std::string> subnets;
        const std::vector<std::string> hosts = emotibit::ipv4ScanHosts (&subnets);
        if (!hosts.empty ())
            searchText += QStringLiteral (" + a scan of ") + joinTargets (subnets);
        t.insert (t.end (), hosts.begin (), hosts.end ());
        return t;
    };
    const std::vector<std::string> targets = typed.empty () ? networkTargets () : std::vector<std::string> {typed};
    const double timeout = cfg.discoveryTimeoutSec > 0.0
        ? cfg.discoveryTimeoutSec
        : std::max (2.0, static_cast<double> (cfg.params.timeout));
    const QString secs = QString::number (timeout, 'g', 3);
    const bool fallback = !typed.empty () && cfg.broadcastFallback;
    const double firstTimeout = fallback ? std::min (timeout, kUnicastTrySeconds) : timeout;
    emit progress (typed.empty ()
            ? QStringLiteral ("discovering EmotiBit: broadcast to %1 (same subnet only, up to %2 s)…")
                  .arg (searchText, secs)
            : QStringLiteral ("looking for the EmotiBit at %1 (up to %2 s)…")
                  .arg (QString::fromStdString (typed), QString::number (firstTimeout, 'g', 3)));

    emotibit::DiscoveryResult r = emotibit::discover (targets, cfg.discoveryPort, firstTimeout,
        cfg.params.serial_number, [this] { return stop_.load (); }, &probes_);
    if (r.cancelled)
        return false;
    if (!r.found && fallback)
    {
        // The typed / remembered IP did not answer: the EmotiBit may have a
        // new address, on this subnet or on another network both joined.
        const std::vector<std::string> bc = networkTargets ();
        emit progress (QStringLiteral ("no answer at %1; broadcast discovery on %2 (same subnet only, up to %3 s)…")
                           .arg (QString::fromStdString (typed), searchText, secs));
        setPhase (PhaseDiscovering); // restarts the elapsed / timeout readout
        probes_.store (0);
        r = emotibit::discover (bc, cfg.discoveryPort, timeout, cfg.params.serial_number,
            [this] { return stop_.load (); }, &probes_);
        if (r.cancelled)
            return false;
        if (!r.found)
        {
            failKind_.store (r.anySendOk ? FailNotFound : FailNoSend);
            throw std::runtime_error (discoveryFailureText (r, std::string (), timeout).toStdString ());
        }
    }
    if (!r.found)
    {
        failKind_.store (r.anySendOk ? FailNotFound : FailNoSend);
        throw std::runtime_error (discoveryFailureText (r, typed, timeout).toStdString ());
    }

    cfg.params.ip_address = r.ip;
    cfg.params.timeout = kBrainFlowEmotiTimeout;
    emit discovered (QString::fromStdString (r.ip), QString::fromStdString (r.serial));
    emit progress (QStringLiteral ("EmotiBit%1 answered at %2 after %3 s; opening the BrainFlow session…")
                       .arg (r.serial.empty () ? QString () : QStringLiteral (" ") + QString::fromStdString (r.serial),
                           QString::fromStdString (r.ip), QString::number (r.seconds, 'f', 1)));
    return true;
}

void DeviceWorker::run (DeviceConfig cfg, std::vector<SignalChannel> chans)
{
    const auto t0 = std::chrono::steady_clock::now ();
    auto elapsed = [&t0] () {
        return std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count ();
    };

    std::unique_ptr<BoardShim> board;
    bool prepared = false;
    bool streaming = false;
    bool cancelled = false;
    QString error;

    try
    {
        if (!cfg.params.serial_port.empty ())
        {
            const QString port = existingSerialPort (QString::fromStdString (cfg.params.serial_port));
            if (port.isEmpty ())
            {
                failKind_.store (FailNotFound);
                throw std::runtime_error ("serial port " + cfg.params.serial_port +
                    " does not exist (dongle unplugged? press the rescan button)");
            }
            cfg.params.serial_port = port.toStdString (); // the OS spelling: BrainFlow's COM10+ prefix needs "COM"
        }
        else if (cfg.boardId == static_cast<int> (BoardIds::CYTON_BOARD))
        {
            failKind_.store (FailNotFound); // auto-detect found no dongle (--probe without --port)
            throw std::runtime_error ("no OpenBCI dongle found (plug it in, or give its serial port)");
        }

        // EmotiBit: find the device without holding BrainFlow's global lock.
        if (cfg.boardId == static_cast<int> (BoardIds::EMOTIBIT_BOARD) && cfg.ownDiscovery)
        {
            setPhase (PhaseDiscovering);
            cancelled = !discoverEmotibit (cfg);
        }

        if (!cancelled)
        {
            board = std::make_unique<BoardShim> (cfg.boardId, cfg.params);
            if (stop_.load ())
                cancelled = true;
            else
            {
                setPhase (PhaseOpening);
                inSetup_.store (true);
                if (cfg.testPrepareDelayMs > 0) // test only: a prepare that ignores stop
                    std::this_thread::sleep_for (std::chrono::milliseconds (cfg.testPrepareDelayMs));
                board->prepare_session (); // blocks: serial reset / network handshake
                inSetup_.store (false);
                prepared = true;
                if (stop_.load ())
                    cancelled = true; // user gave up while we were connecting
            }
        }

        if (!cancelled)
        {
            if (cfg.record)
                startRecording (*board, cfg, cfg.recordDir, cfg.stamp);
            board->start_stream (cfg.bufferSize);
            streaming = true;
            setPhase (PhaseStreaming);
            setState (Streaming);
            emit connected (QString ("session ready in %1 s").arg (elapsed (), 0, 'f', 1));
            pollLoop (*board, cfg, chans);
        }
    }
    catch (const BrainFlowException &e)
    {
        error = cleanWhat (e) + ": " + describeError (e.exit_code, cfg.boardId);
        const auto code = static_cast<BrainFlowExitCodes> (e.exit_code);
        if (cfg.boardId == static_cast<int> (BoardIds::CYTON_BOARD) &&
            (code == BrainFlowExitCodes::UNABLE_TO_OPEN_PORT_ERROR || code == BrainFlowExitCodes::SET_PORT_ERROR ||
                code == BrainFlowExitCodes::PORT_ALREADY_OPEN_ERROR || code == BrainFlowExitCodes::BOARD_NOT_READY_ERROR))
            failKind_.store (FailPortBusy);
    }
    catch (const std::exception &e)
    {
        error = QString::fromStdString (e.what ());
    }
    inSetup_.store (false);
    if (!error.isEmpty () && failKind_.load () == FailNone)
        failKind_.store (FailOther);

    setPhase (PhaseStopping);
    setState (Stopping);
    if (board)
    {
        if (streaming)
        {
            try
            {
                board->stop_stream ();
            }
            catch (const std::exception &)
            {
            }
        }
        if (prepared)
        {
            try
            {
                board->release_session (); // also deletes (closes) any file streamers
            }
            catch (const std::exception &)
            {
            }
        }
        board.reset ();
    }
    streamers_.clear ();
    if (recording_.exchange (false))
        emit recordingStopped (recordingFiles ());

    if (!error.isEmpty ())
        emit failed (error);
    else
        emit disconnected (cancelled ? QStringLiteral ("cancelled") : QStringLiteral ("stopped"));

    setPhase (PhaseIdle);
    state_.store (Idle);
    {
        std::lock_guard<std::mutex> lock (waitMutex_);
        done_.store (true);
    }
    waitCv_.notify_all ();
    emit finished ();
}

void DeviceWorker::applyRecordingRequest (BoardShim &board, const DeviceConfig &cfg)
{
    bool want;
    std::string dir, stamp;
    {
        std::lock_guard<std::mutex> lock (recMutex_);
        want = recWant_;
        dir = recDir_;
        stamp = recStamp_;
    }
    if (want && !recording_.load ())
        startRecording (board, cfg, dir, stamp);
    else if (!want && recording_.load ())
        stopRecording (board);
}

void DeviceWorker::pollLoop (
    BoardShim &board, const DeviceConfig &cfg, std::vector<SignalChannel> &chans)
{
    std::vector<int> presets;
    for (const SignalChannel &ch : chans)
        if (std::find (presets.begin (), presets.end (), ch.spec.preset) == presets.end ())
            presets.push_back (ch.spec.preset);

    std::vector<FilterChain> filters (chans.size ());
    unsigned appliedGen = filterGen_.load () - 1; // force initial build
    unsigned appliedRecGen = startRecGen_;
    std::vector<double> filtered;
    filtered.reserve (4096);
    std::vector<double> rawAdj; // test hook: raw + offset
    std::vector<double> tsBuf;  // rebuilt per-sample display times (EmotiBit)
    tsBuf.reserve (4096);
    std::vector<const double *> ptrs;
    ptrs.reserve (8);
    int lastPkg = -1;

    // Heart rate from the PPG channels (display samples, after re-timing).
    SignalChannel *hrChannel = nullptr;
    double ppgRate = 25.0;
    for (SignalChannel &ch : chans)
    {
        if (ch.spec.derived && ch.spec.key == SignalKeys::EmotiHeartRate)
            hrChannel = &ch;
        if (ch.spec.heartRateInput >= 0 && ch.spec.nominalRate > 0.0)
            ppgRate = ch.spec.nominalRate;
    }
    HeartRate::Tracker hrTracker (ppgRate);
    std::vector<HeartRate::Sample> hrOut;
    std::vector<double> ppgTest; // test hook: synthetic PPG values

    const double streamStart = wallClockSeconds ();
    double lastData = streamStart;
    bool stalled = false;

    while (!stop_.load ())
    {
        const unsigned gen = filterGen_.load ();
        if (gen != appliedGen)
        {
            appliedGen = gen;
            for (std::size_t i = 0; i < chans.size (); ++i)
            {
                if (!chans[i].spec.filterable)
                    continue;
                FilterChain fc;
                const double fs = chans[i].spec.nominalRate;
                if (hpOn_.load ())
                    fc.stages.push_back (Biquad::highPass (fs, hpHz_.load ()));
                if (notchOn_.load ())
                    fc.stages.push_back (Biquad::notch (fs, notchHz_.load (), 30.0));
                if (lpOn_.load ())
                    fc.stages.push_back (Biquad::lowPass (fs, lpHz_.load ()));
                filters[i] = fc; // fresh (reset) state
            }
        }
        const unsigned rg = recGen_.load ();
        if (rg != appliedRecGen)
        {
            appliedRecGen = rg;
            applyRecordingRequest (board, cfg);
        }

        // Test hook: stop reading after N s (the session stays open, BrainFlow
        // keeps buffering); the GUI sees a stalled stream.
        const bool frozen =
            cfg.testFreezeAfterSec >= 0.0 && wallClockSeconds () - streamStart > cfg.testFreezeAfterSec;

        bool got = false;
        for (int preset : frozen ? std::vector<int> {} : presets)
        {
            const int count = board.get_board_data_count (preset);
            if (count <= 0)
                continue;
            BrainFlowArray<double, 2> data = board.get_board_data (count, preset);
            const int rows = data.get_size (0);
            const int n = data.get_size (1);
            if (n <= 0)
                continue;
            got = true;

            if (cfg.packageNumRow >= 0 && preset == cfg.packageNumPreset && cfg.packageNumRow < rows)
                dropped_.fetch_add (Readouts::packageGaps (
                    lastPkg, data.get_address (cfg.packageNumRow), static_cast<std::size_t> (n)));

            double hrNow = -std::numeric_limits<double>::infinity (); // newest PPG time fed this poll
            for (std::size_t i = 0; i < chans.size (); ++i)
            {
                SignalChannel &ch = chans[i]; // persistent: the retimer keeps state across polls
                if (ch.spec.preset != preset || ch.spec.derived)
                    continue;
                if (ch.spec.timestampRow < 0 || ch.spec.timestampRow >= rows)
                    continue;
                ptrs.clear ();
                bool rowsOk = true;
                for (int r : ch.spec.rows)
                {
                    if (r < 0 || r >= rows)
                    {
                        rowsOk = false;
                        break;
                    }
                    ptrs.push_back (data.get_address (r)); // row-major: row r is contiguous
                }
                if (!rowsOk || ptrs.empty ())
                    continue;

                // EmotiBit fills each row only when a packet of that type arrives
                // (sample-and-hold, initialised to 0.0), so the first samples of a
                // stream can be exact zeros that would wreck the autoscale. Skip
                // leading all-zero samples, but only during the first 3 s so a
                // genuinely zero channel is still shown. EXG is never skipped.
                int first = 0;
                if (!ch.seenData)
                {
                    if (!ch.spec.filterable)
                    {
                        if (ch.zeroSkipUntil == 0.0)
                            ch.zeroSkipUntil = wallClockSeconds () + 3.0;
                        if (wallClockSeconds () < ch.zeroSkipUntil)
                        {
                            while (first < n)
                            {
                                bool allZero = true;
                                for (const double *p : ptrs)
                                    if (p[first] != 0.0)
                                    {
                                        allZero = false;
                                        break;
                                    }
                                if (!allZero)
                                    break;
                                ++first;
                            }
                        }
                    }
                    if (first >= n)
                        continue;
                    ch.seenData = true;
                }
                for (const double *&p : ptrs)
                    p += first;
                const int m = n - first;
                const double *ts = data.get_address (ch.spec.timestampRow) + first;

                // EmotiBit: every sample of a packet carries the same host
                // time; spread them evenly so the trace is a waveform, not a
                // comb (display only -- recordings keep BrainFlow's stamps).
                if (ch.retime)
                {
                    tsBuf.resize (static_cast<std::size_t> (m));
                    ch.retimer.process (ts, tsBuf.data (), static_cast<std::size_t> (m));
                    ts = tsBuf.data ();
                }

                if (ch.spec.heartRateInput >= 0 && hrChannel)
                {
                    if (cfg.testPpgBpm > 0.0)
                    {
                        // test hook: a distinct synthetic waveform per PPG colour
                        static constexpr double dc[3] = {118000.0, 64000.0, 152000.0};
                        static constexpr double amp[3] = {900.0, 420.0, 640.0};
                        const int c = std::clamp (ch.spec.heartRateInput, 0, 2);
                        ppgTest.resize (static_cast<std::size_t> (m));
                        for (int k = 0; k < m; ++k)
                            ppgTest[static_cast<std::size_t> (k)] =
                                HeartRate::syntheticPpg (ts[k], cfg.testPpgBpm, dc[c], amp[c]) +
                                0.35 * amp[c] * std::sin (2.0 * M_PI * 0.2 * ts[k] + c);
                        ptrs[0] = ppgTest.data ();
                    }
                    hrTracker.process (ch.spec.heartRateInput, ts, ptrs[0], static_cast<std::size_t> (m));
                    hrNow = std::max (hrNow, ts[m - 1]);
                }

                if (ch.rawChannel >= 0)
                {
                    const double *raw = ptrs[0];
                    if (cfg.testRawOffset != 0.0)
                    {
                        rawAdj.resize (static_cast<std::size_t> (m));
                        for (int k = 0; k < m; ++k)
                            rawAdj[static_cast<std::size_t> (k)] = raw[k] + cfg.testRawOffset;
                        raw = rawAdj.data ();
                    }
                    filtered.resize (static_cast<std::size_t> (m));
                    FilterChain &f = filters[i];
                    if (f.empty ())
                        std::copy (raw, raw + m, filtered.begin ());
                    else
                        for (int k = 0; k < m; ++k)
                            filtered[static_cast<std::size_t> (k)] = f.process (raw[k]);
                    ptrs[0] = filtered.data ();
                    ptrs.push_back (raw);
                }
                ch.ring->append (ts, ptrs.data (), static_cast<std::size_t> (m));
            }

            if (hrChannel && std::isfinite (hrNow))
            {
                hrOut.clear ();
                hrTracker.update (hrNow, hrOut);
                for (const HeartRate::Sample &hs : hrOut)
                {
                    const double v[HeartRateRing::Count] = {hs.bpm, hs.quality, static_cast<double> (hs.source),
                        static_cast<double> (hs.beats), hs.periodicity};
                    const double *vp[HeartRateRing::Count] = {&v[0], &v[1], &v[2], &v[3], &v[4]};
                    hrChannel->ring->append (&hs.t, vp, 1);
                }
            }
        }

        const double now = wallClockSeconds ();
        if (got)
        {
            lastData = now;
            if (stalled)
            {
                stalled = false;
                emit dataStall (false);
            }
        }
        else if (!stalled && now - lastData > 3.0)
        {
            stalled = true;
            emit dataStall (true);
        }

        if (waitStop (cfg.pollIntervalMs))
            break;
    }
}

void DeviceWorker::startRecording (
    BoardShim &board, const DeviceConfig &cfg, const std::string &dirIn, const std::string &stamp)
{
    namespace fs = std::filesystem;
    std::string dir = dirIn.empty () ? homeFallbackDir () : dirIn;

    // BrainFlow's file streamer copies the path into a 512-byte buffer and
    // splits "file://<path>:<mode>" at the first "://" and the LAST ':' --
    // spaces and '@' are fine; a ':' inside the path and over-long paths are not
    // (except a Windows drive letter's, which sits before the last ':').
#ifdef _WIN32
    const std::size_t colonFrom = (dir.size () >= 2 && dir[1] == ':') ? 2 : 0;
#else
    const std::size_t colonFrom = 0;
#endif
    if (dir.find (':', colonFrom) != std::string::npos)
    {
        emit message (QString ("recording folder contains ':' (BrainFlow's file streamer cannot "
                               "handle it), using %1")
                          .arg (QString::fromStdString (homeFallbackDir ())));
        dir = homeFallbackDir ();
    }
    const std::size_t longest = dir.size () + cfg.filePrefix.size () + stamp.size () + 44;
    if (longest >= 500)
    {
        emit message (QString ("recording path too long for BrainFlow, using %1")
                          .arg (QString::fromStdString (homeFallbackDir ())));
        dir = homeFallbackDir ();
    }
    std::error_code ec;
    fs::create_directories (dir, ec);
    if (!fs::is_directory (dir, ec))
    {
        const std::string fb = homeFallbackDir ();
        emit message (QString ("cannot create %1, recording to %2")
                          .arg (QString::fromStdString (dir), QString::fromStdString (fb)));
        dir = fb;
        fs::create_directories (dir, ec);
    }

    QStringList files;
    std::vector<int> presets;
    try
    {
        presets = BoardShim::get_board_presets (cfg.boardId);
    }
    catch (const std::exception &)
    {
        presets = {static_cast<int> (BrainFlowPresets::DEFAULT_PRESET)};
    }
    for (int p : presets)
    {
        std::string base = dir + "/" + cfg.filePrefix + "_" + presetName (p) + "_" + stamp;
        // never overwrite an earlier recording started within the same second
        for (int k = 2; fs::exists (base + ".csv", ec) && k < 100; ++k)
            base = dir + "/" + cfg.filePrefix + "_" + presetName (p) + "_" + stamp + "_" + std::to_string (k);
        const std::string path = base + ".csv";
        const std::string streamer = "file://" + path + ":w";
        try
        {
            board.add_streamer (streamer, p);
            streamers_.emplace_back (streamer, p);
            files << QString::fromStdString (path);
            // Column map next to the data (BrainFlow writes rows without a header).
            std::ofstream descr (base + "_columns.json");
            if (descr)
                descr << BoardShim::get_board_descr (cfg.boardId, p).dump (2) << "\n";
        }
        catch (const BrainFlowException &e)
        {
            emit message (QString ("recording of %1 preset failed: %2")
                              .arg (QString::fromStdString (presetName (p)), cleanWhat (e)));
        }
    }
    {
        std::lock_guard<std::mutex> lock (filesMutex_);
        files_ = files;
    }
    if (!files.isEmpty ())
    {
        recording_.store (true);
        emit recordingStarted (files);
    }
}

void DeviceWorker::stopRecording (BoardShim &board)
{
    for (const auto &s : streamers_)
    {
        try
        {
            board.delete_streamer (s.first, s.second); // closes the file
        }
        catch (const BrainFlowException &e)
        {
            emit message (QString ("could not stop recording %1: %2")
                              .arg (QString::fromStdString (s.first), cleanWhat (e)));
        }
    }
    streamers_.clear ();
    if (recording_.exchange (false))
        emit recordingStopped (recordingFiles ());
}
