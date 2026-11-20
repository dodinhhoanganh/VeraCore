#pragma once
#include "PluginManager.h"
#include <QByteArray>
#include <vector>

class PluginLoader {
public:
    // Scans one candidate (file, or .vst3 bundle directory) and returns every
    // plugin found inside it. A module can expose several plugins (VST3 multi
    // class factories, CLAP multi descriptor factories). On failure a single
    // entry is returned whose type starts with "Invalid".
    static std::vector<PluginInfo> ScanBinary(const PluginInfo& candidate);

    // Runs ScanBinary in a short-lived VeraCore helper process. A malformed,
    // crashing, or hanging foreign plug-in can only terminate that helper.
    static std::vector<PluginInfo> ScanBinaryIsolated(
        const PluginInfo& candidate, int timeoutMs = 15000, QString* errorText = nullptr);

    // Entry point used by main() before QApplication/audio/UI initialization.
    // args are QCoreApplication::arguments().
    static int RunScanHelper(const QStringList& args);

    // Backwards compatible wrapper: fills info with the first plugin found.
    static void ExtractMetadata(PluginInfo& info);

    // VeraCore is x64-only. Returns false before any foreign module is loaded
    // when the candidate is missing, malformed, or not an AMD64 PE binary.
    static bool IsX64Candidate(const PluginInfo& candidate);

    // Stable, low-I/O cache fingerprint for the resolved plug-in binary. Empty means
    // the candidate cannot be fingerprinted and must not use scan cache.
    static QString CandidateFingerprint(const PluginInfo& candidate);

    // Walks the search paths and returns plugin candidates. Handles VST3
    // bundle directories and skips helper DLLs living inside bundles.
    static std::vector<PluginInfo> CollectCandidates(const QStringList& searchPaths);

    // Maps a .vst3 path (bundle directory or plain file) to the loadable
    // binary. Returns an empty string when the bundle has no matching binary.
    static QString ResolveVst3Binary(const QString& path);
};
