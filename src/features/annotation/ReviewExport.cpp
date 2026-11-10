// CGPlay ReviewExport.cpp

#include "ReviewExport.h"
#include "AnnotationItem.h"
#include "AnnotationOverlay.h"
#include "services/ai/api/AIProviderTypes.h"
#include "common/jobs/JobSystem.h"
#include "component/ComponentManager.h"
#include "component/DownloadService.h"

#include <QFile>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QProcess>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QtMath>
#include <QCoreApplication>
#include <QWidget>
#include <QMessageBox>
#include <QProgressDialog>
#include <QEventLoop>
#include <QStandardPaths>
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QRegularExpression>
#include <QDebug>
#include <limits>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#ifdef CGPLAY_OCIO_ENABLED
  #include <OpenColorIO/OpenColorIO.h>
  namespace OCIO = OCIO_NAMESPACE;
#endif

namespace cgplay {

QString videoExportCodecId(VideoExportCodec codec)
{
    switch (codec) {
    case VideoExportCodec::H264: return QStringLiteral("h264");
    case VideoExportCodec::H265: return QStringLiteral("h265");
    case VideoExportCodec::ProRes422HQ: return QStringLiteral("prores_hq");
    case VideoExportCodec::ProRes4444: return QStringLiteral("prores_4444");
    }
    return {};
}

namespace {

constexpr int kProbeTimeoutMs = 8 * 1000;
constexpr int kExportTimeoutMs = 6 * 60 * 60 * 1000;
constexpr int kExtractTimeoutMs = 2 * 60 * 1000;

ProcessOutcome waitForBoundedProcess(QProcess& process, int timeoutMs)
{
    if (process.state() == QProcess::Starting &&
        !process.waitForStarted(qMin(timeoutMs, 15 * 1000))) {
        ProcessOutcome outcome;
        outcome.state = JobState::Failed;
        outcome.standardError = process.errorString().toUtf8();
        return outcome;
    }
    JobContext context(timeoutMs);
    ProcessOutcome outcome = context.waitForProcess(process, 25, timeoutMs);
    if (process.error() == QProcess::FailedToStart) {
        outcome.state = JobState::Failed;
        outcome.exitCode = -1;
        outcome.standardError = process.errorString().toUtf8();
    }
    return outcome;
}

ProcessOutcome waitForInteractiveProcess(
    QProcess& process,
    QWidget* parentWidget,
    const QString& label,
    int timeoutMs)
{
    JobContext context(timeoutMs);
    QProgressDialog dialog(label, QObject::tr("取消"), 0, 0, parentWidget);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(0);
    dialog.setAutoClose(false);

    QEventLoop loop;
    QObject::connect(&dialog, &QProgressDialog::canceled, &loop, [&context]() {
        context.cancel();
    });
    QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);

    QTimer stopPoll;
    stopPoll.setInterval(25);
    QObject::connect(&stopPoll, &QTimer::timeout, &loop, [&]() {
        if (!context.shouldStop() || process.state() == QProcess::NotRunning) {
            return;
        }
        process.terminate();
        QTimer::singleShot(750, &loop, [&process]() {
            if (process.state() != QProcess::NotRunning) {
                process.kill();
            }
        });
    });
    stopPoll.start();
    dialog.show();

    ProcessOutcome outcome;
    if (process.state() == QProcess::Starting &&
        !process.waitForStarted(qMin(timeoutMs, 15 * 1000))) {
        outcome.state = JobState::Failed;
        outcome.standardError = process.errorString().toUtf8();
    } else {
        if (process.state() != QProcess::NotRunning) {
            loop.exec();
        }
        outcome = context.waitForProcess(process, 25, timeoutMs);
        if (context.isCancellationRequested()) {
            outcome.state = JobState::Canceled;
        } else if (context.hasTimedOut()) {
            outcome.state = JobState::TimedOut;
        }
    }

    stopPoll.stop();
    dialog.close();
    return outcome;
}

} // namespace

// ─── Forward declarations for local helpers ─────────────────────────────────────
static void applyLinearToSRGB(QImage& img);
static void drawAnnotationsOnImage(QImage& img, const QVector<AnnotationItem>& anns,
                                    int imgW, int imgH);

// ─── FPS Detection ─────────────────────────────────────────────────────────────
double ReviewExport::detectFps(const QString& srcMedia)
{
    double fps = 24.0;

    QString ffprobePath = _ffmpegDir() + "/ffprobe.exe";
    if (!QFileInfo::exists(ffprobePath))
        ffprobePath = QStandardPaths::findExecutable("ffprobe");

    if (!ffprobePath.isEmpty()) {
        QProcess fp;
        fp.start(ffprobePath, {
            "-v", "0",
            "-select_streams", "v:0",
            "-show_entries", "stream=r_frame_rate",
            "-of", "default=noprint_wrappers=1:nokey=1",
            srcMedia
        }, QIODevice::ReadOnly);
        const ProcessOutcome outcome = waitForBoundedProcess(fp, kProbeTimeoutMs);
        if (outcome.succeeded()) {
            QString fpsStr = QString::fromUtf8(outcome.standardOutput).trimmed();
            auto parts = fpsStr.split('/');
            if (parts.size() == 2 && parts[0].toDouble() > 0 && parts[1].toDouble() > 0)
                fps = parts[0].toDouble() / parts[1].toDouble();
            else if (parts.size() == 1 && parts[0].toDouble() > 0)
                fps = parts[0].toDouble();
        }
    }
    if (fps <= 0) fps = 24.0;
    return fps;
}

// ─── Still Image Detection ─────────────────────────────────────────────────────
bool ReviewExport::isStillImage(const QString& path)
{
    QString ext = QFileInfo(path).suffix().toLower();
    return ext == "exr" || ext == "dpx" || ext == "png" ||
           ext == "jpg" || ext == "jpeg" || ext == "tif" ||
           ext == "tiff" || ext == "bmp" || ext == "psd";
}

// ─── Image Sequence Glob ───────────────────────────────────────────────────────
// 如果文件名包含帧号（如 render.0001.exr），生成 ffmpeg glob 模式
// （如 render_*.exr），让 ffmpeg 读取整个序列。返回空字符串 = 不是序列。
QString ReviewExport::buildSequenceGlob(const QString& path)
{
    QFileInfo fi(path);
    QString dir     = fi.path();
    QString name    = fi.completeBaseName(); // e.g. "render.0001" or "image"
    QString ext     = fi.suffix();           // e.g. "exr"

    // 模式 1: name.0001.ext, name_0001.ext, name-0001.ext
    QRegularExpression re("^(.*?)[\\._\\-](\\d{4,})$");
    auto m = re.match(name);
    if (m.hasMatch()) {
        QString base = m.captured(1);  // e.g. "render"
        return dir + "/" + base + "_*." + ext;
    }
    // 模式 2: name0001.ext (如 img0001.exr)
    re.setPattern(R"(^(.*\D)(\d{4,})$)");
    m = re.match(name);
    if (m.hasMatch()) {
        QString base = m.captured(1);  // e.g. "render"
        return dir + "/" + base + "*." + ext;
    }
    return {}; // 不是序列
}

