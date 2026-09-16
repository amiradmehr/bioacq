#include "SignalSpec.h"

#include "board_shim.h"

#include <algorithm>
#include <sstream>

namespace
{

constexpr int kDefault = static_cast<int> (BrainFlowPresets::DEFAULT_PRESET);
constexpr int kAux = static_cast<int> (BrainFlowPresets::AUXILIARY_PRESET);
constexpr int kAnc = static_cast<int> (BrainFlowPresets::ANCILLARY_PRESET);

const char *descriptorKey (ChannelKind kind)
{
    switch (kind)
    {
        case ChannelKind::Exg:
            return "exg";
        case ChannelKind::Accel:
            return "accel_channels";
        case ChannelKind::Gyro:
            return "gyro_channels";
        case ChannelKind::Magnetometer:
            return "magnetometer_channels";
        case ChannelKind::Ppg:
            return "ppg_channels";
        case ChannelKind::Temperature:
            return "temperature_channels";
    }
    return "";
}

// Channel list for (kind, preset), or empty if the board does not provide it.
// Presence is checked in the JSON descriptor first so that BrainFlow never has
// to report (and log) a missing field.
std::vector<int> channelsOf (ChannelKind kind, int board, int preset)
{
    try
    {
        const json descr = BoardShim::get_board_descr (board, preset);
        auto has = [&descr] (const char *k) {
            return descr.contains (k) && descr[k].is_array () && !descr[k].empty ();
        };
        switch (kind)
        {
            case ChannelKind::Exg:
                if (has ("eeg_channels") || has ("emg_channels") || has ("ecg_channels") ||
                    has ("eog_channels"))
                    return BoardShim::get_exg_channels (board, preset);
                break;
            case ChannelKind::Accel:
                if (has ("accel_channels"))
                    return BoardShim::get_accel_channels (board, preset);
                break;
            case ChannelKind::Gyro:
                if (has ("gyro_channels"))
                    return BoardShim::get_gyro_channels (board, preset);
                break;
            case ChannelKind::Magnetometer:
                if (has ("magnetometer_channels"))
                    return BoardShim::get_magnetometer_channels (board, preset);
                break;
            case ChannelKind::Ppg:
                if (has ("ppg_channels"))
                    return BoardShim::get_ppg_channels (board, preset);
                break;
            case ChannelKind::Temperature:
                if (has ("temperature_channels"))
                    return BoardShim::get_temperature_channels (board, preset);
                break;
        }
    }
    catch (const std::exception &)
    {
    }
    return {};
}

std::string rowsToString (const std::vector<int> &rows)
{
    std::ostringstream os;
    for (std::size_t i = 0; i < rows.size (); ++i)
        os << (i ? "," : "") << rows[i];
    return os.str ();
}

} // namespace

const char *channelKindName (ChannelKind kind)
{
    return descriptorKey (kind);
}

std::string presetName (int preset)
{
    switch (preset)
    {
        case kDefault:
            return "default";
        case kAux:
            return "auxiliary";
        case kAnc:
            return "ancillary";
        default:
            return "preset" + std::to_string (preset);
    }
}

int realBoardIdFor (DeviceKind kind)
{
    return kind == DeviceKind::Cyton ? static_cast<int> (BoardIds::CYTON_BOARD)
                                     : static_cast<int> (BoardIds::EMOTIBIT_BOARD);
}

const char *deviceSlotName (DeviceKind kind)
{
    return kind == DeviceKind::Cyton ? "cyton" : "emotibit";
}

const char *deviceDisplayName (DeviceKind kind)
{
    return kind == DeviceKind::Cyton ? "OpenBCI Cyton" : "EmotiBit";
}

