#pragma once

#include <QJsonObject>
#include <QString>

class QWidget;

namespace cgplay::application_runtime {

QString defaultPhase915MediaPath();
QString defaultPhase14MediaPath(const QString& explicitPhase14Media, const QString& phase915Media);
QString deriveRuntimeDumpPath(const QString& reportPath, const QString& fallbackFileName);
QJsonObject baselineScenario(
    const QJsonObject& report,
    const QString& metricsKey = QStringLiteral("metrics"));
bool writeJsonObjectFile(const QString& outputPath, const QJsonObject& object);
bool reportHasFailures(const QJsonObject& report);
QString deriveSiblingArtifactPath(const QString& outputPath, const QString& suffixWithExtension);
bool saveWindowEvidence(QWidget* widget, const QString& outputPath, QString* method);

} // namespace cgplay::application_runtime