// ─── Frame Count Detection ─────────────────────────────────────────────────────
int ReviewExport::detectFrameCount(const QString& srcMedia)
{
    int count = 0;

    QString ffprobePath = _ffmpegDir() + "/ffprobe.exe";
    if (!QFileInfo::exists(ffprobePath))
        ffprobePath = QStandardPaths::findExecutable("ffprobe");

    if (!ffprobePath.isEmpty()) {
        QProcess fp;
        fp.start(ffprobePath, {
            "-v", "error",
            "-select_streams", "v:0",
            "-count_frames",
            "-show_entries", "stream=nb_frames,nb_read_frames,duration,avg_frame_rate",
            "-of", "json=compact=1",
            srcMedia
        }, QIODevice::ReadOnly);
        const ProcessOutcome outcome = waitForBoundedProcess(fp, kProbeTimeoutMs);
        if (outcome.succeeded()) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(
                outcome.standardOutput, &parseError);
            if (!document.isNull() && document.isObject()) {
                const QJsonArray streams = document.object().value(QStringLiteral("streams")).toArray();
                if (!streams.isEmpty() && streams.first().isObject()) {
                    const QJsonObject stream = streams.first().toObject();
                    const auto numberFor = [&stream](const QString& key) {
                        const QJsonValue value = stream.value(key);
                        if (value.isDouble()) {
                            return value.toDouble();
                        }
                        if (value.isString()) {
                            bool ok = false;
                            const double number = value.toString().toDouble(&ok);
                            return ok ? number : 0.0;
                        }
                        return 0.0;
                    };
                    const auto positiveIntegerFor = [&numberFor](const QString& key) {
                        const double number = numberFor(key);
                        return number > 0.0 && number <= static_cast<double>(std::numeric_limits<int>::max())
                            ? qRound(number) : 0;
                    };

                    count = qMax(count, positiveIntegerFor(QStringLiteral("nb_frames")));
                    count = qMax(count, positiveIntegerFor(QStringLiteral("nb_read_frames")));

                    const QJsonValue frameRateValue = stream.value(QStringLiteral("avg_frame_rate"));
                    const QString frameRate = frameRateValue.isString()
                        ? frameRateValue.toString()
                        : frameRateValue.isDouble()
                            ? QString::number(frameRateValue.toDouble(), 'g', 16)
                            : QString{};
                    double rate = 0.0;
                    const QStringList rateParts = frameRate.split('/');
                    if (rateParts.size() == 2) {
                        bool numeratorOk = false;
                        bool denominatorOk = false;
                        const double numerator = rateParts[0].toDouble(&numeratorOk);
                        const double denominator = rateParts[1].toDouble(&denominatorOk);
                        if (numeratorOk && denominatorOk && denominator > 0.0) {
                            rate = numerator / denominator;
                        }
                    } else {
                        bool ok = false;
                        rate = frameRate.toDouble(&ok);
                        if (!ok) rate = 0.0;
                    }

                    const double duration = numberFor(QStringLiteral("duration"));
                    const double estimatedCount = duration * rate;
                    if (count <= 1 && estimatedCount > 0.0
                        && estimatedCount <= static_cast<double>(std::numeric_limits<int>::max())) {
                        count = qMax(count, qRound(estimatedCount));
                    }
                }
            } else {
                qWarning() << "[ReviewExport] ffprobe frame-count JSON parse failed:"
                           << parseError.errorString();
            }
        }
    }
    return count;
}

// ─── JSON Export ──────────────────────────────────────────────────────────────
bool ReviewExport::exportJson(const QVector<AnnotationItem>& annotations,
                               const QString& filepath)
{
    QFileInfo fi(filepath);
    QDir().mkpath(fi.path());

    QJsonObject root;
    root["version"]            = 1;
    root["export_time"]        = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["total_annotations"]  = static_cast<int>(annotations.size());

    QJsonArray arr;
    for (const auto& ann : annotations) {
        QJsonObject obj;
        obj["id"]           = ann.id;
        obj["frame"]        = ann.frame;
        obj["type"]         = annotationTypeString(ann.type);
        obj["color"]        = ann.color.name();
        obj["author"]       = ann.author;
        obj["created_time"] = ann.createdTime;
        obj["comment"]      = ann.comment;

        QJsonArray ptsArr;
        for (const auto& pt : ann.points) {
            QJsonArray ptArr;
            ptArr.append(pt.x());
            ptArr.append(pt.y());
            ptsArr.append(ptArr);
        }
        obj["points"] = ptsArr;

        QJsonArray cmtsArr;
        for (const auto& c : ann.comments) {
            QJsonObject co;
            co["author"]   = c.author;
            co["text"]     = c.text;
            co["time"]     = c.time;
            co["status"]   = reviewStatusString(c.status);
            co["assignee"] = c.assignee;
            cmtsArr.append(co);
        }
        obj["comments"] = cmtsArr;
        obj["status"]   = reviewStatusString(ann.status);
        obj["assignee"] = ann.assignee;
        arr.append(obj);
    }
    root["annotations"] = arr;

    QSaveFile f(filepath);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "[ReviewExport] Cannot write:" << filepath;
        return false;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return f.write(data) == data.size() && f.commit();
}

