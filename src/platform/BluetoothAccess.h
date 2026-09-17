#pragma once

// Whether this process may use Bluetooth (for the EmotiBit's BLE link).
//
// macOS attributes Bluetooth access to the app that started the process (TCC)
// and kills a process whose responsible app has no
// NSBluetoothAlwaysUsageDescription. So Bluetooth is used only by BioAcq.app
// opened through LaunchServices (Finder, Dock, `open`), where the user's
// Bluetooth permission for the bundle applies; a binary started from a terminal
// or an IDE never touches Bluetooth (BIOACQ_ALLOW_BLUETOOTH=1 overrides this).
// Other platforms have no such permission.

#include <QString>

#include <functional>

class QObject;

namespace bluetooth_access
{

enum class Status
{
    Granted,
    Undetermined, // macOS: the user hasn't been asked yet
    Denied,       // the user said no (System Settings → Privacy & Security → Bluetooth)
    Unavailable   // not in a context that may use Bluetooth (see above)
};

// The current status; *why receives a user-facing reason unless Granted.
Status check (QString *why = nullptr);

// Shows the system prompt if Undetermined and calls done on context's thread
// with the resulting status (at once, if already decided; never if context is
// destroyed first). GUI thread of a QApplication: its event loop delivers it.
void request (QObject *context, const std::function<void (Status)> &done);

// request () for the headless modes, on the main thread: waits until the prompt
// is answered or stop () returns true, and returns the status then. On macOS
// the answer arrives on the main dispatch queue, which a QCoreApplication's
// event loop never drains, so this also runs the CoreFoundation run loop.
Status requestAndWait (const std::function<bool ()> &stop);

} // namespace bluetooth_access
