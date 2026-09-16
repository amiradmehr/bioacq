#pragma once

// Pure readout logic behind the instrument panel: rail headroom, rate tone,
// stall detection, package-number gaps, number formatting, CPU meter.
// Header-only and free of widgets so --selftest can check every rule.

#include <QString>
#include <QStringList>

#include <sys/resource.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Readouts
{

// OpenBCI Cyton (ADS1299, gain 24): +-4.5 V / 24 = +-187,500 uV full scale.
inline constexpr double kCytonFullScaleUv = 187500.0;
// Headroom below which Ch1 counts as "near rail" (enter / leave, hysteresis).
inline constexpr double kNearRailEnter = 0.10;
inline constexpr double kNearRailLeave = 0.12;
// A stream with no new samples for longer than this is "stalled".
inline constexpr double kStallSeconds = 1.0;
// Rate readout turns amber when more than this fraction below nominal.
inline constexpr double kRateTolerance = 0.05;

// 1 - peak|raw| / fullScale, clamped to [0, 1]; NaN if the peak is unknown.
inline double railHeadroom (double peakAbs, double fullScale = kCytonFullScaleUv)
{
    if (!std::isfinite (peakAbs) || !(fullScale > 0.0))
        return std::numeric_limits<double>::quiet_NaN ();
    const double h = 1.0 - std::fabs (peakAbs) / fullScale;
    return h < 0.0 ? 0.0 : (h > 1.0 ? 1.0 : h);
}

inline bool nearRail (double headroom, bool wasNear)
{
    if (!std::isfinite (headroom))
        return false;
    return wasNear ? headroom < kNearRailLeave : headroom < kNearRailEnter;
}

enum class RateTone
{
    None, // no samples yet: "- / nominal", dim
    Ok,   // within tolerance
    Low   // more than 5 % below nominal: amber
};

inline RateTone rateTone (bool haveSamples, double measured, double nominal)
{
    if (!haveSamples || !std::isfinite (measured))
        return RateTone::None;
    if (nominal > 0.0 && measured < (1.0 - kRateTolerance) * nominal)
        return RateTone::Low;
    return RateTone::Ok;
}

inline bool isStalled (bool streaming, double secondsSinceNewSample, double threshold = kStallSeconds)
{
    return streaming && std::isfinite (secondsSinceNewSample) && secondsSinceNewSample > threshold;
}

// A stream that has not delivered its FIRST sample this long after the device
// began streaming counts as stalled too. EmotiBit gets longer: the worker
// skips leading all-zero (not yet filled) samples for up to 3 s.
inline constexpr double kFirstSampleGraceCyton = 1.0;
inline constexpr double kFirstSampleGraceEmotibit = 3.0;

// Seconds a stream has gone without a new sample: since its newest sample, or
// -- before its first sample -- since the device began streaming (NaN = unknown).
inline double silentSeconds (bool haveSamples, double sinceNewSample, double sinceStreaming)
{
    return haveSamples ? sinceNewSample : sinceStreaming;
}

// Threshold for silentSeconds(): the stall rule once samples flow, the
// first-sample grace before that.
inline double stallThreshold (bool haveSamples, double firstSampleGrace)
{
    return haveSamples ? kStallSeconds : firstSampleGrace;
}

// Headroom as a percentage floored to 0.1 %, so the number never reads at or
// above a threshold it is below (0.0996 -> "9.9 %", never "10 %").
inline QString headroomPercent (double headroom)
{
    if (!std::isfinite (headroom))
        return QStringLiteral ("—");
    const double p = std::floor (headroom * 1000.0 + 1e-9) / 10.0;
    return QString::number (p, 'f', 1) + QStringLiteral (" %");
}

// Counts packets missing from a wrapping package-number sequence (Cyton and
// BrainFlow's synthetic board: 0..255). `last` carries the previous number
// across calls (-1 = none yet). Returns the number of missing packets.
inline std::uint64_t packageGaps (int &last, const double *v, std::size_t n, int modulo = 256)
{
    std::uint64_t missing = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (!std::isfinite (v[i]))
            continue;
        const int cur = static_cast<int> (std::lround (v[i]));
        if (cur < 0 || cur >= modulo)
            continue;
        if (last >= 0)
        {
            int d = cur - last;
            if (d <= 0)
                d += modulo;
            if (d > 1 && d < modulo)
                missing += static_cast<std::uint64_t> (d - 1);
        }
        last = cur;
    }
    return missing;
}

