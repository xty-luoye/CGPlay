#pragma once
// CGPlay PlaybackBar.h — Phase 7: 52px playback bar below Timeline
#include <QWidget>
#include <QToolButton>
#include <QLabel>
#include <QSlider>
#include <QString>
#include <memory>

class QTimer;
class QAction;
class QResizeEvent;

namespace cgplay {

class IPlaybackService;

class PlaybackBar : public QWidget
{
    Q_OBJECT
public:
    explicit PlaybackBar(std::shared_ptr<IPlaybackService> playback,
                         QWidget* parent = nullptr);
    ~PlaybackBar() override;

    QToolButton* playBtn() const { return _btnPlay; }
    void setTranslationVisible(bool visible);
    bool translationVisible() const;
    void setTranslationMode(const QString& modeId);
    QString translationMode() const;
    int navigationStepFrames() const;
    void setNavigationStep(bool inSeconds, int amount);
    bool navigationInSeconds() const { return _navigationInSeconds; }
    int navigationStepAmount() const { return _navigationStepAmount; }
    void setDisplaySeconds(bool enabled);
    bool displaySeconds() const { return _displaySeconds; }

Q_SIGNALS:
    void translationVisibilityToggled(bool visible);
    void translationModeChanged(const QString& modeId);
    void navigationStepChanged(bool inSeconds, int amount);
    void displaySecondsChanged(bool enabled);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QToolButton* _makeBtn(const QString& text, const QString& tip, int size, bool accent=false);
    void _updateTranslationModeButton();
    void _onStateChanged(int state);
    void _refreshPlaybackOptions();
    void _queueVolumeValue(int sliderValue);
    void _applyPendingVolume();
    void _updateResponsiveLayout();
    void _refreshTimeDisplay();
    void _refreshNavigationStep();

    std::shared_ptr<IPlaybackService> _playback;

    QToolButton* _btnPrev     = nullptr;
    QToolButton* _btnPlay     = nullptr;
    QToolButton* _btnNext     = nullptr;
    QToolButton* _btnTranslation = nullptr;
    QToolButton* _btnTranslationMode = nullptr;
    QToolButton* _btnSpeed  = nullptr;
    QAction*     _actQuickMode = nullptr;
    QAction*     _actHighQualityMode = nullptr;
    QToolButton* _btnLoop     = nullptr;
    QToolButton* _btnMute     = nullptr;
    QLabel*      _lblFPS      = nullptr;
    QToolButton* _lblFrame    = nullptr;
    QToolButton* _btnNavigationStep = nullptr;
    QSlider*     _volSlider   = nullptr;
    QTimer*      _volumeApplyTimer = nullptr;
    int          _pendingVolumeValue = 1000;
    QString      _translationMode = QStringLiteral("quick-playback");
    bool         _navigationInSeconds = true;
    int          _navigationStepAmount = 5;
    bool         _displaySeconds = false;
};

} // namespace cgplay
