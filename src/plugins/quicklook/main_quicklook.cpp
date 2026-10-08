#include "QuickLookApp.h"
#include "component/ComponentManager.h"

#include <QApplication>
#include <QIcon>
#include <QSharedMemory>
#include <QSurfaceFormat>

#include <windows.h>

int main(int argc, char* argv[])
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QSurfaceFormat fmt;
    fmt.setVersion(4, 1);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8);
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName("CGPlayQuickLook");
    app.setWindowIcon(QIcon(QStringLiteral(":/cgplay/CGPlay.png")));

    cgplay::ComponentManager& componentManager = cgplay::ComponentManager::instance();
    const QString envManifest = QString::fromLocal8Bit(
        qgetenv("CGPLAY_MANIFEST_URL")).trimmed();
    if (!envManifest.isEmpty()) {
        componentManager.setManifestUrl(envManifest);
    }
    QString manifestError;
    componentManager.refreshRemoteManifest(&manifestError);

    QSharedMemory sharedMemory(QStringLiteral("CGPlayQuickLookSingleton"));
    if (!sharedMemory.create(1)) {
        return 0;
    }

    cgplay::quicklook::QuickLookApp quickLook;
    if (!quickLook.start()) {
        if (SUCCEEDED(comResult)) {
            CoUninitialize();
        }
        return 1;
    }

    const int exitCode = app.exec();
    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return exitCode;
}
