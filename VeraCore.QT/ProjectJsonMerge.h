#pragma once

#include <QJsonObject>

namespace ProjectJsonMerge {

// Canonical values always win. Unknown fields from a loaded project are retained
// recursively, while canonical array membership controls additions/deletions.
QJsonObject mergeForSave(const QJsonObject& preserved,
                         const QJsonObject& canonical,
                         bool recoverySnapshot);

} // namespace ProjectJsonMerge