// ---------------------------------------------------------------- formatting
inline const QChar kMinus (0x2212); // typographic minus

// Fixed decimals, optional sign (+ / U+2212), optional thousands separators.
// A value that rounds to zero never carries a sign.
inline QString number (double v, int decimals, bool withPlus = false, bool thousands = true)
{
    if (!std::isfinite (v))
        return QStringLiteral ("—");
    QString s = QString::number (std::fabs (v), 'f', decimals < 0 ? 0 : decimals);
    bool zero = true;
    for (QChar c : s)
        if (c != QLatin1Char ('0') && c != QLatin1Char ('.'))
        {
            zero = false;
            break;
        }
    if (thousands)
    {
        int dot = s.indexOf (QLatin1Char ('.'));
        if (dot < 0)
            dot = s.size ();
        for (int i = dot - 3; i > 0; i -= 3)
            s.insert (i, QLatin1Char (','));
    }
    if (!zero)
    {
        if (v < 0.0)
            s.prepend (kMinus);
        else if (withPlus)
            s.prepend (QLatin1Char ('+'));
    }
    return s;
}

inline QString rateText (bool haveSamples, double measured, double nominal)
{
    const QString nom = QString::number (nominal, 'g', 4);
    if (!haveSamples || !std::isfinite (measured))
        return QStringLiteral ("— / %1 Hz").arg (nom);
    return QStringLiteral ("%1 / %2 Hz").arg (QString::number (measured, 'f', 1), nom);
}

inline QString elapsed (double seconds)
{
    if (!std::isfinite (seconds) || seconds < 0.0)
        seconds = 0.0;
    const long long s = static_cast<long long> (seconds);
    return QStringLiteral ("%1:%2:%3")
        .arg (s / 3600, 2, 10, QLatin1Char ('0'))
        .arg ((s / 60) % 60, 2, 10, QLatin1Char ('0'))
        .arg (s % 60, 2, 10, QLatin1Char ('0'));
}

// Decimal (SI) units, as Finder shows file sizes.
inline QString bytes (double b)
{
    if (!std::isfinite (b) || b < 0.0)
        return QStringLiteral ("—");
    if (b < 1000.0)
        return QStringLiteral ("%1 B").arg (static_cast<long long> (b));
    if (b < 1e6)
        return QStringLiteral ("%1 kB").arg (b / 1e3, 0, 'f', 1);
    if (b < 1e9)
        return QStringLiteral ("%1 MB").arg (b / 1e6, 0, 'f', 1);
    return QStringLiteral ("%1 GB").arg (b / 1e9, 0, 'f', 2);
}

// ---------------------------------------------------------------- CPU meter
// Process CPU time (all threads, user + system) as % of ONE core over the
// interval between two sample() calls.
inline double processCpuSeconds ()
{
    struct rusage ru;
    if (getrusage (RUSAGE_SELF, &ru) != 0)
        return std::numeric_limits<double>::quiet_NaN ();
    return static_cast<double> (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
        1e-6 * static_cast<double> (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
}

class CpuMeter
{
public:
    // Returns the percentage since the previous call (NaN on the first call).
    double sample ()
    {
        const double wall = std::chrono::duration<double> (
            std::chrono::steady_clock::now ().time_since_epoch ()).count ();
        const double cpu = processCpuSeconds ();
        double pct = std::numeric_limits<double>::quiet_NaN ();
        if (std::isfinite (lastCpu_) && wall - lastWall_ > 0.05)
            pct = 100.0 * (cpu - lastCpu_) / (wall - lastWall_);
        lastWall_ = wall;
        lastCpu_ = cpu;
        if (std::isfinite (pct))
            percent_ = pct;
        return pct;
    }
    double percent () const
    {
        return percent_;
    }

private:
    double lastWall_ = 0.0;
    double lastCpu_ = std::numeric_limits<double>::quiet_NaN ();
    double percent_ = std::numeric_limits<double>::quiet_NaN ();
};

} // namespace Readouts
