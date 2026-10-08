// CGPlay - Professional Review Player
// main.cpp

#include "app/Application.h"
#include "core/CrashHandler.h"
#include "core/PlaybackLogger.h"
#include "services/platform/WindowsFileAssociations.h"

#include <QSurfaceFormat>
#include <QApplication>
#include <QIcon>
#include <QElapsedTimer>
#include <cstdio>
#include <cstring>

int main(int argc, char* argv[])
{
    bool deployDefaultAssociations = false;
    bool removeDefaultAssociations = false;
    for (int i = 1; i < argc; ++i) {
        deployDefaultAssociations = deployDefaultAssociations ||
            std::strcmp(argv[i], "--cgplay-deploy-default-associations") == 0;
        removeDefaultAssociations = removeDefaultAssociations ||
            std::strcmp(argv[i], "--cgplay-remove-default-associations") == 0;
    }
    if (deployDefaultAssociations || removeDefaultAssociations) {
        QCoreApplication worker(argc, argv);
        QString error;
        const bool ok = deployDefaultAssociations
            ? cgplay::WindowsFileAssociations::runDeviceDefaultAssociationsWorker(&error)
            : cgplay::WindowsFileAssociations::runRemoveDeviceDefaultAssociationsWorker(&error);
        if (!ok) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            std::fflush(stderr);
        }
        return ok ? 0 : 1;
    }

    QElapsedTimer startupTimer;
    startupTimer.start();
    bool startupTiming = false;
    for (int i = 1; i < argc; ++i) {
        startupTiming = startupTiming || std::strcmp(argv[i], "--overlay-debug") == 0;
    }
    if (startupTiming) {
        std::fprintf(stderr, "[StartupTiming] {\"scope\":\"process\",\"stage\":\"entry\",\"elapsedMs\":0}\n");
        std::fflush(stderr);
    }
    // ── High-DPI ──────────────────────────────────────────────────────────────
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // ── OpenGL surface format ─────────────────────────────────────────────────
    QSurfaceFormat fmt;
    fmt.setVersion(4, 1);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8);
    fmt.setSwapInterval(1); // vsync
    QSurfaceFormat::setDefaultFormat(fmt);

    // ── Application ───────────────────────────────────────────────────────────
    cgplay::Application app(argc, argv);
    if (startupTiming) {
        std::fprintf(stderr, "[StartupTiming] {\"scope\":\"process\",\"stage\":\"applicationConstructed\",\"elapsedMs\":%lld}\n",
            static_cast<long long>(startupTimer.elapsed()));
        std::fflush(stderr);
    }
    app.setWindowIcon(QIcon(QStringLiteral(":/cgplay/CGPlay.png")));

    // ── Crash & debug logging (install AFTER QApplication is ready) ────────────
    cgplay::CrashHandler::install();

    // ── Playback session logger ───────────────────────────────────────────────
    cgplay::PlaybackLogger::instance().startSession();

    int ret = app.run();

    // ── Cleanup ───────────────────────────────────────────────────────────────
    cgplay::PlaybackLogger::instance().endSession();
    cgplay::CrashHandler::shutdown();

    return ret;
}
