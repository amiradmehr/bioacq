#include "HeartRate.h"

#include <algorithm>
#include <cmath>

namespace HeartRate
{

namespace
{

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN ();
// plausible intervals: 30-200 bpm with 5 % slack for timing jitter
constexpr double kMinIbi = 60.0 / (kMaxBpm * 1.05);
constexpr double kMaxIbi = 60.0 / (kMinBpm / 1.05);
// Channels are re-estimated after each new beat, else at most this often.
constexpr double kEstimateIntervalSec = 0.25;

// Synthetic pulse: systolic centre, rise / fall widths, dicrotic wave (s).
struct PulseShape
{
    double period, sys, rise, fall, dic, dicWidth;
};

PulseShape shapeFor (double bpm)
{
    const double p = 60.0 / std::clamp (bpm, 1.0, 400.0);
    PulseShape s;
    s.period = p;
    s.sys = std::min (0.15, 0.35 * p);
    s.rise = std::min (0.05, 0.12 * p);
    s.fall = std::min (0.14, 0.25 * p);
    s.dic = s.sys + std::min (0.30, 0.35 * p);
    s.dicWidth = std::min (0.08, 0.12 * p);
    return s;
}

} // namespace

const char *sourceName (int source)
{
    switch (source)
    {
        case SourceGreen:
            return "GREEN";
        case SourceRed:
            return "RED";
        case SourceIr:
            return "IR";
        default:
            return "—";
    }
}

Estimate estimateFromBeats (const std::deque<double> &beats, double now, double windowSec)
{
    Estimate e;
    std::vector<double> all, inRange;
    double prev = kNaN;
    for (double b : beats)
    {
        if (!(b >= now - windowSec) || b > now + 1.0)
            continue;
        ++e.beats;
        if (std::isfinite (prev))
        {
            const double ibi = b - prev;
            all.push_back (ibi);
            if (ibi >= kMinIbi && ibi <= kMaxIbi)
                inRange.push_back (ibi);
        }
        prev = b;
    }
    e.lastBeat = prev;
    e.ibis = static_cast<int> (all.size ());
    if (inRange.empty ())
        return e;
    std::sort (inRange.begin (), inRange.end ());
    const std::size_t m = inRange.size ();
    const double median = (m % 2) ? inRange[m / 2] : 0.5 * (inRange[m / 2 - 1] + inRange[m / 2]);
    double sum = 0.0;
    int within = 0;
    for (double ibi : all)
        if (std::fabs (ibi - median) <= kIbiTolerance * median)
        {
            sum += ibi;
            ++within;
        }
    e.quality = static_cast<double> (within) / static_cast<double> (all.size ());
    e.bpm = within > 0 ? 60.0 * within / sum : 60.0 / median;
    const bool recent = now - prev <= std::max (2.0, 2.2 * median);
    e.valid = e.beats >= kMinBeats && e.quality >= kMinQuality && e.bpm >= 60.0 / kMaxIbi &&
        e.bpm <= 60.0 / kMinIbi && recent;
    return e;
}

// ------------------------------------------------------------ BeatDetector
BeatDetector::BeatDetector (double nominalFs)
{
    reset (nominalFs);
}

void BeatDetector::reset (double nominalFs)
{
    nominalFs_ = nominalFs > 0.0 ? nominalFs : 25.0;
    fs_ = nominalFs_;
    design ();
    count_ = 0;
    lastT_ = -std::numeric_limits<double>::infinity ();
    restart (0.0);
    absMean_ = 0.0;
    zMean_ = 0.0;
    lastBeatT_ = -std::numeric_limits<double>::infinity ();
    lastBeatY_ = 0.0;
    beats_.clear ();
    peaks_.clear ();
    total_ = 0;
    scale_ = 0.0;
    spanScale_ = 0.0;
    scaleUntil_ = -std::numeric_limits<double>::infinity ();
    span_ = 0.0;
    artefacts_ = 0;
}

void BeatDetector::design ()
{
    bp_.stages.clear ();
    bp_.stages.push_back (Biquad::highPass (fs_, kHighPassHz));
    bp_.stages.push_back (Biquad::lowPass (fs_, kLowPassHz));
    auto odd = [this] (double sec) { return std::max (1, 2 * static_cast<int> (std::lround (sec * fs_ / 2.0)) + 1); };
    n1_ = odd (kPeakWindowSec);
    n2_ = std::max (odd (kBeatWindowSec), n1_ + 2);
    ht_.assign (static_cast<std::size_t> (n2_), 0.0);
    hy_.assign (static_cast<std::size_t> (n2_), 0.0);
    hz_.assign (static_cast<std::size_t> (n2_), 0.0);
    const std::size_t nd = static_cast<std::size_t> (std::max (3, odd (kDetrendSec)));
    dy_.assign (nd, 0.0);
    dt_.assign (nd, 0.0);
    pStep_ = std::max (1, static_cast<int> (std::lround (fs_ / 25.0)));
    py_.assign (static_cast<std::size_t> (std::ceil (kWindowSec * fs_ / pStep_)) + 2, 0.0);
}

void BeatDetector::restart (double t)
{
    bp_.reset ();
    std::fill (hz_.begin (), hz_.end (), 0.0);
    head_ = 0;
    filled_ = 0;
    sumBeat_ = 0.0;
    pHead_ = 0;
    pFilled_ = 0;
    pCount_ = 0;
    dHead_ = 0;
    dFilled_ = 0;
    dSum_ = 0.0;
    artStart_ = -std::numeric_limits<double>::infinity ();
    artEnd_ = -std::numeric_limits<double>::infinity ();
    inBlock_ = false;
    blockLen_ = 0;
    settleUntil_ = t + kSettleSec;
    rateT0_ = t;
    rateSum_ = 0.0;
    rateIntervals_ = 0;
    rateChecked_ = false;
}

void BeatDetector::process (const double *t, const double *x, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i)
    {
        const double ti = t[i], xi = x[i];
        if (!std::isfinite (ti) || !std::isfinite (xi) || !(ti > lastT_))
            continue;
        if (count_ == 0)
        {
            absMean_ = std::fabs (xi);
            restart (ti);
        }
        else if (ti - lastT_ > kDropoutSec)
            restart (ti); // dropout: restart the filters, averages and rate check; keep the beats
        else if (ti - lastT_ <= 3.0 / nominalFs_)
        {
            rateSum_ += ti - lastT_; // a shorter gap is left out of the rate, not counted as samples
            ++rateIntervals_;
        }
        ++count_;
        lastT_ = ti;
        absMean_ += 0.01 * (std::fabs (xi) - absMean_);

        // The filters are designed at the nominal rate; redesign if the
        // timestamps of this uninterrupted stretch say the stream runs more
        // than kRateTolerance off it (within a plausible range of nominal).
        if (!rateChecked_ && ti - rateT0_ >= kRateCheckSec && rateIntervals_ >= 20 && rateSum_ > 0.0)
        {
            rateChecked_ = true;
            const double measured = std::clamp (static_cast<double> (rateIntervals_) / rateSum_,
                kMinRateFactor * nominalFs_, kMaxRateFactor * nominalFs_);
            if (std::fabs (measured - fs_) > kRateTolerance * fs_)
            {
                fs_ = measured;
                design ();
                restart (ti); // checks the new design again after kRateCheckSec
            }
        }

        // Band-pass, inverted: a systolic pulse is a maximum.
        const double y1 = -bp_.process (xi);
        // Artefact: beyond kArtefactFactor x the recent beat height. Clipped
        // before the baseline removal, so it cannot spread over the moving
        // average; the samples it reaches are masked below.
        const double lim = ti <= scaleUntil_ ? kArtefactFactor * spanScale_ : std::numeric_limits<double>::infinity ();
        const double maskBefore = delaySec (), maskAfter = delaySec () + kArtefactSettleSec;
        if (std::fabs (y1) > lim)
        {
            if (ti - artEnd_ > maskBefore + maskAfter)
                artStart_ = ti; // a new stretch (the output has passed the previous one)
            artEnd_ = ti;
        }

        // Baseline removal: the sample in the middle of the delay line minus
        // the mean of the whole line (centred moving average).
        const std::size_t nd = dy_.size ();
        const double y1c = std::clamp (y1, -lim, lim);
        span_ = std::max (span_, std::fabs (y1c));
        dSum_ += y1c - (dFilled_ == nd ? dy_[dHead_] : 0.0);
        dy_[dHead_] = y1c;
        dt_[dHead_] = ti;
        dHead_ = (dHead_ + 1) % nd;
        dFilled_ = std::min (dFilled_ + 1, nd);
        if (dFilled_ < nd)
            continue;
        if (dHead_ == 0)
        {
            dSum_ = 0.0; // re-sum once per turn: no rounding drift over hours
            for (double v : dy_)
                dSum_ += v;
        }
        const std::size_t mid = (dHead_ + nd / 2) % nd;
        const double tc = dt_[mid];
        const bool masked = tc >= artStart_ - maskBefore && tc <= artEnd_ + maskAfter;
        const double y = masked ? 0.0 : dy_[mid] - dSum_ / static_cast<double> (nd);
        const double z = y > 0.0 ? y * y : 0.0;
        if (pCount_++ % static_cast<std::uint64_t> (pStep_) == 0)
        {
            py_[pHead_] = y;
            pHead_ = (pHead_ + 1) % py_.size ();
            pFilled_ = std::min (pFilled_ + 1, py_.size ());
        }
        const std::size_t cap = hz_.size ();
        sumBeat_ += z - hz_[head_];
        ht_[head_] = tc;
        hy_[head_] = y;
        hz_[head_] = z;
        head_ = (head_ + 1) % cap;
        filled_ = std::min (filled_ + 1, cap);
        const bool settling = tc < settleUntil_;
        zMean_ += (settling ? 0.2 : 1.0 / (10.0 * fs_)) * (z - zMean_);
        if (filled_ < cap || settling)
            continue;

        // Both moving averages are centred on sample c, the middle of the history.
        const int h1 = n1_ / 2, h2 = n2_ / 2;
        auto at = [&] (int k) { return (head_ + static_cast<std::size_t> (k)) % cap; }; // k = 0: oldest
        const std::size_t c = at (h2);
        double sumPeak = 0.0;
        for (int k = h2 - h1; k <= h2 + h1; ++k)
            sumPeak += hz_[at (k)];
        const double maPeak = sumPeak / n1_;
        const double maBeat = std::max (0.0, sumBeat_) / n2_;
        const double floorAmp = 1e-9 * absMean_ + 1e-12; // flat input: no beats from rounding noise
        if (maPeak > std::max (maBeat + kBeta * zMean_, floorAmp * floorAmp))
        {
            if (!inBlock_)
            {
                inBlock_ = true;
                blockLen_ = 0;
                peakY_ = -std::numeric_limits<double>::infinity ();
            }
            ++blockLen_;
            if (hy_[c] > peakY_)
            {
                const std::size_t cp = at (h2 - 1), cn = at (h2 + 1);
                peakY_ = hy_[c];
                peakT_ = ht_[c];
                peakPrevY_ = hy_[cp];
                peakPrevT_ = ht_[cp];
                peakNextY_ = hy_[cn];
                peakNextT_ = ht_[cn];
            }
        }
        else if (inBlock_)
        {
            inBlock_ = false;
            if (blockLen_ >= n1_)
            {
                // parabola through the block maximum and its neighbours
                const double den = peakPrevY_ - 2.0 * peakY_ + peakNextY_;
                const double delta = std::clamp (den < 0.0 ? 0.5 * (peakPrevY_ - peakNextY_) / den : 0.0, -0.5, 0.5);
                const double tp = peakT_ + delta * (delta > 0.0 ? peakNextT_ - peakT_ : peakT_ - peakPrevT_);
                candidate (tp, peakY_ - 0.25 * (peakPrevY_ - peakNextY_) * delta);
            }
        }
    }
    while (!beats_.empty () && beats_.front () < lastT_ - 3.0 * kWindowSec)
        beats_.pop_front ();
}

