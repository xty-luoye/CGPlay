#pragma once
// CGPlay TitleBar — 42px frameless title bar with status indicators
#include <QWidget>
#include <QLabel>
#include <QPushButton>

namespace cgplay {

class TitleBar : public QWidget
{
    Q_OBJECT
public:
    explicit TitleBar(QWidget* parent=nullptr);
    QPushButton* fileBtn() const { return _fileBtn; }
    QPushButton* viewBtn() const { return _viewBtn; }
    QPushButton* windowBtn() const { return _windowBtn; }
    QPushButton* ocioBtn() const { return _ocioBtn; }
    QPushButton* audioBtn() const { return _audioBtn; }
    QPushButton* helpBtn() const { return _helpBtn; }

    QLabel* fpsLabel()      const { return _lblFPS; }
    QLabel* codecLabel()    const { return _lblCodec; }
    QLabel* bitDepthLabel() const { return _lblBitDepth; }
    QLabel* decoderLabel()  const { return _lblDecoder; }
    QLabel* gpuLabel()      const { return _lblGPU; }
    QLabel* resLabel()      const { return _lblRes; }

    QPushButton* minBtn() const { return _minBtn; }
    QPushButton* maxBtn() const { return _maxBtn; }
    QPushButton* closeBtn() const { return _closeBtn; }

Q_SIGNALS:
    void menuRequested(const QString& name);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    void _setupUI();
    QPushButton* _makeMenu(const QString& text);
    QLabel* _makeInfo(const QString& text, const QString& color="#9AA4B2");

    QPushButton *_fileBtn=nullptr, *_viewBtn=nullptr, *_windowBtn=nullptr;
    QPushButton *_ocioBtn=nullptr, *_audioBtn=nullptr, *_helpBtn=nullptr;
    QLabel *_lblFPS=nullptr, *_lblCodec=nullptr, *_lblBitDepth=nullptr;
    QLabel *_lblDecoder=nullptr, *_lblGPU=nullptr, *_lblRes=nullptr;
    QPushButton *_minBtn=nullptr, *_maxBtn=nullptr, *_closeBtn=nullptr;
    QPoint _dragPos;
    bool _manualDragging = false;
};

} // namespace cgplay