// ─── HTML Export ──────────────────────────────────────────────────────────────
bool ReviewExport::exportAIReport(const AIResponse& response,
    const QVector<AnnotationItem>& annotations, const QString& filepath, const QString& mediaTitle)
{
    const auto findingsJson = [](const QVector<AIAnalysisFinding>& findings) {
        QJsonArray array;
        for (const auto& finding : findings) {
            array.append(QJsonObject{
                {"frame", finding.frame}, {"category", finding.category},
                {"severity", static_cast<int>(finding.severity)}, {"title", finding.title},
                {"description", finding.description}, {"metadata", finding.metadata}
            });
        }
        return array;
    };
    const auto& summary = response.reviewSummary;
    const QJsonObject aiReport{
        {"jobId", response.jobId}, {"providerId", response.providerId}, {"model", response.model},
        {"success", response.success}, {"errorMessage", response.errorMessage},
        {"rawText", response.rawText}, {"findings", findingsJson(response.findings)},
        {"reviewSummary", QJsonObject{
            {"plainText", summary.plainText}, {"markdown", summary.markdown}, {"html", summary.html},
            {"criticalCount", summary.criticalCount}, {"majorCount", summary.majorCount},
            {"minorCount", summary.minorCount}, {"findings", findingsJson(summary.findings)},
            {"metadata", summary.metadata}}},
        {"startedAt", response.startedAt.toString(Qt::ISODateWithMs)},
        {"finishedAt", response.finishedAt.toString(Qt::ISODateWithMs)}
    };
    // Reuse the annotation writer so AI exports retain the full existing schema.
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const QString basePath = temporary.filePath(QStringLiteral("annotations"));
    const bool json = filepath.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive);
    if (!(json ? exportJson(annotations, basePath) : exportHtml(annotations, basePath, mediaTitle))) return false;
    QFile base(basePath);
    if (!base.open(QIODevice::ReadOnly)) return false;
    QByteArray data;
    if (json) {
        auto root = QJsonDocument::fromJson(base.readAll()).object();
        root.insert(QStringLiteral("ai_report"), aiReport);
        data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    } else {
        QString html = QString::fromUtf8(base.readAll());
        QString section = QStringLiteral("<section><h2>AI Review</h2><p>%1 / %2</p>")
            .arg(response.providerId.toHtmlEscaped(), response.model.toHtmlEscaped());
        const QString summaryText = !summary.plainText.isEmpty() ? summary.plainText
            : (!summary.markdown.isEmpty() ? summary.markdown : summary.html);
        section += QStringLiteral("<pre style='white-space:pre-wrap'>%1</pre><pre style='white-space:pre-wrap'>%2</pre>")
            .arg(summaryText.toHtmlEscaped(), response.rawText.toHtmlEscaped());
        for (const QJsonArray findings : {findingsJson(response.findings), findingsJson(summary.findings)}) {
            for (const auto& value : findings) {
                const auto finding = value.toObject();
                section += QStringLiteral("<h3>Frame %1: %2</h3><p>%3</p>")
                    .arg(finding.value("frame").toInt())
                    .arg(finding.value("title").toString().toHtmlEscaped(),
                         finding.value("description").toString().toHtmlEscaped());
            }
        }
        section += QStringLiteral("</section>");
        html.insert(html.indexOf(QStringLiteral("<div class=\"summary\">")), section);
        data = html.toUtf8();
    }
    if (!QDir().mkpath(QFileInfo(filepath).absolutePath())) return false;
    QSaveFile output(filepath);
    return output.open(QIODevice::WriteOnly) && output.write(data) == data.size() && output.commit();
}

bool ReviewExport::exportHtml(const QVector<AnnotationItem>& annotations,
                               const QString& filepath,
                               const QString& mediaTitle)
{
    QFileInfo fi(filepath);
    QDir().mkpath(fi.path());

    // Group by frame
    QMap<int, QVector<AnnotationItem>> framesAnn;
    for (const auto& a : annotations)
        framesAnn[a.frame].append(a);

    QString css;
    css += "*{box-sizing:border-box;margin:0;padding:0}";
    css += "body{font-family:'Segoe UI','Microsoft YaHei',sans-serif;background:#1a1a1a;color:#ddd;margin:20px;line-height:1.5}";
    css += "h1{color:#0af;border-bottom:2px solid #0af;padding-bottom:8px;margin-bottom:16px}";
    css += ".summary{background:#222;padding:12px 16px;border-radius:6px;margin-bottom:16px;display:flex;gap:24px}";
    css += ".summary span{color:#888;font-size:13px}";
    css += ".summary b{color:#fff;font-size:18px}";
    css += ".frame-section{background:#222;margin:12px 0;padding:12px;border-radius:6px;border-left:4px solid #0af}";
    css += ".frame-title{font-size:16px;font-weight:bold;color:#fff;margin-bottom:8px;padding-left:4px}";
    css += ".ann-item{background:#2a2a2a;margin:6px 0;padding:10px 14px;border-radius:4px;display:flex;align-items:flex-start;gap:14px}";
    css += ".ann-meta{min-width:150px;flex-shrink:0}";
    css += ".ann-type{font-size:11px;color:#888;text-transform:uppercase}";
    css += ".ann-frame{font-size:12px;color:#666}";
    css += ".ann-status{display:inline-block;padding:2px 10px;border-radius:10px;font-size:11px;color:#fff;margin:2px 0}";
    css += ".status-Open{background:#f80}";
    css += ".status-InProgress{background:#0af}";
    css += ".status-Resolved{background:#0c4}";
    css += ".ann-comment{font-size:14px;white-space:pre-wrap;flex:1}";
    css += ".ann-color{display:inline-block;width:14px;height:14px;border-radius:3px;margin-right:6px;vertical-align:middle}";
    css += ".ann-points{font-size:10px;color:#555;margin-top:4px}";
    css += ".footer{text-align:center;color:#555;font-size:11px;margin-top:24px;padding-top:12px;border-top:1px solid #333}";

    QString html;
    html += "<!DOCTYPE html>\n";
    html += "<html><head><meta charset=\"utf-8\">\n";
    html += "<title>" + (mediaTitle.isEmpty() ? QString("CGPlay Review Report") : mediaTitle.toHtmlEscaped()) + "</title>\n";
    html += "<style>" + css + "</style></head><body>\n";

    html += "<h1>CGPlay Review Report</h1>\n";

    // Summary
    html += "<div class=\"summary\">\n";
    html += "<div><span>Total Frames</span><br><b>" + QString::number(framesAnn.size()) + "</b></div>\n";
    html += "<div><span>Total Annotations</span><br><b>" + QString::number(annotations.size()) + "</b></div>\n";
    html += "<div><span>Export Time</span><br><span>" +
            QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss") + "</span></div>\n";
    html += "</div>\n";

    if (!mediaTitle.isEmpty())
        html += "<p style=\"color:#888;font-size:13px;\">Media: " + mediaTitle.toHtmlEscaped() + "</p>\n";

    // Per-frame sections
    for (auto it = framesAnn.begin(); it != framesAnn.end(); ++it) {
        int frame = it.key();
        const auto& anns = it.value();

        html += "<div class=\"frame-section\">\n";
        html += "<div class=\"frame-title\">Frame " + QString::number(frame + 1) +
                " <span style=\"color:#888;font-weight:normal;\">(" +
                QString::number(anns.size()) + " annotations)</span></div>\n";

        for (const auto& a : anns) {
            QString statusCls;
            switch (a.status) {
            case ReviewStatus::Open:       statusCls = "Open"; break;
            case ReviewStatus::InProgress: statusCls = "InProgress"; break;
            case ReviewStatus::Resolved:   statusCls = "Resolved"; break;
            default: break;
            }

            html += "<div class=\"ann-item\">\n";
            html += "<div class=\"ann-meta\">\n";
            html += "<span class=\"ann-color\" style=\"background:" + a.color.name() + "\"></span>";
            html += "<b>" + annotationTypeString(a.type) + "</b><br>\n";
            html += "<span class=\"ann-type\">" + a.createdTime.toHtmlEscaped() + "</span><br>\n";
            html += "<span class=\"ann-status status-" + statusCls + "\">" +
                    reviewStatusString(a.status) + "</span><br>\n";
            html += "<span class=\"ann-frame\">Frame " + QString::number(a.frame + 1) + "</span><br>\n";
            html += "<span class=\"ann-frame\">Assignee: " + a.assignee.toHtmlEscaped() + "</span>\n";
            html += "</div>\n";

            QString comment = a.comment;
            for (const auto& c : a.comments) {
                if (!c.text.isEmpty())
                    comment += (comment.isEmpty() ? "" : "\n---\n") + c.text;
            }

            html += "<div class=\"ann-comment\">\n";
            html += (comment.isEmpty() ? "<em style=\"color:#555;\">(no comment)</em>" : comment.toHtmlEscaped());
            html += "\n</div>\n";

            // Points info
            if (!a.points.isEmpty()) {
                html += "<span class=\"ann-points\">";
                for (const auto& pt : a.points) {
                    html += "(" + QString::number(pt.x(), 'f', 0) + "," +
                             QString::number(pt.y(), 'f', 0) + ") ";
                }
                html += "</span>\n";
            }
            html += "</div>\n"; // ann-item
        }
        html += "</div>\n"; // frame-section
    }

    html += "<div class=\"footer\">Generated by CGPlay</div></body></html>\n";

    QSaveFile f(filepath);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "[ReviewExport] Cannot write HTML:" << filepath;
        return false;
    }
    const QByteArray data = html.toUtf8();
    return f.write(data) == data.size() && f.commit();
}

