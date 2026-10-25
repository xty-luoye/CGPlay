#pragma once
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace cgplay {

// Capture component paths once on the requesting GUI thread. No QObject or
// mutable ComponentManager lookup is carried into the recording/finalizer.
struct RecordingRuntimeSnapshot {
    QString recorderRoot;
    QString helperExecutable;
    QString pythonExecutable;
    QString pythonHome;
    QString toolsRoot;
    QString finalizerScript;
    QString ffmpegExecutable;
    QString ffprobeExecutable;
    QString generation;
    QProcessEnvironment pythonEnvironment;
};

struct RecordingStartRequest {
    RecordingRuntimeSnapshot runtime;
    QJsonObject helperPlan;
    QString finalOutputPath;
    QStringList protectedMediaPaths;
    bool synthetic = false;
};

struct RecordingRecoveryRequest {
    RecordingRuntimeSnapshot runtime;
    QString sessionDescriptorPath;
    QString finalOutputPath;
    QStringList protectedMediaPaths;
};

} // namespace cgplay
