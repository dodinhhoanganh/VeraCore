#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

struct StemRecoveryJob
{
    QString manifestPath;
    QString jobId;
    QString state;
    QString sourcePath;
    QString outputDirectory;
    QStringList expectedStems;
    QStringList outputPaths;
    QString updatedUtc;
    QString message;
    bool sourceValid = false;
    bool outputsComplete = false;
    bool inputWasRendered = false;
};

struct StemModelPresetStatus
{
    int stemCount = 0;
    QString presetName;
    QString directory;
    QString aggregateSha256;
    qint64 totalBytes = 0;
    int fileCount = 0;
    bool installed = false;
    bool checksumMatch = false;
};

struct StemModelCacheReport
{
    QString rootDirectory;
    QString manifestPath;
    QVector<StemModelPresetStatus> presets;
    QString message;
    int installedPresetCount = 0;
    bool success = false;
    bool checksumsCalculated = false;
    bool checksumManifestFound = false;
    bool checksumBaselineCreated = false;
    bool allInstalledChecksumsMatch = false;
    bool cancelled = false;
};

using StemModelProgress = std::function<bool(qint64 completedBytes,
                                             qint64 totalBytes,
                                             const QString& currentFile)>;

class StemSeparationService final : public QObject
{
    Q_OBJECT

public:
    explicit StemSeparationService(QObject* parent = nullptr);

    bool isBusy() const noexcept { return m_active; }
    QString activeJobId() const { return m_jobId; }
    QString activeManifestPath() const;
    static bool resolveRuntime(QString* program, QStringList* prefixArguments,
                               QString* description = nullptr);
    static bool resolveDemucsRuntime(QString* program, QStringList* prefixArguments,
                                     QString* description = nullptr);
    static QVector<StemRecoveryJob> scanRecoverableJobs(
        const QStringList& roots, int maximumJobs = 128);
    static StemModelCacheReport inspectModelCache(
        const QString& modelRoot, bool calculateChecksums = false,
        bool initializeChecksumBaseline = false,
        const StemModelProgress& progress = {});
    static bool markInterruptedJobRetried(const QString& manifestPath,
                                          const QString& replacementJobId,
                                          QString* errorText = nullptr);
    static bool validateCompletedJobSource(const QString& manifestPath,
                                           const QString& expectedSourcePath,
                                           QString* errorText = nullptr);
    static bool markCompletedJobImported(const QString& manifestPath,
                                         const QString& projectPath,
                                         const QString& sourceClipId,
                                         int importedTrackCount,
                                         QString* errorText = nullptr);
    bool startSeparation(const QString& inputPath, const QString& outputDirectory,
                         int stemCount, QString* errorText = nullptr,
                         const QString& provenanceSourcePath = {},
                         bool inputWasRendered = false);
    void cancel();

signals:
    void progressChanged(const QString& jobId, int percent, const QString& detail);
    void separationFinished(const QString& jobId, const QString& inputPath,
                            const QStringList& stemNames, const QStringList& stemPaths);
    void separationFailed(const QString& jobId, const QString& message);
    void separationCancelled(const QString& jobId);

private:
    struct LaunchCommand {
        QString program;
        QStringList prefixArguments;
        QString backend;
    };

    static LaunchCommand resolveSpleeterLaunchCommand();
    static LaunchCommand resolveDemucsLaunchCommand();
    static QStringList namesForStemCount(int stemCount);
    static QString conciseProcessLine(const QByteArray& bytes);
    bool collectOutputs(QStringList* paths, QString* errorText) const;
    void consumeProcessOutput();
    bool writeJobManifest(const QString& state, int exitCode = -1,
                          const QString& message = {},
                          const QStringList& validatedOutputPaths = {}) const;
    bool launchBackend(const LaunchCommand& command, QString* errorText = nullptr);
    bool tryDemucsFallback(const QString& reason);
    void resetJob();

    QProcess* m_process = nullptr;
    QString m_jobId;
    QString m_inputPath;
    QString m_provenanceSourcePath;
    QString m_outputDirectory;
    QString m_backendOutputDirectory;
    QString m_runtimeProgram;
    QString m_modelDirectory;
    QString m_computeMode;
    int m_cpuThreadLimit = 1;
    QString m_requestedBackend;
    QString m_activeBackend;
    QString m_fallbackReason;
    QStringList m_stemNames;
    QByteArray m_processOutput;
    bool m_active = false;
    bool m_cancelRequested = false;
    bool m_inputWasRendered = false;
    bool m_fallbackAttempted = false;
};
