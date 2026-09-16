#pragma once

#include <string>
#include <vector>

// Declarative description of every plotted signal. Rows are never hard-coded:
// each signal lists candidate (channel kind, preset, index) triples which are
// resolved at runtime against BoardShim's board descriptor, so the same
// pipeline runs on the real boards and on BoardIds::SYNTHETIC_BOARD.

enum class ChannelKind
{
    Exg,
    Accel,
    Gyro,
    Magnetometer,
    Ppg,
    Temperature
};

struct ChannelCandidate
{
    ChannelKind kind;
    int preset;     // BrainFlowPresets value
    int firstIndex; // index into the channel list returned by the getter
    int count;      // consecutive channels (1 = scalar, 3 = X/Y/Z)
};

struct SignalDef
{
    std::string key;
    std::string title;
    std::string units;
    std::vector<std::string> traceNames;
    std::vector<ChannelCandidate> candidates; // tried in order; first match wins
    bool filterable = false;                  // HP/notch filters may be applied in the worker
    int valueDecimals = 2;
};

struct ResolvedSignal
{
    std::string key;
    std::string title;
    std::string units;
    std::vector<std::string> traceNames;
    int preset = 0;
    int timestampRow = -1;
    std::vector<int> rows;
    double nominalRate = 0.0;
    bool filterable = false;
    int valueDecimals = 2;
    std::string source;       // e.g. "ppg_channels[2] @ auxiliary, row 3"
    bool substituted = false; // resolved through a fallback candidate / clamped index
};

enum class DeviceKind
{
    Cyton,
    EmotiBit
};

namespace SignalKeys
{
inline constexpr char CytonCh1[] = "cyton.ch1";
inline constexpr char EmotiTemp[] = "emotibit.temp";
inline constexpr char EmotiPpgGreen[] = "emotibit.ppg_green";
inline constexpr char EmotiAccel[] = "emotibit.accel";
inline constexpr char EmotiGyro[] = "emotibit.gyro";
inline constexpr char EmotiMag[] = "emotibit.mag";
} // namespace SignalKeys

const char *channelKindName (ChannelKind kind);
std::string presetName (int preset);

int realBoardIdFor (DeviceKind kind);
const char *deviceSlotName (DeviceKind kind);    // "cyton" / "emotibit"
const char *deviceDisplayName (DeviceKind kind); // "OpenBCI Cyton" / "EmotiBit"
std::vector<SignalDef> signalDefsFor (DeviceKind kind);

// Resolve definitions against the board's descriptor using BoardShim's static
// getters (these do not take BrainFlow's global session lock, so they are safe
// to call from the GUI thread). Definitions that cannot be resolved are skipped
// and described in *problems.
std::vector<ResolvedSignal> resolveSignals (
    int boardId, const std::vector<SignalDef> &defs, std::vector<std::string> *problems);