// ─── ffmpeg Management ────────────────────────────────────────────────────────

QString ReviewExport::_ffmpegDir()
{
    return QCoreApplication::applicationDirPath();
}

QString ReviewExport::locateFfmpeg()
{
    const QString componentPath =
        ComponentManager::instance().componentExecutablePath(QStringLiteral("ffmpeg"), QStringLiteral("ffmpeg.exe"));
    if (!componentPath.isEmpty()) return componentPath;

    // 1. Program directory (bundled ffmpeg)
    QString exe = _ffmpegDir() + "/ffmpeg.exe";
    if (QFileInfo::exists(exe)) return exe;

    // 2. PATH (system ffmpeg)
    exe = QStandardPaths::findExecutable("ffmpeg");
    if (!exe.isEmpty()) return exe;
    return {};
}

// ─── Python-based export engine ──────────────────────────────────────────────────
bool ReviewExport::prepareVideoExportRuntime(QWidget* parent, ComponentRuntimePaths* runtime, QString* error)
{
    if (!qApp || QThread::currentThread() != qApp->thread() || !runtime) {
        if (error) *error = QObject::tr("导出组件必须先在界面线程准备。");
        return false;
    }
    for (const QString& id : {QStringLiteral("python-runtime"), QStringLiteral("tools")}) {
        if (!ComponentManager::instance().ensureComponent(id, parent, true, error)) return false;
    }
    *runtime = captureComponentRuntimePaths();
    if (runtime->pythonExecutable.isEmpty() || runtime->script(QStringLiteral("cgplay/export/export_engine.py")).isEmpty()) {
        if (error) *error = QObject::tr("导出组件不完整，请修复 Python 和导出脚本组件。");
        return false;
    }
    return true;
}

// Run Python export engine, parse PROGRESS/DONE/ERROR_LOG lines
static thread_local QString s_lastExportError;  // stored error log content

static bool runPythonExport(QProcess& proc, std::function<void(double)> progress,
                            const std::atomic_bool* cancel)
{
    s_lastExportError.clear();
    QString errorLogPath;
    QByteArray transcript;
    bool doneReported = false;
    JobContext context(
        kExportTimeoutMs,
        [progress](int percent, const QString&) {
            if (progress) {
                progress(static_cast<double>(percent) / 100.0);
            }
        });

    const auto consumeLine = [&](const QByteArray& rawLine) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty()) {
            return;
        }
        transcript += line;
        transcript += '\n';
        if (transcript.size() > 64 * 1024) transcript.remove(0, transcript.size() - 64 * 1024);
        if (line.startsWith("PROGRESS ")) {
            const auto parts = line.mid(9).split(' ');
            if (!parts.isEmpty()) {
                context.reportProgress(qRound(parts[0].toDouble()), QStringLiteral("review.export"));
            }
        } else if (line == "DONE") {
            doneReported = true;
        } else if (line.startsWith("ERROR_LOG: ")) {
            errorLogPath = QString::fromUtf8(line.mid(11));
            qWarning() << "[ReviewExport] Error log:" << errorLogPath;
        } else if (line.startsWith("ERROR")) {
            qWarning() << "[ReviewExport] Python ERROR:" << line;
        }
    };

    while (proc.state() == QProcess::Running) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            context.cancel();
        }
        if (context.shouldStop()) {
            break;
        }
        if (proc.waitForReadyRead(100)) {
            while (proc.canReadLine()) {
                consumeLine(proc.readLine());
            }
        }
    }
    const ProcessOutcome outcome = context.waitForProcess(proc, 25, kExportTimeoutMs);
    const QByteArray remaining = outcome.standardOutput + outcome.standardError;
    for (const QByteArray& line : remaining.split('\n')) {
        consumeLine(line);
    }

    if (doneReported && proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
        context.reportProgress(100, QStringLiteral("review.export.complete"));
        return true;
    }

    // Try to read the error log file for detailed diagnostics
    if (!errorLogPath.isEmpty()) {
        QFile logFile(errorLogPath);
        if (logFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            s_lastExportError = QString::fromUtf8(logFile.readAll());
            logFile.close();
        }
    }
    if (s_lastExportError.isEmpty()) {
        if (outcome.state == JobState::Canceled) {
            s_lastExportError = QObject::tr("导出已取消。");
        } else if (outcome.state == JobState::TimedOut) {
            s_lastExportError = QObject::tr("导出超时，后台进程已终止。");
        } else {
            s_lastExportError = QString::fromUtf8(transcript).trimmed();
        }
    }

    qCritical() << "[ReviewExport] Engine failed:\n" << s_lastExportError;
    return false;
}

