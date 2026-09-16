#include "PlotWidget.h"

#include "Decimate.h"
#include "Readouts.h"
#include "RingBuffer.h"
#include "Theme.h"

#include <QFontMetricsF>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace
{

// Trace pens. The design asks for 1.4 px (hero) / 1.3 px (panels). Any pen
// wider than 1 device pixel leaves Qt raster's cosmetic fast path for the
// stroker: measured at 100 % of one core (offscreen 1600x1000, both slots
// synthetic, 1 s window, every segment sparse) versus ~18 % with cosmetic
// pens. So every trace is drawn with a cosmetic 1 device-px pen; exact
// (non-decimated) segments keep the design's dash pattern (cosmetic dashes
// are still on the fast path), min/max-decimated ones are solid and aliased.
// kDesignPenWhenSparse re-enables the true width for segments with at most
// kSparseDensity points per pixel (kept for evaluation; off by default).
constexpr bool kDesignPenWhenSparse = false;
constexpr double kSparseDensity = 1.0;

double niceStep (double raw)
{
    if (!(raw > 0.0) || !std::isfinite (raw))
        return 1.0;
    const double e = std::floor (std::log10 (raw));
    const double base = std::pow (10.0, e);
    const double f = raw / base;
    const double nf = f < 1.5 ? 1.0 : (f < 3.0 ? 2.0 : (f < 7.0 ? 5.0 : 10.0));
    return nf * base;
}

int decimalsForStep (double step)
{
    if (!(step > 0.0) || !std::isfinite (step))
        return 0;
    return std::clamp (static_cast<int> (std::ceil (-std::log10 (step) - 1e-9)), 0, 6);
}

double steadySeconds ()
{
    using namespace std::chrono;
    return duration<double> (steady_clock::now ().time_since_epoch ()).count ();
}

// Cosmetic pens (width in device pixels, <= 1) take Qt raster's fast line
// path at any device-pixel ratio; non-cosmetic or wider pens go through the
// stroker and cost ~100x more on dense, zig-zagging traces.
QPen cosmeticPen (const QColor &c, double width = 1.0)
{
    QPen pen (c, width);
    pen.setCosmetic (true);
    return pen;
}

void outline (QPainter &p, const QRect &r, const QColor &c)
{
    if (r.width () <= 0 || r.height () <= 0)
        return;
    p.fillRect (QRect (r.left (), r.top (), r.width (), 1), c);
    p.fillRect (QRect (r.left (), r.bottom (), r.width (), 1), c);
    p.fillRect (QRect (r.left (), r.top (), 1, r.height ()), c);
    p.fillRect (QRect (r.right (), r.top (), 1, r.height ()), c);
}

// Baseline that vertically centres the font's line box on cy.
double baselineFor (const QFontMetricsF &fm, double cy)
{
    return cy + (fm.ascent () - fm.descent ()) / 2.0;
}

// 14 x 6 legend line in the trace's colour and dash pattern (4 2.5 / 1 2.5).
void legendGlyph (QPainter &p, const PlotWidget::Trace &tr, double x0, double cy)
{
    QPen pen (tr.color, 2.0);
    pen.setCapStyle (Qt::FlatCap);
    if (tr.dash == PlotWidget::Dash::Dashed)
        pen.setDashPattern ({4.0 / 2.0, 2.5 / 2.0});
    else if (tr.dash == PlotWidget::Dash::Dotted)
        pen.setDashPattern ({1.0 / 2.0, 2.5 / 2.0});
    p.save ();
    p.setRenderHint (QPainter::Antialiasing);
    p.setPen (pen);
    p.drawLine (QPointF (x0, std::round (cy)), QPointF (x0 + 14.0, std::round (cy)));
    p.restore ();
}

const QString kNoValue = QStringLiteral ("——");

} // namespace

PlotWidget::PlotWidget (Kind kind, const QString &title, const QVector<LaneSpec> &lanes, QWidget *parent)
    : QWidget (parent), kind_ (kind), title_ (title), led_ (Theme::controlEdge), rateColor_ (Theme::textDim),
      placeholder_ (QStringLiteral ("NO SIGNAL"))
{
    for (const LaneSpec &s : lanes)
    {
        Lane L;
        L.spec = s;
        lanes_.push_back (L);
    }
    if (lanes_.empty ())
        lanes_.push_back (Lane ());
    init ();
}

PlotWidget::PlotWidget (
    Kind kind, const QString &title, const QString &units, const QVector<Trace> &traces, QWidget *parent)
    : PlotWidget (kind, title, QVector<LaneSpec> {{QString (), units, traces, 0.0}}, parent)
{
}

void PlotWidget::init ()
{
    setAttribute (Qt::WA_OpaquePaintEvent);
    setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Expanding);

    const bool hero = kind_ == Kind::Hero;
    fTitle_ = Theme::mono (hero ? 11.0 : 10.5, 400, 0.14);
    fUnits_ = Theme::mono (10);
    fSub_ = Theme::sans (11);
    fChip_ = Theme::mono (9.5, 400, 0.1);
    fKicker_ = Theme::mono (9, 400, 0.12);
    fValue_ = (hero || kind_ == Kind::Vital) ? Theme::mono (19, 600) : Theme::mono (14, 600);
    fValueUnit_ = Theme::mono (11);
    fRate_ = Theme::mono (hero ? 12.0 : 10.0);
    fLetter_ = Theme::mono (9.5);
    fLegendValue_ = Theme::mono (11.5);
    fAxis_ = Theme::mono (hero ? 9.5 : 9.0);
    fEmpty_ = Theme::mono (hero ? 11.0 : 10.0, 400, 0.16);
    fStall_ = Theme::mono (10, 400, 0.14);

    for (Lane &L : lanes_)
    {
        const std::size_t nTr = static_cast<std::size_t> (L.spec.traces.size ());
        L.polys.resize (nTr);
        L.segCount.assign (nTr, 0);
        L.means.assign (nTr, 0.0);
        L.denseFlags.resize (nTr);
        L.denseMode.assign (nTr, 0);
        for (int i = 0; i < L.spec.traces.size (); ++i)
            valueText_ << kNoValue;
    }
    ppText_ = QStringLiteral ("— ") + units ();
}

// ------------------------------------------------------------------ setters
void PlotWidget::setTitle (const QString &title)
{
    if (title == title_)
        return;
    title_ = title;
    updateHeader ();
}

void PlotWidget::setUnits (const QString &units)
{
    setLaneUnits (0, units);
}

void PlotWidget::setLaneUnits (int lane, const QString &units)
{
    if (lane < 0 || lane >= laneCount () || units == lanes_[static_cast<std::size_t> (lane)].spec.units)
        return;
    lanes_[static_cast<std::size_t> (lane)].spec.units = units;
    cacheValid_ = false;
    refreshHeaderText ();
    update ();
}

void PlotWidget::setSubtitle (const QString &text)
{
    if (text == subtitle_)
        return;
    subtitle_ = text;
    updateHeader ();
}

