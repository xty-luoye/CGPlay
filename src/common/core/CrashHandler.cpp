// CGPlay CrashHandler.cpp
// Windows SEH + C++ terminate + Qt message handler → file logging.

#include "CrashHandler.h"

#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QDebug>

#ifdef Q_OS_WIN
  #include <windows.h>
  #include <dbghelp.h>
  #include <crtdbg.h>
  #include <signal.h>
  #include <new.h>

  #pragma comment(lib, "dbghelp.lib")
#endif

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <ctime>

namespace cgplay {

// ─── Static members ───────────────────────────────────────────────────────────
QString     CrashHandler::s_logDir;
QFile*      CrashHandler::s_crashFile   = nullptr;
QFile*      CrashHandler::s_debugFile   = nullptr;
QTextStream CrashHandler::s_crashStream;
QTextStream CrashHandler::s_debugStream;
QMutex      CrashHandler::s_mutex;
bool        CrashHandler::s_installed   = false;

// ─── Helpers ──────────────────────────────────────────────────────────────────

QString CrashHandler::_timestamp()
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
}

void CrashHandler::_writeEntry(const QString& prefix, const QString& msg)
{
    QMutexLocker lock(&s_mutex);
    QString line = _timestamp() + " " + prefix + " " + msg + "\n";
    if (s_crashFile && s_crashFile->isOpen()) {
        s_crashStream << line;
        s_crashStream.flush();
    }
}

void CrashHandler::_openLogs()
{
    if (s_logDir.isEmpty()) {
        s_logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                   + "/logs";
    }
    QDir().mkpath(s_logDir);

    QString dateStr = QDateTime::currentDateTime().toString("yyyyMMdd");

    // Crash log — append to today's file
    QString crashPath = s_logDir + "/cgplay_crash_" + dateStr + ".log";
    s_crashFile = new QFile(crashPath);
    s_crashFile->open(QIODevice::Append | QIODevice::Text);
    s_crashStream.setDevice(s_crashFile);

    // Debug log — append to today's file
    QString debugPath = s_logDir + "/cgplay_debug_" + dateStr + ".log";
    s_debugFile = new QFile(debugPath);
    s_debugFile->open(QIODevice::Append | QIODevice::Text);
    s_debugStream.setDevice(s_debugFile);
}

// ─── Install ──────────────────────────────────────────────────────────────────

void CrashHandler::install(const QString& logDir)
{
    if (s_installed) return;
    s_installed = true;
    s_logDir = logDir;
    _openLogs();

    _writeEntry("[INIT]", QString("CGPlay v%1 — PID=%2 — Build: %3 %4")
        .arg(QCoreApplication::applicationVersion())
        .arg(GetCurrentProcessId())
        .arg(__DATE__).arg(__TIME__));

    // Qt message handler
    qInstallMessageHandler(_qtMessageHandler);

#ifdef Q_OS_WIN
    // SEH exception filter
    SetUnhandledExceptionFilter(_sehFilter);

    // C++ terminate
    std::set_terminate(_terminateHandler);

    // Pure virtual call
    _set_purecall_handler(_pureCallHandler);

    // CRT invalid parameter
    _set_invalid_parameter_handler(_invalidParamHandler);

    // CRT memory leaks on exit
    _CrtSetReportMode(_CRT_WARN,   _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN,   _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

    _writeEntry("[HOOKS]", "SEH + terminate + purecall + invalid-param installed");
#endif
}

// ─── Public logging ───────────────────────────────────────────────────────────

void CrashHandler::logCrash(const QString& message)
{
    _writeEntry("[CRASH]", message);
}

void CrashHandler::logDebug(const QString& category, const QString& message)
{
    QMutexLocker lock(&s_mutex);
    QString line = _timestamp() + " [DBG:" + category + "] " + message + "\n";
    if (s_debugFile && s_debugFile->isOpen()) {
        s_debugStream << line;
        s_debugStream.flush();
    }
}

void CrashHandler::shutdown()
{
    _writeEntry("[EXIT]", "Normal shutdown");
    QMutexLocker lock(&s_mutex);
    if (s_crashFile) {
        s_crashStream.flush();
        s_crashFile->close();
        delete s_crashFile;
        s_crashFile = nullptr;
    }
    if (s_debugFile) {
        s_debugStream.flush();
        s_debugFile->close();
        delete s_debugFile;
        s_debugFile = nullptr;
    }
}

// ─── Windows SEH / C++ handlers ──────────────────────────────────────────────

#ifdef Q_OS_WIN

QString CrashHandler::_stackTrace(unsigned skipFrames, unsigned maxFrames)
{
    QString result;
    HANDLE hProcess = GetCurrentProcess();
    HANDLE hThread  = GetCurrentThread();

    SymInitialize(hProcess, nullptr, TRUE);
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);

    void* stack[64];
    unsigned short frames = CaptureStackBackTrace(skipFrames,
        (maxFrames < 64) ? maxFrames : 64, stack, nullptr);

    char symbolBuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symbolBuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 255;

    for (unsigned short i = 0; i < frames; ++i) {
        DWORD64 addr = reinterpret_cast<DWORD64>(stack[i]);
        if (SymFromAddr(hProcess, addr, nullptr, sym)) {
            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD disp = 0;
            if (SymGetLineFromAddr64(hProcess, addr, &disp, &line)) {
                result += QString("  %1!%2+0x%3 [%4:%5]\n")
                    .arg(sym->Name).arg(sym->Name)
                    .arg(addr - sym->Address, 0, 16)
                    .arg(line.FileName).arg(line.LineNumber);
            } else {
                result += QString("  %1+0x%2\n")
                    .arg(sym->Name).arg(addr - sym->Address, 0, 16);
            }
        } else {
            result += QString("  0x%1\n").arg(addr, 0, 16);
        }
    }

    SymCleanup(hProcess);
    return result;
}