bool ReviewExport::exportVideoCompare(const QJsonObject& job,
                                     const ComponentRuntimePaths& paths,
                                     std::function<void(double)> progress,
                                     const std::atomic_bool* cancel)
{
    s_lastExportError.clear();
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        s_lastExportError = QObject::tr("导出已取消。");
        return false;
    }
    if (!paths.captured || paths.pythonExecutable.isEmpty() ||
        paths.script(QStringLiteral("cgplay/export/compare_engine.py")).isEmpty()) {
        s_lastExportError = QObject::tr("A/B 导出组件不完整，请修复导出脚本和 Python 组件。");
        return false;
    }
    const QString output = job.value(QStringLiteral("output")).toObject().value(QStringLiteral("path")).toString();
    if (output.trimmed().isEmpty()) {
        s_lastExportError = QObject::tr("请选择 A/B 视频输出位置。");
        return false;
    }
    QDir outputDirectory(QFileInfo(output).absolutePath());
    if (!outputDirectory.mkpath(QStringLiteral("."))) {
        s_lastExportError = QObject::tr("无法创建输出目录。");
        return false;
    }
    // The same-volume supervisor survives the single Python worker until all
    // file handles are closed, including cancellation/forced process shutdown.
    QTemporaryDir staging(outputDirectory.filePath(QStringLiteral(".cgplay-compare-job-XXXXXX")));
    if (!staging.isValid()) {
        s_lastExportError = QObject::tr("无法创建 A/B 导出暂存目录。");
        return false;
    }
    const QString jobPath = QDir(staging.path()).filePath(QStringLiteral("compare-job.json"));
    QSaveFile file(jobPath);
    file.setDirectWriteFallback(false);
    const QByteArray bytes = QJsonDocument(job).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        s_lastExportError = QObject::tr("无法保存本次 A/B 导出设置。");
        return false;
    }
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString oldPath = env.value(QStringLiteral("PYTHONPATH"));
    env.insert(QStringLiteral("PYTHONPATH"), oldPath.isEmpty() ? paths.toolsRoot
        : paths.toolsRoot + QDir::listSeparator() + oldPath);
    env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    if (!paths.pythonHome.isEmpty()) env.insert(QStringLiteral("PYTHONHOME"), paths.pythonHome);
    proc.setProcessEnvironment(env);
    proc.setWorkingDirectory(paths.toolsRoot);
#ifdef Q_OS_WIN
    proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
#endif
    proc.start(paths.pythonExecutable, {QStringLiteral("-B"), QStringLiteral("-m"),
        QStringLiteral("cgplay.export.compare_engine"), QStringLiteral("--job"), jobPath,
        QStringLiteral("--staging-dir"), staging.path()}, QIODevice::ReadOnly);
    if (!proc.waitForStarted(15000)) {
        s_lastExportError = QObject::tr("A/B 导出运行时启动失败。");
        return false;
    }
    return runPythonExport(proc, std::move(progress), cancel);
}

// Unified export via Python engine
static bool exportWithPython(const QString& srcMedia, const QString& outVideo,
                               int startFrame, int endFrame, double fps,
                               std::function<void(double)> progress, const std::atomic_bool* cancel,
                              VideoExportCodec codec,
                              const QString& annoJson = QString(),
                              const ComponentRuntimePaths& runtime = {},
                              std::optional<ColorTransformSnapshot> colorSnapshot = std::nullopt)
{
    s_lastExportError.clear();
    const QString codecId = videoExportCodecId(codec);
    if (codecId.isEmpty()) {
        s_lastExportError = QObject::tr("不支持的导出编码格式。");
        return false;
    }
    if (!annoJson.isEmpty() && !QFileInfo::exists(annoJson)) {
        s_lastExportError = QObject::tr("批注数据文件不存在，已取消导出以避免生成无批注视频。");
        return false;
    }
    // No GUI/component install is reachable from this worker. Normal UI calls
    // pass a GUI-prepared snapshot; headless callers may resolve existing files
    // once, but a missing component always returns an error without prompting.
    const ComponentRuntimePaths paths = runtime.captured ? runtime : captureComponentRuntimePaths();
    const QString python = paths.pythonExecutable;
    const QString toolsDir = paths.toolsRoot;
    if (python.isEmpty()) {
        s_lastExportError = QObject::tr(
            "未找到可用的导出运行时 Python。\n"
            "请确认安装目录中的 runtime/python 完整，或在开发环境中提供可用的 Python。");
        qCritical() << "[ReviewExport] Python not found!";
        return false;
    }
    if (toolsDir.isEmpty()) {
        s_lastExportError = QObject::tr(
            "未找到导出脚本目录 tools。\n"
            "请确认安装目录中的 tools/cgplay/export 完整。");
        qCritical() << "[ReviewExport] tools/ directory not found!";
        return false;
    }

    const QFileInfo outputInfo(outVideo);
    const QString sourceCanonical = QFileInfo(srcMedia).canonicalFilePath();
    if (!sourceCanonical.isEmpty() && sourceCanonical.compare(outputInfo.canonicalFilePath(), Qt::CaseInsensitive) == 0) {
        s_lastExportError = QObject::tr("输出不能覆盖源素材，请选择其他文件。");
        return false;
    }
    QDir outputDirectory(outputInfo.absolutePath());
    if (!outputDirectory.mkpath(QStringLiteral("."))) {
        s_lastExportError = QObject::tr("无法创建输出目录。");
        return false;
    }
    // This supervisor directory outlives QProcess, including a forced kill;
    // Python only cleans its own child. The final target is never a temp file.
    QTemporaryDir staging(outputDirectory.filePath(QStringLiteral(".cgplay-export-job-XXXXXX")));
    if (!staging.isValid()) {
        s_lastExportError = QObject::tr("无法创建导出暂存目录。");
        return false;
    }
    QString colorFile;
    if (colorSnapshot) {
        colorFile = QDir(staging.path()).filePath(QStringLiteral("color-settings.json"));
        QSaveFile file(colorFile);
        file.setDirectWriteFallback(false);
        const QByteArray bytes = QJsonDocument(colorSnapshot->toJson()).toJson(QJsonDocument::Compact);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            s_lastExportError = QObject::tr("无法保存本次导出颜色设置。");
            return false;
        }
    }
    // Use -m mode for proper package imports (from .models... etc.)
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString existingPyPath = env.value("PYTHONPATH");
    env.insert("PYTHONPATH",
               existingPyPath.isEmpty() ? toolsDir
                                        : toolsDir + QDir::listSeparator() + existingPyPath);
    if (!paths.pythonHome.isEmpty()) env.insert("PYTHONHOME", paths.pythonHome);
    proc.setProcessEnvironment(env);
    proc.setWorkingDirectory(toolsDir);

    QStringList pyArgs = {
        "-m", "cgplay.export.export_engine",
        srcMedia,
        outVideo,
        QString::number(fps, 'g', 17),
        QString::number(startFrame),
        QString::number(endFrame),
        QStringLiteral("20M"),
        codecId,
        // UI ranges and annotation frames are zero-based timeline positions,
        // even when sequence filenames start at 1001 or another source frame.
        QStringLiteral("--relative-frames"),
        QStringLiteral("--staging-dir"), staging.path()
    };
    if (!annoJson.isEmpty()) {
        pyArgs << "--annotations" << annoJson;
    }
    if (!colorFile.isEmpty()) pyArgs << QStringLiteral("--color-settings") << colorFile;
    proc.start(python, pyArgs, QIODevice::ReadOnly);

    if (!proc.waitForStarted(15000)) {
        s_lastExportError = QObject::tr("内置导出运行时启动失败。");
        qCritical() << "[ReviewExport] Failed to start Python engine";
        return false;
    }

    return runPythonExport(proc, progress, cancel);
}

