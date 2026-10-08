#include "PreviewWindow.h"

#include "QuickLookDebug.h"
#include "cache/CacheManager.h"
#include "ocio/OcioManager.h"
#include "playback/PlaybackController.h"
#include "viewer/ViewerWidget.h"

#include <QFileInfo>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <windows.h>

namespace cgplay::quicklook {

namespace {

class PreviewSeekSlider : public QSlider
{
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && orientation() == Qt::Horizontal && maximum() > minimum()) {
            QStyleOptionSlider option;
            initStyleOption(&option);
            const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
            const int sliderMin = groove.x();
            const int sliderMax = groove.right() - handle.width() + 1;
            const int pos = qBound(sliderMin, static_cast<int>(event->position().x()) - handle.width() / 2, sliderMax);
            const int value = QStyle::sliderValueFromPosition(
                minimum(),
                maximum(),
                pos - sliderMin,
                qMax(1, sliderMax - sliderMin),
                option.upsideDown);
            setValue(value);
            Q_EMIT sliderMoved(value);
            Q_EMIT sliderReleased();
            event->accept();
            return;
        }
        QSlider::mousePressEvent(event);
    }
};

class PreviewVolumeSlider : public QSlider
{
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && orientation() == Qt::Horizontal && maximum() > minimum()) {
            QStyleOptionSlider option;
            initStyleOption(&option);
            const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
            const int sliderMin = groove.x();
            const int sliderMax = groove.right() - handle.width() + 1;
            const int pos = qBound(sliderMin, static_cast<int>(event->position().x()) - handle.width() / 2, sliderMax);
            const int value = QStyle::sliderValueFromPosition(
                minimum(),
                maximum(),
                pos - sliderMin,
                qMax(1, sliderMax - sliderMin),
                option.upsideDown);
            setValue(value);
            event->accept();
            return;
        }
        QSlider::mousePressEvent(event);
    }
};

} // namespace

PreviewWindow::PreviewWindow(
    std::shared_ptr<CacheManager> cache,
    std::shared_ptr<OcioManager> ocio,
    QWidget* parent)
    : QWidget(parent)
    , _cache(std::move(cache))
    , _ocio(std::move(ocio))
{
    setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_StyledBackground, false);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(560, 360);
    resize(1120, 700);

    _playback = std::make_shared<PlaybackController>(_cache, _ocio, this);
    connect(_playback.get(), &PlaybackController::fileOpened, this, [](const QString& path) {
        logQuickLook(QStringLiteral("PreviewWindow::fileOpened signal %1").arg(path));
    });
#if CGPLAY_HAS_TLRENDER
    connect(_playback.get(), &PlaybackController::playerReady, this, [this](const std::shared_ptr<tl::Player>& player) {
        logQuickLook(QStringLiteral("PreviewWindow::playerReady signal"));
        if (player && _autoPlayPending && _playback) {
            _autoPlayPending = false;
            logQuickLook(QStringLiteral("PreviewWindow::playerReady autoPlay"));
            _playback->play();
        }
    });
