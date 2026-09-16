#include "MainWindow.h"

#include "BuildConfig.h"
#include "DeviceWorker.h"
#include "EmotiBitDiscovery.h"
#include "PlotWidget.h"
#include "RingBuffer.h"
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
#include <QHBoxLayout>
#include <QHostAddress>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
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
const char *kKeyPort = "cyton/port";
const char *kKeyIp = "emotibit/ip";
const char *kKeyTimeout = "emotibit/timeout";
const char *kKeyWindow = "display/windowSec";
const char *kKeyDc = "cyton/removeDc";
const char *kKeyHp = "cyton/highPass";
const char *kKeyNotch = "cyton/notch";
const char *kKeyRecord = "record/enabled";

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN ();

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

// Single-line label that elides instead of growing (folder paths).
class ElideLabel : public QLabel
{
public:
    using QLabel::QLabel;
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
            fontMetrics ().elidedText (text (), Qt::ElideMiddle, width ()));
    }
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

} // namespace

// =============================================================================
MainWindow::MainWindow (const LaunchOptions &opts, QWidget *parent) : QMainWindow (parent), opts_ (opts)
{
    if (opts_.recordDir.isEmpty ())
        opts_.recordDir = QString::fromUtf8 (BIOACQ_RECORD_DIR);
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

    // Keyboard: Cmd+R rescans serial ports, Cmd+Shift+D connects / discovers the EmotiBit.
    auto *rescan = new QShortcut (QKeySequence (Qt::CTRL | Qt::Key_R), this);
    connect (rescan, &QShortcut::activated, this, [this] {
        if (!portCombo_->isLocked () && portCombo_->isEnabled ())
            refreshPorts ();
    });
    auto *disc = new QShortcut (QKeySequence (Qt::CTRL | Qt::SHIFT | Qt::Key_D), this);
    connect (disc, &QShortcut::activated, this, [this] {
        if (!emotibit_.worker->isActive ())
            connectDevice (DeviceKind::EmotiBit);
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
    if (!opts_.portSet && !st.value (kKeyPort).toString ().isEmpty ())
        opts_.cytonPort = st.value (kKeyPort).toString ();
    if (!opts_.ipSet && st.contains (kKeyIp))
        opts_.emotibitIp = st.value (kKeyIp).toString (); // may be "" = broadcast discovery
    if (!opts_.timeoutSet)
        opts_.emotibitTimeoutSec = st.value (kKeyTimeout, opts_.emotibitTimeoutSec).toInt ();
    if (!opts_.windowSet)
        opts_.windowSec = st.value (kKeyWindow, opts_.windowSec).toInt ();
    if (!opts_.recordSet)
        opts_.record = st.value (kKeyRecord, opts_.record).toBool ();
    opts_.removeDc = st.value (kKeyDc, opts_.removeDc).toBool ();
    opts_.highPass = st.value (kKeyHp, opts_.highPass).toBool ();
    opts_.notch = st.value (kKeyNotch, opts_.notch).toBool ();
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
    st.setValue (kKeyRecord, recordWanted_);
}

void MainWindow::saveDeviceAddress (DeviceKind kind, const QString &address)
{
    if (!opts_.useSettings || address.isEmpty ())
        return;
    QSettings st;
    st.setValue (kind == DeviceKind::Cyton ? kKeyPort : kKeyIp, address);
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
    cl->addWidget (buildCytonModule ());
    cl->addWidget (buildEmotibitModule ());
    cl->addWidget (buildDisplayModule ());
    cl->addStretch (1);
    scroll->setWidget (content);
    rv->addWidget (scroll, 1);
    rv->addWidget (buildRecordModule ());
    return rail;
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
    v->addLayout (headerRow (QStringLiteral ("// CYTON · OPENBCI"), cyton_.chip));
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
    portCombo_->setToolTip (QStringLiteral ("Serial port of the Cyton dongle (/dev/cu.usbserial-*). Editable.\n"
                                            "Close the OpenBCI GUI first: only one program can use the dongle."));
    refreshBtn_ = new RescanButton;
    refreshBtn_->setToolTip (QStringLiteral ("Rescan /dev/cu.usbserial-*  (⌘R)"));
    row->addWidget (portCombo_, 1);
    row->addWidget (refreshBtn_, 0, Qt::AlignTop); // 28 px button top-aligned with the 30 px field (design)
    v->addLayout (row);
    v->addSpacing (8);
    cyton_.button = makeButton (QStringLiteral ("[ connect cyton ]"), "primary", nullptr, 30);
    v->addWidget (cyton_.button);
    v->addSpacing (7);
    cyton_.meta = makeLabel (QString (), Theme::mono (10, 400, 0.02), Theme::textDim);
    cyton_.meta->setWordWrap (true);
    v->addWidget (cyton_.meta);
    v->addWidget (buildErrorBox (cyton_));

    v->addSpacing (12);
    v->addWidget (kicker (QStringLiteral ("// CH1 DISPLAY FILTERS")));
    v->addSpacing (6);
    dcToggle_ = new ToggleSwitch (QStringLiteral ("Remove DC offset"));
    dcToggle_->setChecked (opts_.removeDc);
    dcToggle_->setToolTip (QStringLiteral ("Display only: subtracts the mean of the visible window. The rail headroom\n"
                                           "below still measures the raw value, so a DC offset near the rail shows."));
    hpToggle_ = new ToggleSwitch (QStringLiteral ("High-pass 1 Hz"));
    hpToggle_->setChecked (opts_.highPass);
    hpToggle_->setToolTip (QStringLiteral ("2nd-order RBJ high-pass (IIR) applied sample-by-sample in the worker thread.\n"
                                           "Filter state resets on toggle."));
    notchToggle_ = new ToggleSwitch (QStringLiteral ("Notch 60 Hz"));
    notchToggle_->setChecked (opts_.notch);
    notchToggle_->setToolTip (QStringLiteral ("RBJ notch at 60 Hz (IIR, Q = 30), streaming, in the worker thread."));
    v->addWidget (dcToggle_);
    v->addSpacing (6);
    v->addWidget (hpToggle_);
    v->addSpacing (6);
    v->addWidget (notchToggle_);

    v->addSpacing (12);
    auto *hrow = new QHBoxLayout;
    hrow->setContentsMargins (0, 0, 0, 0);
    hrow->addWidget (kicker (QStringLiteral ("// RAIL HEADROOM")), 0, Qt::AlignBottom);
    hrow->addStretch (1);
    railPct_ = makeLabel (QStringLiteral ("—"), Theme::mono (10.5), Theme::controlEdge);
    hrow->addWidget (railPct_, 0, Qt::AlignBottom);
    v->addLayout (hrow);
    v->addSpacing (4);
    railBar_ = new MeterBar (12, Theme::edge); // content-box 8 + padding 1 + border 1 (design)
    railBar_->setToolTip (QStringLiteral ("1 − max|raw Ch1| / 187,500 µV over the visible window (before filters)."));
    v->addWidget (railBar_);
    v->addSpacing (4);
    railNote_ = makeLabel (QStringLiteral ("No signal"), Theme::mono (10), Theme::textDim);
    v->addWidget (railNote_);

    connect (refreshBtn_, &QPushButton::clicked, this, [this] { refreshPorts (); });
    connect (cyton_.button, &QPushButton::clicked, this, [this] { onButton (DeviceKind::Cyton); });
    for (ToggleSwitch *t : {dcToggle_, hpToggle_, notchToggle_})
        connect (t, &QAbstractButton::toggled, this, [this] (bool) { applyFilterSettings (); });
    return f;
}

QWidget *MainWindow::buildEmotibitModule ()
{
    QVBoxLayout *v = nullptr;
    QFrame *f = moduleFrame (v);
    emotibit_.chip = new StatusChip;
    v->addLayout (headerRow (QStringLiteral ("// EMOTIBIT · WIFI"), emotibit_.chip));
    v->addSpacing (9);

    auto *row = new QHBoxLayout;
    row->setContentsMargins (0, 0, 0, 0);
    row->setSpacing (6);
    auto *c1 = new QVBoxLayout;
    c1->setSpacing (5);
    c1->addWidget (kicker (QStringLiteral ("// IP ADDRESS")));
    ipEdit_ = new QLineEdit (opts_.emotibitIp);
    ipEdit_->setFixedHeight (30); // content-box 28 + 1 px border (design)
    ipEdit_->setPlaceholderText (QStringLiteral ("auto-discover"));
    ipEdit_->setToolTip (QStringLiteral (
        "EmotiBit IP address (shown in its serial boot log and in EmotiBit Oscilloscope).\n"
        "A typed IP works across subnets (unicast). Leave blank for broadcast discovery,\n"
        "which only works when this Mac and the EmotiBit are on the same subnet.\n"
        "The last IP that connected successfully is remembered."));
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
    v->addSpacing (6);
    ipHint_ = makeLabel (QStringLiteral ("Blank = auto-discover on this subnet"), Theme::mono (10), Theme::textDim);
    ipHint_->setWordWrap (true);
    v->addWidget (ipHint_);
    v->addSpacing (8);
    emotibit_.button = makeButton (QStringLiteral ("[ connect emotibit ]"), "primary", nullptr, 30);
    emotibit_.button->setToolTip (QStringLiteral ("Connect / discover the EmotiBit  (⌘⇧D)\n"
                                                  "Close EmotiBit Oscilloscope first: only one host can own the stream."));
    v->addWidget (emotibit_.button);

    // discovering module
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
    bl->addSpacing (8);
    cancelDiscoveryBtn_ = makeButton (QStringLiteral ("[ cancel discovery ]"), "secondary", "sm", 26);
    bl->addWidget (cancelDiscoveryBtn_);
    dwl->addWidget (box);
    discoverWrap_->hide ();
    v->addWidget (discoverWrap_);

    v->addWidget (buildErrorBox (emotibit_));
    emotibit_.meta = makeLabel (QString (), Theme::mono (10, 400, 0.02), Theme::textDim);
    emotibit_.meta->setWordWrap (true);
    emotibit_.meta->setContentsMargins (0, 7, 0, 0); // its 7 px gap hides with it
    v->addWidget (emotibit_.meta);

    connect (emotibit_.button, &QPushButton::clicked, this, [this] { onButton (DeviceKind::EmotiBit); });
    connect (cancelDiscoveryBtn_, &QPushButton::clicked, this, [this] { disconnectDevice (DeviceKind::EmotiBit); });
    connect (ipEdit_, &QLineEdit::textChanged, this, [this] (const QString &t) {
        const QString s = t.trimmed ();
        setFlag (ipEdit_, "invalid", !s.isEmpty () && !validIpv4 (s));
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
    v->addWidget (pauseToggle_);
    v->addSpacing (5);
    v->addWidget (makeLabel (QStringLiteral ("Streams keep running while paused"), Theme::mono (10), Theme::textDim));
    // ("Simulate devices" lives in the idle call to action over the hero plot:
    // it can only change while nothing is connected, and the rail then fits
    // a 1280 x 800 window like the design.)

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
        "Record every BrainFlow preset of each streaming device to CSV (tab-separated rows, no header;\n"
        "a *_columns.json next to each file describes the rows). Starts and stops at any time while\n"
        "streaming; pressed with nothing connected it arms the recording for the next connect.\nFolder: %1")
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
    const QVector<PlotWidget::Trace> xyz = {{QStringLiteral ("X"), Theme::traceX, Dash::Solid},
        {QStringLiteral ("Y"), Theme::traceY, Dash::Dashed}, {QStringLiteral ("Z"), Theme::traceZ, Dash::Dotted}};
    cytonPlot_ = new PlotWidget (PlotWidget::Kind::Hero, QStringLiteral ("CYTON CH1"), QStringLiteral ("µV"),
        {{QStringLiteral ("ch1"), Theme::traceHero, Dash::Solid}});
    cytonPlot_->setSubtitle (QStringLiteral ("single-ended · SRB / AGND / N1P"));
    cytonPlot_->setFullScale (Readouts::kCytonFullScaleUv);
    tempPlot_ = new PlotWidget (PlotWidget::Kind::Scalar, QStringLiteral ("TEMPERATURE"), QStringLiteral ("°C"),
        {{QStringLiteral ("T"), Theme::traceTemp, Dash::Solid}});
    ppgPlot_ = new PlotWidget (PlotWidget::Kind::Scalar, QStringLiteral ("PPG GREEN"), QStringLiteral ("a.u."),
        {{QStringLiteral ("green"), Theme::tracePpg, Dash::Solid}});
    accelPlot_ = new PlotWidget (PlotWidget::Kind::Triple, QStringLiteral ("ACCEL"), QStringLiteral ("g"), xyz);
    gyroPlot_ = new PlotWidget (PlotWidget::Kind::Triple, QStringLiteral ("GYRO"), QStringLiteral ("°/s"), xyz);
    magPlot_ = new PlotWidget (PlotWidget::Kind::Triple, QStringLiteral ("MAG"), QStringLiteral ("µT"), xyz);

    v->addWidget (cytonPlot_, 135);
    auto *r1 = new QHBoxLayout;
    r1->setSpacing (Theme::gutter);
    r1->addWidget (tempPlot_, 1);
    r1->addWidget (ppgPlot_, 1);
    v->addLayout (r1, 100);
    auto *r2 = new QHBoxLayout;
    r2->setSpacing (Theme::gutter);
    r2->addWidget (accelPlot_, 1);
    r2->addWidget (gyroPlot_, 1);
    r2->addWidget (magPlot_, 1);
    v->addLayout (r2, 100);

    for (PlotWidget *p : allPlots ())
    {
        p->setWindowSeconds (windowStepper_->value ());
        p->setPlaceholder (QStringLiteral ("NO SIGNAL"));
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
    auto *t = makeLabel (QStringLiteral ("Connect a device to start streaming"), Theme::mono (19, 400, -0.01),
        Theme::textStrong);
    t->setAlignment (Qt::AlignCenter);
    v->addWidget (t);
    gap (14);
    auto *para = makeLabel (richPara (QStringLiteral (
                                          "Pick the Cyton dongle's serial port in the left rail, or give the "
                                          "EmotiBit's IP address — leave it blank to auto-discover on this "
                                          "subnet. Plots arm themselves as soon as samples arrive."),
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
    auto *bc = makeButton (QStringLiteral ("[ connect cyton ]"), "primary", "lg", 32);
    auto *be = makeButton (QStringLiteral ("[ connect emotibit ]"), "secondary", "lg", 32);
    for (QPushButton *b : {bc, be})
        b->setStyleSheet (QStringLiteral ("padding: 0 16px;"));
    btns->addWidget (bc);
    btns->addWidget (be);
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
    hints->addWidget (keycap (QStringLiteral ("⌘R")));
    hints->addWidget (makeLabel (QStringLiteral ("rescan serial ports"), Theme::mono (10), Theme::textDim));
    hints->addWidget (vsep ());
    hints->addWidget (keycap (QStringLiteral ("⌘⇧D")));
    hints->addWidget (makeLabel (QStringLiteral ("discover EmotiBit"), Theme::mono (10), Theme::textDim));
    hints->addWidget (vsep ());
    // Only changeable while nothing is connected -- exactly when this overlay
    // shows; afterwards the session label / meta line say "synthetic".
    synthToggle_ = new ToggleSwitch (QStringLiteral ("simulate devices"));
    synthToggle_->setFont (Theme::mono (10));
    synthToggle_->setChecked (opts_.synthetic);
    synthToggle_->setToolTip (QStringLiteral ("Connect both slots to BrainFlow's synthetic board (no hardware):\n"
                                              "tests the whole pipeline. Locked while a device is connected."));
    hints->addWidget (synthToggle_);
    hints->addStretch (1);
    v->addLayout (hints);
    v->addStretch (1);
    connect (bc, &QPushButton::clicked, this, [this] { connectDevice (DeviceKind::Cyton); });
    connect (be, &QPushButton::clicked, this, [this] { connectDevice (DeviceKind::EmotiBit); });
    connect (synthToggle_, &QAbstractButton::toggled, this, [this] (bool) { updateWindowTitle (); });
    return ov;
}

std::array<PlotWidget *, 6> MainWindow::allPlots () const
{
    return {cytonPlot_, tempPlot_, ppgPlot_, accelPlot_, gyroPlot_, magPlot_};
}

PlotWidget *MainWindow::plotForKey (const std::string &key) const
{
    if (key == SignalKeys::CytonCh1)
        return cytonPlot_;
    if (key == SignalKeys::EmotiTemp)
        return tempPlot_;
    if (key == SignalKeys::EmotiPpgGreen)
        return ppgPlot_;
    if (key == SignalKeys::EmotiAccel)
        return accelPlot_;
    if (key == SignalKeys::EmotiGyro)
        return gyroPlot_;
    if (key == SignalKeys::EmotiMag)
        return magPlot_;
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

    connect (w, &DeviceWorker::stateChanged, this, [this, kind] (int) { updateSlotUi (slot (kind)); });

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
        if (kind == DeviceKind::EmotiBit && s.autoDiscovery)
        {
            // Show (and, once connected, remember) the address discovery found,
            // so the next connect can go straight to it.
            ipEdit_->setText (ip);
            s.autoDiscovery = false;
        }
        showMessage (QStringLiteral ("EmotiBit%1 found at %2")
                         .arg (serial.isEmpty () ? QString () : QStringLiteral (" ") + serial, ip));
    });

    connect (w, &DeviceWorker::connected, this, [this, kind, name] (const QString &) {
        Slot &s = slot (kind);
        s.linkSeconds = DeviceWorker::steadyNow () - s.connectStarted;
        s.streamingSinceWall = wallClockSeconds (); // emitted right after the worker reaches Streaming
        s.failed = false;
        if (!std::isfinite (sessionStartWall_))
            sessionStartWall_ = wallClockSeconds ();
        showMessage (QStringLiteral ("%1 connected in %2 s").arg (name).arg (s.linkSeconds, 0, 'f', 1), 5000);
        if (!s.synthetic)
            saveDeviceAddress (kind, s.address);
        saveSettings ();
        updateSlotUi (s);
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

bool MainWindow::isRecording () const
{
    return cyton_.worker->isRecording () || emotibit_.worker->isRecording ();
}

void MainWindow::onButton (DeviceKind kind)
{
    Slot &s = slot (kind);
    if (!s.worker->isActive ())
        connectDevice (kind);
    else if (!s.stopRequested && (s.worker->state () == DeviceWorker::Connecting ||
                                     s.worker->state () == DeviceWorker::Streaming))
        disconnectDevice (kind); // Cancel (connecting) or Disconnect (streaming)
}

void MainWindow::connectDevice (DeviceKind kind)
{
    connectDeviceWith (kind, synthToggle_->isChecked ());
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
    s.autoDiscovery = false;
    s.address.clear ();
    s.serial.clear ();
    s.typedIp.clear ();
    s.targets.clear ();
    s.failed = false;
    s.streamingSinceWall = kNaN;
    auto failNow = [&] (const QString &text) {
        s.failed = true;
        s.failKind = DeviceWorker::FailOther;
        s.failText = text;
        s.failAfter = 0.0;
        updateSlotUi (s);
    };
    if (synth)
    {
        // distinct params -> distinct BrainFlow session keys for the two slots
        cfg.params.other_info = "bioacq:" + cfg.slot;
        s.progressText = QStringLiteral ("Opening the BrainFlow synthetic board…");
    }
    else if (kind == DeviceKind::Cyton)
    {
        const QString port = portCombo_->currentText ().trimmed ();
        if (port.isEmpty ())
        {
            failNow (QStringLiteral ("Choose a serial port first."));
            return;
        }
        cfg.params.serial_port = port.toStdString ();
        s.address = port;
        s.progressText = QStringLiteral ("Opening %1 (the soft reset takes a few s)…").arg (port);
    }
    else
    {
        const QString ip = ipEdit_->text ().trimmed ();
        cfg.params.ip_address = ip.toStdString ();
        cfg.params.timeout = timeoutSpin_->value ();
        cfg.ownDiscovery = !opts_.brainflowDiscovery;
        s.address = ip;
        s.typedIp = ip;
        s.autoDiscovery = ip.isEmpty ();
        if (ip.isEmpty ())
        {
            QStringList t;
            for (const std::string &a : emotibit::ipv4BroadcastAddresses ())
                t << QString::fromStdString (a);
            s.targets = t.isEmpty () ? QStringLiteral ("no interface") : t.join (QStringLiteral (", "));
        }
        else
            s.targets = ip;
        s.progressText = cfg.ownDiscovery ? QString ()
                                          : QStringLiteral ("BrainFlow is discovering the EmotiBit (holds its lock)…");
    }
    cfg.countPackageGaps = kind == DeviceKind::Cyton;
    if (kind == DeviceKind::EmotiBit && opts_.testFreezeEmotibitSec >= 0.0)
        cfg.testFreezeAfterSec = opts_.testFreezeEmotibitSec;
    if (kind == DeviceKind::Cyton && opts_.testRailOffsetUv != 0.0)
        cfg.testRawOffset = opts_.testRailOffsetUv;

    const std::vector<SignalDef> defs = signalDefsFor (kind);
    std::vector<std::string> problems;
    cfg.signalSpecs = resolveSignals (cfg.boardId, defs, &problems);
    if (cfg.signalSpecs.empty ())
    {
        failNow (QStringLiteral ("Cannot map any signal on this board."));
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
    s.stopRequested = false;
    s.stalled = s.held = s.pending = false;

    // Plots of this device that the board cannot provide
    for (const SignalDef &d : defs)
        if (PlotWidget *p = plotForKey (d.key))
        {
            p->setSource (nullptr, -1, 0.0, 2);
            p->setNote (QString ());
            p->setPlaceholder (QStringLiteral ("NOT PROVIDED BY THIS BOARD"));
        }

    s.plots.clear ();
    s.meters.clear ();
    for (const SignalChannel &ch : s.worker->channels ())
    {
        PlotWidget *p = plotForKey (ch.spec.key);
        s.plots.push_back (p);
        s.meters.emplace_back (2.0);
        if (!p)
            continue;
        p->setSource (ch.ring, ch.rawChannel, ch.spec.nominalRate, ch.spec.valueDecimals);
        p->setUnits (QString::fromStdString (ch.spec.units));
        // Source in the plot's tooltip; a REAL channel resolved through a
        // fallback is also flagged in the header (SUBST). The synthetic
        // board's stand-ins are expected: the session reads "synthetic".
        p->setNote (QStringLiteral ("source: %1").arg (QString::fromStdString (ch.spec.source)),
            !synth && ch.spec.substituted);
        p->setPlaceholder (QStringLiteral ("AWAITING STREAM"));
    }
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
                                          ? QStringLiteral ("Cancelling… waiting for BrainFlow to finish opening the session")
                                          : QStringLiteral ("Cancelling…"))
                                : QStringLiteral ("Stopping the stream and releasing the session…");
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
QString MainWindow::metaText (const Slot &s) const
{
    const bool active = s.worker->isActive ();
    const DeviceWorker::State st = s.worker->state ();
    if (!active)
    {
        if (s.failed)
            return s.failAfter > 0.0 ? QStringLiteral ("Failed after %1 s").arg (s.failAfter, 0, 'f', 1)
                                     : QStringLiteral ("Not connected");
        if (std::isfinite (s.linkSeconds))
            return QStringLiteral ("Not connected · last link %1 s").arg (s.linkSeconds, 0, 'f', 1);
        return QStringLiteral ("Not connected");
    }
    if (st == DeviceWorker::Streaming && !s.stopRequested)
    {
        QString t = QStringLiteral ("Linked in %1 s").arg (s.linkSeconds, 0, 'f', 1);
        if (s.kind == DeviceKind::Cyton)
        {
            const auto &ch = s.worker->channels ();
            if (!ch.empty ())
                t += QStringLiteral (" · %1 Hz").arg (ch.front ().spec.nominalRate, 0, 'g', 4);
        }
        if (s.synthetic)
            t += QStringLiteral (" · synthetic");
        else if (s.kind == DeviceKind::EmotiBit)
        {
            t += QStringLiteral (" · ") + s.address;
            if (!s.serial.isEmpty ())
                t += QStringLiteral (" · ") + s.serial;
        }
        return t;
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

    // main button
    const QString dev = s.kind == DeviceKind::Cyton ? QStringLiteral ("cyton") : QStringLiteral ("emotibit");
    QString text;
    const char *variant = "primary";
    bool enabled = !closing_;
    if (!active)
        text = s.failed ? QStringLiteral ("[ retry connect ]") : QStringLiteral ("[ connect %1 ]").arg (dev);
    else if (st == DeviceWorker::Connecting && !s.stopRequested)
    {
        if (s.kind == DeviceKind::Cyton || s.synthetic)
        {
            text = QStringLiteral ("[ cancel ]");
            variant = "secondary";
        }
        else
        {
            // the EmotiBit's cancel sits in the discovery module
            text = QStringLiteral ("[ connecting… ]");
            variant = "busy";
            enabled = false;
        }
    }
    else if (st == DeviceWorker::Streaming && !s.stopRequested)
    {
        text = QStringLiteral ("[ disconnect ]");
        variant = "secondary";
    }
    else
    {
        text = st == DeviceWorker::Connecting ? QStringLiteral ("[ cancelling… ]")
                                              : QStringLiteral ("[ disconnecting… ]");
        variant = "busy";
        enabled = false;
    }
    if (s.button->text () != text)
        s.button->setText (text);
    setVariant (s.button, variant);
    s.button->setEnabled (enabled);

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
        const bool nf = s.failed && s.failKind == DeviceWorker::FailNotFound;
        QString hint = QStringLiteral ("Blank = auto-discover on this subnet");
        QColor hc = Theme::textDim;
        if (nf && s.typedIp.isEmpty ())
        {
            hint = QStringLiteral ("Blank — auto-discovery was used. Type an address here.");
            hc = Theme::textBody;
        }
        else if (nf)
        {
            hint = QStringLiteral ("No answer here. Check the address."); // one line; details below
            hc = Theme::textBody;
        }
        if (ipHint_->text () != hint)
            ipHint_->setText (hint);
        Theme::setTextColor (ipHint_, hc);
        setFlag (ipEdit_, "attention", nf && editable);
        updateDiscoverBox (s);
    }
    synthToggle_->setEnabled (!anyActive () && !closing_);

    const QString meta = metaText (s);
    if (s.meta->text () != meta)
        s.meta->setText (meta);
    Theme::setTextColor (s.meta, s.failed && !active ? Theme::fault : Theme::textDim);
    const bool discovering = s.kind == DeviceKind::EmotiBit && discoverWrap_->isVisibleTo (discoverWrap_->parentWidget ());
    bool showMeta = !meta.isEmpty () && !discovering;
    // The design's EmotiBit module has no meta line: it shows only while a
    // connect / stop is in progress; otherwise the text is the chip's tooltip
    // (keeps the rail inside a 1280 x 800 window).
    if (s.kind == DeviceKind::EmotiBit)
        showMeta = showMeta && active && !(st == DeviceWorker::Streaming && !s.stopRequested);
    s.meta->setVisible (showMeta);
    if (s.chip->toolTip () != meta)
        s.chip->setToolTip (meta);
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
    QString title, time, detail, cancel;
    double frac = kNaN;
    if (s.stopRequested)
    {
        title = QStringLiteral ("CANCELLING…");
        detail = s.progressText;
        cancel = QStringLiteral ("[ cancelling… ]");
    }
    else if (ph == DeviceWorker::PhaseIdle || ph == DeviceWorker::PhaseDiscovering)
    {
        const double to = s.worker->discoveryTimeout ();
        title = QStringLiteral ("DISCOVERING…");
        time = QStringLiteral ("%1 / %2 s").arg (el, 0, 'f', 1).arg (to, 0, 'f', 1);
        frac = to > 0.0 ? el / to : kNaN;
        const int n = s.worker->probesSent ();
        const QString probes = n == 1 ? QStringLiteral ("1 probe sent") : QStringLiteral ("%1 probes sent").arg (n);
        detail = s.typedIp.isEmpty () ? QStringLiteral ("Broadcast on %1 · %2").arg (s.targets, probes)
                                      : QStringLiteral ("Unicast to %1 · %2").arg (s.typedIp, probes);
        cancel = QStringLiteral ("[ cancel discovery ]");
    }
    else
    {
        title = QStringLiteral ("OPENING SESSION…");
        time = QStringLiteral ("%1 s").arg (el, 0, 'f', 1);
        frac = 1.0;
        detail = s.progressText.isEmpty () ? QStringLiteral ("BrainFlow prepare_session…") : s.progressText;
        cancel = QStringLiteral ("[ cancel ]");
    }
    discoverTitle_->setText (title);
    discoverTime_->setText (time);
    discoverBar_->setValue (frac, Theme::warn);
    if (discoverDetail_->text () != detail)
        discoverDetail_->setText (detail);
    cancelDiscoveryBtn_->setText (cancel);
    cancelDiscoveryBtn_->setEnabled (!s.stopRequested && !closing_);
}

void MainWindow::updateErrorBox (Slot &s)
{
    const bool show = s.failed && !s.worker->isActive ();
    s.errorWrap->setVisible (show);
    if (!show)
        return;
    QString title, text, hint;
    const QString first = s.failText.section ('\n', 0, 0).trimmed ();
    QString rest = s.failText.section ('\n', 1).trimmed ();
    if (s.kind == DeviceKind::EmotiBit && s.failKind == DeviceWorker::FailNotFound)
    {
        title = QStringLiteral ("EMOTIBIT NOT FOUND");
        if (s.typedIp.isEmpty ())
        {
            text = QStringLiteral ("Auto-discovery is a UDP broadcast — it only reaches devices on the %1 as "
                                   "this Mac. This Mac is on %2.")
                       .arg (b (QStringLiteral ("same subnet"), Theme::textStrong), subnets ().toHtmlEscaped ());
            hint = QStringLiteral ("Enter the EmotiBit's address in the %1 field above (it is printed on the serial "
                                   "monitor at boot), then connect again.")
                       .arg (b (QStringLiteral ("IP address"), Theme::textBody));
        }
        else
        {
            text = QStringLiteral ("No answer from %1 within %2&nbsp;s (unicast, across subnets). This Mac is on %3.")
                       .arg (b (s.typedIp, Theme::textStrong))
                       .arg (s.failTimeout, 0, 'f', 1)
                       .arg (subnets ().toHtmlEscaped ());
            hint = QStringLiteral ("Check the %1 (serial monitor at boot) and that EmotiBit Oscilloscope is closed, "
                                   "or clear the field to auto-discover.")
                       .arg (b (QStringLiteral ("IP address"), Theme::textBody));
        }
    }
    else
    {
        title = s.failKind == DeviceWorker::FailNoSend ? QStringLiteral ("DISCOVERY BLOCKED")
                                                        : QStringLiteral ("CONNECTION FAILED");
        text = first.toHtmlEscaped ();
        hint = rest.isEmpty () ? QStringLiteral ("Check the device and connect again.") : rest.toHtmlEscaped ().replace ('\n', QStringLiteral ("<br>"));
    }
    s.errorTitle->setText (title);
    const QString t = richPara (text, 17), h = richPara (hint, 17); // 11.5 px x 1.5
    if (s.errorText->text () != t)
        s.errorText->setText (t);
    if (s.errorHint->text () != h)
        s.errorHint->setText (h);
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
    QStringList stallTexts; // per channel; applied below (one label per device-wide stall)
    for (std::size_t i = 0; i < nch; ++i)
    {
        const int si = static_cast<int> (stallTexts.size ());
        stallTexts << QString ();
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
            continue;
        }
        p->setPlaceholder (QStringLiteral ("AWAITING STREAM"));
        if (isStale)
        {
            // stalled: the measured rate is 0 by definition (the 2 s rate
            // window would otherwise still be decaying for another second)
            p->setRate (Readouts::rateText (true, 0.0, nominal), Theme::warn);
            if (heldNow)
                stallTexts[si] = QStringLiteral ("HELD · BRAINFLOW OPENING %1").arg (upperName (o.kind));
            else if (have)
                stallTexts[si] = QStringLiteral ("LAST SAMPLE %1 s AGO").arg (age, 0, 'f', 1);
            else
                stallTexts[si] = QStringLiteral ("NO SAMPLES SINCE CONNECT · %1 s").arg (silent, 0, 'f', 1);
            p->setTone (PlotWidget::Tone::Warn);
            p->setLed (Theme::warn);
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
            const QColor rc = rt == Readouts::RateTone::None
                ? Theme::textDim
                : (rt == Readouts::RateTone::Low ? Theme::warn : Theme::textMuted);
            p->setRate (Readouts::rateText (haveRate, rate, nominal), rc);
            if (rt == Readouts::RateTone::Low && s.lowRate.isEmpty ())
                s.lowRate = QStringLiteral ("%1 %2").arg (QString::fromStdString (chans[i].spec.title),
                    Readouts::rateText (true, rate, nominal));
            // The hero's LED stays the Cyton link state; near-rail is carried by
            // LATEST (amber), the headroom module and the banner.
            const bool warnHero = p == cytonPlot_ && nearRail_;
            p->setTone (!have ? PlotWidget::Tone::Off : (warnHero ? PlotWidget::Tone::Warn : PlotWidget::Tone::Normal));
            p->setLed (have ? Theme::ok : Theme::warn);
        }
    }
    const bool allStale = streaming && total > 0 && stale == total;
    // A device-wide stall is labelled once, on the device's last plot (MAG for
    // the EmotiBit, as in the design); a partial stall labels each stale plot.
    PlotWidget *labelPlot = nullptr;
    if (allStale && total > 1)
    {
        for (std::size_t i = 0; i < nch; ++i)
            if (s.plots[i])
                labelPlot = s.plots[i]; // the device's last plot ...
        if (std::find (s.plots.begin (), s.plots.begin () + static_cast<std::ptrdiff_t> (nch), magPlot_) !=
            s.plots.begin () + static_cast<std::ptrdiff_t> (nch))
            labelPlot = magPlot_; // ... MAG when present, as in the design
    }
    for (std::size_t i = 0; i < nch; ++i)
        if (PlotWidget *p = s.plots[i])
            p->setStallText ((labelPlot && p != labelPlot) ? QString () : stallTexts[static_cast<int> (i)]);
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

double MainWindow::cytonRawPeak (double now)
{
    for (const SignalChannel &ch : cyton_.worker->channels ())
    {
        if (ch.spec.key != SignalKeys::CytonCh1 || !ch.ring || ch.rawChannel < 0)
            continue;
        double ref = now;
        const double latest = ch.ring->latestTimestamp ();
        if (std::isfinite (latest) && std::fabs (latest - now) > 30.0)
            ref = latest; // timestamps far from the host clock: same anchor as the plot
        const std::size_t n = ch.ring->copySince (ref - windowStepper_->value (), peakT_, peakV_);
        if (ch.rawChannel >= static_cast<int> (peakV_.size ()))
            return kNaN;
        const std::vector<double> &raw = peakV_[static_cast<std::size_t> (ch.rawChannel)];
        double peak = -1.0;
        for (std::size_t k = 0; k < n; ++k)
            if (std::isfinite (raw[k]))
                peak = std::max (peak, std::fabs (raw[k]));
        return peak >= 0.0 ? peak : kNaN;
    }
    return kNaN;
}

void MainWindow::updateHeadroom (double now)
{
    const bool streaming = isStreaming (DeviceKind::Cyton) && !cyton_.stopRequested;
    // Straight from the Cyton ring over the window, not from the plot: it
    // keeps following the live data while the display is paused.
    rawPeak_ = streaming ? cytonRawPeak (now) : kNaN;
    headroom_ = Readouts::railHeadroom (rawPeak_);
    const bool near = streaming && Readouts::nearRail (headroom_, nearRail_);
    if (near && !nearRail_)
        railSinceWall_ = now;
    nearRail_ = near;
    cytonPlot_->setRailMode (near);
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
    railPct_->setText (Readouts::headroomPercent (headroom_));
    Theme::setTextColor (railPct_, c);
    railBar_->setValue (headroom_, c);
    railNote_->setText (near ? QStringLiteral ("Near rail · check electrode contact") : peakText (rawPeak_));
    Theme::setTextColor (railNote_, near ? Theme::warn : Theme::textDim);
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
            devs << (s->kind == DeviceKind::Cyton ? QStringLiteral ("ch1") : QStringLiteral ("emotibit"));
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
    const Slot *failedSlot = nullptr;
    for (const Slot *s : {&emotibit_, &cyton_})
        if (s->failed && !s->worker->isActive ())
        {
            failedSlot = s;
            break;
        }
    const bool emDiscovering = emotibit_.worker->isActive () && emotibit_.worker->state () == DeviceWorker::Connecting &&
        !emotibit_.stopRequested && !emotibit_.synthetic;
    const bool cyConnecting = cyton_.worker->isActive () && cyton_.worker->state () == DeviceWorker::Connecting;
    const Slot *stallSlot = emotibit_.stalled ? &emotibit_ : (cyton_.stalled ? &cyton_ : nullptr);
    const bool warn = stallSlot || nearRail_;
    if (warn && !std::isfinite (warnSinceWall_))
        warnSinceWall_ = stallSlot ? stallSlot->stallSince : railSinceWall_;
    if (!warn)
        warnSinceWall_ = kNaN;

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
            parts << QStringLiteral ("Cyton Ch1 near rail");
        dmsg = parts.join (QStringLiteral (" · "));
        dcol = Theme::warn;
    }
    else if (emDiscovering && emotibit_.worker->phase () != DeviceWorker::PhaseOpening)
    {
        const double el = std::max (0.0, DeviceWorker::steadyNow () -
                (emotibit_.worker->phase () == DeviceWorker::PhaseIdle ? emotibit_.connectStarted
                                                                         : emotibit_.worker->phaseSince ()));
        dmsg = QStringLiteral ("Discovering EmotiBit — %1 s of %2 s elapsed")
                   .arg (el, 0, 'f', 1)
                   .arg (emotibit_.worker->discoveryTimeout (), 0, 'f', 1);
        dcol = Theme::warn;
    }
    else if (failedSlot)
    {
        if (failedSlot->kind == DeviceKind::EmotiBit && failedSlot->failKind == DeviceWorker::FailNotFound)
            dmsg = failedSlot->typedIp.isEmpty ()
                ? QStringLiteral ("EmotiBit not found on %1 — enter IP manually").arg (subnets ())
                : QStringLiteral ("EmotiBit not found at %1 — check the IP address").arg (failedSlot->typedIp);
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
        else // every channel of every streaming device has data at a nominal rate
            dmsg = QStringLiteral ("All streams nominal");
    }
    else
    {
        dmsg = QStringLiteral ("Select a serial port or an EmotiBit address to begin");
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
                    ? (n == 5 ? QStringLiteral ("all five streams are frozen")
                              : QStringLiteral ("all %1 streams are frozen").arg (n))
                    : QStringLiteral ("%1 of %2 streams are frozen").arg (stallSlot->staleCount).arg (n);
                sentences << QStringLiteral ("No %1 samples for %2 s — %3.")
                                 .arg (shortDeviceName (stallSlot->kind))
                                 .arg (stallSlot->stallAge, 0, 'f', 1)
                                 .arg (streams);
            }
        }
        if (nearRail_)
            sentences << (stallSlot ? QStringLiteral ("Cyton Ch1 is also within %1 of the rail; reseat the electrode.")
                                    : QStringLiteral ("Cyton Ch1 is within %1 of the ±187,500 µV rail; reseat the electrode or check contact."))
                             .arg (Readouts::headroomPercent (headroom_));
        const QDateTime since = QDateTime::fromMSecsSinceEpoch (static_cast<qint64> (
            (std::isfinite (warnSinceWall_) ? warnSinceWall_ : now) * 1000.0));
        banner_->setContent (Theme::warn, Theme::warnBack,
            stallSlot ? QStringLiteral ("STREAM STALLED") : QStringLiteral ("NEAR RAIL"),
            sentences.join (QStringLiteral (" ")), QStringLiteral ("since %1").arg (since.toString (QStringLiteral ("HH:mm:ss"))));
    }
    else if (failedSlot)
    {
        const bool nf = failedSlot->kind == DeviceKind::EmotiBit && failedSlot->failKind == DeviceWorker::FailNotFound;
        QString text;
        if (nf)
            text = failedSlot->typedIp.isEmpty ()
                ? QStringLiteral ("EmotiBit did not answer the discovery broadcast. Enter its IP address in the left rail and connect again.")
                : QStringLiteral ("EmotiBit did not answer at %1. Check the address in the left rail and connect again.").arg (failedSlot->typedIp);
        else
            text = QStringLiteral ("%1: %2").arg (shortDeviceName (failedSlot->kind), failedSlot->failText.section ('\n', 0, 0));
        banner_->setContent (Theme::fault, Theme::faultBack,
            nf ? QStringLiteral ("DEVICE NOT FOUND") : QStringLiteral ("CONNECTION FAILED"), text,
            nf ? QStringLiteral ("timeout after %1 s").arg (failedSlot->failTimeout, 0, 'f', 1)
               : QStringLiteral ("after %1 s").arg (failedSlot->failAfter, 0, 'f', 1));
    }
    else if (recordWanted_ && isRecording ())
    {
        const int n = recFiles_.size ();
        banner_->setContent (Theme::fault, Theme::faultBack, QStringLiteral ("RECORDING"),
            QStringLiteral ("Writing %1 CSV file%2 (BrainFlow rows). Streams and plots are unaffected; stop from the "
                            "left rail when the block is finished.")
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

void MainWindow::refreshPorts (bool announce)
{
    const QString current = portCombo_->currentText ().trimmed ();
    QStringList ports;
    const QStringList entries = QDir (QStringLiteral ("/dev"))
                                    .entryList (QStringList {QStringLiteral ("cu.usbserial-*")},
                                        QDir::System | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &e : entries)
        ports << QStringLiteral ("/dev/") + e;
    if (!opts_.cytonPort.isEmpty () && !ports.contains (opts_.cytonPort))
        ports << opts_.cytonPort; // keep the default selectable even when unplugged
    portCombo_->clear ();
    portCombo_->addItems (ports);
    if (ports.size () <= 1)
    {
        portCombo_->addItem (entries.isEmpty () ? QStringLiteral ("no USB serial ports found")
                                                : QStringLiteral ("no other ports found"));
        if (auto *m = qobject_cast<QStandardItemModel *> (portCombo_->model ()))
            m->item (portCombo_->count () - 1)->setEnabled (false);
    }
    const QString want = current.isEmpty () ? opts_.cytonPort : current;
    portCombo_->setCurrentText (want);
    if (portCombo_->lineEdit ())
        portCombo_->lineEdit ()->setCursorPosition (0); // show the start of the path
    if (statusStrip_ && announce)
        showMessage (QStringLiteral ("Found %1 USB serial port(s)").arg (entries.size ()), 3000);
}

void MainWindow::applyFilterSettings ()
{
    const bool dc = dcToggle_->isChecked (), hp = hpToggle_->isChecked (), notch = notchToggle_->isChecked ();
    cytonPlot_->setRemoveMean (dc);
    cytonPlot_->setSymmetric (dc || hp);
    cytonPlot_->setChips ({{dc ? QStringLiteral ("DC REMOVED") : QStringLiteral ("DC REMOVAL OFF"), dc},
        {hp ? QStringLiteral ("HP 1 Hz") : QStringLiteral ("HP 1 Hz OFF"), hp},
        {notch ? QStringLiteral ("NOTCH 60 Hz") : QStringLiteral ("NOTCH 60 Hz OFF"), notch}});
    cyton_.worker->setHighPass (hp, 1.0);
    cyton_.worker->setNotch (notch, 60.0);
    emotibit_.worker->setHighPass (false);
    emotibit_.worker->setNotch (false);
}

void MainWindow::updateWindowTitle ()
{
    setWindowTitle (QStringLiteral ("Cyton + EmotiBit live stream%1")
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
    for (Slot *s : {&cyton_, &emotibit_})
        updateSlotUi (*s);
    updateRecordUi ();
    refreshChrome (now);
}

void MainWindow::onSlowTimer ()
{
    cpu_.sample (); // only here: every CPU reading covers the last ~1 s
    pollRecordingSizes ();
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
