#include "RecordingPanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <initializer_list>

namespace cgplay {
namespace {
void populateDevices(QComboBox* combo, const QJsonArray& values)
{
    const QString selected = combo->currentData().toString();
    const QSignalBlocker blocker(combo);
    combo->clear();
    for (const auto& value : values) {
        const auto item = value.toObject();
        const QString id = item.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || id == QStringLiteral("default")) continue;
        combo->addItem(item.value(QStringLiteral("label")).toString(), id);
    }
    combo->setCurrentIndex(combo->findData(selected));
}
QString durationText(qint64 nanoseconds)
{
    const qint64 seconds = qMax<qint64>(0, nanoseconds) / 1000000000;
    return QStringLiteral("%1:%2:%3").arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
} // namespace

RecordingPanel::RecordingPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("RecordingPanel"));
    auto* column = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    column->addLayout(form);
    _scope = new QComboBox(this); _scope->setObjectName(QStringLiteral("RecordingScope"));
    _scope->addItem(QStringLiteral("播放器画面"), QStringLiteral("player"));
    _scope->addItem(QStringLiteral("整个屏幕"), QStringLiteral("monitor"));
    _scope->addItem(QStringLiteral("指定窗口"), QStringLiteral("window"));
    form->addRow(QStringLiteral("录制范围"), _scope);
    _source = new QComboBox(this); _source->setObjectName(QStringLiteral("RecordingSource"));
    _source->setPlaceholderText(QStringLiteral("请选择要录制的来源"));
    form->addRow(QStringLiteral("来源"), _source);
    _sourceNote = new QLabel(this); _sourceNote->setWordWrap(true);
    _sourceNote->setObjectName(QStringLiteral("RecordingSourceNote")); column->addWidget(_sourceNote);
    _refresh = new QPushButton(QStringLiteral("刷新来源和声音设备"), this);
    _refresh->setObjectName(QStringLiteral("RecordingRefresh")); column->addWidget(_refresh);
    auto* format = new QFormLayout; column->addLayout(format);
    _resolution = new QComboBox(this); _resolution->setObjectName(QStringLiteral("RecordingResolution"));
    _resolution->addItem(QStringLiteral("1080p · 1920 × 1080"), QJsonObject{{"width",1920},{"height",1080}});
    _resolution->addItem(QStringLiteral("720p · 1280 × 720"), QJsonObject{{"width",1280},{"height",720}});
    format->addRow(QStringLiteral("画质"), _resolution);
    _fps = new QComboBox(this); _fps->setObjectName(QStringLiteral("RecordingFps"));
    _fps->addItem(QStringLiteral("30 FPS"), 30); _fps->addItem(QStringLiteral("60 FPS"), 60);
    format->addRow(QStringLiteral("帧率"), _fps);
    auto* sdr = new QLabel(QStringLiteral("H.264 / MP4 · SDR。HDR 屏幕请先关闭 HDR。"), this);
    sdr->setWordWrap(true); column->addWidget(sdr);
    auto* outputRow = new QHBoxLayout;
    _output = new QLineEdit(this); _output->setObjectName(QStringLiteral("RecordingOutput"));
    _output->setPlaceholderText(QStringLiteral("选择保存 MP4 的位置")); outputRow->addWidget(_output,1);
    _browse = new QPushButton(QStringLiteral("浏览…"),this); _browse->setObjectName(QStringLiteral("RecordingBrowse"));
    outputRow->addWidget(_browse); column->addLayout(outputRow);
    const auto addAudio = [&](const QString& title, const QString& prefix, QCheckBox*& enabled,
                              QComboBox*& device, QSlider*& gain, QCheckBox*& mute) {
        auto* group = new QGroupBox(title,this); auto* layout = new QVBoxLayout(group);
        enabled = new QCheckBox(QStringLiteral("录制") + title,group); enabled->setObjectName(prefix+QStringLiteral("Enabled"));
        layout->addWidget(enabled); device = new QComboBox(group); device->setObjectName(prefix+QStringLiteral("Device"));
        device->setPlaceholderText(QStringLiteral("请选择设备")); layout->addWidget(device);
        auto* row = new QHBoxLayout; gain = new QSlider(Qt::Horizontal,group);
        gain->setObjectName(prefix+QStringLiteral("Gain")); gain->setRange(0,200); gain->setValue(100);
        gain->setToolTip(QStringLiteral("录制音量 0–200%")); row->addWidget(gain,1);
        auto* amount = new QLabel(QStringLiteral("100%"),group); row->addWidget(amount);
        mute = new QCheckBox(QStringLiteral("静音"),group); mute->setObjectName(prefix+QStringLiteral("Mute")); row->addWidget(mute);
        layout->addLayout(row); column->addWidget(group);
        connect(enabled,&QCheckBox::toggled,this,[this]{ updateControls(); });
        connect(device,&QComboBox::currentIndexChanged,this,[this]{ updateControls(); });
        connect(gain,&QSlider::valueChanged,this,[this,amount](int value){ amount->setText(QString::number(value)+QLatin1Char('%')); publishAudio(); });
        connect(mute,&QCheckBox::toggled,this,[this]{ publishAudio(); });
    };
    addAudio(QStringLiteral("系统声音"),QStringLiteral("RecordingSystem"),_systemEnabled,_systemDevice,_systemGain,_systemMute);
    addAudio(QStringLiteral("麦克风"),QStringLiteral("RecordingMic"),_micEnabled,_micDevice,_micGain,_micMute);
    _status = new QLabel(QStringLiteral("尚未录制"),this); _status->setObjectName(QStringLiteral("RecordingStatus"));
    _status->setTextFormat(Qt::PlainText); _status->setWordWrap(true);
    _status->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);
    _status->setFocusPolicy(Qt::StrongFocus); column->addWidget(_status);
    _error = new QLabel(this); _error->setObjectName(QStringLiteral("RecordingError")); _error->setWordWrap(true); column->addWidget(_error);
    auto* actions = new QHBoxLayout;
    _start = new QPushButton(QStringLiteral("开始录制"),this); _start->setObjectName(QStringLiteral("RecordingStart"));
    _pause = new QPushButton(QStringLiteral("暂停"),this); _pause->setObjectName(QStringLiteral("RecordingPause"));
    _stop = new QPushButton(QStringLiteral("停止并保存"),this); _stop->setObjectName(QStringLiteral("RecordingStop"));
    actions->addWidget(_start); actions->addWidget(_pause); actions->addWidget(_stop); column->addLayout(actions);
    _recover = new QPushButton(QStringLiteral("恢复未完成的录制…"),this); _recover->setObjectName(QStringLiteral("RecordingRecover")); column->addWidget(_recover);
    auto* recoveryNote = new QLabel(QStringLiteral("请选择尚不存在的新 MP4 文件，录屏不会覆盖已有文件。录制中先保留可恢复的 MKV；正常停止后生成 MP4。发生异常时请保留录制文件和恢复记录。"),this);
    recoveryNote->setWordWrap(true); column->addWidget(recoveryNote); column->addStretch(1);
    connect(_scope,&QComboBox::currentIndexChanged,this,[this]{ updateSources(); });
    connect(_source,&QComboBox::currentIndexChanged,this,[this]{ updateControls(); });
    connect(_output,&QLineEdit::textChanged,this,[this]{ updateControls(); });
    connect(_refresh,&QPushButton::clicked,this,&RecordingPanel::refreshRequested);
    connect(_browse,&QPushButton::clicked,this,&RecordingPanel::chooseOutput);
    connect(_recover,&QPushButton::clicked,this,&RecordingPanel::chooseRecovery);
    connect(_start,&QPushButton::clicked,this,[this]{
        const QString error = selectionError();
        if (isBusy() || !error.isEmpty()) { _error->setText(error); return; }
        const auto settings = currentSettings();
        setServiceState({{"state", "Preparing"}}); emit startRequested(settings);
    });
    connect(_pause,&QPushButton::clicked,this,[this]{
        if (!_pauseAwaitingState.isEmpty() || !_canPause || (_state!=QStringLiteral("Recording") && _state!=QStringLiteral("Paused"))) return;
        _pauseAwaitingState=_state==QStringLiteral("Recording") ? QStringLiteral("Paused") : QStringLiteral("Recording");
        _pause->setEnabled(false); emit pauseRequested(_state==QStringLiteral("Recording"));
    });
    connect(_stop,&QPushButton::clicked,this,[this]{
        if (!isBusy() || _state==QStringLiteral("Stopping") || _state==QStringLiteral("Finalizing")) return;
        setServiceState({{"state","Stopping"}}); emit stopRequested();
    });
    updateSources();
}

