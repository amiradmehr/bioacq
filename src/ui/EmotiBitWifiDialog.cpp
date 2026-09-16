#include "EmotiBitWifiDialog.h"

#include "Theme.h"
#include "Widgets.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace
{

constexpr int kDialogWidth = 480;
constexpr int kContentWidth = kDialogWidth - 40; // 20 px margins

QLabel *label (const QString &text, const QFont &font, const QColor &color, bool wrap = false)
{
    auto *l = new QLabel (text);
    l->setFont (font);
    Theme::setTextColor (l, color);
    l->setWordWrap (wrap);
    if (wrap && !text.isEmpty ())
    {
        // a wrapped label in a fixed-width dialog: reserve its full height
        l->ensurePolished ();
        l->setMinimumHeight (l->heightForWidth (kContentWidth));
    }
    return l;
}

QLabel *kicker (const QString &text)
{
    return label (text, Theme::mono (9, 400, 0.14), Theme::textDim);
}

QPushButton *button (const QString &text, const char *variant)
{
    auto *b = new QPushButton (text);
    b->setProperty ("variant", variant);
    b->setFixedHeight (28);
    b->setCursor (Qt::PointingHandCursor);
    b->setFocusPolicy (Qt::TabFocus);
    return b;
}

} // namespace

EmotiBitWifiDialog::EmotiBitWifiDialog (QWidget *parent) : QDialog (parent)
{
    setWindowTitle (QStringLiteral ("EmotiBit Wi-Fi"));
    setModal (true);
    setFixedWidth (kDialogWidth);
    setAutoFillBackground (true);
    QPalette pal = palette ();
    pal.setColor (QPalette::Window, Theme::panel);
    setPalette (pal);

    auto *v = new QVBoxLayout (this);
    v->setContentsMargins (20, 18, 20, 18);
    v->setSpacing (0);
    v->addWidget (kicker (QStringLiteral ("// EMOTIBIT · WI-FI OVER USB")));
    v->addSpacing (8);
    v->addWidget (label (QStringLiteral ("Networks the EmotiBit joins"), Theme::mono (15, 500), Theme::textStrong));
    v->addSpacing (8);
    v->addWidget (label (QStringLiteral (
                             "Connect the EmotiBit to this computer with a USB data cable. Save the network you want "
                             "it to use once (this computer's or your phone's hotspot works anywhere), and Connect "
                             "finds the EmotiBit on it. Each action restarts the EmotiBit (about 10 s)."),
        Theme::sans (11.5), Theme::textMuted, true));
    v->addSpacing (14);

    v->addWidget (kicker (QStringLiteral ("// USB PORT")));
    v->addSpacing (5);
    auto *pr = new QHBoxLayout;
    pr->setSpacing (6);
    portCombo_ = new QComboBox;
    portCombo_->setEditable (true);
    portCombo_->setFixedHeight (30);
    portCombo_->setToolTip (QStringLiteral (
        "The EmotiBit Feather's USB serial port (a CP210x or CH340 USB-UART). The Cyton's dongle is not listed."));
    rescanBtn_ = new RescanButton;
    rescanBtn_->setToolTip (QStringLiteral ("Rescan USB serial ports"));
    pr->addWidget (portCombo_, 1);
    pr->addWidget (rescanBtn_);
    v->addLayout (pr);
    v->addSpacing (14);

    v->addWidget (kicker (QStringLiteral ("// SAVED NETWORKS · TRIED IN THIS ORDER, UP TO 20 S EACH")));
    v->addSpacing (5);
    list_ = new QListWidget;
    list_->setFixedHeight (120);
    list_->setFont (Theme::mono (11));
    list_->setStyleSheet (QStringLiteral (
                              "QListWidget { background: %1; border: 1px solid %2; color: %3; outline: 0; }"
                              "QListWidget::item { padding: 4px 8px; }"
                              "QListWidget::item:selected { background: %4; color: %5; }")
                              .arg (Theme::inputFill.name (), Theme::controlEdge.name (), Theme::textStrong.name (),
                                  Theme::action.name (), Theme::onAction.name ()));
    v->addWidget (list_);
    v->addSpacing (6);
    auto *lr = new QHBoxLayout;
    lr->setSpacing (6);
    readBtn_ = button (QStringLiteral ("[ read saved networks ]"), "secondary");
    removeBtn_ = button (QStringLiteral ("[ remove selected ]"), "ghost");
    lr->addWidget (readBtn_);
    lr->addWidget (removeBtn_);
    lr->addStretch (1);
    v->addLayout (lr);
    v->addSpacing (16);

    v->addWidget (kicker (QStringLiteral ("// ADD A NETWORK")));
    v->addSpacing (5);
    ssidEdit_ = new QLineEdit;
    ssidEdit_->setFixedHeight (30);
    ssidEdit_->setPlaceholderText (QStringLiteral ("network name (SSID)"));
    passEdit_ = new QLineEdit;
    passEdit_->setFixedHeight (30);
    passEdit_->setEchoMode (QLineEdit::Password);
    passEdit_->setPlaceholderText (QStringLiteral ("password"));
    v->addWidget (ssidEdit_);
    v->addSpacing (6);
    v->addWidget (passEdit_);
    v->addSpacing (6);
    auto *ar = new QHBoxLayout;
    addBtn_ = button (QStringLiteral ("[ add network ]"), "primary");
    ar->addWidget (addBtn_);
    ar->addStretch (1);
    v->addLayout (ar);
    v->addSpacing (10);
    v->addWidget (label (QStringLiteral (
                             "2.4 GHz WPA2-Personal networks only (not eduroam or other logins). iPhone hotspot: turn "
                             "on Maximize Compatibility. A Mac can share Wi-Fi only from another connection "
                             "(Ethernet or a USB-tethered phone); Windows Mobile Hotspot works on its own "
                             "(set its band to 2.4 GHz)."),
        Theme::sans (10.5), Theme::textDim, true));
    v->addSpacing (14);

    status_ = label (QString (), Theme::mono (10.5), Theme::textMuted, true);
    status_->setMinimumHeight (32);
    v->addWidget (status_);
    v->addSpacing (10);
    auto *br = new QHBoxLayout;
    br->addStretch (1);
    closeBtn_ = button (QStringLiteral ("[ close ]"), "secondary");
    br->addWidget (closeBtn_);
    v->addLayout (br);

    connect (rescanBtn_, &QPushButton::clicked, this, [this] { rescanPorts (); });
    connect (readBtn_, &QPushButton::clicked, this, [this] { readNetworks (); });
    connect (removeBtn_, &QPushButton::clicked, this, [this] { removeNetwork (); });
    connect (addBtn_, &QPushButton::clicked, this, [this] { addNetwork (); });
    connect (passEdit_, &QLineEdit::returnPressed, this, [this] { addNetwork (); });
    connect (closeBtn_, &QPushButton::clicked, this, [this] { reject (); });
    connect (list_, &QListWidget::currentRowChanged, this, [this] (int) { updateButtons (); });
    connect (portCombo_, &QComboBox::currentTextChanged, this, [this] (const QString &) { updateButtons (); });

    rescanPorts ();
    setStatus (portCombo_->count () > 0
            ? QStringLiteral ("Read the saved networks, or add one.")
            : QStringLiteral ("No EmotiBit USB port found: plug it in with a USB data cable, then rescan."),
        portCombo_->count () > 0 ? Theme::textMuted : Theme::warn);
    adjustSize (); // height for the wrapped paragraphs at the fixed width
}

