#include "ApplicationRuntimeSupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QPixmap>
#include <QSaveFile>
#include <QWidget>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay::application_runtime {

QString defaultPhase915MediaPath()
{
    const QString candidate =
        QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath(QStringLiteral("../../../tests/media/1080p_h264.mp4"));
    return QFileInfo::exists(candidate) ? QFileInfo(candidate).absoluteFilePath() : QString();
}

QString defaultPhase14MediaPath(const QString& explicitPhase14Media, const QString& phase915Media)
{
    if (!explicitPhase14Media.isEmpty()) {
        return explicitPhase14Media;
    }
    if (!phase915Media.isEmpty()) {
        return phase915Media;
    }
    return defaultPhase915MediaPath();
}

QString deriveRuntimeDumpPath(const QString& reportPath, const QString& fallbackFileName)
{
    if (reportPath.isEmpty()) {
        return QDir::current().absoluteFilePath(fallbackFileName);
    }

    const QFileInfo info(reportPath);
    const QString baseName = info.completeBaseName().isEmpty()
        ? info.fileName()
        : info.completeBaseName();
    return info.dir().filePath(baseName + QStringLiteral(".runtime_dump.json"));
}

QJsonObject baselineScenario(const QJsonObject& report, const QString& metricsKey)
{
    return QJsonObject{
        { QStringLiteral("summary"), report.value(QStringLiteral("summary")).toObject() },
        { QStringLiteral("metrics"), report.value(metricsKey).toObject() }
    };
}

bool writeJsonObjectFile(const QString& outputPath, const QJsonObject& object)
{
    QSaveFile out(outputPath);
    QDir().mkpath(QFileInfo(out).absolutePath());
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (out.write(payload) != payload.size()) {
        out.cancelWriting();
        return false;
    }
    return out.commit();
}

bool reportHasFailures(const QJsonObject& report)
{
    return report.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() > 0;
}

QString deriveSiblingArtifactPath(const QString& outputPath, const QString& suffixWithExtension)
{
    if (outputPath.isEmpty()) {
        return QDir::current().absoluteFilePath(QStringLiteral("cgplay_artifact") + suffixWithExtension);
    }

    const QFileInfo info(outputPath);
    const QString baseName = info.completeBaseName().isEmpty()
        ? info.fileName()
        : info.completeBaseName();
    return info.dir().filePath(baseName + suffixWithExtension);
}

bool saveWindowEvidence(QWidget* widget, const QString& outputPath, QString* method)
{
    if (!widget) {
        return false;
    }
    QDir().mkpath(QFileInfo(outputPath).absolutePath());

#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(widget->winId());
    RECT rect{};
    if (hwnd && GetWindowRect(hwnd, &rect)) {
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        HDC windowDc = GetWindowDC(hwnd);
        HDC memoryDc = windowDc ? CreateCompatibleDC(windowDc) : nullptr;
        HBITMAP bitmap = memoryDc ? CreateCompatibleBitmap(windowDc, width, height) : nullptr;
        HGDIOBJ previous = bitmap ? SelectObject(memoryDc, bitmap) : nullptr;
        const BOOL printed = bitmap ? PrintWindow(hwnd, memoryDc, 0x00000002) : FALSE;
        if (previous) {
            SelectObject(memoryDc, previous);
        }

        bool saved = false;
        if (printed && width > 0 && height > 0) {
            QImage image(width, height, QImage::Format_RGB32);
            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = width;
            bitmapInfo.bmiHeader.biHeight = -height;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;
            const int copiedRows = GetDIBits(
                windowDc,
                bitmap,
                0,
                static_cast<UINT>(height),
                image.bits(),
                &bitmapInfo,
                DIB_RGB_COLORS);
            saved = copiedRows == height && image.save(outputPath, "PNG");
        }

        if (bitmap) DeleteObject(bitmap);
        if (memoryDc) DeleteDC(memoryDc);
        if (windowDc) ReleaseDC(hwnd, windowDc);
        if (saved) {
            if (method) *method = QStringLiteral("PrintWindow");
            return true;
        }
    }
#endif

    const QPixmap capture = widget->grab();
    const bool saved = !capture.isNull() && capture.save(outputPath, "PNG");
    if (saved && method) {
        *method = QStringLiteral("QWidget::grab fallback");
    }
    return saved;
}

} // namespace cgplay::application_runtime
