#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

namespace cgplay {

// Per-user Windows Shell integration used by the settings UI.  The helper
// deliberately never touches UserChoice: Windows owns that key and changing
// it would make the association unreliable and unsafe across Windows builds.
class WindowsFileAssociations
{
public:
    static QStringList supportedVideoExtensions();
    static QSet<QString> selectedVideoExtensions();
    // Returns extensions that Windows currently resolves to the installed
    // CGPlay command.  This is intentionally different from "registered":
    // an existing UserChoice or stale OpenWith ProgID can win over the
    // Classes registration.
    static QSet<QString> effectiveDefaultVideoExtensions();
    static bool applyVideoExtensions(const QSet<QString>& extensions, QString* error = nullptr);
    // Launch an elevated, non-GUI worker that installs the Windows device-level
    // default associations policy.  The policy is intentionally separate from
    // per-user registration so the user sees an explicit UAC consent prompt.
    static bool requestDeviceDefaultAssociations(QString* error = nullptr);
    static bool requestRemoveDeviceDefaultAssociations(QString* error = nullptr);
    // Entry points used by the elevated worker path in main.cpp.
    static bool runDeviceDefaultAssociationsWorker(QString* error = nullptr);
    static bool runRemoveDeviceDefaultAssociationsWorker(QString* error = nullptr);
    static QString defaultAssociationsXml();
    static bool registerThumbnailProvider(QString* error = nullptr);
    static QString thumbnailProviderClsid();
    static QString thumbnailProviderPath();
};

} // namespace cgplay
