#pragma once
#include <QThread>
#include <QStringList>
#include "PluginManager.h"

class PluginScannerWorker : public QThread {
    Q_OBJECT
public:
    PluginScannerWorker(const QStringList& searchPaths, const std::vector<PluginInfo>& existingPlugins, QObject* parent = nullptr);
    ~PluginScannerWorker();

signals:
    void progressUpdated(int percent, const QString& currentPlugin);
    void pluginDiscovered(const PluginInfo& info);
    void scanFinished(const std::vector<PluginInfo>& plugins);

protected:
    void run() override;

private:
    QStringList m_searchPaths;
    std::vector<PluginInfo> m_existingPlugins;
};
