#include "EmotiBitWifiSetup.h"

#include "SerialPorts.h"

#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSerialPort>
#include <QThread>

#include <algorithm>
#include <cstdio>

namespace emotibit_wifi
{

namespace
{

constexpr double kBootSeconds = 40.0;     // reset -> "Wifi Credential edit mode" (normally ~8 s)
constexpr double kResetHintSeconds = 8.0; // no boot output by then: ask for the reset button
constexpr double kCommandSeconds = 6.0;

std::string jsonEscaped (const QString &s)
{
    std::string out;
    for (unsigned char c : s.toUtf8 ())
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += static_cast<char> (c);
        }
        else if (c < 0x20)
        {
            char buf[8];
            std::snprintf (buf, sizeof (buf), "\\u%04x", c);
            out += buf;
        }
        else
            out += static_cast<char> (c);
    }
    return out;
}

void wipe (std::string &s)
{
    std::fill (s.begin (), s.end (), '\0');
    s.clear ();
}

class Session
{
public:
    explicit Session (const Progress &progress) : progress_ (progress)
    {
    }
    ~Session ()
    {
        wipeBuffer ();
        if (port_.isOpen ())
            port_.close ();
    }

    bool open (const QString &name, QString *error)
    {
        port_.setPortName (name);
        port_.setBaudRate (kBaud);
        port_.setDataBits (QSerialPort::Data8);
        port_.setParity (QSerialPort::NoParity);
        port_.setStopBits (QSerialPort::OneStop);
        port_.setFlowControl (QSerialPort::NoFlowControl);
        if (port_.open (QIODevice::ReadWrite))
            return true;
        *error = QStringLiteral ("Cannot open %1 (%2). Close any serial monitor or EmotiBit tool that uses it.")
                     .arg (name, port_.errorString ());
        return false;
    }

    // Resets the EmotiBit and enters the firmware's WiFi config edit mode.
    bool enterSetupMode (QString *error)
    {
        say (QStringLiteral ("Restarting the EmotiBit into Wi-Fi setup mode (about 10 s)…"));
        // ESP32 auto-reset circuit: RTS pulls EN low; DTR stays released, so
        // IO0 is high and the chip boots the firmware, not the bootloader.
        port_.setDataTerminalReady (false);
        port_.setRequestToSend (true);
        QThread::msleep (150);
        port_.setRequestToSend (false);
        port_.clear ();
        wipeBuffer ();

        bool sentC = false, hinted = false, noSd = false;
        QElapsedTimer t;
        t.start ();
        const bool ok = readUntil (
            [&] {
                if (buf_.contains ("Wifi Credential edit mode") || buf_.contains ("@FV,"))
                    return true;
                if (buf_.contains ("SD-Card not detected"))
                {
                    noSd = true;
                    return true;
                }
                // "EmotiBit ready": the boot's first input window has closed;
                // the 'C' waits in the UART buffer for the WiFi config prompt.
                // "…add/update cofig file.": a missing / broken config.txt
                // (the firmware's spelling), where 'C' is read at any time.
                if (!sentC && (buf_.contains ("EmotiBit ready") || buf_.contains ("add/update cofig file")))
                {
                    port_.write ("C");
                    port_.waitForBytesWritten (500);
                    sentC = true;
                }
                if (!hinted && !buf_.contains ("EmotiBit") && t.elapsed () > kResetHintSeconds * 1000.0)
                {
                    hinted = true;
                    say (QStringLiteral ("No answer yet: press the reset button on the EmotiBit's Feather board."));
                }
                return false;
            },
            kBootSeconds);
        const bool sawBoot = buf_.contains ("EmotiBit");
        wipeBuffer ();
        if (noSd)
        {
            *error = QStringLiteral ("The EmotiBit has no SD card (it keeps its Wi-Fi networks on it).");
            return false;
        }
        if (!ok)
        {
            *error = sawBoot
                ? QStringLiteral ("The EmotiBit restarted but did not enter Wi-Fi setup mode.")
                : QStringLiteral ("No EmotiBit on this port answered. Check the USB cable (it must carry data) "
                                  "and that the EmotiBit is switched on.");
            return false;
        }
        return true;
    }

    // Sends one command and waits for its @AK / @NK reply.
    bool command (const std::string &cmd, const QString &tag, QString *reply, QString *error)
    {
        wipeBuffer ();
        port_.write (cmd.data (), static_cast<qint64> (cmd.size ()));
        if (!port_.waitForBytesWritten (2000))
        {
            *error = QStringLiteral ("Could not write to the EmotiBit's serial port.");
            return false;
        }
        const QByteArray ak = "@AK," + tag.toLatin1 () + "~", nk = "@NK," + tag.toLatin1 () + "~";
        if (!readUntil ([&] { return buf_.contains (ak) || buf_.contains (nk); }, kCommandSeconds))
        {
            wipeBuffer ();
            *error = QStringLiteral ("The EmotiBit did not answer (%1).").arg (tag);
            return false;
        }
        *reply = buf_.contains (ak) ? QStringLiteral ("AK") : QStringLiteral ("NK");
        return true;
    }

