#include "StemSeparationService.h"
#include "AudioCodecBackend.h"
#include "AudioFileDecoder.h"

#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
QString executableFromSetting(const char* key)
{
    const QString configured = QSettings().value(QLatin1String(key)).toString().trimmed();
    if (configured.isEmpty()) return {};
    const QFileInfo info(configured);
    return info.exists() && info.isFile() ? info.absoluteFilePath() : QString{};
}

constexpr int kMaximumModelFiles = 4096;
constexpr qint64 kMaximumModelBytes = 32ll * 1024ll * 1024ll * 1024ll;
constexpr qint64 kModelHashBlockBytes = 1024 * 1024;
constexpr double kMaximumStemAlignmentErrorSeconds = 0.002;

bool probeValidatedStemWav(const QString& path, const QString& outputRoot,
                           AudioFileProbeResult* probe, QString* errorText)
{
    const QFileInfo rootInfo(outputRoot);
    const QFileInfo fileInfo(path);
    const QString rootCanonical = rootInfo.canonicalFilePath();
    const QString fileCanonical = fileInfo.canonicalFilePath();
    if (!rootInfo.isDir() || rootInfo.isSymLink() || rootCanonical.isEmpty() ||
        !fileInfo.isFile() || fileInfo.isSymLink() || fileInfo.size() <= 44 ||
        fileCanonical.isEmpty()) {
        if (errorText) *errorText = QObject::tr("A stem output is missing or unsafe.");
        return false;
    }
    const QString relative = QDir(rootCanonical).relativeFilePath(fileCanonical);
    if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) ||
        QDir::isAbsolutePath(relative)) {
        if (errorText) *errorText = QObject::tr("A stem output escaped its job directory.");
        return false;
    }
    QString headerError;
    if (!AudioCodecBackend::hasValidContainerHeader(
            fileCanonical, AudioCodecFormat::Wav, &headerError)) {
        if (errorText) *errorText = headerError.isEmpty()
            ? QObject::tr("A stem output has an invalid WAV header.") : headerError;
        return false;
    }
    const AudioFileProbeResult result = AudioFileDecoder::probeAudio(fileCanonical);
    if (!result.success || result.sampleRate <= 0 || result.frameCount <= 0 ||
        !std::isfinite(result.durationSeconds()) || result.durationSeconds() <= 0.0) {
        if (errorText) *errorText = result.message.isEmpty()
            ? QObject::tr("A stem output contains no decodable audio.") : result.message;
        return false;
    }
    if (probe) *probe = result;
    return true;
}

bool stemDurationMatches(const AudioFileProbeResult& reference,
                         const AudioFileProbeResult& candidate)
{
    if (!reference.success || !candidate.success) return false;
    const double frameTolerance = 2.0 / static_cast<double>(
        std::max(1, std::min(reference.sampleRate, candidate.sampleRate)));
    const double tolerance = std::max(kMaximumStemAlignmentErrorSeconds, frameTolerance);
    return std::abs(reference.durationSeconds() - candidate.durationSeconds()) <= tolerance;
}

QJsonObject stemOutputIdentity(const QString& stem, const QString& path,
                               const QString& jobRoot)
{
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    const QString rootCanonical = QFileInfo(jobRoot).canonicalFilePath();
    const AudioFileProbeResult probe = AudioFileDecoder::probeAudio(canonical);
    if (canonical.isEmpty() || rootCanonical.isEmpty() || !probe.success) return {};
    const QString relative = QDir(rootCanonical).relativeFilePath(canonical);
    if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) ||
        QDir::isAbsolutePath(relative)) return {};
    QJsonObject identity;
    identity.insert(QStringLiteral("stem"), stem.toLower());
    identity.insert(QStringLiteral("relativePath"), relative);
    identity.insert(QStringLiteral("sizeBytes"), QString::number(file.size()));
    identity.insert(QStringLiteral("modifiedUtc"),
                    file.lastModified().toUTC().toString(Qt::ISODateWithMs));
    identity.insert(QStringLiteral("sampleRate"), probe.sampleRate);
    identity.insert(QStringLiteral("channelCount"), probe.channelCount);
    identity.insert(QStringLiteral("frameCount"), QString::number(probe.frameCount));
    return identity;
}

bool recordedStemOutputMatches(const QJsonObject& identity, const QString& stem,
                               const QString& actualPath, const QString& jobRoot,
                               const AudioFileProbeResult& actualProbe)
{
    if (identity.value(QStringLiteral("stem")).toString().compare(
            stem, Qt::CaseInsensitive) != 0 || !actualProbe.success) return false;
    const QString rootCanonical = QFileInfo(jobRoot).canonicalFilePath();
    const QString actualCanonical = QFileInfo(actualPath).canonicalFilePath();
    const QString relative = QDir::cleanPath(
        identity.value(QStringLiteral("relativePath")).toString());
    const QString recordedCanonical = QFileInfo(
        QDir(rootCanonical).filePath(relative)).canonicalFilePath();
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
    if (rootCanonical.isEmpty() || actualCanonical.isEmpty() || relative.isEmpty() ||
        relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) ||
        QDir::isAbsolutePath(relative) || recordedCanonical.isEmpty() ||
        recordedCanonical.compare(actualCanonical, pathCase) != 0) return false;
    bool sizeValid = false;
    bool framesValid = false;
    const qint64 size = identity.value(QStringLiteral("sizeBytes"))
        .toString().toLongLong(&sizeValid);
    const int frames = identity.value(QStringLiteral("frameCount"))
        .toString().toInt(&framesValid);
    const QDateTime modified = QDateTime::fromString(
        identity.value(QStringLiteral("modifiedUtc")).toString(), Qt::ISODateWithMs);
    const QFileInfo actual(actualCanonical);
    return sizeValid && size > 0 && actual.size() == size && framesValid && frames > 0 &&
        actualProbe.frameCount == frames &&
        actualProbe.sampleRate == identity.value(QStringLiteral("sampleRate")).toInt() &&
        actualProbe.channelCount == identity.value(QStringLiteral("channelCount")).toInt() &&
        modified.isValid() &&
        qAbs(actual.lastModified().toUTC().msecsTo(modified)) <= 2000;
}

QString stemModelManifestPath(const QString& root)
{
    return QDir(root).filePath(QStringLiteral("VeraCoreStemModels.sha256.json"));
}

QString locatedPresetDirectory(const QString& root, const QString& preset)
{
    const QString rootCanonical = QFileInfo(root).canonicalFilePath();
    if (rootCanonical.isEmpty()) return {};
    const QStringList candidates = {
        QDir(rootCanonical).filePath(preset),
        QDir(rootCanonical).filePath(QStringLiteral("pretrained_models/%1").arg(preset))
    };
    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate);
        const QString canonical = info.canonicalFilePath();
        if (!info.isDir() || info.isSymLink() || canonical.isEmpty()) continue;
        const QString relative = QDir(rootCanonical).relativeFilePath(canonical);
        if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) ||
            QDir::isAbsolutePath(relative)) continue;
        return canonical;
    }
    return {};
}