EmotiBitWifiDialog::~EmotiBitWifiDialog ()
{
    if (thread_.joinable ())
        thread_.join ();
}

void EmotiBitWifiDialog::closeEvent (QCloseEvent *event)
{
    if (busy_)
    {
        event->ignore ();
        setStatus (QStringLiteral ("Wait until the EmotiBit has finished restarting."), Theme::warn);
        return;
    }
    QDialog::closeEvent (event);
}

void EmotiBitWifiDialog::reject ()
{
    if (busy_)
    {
        setStatus (QStringLiteral ("Wait until the EmotiBit has finished restarting."), Theme::warn);
        return;
    }
    QDialog::reject ();
}

void EmotiBitWifiDialog::rescanPorts ()
{
    const QString current = portCombo_->currentText ();
    const QStringList ports = emotibit_wifi::candidatePorts ();
    portCombo_->clear ();
    portCombo_->addItems (ports);
    if (!current.isEmpty () && ports.contains (current))
        portCombo_->setCurrentText (current);
    updateButtons ();
}

void EmotiBitWifiDialog::readNetworks ()
{
    run (QStringLiteral ("Reading the saved networks…"), QStringLiteral ("These are the networks saved on the EmotiBit."),
        [] (const QString &port, const emotibit_wifi::Progress &progress) {
            return emotibit_wifi::listNetworks (port, progress);
        });
}

