#pragma once
// CGPlay AnnotationToolbar.h — 批注工具条

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QColor>
#include <QVector>

namespace cgplay {

class AnnotationToolbar : public QWidget
{
    Q_OBJECT
public:
    explicit AnnotationToolbar(QWidget* parent = nullptr);

    enum Tool {
        Select,
        Arrow,
        Rectangle,
        Circle,
        Text,
        FreeDraw,
        Point
    };

    Tool currentTool() const { return _currentTool; }
    QColor currentColor() const { return _currentColor; }

public Q_SLOTS:
    void setTool(Tool tool);
    void setColor(const QColor& color);

Q_SIGNALS:
    void toolChanged(int tool, QColor color);
    void colorChanged(QColor color);
    void deleteRequested();
    void visibilityToggled(bool visible);

private:
    void _onToolClicked(int tool);
    void _pickColor();
    void _updateColorIndicator();

    QVector<QPushButton*> _buttons;
    QLabel* _colorIndicator = nullptr;
    Tool    _currentTool = Select;
    QColor  _currentColor = QColor(255, 0, 0);

    static const QVector<QColor> kColorPresets;
};

} // namespace cgplay