QByteArray hashModelFile(const QString& path, qint64* completedBytes,
                         qint64 totalBytes, const StemModelProgress& progress,
                         bool* cancelled)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray block = file.read(kModelHashBlockBytes);
        if (block.isEmpty() && !file.atEnd()) return {};
        hash.addData(block);
        if (completedBytes) *completedBytes += block.size();
        if (progress && !progress(completedBytes ? *completedBytes : 0,
                                  totalBytes, path)) {
            if (cancelled) *cancelled = true;
            return {};
        }
    }
    return hash.result();
}
}

StemSeparationService::StemSeparationService(QObject* parent)
    : QObject(parent), m_process(new QProcess(this))
{
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::started, this, [this]() {
        if (!m_active) return;
        writeJobManifest(QStringLiteral("running"));
        emit progressChanged(m_jobId, 5, tr("%1 model started").arg(m_activeBackend));
    });
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &StemSeparationService::consumeProcessOutput);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!m_active || error != QProcess::FailedToStart) return;
        const QString job = m_jobId;
        const QString detail = m_process->errorString();
        const QString message = tr("Could not start %1: %2")
            .arg(m_activeBackend, detail);
        if (tryDemucsFallback(message)) return;
        writeJobManifest(QStringLiteral("failed"), -1, message);
        resetJob();
        emit separationFailed(job, message);
    });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!m_active) return;
        consumeProcessOutput();
        const QString job = m_jobId;
        if (m_cancelRequested) {
            writeJobManifest(QStringLiteral("cancelled"), exitCode,
                             tr("Cancelled by user."));
            resetJob();
            emit separationCancelled(job);
            return;
        }
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            const QString detail = conciseProcessLine(m_processOutput);
            const QString message = detail.isEmpty()
                ? tr("%1 exited with code %2.").arg(m_activeBackend).arg(exitCode)
                : tr("%1 exited with code %2: %3")
                    .arg(m_activeBackend).arg(exitCode).arg(detail);
            if (tryDemucsFallback(message)) return;
            writeJobManifest(QStringLiteral("failed"), exitCode, message);
            resetJob();
            emit separationFailed(job, message);
            return;
        }
        writeJobManifest(QStringLiteral("validating"), exitCode);
        emit progressChanged(job, 95, tr("Validating separated audio"));
        QStringList paths;
        QString error;
        if (!collectOutputs(&paths, &error)) {
            writeJobManifest(QStringLiteral("failed"), exitCode, error);
            resetJob();
            emit separationFailed(job, error);
            return;
        }
        const QString input = m_inputPath;
        const QStringList names = m_stemNames;
        writeJobManifest(QStringLiteral("completed"), exitCode,
                         tr("Validated %1 stem files.").arg(paths.size()), paths);
        resetJob();
        emit progressChanged(job, 100, tr("Stem separation complete"));
        emit separationFinished(job, input, names, paths);
    });
}

QString StemSeparationService::activeManifestPath() const
{
    return m_outputDirectory.isEmpty() ? QString()
        : QDir(m_outputDirectory).filePath(QStringLiteral("VeraCoreStemJob.json"));
}

StemSeparationService::LaunchCommand StemSeparationService::resolveSpleeterLaunchCommand()
{
    LaunchCommand command;
    command.backend = QStringLiteral("Spleeter");
    command.program = executableFromSetting("Audio/StemSeparatorExecutable");
    if (!command.program.isEmpty()) return command;

    command.program = QStandardPaths::findExecutable(QStringLiteral("spleeter"));
    if (!command.program.isEmpty()) return command;

    command.program = executableFromSetting("Audio/StemSeparatorPython");
    if (command.program.isEmpty())
        command.program = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (!command.program.isEmpty()) {
        command.prefixArguments = {QStringLiteral("-m"), QStringLiteral("spleeter")};
        return command;
    }

    command.program = QStandardPaths::findExecutable(QStringLiteral("py"));
    if (!command.program.isEmpty())
        command.prefixArguments = {QStringLiteral("-3"), QStringLiteral("-m"),
                                   QStringLiteral("spleeter")};
    return command;
}

StemSeparationService::LaunchCommand StemSeparationService::resolveDemucsLaunchCommand()
{
    LaunchCommand command;
    command.backend = QStringLiteral("Demucs");
    command.program = executableFromSetting("Audio/DemucsExecutable");
    if (!command.program.isEmpty()) return command;

    command.program = QStandardPaths::findExecutable(QStringLiteral("demucs"));
    if (!command.program.isEmpty()) return command;

    command.program = executableFromSetting("Audio/DemucsPython");
    if (command.program.isEmpty())
        command.program = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (!command.program.isEmpty()) {
        command.prefixArguments = {QStringLiteral("-m"), QStringLiteral("demucs")};
        return command;
    }
    command.program = QStandardPaths::findExecutable(QStringLiteral("py"));
    if (!command.program.isEmpty())
        command.prefixArguments = {QStringLiteral("-3"), QStringLiteral("-m"),
                                   QStringLiteral("demucs")};
    return command;
}

bool StemSeparationService::resolveRuntime(QString* program,
                                           QStringList* prefixArguments,
                                           QString* description)
{
    const LaunchCommand command = resolveSpleeterLaunchCommand();
    if (program) *program = command.program;
    if (prefixArguments) *prefixArguments = command.prefixArguments;
    if (description) {
        *description = command.program.isEmpty() ? QString()
            : (command.prefixArguments.isEmpty()
                ? QFileInfo(command.program).fileName()
                : tr("%1 -m spleeter").arg(QFileInfo(command.program).fileName()));
    }
    return !command.program.isEmpty();
}

bool StemSeparationService::resolveDemucsRuntime(QString* program,
                                                 QStringList* prefixArguments,
                                                 QString* description)
{
    const LaunchCommand command = resolveDemucsLaunchCommand();
    if (program) *program = command.program;
    if (prefixArguments) *prefixArguments = command.prefixArguments;
    if (description) {
        *description = command.program.isEmpty() ? QString()
            : (command.prefixArguments.isEmpty()
                ? QFileInfo(command.program).fileName()
                : tr("%1 -m demucs").arg(QFileInfo(command.program).fileName()));
    }
    return !command.program.isEmpty();
}