void PlotWidget::setChips (const QVector<Chip> &chips)
{
    if (chips == chips_)
        return;
    chips_ = chips;
    updateHeader ();
}

void PlotWidget::setStripChips (const QVector<Chip> &chips)
{
    if (chips == stripChips_)
        return;
    stripChips_ = chips;
    updateHeader ();
}

void PlotWidget::setValueText (const QString &text)
{
    if (text == valueOverride_)
        return;
    valueOverride_ = text;
    if (refreshHeaderText ())
        updateHeader ();
}

void PlotWidget::setLed (const QColor &c)
{
    if (c == led_)
        return;
    led_ = c;
    updateHeader ();
}

void PlotWidget::setRate (const QString &text, const QColor &color)
{
    if (text == rateText_ && color == rateColor_)
        return;
    rateText_ = text;
    rateColor_ = color;
    updateHeader ();
}

void PlotWidget::setTone (Tone t)
{
    if (t == tone_)
        return;
    tone_ = t;
    refreshHeaderText ();
    updateHeader ();
}

void PlotWidget::setPlaceholder (const QString &text)
{
    if (text == placeholder_)
        return;
    placeholder_ = text;
    update (recessRect_);
}

void PlotWidget::setStallText (const QString &text)
{
    if (text == stallText_)
        return;
    stallText_ = text;
    update (recessRect_);
}

void PlotWidget::setNote (const QString &text, bool flagged)
{
    if (text == note_ && flagged == noteFlag_)
        return;
    note_ = text;
    noteFlag_ = flagged;
    setToolTip (text);
    updateHeader ();
}

void PlotWidget::setRailMode (bool on)
{
    if (on == railMode_)
        return;
    railMode_ = on;
    dirty_ = true;
    lanes_.front ().yValid = false; // the autoscale restarts from the displayed data afterwards
    cacheValid_ = false;
    refreshHeaderText ();
    rebuild ();
    update ();
}

void PlotWidget::setSymmetric (bool on)
{
    if (on == symmetric_)
        return;
    symmetric_ = on;
    dirty_ = true;
}

void PlotWidget::setOverlay (QWidget *w)
{
    overlay_ = w;
    if (!w)
        return;
    w->setParent (this);
    w->setGeometry (plotRect_);
    w->raise ();
}

void PlotWidget::setSource (
    int lane, std::shared_ptr<SignalRing> ring, int rawChannel, double nominalRate, int valueDecimals)
{
    if (lane < 0 || lane >= laneCount ())
        return;
    Lane &L = lanes_[static_cast<std::size_t> (lane)];
    L.ring = std::move (ring);
    L.rawChannel = rawChannel;
    L.nominalRate = nominalRate;
    L.decimals = valueDecimals;
    L.lastWritten = UINT64_MAX;
    L.n = 0;
    L.finite = false;
    L.haveLatest = false;
    L.yValid = false;
    std::fill (L.segCount.begin (), L.segCount.end (), 0);
    std::fill (L.denseMode.begin (), L.denseMode.end (), 0);
    dirty_ = true;
    pp_ = std::numeric_limits<double>::quiet_NaN ();
    refreshHeaderText ();
    update ();
}

void PlotWidget::setWindowSeconds (double seconds)
{
    windowSec_ = std::max (0.5, seconds);
    for (Lane &L : lanes_)
        L.yValid = false;
    dirty_ = true;
}

void PlotWidget::setRemoveMean (bool on)
{
    if (removeMean_ == on)
        return;
    removeMean_ = on;
    for (Lane &L : lanes_)
        L.yValid = false;
    dirty_ = true;
}

void PlotWidget::setPaused (bool on)
{
    paused_ = on;
    dirty_ = true;
    update (recessRect_);
}

bool PlotWidget::hasSamples () const
{
    for (const Lane &L : lanes_)
        if (L.n > 0)
            return true;
    return false;
}

// ------------------------------------------------------------------ data
void PlotWidget::tick (double now)
{
    bool anyRing = false;
    for (const Lane &L : lanes_)
        anyRing = anyRing || L.ring;
    if (!anyRing)
    {
        if (hasSamples () || dirty_)
        {
            for (Lane &L : lanes_)
            {
                L.n = 0;
                L.finite = false;
                L.haveLatest = false;
                std::fill (L.segCount.begin (), L.segCount.end (), 0);
            }
            dirty_ = false;
            pp_ = std::numeric_limits<double>::quiet_NaN ();
            if (refreshHeaderText ())
                updateHeader ();
            update (recessRect_);
        }
        return;
    }

    if (!paused_)
    {
        bool newData = false;
        double latestTs = std::numeric_limits<double>::quiet_NaN ();
        for (const Lane &L : lanes_)
        {
            if (!L.ring)
                continue;
            newData = newData || L.ring->totalWritten () != L.lastWritten;
            const double ts = L.ring->latestTimestamp ();
            if (std::isfinite (ts) && !(ts <= latestTs))
                latestTs = ts;
        }
        if (newData)
            lastChangeWall_ = now;

        // Right edge = host "now" (BrainFlow timestamps use the host clock), so
        // a stalled stream visibly scrolls away. Safety net: if data is
        // arriving but its timestamps are far from the host clock, anchor to
        // the newest sample instead of showing an empty plot.
        double ref = now;
        clockMismatch_ = std::isfinite (latestTs) && (now - lastChangeWall_) < 2.0 && std::fabs (latestTs - now) > 30.0;
        if (clockMismatch_)
            ref = latestTs;

        const double pxPerSec = std::max (1.0, static_cast<double> (plotRect_.width ())) / windowSec_;
        const bool moved = std::fabs (ref - refTime_) * pxPerSec >= 1.0;
        if (!dirty_ && !moved)
        {
            // New samples, but the view has not scrolled a whole pixel yet:
            // repaint for them at most ~30 times per second.
            if (!newData || now - lastRebuildWall_ < 1.0 / 30.0)
                return;
        }
        refTime_ = ref;

        std::size_t prevTotal = 0, total = 0;
        for (Lane &L : lanes_)
        {
            prevTotal += L.n;
            if (!L.ring)
            {
                L.n = 0;
                L.haveLatest = false;
                continue;
            }
            L.lastWritten = L.ring->totalWritten (); // consumed only when we actually rebuild
            const double tmin = ref - windowSec_ * 1.02 - 1.0 / std::max (1.0, L.nominalRate);
            L.n = L.ring->copySince (tmin, L.t, L.v);
            L.haveLatest = L.ring->latest (L.latestTs, L.latestVals);
            total += L.n;
        }
        if (total == 0 && prevTotal == 0 && !dirty_ && !newData)
            return;
    }
    else if (!dirty_)
    {
        return;
    }

    lastRebuildWall_ = now;
    rebuild ();
    update (recessRect_);
    if (now - lastHeaderWall_ >= 0.125)
    {
        lastHeaderWall_ = now;
        if (refreshHeaderText ())
            updateHeader ();
    }
}

