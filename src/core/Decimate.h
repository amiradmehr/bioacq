#pragma once

#include <QPolygonF>

#include <climits>
#include <cmath>
#include <cstddef>
#include <vector>

// Maps (time, value) to widget pixel coordinates.
struct PixelMap
{
    double tStart = 0.0; // time at the left edge of the plot area
    double tSpan = 1.0;  // seconds across the plot area
    double left = 0.0, width = 1.0;
    double vLo = 0.0, vSpan = 1.0; // value at bottom edge, value range
    double top = 0.0, height = 1.0;

    double x (double t) const
    {
        return left + (t - tStart) / tSpan * width;
    }
    double y (double v) const
    {
        return top + height - (v - vLo) / vSpan * height;
    }
    double secPerPx () const
    {
        return tSpan / (width > 0.0 ? width : 1.0);
    }
};

// Density thresholds (samples per pixel column) for switching a trace between
// exact and min/max-decimated drawing. The gap between them is hysteresis, so
// a stream sitting near the boundary does not flip mode from frame to frame.
constexpr double kDecimateAbove = 2.5;
constexpr double kExactBelow = 2.0;

// Build polylines for samples (t[i], v[i] - vOffset), i in [0, n).
//
// * The series is split into separate segments wherever consecutive samples
//   are more than gapSec apart (so dropouts show as gaps, not ramps), and at
//   every non-finite value (NaN / Inf are skipped and drawn as gaps, so no
//   NaN vertex ever reaches QPainter).
// * A dense segment is decimated to the per-bin min and max, emitted in their
//   original order, so the number of points never exceeds 2 x (bins spanned)
//   regardless of sample rate. Bins are one pixel wide but anchored in
//   ABSOLUTE time (bin = floor(t / secPerPx)), not in screen columns: as the
//   plot scrolls, a sample always stays in the same bin and the whole
//   polyline just translates by a sub-pixel offset, so envelopes and peaks do
//   not shimmer. Sparse segments keep every sample at its exact x position.
// * *denseMode (optional, in/out, per trace) carries the exact/decimated
//   decision between frames with hysteresis (kDecimateAbove / kExactBelow).
// * denseFlags (optional) receives, per filled segment, 1 if it was decimated
//   (the caller draws those without antialiasing).
//
// 'segs' is reused across calls (QPolygonF keeps its capacity); the return
// value is the number of segments filled. totalPoints receives the point count.
inline int buildPolylines (const double *t, const double *v, std::size_t n, double vOffset,
    const PixelMap &m, double gapSec, std::vector<QPolygonF> &segs, std::size_t *totalPoints = nullptr,
    std::vector<unsigned char> *denseFlags = nullptr, bool *denseMode = nullptr)
{
    int used = 0;
    std::size_t points = 0;
    std::size_t a = 0;
    const double spp = m.secPerPx ();
    const bool wasDense = denseMode ? *denseMode : false;
    std::size_t biggest = 0;
    bool biggestDense = wasDense;
    if (denseFlags)
        denseFlags->clear ();

    auto finite = [&] (std::size_t i) { return std::isfinite (t[i]) && std::isfinite (v[i]); };
    while (a < n)
    {
        if (!finite (a))
        {
            ++a; // non-finite sample: part of a gap
            continue;
        }
        std::size_t b = a + 1;
        while (b < n && finite (b) && std::fabs (t[b] - t[b - 1]) <= gapSec)
            ++b;

        if (static_cast<int> (segs.size ()) <= used)
            segs.emplace_back ();
        QPolygonF &poly = segs[static_cast<std::size_t> (used++)];
        poly.resize (0);

        const double x0 = m.x (t[a]);
        const double x1 = m.x (t[b - 1]);
        const double columns = std::fabs (x1 - x0) + 1.0;
        const std::size_t count = b - a;
        const double density = static_cast<double> (count) / columns;
        const bool dense = denseMode ? (wasDense ? density >= kExactBelow : density > kDecimateAbove)
                                     : density > 2.0;
        if (count > biggest)
        {
            biggest = count;
            biggestDense = dense;
        }

        if (!dense)
        {
            poly.reserve (static_cast<qsizetype> (count));
            for (std::size_t i = a; i < b; ++i)
                poly.append (QPointF (m.x (t[i]), m.y (v[i] - vOffset)));
        }
        else
        {
            poly.reserve (static_cast<qsizetype> (2.0 * columns + 4.0));
            long long bin = LLONG_MIN;
            double minY = 0.0, maxY = 0.0;
            std::size_t minI = 0, maxI = 0;
            auto flush = [&] () {
                if (bin == LLONG_MIN)
                    return;
                const double cx = m.x ((static_cast<double> (bin) + 0.5) * spp);
                if (minI == maxI)
                    poly.append (QPointF (cx, minY));
                else if (minI < maxI)
                {
                    poly.append (QPointF (cx, minY));
                    poly.append (QPointF (cx, maxY));
                }
                else
                {
                    poly.append (QPointF (cx, maxY));
                    poly.append (QPointF (cx, minY));
                }
            };
            for (std::size_t i = a; i < b; ++i)
            {
                const double y = m.y (v[i] - vOffset);
                const long long c = static_cast<long long> (std::floor (t[i] / spp));
                if (c != bin)
                {
                    flush ();
                    bin = c;
                    minY = maxY = y;
                    minI = maxI = i;
                }
                else
                {
                    if (y < minY)
                    {
                        minY = y;
                        minI = i;
                    }
                    if (y > maxY)
                    {
                        maxY = y;
                        maxI = i;
                    }
                }
            }
            flush ();
        }
        if (denseFlags)
            denseFlags->push_back (dense ? 1 : 0);
        points += static_cast<std::size_t> (poly.size ());
        a = b;
    }
    if (denseMode && biggest > 0)
        *denseMode = biggestDense;
    if (totalPoints)
        *totalPoints = points;
    return used;
}