void BeatDetector::candidate (double tp, double yp)
{
    if (tp <= scaleUntil_ && yp > kArtefactFactor * scale_)
    {
        ++artefacts_; // motion or contact artefact: neither a beat nor a gate reference
        return;
    }
    double ref = 0.0; // largest recent beat
    for (std::size_t i = peaks_.size (); i-- > 0;)
    {
        if (peaks_[i].t < tp - kGateMemorySec)
            break;
        ref = std::max (ref, peaks_[i].height);
    }
    if (yp < kAmplitudeGate * ref)
        return;
    if (tp - lastBeatT_ < kMinIbi)
    {
        // refractory period: keep the larger peak
        if (yp > lastBeatY_ && !beats_.empty ())
        {
            beats_.back () = tp;
            peaks_.back () = {tp, yp, std::max (peaks_.back ().span, span_)};
            span_ = 0.0;
            lastBeatT_ = tp;
            lastBeatY_ = yp;
            updateScale (tp);
        }
        return;
    }
    beats_.push_back (tp);
    peaks_.push_back ({tp, yp, span_});
    span_ = 0.0;
    while (peaks_.size () > 16)
        peaks_.pop_front ();
    ++total_;
    lastBeatT_ = tp;
    lastBeatY_ = yp;
    updateScale (tp);
}