std::vector<SignalDef> signalDefsFor (DeviceKind kind)
{
    std::vector<SignalDef> defs;
    if (kind == DeviceKind::Cyton)
    {
        // One channel: ECG on EXG channel 1 (N1P against SRB, single-ended).
        SignalDef ecg;
        ecg.key = SignalKeys::CytonEcg;
        ecg.title = "ECG";
        ecg.units = "µV";
        ecg.traceNames = {"ECG"};
        ecg.candidates = {{ChannelKind::Exg, kDefault, 0, 1}};
        ecg.filterable = true;
        ecg.valueDecimals = 0;
        defs.push_back (ecg);
        return defs;
    }

    // EmotiBit: IMU in DEFAULT, PPG in AUXILIARY, temperature in ANCILLARY.
    // Later candidates only matter for boards that lack the preferred layout
    // (e.g. the synthetic board used by --selftest / --synthetic).
    //
    // PPG order in BrainFlow's emotibit.cpp: ppg_channels = [infrared, red,
    // green] (PI -> [0], PR -> [1], PG -> [2]).
    auto ppg = [] (const char *key, const char *title, const char *trace, int index, int hrInput) {
        SignalDef d;
        d.key = key;
        d.title = title;
        d.units = "a.u.";
        d.traceNames = {trace};
        d.candidates = {{ChannelKind::Ppg, kAux, index, 1}, {ChannelKind::Ppg, kDefault, index, 1}};
        d.valueDecimals = 0;
        d.heartRateInput = hrInput;
        return d;
    };
    defs.push_back (ppg (SignalKeys::EmotiPpgGreen, "PPG green", "green", 2, 0));
    defs.push_back (ppg (SignalKeys::EmotiPpgRed, "PPG red", "red", 1, 1));
    defs.push_back (ppg (SignalKeys::EmotiPpgIr, "PPG IR", "IR", 0, 2));

    SignalDef temp;
    temp.key = SignalKeys::EmotiTemp;
    temp.title = "Temperature";
    temp.units = "°C";
    temp.traceNames = {"T"};
    temp.candidates = {{ChannelKind::Temperature, kAnc, 0, 1},
        {ChannelKind::Temperature, kAux, 0, 1}, {ChannelKind::Temperature, kDefault, 0, 1}};
    temp.valueDecimals = 2;
    defs.push_back (temp);

    SignalDef accel;
    accel.key = SignalKeys::EmotiAccel;
    accel.title = "Accelerometer";
    accel.units = "g";
    accel.traceNames = {"X", "Y", "Z"};
    accel.candidates = {{ChannelKind::Accel, kDefault, 0, 3}, {ChannelKind::Accel, kAux, 0, 3}};
    accel.valueDecimals = 3;
    defs.push_back (accel);

    SignalDef gyro;
    gyro.key = SignalKeys::EmotiGyro;
    gyro.title = "Gyroscope";
    gyro.units = "°/s";
    gyro.traceNames = {"X", "Y", "Z"};
    gyro.candidates = {{ChannelKind::Gyro, kDefault, 0, 3}, {ChannelKind::Gyro, kAux, 0, 3}};
    gyro.valueDecimals = 1;
    defs.push_back (gyro);

    SignalDef mag;
    mag.key = SignalKeys::EmotiMag;
    mag.title = "Magnetometer";
    mag.units = "µT";
    mag.traceNames = {"X", "Y", "Z"};
    mag.candidates = {{ChannelKind::Magnetometer, kDefault, 0, 3},
        {ChannelKind::Magnetometer, kAux, 0, 3}, {ChannelKind::Magnetometer, kAnc, 0, 3},
        // no magnetometer (synthetic board): exercise another 3-axis stream instead
        {ChannelKind::Accel, kAux, 0, 3}, {ChannelKind::Accel, kDefault, 0, 3}};
    mag.valueDecimals = 1;
    defs.push_back (mag);

    SignalDef hr;
    hr.key = SignalKeys::EmotiHeartRate;
    hr.title = "Heart rate";
    hr.units = "bpm";
    hr.traceNames = {"HR"};
    hr.derivedFrom = {SignalKeys::EmotiPpgGreen, SignalKeys::EmotiPpgRed, SignalKeys::EmotiPpgIr};
    hr.valueDecimals = 0;
    defs.push_back (hr);
    return defs;
}