void PlotWidget::updateYRange (Lane &L, double lo, double hi)
{
    // Smallest span worth resolving: one unit of the displayed precision
    // (or the lane's minimum span).
    const double floorSpan = std::max (std::pow (10.0, -std::clamp (L.decimals, 0, 6)), L.spec.minSpan);
    double span = hi - lo;
    if (!(span > 0.0))
    {
        const double mag = std::max (std::fabs (hi), std::fabs (lo));
        span = std::max ({mag * 1e-3, floorSpan, 1e-6});
        const double mid = 0.5 * (lo + hi);
        lo = mid - span / 2.0;
        hi = mid + span / 2.0;
    }
    else if (span < floorSpan)
    {
        const double mid = 0.5 * (lo + hi);
        span = floorSpan;
        lo = mid - span / 2.0;
        hi = mid + span / 2.0;
    }
    const double pad = span * 0.08;
    const double tLo = lo - pad;
    const double tHi = hi + pad;

    const double now = steadySeconds ();
    const double dt = std::clamp (now - L.lastYUpdate, 0.0, 0.25);
    L.lastYUpdate = now;

    if (!L.yValid)
    {
        L.yLo = tLo;
        L.yHi = tHi;
        L.yValid = true;
        L.shrinking = false;
        return;
    }
    if (tLo < L.yLo)
        L.yLo = tLo;
    if (tHi > L.yHi)
        L.yHi = tHi;
    const double cur = L.yHi - L.yLo;
    const double tgt = tHi - tLo;
    if (!L.shrinking && cur > 1.6 * tgt)
        L.shrinking = true;
    if (L.shrinking)
    {
        const double k = 1.0 - std::exp (-dt / 0.6);
        L.yLo += (tLo - L.yLo) * k;
        L.yHi += (tHi - L.yHi) * k;
        if (L.yHi - L.yLo <= 1.05 * tgt)
            L.shrinking = false;
    }
}

bool PlotWidget::rawView () const
{
    return railMode_ && kind_ == Kind::Hero && lanes_.front ().rawChannel >= 0;
}

void PlotWidget::computeDisplayRange (Lane &L)
{
    if (railMode_ && kind_ == Kind::Hero)
    {
        // Near-rail view: the raw value on a fixed +-full-scale range. The
        // rails sit near the recess top / bottom as in the design's artboard
        // (22 / 300 and 278 / 300 of the recess), 0 in the middle.
        L.dLo = -fullScale_;
        L.dHi = fullScale_;
        L.dStep = fullScale_;
        const double h = plotRect_.height ();
        L.yTop = plotRect_.top () + h * 22.0 / 300.0;
        L.yBot = plotRect_.top () + h * 278.0 / 300.0;
        return;
    }
    L.yTop = L.mapTop;
    L.yBot = L.mapBottom;
    double lo = L.yLo, hi = L.yHi;
    if (symmetric_ && kind_ == Kind::Hero)
    {
        double m = std::max (std::fabs (lo), std::fabs (hi));
        if (!(m > 0.0))
            m = 1.0;
        lo = -m;
        hi = m;
    }
    // Snap outwards to a nice step: stable, round axis labels; the range only
    // changes when the autoscale crosses a step.
    const double step = niceStep ((hi - lo) / 4.0);
    double sl = std::floor (lo / step + 1e-9) * step;
    double sh = std::ceil (hi / step - 1e-9) * step;
    if (!(sh > sl))
        sh = sl + step;
    L.dLo = sl;
    L.dHi = sh;
    L.dStep = step;
}

void PlotWidget::rebuild ()
{
    dirty_ = false;
    pp_ = std::numeric_limits<double>::quiet_NaN ();
    lastPoints_ = 0;
    for (std::size_t i = 0; i < lanes_.size (); ++i)
        rebuildLane (lanes_[i], i == 0);
}

void PlotWidget::rebuildLane (Lane &L, bool first)
{
    const int nTr = std::min (static_cast<int> (L.spec.traces.size ()), static_cast<int> (L.v.size ()));
    double lo = std::numeric_limits<double>::infinity ();
    double hi = -std::numeric_limits<double>::infinity ();
    std::fill (L.means.begin (), L.means.end (), 0.0);
    // Near-rail view plots the raw channel (no mean removal) as trace 0.
    const bool raw = first && rawView () && L.rawChannel < static_cast<int> (L.v.size ());
    auto series = [&] (int tr) -> const std::vector<double> & {
        return L.v[static_cast<std::size_t> (raw && tr == 0 ? L.rawChannel : tr)];
    };

    if (L.n > 0)
    {
        const double tWin = refTime_ - windowSec_;
        for (int tr = 0; tr < nTr; ++tr)
        {
            const std::vector<double> &vv = series (tr);
            double off = 0.0;
            if (removeMean_ && kind_ == Kind::Hero && !(raw && tr == 0))
            {
                double sum = 0.0;
                std::size_t cnt = 0;
                for (std::size_t k = 0; k < L.n; ++k)
                    if (std::isfinite (vv[k]))
                    {
                        sum += vv[k];
                        ++cnt;
                    }
                off = cnt ? sum / static_cast<double> (cnt) : 0.0;
            }
            L.means[static_cast<std::size_t> (tr)] = off;
            double wLo = std::numeric_limits<double>::infinity ();
            double wHi = -std::numeric_limits<double>::infinity ();
            for (std::size_t k = 0; k < L.n; ++k)
            {
                const double x = vv[k] - off;
                if (!std::isfinite (x))
                    continue;
                lo = std::min (lo, x);
                hi = std::max (hi, x);
                if (first && tr == 0 && L.t[k] >= tWin)
                {
                    wLo = std::min (wLo, x);
                    wHi = std::max (wHi, x);
                }
            }
            if (first && tr == 0 && std::isfinite (wLo))
                pp_ = wHi - wLo;
        }
    }
    L.finite = std::isfinite (lo) && std::isfinite (hi);
    if (raw)
    {
        // fixed range (computeDisplayRange); the autoscale state is left alone
    }
    else if (std::isfinite (lo) && std::isfinite (hi))
        updateYRange (L, lo, hi);
    else if (!L.yValid)
    {
        L.yLo = -1.0;
        L.yHi = 1.0;
    }
    computeDisplayRange (L);

    PixelMap m;
    m.tStart = refTime_ - windowSec_;
    m.tSpan = windowSec_;
    m.left = plotRect_.left ();
    m.width = std::max (1, plotRect_.width ());
    m.vLo = L.dLo;
    m.vSpan = std::max (1e-12, L.dHi - L.dLo);
    m.top = L.yTop;
    m.height = std::max (1.0, L.yBot - L.yTop);
    const double gapSec = std::max (1.0, 10.0 / std::max (0.1, L.nominalRate));

    for (int tr = 0; tr < static_cast<int> (L.segCount.size ()); ++tr)
    {
        const std::size_t ti = static_cast<std::size_t> (tr);
        if (tr >= nTr || L.n == 0)
        {
            L.segCount[ti] = 0;
            L.denseFlags[ti].clear ();
            continue;
        }
        std::size_t pts = 0;
        bool dense = L.denseMode[ti] != 0;
        L.segCount[ti] = buildPolylines (L.t.data (), series (tr).data (), L.n, L.means[ti], m, gapSec, L.polys[ti],
            &pts, &L.denseFlags[ti], &dense);
        L.denseMode[ti] = dense ? 1 : 0;
        lastPoints_ += pts;
    }
}

