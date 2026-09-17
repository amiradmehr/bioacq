#include "MainWindow.h"

#include "AppPaths.h"
#include "BluetoothAccess.h"
#include "BuildConfig.h"
#include "DeviceWorker.h"
#include "EmotiBitDiscovery.h"
#include "EmotiBitWifiDialog.h"
#include "HeartRate.h"
#include "PlotWidget.h"
#include "RingBuffer.h"
#include "SerialPorts.h"
#include "Theme.h"
#include "Widgets.h"

#include "board_shim.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPainter>
#include <QSettings>
#include <QShortcut>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStyle>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{

// QSettings keys
const char *kKeyPort = "cyton/port";                 // last port that connected (auto-detect prefers it)
const char *kKeyPortOverride = "cyton/portOverride"; // the rail's port choice, "" = auto-detect
const char *kKeyIp = "emotibit/ip";                  // the IP field ("" = last IP, then broadcast)
const char *kKeyLastIp = "emotibit/lastIp";          // the last EmotiBit that answered
const char *kKeyLastBle = "emotibit/lastBluetoothName"; // the last EmotiBit that streamed over Bluetooth
const char *kKeyTimeout = "emotibit/timeout";
const char *kKeyWindow = "display/windowSec";
// ECG display filters. New keys (the old cyton/* ones are ignored) because the
// defaults changed to all-on and the high-pass corner moved from 1 to 0.5 Hz.
const char *kKeyDc = "ecg/removeDc";
const char *kKeyHp = "ecg/highPass0p5Hz";
const char *kKeyNotch = "ecg/notch60Hz";
const char *kKeyLp = "ecg/lowPass40Hz";
const char *kKeyRecord = "record/enabled";

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN ();
constexpr double kEcgHighPassHz = 0.5;
constexpr double kEcgNotchHz = 60.0;
constexpr double kEcgLowPassHz = 40.0;

const QKeySequence kConnectKey (Qt::CTRL | Qt::Key_K);
const QKeySequence kRescanKey (Qt::CTRL | Qt::Key_R);

QString keyText (const QKeySequence &k)
{
    return k.toString (QKeySequence::NativeText); // "⌘K" on macOS, "Ctrl+K" elsewhere
}

QString shortDeviceName (DeviceKind kind)
{
    return kind == DeviceKind::Cyton ? QStringLiteral ("Cyton") : QStringLiteral ("EmotiBit");
}

QString upperName (DeviceKind kind)
{
    return kind == DeviceKind::Cyton ? QStringLiteral ("CYTON") : QStringLiteral ("EMOTIBIT");
}

QLabel *makeLabel (const QString &text, const QFont &f, const QColor &c)
{
    auto *l = new QLabel (text);
    l->setFont (f);
    Theme::setTextColor (l, c);
    return l;
}

// "// SERIAL PORT" style kicker (mono, tracked, dim)
QLabel *kicker (const QString &t, double px = 9.0, double track = 0.14)
{
    return makeLabel (t, Theme::mono (px, 400, track), Theme::textDim);
}

void repolish (QWidget *w)
{
    w->style ()->unpolish (w);
    w->style ()->polish (w);
    w->update ();
}

void setVariant (QPushButton *b, const char *v)
{
    if (b->property ("variant").toString () == QLatin1String (v))
        return;
    b->setProperty ("variant", v);
    repolish (b);
}

void setFlag (QWidget *w, const char *prop, bool on)
{
    if (w->property (prop).toBool () == on)
        return;
    w->setProperty (prop, on);
    repolish (w);
}

QPushButton *makeButton (const QString &text, const char *variant, const char *size, int height)
{
    auto *b = new QPushButton (text);
    b->setProperty ("variant", variant);
    if (size)
        b->setProperty ("size", size);
    b->setFixedHeight (height);
    b->setCursor (Qt::PointingHandCursor);
    b->setFocusPolicy (Qt::TabFocus);
    return b;
}

// Single-line label that elides instead of growing (folder paths: middle;
// detail lines: right).
class ElideLabel : public QLabel
{
public:
    explicit ElideLabel (const QString &text = QString (), Qt::TextElideMode mode = Qt::ElideMiddle)
        : QLabel (text), mode_ (mode)
    {
    }
    QSize minimumSizeHint () const override
    {
        return QSize (12, QLabel::minimumSizeHint ().height ());
    }
    QSize sizeHint () const override
    {
        return QSize (60, QLabel::sizeHint ().height ());
    }

protected:
    void paintEvent (QPaintEvent *) override
    {
        QPainter p (this);
        p.setFont (font ());
        p.setPen (palette ().color (QPalette::WindowText));
        p.drawText (rect (), static_cast<int> (alignment ()) | Qt::AlignVCenter,
            fontMetrics ().elidedText (text (), mode_, width ()));
    }

private:
    Qt::TextElideMode mode_;
};

// 12 px warning triangle from the design's error module.
class WarnGlyph : public QWidget
{
public:
    explicit WarnGlyph (QWidget *parent = nullptr) : QWidget (parent)
    {
        setFixedSize (12, 12);
    }

protected:
    void paintEvent (QPaintEvent *) override
    {
        QPainter p (this);
        p.setRenderHint (QPainter::Antialiasing);
        p.setPen (QPen (Theme::fault, 1.2));
        p.setBrush (Qt::NoBrush);
        const QPointF tri[3] = {QPointF (6, 0.6), QPointF (11.4, 10.6), QPointF (0.6, 10.6)};
        p.drawPolygon (tri, 3);
        p.fillRect (QRectF (5.4, 4.0, 1.2, 3.4), Theme::fault);
        p.fillRect (QRectF (5.4, 8.2, 1.2, 1.2), Theme::fault);
    }
};

QString homeAbbrev (const QString &path)
{
    const QString home = QDir::homePath ();
    return path.startsWith (home) ? QStringLiteral ("~") + path.mid (home.size ()) : path;
}

// The Bluetooth part of an EmotiBit failure text ("Bluetooth: ..." or
// "Bluetooth not used: ..." from DeviceWorker::discoverEmotibit); "" if none.
QString bluetoothReason (const QString &failText)
{
    for (const QString &line : failText.split (QLatin1Char ('\n')))
    {
        if (line.startsWith (QLatin1String ("Bluetooth: ")))
            return line.mid (11).trimmed ();
        if (line.startsWith (QLatin1String ("Bluetooth not used: ")))
            return line.mid (20).trimmed ();
    }
    return QString ();
}

bool validIpv4 (const QString &s)
{
    QHostAddress a;
    return s.count (QLatin1Char ('.')) == 3 && a.setAddress (s) && a.protocol () == QAbstractSocket::IPv4Protocol;
}

// "Peak +-11.2 mV of +-187.5 mV" -- uV below 1 mV.
QString peakText (double peakUv)
{
    const QString pk = peakUv >= 1000.0
        ? QStringLiteral ("±%1 mV").arg (peakUv / 1000.0, 0, 'f', 1)
        : QStringLiteral ("±%1 µV").arg (peakUv, 0, 'f', 1);
    return QStringLiteral ("Peak %1 of ±%2 mV").arg (pk).arg (Readouts::kCytonFullScaleUv / 1000.0, 0, 'f', 1);
}

// Rich-text paragraph with a fixed line pitch in px. (A CSS line-height % in
// Qt's rich text scales the font's natural line spacing -- ~1.21 em for Inter
// -- not the em size, so the design's 1.6 / 1.5 came out ~25 % too loose.)
QString richPara (const QString &html, int lineHeightPx)
{
    return QStringLiteral ("<p style=\"margin:0; line-height:%1px;\">%2</p>").arg (lineHeightPx).arg (html);
}

QString b (const QString &s, const QColor &c)
{
    return QStringLiteral ("<b style=\"color:%1; font-weight:600;\">%2</b>").arg (c.name (), s.toHtmlEscaped ());
}

bool isAuto (const QString &port)
{
    return port.isEmpty () || port.compare (QStringLiteral ("Auto"), Qt::CaseInsensitive) == 0;
}

// Auto-detect: the preferred port (the last one that connected) if it is one
// of the detected OpenBCI dongles, else the first dongle. "" = none.
QString pickCytonDongle (const QVector<SerialPortEntry> &ports, const QString &preferred)
{
    QString first;
    for (const SerialPortEntry &e : ports)
    {
        if (!e.cytonDongle)
            continue;
        const QString p = brainflowSerialPort (e);
        if (!preferred.isEmpty () && sameSerialPort (p, preferred))
            return p;
        if (first.isEmpty ())
            first = p;
    }
    return first;
}

} // namespace

// =============================================================================
MainWindow::MainWindow (const LaunchOptions &opts, QWidget *parent) : QMainWindow (parent), opts_ (opts)
{
    if (opts_.recordDir.isEmpty ())
        opts_.recordDir = defaultRecordDir ();
    if (opts_.portSet)
        portOverride_ = opts_.cytonPort; // an explicit --port is the rail's override
    if (opts_.useSettings)
        loadSettings ();
    opts_.emotibitTimeoutSec = std::clamp (opts_.emotibitTimeoutSec, 2, kMaxDiscoveryTimeoutSec);
    opts_.windowSec = std::clamp (opts_.windowSec, 1, 60);
    recordWanted_ = opts_.record;

    cyton_.kind = DeviceKind::Cyton;
    emotibit_.kind = DeviceKind::EmotiBit;
    cyton_.worker = new DeviceWorker (this);
    emotibit_.worker = new DeviceWorker (this);

    auto *central = new QWidget;
    central->setObjectName (QStringLiteral ("chassis"));
    auto *cv = new QVBoxLayout (central);
    cv->setContentsMargins (0, 0, 0, 0);
    cv->setSpacing (0);
    sessionBar_ = new SessionBar;
    cv->addWidget (sessionBar_);
    auto *body = new QHBoxLayout;
    body->setContentsMargins (0, 0, 0, 0);
    body->setSpacing (0);
    body->addWidget (buildRail ()); // before buildMain(): plots read the window stepper
    body->addWidget (buildMain (), 1);
    cv->addLayout (body, 1);
    statusStrip_ = new StatusStrip;
    cv->addWidget (statusStrip_);
    setCentralWidget (central);

    wireWorker (DeviceKind::Cyton);
    wireWorker (DeviceKind::EmotiBit);
    refreshPorts (false); // silent: the idle status message is the call to action
    applyFilterSettings ();
    updateSlotUi (cyton_);
    updateSlotUi (emotibit_);
    updateRecordUi ();
    updateWindowTitle ();

    // Keyboard: Cmd/Ctrl+K connects (or cancels a pending connect; never
    // disconnects), Cmd/Ctrl+R rescans serial ports.
    auto *connectKey = new QShortcut (kConnectKey, this);
    connect (connectKey, &QShortcut::activated, this, [this] {
        if (anyConnecting ())
            cancelPending ();
        else if (!anyActive ())
            connectAll ();
    });
    auto *rescan = new QShortcut (kRescanKey, this);
    connect (rescan, &QShortcut::activated, this, [this] {
        if (!portCombo_->isLocked () && portCombo_->isEnabled ())
            refreshPorts ();
    });

    frameTimer_ = new QTimer (this);
    frameTimer_->setTimerType (Qt::PreciseTimer);
    frameTimer_->setInterval (16);
    connect (frameTimer_, &QTimer::timeout, this, &MainWindow::onFrame);
    frameTimer_->start ();

    rateTimer_ = new QTimer (this);
    rateTimer_->setInterval (250);
    connect (rateTimer_, &QTimer::timeout, this, &MainWindow::onRateTimer);
    rateTimer_->start ();

    slowTimer_ = new QTimer (this);
    slowTimer_->setInterval (1000);
    connect (slowTimer_, &QTimer::timeout, this, &MainWindow::onSlowTimer);
    slowTimer_->start ();

    cpu_.sample ();
    onRateTimer (); // the idle status message is the design's call to action
}

MainWindow::~MainWindow ()
{
    frameTimer_->stop ();
    rateTimer_->stop ();
    slowTimer_->stop ();
    cyton_.worker->requestStop ();
    emotibit_.worker->requestStop ();
    cyton_.worker->waitForFinished (60000);
    emotibit_.worker->waitForFinished (60000);
}

void MainWindow::loadSettings ()
{
    QSettings st;
    if (!opts_.portSet)
    {
        if (!st.value (kKeyPort).toString ().isEmpty ())
            opts_.cytonPort = st.value (kKeyPort).toString ();
        portOverride_ = st.value (kKeyPortOverride).toString ();
    }
    if (!opts_.ipSet && st.contains (kKeyIp))
        opts_.emotibitIp = st.value (kKeyIp).toString (); // may be "" = last IP, then broadcast
    lastEmotibitIp_ = st.value (kKeyLastIp).toString ();
    lastBluetoothName_ = st.value (kKeyLastBle).toString ();
    if (!opts_.timeoutSet)
        opts_.emotibitTimeoutSec = st.value (kKeyTimeout, opts_.emotibitTimeoutSec).toInt ();
    if (!opts_.windowSet)
        opts_.windowSec = st.value (kKeyWindow, opts_.windowSec).toInt ();
    if (!opts_.recordSet)
        opts_.record = st.value (kKeyRecord, opts_.record).toBool ();
    opts_.removeDc = st.value (kKeyDc, opts_.removeDc).toBool ();
    opts_.highPass = st.value (kKeyHp, opts_.highPass).toBool ();
    opts_.notch = st.value (kKeyNotch, opts_.notch).toBool ();
    opts_.lowPass = st.value (kKeyLp, opts_.lowPass).toBool ();
}

void MainWindow::saveSettings ()
{
    if (!opts_.useSettings)
        return;
    QSettings st;
    st.setValue (kKeyTimeout, timeoutSpin_->value ());
    st.setValue (kKeyWindow, windowStepper_->value ());
    st.setValue (kKeyDc, dcToggle_->isChecked ());
    st.setValue (kKeyHp, hpToggle_->isChecked ());
    st.setValue (kKeyNotch, notchToggle_->isChecked ());
    st.setValue (kKeyLp, lpToggle_->isChecked ());
    st.setValue (kKeyRecord, recordWanted_);
    st.setValue (kKeyPortOverride, portOverride ());
}

void MainWindow::saveDeviceAddress (DeviceKind kind, const QString &address)
{
    if (!opts_.useSettings || address.isEmpty ())
        return;
    QSettings st;
    if (kind == DeviceKind::Cyton)
        st.setValue (kKeyPort, address);
    else if (validIpv4 (address))
    {
        st.setValue (kKeyIp, ipEdit_->text ().trimmed ());
        st.setValue (kKeyLastIp, address);
    }
}

// =============================================================== rail
namespace
{

// The rail's scrolling module list. Its viewport reaches kClip px below the
// visible area, so an overflow no larger than the last module's bottom
// padding + border is clipped instead of adding a scroll bar (which would
// also narrow the rail) -- the design's 1280 x 800 artboard hides that
// padding the same way. Larger overflows scroll as usual.
class RailScroll : public QScrollArea
{
public:
    static constexpr int kClip = 14; // Display module: 13 px bottom padding + 1 px border
    RailScroll ()
    {
        setViewportMargins (0, 0, 0, -kClip);
    }
};

QFrame *moduleFrame (QVBoxLayout *&lay, const char *name = "module", int bottom = 13)
{
    auto *f = new QFrame;
    f->setObjectName (QString::fromLatin1 (name));
    lay = new QVBoxLayout (f);
    lay->setContentsMargins (12, 11, 12, bottom);
    lay->setSpacing (0);
    return f;
}

QHBoxLayout *headerRow (const QString &title, StatusChip *chip)
{
    auto *h = new QHBoxLayout;
    h->setContentsMargins (0, 0, 0, 0);
    h->setSpacing (6);
    h->addWidget (kicker (title, 10.0));
    h->addStretch (1);
    if (chip)
        h->addWidget (chip, 0, Qt::AlignVCenter);
    return h;
}

} // namespace

