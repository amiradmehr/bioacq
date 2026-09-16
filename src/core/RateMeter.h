#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <utility>

// Host wall clock as UNIX epoch seconds -- the same clock BrainFlow uses for its
// timestamp rows (gettimeofday), so "ts - wallClockSeconds()" is the sample age.
inline double wallClockSeconds ()
{
    using namespace std::chrono;
    return duration_cast<duration<double>> (system_clock::now ().time_since_epoch ()).count ();
}

// Effective sample rate over a sliding window, fed with (time, total sample
// count) snapshots, e.g. from SignalRing::totalWritten() every ~250 ms.
class RateMeter
{
public:
    explicit RateMeter (double windowSec = 2.0) : win_ (windowSec)
    {
    }

    void reset ()
    {
        hist_.clear ();
        lastCount_ = 0;
        lastChange_ = 0.0;
        haveChange_ = false;
    }

    void sample (double now, std::uint64_t count)
    {
        if (!hist_.empty () && count < hist_.back ().second)
            hist_.clear (); // counter restarted (new ring)
        hist_.emplace_back (now, count);
        // keep the newest snapshot that is at least win_ old as the reference
        while (hist_.size () > 2 && now - hist_[1].first >= win_)
            hist_.pop_front ();
        if (!haveChange_ || count != lastCount_)
        {
            lastCount_ = count;
            lastChange_ = now;
            haveChange_ = true;
        }
    }

    double rate () const
    {
        if (hist_.size () < 2)
            return 0.0;
        const auto &a = hist_.front ();
        const auto &b = hist_.back ();
        const double dt = b.first - a.first;
        return dt > 0.0 ? static_cast<double> (b.second - a.second) / dt : 0.0;
    }

    // Seconds since the sample count last changed (0 if never sampled).
    double secondsSinceChange (double now) const
    {
        return haveChange_ ? now - lastChange_ : 0.0;
    }

    std::uint64_t count () const
    {
        return lastCount_;
    }

private:
    double win_;
    std::deque<std::pair<double, std::uint64_t>> hist_;
    std::uint64_t lastCount_ = 0;
    double lastChange_ = 0.0;
    bool haveChange_ = false;
};
