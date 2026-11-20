#pragma once
#include <QStringList>
#include <QString>
#include <QSettings>
#include <QDir>
#include <QFileInfoList>
#include <vector>

struct PluginInfo {
    QString path;     // file path, or bundle directory for packaged VST3
    QString name;
    QString type;     // "VST2", "VST3", "CLAP" or "Invalid (...)"
    QString vendor;
    QString version;
    QString category; // "Instrument" or "Effect"
    QString uid;      // VST3 class id (hex), VST2 unique id, CLAP plugin id
    QString scanFingerprint; // Fast fingerprint of the resolved binary state
};

class PluginManager {
public:
    static PluginManager& GetInstance() {
        static PluginManager instance;
        return instance;
    }

    void SetCustomPaths(const QStringList& paths);
    QStringList GetCustomPaths() const;

    QStringList GetAutoDiscoveredPaths() const;
    QStringList GetAllPaths() const;

    void ScanPlugins(); // Legacy synchronous scan
    void SetDiscoveredPlugins(const std::vector<PluginInfo>& plugins);
    const std::vector<PluginInfo>& GetDiscoveredPlugins() const;

private:
    PluginManager();
    ~PluginManager() = default;

    QStringList m_customPaths;
    std::vector<PluginInfo> m_discoveredPlugins;
    
    void loadSettings();
    void saveSettings();
    void loadDiscoveredPlugins();
    void saveDiscoveredPlugins();
};

Q_DECLARE_METATYPE(PluginInfo)
Q_DECLARE_METATYPE(std::vector<PluginInfo>)
