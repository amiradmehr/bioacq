// bioacq -- native real-time viewer for an OpenBCI Cyton ECG channel + EmotiBit
// (PPG green / red / IR, heart rate, temperature, IMU) via BrainFlow. See README.md.

#include "AppPaths.h"
#include "BuildConfig.h"
#include "Headless.h"
#include "MainWindow.h"
#include "ProcessSetup.h"
#include "Readouts.h"
#include "SerialPorts.h"
#include "Theme.h"

#include "board_shim.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QPixmap>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace
{

void onSignal (int)
{
    g_interruptRequested = 1;
}

struct Args
{
    enum Mode
    {
        Gui,
        Selftest,
        Probe,
        Screenshot,
        ListPorts
    } mode = Gui;
    double seconds = -1.0;
    QString screenshotPath;
    QString state = QStringLiteral ("live");
    int width = 1600, height = 1000;
    bool synthetic = false;
    QString port; // empty = auto-detect the OpenBCI dongle (findCytonDongle)
    QString ip = QStringLiteral ("192.168.1.12");
    int timeout = 5;
    QString recordDir = defaultRecordDir ();
    bool recordDirSet = false;
    bool record = false;
    int window = -1;
    bool bfDiscovery = false;
    bool verbose = false;
    bool noCyton = false;
    bool noEmotibit = false;
    bool help = false;
    // given explicitly on the command line (wins over the GUI's saved settings)
    bool portSet = false;
    bool ipSet = false;
    bool timeoutSet = false;
    // test hooks (command line only)
    double testStallSec = -1.0;
    double testRailOffsetUv = 0.0;
    double testPpgBpm = 0.0;
    QString error;
};

bool isNumber (const char *s)
{
    char *end = nullptr;
    std::strtod (s, &end);
    return end != s && end && *end == '\0';
}

Args parseArgs (int argc, char **argv)
{
    Args a;
    for (int i = 1; i < argc; ++i)
    {
        const std::string s = argv[i];
        auto value = [&] (const char *opt) -> QString {
            if (i + 1 >= argc)
            {
                a.error = QStringLiteral ("missing value for %1").arg (QString::fromLatin1 (opt));
                return QString ();
            }
            return QString::fromLocal8Bit (argv[++i]);
        };
        if (s == "--selftest" || s == "--probe")
        {
            a.mode = s == "--selftest" ? Args::Selftest : Args::Probe;
            if (i + 1 < argc && isNumber (argv[i + 1]))
                a.seconds = std::atof (argv[++i]);
        }
        else if (s == "--list-ports")
            a.mode = Args::ListPorts;
        else if (s == "--screenshot")
        {
            a.mode = Args::Screenshot;
            a.screenshotPath = value ("--screenshot");
        }
        else if (s == "--state")
        {
            a.state = value ("--state").toLower ();
            static const QStringList states = {QStringLiteral ("live"), QStringLiteral ("idle"),
                QStringLiteral ("connecting"), QStringLiteral ("error"), QStringLiteral ("recording"),
                QStringLiteral ("warning")};
            if (!states.contains (a.state))
                a.error = QStringLiteral ("unknown --state '%1' (live|idle|connecting|error|recording|warning)").arg (a.state);
        }
        else if (s == "--size")
        {
            const QStringList wh = value ("--size").toLower ().split (QLatin1Char ('x'));
            bool ok1 = false, ok2 = false;
            if (wh.size () == 2)
            {
                a.width = wh[0].toInt (&ok1);
                a.height = wh[1].toInt (&ok2);
            }
            if (!ok1 || !ok2 || a.width < 640 || a.height < 400)
                a.error = QStringLiteral ("--size expects WxH, e.g. 1280x800");
        }
        else if (s == "--synthetic")
            a.synthetic = true;
        else if (s == "--port")
        {
            a.port = value ("--port");
            a.portSet = true;
        }
        else if (s == "--ip")
        {
            a.ip = value ("--ip").trimmed ();
            a.ipSet = true;
        }
        else if (s == "--discover")
        {
            a.ip.clear (); // blank IP = broadcast discovery
            a.ipSet = true;
        }
        else if (s == "--bf-discovery")
            a.bfDiscovery = true;
        else if (s == "--timeout")
        {
            a.timeout = value ("--timeout").toInt ();
            a.timeoutSet = true;
        }
        else if (s == "--record")
            a.record = true;
        else if (s == "--record-dir")
        {
            a.recordDir = value ("--record-dir");
            a.recordDirSet = true;
        }
        else if (s == "--window")
            a.window = value ("--window").toInt ();
        else if (s == "--test-stall-emotibit")
        {
            a.testStallSec = 1.5;
            if (i + 1 < argc && isNumber (argv[i + 1]))
                a.testStallSec = std::max (0.0, std::atof (argv[++i]));
        }
        else if (s == "--test-rail-offset-uv")
            a.testRailOffsetUv = value ("--test-rail-offset-uv").toDouble ();
        else if (s == "--test-ppg-bpm")
            a.testPpgBpm = std::clamp (value ("--test-ppg-bpm").toDouble (), 0.0, 220.0);
        else if (s == "--no-cyton")
            a.noCyton = true;
        else if (s == "--no-emotibit")
            a.noEmotibit = true;
        else if (s == "--verbose" || s == "-v")
            a.verbose = true;
        else if (s == "--help" || s == "-h")
            a.help = true;
        else if (s == "-platform" || s == "-style" || s == "-qwindowgeometry")
            ++i; // Qt's own options, handled by QApplication
        else if (s.rfind ("-psn_", 0) == 0)
            ; // macOS Finder process serial number
        else
            std::fprintf (stderr, "warning: ignoring unknown argument '%s'\n", s.c_str ());
    }
    a.timeout = std::clamp (a.timeout, 2, kMaxDiscoveryTimeoutSec);
    if (a.window > 0)
        a.window = std::clamp (a.window, 1, 60);
    return a;
}

void usage ()
{
    std::printf (
        "bioacq %s -- real-time OpenBCI Cyton + EmotiBit viewer (BrainFlow, Qt6)\n\n"
        "usage: bioacq [options]      (or scripts/run.sh [options] from the repo)\n"
        "  (no mode)                 interactive GUI (remembers port, IP and display settings)\n"
        "  --synthetic               GUI with both devices simulated (BrainFlow synthetic board)\n"
        "  --selftest [seconds]      headless end-to-end test on the synthetic board (default 6)\n"
        "  --probe [seconds]         headless test of the REAL devices (default 8 s of data)\n"
        "  --screenshot <file.png>   render one UI state through the real code paths, save a PNG, quit\n"
        "      --state <s>           live (default) | idle | connecting | error | recording | warning\n"
        "                            every state presses Connect (both devices at once);\n"
        "                            live / recording: synthetic, with --test-ppg-bpm 72 (heart rate);\n"
        "                            connecting: Cyton held before prepare_session + EmotiBit discovery\n"
        "                            toward the unanswered TEST-NET address 192.0.2.1 (timeout 20 s);\n"
        "                            error: Cyton synthetic, EmotiBit not found at 192.0.2.1 (2 s);\n"
        "                            recording: records into --record-dir (default: a temporary folder);\n"
        "                            warning: stall + rail-offset test hooks below\n"
        "      --size WxH            window size (default 1600x1000)\n"
        "  --list-ports              list serial ports (OpenBCI dongles first) and exit\n\n"
        "options:\n"
        "  --port <dev>              Cyton serial port, e.g. COM3 or /dev/cu.usbserial-XXXXXXXX\n"
        "                            (default: auto-detect the OpenBCI dongle, FTDI 0403:6015, at connect)\n"
        "  --ip <addr>               EmotiBit IP (default 192.168.1.12; works across subnets)\n"
        "  --discover                blank EmotiBit IP: the GUI tries the last EmotiBit that answered,\n"
        "                            then broadcast; --probe broadcasts (same subnet only)\n"
        "  --timeout <s>             EmotiBit discovery timeout, 2-%d (default 5)\n"
        "  --bf-discovery            let BrainFlow do the EmotiBit discovery (old behaviour; holds\n"
        "                            BrainFlow's global lock, pausing the Cyton meanwhile)\n"
        "  --record                  arm recording: each device records from its first sample\n"
        "  --record-dir <dir>        recording folder (default %s)\n"
        "  --window <s>              plot window length, 1-60 (default 10)\n"
        "  --no-cyton / --no-emotibit   probe only one device\n"
        "  --verbose                 BrainFlow log level INFO (stderr)\n\n"
        "test hooks (display only, never in the UI):\n"
        "  --test-stall-emotibit [s] stop polling the EmotiBit after s seconds of streaming (default 1.5)\n"
        "  --test-rail-offset-uv <v> add v uV to the Cyton's raw Ch1 display value (near-rail path)\n"
        "  --test-ppg-bpm <bpm>      replace the EmotiBit PPG display values with a synthetic pulse\n"
        "                            (drives the heart-rate panel)\n",
        BIOACQ_VERSION, kMaxDiscoveryTimeoutSec, qPrintable (QDir::toNativeSeparators (defaultRecordDir ())));
}

double steadyNow ()
{
    return std::chrono::duration<double> (std::chrono::steady_clock::now ().time_since_epoch ()).count ();
}

} // namespace