void BeatDetector::updateScale (double t)
{
    // medians over the newest (up to 8) beats of the last kWindowSec
    double h[8], sp[8];
    int m = 0;
    double until = -std::numeric_limits<double>::infinity ();
    for (std::size_t i = peaks_.size (); i-- > 0 && m < 8;)
    {
        if (peaks_[i].t < t - kWindowSec)
            break;
        h[m] = peaks_[i].height;
        sp[m] = peaks_[i].span;
        if (++m == kScaleBeats)
            until = peaks_[i].t + kWindowSec; // fewer than kScaleBeats left in the window after this
    }
    if (m < kScaleBeats)
    {
        scaleUntil_ = -std::numeric_limits<double>::infinity ();
        return;
    }
    auto median = [m] (double *v) {
        std::sort (v, v + m);
        return (m % 2) ? v[m / 2] : 0.5 * (v[m / 2 - 1] + v[m / 2]);
    };
    scale_ = median (h);
    spanScale_ = median (sp);
    scaleUntil_ = until;
}

double BeatDetector::correlation (int lag) const
{
    const std::size_t w = pFilled_;
    const std::size_t l = static_cast<std::size_t> (std::max (1, lag));
    if (l >= w)
        return 0.0;
    const std::size_t cap = py_.size ();
    auto at = [&] (std::size_t k) { return py_[(pHead_ + cap - w + k) % cap]; }; // k = 0: oldest
    double sxy = 0.0, sxx = 0.0, syy = 0.0;
    for (std::size_t k = l; k < w; ++k)
    {
        const double a = at (k), b = at (k - l);
        sxy += a * b;
        sxx += a * a;
        syy += b * b;
    }
    const double den = std::sqrt (sxx * syy);
    return den > 0.0 ? sxy / den : 0.0;
}

