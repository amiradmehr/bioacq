#include "BluetoothAccess.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QPermissions>

#ifdef Q_OS_MACOS
#include <unistd.h>
#endif

namespace bluetooth_access
{

namespace
{

#if QT_CONFIG(permissions)
QBluetoothPermission permission ()
{
    QBluetoothPermission p;
    p.setCommunicationModes (QBluetoothPermission::Access);
    return p;
}

Status fromQt (Qt::PermissionStatus s)
{
    switch (s)
    {
    case Qt::PermissionStatus::Granted:
        return Status::Granted;
    case Qt::PermissionStatus::Undetermined:
        return Status::Undetermined;
    case Qt::PermissionStatus::Denied:
        break;
    }
    return Status::Denied;
}
#endif

bool mayUseBluetooth (QString *why)
{
#ifdef Q_OS_MACOS
    // launchd is the parent of an app opened through LaunchServices
    if (getppid () != 1 && !qEnvironmentVariableIsSet ("BIOACQ_ALLOW_BLUETOOTH"))
    {
        if (why)
            *why = QStringLiteral ("Bluetooth works when BioAcq.app is opened from Finder or the Dock (macOS ties "
                                   "Bluetooth access to the app that started it)");
        return false;
    }
#else
    Q_UNUSED (why);
#endif
    return true;
}

QString deniedText ()
{
#ifdef Q_OS_MACOS
    return QStringLiteral ("Bluetooth access is off for BioAcq: turn it on in System Settings → Privacy & Security → "
                           "Bluetooth");
#else
    return QStringLiteral ("Bluetooth access is not allowed for BioAcq");
#endif
}

} // namespace

Status check (QString *why)
{
    if (!mayUseBluetooth (why))
        return Status::Unavailable;
#if QT_CONFIG(permissions)
    const Status s = fromQt (qApp->checkPermission (permission ()));
    if (s == Status::Denied && why)
        *why = deniedText ();
    else if (s == Status::Undetermined && why)
        *why = QStringLiteral ("Bluetooth permission not asked yet");
    return s;
#else
    return Status::Granted;
#endif
}

void request (QObject *context, const std::function<void (Status)> &done)
{
    const Status now = check ();
    if (now != Status::Undetermined)
    {
        done (now);
        return;
    }
#if QT_CONFIG(permissions)
    qApp->requestPermission (permission (), context, [done] (const QPermission &p) { done (fromQt (p.status ())); });
#else
    done (Status::Granted);
#endif
}

Status requestBlocking ()
{
    if (check () != Status::Undetermined)
        return check ();
    QEventLoop loop;
    QObject ctx;
    Status out = Status::Undetermined;
    request (&ctx, [&] (Status s) {
        out = s;
        loop.quit ();
    });
    if (out == Status::Undetermined)
        loop.exec ();
    return out;
}

} // namespace bluetooth_access
