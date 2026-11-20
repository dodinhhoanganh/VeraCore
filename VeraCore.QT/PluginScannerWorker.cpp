#include "PluginScannerWorker.h"
#include "PluginLoader.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSettings>
#include <QSet>

namespace {

constexpr char kFailureCacheSettingsKey[] = "PluginScan/FailureCacheV1";

QString candidateIdentity(const PluginInfo& info)
{
    return info.type.toUpper() + QLatin1Char('|') +
        QDir::cleanPath(QFileInfo(info.path).absoluteFilePath()).toCaseFolded();
}

QJsonObject loadQuarantine()
{
    QSettings settings("VeraCore", "Preferences");
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(
        settings.value(kFailureCacheSettingsKey).toByteArray(), &error);
    return error.error == QJsonParseError::NoError && document.isObject()
        ? document.object() : QJsonObject{};
}

void saveQuarantine(const QJsonObject& quarantine)
{
    QSettings settings("VeraCore", "Preferences");
    if (quarantine.isEmpty())
        settings.remove(kFailureCacheSettingsKey);
    else
        settings.setValue(kFailureCacheSettingsKey,
            QJsonDocument(quarantine).toJson(QJsonDocument::Compact));
    settings.sync();
}

bool isHazardousFailure(const QString& scannerError, const QString& resultType)
{
    const QString text = (scannerError + QLatin1Char(' ') + resultType).toLower();
    return text.contains("timeout") || text.contains("crash") ||
        text.contains("abnormal") || text.contains("protocol") ||
        text.contains("failed to start") || text.contains("exit ");
}

} // namespace

PluginScannerWorker::PluginScannerWorker(
    const QStringList& searchPaths, const std::vector<PluginInfo>& existingPlugins,
    bool forceRescan, QObject* parent)
    : QThread(parent), m_searchPaths(searchPaths),
      m_existingPlugins(existingPlugins), m_forceRescan(forceRescan)
{
}

PluginScannerWorker::~PluginScannerWorker() = default;

int PluginScannerWorker::QuarantinedCount()
{
    const QJsonObject quarantine = loadQuarantine();
    int count = 0;
    for (auto it = quarantine.constBegin(); it != quarantine.constEnd(); ++it) {
        if (it.value().toObject().value("quarantined").toBool()) ++count;
    }
    return count;
}

void PluginScannerWorker::ClearQuarantine()
{
    QJsonObject quarantine = loadQuarantine();
    for (auto it = quarantine.begin(); it != quarantine.end();) {
        if (it.value().toObject().value("quarantined").toBool())
            it = quarantine.erase(it);
        else
            ++it;
    }
    saveQuarantine(quarantine);
}

void PluginScannerWorker::run()
{
    std::vector<PluginInfo> discoveredPlugins;
    const std::vector<PluginInfo> candidates =
        PluginLoader::CollectCandidates(m_searchPaths);
    if (candidates.empty()) {
        emit scanFinished(discoveredPlugins);
        return;
    }

    QMap<QString, std::vector<PluginInfo>> existingMap;
    for (const PluginInfo& info : m_existingPlugins)
        existingMap[candidateIdentity(info)].push_back(info);

    QJsonObject quarantine = loadQuarantine();
    bool quarantineChanged = false;
    QSet<QString> candidateIdentities;
    for (const PluginInfo& candidate : candidates)
        candidateIdentities.insert(candidateIdentity(candidate));
    for (auto it = quarantine.begin(); it != quarantine.end();) {
        if (!candidateIdentities.contains(it.key())) {
            it = quarantine.erase(it);
            quarantineChanged = true;
        } else {
            ++it;
        }
    }
    const int totalFiles = static_cast<int>(candidates.size());
    int processedFiles = 0;

    for (const PluginInfo& candidate : candidates) {
        const QString displayName = QFileInfo(candidate.path).fileName();
        const QString identity = candidateIdentity(candidate);
        const QString fingerprint = PluginLoader::CandidateFingerprint(candidate);
        QJsonObject quarantineEntry = quarantine.value(identity).toObject();
        if (!fingerprint.isEmpty() && !quarantineEntry.isEmpty() &&
            quarantineEntry.value("fingerprint").toString() != fingerprint) {
            quarantine.remove(identity);
            quarantineEntry = {};
            quarantineChanged = true;
        }
        const bool cachedFailure = !m_forceRescan && !fingerprint.isEmpty() &&
            !quarantineEntry.isEmpty() &&
            quarantineEntry.value("fingerprint").toString() == fingerprint;
        const bool quarantined = cachedFailure &&
            quarantineEntry.value("quarantined").toBool();

        emit progressUpdated((processedFiles * 100) / totalFiles,
            quarantined ? QString("Quarantined: %1").arg(displayName)
                        : cachedFailure ? QString("Cached invalid: %1").arg(displayName)
                                        : displayName);

        if (cachedFailure) {
            ++processedFiles;
            continue;
        }

        bool usedCache = false;
        const auto cacheIt = existingMap.constFind(identity);
        if (!m_forceRescan && !fingerprint.isEmpty() &&
            cacheIt != existingMap.constEnd() && !cacheIt->empty() &&
            cacheIt->front().scanFingerprint == fingerprint) {
            for (const PluginInfo& cached : *cacheIt)
                if (!cached.type.startsWith("Invalid")) discoveredPlugins.push_back(cached);
            usedCache = true;
        }

        if (!usedCache) {
            QString scannerError;
            const std::vector<PluginInfo> found =
                PluginLoader::ScanBinaryIsolated(candidate, 15000, &scannerError);
            bool foundValidPlugin = false;
            QString resultType;
            for (PluginInfo info : found) {
                if (resultType.isEmpty()) resultType = info.type;
                if (info.type.startsWith("Invalid")) continue;
                info.scanFingerprint = fingerprint;
                discoveredPlugins.push_back(info);
                emit pluginDiscovered(info);
                foundValidPlugin = true;
            }

            if (foundValidPlugin) {
                if (quarantine.contains(identity)) {
                    quarantine.remove(identity);
                    quarantineChanged = true;
                }
            } else if (!fingerprint.isEmpty()) {
                const bool hazardous = isHazardousFailure(scannerError, resultType);
                const bool sameBinary =
                    quarantineEntry.value("fingerprint").toString() == fingerprint;
                QJsonObject updated;
                updated["path"] = candidate.path;
                updated["type"] = candidate.type;
                updated["fingerprint"] = fingerprint;
                updated["reason"] = (scannerError.isEmpty() ? resultType : scannerError).left(512);
                updated["failures"] = sameBinary
                    ? quarantineEntry.value("failures").toInt() + 1 : 1;
                updated["quarantined"] = hazardous;
                updated["updatedUtcMs"] =
                    static_cast<double>(QDateTime::currentMSecsSinceEpoch());
                quarantine[identity] = updated;
                quarantineChanged = true;
                if (hazardous) {
                    saveQuarantine(quarantine);
                    quarantineChanged = false;
                }
            }
        }

        ++processedFiles;
    }

    if (quarantineChanged) saveQuarantine(quarantine);
    emit progressUpdated(100, "Done");
    emit scanFinished(discoveredPlugins);
}