double BeatDetector::periodicity (double ibiSec) const
{
    const double fsDec = fs_ / pStep_;
    const int l0 = static_cast<int> (std::lround (ibiSec * fsDec));
    if (!(ibiSec > 0.0) || l0 < 2 || pFilled_ < static_cast<std::size_t> (2 * l0))
        return 0.0;
    // Best correlation within +-20 % of the beat interval ...
    double best = -1.0;
    int bestLag = l0;
    const int lagHi = static_cast<int> (std::ceil (1.2 * l0));
    for (int lag = std::max (2, static_cast<int> (std::floor (0.8 * l0))); lag <= lagHi; ++lag)
    {
        const double r = correlation (lag);
        if (r > best)
        {
            best = r;
            bestLag = lag;
        }
    }
    // ... minus the correlation half a beat away: a pulse train decorrelates
    // there, while slow baseline wander (respiration) stays correlated.
    const double half = correlation (static_cast<int> (std::lround (bestLag / 2.0)));
    return best - std::max (0.0, half);
}

double BeatDetector::periodicityThreshold () const
{
    // The correlation of a short buffer (after the start or a dropout) is
    // noisier, so noise passes kMinPeriodicity more often: scale the gate by
    // sqrt(kWindowSec / buffered seconds).
    const double sec = static_cast<double> (pFilled_) * pStep_ / fs_;
    return kMinPeriodicity * std::sqrt (kWindowSec / std::clamp (sec, 1.0, kWindowSec));
}

Estimate BeatDetector::estimate (double now) const
{
    Estimate e = estimateFromBeats (beats_, now - delaySec ());
    if (std::isfinite (e.bpm))
    {
        e.periodicity = periodicity (60.0 / e.bpm);
        e.valid = e.valid && e.periodicity >= periodicityThreshold ();
    }
    return e;
}

// ------------------------------------------------------------ Tracker
Tracker::Tracker (double nominalFs)
{
    reset (nominalFs);
}

