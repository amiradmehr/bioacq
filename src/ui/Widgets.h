#pragma once

// Small custom widgets for the instrument-panel skin where QSS cannot express
// the design: square toggle switch, status chip with LED, meter bar (rail
// headroom / discovery progress), banner, stepper, icon + record buttons, the
// port combo's caret, and the painted session / status bars.

#include <QAbstractButton>
#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QPushButton>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cmath>

class QLabel;

// Square LED (7 px in panels, 6 px in chips).
class Led : public QWidget
{
public:
    explicit Led (int size = 7, QWidget *parent = nullptr);
    void setColor (const QColor &c);
    QSize sizeHint () const override
    {
        return QSize (size_, size_);
    }

protected:
    void paintEvent (QPaintEvent *) override;

private:
    int size_;
    QColor color_;
};

// Outlined chip: [LED  TEXT] in one semantic colour (optional tinted back).
class StatusChip : public QWidget
{
public:
    // 19 px: the design's 17 px content box + 1 px border (content-box sizing)
    StatusChip (int height = 19, double fontPx = 9.5, int ledSize = 6, int padX = 6, QWidget *parent = nullptr);
    void setState (const QString &text, const QColor &color, const QColor &fill = QColor ());
    QSize sizeHint () const override;
    QSize minimumSizeHint () const override
    {
        return sizeHint ();
    }

protected:
    void paintEvent (QPaintEvent *) override;

private:
    int h_, led_, padX_;
    QFont font_;
    QString text_;
    QColor color_, fill_;
};

// Square toggle with an Inter label (left or right of the switch). 34 x 21:
// the design's 28 x 15 content box + 2 px padding + 1 px border, 11 px knob.
class ToggleSwitch : public QAbstractButton
{
public:
    explicit ToggleSwitch (const QString &text, bool labelLeft = false, QWidget *parent = nullptr);
    QSize sizeHint () const override;
    QSize minimumSizeHint () const override;

protected:
    void paintEvent (QPaintEvent *) override;
    bool hitButton (const QPoint &) const override
    {
        return true;
    }

private:
    bool labelLeft_;
};

// 1 px framed bar with 1 px inner padding and a flat fill (headroom, progress).
class MeterBar : public QWidget
{
public:
    MeterBar (int height, const QColor &border, QWidget *parent = nullptr);
    void setValue (double fraction, const QColor &color); // NaN = empty
    QSize sizeHint () const override
    {
        return QSize (100, h_);
    }

protected:
    void paintEvent (QPaintEvent *) override;

private:
    int h_;
    QColor border_, color_;
    double frac_ = std::nan ("");
};

// Main-area banner: [LED TAG | text ........ meta], 32 px, outlined + tinted.
class Banner : public QWidget
{
public:
    explicit Banner (QWidget *parent = nullptr);
    void setContent (const QColor &color, const QColor &back, const QString &tag, const QString &text,
        const QString &meta);
    QSize sizeHint () const override
    {
        return QSize (400, 32);
    }

protected:
    void paintEvent (QPaintEvent *) override;

private:
    QColor color_, back_;
    QString tag_, text_, meta_;
};

// [ - | 10.0 s | + ] integer seconds stepper.
class Stepper : public QFrame
{
    Q_OBJECT

public:
    Stepper (int minimum, int maximum, int value, QWidget *parent = nullptr);
    int value () const
    {
        return value_;
    }
    void setValue (int v);

signals:
    void valueChanged (int value);

protected:
    void wheelEvent (QWheelEvent *e) override;
    void keyPressEvent (QKeyEvent *e) override;

private:
    void refresh ();
    int min_, max_, value_;
    QPushButton *minus_ = nullptr;
    QPushButton *plus_ = nullptr;
    QLabel *label_ = nullptr;
};

// 28 x 28 secondary button with the design's rescan glyph. Locked (device in
// use): inert, but drawn at rest like the design's live screen.
class RescanButton : public QPushButton
{
public:
    explicit RescanButton (QWidget *parent = nullptr);
    void setLocked (bool on);
    bool isLocked () const
    {
        return locked_;
    }

protected:
    void paintEvent (QPaintEvent *) override;

private:
    bool locked_ = false;
};

// Record module button: [ ■  label ] -- idle / armed / recording.
class RecordButton : public QPushButton
{
public:
    enum class Mode
    {
        Idle,
        Armed,
        Recording
    };
    explicit RecordButton (QWidget *parent = nullptr);
    void setMode (Mode m, const QString &label);

protected:
    void paintEvent (QPaintEvent *) override;

private:
    Mode mode_ = Mode::Idle;
    QString label_;
};

// Editable 30 px combo with the design's filled caret. While unfocused, a
// path too long for the field is middle-elided instead of hard-clipped.
// Locked (device in use): read-only, no popup / wheel / arrow-key changes,
// but drawn at rest like the design's live screen.
class PortCombo : public QComboBox
{
public:
    explicit PortCombo (QWidget *parent = nullptr);
    void setLocked (bool on);
    bool isLocked () const
    {
        return locked_;
    }
    void showPopup () override;

protected:
    void paintEvent (QPaintEvent *e) override;
    void wheelEvent (QWheelEvent *e) override;
    void keyPressEvent (QKeyEvent *e) override;

private:
    bool locked_ = false;
};

// 30 px top bar: "bioacq ~ $ | session label ...... [REC hh:mm:ss] clock".
class SessionBar : public QWidget
{
public:
    explicit SessionBar (QWidget *parent = nullptr);
    void setSession (const QString &text);
    void setRecording (bool on, const QString &elapsed);
    void setClock (const QString &text);

protected:
    void paintEvent (QPaintEvent *) override;

private:
    QString session_, rec_, clock_;
    bool recOn_ = false;
};

// 26 px bottom bar: device segments | ... "$ message" ... | right text.
class StatusStrip : public QWidget
{
public:
    struct Segment
    {
        QString text;
        QColor color;
        QColor led; // invalid = no LED
        double trackingEm = 0.0;
        bool operator== (const Segment &o) const
        {
            return text == o.text && color == o.color && led == o.led && trackingEm == o.trackingEm;
        }
    };
    explicit StatusStrip (QWidget *parent = nullptr);
    void setSegments (const QVector<Segment> &segs);
    void setMessage (const QString &text, const QColor &color);
    void setRight (const QString &text);

protected:
    void paintEvent (QPaintEvent *) override;

private:
    QVector<Segment> segs_;
    QString msg_, right_;
    QColor msgColor_;
};