    bool list (QStringList *ssids, int *budget, QString *error)
    {
        QString reply;
        if (!command ("@LS~", QStringLiteral ("LS"), &reply, error))
            return false;
        const bool noFile = buf_.contains ("config file does not exist");
        if (reply == QLatin1String ("AK"))
            *ssids = parseNetworkList (QString::fromUtf8 (buf_), budget);
        wipeBuffer ();
        if (reply == QLatin1String ("AK"))
            return true;
        if (noFile)
        {
            ssids->clear ();
            *budget = kConfigBase;
            return true;
        }
        *error = QStringLiteral ("The EmotiBit could not read config.txt on its SD card.");
        return false;
    }

    bool remove (int index, QString *error)
    {
        QString reply;
        if (!command ("@WD," + std::to_string (index) + "~", QStringLiteral ("WD"), &reply, error))
            return false;
        wipeBuffer ();
        if (reply == QLatin1String ("AK"))
            return true;
        *error = QStringLiteral ("The EmotiBit could not remove the network.");
        return false;
    }

    // Leaves the edit mode (@RS~ restarts the MCU; the EmotiBit then joins WiFi).
    void restart ()
    {
        if (!port_.isOpen ())
            return;
        wipeBuffer ();
        port_.write ("@RS~");
        port_.waitForBytesWritten (1000);
        readUntil ([&] { return buf_.contains ("@AK,RS~"); }, 3.0);
        wipeBuffer ();
    }

    void say (const QString &m) const
    {
        if (progress_)
            progress_ (m);
    }

private:
    bool readUntil (const std::function<bool ()> &done, double timeoutSec)
    {
        QElapsedTimer t;
        t.start ();
        for (;;)
        {
            if (port_.bytesAvailable () > 0)
                buf_.append (port_.readAll ());
            if (done ())
                return true;
            if (t.elapsed () > timeoutSec * 1000.0)
                return false;
            if (port_.waitForReadyRead (50))
                buf_.append (port_.readAll ());
            if (buf_.size () > 512 * 1024) // boot output is a few kB; never grow without bound
            {
                std::fill (buf_.begin (), buf_.begin () + 256 * 1024, '\0');
                buf_.remove (0, 256 * 1024);
            }
        }
    }

    // the buffer can hold passwords the firmware echoes
    void wipeBuffer ()
    {
        buf_.fill ('\0');
        buf_.clear ();
    }

    QSerialPort port_;
    QByteArray buf_;
    Progress progress_;
};

} // namespace

std::string addCommand (const QString &ssid, const QString &password, QString *error)
{
    auto fail = [error] (const QString &e) {
        if (error)
            *error = e;
        return std::string ();
    };
    const int ssidBytes = ssid.toUtf8 ().size (), passBytes = password.toUtf8 ().size ();
    if (ssid.isEmpty ())
        return fail (QStringLiteral ("Enter the network name (SSID)."));
    if (ssidBytes > 32)
        return fail (QStringLiteral ("A Wi-Fi network name has at most 32 bytes."));
    if (!password.isEmpty () && (passBytes < 8 || passBytes > 64))
        return fail (QStringLiteral ("A WPA2 password has 8 to 63 characters (leave it empty for an open network)."));
    if (ssid.contains (QLatin1Char ('~')) || password.contains (QLatin1Char ('~')))
        return fail (QStringLiteral ("The EmotiBit firmware cannot take a '~' in the network name or password."));
    return "@WA,{\"ssid\":\"" + jsonEscaped (ssid) + "\",\"password\":\"" + jsonEscaped (password) + "\"}~";
}

QStringList parseNetworkList (const QString &serialText, int *budgetUsed)
{
    QStringList out;
    int budget = kConfigBase;
    const int start = serialText.indexOf (QStringLiteral ("config file credentials:"));
    if (start >= 0)
    {
        int end = serialText.indexOf (QStringLiteral ("@AK,LS~"), start);
        if (end < 0)
            end = serialText.size ();
        static const QRegularExpression entry (QStringLiteral ("^(\\d+)\\. (.*)$"));
        for (QString line : serialText.mid (start, end - start).split (QLatin1Char ('\n')))
        {
            line.remove (QLatin1Char ('\r'));
            const QRegularExpressionMatch m = entry.match (line);
            if (!m.hasMatch ())
                continue;
            // "<i>. <ssid> : <password>": keep the SSID, only count the password
            const QString rest = m.captured (2);
            const int sep = rest.indexOf (QStringLiteral (" : "));
            const QString ssid = sep >= 0 ? rest.left (sep) : rest;
            const int passBytes = sep >= 0 ? rest.mid (sep + 3).toUtf8 ().size () : 0;
            out << ssid;
            budget += kPerNetwork + ssid.toUtf8 ().size () + passBytes;
        }
    }
    if (budgetUsed)
        *budgetUsed = budget;
    return out;
}

QString replyFor (const QString &serialText, const QString &typetag)
{
    if (serialText.contains (QStringLiteral ("@AK,") + typetag + QLatin1Char ('~')))
        return QStringLiteral ("AK");
    if (serialText.contains (QStringLiteral ("@NK,") + typetag + QLatin1Char ('~')))
        return QStringLiteral ("NK");
    return QString ();
}