void Tracker::reset (double nominalFs)
{
    for (int c = 0; c < kSources; ++c)
    {
        det_[c].reset (nominalFs);
        est_[c] = Estimate ();
        estBeats_[c] = 0;
        estAt_[c] = -std::numeric_limits<double>::infinity ();
    }
    source_ = SourceNone;
    candidate_ = SourceNone;
    candidateSince_ = 0.0;
    publishedSource_ = SourceNone;
    publishedBeats_ = 0;
    publishedValid_ = false;
    lastPublished_ = -std::numeric_limits<double>::infinity ();
}

void Tracker::process (int channel, const double *t, const double *x, std::size_t n)
{
    if (channel >= 0 && channel < kSources)
        det_[channel].process (t, x, n);
}

void Tracker::update (double now, std::vector<Sample> &out)
{
    int best = SourceNone;
    for (int c = 0; c < kSources; ++c)
    {
        if (!det_[c].hasData ())
            est_[c] = Estimate ();
        else if (det_[c].totalBeats () != estBeats_[c] || now - estAt_[c] >= kEstimateIntervalSec || now < estAt_[c])
        {
            est_[c] = det_[c].estimate (now);
            estBeats_[c] = det_[c].totalBeats ();
            estAt_[c] = now;
        }
        if (est_[c].valid && (best == SourceNone || est_[c].quality > est_[best].quality + 1e-9))
            best = c;
    }

    // source selection with hysteresis
    if (source_ == SourceNone || !est_[source_].valid)
    {
        source_ = best;
        candidate_ = SourceNone;
    }
    else if (best != SourceNone && best != source_ && est_[best].quality >= est_[source_].quality + kSwitchMargin)
    {
        if (candidate_ != best)
        {
            candidate_ = best;
            candidateSince_ = now;
        }
        else if (now - candidateSince_ >= kSwitchHoldSec)
        {
            source_ = best;
            candidate_ = SourceNone;
        }
    }
    else
        candidate_ = SourceNone;

    auto publish = [&] (double t, const Estimate &e, int src, bool valid) {
        Sample s;
        s.t = std::max (t, lastPublished_ + 1e-6);
        s.bpm = valid ? e.bpm : kNaN;
        s.quality = e.quality;
        s.periodicity = e.periodicity;
        s.source = src;
        s.beats = e.beats;
        lastPublished_ = s.t;
        out.push_back (s);
    };

    if (source_ != SourceNone && est_[source_].valid)
    {
        const std::uint64_t total = det_[source_].totalBeats ();
        if (!publishedValid_ || publishedSource_ != source_ || total != publishedBeats_)
        {
            const double tb = est_[source_].lastBeat;
            publish (std::isfinite (tb) ? tb : now, est_[source_], source_, true);
            publishedSource_ = source_;
            publishedBeats_ = total;
            publishedValid_ = true;
        }
        return;
    }

    // No valid HR: publish the best channel's status now and then (NaN = gap).
    int shown = SourceNone;
    for (int c = 0; c < kSources; ++c)
        if (det_[c].hasData () && (shown == SourceNone || est_[c].quality > est_[shown].quality))
            shown = c;
    if (shown == SourceNone)
        return;
    if (publishedValid_ || now - lastPublished_ >= kStatusIntervalSec)
    {
        publish (now, est_[shown], SourceNone, false);
        publishedValid_ = false;
        publishedSource_ = SourceNone;
    }
}

// ------------------------------------------------------------ synthetic PPG
double pulseWave (double t, double bpm)
{
    const PulseShape s = shapeFor (bpm);
    double ph = std::fmod (t, s.period);
    if (ph < 0.0)
        ph += s.period;
    double v = 0.0;
    for (int k = -1; k <= 1; ++k) // neighbouring beats keep the wave smooth across the wrap
    {
        const double x = ph - k * s.period;
        const double a = (x - s.sys) / (x < s.sys ? s.rise : s.fall);
        const double d = (x - s.dic) / s.dicWidth;
        v += std::exp (-0.5 * a * a) + 0.35 * std::exp (-0.5 * d * d);
    }
    return v;
}

double syntheticPpg (double t, double bpm, double dc, double amplitude)
{
    return dc - amplitude * pulseWave (t, bpm);
}

double systolicTime (double t, double bpm)
{
    const PulseShape s = shapeFor (bpm);
    return std::floor ((t - s.sys) / s.period) * s.period + s.sys;
}

} // namespace HeartRate
