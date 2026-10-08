#pragma once
// CGPlay AnnotationItem.h — 批注数据模型

#include <QString>
#include <QColor>
#include <QDateTime>
#include <QUuid>
#include <QRectF>
#include <QPointF>
#include <QVector>

namespace cgplay {

// ─── Annotation Types ─────────────────────────────────────────────────────────
enum class AnnotationType {
    Arrow,
    Rectangle,
    Circle,
    Text,
    FreeDraw,
    Point,
    _COUNT
};

inline QString annotationTypeString(AnnotationType t) {
    switch (t) {
    case AnnotationType::Arrow:     return "arrow";
    case AnnotationType::Rectangle: return "rectangle";
    case AnnotationType::Circle:    return "circle";
    case AnnotationType::Text:      return "text";
    case AnnotationType::FreeDraw:  return "freedraw";
    case AnnotationType::Point:     return "point";
    default: return "unknown";
    }
}

inline AnnotationType annotationTypeFromString(const QString& s) {
    if (s == "arrow")     return AnnotationType::Arrow;
    if (s == "rectangle") return AnnotationType::Rectangle;
    if (s == "circle")    return AnnotationType::Circle;
    if (s == "text")      return AnnotationType::Text;
    if (s == "freedraw")  return AnnotationType::FreeDraw;
    if (s == "point")     return AnnotationType::Point;
    return AnnotationType::Arrow;
}

// ─── Review Status ────────────────────────────────────────────────────────────
enum class ReviewStatus {
    Open,
    InProgress,
    Resolved,
    _COUNT
};

inline QString reviewStatusString(ReviewStatus s) {
    switch (s) {
    case ReviewStatus::Open:       return "Open";
    case ReviewStatus::InProgress: return "In Progress";
    case ReviewStatus::Resolved:   return "Resolved";
    default: return "Open";
    }
}

inline ReviewStatus reviewStatusFromString(const QString& s) {
    if (s == "In Progress") return ReviewStatus::InProgress;
    if (s == "Resolved")    return ReviewStatus::Resolved;
    return ReviewStatus::Open;
}

// ─── Comment ─────────────────────────────────────────────────────────────────
struct Comment {
    QString author = "user";
    QString text;
    QString time;
    ReviewStatus status = ReviewStatus::Open;
    QString assignee = "Other";
};

// ─── AnnotationItem ──────────────────────────────────────────────────────────
struct AnnotationItem
{
    QString        id;                    // UUID 前8位
    int            frame = 0;             // 绑定的帧号
    AnnotationType type = AnnotationType::Arrow;
    QColor         color = QColor(255, 0, 0); // #ff0000
    QString        author = "user";
    QString        createdTime;
    QString        comment;              // 主评论文本
    QVector<QPointF> points;            // 图像坐标
    QVector<Comment> comments;          // 历史评论
    ReviewStatus   status = ReviewStatus::Open;
    QString        assignee = "Other";

    AnnotationItem() {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        createdTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    }

    bool isValid() const { return !id.isEmpty() && !points.isEmpty(); }

    // 包围盒 [x, y, w, h] in image coords
    QRectF boundingRect() const {
        if (points.isEmpty()) return {};
        double xMin = points[0].x(), xMax = xMin;
        double yMin = points[0].y(), yMax = yMin;
        for (const auto& p : points) {
            if (p.x() < xMin) xMin = p.x();
            if (p.x() > xMax) xMax = p.x();
            if (p.y() < yMin) yMin = p.y();
            if (p.y() > yMax) yMax = p.y();
        }
        return QRectF(xMin, yMin, xMax - xMin, yMax - yMin);
    }

    void addComment(const QString& text,
                    const QString& author = "user",
                    ReviewStatus newStatus = ReviewStatus::Open,
                    const QString& newAssignee = "Other")
    {
        Comment c;
        c.author   = author;
        c.text     = text;
        c.time     = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
        c.status   = newStatus;
        c.assignee = newAssignee;
        comments.append(c);
        status   = newStatus;
        assignee = newAssignee;
    }

    QString latestCommentText() const {
        if (!comments.isEmpty())
            return comments.last().text;
        return comment;
    }
};

inline bool operator==(const AnnotationItem& lhs, const AnnotationItem& rhs)
{
    return lhs.id == rhs.id &&
           lhs.frame == rhs.frame &&
           lhs.type == rhs.type &&
           lhs.color == rhs.color &&
           lhs.comment == rhs.comment &&
           lhs.points == rhs.points &&
           lhs.status == rhs.status &&
           lhs.assignee == rhs.assignee;
}

} // namespace cgplay
