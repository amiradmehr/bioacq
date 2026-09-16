#pragma once

#include "RateMeter.h"
#include "Readouts.h"
#include "SignalSpec.h"

#include <QMainWindow>
#include <QPointer>
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
class QMessageBox;
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
    // Cyton: the port auto-detect prefers (the last port that connected);
    // an explicit --port (portSet) becomes the rail's port override instead.
    QString cytonPort = QStringLiteral ("/dev/cu.usbserial-DP04W4GA");
    QString emotibitIp = QStringLiteral ("192.168.1.12"); // blank = last IP that answered, then broadcast
    int emotibitTimeoutSec = 5;
    QString recordDir;
    bool record = false; // arm recording: every device records from its first sample
    int windowSec = 10;
    // ECG display filters (all on by default)
    bool removeDc = true;
    bool highPass = true; // 0.5 Hz
    bool notch = true;    // 60 Hz
    bool lowPass = true;  // 40 Hz
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
    // of streaming, add a DC offset to the Cyton's raw display value, replace
    // the EmotiBit PPG display values with a synthetic pulse at N bpm.
    double testFreezeEmotibitSec = -1.0;
    double testRailOffsetUv = 0.0;
    double testPpgBpm = 0.0;
    // --screenshot "connecting" only: hold the Cyton before prepare_session.
    int testCytonPrepareDelayMs = 0;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow (const LaunchOptions &opts, QWidget *parent = nullptr);
    ~MainWindow () override;

    // The Connect button: starts both devices at once with the rail's
    // settings (the "simulate devices" switch picks the synthetic board).
    void connectAll ();
    // Same, with an explicit board choice per device (--screenshot states).
    void connectAllWith (bool cytonSynthetic, bool emotibitSynthetic);
    // Cancel the attempts still in progress (streaming devices keep streaming).
    void cancelPending ();
    void disconnectAll ();
    // Same as pressing the record button: start / stop recording on every
    // streaming device (and arm it for devices that connect later).
    void setRecording (bool on);
    bool isRecording () const;
    bool isStreaming (DeviceKind kind) const;
    bool anyActive () const;
    bool anyConnecting () const;
    bool anyStreaming () const;

protected:
    void closeEvent (QCloseEvent *event) override;

private:
    struct Slot
    {
        DeviceKind kind = DeviceKind::Cyton;
        DeviceWorker *worker = nullptr;
        // rail widgets
        StatusChip *chip = nullptr;
        QLabel *meta = nullptr; // one-line detail: port / IP + device id / failure reason
        QWidget *errorWrap = nullptr;
        QLabel *errorTitle = nullptr;
        QLabel *errorText = nullptr;
        QLabel *errorHint = nullptr;

        std::vector<RateMeter> meters;   // parallel to worker->channels()
        std::vector<PlotWidget *> plots; // parallel to worker->channels()
        std::vector<int> lanes;          // the plot lane of each channel
        bool stopRequested = false;
        bool synthetic = false;      // current/last session runs on the synthetic board
        bool fieldBlank = false;     // EmotiBit: the IP field was blank at connect
        QString address;             // serial port / EmotiBit IP of the session
        QString typedIp;             // EmotiBit: unicast target at connect ("" = broadcast)
        QString serial;              // EmotiBit serial from discovery
        QString targets;             // EmotiBit: discovery targets (display)
        QString progressText;        // latest connecting-phase text
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
        int dataCount = 0;   // BrainFlow channels of the session (derived ones excluded)
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
    QWidget *buildConnectModule ();
    QWidget *buildCytonModule ();
    QWidget *buildEmotibitModule ();
    QWidget *buildDisplayModule ();
    QWidget *buildRecordModule ();
    QWidget *buildErrorBox (Slot &s);
    QWidget *buildMain ();
    QWidget *buildIdleOverlay ();

    void wireWorker (DeviceKind kind);
    void onConnectButton ();
    void connectDeviceWith (DeviceKind kind, bool synthetic);
    void disconnectDevice (DeviceKind kind);
    void updateConnectUi ();
    void updateSlotUi (Slot &s);
    void updateDiscoverBox (Slot &s);
    void updateErrorBox (Slot &s);
    bool showsErrorBox (const Slot &s) const;
    void showErrorDialog (const Slot &s);
    void updateStreamHealth (Slot &s, double now);
    void updateHeartRate (double now);
    void updateHeadroom (double now);
    double cytonRawPeak (double now); // max |raw ECG| over the last Readouts::kRailWindowSec
    void applyEcgChips ();
    void updateRecordUi ();
    void pollRecordingSizes ();
    void refreshChrome (double now);
    QString metaText (const Slot &s) const;
    void refreshPorts (bool announce = true); // announce: "Found N USB serial port(s)"
    QString portOverride () const;            // the rail's port choice, "" = auto-detect
    QString cytonPortFor (const QString &override) const;
    void applyFilterSettings ();
    void updateWindowTitle ();
    void onFrame ();
    void onRateTimer ();
    void onSlowTimer ();
    void showMessage (const QString &text, int ms = 8000, const QColor &color = QColor ());
    void loadSettings ();
    void saveSettings ();
    void saveDeviceAddress (DeviceKind kind, const QString &address);
    PlotWidget *plotForKey (const std::string &key, int *lane = nullptr) const;
    std::array<PlotWidget *, 7> allPlots () const;
    const QString &subnets ();

    LaunchOptions opts_;
    Slot cyton_;
    Slot emotibit_;
    bool closing_ = false;

    // rail: connect
    QPushButton *connectBtn_ = nullptr;
    // rail: Cyton
    PortCombo *portCombo_ = nullptr;
    RescanButton *refreshBtn_ = nullptr;
    QString portOverride_;  // restored / --port; "" = auto-detect
    QString detectedPort_;  // auto-detect result, refreshed at 1 Hz while idle
    ToggleSwitch *dcToggle_ = nullptr;
    ToggleSwitch *hpToggle_ = nullptr;
    ToggleSwitch *notchToggle_ = nullptr;
    ToggleSwitch *lpToggle_ = nullptr;
    QLabel *railPct_ = nullptr;
    MeterBar *railBar_ = nullptr;
    QLabel *railNote_ = nullptr;
    // rail: EmotiBit
    QLineEdit *ipEdit_ = nullptr;
    QSpinBox *timeoutSpin_ = nullptr;
    QString lastEmotibitIp_; // the last EmotiBit that answered (blank field: tried first)
    QWidget *discoverWrap_ = nullptr;
    QLabel *discoverTitle_ = nullptr;
    QLabel *discoverTime_ = nullptr;
    MeterBar *discoverBar_ = nullptr;
    QLabel *discoverDetail_ = nullptr;
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
    QPointer<QMessageBox> errorDialog_;

    // plots
    PlotWidget *cytonPlot_ = nullptr;
    PlotWidget *ppgGreenPlot_ = nullptr;
    PlotWidget *ppgRedPlot_ = nullptr;
    PlotWidget *ppgIrPlot_ = nullptr;
    PlotWidget *hrPlot_ = nullptr;
    PlotWidget *imuPlot_ = nullptr;
    PlotWidget *tempPlot_ = nullptr;

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