// Public API to retrieve last export error for UI display
QString ReviewExport::lastError() { return s_lastExportError; }

// Checks whether a sentinel file (.ffmpeg_ok) exists, which is written
// once ffmpeg has been successfully downloaded and verified.
static bool _ffmpegSentinelExists()
{
    return QFileInfo::exists(
        QCoreApplication::applicationDirPath() + "/.ffmpeg_ok");
}

static void _writeFfmpegSentinel()
{
    QFile f(QCoreApplication::applicationDirPath() + "/.ffmpeg_ok");
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    // Write a simple version stamp so the file isn't empty
    f.write("1");
    f.close();
}

bool ReviewExport::hasFfmpeg()
{
    // Fast path: sentinel exists → skip process probe
    if (_ffmpegSentinelExists()) return true;

    QString path = locateFfmpeg();
    if (path.isEmpty()) return false;

    // Only probe if the exe size looks reasonable (> 1 MiB)
    QFileInfo fi(path);
    if (fi.size() < 1 * 1024 * 1024) return false;

    QProcess ffmpeg;
    ffmpeg.start(path, {"-version"}, QIODevice::ReadOnly);
    const bool ok = waitForBoundedProcess(ffmpeg, kProbeTimeoutMs).succeeded();

    // If probe succeeded, write sentinel so we never probe again
    if (ok) _writeFfmpegSentinel();
    return ok;
}

