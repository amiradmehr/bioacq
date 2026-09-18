#pragma once

// Display-only smoothing for PlotWidget (the PPG DC removal and the
// temperature moving average). Header-only and pure, so the selftest checks it.

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace Smoothing
{

// Trailing moving average over time: out[k] = the mean of the finite x[j]
// with j <= k and t[k] - t[j] <= windowSec. Working on timestamps keeps it
// right for re-timed samples and across rate changes; a gap longer than the
// window starts it afresh. A non-finite x[k] stays NaN (a gap in the trace).
// subtractMean: out[k] = x[k] - that mean instead, which removes the DC level
// and slow drift and keeps the faster part (the PPG pulses).
inline void trailingMean (const double *t, const double *x, std::size_t n, double windowSec, bool subtractMean,
    std::vector<double> &out)
{
    const double nan = std::numeric_limits<double>::quiet_NaN ();
    out.resize (n);
    double sum = 0.0;
    std::size_t count = 0, first = 0;
    for (std::size_t k = 0; k < n; ++k)
    {
        if (std::isfinite (x[k]))
        {
            sum += x[k];
            ++count;
        }
        while (first < k && t[k] - t[first] > windowSec)
        {
            if (std::isfinite (x[first]))
            {
                sum -= x[first];
                --count;
            }
            ++first;
        }
        if (!std::isfinite (x[k]) || count == 0)
            out[k] = nan;
        else
        {
            const double mean = sum / static_cast<double> (count);
            out[k] = subtractMean ? x[k] - mean : mean;
        }
    }
}

} // namespace Smoothing
