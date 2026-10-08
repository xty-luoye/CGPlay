#include "BackdropRenderer.h"

#include <QApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QLinearGradient>
#include <QPainter>
#include <QUrl>
#include <QWidget>

#include <algorithm>

namespace cgplay {
namespace {

struct SourceEntry
{
    qint64 modifiedMs = -1;
    qint64 fileSize = -1;
    QImage image;
};

QHash<QString, SourceEntry>& sourceCache()
{
    static QHash<QString, SourceEntry> value;
    return value;
}

QHash<QString, QPixmap>& preparedCache()
{
    static QHash<QString, QPixmap> value;
    return value;
}

QVariant applicationProperty(const char* name)
{
    return qApp ? qApp->property(name) : QVariant();
}

int applicationInt(const char* name, int fallback)
{
    bool ok = false;
    const int result = applicationProperty(name).toInt(&ok);
    return ok ? result : fallback;
}

QString applicationString(const char* name, const QString& fallback = {})
{
    const QString result = applicationProperty(name).toString().trimmed();
    return result.isEmpty() ? fallback : result;
}

QColor applicationColor(const char* name, const QColor& fallback)
{
    const QColor result(applicationProperty(name).toString());
    return result.isValid() ? result : fallback;
}

QSize boundedDecodeSize(const QSize& source, const QSize& target, const QString& fillMode)
{
    if (!source.isValid() || !target.isValid() ||
        (source.width() <= target.width() && source.height() <= target.height())) {
        return source;
    }
    const bool contain = fillMode.compare(QStringLiteral("contain"), Qt::CaseInsensitive) == 0;
    const qreal xScale = target.width() / qreal(source.width());
    const qreal yScale = target.height() / qreal(source.height());
    const qreal scale = std::min<qreal>(1.0, contain ? std::min(xScale, yScale) : std::max(xScale, yScale));
    return QSize(qMax(1, qRound(source.width() * scale)),
                 qMax(1, qRound(source.height() * scale)));
}

QRgb adjustedPixel(QRgb pixel, int brightness, int saturation)
{
    const int red = qRed(pixel);
    const int green = qGreen(pixel);
    const int blue = qBlue(pixel);
    const int gray = (red * 77 + green * 150 + blue * 29) >> 8;
    const auto channel = [gray, brightness, saturation](int value) {
        const int saturated = gray + ((value - gray) * saturation) / 100;
        return qBound(0, (saturated * brightness) / 100, 255);
    };
    return qRgba(channel(red), channel(green), channel(blue), qAlpha(pixel));
}

QImage loadSourceImage(const QString& path, const QSize& target, const QString& fillMode,
                       qint64 modifiedMs, qint64 fileSize)
{
    const QString sourceKey = QStringLiteral("%1|%2|%3|%4x%5|%6")
        .arg(path).arg(modifiedMs).arg(fileSize).arg(target.width()).arg(target.height()).arg(fillMode);
    auto& cache = sourceCache();
    const auto found = cache.constFind(sourceKey);
    if (found != cache.cend()) return found->image;

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize decodeSize = boundedDecodeSize(reader.size(), target, fillMode);
    if (decodeSize.isValid() && decodeSize != reader.size()) reader.setScaledSize(decodeSize);
    SourceEntry entry;
    entry.modifiedMs = modifiedMs;
    entry.fileSize = fileSize;
    entry.image = reader.read().convertToFormat(QImage::Format_ARGB32);
    if (cache.size() >= 6) cache.clear();
    cache.insert(sourceKey, entry);
    return entry.image;
}

void drawPixmap(QPainter& painter, const QPixmap& pixmap, const QRect& target, const QString& fillMode)
{
    if (pixmap.isNull() || target.isEmpty()) return;
    if (fillMode.compare(QStringLiteral("tile"), Qt::CaseInsensitive) == 0) {
        painter.drawTiledPixmap(target, pixmap);
        return;
    }
    const Qt::AspectRatioMode aspect = fillMode.compare(QStringLiteral("contain"), Qt::CaseInsensitive) == 0
        ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding;
    const QPixmap scaled = pixmap.size() == target.size()
        ? pixmap : pixmap.scaled(target.size(), aspect, Qt::SmoothTransformation);
    painter.drawPixmap(target.center() - QPoint(scaled.width() / 2, scaled.height() / 2), scaled);
}

} // namespace

QString normalizedBackdropPath(QString path)
{
    path = path.trimmed();
    if (path.isEmpty()) return {};
    const QUrl url(path);
    if (url.isLocalFile()) path = url.toLocalFile();
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return QFileInfo(path).absoluteFilePath();
}

QColor adjustedBackdropColor(QColor color, int brightness, int saturation)
{
    if (!color.isValid()) return color;
    return QColor::fromRgba(adjustedPixel(
        color.rgba(), qBound(0, brightness, 200), qBound(0, saturation, 200)));
}

QPixmap preparedBackdropPixmap(const QString& rawPath, int brightness, int saturation, int blurRadius,
                               const QSize& targetUpperBound, const QString& fillMode)
{
    const QString path = normalizedBackdropPath(rawPath);
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile()) return {};
    const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();
    const qint64 fileSize = info.size();
    const QSize target(qMax(1, targetUpperBound.width()), qMax(1, targetUpperBound.height()));
    brightness = qBound(0, brightness, 200);
    saturation = qBound(0, saturation, 200);
    blurRadius = qBound(0, blurRadius, 64);
    const QString key = QStringLiteral("%1|%2|%3|%4|%5|%6|%7x%8|%9")
        .arg(path).arg(modifiedMs).arg(fileSize).arg(brightness).arg(saturation).arg(blurRadius)
        .arg(target.width()).arg(target.height()).arg(fillMode.toLower());
    auto& cache = preparedCache();
    const auto found = cache.constFind(key);
    if (found != cache.cend()) return *found;

