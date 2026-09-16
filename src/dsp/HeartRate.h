#pragma once

#include "Biquad.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <utility>
#include <vector>

// Streaming heart rate from reflectance PPG (EmotiBit: MAX30101 green / red /
// infrared). Runs in the EmotiBit worker on the re-timed display samples; it
// never touches BrainFlow's rows, so recordings stay raw.
//
// BeatDetector, one per PPG channel:
//  1. Band-pass 0.5-4 Hz: one RBJ high-pass and one RBJ low-pass biquad
//     (Q = 1/sqrt(2)) designed at the nominal sample rate. The rate is
//     re-measured over the first kRateCheckSec of every uninterrupted stretch
//     (intervals longer than 3 nominal periods do not count) and the filters
//     are redesigned if it is more than kRateTolerance off, clamped to
//     kMinRateFactor-kMaxRateFactor x nominal. (A second high-pass stage rings:
//     at 30-45 bpm its rebound in the long diastole reached half a beat's
//     height.)
//  2. Inverted: reflectance counts DROP at systole (more blood absorbs more
//     light), so a systolic pulse is a MAXIMUM of -bandpass(counts).
//  3. Artefacts: once kScaleBeats beats of the last kWindowSec are known, the
//     band-pass is clipped to kArtefactFactor x the median largest |band-pass|
//     between those beats, and the output samples a clipped stretch reaches
//     (half the baseline window before it, that plus kArtefactSettleSec after)
//     are masked to 0: no beat there, nothing for the periodicity check. A
//     candidate beat above kArtefactFactor x the median beat height is dropped
//     and cannot replace a beat or raise the amplitude gate. The scales expire
//     kWindowSec after their kScaleBeats-th newest beat, so a real, lasting
//     change of the pulse amplitude is accepted again.
//  4. Baseline removal: the clipped signal minus its centred kDetrendSec
//     moving average (a delay line; the output lags by delaySec ()).
//     Respiratory wander at 0.2-0.3 Hz is up to about the pulse's size on
//     reflectance PPG and step 1 leaves a quarter of it at 0.25 Hz; a steeper
//     IIR high-pass rings into false beats at 30-45 bpm, while a moving
//     average is FIR and cannot ring past its window.
//  5. Blocks of interest after Elgendi et al. (2013): z = square of the
//     positive part of that signal; a block is where the 111 ms moving average
//     of z (systolic peak) exceeds the 667 ms moving average (one beat) plus
//     kBeta x the long-term mean of z. Both averages are centred, so a block
//     is decided kBeatWindowSec / 2 after the fact. A block at least 111 ms
//     long yields one candidate beat at its largest sample, timed by
//     parabolic interpolation between samples.
//  6. A candidate must reach kAmplitudeGate of the largest beat of the last
//     kGateMemorySec (rejects band-pass ringing and small wiggles), and lies
//     at least 60 / (1.05 x kMaxBpm) s after the previous beat (refractory
//     period); a larger peak inside the refractory period replaces it.
//
// A gap of more than kDropoutSec in the timestamps (a dropout) restarts the
// filters, averages and the rate measurement; the beats are kept.
//
// estimate(): inter-beat intervals (IBIs) of the beats inside the last
// kWindowSec. IBIs outside 30-200 bpm (5 % slack) are outliers. The median of
// the in-range IBIs anchors the estimate; quality = share of ALL IBIs within
// +-20 % of that median; HR = 60 / mean of those inlier IBIs (the mean of the
// inliers averages the sub-sample timing error that a single median IBI
// keeps). Valid only with >= kMinBeats beats, quality >= kMinQuality, a beat
// in the last max(2 s, 2.2 IBIs) (counted from the detector's delayed time)
// and periodicity >= kMinPeriodicity: the autocorrelation of the band-passed,
// baseline-removed signal at the beat interval minus its (positive)
// autocorrelation at half that interval. Band-passed noise also yields fairly
// regular "beats", but it does not repeat itself; slow respiratory wander
// repeats, but stays correlated at half a beat too. With less than kWindowSec
// of signal buffered the gate is raised by sqrt(kWindowSec / buffered s): a
// short correlation is noisier (noise-only starts passed ~4x more often).
//
// Tracker: one detector per PPG channel; the source is the valid channel with
// the best quality, switched only when another channel is better by
// kSwitchMargin for kSwitchHoldSec (no flicker).
namespace HeartRate
{

inline constexpr double kMinBpm = 30.0;
inline constexpr double kMaxBpm = 200.0;
inline constexpr double kWindowSec = 8.0;
inline constexpr int kMinBeats = 4;
inline constexpr double kIbiTolerance = 0.20;
inline constexpr double kMinQuality = 0.70;
inline constexpr double kMinPeriodicity = 0.50;
inline constexpr double kSwitchMargin = 0.15;
inline constexpr double kSwitchHoldSec = 3.0;
inline constexpr double kHighPassHz = 0.5;
inline constexpr double kLowPassHz = 4.0;
inline constexpr double kPeakWindowSec = 0.111; // systolic peak duration
inline constexpr double kBeatWindowSec = 0.667; // beat duration
inline constexpr double kBeta = 0.02;           // threshold offset, x mean of z
inline constexpr double kAmplitudeGate = 0.35;
inline constexpr double kGateMemorySec = 3.0;
inline constexpr double kSettleSec = 1.5; // filters settling: no beats yet
inline constexpr double kDetrendSec = 1.5; // centred moving average removed after the band-pass
inline constexpr double kDropoutSec = 1.0; // a longer gap restarts the filters
inline constexpr double kRateCheckSec = 4.0;
inline constexpr double kRateTolerance = 0.15;
inline constexpr double kMinRateFactor = 0.6; // redesign range, x nominal rate
inline constexpr double kMaxRateFactor = 1.6;
inline constexpr int kScaleBeats = 4;         // beats needed for an artefact scale (median of up to 8)
inline constexpr double kArtefactFactor = 3.0; // x the median recent beat height
// masked after an artefact beyond the moving average's half window (band-pass ringing)
inline constexpr double kArtefactSettleSec = 0.25;
// While there is no valid HR the tracker still publishes its status (NaN HR,
// quality) this often, so the UI can show the quality it is getting.
inline constexpr double kStatusIntervalSec = 1.0;

enum Source
{
    SourceNone = -1,
    SourceGreen = 0,
    SourceRed = 1,
    SourceIr = 2
};
inline constexpr int kSources = 3;
const char *sourceName (int source); // "GREEN" / "RED" / "IR" / "—"

struct Estimate
{
    double bpm = std::numeric_limits<double>::quiet_NaN ();
    double quality = 0.0;     // share of IBIs within +-20 % of the median, 0..1
    double periodicity = 0.0; // autocorrelation at the beat interval
    int beats = 0;            // beats inside the window
    int ibis = 0;             // intervals inside the window
    double lastBeat = std::numeric_limits<double>::quiet_NaN ();
    bool valid = false;
};

// IBI statistics of `beats` inside [now - windowSec, now] (periodicity is not
// checked here: see BeatDetector::estimate).
Estimate estimateFromBeats (const std::deque<double> &beats, double now, double windowSec = kWindowSec);

class BeatDetector
{
public:
    explicit BeatDetector (double nominalFs = 25.0);
    void reset (double nominalFs);
    // Samples in time order (s, raw counts); non-finite or non-increasing
    // samples are skipped.
    void process (const double *t, const double *x, std::size_t n);

