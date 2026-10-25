#pragma once

#include <QJsonObject>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;

namespace cgplay {

// Value-only UI. Enumerating or creating native capture sources belongs to the
// recording service, and is never a side effect of constructing this widget.
class RecordingPanel final : public QWidget
{
    Q_OBJECT
public:
    explicit RecordingPanel(QWidget* parent = nullptr);
    // Source entries: {label, plan:{actual OBS selection/identity fields}}.
    // Audio entries: {label, id:<actual OBS device property value>}.
    void setCatalog(const QJsonObject& catalog);
    void setPlayerSelection(const QJsonObject& selection);
    void setServiceState(const QJsonObject& state);
    QJsonObject currentSettings() const;
    bool isBusy() const;

Q_SIGNALS:
    void refreshRequested();
    void startRequested(QJsonObject settings);
    void pauseRequested(bool paused);
    void stopRequested();
    void recoverRequested(QString sidecarPath, QString outputPath);
    void audioChanged(QJsonObject settings);

private:
    void updateControls();
    void updateSources();
    void publishAudio();
    void chooseOutput();
    void chooseRecovery();
    QString selectionError() const;

    QJsonObject _catalog;
    QJsonObject _playerSelection;
    QString _state = QStringLiteral("Idle");
    QString _catalogError;
    QString _recoveryDescriptorPath;
    QString _recoveryMkvPath;
    QString _pauseAwaitingState;
    bool _backendBusy = false;
    bool _canPause = false;
    QComboBox* _scope = nullptr;
    QComboBox* _source = nullptr;
    QLabel* _sourceNote = nullptr;
    QComboBox* _resolution = nullptr;
    QComboBox* _fps = nullptr;
    QLineEdit* _output = nullptr;
    QPushButton* _browse = nullptr;
    QPushButton* _refresh = nullptr;
    QCheckBox* _systemEnabled = nullptr;
    QCheckBox* _micEnabled = nullptr;
    QComboBox* _systemDevice = nullptr;
    QComboBox* _micDevice = nullptr;
    QSlider* _systemGain = nullptr;
    QSlider* _micGain = nullptr;
    QCheckBox* _systemMute = nullptr;
    QCheckBox* _micMute = nullptr;
    QLabel* _status = nullptr;
    QLabel* _error = nullptr;
    QPushButton* _start = nullptr;
    QPushButton* _pause = nullptr;
    QPushButton* _stop = nullptr;
    QPushButton* _recover = nullptr;
};

} // namespace cgplay
