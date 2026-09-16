#pragma once

// Process-wide platform setup, called once from main (). No-ops where not needed.

// Windows: the executable uses the GUI subsystem, so a double-click opens no
// console window. When it was started from cmd / PowerShell with arguments,
// attach to that console so --help, --selftest, --probe and --screenshot print
// there. Output that is already redirected to a file or pipe is left alone.
// Note: cmd does not wait for GUI-subsystem programs; use
// "start /wait BioAcq.exe --selftest" (or pipe the output) to get the exit code.
void attachParentConsole ();

// Windows: raise the system timer resolution to 1 ms for the lifetime of the
// process (timeBeginPeriod). The default 15.6 ms tick would stretch the
// workers' 10 / 15 ms poll intervals and the 16 ms frame timer.
void raiseTimerResolution ();
