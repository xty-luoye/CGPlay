#include "Application.h"
#include "ApplicationPrivate.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace cgplay {
QJsonObject MainWindow::_automationCoverage() const
{
    QJsonObject manifestCommands; QString coverageManifestPath;
    const QStringList manifestCandidates = {QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
        QStringLiteral("../../../tests/automation_coverage_manifest.json")), QDir::current().absoluteFilePath(QStringLiteral("tests/automation_coverage_manifest.json"))};
    for (const QString& candidate : manifestCandidates) {
        QFile manifestFile(candidate);
        if (!manifestFile.open(QIODevice::ReadOnly)) continue;
        const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll()); if (!document.isObject()) continue;
        manifestCommands = document.object().value(QStringLiteral("commands")).toObject();
        coverageManifestPath = QFileInfo(candidate).absoluteFilePath();
        break;
    }
    QJsonArray commandInventory, uncoveredCommands, uncoveredShortcuts;
    for (const auto& descriptor : _p->commandDescriptors) {
        const QString commandId = descriptor.id.trimmed(), shortcut = _p->commandDefaultShortcuts.value(commandId, descriptor.shortcut).trimmed();
        const QJsonObject evidence = manifestCommands.value(commandId).toObject();
        commandInventory.append(QJsonObject{{QStringLiteral("id"), commandId}, {QStringLiteral("shortcut"), shortcut}, {QStringLiteral("test"),
            evidence.value(QStringLiteral("test")).toString()}, {QStringLiteral("shortcutTest"), evidence.value(QStringLiteral("shortcutTest")).toString()}});
        if (commandId.isEmpty() || evidence.value(QStringLiteral("test")).toString().trimmed().isEmpty()) uncoveredCommands.append(commandId);
        if (!shortcut.isEmpty() && evidence.value(QStringLiteral("shortcutTest")).toString().trimmed().isEmpty())
            uncoveredShortcuts.append(QJsonObject{{QStringLiteral("id"), commandId}, {QStringLiteral("shortcut"), shortcut}});
    }
    const bool coverageComplete = !coverageManifestPath.isEmpty() && uncoveredCommands.isEmpty() &&
        uncoveredShortcuts.isEmpty() && manifestCommands.size() == _p->commandDescriptors.size();
    return QJsonObject{{QStringLiteral("complete"), coverageComplete}, {QStringLiteral("manifest"), coverageManifestPath},
        {QStringLiteral("registeredCommandCount"), _p->commandDescriptors.size()}, {QStringLiteral("manifestCommandCount"), manifestCommands.size()},
        {QStringLiteral("commands"), commandInventory}, {QStringLiteral("uncoveredCommands"), uncoveredCommands}, {QStringLiteral("uncoveredShortcuts"), uncoveredShortcuts}};
}
} // namespace cgplay
