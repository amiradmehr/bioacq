#pragma once

#include <QColor>
#include <QFont>
#include <QPixmap>
#include <QPointer>
#include <QPolygonF>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

class SignalRing;

// Real-time strip chart drawn as one instrument-panel "section": 1 px panel
// edge, header (hero: LED, title, subtitle, filter chips, P-P / RATE / LATEST;
// panel: LED, title, units, rate, value), an optional readout strip, and a
// recessed plot on a dot raster.
//
// The recess holds one or more LANES stacked on one shared time axis. Each
// lane has its own ring buffer, traces, units and Y autoscale (Kind::Lanes:
// the IMU panel with ACC / GYR / MAG); every other kind has a single lane.
//
// Performance: tick() (one ~60 Hz timer for all plots) copies the visible
// window of every lane out of its ring buffer and rebuilds min/max-decimated
// polylines per trace (<= ~2 points per pixel column); paintEvent() blits a
// cached pixmap of the static recess and draws the cached geometry. The recess
// cache is two layers: the dot raster + border (rasterised only when size /
// DPR change) and, on top of one blit of it, the lane dividers, zero / rail
// lines and axis labels (redrawn only when a snapped Y range, the window or
// the rail view change). Trace frames repaint only the recess; header text is
// refreshed at <= 8 Hz and repaints only the header and strip. Traces use
// cosmetic 1 device-px pens -- Qt's fast line path; exact segments keep the
// design's dash patterns.
class PlotWidget : public QWidget
{
    Q_OBJECT

public:
    enum class Kind
    {
        Hero,   // ECG: big header readouts, 12 px dot raster, axis overlay, near-rail view
        Scalar, // one trace, value in the header (temperature, PPG)
        Lanes,  // stacked lanes with their own Y scale; readout strip with every value (IMU)
        Vital   // derived rate (heart rate): large value in the header, chip strip, trend
    };
    enum class Dash
    {
        Solid,
        Dashed, // 7 4
        Dotted  // 2 4
    };
    enum class Tone
    {
        Off,    // device not streaming: dim, values "--"
        Normal, // strong text
        Warn    // amber (stalled / near rail)
    };
    struct Trace
    {
        QString name;
        QColor color;
        Dash dash = Dash::Solid;
    };
    struct Chip
    {
        QString text;
        bool on = true;
        bool warn = false; // amber outline + text
        bool operator== (const Chip &o) const
        {
            return text == o.text && on == o.on && warn == o.warn;
        }
    };
    struct LaneSpec
    {
        QString label; // Lanes: "ACC" (in the lane and the readout strip)
        QString units;
        QVector<Trace> traces;
        double minSpan = 0.0; // smallest Y span shown; 0 = one display digit
    };

    PlotWidget (Kind kind, const QString &title, const QVector<LaneSpec> &lanes, QWidget *parent = nullptr);
    PlotWidget (Kind kind, const QString &title, const QString &units, const QVector<Trace> &traces,
        QWidget *parent = nullptr);

    int laneCount () const
    {
        return static_cast<int> (lanes_.size ());
    }

    // ---- header
    void setTitle (const QString &title);
    void setUnits (const QString &units); // lane 0 (the header's units)
    void setLaneUnits (int lane, const QString &units);
    void setSubtitle (const QString &text);     // hero
    void setChips (const QVector<Chip> &chips); // hero
    void setLed (const QColor &c);
    void setRate (const QString &text, const QColor &color); // "24.9 / 25 Hz"
    // Panels (not the hero): a header too narrow for the full rate shows the
    // measured part ("24.9 Hz") before it hides the rate. setRateCompact
    // forces that form, so a row of sibling panels can stay uniform;
    // fullRateFits says whether the full text fits in this header.
    void setRateCompact (bool on);
    bool fullRateFits () const;
    void setTone (Tone t);
    // Vital: the header value and the chips of the strip, set by the owner
    // (the heart-rate readout rules live in Readouts.h).
    void setValueText (const QString &text);
    void setStripChips (const QVector<Chip> &chips);

    // ---- recess
    void setPlaceholder (const QString &text); // shown while there are no samples
    void setStallText (const QString &text);   // amber overlay, "" = none
    // Signal source description: shown as the plot's tooltip; `flagged` adds a
    // faint SUBST tag to the header (channel resolved through a fallback).
    void setNote (const QString &text, bool flagged = false);
    // hero: raw Ch1 on a fixed +-full-scale range with the dashed rails
    void setRailMode (bool on);
    void setFullScale (double fs)
    {
        fullScale_ = fs;
    }
    void setSymmetric (bool on); // hero: range symmetric around 0
    void setOverlay (QWidget *w); // placed over the recess (idle call to action)
    QRect recessRect () const
    {
        return recessRect_;
    }

    // ---- data
    // rawChannel: ring channel with the unfiltered value (-1 = none).
    void setSource (int lane, std::shared_ptr<SignalRing> ring, int rawChannel, double nominalRate, int valueDecimals);
    void setSource (std::shared_ptr<SignalRing> ring, int rawChannel, double nominalRate, int valueDecimals)
    {
        setSource (0, std::move (ring), rawChannel, nominalRate, valueDecimals);
    }
    void setWindowSeconds (double seconds);
    void setRemoveMean (bool on);
    void setPaused (bool on);
    void tick (double nowUnix);

