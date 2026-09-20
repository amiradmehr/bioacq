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
    std::string title; // short name for status text ("PPG green")
    std::string units;
    std::vector<std::string> traceNames;
    std::vector<ChannelCandidate> candidates; // tried in order; first match wins
    bool filterable = false;                  // HP / notch / LP filters may be applied in the worker
    int valueDecimals = 2;
    // PPG channel index for HeartRate::Tracker (HeartRate::Source), -1 = none.
    int heartRateInput = -1;
    // Non-empty: no BrainFlow rows; the worker computes this signal from the
    // listed signals (resolved when at least one of them is).
    std::vector<std::string> derivedFrom;
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
    int heartRateInput = -1;
    bool derived = false;     // computed in the worker (rows empty, timestampRow -1)
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
inline constexpr char CytonEcg[] = "cyton.ecg"; // EXG channel 1
inline constexpr char EmotiPpgGreen[] = "emotibit.ppg_green";
inline constexpr char EmotiPpgRed[] = "emotibit.ppg_red";
inline constexpr char EmotiPpgIr[] = "emotibit.ppg_ir";
inline constexpr char EmotiTemp[] = "emotibit.temp";
inline constexpr char EmotiAccel[] = "emotibit.accel";
inline constexpr char EmotiGyro[] = "emotibit.gyro";
inline constexpr char EmotiMag[] = "emotibit.mag";
inline constexpr char CytonHeartRate[] = "cyton.hr";    // derived from the ECG's R peaks
inline constexpr char EmotiHeartRate[] = "emotibit.hr"; // derived from the three PPG channels
} // namespace SignalKeys

// Ring channels of the derived heart-rate signal: one sample per beat of the
// source channel (bpm), or a status sample while there is no valid HR (bpm NaN).
namespace HeartRateRing
{
inline constexpr int Bpm = 0;
inline constexpr int Quality = 1;     // 0..1
inline constexpr int Source = 2;      // HeartRate::Source (SourceEcg from the ECG), -1 = none
inline constexpr int Beats = 3;       // beats in the estimate window
inline constexpr int Periodicity = 4; // autocorrelation at the beat interval
inline constexpr int Count = 5;
} // namespace HeartRateRing

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
