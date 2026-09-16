#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

// Fixed-capacity, multi-channel ring buffer of timestamped samples.
//
// One producer (a device worker thread) appends batches; readers (the GUI
// thread, the headless runner) copy out a time window. A single mutex guards
// the storage; critical sections are short copy loops over at most a few
// thousand doubles, so contention is negligible at biosignal sample rates.
// Memory is allocated once in the constructor and never again.
class SignalRing
{
public:
    SignalRing (int channels, std::size_t capacity)
        : nch_ (channels > 0 ? channels : 1)
        , cap_ (capacity > 0 ? capacity : 1)
        , t_ (cap_, 0.0)
        , v_ (cap_ * static_cast<std::size_t> (nch_), 0.0)
    {
    }

    SignalRing (const SignalRing &) = delete;
    SignalRing &operator= (const SignalRing &) = delete;

    int channels () const
    {
        return nch_;
    }
    std::size_t capacity () const
    {
        return cap_;
    }

    // Append n samples. ts[i] is the timestamp of sample i; vals[c][i] is the
    // value of channel c (vals must have channels() entries).
    void append (const double *ts, const double *const *vals, std::size_t n)
    {
        if (n == 0)
            return;
        std::lock_guard<std::mutex> lock (m_);
        const std::size_t skip = n > cap_ ? n - cap_ : 0; // only the newest cap_ survive
        for (std::size_t i = skip; i < n; ++i)
        {
            t_[head_] = ts[i];
            for (int c = 0; c < nch_; ++c)
                v_[static_cast<std::size_t> (c) * cap_ + head_] = vals[c][i];
            head_ = (head_ + 1 == cap_) ? 0 : head_ + 1;
        }
        size_ = std::min (cap_, size_ + (n - skip));
        latestTs_ = ts[n - 1];
        written_.fetch_add (n, std::memory_order_release);
    }

    // Copy every sample whose timestamp is >= tmin, oldest first, into t and
    // v[c]. The scan runs backwards from the newest sample and stops at the
    // first older sample, so the cost is proportional to the window, not the
    // capacity. Output vectors are resized (capacity is reused across calls).
    std::size_t copySince (
        double tmin, std::vector<double> &t, std::vector<std::vector<double>> &v) const
    {
        std::lock_guard<std::mutex> lock (m_);
        std::size_t k = 0;
        std::size_t idx = head_;
        while (k < size_)
        {
            idx = (idx == 0) ? cap_ - 1 : idx - 1;
            if (t_[idx] < tmin)
                break;
            ++k;
        }
        const std::size_t start = (head_ + cap_ - k) % cap_;
        t.resize (k);
        v.resize (static_cast<std::size_t> (nch_));
        for (auto &ch : v)
            ch.resize (k);

        // copy in (at most) two contiguous chunks
        const std::size_t first = std::min (k, cap_ - start);
        std::copy (t_.begin () + start, t_.begin () + start + first, t.begin ());
        std::copy (t_.begin (), t_.begin () + (k - first), t.begin () + first);
        for (int c = 0; c < nch_; ++c)
        {
            const double *src = v_.data () + static_cast<std::size_t> (c) * cap_;
            double *dst = v[static_cast<std::size_t> (c)].data ();
            std::copy (src + start, src + start + first, dst);
            std::copy (src, src + (k - first), dst + first);
        }
        return k;
    }

    // Newest sample (returns false if empty).
    bool latest (double &ts, std::vector<double> &vals) const
    {
        std::lock_guard<std::mutex> lock (m_);
        if (size_ == 0)
            return false;
        const std::size_t idx = (head_ == 0) ? cap_ - 1 : head_ - 1;
        ts = t_[idx];
        vals.resize (static_cast<std::size_t> (nch_));
        for (int c = 0; c < nch_; ++c)
            vals[static_cast<std::size_t> (c)] = v_[static_cast<std::size_t> (c) * cap_ + idx];
        return true;
    }

    std::size_t size () const
    {
        std::lock_guard<std::mutex> lock (m_);
        return size_;
    }

    double latestTimestamp () const
    {
        std::lock_guard<std::mutex> lock (m_);
        return size_ ? latestTs_ : std::numeric_limits<double>::quiet_NaN ();
    }

    // Monotonic count of samples ever appended (lock-free; used for change
    // detection and rate measurement).
    std::uint64_t totalWritten () const
    {
        return written_.load (std::memory_order_acquire);
    }

    void clear ()
    {
        std::lock_guard<std::mutex> lock (m_);
        head_ = 0;
        size_ = 0;
    }

private:
    const int nch_;
    const std::size_t cap_;
    mutable std::mutex m_;
    std::vector<double> t_;
    std::vector<double> v_; // channel-major: v_[c * cap_ + i]
    std::size_t head_ = 0;  // next write position
    std::size_t size_ = 0;
    double latestTs_ = 0.0;
    std::atomic<std::uint64_t> written_ {0};
};