bool PlotWidget::refreshHeaderText ()
{
    const bool raw = rawView ();
    QStringList vals;
    const bool withPlus = kind_ == Kind::Hero || kind_ == Kind::Lanes;
    for (std::size_t li = 0; li < lanes_.size (); ++li)
    {
        const Lane &L = lanes_[li];
        const bool have = tone_ != Tone::Off && L.haveLatest;
        for (int i = 0; i < L.spec.traces.size (); ++i)
        {
            if (!have || i >= static_cast<int> (L.latestVals.size ()))
            {
                vals << kNoValue;
                continue;
            }
            double val = L.latestVals[static_cast<std::size_t> (i)];
            if (kind_ == Kind::Hero && li == 0)
            {
                if (raw && L.rawChannel < static_cast<int> (L.latestVals.size ()))
                    val = L.latestVals[static_cast<std::size_t> (L.rawChannel)];
                else if (removeMean_ && i < static_cast<int> (L.means.size ()))
                    val -= L.means[static_cast<std::size_t> (i)];
            }
            vals << Readouts::number (val, L.decimals, withPlus, true);
        }
    }
    if (kind_ == Kind::Vital && !vals.isEmpty ())
        vals[0] = (tone_ == Tone::Off || valueOverride_.isEmpty ()) ? kNoValue : valueOverride_;
    QString pp = QStringLiteral ("— ") + units ();
    if (tone_ != Tone::Off && std::isfinite (pp_))
        pp = Readouts::number (pp_, lanes_.front ().decimals, false, true) + QStringLiteral (" ") + units ();
    const bool changed = vals != valueText_ || pp != ppText_ || raw != latestIsRaw_;
    valueText_ = vals;
    ppText_ = pp;
    latestIsRaw_ = raw;
    return changed;
}

void PlotWidget::updateHeader ()
{
    QRegion r (headerRect_);
    if (!stripRect_.isNull ())
        r += stripRect_;
    update (r);
}

// ------------------------------------------------------------------ geometry
void PlotWidget::layoutRects ()
{
    const int w = width (), h = height ();
    int headerH = 6 + 15 + 6;
    if (kind_ == Kind::Hero)
        headerH = 8 + 33 + 8;
    else if (kind_ == Kind::Scalar)
        headerH = 6 + 19 + 6;
    else if (kind_ == Kind::Vital)
        headerH = 6 + 25 + 6;
    headerRect_ = QRect (1, 1, std::max (0, w - 2), headerH);
    int y = 1 + headerH + 1; // divider under the header
    if (hasStrip ())
    {
        stripRect_ = QRect (1, y, std::max (0, w - 2), 5 + 16 + 5);
        y += stripRect_.height () + 1;
    }
    else
        stripRect_ = QRect ();
    const int m = kind_ == Kind::Hero ? 8 : 6;
    recessRect_ = QRect (1 + m, y + m, std::max (12, w - 2 - 2 * m), std::max (12, h - 1 - m - (y + m)));
    plotRect_ = recessRect_.adjusted (1, 1, -1, -1);

    // Lanes share the plot's width and split its height, 1 px divider between.
    const int nL = laneCount ();
    const int avail = std::max (nL, plotRect_.height () - (nL - 1));
    int top = plotRect_.top ();
    for (int i = 0; i < nL; ++i)
    {
        Lane &L = lanes_[static_cast<std::size_t> (i)];
        const int bh = avail / nL + (i < avail % nL ? 1 : 0);
        L.band = QRect (plotRect_.left (), top, plotRect_.width (), bh);
        top += bh + 1;
        if (kind_ == Kind::Hero)
        {
            L.mapTop = plotRect_.top () + 11.0;
            L.mapBottom = plotRect_.top () + plotRect_.height () - 24.0;
        }
        else
        {
            const double pad = kind_ == Kind::Lanes && bh < 48 ? 5.0 : 8.0;
            L.mapTop = L.band.top () + pad;
            L.mapBottom = L.band.top () + L.band.height () - pad;
        }
        if (L.mapBottom - L.mapTop < 8.0)
        {
            L.mapTop = L.band.top ();
            L.mapBottom = L.band.top () + L.band.height ();
        }
        L.yTop = L.mapTop;
        L.yBot = L.mapBottom;
    }
    if (overlay_)
        overlay_->setGeometry (plotRect_);
    cacheValid_ = false;
}

double PlotWidget::mapY (const Lane &L, double v) const
{
    return L.yBot - (v - L.dLo) / std::max (1e-12, L.dHi - L.dLo) * (L.yBot - L.yTop);
}

QString PlotWidget::axisLabel (const Lane &L, double v, bool withUnits) const
{
    // explicit '+' only on axes that also show negative values
    QString s = Readouts::number (v, decimalsForStep (L.dStep), L.dLo < 0.0, true);
    if (withUnits && !L.spec.units.isEmpty ())
        s += QStringLiteral (" ") + L.spec.units;
    return s;
}

void PlotWidget::resizeEvent (QResizeEvent *event)
{
    QWidget::resizeEvent (event);
    layoutRects ();
    rebuild ();
}

// ------------------------------------------------------------------ painting
void PlotWidget::paintEvent (QPaintEvent *event)
{
    QPainter p (this);
    const QRect br = event->rect ();
    if (!recessRect_.contains (br))
        paintChrome (p);
    if (br.intersects (recessRect_))
        paintRecess (p);
}

void PlotWidget::paintChrome (QPainter &p)
{
    p.fillRect (rect (), Theme::panel);
    outline (p, rect (), Theme::edge);
    p.fillRect (QRect (1, headerRect_.bottom () + 1, width () - 2, 1), Theme::divider);
    if (hasStrip ())
        p.fillRect (QRect (1, stripRect_.bottom () + 1, width () - 2, 1), Theme::divider);
    p.setRenderHint (QPainter::TextAntialiasing);
    if (kind_ == Kind::Hero)
        paintHeroHeader (p);
    else
        paintPanelHeader (p);
    if (kind_ == Kind::Lanes)
        paintLaneStrip (p);
    else if (kind_ == Kind::Vital)
        paintChipStrip (p);
}