    // estimateFromBeats plus the periodicity check
    // (`now` is the newest sample time; the beats lag it by delaySec ())
    Estimate estimate (double now) const;
    // how far the beat detection lags the newest sample: half the baseline window
    double delaySec () const
    {
        return static_cast<double> (dy_.size () / 2) / fs_;
    }
    // Largest normalised autocorrelation of the band-passed signal (last
    // kWindowSec) at lags within +-20 % of ibiSec, minus the (positive part of
    // the) autocorrelation at half that lag; 0 without enough data.
    double periodicity (double ibiSec) const;
    // kMinPeriodicity, raised while less than kWindowSec is buffered
    double periodicityThreshold () const;
    double correlation (int lagSamples) const; // at the ~25 Hz periodicity rate

    const std::deque<double> &beats () const
    {
        return beats_;
    }
    std::uint64_t totalBeats () const
    {
        return total_;
    }
    double designRate () const
    {
        return fs_;
    }
    bool hasData () const
    {
        return count_ > 0;
    }
    // candidates dropped as artefacts (above kArtefactFactor x the beat scale)
    std::uint64_t artefacts () const
    {
        return artefacts_;
    }

private:
    void design ();
    void restart (double t);
    void candidate (double tp, double yp);
    void updateScale (double t);

    double nominalFs_ = 25.0;
    double fs_ = 25.0;
    FilterChain bp_;
    std::uint64_t count_ = 0;
    double lastT_ = -std::numeric_limits<double>::infinity ();
    // rate measurement over the current uninterrupted stretch
    double rateT0_ = 0.0;
    double rateSum_ = 0.0;
    std::uint64_t rateIntervals_ = 0;
    bool rateChecked_ = false;
    double settleUntil_ = 0.0;
    // baseline removal: centred moving average (delay line of the clipped band-pass)
    std::vector<double> dy_, dt_;
    std::size_t dHead_ = 0, dFilled_ = 0;
    double dSum_ = 0.0;
    // the latest artefact stretch (times of its first and last clipped sample)
    double artStart_ = -std::numeric_limits<double>::infinity ();
    double artEnd_ = -std::numeric_limits<double>::infinity ();
    // artefact scales, valid until scaleUntil_: the median height of recent
    // beats, and the median largest |band-pass| between them (the limit
    // before the baseline removal, where respiratory wander is still in)
    double scale_ = 0.0;
    double spanScale_ = 0.0;
    double scaleUntil_ = -std::numeric_limits<double>::infinity ();
    double span_ = 0.0; // largest |clipped band-pass| since the last beat
    std::uint64_t artefacts_ = 0;
    double absMean_ = 0.0;
    double zMean_ = 0.0;
    // the newest n2_ samples: time, baseline-removed signal (step 4), square
    // of its positive part
    int n1_ = 3, n2_ = 17;
    std::vector<double> ht_, hy_, hz_;
    std::size_t head_ = 0, filled_ = 0;
    double sumBeat_ = 0.0;
    // baseline-removed signal of the last window at ~25 Hz (periodicity check)
    int pStep_ = 1;
    std::vector<double> py_;
    std::size_t pHead_ = 0, pFilled_ = 0;
    std::uint64_t pCount_ = 0;
    // block of interest
    bool inBlock_ = false;
    int blockLen_ = 0;
    double peakY_ = 0.0, peakT_ = 0.0, peakPrevY_ = 0.0, peakPrevT_ = 0.0, peakNextY_ = 0.0, peakNextT_ = 0.0;
    double lastBeatT_ = -std::numeric_limits<double>::infinity ();
    double lastBeatY_ = 0.0;
    std::deque<double> beats_;
    struct Peak
    {
        double t, height, span;
    };
    std::deque<Peak> peaks_; // recent beats: time, height, largest |band-pass| since the beat before
    std::uint64_t total_ = 0;
};

// One published point: the HR at a beat of the source channel, or -- while
// there is no valid HR -- the status (bpm NaN, the best channel's quality).
struct Sample
{
    double t = 0.0;
    double bpm = std::numeric_limits<double>::quiet_NaN ();
    double quality = 0.0;
    double periodicity = 0.0;
    int source = SourceNone;
    int beats = 0;
};

class Tracker
{
public:
    explicit Tracker (double nominalFs = 25.0);
    void reset (double nominalFs);
    void process (int channel, const double *t, const double *x, std::size_t n);
    // Re-estimates the channels at `now` (newest sample time), selects the
    // source and appends what is new since the last call to `out`.
    void update (double now, std::vector<Sample> &out);

