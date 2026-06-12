#include "PluginScannerWorker.h"
#include "PluginLoader.h"
#include <QFileInfo>
#include <QMap>

PluginScannerWorker::PluginScannerWorker(const QStringList& searchPaths, const std::vector<PluginInfo>& existingPlugins, QObject* parent)
    : QThread(parent), m_searchPaths(searchPaths), m_existingPlugins(existingPlugins)
{
}

PluginScannerWorker::~PluginScannerWorker()
{
}

void PluginScannerWorker::run()
{
    std::vector<PluginInfo> discoveredPlugins;

    // Collect candidates first (handles VST3 bundle directories and skips
    // helper DLLs that live inside bundles).
    const std::vector<PluginInfo> candidates = PluginLoader::CollectCandidates(m_searchPaths);

    if (candidates.empty()) {
        emit scanFinished(discoveredPlugins);
        return;
    }

    // Cache from previous scans, keyed by path. Entries can hold several
    // plugins per path (multi-plugin VST3/CLAP modules).
    QMap<QString, std::vector<PluginInfo>> existingMap;
    for (const auto& pi : m_existingPlugins) {
        existingMap[pi.path].push_back(pi);
    }

    const int totalFiles = (int)candidates.size();
    int processedFiles = 0;

    for (const PluginInfo& candidate : candidates) {
        const int percent = (processedFiles * 100) / totalFiles;
        emit progressUpdated(percent, QFileInfo(candidate.path).fileName());

        // Reuse cached results only when they carry the new metadata format;
        // entries without a category come from older scans and are redone.
        bool usedCache = false;
        auto cacheIt = existingMap.constFind(candidate.path);
        if (cacheIt != existingMap.constEnd() && !cacheIt->empty() && !cacheIt->front().category.isEmpty()) {
            for (const PluginInfo& cached : *cacheIt) {
                if (!cached.type.startsWith("Invalid")) {
                    discoveredPlugins.push_back(cached);
                }
            }
            usedCache = true;
        }

        if (!usedCache) {
            const std::vector<PluginInfo> found = PluginLoader::ScanBinary(candidate);
            for (const PluginInfo& info : found) {
                if (!info.type.startsWith("Invalid")) {
                    discoveredPlugins.push_back(info);
                    emit pluginDiscovered(info);
                }
            }
        }

        processedFiles++;
    }

    emit progressUpdated(100, "Done");
    emit scanFinished(discoveredPlugins);
}
