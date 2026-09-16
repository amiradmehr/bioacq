#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

// Rebuilds per-sample display timestamps for packetised streams (EmotiBit).
//
// An EmotiBit packet carries several samples (BrainFlow notes up to ~9), and
// BrainFlow's emotibit.cpp read_thread stamps every sample of a packet with
// get_timestamp() inside one tight loop, so the stamps of a packet differ by
// only a few microseconds. Plotted as is, each packet collapses onto a single
// x position (a comb / staircase instead of a waveform).
//
// PacketRetimer groups consecutive samples whose stamps are closer than a
// small tolerance (one packet), and spreads a packet of k samples ending at
// time T evenly over the interval since the previous packet:
//     t_j = lastOut + (j + 1) * (T - lastOut) / k,     j = 0..k-1
// The first packet, and a packet after a dropout (interval > 2 k periods),
// is laid out backwards from T at the estimated sample period. Output is
// strictly increasing. Only the display ring uses the result; BrainFlow's
// file-streamer recordings keep BrainFlow's raw timestamps.
class PacketRetimer
{
public:
    explicit PacketRetimer (double nominalRate = 25.0)
    {
        reset (nominalRate);
    }

    void reset (double nominalRate)
    {
        nominalPeriod_ = nominalRate > 0.0 ? 1.0 / nominalRate : 0.04;
        period_ = nominalPeriod_;
        lastOut_ = std::numeric_limits<double>::quiet_NaN ();
        lastGroupT_ = std::numeric_limits<double>::quiet_NaN ();
    }

    double period () const
    {
        return period_;
    }

    // in/out may alias. n timestamps as delivered by BrainFlow (one poll).
    void process (const double *in, double *out, std::size_t n)
    {
        const double tol = std::min (0.005, 0.25 * nominalPeriod_);
        const double eps = 1e-6;
        std::size_t a = 0;
        while (a < n)
        {
            std::size_t b = a + 1;
            double tMax = in[a];
            while (b < n && std::fabs (in[b] - in[b - 1]) < tol)
            {
                tMax = std::max (tMax, in[b]);
                ++b;
            }
            const std::size_t k = b - a;
            const double T = tMax;
            const double first = in[a];
            const bool haveLast = std::isfinite (lastOut_);
            // Rest of a packet that BrainFlow was still pushing when we polled.
            const bool continuation = std::isfinite (lastGroupT_) && std::fabs (first - lastGroupT_) < tol;

            double prev = haveLast ? lastOut_ : -std::numeric_limits<double>::infinity ();
            for (std::size_t j = 0; j < k; ++j)
            {
                double t;
                const double span = haveLast ? T - lastOut_ : -1.0;
                if (continuation && haveLast)
                    t = lastOut_ + static_cast<double> (j + 1) * period_;
                else if (haveLast && span > 0.0 && span <= 2.0 * static_cast<double> (k) * period_ + tol)
                    t = lastOut_ + static_cast<double> (j + 1) * span / static_cast<double> (k);
                else // first packet, dropout, or clock step: end-align at T
                    t = T - static_cast<double> (k - 1 - j) * period_;
                t = std::max (t, prev + eps);
                out[a + j] = t;
                prev = t;
            }

            // Track the real sample period (only from ordinary packet intervals).
            if (haveLast && !continuation)
            {
                const double span = T - lastOut_;
                const double measured = span / static_cast<double> (k);
                if (measured > 0.25 * nominalPeriod_ && measured < 4.0 * nominalPeriod_)
                    period_ += 0.1 * (measured - period_);
            }
            lastOut_ = out[b - 1];
            lastGroupT_ = T;
            a = b;
        }
    }

private:
    double nominalPeriod_ = 0.04;
    double period_ = 0.04;
    double lastOut_ = std::numeric_limits<double>::quiet_NaN ();
    double lastGroupT_ = std::numeric_limits<double>::quiet_NaN ();
};