    QImage image = loadSourceImage(path, target, fillMode, modifiedMs, fileSize);
    if (image.isNull()) return {};
    if (brightness != 100 || saturation != 100) {
        for (int y = 0; y < image.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                row[x] = adjustedPixel(row[x], brightness, saturation);
            }
        }
    }
    if (blurRadius > 0 && image.width() > 2 && image.height() > 2) {
        const QSize originalSize = image.size();
        const int divisor = qBound(2, 1 + blurRadius / 3, 12);
        image = image.scaled(qMax(1, image.width() / divisor), qMax(1, image.height() / divisor),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                     .scaled(originalSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    const QPixmap result = QPixmap::fromImage(image);
    if (cache.size() >= 12) cache.clear();
    cache.insert(key, result);
    return result;
}

bool drawApplicationBackdrop(QPainter& painter, const QRect& target, const QWidget* surface,
                             const BackdropRenderOptions& options)
{
    if (target.isEmpty()) return false;
    const QWidget* root = surface && surface->window() ? surface->window() : surface;
    const QSize rootSize = root ? root->size() : target.size();
    QSize cacheTarget = rootSize;
    if (qApp) {
        for (QWidget* topLevel : qApp->topLevelWidgets()) {
            if (!topLevel || !topLevel->isVisible()) continue;
            cacheTarget.setWidth(qMax(cacheTarget.width(), topLevel->width()));
            cacheTarget.setHeight(qMax(cacheTarget.height(), topLevel->height()));
        }
    }
    const QPoint offset = surface && root ? surface->mapTo(root, QPoint(0, 0)) : QPoint();
    const QRect alignedTarget(QPoint(0, 0), rootSize);
    const int brightness = qBound(0, applicationInt("cgplay.backgroundBrightness", 100), 200);
    const int saturation = qBound(0, applicationInt("cgplay.backgroundSaturation", 100), 200);
    const int opacity = qBound(options.minimumOpacity, applicationInt("cgplay.backgroundOpacity", 100), 100);
    const QString type = applicationString("cgplay.backgroundType", QStringLiteral("solid")).toLower();
    const QString fillMode = applicationString("cgplay.backgroundFillMode",
                                                applicationString("cgplay.fillMode", QStringLiteral("cover")));
    QString path = applicationString("cgplay.backgroundImage");
    if (path.isEmpty()) path = applicationString("cgplay.texturePath");

    bool drewImage = false;
    painter.save();
    painter.translate(-offset);
    if ((type == QStringLiteral("image") || type == QStringLiteral("texture")) && !path.isEmpty()) {
        const QPixmap pixmap = preparedBackdropPixmap(path, brightness, saturation,
            applicationInt("cgplay.backgroundBlurRadius", 0), cacheTarget, fillMode);
        if (!pixmap.isNull()) {
            painter.fillRect(alignedTarget, options.underlay);
            painter.setOpacity(opacity / 100.0);
            drawPixmap(painter, pixmap, alignedTarget, fillMode);
            painter.setOpacity(1.0);
            drewImage = true;
        }
    }
    if (!drewImage && options.drawFallback) {
        const QColor primary = adjustedBackdropColor(
            applicationColor("cgplay.backgroundColor", options.underlay), brightness, saturation);
        const QColor secondary = adjustedBackdropColor(
            applicationColor("cgplay.backgroundSecondary", primary.darker(112)), brightness, saturation);
        if (type == QStringLiteral("solid")) {
            painter.fillRect(alignedTarget, primary);
        } else {
            QLinearGradient gradient(alignedTarget.topLeft(), alignedTarget.bottomLeft());
            gradient.setColorAt(0.0, primary);
            gradient.setColorAt(0.55, secondary);
            gradient.setColorAt(1.0, secondary.darker(108));
            painter.fillRect(alignedTarget, gradient);
        }
    }
    if ((drewImage || options.drawFallback) && options.readabilityWashAlpha > 0) {
        painter.fillRect(alignedTarget, QColor(0, 0, 0, qBound(0, options.readabilityWashAlpha, 255)));
    }
    if ((drewImage || options.drawFallback) && options.drawVignette) {
        const int strength = qBound(0, applicationInt("cgplay.backgroundVignette", 0), 100);
        if (strength > 0) {
            const int alpha = qBound(0, qRound(strength * 0.9), 100);
            QLinearGradient vignette(0, 0, alignedTarget.width(), 0);
            vignette.setColorAt(0.0, QColor(0, 0, 0, alpha));
            vignette.setColorAt(0.18, QColor(0, 0, 0, 0));
            vignette.setColorAt(0.82, QColor(0, 0, 0, 0));
            vignette.setColorAt(1.0, QColor(0, 0, 0, alpha));
            painter.fillRect(alignedTarget, vignette);
        }
    }
    painter.restore();
    if (qApp && (type == QStringLiteral("image") || type == QStringLiteral("texture"))) {
        qApp->setProperty("cgplay.backgroundImageValid", drewImage);
    }
    return drewImage;
}

void clearBackdropCache()
{
    sourceCache().clear();
    preparedCache().clear();
}

} // namespace cgplay
