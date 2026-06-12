#pragma once
#include "PluginManager.h"
#include <vector>

class PluginLoader {
public:
    // Scans one candidate (file, or .vst3 bundle directory) and returns every
    // plugin found inside it. A module can expose several plugins (VST3 multi
    // class factories, CLAP multi descriptor factories). On failure a single
    // entry is returned whose type starts with "Invalid".
    static std::vector<PluginInfo> ScanBinary(const PluginInfo& candidate);

    // Backwards compatible wrapper: fills info with the first plugin found.
    static void ExtractMetadata(PluginInfo& info);

    // Walks the search paths and returns plugin candidates. Handles VST3
    // bundle directories and skips helper DLLs living inside bundles.
    static std::vector<PluginInfo> CollectCandidates(const QStringList& searchPaths);

    // Maps a .vst3 path (bundle directory or plain file) to the loadable
    // binary. Returns an empty string when the bundle has no matching binary.
    static QString ResolveVst3Binary(const QString& path);
};
