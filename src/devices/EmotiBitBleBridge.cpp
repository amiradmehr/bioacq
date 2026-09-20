#include "EmotiBitBleBridge.h"

#include "BluetoothAccess.h"

#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QBluetoothUuid>
#include <QHostAddress>
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QNetworkDatagram>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>

#include <cmath>

namespace
{

constexpr int kMaxDatagram = 16 * 1024;  // loopback: keep datagrams well under 64 kB
constexpr int kMaxLineBuffer = 64 * 1024; // no '\n' in this much data: garbage, drop it
constexpr int kReconnectMs = 1500;

} // namespace

// Lives on the bridge thread; every member is touched only there.
class EmotiBitBleBridge::Impl : public QObject
{
public:
    Impl (std::shared_ptr<Shared> shared, Source source, QString preferred, double scanTimeoutSec)
        : shared_ (std::move (shared)), source_ (source), preferred_ (std::move (preferred)),
          scanTimeoutSec_ (scanTimeoutSec)
    {
    }

    void begin ()
    {
        adv_ = new QUdpSocket (this);
        if (!adv_->bind (QHostAddress::LocalHost, kLoopbackPort))
        {
            fail (QStringLiteral ("cannot listen on 127.0.0.1:%1 (%2); close EmotiBit Oscilloscope or another "
                                  "BioAcq that uses the EmotiBit")
                      .arg (kLoopbackPort)
                      .arg (adv_->errorString ()));
            return;
        }
        connect (adv_, &QUdpSocket::readyRead, this, [this] { onAdvertising (); });

        if (source_ == Source::Synthetic)
        {
            setName (QStringLiteral ("EmotiBit: SYNTHETIC-BLE"));
            shared_->linkUp.store (true);
            setState (Ready, QStringLiteral ("synthetic EmotiBit packets"));
            synth_ = new QTimer (this);
            synth_->setInterval (100); // the firmware's DATA_SEND_INTERVAL
            connect (synth_, &QTimer::timeout, this, [this] { generateSynthetic (); });
            synth_->start ();
            return;
        }

        setState (Scanning, QStringLiteral ("scanning for an EmotiBit over Bluetooth…"));
        agent_ = new QBluetoothDeviceDiscoveryAgent (this);
        agent_->setLowEnergyDiscoveryTimeout (static_cast<int> (std::lround (scanTimeoutSec_ * 1000.0)));
        connect (agent_, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this,
            [this] (const QBluetoothDeviceInfo &info) { onDevice (info); });
        connect (agent_, &QBluetoothDeviceDiscoveryAgent::deviceUpdated, this,
            [this] (const QBluetoothDeviceInfo &info, QBluetoothDeviceInfo::Fields) { onDevice (info); });
        connect (agent_, &QBluetoothDeviceDiscoveryAgent::errorOccurred, this,
            [this] (QBluetoothDeviceDiscoveryAgent::Error e) {
                if (shared_->state.load () != Scanning)
                    return;
                if (e == QBluetoothDeviceDiscoveryAgent::MissingPermissionsError)
                    fail (QStringLiteral ("Bluetooth access is not allowed for this app"));
                else if (e == QBluetoothDeviceDiscoveryAgent::PoweredOffError)
                    fail (QStringLiteral ("Bluetooth is turned off") + bluetooth_access::stackHint ());
                else
                    fail (QStringLiteral ("Bluetooth scan failed: %1").arg (agent_->errorString ()) +
                        bluetooth_access::stackHint ());
            });
        connect (agent_, &QBluetoothDeviceDiscoveryAgent::finished, this, [this] {
            if (shared_->state.load () != Scanning)
                return;
            if (candidate_.isValid ())
                connectTo (candidate_);
            else
            {
                shared_->notFound.store (true);
                fail (QStringLiteral ("no EmotiBit advertising over Bluetooth (is it on the bioacq BLE firmware "
                                      "and in Bluetooth mode?)"));
            }
        });
        agent_->start (QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
    }

    void end ()
    {
        stopping_ = true;
        if (synth_)
            synth_->stop ();
        if (agent_)
            agent_->stop ();
        if (ctrl_)
            ctrl_->disconnectFromDevice ();
        if (ctl_)
            ctl_->abort ();
        if (adv_)
            adv_->close ();
        shared_->linkUp.store (false);
        if (shared_->state.load () != Failed)
            shared_->state.store (Stopped);
    }

private:
    void setState (State s, const QString &status)
    {
        {
            std::lock_guard<std::mutex> lock (shared_->mutex);
            shared_->status = status;
        }
        shared_->state.store (s);
    }

    void setStatus (const QString &status)
    {
        std::lock_guard<std::mutex> lock (shared_->mutex);
        shared_->status = status;
    }

    void setName (const QString &name)
    {
        std::lock_guard<std::mutex> lock (shared_->mutex);
        shared_->name = name;
    }

    void fail (const QString &error)
    {
        {
            std::lock_guard<std::mutex> lock (shared_->mutex);
            shared_->error = error;
            shared_->status = error;
        }
        shared_->linkUp.store (false);
        shared_->state.store (Failed);
        if (agent_)
            agent_->stop ();
    }

    void onDevice (const QBluetoothDeviceInfo &info)
    {
        if (shared_->state.load () != Scanning ||
            !(info.coreConfigurations () & QBluetoothDeviceInfo::LowEnergyCoreConfiguration))
            return;
        const QString name = info.name ();
        if (!name.startsWith (QLatin1String ("EmotiBit")))
            return;
        if (preferred_.isEmpty () || name == preferred_)
        {
            agent_->stop ();
            connectTo (info);
        }
        else if (!candidate_.isValid ())
        {
            candidate_ = info; // another EmotiBit: taken if the preferred one never shows up
        }
    }

    void connectTo (const QBluetoothDeviceInfo &info)
    {
        setName (info.name ());
        setState (Connecting, QStringLiteral ("connecting to %1 over Bluetooth…").arg (info.name ()));
        ctrl_ = QLowEnergyController::createCentral (info, this);
        connect (ctrl_, &QLowEnergyController::connected, this, [this] {
            setStatus (QStringLiteral ("reading the EmotiBit's Bluetooth services…"));
            ctrl_->discoverServices ();
        });
        connect (ctrl_, &QLowEnergyController::discoveryFinished, this, [this] { openService (); });
        connect (ctrl_, &QLowEnergyController::disconnected, this, [this] { onLinkLost (); });
        connect (ctrl_, &QLowEnergyController::errorOccurred, this, [this] (QLowEnergyController::Error) {
            if (shared_->state.load () == Connecting)
                fail (QStringLiteral ("Bluetooth connection failed: %1").arg (ctrl_->errorString ()));
            else
                onLinkLost ();
        });
        ctrl_->connectToDevice ();
    }

    void openService ()
    {
        const QBluetoothUuid serviceUuid (QString::fromLatin1 (kServiceUuid));
        if (!ctrl_->services ().contains (serviceUuid))
        {
            fail (QStringLiteral ("%1 has no EmotiBit data service: is it running the bioacq BLE firmware?")
                      .arg (ctrl_->remoteName ()));
            return;
        }
        if (svc_)
            svc_->deleteLater ();
        svc_ = ctrl_->createServiceObject (serviceUuid, this);
        if (!svc_)
        {
            fail (QStringLiteral ("cannot open the EmotiBit's Bluetooth data service"));
            return;
        }
        connect (svc_, &QLowEnergyService::stateChanged, this, [this] (QLowEnergyService::ServiceState s) {
            if (s == QLowEnergyService::RemoteServiceDiscovered)
                subscribe ();
        });
        connect (svc_, &QLowEnergyService::characteristicChanged, this,
            [this] (const QLowEnergyCharacteristic &, const QByteArray &value) { onBytes (value); });
        connect (svc_, &QLowEnergyService::descriptorWritten, this,
            [this] (const QLowEnergyDescriptor &, const QByteArray &) {
                shared_->linkUp.store (true);
                setState (Ready, QStringLiteral ("streaming over Bluetooth"));
            });
        svc_->discoverDetails ();
    }

    void subscribe ()
    {
        const QLowEnergyCharacteristic tx = svc_->characteristic (QBluetoothUuid (QString::fromLatin1 (kTxUuid)));
        const QLowEnergyDescriptor cccd =
            tx.descriptor (QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
        if (!tx.isValid () || !cccd.isValid ())
        {
            fail (QStringLiteral ("the EmotiBit's Bluetooth service has no data characteristic"));
            return;
        }
        svc_->writeDescriptor (cccd, QLowEnergyCharacteristic::CCCDEnableNotification);
    }

    void onLinkLost ()
    {
        if (stopping_ || shared_->state.load () == Failed)
            return;
        if (shared_->state.load () != Ready)
        {
            fail (QStringLiteral ("the Bluetooth link closed while connecting"));
            return;
        }
        shared_->linkUp.store (false);
        setStatus (QStringLiteral ("Bluetooth link lost, reconnecting…"));
        lineBuf_.clear ();
        QTimer::singleShot (kReconnectMs, this, [this] {
            if (!stopping_ && ctrl_ && ctrl_->state () == QLowEnergyController::UnconnectedState)
                ctrl_->connectToDevice ();
        });
    }

    // BrainFlow's side of the Wi-Fi protocol, on 127.0.0.1:3131.
    void onAdvertising ()
    {
        while (adv_->hasPendingDatagrams ())
        {
            const QNetworkDatagram d = adv_->receiveDatagram ();
            for (const QByteArray &line : d.data ().split ('\n'))
            {
                const QList<QByteArray> f = line.trimmed ().split (',');
                if (f.size () < 6)
                    continue;
                const QByteArray &tag = f[3];
                if (tag == "HE")
                {
                    // HELLO_HOST as the firmware sends it: DP -1 (no host yet), DI <id>
                    const QByteArray hh = QByteArray::number (++hostPackets_) + ",0,0,HH,1,100,DP,-1,DI," +
                        deviceIdFromName (deviceName ()).toLatin1 () + "\n";
                    adv_->writeDatagram (hh, d.senderAddress (), static_cast<quint16> (d.senderPort ()));
                }
                else if (tag == "EC" || tag == "PN")
                {
                    int controlPort = 0;
                    for (int i = 6; i + 1 < f.size (); i += 2)
                    {
                        if (f[i] == "CP")
                            controlPort = f[i + 1].toInt ();
                        else if (f[i] == "DP")
                            dataPort_ = static_cast<quint16> (f[i + 1].toInt ());
                    }
                    if (tag == "EC" && controlPort > 0)
                        openControl (static_cast<quint16> (controlPort));
                }
            }
        }
    }

    void openControl (quint16 port)
    {
        if (ctl_)
            ctl_->deleteLater ();
        ctl_ = new QTcpSocket (this);
        // BrainFlow's control messages (MN / ML power modes, ED) have no Bluetooth
        // meaning: the BLE firmware switches modes only at boot. Read and drop them.
        connect (ctl_, &QTcpSocket::readyRead, this, [this] { ctl_->readAll (); });
        ctl_->connectToHost (QHostAddress::LocalHost, port);
    }

    void onBytes (const QByteArray &bytes)
    {
        lineBuf_.append (bytes);
        if (lineBuf_.size () > kMaxLineBuffer)
            lineBuf_.clear ();
        const QByteArray packets = takeCompletePackets (lineBuf_);
        if (packets.isEmpty () || dataPort_ == 0)
            return; // BrainFlow is not connected yet
        int from = 0;
        while (from < packets.size ())
        {
            int to = packets.size ();
            if (to - from > kMaxDatagram)
            {
                to = packets.lastIndexOf ('\n', from + kMaxDatagram - 1) + 1;
                if (to <= from)
                    to = from + kMaxDatagram;
            }
            const QByteArray chunk = packets.mid (from, to - from);
            adv_->writeDatagram (chunk, QHostAddress::LocalHost, dataPort_);
            shared_->forwarded.fetch_add (static_cast<std::uint64_t> (chunk.count ('\n')));
            from = to;
        }
    }

    // One 100 ms send interval of the firmware: 25 Hz PPG and IMU, 15 Hz EDA,
    // 7.5 Hz temperature, in its packet format and order (the ancillary values
    // before EA, MZ last in the IMU group and PG last in the PPG group, as
    // BrainFlow pushes a preset row on those tags).
    void generateSynthetic ()
    {
        synthMs_ += 100;
        auto take = [] (double &acc, double perInterval) {
            acc += perInterval;
            const int n = static_cast<int> (acc);
            acc -= n;
            return n;
        };
        const int n25 = take (acc25_, 2.5), n15 = take (acc15_, 1.5), n7 = take (acc7_, 0.75);
        QByteArray out;
        auto packet = [&] (const char *tag, int n, double base, double amp, double hz) {
            if (n <= 0)
                return;
            QByteArray p = QByteArray::number (synthMs_) + ',' + QByteArray::number (++synthPackets_) + ',' +
                QByteArray::number (n) + ',' + tag + ",1,100";
            for (int k = 0; k < n; ++k)
            {
                const double t = (synthMs_ - 100.0 + 100.0 * (k + 1) / n) / 1000.0;
                p += ',' + QByteArray::number (base + amp * std::sin (2.0 * M_PI * hz * t), 'f', 3);
            }
            out += p + '\n';
        };
        for (const char *tag : {"AX", "AY", "AZ", "GX", "GY", "GZ", "MX", "MY", "MZ"})
            packet (tag, n25, tag[0] == 'A' && tag[1] == 'Z' ? 1.0 : 0.0, 0.01, 1.0);
        packet ("PI", n25, 152000.0, 600.0, 1.2);
        packet ("PR", n25, 64000.0, 400.0, 1.2);
        packet ("PG", n25, 118000.0, 900.0, 1.2);
        packet ("T1", n7, 33.5, 0.05, 0.05);
        packet ("TH", n7, 32.0, 0.05, 0.05);
        packet ("EA", n15, 0.35, 0.02, 0.1);
        onBytes (out);
    }

    QString deviceName () const
    {
        std::lock_guard<std::mutex> lock (shared_->mutex);
        return shared_->name;
    }

    std::shared_ptr<Shared> shared_;
    const Source source_;
    const QString preferred_;
    const double scanTimeoutSec_;
    bool stopping_ = false;

    QUdpSocket *adv_ = nullptr;
    QTcpSocket *ctl_ = nullptr;
    quint16 dataPort_ = 0;
    int hostPackets_ = 0;

    QBluetoothDeviceDiscoveryAgent *agent_ = nullptr;
    QBluetoothDeviceInfo candidate_;
    QLowEnergyController *ctrl_ = nullptr;
    QLowEnergyService *svc_ = nullptr;
    QByteArray lineBuf_;

    QTimer *synth_ = nullptr;
    qint64 synthMs_ = 0;
    int synthPackets_ = 0;
    double acc25_ = 0.0, acc15_ = 0.0, acc7_ = 0.0;
};

EmotiBitBleBridge::EmotiBitBleBridge () : shared_ (std::make_shared<Shared> ())
{
}

EmotiBitBleBridge::~EmotiBitBleBridge ()
{
    stop ();
}

void EmotiBitBleBridge::start (Source source, const QString &preferredName, double scanTimeoutSec)
{
    if (thread_)
        return;
    thread_ = std::make_unique<QThread> ();
    thread_->setObjectName (QStringLiteral ("EmotiBitBleBridge"));
    impl_ = new Impl (shared_, source, preferredName, scanTimeoutSec);
    impl_->moveToThread (thread_.get ());
    thread_->start ();
    Impl *impl = impl_;
    QMetaObject::invokeMethod (impl, [impl] { impl->begin (); }, Qt::QueuedConnection);
}

void EmotiBitBleBridge::stop ()
{
    if (!thread_)
        return;
    Impl *impl = impl_;
    impl_ = nullptr;
    // end () runs on the bridge thread, which owns every Qt object; the deferred
    // delete is processed when that thread finishes (QThread flushes it on exit)
    QMetaObject::invokeMethod (
        impl, [impl] {
            impl->end ();
            impl->deleteLater ();
        },
        Qt::BlockingQueuedConnection);
    thread_->quit ();
    thread_->wait ();
    thread_.reset ();
}

QString EmotiBitBleBridge::deviceName () const
{
    std::lock_guard<std::mutex> lock (shared_->mutex);
    return shared_->name;
}

QString EmotiBitBleBridge::error () const
{
    std::lock_guard<std::mutex> lock (shared_->mutex);
    return shared_->error;
}

QString EmotiBitBleBridge::status () const
{
    std::lock_guard<std::mutex> lock (shared_->mutex);
    return shared_->status;
}

QByteArray EmotiBitBleBridge::takeCompletePackets (QByteArray &buf)
{
    const int last = buf.lastIndexOf ('\n');
    if (last < 0)
        return QByteArray ();
    QByteArray packets = buf.left (last + 1);
    buf.remove (0, last + 1);
    return packets;
}

QString EmotiBitBleBridge::deviceIdFromName (const QString &name)
{
    const int colon = name.indexOf (QLatin1Char (':'));
    return colon >= 0 ? name.mid (colon + 1).trimmed () : name.trimmed ();
}
