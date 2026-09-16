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

const QString kNoValue = QStringLiteral ("——");

} // namespace

PlotWidget::PlotWidget (
    Kind kind, const QString &title, const QString &units, const QVector<Trace> &traces, QWidget *parent)
    : QWidget (parent), kind_ (kind), title_ (title), units_ (units), led_ (Theme::controlEdge),
      rateColor_ (Theme::textDim), placeholder_ (QStringLiteral ("NO SIGNAL")), traces_ (traces)
{
    setAttribute (Qt::WA_OpaquePaintEvent);
    setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Expanding);

    const bool hero = kind_ == Kind::Hero;
    fTitle_ = Theme::mono (hero ? 11.0 : 10.5, 400, 0.14);
    fUnits_ = Theme::mono (10);
    fSub_ = Theme::sans (11);
    fChip_ = Theme::mono (9.5, 400, 0.1);
    fKicker_ = Theme::mono (9, 400, 0.12);
    fValue_ = hero ? Theme::mono (19, 600) : Theme::mono (14, 600);
    fValueUnit_ = Theme::mono (11);
    fRate_ = Theme::mono (hero ? 12.0 : 10.0);
    fLetter_ = Theme::mono (9.5);
    fLegendValue_ = Theme::mono (11.5);
    fAxis_ = Theme::mono (hero ? 9.5 : 9.0);
    fEmpty_ = Theme::mono (hero ? 11.0 : 10.0, 400, 0.16);
    fStall_ = Theme::mono (10, 400, 0.14);

    const std::size_t nTr = static_cast<std::size_t> (traces_.size ());
    polys_.resize (nTr);
    segCount_.assign (nTr, 0);
    means_.assign (nTr, 0.0);
    denseFlags_.resize (nTr);
    denseMode_.assign (nTr, 0);
    for (int i = 0; i < traces_.size (); ++i)
        valueText_ << kNoValue;
    ppText_ = QStringLiteral ("— ") + units_;
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
    if (units == units_)
        return;
    units_ = units;
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
    yValid_ = false; // the autoscale restarts from the displayed data afterwards
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
    std::shared_ptr<SignalRing> ring, int rawChannel, double nominalRate, int valueDecimals)
{
    ring_ = std::move (ring);
    rawChannel_ = rawChannel;
    nominalRate_ = nominalRate;
    decimals_ = valueDecimals;
    lastWritten_ = UINT64_MAX;
    n_ = 0;
    haveLatest_ = false;
    yValid_ = false;
    dirty_ = true;
    pp_ = std::numeric_limits<double>::quiet_NaN ();
    for (auto &s : segCount_)
        s = 0;
    std::fill (denseMode_.begin (), denseMode_.end (), 0);
    refreshHeaderText ();
    update ();
}

void PlotWidget::setWindowSeconds (double seconds)
{
    windowSec_ = std::max (0.5, seconds);
    yValid_ = false;
    dirty_ = true;
}

void PlotWidget::setRemoveMean (bool on)
{
    if (removeMean_ == on)
        return;
    removeMean_ = on;
    yValid_ = false;
    dirty_ = true;
}

void PlotWidget::setPaused (bool on)
{
    paused_ = on;
    dirty_ = true;
    update (recessRect_);
}

