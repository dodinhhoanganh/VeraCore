#include "PluginManager.h"
#include "PluginLoader.h"
#include <QDirIterator>
#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

PluginManager::PluginManager() {
    loadSettings();
    loadDiscoveredPlugins();
}

void PluginManager::loadSettings() {
    QSettings settings("VeraCore", "Preferences");
    m_customPaths = settings.value("PluginPaths").toStringList();
}

void PluginManager::saveSettings() {
    QSettings settings("VeraCore", "Preferences");
    settings.setValue("PluginPaths", m_customPaths);
}

void PluginManager::loadDiscoveredPlugins() {
    QSettings settings("VeraCore", "Preferences");
    int size = settings.beginReadArray("DiscoveredPlugins");
    m_discoveredPlugins.clear();
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        PluginInfo info;
        info.path = settings.value("path").toString();
        info.name = settings.value("name").toString();
        info.type = settings.value("type").toString();
        info.vendor = settings.value("vendor").toString();
        info.version = settings.value("version").toString();
        info.category = settings.value("category").toString();
        info.uid = settings.value("uid").toString();
        m_discoveredPlugins.push_back(info);
    }
    settings.endArray();
}

void PluginManager::saveDiscoveredPlugins() {
    QSettings settings("VeraCore", "Preferences");
    settings.beginWriteArray("DiscoveredPlugins");
    for (int i = 0; i < m_discoveredPlugins.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue("path", m_discoveredPlugins[i].path);
        settings.setValue("name", m_discoveredPlugins[i].name);
        settings.setValue("type", m_discoveredPlugins[i].type);
        settings.setValue("vendor", m_discoveredPlugins[i].vendor);
        settings.setValue("version", m_discoveredPlugins[i].version);
        settings.setValue("category", m_discoveredPlugins[i].category);
        settings.setValue("uid", m_discoveredPlugins[i].uid);
    }
    settings.endArray();
}

void PluginManager::SetCustomPaths(const QStringList& paths) {
    m_customPaths = paths;
    m_customPaths.removeDuplicates();
    saveSettings();
}

QStringList PluginManager::GetCustomPaths() const {
    return m_customPaths;
}

QStringList PluginManager::GetAutoDiscoveredPaths() const {
    QStringList paths;

#ifdef Q_OS_WIN
    // 1. VST3 Default Paths
    paths << "C:/Program Files/Common Files/VST3";
    paths << "C:/Program Files (x86)/Common Files/VST3";

    // 2. CLAP Default Paths
    paths << "C:/Program Files/Common Files/CLAP";
    paths << "C:/Program Files (x86)/Common Files/CLAP";

    // 3. VST2 Registry Paths
    QSettings regVST2("HKEY_LOCAL_MACHINE\\SOFTWARE\\VST", QSettings::NativeFormat);
    if (regVST2.contains("VSTPluginsPath")) {
        paths << regVST2.value("VSTPluginsPath").toString();
    }
    
    // Common fallback for VST2
    paths << "C:/Program Files/VSTPlugins";
    paths << "C:/Program Files/Steinberg/VstPlugins";
#endif

    // Clean up paths
    QStringList validPaths;
    for (const QString& p : paths) {
        QDir dir(p);
        if (dir.exists()) {
            validPaths << dir.absolutePath();
        }
    }
    validPaths.removeDuplicates();
    return validPaths;
}

QStringList PluginManager::GetAllPaths() const {
    QStringList allPaths = GetCustomPaths() + GetAutoDiscoveredPaths();
    allPaths.removeDuplicates();
    return allPaths;
}

void PluginManager::ScanPlugins() {
    m_discoveredPlugins.clear();

    const std::vector<PluginInfo> candidates = PluginLoader::CollectCandidates(GetAllPaths());
    for (const PluginInfo& candidate : candidates) {
        const std::vector<PluginInfo> found = PluginLoader::ScanBinary(candidate);
        for (const PluginInfo& info : found) {
            if (!info.type.startsWith("Invalid")) {
                m_discoveredPlugins.push_back(info);
            }
        }
    }
}

void PluginManager::SetDiscoveredPlugins(const std::vector<PluginInfo>& plugins) {
    m_discoveredPlugins = plugins;
    saveDiscoveredPlugins();
}

const std::vector<PluginInfo>& PluginManager::GetDiscoveredPlugins() const {
    return m_discoveredPlugins;
}
