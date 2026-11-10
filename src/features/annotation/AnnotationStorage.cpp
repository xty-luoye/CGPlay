// CGPlay AnnotationStorage.cpp

#include "AnnotationStorage.h"
#include "AnnotationItem.h"

#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

namespace cgplay {

bool AnnotationStorage::save(const QVector<AnnotationItem>& annotations,
                              const QString& filepath)
{
    QFileInfo fi(filepath);
    QDir().mkpath(fi.path());

    QJsonObject root;
    root["version"] = 1;

    QJsonArray arr;
    for (const auto& ann : annotations) {
        QJsonObject obj;
        obj["id"]           = ann.id;
        obj["frame"]        = ann.frame;
        obj["type"]         = annotationTypeString(ann.type);
        obj["color"]        = ann.color.name();
        obj["author"]       = ann.author;
        obj["created_time"] = ann.createdTime;
        obj["comment"]      = ann.comment;

        QJsonArray ptsArr;
        for (const auto& pt : ann.points) {
            QJsonArray ptArr;
            ptArr.append(pt.x());
            ptArr.append(pt.y());
            ptsArr.append(ptArr);
        }
        obj["points"] = ptsArr;

        QJsonArray cmtsArr;
        for (const auto& c : ann.comments) {
            QJsonObject co;
            co["author"]   = c.author;
            co["text"]     = c.text;
            co["time"]     = c.time;
            co["status"]   = reviewStatusString(c.status);
            co["assignee"] = c.assignee;
            cmtsArr.append(co);
        }
        obj["comments"] = cmtsArr;
        obj["status"]   = reviewStatusString(ann.status);
        obj["assignee"] = ann.assignee;

        arr.append(obj);
    }
    root["annotations"] = arr;

    QSaveFile f(filepath);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "[AnnotationStorage] Cannot write:" << filepath;
        return false;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return f.write(data) == data.size() && f.commit();
}

QVector<AnnotationItem> AnnotationStorage::load(const QString& filepath)
{
    QVector<AnnotationItem> result;
    load(filepath, &result);
    return result;
}

bool AnnotationStorage::load(const QString& filepath, QVector<AnnotationItem>* annotations,
                             QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!annotations) return fail(QStringLiteral("缺少批注接收对象"));
    QVector<AnnotationItem> result;

    QFile f(filepath);
    if (!f.open(QIODevice::ReadOnly)) return fail(f.errorString());

    const QByteArray data = f.readAll();
    if (f.error() != QFileDevice::NoError) return fail(f.errorString());
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    f.close();

    if (parseError.error != QJsonParseError::NoError) return fail(parseError.errorString());
    if (!doc.isObject() || !doc.object().value("annotations").isArray())
        return fail(QStringLiteral("无效的批注 JSON：缺少 annotations 数组"));

    auto root = doc.object();
    auto arr  = root["annotations"].toArray();

    for (const auto& val : arr) {
        if (!val.isObject()) return fail(QStringLiteral("无效的批注记录"));
        auto obj = val.toObject();
        if (obj.value("id").toString().isEmpty() || !obj.value("points").isArray())
            return fail(QStringLiteral("批注缺少标识或坐标"));
        AnnotationItem ann;
        ann.id          = obj["id"].toString();
        ann.frame       = obj["frame"].toInt();
        ann.type        = annotationTypeFromString(obj["type"].toString());
        ann.color       = QColor(obj["color"].toString());
        ann.author      = obj["author"].toString();
        ann.createdTime = obj["created_time"].toString();
        ann.comment     = obj["comment"].toString();

        auto ptsArr = obj["points"].toArray();
        for (const auto& p : ptsArr) {
            auto ptArr = p.toArray();
            if (ptArr.size() < 2 || !ptArr[0].isDouble() || !ptArr[1].isDouble())
                return fail(QStringLiteral("无效的批注坐标"));
            ann.points.append(QPointF(ptArr[0].toDouble(), ptArr[1].toDouble()));
        }

        auto cmtsArr = obj["comments"].toArray();
        for (const auto& c : cmtsArr) {
            auto co = c.toObject();
            Comment comment;
            comment.author   = co["author"].toString("user");
            comment.text     = co["text"].toString();
            comment.time     = co["time"].toString();
            comment.status   = reviewStatusFromString(co["status"].toString("Open"));
            comment.assignee = co["assignee"].toString("Other");
            ann.comments.append(comment);
        }
        ann.status   = reviewStatusFromString(obj["status"].toString("Open"));
        ann.assignee = obj["assignee"].toString("Other");

        result.append(ann);
    }
    *annotations = std::move(result);
    if (error) error->clear();
    return true;
}

QString AnnotationStorage::getReviewPath(const QString& mediaPath)
{
    QFileInfo fi(mediaPath);
    if (fi.isDir())
        return fi.absoluteFilePath() + ".review.json";
    return fi.absolutePath() + "/" + fi.completeBaseName() + ".review.json";
}

} // namespace cgplay