#endif
    connect(_playback.get(), &PlaybackController::currentFrameChanged, this, [this](int frame, int total) {
        if (isQuickLookVerboseLoggingEnabled()) {
            static int s_lastLoggedFrame = -1;
            if (frame != s_lastLoggedFrame && (frame < 3 || frame % 24 == 0)) {
                s_lastLoggedFrame = frame;
                logQuickLook(QStringLiteral("PreviewWindow::currentFrameChanged frame=%1 total=%2").arg(frame).arg(total));
            }
        }
        _updateTransport(frame, total);
    });
    connect(_playback.get(), &PlaybackController::playbackStateChanged, this, &PreviewWindow::_updatePlayState);
    connect(_playback.get(), &PlaybackController::muteChanged, this, [this](bool muted) {
        if (_muteButton) {
            _muteButton->setText(muted ? QString::fromUtf8("静音") : QString::fromUtf8("声音"));
        }
    });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 18);
    root->setSpacing(10);

    auto* topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(8);

    auto* badge = new QLabel(QString::fromUtf8("预览"), this);
    badge->setStyleSheet(
        "QLabel{"
        "color:#FF8C32;"
        "background:rgba(255,140,50,0.12);"
        "border:1px solid rgba(255,140,50,0.22);"
        "border-radius:10px;"
        "padding:4px 10px;"
        "font-size:11px;"
        "font-weight:700;"
        "}");
    topRow->addWidget(badge, 0, Qt::AlignLeft);

    _title = new QLabel(QString::fromUtf8("未选择文件"), this);
    _title->setStyleSheet("QLabel{color:#D8DEE7;font-size:13px;font-weight:600;background:transparent;}");
    topRow->addWidget(_title, 1);

    _hint = new QLabel(QString::fromUtf8("Space 播放/暂停   Esc 关闭"), this);
    _hint->setStyleSheet("QLabel{color:#9AA4B2;font-size:11px;background:transparent;}");
    topRow->addWidget(_hint, 0, Qt::AlignRight);
    root->addLayout(topRow);

    _viewer = new ViewerWidget(_playback, _ocio, this);
    _viewer->setChromeVisible(false);
    _viewer->setTimecodeVisible(false);
    root->addWidget(_viewer, 1);

    auto* controls = new QWidget(this);
    controls->setAttribute(Qt::WA_StyledBackground, true);
    controls->setStyleSheet(
        "QWidget{background:rgba(10,14,18,0.72);border:1px solid rgba(255,255,255,0.08);border-radius:10px;}"
        "QPushButton{background:rgba(22,28,35,0.82);color:#D8DEE7;border:1px solid rgba(255,255,255,0.10);border-radius:8px;padding:5px 10px;font-size:12px;font-weight:700;}"
        "QPushButton:hover{background:#202832;border-color:rgba(255,140,50,0.38);color:#FFFFFF;}"
        "QSlider{background:transparent;}"
        "QSlider::groove:horizontal{background:rgba(255,255,255,0.16);height:4px;border-radius:2px;}"
        "QSlider::sub-page:horizontal{background:#FF8C32;border-radius:2px;}"
        "QSlider::handle:horizontal{background:#F3F7FC;width:14px;height:14px;margin:-5px 0;border-radius:7px;border:1px solid rgba(255,255,255,0.28);}"
        "QLabel{background:transparent;color:#BEC8D5;font-family:Consolas,'Microsoft YaHei UI';font-size:12px;font-weight:700;}");
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(10, 8, 10, 8);
    controlsLayout->setSpacing(8);

    _playButton = new QPushButton(QString::fromUtf8("播放"), controls);
    _playButton->setToolTip(QString::fromUtf8("播放 / 暂停"));
    _playButton->setFixedWidth(58);
    controlsLayout->addWidget(_playButton);

    _progressSlider = new PreviewSeekSlider(Qt::Horizontal, controls);
    _progressSlider->setRange(0, 0);
    _progressSlider->setTracking(true);
    _progressSlider->setToolTip(QString::fromUtf8("拖动预览进度"));
    controlsLayout->addWidget(_progressSlider, 1);

    _timeLabel = new QLabel(QStringLiteral("00:00:00:00 / 00:00:00:00"), controls);
    _timeLabel->setMinimumWidth(170);
    _timeLabel->setAlignment(Qt::AlignCenter);
    controlsLayout->addWidget(_timeLabel);

    _muteButton = new QPushButton(QString::fromUtf8("声音"), controls);
    _muteButton->setToolTip(QString::fromUtf8("静音 / 恢复声音"));
    _muteButton->setFixedWidth(58);
    controlsLayout->addWidget(_muteButton);

    _volumeSlider = new PreviewVolumeSlider(Qt::Horizontal, controls);
    _volumeSlider->setRange(0, 100);
    _volumeSlider->setValue(100);
    _volumeSlider->setFixedWidth(92);
    _volumeSlider->setToolTip(QString::fromUtf8("音量"));
    controlsLayout->addWidget(_volumeSlider);
    root->addWidget(controls, 0);

    connect(_playButton, &QPushButton::clicked, this, &PreviewWindow::togglePlayback);
    connect(_muteButton, &QPushButton::clicked, _playback.get(), &PlaybackController::toggleMute);
    connect(_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        if (_playback) {
            _playback->setVolume(qBound(0, value, 100) / 100.0f);
        }
    });
    connect(_progressSlider, &QSlider::sliderMoved, this, [this](int frame) {
        if (_playback) {
            _playback->seekToFrame(frame);
        }
    });
    connect(_progressSlider, &QSlider::sliderPressed, this, [this] {
        if (_playback) {
            _playback->pause();
        }
    });
    connect(_progressSlider, &QSlider::sliderReleased, this, [this] {
        if (_playback && _progressSlider) {
            _playback->seekToFrame(_progressSlider->value());
        }
    });
}

