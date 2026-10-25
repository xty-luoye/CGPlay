// The header is generated from the storage declaration and lastError() method
// extracted verbatim from the applied ReviewExport.cpp, with its source digest.
#include "extracted_error_storage.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <atomic>
#include <future>
#include <thread>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 2) return 2;
    std::atomic_int ready{0};
    const auto run = [&](const QString& text) {
        cgplay::s_lastExportError = text;
        ready.fetch_add(1);
        while (ready.load() != 2) std::this_thread::yield();
        return cgplay::ReviewExport::lastError();
    };
    auto first = std::async(std::launch::async, run, QStringLiteral("task-A-error"));
    auto second = std::async(std::launch::async, run, QStringLiteral("task-B-error"));
    const QString a = first.get(), b = second.get();
    int visibleWindows=0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window,LPARAM context)->BOOL {
        DWORD pid=0;
        GetWindowThreadProcessId(window,&pid);
        if(pid==GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(context);
        return TRUE;
    },reinterpret_cast<LPARAM>(&visibleWindows));
#endif
    const bool passed = a == QStringLiteral("task-A-error") && b == QStringLiteral("task-B-error") && visibleWindows==0;
    const QJsonObject result{{QStringLiteral("passed"),passed},{QStringLiteral("backgroundMode"),true},
        {QStringLiteral("visiblePlatformWindows"),visibleWindows},
        {QStringLiteral("a"),a},{QStringLiteral("b"),b},{QStringLiteral("storageExtractedVerbatim"),true}};
    QFile output(QString::fromLocal8Bit(argv[1]));
    if (!output.open(QIODevice::WriteOnly)) return 3;
    output.write(QJsonDocument(result).toJson(QJsonDocument::Indented));
    return passed ? 0 : 1;
}
