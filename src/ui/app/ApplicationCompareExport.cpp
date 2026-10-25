#include "Application.h"
#include "ApplicationPrivate.h"
#include "annotation/CompareExportSnapshot.h"
#include "annotation/ReviewExport.h"
#include "annotation/VideoExportTask.h"
#include "playback/api/IPlaybackService.h"
#include "ocio/OcioManager.h"
#include "viewer/CompareToolbar.h"
#include "viewer/TlViewport.h"
#include "viewer/ViewerWidget.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QVBoxLayout>

namespace cgplay {
namespace {
void compareExportError(QWidget* owner, const QString& error)
{
    owner->setProperty("cgplay.compareExportError", error);
    if (qApp->property("cgplay.automationBackground").toBool()) return;
    auto* message = new QMessageBox(QMessageBox::Warning, QStringLiteral("导出 A/B 对比视频"),
        error, QMessageBox::Ok, owner);
    message->setAttribute(Qt::WA_DeleteOnClose);
    message->open();
}
void disableAudioItem(QComboBox* combo, int index, const QString& explanation)
{
    if (auto* model = qobject_cast<QStandardItemModel*>(combo->model())) {
        if (auto* item = model->item(index)) {
            item->setEnabled(false);
            item->setToolTip(explanation.trimmed().isEmpty()
                ? QStringLiteral("该来源没有可导出的内嵌音轨。") : explanation);
        }
    }
}
} // namespace

bool MainWindow::_exportCompareVideo()
{
    if (property("cgplay.compareExportBusy").toBool() || isVideoExportTaskRunning(this)) return false;
    if (!_p->playbackCtrl || !_p->compareBar || !_p->ocioManager || !_p->viewer || !_p->viewer->viewport()) {
        compareExportError(this, QStringLiteral("请先载入 A 和 B 镜头。"));
        return false;
    }
    QString error;
    QJsonObject captured = captureCompareExportInputs(*_p->playbackCtrl, *_p->compareBar,
        *_p->ocioManager, *_p->viewer->viewport(), &error);
    if (captured.isEmpty()) {
        compareExportError(this, error);
        return false;
    }
    setProperty("cgplay.compareExportBusy", true);
    setProperty("cgplay.compareExportError", QString());
    setProperty("cgplay.compareExportResult", QVariant());
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("CompareExportDialog"));
    dialog->setWindowTitle(QStringLiteral("导出 A/B 对比视频"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (testAttribute(Qt::WA_DontShowOnScreen)) dialog->setAttribute(Qt::WA_DontShowOnScreen);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->resize(620, 500);
    auto* column = new QVBoxLayout(dialog);
    auto* form = new QFormLayout;
    column->addLayout(form);
    const QJsonObject sources = captured.value(QStringLiteral("sources")).toObject();
    const QJsonObject a = sources.value(QStringLiteral("a")).toObject();
    const QJsonObject b = sources.value(QStringLiteral("b")).toObject();
    const QJsonObject options = captured.value(QStringLiteral("compare")).toObject();

    auto* modes = new QComboBox(dialog);
    modes->setObjectName(QStringLiteral("CompareExportMode"));
    const QList<QPair<QString, QString>> modeItems{
        {QStringLiteral("A"), QStringLiteral("a")}, {QStringLiteral("B"), QStringLiteral("b")},
        {QStringLiteral("划像"), QStringLiteral("wipe")}, {QStringLiteral("叠加"), QStringLiteral("overlay")},
        {QStringLiteral("差异"), QStringLiteral("difference")}, {QStringLiteral("左右"), QStringLiteral("horizontal")},
        {QStringLiteral("上下"), QStringLiteral("vertical")}, {QStringLiteral("平铺"), QStringLiteral("tile")}};
    for (const auto& item : modeItems) modes->addItem(item.first, item.second);
    modes->setCurrentIndex(modes->findData(options.value(QStringLiteral("mode")).toString()));
    form->addRow(QStringLiteral("对比方式"), modes);
    auto* ranges = new QComboBox(dialog);
    ranges->setObjectName(QStringLiteral("CompareExportRange"));
    ranges->addItem(QStringLiteral("A 全长"), QStringLiteral("full"));
    ranges->addItem(QStringLiteral("A 入出点（包含末帧）"), QStringLiteral("inout"));
    const QJsonObject selection = captured.value(QStringLiteral("selection")).toObject();
    if (!selection.value(QStringLiteral("hasInOut")).toBool())
        disableAudioItem(ranges, 1, QStringLiteral("请先设置有效的 A 入点和出点。"));
    form->addRow(QStringLiteral("范围"), ranges);
    auto* fps = new QLabel(QString::number(_p->playbackCtrl->fps(), 'g', 12) + QStringLiteral(" FPS / CFR"), dialog);
    fps->setObjectName(QStringLiteral("CompareExportFps"));
    form->addRow(QStringLiteral("A 源帧率"), fps);
    form->addRow(QStringLiteral("时间对齐"), new QLabel(options.value(QStringLiteral("time")).toString() == QStringLiteral("absolute")
        ? QStringLiteral("绝对时间（当前播放器）") : QStringLiteral("相对起点（当前播放器）"), dialog));

    auto* wipe = new QWidget(dialog);
    auto* wipeRow = new QHBoxLayout(wipe);
    wipeRow->setContentsMargins(0, 0, 0, 0);
    const QJsonArray center = options.value(QStringLiteral("wipeCenter")).toArray();
    auto makeSpin = [dialog](const QString& name, double low, double high, int decimals, double value) {
        auto* spin = new QDoubleSpinBox(dialog);
        spin->setObjectName(name); spin->setRange(low, high); spin->setDecimals(decimals); spin->setValue(value);
        return spin;
    };
    auto* wipeX = makeSpin(QStringLiteral("CompareExportWipeX"), 0, 1, 3, center.at(0).toDouble());
    auto* wipeY = makeSpin(QStringLiteral("CompareExportWipeY"), 0, 1, 3, center.at(1).toDouble());
    auto* angle = makeSpin(QStringLiteral("CompareExportWipeRotation"), -180, 180, 1,
        options.value(QStringLiteral("wipeRotation")).toDouble());
    wipeRow->addWidget(new QLabel(QStringLiteral("X"), wipe)); wipeRow->addWidget(wipeX);
    wipeRow->addWidget(new QLabel(QStringLiteral("Y"), wipe)); wipeRow->addWidget(wipeY);
    wipeRow->addWidget(new QLabel(QStringLiteral("角度"), wipe)); wipeRow->addWidget(angle);
    form->addRow(QStringLiteral("划像位置"), wipe);
    auto* overlay = makeSpin(QStringLiteral("CompareExportOverlay"), 0, 1, 3,
        options.value(QStringLiteral("overlay")).toDouble());
    form->addRow(QStringLiteral("叠加 A 比例"), overlay);
    const auto modeControls = [modes, wipe, overlay] {
        wipe->setEnabled(modes->currentData().toString() == QStringLiteral("wipe"));
        overlay->setEnabled(modes->currentData().toString() == QStringLiteral("overlay"));
    };
    connect(modes, &QComboBox::currentIndexChanged, dialog, modeControls); modeControls();

    auto* audio = new QComboBox(dialog);
    audio->setObjectName(QStringLiteral("CompareExportAudio"));
    audio->addItem(QStringLiteral("无声音"), QStringLiteral("none"));
    audio->addItem(QStringLiteral("A 的源音轨"), QStringLiteral("a"));
    audio->addItem(QStringLiteral("B 的源音轨"), QStringLiteral("b"));
    if (!a.value(QStringLiteral("audioAvailable")).toBool()) disableAudioItem(audio, 1,
        a.value(QStringLiteral("audioUnavailableReason")).toString(QStringLiteral("A 没有可导出的音轨。")));
    else audio->setCurrentIndex(1);
    if (!b.value(QStringLiteral("audioAvailable")).toBool()) disableAudioItem(audio, 2,
        b.value(QStringLiteral("audioUnavailableReason")).toString(QStringLiteral("B 没有可导出的音轨。")));
    form->addRow(QStringLiteral("声音"), audio);

    const auto colorA = a.value(QStringLiteral("color")).toObject();
    const auto colorB = b.value(QStringLiteral("color")).toObject();
    const bool ocio = colorA.value(QStringLiteral("mode")).toString() == QStringLiteral("ocio");
    const QString encoding = ocio ? colorA.value(QStringLiteral("outputEncoding")).toString() : QStringLiteral("srgb");
    auto* colorsA = new QComboBox(dialog); colorsA->setObjectName(QStringLiteral("CompareExportInputA"));
    auto* colorsB = new QComboBox(dialog); colorsB->setObjectName(QStringLiteral("CompareExportInputB"));
    const QStringList spaces = ocio ? _p->ocioManager->availableColorSpaces() : QStringList{QStringLiteral("直接 RGB")};
    for (auto* combo : {colorsA, colorsB}) { combo->addItems(spaces); combo->setEnabled(ocio); }
    if (ocio) {
        colorsA->setCurrentText(colorA.value(QStringLiteral("input")).toString());
        colorsB->setCurrentText(colorB.value(QStringLiteral("input")).toString());
    }
    form->addRow(QStringLiteral("A 输入空间"), colorsA);
    form->addRow(QStringLiteral("B 输入空间"), colorsB);
    auto* colorDescription = new QLabel(ocio
        ? QStringLiteral("固定当前显示/视图，输出 %1 SDR；两路颜色设置独立保存。").arg(encoding)
        : QStringLiteral("颜色管理已关闭：保留当前 RGB 数值，以 sRGB SDR 输出。"), dialog);
    colorDescription->setWordWrap(true); form->addRow(colorDescription);

    auto* codecs = new QComboBox(dialog); codecs->setObjectName(QStringLiteral("CompareExportCodec"));
    codecs->addItem(QStringLiteral("H.264 / MP4"), QStringLiteral("h264"));
    codecs->addItem(QStringLiteral("H.265 / MP4"), QStringLiteral("h265"));
    codecs->addItem(QStringLiteral("ProRes 422 HQ / MOV"), QStringLiteral("prores_hq"));
    form->addRow(QStringLiteral("编码"), codecs);
    const QFileInfo defaultSource(a.value(QStringLiteral("originalPath")).toString().isEmpty()
        ? a.value(QStringLiteral("path")).toString() : a.value(QStringLiteral("originalPath")).toString());
    auto* output = new QLineEdit(defaultSource.absolutePath() + QLatin1Char('/') +
        defaultSource.completeBaseName() + QStringLiteral("_AB.mp4"), dialog);
    output->setObjectName(QStringLiteral("CompareExportOutput"));
    auto* browse = new QPushButton(QStringLiteral("浏览…"), dialog);
    auto* outputRow = new QHBoxLayout; outputRow->addWidget(output); outputRow->addWidget(browse);
    form->addRow(QStringLiteral("输出文件"), outputRow);
    connect(codecs, &QComboBox::currentIndexChanged, dialog, [codecs, output] {
        const QFileInfo current(output->text());
        const QString suffix = codecs->currentData().toString() == QStringLiteral("prores_hq")
            ? QStringLiteral("mov") : QStringLiteral("mp4");
        if (current.suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) == 0 ||
            current.suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) == 0)
            output->setText(current.absolutePath() + QLatin1Char('/') + current.completeBaseName() + QLatin1Char('.') + suffix);
    });
    connect(browse, &QPushButton::clicked, dialog, [dialog, output, codecs] {
        const QString filter = codecs->currentData().toString() == QStringLiteral("prores_hq")
            ? QStringLiteral("MOV (*.mov)") : QStringLiteral("MP4 (*.mp4)");
        auto* picker = new QFileDialog(dialog, QStringLiteral("保存 A/B 对比视频"), output->text(), filter);
        picker->setAttribute(Qt::WA_DeleteOnClose);
        picker->setAcceptMode(QFileDialog::AcceptSave);
        picker->setFileMode(QFileDialog::AnyFile);
        QObject::connect(picker, &QFileDialog::fileSelected, output, &QLineEdit::setText);
        picker->open();
    });
    auto* note = new QLabel(QStringLiteral("保持两路比例，按 A 帧率导出不透明视频。B 超出范围时留黑；活动 LUT 或未支持的显示效果会明确报错。"), dialog);
    note->setWordWrap(true); column->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("开始导出"));
    buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("CompareExportStart"));
    column->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QObject::destroyed, this, [this] {
        if (!property("cgplay.compareExportRunning").toBool()) setProperty("cgplay.compareExportBusy", false);
    });
    connect(buttons, &QDialogButtonBox::accepted, dialog,
        [this, captured, dialog, modes, ranges, selection, wipeX, wipeY, angle, overlay,
         colorsA, colorsB, ocio, encoding, output, codecs, audio] {
        if (output->text().trimmed().isEmpty()) { compareExportError(this, QStringLiteral("请选择输出文件。")); return; }
        if (codecs->currentData().toString() == QStringLiteral("prores_hq") &&
            QFileInfo(output->text().trimmed()).suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) != 0) {
            compareExportError(this, QStringLiteral("ProRes 422 HQ 必须保存为 MOV，请将输出扩展名改为 .mov。"));
            return;
        }
        QJsonObject job = captured;
        QString layoutError;
        if (!updateCompareExportLayout(&job, modes->currentData().toString(), &layoutError)) {
            compareExportError(this, layoutError); return;
        }
        auto comparison = job.value(QStringLiteral("compare")).toObject();
        comparison.insert(QStringLiteral("wipeCenter"), QJsonArray{wipeX->value(), wipeY->value()});
        comparison.insert(QStringLiteral("wipeRotation"), angle->value());
        comparison.insert(QStringLiteral("overlay"), overlay->value());
        job.insert(QStringLiteral("compare"), comparison);
        if (ranges->currentData().toString() == QStringLiteral("inout"))
            job.insert(QStringLiteral("range"), QJsonObject{{QStringLiteral("start"), selection.value(QStringLiteral("in"))},
                {QStringLiteral("end"), selection.value(QStringLiteral("out"))}});
        QJsonObject frozenSources = job.value(QStringLiteral("sources")).toObject();
        for (const auto& item : {qMakePair(QStringLiteral("a"), colorsA), qMakePair(QStringLiteral("b"), colorsB)}) {
            auto source = frozenSources.value(item.first).toObject();
            auto color = source.value(QStringLiteral("color")).toObject();
            if (ocio) color.insert(QStringLiteral("input"), item.second->currentText());
            source.insert(QStringLiteral("color"), color); frozenSources.insert(item.first, source);
        }
        job.insert(QStringLiteral("sources"), frozenSources);
        const QString outPath = QFileInfo(output->text().trimmed()).absoluteFilePath();
        job.insert(QStringLiteral("output"), QJsonObject{{QStringLiteral("path"), outPath},
            {QStringLiteral("codec"), codecs->currentData().toString()}, {QStringLiteral("bitrate"), QStringLiteral("20M")},
            {QStringLiteral("encoding"), encoding}});
        job.insert(QStringLiteral("audio"), QJsonObject{{QStringLiteral("source"), audio->currentData().toString()},
            {QStringLiteral("offsetSeconds"), audio->currentData().toString() == QStringLiteral("a")
                ? captured.value(QStringLiteral("aAudioOffsetSeconds")) : QJsonValue(QStringLiteral("0"))}});
        ComponentRuntimePaths runtime;
        QString runtimeError;
        const QPointer<MainWindow> guardedOwner(this);
        const QPointer<QDialog> guardedDialog(dialog);
        const QPointer<QPushButton> guardedStart(dialog->findChild<QPushButton*>(QStringLiteral("CompareExportStart")));
        if (guardedStart && !guardedStart->isEnabled()) return;
        if (guardedStart) guardedStart->setEnabled(false);
        if (!ReviewExport::prepareVideoExportRuntime(this, &runtime, &runtimeError)) {
            if (guardedStart) guardedStart->setEnabled(true);
            if (guardedOwner) compareExportError(guardedOwner, runtimeError);
            return;
        }
        // Component preparation may process GUI events. The owner or form can
        // have closed while a missing runtime was being prepared.
        if (!guardedOwner || !guardedDialog || !guardedDialog->isVisible()) return;
        setProperty("cgplay.compareExportLastJob", job.toVariantMap());
        setProperty("cgplay.compareExportRunning", true);
        dialog->accept();
        const bool accepted = startVideoExportTask(this, QStringLiteral("正在导出 A/B 对比视频…"),
            [job, runtime](const std::atomic_bool* canceled, const std::function<void(double)>& progress) {
                const bool ok = ReviewExport::exportVideoCompare(job, runtime, progress, canceled);
                return qMakePair(ok, ReviewExport::lastError());
            }, [this, outPath](const VideoExportTaskResult& result, bool canceled) {
                setProperty("cgplay.compareExportRunning", false);
                setProperty("cgplay.compareExportBusy", false);
                setProperty("cgplay.compareExportResult", QVariantMap{{QStringLiteral("success"), result.first},
                    {QStringLiteral("canceled"), canceled}, {QStringLiteral("path"), outPath},
                    {QStringLiteral("error"), result.second}});
                if (!result.first && !canceled) compareExportError(this, result.second);
                else if (result.first && statusBar()) statusBar()->showMessage(QStringLiteral("A/B 对比视频已导出：%1").arg(outPath), 5000);
            });
        if (!accepted) {
            const QString reason = QStringLiteral("当前窗口已有视频导出任务，请等待完成或先取消该任务。");
            setProperty("cgplay.compareExportRunning", false);
            setProperty("cgplay.compareExportBusy", false);
            setProperty("cgplay.compareExportResult", QVariantMap{{QStringLiteral("success"), false},
                {QStringLiteral("canceled"), false}, {QStringLiteral("path"), outPath}, {QStringLiteral("error"), reason}});
            compareExportError(this, reason);
        }
    });
    dialog->open();
    return true;
}
} // namespace cgplay
