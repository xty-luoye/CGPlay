#pragma once

#include <QJsonObject>
#include <QString>

namespace cgplay {

// Plain value copied on the GUI thread before starting the export worker.
// It owns no QObject and does not read mutable UI state during decoding.
struct ColorTransformSnapshot
{
    bool enabled = false;
    QString configPath;
    QString input;
    QString display;
    QString view;
    QString look;
    float exposure = 0.F;
    float gamma = 1.F;
    QString configCacheId;

    QString outputEncoding() const
    {
        if (!enabled) return QStringLiteral("source");
        // The current exporter is SDR. Do not label arbitrary HDR/P3 display
        // transforms as BT.709 merely because the encoder accepts 8-bit RGB.
        const QString name = display.trimmed();
        for (const QString& supported : {QStringLiteral("sRGB"), QStringLiteral("sRGB - Display")})
            if (name.compare(supported, Qt::CaseInsensitive) == 0) return QStringLiteral("srgb");
        for (const QString& supported : {QStringLiteral("Rec.1886 Rec.709 - Display"),
                 QStringLiteral("Rec.709 - Display"), QStringLiteral("Rec.709")})
            if (name.compare(supported, Qt::CaseInsensitive) == 0) return QStringLiteral("rec709");
        return QStringLiteral("unsupported");
    }

    QJsonObject toJson() const
    {
        return {
            {QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("mode"), enabled ? QStringLiteral("ocio") : QStringLiteral("bypass")},
            {QStringLiteral("configPath"), configPath},
            {QStringLiteral("input"), input},
            {QStringLiteral("display"), display},
            {QStringLiteral("view"), view},
            {QStringLiteral("look"), look},
            {QStringLiteral("exposure"), exposure},
            {QStringLiteral("gamma"), gamma},
            {QStringLiteral("sourceRole"), QStringLiteral("color")},
            {QStringLiteral("configCacheId"), configCacheId},
            {QStringLiteral("outputEncoding"), outputEncoding()},
        };
    }
};

} // namespace cgplay
