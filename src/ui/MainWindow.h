#pragma once

#include "RateMeter.h"
#include "Readouts.h"
#include "SignalSpec.h"

#include <QMainWindow>
#include <QString>
#include <QStringList>

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

class QCloseEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QWidget;
class Banner;
class DeviceWorker;
class MeterBar;
class PlotWidget;
class PortCombo;
class RecordButton;
class RescanButton;
class SessionBar;
class StatusChip;
class StatusStrip;
class Stepper;
class ToggleSwitch;

// Upper bound for the EmotiBit discovery timeout (UI field and --timeout).
inline constexpr int kMaxDiscoveryTimeoutSec = 20;

struct LaunchOptions
{
    bool synthetic = false;
    QString cytonPort; // empty = auto-detect (main.cpp preselects findCytonDongle ())
    QString emotibitIp = QStringLiteral ("192.168.1.12"); // blank = broadcast discovery
    int emotibitTimeoutSec = 5;
    QString recordDir;
    bool record = false; // arm recording: every device records from its first sample
    int windowSec = 10;
    bool removeDc = true;
    bool highPass = false;
    bool notch = false;
    bool brainflowDiscovery = false; // --bf-discovery: let BrainFlow search (holds its lock)

    // QSettings: restore the last used values at start, save them on a
    // successful connect and on close (interactive GUI only). Values given on
    // the command line (the *Set flags) win over stored ones.
    bool useSettings = false;
    bool interactive = true;
    bool portSet = false;
    bool ipSet = false;
    bool timeoutSet = false;
    bool windowSet = false;
    bool recordSet = false;

    // Test hooks (command line only; no UI): freeze EmotiBit polling after N s
    // of streaming, add a DC offset to the Cyton's raw display value.
    double testFreezeEmotibitSec = -1.0;
    double testRailOffsetUv = 0.0;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow (const LaunchOptions &opts, QWidget *parent = nullptr);
    ~MainWindow () override;

    // Connect with the rail's settings (the "Simulate devices" toggle picks
    // the synthetic board) / with an explicit board choice.
    void connectDevice (DeviceKind kind);
    void connectDeviceWith (DeviceKind kind, bool synthetic);
    void disconnectDevice (DeviceKind kind);
    // Same as pressing the record button: start / stop recording on every
    // streaming device (and arm it for devices that connect later).
    void setRecording (bool on);
    bool isRecording () const;
    bool isStreaming (DeviceKind kind) const;
    bool anyActive () const;

protected:
    void closeEvent (QCloseEvent *event) override;

private:
    struct Slot
    {
        DeviceKind kind = DeviceKind::Cyton;
        DeviceWorker *worker = nullptr;
        // rail widgets
        StatusChip *chip = nullptr;
        QPushButton *button = nullptr;
        QLabel *meta = nullptr;
        QWidget *errorWrap = nullptr;
        QLabel *errorTitle = nullptr;
        QLabel *errorText = nullptr;
        QLabel *errorHint = nullptr;

        std::vector<RateMeter> meters;   // parallel to worker->channels()
        std::vector<PlotWidget *> plots; // parallel to worker->channels()
        bool stopRequested = false;
        bool synthetic = false;     // current/last session runs on the synthetic board
        bool autoDiscovery = false; // EmotiBit: blank IP, broadcast discovery pending
        QString address;            // serial port / EmotiBit IP of the session
        QString typedIp;            // EmotiBit: the IP field at connect ("" = broadcast)
        QString serial;             // EmotiBit serial from discovery
        QString targets;            // EmotiBit: discovery targets (display)
        QString progressText;       // latest connecting-phase text
        double connectStarted = 0.0; // DeviceWorker::steadyNow() at connect
        double linkSeconds = std::numeric_limits<double>::quiet_NaN ();
        double streamingSinceWall = std::numeric_limits<double>::quiet_NaN (); // wall clock at Streaming

        // failure of the last attempt (cleared on the next connect)
        bool failed = false;
        int failKind = 0; // DeviceWorker::FailureKind
        QString failText;
        double failAfter = 0.0;
        double failTimeout = 0.0;

        // stream health (updated at 4 Hz)
        bool stalled = false, held = false;
        double stallSince = std::numeric_limits<double>::quiet_NaN (); // wall clock
        double stallAge = 0.0;
        int staleCount = 0;  // channels without a new sample past their threshold
        int neverCount = 0;  // ... of which never delivered one since streaming began
        int dataCount = 0;   // channels of the session
        bool pending = false; // streaming, some channel still inside its first-sample grace
        double rateSum = 0.0;
        QString lowRate;     // "PPG green 18.1 / 25 Hz" when below tolerance
        QString partialText; // first stale channel when only some are stale
    };

