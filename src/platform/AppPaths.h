#pragma once

// Default locations that depend on how the app was built and installed.

#include <QString>

// Default recording folder.
//   Development builds: the folder baked in at configure time (CMake
//   BIOACQ_RECORD_DIR, <repo>/recordings) if it is set and writable.
//   Packaged builds (CMake BIOACQ_PACKAGED=ON) and unusable dev folders:
//   <Documents>/BioAcq Recordings.
QString defaultRecordDir ();

// Where a recording goes when the chosen folder cannot be used:
// <home>/bioacq_recordings.
QString fallbackRecordDir ();