int main (int argc, char **argv)
{
    // Windows GUI-subsystem build: print to the calling console (if any) when
    // started with arguments; a double-click passes none.
    if (argc > 1)
        attachParentConsole ();
    raiseTimerResolution ();

    const Args a = parseArgs (argc, argv);
    if (a.help)
    {
        usage ();
        return 0;
    }
    if (!a.error.isEmpty ())
    {
        std::fprintf (stderr, "error: %s\n\n", qPrintable (a.error));
        usage ();
        return 2;
    }

    std::signal (SIGINT, onSignal);
    std::signal (SIGTERM, onSignal);

    try
    {
        BoardShim::set_log_level (static_cast<int> (a.verbose ? LogLevels::LEVEL_INFO : LogLevels::LEVEL_ERROR));
    }
    catch (const std::exception &)
    {
    }

    if (a.mode == Args::ListPorts)
    {
        QCoreApplication app (argc, argv);
        const QVector<SerialPortEntry> ports = listSerialPorts ();
        if (ports.isEmpty ())
            std::printf ("no serial ports found\n");
        for (const SerialPortEntry &e : ports)
            std::printf ("%-36s %04x:%04x  %s%s\n", qPrintable (brainflowSerialPort (e)), e.vid, e.pid,
                qPrintable (e.description), e.cytonDongle ? "  [OpenBCI dongle]" : "");
        std::fflush (stdout);
        return 0;
    }
    if (a.mode == Args::Selftest)
    {
        QCoreApplication app (argc, argv);
        return runSelftest (a.seconds > 0.0 ? a.seconds : 6.0);
    }
    if (a.mode == Args::Probe)
    {
        QCoreApplication app (argc, argv);
        ProbeOptions o;
        o.cytonPort = a.port;
        o.emotibitIp = a.ip;
        o.emotibitTimeoutSec = a.timeout;
        o.bfDiscovery = a.bfDiscovery;
        o.seconds = a.seconds > 0.0 ? a.seconds : 8.0;
        o.record = a.record;
        o.recordDir = a.recordDir;
        o.skipCyton = a.noCyton;
        o.skipEmotibit = a.noEmotibit;
        return runProbe (o);
    }

    QApplication app (argc, argv);
    QApplication::setOrganizationName (QStringLiteral ("bioacq")); // QSettings location
    QApplication::setApplicationName (QStringLiteral ("bioacq"));
    Theme::apply (app);

    const bool shot = a.mode == Args::Screenshot;
    LaunchOptions lo;
    lo.synthetic = a.synthetic || shot;
    lo.cytonPort = a.port; // empty: MainWindow auto-detects the dongle at every connect
    lo.emotibitIp = a.ip;
    lo.emotibitTimeoutSec = a.timeout;
    lo.recordDir = a.recordDir;
    lo.record = a.record;
    lo.windowSec = a.window > 0 ? a.window : (shot ? 8 : 10);
    lo.brainflowDiscovery = a.bfDiscovery;
    // Saved settings only for the interactive GUI: --screenshot stays
    // reproducible and never overwrites what the user last used.
    lo.useSettings = a.mode == Args::Gui;
    lo.interactive = a.mode == Args::Gui;
    lo.portSet = a.portSet;
    lo.ipSet = a.ipSet;
    lo.timeoutSet = a.timeoutSet;
    lo.windowSet = a.window > 0;
    lo.recordSet = a.record;
    lo.testFreezeEmotibitSec = a.testStallSec;
    lo.testRailOffsetUv = a.testRailOffsetUv;
    lo.testPpgBpm = a.testPpgBpm;

    // --screenshot states: set up the real code paths that produce each state.
    std::unique_ptr<QTemporaryDir> tmpRecord;
    if (shot)
    {
        if (a.state == QLatin1String ("connecting") || a.state == QLatin1String ("error"))
        {
            lo.emotibitIp = QStringLiteral ("192.0.2.1"); // TEST-NET-1: never answers
            lo.emotibitTimeoutSec = a.state == QLatin1String ("connecting") ? 20 : 2;
        }
        if (a.state == QLatin1String ("connecting"))
            lo.testCytonPrepareDelayMs = 8000; // both devices still connecting at the capture
        if ((a.state == QLatin1String ("live") || a.state == QLatin1String ("recording")) && lo.testPpgBpm <= 0.0)
            lo.testPpgBpm = 72.0; // the synthetic board's PPG is noise: no heart rate without it
        if (a.state == QLatin1String ("warning"))
        {
            if (lo.testFreezeEmotibitSec < 0.0)
                lo.testFreezeEmotibitSec = 1.5;
            if (lo.testRailOffsetUv == 0.0)
                lo.testRailOffsetUv = 181000.0;
        }
        if (a.state == QLatin1String ("recording") && !a.recordDirSet)
        {
            tmpRecord = std::make_unique<QTemporaryDir> ();
            lo.recordDir = tmpRecord->path ();
        }
    }
    if (lo.testFreezeEmotibitSec >= 0.0 || lo.testRailOffsetUv != 0.0 || lo.testPpgBpm > 0.0)
    {
        QStringList hooks;
        if (lo.testFreezeEmotibitSec >= 0.0)
            hooks << QStringLiteral ("EmotiBit polling freezes");
        if (lo.testRailOffsetUv != 0.0)
            hooks << QStringLiteral ("Cyton raw offset");
        if (lo.testPpgBpm > 0.0)
            hooks << QStringLiteral ("synthetic PPG pulse at %1 bpm").arg (lo.testPpgBpm);
        std::fprintf (stderr, "bioacq: TEST HOOK ACTIVE (%s) -- display data is modified\n",
            qPrintable (hooks.join (QStringLiteral (", "))));
    }

    MainWindow w (lo);
    w.resize (a.width, a.height);

    // Ctrl+C / SIGTERM -> the same clean close path as the window's close button.
    QTimer sigTimer;
    QObject::connect (&sigTimer, &QTimer::timeout, &w, [&w] {
        if (g_interruptRequested)
        {
            g_interruptRequested = 0;
            w.close ();
        }
    });
    sigTimer.start (200);

    if (shot)
    {
        bool saved = false;
        const QString state = a.state;
        const bool idle = state == QLatin1String ("idle");
        const bool emReal = state == QLatin1String ("connecting") || state == QLatin1String ("error");
        w.show ();
        // CPU over t = 2..4 s after the connect (getrusage, all threads; % of one core)
        double cpuA = 0.0, wallA = 0.0;
        QTimer::singleShot (150, &w, [&w, idle, emReal] {
            if (!idle)
                w.connectAllWith (true, !emReal); // the Connect button, both devices at once
        });
        if (state == QLatin1String ("recording"))
            QTimer::singleShot (1500, &w, [&w] { w.setRecording (true); });
        QTimer::singleShot (2150, &w, [&] {
            cpuA = Readouts::processCpuSeconds ();
            wallA = steadyNow ();
        });
        QTimer::singleShot (4150, &w, [&] {
            const double pct = 100.0 * (Readouts::processCpuSeconds () - cpuA) / std::max (1e-6, steadyNow () - wallA);
            std::printf ("cpu t=2.0-4.0 s: %.1f %% of one core (state %s, %dx%d, dpr %.1f)\n", pct, qPrintable (state),
                a.width, a.height, w.devicePixelRatioF ());
            std::fflush (stdout);
        });
        // late enough for a first heart-rate estimate (4 beats at 72 bpm) and
        // for the 2 s error timeout; the connecting state is still connecting
        QTimer::singleShot (idle ? 1500 : 6300, &w, [&] {
            const QPixmap pm = w.grab ();
            saved = !pm.isNull () && pm.save (a.screenshotPath);
            std::printf ("screenshot %s: %s (%dx%d, state %s)\n", saved ? "saved" : "FAILED",
                qPrintable (a.screenshotPath), pm.width (), pm.height (), qPrintable (state));
            std::fflush (stdout);
            w.close ();
        });
        QTimer::singleShot (40000, [] {
            std::fprintf (stderr, "screenshot mode: timed out\n");
            std::_Exit (3);
        });
        const int rc = app.exec ();
        return (rc == 0 && saved) ? 0 : 1;
    }

    w.show ();
    return app.exec ();
}
