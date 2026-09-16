#include "SerialPorts.h"

#include <QFileInfo>
#include <QSerialPortInfo>

#include <algorithm>

namespace
{

bool looksLikeCyton (const QSerialPortInfo &info)
{
    if (info.hasVendorIdentifier () && info.hasProductIdentifier () &&
        info.vendorIdentifier () == kFtdiVendorId && info.productIdentifier () == kFt231xProductId)
        return true;
    const QString text = info.description () + QLatin1Char (' ') + info.manufacturer ();
    return text.contains (QLatin1String ("FT231X"), Qt::CaseInsensitive) ||
        text.contains (QLatin1String ("OpenBCI"), Qt::CaseInsensitive);
}

} // namespace

QVector<SerialPortEntry> listSerialPorts ()
{
    const QList<QSerialPortInfo> infos = QSerialPortInfo::availablePorts ();
    QVector<SerialPortEntry> out;
    for (const QSerialPortInfo &info : infos)
    {
#ifdef Q_OS_MACOS
        const QString name = info.portName ();
        if (name.startsWith (QLatin1String ("tty.")))
        {
            const QString cu = QStringLiteral ("cu.") + name.mid (4);
            const bool hasCu = std::any_of (infos.begin (), infos.end (),
                [&cu] (const QSerialPortInfo &other) { return other.portName () == cu; });
            if (hasCu)
                continue;
        }
#endif
        SerialPortEntry e;
        e.portName = info.portName ();
        e.systemLocation = info.systemLocation ();
        e.vid = info.hasVendorIdentifier () ? info.vendorIdentifier () : 0;
        e.pid = info.hasProductIdentifier () ? info.productIdentifier () : 0;
        e.description = info.description ();
        e.cytonDongle = looksLikeCyton (info);
        out.push_back (e);
    }
    std::stable_sort (out.begin (), out.end (),
        [] (const SerialPortEntry &a, const SerialPortEntry &b) { return a.cytonDongle && !b.cytonDongle; });
    return out;
}

QString brainflowSerialPort (const SerialPortEntry &entry)
{
#ifdef Q_OS_WIN
    return entry.portName;
#else
    return entry.systemLocation;
#endif
}

QString findCytonDongle ()
{
    const QVector<SerialPortEntry> ports = listSerialPorts ();
    for (const SerialPortEntry &e : ports)
        if (e.cytonDongle)
            return brainflowSerialPort (e);
    return QString ();
}

QString matchPortName (const QString &port, const QVector<SerialPortEntry> &ports)
{
    QString name = port.trimmed ();
    if (name.startsWith (QLatin1String ("\\\\.\\")))
        name = name.mid (4);
    if (name.isEmpty ())
        return QString ();
    for (const SerialPortEntry &e : ports)
        if (e.portName.compare (name, Qt::CaseInsensitive) == 0)
            return e.portName;
    return QString ();
}

QString existingSerialPort (const QString &port)
{
#ifdef Q_OS_WIN
    return matchPortName (port, listSerialPorts ());
#else
    const QString p = port.trimmed ();
    return !p.isEmpty () && QFileInfo::exists (p) ? p : QString ();
#endif
}

bool serialPortExists (const QString &port)
{
    return !existingSerialPort (port).isEmpty ();
}

bool sameSerialPort (const QString &a, const QString &b)
{
#ifdef Q_OS_WIN
    return a.compare (b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}