void PlotWidget::paintHeroHeader (QPainter &p)
{
    const QRect hr = headerRect_;
    const double cy = hr.top () + hr.height () / 2.0;
    const QFontMetricsF fk (fKicker_), fv (fValue_), fu (fValueUnit_), fr (fRate_);
    const QColor valC = tone_ == Tone::Off ? Theme::textDim : (tone_ == Tone::Warn ? Theme::warn : Theme::textStrong);

    // Readout columns share the kicker baseline (CSS align-items: baseline).
    const double kickH = 12.0, valH = 21.0;
    const double top = cy - (kickH + valH) / 2.0;
    const double kickBase = baselineFor (fk, top + kickH / 2.0);
    const double smallBase = baselineFor (fr, top + kickH + 16.0 / 2.0);
    const double bigBase = baselineFor (fv, top + kickH + valH / 2.0);
    double right = hr.left () + hr.width () - 10.0;

    auto column = [&] (const QString &kick, double minW, auto drawValue, double valueW) {
        const double w = std::max ({minW, fk.horizontalAdvance (kick), valueW});
        p.setFont (fKicker_);
        p.setPen (Theme::textDim);
        p.drawText (QPointF (right - fk.horizontalAdvance (kick), kickBase), kick);
        drawValue (right);
        right -= w + 14.0;
    };

    // LATEST (or LATEST RAW when near the rail): 19 / 600 + unit 11 / 400
    const QString latest = valueText_.value (0, kNoValue);
    const double uw = fu.horizontalAdvance (units ());
    const double vw = fv.horizontalAdvance (latest);
    column (latestIsRaw_ ? QStringLiteral ("LATEST RAW") : QStringLiteral ("LATEST"), 104.0,
        [&] (double r) {
            p.setFont (fValueUnit_);
            p.setPen (Theme::textMuted);
            p.drawText (QPointF (r - uw, bigBase), units ());
            p.setFont (fValue_);
            p.setPen (valC);
            p.drawText (QPointF (r - uw - 4.0 - vw, bigBase), latest);
        },
        vw + 4.0 + uw);
    // RATE
    const double rw = fr.horizontalAdvance (rateText_);
    column (QStringLiteral ("RATE"), 0.0,
        [&] (double r) {
            p.setFont (fRate_);
            p.setPen (rateColor_);
            p.drawText (QPointF (r - rw, smallBase), rateText_);
        },
        rw);
    // P-P
    const double pw = fr.horizontalAdvance (ppText_);
    column (QStringLiteral ("P-P"), 0.0,
        [&] (double r) {
            p.setFont (fRate_);
            p.setPen (tone_ == Tone::Off ? Theme::textDim : Theme::textBody);
            p.drawText (QPointF (r - pw, smallBase), ppText_);
        },
        pw);
    const double limit = right + 14.0 - 12.0;

    // Left: LED, title, subtitle, filter chips (subtitle, then chips, give way)
    const QFontMetricsF ft (fTitle_), fs (fSub_), fc (fChip_);
    double x = hr.left () + 10.0;
    const double titleW = ft.horizontalAdvance (title_);
    const double subW = subtitle_.isEmpty () ? 0.0 : fs.horizontalAdvance (subtitle_);
    QVector<Chip> chips = chips_;
    if (noteFlag_)
        chips.prepend ({QStringLiteral ("SUBST"), false}); // source substituted: see tooltip
    std::vector<double> chipW;
    for (const Chip &c : chips)
        chipW.push_back (std::ceil (fc.horizontalAdvance (c.text) + 10.0 + 2.0));
    const double base = 8.0 + 12.0 + titleW;
    bool showSub = subW > 0.0;
    int nChips = static_cast<int> (chips.size ());
    auto need = [&] () {
        double n = base + (showSub ? 12.0 + subW : 0.0);
        double cw = 0.0;
        for (int i = 0; i < nChips; ++i)
            cw += chipW[static_cast<std::size_t> (i)] + (i ? 6.0 : 0.0);
        return n + (nChips ? 12.0 + cw : 0.0);
    };
    if (x + need () > limit)
        showSub = false;
    while (nChips > 0 && x + need () > limit)
        --nChips;

    p.fillRect (QRectF (x, std::floor (cy - 4.0), 8, 8), led_);
    x += 8.0 + 12.0;
    p.setFont (fTitle_);
    p.setPen (Theme::textStrong);
    const QString title = ft.elidedText (title_, Qt::ElideRight, std::max (20.0, limit - x));
    p.drawText (QPointF (x, baselineFor (ft, cy)), title);
    x += ft.horizontalAdvance (title);
    if (showSub)
    {
        x += 12.0;
        p.setFont (fSub_);
        p.setPen (Theme::textDim);
        p.drawText (QPointF (x, baselineFor (fs, cy)), subtitle_);
        x += subW;
    }
    if (nChips > 0)
        x += 12.0;
    p.setFont (fChip_);
    const int chipH = 15;
    for (int i = 0; i < nChips; ++i)
    {
        const Chip &c = chips[i];
        const int w = static_cast<int> (chipW[static_cast<std::size_t> (i)]);
        const QRect r (static_cast<int> (std::round (x)), static_cast<int> (std::round (cy - chipH / 2.0)), w, chipH);
        outline (p, r, c.warn ? Theme::warn : (c.on ? Theme::controlEdge : Theme::divider));
        p.setPen (c.warn ? Theme::warn : (c.on ? Theme::textMuted : Theme::textFainter));
        p.drawText (QPointF (r.left () + 6.0, baselineFor (fc, r.top () + chipH / 2.0)), c.text);
        x += w + 6.0;
    }
}

