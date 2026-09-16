#include "Widgets.h"

#include "Theme.h"

#include <QAbstractItemView>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QWheelEvent>

#include <algorithm>

namespace
{

// Crisp 1 px outline of r (widget coordinates, integer rect).
void outline (QPainter &p, const QRect &r, const QColor &c)
{
    if (r.width () <= 0 || r.height () <= 0)
        return;
    p.fillRect (QRect (r.left (), r.top (), r.width (), 1), c);
    p.fillRect (QRect (r.left (), r.bottom (), r.width (), 1), c);
    p.fillRect (QRect (r.left (), r.top (), 1, r.height ()), c);
    p.fillRect (QRect (r.right (), r.top (), 1, r.height ()), c);
}

// Draw text with its line box vertically centred on cy; returns the advance.
double drawTextV (QPainter &p, const QFont &f, const QColor &c, double x, double cy, const QString &s)
{
    const QFontMetricsF fm (f);
    p.setFont (f);
    p.setPen (c);
    p.drawText (QPointF (x, cy + (fm.ascent () - fm.descent ()) / 2.0), s);
    return fm.horizontalAdvance (s);
}

} // namespace

// ============================================================== Led
Led::Led (int size, QWidget *parent) : QWidget (parent), size_ (size), color_ (Theme::controlEdge)
{
    setFixedSize (size, size);
}

void Led::setColor (const QColor &c)
{
    if (c == color_)
        return;
    color_ = c;
    update ();
}

void Led::paintEvent (QPaintEvent *)
{
    QPainter p (this);
    p.fillRect (rect (), color_);
}

