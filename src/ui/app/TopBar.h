#pragma once

#include <QLabel>
#include <QPoint>
#include <QPushButton>
#include <QWidget>
#include <memory>

namespace cgplay {

class TopBar : public QWidget
{
    Q_OBJECT
public:
    explicit TopBar(QWidget* parent = nullptr);
    ~TopBar() override;

    QPushButton* fileMenuBtn() const;
    QPushButton* viewMenuBtn() const;
    QPushButton* windowMenuBtn() const;
    QPushButton* otioMenuBtn() const;
    QPushButton* colorMenuBtn() const;
    QPushButton* audioMenuBtn() const;
    QPushButton* playMenuBtn() const;
    QPushButton* helpMenuBtn() const;

    QLabel* fpsLabel() const;
    QLabel* decoderLabel() const;
    QLabel* codecLabel() const;
    QLabel* resolutionLabel() const;
    QPushButton* settingsButton() const;
    QPushButton* menuButton() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void _setupUI();
    QPushButton* _makeMenuBtn(const QString& text);
    QPushButton* _makeWindowBtn(const QString& text, const QString& tip, bool danger = false);
    QLabel* _makeStatusLabel(const QString& text);
    void _toggleMaximized();

    struct Private;
    std::unique_ptr<Private> _p;
};

} // namespace cgplay