Result listNetworks (const QString &port, const Progress &progress)
{
    Result r;
    Session s (progress);
    int budget = 0;
    if (!s.open (port, &r.error))
        return r;
    if (s.enterSetupMode (&r.error))
        r.ok = s.list (&r.networks, &budget, &r.error);
    s.say (QStringLiteral ("Restarting the EmotiBit…"));
    s.restart ();
    return r;
}

Result addNetwork (const QString &port, const QString &ssid, const QString &password, const Progress &progress)
{
    Result r;
    std::string cmd = addCommand (ssid, password, &r.error);
    if (cmd.empty ())
        return r;
    Session s (progress);
    auto finish = [&] {
        wipe (cmd);
        s.say (QStringLiteral ("Restarting the EmotiBit…"));
        s.restart ();
        return r;
    };
    if (!s.open (port, &r.error) || !s.enterSetupMode (&r.error))
        return finish ();

    QStringList before;
    int budget = 0;
    if (!s.list (&before, &budget, &r.error))
        return finish ();
    // an entry with the same name is replaced after the new one is saved
    int sameName = 0;
    for (const QString &n : before)
        sameName += n == ssid ? 1 : 0;
    if (before.size () - sameName >= kMaxNetworks)
    {
        r.error = QStringLiteral ("The EmotiBit already has %1 saved networks, its maximum. Remove one first.")
                      .arg (kMaxNetworks);
        return finish ();
    }
    if (before.size () >= kMaxNetworks ||
        budget + kPerNetwork + ssid.toUtf8 ().size () + password.toUtf8 ().size () > kConfigBudget)
    {
        r.error = QStringLiteral ("The EmotiBit's config file has no room for another network (its firmware reads "
                                  "about 1 kB of it). Remove a network first.");
        return finish ();
    }

    s.say (QStringLiteral ("Saving \"%1\" on the EmotiBit…").arg (ssid));
    QString reply;
    if (!s.command (cmd, QStringLiteral ("WA"), &reply, &r.error))
        return finish ();
    wipe (cmd);
    if (reply != QLatin1String ("AK"))
    {
        r.error = QStringLiteral ("The EmotiBit refused the network (it could not write config.txt on its SD card).");
        return finish ();
    }
    // An ACK is not proof: the firmware drops an entry that doesn't fit its
    // JSON buffer and still acknowledges. The new entry is the last one.
    QStringList after;
    if (!s.list (&after, &budget, &r.error))
        return finish ();
    if (after.size () != before.size () + 1 || after.last () != ssid)
    {
        r.error = QStringLiteral ("The EmotiBit acknowledged the network, but it is not in its saved list.");
        r.networks = after;
        return finish ();
    }
    for (int i = after.size () - 2; i >= 0; --i)
    {
        if (after[i] != ssid)
            continue;
        s.say (QStringLiteral ("Removing the older entry for \"%1\"…").arg (ssid));
        if (!s.remove (i, &r.error) || !s.list (&after, &budget, &r.error))
        {
            r.networks = after;
            return finish ();
        }
    }
    r.networks = after;
    r.ok = true;
    return finish ();
}

Result removeNetwork (const QString &port, int index, const QString &expectedSsid, const Progress &progress)
{
    Result r;
    Session s (progress);
    auto finish = [&] {
        s.say (QStringLiteral ("Restarting the EmotiBit…"));
        s.restart ();
        return r;
    };
    if (!s.open (port, &r.error) || !s.enterSetupMode (&r.error))
        return finish ();
    QStringList before;
    int budget = 0;
    if (!s.list (&before, &budget, &r.error))
        return finish ();
    r.networks = before;
    if (index < 0 || index >= before.size () || before[index] != expectedSsid)
    {
        r.error = QStringLiteral ("The EmotiBit's saved networks have changed. Read them again.");
        return finish ();
    }
    if (before.size () == 1)
    {
        r.error = QStringLiteral ("This is the EmotiBit's only saved network. An EmotiBit without a network does not "
                                  "finish starting up: add another network first.");
        return finish ();
    }
    s.say (QStringLiteral ("Removing \"%1\"…").arg (expectedSsid));
    if (!s.remove (index, &r.error) || !s.list (&r.networks, &budget, &r.error))
        return finish ();
    r.ok = true;
    return finish ();
}

QStringList candidatePorts ()
{
    // USB-UART bridges on Feather ESP32 boards: Silicon Labs CP210x (HUZZAH32),
    // WCH CH34x / CH9102 (HUZZAH32 V2), Adafruit, Espressif native USB.
    static const quint16 kVendors[] = {0x10C4, 0x1A86, 0x239A, 0x303A};
    QStringList likely, other;
    for (const SerialPortEntry &e : listSerialPorts ())
    {
        if (e.cytonDongle || e.vid == 0)
            continue;
        const bool known = std::find (std::begin (kVendors), std::end (kVendors), e.vid) != std::end (kVendors);
        (known ? likely : other) << brainflowSerialPort (e);
    }
    return likely + other;
}

} // namespace emotibit_wifi