StemModelCacheReport StemSeparationService::inspectModelCache(
    const QString& modelRoot, bool calculateChecksums,
    bool initializeChecksumBaseline, const StemModelProgress& progress)
{
    StemModelCacheReport report;
    const QFileInfo rootInfo(QDir::cleanPath(modelRoot.trimmed()));
    report.rootDirectory = rootInfo.absoluteFilePath();
    report.manifestPath = stemModelManifestPath(report.rootDirectory);
    if (!rootInfo.exists() || !rootInfo.isDir() || rootInfo.isSymLink() ||
        rootInfo.canonicalFilePath().isEmpty()) {
        report.message = tr("Model cache directory is missing or invalid.");
        return report;
    }

    struct PresetFiles {
        StemModelPresetStatus status;
        QVector<QFileInfo> files;
    };
    QVector<PresetFiles> discovered;
    discovered.reserve(3);
    qint64 totalHashBytes = 0;
    int totalFileCount = 0;
    for (int stemCount : {2, 4, 5}) {
        PresetFiles preset;
        preset.status.stemCount = stemCount;
        preset.status.presetName = QStringLiteral("%1stems").arg(stemCount);
        preset.status.directory = locatedPresetDirectory(
            report.rootDirectory, preset.status.presetName);
        bool hasIndex = false;
        bool hasData = false;
        bool hasSavedModel = false;
        bool hasSavedVariables = false;
        if (!preset.status.directory.isEmpty()) {
            QDirIterator iterator(preset.status.directory, QDir::Files | QDir::NoSymLinks,
                                  QDirIterator::Subdirectories);
            while (iterator.hasNext()) {
                const QFileInfo fileInfo(iterator.next());
                if (!fileInfo.isFile() || fileInfo.isSymLink() || fileInfo.size() <= 0) continue;
                if (++totalFileCount > kMaximumModelFiles ||
                    fileInfo.size() > kMaximumModelBytes - totalHashBytes) {
                    report.message = tr("Model cache exceeds the bounded verification limit.");
                    return report;
                }
                totalHashBytes += fileInfo.size();
                preset.status.totalBytes += fileInfo.size();
                ++preset.status.fileCount;
                preset.files.push_back(fileInfo);
                const QString relative = QDir(preset.status.directory).relativeFilePath(
                    fileInfo.absoluteFilePath()).replace('\\', '/').toLower();
                hasIndex = hasIndex || relative.endsWith(QStringLiteral(".index"));
                hasData = hasData || relative.contains(QStringLiteral(".data-"));
                hasSavedModel = hasSavedModel || relative == QStringLiteral("saved_model.pb");
                hasSavedVariables = hasSavedVariables ||
                    (relative.startsWith(QStringLiteral("variables/")) &&
                     (relative.endsWith(QStringLiteral(".index")) ||
                      relative.contains(QStringLiteral(".data-"))));
            }
        }
        preset.status.installed = preset.status.totalBytes >= 1024 * 1024 &&
            ((hasIndex && hasData) || (hasSavedModel && hasSavedVariables));
        if (preset.status.installed) ++report.installedPresetCount;
        discovered.push_back(std::move(preset));
    }

    if (!calculateChecksums) {
        for (const PresetFiles& preset : discovered) report.presets.push_back(preset.status);
        report.success = true;
        report.message = tr("%1 of 3 Spleeter presets detected.")
            .arg(report.installedPresetCount);
        return report;
    }
    if (report.installedPresetCount == 0) {
        for (const PresetFiles& preset : discovered) report.presets.push_back(preset.status);
        report.message = tr("No complete Spleeter model preset was found.");
        return report;
    }

    totalHashBytes = 0;
    for (const PresetFiles& preset : discovered)
        if (preset.status.installed) totalHashBytes += preset.status.totalBytes;

    qint64 completedBytes = 0;
    for (PresetFiles& preset : discovered) {
        if (!preset.status.installed) continue;
        std::sort(preset.files.begin(), preset.files.end(),
            [&preset](const QFileInfo& left, const QFileInfo& right) {
                return QDir(preset.status.directory).relativeFilePath(left.absoluteFilePath()) <
                    QDir(preset.status.directory).relativeFilePath(right.absoluteFilePath());
            });
        QCryptographicHash aggregate(QCryptographicHash::Sha256);
        for (const QFileInfo& fileInfo : preset.files) {
            const QString relative = QDir(preset.status.directory).relativeFilePath(
                fileInfo.absoluteFilePath()).replace('\\', '/');
            const QByteArray digest = hashModelFile(fileInfo.absoluteFilePath(),
                &completedBytes, totalHashBytes, progress, &report.cancelled);
            if (digest.isEmpty()) {
                report.message = report.cancelled
                    ? tr("Model verification cancelled.")
                    : tr("Could not hash model file: %1").arg(relative);
                return report;
            }
            aggregate.addData(relative.toUtf8());
            aggregate.addData("\0", 1);
            aggregate.addData(QByteArray::number(fileInfo.size()));
            aggregate.addData("\0", 1);
            aggregate.addData(digest);
        }
        preset.status.aggregateSha256 = QString::fromLatin1(
            aggregate.result().toHex());
    }
    report.checksumsCalculated = true;

    QJsonObject expectedPresets;
    const QFileInfo manifestInfo(report.manifestPath);
    report.checksumManifestFound = manifestInfo.exists();
    if (report.checksumManifestFound) {
        if (!manifestInfo.isFile() || manifestInfo.isSymLink() ||
            manifestInfo.size() <= 0 || manifestInfo.size() > 256 * 1024) {
            report.message = tr("The model checksum manifest is invalid.");
            return report;
        }
        QFile manifest(report.manifestPath);
        if (!manifest.open(QIODevice::ReadOnly)) {
            report.message = tr("Could not read the model checksum manifest.");
            return report;
        }
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(
            manifest.read(256 * 1024 + 1), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
            document.object().value(QStringLiteral("schema")).toString() !=
                QStringLiteral("veracore.spleeter-model-checksums.v1")) {
            report.message = tr("The model checksum manifest is malformed.");
            return report;
        }
        expectedPresets = document.object().value(QStringLiteral("presets")).toObject();
    } else if (initializeChecksumBaseline) {
        QJsonObject presets;
        for (const PresetFiles& preset : discovered) {
            if (!preset.status.installed) continue;
            QJsonObject value;
            value.insert(QStringLiteral("sha256"), preset.status.aggregateSha256);
            value.insert(QStringLiteral("bytes"), QString::number(preset.status.totalBytes));
            value.insert(QStringLiteral("files"), preset.status.fileCount);
            presets.insert(preset.status.presetName, value);
        }
        QJsonObject object;
        object.insert(QStringLiteral("schema"),
            QStringLiteral("veracore.spleeter-model-checksums.v1"));
        object.insert(QStringLiteral("createdUtc"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        object.insert(QStringLiteral("presets"), presets);
        QSaveFile manifest(report.manifestPath);
        if (!manifest.open(QIODevice::WriteOnly)) {
            report.message = tr("Could not create the model checksum baseline.");
            return report;
        }
        const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
        if (manifest.write(bytes) != bytes.size() || !manifest.commit()) {
            manifest.cancelWriting();
            report.message = tr("Could not commit the model checksum baseline.");
            return report;
        }
        expectedPresets = presets;
        report.checksumManifestFound = true;
        report.checksumBaselineCreated = true;
    }

    bool allMatch = report.checksumManifestFound &&
        expectedPresets.size() == report.installedPresetCount;
    for (PresetFiles& preset : discovered) {
        if (!preset.status.installed) continue;
        const QJsonObject expected = expectedPresets.value(
            preset.status.presetName).toObject();
        preset.status.checksumMatch = !expected.isEmpty() &&
            expected.value(QStringLiteral("sha256")).toString().compare(
                preset.status.aggregateSha256, Qt::CaseInsensitive) == 0 &&
            expected.value(QStringLiteral("bytes")).toString() ==
                QString::number(preset.status.totalBytes) &&
            expected.value(QStringLiteral("files")).toInt(-1) == preset.status.fileCount;
        allMatch = allMatch && preset.status.checksumMatch;
    }
    for (const PresetFiles& preset : discovered) report.presets.push_back(preset.status);
    report.allInstalledChecksumsMatch = allMatch;
    report.success = report.checksumManifestFound && allMatch;
    report.message = report.checksumBaselineCreated
        ? tr("Checksum baseline created for %1 preset(s).").arg(report.installedPresetCount)
        : (allMatch
            ? tr("Verified %1 Spleeter preset checksum(s).").arg(report.installedPresetCount)
            : (report.checksumManifestFound
                ? tr("Spleeter model checksum mismatch.")
                : tr("Checksum baseline has not been created.")));
    return report;
}

QVector<StemRecoveryJob> StemSeparationService::scanRecoverableJobs(
    const QStringList& roots, int maximumJobs)
{
    QVector<StemRecoveryJob> jobs;
    const int limit = qBound(1, maximumJobs, 1024);
    const QSet<QString> recoverableStates = {
        QStringLiteral("queued"), QStringLiteral("running"),
        QStringLiteral("cancelling"), QStringLiteral("validating"),
        QStringLiteral("completed")
    };
    QSet<QString> visitedRoots;
    for (const QString& requestedRoot : roots) {
        const QFileInfo rootInfo(requestedRoot);
        const QString root = rootInfo.canonicalFilePath();
        if (root.isEmpty() || !rootInfo.isDir() || visitedRoots.contains(root)) continue;
        visitedRoots.insert(root);
        QDirIterator iterator(root, QStringList{QStringLiteral("VeraCoreStemJob.json")},
                              QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext() && jobs.size() < limit) {
            const QFileInfo manifestInfo(iterator.next());
            if (manifestInfo.size() <= 0 || manifestInfo.size() > 256 * 1024) continue;
            QFile file(manifestInfo.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QByteArray bytes = file.read(256 * 1024 + 1);
            if (bytes.isEmpty() || bytes.size() > 256 * 1024) continue;
            QJsonParseError parseError{};
            const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) continue;
            const QJsonObject object = document.object();
            if (object.value(QStringLiteral("schema")).toString() !=
                    QStringLiteral("veracore.stem-job.v1")) continue;
            StemRecoveryJob job;
            job.state = object.value(QStringLiteral("state")).toString().trimmed().toLower();
            if (!recoverableStates.contains(job.state)) continue;
            job.jobId = object.value(QStringLiteral("jobId")).toString().trimmed().left(128);
            job.sourcePath = QFileInfo(object.value(
                QStringLiteral("sourcePath")).toString()).absoluteFilePath();
            if (job.jobId.isEmpty() || job.sourcePath.isEmpty()) continue;
            job.manifestPath = manifestInfo.absoluteFilePath();
            job.outputDirectory = manifestInfo.absolutePath();
            job.updatedUtc = object.value(QStringLiteral("updatedUtc")).toString();
            job.message = object.value(QStringLiteral("message")).toString().left(1024);
            job.inputWasRendered = object.value(
                QStringLiteral("inputWasRendered")).toBool(false);
            const QJsonArray expected = object.value(
                QStringLiteral("expectedStems")).toArray();
            for (const QJsonValue& value : expected) {
                const QString stem = value.toString().trimmed().toLower();
                if ((stem == QStringLiteral("vocals") ||
                     stem == QStringLiteral("accompaniment") ||
                     stem == QStringLiteral("drums") ||
                     stem == QStringLiteral("bass") ||
                     stem == QStringLiteral("piano") ||
                     stem == QStringLiteral("other")) &&
                    !job.expectedStems.contains(stem)) job.expectedStems.push_back(stem);
            }
            if (job.expectedStems.size() != 2 && job.expectedStems.size() != 4 &&
                job.expectedStems.size() != 5) continue;
            const QFileInfo sourceInfo(job.sourcePath);
            bool sizeValid = false;
            const qint64 recordedSize = object.value(
                QStringLiteral("sourceSizeBytes")).toString().toLongLong(&sizeValid);
            const QDateTime recordedModified = QDateTime::fromString(
                object.value(QStringLiteral("sourceModifiedUtc")).toString(),
                Qt::ISODateWithMs);
            const bool modifiedValid = !recordedModified.isValid() ||
                qAbs(sourceInfo.lastModified().toUTC().msecsTo(recordedModified)) <= 2000;
            job.sourceValid = sourceInfo.exists() && sourceInfo.isFile() &&
                (!sizeValid || sourceInfo.size() == recordedSize) && modifiedValid;
            if (job.state == QStringLiteral("completed")) {
                QHash<QString, QString> located;
                QHash<QString, AudioFileProbeResult> locatedProbes;
                QSet<QString> ambiguousStems;
                const bool hasRecordedOutputs = object.contains(QStringLiteral("outputs"));
                QHash<QString, QJsonObject> recordedOutputs;
                if (hasRecordedOutputs) {
                    const QJsonArray outputs = object.value(QStringLiteral("outputs")).toArray();
                    for (const QJsonValue& value : outputs) {
                        if (!value.isObject()) continue;
                        const QJsonObject identity = value.toObject();
                        const QString stem = identity.value(
                            QStringLiteral("stem")).toString().trimmed().toLower();
                        if (!job.expectedStems.contains(stem) || recordedOutputs.contains(stem)) {
                            recordedOutputs.clear();
                            break;
                        }
                        recordedOutputs.insert(stem, identity);
                    }
                }
                QString completedOutputRoot = job.outputDirectory;
                const QFileInfo backendOutputInfo(object.value(
                    QStringLiteral("backendOutputDirectory")).toString());
                const QString outputCanonical = QFileInfo(
                    job.outputDirectory).canonicalFilePath();
                const QString backendCanonical = backendOutputInfo.canonicalFilePath();
                if (backendOutputInfo.isDir() && !backendOutputInfo.isSymLink() &&
                    !outputCanonical.isEmpty() && !backendCanonical.isEmpty()) {
                    const QString relative = QDir(outputCanonical).relativeFilePath(
                        backendCanonical);
                    if (relative != QStringLiteral("..") &&
                        !relative.startsWith(QStringLiteral("../")) &&
                        !QDir::isAbsolutePath(relative))
                        completedOutputRoot = backendCanonical;
                }
                QDirIterator outputIterator(completedOutputRoot,
                    QStringList{QStringLiteral("*.wav")}, QDir::Files,
                    QDirIterator::Subdirectories);
                while (outputIterator.hasNext()) {
                    const QFileInfo outputInfo(outputIterator.next());
                    QString key = outputInfo.completeBaseName().toLower();
                    if (object.value(QStringLiteral("activeBackend")).toString() ==
                            QStringLiteral("Demucs") &&
                        key == QStringLiteral("no_vocals"))
                        key = QStringLiteral("accompaniment");
                    if (!job.expectedStems.contains(key, Qt::CaseInsensitive)) continue;
                    AudioFileProbeResult probe;
                    if (!probeValidatedStemWav(outputInfo.absoluteFilePath(),
                                               completedOutputRoot, &probe, nullptr)) continue;
                    if (located.contains(key)) {
                        ambiguousStems.insert(key);
                        continue;
                    }
                    located.insert(key, outputInfo.absoluteFilePath());
                    locatedProbes.insert(key, probe);
                }
                AudioFileProbeResult alignmentReference = AudioFileDecoder::probeAudio(
                    object.value(QStringLiteral("inputPath")).toString());
                for (const QString& stem : job.expectedStems) {
                    const QString path = located.value(stem.toLower());
                    const AudioFileProbeResult probe = locatedProbes.value(stem.toLower());
                    if (path.isEmpty() || ambiguousStems.contains(stem.toLower()) ||
                        (alignmentReference.success &&
                         !stemDurationMatches(alignmentReference, probe)) ||
                        (hasRecordedOutputs &&
                         (recordedOutputs.size() != job.expectedStems.size() ||
                          !recordedStemOutputMatches(recordedOutputs.value(stem.toLower()),
                              stem, path, job.outputDirectory, probe)))) {
                        job.outputPaths.clear();
                        break;
                    }
                    if (!alignmentReference.success) alignmentReference = probe;
                    job.outputPaths.push_back(path);
                }
                job.outputsComplete = job.outputPaths.size() == job.expectedStems.size();
            }
            jobs.push_back(std::move(job));
        }
        if (jobs.size() >= limit) break;
    }
    std::stable_sort(jobs.begin(), jobs.end(), [](const StemRecoveryJob& left,
                                                  const StemRecoveryJob& right) {
        return left.updatedUtc > right.updatedUtc;
    });
    return jobs;
}

bool StemSeparationService::markInterruptedJobRetried(
    const QString& manifestPath, const QString& replacementJobId, QString* errorText)
{
    const QFileInfo manifestInfo(manifestPath);
    if (manifestInfo.fileName() != QStringLiteral("VeraCoreStemJob.json") ||
        !manifestInfo.isFile() || manifestInfo.size() <= 0 ||
        manifestInfo.size() > 256 * 1024 || replacementJobId.trimmed().isEmpty()) {
        if (errorText) *errorText = tr("The interrupted stem-job manifest is invalid.");
        return false;
    }
    QFile source(manifestInfo.absoluteFilePath());
    if (!source.open(QIODevice::ReadOnly)) {
        if (errorText) *errorText = tr("Could not read the interrupted stem-job manifest.");
        return false;
    }
    QJsonParseError parseError{};
    QJsonDocument document = QJsonDocument::fromJson(source.read(256 * 1024 + 1),
                                                      &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorText) *errorText = tr("The interrupted stem-job manifest is malformed.");
        return false;
    }
    QJsonObject object = document.object();
    const QSet<QString> interruptedStates = {
        QStringLiteral("queued"), QStringLiteral("running"),
        QStringLiteral("cancelling"), QStringLiteral("validating"),
        QStringLiteral("completed")
    };
    const QString previousState = object.value(
        QStringLiteral("state")).toString().trimmed().toLower();
    if (object.value(QStringLiteral("schema")).toString() !=
            QStringLiteral("veracore.stem-job.v1") ||
        !interruptedStates.contains(previousState)) {
        if (errorText) *errorText = tr("The stem job is no longer recoverable.");
        return false;
    }
    object.insert(QStringLiteral("previousState"), previousState);
    object.insert(QStringLiteral("state"), QStringLiteral("retried"));
    object.insert(QStringLiteral("replacementJobId"), replacementJobId.left(128));
    object.insert(QStringLiteral("updatedUtc"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("message"), QStringLiteral("Retried by user."));
    QSaveFile destination(manifestInfo.absoluteFilePath());
    if (!destination.open(QIODevice::WriteOnly)) {
        if (errorText) *errorText = tr("Could not update the interrupted stem-job manifest.");
        return false;
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (destination.write(bytes) != bytes.size() || !destination.commit()) {
        destination.cancelWriting();
        if (errorText) *errorText = tr("Could not commit the stem-job retry state.");
        return false;
    }
    return true;
}

bool StemSeparationService::validateCompletedJobSource(
    const QString& manifestPath, const QString& expectedSourcePath, QString* errorText)
{
    const QFileInfo manifestInfo(manifestPath);
    if (manifestInfo.fileName() != QStringLiteral("VeraCoreStemJob.json") ||
        !manifestInfo.isFile() || manifestInfo.size() <= 0 ||
        manifestInfo.size() > 256 * 1024) {
        if (errorText) *errorText = tr("The completed stem-job manifest is invalid.");
        return false;
    }
    QFile source(manifestInfo.absoluteFilePath());
    if (!source.open(QIODevice::ReadOnly)) {
        if (errorText) *errorText = tr("Could not read the completed stem-job manifest.");
        return false;
    }
    const QByteArray bytes = source.read(256 * 1024 + 1);
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (bytes.isEmpty() || bytes.size() > 256 * 1024 ||
        parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorText) *errorText = tr("The completed stem-job manifest is malformed.");
        return false;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("schema")).toString() !=
            QStringLiteral("veracore.stem-job.v1") ||
        object.value(QStringLiteral("state")).toString() != QStringLiteral("completed")) {
        if (errorText) *errorText = tr("The stem job is not ready for import.");
        return false;
    }

    const QFileInfo recordedSource(object.value(QStringLiteral("sourcePath")).toString());
    const QFileInfo expectedSource(expectedSourcePath);
    const QString recordedCanonical = recordedSource.canonicalFilePath();
    const QString expectedCanonical = expectedSource.canonicalFilePath();
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
    bool sizeValid = false;
    const qint64 recordedSize = object.value(
        QStringLiteral("sourceSizeBytes")).toString().toLongLong(&sizeValid);
    const QDateTime recordedModified = QDateTime::fromString(
        object.value(QStringLiteral("sourceModifiedUtc")).toString(), Qt::ISODateWithMs);
    const bool samePath = !recordedCanonical.isEmpty() && !expectedCanonical.isEmpty() &&
        recordedCanonical.compare(expectedCanonical, pathCase) == 0;
    const bool sameModifiedTime = recordedModified.isValid() &&
        qAbs(expectedSource.lastModified().toUTC().msecsTo(recordedModified)) <= 2000;
    if (!samePath || !expectedSource.exists() || !expectedSource.isFile() ||
        !sizeValid || recordedSize <= 0 || expectedSource.size() != recordedSize ||
        !sameModifiedTime) {
        if (errorText) *errorText = tr(
            "The stem source file changed after separation started; stale outputs were not imported.");
        return false;
    }
    return true;
}

bool StemSeparationService::markCompletedJobImported(
    const QString& manifestPath, const QString& projectPath,
    const QString& sourceClipId, int importedTrackCount, QString* errorText)
{
    const QFileInfo manifestInfo(manifestPath);
    if (manifestInfo.fileName() != QStringLiteral("VeraCoreStemJob.json") ||
        !manifestInfo.isFile() || manifestInfo.size() <= 0 ||
        manifestInfo.size() > 256 * 1024 || sourceClipId.trimmed().isEmpty() ||
        (importedTrackCount != 2 && importedTrackCount != 4 && importedTrackCount != 5)) {
        if (errorText) *errorText = tr("The completed stem-job manifest is invalid.");
        return false;
    }
    QFile source(manifestInfo.absoluteFilePath());
    if (!source.open(QIODevice::ReadOnly)) {
        if (errorText) *errorText = tr("Could not read the completed stem-job manifest.");
        return false;
    }
    QJsonParseError parseError{};
    QJsonDocument document = QJsonDocument::fromJson(source.read(256 * 1024 + 1),
                                                      &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorText) *errorText = tr("The completed stem-job manifest is malformed.");
        return false;
    }
    QJsonObject object = document.object();
    if (object.value(QStringLiteral("schema")).toString() !=
            QStringLiteral("veracore.stem-job.v1") ||
        object.value(QStringLiteral("state")).toString() != QStringLiteral("completed")) {
        if (errorText) *errorText = tr("The stem job is not ready for import.");
        return false;
    }
    object.insert(QStringLiteral("previousState"), QStringLiteral("completed"));
    object.insert(QStringLiteral("state"), QStringLiteral("imported"));
    object.insert(QStringLiteral("projectPath"), projectPath.trimmed().isEmpty()
        ? QString() : QFileInfo(projectPath).absoluteFilePath());
    object.insert(QStringLiteral("sourceClipId"), sourceClipId.left(128));
    object.insert(QStringLiteral("importedTrackCount"), importedTrackCount);
    object.insert(QStringLiteral("updatedUtc"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("message"), QStringLiteral("Imported into project."));
    QSaveFile destination(manifestInfo.absoluteFilePath());
    if (!destination.open(QIODevice::WriteOnly)) {
        if (errorText) *errorText = tr("Could not update the completed stem-job manifest.");
        return false;
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (destination.write(bytes) != bytes.size() || !destination.commit()) {
        destination.cancelWriting();
        if (errorText) *errorText = tr("Could not commit the stem-job import state.");
        return false;
    }
    return true;
}

QStringList StemSeparationService::namesForStemCount(int stemCount)
{
    if (stemCount == 2) return {QStringLiteral("vocals"), QStringLiteral("accompaniment")};
    if (stemCount == 4) return {QStringLiteral("vocals"), QStringLiteral("drums"),
                               QStringLiteral("bass"), QStringLiteral("other")};
    if (stemCount == 5) return {QStringLiteral("vocals"), QStringLiteral("drums"),
                               QStringLiteral("bass"), QStringLiteral("piano"),
                               QStringLiteral("other")};
    return {};
}

bool StemSeparationService::startSeparation(const QString& inputPath,
                                             const QString& outputDirectory,
                                             int stemCount, QString* errorText,
                                             const QString& provenanceSourcePath,
                                             bool inputWasRendered)
{
    if (m_active) {
        if (errorText) *errorText = tr("Another stem-separation job is already running.");
        return false;
    }
    const QFileInfo input(inputPath);
    if (!input.exists() || !input.isFile() || input.size() <= 0) {
        if (errorText) *errorText = tr("The selected audio source is missing or empty.");
        return false;
    }
    const QFileInfo provenance(provenanceSourcePath.trimmed().isEmpty()
        ? input.absoluteFilePath() : provenanceSourcePath);
    if (!provenance.exists() || !provenance.isFile() || provenance.size() <= 0) {
        if (errorText) *errorText = tr("The original stem source is missing or empty.");
        return false;
    }
    const QStringList stems = namesForStemCount(stemCount);
    if (stems.isEmpty()) {
        if (errorText) *errorText = tr("Spleeter supports 2, 4, or 5 stems.");
        return false;
    }
    QSettings settings;
    QString requestedBackend = settings.value(
        "Audio/StemSeparatorBackend", QStringLiteral("Auto")).toString().trimmed();
    if (requestedBackend.compare(QStringLiteral("Spleeter"), Qt::CaseInsensitive) == 0)
        requestedBackend = QStringLiteral("Spleeter");
    else if (requestedBackend.compare(QStringLiteral("Demucs"), Qt::CaseInsensitive) == 0)
        requestedBackend = QStringLiteral("Demucs");
    else
        requestedBackend = QStringLiteral("Auto");
    if (requestedBackend == QStringLiteral("Demucs") && stemCount == 5) {
        if (errorText) *errorText = tr("Demucs fallback supports VeraCore 2- and 4-stem layouts; use Spleeter for 5 stems.");
        return false;
    }
    LaunchCommand command = requestedBackend == QStringLiteral("Demucs")
        ? resolveDemucsLaunchCommand() : resolveSpleeterLaunchCommand();
    if (command.program.isEmpty() && requestedBackend == QStringLiteral("Auto") &&
        stemCount != 5)
        command = resolveDemucsLaunchCommand();
    if (command.program.isEmpty()) {
        if (errorText) *errorText = requestedBackend == QStringLiteral("Demucs")
            ? tr("Demucs was not found. Configure Audio/DemucsExecutable or Audio/DemucsPython.")
            : tr("No compatible Spleeter/Demucs stem runtime was found.");
        return false;
    }
    QDir output;
    if (!output.mkpath(outputDirectory)) {
        if (errorText) *errorText = tr("Could not create the stem output directory.");
        return false;
    }
    const QString defaultModelDirectory = QDir(
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("Spleeter"));
    QString modelDirectory = QDir::cleanPath(settings.value(
        "Audio/StemSeparatorModelDirectory", defaultModelDirectory).toString().trimmed());
    if (modelDirectory.isEmpty() || modelDirectory == QStringLiteral("."))
        modelDirectory = defaultModelDirectory;
    if (!output.mkpath(modelDirectory)) {
        if (errorText) *errorText = tr("Could not create the Spleeter model-cache directory.");
        return false;
    }

    const QString compute = settings.value(
        "Audio/StemSeparatorCompute", QStringLiteral("Auto")).toString().trimmed();
    const int configuredThreads = qBound(0, settings.value(
        "Audio/StemSeparatorCpuThreads", 0).toInt(), 256);
    const int logicalThreads = qMax(1, QThread::idealThreadCount());
    const int automaticThreads = qMax(1, logicalThreads - 2);

    m_jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_inputPath = input.absoluteFilePath();
    m_provenanceSourcePath = provenance.absoluteFilePath();
    m_outputDirectory = QFileInfo(outputDirectory).absoluteFilePath();
    m_modelDirectory = QFileInfo(modelDirectory).absoluteFilePath();
    m_computeMode = compute.isEmpty() ? QStringLiteral("Auto") : compute;
    m_cpuThreadLimit = configuredThreads > 0
        ? configuredThreads : qMin(256, automaticThreads);
    m_requestedBackend = requestedBackend;
    m_stemNames = stems;
    m_processOutput.clear();
    m_cancelRequested = false;
    m_inputWasRendered = inputWasRendered;
    m_fallbackAttempted = false;
    m_active = true;
    if (!launchBackend(command, errorText)) {
        resetJob();
        return false;
    }
    return true;
}

bool StemSeparationService::launchBackend(const LaunchCommand& command,
                                          QString* errorText)
{
    if (!m_active || command.program.isEmpty() ||
        (command.backend != QStringLiteral("Spleeter") &&
         command.backend != QStringLiteral("Demucs"))) {
        if (errorText) *errorText = tr("The selected stem backend is invalid.");
        return false;
    }
    m_activeBackend = command.backend;
    m_runtimeProgram = QFileInfo(command.program).absoluteFilePath();
    m_backendOutputDirectory = QDir(m_outputDirectory).filePath(m_activeBackend);
    if (!QDir().mkpath(m_backendOutputDirectory)) {
        if (errorText) *errorText = tr("Could not create the %1 output directory.")
            .arg(m_activeBackend);
        return false;
    }

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (m_computeMode.compare(QStringLiteral("CPU"), Qt::CaseInsensitive) == 0) {
        environment.insert(QStringLiteral("CUDA_VISIBLE_DEVICES"), QStringLiteral("-1"));
    } else if (m_computeMode.startsWith(QStringLiteral("GPU:"), Qt::CaseInsensitive)) {
        bool validDevice = false;
        const int device = m_computeMode.mid(4).toInt(&validDevice);
        environment.insert(QStringLiteral("CUDA_VISIBLE_DEVICES"),
                           QString::number(validDevice ? qMax(0, device) : 0));
        environment.insert(QStringLiteral("TF_FORCE_GPU_ALLOW_GROWTH"),
                           QStringLiteral("true"));
    } else {
        environment.insert(QStringLiteral("TF_FORCE_GPU_ALLOW_GROWTH"),
                           QStringLiteral("true"));
    }
    environment.insert(QStringLiteral("TF_CPP_MIN_LOG_LEVEL"), QStringLiteral("1"));
    const QString cpuThreads = QString::number(qBound(1, m_cpuThreadLimit, 256));
    environment.insert(QStringLiteral("OMP_NUM_THREADS"), cpuThreads);
    environment.insert(QStringLiteral("MKL_NUM_THREADS"), cpuThreads);
    environment.insert(QStringLiteral("OPENBLAS_NUM_THREADS"), cpuThreads);
    environment.insert(QStringLiteral("NUMEXPR_NUM_THREADS"), cpuThreads);
    environment.insert(QStringLiteral("TF_NUM_INTRAOP_THREADS"), cpuThreads);
    environment.insert(QStringLiteral("TF_NUM_INTEROP_THREADS"),
                       QString::number(qMin(2, m_cpuThreadLimit)));
    environment.insert(QStringLiteral("MODEL_PATH"), m_modelDirectory);
    if (m_activeBackend == QStringLiteral("Demucs")) {
        const QString demucsCache = QDir(m_modelDirectory).filePath(
            QStringLiteral("Demucs"));
        if (!QDir().mkpath(demucsCache)) {
            if (errorText) *errorText = tr("Could not create the Demucs model cache.");
            return false;
        }
        environment.insert(QStringLiteral("TORCH_HOME"), demucsCache);
    }

    QStringList arguments = command.prefixArguments;
    if (m_activeBackend == QStringLiteral("Spleeter")) {
        arguments << QStringLiteral("separate")
                  << QStringLiteral("-p")
                  << QStringLiteral("spleeter:%1stems").arg(m_stemNames.size())
                  << QStringLiteral("-o") << m_backendOutputDirectory
                  << QStringLiteral("-c") << QStringLiteral("wav")
                  << m_inputPath;
    } else {
        arguments << QStringLiteral("--out") << m_backendOutputDirectory
                  << QStringLiteral("--name") << QStringLiteral("htdemucs");
        if (m_stemNames.size() == 2)
            arguments << QStringLiteral("--two-stems") << QStringLiteral("vocals");
        if (m_computeMode.compare(QStringLiteral("CPU"), Qt::CaseInsensitive) == 0) {
            arguments << QStringLiteral("--device") << QStringLiteral("cpu");
        } else if (m_computeMode.startsWith(QStringLiteral("GPU:"), Qt::CaseInsensitive)) {
            bool validDevice = false;
            const int device = m_computeMode.mid(4).toInt(&validDevice);
            arguments << QStringLiteral("--device")
                      << QStringLiteral("cuda:%1").arg(validDevice ? qMax(0, device) : 0);
        }
        arguments << m_inputPath;
    }
    m_process->setProcessEnvironment(environment);
    m_process->setWorkingDirectory(m_modelDirectory);
    writeJobManifest(m_fallbackAttempted
        ? QStringLiteral("fallback") : QStringLiteral("queued"), -1,
        m_fallbackAttempted ? tr("Retrying with Demucs fallback.") : QString());
    emit progressChanged(m_jobId, 0, m_fallbackAttempted
        ? tr("Spleeter failed; starting Demucs fallback")
        : tr("Queued %1-stem separation with %2")
            .arg(m_stemNames.size()).arg(m_activeBackend));
    m_processOutput.clear();
    m_process->start(command.program, arguments, QIODevice::ReadOnly);
    return true;
}

bool StemSeparationService::tryDemucsFallback(const QString& reason)
{
    if (!m_active || m_cancelRequested || m_fallbackAttempted ||
        m_requestedBackend != QStringLiteral("Auto") ||
        m_activeBackend != QStringLiteral("Spleeter") ||
        (m_stemNames.size() != 2 && m_stemNames.size() != 4)) return false;
    const LaunchCommand fallback = resolveDemucsLaunchCommand();
    if (fallback.program.isEmpty()) return false;
    m_fallbackAttempted = true;
    m_fallbackReason = reason.left(1024);
    m_processOutput = reason.toUtf8().right(4096);
    return launchBackend(fallback, nullptr);
}

void StemSeparationService::cancel()
{
    if (!m_active || m_cancelRequested) return;
    m_cancelRequested = true;
    const QString job = m_jobId;
    writeJobManifest(QStringLiteral("cancelling"), -1, tr("Cancellation requested."));
    emit progressChanged(job, 0, tr("Cancelling stem separation"));
    if (m_process->state() == QProcess::NotRunning) {
        writeJobManifest(QStringLiteral("cancelled"), -1, tr("Cancelled before launch."));
        resetJob();
        emit separationCancelled(job);
        return;
    }
    m_process->terminate();
    QTimer::singleShot(1500, this, [this, job]() {
        if (m_active && m_cancelRequested && m_jobId == job &&
            m_process->state() != QProcess::NotRunning)
            m_process->kill();
    });
}

void StemSeparationService::consumeProcessOutput()
{
    const QByteArray bytes = m_process->readAllStandardOutput();
    if (bytes.isEmpty()) return;
    m_processOutput += bytes;
    constexpr int kMaximumDiagnostics = 128 * 1024;
    if (m_processOutput.size() > kMaximumDiagnostics)
        m_processOutput.remove(0, m_processOutput.size() - kMaximumDiagnostics);
    const QString detail = conciseProcessLine(bytes);
    static const QRegularExpression percentPattern(QStringLiteral("(\\d{1,3})\\s*%"));
    const QRegularExpressionMatch match = percentPattern.match(QString::fromUtf8(bytes));
    const int percent = match.hasMatch() ? qBound(5, match.captured(1).toInt(), 94) : 15;
    if (m_active && !detail.isEmpty()) emit progressChanged(m_jobId, percent, detail);
}

QString StemSeparationService::conciseProcessLine(const QByteArray& bytes)
{
    const QStringList lines = QString::fromUtf8(bytes).split(QRegularExpression("[\\r\\n]+"),
                                                            Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = it->simplified();
        if (!line.isEmpty()) return line.left(320);
    }
    return {};
}

bool StemSeparationService::collectOutputs(QStringList* paths, QString* errorText) const
{
    if (!paths) return false;
    QHash<QString, QString> located;
    QHash<QString, AudioFileProbeResult> locatedProbes;
    QSet<QString> ambiguousStems;
    QDirIterator iterator(m_backendOutputDirectory,
                          QStringList{QStringLiteral("*.wav")},
                          QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info(path);
        QString key = info.completeBaseName().toLower();
        if (m_activeBackend == QStringLiteral("Demucs") &&
            key == QStringLiteral("no_vocals"))
            key = QStringLiteral("accompaniment");
        if (!m_stemNames.contains(key, Qt::CaseInsensitive)) continue;
        AudioFileProbeResult probe;
        QString validationError;
        if (!probeValidatedStemWav(info.absoluteFilePath(), m_backendOutputDirectory,
                                   &probe, &validationError)) {
            if (errorText) *errorText = tr("Invalid %1 stem: %2")
                .arg(key, validationError);
            return false;
        }
        if (located.contains(key)) {
            ambiguousStems.insert(key);
            continue;
        }
        located.insert(key, info.absoluteFilePath());
        locatedProbes.insert(key, probe);
    }
    paths->clear();
    paths->reserve(m_stemNames.size());
    AudioFileProbeResult alignmentReference = AudioFileDecoder::probeAudio(m_inputPath);
    for (const QString& stem : m_stemNames) {
        const QString key = stem.toLower();
        const QString path = located.value(key);
        const AudioFileProbeResult probe = locatedProbes.value(key);
        if (path.isEmpty() || ambiguousStems.contains(key)) {
            if (errorText) *errorText = ambiguousStems.contains(key)
                ? tr("%1 produced duplicate %2.wav stems.").arg(m_activeBackend, stem)
                : tr("%1 did not produce a valid %2.wav stem.").arg(m_activeBackend, stem);
            paths->clear();
            return false;
        }
        if (alignmentReference.success && !stemDurationMatches(alignmentReference, probe)) {
            if (errorText) *errorText = tr(
                "%1.wav is not timeline-aligned with the separation input (maximum %2 ms drift).")
                    .arg(stem).arg(qRound(kMaximumStemAlignmentErrorSeconds * 1000.0));
            paths->clear();
            return false;
        }
        if (!alignmentReference.success) alignmentReference = probe;
        paths->push_back(path);
    }
    return true;
}

bool StemSeparationService::writeJobManifest(
    const QString& state, int exitCode, const QString& message,
    const QStringList& validatedOutputPaths) const
{
    if (m_outputDirectory.isEmpty() || m_jobId.isEmpty()) return false;
    const QFileInfo source(m_provenanceSourcePath.isEmpty()
        ? m_inputPath : m_provenanceSourcePath);
    const QFileInfo input(m_inputPath);
    QJsonObject manifest;
    manifest.insert(QStringLiteral("schema"), QStringLiteral("veracore.stem-job.v1"));
    manifest.insert(QStringLiteral("jobId"), m_jobId);
    manifest.insert(QStringLiteral("state"), state);
    manifest.insert(QStringLiteral("updatedUtc"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    manifest.insert(QStringLiteral("sourcePath"), source.absoluteFilePath());
    manifest.insert(QStringLiteral("sourceSizeBytes"), QString::number(source.size()));
    manifest.insert(QStringLiteral("sourceModifiedUtc"),
                    source.lastModified().toUTC().toString(Qt::ISODateWithMs));
    manifest.insert(QStringLiteral("inputPath"), input.absoluteFilePath());
    manifest.insert(QStringLiteral("inputWasRendered"), m_inputWasRendered);
    manifest.insert(QStringLiteral("outputDirectory"), m_outputDirectory);
    manifest.insert(QStringLiteral("backendOutputDirectory"), m_backendOutputDirectory);
    manifest.insert(QStringLiteral("requestedBackend"), m_requestedBackend);
    manifest.insert(QStringLiteral("activeBackend"), m_activeBackend);
    manifest.insert(QStringLiteral("fallbackAttempted"), m_fallbackAttempted);
    if (!m_fallbackReason.isEmpty())
        manifest.insert(QStringLiteral("fallbackReason"), m_fallbackReason);
    manifest.insert(QStringLiteral("runtimeProgram"), m_runtimeProgram);
    manifest.insert(QStringLiteral("modelDirectory"), m_modelDirectory);
    manifest.insert(QStringLiteral("computeMode"), m_computeMode);
    manifest.insert(QStringLiteral("cpuThreadLimit"), m_cpuThreadLimit);
    QJsonArray stems;
    for (const QString& stem : m_stemNames) stems.push_back(stem);
    manifest.insert(QStringLiteral("expectedStems"), stems);
    if (!validatedOutputPaths.isEmpty()) {
        if (validatedOutputPaths.size() != m_stemNames.size()) return false;
        QJsonArray outputs;
        for (int index = 0; index < validatedOutputPaths.size(); ++index) {
            const QJsonObject identity = stemOutputIdentity(
                m_stemNames[index], validatedOutputPaths[index], m_outputDirectory);
            if (identity.isEmpty()) return false;
            outputs.push_back(identity);
        }
        manifest.insert(QStringLiteral("outputs"), outputs);
    }
    if (exitCode >= 0) manifest.insert(QStringLiteral("exitCode"), exitCode);
    if (!message.isEmpty()) manifest.insert(QStringLiteral("message"), message.left(1024));
    if (!m_processOutput.isEmpty()) {
        const QByteArray tail = m_processOutput.right(4096);
        manifest.insert(QStringLiteral("diagnosticTail"), QString::fromUtf8(tail));
    }

    QSaveFile file(QDir(m_outputDirectory).filePath(
        QStringLiteral("VeraCoreStemJob.json")));
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

void StemSeparationService::resetJob()
{
    m_active = false;
    m_cancelRequested = false;
    m_jobId.clear();
    m_inputPath.clear();
    m_provenanceSourcePath.clear();
    m_outputDirectory.clear();
    m_backendOutputDirectory.clear();
    m_runtimeProgram.clear();
    m_modelDirectory.clear();
    m_computeMode.clear();
    m_cpuThreadLimit = 1;
    m_requestedBackend.clear();
    m_activeBackend.clear();
    m_fallbackReason.clear();
    m_stemNames.clear();
    m_processOutput.clear();
    m_inputWasRendered = false;
    m_fallbackAttempted = false;
}