void RecordingPanel::setCatalog(const QJsonObject& catalog)
{
    if (isBusy()) return;
    _catalog = catalog; _catalogError = catalog.value(QStringLiteral("error")).toString();
    _error->setText(_catalogError);
    populateDevices(_systemDevice,catalog.value(QStringLiteral("systemAudio")).toArray());
    populateDevices(_micDevice,catalog.value(QStringLiteral("microphones")).toArray()); updateSources();
}
void RecordingPanel::setPlayerSelection(const QJsonObject& value) { _playerSelection=value; updateControls(); }
bool RecordingPanel::isBusy() const
{
    return _backendBusy || _state==QStringLiteral("Preparing") || _state==QStringLiteral("Starting") ||
        _state==QStringLiteral("Recording") || _state==QStringLiteral("Paused") ||
        _state==QStringLiteral("Stopping") || _state==QStringLiteral("Finalizing");
}
void RecordingPanel::updateSources()
{
    const QString kind = _scope->currentData().toString();
    const auto previous = _source->currentData().toJsonObject();
    const QSignalBlocker blocker(_source); _source->clear();
    const auto entries = _catalog.value(kind==QStringLiteral("monitor") ? QStringLiteral("monitors") : QStringLiteral("windows")).toArray();
    int selection=-1;
    for (const auto& item : entries) {
        const auto value=item.toObject(); const auto plan=value.value(QStringLiteral("plan")).toObject();
        if (plan.isEmpty()) continue;
        _source->addItem(value.value(QStringLiteral("label")).toString(), plan);
        if (plan==previous) selection=_source->count()-1;
    }
    _source->setCurrentIndex(selection); _source->setVisible(kind!=QStringLiteral("player"));
    _sourceNote->setText(kind==QStringLiteral("player")
        ? QStringLiteral("录制当前播放器显示区域，字幕、批注和浮层会一同录制。SDR 画面不再重复应用 OCIO。录制中切全屏或跨缩放比例屏幕导致尺寸变化时会停止并保留录像，请重新开始。")
        : QStringLiteral("只录制明确选择的屏幕或窗口。来源关闭、最小化或失效时会停止并保留可恢复文件。"));
    updateControls();
}
QJsonObject RecordingPanel::currentSettings() const
{
    const QString kind=_scope->currentData().toString();
    QJsonObject result=kind==QStringLiteral("player") ? _playerSelection : _source->currentData().toJsonObject();
    result.insert(QStringLiteral("source"),kind);
    const auto size=_resolution->currentData().toJsonObject();
    result.insert(QStringLiteral("width"),size.value(QStringLiteral("width")));
    result.insert(QStringLiteral("height"),size.value(QStringLiteral("height")));
    result.insert(QStringLiteral("fps"),_fps->currentData().toInt());
    result.insert(QStringLiteral("output"),_output->text().trimmed());
    result.insert(QStringLiteral("systemEnabled"),_systemEnabled->isChecked());
    result.insert(QStringLiteral("micEnabled"),_micEnabled->isChecked());
    result.insert(QStringLiteral("systemDeviceId"),_systemDevice->currentData().toString());
    result.insert(QStringLiteral("micDeviceId"),_micDevice->currentData().toString());
    result.insert(QStringLiteral("systemGain"),_systemGain->value()/100.);
    result.insert(QStringLiteral("micGain"),_micGain->value()/100.);
    result.insert(QStringLiteral("systemMute"),_systemMute->isChecked());
    result.insert(QStringLiteral("micMute"),_micMute->isChecked());
    return result;
}
QString RecordingPanel::selectionError() const
{
    if (!_catalogError.isEmpty()) return _catalogError;
    const QString kind=_scope->currentData().toString();
    if ((kind==QStringLiteral("player") && _playerSelection.isEmpty()) ||
        (kind!=QStringLiteral("player") && _source->currentIndex()<0)) return QStringLiteral("请选择有效的录制来源。需要时可刷新来源列表。");
    if (_systemEnabled->isChecked() && _systemDevice->currentIndex()<0) return QStringLiteral("请选择系统声音设备。");
    if (_micEnabled->isChecked() && _micDevice->currentIndex()<0) return QStringLiteral("请选择麦克风设备。");
    const QFileInfo file(_output->text().trimmed());
    if (_output->text().trimmed().isEmpty() || !file.isAbsolute() || file.suffix().compare(QStringLiteral("mp4"),Qt::CaseInsensitive)!=0)
        return QStringLiteral("请选择完整的 MP4 保存路径。");
    if (file.exists()) return QStringLiteral("保存位置已经有文件，请为录屏选择一个新文件名。");
    return {};
}
void RecordingPanel::updateControls()
{
    const bool busy=isBusy();
    for (QWidget* widget : std::initializer_list<QWidget*>{_scope,_source,_resolution,_fps,_output,_browse,_refresh,_systemEnabled,_micEnabled,_recover})
        widget->setEnabled(!busy);
    _systemDevice->setEnabled(!busy && _systemEnabled->isChecked());
    _micDevice->setEnabled(!busy && _micEnabled->isChecked());
    const bool audioActive=!busy || _state==QStringLiteral("Recording") || _state==QStringLiteral("Paused");
    _systemGain->setEnabled(audioActive && _systemEnabled->isChecked()); _systemMute->setEnabled(audioActive && _systemEnabled->isChecked());
    _micGain->setEnabled(audioActive && _micEnabled->isChecked()); _micMute->setEnabled(audioActive && _micEnabled->isChecked());
    const QString error=selectionError(); _start->setEnabled(!busy && error.isEmpty()); _start->setToolTip(error);
    _pause->setEnabled(_canPause && _pauseAwaitingState.isEmpty() && (_state==QStringLiteral("Recording") || _state==QStringLiteral("Paused")));
    _pause->setText(_state==QStringLiteral("Paused") ? QStringLiteral("继续录制") : QStringLiteral("暂停"));
    _stop->setEnabled(busy && _state!=QStringLiteral("Stopping") && _state!=QStringLiteral("Finalizing"));
}
void RecordingPanel::setServiceState(const QJsonObject& value)
{
    _state=value.value(QStringLiteral("state")).toString(_state);
    _backendBusy=value.value(QStringLiteral("busy")).toBool(false);
    _canPause=value.value(QStringLiteral("canPause")).toBool(_state==QStringLiteral("Recording") || _state==QStringLiteral("Paused"));
    if (_state==_pauseAwaitingState || !value.value(QStringLiteral("error")).toString().isEmpty() || !isBusy())
        _pauseAwaitingState.clear();
    const QMap<QString,QString> labels{{"Idle",QStringLiteral("尚未录制")},{"Preparing",QStringLiteral("正在准备")},
        {"Starting",QStringLiteral("正在启动")},{"Recording",QStringLiteral("正在录制")},{"Paused",QStringLiteral("已暂停")},
        {"Stopping",QStringLiteral("正在停止")},{"Finalizing",QStringLiteral("正在保存 MP4")},
        {"Completed",QStringLiteral("已保存")},{"Recoverable",QStringLiteral("录制已停止，可恢复")},{"Failed",QStringLiteral("录制失败")}};
    const QString descriptor=value.value(QStringLiteral("sessionDescriptorPath")).toString();
    const QString mkv=value.value(QStringLiteral("mkvPath")).toString();
    if (!descriptor.isEmpty()) _recoveryDescriptorPath=descriptor;
    if (!mkv.isEmpty()) _recoveryMkvPath=mkv;
    QString status=labels.value(_state,_state)+QStringLiteral("  ")+durationText(value.value(QStringLiteral("durationNs")).toVariant().toLongLong());
    QString recoveryPaths;
    if (!_recoveryDescriptorPath.isEmpty()) recoveryPaths+=QStringLiteral("恢复记录：%1").arg(_recoveryDescriptorPath);
    if (!_recoveryMkvPath.isEmpty()) recoveryPaths+=(recoveryPaths.isEmpty()?QString():QStringLiteral("\n"))+QStringLiteral("录像文件：%1").arg(_recoveryMkvPath);
    if (_state==QStringLiteral("Recoverable") && !recoveryPaths.isEmpty()) status+=QLatin1Char('\n')+recoveryPaths;
    _status->setText(status);
    _status->setToolTip(_state==QStringLiteral("Recoverable") && !recoveryPaths.isEmpty()
        ? recoveryPaths : value.value(QStringLiteral("outputPath")).toString());
    _recover->setToolTip(recoveryPaths);
    const QString error=value.value(QStringLiteral("error")).toString();
    _error->setText(error.contains(QStringLiteral("player-geometry-refresh-required"))
        ? QStringLiteral("录制区域尺寸发生变化，已停止并保留录像；请重新开始。") : error);
    updateControls();
}
void RecordingPanel::publishAudio()
{
    if (_state!=QStringLiteral("Recording") && _state!=QStringLiteral("Paused")) return;
    emit audioChanged({{"systemGain",_systemGain->value()/100.},{"micGain",_micGain->value()/100.},
                       {"systemMute",_systemMute->isChecked()},{"micMute",_micMute->isChecked()}});
}
void RecordingPanel::chooseOutput()
{
    const bool background=qApp->property("cgplay.automationBackground").toBool();
    const QString path=background ? property("cgplay.recordingInjectedOutput").toString()
        : QFileDialog::getSaveFileName(this,QStringLiteral("保存录屏（新文件）"),_output->text(),QStringLiteral("MP4 视频 (*.mp4)"),nullptr,QFileDialog::DontConfirmOverwrite);
    if (!path.isEmpty()) _output->setText(path);
}
void RecordingPanel::chooseRecovery()
{
    if (isBusy()) return;
    const QFileInfo destination(_output->text().trimmed());
    if (!destination.isAbsolute() || destination.suffix().compare(QStringLiteral("mp4"),Qt::CaseInsensitive)!=0 || destination.exists()) {
        _error->setText(QStringLiteral("恢复前请先选择一个尚不存在的新 MP4 保存路径。")); return;
    }
    const bool background=qApp->property("cgplay.automationBackground").toBool();
    // This is only a suggested picker location. The recording service validates
    // the selected envelope; the panel never opens or follows recovery metadata.
    setProperty("cgplay.recordingRecoveryDialogInitial",_recoveryDescriptorPath);
    const QString path=background ? property("cgplay.recordingInjectedRecovery").toString()
        : QFileDialog::getOpenFileName(this,QStringLiteral("选择录制恢复记录"),_recoveryDescriptorPath,QStringLiteral("录制恢复记录 (*.json)"));
    if (!path.isEmpty()) {
        setServiceState({{"state","Preparing"}});
        emit recoverRequested(path,_output->text().trimmed());
    }
}

} // namespace cgplay