    int source () const
    {
        return source_;
    }
    const Estimate &estimate (int channel) const
    {
        return est_[channel < 0 || channel >= kSources ? 0 : channel];
    }

private:
    BeatDetector det_[kSources];
    Estimate est_[kSources];
    std::uint64_t estBeats_[kSources] = {0, 0, 0};
    double estAt_[kSources] = {0.0, 0.0, 0.0};
    int source_ = SourceNone;
    int candidate_ = SourceNone;
    double candidateSince_ = 0.0;
    int publishedSource_ = SourceNone;
    std::uint64_t publishedBeats_ = 0;
    bool publishedValid_ = false;
    double lastPublished_ = -std::numeric_limits<double>::infinity ();
};

// Synthetic reflectance PPG for --selftest and the --test-ppg-bpm hook:
// counts = dc - amplitude x pulseWave(t), where pulseWave is a normalised
// blood-volume wave (a steep systolic wave and a smaller dicrotic wave) at a
// constant rate -- so the counts DROP at systole, as on the MAX30101.
// systolicTime() is the time of the systolic maximum of the pulse (= the
// minimum of the counts) of the beat at or before t.
double pulseWave (double t, double bpm);
double syntheticPpg (double t, double bpm, double dc, double amplitude);
double systolicTime (double t, double bpm);

} // namespace HeartRate
