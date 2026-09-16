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
// panel: LED, title, units, rate, value), optional X/Y/Z legend strip, and a
// recessed plot on a dot raster.
//
// Performance: tick() (one ~60 Hz timer for all plots) copies the visible
// window out of the ring buffer and rebuilds min/max-decimated polylines
// (<= ~2 points per pixel column); paintEvent() blits a cached pixmap of the
// static recess and draws the cached geometry. The recess cache is two
// layers: the dot raster + border (rasterised only when size / DPR change)
// and, on top of one blit of it, the zero / rail lines and axis labels
// (redrawn only when the snapped Y range, window or rail view change).
// Trace frames repaint only the recess; header text is refreshed at <= 8 Hz
// and repaints only the header. Traces use cosmetic 1 device-px pens -- Qt's
// fast line path; exact segments keep the design's dash patterns.
class PlotWidget : public QWidget
{
    Q_OBJECT

public:
    enum class Kind
    {
        Hero,   // Cyton Ch1: big header readouts, 12 px dot raster, axis overlay
        Scalar, // temperature / PPG: value in the header
        Triple  // accel / gyro / mag: X / Y / Z legend strip with values
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
        bool operator== (const Chip &o) const
        {
            return text == o.text && on == o.on;
        }
    };

    PlotWidget (Kind kind, const QString &title, const QString &units, const QVector<Trace> &traces,
        QWidget *parent = nullptr);

    // ---- header
    void setTitle (const QString &title);
    void setUnits (const QString &units);
    void setSubtitle (const QString &text); // hero
    void setChips (const QVector<Chip> &chips); // hero
    void setLed (const QColor &c);
    void setRate (const QString &text, const QColor &color);
    void setTone (Tone t);

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
    void setSource (std::shared_ptr<SignalRing> ring, int rawChannel, double nominalRate, int valueDecimals);
    void setWindowSeconds (double seconds);
    void setRemoveMean (bool on);
    void setPaused (bool on);
    void tick (double nowUnix);

    // ---- readouts (visible window)
    bool hasSamples () const
    {
        return n_ > 0;
    }
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
        return QSize (180, kind_ == Kind::Hero ? 150 : 100);
    }
    QSize sizeHint () const override
    {
        return QSize (400, 220);
    }

protected:
    void paintEvent (QPaintEvent *event) override;
    void resizeEvent (QResizeEvent *event) override;

private:
    void layoutRects ();
    void rebuild ();
    void updateYRange (double lo, double hi);
    void computeDisplayRange ();
    bool rawView () const; // hero in rail mode: plotting the raw channel
    void ensureDotRaster (const QSize &sz, qreal dpr);
    void ensureRecessCache ();
    bool refreshHeaderText (); // true if any header string changed
    void updateHeader ();
    void paintChrome (QPainter &p);
    void paintHeroHeader (QPainter &p);
    void paintPanelHeader (QPainter &p);
    void paintLegend (QPainter &p);
    void paintRecess (QPainter &p);
    double mapY (double v) const;
    QString axisLabel (double v, bool withUnits) const;

    const Kind kind_;
    QString title_, units_, subtitle_;
    QVector<Chip> chips_;
    QColor led_;
    QString rateText_;
    QColor rateColor_;
    Tone tone_ = Tone::Off;
    QString placeholder_, stallText_, note_;
    bool noteFlag_ = false;
    bool railMode_ = false;
    bool symmetric_ = false;
    double fullScale_ = 187500.0;
    QPointer<QWidget> overlay_;
    QVector<Trace> traces_;

    std::shared_ptr<SignalRing> ring_;
    int rawChannel_ = -1;
    double nominalRate_ = 0.0;
    int decimals_ = 2;

    double windowSec_ = 10.0;
    bool removeMean_ = false;
    bool paused_ = false;
    bool dirty_ = true;
    bool clockMismatch_ = false;

    // visible window (reused storage)
    std::vector<double> t_;
    std::vector<std::vector<double>> v_;
    std::size_t n_ = 0;
    std::vector<double> means_;
    double refTime_ = 0.0;
    std::uint64_t lastWritten_ = UINT64_MAX;
    double lastChangeWall_ = 0.0;
    double lastRebuildWall_ = 0.0;

    // newest sample
    bool haveLatest_ = false;
    double latestTs_ = 0.0;
    std::vector<double> latestVals_;

    // Y autoscale with hysteresis, then snapped to nice steps for display
    bool yValid_ = false;
    bool shrinking_ = false;
    double yLo_ = -1.0, yHi_ = 1.0;
    double lastYUpdate_ = 0.0;
    double dLo_ = -1.0, dHi_ = 1.0, dStep_ = 0.5;

    // readouts
    double pp_ = std::numeric_limits<double>::quiet_NaN ();
    QStringList valueText_; // header / legend values
    QString ppText_;
    bool latestIsRaw_ = false;
    double lastHeaderWall_ = 0.0;

    // geometry (logical px)
    QRect headerRect_, legendRect_, recessRect_, plotRect_;
    double mapTop_ = 0.0, mapBottom_ = 1.0; // autoscaled value range -> these rows
    double yTop_ = 0.0, yBot_ = 1.0;        // rows actually used (rail view differs)

    // recess cache: dot raster (size / DPR only), then lines + labels on top
    QPixmap dots_;
    QSize dotsSize_;
    double dotsDpr_ = 0.0;
    QPixmap cache_;
    bool cacheValid_ = false;
    QSize cacheSize_;
    double cacheDpr_ = 0.0;
    double cacheLo_ = 0.0, cacheHi_ = 0.0, cacheWindow_ = 0.0;
    bool cacheRail_ = false;
    bool cacheLabels_ = false; // axis labels only while there are samples

    // cached geometry
    std::vector<std::vector<QPolygonF>> polys_;
    std::vector<int> segCount_;
    std::vector<std::vector<unsigned char>> denseFlags_;
    std::vector<char> denseMode_;
    std::size_t lastPoints_ = 0;

    // fonts
    QFont fTitle_, fUnits_, fSub_, fChip_, fKicker_, fValue_, fValueUnit_, fRate_, fLetter_, fLegendValue_,
        fAxis_, fEmpty_, fStall_;
};
