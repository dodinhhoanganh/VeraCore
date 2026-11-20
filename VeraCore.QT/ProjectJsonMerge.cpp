#include "ProjectJsonMerge.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QStringList>

namespace {
constexpr int kMaximumMergeDepth = 64;

QString stableIdentity(const QJsonObject& object)
{
    static const QStringList keys{
        QStringLiteral("clipId"), QStringLiteral("contentId"),
        QStringLiteral("trackId"), QStringLiteral("layerId"),
        QStringLiteral("routeId"), QStringLiteral("parameterId"),
        QStringLiteral("noteId"), QStringLiteral("id"),
        QStringLiteral("pluginUid")
    };
    for (const QString& key : keys) {
        const QJsonValue value = object.value(key);
        if (value.isString() && !value.toString().isEmpty())
            return key + QLatin1Char(':') + value.toString();
        if (value.isDouble())
            return key + QLatin1Char(':') + QString::number(value.toDouble(), 'g', 17);
    }
    return {};
}

QJsonValue mergeValue(const QJsonValue& preserved, const QJsonValue& canonical, int depth);

QJsonArray mergeArray(const QJsonArray& preserved, const QJsonArray& canonical, int depth)
{
    if (depth >= kMaximumMergeDepth || preserved.isEmpty() || canonical.isEmpty())
        return canonical;

    QHash<QString, int> preservedByIdentity;
    preservedByIdentity.reserve(preserved.size());
    for (qsizetype index = 0; index < preserved.size(); ++index) {
        if (!preserved[index].isObject()) continue;
        const QString identity = stableIdentity(preserved[index].toObject());
        if (identity.isEmpty()) continue;
        const auto found = preservedByIdentity.constFind(identity);
        if (found == preservedByIdentity.cend()) preservedByIdentity.insert(identity, index);
        else preservedByIdentity[identity] = -1;
    }

    QJsonArray result;
    for (qsizetype index = 0; index < canonical.size(); ++index) {
        const QJsonValue canonicalValue = canonical[index];
        int preservedIndex = -1;
        if (canonicalValue.isObject()) {
            const QString identity = stableIdentity(canonicalValue.toObject());
            const auto found = preservedByIdentity.constFind(identity);
            if (!identity.isEmpty() && found != preservedByIdentity.cend() && found.value() >= 0)
                preservedIndex = found.value();
        }
        if (preservedIndex < 0 && preserved.size() == canonical.size())
            preservedIndex = static_cast<int>(index);

        result.append(preservedIndex >= 0 && preservedIndex < preserved.size()
            ? mergeValue(preserved[preservedIndex], canonicalValue, depth + 1)
            : canonicalValue);
    }
    return result;
}

QJsonObject mergeObject(const QJsonObject& preserved, const QJsonObject& canonical, int depth)
{
    if (depth >= kMaximumMergeDepth) return canonical;
    QJsonObject result = preserved;
    for (auto it = canonical.begin(); it != canonical.end(); ++it)
        result.insert(it.key(), mergeValue(preserved.value(it.key()), it.value(), depth + 1));
    return result;
}

QJsonValue mergeValue(const QJsonValue& preserved, const QJsonValue& canonical, int depth)
{
    if (depth >= kMaximumMergeDepth) return canonical;
    if (preserved.isObject() && canonical.isObject())
        return mergeObject(preserved.toObject(), canonical.toObject(), depth);
    if (preserved.isArray() && canonical.isArray())
        return mergeArray(preserved.toArray(), canonical.toArray(), depth);
    return canonical;
}
}

QJsonObject ProjectJsonMerge::mergeForSave(const QJsonObject& preserved,
                                            const QJsonObject& canonical,
                                            bool recoverySnapshot)
{
    QJsonObject result = mergeObject(preserved, canonical, 0);
    result.remove(QStringLiteral("midiDocuments"));
    if (!recoverySnapshot) {
        result.remove(QStringLiteral("recoverySnapshot"));
        result.remove(QStringLiteral("recoverySourcePath"));
        result.remove(QStringLiteral("recoveryCreatedUtcMs"));
    }
    return result;
}