std::vector<ResolvedSignal> resolveSignals (
    int boardId, const std::vector<SignalDef> &defs, std::vector<std::string> *problems)
{
    std::vector<ResolvedSignal> out;
    std::vector<int> presets;
    try
    {
        presets = BoardShim::get_board_presets (boardId);
    }
    catch (const std::exception &e)
    {
        if (problems)
            problems->push_back (
                "board " + std::to_string (boardId) + " has no descriptor: " + e.what ());
        return out;
    }

    for (const SignalDef &def : defs)
    {
        if (!def.derivedFrom.empty ())
            continue; // below, once its inputs are known
        bool resolved = false;
        for (std::size_t ci = 0; ci < def.candidates.size () && !resolved; ++ci)
        {
            const ChannelCandidate &c = def.candidates[ci];
            if (std::find (presets.begin (), presets.end (), c.preset) == presets.end ())
                continue;
            const std::vector<int> chans = channelsOf (c.kind, boardId, c.preset);
            if (c.count <= 0 || static_cast<int> (chans.size ()) < c.count)
                continue;
            int first = c.firstIndex;
            bool clamped = false;
            if (first < 0 || first + c.count > static_cast<int> (chans.size ()))
            {
                first = static_cast<int> (chans.size ()) - c.count;
                clamped = true;
            }
            try
            {
                ResolvedSignal r;
                r.key = def.key;
                r.title = def.title;
                r.units = def.units;
                r.traceNames = def.traceNames;
                r.preset = c.preset;
                r.rows.assign (chans.begin () + first, chans.begin () + first + c.count);
                r.timestampRow = BoardShim::get_timestamp_channel (boardId, c.preset);
                r.nominalRate = BoardShim::get_sampling_rate (boardId, c.preset);
                r.filterable = def.filterable;
                r.valueDecimals = def.valueDecimals;
                r.heartRateInput = def.heartRateInput;
                r.substituted = (ci != 0) || clamped;
                std::ostringstream src;
                src << channelKindName (c.kind) << "[" << first;
                if (c.count > 1)
                    src << ".." << first + c.count - 1;
                src << "] @ " << presetName (c.preset) << ", row" << (c.count > 1 ? "s " : " ")
                    << rowsToString (r.rows);
                r.source = src.str ();
                out.push_back (r);
                resolved = true;
            }
            catch (const std::exception &)
            {
                // try next candidate
            }
        }
        if (!resolved && problems)
            problems->push_back (def.key + ": not provided by board " + std::to_string (boardId));
    }

    // Derived signals: computed by the worker from resolved inputs.
    for (const SignalDef &def : defs)
    {
        if (def.derivedFrom.empty ())
            continue;
        std::vector<const ResolvedSignal *> inputs;
        for (const std::string &k : def.derivedFrom)
            for (const ResolvedSignal &r : out)
                if (r.key == k)
                    inputs.push_back (&r);
        if (inputs.empty ())
        {
            if (problems)
                problems->push_back (def.key + ": none of its inputs is provided by board " + std::to_string (boardId));
            continue;
        }
        ResolvedSignal r;
        r.key = def.key;
        r.title = def.title;
        r.units = def.units;
        r.traceNames = def.traceNames;
        r.preset = inputs.front ()->preset;
        r.valueDecimals = def.valueDecimals;
        r.derived = true;
        std::ostringstream src;
        src << "derived from";
        for (std::size_t i = 0; i < inputs.size (); ++i)
        {
            src << (i ? ", " : " ") << inputs[i]->key;
            r.substituted = r.substituted || inputs[i]->substituted;
        }
        r.source = src.str ();
        out.push_back (r);
    }
    return out;
}
