#pragma once
// CGPlay AnnotationStorage.h — .review.json 持久化

#include <QString>
#include <QVector>

namespace cgplay {

struct AnnotationItem;

class AnnotationStorage
{
public:
    static bool save(const QVector<AnnotationItem>& annotations,
                     const QString& filepath);
    static QVector<AnnotationItem> load(const QString& filepath);

    static QString getReviewPath(const QString& mediaPath);
};

} // namespace cgplay