bool ReviewExport::ensureFfmpeg(QWidget* parentWidget)
{
    // Sentinel fast path — skip everything
    if (_ffmpegSentinelExists()) return true;
    if (hasFfmpeg()) return true;

    QString componentError;
    if (ComponentManager::instance().ensureComponent(
            QStringLiteral("ffmpeg"),
            parentWidget,
            true,
            &componentError)) {
        if (hasFfmpeg()) {
            return true;
        }
    }

    // Ask user
    auto reply = QMessageBox::question(parentWidget,
        QObject::tr("需要 ffmpeg"),
        QObject::tr("视频导出需要 ffmpeg，但未找到。\n\n"
                   "是否需要自动下载 ffmpeg (~40MB)？\n\n"
                   "下载后将放置在程序目录下。"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);

    if (reply != QMessageBox::Yes) return false;

    return _downloadFfmpeg(parentWidget);
}

bool ReviewExport::_downloadFfmpeg(QWidget* parentWidget)
{
    const QDir tempDir(QDir::tempPath());
    const QString tmpZip = tempDir.filePath(QStringLiteral("cgplay_ffmpeg.zip"));
    const QString extractDir = tempDir.filePath(QStringLiteral("cgplay_ffmpeg_extract"));
    QFile::remove(tmpZip);
    QDir(extractDir).removeRecursively();

    QString downloadError;
    const QStringList mirrors = {
        QStringLiteral("https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"),
        QStringLiteral("https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip")
    };
    if (!DownloadService::instance().downloadFileFromUrls(
            mirrors, tmpZip, parentWidget, &downloadError)) {
        QMessageBox::warning(parentWidget, QObject::tr("下载失败"),
            QObject::tr("所有下载源均失败：%1\n\n请手动下载 ffmpeg：\n\n"
                       "1. 访问 https://www.gyan.dev/ffmpeg/builds/\n"
                       "2. 下载 ffmpeg-release-essentials.zip\n"
                       "3. 解压后将 ffmpeg.exe 和 ffprobe.exe 复制到程序目录：\n   %2")
                .arg(downloadError, QDir::toNativeSeparators(_ffmpegDir())));
        return false;
    }

    QProcess extract;
    extract.start(
        QStringLiteral("powershell"),
        {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
            QStringLiteral("-Command"),
            QStringLiteral("Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
                .arg(QDir::toNativeSeparators(tmpZip), QDir::toNativeSeparators(extractDir))
        },
        QIODevice::ReadOnly);
    const ProcessOutcome extractOutcome = waitForInteractiveProcess(
        extract,
        parentWidget,
        QObject::tr("正在解压 ffmpeg..."),
        kExtractTimeoutMs);
    if (!extractOutcome.succeeded()) {
        QFile::remove(tmpZip);
        QDir(extractDir).removeRecursively();
        const QString detail = QString::fromUtf8(extractOutcome.standardError).trimmed();
        QMessageBox::warning(
            parentWidget,
            QObject::tr("解压失败"),
            detail.isEmpty() ? QObject::tr("ffmpeg 压缩包解压失败或超时。") : detail);
        return false;
    }

    // Find ffmpeg.exe and ffprobe.exe in extracted directory
    QString ffmpegExe, ffprobeExe;

    QDirIterator it(extractDir, {"ffmpeg.exe"}, QDir::Files,
                    QDirIterator::Subdirectories);
    if (it.hasNext()) ffmpegExe = it.next();

    QDirIterator it2(extractDir, {"ffprobe.exe"}, QDir::Files,
                     QDirIterator::Subdirectories);
    if (it2.hasNext()) ffprobeExe = it2.next();

    if (ffmpegExe.isEmpty()) {
        QMessageBox::warning(parentWidget, QObject::tr("解压失败"),
            QObject::tr("无法在下载的压缩包中找到 ffmpeg.exe"));
        QFile::remove(tmpZip);
        QDir(extractDir).removeRecursively();
        return false;
    }

    // Copy to program dir (overwrite if exists)
    QString destDir = _ffmpegDir();
    QFile::remove(destDir + "/ffmpeg.exe");   // remove old copy first
    QFile::remove(destDir + "/ffprobe.exe");
    bool ok = QFile::copy(ffmpegExe, destDir + "/ffmpeg.exe");
    if (!ffprobeExe.isEmpty())
        QFile::copy(ffprobeExe, destDir + "/ffprobe.exe");

    // Cleanup temp files
    QFile::remove(tmpZip);
    QDir(extractDir).removeRecursively();

    if (!ok) {
        QMessageBox::warning(parentWidget, QObject::tr("安装失败"),
            QObject::tr("无法复制 ffmpeg.exe 到程序目录。"));
        return false;
    }

    // Write sentinel so we never ask again
    _writeFfmpegSentinel();
    return true;
}

// ─── Clean Video Export ──────────────────────────────────────────────────────────

bool ReviewExport::exportVideoClean(const QString& srcMedia,
                                     const QString& outVideo,
                                     std::pair<int, int> frameRange,
                                     double fps,
                                     VideoExportCodec codec,
                                     std::function<void(double)> progress,
                                     const std::atomic_bool* cancel,
                                     const ComponentRuntimePaths& runtime,
                                     std::optional<ColorTransformSnapshot> colorSnapshot)
{
    if (fps <= 0) fps = detectFps(srcMedia);
    return exportWithPython(srcMedia, outVideo,
        frameRange.first, frameRange.second, fps, progress, cancel, codec, {}, runtime, std::move(colorSnapshot));
}

// Helper: draw annotations into a QImage at media resolution

// Helper: draw annotations into a QImage at media resolution
static void drawAnnotationsOnImage(QImage& img, const QVector<AnnotationItem>& anns,
                                    int /*imgW*/, int /*imgH*/)
{
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);

    for (const auto& ann : anns) {
        if (ann.points.isEmpty()) continue;

        double penW = qMax(2.0, img.width() / 720.0 * 3.0);
        QColor color = ann.color;

        switch (ann.type) {
        case AnnotationType::Arrow:
            if (ann.points.size() < 2) break;
            {
                QColor c(color);
                p.setPen(QPen(c, penW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.drawLine(ann.points[0], ann.points[1]);

                double arrowSz = qMax(10.0, img.width() / 720.0 * 16.0);
                QLineF line(ann.points[1], ann.points[0]);
                double angle = qAtan2(-line.dy(), line.dx());
                double a1 = angle + qDegreesToRadians(25.0);
                double a2 = angle - qDegreesToRadians(25.0);
                QPointF tip1(ann.points[1].x() - arrowSz * qCos(a1),
                             ann.points[1].y() + arrowSz * qSin(a1));
                QPointF tip2(ann.points[1].x() - arrowSz * qCos(a2),
                             ann.points[1].y() + arrowSz * qSin(a2));
                QPolygonF tri({ann.points[1], tip1, tip2});
                p.setBrush(c);
                p.setPen(Qt::NoPen);
                p.drawPolygon(tri);

                double dotR = qMax(3.0, img.width() / 720.0 * 5.0);
                p.drawEllipse(ann.points[0], dotR, dotR);
            }
            break;

        case AnnotationType::Rectangle:
            if (ann.points.size() < 2) break;
            {
                QColor fill(color);
                fill.setAlpha(40);
                p.setBrush(fill);
                p.setPen(QPen(color, penW, Qt::SolidLine));
                p.drawRect(QRectF(ann.points[0], ann.points[1]));
            }
            break;

        case AnnotationType::Circle:
            if (ann.points.isEmpty()) break;
            {
                double radius = (ann.points.size() >= 2)
                    ? QLineF(ann.points[0], ann.points[1]).length()
                    : qMax(5.0, img.width() / 720.0 * 14.0);
                QColor fill(color);
                fill.setAlpha(40);
                p.setBrush(fill);
                p.setPen(QPen(color, penW, Qt::SolidLine));
                p.drawEllipse(ann.points[0], radius, radius);

                // Cross hair
                double cross = qMax(5.0, img.width() / 720.0 * 6.0);
                p.setPen(QPen(color.darker(130), penW * 0.5));
                p.setBrush(Qt::NoBrush);
                p.drawLine(QPointF(ann.points[0].x() - cross, ann.points[0].y()),
                           QPointF(ann.points[0].x() + cross, ann.points[0].y()));
                p.drawLine(QPointF(ann.points[0].x(), ann.points[0].y() - cross),
                           QPointF(ann.points[0].x(), ann.points[0].y() + cross));
            }
            break;

        case AnnotationType::Text:
            if (ann.points.isEmpty()) break;
            {
                QString str = ann.comment.isEmpty() ? "T" : ann.comment;
                QFont font("Microsoft YaHei", qMax(11, static_cast<int>(img.width() / 720.0 * 14)));
                font.setBold(true);
                p.setFont(font);
                QFontMetrics fm(font);
                QRect textRect = fm.boundingRect(str);
                int pad = qMax(4, static_cast<int>(img.width() / 720.0 * 5));

                QRectF bg(ann.points[0].x(), ann.points[0].y() - textRect.height() - pad,
                          textRect.width() + pad * 2,
                          textRect.height() + pad * 2);
                QColor bgColor(color);
                bgColor.setAlpha(200);
                p.fillRect(bg, bgColor);
                p.setPen(Qt::white);
                p.drawText(QPointF(ann.points[0].x() + pad, ann.points[0].y() - pad), str);
            }
            break;

        case AnnotationType::FreeDraw:
            if (ann.points.size() < 2) break;
            {
                QPainterPath path;
                path.moveTo(ann.points[0]);
                for (int i = 1; i < ann.points.size(); ++i)
                    path.lineTo(ann.points[i]);
                p.setPen(QPen(color, penW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
            }
            break;

        case AnnotationType::Point:
            if (ann.points.isEmpty()) break;
            {
                double cross = qMax(5.0, img.width() / 720.0 * 8.0);
                p.setPen(QPen(color, penW, Qt::SolidLine));
                p.setBrush(Qt::NoBrush);
                p.drawLine(QPointF(ann.points[0].x() - cross, ann.points[0].y()),
                           QPointF(ann.points[0].x() + cross, ann.points[0].y()));
                p.drawLine(QPointF(ann.points[0].x(), ann.points[0].y() - cross),
                           QPointF(ann.points[0].x(), ann.points[0].y() + cross));
                double dotR = qMax(3.0, img.width() / 720.0 * 4.0);
                p.setPen(Qt::NoPen);
                p.setBrush(color);
                p.drawEllipse(ann.points[0], dotR, dotR);
            }
            break;

        default: break;
        }
    }
    p.end();
}

// ─── Annotated Video Export ──────────────────────────────────────────────────────

bool ReviewExport::exportVideoAnnotated(const QVector<AnnotationItem>& annotations,
                                const QString& srcMedia,
                                const QString& outVideo,
                                int mediaW, int mediaH,
                                 std::pair<int, int> frameRange,
                                 double fps,
                                 const tl::OCIOOptions& ocio,
                                 VideoExportCodec codec,
                                 std::function<void(double)> progress,
                                const std::atomic_bool* cancel,
                                const ComponentRuntimePaths& runtime,
                                std::optional<ColorTransformSnapshot> colorSnapshot)
{
    if (fps <= 0) fps = detectFps(srcMedia);
    if (annotations.isEmpty()) {
        s_lastExportError = QObject::tr("没有可导出的批注，已取消导出。");
        return false;
    }
    const auto isDrawable = [frameRange](const AnnotationItem& annotation) {
        if (annotation.frame < frameRange.first || annotation.frame > frameRange.second) {
            return false;
        }
        const int pointCount = annotation.points.size();
        const bool hasRequiredPoints = [&] {
            switch (annotation.type) {
            case AnnotationType::Arrow:
            case AnnotationType::Rectangle:
            case AnnotationType::FreeDraw: return pointCount >= 2;
            case AnnotationType::Circle:
            case AnnotationType::Text:
            case AnnotationType::Point: return pointCount >= 1;
            default: return false;
            }
        }();
        if (!hasRequiredPoints) {
            return false;
        }
        return std::all_of(annotation.points.cbegin(), annotation.points.cend(), [](const QPointF& point) {
            return qIsFinite(point.x()) && qIsFinite(point.y());
        });
    };
    if (std::none_of(annotations.cbegin(), annotations.cend(), isDrawable)) {
        s_lastExportError = QObject::tr("所选帧范围内没有有效批注，已取消导出以避免生成无批注视频。");
        return false;
    }

    QTemporaryFile annotationFile(
        QFileInfo(outVideo).absolutePath() + QStringLiteral("/.cgplay_annotations_XXXXXX.json"));
    annotationFile.setAutoRemove(true);
    if (!annotationFile.open()) {
        s_lastExportError = QObject::tr("无法创建临时批注数据文件，已取消视频导出。");
        return false;
    }
    const QString annoJson = annotationFile.fileName();
    annotationFile.close();
    {
        QJsonArray arr;
        for (const auto& a : annotations) {
            if (!isDrawable(a)) {
                continue;
            }
            QJsonObject o;
            o["frame"] = a.frame; o["type"] = (int)a.type;
            o["color"] = a.color.name(); o["author"] = a.author; o["comment"] = a.comment;
            QJsonArray pts;
            for (const auto& pt : a.points) { QJsonObject p; p["x"]=pt.x(); p["y"]=pt.y(); pts.append(p); }
            o["points"] = pts; arr.append(o);
        }
        const QByteArray payload = QJsonDocument(arr).toJson(QJsonDocument::Compact);
        QSaveFile file(annoJson);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(payload) != payload.size() ||
            !file.commit()) {
            s_lastExportError = QObject::tr("无法安全写入批注数据文件，已取消视频导出。\n%1")
                .arg(annoJson);
            return false;
        }
    }
    if (!colorSnapshot && ocio.enabled) {
        ColorTransformSnapshot legacy;
        legacy.enabled = true;
        legacy.configPath = QString::fromStdString(ocio.fileName);
        legacy.input = QString::fromStdString(ocio.input);
        legacy.display = QString::fromStdString(ocio.display);
        legacy.view = QString::fromStdString(ocio.view);
#if CGPLAY_HAS_TLRENDER
        legacy.look = QString::fromStdString(ocio.look);
#endif
        colorSnapshot = std::move(legacy);
    }
    const bool success = exportWithPython(srcMedia, outVideo,
        frameRange.first, frameRange.second, fps, progress, cancel, codec, annoJson, runtime, std::move(colorSnapshot));
    return success;
}

// ─── Linear → sRGB Gamma Correction ────────────────────────────────────────────
// 用于没有 OCIO 配置时的线性色彩空间（EXR/DPX）基础校正
static void applyLinearToSRGB(QImage& img)
{
    // gamma 2.2 = pow(x, 1/2.2) = pow(x, 0.4545)
    const double invGamma = 1.0 / 2.2;
    uchar* bits = img.bits();
    int lineBytes = img.bytesPerLine();
    for (int y = 0; y < img.height(); ++y) {
        uchar* line = bits + y * lineBytes;
        for (int x = 0; x < img.width(); ++x) {
            // QImage ARGB32 format: bytes are [B, G, R, A] (little-endian)
            for (int c = 0; c < 3; ++c) {
                double v = line[x * 4 + c] / 255.0;
                v = std::pow(std::max(0.0, v), invGamma);
                line[x * 4 + c] = (uchar)std::min(255, (int)(v * 255.0 + 0.5));
            }
        }
    }
}

// ─── OCIO CPU Processing ───────────────────────────────────────────────────────

bool ReviewExport::applyOcioToImage(QImage& img,
                                     const tl::OCIOOptions& ocio)
{
#ifdef CGPLAY_OCIO_ENABLED
    try {
        // OCIO options use std::string, not QString
        if (ocio.fileName.empty()) return false;

        OCIO::ConstConfigRcPtr config = OCIO::Config::CreateFromFile(
            ocio.fileName.c_str());
        if (!config) return false;

        // Create processor: input → display/view
        auto processor = config->getProcessor(
            ocio.input.c_str(),
            ocio.display.c_str(),
            ocio.view.c_str(),
            OCIO::TRANSFORM_DIR_FORWARD);

        if (!processor) {
            qWarning() << "[ReviewExport] OCIO: failed to create processor";
            return false;
        }

        // Apply to image pixels using OCIO v2 CPU processing
        OCIO::PackedImageDesc imgDesc(
            img.bits(),
            img.width(), img.height(),
            4,  // 4 channels (RGBA)
            OCIO::BIT_DEPTH_UINT8,
            sizeof(uchar),        // channel stride (packed)
            img.bytesPerLine(),   // row stride
            img.height());        // num lines

        auto cpuProcessor = processor->getDefaultCPUProcessor();
        cpuProcessor->apply(imgDesc);

        return true;
    }
    catch (const std::exception& e) {
        qWarning() << "[ReviewExport] OCIO error:" << e.what();
        return false;
    }
#else
    Q_UNUSED(img);
    Q_UNUSED(ocio);
    return false;
#endif
}

} // namespace cgplay