LONG WINAPI CrashHandler::_sehFilter(PEXCEPTION_POINTERS p)
{
    DWORD code = p->ExceptionRecord->ExceptionCode;
    DWORD64 addr = reinterpret_cast<DWORD64>(p->ExceptionRecord->ExceptionAddress);

    QString msg;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        msg = QString("ACCESS_VIOLATION at 0x%1 — addr %2 r/w=%3")
            .arg(addr, 0, 16)
            .arg(p->ExceptionRecord->ExceptionInformation[1], 0, 16)
            .arg(p->ExceptionRecord->ExceptionInformation[0]);
        break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        msg = QString("ARRAY_BOUNDS_EXCEEDED at 0x%1").arg(addr, 0, 16); break;
    case EXCEPTION_BREAKPOINT:
        msg = QString("BREAKPOINT at 0x%1").arg(addr, 0, 16); break;
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        msg = QString("DATATYPE_MISALIGNMENT at 0x%1").arg(addr, 0, 16); break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        msg = QString("FLT_DIVIDE_BY_ZERO at 0x%1").arg(addr, 0, 16); break;
    case EXCEPTION_FLT_OVERFLOW:
        msg = QString("FLT_OVERFLOW at 0x%1").arg(addr, 0, 16); break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        msg = QString("ILLEGAL_INSTRUCTION at 0x%1").arg(addr, 0, 16); break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        msg = QString("INT_DIVIDE_BY_ZERO at 0x%1").arg(addr, 0, 16); break;
    case EXCEPTION_STACK_OVERFLOW:
        msg = QString("STACK_OVERFLOW at 0x%1").arg(addr, 0, 16); break;
    default:
        msg = QString("UNKNOWN_EXCEPTION 0x%1 at 0x%2")
            .arg(code, 0, 16).arg(addr, 0, 16); break;
    }

    _writeEntry("[SEH]", msg);
    _writeEntry("[STACK]", "\n" + _stackTrace(0, 32));

    return EXCEPTION_CONTINUE_SEARCH; // Let the OS show the crash dialog
}

void CrashHandler::_terminateHandler()
{
    // Try to get the current exception
    QString what = "(unknown)";
    try { auto eptr = std::current_exception(); if (eptr) { std::rethrow_exception(eptr); } }
    catch (const std::exception& e) { what = QString("std::exception: %1").arg(e.what()); }
    catch (...) { what = "unknown C++ exception"; }

    _writeEntry("[TERMINATE]", what);
    _writeEntry("[STACK]", "\n" + _stackTrace(1, 32));

    // Flush before abort
    shutdown();
    std::abort();
}

void CrashHandler::_pureCallHandler()
{
    _writeEntry("[PURECALL]", "Pure virtual function called");
    _writeEntry("[STACK]", "\n" + _stackTrace(1, 32));
    shutdown();
    std::abort();
}

void CrashHandler::_invalidParamHandler(const wchar_t* expr, const wchar_t* func,
                                         const wchar_t* file, unsigned int line,
                                         uintptr_t /*reserved*/)
{
    QString msg = QString("Invalid parameter");
    if (expr) msg += QString(" expr='%1'").arg(QString::fromWCharArray(expr));
    if (func) msg += QString(" func='%1'").arg(QString::fromWCharArray(func));
    if (file) msg += QString(" file='%1:%2'").arg(QString::fromWCharArray(file)).arg(line);
    _writeEntry("[CRT_INVALID]", msg);
    _writeEntry("[STACK]", "\n" + _stackTrace(2, 32));
    // Don't abort — just log. CRT will continue.
}

void CrashHandler::_qtMessageHandler(QtMsgType type,
                                      const QMessageLogContext& ctx,
                                      const QString& msg)
{
    QString prefix;
    switch (type) {
    case QtDebugMsg:    prefix = "[QT_DBG]"; break;
    case QtInfoMsg:     prefix = "[QT_INF]"; break;
    case QtWarningMsg:  prefix = "[QT_WRN]"; break;
    case QtCriticalMsg: prefix = "[QT_CRI]"; break;
    case QtFatalMsg:    prefix = "[QT_FAT]"; break;
    }

    QString detail;
    if (ctx.file) detail += QString("file=%1 ").arg(ctx.file);
    if (ctx.line) detail += QString("line=%1 ").arg(ctx.line);
    if (ctx.function) detail += QString("func=%1 ").arg(ctx.function);

    _writeEntry(prefix, detail + msg);

    // Also output to stderr / debugger so it's visible during dev
    fprintf(stderr, "%s %s %s\n",
        qPrintable(_timestamp()), qPrintable(prefix), qPrintable(detail + msg));
    fflush(stderr);

    // QtFatal aborts — let us flush first
    if (type == QtFatalMsg) {
        _writeEntry("[FATAL]", "Fatal Qt message — about to abort");
        shutdown();
    }
}

#endif // Q_OS_WIN

} // namespace cgplay