void PlotWidget::paintPanelHeader (QPainter &p)
{
    const QRect hr = headerRect_;
    const double cy = hr.top () + hr.height () / 2.0;
    const bool valued = kind_ == Kind::Scalar || kind_ == Kind::Vital;
    const double gap = valued ? 9.0 : 8.0;
    const QFontMetricsF ft (fTitle_), fu (fUnits_), fr (fRate_), fv (fValue_), fvu (fValueUnit_), fc (fChip_);
    const QColor valC = tone_ == Tone::Off ? Theme::textDim : (tone_ == Tone::Warn ? Theme::warn : Theme::textStrong);
    const QString v = valueText_.value (0, kNoValue);
    const double vw = fv.horizontalAdvance (v);

    // Widths first, then what fits: the title and the value always show; a
    // narrow panel drops the units, then the rate, then elides the title.
    const double left = hr.left () + 9.0 + 7.0 + gap;
    double right = hr.left () + hr.width () - 9.0;
    double valueW = 0.0;
    if (kind_ == Kind::Scalar)
        valueW = std::max (62.0, vw) + 9.0;
    else if (kind_ == Kind::Vital)
        valueW = std::max (58.0, vw + 4.0 + fvu.horizontalAdvance (units ())) + 12.0;
    const QString u = kind_ == Kind::Scalar ? units () : QString (); // Lanes: in the strip; Vital: by the value
    const QString tag = QStringLiteral ("SUBST");
    const double titleW = ft.horizontalAdvance (title_);
    const double rateW = rateText_.isEmpty () ? 0.0 : fr.horizontalAdvance (rateText_) + gap;
    const double unitsW = u.isEmpty () ? 0.0 : fu.horizontalAdvance (u) + gap;
    const double tagW = noteFlag_ ? fc.horizontalAdvance (tag) + gap : 0.0;
    double room = right - valueW - left;
    if (kind_ == Kind::Scalar && titleW + unitsW + tagW + rateW > room)
    {
        valueW = vw + 9.0; // the value's 62 px minimum gives way first
        room = right - valueW - left;
    }
    const bool showUnits = titleW + unitsW + tagW + rateW <= room;
    const bool showTag = noteFlag_ && titleW + (showUnits ? unitsW : 0.0) + tagW + rateW <= room;
    const bool showRate = rateW > 0.0 && titleW + rateW <= room;

    if (kind_ == Kind::Scalar)
    {
        p.setFont (fValue_);
        p.setPen (valC);
        p.drawText (QPointF (right - vw, baselineFor (fv, cy)), v);
    }
    else if (kind_ == Kind::Vital)
    {
        // large readout: 19 / 600 value + 11 / 400 unit, as the hero's LATEST
        const double uw = fvu.horizontalAdvance (units ());
        const double base = baselineFor (fv, cy);
        p.setFont (fValueUnit_);
        p.setPen (Theme::textMuted);
        p.drawText (QPointF (right - uw, base), units ());
        p.setFont (fValue_);
        p.setPen (valC);
        p.drawText (QPointF (right - uw - 4.0 - vw, base), v);
    }
    right -= valueW;
    if (showRate)
    {
        p.setFont (fRate_);
        p.setPen (rateColor_);
        p.drawText (QPointF (right - fr.horizontalAdvance (rateText_), baselineFor (fr, cy)), rateText_);
        right -= rateW;
    }

    p.fillRect (QRectF (hr.left () + 9.0, std::floor (cy - 3.5), 7, 7), led_);
    double x = left;
    p.setFont (fTitle_);
    p.setPen (Theme::textStrong);
    const QString title = ft.elidedText (title_, Qt::ElideRight, std::max (16.0, right - x));
    p.drawText (QPointF (x, baselineFor (ft, cy)), title);
    x += ft.horizontalAdvance (title) + gap;
    if (showUnits && !u.isEmpty ())
    {
        p.setFont (fUnits_);
        p.setPen (Theme::textDim);
        p.drawText (QPointF (x, baselineFor (fu, cy)), u);
        x += fu.horizontalAdvance (u) + gap;
    }
    // source substituted through a fallback (details in the tooltip)
    if (showTag)
    {
        p.setFont (fChip_);
        p.setPen (Theme::textFaint);
        p.drawText (QPointF (x, baselineFor (fc, cy)), tag);
    }
}

void PlotWidget::paintLaneStrip (QPainter &p)
{
    // [ACC g  -- X +0.012  - - Y -0.981  .. Z +0.105 | GYR °/s ... | MAG µT ...]
    const QRect sr = stripRect_;
    const double cy = sr.top () + sr.height () / 2.0;
    const double left = sr.left () + 9.0;
    const double inner = sr.width () - 18.0;
    const int nL = laneCount ();
    const double sepW = 21.0;
    const double groupW = (inner - sepW * (nL - 1)) / std::max (1, nL);
    const QFontMetricsF fk (fKicker_), fl (fLetter_), fv (fLegendValue_);
    const QColor valC = tone_ == Tone::Off ? Theme::textDim : (tone_ == Tone::Warn ? Theme::warn : Theme::textStrong);

    struct Column
    {
        double x, w;
    };
    std::vector<QString> kicks;
    std::vector<Column> cols;
    // One format for the whole strip: glyph + letter + value when every
    // column has room, else glyph + value, else the values alone.
    int mode = 2;
    int flat = 0;
    for (int li = 0; li < nL; ++li)
    {
        const Lane &L = lanes_[static_cast<std::size_t> (li)];
        QString kick = L.spec.label;
        if (!L.spec.units.isEmpty ())
            kick += QStringLiteral (" ") + L.spec.units;
        kicks.push_back (kick);
        const double x0 = left + li * (groupW + sepW);
        const int nT = L.spec.traces.size ();
        const double cx0 = x0 + fk.horizontalAdvance (kick) + 10.0;
        const double colW = (x0 + groupW - cx0 - 8.0 * (nT - 1)) / std::max (1, nT);
        for (int i = 0; i < nT; ++i, ++flat)
        {
            cols.push_back ({cx0 + i * (colW + 8.0), colW});
            const double vw = fv.horizontalAdvance (valueText_.value (flat, kNoValue));
            if (colW < 14.0 + 4.0 + fl.horizontalAdvance (L.spec.traces[i].name) + 6.0 + vw)
                mode = std::min (mode, 1);
            if (colW < 14.0 + 6.0 + vw)
                mode = 0;
        }
    }

    flat = 0;
    for (int li = 0; li < nL; ++li)
    {
        const Lane &L = lanes_[static_cast<std::size_t> (li)];
        const double x0 = left + li * (groupW + sepW);
        if (li > 0)
            p.fillRect (QRectF (std::round (x0 - 11.0), std::floor (cy - 7.0), 1, 14), Theme::edge);
        p.setFont (fKicker_);
        p.setPen (Theme::textDim);
        p.drawText (QPointF (x0, baselineFor (fk, cy)), kicks[static_cast<std::size_t> (li)]);
        for (int i = 0; i < L.spec.traces.size (); ++i, ++flat)
        {
            const Trace &T = L.spec.traces[i];
            const Column &c = cols[static_cast<std::size_t> (flat)];
            if (mode >= 1)
                legendGlyph (p, T, c.x, cy);
            if (mode == 2)
            {
                p.setFont (fLetter_);
                p.setPen (Theme::textMuted);
                p.drawText (QPointF (c.x + 14.0 + 4.0, baselineFor (fl, cy)), T.name);
            }
            const QString v = valueText_.value (flat, kNoValue);
            p.setFont (fLegendValue_);
            p.setPen (valC);
            p.drawText (QPointF (c.x + c.w - fv.horizontalAdvance (v), baselineFor (fv, cy)), v);
        }
    }
}

void PlotWidget::paintChipStrip (QPainter &p)
{
    const QRect sr = stripRect_;
    const double cy = sr.top () + sr.height () / 2.0;
    const QFontMetricsF fc (fChip_);
    const int chipH = 15;
    double x = sr.left () + 9.0;
    const double right = sr.left () + sr.width () - 9.0;
    p.setFont (fChip_);
    for (const Chip &c : stripChips_)
    {
        const int w = static_cast<int> (std::ceil (fc.horizontalAdvance (c.text) + 12.0));
        if (x + w > right)
            break;
        const QRect r (static_cast<int> (std::round (x)), static_cast<int> (std::round (cy - chipH / 2.0)), w, chipH);
        outline (p, r, c.warn ? Theme::warn : (c.on ? Theme::controlEdge : Theme::divider));
        p.setPen (c.warn ? Theme::warn : (c.on ? Theme::textMuted : Theme::textFainter));
        p.drawText (QPointF (r.left () + 6.0, baselineFor (fc, r.top () + chipH / 2.0)), c.text);
        x += w + 6.0;
    }
}