QWidget *MainWindow::buildRail ()
{
    auto *rail = new QFrame;
    rail->setObjectName (QStringLiteral ("rail"));
    rail->setFixedWidth (Theme::railWidth);
    auto *rv = new QVBoxLayout (rail);
    rv->setContentsMargins (0, 0, 0, 0);
    rv->setSpacing (0);

    auto *scroll = new RailScroll;
    scroll->setFrameShape (QFrame::NoFrame);
    scroll->setWidgetResizable (true);
    scroll->setHorizontalScrollBarPolicy (Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget;
    content->setObjectName (QStringLiteral ("railContent"));
    auto *cl = new QVBoxLayout (content);
    cl->setContentsMargins (0, 0, 0, 0);
    cl->setSpacing (0);
    cl->addWidget (buildConnectModule ());
    cl->addWidget (buildCytonModule ());
    cl->addWidget (buildEmotibitModule ());
    cl->addWidget (buildDisplayModule ());
    cl->addStretch (1);
    scroll->setWidget (content);
    rv->addWidget (scroll, 1);
    rv->addWidget (buildRecordModule ());
    return rail;
}

QWidget *MainWindow::buildConnectModule ()
{
    // Just the one button (its tooltip says what it does right now): the
    // rail must fit 1280 x 800 with the discovery module open.
    QVBoxLayout *v = nullptr;
    QFrame *f = moduleFrame (v, "module", 11);
    connectBtn_ = makeButton (QStringLiteral ("[ connect ]"), "primary", "lg", 32);
    v->addWidget (connectBtn_);
    connect (connectBtn_, &QPushButton::clicked, this, &MainWindow::onConnectButton);
    return f;
}

QWidget *MainWindow::buildErrorBox (Slot &s)
{
    auto *wrap = new QWidget;
    auto *wl = new QVBoxLayout (wrap);
    wl->setContentsMargins (0, 9, 0, 0);
    wl->setSpacing (0);
    auto *box = new QFrame;
    box->setObjectName (QStringLiteral ("errorBox"));
    auto *bl = new QVBoxLayout (box);
    bl->setContentsMargins (10, 10, 10, 10);
    bl->setSpacing (0);
    auto *hr = new QHBoxLayout;
    hr->setContentsMargins (0, 0, 0, 0);
    hr->setSpacing (6);
    hr->addWidget (new WarnGlyph);
    s.errorTitle = makeLabel (QString (), Theme::mono (9.5, 400, 0.12), Theme::fault);
    hr->addWidget (s.errorTitle, 1);
    bl->addLayout (hr);
    bl->addSpacing (6);
    s.errorText = makeLabel (QString (), Theme::sans (11.5), Theme::textBody);
    s.errorText->setWordWrap (true);
    s.errorText->setTextFormat (Qt::RichText);
    s.errorText->setTextInteractionFlags (Qt::TextSelectableByMouse);
    bl->addWidget (s.errorText);
    bl->addSpacing (7);
    auto *rule = new QFrame;
    rule->setObjectName (QStringLiteral ("boxRule"));
    rule->setFixedHeight (1);
    bl->addWidget (rule);
    bl->addSpacing (7);
    s.errorHint = makeLabel (QString (), Theme::sans (11.5), Theme::textMuted);
    s.errorHint->setWordWrap (true);
    s.errorHint->setTextFormat (Qt::RichText);
    s.errorHint->setTextInteractionFlags (Qt::TextSelectableByMouse);
    bl->addWidget (s.errorHint);
    wl->addWidget (box);
    wrap->hide ();
    s.errorWrap = wrap;
    return wrap;
}

QWidget *MainWindow::buildCytonModule ()
{
    QVBoxLayout *v = nullptr;
    QFrame *f = moduleFrame (v);
    cyton_.chip = new StatusChip;
    v->addLayout (headerRow (QStringLiteral ("// CYTON · ECG"), cyton_.chip));
    v->addSpacing (9);
    v->addWidget (kicker (QStringLiteral ("// SERIAL PORT")));
    v->addSpacing (5);
    auto *row = new QHBoxLayout;
    row->setContentsMargins (0, 0, 0, 0);
    row->setSpacing (6);
    portCombo_ = new PortCombo;
    portCombo_->setEditable (true);
    portCombo_->setSizeAdjustPolicy (QComboBox::AdjustToMinimumContentsLengthWithIcon);
    portCombo_->setMinimumContentsLength (8);
    portCombo_->setToolTip (QStringLiteral ("Auto finds the Cyton's USB dongle (preferring the last port that connected).\n"
                                            "Pick or type a port to override it. Close the OpenBCI GUI first:\n"
                                            "only one program can use the dongle."));
    refreshBtn_ = new RescanButton;
    refreshBtn_->setToolTip (QStringLiteral ("Rescan serial ports  (%1)").arg (keyText (kRescanKey)));
    row->addWidget (portCombo_, 1);
    row->addWidget (refreshBtn_, 0, Qt::AlignTop); // 28 px button top-aligned with the 30 px field (design)
    v->addLayout (row);
    v->addSpacing (7);
    cyton_.meta = new ElideLabel (QString (), Qt::ElideRight);
    cyton_.meta->setFont (Theme::mono (10, 400, 0.02));
    Theme::setTextColor (cyton_.meta, Theme::textDim);
    v->addWidget (cyton_.meta);
    v->addWidget (buildErrorBox (cyton_));

    v->addSpacing (12);
    v->addWidget (kicker (QStringLiteral ("// ECG DISPLAY FILTERS")));
    v->addSpacing (6);
    dcToggle_ = new ToggleSwitch (QStringLiteral ("Remove DC"));
    dcToggle_->setChecked (opts_.removeDc);
    dcToggle_->setToolTip (QStringLiteral ("Display only: subtracts the mean of the visible window. The rail headroom\n"
                                           "below still measures the raw value, so a DC offset near the rail shows."));
    hpToggle_ = new ToggleSwitch (QStringLiteral ("HP 0.5 Hz"));
    hpToggle_->setChecked (opts_.highPass);
    hpToggle_->setToolTip (QStringLiteral ("High-pass 0.5 Hz: 2nd-order RBJ biquad (IIR) applied sample-by-sample in the\n"
                                           "worker thread; removes baseline wander, keeps the ECG's P and T waves.\n"
                                           "Filter state resets on toggle."));
    notchToggle_ = new ToggleSwitch (QStringLiteral ("Notch 60 Hz"));
    notchToggle_->setChecked (opts_.notch);
    notchToggle_->setToolTip (QStringLiteral ("RBJ notch at 60 Hz (IIR, Q = 30), streaming, in the worker thread."));
    lpToggle_ = new ToggleSwitch (QStringLiteral ("LP 40 Hz"));
    lpToggle_->setChecked (opts_.lowPass);
    lpToggle_->setToolTip (QStringLiteral ("Low-pass 40 Hz: 2nd-order RBJ biquad (IIR) after the notch, in the worker\n"
                                           "thread; removes EMG and high-frequency noise."));
    auto *grid = new QGridLayout;
    grid->setContentsMargins (0, 0, 0, 0);
    grid->setHorizontalSpacing (6);
    grid->setVerticalSpacing (6);
    grid->addWidget (dcToggle_, 0, 0);
    grid->addWidget (notchToggle_, 0, 1);
    grid->addWidget (hpToggle_, 1, 0);
    grid->addWidget (lpToggle_, 1, 1);
    grid->setColumnStretch (0, 0); // "Remove DC" / "HP 0.5 Hz" at their size, the rest to "Notch 60 Hz"
    grid->setColumnStretch (1, 1);
    v->addLayout (grid);

    v->addSpacing (12);
    auto *hrow = new QHBoxLayout;
    hrow->setContentsMargins (0, 0, 0, 0);
    hrow->addWidget (kicker (QStringLiteral ("// RAIL HEADROOM · 2 s")), 0, Qt::AlignBottom);
    hrow->addStretch (1);
    railPct_ = makeLabel (QStringLiteral ("—"), Theme::mono (10.5), Theme::controlEdge);
    hrow->addWidget (railPct_, 0, Qt::AlignBottom);
    v->addLayout (hrow);
    v->addSpacing (4);
    railBar_ = new MeterBar (12, Theme::edge); // content-box 8 + padding 1 + border 1 (design)
    railBar_->setToolTip (QStringLiteral ("1 − max|raw ECG| / 187,500 µV over the last 2 s (before filters)."));
    v->addWidget (railBar_);
    v->addSpacing (4);
    railNote_ = makeLabel (QStringLiteral ("No signal"), Theme::mono (10), Theme::textDim);
    v->addWidget (railNote_);

    connect (refreshBtn_, &QPushButton::clicked, this, [this] { refreshPorts (); });
    for (ToggleSwitch *t : {dcToggle_, hpToggle_, notchToggle_, lpToggle_})
        connect (t, &QAbstractButton::toggled, this, [this] (bool) { applyFilterSettings (); });
    connect (portCombo_, &QComboBox::currentTextChanged, this, [this] (const QString &) { updateSlotUi (cyton_); });
    return f;
}

QWidget *MainWindow::buildEmotibitModule ()
{
    QVBoxLayout *v = nullptr;
    QFrame *f = moduleFrame (v);
    emotibit_.chip = new StatusChip;
    v->addLayout (headerRow (QStringLiteral ("// EMOTIBIT · PPG"), emotibit_.chip));
    v->addSpacing (9);

    auto *row = new QHBoxLayout;
    row->setContentsMargins (0, 0, 0, 0);
    row->setSpacing (6);
    auto *c1 = new QVBoxLayout;
    c1->setSpacing (5);
    c1->addWidget (kicker (QStringLiteral ("// IP ADDRESS")));
    ipEdit_ = new QLineEdit (opts_.emotibitIp);
    ipEdit_->setFixedHeight (30); // content-box 28 + 1 px border (design)
    ipEdit_->setPlaceholderText (QStringLiteral ("auto"));
    ipEdit_->setToolTip (QStringLiteral (
        "EmotiBit IP address (shown in its serial boot log and in EmotiBit Oscilloscope).\n"
        "Connect tries this address first (unicast, works across subnets; blank: the last EmotiBit\n"
        "that answered), then broadcast discovery, which finds the EmotiBit on any network this\n"
        "computer shares with it (same subnet), e.g. a hotspot. A new address replaces the field."));
    c1->addWidget (ipEdit_);
    auto *c2 = new QVBoxLayout;
    c2->setSpacing (5);
    c2->addWidget (kicker (QStringLiteral ("// TIMEOUT")));
    timeoutSpin_ = new QSpinBox;
    timeoutSpin_->setFixedSize (72, 30);
    timeoutSpin_->setRange (2, kMaxDiscoveryTimeoutSec);
    timeoutSpin_->setValue (opts_.emotibitTimeoutSec);
    timeoutSpin_->setSuffix (QStringLiteral (" s"));
    timeoutSpin_->setButtonSymbols (QAbstractSpinBox::NoButtons);
    timeoutSpin_->setToolTip (QStringLiteral (
        "How long to wait for the EmotiBit to answer (2-20 s; arrow keys / scroll to change).\n"
        "The search runs outside BrainFlow and can be cancelled at any time; it does not pause the Cyton."));
    c2->addWidget (timeoutSpin_);
    row->addLayout (c1, 1);
    row->addLayout (c2);
    v->addLayout (row);
    v->addSpacing (7);
    auto *metaRow = new QHBoxLayout;
    metaRow->setContentsMargins (0, 0, 0, 0);
    metaRow->setSpacing (8);
    emotibit_.meta = new ElideLabel (QString (), Qt::ElideRight);
    emotibit_.meta->setFont (Theme::mono (10, 400, 0.02));
    Theme::setTextColor (emotibit_.meta, Theme::textDim);
    metaRow->addWidget (emotibit_.meta, 1);
    // Wi-Fi setup over USB (EmotiBitWifiDialog): a text link, so the rail keeps its height
    wifiBtn_ = new QPushButton (QStringLiteral ("wi-fi setup"));
    wifiBtn_->setObjectName (QStringLiteral ("wifiLink"));
    wifiBtn_->setFixedHeight (16);
    wifiBtn_->setCursor (Qt::PointingHandCursor);
    wifiBtn_->setFocusPolicy (Qt::TabFocus);
    wifiBtn_->setStyleSheet (
        QStringLiteral ("QPushButton#wifiLink { border: 0; background: transparent; padding: 0; color: %1;"
                        " font-family: \"%4\"; font-size: 10px; font-weight: 400; text-decoration: underline; }"
                        "QPushButton#wifiLink:hover { color: %2; }"
                        "QPushButton#wifiLink:disabled { color: %3; text-decoration: none; }")
            .arg (Theme::textMuted.name (), Theme::textStrong.name (), Theme::textFainter.name (), Theme::monoFamily ()));
    connect (wifiBtn_, &QPushButton::clicked, this, [this] { openWifiSetup (); });
    metaRow->addWidget (wifiBtn_, 0, Qt::AlignRight | Qt::AlignVCenter);
    v->addLayout (metaRow);

    // discovering module (cancelled with the Connect button)
    discoverWrap_ = new QWidget;
    auto *dwl = new QVBoxLayout (discoverWrap_);
    dwl->setContentsMargins (0, 9, 0, 0);
    dwl->setSpacing (0);
    auto *box = new QFrame;
    box->setObjectName (QStringLiteral ("discoverBox"));
    auto *bl = new QVBoxLayout (box);
    bl->setContentsMargins (10, 10, 10, 10);
    bl->setSpacing (0);
    auto *tr = new QHBoxLayout;
    tr->setContentsMargins (0, 0, 0, 0);
    discoverTitle_ = makeLabel (QStringLiteral ("DISCOVERING…"), Theme::mono (9.5, 400, 0.12), Theme::warn);
    discoverTime_ = makeLabel (QString (), Theme::mono (10.5), Theme::warn);
    tr->addWidget (discoverTitle_, 0, Qt::AlignBottom);
    tr->addStretch (1);
    tr->addWidget (discoverTime_, 0, Qt::AlignBottom);
    bl->addLayout (tr);
    bl->addSpacing (6);
    discoverBar_ = new MeterBar (10, Theme::controlEdge); // content-box 6 + 1 + 1 (design)
    bl->addWidget (discoverBar_);
    bl->addSpacing (7);
    discoverDetail_ = makeLabel (QString (), Theme::mono (10), Theme::textMuted);
    discoverDetail_->setWordWrap (true);
    bl->addWidget (discoverDetail_);
    dwl->addWidget (box);
    discoverWrap_->hide ();
    v->addWidget (discoverWrap_);

    v->addWidget (buildErrorBox (emotibit_));

    connect (ipEdit_, &QLineEdit::textChanged, this, [this] (const QString &t) {
        const QString s = t.trimmed ();
        setFlag (ipEdit_, "invalid", !s.isEmpty () && !validIpv4 (s));
        updateSlotUi (emotibit_);
    });
    setFlag (ipEdit_, "invalid", !ipEdit_->text ().trimmed ().isEmpty () && !validIpv4 (ipEdit_->text ().trimmed ()));
    return f;
}

QWidget *MainWindow::buildDisplayModule ()
{
    QVBoxLayout *v = nullptr;
    QFrame *f = moduleFrame (v);
    v->addWidget (kicker (QStringLiteral ("// DISPLAY"), 10.0));
    v->addSpacing (9);
    auto *row = new QHBoxLayout;
    row->setContentsMargins (0, 0, 0, 0);
    row->addWidget (makeLabel (QStringLiteral ("Time window"), Theme::sans (11.5), Theme::textBody));
    row->addStretch (1);
    windowStepper_ = new Stepper (1, 60, opts_.windowSec);
    windowStepper_->setToolTip (QStringLiteral ("Visible time span of every plot, 1-60 s."));
    row->addWidget (windowStepper_);
    v->addLayout (row);
    v->addSpacing (9);
    pauseToggle_ = new ToggleSwitch (QStringLiteral ("Pause display"), true);
    pauseToggle_->setToolTip (QStringLiteral ("Freezes the plots only: streams, recording, heart rate and the\n"
                                              "rail-headroom warning keep running."));
    v->addWidget (pauseToggle_);
    // ("Simulate devices" lives in the idle call to action over the hero plot:
    // it can only change while nothing is connected.)

    QString ver;
    try
    {
        ver = QString::fromStdString (BoardShim::get_version ());
    }
    catch (const std::exception &)
    {
        ver = QStringLiteral ("?");
    }
    f->setToolTip (QStringLiteral ("bioacq %1 · BrainFlow %2 · Qt %3%4")
                       .arg (QStringLiteral (BIOACQ_VERSION), ver, QString::fromLatin1 (qVersion ()),
                           Theme::fontsEmbedded () ? QStringLiteral (" · JetBrains Mono + Inter (OFL)")
                                                   : QStringLiteral (" · fallback fonts")));

    connect (windowStepper_, &Stepper::valueChanged, this, [this] (int s) {
        for (PlotWidget *p : allPlots ())
            p->setWindowSeconds (s);
    });
    connect (pauseToggle_, &QAbstractButton::toggled, this, [this] (bool on) {
        for (PlotWidget *p : allPlots ())
            p->setPaused (on);
    });
    return f;
}

QWidget *MainWindow::buildRecordModule ()
{
    QVBoxLayout *v = nullptr;
    QFrame *f = moduleFrame (v, "recordModule", 12);
    v->addWidget (kicker (QStringLiteral ("// RECORD"), 10.0));
    v->addSpacing (9);
    recordBtn_ = new RecordButton;
    recordBtn_->setToolTip (QStringLiteral (
        "Record every BrainFlow preset of each streaming device to CSV (tab-separated raw rows, no header;\n"
        "a *_columns.json next to each file describes the rows). Filters and heart rate are display-only and\n"
        "not recorded. Starts and stops at any time while streaming; pressed with nothing connected it arms\n"
        "the recording for the next connect.\nFolder: %1")
                                .arg (opts_.recordDir));
    v->addWidget (recordBtn_);
    v->addSpacing (8);
    auto addRow = [&] (const QString &k, QLabel *value) {
        auto *r = new QHBoxLayout;
        r->setContentsMargins (0, 0, 0, 0);
        r->setSpacing (8);
        r->addWidget (kicker (k, 9.0, 0.12), 0, Qt::AlignBottom);
        r->addWidget (value, 1, Qt::AlignBottom);
        v->addLayout (r);
    };
    recFolder_ = new ElideLabel (homeAbbrev (opts_.recordDir));
    recFolder_->setFont (Theme::mono (10));
    recFolder_->setAlignment (Qt::AlignRight | Qt::AlignVCenter);
    recFolder_->setToolTip (opts_.recordDir);
    Theme::setTextColor (recFolder_, Theme::textMuted);
    recElapsed_ = makeLabel (QStringLiteral ("—"), Theme::mono (10.5), Theme::textDim);
    recElapsed_->setAlignment (Qt::AlignRight | Qt::AlignVCenter);
    recSize_ = makeLabel (QStringLiteral ("—"), Theme::mono (10.5), Theme::textDim);
    recSize_->setAlignment (Qt::AlignRight | Qt::AlignVCenter);
    addRow (QStringLiteral ("FOLDER"), recFolder_);
    v->addSpacing (4);
    addRow (QStringLiteral ("ELAPSED"), recElapsed_);
    v->addSpacing (4);
    addRow (QStringLiteral ("FILE SIZE"), recSize_);
    connect (recordBtn_, &QPushButton::clicked, this, [this] { setRecording (!recordWanted_); });
    return f;
}

// =============================================================== main area
QWidget *MainWindow::buildMain ()
{
    auto *w = new QWidget;
    w->setObjectName (QStringLiteral ("mainArea"));
    w->setAttribute (Qt::WA_StyledBackground);
    auto *v = new QVBoxLayout (w);
    v->setContentsMargins (Theme::gutter, Theme::gutter, Theme::gutter, Theme::gutter);
    v->setSpacing (Theme::gutter);

    banner_ = new Banner;
    banner_->hide ();
    v->addWidget (banner_);

    using Dash = PlotWidget::Dash;
    using Kind = PlotWidget::Kind;
    const QVector<PlotWidget::Trace> xyz = {{QStringLiteral ("X"), Theme::traceX, Dash::Solid},
        {QStringLiteral ("Y"), Theme::traceY, Dash::Dashed}, {QStringLiteral ("Z"), Theme::traceZ, Dash::Dotted}};
    cytonPlot_ = new PlotWidget (Kind::Hero, QStringLiteral ("ECG · CYTON CH1"), QStringLiteral ("µV"),
        {{QStringLiteral ("ECG"), Theme::traceHero, Dash::Solid}});
    cytonPlot_->setSubtitle (QStringLiteral ("single-ended · SRB / AGND / N1P"));
    cytonPlot_->setFullScale (Readouts::kCytonFullScaleUv);
    ppgGreenPlot_ = new PlotWidget (Kind::Scalar, QStringLiteral ("PPG GREEN"), QStringLiteral ("a.u."),
        {{QStringLiteral ("green"), Theme::tracePpgGreen, Dash::Solid}});
    ppgRedPlot_ = new PlotWidget (Kind::Scalar, QStringLiteral ("PPG RED"), QStringLiteral ("a.u."),
        {{QStringLiteral ("red"), Theme::tracePpgRed, Dash::Solid}});
    ppgIrPlot_ = new PlotWidget (Kind::Scalar, QStringLiteral ("PPG IR"), QStringLiteral ("a.u."),
        {{QStringLiteral ("IR"), Theme::tracePpgIr, Dash::Solid}});
    hrPlot_ = new PlotWidget (Kind::Vital, QStringLiteral ("HEART RATE"),
        QVector<PlotWidget::LaneSpec> {
            {QString (), QStringLiteral ("bpm"), {{QStringLiteral ("HR"), Theme::traceHeartRate, Dash::Solid}}, 20.0}});
    imuPlot_ = new PlotWidget (Kind::Lanes, QStringLiteral ("IMU"),
        QVector<PlotWidget::LaneSpec> {{QStringLiteral ("ACC"), QStringLiteral ("g"), xyz, 0.02},
            {QStringLiteral ("GYR"), QStringLiteral ("°/s"), xyz, 1.0},
            {QStringLiteral ("MAG"), QStringLiteral ("µT"), xyz, 1.0}});
    tempPlot_ = new PlotWidget (Kind::Scalar, QStringLiteral ("TEMPERATURE"), QStringLiteral ("°C"),
        {{QStringLiteral ("T"), Theme::traceTemp, Dash::Solid}});

    // Hero ECG across the top; the PPG channels matter most after it, so
    // green | red | IR get a third of the width each and the tallest row; heart
    // rate, temperature (slow, sample-and-hold) and the IMU's three lanes share
    // a shorter bottom row (12-column grid: 3 | 3 | 6).
    auto *grid = new QGridLayout;
    grid->setContentsMargins (0, 0, 0, 0);
    grid->setSpacing (Theme::gutter);
    grid->addWidget (cytonPlot_, 0, 0, 1, 12);
    grid->addWidget (ppgGreenPlot_, 1, 0, 1, 4);
    grid->addWidget (ppgRedPlot_, 1, 4, 1, 4);
    grid->addWidget (ppgIrPlot_, 1, 8, 1, 4);
    grid->addWidget (hrPlot_, 2, 0, 1, 3);
    grid->addWidget (tempPlot_, 2, 3, 1, 3);
    grid->addWidget (imuPlot_, 2, 6, 1, 6);
    for (int c = 0; c < 12; ++c)
        grid->setColumnStretch (c, 1);
    grid->setRowStretch (0, 110);
    grid->setRowStretch (1, 150);
    grid->setRowStretch (2, 90);
    v->addLayout (grid, 1);

    for (PlotWidget *p : allPlots ())
    {
        p->setWindowSeconds (windowStepper_->value ());
        p->setPlaceholder (QStringLiteral ("NO SIGNAL"));
        if (p != hrPlot_)
            p->setRate (Readouts::rateText (false, 0.0, p == cytonPlot_ ? 250.0 : (p == tempPlot_ ? 15.0 : 25.0)),
                Theme::textDim);
    }
    idleOverlay_ = buildIdleOverlay ();
    cytonPlot_->setOverlay (idleOverlay_);
    return w;
}

QWidget *MainWindow::buildIdleOverlay ()
{
    auto *ov = new QWidget;
    ov->setAutoFillBackground (true);
    QPalette pal = ov->palette ();
    pal.setColor (QPalette::Window, Theme::recess);
    ov->setPalette (pal);
    auto *v = new QVBoxLayout (ov);
    v->setContentsMargins (16, 8, 16, 8);
    v->setSpacing (0);
    // The design's gaps; they shrink (down to 0) before anything is clipped
    // when the hero recess is short (1280 x 800), and never grow.
    auto gap = [v] (int px) { v->addItem (new QSpacerItem (0, px, QSizePolicy::Minimum, QSizePolicy::Maximum)); };
    v->addStretch (1);
    auto *k = makeLabel (QStringLiteral ("NO DEVICES CONNECTED"), Theme::mono (10, 400, 0.2), Theme::textDim);
    k->setAlignment (Qt::AlignCenter);
    v->addWidget (k);
    gap (14);
    auto *t = makeLabel (QStringLiteral ("Connect to start streaming"), Theme::mono (19, 400, -0.01), Theme::textStrong);
    t->setAlignment (Qt::AlignCenter);
    v->addWidget (t);
    gap (14);
    auto *para = makeLabel (richPara (QStringLiteral (
                                          "One button opens every available device at once: the Cyton on its USB "
                                          "dongle and the EmotiBit over Bluetooth or Wi-Fi (the address in the left "
                                          "rail, then this computer's network). Plots arm themselves as soon as "
                                          "samples arrive."),
                                20), // 12.5 px x 1.6
        Theme::sans (12.5), Theme::textMuted);
    para->setWordWrap (true);
    para->setAlignment (Qt::AlignCenter);
    para->setFixedWidth (520);
    para->setFixedHeight (para->heightForWidth (520));
    v->addWidget (para, 0, Qt::AlignHCenter);
    gap (16);
    auto *btns = new QHBoxLayout;
    btns->setSpacing (10);
    btns->addStretch (1);
    auto *bc = makeButton (QStringLiteral ("[ connect ]"), "primary", "lg", 32);
    bc->setStyleSheet (QStringLiteral ("padding: 0 16px;"));
    bc->setToolTip (QStringLiteral ("Connect every available device at once (%1).").arg (keyText (kConnectKey)));
    btns->addWidget (bc);
    btns->addStretch (1);
    v->addLayout (btns);
    gap (18);
    auto *hints = new QHBoxLayout;
    hints->setSpacing (8);
    hints->addStretch (1);
    auto keycap = [] (const QString &s) {
        auto *l = makeLabel (s, Theme::mono (10), Theme::textDim);
        l->setObjectName (QStringLiteral ("keycap"));
        return l;
    };
    auto vsep = [] {
        auto *sep = new QFrame;
        sep->setFixedSize (1, 12);
        sep->setAutoFillBackground (true);
        QPalette sp = sep->palette ();
        sp.setColor (QPalette::Window, Theme::edge);
        sep->setPalette (sp);
        return sep;
    };
    hints->addWidget (keycap (keyText (kConnectKey)));
    hints->addWidget (makeLabel (QStringLiteral ("connect"), Theme::mono (10), Theme::textDim));
    hints->addWidget (vsep ());
    hints->addWidget (keycap (keyText (kRescanKey)));
    hints->addWidget (makeLabel (QStringLiteral ("rescan serial ports"), Theme::mono (10), Theme::textDim));
    hints->addWidget (vsep ());
    // Only changeable while nothing is connected -- exactly when this overlay
    // shows; afterwards the session label / detail lines say "synthetic".
    synthToggle_ = new ToggleSwitch (QStringLiteral ("simulate devices"));
    synthToggle_->setFont (Theme::mono (10));
    synthToggle_->setChecked (opts_.synthetic);
    synthToggle_->setToolTip (QStringLiteral ("Connect both slots to BrainFlow's synthetic board (no hardware):\n"
                                              "tests the whole pipeline. Locked while a device is connected."));
    hints->addWidget (synthToggle_);
    hints->addStretch (1);
    v->addLayout (hints);
    v->addStretch (1);
    connect (bc, &QPushButton::clicked, this, &MainWindow::connectAll);
    connect (synthToggle_, &QAbstractButton::toggled, this, [this] (bool) {
        updateWindowTitle ();
        updateSlotUi (cyton_);
        updateSlotUi (emotibit_);
    });
    return ov;
}

std::array<PlotWidget *, 7> MainWindow::allPlots () const
{
    return {cytonPlot_, ppgGreenPlot_, ppgRedPlot_, ppgIrPlot_, hrPlot_, imuPlot_, tempPlot_};
}

PlotWidget *MainWindow::plotForKey (const std::string &key, int *lane) const
{
    if (lane)
        *lane = 0;
    if (key == SignalKeys::CytonEcg)
        return cytonPlot_;
    if (key == SignalKeys::EmotiPpgGreen)
        return ppgGreenPlot_;
    if (key == SignalKeys::EmotiPpgRed)
        return ppgRedPlot_;
    if (key == SignalKeys::EmotiPpgIr)
        return ppgIrPlot_;
    if (key == SignalKeys::EmotiHeartRate)
        return hrPlot_;
    if (key == SignalKeys::EmotiTemp)
        return tempPlot_;
    const int imuLane = key == SignalKeys::EmotiAccel ? 0 : (key == SignalKeys::EmotiGyro ? 1 : (key == SignalKeys::EmotiMag ? 2 : -1));
    if (imuLane >= 0)
    {
        if (lane)
            *lane = imuLane;
        return imuPlot_;
    }
    return nullptr;
}

const QString &MainWindow::subnets ()
{
    const double now = DeviceWorker::steadyNow ();
    if (now - subnetsAt_ < 5.0)
        return subnets_;
    subnetsAt_ = now;
    QStringList out;
    for (const QNetworkInterface &ifc : QNetworkInterface::allInterfaces ())
    {
        const auto fl = ifc.flags ();
        if (!(fl & QNetworkInterface::IsUp) || !(fl & QNetworkInterface::IsRunning) ||
            (fl & QNetworkInterface::IsLoopBack) || (fl & QNetworkInterface::IsPointToPoint))
            continue;
        for (const QNetworkAddressEntry &e : ifc.addressEntries ())
        {
            const QHostAddress ip = e.ip ();
            const int prefix = e.prefixLength ();
            if (ip.protocol () != QAbstractSocket::IPv4Protocol || prefix <= 0 || prefix >= 32)
                continue;
            const quint32 mask = 0xFFFFFFFFu << (32 - prefix);
            out << QStringLiteral ("%1/%2 (%3)")
                       .arg (QHostAddress (ip.toIPv4Address () & mask).toString ())
                       .arg (prefix)
                       .arg (ifc.humanReadableName ());
        }
    }
    subnets_ = out.isEmpty () ? QStringLiteral ("no IPv4 network") : out.join (QStringLiteral (", "));
    return subnets_;
}

// =============================================================== workers
void MainWindow::wireWorker (DeviceKind kind)
{
    DeviceWorker *w = slot (kind).worker;
    const QString name = QString::fromLatin1 (deviceDisplayName (kind));

    connect (w, &DeviceWorker::stateChanged, this, [this, kind] (int) {
        updateSlotUi (slot (kind));
        updateConnectUi ();
    });

    connect (w, &DeviceWorker::progress, this, [this, kind] (const QString &text) {
        Slot &s = slot (kind);
        if (s.worker->state () == DeviceWorker::Connecting && !s.stopRequested)
        {
            s.progressText = text;
            updateSlotUi (s);
        }
    });

    connect (w, &DeviceWorker::discovered, this, [this, kind] (const QString &ip, const QString &serial) {
        Slot &s = slot (kind);
        s.address = ip;
        s.serial = serial;
        // found by the broadcast fallback at a new address: the typed IP was
        // stale, so the field (saved on connect) follows the device
        const QString field = ipEdit_->text ().trimmed ();
        if (kind == DeviceKind::EmotiBit && !field.isEmpty () && field != ip && validIpv4 (ip))
            ipEdit_->setText (ip);
        showMessage (QStringLiteral ("EmotiBit%1 found at %2")
                         .arg (serial.isEmpty () ? QString () : QStringLiteral (" ") + serial, ip));
    });

    connect (w, &DeviceWorker::bluetoothLinked, this, [this, kind] (const QString &deviceName) {
        Slot &s = slot (kind);
        s.bluetooth = true;
        s.bluetoothName = deviceName;
        s.address.clear ();
        s.serial = EmotiBitBleBridge::deviceIdFromName (deviceName);
        showMessage (QStringLiteral ("%1 found over Bluetooth").arg (deviceName));
    });

    connect (w, &DeviceWorker::connected, this, [this, kind, name] (const QString &) {
        Slot &s = slot (kind);
        s.linkSeconds = DeviceWorker::steadyNow () - s.connectStarted;
        s.streamingSinceWall = wallClockSeconds (); // emitted right after the worker reaches Streaming
        s.failed = false;
        if (!std::isfinite (sessionStartWall_))
            sessionStartWall_ = wallClockSeconds ();
        showMessage (QStringLiteral ("%1 connected in %2 s").arg (name).arg (s.linkSeconds, 0, 'f', 1), 5000);
        if (!s.synthetic && s.bluetooth)
        {
            lastBluetoothName_ = s.bluetoothName; // the scan prefers it next time
            if (opts_.useSettings)
                QSettings ().setValue (kKeyLastBle, s.bluetoothName);
        }
        else if (!s.synthetic)
        {
            saveDeviceAddress (kind, s.address);
            if (kind == DeviceKind::Cyton)
                opts_.cytonPort = s.address; // auto-detect prefers it next time
            else if (validIpv4 (s.address))
                lastEmotibitIp_ = s.address;
        }
        saveSettings ();
        updateSlotUi (s);
        updateConnectUi ();
    });

    connect (w, &DeviceWorker::failed, this, [this, kind, name] (const QString &err) {
        Slot &s = slot (kind);
        s.failed = true;
        s.failKind = s.worker->failureKind ();
        s.failText = err;
        s.failAfter = DeviceWorker::steadyNow () - s.connectStarted;
        s.failTimeout = s.worker->discoveryTimeout ();
        showMessage (QStringLiteral ("%1: %2").arg (name, err.section ('\n', 0, 0)), 10000, Theme::fault);
        updateSlotUi (s);
        // A modal only for errors that need the user to act outside this app.
        if (s.failKind == DeviceWorker::FailPortBusy || s.failKind == DeviceWorker::FailNoSend)
            showErrorDialog (s);
    });

    connect (w, &DeviceWorker::disconnected, this, [this, kind] (const QString &info) {
        if (info == QStringLiteral ("cancelled"))
            showMessage (QStringLiteral ("%1 connection cancelled").arg (shortDeviceName (kind)));
    });

    connect (w, &DeviceWorker::finished, this, [this, kind] () {
        Slot &s = slot (kind);
        s.worker->waitForFinished (5000); // thread already done: joins immediately
        s.stopRequested = false;
        s.stalled = s.held = s.pending = false;
        s.streamingSinceWall = kNaN;
        if (!cyton_.worker->isActive () && !emotibit_.worker->isActive ())
            sessionStartWall_ = kNaN;
        updateSlotUi (cyton_);
        updateSlotUi (emotibit_);
        updateConnectUi ();
        updateRecordUi ();
        refreshChrome (wallClockSeconds ());
        if (closing_ && !anyActive ())
            QTimer::singleShot (0, this, [this] {
                close ();
                QCoreApplication::quit (); // the window may already be hidden
            });
    });

    connect (w, &DeviceWorker::recordingStarted, this, [this] (const QStringList &files) {
        if (!std::isfinite (recStartWall_))
        {
            recStartWall_ = wallClockSeconds ();
            recFiles_.clear ();
            recBytes_ = 0.0;
        }
        for (const QString &f : files)
            if (!recFiles_.contains (f))
                recFiles_ << f;
        showMessage (QStringLiteral ("Recording to %1").arg (homeAbbrev (files.value (0))), 6000);
        updateRecordUi ();
    });

    connect (w, &DeviceWorker::recordingStopped, this, [this] (const QStringList &) {
        if (!isRecording ())
        {
            pollRecordingSizes (); // final sizes (the CPU meter stays on its 1 s cadence)
            recStartWall_ = kNaN;
        }
        updateRecordUi ();
    });

    connect (w, &DeviceWorker::message, this, [this, name] (const QString &m) {
        showMessage (QStringLiteral ("%1: %2").arg (name, m), 10000);
    });
}

bool MainWindow::isStreaming (DeviceKind kind) const
{
    return slot (kind).worker->state () == DeviceWorker::Streaming;
}

bool MainWindow::anyActive () const
{
    return cyton_.worker->isActive () || emotibit_.worker->isActive ();
}

bool MainWindow::anyConnecting () const
{
    for (const Slot *s : {&cyton_, &emotibit_})
        if (s->worker->isActive () && s->worker->state () == DeviceWorker::Connecting && !s->stopRequested)
            return true;
    return false;
}

bool MainWindow::anyStreaming () const
{
    for (const Slot *s : {&cyton_, &emotibit_})
        if (s->worker->isActive () && s->worker->state () == DeviceWorker::Streaming && !s->stopRequested)
            return true;
    return false;
}

bool MainWindow::isRecording () const
{
    return cyton_.worker->isRecording () || emotibit_.worker->isRecording ();
}

void MainWindow::onConnectButton ()
{
    if (anyConnecting ())
        cancelPending (); // [ cancel ]: the attempts still in progress
    else if (anyStreaming ())
        disconnectAll (); // [ disconnect ]: everything
    else if (!anyActive ())
        connectAll (); // [ connect ]
}

void MainWindow::connectAll ()
{
    connectAllWith (synthToggle_->isChecked (), synthToggle_->isChecked ());
}

void MainWindow::connectAllWith (bool cytonSynthetic, bool emotibitSynthetic)
{
    if (closing_ || anyActive ())
        return;
    if (errorDialog_)
        errorDialog_->close ();
    connectDeviceWith (DeviceKind::Cyton, cytonSynthetic);
    connectDeviceWith (DeviceKind::EmotiBit, emotibitSynthetic);
    updateConnectUi ();
    refreshChrome (wallClockSeconds ());
}

void MainWindow::cancelPending ()
{
    for (Slot *s : {&cyton_, &emotibit_})
        if (s->worker->isActive () && s->worker->state () == DeviceWorker::Connecting && !s->stopRequested)
            disconnectDevice (s->kind);
    updateConnectUi ();
}

void MainWindow::disconnectAll ()
{
    for (Slot *s : {&cyton_, &emotibit_})
        if (s->worker->isActive () && !s->stopRequested)
            disconnectDevice (s->kind);
    updateConnectUi ();
}

// The Cyton dongle's serial port for a connect, as BrainFlow takes it ("COM3",
// "/dev/cu.usbserial-..."): the override if that port exists, in the OS
// spelling ("com12" -> "COM12" on Windows, see existingSerialPort); otherwise
// (auto) the last port that connected if it is a detected OpenBCI dongle, else
// the first dongle found (SerialPorts: FTDI 0403:6015). "" = no dongle.
// Enumeration only reads the OS device registry; no port is opened.
QString MainWindow::cytonPortFor (const QString &override) const
{
    if (!override.isEmpty ())
        return existingSerialPort (override);
    return pickCytonDongle (listSerialPorts (), opts_.cytonPort);
}

QString MainWindow::portOverride () const
{
    const QString t = portCombo_ ? portCombo_->currentText ().trimmed () : portOverride_;
    return isAuto (t) ? QString () : t;
}

bool MainWindow::bluetoothUsable () const
{
    if (opts_.emotibitLink == EmotiBitLink::WiFi || (opts_.emotibitLink == EmotiBitLink::Auto && opts_.brainflowDiscovery))
        return false;
    const bluetooth_access::Status st = bluetooth_access::check ();
    return st == bluetooth_access::Status::Granted || st == bluetooth_access::Status::Undetermined;
}

void MainWindow::connectDeviceWith (DeviceKind kind, bool synth)
{
    Slot &s = slot (kind);
    if (closing_ || s.worker->isActive ())
        return;

    DeviceConfig cfg;
    cfg.slot = deviceSlotName (kind);
    cfg.displayName = deviceDisplayName (kind);
    cfg.boardId = synth ? static_cast<int> (BoardIds::SYNTHETIC_BOARD) : realBoardIdFor (kind);
    s.synthetic = synth;
    s.fieldBlank = false;
    s.address.clear ();
    s.serial.clear ();
    s.typedIp.clear ();
    s.targets.clear ();
    s.bluetooth = false;
    s.bluetoothName.clear ();
    s.failed = false;
    s.streamingSinceWall = kNaN;
    auto failNow = [&] (int failKind, const QString &text) {
        s.failed = true;
        s.failKind = failKind;
        s.failText = text;
        s.failAfter = 0.0;
        for (const SignalDef &d : signalDefsFor (kind))
        {
            int lane = 0;
            if (PlotWidget *p = plotForKey (d.key, &lane))
                p->setSource (lane, nullptr, -1, 0.0, 2);
        }
        updateSlotUi (s);
    };
    if (synth)
    {
        // distinct params -> distinct BrainFlow session keys for the two slots
        cfg.params.other_info = "bioacq:" + cfg.slot;
        s.progressText = QStringLiteral ("opening the BrainFlow synthetic board…");
    }
    else if (kind == DeviceKind::Cyton)
    {
        const QString over = portOverride ();
        const QString port = cytonPortFor (over);
        if (port.isEmpty ())
        {
            // not available: marked inline (chip + detail), never a dialog
            failNow (DeviceWorker::FailNotFound, over.isEmpty () ? QStringLiteral ("no dongle")
                                                                 : QStringLiteral ("no device at %1").arg (over));
            return;
        }
        cfg.params.serial_port = port.toStdString ();
        s.address = port;
        s.progressText = QStringLiteral ("opening %1 · soft reset takes a few s…").arg (port);
    }
    else
    {
        const QString field = ipEdit_->text ().trimmed ();
        if (!field.isEmpty () && !validIpv4 (field))
        {
            failNow (DeviceWorker::FailOther, QStringLiteral ("'%1' is not an IPv4 address").arg (field));
            return;
        }
        // The typed IP (blank field: the last EmotiBit that answered) gets a
        // short unicast try, which works across subnets; then broadcast
        // discovery on every interface, so an EmotiBit that joined another
        // network both share (e.g. a hotspot) is found at its new address.
        QString target = field;
        s.fieldBlank = field.isEmpty ();
        cfg.ownDiscovery = !opts_.brainflowDiscovery;
        if (s.fieldBlank && validIpv4 (lastEmotibitIp_) && cfg.ownDiscovery)
            target = lastEmotibitIp_;
        cfg.broadcastFallback = cfg.ownDiscovery && !target.isEmpty ();
        cfg.params.ip_address = target.toStdString ();
        cfg.params.timeout = timeoutSpin_->value ();
        s.address = target;
        s.typedIp = target;
        QStringList bc;
        for (const std::string &a : emotibit::ipv4BroadcastAddresses ())
            bc << QString::fromStdString (a);
        s.targets = target.isEmpty () ? (bc.isEmpty () ? QStringLiteral ("no interface") : bc.join (QStringLiteral (", ")))
                                      : target;
        s.progressText = cfg.ownDiscovery ? QString ()
                                          : QStringLiteral ("BrainFlow is discovering the EmotiBit (holds its lock)…");
        // Bluetooth (the bioacq BLE firmware): scanned alongside the Wi-Fi search.
        // An unanswered permission prompt does not hold the Wi-Fi search up.
        cfg.emotiLink = opts_.emotibitLink;
        if (cfg.emotiLink == EmotiBitLink::Bluetooth || (cfg.emotiLink == EmotiBitLink::Auto && cfg.ownDiscovery))
        {
            QString why;
            const bluetooth_access::Status bt = bluetooth_access::check (&why);
            cfg.bluetoothAllowed = bt == bluetooth_access::Status::Granted;
            cfg.bluetoothPending = bt == bluetooth_access::Status::Undetermined;
            // denied: the failure text tells how to allow it. Unavailable (a
            // development binary started from a terminal): Wi-Fi only, silently.
            if (bt == bluetooth_access::Status::Denied || cfg.emotiLink == EmotiBitLink::Bluetooth)
                cfg.bluetoothWhy = why;
            cfg.bluetoothName = lastBluetoothName_;
        }
    }
    cfg.countPackageGaps = kind == DeviceKind::Cyton;
    if (kind == DeviceKind::EmotiBit && opts_.testFreezeEmotibitSec >= 0.0)
        cfg.testFreezeAfterSec = opts_.testFreezeEmotibitSec;
    if (kind == DeviceKind::EmotiBit && opts_.testPpgBpm > 0.0)
        cfg.testPpgBpm = opts_.testPpgBpm;
    if (kind == DeviceKind::Cyton && opts_.testRailOffsetUv != 0.0)
        cfg.testRawOffset = opts_.testRailOffsetUv;
    if (kind == DeviceKind::Cyton && opts_.testCytonPrepareDelayMs > 0)
        cfg.testPrepareDelayMs = opts_.testCytonPrepareDelayMs;

    const std::vector<SignalDef> defs = signalDefsFor (kind);
    std::vector<std::string> problems;
    cfg.signalSpecs = resolveSignals (cfg.boardId, defs, &problems);
    if (cfg.signalSpecs.empty ())
    {
        failNow (DeviceWorker::FailOther, QStringLiteral ("Cannot map any signal on this board."));
        return;
    }
    cfg.record = recordWanted_;
    cfg.recordDir = opts_.recordDir.toStdString ();
    cfg.filePrefix = cfg.slot + (synth ? "_synthetic" : "");
    cfg.stamp = QDateTime::currentDateTime ().toString (QStringLiteral ("yyyyMMdd_HHmmss")).toStdString ();
    cfg.pollIntervalMs = kind == DeviceKind::Cyton ? 10 : 15;

    applyFilterSettings ();
    s.connectStarted = DeviceWorker::steadyNow ();
    if (!s.worker->start (cfg))
        return;
    if (cfg.bluetoothPending)
        bluetooth_access::request (this, [this, kind] (bluetooth_access::Status st) {
            QString why;
            if (st != bluetooth_access::Status::Granted)
                bluetooth_access::check (&why);
            slot (kind).worker->setBluetoothPermission (st == bluetooth_access::Status::Granted, why);
        });
    s.stopRequested = false;
    s.stalled = s.held = s.pending = false;

    // Plots of this device that the board cannot provide
    for (const SignalDef &d : defs)
    {
        int lane = 0;
        if (PlotWidget *p = plotForKey (d.key, &lane))
        {
            p->setSource (lane, nullptr, -1, 0.0, 2);
            p->setNote (QString ());
            p->setPlaceholder (QStringLiteral ("NOT PROVIDED BY THIS BOARD"));
        }
    }

    s.plots.clear ();
    s.lanes.clear ();
    s.meters.clear ();
    std::vector<std::pair<PlotWidget *, QStringList>> notes;
    std::vector<PlotWidget *> flagged;
    for (const SignalChannel &ch : s.worker->channels ())
    {
        int lane = 0;
        PlotWidget *p = plotForKey (ch.spec.key, &lane);
        s.plots.push_back (p);
        s.lanes.push_back (lane);
        s.meters.emplace_back (2.0);
        if (!p)
            continue;
        // heart rate: ~1 sample per beat (the rate only sizes gaps / window margins)
        p->setSource (lane, ch.ring, ch.rawChannel, ch.spec.derived ? 1.0 : ch.spec.nominalRate, ch.spec.valueDecimals);
        p->setLaneUnits (lane, QString::fromStdString (ch.spec.units));
        // Source in the plot's tooltip; a REAL channel resolved through a
        // fallback is also flagged in the header (SUBST). The synthetic
        // board's stand-ins are expected: the session reads "synthetic".
        auto it = std::find_if (notes.begin (), notes.end (), [p] (const auto &n) { return n.first == p; });
        if (it == notes.end ())
        {
            notes.push_back ({p, QStringList ()});
            it = notes.end () - 1;
        }
        it->second << QStringLiteral ("%1: %2").arg (QString::fromStdString (ch.spec.title),
            QString::fromStdString (ch.spec.source));
        if (!synth && ch.spec.substituted)
            flagged.push_back (p);
        p->setPlaceholder (QStringLiteral ("AWAITING STREAM"));
    }
    for (const auto &n : notes)
        n.first->setNote (QStringLiteral ("source · ") + n.second.join (QStringLiteral ("\nsource · ")),
            std::find (flagged.begin (), flagged.end (), n.first) != flagged.end ());
    if (kind == DeviceKind::Cyton)
        cytonPlot_->setSubtitle (synth ? QStringLiteral ("synthetic board · exg[0]")
                                       : QStringLiteral ("single-ended · SRB / AGND / N1P"));
    updateSlotUi (s);
    refreshChrome (wallClockSeconds ());
}

void MainWindow::disconnectDevice (DeviceKind kind)
{
    Slot &s = slot (kind);
    if (!s.worker->isActive ())
        return;
    const bool connecting = s.worker->state () == DeviceWorker::Connecting;
    s.stopRequested = true;
    s.worker->requestStop ();
    s.progressText = connecting ? (s.worker->inBrainFlowSetup ()
                                          ? QStringLiteral ("cancelling… waiting for BrainFlow to finish opening")
                                          : QStringLiteral ("cancelling…"))
                                : QStringLiteral ("stopping the stream, releasing the session…");
    updateSlotUi (s);
}

void MainWindow::setRecording (bool on)
{
    if (on == recordWanted_)
        return;
    recordWanted_ = on;
    const std::string dir = opts_.recordDir.toStdString ();
    const std::string stamp =
        QDateTime::currentDateTime ().toString (QStringLiteral ("yyyyMMdd_HHmmss")).toStdString ();
    if (on)
    {
        recStartWall_ = kNaN;
        recFiles_.clear ();
        recBytes_ = 0.0;
    }
    for (Slot *s : {&cyton_, &emotibit_})
        if (s->worker->isActive ())
            s->worker->requestRecording (on, dir, stamp);
    if (!on)
        showMessage (QStringLiteral ("Recording stopped"), 5000);
    else if (!anyActive ())
        showMessage (QStringLiteral ("Recording armed: starts when a device connects"), 6000, Theme::warn);
    updateRecordUi ();
}

// =============================================================== UI state
void MainWindow::updateConnectUi ()
{
    QString text, hint;
    const char *variant = "primary";
    bool enabled = !closing_;
    if (anyConnecting ())
    {
        text = QStringLiteral ("[ cancel ]");
        variant = "secondary";
        hint = QStringLiteral ("Cancel the connection attempts still in progress (%1).\n"
                               "Devices that already stream keep streaming.")
                   .arg (keyText (kConnectKey));
    }
    else if (anyStreaming ())
    {
        text = QStringLiteral ("[ disconnect ]");
        variant = "secondary";
        hint = QStringLiteral ("Stop every device and release the sessions.");
    }
    else if (anyActive ())
    {
        text = QStringLiteral ("[ stopping… ]");
        variant = "busy";
        enabled = false;
        hint = QStringLiteral ("Releasing the devices…");
    }
    else
    {
        text = QStringLiteral ("[ connect ]");
        hint = QStringLiteral ("Connect every available device at once (%1): the Cyton on its USB dongle and the "
                               "EmotiBit over WiFi.\nClose the OpenBCI GUI and EmotiBit Oscilloscope first: only one "
                               "program can own each device.")
                   .arg (keyText (kConnectKey));
    }
    if (connectBtn_->text () != text)
        connectBtn_->setText (text);
    setVariant (connectBtn_, variant);
    connectBtn_->setEnabled (enabled);
    if (connectBtn_->toolTip () != hint)
        connectBtn_->setToolTip (hint);
}

QString MainWindow::metaText (const Slot &s) const
{
    const bool active = s.worker->isActive ();
    const DeviceWorker::State st = s.worker->state ();
    const bool cy = s.kind == DeviceKind::Cyton;
    if (!active)
    {
        if (s.failed)
        {
            if (cy && s.failKind == DeviceWorker::FailNotFound)
                return s.failText.startsWith (QStringLiteral ("no dongle"))
                    ? QStringLiteral ("no dongle on USB · plug it in, then connect")
                    : s.failText.section ('\n', 0, 0);
            if (cy && s.failKind == DeviceWorker::FailPortBusy)
                return QStringLiteral ("%1 busy or board silent").arg (s.address);
            const QString bt = cy ? QString () : bluetoothReason (s.failText);
            if (!cy && s.failKind == DeviceWorker::FailNotFound && !bt.isEmpty ())
                return bt.startsWith (QLatin1String ("no EmotiBit advertising"))
                    ? QStringLiteral ("no answer over bluetooth or wi-fi")
                    : QStringLiteral ("not on wi-fi · bluetooth: %1").arg (bt);
            if (!cy && s.failKind == DeviceWorker::FailNotFound)
                return s.typedIp.isEmpty () || s.fieldBlank
                    ? QStringLiteral ("no answer%1 · broadcast failed")
                          .arg (s.typedIp.isEmpty () ? QString () : QStringLiteral (" at ") + s.typedIp)
                    : QStringLiteral ("no answer at %1 within %2 s").arg (s.typedIp).arg (s.failTimeout, 0, 'f', 0);
            if (!cy && s.failKind == DeviceWorker::FailNoSend)
                return QStringLiteral ("discovery blocked · see the dialog");
            return s.failText.section ('\n', 0, 0);
        }
        const bool synth = synthToggle_ && synthToggle_->isChecked ();
        if (synth)
            return QStringLiteral ("synthetic board");
        if (cy)
        {
            const QString over = portOverride ();
            if (!over.isEmpty ())
                return QStringLiteral ("port %1").arg (over);
            return detectedPort_.isEmpty () ? QStringLiteral ("auto · no dongle detected")
                                            : QStringLiteral ("auto · %1").arg (detectedPort_);
        }
        const bool ble = bluetoothUsable ();
        if (opts_.emotibitLink == EmotiBitLink::Bluetooth)
            return ble ? QStringLiteral ("bluetooth only") : QStringLiteral ("bluetooth only · not available here");
        const QString plus = ble ? QStringLiteral ("ble + ") : QString ();
        const QString field = ipEdit_->text ().trimmed ();
        if (!field.isEmpty ())
            return validIpv4 (field) ? QStringLiteral ("%1%2, then broadcast").arg (plus, field)
                                     : QStringLiteral ("not an IPv4 address");
        return validIpv4 (lastEmotibitIp_) ? QStringLiteral ("auto · %1%2, then broadcast").arg (plus, lastEmotibitIp_)
                                           : QStringLiteral ("auto · %1broadcast (same subnet)").arg (plus);
    }
    if (st == DeviceWorker::Streaming && !s.stopRequested)
    {
        QStringList t;
        if (s.synthetic)
            t << QStringLiteral ("synthetic");
        else if (s.bluetooth)
        {
            const DeviceWorker::BluetoothStage bt = s.worker->bluetoothStage ();
            t << (bt == DeviceWorker::BtLost          ? QStringLiteral ("bluetooth link lost · reconnecting")
                     : bt == DeviceWorker::BtFailed ? QStringLiteral ("bluetooth link failed")
                                                    : QStringLiteral ("bluetooth"));
        }
        else
            t << s.address;
        if (!s.synthetic && !s.serial.isEmpty ())
            t << s.serial;
        if (cy)
        {
            const auto &ch = s.worker->channels ();
            if (!ch.empty ())
                t << QStringLiteral ("%1 Hz").arg (ch.front ().spec.nominalRate, 0, 'g', 4);
        }
        t << QStringLiteral ("linked %1 s").arg (s.linkSeconds, 0, 'f', 1);
        return t.join (QStringLiteral (" · "));
    }
    return s.progressText;
}

void MainWindow::updateSlotUi (Slot &s)
{
    const bool active = s.worker->isActive ();
    const DeviceWorker::State st = s.worker->state ();

    // status chip
    QString chipText = QStringLiteral ("OFFLINE");
    QColor chipC = Theme::textDim;
    if (!active)
    {
        if (s.failed)
        {
            chipText = s.failKind == DeviceWorker::FailNotFound ? QStringLiteral ("NOT FOUND") : QStringLiteral ("FAILED");
            chipC = Theme::fault;
        }
    }
    else if (st == DeviceWorker::Streaming && !s.stopRequested)
    {
        if (s.held)
        {
            chipText = QStringLiteral ("HELD");
            chipC = Theme::warn;
        }
        else if (s.stalled)
        {
            chipText = QStringLiteral ("STALLED");
            chipC = Theme::warn;
        }
        else
        {
            chipText = QStringLiteral ("STREAMING");
            chipC = Theme::ok;
        }
    }
    else if (s.stopRequested || st == DeviceWorker::Stopping)
    {
        chipText = QStringLiteral ("STOPPING");
        chipC = Theme::warn;
    }
    else
    {
        chipText = QStringLiteral ("CONNECTING");
        chipC = Theme::warn;
    }
    s.chip->setState (chipText, chipC);

    // Inputs are locked while the device is in use: read-only and inert, but
    // drawn at rest (the design's live screen shows them undimmed).
    const bool editable = !active && !closing_;
    if (s.kind == DeviceKind::Cyton)
    {
        portCombo_->setLocked (!editable);
        refreshBtn_->setLocked (!editable);
    }
    else
    {
        ipEdit_->setReadOnly (!editable);
        timeoutSpin_->setReadOnly (!editable);
        wifiBtn_->setEnabled (editable);
        const QString wifiTip = editable
            ? QStringLiteral ("Add or remove the Wi-Fi networks the EmotiBit joins, over USB (no SD card needed).")
            : QStringLiteral ("Disconnect the EmotiBit first: Wi-Fi setup restarts it.");
        if (wifiBtn_->toolTip () != wifiTip)
            wifiBtn_->setToolTip (wifiTip);
        const bool nf = s.failed && s.failKind == DeviceWorker::FailNotFound;
        setFlag (ipEdit_, "attention", nf && editable);
        updateDiscoverBox (s);
    }
    if (synthToggle_)
        synthToggle_->setEnabled (!anyActive () && !closing_);

    const QString meta = metaText (s);
    if (s.meta->text () != meta)
        s.meta->setText (meta);
    Theme::setTextColor (s.meta, s.failed && !active ? Theme::fault : Theme::textDim);
    const bool discovering = s.kind == DeviceKind::EmotiBit && discoverWrap_->isVisibleTo (discoverWrap_->parentWidget ());
    s.meta->setVisible (!meta.isEmpty () && !discovering);
    if (s.chip->toolTip () != meta)
        s.chip->setToolTip (meta);
    if (s.meta->toolTip () != meta)
        s.meta->setToolTip (meta);
    updateErrorBox (s);
}

void MainWindow::updateDiscoverBox (Slot &s)
{
    const bool active = s.worker->isActive ();
    const bool show = active && s.worker->state () == DeviceWorker::Connecting && !s.synthetic;
    discoverWrap_->setVisible (show);
    if (!show)
        return;
    const DeviceWorker::Phase ph = s.worker->phase ();
    const double since = ph == DeviceWorker::PhaseIdle ? s.connectStarted : s.worker->phaseSince ();
    const double el = std::max (0.0, DeviceWorker::steadyNow () - since);
    QString title, time, detail;
    double frac = kNaN;
    if (s.stopRequested)
    {
        title = QStringLiteral ("CANCELLING…");
        detail = s.progressText;
    }
    else if (ph == DeviceWorker::PhaseIdle || ph == DeviceWorker::PhaseDiscovering)
    {
        const double to = s.worker->discoveryTimeout ();
        title = QStringLiteral ("DISCOVERING…");
        time = QStringLiteral ("%1 / %2 s").arg (el, 0, 'f', 1).arg (to, 0, 'f', 1);
        frac = to > 0.0 ? el / to : kNaN;
        const int n = s.worker->probesSent ();
        const QString probes = n == 1 ? QStringLiteral ("1 probe sent") : QStringLiteral ("%1 probes sent").arg (n);
        const bool broadcast = s.typedIp.isEmpty () || s.progressText.contains (QStringLiteral ("broadcast discovery on"));
        detail = broadcast ? QStringLiteral ("Broadcast · %1").arg (probes)
                           : QStringLiteral ("Unicast to %1 · %2").arg (s.typedIp, probes);
        if (broadcast && !s.typedIp.isEmpty ())
            detail = QStringLiteral ("No answer at %1 · broadcast · %2").arg (s.typedIp, probes);
        switch (s.worker->bluetoothStage ())
        {
            case DeviceWorker::BtPermission:
                detail += QStringLiteral (" · Bluetooth: allow access in the system prompt");
                break;
            case DeviceWorker::BtScanning:
                detail += QStringLiteral (" · Bluetooth scan");
                break;
            case DeviceWorker::BtConnecting:
            case DeviceWorker::BtLinked:
            case DeviceWorker::BtLost:
                detail += QStringLiteral (" · EmotiBit found over Bluetooth");
                break;
            case DeviceWorker::BtFailed:
                detail += QStringLiteral (" · no Bluetooth");
                break;
            case DeviceWorker::BtOff:
                break;
        }
    }
    else if (ph == DeviceWorker::PhaseBluetooth)
    {
        title = QStringLiteral ("BLUETOOTH…");
        time = QStringLiteral ("%1 s").arg (el, 0, 'f', 1);
        detail = s.progressText;
    }
    else
    {
        title = QStringLiteral ("OPENING SESSION…");
        time = QStringLiteral ("%1 s").arg (el, 0, 'f', 1);
        frac = 1.0;
        detail = s.progressText.isEmpty () ? QStringLiteral ("BrainFlow prepare_session…") : s.progressText;
    }
    discoverTitle_->setText (title);
    discoverTime_->setText (time);
    discoverBar_->setValue (frac, Theme::warn);
    if (discoverDetail_->text () != detail)
        discoverDetail_->setText (detail);
}

bool MainWindow::showsErrorBox (const Slot &s) const
{
    // A missing Cyton dongle is "not available", not an error: chip + detail only.
    return s.failed && !s.worker->isActive () && !(s.kind == DeviceKind::Cyton && s.failKind == DeviceWorker::FailNotFound);
}

void MainWindow::updateErrorBox (Slot &s)
{
    const bool show = showsErrorBox (s);
    s.errorWrap->setVisible (show);
    if (!show)
        return;
    QString title, text, hint;
    const QString first = s.failText.section ('\n', 0, 0).trimmed ();
    QString rest = s.failText.section ('\n', 1).trimmed ();
    const QString bt = s.kind == DeviceKind::EmotiBit ? bluetoothReason (s.failText) : QString ();
    if (s.kind == DeviceKind::EmotiBit && s.failKind == DeviceWorker::FailNotFound && !bt.isEmpty ())
    {
        title = QStringLiteral ("EMOTIBIT NOT FOUND");
        const QString wifi = s.typedIp.isEmpty () ? QStringLiteral ("broadcast on %1").arg (subnets ().toHtmlEscaped ())
                                                  : QStringLiteral ("%1, then broadcast on %2")
                                                        .arg (b (s.typedIp, Theme::textStrong), subnets ().toHtmlEscaped ());
        if (bt.startsWith (QLatin1String ("no EmotiBit advertising")))
        {
            text = QStringLiteral ("No EmotiBit is advertising over %1, and none answered on Wi-Fi (%2).")
                       .arg (b (QStringLiteral ("Bluetooth"), Theme::textStrong), wifi);
            hint = QStringLiteral ("Switch the EmotiBit on and keep it near this computer; it starts in Bluetooth mode "
                                   "unless its button is held at start-up. Close EmotiBit Oscilloscope. In Wi-Fi mode it "
                                   "must share a network with this computer (%1, over USB).")
                       .arg (b (QStringLiteral ("wi-fi setup"), Theme::textBody));
        }
        else
        {
            text = QStringLiteral ("Bluetooth: %1. No EmotiBit answered on Wi-Fi (%2).").arg (bt.toHtmlEscaped (), wifi);
            hint = QStringLiteral ("Fix Bluetooth and connect again, or start the EmotiBit in Wi-Fi mode (button held at "
                                   "start-up) on a network this computer shares (%1).")
                       .arg (b (QStringLiteral ("wi-fi setup"), Theme::textBody));
        }
    }
    else if (s.kind == DeviceKind::EmotiBit && s.failKind == DeviceWorker::FailNotFound)
    {
        title = QStringLiteral ("EMOTIBIT NOT FOUND");
        if (s.fieldBlank || s.typedIp.isEmpty ())
        {
            text = QStringLiteral ("Auto-discovery is a UDP broadcast — it only reaches devices on the %1 as "
                                   "this computer. This computer is on %2.")
                       .arg (b (QStringLiteral ("same subnet"), Theme::textStrong), subnets ().toHtmlEscaped ());
            hint = QStringLiteral ("Enter the EmotiBit's address in the %1 field above (it is printed on the serial "
                                   "monitor at boot), then connect again.")
                       .arg (b (QStringLiteral ("IP address"), Theme::textBody));
        }
        else
        {
            if (opts_.brainflowDiscovery)
                text = QStringLiteral ("No answer from %1 within %2&nbsp;s (unicast, across subnets). This computer is on %3.")
                           .arg (b (s.typedIp, Theme::textStrong))
                           .arg (s.failTimeout, 0, 'f', 1)
                           .arg (subnets ().toHtmlEscaped ());
            else
                text = QStringLiteral ("No answer from %1, and broadcast discovery found no EmotiBit on this "
                                       "computer's network (%2).")
                           .arg (b (s.typedIp, Theme::textStrong), subnets ().toHtmlEscaped ());
            hint = QStringLiteral ("Check that the EmotiBit is on and has joined a network this computer is on, and "
                                   "that EmotiBit Oscilloscope is closed. To use this network, add it to the EmotiBit "
                                   "with %1 (USB).")
                       .arg (b (QStringLiteral ("wi-fi setup"), Theme::textBody));
        }
    }
    else
    {
        title = s.failKind == DeviceWorker::FailNoSend
            ? QStringLiteral ("DISCOVERY BLOCKED")
            : (s.failKind == DeviceWorker::FailPortBusy ? QStringLiteral ("CYTON PORT BUSY") : QStringLiteral ("CONNECTION FAILED"));
        text = first.toHtmlEscaped ();
        hint = rest.isEmpty () ? QStringLiteral ("Check the device and connect again.")
                               : rest.toHtmlEscaped ().replace ('\n', QStringLiteral ("<br>"));
    }
    s.errorTitle->setText (title);
    const QString t = richPara (text, 17), h = richPara (hint, 17); // 11.5 px x 1.5
    if (s.errorText->text () != t)
        s.errorText->setText (t);
    if (s.errorHint->text () != h)
        s.errorHint->setText (h);
}

void MainWindow::showErrorDialog (const Slot &s)
{
    if (!opts_.interactive || closing_)
        return;
    if (errorDialog_)
        errorDialog_->close ();
    const bool cy = s.kind == DeviceKind::Cyton;
    auto *mb = new QMessageBox (QMessageBox::Warning,
        cy ? QStringLiteral ("Cyton port busy") : QStringLiteral ("EmotiBit discovery blocked"),
        cy ? QStringLiteral ("The Cyton on %1 could not be opened.").arg (s.address)
           : QStringLiteral ("The EmotiBit discovery packet could not be sent."),
        QMessageBox::Ok, this);
    mb->setInformativeText (s.failText);
    mb->setAttribute (Qt::WA_DeleteOnClose);
    mb->setWindowModality (Qt::WindowModal);
    errorDialog_ = mb;
    mb->open ();
}

void MainWindow::updateStreamHealth (Slot &s, double now)
{
    const bool active = s.worker->isActive ();
    const bool streaming = s.worker->state () == DeviceWorker::Streaming && !s.stopRequested;
    const Slot &o = otherSlot (s.kind);
    const bool heldNow = o.worker->inBrainFlowSetup ();
    const auto &chans = s.worker->channels ();
    // A channel that never delivered a sample is measured from the moment the
    // device reached Streaming, with a first-sample grace (EmotiBit: the
    // worker skips leading all-zero samples for up to 3 s).
    const double sinceStream =
        std::isfinite (s.streamingSinceWall) ? std::max (0.0, now - s.streamingSinceWall) : kNaN;
    const double grace =
        s.kind == DeviceKind::Cyton ? Readouts::kFirstSampleGraceCyton : Readouts::kFirstSampleGraceEmotibit;
    int stale = 0, never = 0, total = 0, pending = 0;
    double minAge = std::numeric_limits<double>::infinity ();
    s.rateSum = 0.0;
    s.lowRate.clear ();
    s.partialText.clear ();
    const std::size_t nch = std::min ({chans.size (), s.meters.size (), s.plots.size ()});

    // Several channels can share one plot (the IMU's lanes): collect per plot,
    // apply once. Rank: the worst state wins (stale > low > ok > none).
    struct PlotState
    {
        PlotWidget *plot = nullptr;
        int rateRank = -1;
        QString rate;
        QColor rateColor;
        int tone = 0; // 0 off, 1 normal, 2 warn
        bool ledWarn = false;
        QString stall;
    };
    std::vector<PlotState> states;
    auto stateFor = [&states] (PlotWidget *p) -> PlotState & {
        for (PlotState &ps : states)
            if (ps.plot == p)
                return ps;
        states.push_back (PlotState ());
        states.back ().plot = p;
        return states.back ();
    };

    for (std::size_t i = 0; i < nch; ++i)
    {
        if (chans[i].spec.derived)
            continue; // heart rate: updateHeartRate()
        RateMeter &m = s.meters[i];
        const std::uint64_t count = chans[i].ring->totalWritten ();
        if (active)
            m.sample (now, count);
        const bool have = count > 0;
        const double rate = m.rate ();
        const double silent = Readouts::silentSeconds (have, m.secondsSinceChange (now), sinceStream);
        const bool isStale = Readouts::isStalled (streaming, silent, Readouts::stallThreshold (have, grace));
        const double ts = chans[i].ring->latestTimestamp ();
        const double age =
            (have && std::isfinite (ts) && std::fabs (now - ts) < 3600.0) ? std::max (0.0, now - ts) : silent;
        ++total;
        if (streaming)
            s.rateSum += rate;
        if (isStale)
        {
            ++stale;
            if (!have)
                ++never;
            if (std::isfinite (age))
                minAge = std::min (minAge, age);
        }
        else if (streaming && !have)
            ++pending;
        PlotWidget *p = s.plots[i];
        if (!p)
            continue;
        const double nominal = chans[i].spec.nominalRate;
        if (!streaming)
        {
            p->setRate (Readouts::rateText (false, 0.0, nominal), Theme::textDim);
            p->setTone (PlotWidget::Tone::Off);
            p->setLed (active ? Theme::warn : (s.failed ? Theme::fault : Theme::textDim));
            p->setPlaceholder (active ? QStringLiteral ("AWAITING STREAM")
                                      : (s.failed ? QStringLiteral ("NO DATA") : QStringLiteral ("NO SIGNAL")));
            p->setStallText (QString ());
            continue;
        }
        p->setPlaceholder (QStringLiteral ("AWAITING STREAM"));
        PlotState &ps = stateFor (p);
        if (isStale)
        {
            // stalled: the measured rate is 0 by definition (the 2 s rate
            // window would otherwise still be decaying for another second)
            if (ps.rateRank < 3)
            {
                ps.rateRank = 3;
                ps.rate = Readouts::rateText (true, 0.0, nominal);
                ps.rateColor = Theme::warn;
            }
            QString text;
            if (heldNow)
                text = QStringLiteral ("HELD · BRAINFLOW OPENING %1").arg (upperName (o.kind));
            else if (have)
                text = QStringLiteral ("LAST SAMPLE %1 s AGO").arg (age, 0, 'f', 1);
            else
                text = QStringLiteral ("NO SAMPLES SINCE CONNECT · %1 s").arg (silent, 0, 'f', 1);
            if (ps.stall.isEmpty ())
                ps.stall = text;
            ps.tone = 2;
            ps.ledWarn = true;
            if (s.partialText.isEmpty ())
            {
                const QString title = QString::fromStdString (chans[i].spec.title);
                s.partialText = have ? QStringLiteral ("%1: last sample %2 s ago").arg (title).arg (age, 0, 'f', 1)
                                     : QStringLiteral ("%1: no samples since connect").arg (title);
            }
        }
        else
        {
            const bool haveRate = have && rate > 0.0;
            const Readouts::RateTone rt = Readouts::rateTone (haveRate, rate, nominal);
            const int rank = rt == Readouts::RateTone::None ? 0 : (rt == Readouts::RateTone::Low ? 2 : 1);
            if (rank > ps.rateRank)
            {
                ps.rateRank = rank;
                ps.rate = Readouts::rateText (haveRate, rate, nominal);
                ps.rateColor = rank == 0 ? Theme::textDim : (rank == 2 ? Theme::warn : Theme::textMuted);
            }
            if (rt == Readouts::RateTone::Low && s.lowRate.isEmpty ())
                s.lowRate = QStringLiteral ("%1 %2").arg (QString::fromStdString (chans[i].spec.title),
                    Readouts::rateText (true, rate, nominal));
            // The hero's LED stays the Cyton link state; near-rail is carried by
            // LATEST (amber), the headroom module and the banner.
            const bool warnHero = p == cytonPlot_ && nearRail_;
            ps.tone = std::max (ps.tone, !have ? 0 : (warnHero ? 2 : 1));
            ps.ledWarn = ps.ledWarn || !have;
        }
    }

    const bool allStale = streaming && total > 0 && stale == total;
    // A device-wide stall is labelled once, on the device's largest panel (PPG
    // green for the EmotiBit); a partial stall labels each stale plot.
    PlotWidget *labelPlot = nullptr;
    if (allStale && states.size () > 1)
    {
        labelPlot = states.back ().plot;
        for (const PlotState &ps : states)
            if (ps.plot == ppgGreenPlot_)
                labelPlot = ppgGreenPlot_;
    }
    for (const PlotState &ps : states)
    {
        ps.plot->setRate (ps.rate, ps.rateColor);
        ps.plot->setTone (ps.tone == 2 ? PlotWidget::Tone::Warn : (ps.tone == 1 ? PlotWidget::Tone::Normal : PlotWidget::Tone::Off));
        ps.plot->setLed (ps.ledWarn ? Theme::warn : Theme::ok);
        ps.plot->setStallText ((labelPlot && ps.plot != labelPlot) ? QString () : ps.stall);
    }
    const bool devStall = allStale && !heldNow;
    if (devStall && !s.stalled)
        s.stallSince = now - (std::isfinite (minAge) ? minAge : 0.0);
    s.stalled = devStall;
    s.held = allStale && heldNow;
    s.stallAge = std::isfinite (minAge) ? minAge : 0.0;
    s.staleCount = stale;
    s.neverCount = never;
    s.dataCount = total;
    s.pending = streaming && pending > 0;
}

void MainWindow::updateHeartRate (double now)
{
    const Slot &s = emotibit_;
    const bool active = s.worker->isActive ();
    const bool streaming = s.worker->state () == DeviceWorker::Streaming && !s.stopRequested;
    const SignalChannel *hr = nullptr;
    for (const SignalChannel &ch : s.worker->channels ())
        if (ch.spec.derived && ch.spec.key == SignalKeys::EmotiHeartRate)
            hr = &ch;
    if (!streaming || !hr)
    {
        hrPlot_->setTone (PlotWidget::Tone::Off);
        hrPlot_->setLed (active ? Theme::warn : (s.failed ? Theme::fault : Theme::textDim));
        hrPlot_->setValueText (QString ());
        hrPlot_->setStripChips ({{QStringLiteral ("SOURCE —"), false}, {QStringLiteral ("QUALITY —"), false}});
        hrPlot_->setPlaceholder (active ? QStringLiteral ("AWAITING STREAM")
                                        : (s.failed ? QStringLiteral ("NO DATA") : QStringLiteral ("NO SIGNAL")));
        return;
    }
    double ts = kNaN;
    std::vector<double> v;
    const bool have = hr->ring->latest (ts, v) && static_cast<int> (v.size ()) >= HeartRateRing::Count;
    const double age = have ? now - ts : kNaN;
    const double sinceStream = std::isfinite (s.streamingSinceWall) ? now - s.streamingSinceWall : 0.0;
    const Readouts::HeartRateView view = Readouts::heartRateView (true, have, age,
        have ? v[HeartRateRing::Bpm] : kNaN, have ? v[HeartRateRing::Quality] : 0.0,
        have ? static_cast<int> (v[HeartRateRing::Source]) : HeartRate::SourceNone,
        have ? static_cast<int> (v[HeartRateRing::Beats]) : 0, have ? v[HeartRateRing::Periodicity] : 0.0, sinceStream);
    hrPlot_->setValueText (view.value);
    hrPlot_->setTone (view.warn ? PlotWidget::Tone::Warn : PlotWidget::Tone::Normal);
    hrPlot_->setLed (view.valid ? Theme::ok : Theme::warn);
    hrPlot_->setStripChips (
        {{QStringLiteral ("SOURCE ") + view.source, view.valid}, {view.chip, view.valid, view.warn}});
    hrPlot_->setPlaceholder (view.warn ? QStringLiteral ("NO HEART RATE") : QStringLiteral ("ACQUIRING HEART RATE"));
}

double MainWindow::cytonRawPeak (double now)
{
    for (const SignalChannel &ch : cyton_.worker->channels ())
    {
        if (ch.spec.key != SignalKeys::CytonEcg || !ch.ring || ch.rawChannel < 0)
            continue;
        double ref = now;
        const double latest = ch.ring->latestTimestamp ();
        if (std::isfinite (latest) && std::fabs (latest - now) > 30.0)
            ref = latest; // timestamps far from the host clock: same anchor as the plot
        return Readouts::ringPeakAbs (*ch.ring, ch.rawChannel, ref, Readouts::kRailWindowSec, peakT_, peakV_);
    }
    return kNaN;
}

void MainWindow::updateHeadroom (double now)
{
    const bool streaming = isStreaming (DeviceKind::Cyton) && !cyton_.stopRequested;
    // Straight from the Cyton ring over the last 2 s, not from the plot: it
    // keeps following the live data while the display is paused, and clears
    // within ~2 s once the input recovers.
    rawPeak_ = streaming ? cytonRawPeak (now) : kNaN;
    headroom_ = Readouts::railHeadroom (rawPeak_);
    const bool near = streaming && Readouts::nearRail (headroom_, nearRail_);
    if (near && !nearRail_)
        railSinceWall_ = now;
    const bool changed = near != nearRail_;
    nearRail_ = near;
    cytonPlot_->setRailMode (near);
    if (changed)
        applyEcgChips ();
    if (!std::isfinite (headroom_))
    {
        railPct_->setText (QStringLiteral ("—"));
        Theme::setTextColor (railPct_, Theme::controlEdge);
        railBar_->setValue (kNaN, Theme::controlEdge);
        railNote_->setText (QStringLiteral ("No signal"));
        Theme::setTextColor (railNote_, Theme::textDim);
        return;
    }
    // colour follows the (hysteretic) near-rail state; the number is floored
    // so it never reads "10 %" while below the 10 % threshold
    const QColor c = near ? Theme::warn : Theme::ok;
    railPct_->setText (Readouts::saturated (rawPeak_) ? QStringLiteral ("SATURATED") : Readouts::headroomPercent (headroom_));
    Theme::setTextColor (railPct_, c);
    railBar_->setValue (headroom_, c);
    railNote_->setText (Readouts::saturated (rawPeak_)
            ? QStringLiteral ("At ±187,500 µV · check electrode contact")
            : (near ? QStringLiteral ("Near rail · check electrode contact") : peakText (rawPeak_)));
    Theme::setTextColor (railNote_, near ? Theme::warn : Theme::textDim);
}

void MainWindow::applyEcgChips ()
{
    if (nearRail_)
    {
        // raw near-rail view: none of the display filters apply
        cytonPlot_->setChips ({{QStringLiteral ("RAW"), true, true}});
        return;
    }
    const bool dc = dcToggle_->isChecked (), hp = hpToggle_->isChecked (), notch = notchToggle_->isChecked (),
               lp = lpToggle_->isChecked ();
    cytonPlot_->setChips ({{dc ? QStringLiteral ("DC REMOVED") : QStringLiteral ("DC REMOVAL OFF"), dc},
        {hp ? QStringLiteral ("HP 0.5 Hz") : QStringLiteral ("HP 0.5 Hz OFF"), hp},
        {notch ? QStringLiteral ("NOTCH 60 Hz") : QStringLiteral ("NOTCH 60 Hz OFF"), notch},
        {lp ? QStringLiteral ("LP 40 Hz") : QStringLiteral ("LP 40 Hz OFF"), lp}});
}

void MainWindow::updateRecordUi ()
{
    const bool active = isRecording ();
    const double now = wallClockSeconds ();
    if (recordWanted_ && active)
    {
        recordBtn_->setMode (RecordButton::Mode::Recording, QStringLiteral ("[ stop recording ]"));
        const QString el = Readouts::elapsed (std::isfinite (recStartWall_) ? now - recStartWall_ : 0.0);
        recElapsed_->setText (el);
        Theme::setTextColor (recElapsed_, Theme::textStrong);
        recSize_->setText (Readouts::bytes (recBytes_));
        Theme::setTextColor (recSize_, Theme::textStrong);
        sessionBar_->setRecording (true, el);
    }
    else if (recordWanted_)
    {
        recordBtn_->setMode (RecordButton::Mode::Armed, QStringLiteral ("[ disarm recording ]"));
        recElapsed_->setText (QStringLiteral ("armed · starts on connect"));
        Theme::setTextColor (recElapsed_, Theme::warn);
        recSize_->setText (QStringLiteral ("—"));
        Theme::setTextColor (recSize_, Theme::textDim);
        sessionBar_->setRecording (false, QString ());
    }
    else
    {
        recordBtn_->setMode (RecordButton::Mode::Idle, QStringLiteral ("[ record to csv ]"));
        recElapsed_->setText (QStringLiteral ("—"));
        Theme::setTextColor (recElapsed_, Theme::textDim);
        recSize_->setText (QStringLiteral ("—"));
        Theme::setTextColor (recSize_, Theme::textDim);
        sessionBar_->setRecording (false, QString ());
    }
    recordBtn_->setEnabled (!closing_);
}

void MainWindow::refreshChrome (double now)
{
    // ---- session label (start time + devices actually streaming)
    QStringList devs;
    bool allSynth = true;
    for (const Slot *s : {&cyton_, &emotibit_})
        if (s->worker->state () == DeviceWorker::Streaming)
        {
            devs << (s->kind == DeviceKind::Cyton ? QStringLiteral ("ecg") : QStringLiteral ("emotibit"));
            allSynth = allSynth && s->synthetic;
        }
    if (std::isfinite (sessionStartWall_) && !devs.isEmpty ())
    {
        const QDateTime t = QDateTime::fromMSecsSinceEpoch (static_cast<qint64> (sessionStartWall_ * 1000.0));
        QString label = QStringLiteral ("session_%1 · %2")
                            .arg (t.toString (QStringLiteral ("yyyy-MM-dd_HHmm")), devs.join (QStringLiteral (" + ")));
        if (allSynth)
            label += QStringLiteral (" · synthetic");
        else if (cyton_.synthetic != emotibit_.synthetic && devs.size () == 2)
            label += cyton_.synthetic ? QStringLiteral (" · cyton synthetic") : QStringLiteral (" · emotibit synthetic");
        sessionBar_->setSession (label);
    }
    else if (anyActive ())
        sessionBar_->setSession (QStringLiteral ("connecting…"));
    else
        sessionBar_->setSession (QStringLiteral ("no active session"));

    // ---- idle call to action (nothing connecting or connected)
    const bool idle = !anyActive () && !closing_;
    if (idleOverlay_->isVisible () != idle)
    {
        idleOverlay_->setVisible (idle);
        cytonPlot_->update ();
    }

    // ---- status strip segments
    auto devSeg = [] (const Slot &s) {
        StatusStrip::Segment seg;
        const bool active = s.worker->isActive ();
        const DeviceWorker::State st = s.worker->state ();
        QString state = QStringLiteral ("OFFLINE");
        QColor c = Theme::textDim;
        if (!active && s.failed)
        {
            state = s.failKind == DeviceWorker::FailNotFound ? QStringLiteral ("NOT FOUND") : QStringLiteral ("FAILED");
            c = Theme::fault;
        }
        else if (active && st == DeviceWorker::Streaming && !s.stopRequested)
        {
            state = s.held ? QStringLiteral ("HELD") : (s.stalled ? QStringLiteral ("STALLED") : QStringLiteral ("STREAMING"));
            c = s.held || s.stalled ? Theme::warn : Theme::ok;
        }
        else if (active)
        {
            state = s.stopRequested || st == DeviceWorker::Stopping ? QStringLiteral ("STOPPING") : QStringLiteral ("CONNECTING");
            c = Theme::warn;
        }
        seg.text = upperName (s.kind) + QStringLiteral (" ") + state;
        seg.color = Theme::textMuted;
        seg.led = c;
        seg.trackingEm = 0.08;
        return seg;
    };
    QVector<StatusStrip::Segment> segs;
    segs << devSeg (cyton_) << devSeg (emotibit_);
    const bool cyOn = isStreaming (DeviceKind::Cyton), emOn = isStreaming (DeviceKind::EmotiBit);
    auto hz = [] (bool on, double r) { return on ? QString::number (r, 'f', 1) : QStringLiteral ("—"); };
    StatusStrip::Segment agg;
    agg.text = (!cyOn && !emOn) ? QStringLiteral ("AGGREGATE — Hz")
                                : QStringLiteral ("AGGREGATE %1 + %2 Hz").arg (hz (cyOn, cyton_.rateSum), hz (emOn, emotibit_.rateSum));
    agg.color = Theme::textDim;
    segs << agg;
    StatusStrip::Segment win;
    win.text = QStringLiteral ("WINDOW %1.0 s").arg (windowStepper_->value ());
    win.color = Theme::textDim;
    segs << win;
    StatusStrip::Segment rec;
    if (recordWanted_ && isRecording ())
    {
        rec.text = QStringLiteral ("REC %1 · %2")
                       .arg (Readouts::elapsed (std::isfinite (recStartWall_) ? now - recStartWall_ : 0.0),
                           Readouts::bytes (recBytes_));
        rec.color = Theme::fault;
    }
    else if (recordWanted_)
    {
        rec.text = QStringLiteral ("REC ARMED");
        rec.color = Theme::warn;
    }
    else
    {
        rec.text = QStringLiteral ("REC IDLE");
        rec.color = Theme::textDim;
    }
    segs << rec;
    statusStrip_->setSegments (segs);

    // DROPPED only once package numbers have actually been seen (not while
    // connecting / soft-resetting, when nothing has been measured yet)
    QString right;
    bool cyData = false;
    if (cyOn && !cyton_.stopRequested)
        for (const SignalChannel &ch : cyton_.worker->channels ())
            cyData = cyData || (ch.ring && ch.ring->totalWritten () > 0);
    if (cyton_.worker->countsDrops () && cyData)
        right = QStringLiteral ("CYTON DROPPED %1 · ").arg (cyton_.worker->droppedPackets ());
    right += std::isfinite (cpu_.percent ()) ? QStringLiteral ("CPU %1 %").arg (std::round (cpu_.percent ()), 0, 'f', 0)
                                             : QStringLiteral ("CPU — %");
    statusStrip_->setRight (right);

    // ---- derived message / banner
    // A missing Cyton dongle is only reported while nothing streams (the
    // EmotiBit may be the only device in use); every other failure is a fault.
    const Slot *failedSlot = nullptr;
    for (const Slot *s : {&emotibit_, &cyton_})
        if (s->failed && !s->worker->isActive () &&
            !(s->kind == DeviceKind::Cyton && s->failKind == DeviceWorker::FailNotFound && (cyOn || emOn || anyActive ())))
        {
            failedSlot = s;
            break;
        }
    const bool emDiscovering = emotibit_.worker->isActive () && emotibit_.worker->state () == DeviceWorker::Connecting &&
        !emotibit_.stopRequested && !emotibit_.synthetic;
    const bool cyConnecting = cyton_.worker->isActive () && cyton_.worker->state () == DeviceWorker::Connecting &&
        !cyton_.stopRequested;
    const Slot *stallSlot = emotibit_.stalled ? &emotibit_ : (cyton_.stalled ? &cyton_ : nullptr);
    const bool warn = stallSlot || nearRail_;
    if (warn && !std::isfinite (warnSinceWall_))
        warnSinceWall_ = stallSlot ? stallSlot->stallSince : railSinceWall_;
    if (!warn)
        warnSinceWall_ = kNaN;
    const bool saturated = nearRail_ && Readouts::saturated (rawPeak_);

    QString dmsg;
    QColor dcol = Theme::textMuted;
    if (closing_)
        dmsg = QStringLiteral ("Closing — stopping streams and releasing devices…");
    else if (warn)
    {
        QStringList parts;
        if (stallSlot)
            parts << QStringLiteral ("%1 stalled %2 s").arg (shortDeviceName (stallSlot->kind)).arg (stallSlot->stallAge, 0, 'f', 1);
        if (nearRail_)
            parts << (saturated ? QStringLiteral ("ECG saturated") : QStringLiteral ("ECG near rail"));
        dmsg = parts.join (QStringLiteral (" · "));
        dcol = Theme::warn;
    }
    else if (emDiscovering && emotibit_.worker->phase () == DeviceWorker::PhaseBluetooth)
    {
        dmsg = QStringLiteral ("%1Connecting EmotiBit over Bluetooth — %2 s")
                   .arg (cyConnecting ? QStringLiteral ("Connecting Cyton · ") : QString ())
                   .arg (std::max (0.0, DeviceWorker::steadyNow () - emotibit_.worker->phaseSince ()), 0, 'f', 1);
        dcol = Theme::warn;
    }
    else if (emDiscovering && emotibit_.worker->phase () != DeviceWorker::PhaseOpening)
    {
        const double el = std::max (0.0, DeviceWorker::steadyNow () -
                (emotibit_.worker->phase () == DeviceWorker::PhaseIdle ? emotibit_.connectStarted
                                                                         : emotibit_.worker->phaseSince ()));
        dmsg = QStringLiteral ("%1Discovering EmotiBit — %2 s of %3 s elapsed")
                   .arg (cyConnecting ? QStringLiteral ("Connecting Cyton · ") : QString ())
                   .arg (el, 0, 'f', 1)
                   .arg (emotibit_.worker->discoveryTimeout (), 0, 'f', 1);
        dcol = Theme::warn;
    }
    else if (failedSlot)
    {
        if (failedSlot->kind == DeviceKind::EmotiBit && failedSlot->failKind == DeviceWorker::FailNotFound &&
            !bluetoothReason (failedSlot->failText).isEmpty ())
            dmsg = QStringLiteral ("EmotiBit not found over Bluetooth or Wi-Fi");
        else if (failedSlot->kind == DeviceKind::EmotiBit && failedSlot->failKind == DeviceWorker::FailNotFound)
            dmsg = (failedSlot->fieldBlank || failedSlot->typedIp.isEmpty ())
                ? QStringLiteral ("EmotiBit not found on %1 — enter IP manually").arg (subnets ())
                : QStringLiteral ("EmotiBit not found at %1 — check the IP address").arg (failedSlot->typedIp);
        else if (failedSlot->kind == DeviceKind::Cyton && failedSlot->failKind == DeviceWorker::FailNotFound)
            dmsg = QStringLiteral ("Cyton not found — no dongle on USB");
        else
            dmsg = QStringLiteral ("%1 connection failed — see the left rail").arg (shortDeviceName (failedSlot->kind));
        dcol = Theme::fault;
    }
    else if (cyConnecting || emDiscovering)
    {
        dmsg = cyConnecting ? QStringLiteral ("Connecting Cyton…") : QStringLiteral ("Opening the EmotiBit session…");
        dcol = Theme::warn;
    }
    else if (recordWanted_ && isRecording ())
        dmsg = QStringLiteral ("Recording to %1").arg (homeAbbrev (recFiles_.value (0, opts_.recordDir)));
    else if (cyOn || emOn)
    {
        auto pick = [this] (auto pred) -> const Slot * {
            return pred (cyton_) ? &cyton_ : (pred (emotibit_) ? &emotibit_ : nullptr);
        };
        const Slot *low = pick ([] (const Slot &s) { return !s.lowRate.isEmpty (); });
        const Slot *held = pick ([] (const Slot &s) { return s.held; });
        const Slot *part = pick ([] (const Slot &s) { return s.staleCount > 0 && !s.stalled && !s.held; });
        const Slot *wait = pick ([] (const Slot &s) { return s.pending; });
        if (low)
        {
            dmsg = QStringLiteral ("%1 below nominal").arg (low->lowRate);
            dcol = Theme::warn;
        }
        else if (held)
        {
            dmsg = QStringLiteral ("%1 held while BrainFlow opens the %2 — samples are buffered")
                       .arg (shortDeviceName (held->kind), shortDeviceName (otherSlot (held->kind).kind));
            dcol = Theme::warn;
        }
        else if (part)
        {
            dmsg = QStringLiteral ("%1 %2").arg (shortDeviceName (part->kind), part->partialText);
            dcol = Theme::warn;
        }
        else if (wait)
            dmsg = QStringLiteral ("Waiting for the first %1 samples…").arg (shortDeviceName (wait->kind));
        else if (cyton_.failed && cyton_.failKind == DeviceWorker::FailNotFound && !cyOn)
            dmsg = QStringLiteral ("EmotiBit streams nominal · no Cyton dongle");
        else // every channel of every streaming device has data at a nominal rate
            dmsg = QStringLiteral ("All streams nominal");
    }
    else
    {
        dmsg = QStringLiteral ("Press connect (%1) to start streaming").arg (keyText (kConnectKey));
        dcol = Theme::textDim;
    }
    // transient messages never hide a warning / fault / discovery status
    const bool urgent = dcol == Theme::warn || dcol == Theme::fault;
    if (DeviceWorker::steadyNow () < msgUntil_ && !msg_.isEmpty () && !urgent)
        statusStrip_->setMessage (msg_, msgColor_.isValid () ? msgColor_ : Theme::textMuted);
    else
        statusStrip_->setMessage (dmsg, dcol);

    // ---- banner: warning > fault > recording
    bool showBanner = true;
    if (warn)
    {
        QStringList sentences;
        if (stallSlot)
        {
            const int n = stallSlot->dataCount;
            if (stallSlot->neverCount == stallSlot->staleCount)
                sentences << QStringLiteral ("No %1 samples since it started streaming %2 s ago — no stream has "
                                             "delivered data.")
                                 .arg (shortDeviceName (stallSlot->kind))
                                 .arg (stallSlot->stallAge, 0, 'f', 1);
            else
            {
                const QString streams = stallSlot->staleCount == n
                    ? QStringLiteral ("all %1 streams are frozen").arg (n)
                    : QStringLiteral ("%1 of %2 streams are frozen").arg (stallSlot->staleCount).arg (n);
                sentences << QStringLiteral ("No %1 samples for %2 s — %3.")
                                 .arg (shortDeviceName (stallSlot->kind))
                                 .arg (stallSlot->stallAge, 0, 'f', 1)
                                 .arg (streams);
            }
        }
        if (nearRail_)
            sentences << Readouts::nearRailSentence (headroom_, rawPeak_, stallSlot != nullptr);
        const QDateTime since = QDateTime::fromMSecsSinceEpoch (static_cast<qint64> (
            (std::isfinite (warnSinceWall_) ? warnSinceWall_ : now) * 1000.0));
        banner_->setContent (Theme::warn, Theme::warnBack,
            stallSlot ? QStringLiteral ("STREAM STALLED") : (saturated ? QStringLiteral ("SATURATED") : QStringLiteral ("NEAR RAIL")),
            sentences.join (QStringLiteral (" ")), QStringLiteral ("since %1").arg (since.toString (QStringLiteral ("HH:mm:ss"))));
    }
    else if (failedSlot)
    {
        const bool nf = failedSlot->failKind == DeviceWorker::FailNotFound;
        QString text;
        if (nf && failedSlot->kind == DeviceKind::EmotiBit && !bluetoothReason (failedSlot->failText).isEmpty ())
            text = QStringLiteral ("EmotiBit not found over Bluetooth or Wi-Fi. Check that it is on and near this computer, "
                                   "then connect again.");
        else if (nf && failedSlot->kind == DeviceKind::EmotiBit)
            text = (failedSlot->fieldBlank || failedSlot->typedIp.isEmpty ())
                ? QStringLiteral ("EmotiBit did not answer the discovery. Enter its IP address in the left rail and connect again.")
                : QStringLiteral ("EmotiBit did not answer at %1. Check the address in the left rail and connect again.").arg (failedSlot->typedIp);
        else if (nf)
            text = QStringLiteral ("No Cyton dongle on USB. Plug it in, then connect again.");
        else
            text = QStringLiteral ("%1: %2").arg (shortDeviceName (failedSlot->kind), failedSlot->failText.section ('\n', 0, 0));
        QString meta = QStringLiteral ("after %1 s").arg (failedSlot->failAfter, 0, 'f', 1);
        if (nf && failedSlot->kind == DeviceKind::EmotiBit)
            meta = QStringLiteral ("timeout after %1 s").arg (failedSlot->failTimeout, 0, 'f', 1);
        else if (nf)
            meta = QStringLiteral ("rescan: %1").arg (keyText (kRescanKey));
        banner_->setContent (Theme::fault, Theme::faultBack,
            nf ? QStringLiteral ("DEVICE NOT FOUND") : QStringLiteral ("CONNECTION FAILED"), text, meta);
    }
    else if (recordWanted_ && isRecording ())
    {
        const int n = recFiles_.size ();
        banner_->setContent (Theme::fault, Theme::faultBack, QStringLiteral ("RECORDING"),
            QStringLiteral ("Writing %1 CSV file%2 (raw BrainFlow rows; filters and heart rate are display-only). "
                            "Stop from the left rail when the block is finished.")
                .arg (n)
                .arg (n == 1 ? QString () : QStringLiteral ("s")),
            QStringLiteral ("%1 · %2").arg (homeAbbrev (opts_.recordDir), Readouts::bytes (recBytes_)));
    }
    else
        showBanner = false;
    if (banner_->isVisible () != showBanner)
        banner_->setVisible (showBanner);
}

void MainWindow::showMessage (const QString &text, int ms, const QColor &color)
{
    msg_ = text;
    msgColor_ = color;
    msgUntil_ = DeviceWorker::steadyNow () + ms / 1000.0;
    if (statusStrip_)
        statusStrip_->setMessage (text, color.isValid () ? color : Theme::textMuted);
}

void MainWindow::openWifiSetup ()
{
    // Setup restarts the EmotiBit: the link is enabled only while it is idle.
    if (!wifiBtn_->isEnabled ())
        return;
    EmotiBitWifiDialog dialog (this);
    dialog.exec ();
}

void MainWindow::refreshPorts (bool announce)
{
    const QString current = portCombo_->currentText ().trimmed ();
    const QString keep = current.isEmpty () ? portOverride_ : current;
    const QVector<SerialPortEntry> all = listSerialPorts (); // OpenBCI dongles first
    portCombo_->blockSignals (true);
    portCombo_->clear ();
    portCombo_->addItem (QStringLiteral ("Auto"));
    portCombo_->setItemData (0, QStringLiteral ("The last port that connected if it is an OpenBCI dongle, else the first dongle"),
        Qt::ToolTipRole);
    int usb = 0, dongles = 0;
    bool keepListed = isAuto (keep);
    for (const SerialPortEntry &e : all)
    {
        if (e.vid == 0 && !e.cytonDongle)
            continue; // not a USB serial device (Bluetooth, debug console, ...)
        const QString port = brainflowSerialPort (e);
        portCombo_->addItem (port);
        auto hex4 = [] (quint16 id) { return QString::number (id, 16).rightJustified (4, QLatin1Char ('0')); };
        portCombo_->setItemData (portCombo_->count () - 1,
            QStringLiteral ("%1 · %2:%3%4")
                .arg (e.description.isEmpty () ? QStringLiteral ("USB serial") : e.description, hex4 (e.vid),
                    hex4 (e.pid), e.cytonDongle ? QStringLiteral (" · OpenBCI dongle") : QString ()),
            Qt::ToolTipRole);
        keepListed = keepListed || sameSerialPort (port, keep);
        ++usb;
        dongles += e.cytonDongle ? 1 : 0;
    }
    if (!keepListed)
        portCombo_->addItem (keep); // keep a chosen override selectable even when unplugged
    if (usb == 0)
    {
        portCombo_->addItem (QStringLiteral ("no USB serial ports found"));
        if (auto *m = qobject_cast<QStandardItemModel *> (portCombo_->model ()))
            m->item (portCombo_->count () - 1)->setEnabled (false);
    }
    portCombo_->setCurrentText (isAuto (keep) ? QStringLiteral ("Auto") : keep);
    portCombo_->blockSignals (false);
    if (portCombo_->lineEdit ())
        portCombo_->lineEdit ()->setCursorPosition (0); // show the start of the path
    detectedPort_ = pickCytonDongle (all, opts_.cytonPort);
    if (statusStrip_ && announce)
        showMessage (dongles > 0 ? QStringLiteral ("Found %1 USB serial port(s), %2 OpenBCI dongle(s)").arg (usb).arg (dongles)
                                 : QStringLiteral ("Found %1 USB serial port(s), no OpenBCI dongle").arg (usb),
            3000);
    if (cyton_.meta)
        updateSlotUi (cyton_);
}

void MainWindow::applyFilterSettings ()
{
    const bool dc = dcToggle_->isChecked (), hp = hpToggle_->isChecked (), notch = notchToggle_->isChecked (),
               lp = lpToggle_->isChecked ();
    cytonPlot_->setRemoveMean (dc);
    cytonPlot_->setSymmetric (dc || hp);
    applyEcgChips ();
    cyton_.worker->setHighPass (hp, kEcgHighPassHz);
    cyton_.worker->setNotch (notch, kEcgNotchHz);
    cyton_.worker->setLowPass (lp, kEcgLowPassHz);
    emotibit_.worker->setHighPass (false);
    emotibit_.worker->setNotch (false);
    emotibit_.worker->setLowPass (false);
}

void MainWindow::updateWindowTitle ()
{
    setWindowTitle (QStringLiteral ("bioacq · Cyton ECG + EmotiBit%1")
                        .arg (synthToggle_->isChecked () ? QStringLiteral ("  [SYNTHETIC]") : QString ()));
}

// =============================================================== timers
void MainWindow::onFrame ()
{
    const double now = wallClockSeconds ();
    for (PlotWidget *p : allPlots ())
        p->tick (now);
}

void MainWindow::onRateTimer ()
{
    const double now = wallClockSeconds ();
    sessionBar_->setClock (QTime::currentTime ().toString (QStringLiteral ("HH:mm:ss")));
    updateHeadroom (now);
    for (Slot *s : {&cyton_, &emotibit_})
        updateStreamHealth (*s, now);
    // The PPG row keeps one rate form: when one header is too narrow for
    // "24.9 / 25 Hz", all three show "24.9 Hz".
    const bool fullPpgRates =
        ppgGreenPlot_->fullRateFits () && ppgRedPlot_->fullRateFits () && ppgIrPlot_->fullRateFits ();
    for (PlotWidget *p : {ppgGreenPlot_, ppgRedPlot_, ppgIrPlot_})
        p->setRateCompact (!fullPpgRates);
    updateHeartRate (now);
    for (Slot *s : {&cyton_, &emotibit_})
        updateSlotUi (*s);
    updateConnectUi ();
    updateRecordUi ();
    refreshChrome (now);
}

void MainWindow::onSlowTimer ()
{
    cpu_.sample (); // only here: every CPU reading covers the last ~1 s
    pollRecordingSizes ();
    if (!cyton_.worker->isActive ())
        detectedPort_ = cytonPortFor (QString ()); // the idle detail line follows plugging / unplugging
}

void MainWindow::pollRecordingSizes ()
{
    if (recFiles_.isEmpty ())
        return;
    double total = 0.0;
    for (const QString &f : recFiles_)
    {
        const QFileInfo fi (f); // fresh stat every call
        if (fi.exists ())
            total += static_cast<double> (fi.size ());
    }
    recBytes_ = total;
}

void MainWindow::closeEvent (QCloseEvent *event)
{
    saveSettings ();
    if (!anyActive ())
    {
        frameTimer_->stop ();
        rateTimer_->stop ();
        slowTimer_->stop ();
        event->accept ();
        return;
    }
    // Stop both sessions on their worker threads and close once both are
    // released. If a prepare_session is still blocking, the worker releases
    // the session as soon as BrainFlow returns.
    event->ignore ();
    if (closing_)
        return;
    closing_ = true;
    if (errorDialog_)
        errorDialog_->close ();

    // Forced-exit deadline from the actual configuration. Each worker may first
    // have to wait for the other one's BrainFlow call (one global lock), so
    // the per-worker worst cases add up; plus a margin.
    double budget = 15.0;
    for (Slot *s : {&cyton_, &emotibit_})
        if (s->worker->isActive ())
        {
            budget += s->worker->worstCaseStopSeconds ();
            s->stopRequested = true;
            s->worker->requestStop ();
            updateSlotUi (*s);
        }
    updateConnectUi ();
    centralWidget ()->setEnabled (false);
    showMessage (QStringLiteral ("Closing — stopping streams and releasing devices…"), 600000);
    hide (); // the close takes effect at once; the release finishes in the background

    QTimer::singleShot (static_cast<int> (budget * 1000.0), this, [this, budget] {
        QStringList busy;
        for (const Slot *s : {&cyton_, &emotibit_})
            if (s->worker->isActive ())
                busy << QStringLiteral ("%1 (%2%3)")
                            .arg (QString::fromLatin1 (deviceSlotName (s->kind)),
                                s->worker->state () == DeviceWorker::Connecting ? QStringLiteral ("connecting")
                                                                                 : QStringLiteral ("streaming/stopping"),
                                s->worker->inBrainFlowSetup () ? QStringLiteral (", inside prepare_session")
                                                               : QString ());
        std::fprintf (stderr,
            "bioacq: device release did not finish within %.0f s (still busy: %s); forcing exit\n",
            budget, qPrintable (busy.join (QStringLiteral (", "))));
        std::fflush (nullptr); // flush every stdio stream, incl. BrainFlow's file:// recordings
        std::_Exit (1);
    });
}