void EmotiBitWifiDialog::addNetwork ()
{
    const QString ssid = ssidEdit_->text ();
    const QString password = passEdit_->text ();
    QString error;
    std::string probe = emotibit_wifi::addCommand (ssid, password, &error);
    std::fill (probe.begin (), probe.end (), '\0');
    if (probe.empty () && !error.isEmpty ())
    {
        setStatus (error, Theme::fault);
        return;
    }
    run (QStringLiteral ("Saving \"%1\" on the EmotiBit…").arg (ssid),
        QStringLiteral ("Saved \"%1\". The EmotiBit is restarting and joins the first saved network in range; "
                        "then press Connect.")
            .arg (ssid),
        [ssid, password] (const QString &port, const emotibit_wifi::Progress &progress) {
            return emotibit_wifi::addNetwork (port, ssid, password, progress);
        },
        true);
}

void EmotiBitWifiDialog::removeNetwork ()
{
    const int row = list_->currentRow ();
    if (row < 0)
        return;
    const QString ssid = list_->currentItem ()->data (Qt::UserRole).toString ();
    if (QMessageBox::question (this, QStringLiteral ("Remove network"),
            QStringLiteral ("Remove \"%1\" from the EmotiBit?").arg (ssid)) != QMessageBox::Yes)
        return;
    run (QStringLiteral ("Removing \"%1\"…").arg (ssid), QStringLiteral ("Removed \"%1\".").arg (ssid),
        [row, ssid] (const QString &port, const emotibit_wifi::Progress &progress) {
            return emotibit_wifi::removeNetwork (port, row, ssid, progress);
        });
}

void EmotiBitWifiDialog::run (const QString &startText, const QString &okText, Op op, bool clearPasswordOnSuccess)
{
    const QString port = portCombo_->currentText ().trimmed ();
    if (port.isEmpty () || busy_)
        return;
    if (thread_.joinable ())
        thread_.join ();
    setBusy (true);
    setStatus (startText, Theme::warn);
    QPointer<EmotiBitWifiDialog> self (this);
    thread_ = std::thread ([self, port, op, okText, clearPasswordOnSuccess] {
        const emotibit_wifi::Progress progress = [self] (const QString &m) {
            QMetaObject::invokeMethod (
                self.data (), [self, m] {
                    if (self)
                        self->setStatus (m, Theme::warn);
                },
                Qt::QueuedConnection);
        };
        const emotibit_wifi::Result r = op (port, progress);
        QMetaObject::invokeMethod (
            self.data (), [self, r, okText, clearPasswordOnSuccess] {
                if (self)
                    self->finish (r, okText, clearPasswordOnSuccess);
            },
            Qt::QueuedConnection);
    });
}

void EmotiBitWifiDialog::finish (const emotibit_wifi::Result &r, const QString &okText, bool clearPassword)
{
    if (r.ok || !r.networks.isEmpty ())
    {
        list_->clear ();
        for (int i = 0; i < r.networks.size (); ++i)
        {
            auto *item = new QListWidgetItem (QStringLiteral ("%1. %2").arg (i + 1).arg (r.networks[i]));
            item->setData (Qt::UserRole, r.networks[i]);
            list_->addItem (item);
        }
    }
    if (r.ok && clearPassword)
        passEdit_->clear ();
    setBusy (false);
    setStatus (r.ok ? okText : r.error, r.ok ? Theme::ok : Theme::fault);
}

void EmotiBitWifiDialog::setBusy (bool busy)
{
    busy_ = busy;
    updateButtons ();
}

void EmotiBitWifiDialog::setStatus (const QString &text, const QColor &color)
{
    status_->setText (text);
    Theme::setTextColor (status_, color);
}

void EmotiBitWifiDialog::updateButtons ()
{
    const bool port = !portCombo_->currentText ().trimmed ().isEmpty ();
    readBtn_->setEnabled (!busy_ && port);
    addBtn_->setEnabled (!busy_ && port);
    removeBtn_->setEnabled (!busy_ && port && list_->currentRow () >= 0 && list_->count () > 1);
    closeBtn_->setEnabled (!busy_);
    portCombo_->setEnabled (!busy_);
    rescanBtn_->setEnabled (!busy_);
    list_->setEnabled (!busy_);
    ssidEdit_->setEnabled (!busy_);
    passEdit_->setEnabled (!busy_);
}