// ============================================================== StatusChip
StatusChip::StatusChip (int height, double fontPx, int ledSize, int padX, QWidget *parent)
    : QWidget (parent), h_ (height), led_ (ledSize), padX_ (padX), font_ (Theme::mono (fontPx, 400, 0.1)),
      color_ (Theme::textDim)
{
    setSizePolicy (QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void StatusChip::setState (const QString &text, const QColor &color, const QColor &fill)
{
    if (text == text_ && color == color_ && fill == fill_)
        return;
    const bool resize = text != text_;
    text_ = text;
    color_ = color;
    fill_ = fill;
    if (resize)
        updateGeometry ();
    update ();
}

QSize StatusChip::sizeHint () const
{
    const QFontMetricsF fm (font_);
    const int w = static_cast<int> (std::ceil (padX_ * 2 + led_ + 5 + fm.horizontalAdvance (text_)));
    return QSize (w, h_);
}

void StatusChip::paintEvent (QPaintEvent *)
{
    QPainter p (this);
    if (fill_.isValid ())
        p.fillRect (rect (), fill_);
    outline (p, rect (), color_);
    const int cy = height () / 2;
    p.fillRect (QRect (padX_, cy - led_ / 2, led_, led_), color_);
    p.setRenderHint (QPainter::TextAntialiasing);
    drawTextV (p, font_, color_, padX_ + led_ + 5, height () / 2.0, text_);
}

// ============================================================== ToggleSwitch
namespace
{
// The design's inline toggle is a content-box span: width 28, height 15,
// padding 2, border 1 -> 34 x 21 rendered, with an 11 px knob.
constexpr int kTrackW = 34;
constexpr int kTrackH = 21;
constexpr int kKnob = 11;
constexpr int kKnobInsetX = 3; // border 1 + padding 2
} // namespace

ToggleSwitch::ToggleSwitch (const QString &text, bool labelLeft, QWidget *parent)
    : QAbstractButton (parent), labelLeft_ (labelLeft)
{
    setText (text);
    setCheckable (true);
    setFont (Theme::sans (11.5));
    setCursor (Qt::PointingHandCursor);
    setFocusPolicy (Qt::TabFocus);
    setAttribute (Qt::WA_Hover);
    setSizePolicy (labelLeft ? QSizePolicy::Expanding : QSizePolicy::Preferred, QSizePolicy::Fixed);
}

QSize ToggleSwitch::sizeHint () const
{
    const QFontMetricsF fm (font ());
    const int h = std::max (kTrackH, static_cast<int> (std::ceil (fm.height ())));
    return QSize (kTrackW + 8 + static_cast<int> (std::ceil (fm.horizontalAdvance (text ()))), h);
}

QSize ToggleSwitch::minimumSizeHint () const
{
    return QSize (kTrackW + 8 + 40, sizeHint ().height ());
}

void ToggleSwitch::paintEvent (QPaintEvent *)
{
    QPainter p (this);
    p.setRenderHint (QPainter::TextAntialiasing);
    const bool on = isChecked ();
    const bool en = isEnabled ();
    const bool hover = en && underMouse ();
    const int ty = (height () - kTrackH) / 2;
    const int tx = labelLeft_ ? width () - kTrackW : 0;
    const QRect track (tx, ty, kTrackW, kTrackH);

    QColor bg, bd, knob, label;
    if (!en)
    {
        bg = Theme::panel;
        bd = Theme::divider;
        knob = Theme::controlEdge;
        label = Theme::textFainter;
    }
    else if (on)
    {
        bg = hover ? Theme::actionHover : Theme::action;
        bd = bg;
        knob = Theme::chassis;
        label = Theme::textBody;
    }
    else
    {
        bg = Theme::controlFill;
        bd = hover ? Theme::controlEdgeHover : Theme::controlEdge;
        knob = Theme::textDim;
        label = Theme::textMuted;
    }
    if (hasFocus ())
        bd = Theme::action;
    p.fillRect (track, bg);
    outline (p, track, bd);
    // 11 x 11 knob, vertically centred, flex-start / flex-end in the content box
    const int kx = on ? track.left () + kTrackW - kKnobInsetX - kKnob : track.left () + kKnobInsetX;
    p.fillRect (QRect (kx, track.top () + (kTrackH - kKnob) / 2, kKnob, kKnob), knob);

    const QFontMetricsF fm (font ());
    p.setFont (font ());
    p.setPen (label);
    const double base = height () / 2.0 + (fm.ascent () - fm.descent ()) / 2.0;
    if (labelLeft_)
    {
        const QString s = fm.elidedText (text (), Qt::ElideRight, std::max (0, width () - kTrackW - 8));
        p.drawText (QPointF (0, base), s);
    }
    else
    {
        const QString s = fm.elidedText (text (), Qt::ElideRight, std::max (0, width () - kTrackW - 8));
        p.drawText (QPointF (kTrackW + 8, base), s);
    }
}

// ============================================================== MeterBar
MeterBar::MeterBar (int height, const QColor &border, QWidget *parent)
    : QWidget (parent), h_ (height), border_ (border), color_ (Theme::controlEdge)
{
    setFixedHeight (height);
    setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void MeterBar::setValue (double fraction, const QColor &color)
{
    if (std::isfinite (fraction))
        fraction = std::clamp (fraction, 0.0, 1.0);
    const bool same = color == color_ &&
        ((std::isnan (fraction) && std::isnan (frac_)) || fraction == frac_);
    if (same)
        return;
    frac_ = fraction;
    color_ = color;
    update ();
}

void MeterBar::paintEvent (QPaintEvent *)
{
    QPainter p (this);
    p.fillRect (rect (), Theme::inputFill);
    outline (p, rect (), border_);
    if (!std::isfinite (frac_) || frac_ <= 0.0)
        return;
    const QRect inner = rect ().adjusted (2, 2, -2, -2); // 1 px border + 1 px padding
    const int w = std::max (1, static_cast<int> (std::round (inner.width () * frac_)));
    p.fillRect (QRect (inner.left (), inner.top (), w, inner.height ()), color_);
}

// ============================================================== Banner
Banner::Banner (QWidget *parent) : QWidget (parent)
{
    setFixedHeight (32);
    setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void Banner::setContent (const QColor &color, const QColor &back, const QString &tag, const QString &text,
    const QString &meta)
{
    if (color == color_ && back == back_ && tag == tag_ && text == text_ && meta == meta_)
        return;
    color_ = color;
    back_ = back;
    tag_ = tag;
    text_ = text;
    meta_ = meta;
    update ();
}

void Banner::paintEvent (QPaintEvent *)
{
    QPainter p (this);
    p.setRenderHint (QPainter::TextAntialiasing);
    p.fillRect (rect (), back_);
    outline (p, rect (), color_);
    const double cy = height () / 2.0;
    double x = 10.0;
    p.fillRect (QRectF (x, std::floor (cy - 3.5), 7, 7), color_);
    x += 7 + 10;
    x += drawTextV (p, Theme::mono (10, 400, 0.12), color_, x, cy, tag_) + 10;
    p.fillRect (QRectF (x, std::floor (cy - 8), 1, 16), Theme::controlEdge);
    x += 1 + 10;

    const QFont metaF = Theme::mono (10);
    const QFontMetricsF fmm (metaF);
    const double right = width () - 10.0;
    const QString meta = fmm.elidedText (meta_, Qt::ElideMiddle, std::max (0.0, (right - x) * 0.4));
    const double metaW = fmm.horizontalAdvance (meta);
    drawTextV (p, metaF, Theme::textMuted, right - metaW, cy, meta);

    const QFont bodyF = Theme::sans (11.5);
    const QFontMetricsF fmb (bodyF);
    const QString body = fmb.elidedText (text_, Qt::ElideRight, std::max (0.0, right - metaW - 16.0 - x));
    drawTextV (p, bodyF, Theme::textBody, x, cy, body);
}

// ============================================================== Stepper
Stepper::Stepper (int minimum, int maximum, int value, QWidget *parent)
    : QFrame (parent), min_ (minimum), max_ (maximum), value_ (std::clamp (value, minimum, maximum))
{
    setObjectName (QStringLiteral ("stepper"));
    setFixedHeight (28); // content-box 26 + 1 px border (design)
    setFocusPolicy (Qt::TabFocus);
    auto *h = new QHBoxLayout (this);
    h->setContentsMargins (1, 1, 1, 1);
    h->setSpacing (0);
    minus_ = new QPushButton (QStringLiteral ("−"));
    plus_ = new QPushButton (QStringLiteral ("+"));
    for (QPushButton *b : {minus_, plus_})
    {
        b->setProperty ("variant", "step");
        b->setFixedSize (24, 26);
        b->setFocusPolicy (Qt::NoFocus);
        b->setAutoRepeat (true);
        b->setAutoRepeatDelay (400);
        b->setAutoRepeatInterval (120);
    }
    minus_->setProperty ("side", "left");
    plus_->setProperty ("side", "right");
    label_ = new QLabel;
    label_->setFixedWidth (44);
    label_->setAlignment (Qt::AlignCenter);
    label_->setFont (Theme::mono (11));
    Theme::setTextColor (label_, Theme::textStrong);
    h->addWidget (minus_);
    h->addWidget (label_);
    h->addWidget (plus_);
    connect (minus_, &QPushButton::clicked, this, [this] { setValue (value_ - 1); });
    connect (plus_, &QPushButton::clicked, this, [this] { setValue (value_ + 1); });
    refresh ();
}

void Stepper::setValue (int v)
{
    v = std::clamp (v, min_, max_);
    if (v == value_)
        return;
    value_ = v;
    refresh ();
    emit valueChanged (value_);
}

void Stepper::refresh ()
{
    label_->setText (QStringLiteral ("%1.0 s").arg (value_));
    minus_->setEnabled (isEnabled () && value_ > min_);
    plus_->setEnabled (isEnabled () && value_ < max_);
}

void Stepper::wheelEvent (QWheelEvent *e)
{
    const int d = e->angleDelta ().y ();
    if (d != 0)
        setValue (value_ + (d > 0 ? 1 : -1));
    e->accept ();
}

void Stepper::keyPressEvent (QKeyEvent *e)
{
    if (e->key () == Qt::Key_Up || e->key () == Qt::Key_Right || e->key () == Qt::Key_Plus)
        setValue (value_ + 1);
    else if (e->key () == Qt::Key_Down || e->key () == Qt::Key_Left || e->key () == Qt::Key_Minus)
        setValue (value_ - 1);
    else
        QFrame::keyPressEvent (e);
}

// ============================================================== RescanButton
RescanButton::RescanButton (QWidget *parent) : QPushButton (parent)
{
    setFixedSize (28, 28);
    setProperty ("variant", "icon");
    setCursor (Qt::PointingHandCursor);
    setAttribute (Qt::WA_Hover);
}

void RescanButton::setLocked (bool on)
{
    if (on == locked_)
        return;
    locked_ = on;
    setEnabled (!on); // inert ...
    setCursor (on ? Qt::ArrowCursor : Qt::PointingHandCursor);
    update ();
}

void RescanButton::paintEvent (QPaintEvent *)
{
    QPainter p (this);
    const bool en = isEnabled ();
    const bool down = isDown ();
    const bool hover = en && underMouse ();
    QColor bg = Theme::controlFill, bd = Theme::controlEdge, fg = Theme::textBody;
    if (!en && !locked_) // ... but a locked button keeps the rest look
    {
        bg = Theme::panel;
        bd = Theme::divider;
        fg = Theme::textFainter;
    }
    else if (down)
    {
        bg = Theme::inputFill;
        bd = Theme::controlEdgeHover;
        fg = Theme::textMuted;
    }
    else if (hover)
    {
        bg = Theme::controlFillHover;
        bd = Theme::controlEdgeHover;
        fg = Theme::textStrong;
    }
    if (hasFocus ())
        bd = Theme::action;
    p.fillRect (rect (), bg);
    outline (p, rect (), bd);

    // Glyph from the design: arc "M13.5 8 a5.5 5.5 0 1 1 -1.8 -4.1" + arrow
    // head "M13.6 1.6 v2.8 h-2.8" in a 16-unit box, drawn at 13 px.
    p.setRenderHint (QPainter::Antialiasing);
    p.translate ((width () - 13) / 2.0, (height () - 13) / 2.0);
    p.scale (13.0 / 16.0, 13.0 / 16.0);
    QPen pen (fg, 1.4);
    pen.setCapStyle (Qt::FlatCap);
    pen.setJoinStyle (Qt::MiterJoin);
    p.setPen (pen);
    p.setBrush (Qt::NoBrush);
    QPainterPath arc;
    arc.moveTo (13.5, 8.0);
    arc.arcTo (QRectF (2.5, 2.5, 11.0, 11.0), 0.0, -312.0);
    p.drawPath (arc);
    QPainterPath head;
    head.moveTo (13.6, 1.6);
    head.lineTo (13.6, 4.4);
    head.lineTo (10.8, 4.4);
    p.drawPath (head);
}

// ============================================================== RecordButton
RecordButton::RecordButton (QWidget *parent) : QPushButton (parent)
{
    setFixedHeight (32);
    setCursor (Qt::PointingHandCursor);
    setAttribute (Qt::WA_Hover);
    setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void RecordButton::setMode (Mode m, const QString &label)
{
    if (m == mode_ && label == label_)
        return;
    mode_ = m;
    label_ = label;
    setAccessibleName (label);
    update ();
}

void RecordButton::paintEvent (QPaintEvent *)
{
    QPainter p (this);
    p.setRenderHint (QPainter::TextAntialiasing);
    const bool en = isEnabled ();
    const bool down = isDown ();
    const bool hover = en && underMouse ();
    QColor bg, bd, fg, dot;
    switch (mode_)
    {
        case Mode::Recording:
            bg = down ? QColor (0x10, 0x0F, 0x0E) : (hover ? QColor (0x1F, 0x1A, 0x18) : Theme::faultBack);
            bd = Theme::fault;
            fg = Theme::fault;
            dot = Theme::fault;
            break;
        case Mode::Armed:
            bg = down ? Theme::inputFill : (hover ? QColor (0x1C, 0x1B, 0x17) : Theme::warnBack);
            bd = Theme::warn;
            fg = Theme::warn;
            dot = Theme::warn;
            break;
        case Mode::Idle:
            bg = down ? Theme::inputFill : (hover ? Theme::controlFillHover : Theme::controlFill);
            bd = hover || down ? Theme::controlEdgeHover : Theme::controlEdge;
            fg = down ? Theme::textMuted : Theme::textStrong;
            dot = Theme::textFaint;
            break;
    }
    if (!en)
    {
        bg = Theme::panel;
        bd = Theme::divider;
        fg = Theme::textFainter;
        dot = Theme::controlEdge;
    }
    if (hasFocus () && en)
        bd = Theme::action;
    p.fillRect (rect (), bg);
    outline (p, rect (), bd);

    const QFont f = Theme::mono (12.5, 600);
    const QFontMetricsF fm (f);
    const double tw = fm.horizontalAdvance (label_);
    const double total = 9.0 + 8.0 + tw;
    const double x0 = std::round ((width () - total) / 2.0);
    const double cy = height () / 2.0;
    p.fillRect (QRectF (x0, std::floor (cy - 4.5), 9, 9), dot);
    drawTextV (p, f, fg, x0 + 17.0, cy, label_);
}

// ============================================================== PortCombo
namespace
{

// The combo's line edit: unfocused, a path wider than the field is drawn
// middle-elided ("/dev/cu.usbse…DP04W4GA") instead of hard-clipped mid-glyph.
// Focused, it is a plain QLineEdit (scrolling, cursor, selection).
class ElideLineEdit : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;

protected:
    void paintEvent (QPaintEvent *e) override
    {
        const QFontMetricsF fm (font ());
        const QRectF r = QRectF (contentsRect ()).adjusted (2.0, 0.0, -2.0, 0.0); // QLineEdit's 2 px margin
        if (hasFocus () || fm.horizontalAdvance (text ()) <= r.width ())
        {
            QLineEdit::paintEvent (e);
            return;
        }
        QPainter p (this); // background is transparent (QSS): the combo paints the field
        p.setRenderHint (QPainter::TextAntialiasing);
        p.setFont (font ());
        p.setPen (palette ().color (QPalette::Text));
        p.drawText (r, Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText (text (), Qt::ElideMiddle, r.width ()));
    }
};

} // namespace

PortCombo::PortCombo (QWidget *parent) : QComboBox (parent)
{
    setFixedHeight (30); // content-box 28 + 1 px border (design)
    setLineEdit (new ElideLineEdit (this));
}

void PortCombo::setLocked (bool on)
{
    if (on == locked_)
        return;
    locked_ = on;
    if (lineEdit ())
        lineEdit ()->setReadOnly (on);
    update ();
}

void PortCombo::showPopup ()
{
    if (!locked_)
        QComboBox::showPopup ();
}

void PortCombo::wheelEvent (QWheelEvent *e)
{
    if (locked_)
        e->ignore ();
    else
        QComboBox::wheelEvent (e);
}

void PortCombo::keyPressEvent (QKeyEvent *e)
{
    if (locked_)
    {
        switch (e->key ())
        {
            case Qt::Key_Up:
            case Qt::Key_Down:
            case Qt::Key_PageUp:
            case Qt::Key_PageDown:
            case Qt::Key_Home:
            case Qt::Key_End:
            case Qt::Key_F4:
                e->ignore (); // no selection change / popup while the device is in use
                return;
            default:
                break;
        }
    }
    QComboBox::keyPressEvent (e);
}

void PortCombo::paintEvent (QPaintEvent *e)
{
    QComboBox::paintEvent (e);
    QPainter p (this);
    p.setRenderHint (QPainter::Antialiasing);
    const bool open = view () && view ()->isVisible ();
    const QColor c = !isEnabled () ? Theme::textFainter : (open ? Theme::action : Theme::textMuted);
    // 8 x 5 caret, 8 px from the right edge's padding (design): x = W-17 .. W-9
    const double cx = width () - 13.0;
    const double cy = height () / 2.0;
    QPainterPath tri;
    if (open)
    {
        tri.moveTo (cx - 4, cy + 2.5);
        tri.lineTo (cx, cy - 2.5);
        tri.lineTo (cx + 4, cy + 2.5);
    }
    else
    {
        tri.moveTo (cx - 4, cy - 2.5);
        tri.lineTo (cx, cy + 2.5);
        tri.lineTo (cx + 4, cy - 2.5);
    }
    tri.closeSubpath ();
    p.fillPath (tri, c);
}

// ============================================================== SessionBar
SessionBar::SessionBar (QWidget *parent) : QWidget (parent)
{
    setFixedHeight (Theme::sessionBarHeight);
    setAttribute (Qt::WA_OpaquePaintEvent);
}

void SessionBar::setSession (const QString &text)
{
    if (text == session_)
        return;
    session_ = text;
    update ();
}

void SessionBar::setRecording (bool on, const QString &elapsed)
{
    if (on == recOn_ && elapsed == rec_)
        return;
    recOn_ = on;
    rec_ = elapsed;
    update ();
}

void SessionBar::setClock (const QString &text)
{
    if (text == clock_)
        return;
    clock_ = text;
    update ();
}

void SessionBar::paintEvent (QPaintEvent *)
{
    QPainter p (this);
    p.setRenderHint (QPainter::TextAntialiasing);
    p.fillRect (rect (), Theme::panel);
    p.fillRect (QRect (0, height () - 1, width (), 1), Theme::edge);
    const double cy = (height () - 1) / 2.0;
    double x = 10.0;
    x += drawTextV (p, Theme::mono (11, 400, 0.02), Theme::textMuted, x, cy, QStringLiteral ("bioacq ~ $")) + 10;
    p.fillRect (QRectF (x, std::floor (cy - 7), 1, 14), Theme::edge);
    x += 1 + 10;

    const QFont f11 = Theme::mono (11);
    const QFontMetricsF fm (f11);
    double right = width () - 10.0;
    const double cw = fm.horizontalAdvance (clock_);
    drawTextV (p, f11, Theme::textDim, right - cw, cy, clock_);
    right -= cw + 10.0;
    if (recOn_)
    {
        const QFont rf = Theme::mono (11, 400, 0.06);
        const QString t = QStringLiteral ("REC ") + rec_;
        const double tw = QFontMetricsF (rf).horizontalAdvance (t);
        const double w = std::ceil (8 + 7 + 6 + tw + 8);
        const QRect chip (static_cast<int> (right - w), static_cast<int> (std::round (cy - 10)), static_cast<int> (w), 20);
        p.fillRect (chip, Theme::faultBack);
        outline (p, chip, Theme::fault);
        p.fillRect (QRectF (chip.left () + 8, std::floor (cy - 3.5), 7, 7), Theme::fault);
        drawTextV (p, rf, Theme::fault, chip.left () + 8 + 7 + 6, cy, t);
        right -= w + 10.0;
    }
    const QString s = fm.elidedText (session_, Qt::ElideRight, std::max (0.0, right - x));
    drawTextV (p, f11, Theme::textDim, x, cy, s);
}

// ============================================================== StatusStrip
StatusStrip::StatusStrip (QWidget *parent) : QWidget (parent), msgColor_ (Theme::textDim)
{
    setFixedHeight (Theme::statusBarHeight);
    setAttribute (Qt::WA_OpaquePaintEvent);
}

void StatusStrip::setSegments (const QVector<Segment> &segs)
{
    if (segs == segs_)
        return;
    segs_ = segs;
    update ();
}

void StatusStrip::setMessage (const QString &text, const QColor &color)
{
    if (text == msg_ && color == msgColor_)
        return;
    msg_ = text;
    msgColor_ = color;
    update ();
}

void StatusStrip::setRight (const QString &text)
{
    if (text == right_)
        return;
    right_ = text;
    update ();
}

void StatusStrip::paintEvent (QPaintEvent *)
{
    QPainter p (this);
    p.setRenderHint (QPainter::TextAntialiasing);
    p.fillRect (rect (), Theme::panel);
    p.fillRect (QRect (0, 0, width (), 1), Theme::edge);
    const double cy = 1.0 + (height () - 1) / 2.0;
    double x = 10.0;
    for (int i = 0; i < segs_.size (); ++i)
    {
        const Segment &s = segs_[i];
        if (i > 0)
        {
            p.fillRect (QRectF (x, std::floor (cy - 7), 1, 14), Theme::edge);
            x += 1 + 12;
        }
        if (s.led.isValid ())
        {
            p.fillRect (QRectF (x, std::floor (cy - 3), 6, 6), s.led);
            x += 6 + 6;
        }
        x += drawTextV (p, Theme::mono (10, 400, s.trackingEm), s.color, x, cy, s.text) + 12;
    }
    const QFont f10 = Theme::mono (10);
    const QFontMetricsF fm (f10);
    const double rw = fm.horizontalAdvance (right_);
    const double right = width () - 10.0;
    drawTextV (p, f10, Theme::textDim, right - rw, cy, right_);

    // "$ message" centred in the space left between the segments and the right text
    const QFont mf = Theme::mono (10, 400, 0.06);
    const QFontMetricsF fmm (mf);
    const double room = std::max (0.0, right - rw - 16.0 - x);
    const QString dollar = QStringLiteral ("$");
    const double dw = fmm.horizontalAdvance (dollar) + 6.0;
    const QString msg = fmm.elidedText (msg_, Qt::ElideRight, std::max (0.0, room - dw));
    const double total = dw + fmm.horizontalAdvance (msg);
    const double mx = x + std::max (0.0, (room - total) / 2.0);
    drawTextV (p, mf, Theme::textFaint, mx, cy, dollar);
    drawTextV (p, mf, msgColor_, mx + dw, cy, msg);
}
