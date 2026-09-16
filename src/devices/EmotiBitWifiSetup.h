#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <string>

// The WiFi networks an EmotiBit joins, edited over its USB serial port with
// the stock firmware's "WiFi config edit mode" (v1.14: EmotiBit.cpp,
// EmotiBitConfigManager.cpp in EmotiBit_FeatherWing).
//
// * The firmware offers that mode only at boot, so every session resets the
//   EmotiBit (RTS -> EN), sends 'C' once it prints "EmotiBit ready" (the byte
//   waits in the UART buffer until the prompt reads it) and restarts it
//   (@RS~) at the end.
// * Commands are "@TAG,PAYLOAD~" with no line ending; replies "@AK,TAG~" /
//   "@NK,TAG~". Add: @WA,{"ssid":"…","password":"…"}~, list: @LS~, delete:
//   @WD,<index>~. Changes are written to config.txt on the SD card. At boot
//   the firmware tries its networks in list order, up to 20 s each.
// * The firmware echoes passwords (the add confirmation, the list, every
//   boot). Nothing read from the port is logged or kept beyond the SSID names
//   parsed out of the list; the read buffer is zeroed after each command.
namespace emotibit_wifi
{

constexpr int kBaud = 2000000;
constexpr int kMaxNetworks = 12; // MAX_CREDENTIALS in the firmware

// The firmware parses config.txt into a StaticJsonDocument<1024>; an entry
// that doesn't fit is dropped silently and an oversized file stops the boot.
// Rough budget: kConfigBase + per network kPerNetwork + SSID + password bytes.
constexpr int kConfigBudget = 1000;
constexpr int kConfigBase = 60;
constexpr int kPerNetwork = 50;

// "@WA,{"ssid":"…","password":"…"}~" with JSON escaping, or "" with *error
// set when the network can't be sent (empty or oversized SSID, a password
// outside 8-63 characters, or a '~', which ends a firmware command).
std::string addCommand (const QString &ssid, const QString &password, QString *error);

// SSIDs from the firmware's list output: the "0. ssid : password" lines
// between "config file credentials:" and "@AK,LS~". Passwords are dropped;
// *budgetUsed receives the config size estimate (kConfigBase + entries).
QStringList parseNetworkList (const QString &serialText, int *budgetUsed = nullptr);

// "AK" or "NK" if serialText holds the reply to typetag, else "".
QString replyFor (const QString &serialText, const QString &typetag);

struct Result
{
    bool ok = false;
    QString error;        // why it failed (never contains a password)
    QStringList networks; // the saved SSIDs afterwards, in the order the EmotiBit tries them
};

using Progress = std::function<void (const QString &)>;

// Blocking (10-15 s: the EmotiBit reboots twice); run off the GUI thread.
// port: the EmotiBit's USB serial port ("COM5", "/dev/cu.usbserial-…").
Result listNetworks (const QString &port, const Progress &progress);
// Adds ssid (an entry with the same name is replaced once the new one is saved).
Result addNetwork (const QString &port, const QString &ssid, const QString &password, const Progress &progress);
// Removes the entry at index if it is still expectedSsid; refuses to remove
// the last network (an EmotiBit without one never finishes booting).
Result removeNetwork (const QString &port, int index, const QString &expectedSsid, const Progress &progress);

// USB serial ports that may be an EmotiBit's Feather, likeliest first:
// CP210x / CH34x / CH9102 / Adafruit / Espressif USB, never the Cyton's
// FTDI dongle. Strings as QSerialPort and BrainFlow take them.
QStringList candidatePorts ();

} // namespace emotibit_wifi
