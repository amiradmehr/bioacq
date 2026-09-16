#pragma once

#include "EmotiBitWifiSetup.h"

#include <QDialog>

#include <functional>
#include <thread>

class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class RescanButton;

// Edits the WiFi networks the EmotiBit joins, over USB (EmotiBitWifiSetup):
// read the saved networks, add one (this computer's or a phone's hotspot, a
// travel router), remove one. Every action restarts the EmotiBit, so
// MainWindow opens this only while the EmotiBit is not connected, and the
// dialog can't be closed while an action runs. Actions run on a std::thread;
// results come back queued. The password is never logged, and the field is
// cleared once a network has been saved.
class EmotiBitWifiDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EmotiBitWifiDialog (QWidget *parent = nullptr);
    ~EmotiBitWifiDialog () override;

protected:
    void closeEvent (QCloseEvent *event) override;
    void reject () override;

private:
    using Op = std::function<emotibit_wifi::Result (const QString &port, const emotibit_wifi::Progress &)>;

    void rescanPorts ();
    void readNetworks ();
    void addNetwork ();
    void removeNetwork ();
    void run (const QString &startText, const QString &okText, Op op, bool clearPasswordOnSuccess = false);
    void finish (const emotibit_wifi::Result &r, const QString &okText, bool clearPassword);
    void setBusy (bool busy);
    void setStatus (const QString &text, const QColor &color);
    void updateButtons ();

    QComboBox *portCombo_ = nullptr;
    RescanButton *rescanBtn_ = nullptr;
    QListWidget *list_ = nullptr;
    QPushButton *readBtn_ = nullptr;
    QPushButton *removeBtn_ = nullptr;
    QLineEdit *ssidEdit_ = nullptr;
    QLineEdit *passEdit_ = nullptr;
    QPushButton *addBtn_ = nullptr;
    QPushButton *closeBtn_ = nullptr;
    QLabel *status_ = nullptr;
    std::thread thread_;
    bool busy_ = false;
};