void PlotWidget::ensureDotRaster (const QSize &sz, qreal dpr)
{
    if (!dots_.isNull () && sz == dotsSize_ && dpr == dotsDpr_)
        return;
    dotsSize_ = sz;
    dotsDpr_ = dpr;
    dots_ = QPixmap (sz * dpr);
    dots_.setDevicePixelRatio (dpr);
    dots_.fill (Theme::recess);
    QPainter p (&dots_);
    outline (p, QRect (QPoint (0, 0), sz), Theme::divider);

    // The design tiles radial-gradient(rgba(255,255,255,a) .8px, transparent
    // .9px) every 12 / 10 px. A browser samples it at pixel centres, so each
    // dot is a crisp 2 x 2 device-pixel block in the fully composited colour
    // (a = .16 hero / .14 panels over the recess), centred in its tile.
    const bool hero = kind_ == Kind::Hero;
    const int pitch = hero ? 12 : 10;
    const double a = hero ? 0.16 : 0.14;
    auto over = [a] (int c) { return static_cast<int> (std::lround (c + a * (255 - c))); };
    const QColor dot (over (Theme::recess.red ()), over (Theme::recess.green ()), over (Theme::recess.blue ()));
    const double d = 2.0 / dpr; // 2 device px
    for (int y = 1 + pitch / 2; y < sz.height () - 1; y += pitch)
        for (int x = 1 + pitch / 2; x < sz.width () - 1; x += pitch)
            p.fillRect (QRectF (x - d / 2.0, y - d / 2.0, d, d), dot);
}

void PlotWidget::ensureRecessCache ()
{
    const qreal dpr = devicePixelRatioF ();
    const QSize sz = recessRect_.size ();
    const bool hero = kind_ == Kind::Hero;
    const bool railOn = railMode_ && hero;
    bool labels = false;
    for (const Lane &L : lanes_)
        labels = labels || L.finite;
    std::vector<double> ranges;
    ranges.reserve (lanes_.size () * 3);
    for (const Lane &L : lanes_)
    {
        ranges.push_back (L.dLo);
        ranges.push_back (L.dHi);
        ranges.push_back (L.finite ? 1.0 : 0.0);
    }
    if (cacheValid_ && sz == cacheSize_ && dpr == cacheDpr_ && ranges == cacheRanges_ && windowSec_ == cacheWindow_ &&
        railOn == cacheRail_ && labels == cacheLabels_)
        return;

    // Layer 1, the dot raster, is rasterised only when the size / DPR change;
    // a range / rail / label change costs one blit of it plus a few lines and
    // labels drawn on top.
    ensureDotRaster (sz, dpr);
    if (sz != cacheSize_ || dpr != cacheDpr_ || cache_.isNull ())
    {
        cache_ = QPixmap (sz * dpr);
        cache_.setDevicePixelRatio (dpr);
    }
    cacheValid_ = true;
    cacheSize_ = sz;
    cacheDpr_ = dpr;
    cacheRanges_ = ranges;
    cacheWindow_ = windowSec_;
    cacheRail_ = railOn;
    cacheLabels_ = labels;

    QPainter p (&cache_);
    p.setCompositionMode (QPainter::CompositionMode_Source);
    p.drawPixmap (0, 0, dots_);
    p.setCompositionMode (QPainter::CompositionMode_SourceOver);
    const QPoint o = recessRect_.topLeft ();
    p.setRenderHint (QPainter::TextAntialiasing);

    const double pl = plotRect_.left () - o.x (), pr = plotRect_.left () + plotRect_.width () - o.x ();
    const double ptop = plotRect_.top () - o.y (), pbot = plotRect_.top () + plotRect_.height () - o.y ();
    const QFontMetricsF fa (fAxis_);
    p.setFont (fAxis_);

    if (hero)
    {
        const Lane &L = lanes_.front ();
        // zero line
        if (0.0 >= L.dLo && 0.0 <= L.dHi)
        {
            const double y = std::round (mapY (L, 0.0)) - o.y ();
            p.fillRect (QRectF (pl, y, pr - pl, 1.0), Theme::zeroLine);
        }
        // +-FS rails (dashed 6 5) in the near-rail view, in raw coordinates;
        // the top / bottom labels below then read +-187,500 uV
        if (railOn)
        {
            QPen rp = cosmeticPen (Theme::fault);
            rp.setDashPattern ({6.0 * dpr, 5.0 * dpr});
            p.setPen (rp);
            for (double v : {fullScale_, -fullScale_})
            {
                const double y = std::round (mapY (L, v)) - o.y () + 0.5;
                if (y >= ptop && y <= pbot)
                    p.drawLine (QPointF (pl, y), QPointF (pr, y));
            }
        }
        // Y labels: top / middle / bottom of the range (design positions)
        p.setPen (Theme::textDim);
        const double mid = (0.0 >= L.dLo && 0.0 <= L.dHi) ? 0.0 : 0.5 * (L.dLo + L.dHi);
        const double lx = pl + 6.0;
        if (labels)
        {
            p.drawText (QPointF (lx, baselineFor (fa, L.mapTop - o.y ())), axisLabel (L, L.dHi, true));
            p.drawText (QPointF (lx, baselineFor (fa, mapY (L, mid) - o.y ()) - (mid == 0.0 ? 7.0 : 0.0)),
                axisLabel (L, mid, true));
            p.drawText (QPointF (lx, baselineFor (fa, L.mapBottom - o.y ())), axisLabel (L, L.dLo, true));
        }
        // full scale, top right
        const QString fs = QStringLiteral ("FS ±") + Readouts::number (fullScale_, 0, false, true) +
            QStringLiteral (" ") + units ();
        p.setPen (Theme::textFainter);
        p.drawText (QPointF (pr - 7.0 - fa.horizontalAdvance (fs), ptop + 5.0 + fa.ascent ()), fs);
        // time ticks along the bottom
        const double xs = niceStep (windowSec_ / 4.0);
        const int xd = decimalsForStep (xs);
        const double base = pbot - 4.0 - fa.descent ();
        p.setPen (Theme::textDim);
        for (int k = 0; k <= 60; ++k)
        {
            const double back = k * xs;
            if (back > windowSec_ + 1e-9)
                break;
            const double xx = pr - back / windowSec_ * (pr - pl);
            const QString s = k == 0 ? QStringLiteral ("0 s") : Readouts::number (-back, xd, false, false);
            const double w = fa.horizontalAdvance (s);
            const double lxx = std::clamp (xx - w / 2.0, pl + 8.0, pr - 8.0 - w);
            p.drawText (QPointF (lxx, base), s);
        }
        return;
    }

    // Panels: faint range labels at the left (top / bottom of each lane's
    // range); lanes are separated by a 1 px divider and carry their label.
    const double lx = pl + 5.0;
    for (std::size_t li = 0; li < lanes_.size (); ++li)
    {
        const Lane &L = lanes_[li];
        if (li > 0)
            p.fillRect (QRectF (pl, L.band.top () - 1 - o.y (), pr - pl, 1.0), Theme::divider);
        double x = lx;
        const double topBase = baselineFor (fa, L.mapTop - o.y ());
        if (kind_ == Kind::Lanes && !L.spec.label.isEmpty ())
        {
            p.setPen (Theme::textDim);
            p.drawText (QPointF (x, topBase), L.spec.label);
            x += fa.horizontalAdvance (L.spec.label) + 8.0;
        }
        if (L.finite)
        {
            p.setPen (Theme::textFaint);
            p.drawText (QPointF (x, topBase), axisLabel (L, L.dHi, false));
            p.drawText (QPointF (lx, baselineFor (fa, L.mapBottom - o.y ())), axisLabel (L, L.dLo, false));
        }
    }
}