    // ---- readouts (visible window)
    bool hasSamples () const;
    double peakToPeak () const
    {
        return pp_;
    }
    std::size_t lastPointCount () const
    {
        return lastPoints_;
    }
    QSize minimumSizeHint () const override
    {
        return QSize (180, kind_ == Kind::Hero ? 150 : (kind_ == Kind::Lanes ? 160 : 100));
    }
    QSize sizeHint () const override
    {
        return QSize (400, 220);
    }

protected:
    void paintEvent (QPaintEvent *event) override;
    void resizeEvent (QResizeEvent *event) override;

private:
    struct Lane
    {
        LaneSpec spec;
        std::shared_ptr<SignalRing> ring;
        int rawChannel = -1;
        double nominalRate = 0.0;
        int decimals = 2;
        std::uint64_t lastWritten = UINT64_MAX;

        // visible window (reused storage)
        std::vector<double> t;
        std::vector<std::vector<double>> v;
        std::size_t n = 0;
        bool finite = false; // any finite value in view
        std::vector<double> means;

        // newest sample
        bool haveLatest = false;
        double latestTs = 0.0;
        std::vector<double> latestVals;

        // Y autoscale with hysteresis, then snapped to nice steps for display
        bool yValid = false;
        bool shrinking = false;
        double yLo = -1.0, yHi = 1.0;
        double lastYUpdate = 0.0;
        double dLo = -1.0, dHi = 1.0, dStep = 0.5;

        // geometry: the lane's band, the autoscaled value range -> these rows,
        // and the rows actually used (the rail view differs)
        QRect band;
        double mapTop = 0.0, mapBottom = 1.0;
        double yTop = 0.0, yBot = 1.0;

        // cached geometry per trace
        std::vector<std::vector<QPolygonF>> polys;
        std::vector<int> segCount;
        std::vector<std::vector<unsigned char>> denseFlags;
        std::vector<char> denseMode;
    };

    struct StripColumn
    {
        double x, w;
    };
    // What a panel header shows: the value's reserved width, the rate form
    // (2 full, 1 measured only, 0 none), the units, the SUBST tag and (Lanes)
    // the X / Y / Z key.
    struct PanelHeaderFit
    {
        double valueW = 0.0;
        int rate = 0;
        bool showUnits = false, showTag = false, showKey = false;
    };

    void init ();
    PanelHeaderFit panelHeaderFit (bool allowFullRate) const;
    QString compactRateText () const;
    // Lanes strip format from the geometry and a fixed value template per
    // lane: 3 kicker with units + glyph + letter + value, 2 kicker without
    // units, 1 glyph + value, 0 values only. Fills the kickers and columns.
    int laneStripLayout (std::vector<QString> *kicks, std::vector<StripColumn> *cols) const;
    double laneKeyWidth () const;
    void paintLaneKey (QPainter &p, double x, double cy);
    bool hasStrip () const
    {
        return kind_ == Kind::Lanes || kind_ == Kind::Vital;
    }
    void layoutRects ();
    void rebuild ();
    void rebuildLane (Lane &L, bool first);
    void updateYRange (Lane &L, double lo, double hi);
    void computeDisplayRange (Lane &L);
    bool rawView () const; // hero in rail mode: plotting the raw channel
    void ensureDotRaster (const QSize &sz, qreal dpr);
    void ensureRecessCache ();
    bool refreshHeaderText (); // true if any header string changed
    void updateHeader ();
    void paintChrome (QPainter &p);
    void paintHeroHeader (QPainter &p);
    void paintPanelHeader (QPainter &p);
    void paintLaneStrip (QPainter &p);
    void paintChipStrip (QPainter &p);
    void paintRecess (QPainter &p);
    double mapY (const Lane &L, double v) const;
    QString axisLabel (const Lane &L, double v, bool withUnits) const;
    const QString &units () const
    {
        return lanes_.front ().spec.units;
    }

    const Kind kind_;
    QString title_, subtitle_;
    QVector<Chip> chips_, stripChips_;
    QColor led_;
    QString rateText_;
    QColor rateColor_;
    bool rateCompact_ = false;
    Tone tone_ = Tone::Off;
    QString placeholder_, stallText_, note_, valueOverride_;
    bool noteFlag_ = false;
    bool railMode_ = false;
    bool symmetric_ = false;
    double fullScale_ = 187500.0;
    QPointer<QWidget> overlay_;
    std::vector<Lane> lanes_; // never empty

    double windowSec_ = 10.0;
    bool removeMean_ = false;
    bool paused_ = false;
    bool dirty_ = true;
    bool clockMismatch_ = false;
    double refTime_ = 0.0;
    double lastChangeWall_ = 0.0;
    double lastRebuildWall_ = 0.0;

    // readouts
    double pp_ = std::numeric_limits<double>::quiet_NaN ();
    QStringList valueText_; // header / strip values, lane by lane
    QString ppText_;
    bool latestIsRaw_ = false;
    double lastHeaderWall_ = 0.0;

    // geometry (logical px)
    QRect headerRect_, stripRect_, recessRect_, plotRect_;

    // recess cache: dot raster (size / DPR only), then lines + labels on top
    QPixmap dots_;
    QSize dotsSize_;
    double dotsDpr_ = 0.0;
    QPixmap cache_;
    bool cacheValid_ = false;
    QSize cacheSize_;
    double cacheDpr_ = 0.0;
    std::vector<double> cacheRanges_; // dLo, dHi per lane
    double cacheWindow_ = 0.0;
    bool cacheRail_ = false;
    bool cacheLabels_ = false; // axis labels only while there are samples

    std::size_t lastPoints_ = 0;

    // fonts
    QFont fTitle_, fUnits_, fSub_, fChip_, fKicker_, fValue_, fValueUnit_, fRate_, fLetter_, fLegendValue_,
        fAxis_, fEmpty_, fStall_;
};