// ------------------------------------------------------------------ data
void PlotWidget::tick (double now)
{
    if (!ring_)
    {
        if (n_ != 0 || dirty_)
        {
            n_ = 0;
            haveLatest_ = false;
            dirty_ = false;
            pp_ = std::numeric_limits<double>::quiet_NaN ();
            for (auto &s : segCount_)
                s = 0;
            if (refreshHeaderText ())
                updateHeader ();
            update (recessRect_);
        }
        return;
    }

    if (!paused_)
    {
        const std::uint64_t written = ring_->totalWritten ();
        const bool newData = written != lastWritten_;
        if (newData)
            lastChangeWall_ = now;

        // Right edge = host "now" (BrainFlow timestamps use the host clock), so
        // a stalled stream visibly scrolls away. Safety net: if data is
        // arriving but its timestamps are far from the host clock, anchor to
        // the newest sample instead of showing an empty plot.
        double ref = now;
        const double latestTs = ring_->latestTimestamp ();
        clockMismatch_ = std::isfinite (latestTs) && (now - lastChangeWall_) < 2.0 &&
            std::fabs (latestTs - now) > 30.0;
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
        lastWritten_ = written; // consumed only when we actually rebuild
        refTime_ = ref;

        const std::size_t prevN = n_;
        const double tmin = ref - windowSec_ * 1.02 - 1.0 / std::max (1.0, nominalRate_);
        n_ = ring_->copySince (tmin, t_, v_);
        haveLatest_ = ring_->latest (latestTs_, latestVals_);
        if (n_ == 0 && prevN == 0 && !dirty_ && !newData)
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

void PlotWidget::updateYRange (double lo, double hi)
{
    // Smallest span worth resolving: one unit of the displayed precision.
    const double floorSpan = std::pow (10.0, -std::clamp (decimals_, 0, 6));
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
    const double dt = std::clamp (now - lastYUpdate_, 0.0, 0.25);
    lastYUpdate_ = now;

    if (!yValid_)
    {
        yLo_ = tLo;
        yHi_ = tHi;
        yValid_ = true;
        shrinking_ = false;
        return;
    }
    if (tLo < yLo_)
        yLo_ = tLo;
    if (tHi > yHi_)
        yHi_ = tHi;
    const double cur = yHi_ - yLo_;
    const double tgt = tHi - tLo;
    if (!shrinking_ && cur > 1.6 * tgt)
        shrinking_ = true;
    if (shrinking_)
    {
        const double k = 1.0 - std::exp (-dt / 0.6);
        yLo_ += (tLo - yLo_) * k;
        yHi_ += (tHi - yHi_) * k;
        if (yHi_ - yLo_ <= 1.05 * tgt)
            shrinking_ = false;
    }
}

bool PlotWidget::rawView () const
{
    return railMode_ && kind_ == Kind::Hero && rawChannel_ >= 0;
}

void PlotWidget::computeDisplayRange ()
{
    if (railMode_ && kind_ == Kind::Hero)
    {
        // Near-rail view: the raw value on a fixed +-full-scale range. The
        // rails sit near the recess top / bottom as in the design's artboard
        // (22 / 300 and 278 / 300 of the recess), 0 in the middle.
        dLo_ = -fullScale_;
        dHi_ = fullScale_;
        dStep_ = fullScale_;
        const double h = plotRect_.height ();
        yTop_ = plotRect_.top () + h * 22.0 / 300.0;
        yBot_ = plotRect_.top () + h * 278.0 / 300.0;
        return;
    }
    yTop_ = mapTop_;
    yBot_ = mapBottom_;
    double lo = yLo_, hi = yHi_;
    if (symmetric_)
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
    dLo_ = sl;
    dHi_ = sh;
    dStep_ = step;
}

void PlotWidget::rebuild ()
{
    dirty_ = false;
    const int nTr = std::min (static_cast<int> (traces_.size ()), static_cast<int> (v_.size ()));

    double lo = std::numeric_limits<double>::infinity ();
    double hi = -std::numeric_limits<double>::infinity ();
    for (double &m : means_)
        m = 0.0;
    pp_ = std::numeric_limits<double>::quiet_NaN ();
    // Near-rail view plots the raw channel (no mean removal) as trace 0.
    const bool raw = rawView () && rawChannel_ < static_cast<int> (v_.size ());
    auto series = [&] (int tr) -> const std::vector<double> & {
        return v_[static_cast<std::size_t> (raw && tr == 0 ? rawChannel_ : tr)];
    };

    if (n_ > 0)
    {
        const double tWin = refTime_ - windowSec_;
        for (int tr = 0; tr < nTr; ++tr)
        {
            const std::vector<double> &vv = series (tr);
            double off = 0.0;
            if (removeMean_ && !(raw && tr == 0))
            {
                double sum = 0.0;
                std::size_t cnt = 0;
                for (std::size_t k = 0; k < n_; ++k)
                    if (std::isfinite (vv[k]))
                    {
                        sum += vv[k];
                        ++cnt;
                    }
                off = cnt ? sum / static_cast<double> (cnt) : 0.0;
            }
            means_[static_cast<std::size_t> (tr)] = off;
            double wLo = std::numeric_limits<double>::infinity ();
            double wHi = -std::numeric_limits<double>::infinity ();
            for (std::size_t k = 0; k < n_; ++k)
            {
                const double x = vv[k] - off;
                if (!std::isfinite (x))
                    continue;
                lo = std::min (lo, x);
                hi = std::max (hi, x);
                if (tr == 0 && t_[k] >= tWin)
                {
                    wLo = std::min (wLo, x);
                    wHi = std::max (wHi, x);
                }
            }
            if (tr == 0 && std::isfinite (wLo))
                pp_ = wHi - wLo;
        }
    }
    if (raw)
    {
        // fixed range (computeDisplayRange); the autoscale state is left alone
    }
    else if (std::isfinite (lo) && std::isfinite (hi))
        updateYRange (lo, hi);
    else if (!yValid_)
    {
        yLo_ = -1.0;
        yHi_ = 1.0;
    }
    computeDisplayRange ();

    PixelMap m;
    m.tStart = refTime_ - windowSec_;
    m.tSpan = windowSec_;
    m.left = plotRect_.left ();
    m.width = std::max (1, plotRect_.width ());
    m.vLo = dLo_;
    m.vSpan = std::max (1e-12, dHi_ - dLo_);
    m.top = yTop_;
    m.height = std::max (1.0, yBot_ - yTop_);
    const double gapSec = std::max (1.0, 10.0 / std::max (0.1, nominalRate_));

    lastPoints_ = 0;
    for (int tr = 0; tr < static_cast<int> (segCount_.size ()); ++tr)
    {
        const std::size_t ti = static_cast<std::size_t> (tr);
        auto &segs = polys_[ti];
        if (tr >= nTr || n_ == 0)
        {
            segCount_[ti] = 0;
            denseFlags_[ti].clear ();
            continue;
        }
        std::size_t pts = 0;
        bool dense = denseMode_[ti] != 0;
        segCount_[ti] = buildPolylines (t_.data (), series (tr).data (), n_, means_[ti], m, gapSec, segs, &pts,
            &denseFlags_[ti], &dense);
        denseMode_[ti] = dense ? 1 : 0;
        lastPoints_ += pts;
    }
}

bool PlotWidget::refreshHeaderText ()
{
    const bool raw = rawView ();
    QStringList vals;
    const bool have = tone_ != Tone::Off && haveLatest_;
    for (int i = 0; i < traces_.size (); ++i)
    {
        if (!have || i >= static_cast<int> (latestVals_.size ()))
        {
            vals << kNoValue;
            continue;
        }
        double val = latestVals_[static_cast<std::size_t> (i)];
        if (kind_ == Kind::Hero)
        {
            if (raw && rawChannel_ < static_cast<int> (latestVals_.size ()))
                val = latestVals_[static_cast<std::size_t> (rawChannel_)];
            else if (removeMean_ && i < static_cast<int> (means_.size ()))
                val -= means_[static_cast<std::size_t> (i)];
        }
        vals << Readouts::number (val, decimals_, kind_ != Kind::Scalar, true);
    }
    QString pp = QStringLiteral ("— ") + units_;
    if (tone_ != Tone::Off && std::isfinite (pp_))
        pp = Readouts::number (pp_, decimals_, false, true) + QStringLiteral (" ") + units_;
    const bool changed = vals != valueText_ || pp != ppText_ || raw != latestIsRaw_;
    valueText_ = vals;
    ppText_ = pp;
    latestIsRaw_ = raw;
    return changed;
}

void PlotWidget::updateHeader ()
{
    QRegion r (headerRect_);
    if (!legendRect_.isNull ())
        r += legendRect_;
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
    headerRect_ = QRect (1, 1, std::max (0, w - 2), headerH);
    int y = 1 + headerH + 1; // divider under the header
    if (kind_ == Kind::Triple)
    {
        legendRect_ = QRect (1, y, std::max (0, w - 2), 5 + 16 + 5);
        y += legendRect_.height () + 1;
    }
    else
        legendRect_ = QRect ();
    const int m = kind_ == Kind::Hero ? 8 : 6;
    recessRect_ = QRect (1 + m, y + m, std::max (12, w - 2 - 2 * m), std::max (12, h - 1 - m - (y + m)));
    plotRect_ = recessRect_.adjusted (1, 1, -1, -1);
    if (kind_ == Kind::Hero)
    {
        mapTop_ = plotRect_.top () + 11.0;
        mapBottom_ = plotRect_.top () + plotRect_.height () - 24.0;
    }
    else
    {
        mapTop_ = plotRect_.top () + 8.0;
        mapBottom_ = plotRect_.top () + plotRect_.height () - 8.0;
    }
    if (mapBottom_ - mapTop_ < 8.0)
    {
        mapTop_ = plotRect_.top ();
        mapBottom_ = plotRect_.top () + plotRect_.height ();
    }
    yTop_ = mapTop_;
    yBot_ = mapBottom_;
    if (overlay_)
        overlay_->setGeometry (plotRect_);
    cacheValid_ = false;
}

double PlotWidget::mapY (double v) const
{
    return yBot_ - (v - dLo_) / std::max (1e-12, dHi_ - dLo_) * (yBot_ - yTop_);
}

QString PlotWidget::axisLabel (double v, bool withUnits) const
{
    // explicit '+' only on axes that also show negative values
    QString s = Readouts::number (v, decimalsForStep (dStep_), dLo_ < 0.0, true);
    if (withUnits && !units_.isEmpty ())
        s += QStringLiteral (" ") + units_;
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
    if (kind_ == Kind::Triple)
        p.fillRect (QRect (1, legendRect_.bottom () + 1, width () - 2, 1), Theme::divider);
    p.setRenderHint (QPainter::TextAntialiasing);
    if (kind_ == Kind::Hero)
        paintHeroHeader (p);
    else
        paintPanelHeader (p);
    if (kind_ == Kind::Triple)
        paintLegend (p);
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
    const double uw = fu.horizontalAdvance (units_);
    const double vw = fv.horizontalAdvance (latest);
    column (latestIsRaw_ ? QStringLiteral ("LATEST RAW") : QStringLiteral ("LATEST"), 104.0,
        [&] (double r) {
            p.setFont (fValueUnit_);
            p.setPen (Theme::textMuted);
            p.drawText (QPointF (r - uw, bigBase), units_);
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
        outline (p, r, c.on ? Theme::controlEdge : Theme::divider);
        p.setPen (c.on ? Theme::textMuted : Theme::textFainter);
        p.drawText (QPointF (r.left () + 6.0, baselineFor (fc, r.top () + chipH / 2.0)), c.text);
        x += w + 6.0;
    }
}

void PlotWidget::paintPanelHeader (QPainter &p)
{
    const QRect hr = headerRect_;
    const double cy = hr.top () + hr.height () / 2.0;
    const double gap = kind_ == Kind::Scalar ? 9.0 : 8.0;
    const QFontMetricsF ft (fTitle_), fu (fUnits_), fr (fRate_), fv (fValue_);
    const QColor valC = tone_ == Tone::Off ? Theme::textDim : (tone_ == Tone::Warn ? Theme::warn : Theme::textStrong);
    double right = hr.left () + hr.width () - 9.0;
    if (kind_ == Kind::Scalar)
    {
        const QString v = valueText_.value (0, kNoValue);
        const double vw = fv.horizontalAdvance (v);
        p.setFont (fValue_);
        p.setPen (valC);
        p.drawText (QPointF (right - vw, baselineFor (fv, cy)), v);
        right -= std::max (62.0, vw) + 9.0;
    }
    const double rw = fr.horizontalAdvance (rateText_);
    p.setFont (fRate_);
    p.setPen (rateColor_);
    p.drawText (QPointF (right - rw, baselineFor (fr, cy)), rateText_);
    right -= rw + gap;

    double x = hr.left () + 9.0;
    p.fillRect (QRectF (x, std::floor (cy - 3.5), 7, 7), led_);
    x += 7.0 + gap;
    p.setFont (fTitle_);
    p.setPen (Theme::textStrong);
    const QString title = ft.elidedText (title_, Qt::ElideRight, std::max (16.0, right - x));
    p.drawText (QPointF (x, baselineFor (ft, cy)), title);
    x += ft.horizontalAdvance (title) + gap;
    if (x + fu.horizontalAdvance (units_) <= right)
    {
        p.setFont (fUnits_);
        p.setPen (Theme::textDim);
        p.drawText (QPointF (x, baselineFor (fu, cy)), units_);
        x += fu.horizontalAdvance (units_) + gap;
        // source substituted through a fallback (details in the tooltip)
        const QString tag = QStringLiteral ("SUBST");
        const QFontMetricsF fc (fChip_);
        if (noteFlag_ && x + fc.horizontalAdvance (tag) <= right)
        {
            p.setFont (fChip_);
            p.setPen (Theme::textFaint);
            p.drawText (QPointF (x, baselineFor (fc, cy)), tag);
        }
    }
}

void PlotWidget::paintLegend (QPainter &p)
{
    const QRect lr = legendRect_;
    const double cy = lr.top () + lr.height () / 2.0;
    const double left = lr.left () + 9.0;
    const double inner = lr.width () - 18.0;
    const int n = static_cast<int> (traces_.size ());
    const double colW = (inner - 10.0 * (n - 1)) / std::max (1, n);
    const QFontMetricsF fl (fLetter_), fv (fLegendValue_);
    const QColor valC = tone_ == Tone::Off ? Theme::textDim : (tone_ == Tone::Warn ? Theme::warn : Theme::textStrong);
    for (int i = 0; i < n; ++i)
    {
        const double x0 = left + i * (colW + 10.0);
        // 14 x 6 legend line, 2 px, the trace's dash pattern (4 2.5 / 1 2.5)
        QPen pen (traces_[i].color, 2.0);
        pen.setCapStyle (Qt::FlatCap);
        if (traces_[i].dash == Dash::Dashed)
            pen.setDashPattern ({4.0 / 2.0, 2.5 / 2.0});
        else if (traces_[i].dash == Dash::Dotted)
            pen.setDashPattern ({1.0 / 2.0, 2.5 / 2.0});
        p.save ();
        p.setRenderHint (QPainter::Antialiasing);
        p.setPen (pen);
        p.drawLine (QPointF (x0, std::round (cy)), QPointF (x0 + 14.0, std::round (cy)));
        p.restore ();
        p.setFont (fLetter_);
        p.setPen (Theme::textMuted);
        p.drawText (QPointF (x0 + 14.0 + 5.0, baselineFor (fl, cy)), traces_[i].name);
        const QString v = valueText_.value (i, kNoValue);
        p.setFont (fLegendValue_);
        p.setPen (valC);
        p.drawText (QPointF (x0 + colW - fv.horizontalAdvance (v), baselineFor (fv, cy)), v);
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
    const bool labels = n_ > 0;
    if (cacheValid_ && sz == cacheSize_ && dpr == cacheDpr_ && dLo_ == cacheLo_ && dHi_ == cacheHi_ &&
        windowSec_ == cacheWindow_ && railOn == cacheRail_ && labels == cacheLabels_)
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
    cacheLo_ = dLo_;
    cacheHi_ = dHi_;
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
        // zero line
        if (0.0 >= dLo_ && 0.0 <= dHi_)
        {
            const double y = std::round (mapY (0.0)) - o.y ();
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
                const double y = std::round (mapY (v)) - o.y () + 0.5;
                if (y >= ptop && y <= pbot)
                    p.drawLine (QPointF (pl, y), QPointF (pr, y));
            }
        }
        // Y labels: top / middle / bottom of the range (design positions)
        p.setPen (Theme::textDim);
        const double mid = (0.0 >= dLo_ && 0.0 <= dHi_) ? 0.0 : 0.5 * (dLo_ + dHi_);
        const double lx = pl + 6.0;
        if (labels)
        {
            p.drawText (QPointF (lx, baselineFor (fa, mapTop_ - o.y ())), axisLabel (dHi_, true));
            p.drawText (QPointF (lx, baselineFor (fa, mapY (mid) - o.y ()) - (mid == 0.0 ? 7.0 : 0.0)),
                axisLabel (mid, true));
            p.drawText (QPointF (lx, baselineFor (fa, mapBottom_ - o.y ())), axisLabel (dLo_, true));
        }
        // full scale, top right
        const QString fs = QStringLiteral ("FS ±") + Readouts::number (fullScale_, 0, false, true) +
            QStringLiteral (" ") + units_;
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
    }
    else
    {
        // Panels: faint range labels at the left (top / bottom of the range).
        p.setPen (Theme::textFaint);
        const double lx = pl + 5.0;
        if (labels)
        {
            p.drawText (QPointF (lx, baselineFor (fa, mapTop_ - o.y ())), axisLabel (dHi_, false));
            p.drawText (QPointF (lx, baselineFor (fa, mapBottom_ - o.y ())), axisLabel (dLo_, false));
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
    p.setClipRect (plotRect_);
    for (int tr = 0; tr < static_cast<int> (segCount_.size ()); ++tr)
    {
        const std::size_t ti = static_cast<std::size_t> (tr);
        const Trace &T = traces_[tr];
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
        const auto &segs = polys_[ti];
        const auto &flags = denseFlags_[ti];
        for (int s = 0; s < segCount_[ti]; ++s)
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
    p.restore ();

    // ---- dynamic overlays
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
    else if (n_ == 0 && !placeholder_.isEmpty () && !overlayUp)
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