void PlotWidget::paintRecess (QPainter &p)
{
    ensureRecessCache ();
    p.drawPixmap (recessRect_.topLeft (), cache_);

    const qreal dpr = devicePixelRatioF ();
    const bool hero = kind_ == Kind::Hero;
    const double designW = hero ? 1.4 : 1.3;

    p.save ();
    for (const Lane &L : lanes_)
    {
        p.setClipRect (L.band.intersected (plotRect_));
        for (int tr = 0; tr < static_cast<int> (L.segCount.size ()); ++tr)
        {
            const std::size_t ti = static_cast<std::size_t> (tr);
            const Trace &T = L.spec.traces[tr];
            const QPen fast = cosmeticPen (T.color);
            QPen fastDash = fast;
            QPen design (T.color, designW);
            design.setCapStyle (Qt::FlatCap);
            design.setJoinStyle (Qt::RoundJoin);
            if (T.dash == Dash::Dashed)
            {
                fastDash.setDashPattern ({7.0 * dpr, 4.0 * dpr});
                design.setDashPattern ({7.0 / designW, 4.0 / designW});
            }
            else if (T.dash == Dash::Dotted)
            {
                fastDash.setDashPattern ({2.0 * dpr, 4.0 * dpr});
                design.setDashPattern ({2.0 / designW, 4.0 / designW});
            }
            const QPen dot = [&T] {
                QPen d = cosmeticPen (T.color, 3.0);
                d.setCapStyle (Qt::RoundCap);
                return d;
            }();
            const auto &segs = L.polys[ti];
            const auto &flags = L.denseFlags[ti];
            for (int s = 0; s < L.segCount[ti]; ++s)
            {
                const std::size_t si = static_cast<std::size_t> (s);
                const QPolygonF &poly = segs[si];
                if (poly.size () == 1)
                {
                    p.setRenderHint (QPainter::Antialiasing, true);
                    p.setPen (dot);
                    p.drawPoint (poly[0]);
                    continue;
                }
                if (poly.size () < 2)
                    continue;
                const double span = std::fabs (poly.last ().x () - poly.first ().x ()) + 1.0;
                const double density = static_cast<double> (poly.size ()) / span;
                const bool decimated = si < flags.size () && flags[si] != 0;
                if (decimated || density > 2.0)
                {
                    // min/max zig-zag: vertical pixel runs, aliased, solid
                    p.setRenderHint (QPainter::Antialiasing, false);
                    p.setPen (fast);
                }
                else if (density > kSparseDensity || !kDesignPenWhenSparse)
                {
                    // exact samples: antialiased, the trace's dash pattern
                    // (cosmetic dashes stay on the fast path)
                    p.setRenderHint (QPainter::Antialiasing, true);
                    p.setPen (fastDash);
                }
                else
                {
                    p.setRenderHint (QPainter::Antialiasing, true);
                    p.setPen (design);
                }
                p.drawPolyline (poly);
            }
        }
    }
    p.restore ();

    // ---- dynamic overlays
    bool labels = false; // any finite sample in view (a status-only ring shows the placeholder)
    for (const Lane &L : lanes_)
        labels = labels || L.finite;
    p.setRenderHint (QPainter::TextAntialiasing);
    const QPointF c = QRectF (plotRect_).center ();
    const bool overlayUp = overlay_ && overlay_->isVisible ();
    if (!stallText_.isEmpty ())
    {
        // Plain amber text over the traces, as in the design; a 1 px recess
        // halo keeps it legible over dense data without blanking a band.
        const QFontMetricsF fm (fStall_);
        const double w = fm.horizontalAdvance (stallText_);
        const QPointF at (c.x () - w / 2.0, baselineFor (fm, c.y ()));
        p.setFont (fStall_);
        p.setPen (Theme::recess);
        for (const QPointF d : {QPointF (-1, 0), QPointF (1, 0), QPointF (0, -1), QPointF (0, 1)})
            p.drawText (at + d, stallText_);
        p.setPen (Theme::warn);
        p.drawText (at, stallText_);
    }
    else if (!labels && !placeholder_.isEmpty () && !overlayUp)
    {
        const QFontMetricsF fm (fEmpty_);
        p.setFont (fEmpty_);
        p.setPen (Theme::textFainter);
        p.drawText (QPointF (c.x () - fm.horizontalAdvance (placeholder_) / 2.0, baselineFor (fm, c.y ())), placeholder_);
    }
    if (paused_)
    {
        const QFont f = Theme::mono (9.5, 400, 0.12);
        const QFontMetricsF fm (f);
        const QString s = QStringLiteral ("PAUSED");
        const double w = std::ceil (fm.horizontalAdvance (s) + 12.0);
        const double top = plotRect_.top () + (hero ? 22.0 : 5.0);
        const QRect box (static_cast<int> (plotRect_.left () + plotRect_.width () - 7 - w), static_cast<int> (top),
            static_cast<int> (w), 16);
        p.fillRect (box, Theme::panel);
        outline (p, box, Theme::controlEdge);
        p.setFont (f);
        p.setPen (Theme::textBody);
        p.drawText (QPointF (box.left () + 6.0, baselineFor (fm, box.top () + 8.0)), s);
    }
    if (clockMismatch_)
    {
        const QFontMetricsF fm (fAxis_);
        p.setFont (fAxis_);
        p.setPen (Theme::textFaint);
        p.drawText (QPointF (plotRect_.left () + 6.0, plotRect_.top () + plotRect_.height () - (hero ? 20.0 : 4.0) - fm.descent ()),
            QStringLiteral ("timestamps offset from host clock: aligned to newest sample"));
    }
}
