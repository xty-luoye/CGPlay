// CGPlay AnnotationOverlay.cpp — 批注 QPainter 绘制 + 鼠标交互

#include "AnnotationOverlay.h"
#include "AnnotationManager.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QFontMetrics>
#include <QLineF>
#include <QtMath>
#include <QDebug>

namespace cgplay {

// ─── Constructor ──────────────────────────────────────────────────────────────
AnnotationOverlay::AnnotationOverlay(AnnotationManager* mgr, QWidget* parent)
    : QWidget(parent)
    , _mgr(mgr)
{
    // Transparent background, pass-through when not drawing
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
}

// ─── View Transform ───────────────────────────────────────────────────────────
// tlRender 内部 viewport 的 geometry 是物理像素（乘了 devicePixelRatio），
// 因此 getZoom() / getViewPos() 返回的是物理像素坐标系的值。
// AnnotationOverlay 作为 Qt QWidget 使用逻辑像素，坐标转换必须统一。

double AnnotationOverlay::_currentZoom() const
{
    if (_coordinateMapper) {
        return _coordinateMapper->zoom();
    }
    return 1.0;
}

// 图像坐标(像素) → Widget 逻辑坐标
// 直接复用 TlViewport 的坐标映射，确保和 OpenGL 渲染完全一致
QPointF AnnotationOverlay::imgToWidget(QPointF imgPt) const
{
    if (!_coordinateMapper) return imgPt;
    return _coordinateMapper->imageToWidget(imgPt, _imgW, _imgH);
}

// Widget 逻辑坐标 → 图像坐标(像素)
// 直接复用 TlViewport 的坐标映射，确保和 OpenGL 渲染完全一致
QPointF AnnotationOverlay::widgetToImg(QPointF wtPt) const
{
    if (!_coordinateMapper) return wtPt;
    return _coordinateMapper->widgetToImage(wtPt, _imgW, _imgH);
}

void AnnotationOverlay::setAnnotations(const QVector<AnnotationItem>& anns)
{
    if (_annotations == anns) {
        return;
    }
    _annotations = anns;
    update();
}

void AnnotationOverlay::setSelectedId(const QString& id)
{
    if (_selectedId == id) {
        return;
    }
    _selectedId = id;
    update();
}

void AnnotationOverlay::setToolMode(ToolMode mode)
{
    if (_toolMode == mode && !_drawing) {
        return;
    }
    _toolMode = mode;
    _drawing = false;
    _drawPts.clear();
    // Select mode → transparent mouse; drawing mode → capture mouse
    if (mode == ToolMode::Select) {
        setCursor(Qt::ArrowCursor);
    } else {
        setCursor(Qt::CrossCursor);
    }
    update();
}

// ─── Paint ────────────────────────────────────────────────────────────────────
void AnnotationOverlay::paintEvent(QPaintEvent*)
{
    if (_annotations.isEmpty() && _drawPts.isEmpty()) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Draw existing annotations
    for (const auto& ann : _annotations) {
        bool sel = (ann.id == _selectedId);
        _drawAnnotation(p, ann, sel);
    }

    // Draw in-progress shape (from mouse drag)
    if (_drawing && !_drawPts.isEmpty()) {
        AnnotationItem tempAnn;
        tempAnn.type = static_cast<AnnotationType>(
            static_cast<int>(_toolMode) - 1); // ToolMode maps to AnnotationType+1
        tempAnn.color = _toolColor;
        tempAnn.points = _drawPts;

        QColor previewColor = _toolColor;
        previewColor.setAlpha(150);
        tempAnn.color = previewColor;
        _drawAnnotation(p, tempAnn, false);
    }
}

void AnnotationOverlay::_drawAnnotation(QPainter& p, const AnnotationItem& ann,
                                         bool isSelected)
{
    if (ann.points.isEmpty()) return;

    double penW = isSelected ? qMax(2.0, 3.0 * _currentZoom()) : qMax(1.5, 2.0 * _currentZoom());
    QColor color = isSelected ? QColor(0, 160, 255, 200) : ann.color;

    switch (ann.type) {
    case AnnotationType::Arrow:     _drawArrow(p, ann.points, color, penW, isSelected); break;
    case AnnotationType::Rectangle: _drawRect(p, ann.points, color, penW, isSelected); break;
    case AnnotationType::Circle:    _drawCircle(p, ann.points, color, penW, isSelected); break;
    case AnnotationType::Text:      _drawText(p, ann.points, ann.comment, color); break;
    case AnnotationType::FreeDraw:  _drawFreeDraw(p, ann.points, color, penW, isSelected); break;
    case AnnotationType::Point:     _drawPoint(p, ann.points, color, penW, isSelected); break;
    default: break;
    }
}

// ─── Arrow ───────────────────────────────────────────────────────────────────
void AnnotationOverlay::_drawArrow(QPainter& p, const QVector<QPointF>& pts,
                                    const QColor& color, double penW, bool)
{
    if (pts.size() < 2) return;
    QPointF p1 = imgToWidget(pts[0]);
    QPointF p2 = imgToWidget(pts[1]);

    QPen pen(color, penW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.drawLine(p1, p2);

    // Arrow head
    double arrowSz = qMax(8.0, 12.0 * _currentZoom());
    QLineF line(p2, p1);
    double angle = qAtan2(-line.dy(), line.dx());
    double a1 = angle + qDegreesToRadians(25.0);
    double a2 = angle - qDegreesToRadians(25.0);
    QPointF tip1(p2.x() - arrowSz * qCos(a1), p2.y() + arrowSz * qSin(a1));
    QPointF tip2(p2.x() - arrowSz * qCos(a2), p2.y() + arrowSz * qSin(a2));

    QPolygonF tri({p2, tip1, tip2});
    p.setBrush(color);
    p.setPen(Qt::NoPen);
    p.drawPolygon(tri);

    // Start dot
    double dotR = qMax(2.0, 3.5 * _currentZoom());
    p.drawEllipse(p1, dotR, dotR);
}

// ─── Rectangle ───────────────────────────────────────────────────────────────
void AnnotationOverlay::_drawRect(QPainter& p, const QVector<QPointF>& pts,
                                   const QColor& color, double penW, bool)
{
    if (pts.size() < 2) return;
    QPointF p1 = imgToWidget(pts[0]);
    QPointF p2 = imgToWidget(pts[1]);
    QRectF rect(p1, p2);

    QColor fill(color);
    fill.setAlpha(30);
    p.setBrush(fill);
    p.setPen(QPen(color, penW, Qt::SolidLine));
    p.drawRect(rect);
}

// ─── Circle ──────────────────────────────────────────────────────────────────
void AnnotationOverlay::_drawCircle(QPainter& p, const QVector<QPointF>& pts,
                                     const QColor& color, double penW, bool)
{
    if (pts.size() < 1) return;
    QPointF center = imgToWidget(pts[0]);
    double radius = 0;

    if (pts.size() >= 2) {
        QPointF edge = imgToWidget(pts[1]);
        radius = QLineF(center, edge).length();
    } else {
        radius = qMax(3.0, 10.0 * _currentZoom());
    }

    QColor fill(color);
    fill.setAlpha(30);
    p.setBrush(fill);
    p.setPen(QPen(color, penW, Qt::SolidLine));
    p.drawEllipse(center, radius, radius);

    // Cross hair
    double cross = qMax(3.0, 4.0 * _currentZoom());
    QPen crossPen(color.darker(130), qMax(1.0, 1.5 * _currentZoom()));
    p.setPen(crossPen);
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(center.x() - cross, center.y()),
                QPointF(center.x() + cross, center.y()));
    p.drawLine(QPointF(center.x(), center.y() - cross),
                QPointF(center.x(), center.y() + cross));
}

// ─── Text ────────────────────────────────────────────────────────────────────
void AnnotationOverlay::_drawText(QPainter& p, const QVector<QPointF>& pts,
                                   const QString& text, const QColor& color)
{
    if (pts.isEmpty()) return;
    QPointF p1 = imgToWidget(pts[0]);
    QString str = text.isEmpty() ? "T" : text;

    QFont font("Microsoft YaHei", qMax(10, static_cast<int>(12 * _currentZoom())));
    font.setBold(true);
    p.setFont(font);
    QFontMetrics fm(font);
    QRect textRect = fm.boundingRect(str);
    int pad = qMax(3, static_cast<int>(4 * _currentZoom()));

    QRectF bg(p1.x(), p1.y() - textRect.height() - pad,
              textRect.width() + pad * 2,
              textRect.height() + pad * 2);
    QColor bgColor(color);
    bgColor.setAlpha(180);
    p.fillRect(bg, bgColor);

    p.setPen(Qt::white);
    p.drawText(QPointF(p1.x() + pad, p1.y() - pad), str);
}

// ─── FreeDraw ────────────────────────────────────────────────────────────────
void AnnotationOverlay::_drawFreeDraw(QPainter& p, const QVector<QPointF>& pts,
                                       const QColor& color, double penW, bool)
{
    if (pts.size() < 2) return;
    QPainterPath path;
    path.moveTo(imgToWidget(pts[0]));
    for (int i = 1; i < pts.size(); ++i)
        path.lineTo(imgToWidget(pts[i]));

    p.setPen(QPen(color, penW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

// ─── Point ───────────────────────────────────────────────────────────────────
void AnnotationOverlay::_drawPoint(QPainter& p, const QVector<QPointF>& pts,
                                    const QColor& color, double penW, bool)
{
    if (pts.isEmpty()) return;
    QPointF p1 = imgToWidget(pts[0]);
    double cross = qMax(4.0, 6.0 * _currentZoom());

    p.setPen(QPen(color, penW, Qt::SolidLine));
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(p1.x() - cross, p1.y()), QPointF(p1.x() + cross, p1.y()));
    p.drawLine(QPointF(p1.x(), p1.y() - cross), QPointF(p1.x(), p1.y() + cross));

    double dotR = qMax(2.0, 3.0 * _currentZoom());
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(p1, dotR, dotR);
}

// ─── Hit Test ────────────────────────────────────────────────────────────────
QString AnnotationOverlay::_hitTest(const QPointF& imgPos) const
{
    double tolerance = 8.0 / _currentZoom(); // screen tolerance → image space
    double bestDist  = std::numeric_limits<double>::max();
    QString bestId;

    for (const auto& ann : _annotations) {
        bool hit = false;
        double dist = 0;

        if (ann.type == AnnotationType::Arrow || ann.type == AnnotationType::FreeDraw) {
            for (const auto& pt : ann.points) {
                double d = QLineF(pt, imgPos).length();
                if (d < tolerance + 2) { hit = true; dist = d; break; }
            }
            if (!hit && ann.type == AnnotationType::Arrow && ann.points.size() >= 2) {
                // Point-to-segment distance
                QPointF a = ann.points[0], b = ann.points[1];
                double dx = b.x() - a.x(), dy = b.y() - a.y();
                if (dx == 0 && dy == 0) {
                    dist = QLineF(a, imgPos).length();
                } else {
                    double t = qBound(0.0,
                        ((imgPos.x()-a.x())*dx + (imgPos.y()-a.y())*dy) / (dx*dx + dy*dy),
                        1.0);
                    dist = QLineF(imgPos, QPointF(a.x()+t*dx, a.y()+t*dy)).length();
                }
                if (dist < tolerance + 3) hit = true;
            }
        }
        else if (ann.type == AnnotationType::Rectangle && ann.points.size() >= 2) {
            QRectF r = QRectF(ann.points[0], ann.points[1]).normalized();
            if (r.contains(imgPos)) { hit = true; dist = 0; }
            else {
                double dx = std::max({r.left() - imgPos.x(), 0.0, imgPos.x() - r.right()});
                double dy = std::max({r.top() - imgPos.y(), 0.0, imgPos.y() - r.bottom()});
                dist = qSqrt(dx*dx + dy*dy);
                if (dist < tolerance + 4) hit = true;
            }
        }
        else if (ann.type == AnnotationType::Circle && ann.points.size() >= 2) {
            QPointF c = ann.points[0];
            double r = QLineF(ann.points[0], ann.points[1]).length();
            double d = qAbs(QLineF(c, imgPos).length() - r);
            if (d < tolerance + 4) { hit = true; dist = d; }
        }
        else if ((ann.type == AnnotationType::Text || ann.type == AnnotationType::Point)
                 && !ann.points.isEmpty()) {
            double d = QLineF(ann.points[0], imgPos).length();
            double tol = (ann.type == AnnotationType::Text) ? tolerance + 10 : tolerance + 3;
            if (d < tol) { hit = true; dist = d; }
        }

        if (hit && dist < bestDist) {
            bestDist = dist;
            bestId = ann.id;
        }
    }
    return bestId;
}

// ─── Mouse Events ────────────────────────────────────────────────────────────
// 返回当前视频画面在 widget 中的矩形（逻辑像素）
QRectF AnnotationOverlay::_videoRect() const
{
    return _coordinateMapper ? _coordinateMapper->visibleVideoRect() : QRectF();
}

// 把 widget 坐标限制在视频画面内，再转回图像坐标
QPointF AnnotationOverlay::_clampToVideo(QPointF wtPt) const
{
    QRectF r = _videoRect();
    if (!r.isEmpty()) {
        wtPt.setX(qBound(r.left(),   wtPt.x(), r.right()));
        wtPt.setY(qBound(r.top(),    wtPt.y(), r.bottom()));
    }
    return widgetToImg(wtPt);
}

void AnnotationOverlay::mousePressEvent(QMouseEvent* event)
{
    if (_toolMode == ToolMode::Select) {
        // Hit test existing annotations
        QPointF imgPt = widgetToImg(event->pos());
        QString hitId = _hitTest(imgPt);
        if (!hitId.isEmpty()) {
            _selectedId = hitId;
            Q_EMIT annotationSelected(hitId);
            update();
        } else {
            _selectedId.clear();
            Q_EMIT annotationSelected({});
            update();
        }
        event->accept();
        return;
    }

    // Drawing mode: 只能在视频画面内开始绘制
    if (event->button() == Qt::LeftButton) {
        QRectF r = _videoRect();
        if (!r.isEmpty() && !r.contains(event->pos())) {
            event->ignore();
            return;
        }
        _drawing = true;
        _drawPts.clear();
        _drawPts.append(_clampToVideo(event->pos()));
        event->accept();
    }
}

void AnnotationOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (_toolMode == ToolMode::Select) {
        event->ignore();
        return;
    }

    if (_drawing) {
        QPointF imgPt = _clampToVideo(event->pos());

        if (_toolMode == ToolMode::FreeDraw) {
            if (_drawPts.isEmpty() || QLineF(_drawPts.constLast(), imgPt).length() >= 2.0) {
                _drawPts.append(imgPt);
            } else {
                event->accept();
                return;
            }
        } else if (_toolMode == ToolMode::Point) {
            // Single point — update position
            if (!_drawPts.isEmpty()) _drawPts[0] = imgPt;
        } else {
            // Arrow/Rect/Circle/Text: two points, update second
            if (_drawPts.size() == 1)
                _drawPts.append(imgPt);
            else if (_drawPts.size() >= 2)
                _drawPts[1] = imgPt;
        }
        update();
        event->accept();
    }
}

void AnnotationOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;

    if (_toolMode == ToolMode::Select) {
        event->ignore();
        return;
    }

    if (!_drawing || _drawPts.isEmpty()) {
        _drawing = false;
        return;
    }

    // Finalize point (限制在画面内)
    QPointF imgPt = _clampToVideo(event->pos());

    if (_toolMode == ToolMode::Point) {
        if (!_drawPts.isEmpty()) _drawPts[0] = imgPt;
        else _drawPts.append(imgPt);
    } else if (_toolMode != ToolMode::FreeDraw) {
        if (_drawPts.size() < 2) _drawPts.append(imgPt);
        else _drawPts[1] = imgPt;
    }

    // Create annotation
    AnnotationItem ann;
    ann.frame = _currentFrame;
    ann.type  = static_cast<AnnotationType>(
        static_cast<int>(_toolMode) - 1);
    ann.color = _toolColor;
    ann.points = _drawPts;

    if (ann.type == AnnotationType::Text) {
        // For text, require a comment... for now use empty
        ann.comment = "";
    }

    _drawing = false;
    _drawPts.clear();
    update();

    Q_EMIT annotationCreated(ann);
    event->accept();
}

// ─── Context Menu ────────────────────────────────────────────────────────────
void AnnotationOverlay::contextMenuEvent(QContextMenuEvent* event)
{
    if (_selectedId.isEmpty()) {
        event->ignore();
        return;
    }

    QMenu menu(this);
    menu.setStyleSheet(
        "QMenu { background:#2d2d2d; color:#ddd; border:1px solid #555; }"
        "QMenu::item { padding:4px 20px; }"
        "QMenu::item:selected { background:#4a6fa5; }");
    QAction* delAct = menu.addAction(tr("Delete Annotation\tDel"));
    if (menu.exec(event->globalPos()) == delAct) {
        QString id = _selectedId;
        _selectedId.clear();
        update();
        Q_EMIT annotationDeleteRequested(id);
    }
    event->accept();
}

} // namespace cgplay