    Slot &slot (DeviceKind kind)
    {
        return kind == DeviceKind::Cyton ? cyton_ : emotibit_;
    }
    const Slot &slot (DeviceKind kind) const
    {
        return kind == DeviceKind::Cyton ? cyton_ : emotibit_;
    }
    const Slot &otherSlot (DeviceKind kind) const
    {
        return kind == DeviceKind::Cyton ? emotibit_ : cyton_;
    }

    QWidget *buildRail ();
    QWidget *buildCytonModule ();
    QWidget *buildEmotibitModule ();
    QWidget *buildDisplayModule ();
    QWidget *buildRecordModule ();
    QWidget *buildErrorBox (Slot &s);
    QWidget *buildMain ();
    QWidget *buildIdleOverlay ();

    void wireWorker (DeviceKind kind);
    void onButton (DeviceKind kind);
    void updateSlotUi (Slot &s);
    void updateDiscoverBox (Slot &s);
    void updateErrorBox (Slot &s);
    void updateStreamHealth (Slot &s, double now);
    void updateHeadroom (double now);
    double cytonRawPeak (double now); // max |raw Ch1| over the window, from the ring
    void updateRecordUi ();
    void pollRecordingSizes ();
    void refreshChrome (double now);
    QString metaText (const Slot &s) const;
    void refreshPorts (bool announce = true); // announce: "Found N USB serial port(s)"
    void applyFilterSettings ();
    void updateWindowTitle ();
    void onFrame ();
    void onRateTimer ();
    void onSlowTimer ();
    void showMessage (const QString &text, int ms = 8000, const QColor &color = QColor ());
    void loadSettings ();
    void saveSettings ();
    void saveDeviceAddress (DeviceKind kind, const QString &address);
    PlotWidget *plotForKey (const std::string &key) const;
    std::array<PlotWidget *, 6> allPlots () const;
    const QString &subnets ();

    LaunchOptions opts_;
    Slot cyton_;
    Slot emotibit_;
    bool closing_ = false;

    // rail: Cyton
    PortCombo *portCombo_ = nullptr;
    RescanButton *refreshBtn_ = nullptr;
    ToggleSwitch *dcToggle_ = nullptr;
    ToggleSwitch *hpToggle_ = nullptr;
    ToggleSwitch *notchToggle_ = nullptr;
    QLabel *railPct_ = nullptr;
    MeterBar *railBar_ = nullptr;
    QLabel *railNote_ = nullptr;
    // rail: EmotiBit
    QLineEdit *ipEdit_ = nullptr;
    QSpinBox *timeoutSpin_ = nullptr;
    QLabel *ipHint_ = nullptr;
    QWidget *discoverWrap_ = nullptr;
    QLabel *discoverTitle_ = nullptr;
    QLabel *discoverTime_ = nullptr;
    MeterBar *discoverBar_ = nullptr;
    QLabel *discoverDetail_ = nullptr;
    QPushButton *cancelDiscoveryBtn_ = nullptr;
    // rail: display
    Stepper *windowStepper_ = nullptr;
    ToggleSwitch *pauseToggle_ = nullptr;
    ToggleSwitch *synthToggle_ = nullptr;
    // rail: record
    RecordButton *recordBtn_ = nullptr;
    QLabel *recFolder_ = nullptr;
    QLabel *recElapsed_ = nullptr;
    QLabel *recSize_ = nullptr;

    // chrome
    SessionBar *sessionBar_ = nullptr;
    StatusStrip *statusStrip_ = nullptr;
    Banner *banner_ = nullptr;
    QWidget *idleOverlay_ = nullptr;

    // plots
    PlotWidget *cytonPlot_ = nullptr;
    PlotWidget *tempPlot_ = nullptr;
    PlotWidget *ppgPlot_ = nullptr;
    PlotWidget *accelPlot_ = nullptr;
    PlotWidget *gyroPlot_ = nullptr;
    PlotWidget *magPlot_ = nullptr;

    QTimer *frameTimer_ = nullptr;
    QTimer *rateTimer_ = nullptr;
    QTimer *slowTimer_ = nullptr;

    // recording
    bool recordWanted_ = false;
    double recStartWall_ = std::numeric_limits<double>::quiet_NaN ();
    QStringList recFiles_;
    double recBytes_ = 0.0;

    // readouts
    double rawPeak_ = std::numeric_limits<double>::quiet_NaN ();
    std::vector<double> peakT_; // reused ring-copy storage for cytonRawPeak()
    std::vector<std::vector<double>> peakV_;
    double headroom_ = std::numeric_limits<double>::quiet_NaN ();
    bool nearRail_ = false;
    double railSinceWall_ = std::numeric_limits<double>::quiet_NaN ();
    double warnSinceWall_ = std::numeric_limits<double>::quiet_NaN ();
    double sessionStartWall_ = std::numeric_limits<double>::quiet_NaN ();
    Readouts::CpuMeter cpu_;
    QString subnets_;
    double subnetsAt_ = -1e9;

    // transient status message
    QString msg_;
    QColor msgColor_;
    double msgUntil_ = 0.0;
};
