#pragma once
// CGPlay CrashHandler.h
// Crash & debug logging — SEH + terminate hook + Qt message redirect.

#include <QString>
#include <QDateTime>
#include <QMutex>
#include <QFile>
#include <QTextStream>
#include <functional>

#ifdef Q_OS_WIN
  #include <windows.h>
  #include <dbghelp.h>
#endif

namespace cgplay {

class CrashHandler
{
public:
    // Init once at app start — installs all hooks.
    static void install(const QString& logDir = QString());

    // Write raw message into the crash log (timestamped).
    static void logCrash(const QString& message);

    // Write debug-level message (only goes to the debug companion log).
    static void logDebug(const QString& category, const QString& message);

    // Flush & close logs (called at exit).
    static void shutdown();

private:
    static void _openLogs();
    static void _writeEntry(const QString& prefix, const QString& msg);
    static QString _timestamp();

    static QString s_logDir;
    static QFile*  s_crashFile;
    static QFile*  s_debugFile;
    static QTextStream s_crashStream;
    static QTextStream s_debugStream;
    static QMutex  s_mutex;
    static bool s_installed;

#ifdef Q_OS_WIN
    static LONG WINAPI _sehFilter(PEXCEPTION_POINTERS p);
    static void _terminateHandler();
    static void _pureCallHandler();
    static void _invalidParamHandler(const wchar_t* expr, const wchar_t* func,
                                      const wchar_t* file, unsigned int line, uintptr_t reserved);
    static void _qtMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg);
    static QString _stackTrace(unsigned skipFrames = 2, unsigned maxFrames = 32);
#endif
};

} // namespace cgplay
