#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPalette>
#include <QScreen>
#include <QStyleFactory>
#include <QWidget>

#include <cstdio>

namespace Theme
{

namespace
{

QString g_mono;
QString g_sans;
bool g_loaded = false;
bool g_embedded = false;

// Registers every TTF of one family; returns the family name Qt reports for
// it (typographic family, e.g. "JetBrains Mono"), or "" if none registered.
QString registerFamily (const char *const *files, int count)
{
    QString family;
    for (int i = 0; i < count; ++i)
    {
        const int id = QFontDatabase::addApplicationFont (QString::fromLatin1 (files[i]));
        if (id < 0)
            continue;
        const QStringList fams = QFontDatabase::applicationFontFamilies (id);
        if (family.isEmpty () && !fams.isEmpty ())
            family = fams.front ();
    }
    return family;
}

QString hex (const QColor &c)
{
    return c.name (QColor::HexRgb);
}

} // namespace

void loadFonts ()
{
    if (g_loaded)
        return;
    g_loaded = true;
    static const char *const monoFiles[] = {":/fonts/JetBrainsMono-Regular.ttf",
        ":/fonts/JetBrainsMono-Medium.ttf", ":/fonts/JetBrainsMono-SemiBold.ttf",
        ":/fonts/JetBrainsMono-Bold.ttf"};
    static const char *const sansFiles[] = {":/fonts/Inter-Regular.ttf", ":/fonts/Inter-Medium.ttf",
        ":/fonts/Inter-SemiBold.ttf", ":/fonts/Inter-Bold.ttf"};
    g_mono = registerFamily (monoFiles, 4);
    g_sans = registerFamily (sansFiles, 4);
    g_embedded = !g_mono.isEmpty () && !g_sans.isEmpty ();
    if (g_mono.isEmpty ())
    {
        std::fprintf (stderr, "bioacq: JetBrains Mono could not be registered, using Menlo\n");
        g_mono = QStringLiteral ("Menlo");
    }
    if (g_sans.isEmpty ())
    {
        std::fprintf (stderr, "bioacq: Inter could not be registered, using the system font\n");
        g_sans = QGuiApplication::font ().family ();
    }
}

QString monoFamily ()
{
    loadFonts ();
    return g_mono;
}

QString sansFamily ()
{
    loadFonts ();
    return g_sans;
}

bool fontsEmbedded ()
{
    loadFonts ();
    return g_embedded;
}

double ptFromPx (double px)
{
    double dpi = 72.0;
    if (const QScreen *s = QGuiApplication::primaryScreen ())
        dpi = s->logicalDotsPerInchY ();
    if (!(dpi > 0.0))
        dpi = 72.0;
    return px * 72.0 / dpi;
}

namespace
{

QFont makeFont (const QString &family, double px, int weight, double trackingEm, bool mono)
{
    QFont f (family);
    f.setPointSizeF (ptFromPx (px));
    f.setWeight (static_cast<QFont::Weight> (weight));
    if (mono)
        f.setStyleHint (QFont::Monospace, QFont::PreferDefault);
    f.setKerning (!mono);
    if (trackingEm != 0.0)
        f.setLetterSpacing (QFont::AbsoluteSpacing, trackingEm * px);
    return f;
}

} // namespace

QFont mono (double px, int weight, double trackingEm)
{
    return makeFont (monoFamily (), px, weight, trackingEm, true);
}

QFont sans (double px, int weight, double trackingEm)
{
    QFont f = makeFont (sansFamily (), px, weight, trackingEm, false);
    f.setFeature ("tnum", 1); // tabular figures where Inter shows numbers
    return f;
}

void setTextColor (QWidget *w, const QColor &c)
{
    QPalette p = w->palette ();
    p.setColor (QPalette::WindowText, c);
    p.setColor (QPalette::Text, c);
    p.setColor (QPalette::ButtonText, c);
    w->setPalette (p);
}

void apply (QApplication &app)
{
    loadFonts ();
    app.setStyle (QStyleFactory::create (QStringLiteral ("Fusion")));

    QPalette p;
    p.setColor (QPalette::Window, panel);
    p.setColor (QPalette::WindowText, textBody);
    p.setColor (QPalette::Base, inputFill);
    p.setColor (QPalette::AlternateBase, panel);
    p.setColor (QPalette::ToolTipBase, panel);
    p.setColor (QPalette::ToolTipText, textBody);
    p.setColor (QPalette::PlaceholderText, textFaint);
    p.setColor (QPalette::Text, textStrong);
    p.setColor (QPalette::Button, controlFill);
    p.setColor (QPalette::ButtonText, textBody);
    p.setColor (QPalette::BrightText, fault);
    p.setColor (QPalette::Highlight, action);
    p.setColor (QPalette::HighlightedText, onAction);
    p.setColor (QPalette::Link, textStrong);
    p.setColor (QPalette::Mid, controlEdge);
    p.setColor (QPalette::Dark, recess);
    p.setColor (QPalette::Disabled, QPalette::Text, textDim);
    p.setColor (QPalette::Disabled, QPalette::ButtonText, textFainter);
    p.setColor (QPalette::Disabled, QPalette::WindowText, textFainter);
    app.setPalette (p);
    app.setFont (sans (11.5));

    auto pt = [] (double px) { return QString::number (ptFromPx (px), 'f', 2) + QStringLiteral ("pt"); };
    const QString m = QStringLiteral ("\"%1\"").arg (monoFamily ());

    QString q;
    // ---- tooltips / scroll areas / scrollbars (thin, #303130 on #121313)
    q += QStringLiteral ("QToolTip { background: %1; color: %2; border: 1px solid %3; padding: 4px 6px;"
                         " font-family: %4; font-size: %5; }\n")
             .arg (hex (panel), hex (textBody), hex (controlEdge), m, pt (10.5));
    q += QStringLiteral ("QScrollArea { background: transparent; border: 0; }\n"
                         "QScrollArea > QWidget > QWidget#railContent { background: %1; }\n")
             .arg (hex (panel));
    q += QStringLiteral (
        "QScrollBar:vertical { background: %1; width: 6px; margin: 0; border: 0; }\n"
        "QScrollBar::handle:vertical { background: %2; min-height: 24px; border: 0; }\n"
        "QScrollBar::handle:vertical:hover { background: %3; }\n"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; border: 0; background: none; }\n"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }\n"
        "QScrollBar:horizontal { background: %1; height: 6px; margin: 0; border: 0; }\n"
        "QScrollBar::handle:horizontal { background: %2; min-width: 24px; border: 0; }\n"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; border: 0; background: none; }\n"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }\n")
             .arg (hex (panel), hex (controlEdge), hex (controlEdgeHover));

    // ---- structural frames
    q += QStringLiteral ("QFrame#rail { background: %1; border: 0; border-right: 1px solid %2; }\n"
                         "QFrame#module { background: %1; border: 0; border-bottom: 1px solid %3; }\n"
                         "QFrame#recordModule { background: %1; border: 0; border-top: 1px solid %2; }\n"
                         "QWidget#mainArea { background: %4; }\n"
                         "QFrame#discoverBox { background: %5; border: 1px solid %6; }\n"
                         "QFrame#errorBox { background: %7; border: 1px solid %8; }\n"
                         "QFrame#boxRule { background: %9; border: 0; }\n"
                         "QFrame#stepper { background: %10; border: 1px solid %11; }\n"
                         "QLabel#keycap { border: 1px solid %11; padding: 1px 5px; }\n")
             .arg (hex (panel), hex (edge), hex (divider), hex (chassis), hex (warnBack), hex (warn),
                 hex (faultBack), hex (fault), hex (controlEdge))
             .arg (hex (inputFill), hex (controlEdge));

    // ---- buttons: square, 1 px, flat; variants via the "variant" property
    q += QStringLiteral ("QPushButton { border-radius: 0; border: 1px solid %1; background: %2; color: %3;"
                         " font-family: %4; font-size: %5; font-weight: 600; padding: 0 10px; }\n"
                         "QPushButton:focus { outline: none; }\n"
                         "QPushButton[size=\"sm\"] { font-size: %6; }\n"
                         "QPushButton[size=\"lg\"] { font-size: %7; }\n")
             .arg (hex (controlEdge), hex (controlFill), hex (textBody), m, pt (12), pt (11.5), pt (12.5));
    struct V
    {
        const char *name;
        QColor bg, bd, fg, hbg, hbd, hfg, pbg, pbd, pfg, dbg, dbd, dfg;
    };
    const V variants[] = {
        {"primary", action, action, onAction, actionHover, actionHover, onAction, actionPressed, actionPressed,
            onAction, panel, divider, textFaint},
        {"secondary", controlFill, controlEdge, textBody, controlFillHover, controlEdgeHover, textStrong, inputFill,
            controlEdgeHover, textMuted, panel, divider, textFainter},
        {"ghost", QColor (0, 0, 0, 0), divider, textMuted, panel, controlEdge, textBody, inputFill, controlEdge,
            textMuted, QColor (0, 0, 0, 0), divider, textFainter},
        {"destructive", faultBack, fault, fault, QColor (0x1F, 0x1A, 0x18), fault, QColor (0xD0, 0x8C, 0x80),
            QColor (0x10, 0x0F, 0x0E), fault, fault, panel, divider, textFainter},
        {"record", controlFill, controlEdge, textStrong, controlFillHover, controlEdgeHover, textStrong, inputFill,
            controlEdgeHover, textMuted, panel, divider, textFainter},
        // "[ connecting… ]": a busy primary, shown disabled
        {"busy", panel, controlEdge, textDim, panel, controlEdge, textDim, panel, controlEdge, textDim, panel,
            controlEdge, textDim},
    };
    auto rgba = [] (const QColor &c) {
        return c.alpha () == 0 ? QStringLiteral ("transparent") : c.name (QColor::HexRgb);
    };
    for (const V &v : variants)
    {
        const QString sel = QStringLiteral ("QPushButton[variant=\"%1\"]").arg (QLatin1String (v.name));
        q += QStringLiteral ("%1 { background: %2; border-color: %3; color: %4; }\n"
                             "%1:hover { background: %5; border-color: %6; color: %7; }\n"
                             "%1:pressed { background: %8; border-color: %9; color: %10; }\n"
                             "%1:disabled { background: %11; border-color: %12; color: %13; }\n")
                 .arg (sel, rgba (v.bg), rgba (v.bd), rgba (v.fg), rgba (v.hbg), rgba (v.hbd), rgba (v.hfg),
                     rgba (v.pbg), rgba (v.pbd))
                 .arg (rgba (v.pfg), rgba (v.dbg), rgba (v.dbd), rgba (v.dfg));
    }
    // stepper halves and the square icon button
    q += QStringLiteral (
        "QPushButton[variant=\"step\"] { border: 0; background: transparent; color: %1; padding: 0;"
        " font-size: %2; font-weight: 400; }\n"
        "QPushButton[variant=\"step\"][side=\"left\"] { border-right: 1px solid %3; }\n"
        "QPushButton[variant=\"step\"][side=\"right\"] { border-left: 1px solid %3; }\n"
        "QPushButton[variant=\"step\"]:hover { background: %4; color: %5; }\n"
        "QPushButton[variant=\"step\"]:pressed { background: %6; }\n"
        "QPushButton[variant=\"step\"]:disabled { color: %7; }\n"
        "QPushButton[variant=\"icon\"] { padding: 0; }\n")
             .arg (hex (textBody), pt (13), hex (controlEdge), hex (controlFill), hex (textStrong),
                 hex (panel), hex (textFainter));

    // ---- text inputs, spin box, combo box + popup
    q += QStringLiteral (
        "QLineEdit, QSpinBox, QComboBox { background: %1; border: 1px solid %2; border-radius: 0; color: %3;"
        " font-family: %4; font-size: %5; padding: 0 6px; selection-background-color: %6;"
        " selection-color: %7; }\n"
        "QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: %6; }\n"
        "QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled { background: %8; border-color: %9;"
        " color: %10; }\n"
        "QSpinBox::up-button, QSpinBox::down-button { width: 0; border: 0; }\n"
        // text origin 1 + 6 + QLineEdit's 2 px margin = 9 px (design: border 1 +
        // padding 8); the 20 px drop-down holds the 8 px caret + 8 px padding
        // and leaves the design's 6 px gap before it
        "QComboBox { padding-left: 6px; padding-right: 0; }\n"
        "QComboBox::drop-down { border: 0; width: 20px; subcontrol-origin: padding;"
        " subcontrol-position: center right; }\n"
        "QComboBox::down-arrow { image: none; width: 0; height: 0; border: 0; }\n"
        "QComboBox QLineEdit { border: 0; padding: 0; background: transparent; }\n"
        "QComboBox QAbstractItemView { background: %1; border: 1px solid %6; color: %11; outline: 0;"
        " padding: 0; margin: 0; font-family: %4; font-size: %5; selection-background-color: %6;"
        " selection-color: %7; }\n"
        "QComboBox QAbstractItemView::item { padding: 5px 8px; min-height: 16px; border: 0; }\n"
        "QComboBox QAbstractItemView::item:selected { background: %6; color: %7; }\n"
        "QComboBox QAbstractItemView::item:disabled { color: %12; background: %1; }\n")
             .arg (hex (inputFill), hex (controlEdge), hex (textStrong), m, pt (11), hex (action), hex (onAction),
                 hex (panel), hex (divider))
             .arg (hex (textDim), hex (textBody), hex (textFaint));

    app.setStyleSheet (q);
}

} // namespace Theme
