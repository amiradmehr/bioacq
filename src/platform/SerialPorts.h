#pragma once

// Serial port enumeration (Qt SerialPort) and OpenBCI dongle detection.
// Enumeration only queries the OS device registry (IOKit / SetupAPI / udev);
// it never opens a port, so it is safe while another program owns the dongle.

#include <QString>
#include <QVector>
#include <QtGlobal>

// The OpenBCI Cyton USB dongle is an FTDI FT231X.
inline constexpr quint16 kFtdiVendorId = 0x0403;
inline constexpr quint16 kFt231xProductId = 0x6015;

struct SerialPortEntry
{
    QString portName;       // "COM3", "cu.usbserial-DP04W4GA"
    QString systemLocation; // "\\.\COM3", "/dev/cu.usbserial-DP04W4GA"
    quint16 vid = 0;        // 0 = unknown
    quint16 pid = 0;
    QString description;    // e.g. "FT231X USB UART"
    bool cytonDongle = false;
};

// Every serial port, OpenBCI dongles first (otherwise in the OS order). On
// macOS each USB serial device appears once, as its /dev/cu.* node (the
// /dev/tty.* twin blocks on open until carrier detect, so it is dropped).
QVector<SerialPortEntry> listSerialPorts ();

// The port string BrainFlow accepts as BrainFlowInputParams::serial_port:
// "COM3" on Windows (BrainFlow adds the \\.\ prefix itself), the /dev path elsewhere.
QString brainflowSerialPort (const SerialPortEntry &entry);

// BrainFlow serial_port value of the first OpenBCI dongle (FTDI VID 0x0403 /
// PID 0x6015, or a description naming "FT231X" / "OpenBCI"), empty if none.
QString findCytonDongle ();

// True if the port (a BrainFlow serial_port value: "COM3", "\\.\COM3" or a
// device path) currently exists.
bool serialPortExists (const QString &port);
