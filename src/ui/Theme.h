#pragma once

// Design tokens of the "industrial instrumentation panel" skin (Claude Design:
// Instrument Screen + Component Sheet). Flat fills, 1 px lines, square corners,
// no shadows / gradients. Semantic hues (ok / warn / fault) carry meaning only;
// trace colours are plot ink and never used for UI state.

#include <QColor>
#include <QFont>
#include <QString>

class QApplication;
class QWidget;

namespace Theme
{

// ---- surfaces -------------------------------------------------------------
inline const QColor chassis {0x0C, 0x0D, 0x0D};
inline const QColor panel {0x12, 0x13, 0x13};
inline const QColor recess {0x08, 0x09, 0x0A};    // plot ground
inline const QColor inputFill {0x0E, 0x0F, 0x0F};
inline const QColor controlFill {0x1A, 0x1B, 0x1B};
inline const QColor controlFillHover {0x22, 0x23, 0x22};
inline const QColor divider {0x1E, 0x1F, 0x1F};    // 1 px panel divider
inline const QColor edge {0x26, 0x27, 0x26};       // 1 px panel edge
inline const QColor controlEdge {0x30, 0x31, 0x30}; // 1 px control edge
inline const QColor controlEdgeHover {0x3C, 0x3D, 0x3A};

// ---- text -----------------------------------------------------------------
inline const QColor textStrong {0xD6, 0xDA, 0xD2};
inline const QColor textBody {0xB4, 0xBA, 0xB0};
inline const QColor textMuted {0x8A, 0x90, 0x88};
inline const QColor textDim {0x6E, 0x73, 0x6B};
inline const QColor textFaint {0x56, 0x5A, 0x54};
inline const QColor textFainter {0x47, 0x4B, 0x46};

// ---- semantic (meaning only) ---------------------------------------------
inline const QColor ok {0x7F, 0xBF, 0x7F};
inline const QColor warn {0xBF, 0xA0, 0x5A};
inline const QColor fault {0xC0, 0x7A, 0x6E}; // also "recording"
inline const QColor warnBack {0x16, 0x16, 0x13};
inline const QColor faultBack {0x17, 0x15, 0x14};

// ---- primary action = inverse block ---------------------------------------
inline const QColor action {0xB4, 0xBA, 0xB0};
inline const QColor actionHover {0xC9, 0xCE, 0xC5};
inline const QColor actionPressed {0x8A, 0x90, 0x88};
inline const QColor onAction {0x08, 0x09, 0x0A};

// ---- plot ink ---------------------------------------------------------------
inline const QColor traceX {0xBF, 0xA0, 0x5A};  // solid
inline const QColor traceY {0x6F, 0xA8, 0xA8};  // dashed 7 4
inline const QColor traceZ {0xD9, 0xD5, 0xC7};  // dotted 2 4
inline const QColor traceHero {0xBF, 0xA0, 0x5A};
inline const QColor traceTemp {0xBF, 0xA0, 0x5A};
inline const QColor tracePpg {0x7F, 0xBF, 0x7F};
inline const QColor zeroLine {0x30, 0x31, 0x30};

// ---- geometry ---------------------------------------------------------------
inline constexpr int railWidth = 264;
inline constexpr int sessionBarHeight = 30;
inline constexpr int statusBarHeight = 26;
inline constexpr int gutter = 8;

// ---- fonts ------------------------------------------------------------------
// Registers the embedded JetBrains Mono + Inter faces (qrc :/fonts). Falls
// back to Menlo / the system UI font when registration fails. Idempotent.
void loadFonts ();
QString monoFamily ();
QString sansFamily ();
bool fontsEmbedded (); // true if both families came from the embedded TTFs

// Point size that renders as `px` logical pixels on the primary screen.
double ptFromPx (double px);
// JetBrains Mono / Inter at a CSS-like pixel size, weight (400/500/600/700)
// and letter-spacing in em.
QFont mono (double px, int weight = 400, double trackingEm = 0.0);
QFont sans (double px, int weight = 400, double trackingEm = 0.0);

// Text colour via the palette (labels are not styled by the global QSS).
void setTextColor (QWidget *w, const QColor &c);

// Fusion + palette + global stylesheet for the standard widgets.
void apply (QApplication &app);

} // namespace Theme