void PreviewWindow::openFile(const QString& path, bool autoPlay)
{
    if (path.isEmpty()) {
        return;
    }

    const quint64 openGeneration = ++_openGeneration;
    _autoPlayPending = autoPlay;

    const QFileInfo info(path);
    const QString displayName = info.fileName().isEmpty() ? path : info.fileName();
    _title->setText(info.fileName().isEmpty() ? path : info.fileName());
    logQuickLook(QStringLiteral("PreviewWindow::openFile %1").arg(path));
    setStatusText(QString::fromUtf8("正在打开预览..."));
    if (_playButton) {
        _playButton->setEnabled(false);
    }
    if (_progressSlider) {
        const QSignalBlocker blocker(_progressSlider);
        _progressSlider->setRange(0, 0);
        _progressSlider->setValue(0);
    }
    if (_timeLabel) {
        _timeLabel->setText(QStringLiteral("00:00:00:00 / 00:00:00:00"));
    }

    const QScreen* targetScreen = screen() ? screen() : QGuiApplication::primaryScreen();
    if (targetScreen) {
        const QRect available = targetScreen->availableGeometry();
        const QSize previewSize(
            qBound(720, static_cast<int>(available.width() * 0.58), 1440),
            qBound(420, static_cast<int>(available.height() * 0.62), 960));
        resize(previewSize);
        move(available.center() - QPoint(width() / 2, height() / 2));
    }

    show();
    raise();
    activateWindow();
    setFocus();

    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd) {
        SetWindowPos(
            hwnd,
            HWND_TOPMOST,
            x(),
            y(),
            width(),
            height(),
            SWP_SHOWWINDOW);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
    }

    logQuickLook(
        QStringLiteral("PreviewWindow::shown x=%1 y=%2 w=%3 h=%4 visible=%5")
            .arg(x())
            .arg(y())
            .arg(width())
            .arg(height())
            .arg(isVisible()));

    QTimer::singleShot(0, this, [this, path, displayName, autoPlay, openGeneration] {
        // Closing the preview can happen before this queued load callback runs.
        // Do not resurrect a hidden preview (and its file handle) after close,
        // nor let an older open win a race with a newer selection.
        if (!_playback || openGeneration != _openGeneration) {
            return;
        }
        logQuickLook(QStringLiteral("PreviewWindow::beginLoad %1").arg(path));
        _playback->openFile(path);
        _playback->setMute(false);
        _playback->setVolume(_volumeSlider ? _volumeSlider->value() / 100.0f : 1.0f);
        if (_viewer) {
            _viewer->fitToWindow();
        }
        if (_playButton) {
            _playButton->setEnabled(true);
        }
        setStatusText(QString::fromUtf8("Space 播放/暂停   Esc 关闭"));
        _title->setText(displayName);

        if (autoPlay) {
            logQuickLook(QStringLiteral("PreviewWindow::autoPlay %1").arg(path));
            _playback->setMute(false);
            // Normal files emit playerReady synchronously. Transcoded files
            // emit it later; the playerReady handler above starts playback in
            // both cases without racing the asynchronous open.
            if (_autoPlayPending && _playback->isValid()) {
                _autoPlayPending = false;
                _playback->play();
            }
        }
    });
}

void PreviewWindow::togglePlayback()
{
    if (_playback && !_playback->currentPath().isEmpty()) {
        logQuickLook(QStringLiteral("PreviewWindow::togglePlayback %1").arg(_playback->currentPath()));
        _playback->togglePlay();
    }
}

void PreviewWindow::closePreview()
{
    ++_openGeneration;
    _autoPlayPending = false;
    if (_playback) {
        logQuickLook(QStringLiteral("PreviewWindow::closePreview"));
        // QuickLook is a resident background process. Stopping and hiding the
        // viewer is not enough: tlRender keeps the source reader alive until
        // PlaybackController::closeFile() releases the player and emits the
        // viewer clear signal, otherwise Windows reports the media as locked.
        _playback->closeFile();
        _playback->setMute(true);
    }
    hide();
}

void PreviewWindow::setStatusText(const QString& text)
{
    if (_hint) {
        _hint->setText(text);
    }
}

bool PreviewWindow::isShowingFile(const QString& path) const
{
    return _playback && _playback->currentPath() == path && isVisible();
}

bool PreviewWindow::hasFile() const
{
    return _playback && !_playback->currentPath().isEmpty();
}

QString PreviewWindow::currentPath() const
{
    return _playback ? _playback->currentPath() : QString();
}

void PreviewWindow::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Space:
        togglePlayback();
        return;
    case Qt::Key_Escape:
        closePreview();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void PreviewWindow::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(9, 12, 16, 236));

    painter.setPen(QPen(QColor(255, 255, 255, 22), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 18, 18);
}

void PreviewWindow::closeEvent(QCloseEvent* event)
{
    closePreview();
    if (event) {
        event->ignore();
    }
}

QString PreviewWindow::_formatTimecode(int frame) const
{
    double fps = _playback ? _playback->fps() : 24.0;
    if (fps <= 0.0) {
        fps = 24.0;
    }
    const int fpsInt = qMax(1, static_cast<int>(fps + 0.5));
    const int safeFrame = qMax(0, frame);
    const int ff = safeFrame % fpsInt;
    const int totalSeconds = safeFrame / fpsInt;
    const int ss = totalSeconds % 60;
    const int mm = (totalSeconds / 60) % 60;
    const int hh = totalSeconds / 3600;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(hh, 2, 10, QChar('0'))
        .arg(mm, 2, 10, QChar('0'))
        .arg(ss, 2, 10, QChar('0'))
        .arg(ff, 2, 10, QChar('0'));
}

void PreviewWindow::_updateTransport(int frame, int total)
{
    const int maxFrame = qMax(0, total - 1);
    if (_progressSlider) {
        const QSignalBlocker blocker(_progressSlider);
        _progressSlider->setRange(0, maxFrame);
        _progressSlider->setValue(qBound(0, frame, maxFrame));
    }
    if (_timeLabel) {
        _timeLabel->setText(QStringLiteral("%1 / %2")
            .arg(_formatTimecode(frame), _formatTimecode(maxFrame)));
    }
}

void PreviewWindow::_updatePlayState(int state)
{
    if (_playButton) {
        _playButton->setText(state == 0 ? QString::fromUtf8("播放") : QString::fromUtf8("暂停"));
    }
}

} // namespace cgplay::quicklook
