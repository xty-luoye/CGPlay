// CGPlay SecondaryWindow.cpp

#include "SecondaryWindow.h"
#include "common/core/ServiceLocator.h"
#include "viewer/ViewerWidget.h"
#include "viewer/TlViewport.h"
#include "timeline/TimelineWidget.h"
#include "playback/PlaybackController.h"
#include "playback/api/IPlaybackService.h"
#include "ocio/OcioManager.h"
#include "cache/CacheManager.h"

#include <QFileInfo>
#include <QVBoxLayout>
#include <QComboBox>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QTimer>
#include <QApplication>
#include <QEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTextEdit>

namespace cgplay {

namespace {

bool isTextInputFocusWidget(QWidget* widget)
{
    while (widget) {
        if (qobject_cast<QLineEdit*>(widget) ||
            qobject_cast<QTextEdit*>(widget) ||
            qobject_cast<QPlainTextEdit*>(widget))
        {
            return true;
        }
        if (auto* comboBox = qobject_cast<QComboBox*>(widget)) {
            if (comboBox->isEditable()) {
                return true;
            }
        }
        widget = widget->parentWidget();
    }
    return false;
}

} // namespace

SecondaryWindow::SecondaryWindow(
    std::shared_ptr<OcioManager>        ocio,
    std::shared_ptr<CacheManager>       cache,
    int windowIndex,
    QWidget* parent)
    : QMainWindow(parent)
    , _ocio(std::move(ocio))
    , _cache(std::move(cache))
    , _index(windowIndex)
{
    setWindowTitle(QString("CGPlay v1.1 — Window %1").arg(_index + 1));
    setMinimumSize(640, 400);
    resize(960, 640);

    // Create independent PlaybackController through the shared service construction seam.
    _playbackCtrl = std::make_shared<PlaybackController>(_cache, _ocio);

    _setupUI();
    _connectSignals();
}

SecondaryWindow::~SecondaryWindow() = default;

void SecondaryWindow::_setupUI()
{
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Viewer
    _viewer = new ViewerWidget(_playbackCtrl, _ocio, this);
    layout->addWidget(_viewer, 1);

    // Compact timeline
    _timeline = new TimelineWidget(_playbackCtrl, this);
    layout->addWidget(_timeline, 0);

    setCentralWidget(central);

    _fullscreenChromeTimer = new QTimer(this);
    _fullscreenChromeTimer->setSingleShot(true);
    _fullscreenChromeTimer->setInterval(2000);
    connect(_fullscreenChromeTimer, &QTimer::timeout, this, &SecondaryWindow::_hideFullScreenChrome);
    qApp->installEventFilter(this);
}

void SecondaryWindow::_connectSignals()
{
    // Drop on viewer opens file in this window
    connect(_viewer, &ViewerWidget::droppedFile,
            this, &SecondaryWindow::openFile);

    // Drop on timeline opens file
    connect(_timeline, &TimelineWidget::droppedFile,
            this, &SecondaryWindow::openFile);
    connect(_viewer, &ViewerWidget::fullscreenRequested, this, [this] { _toggleFullScreen(); });
}

void SecondaryWindow::openFile(const QString& path)
{
    if (_timeline) _timeline->setMediaPath(path);
    _playbackCtrl->openFile(path);
    setWindowTitle(QString("CGPlay v1.1 — %1").arg(
        QFileInfo(path).fileName()));
}

void SecondaryWindow::setCompareFile(const QString& path)
{
    _playbackCtrl->setCompareFile(path);
}

void SecondaryWindow::closeEvent(QCloseEvent* event)
{
    _setFullScreenCursorHidden(false);
    // Release the timeline thumbnail job and tlRender player before the
    // window is queued for deletion.  The application keeps secondary
    // windows in a managed list, so relying on deleteLater() can leave the
    // source file locked after a preview is closed.
    if (_timeline) {
        _timeline->setMediaPath({});
    }
    if (_playbackCtrl) {
        _playbackCtrl->closeFile();
    }
    Q_EMIT closed(_index);
    event->accept();
}

bool SecondaryWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (isFullScreen()) {
        switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::Wheel:
        case QEvent::KeyPress:
            _showFullScreenChromeTemporarily();
            break;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void SecondaryWindow::keyPressEvent(QKeyEvent* event)
{
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    if (isTextInputFocusWidget(QApplication::focusWidget()) &&
        ((modifiers == Qt::NoModifier || modifiers == Qt::ShiftModifier) &&
         (!event->text().isEmpty() || event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace))) {
        QMainWindow::keyPressEvent(event);
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        if (isFullScreen()) {
            _toggleFullScreen();
            return;
        }
    }

    QSettings settings(QStringLiteral("CGPlay"), QStringLiteral("CGPlay"));
    const QKeySequence pressed(QKeyCombination(modifiers, Qt::Key(event->key())));
    const auto matches = [&settings, &pressed](const QString& id, const QString& fallback) {
        const QString key = QStringLiteral("shortcuts/%1").arg(id);
        const QString configured = settings.contains(key) ? settings.value(key).toString().trimmed() : fallback;
        return !configured.isEmpty() && QKeySequence(configured).matches(pressed) == QKeySequence::ExactMatch;
    };
    if (matches(QStringLiteral("playback.toggle"), QStringLiteral("Space"))) { _playbackCtrl->togglePlay(); return; }
    if (matches(QStringLiteral("playback.reverse"), QStringLiteral("J"))) { _playbackCtrl->reverse(); return; }
    if (matches(QStringLiteral("playback.stop"), QStringLiteral("K"))) { _playbackCtrl->stop(); return; }
    if (matches(QStringLiteral("playback.forward"), QStringLiteral("L"))) { _playbackCtrl->forward(); return; }
    if (matches(QStringLiteral("playback.previousFrame"), QStringLiteral("Left"))) { _playbackCtrl->prevFrame(); return; }
    if (matches(QStringLiteral("playback.nextFrame"), QStringLiteral("Right"))) { _playbackCtrl->nextFrame(); return; }
    if (matches(QStringLiteral("playback.seekBackward10"), QStringLiteral("Shift+Left"))) { _playbackCtrl->seekRelative(-10); return; }
    if (matches(QStringLiteral("playback.seekForward10"), QStringLiteral("Shift+Right"))) { _playbackCtrl->seekRelative(10); return; }
    if (matches(QStringLiteral("audio.toggleMute"), QStringLiteral("M"))) { _playbackCtrl->toggleMute(); return; }
    if (matches(QStringLiteral("view.fitToWindow"), QStringLiteral("F"))) { _viewer->fitToWindow(); return; }
    if (matches(QStringLiteral("view.fullscreen"), QStringLiteral("F11"))) { _toggleFullScreen(); return; }
    QMainWindow::keyPressEvent(event);
}

void SecondaryWindow::_toggleFullScreen()
{
    if (isFullScreen()) {
        if (_fullscreenChromeTimer) {
            _fullscreenChromeTimer->stop();
        }
        _setFullScreenCursorHidden(false);
        if (_timeline) {
            _timeline->setVisible(_fullscreenTimeline);
        }
        if (_viewer) {
            _viewer->setChromeVisible(_fullscreenViewerChrome);
        }
        if (auto* vp = _viewer ? _viewer->viewport() : nullptr) {
            vp->setFrameView(true);
        }
        showNormal();
        return;
    }

    _fullscreenTimeline = _timeline ? _timeline->isVisible() : true;
    _fullscreenViewerChrome = true;
    if (auto* vp = _viewer ? _viewer->viewport() : nullptr) {
        vp->setFrameView(true);
    }
    _setFullScreenChromeVisible(false, true);
    showFullScreen();
    QTimer::singleShot(0, this, [this]() {
        _setFullScreenChromeVisible(false, true);
    });
}

void SecondaryWindow::_showFullScreenChromeTemporarily()
{
    if (!isFullScreen()) {
        return;
    }
    _setFullScreenChromeVisible(true);
    if (_fullscreenChromeTimer) {
        _fullscreenChromeTimer->start(2000);
    }
}

void SecondaryWindow::_hideFullScreenChrome()
{
    if (!isFullScreen()) {
        return;
    }
    _setFullScreenChromeVisible(false);
}

void SecondaryWindow::_setFullScreenChromeVisible(bool visible, bool forceApply)
{
    if (!forceApply && !isFullScreen()) {
        return;
    }
    _fullscreenChromeVisible = visible;
    if (_timeline) {
        _timeline->setVisible(visible && _fullscreenTimeline);
    }
    if (_viewer) {
        _viewer->setChromeVisible(false);
    }
    _setFullScreenCursorHidden(!visible);
}

void SecondaryWindow::_setFullScreenCursorHidden(bool hidden)
{
    if (_fullscreenCursorHidden == hidden) {
        return;
    }
    _fullscreenCursorHidden = hidden;
    if (hidden) {
        QApplication::setOverrideCursor(Qt::BlankCursor);
    } else {
        QApplication::restoreOverrideCursor();
    }
}

} // namespace cgplay
