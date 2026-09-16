#pragma once

#include <cmath>
#include <vector>

// Second-order IIR section (RBJ "Audio EQ Cookbook" designs), Direct Form II
// transposed, double precision. Used sample-by-sample in the device worker.
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0; // normalised by a0
    double a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;
    bool primed = false;

    static Biquad identity ()
    {
        return Biquad ();
    }

    // 2nd-order Butterworth-style high-pass (Q = 1/sqrt(2) by default).
    static Biquad highPass (double fs, double f0, double q = 0.7071067811865476)
    {
        if (!(fs > 0.0) || !(f0 > 0.0) || f0 >= 0.49 * fs)
            return identity ();
        const double w0 = 2.0 * M_PI * f0 / fs;
        const double c = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        Biquad f;
        f.b0 = (1.0 + c) / 2.0 / a0;
        f.b1 = -(1.0 + c) / a0;
        f.b2 = (1.0 + c) / 2.0 / a0;
        f.a1 = -2.0 * c / a0;
        f.a2 = (1.0 - alpha) / a0;
        return f;
    }

    // 2nd-order Butterworth-style low-pass (Q = 1/sqrt(2) by default).
    static Biquad lowPass (double fs, double f0, double q = 0.7071067811865476)
    {
        if (!(fs > 0.0) || !(f0 > 0.0) || f0 >= 0.49 * fs)
            return identity ();
        const double w0 = 2.0 * M_PI * f0 / fs;
        const double c = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        Biquad f;
        f.b0 = (1.0 - c) / 2.0 / a0;
        f.b1 = (1.0 - c) / a0;
        f.b2 = (1.0 - c) / 2.0 / a0;
        f.a1 = -2.0 * c / a0;
        f.a2 = (1.0 - alpha) / a0;
        return f;
    }

    // Band-stop (notch). Q = f0 / bandwidth; Q = 30 at 60 Hz -> ~2 Hz wide.
    static Biquad notch (double fs, double f0, double q = 30.0)
    {
        if (!(fs > 0.0) || !(f0 > 0.0) || f0 >= 0.49 * fs)
            return identity ();
        const double w0 = 2.0 * M_PI * f0 / fs;
        const double c = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        Biquad f;
        f.b0 = 1.0 / a0;
        f.b1 = -2.0 * c / a0;
        f.b2 = 1.0 / a0;
        f.a1 = -2.0 * c / a0;
        f.a2 = (1.0 - alpha) / a0;
        return f;
    }

    double dcGain () const
    {
        const double den = 1.0 + a1 + a2;
        return std::fabs (den) < 1e-15 ? 0.0 : (b0 + b1 + b2) / den;
    }

    void reset ()
    {
        z1 = z2 = 0.0;
        primed = false;
    }

    double process (double x)
    {
        if (!std::isfinite (x))
            return x; // never poison the filter state with NaN/Inf
        if (!primed)
        {
            // Initialise the state to the steady state for a constant input x,
            // so a large electrode DC offset does not ring through the filter.
            const double y = dcGain () * x;
            z2 = b2 * x - a2 * y;
            z1 = y - b0 * x;
            primed = true;
        }
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Cascade of biquads.
struct FilterChain
{
    std::vector<Biquad> stages;

    bool empty () const
    {
        return stages.empty ();
    }

    // Magnitude response of the cascade at f (Hz) for sample rate fs.
    double gainAt (double fs, double f) const
    {
        const double w = 2.0 * M_PI * f / fs;
        const double c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2.0 * w), s2 = std::sin (2.0 * w);
        double g = 1.0;
        for (const Biquad &s : stages)
        {
            const double nr = s.b0 + s.b1 * c1 + s.b2 * c2, ni = -(s.b1 * s1 + s.b2 * s2);
            const double dr = 1.0 + s.a1 * c1 + s.a2 * c2, di = -(s.a1 * s1 + s.a2 * s2);
            const double den = dr * dr + di * di;
            g *= den > 0.0 ? std::sqrt ((nr * nr + ni * ni) / den) : 0.0;
        }
        return g;
    }

    void reset ()
    {
        for (auto &s : stages)
            s.reset ();
    }

    double process (double x)
    {
        for (auto &s : stages)
            x = s.process (x);
        return x;
    }
};
