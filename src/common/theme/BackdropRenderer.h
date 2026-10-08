#pragma once

#include <QColor>
#include <QPixmap>
#include <QSize>
#include <QString>

QT_BEGIN_NAMESPACE
class QPainter;
class QWidget;
QT_END_NAMESPACE

#if defined(CGPLAY_THEME_LIBRARY)
#define CGPLAY_THEME_EXPORT Q_DECL_EXPORT
#else
#define CGPLAY_THEME_EXPORT Q_DECL_IMPORT
#endif

namespace cgplay {

struct BackdropRenderOptions
{
    QColor underlay = QColor(QStringLiteral("#050505"));
    int minimumOpacity = 20;
    int readabilityWashAlpha = 24;
    bool drawFallback = true;
    bool drawVignette = true;
};

CGPLAY_THEME_EXPORT QString normalizedBackdropPath(QString path);
CGPLAY_THEME_EXPORT QColor adjustedBackdropColor(QColor color, int brightness, int saturation);

// The returned pixmap is implicitly shared. Source decoding and effect processing
// are cached across the player and plugin modules by the CGPlayTheme DLL.
CGPLAY_THEME_EXPORT QPixmap preparedBackdropPixmap(
    const QString& path,
    int brightness,
    int saturation,
    int blurRadius,
    const QSize& targetUpperBound,
    const QString& fillMode);

CGPLAY_THEME_EXPORT bool drawApplicationBackdrop(
    QPainter& painter,
    const QRect& target,
    const QWidget* surface,
    const BackdropRenderOptions& options = {});

CGPLAY_THEME_EXPORT void clearBackdropCache();

} // namespace cgplay

