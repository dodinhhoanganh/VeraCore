#include "AudioMediaResolver.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStringList>

namespace {
QString normalizedExistingFile(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() && info.isFile() ? info.absoluteFilePath() : QString{};
}

bool samePath(const QString& left, const QString& right)
{
    const QString cleanLeft = QDir::cleanPath(QFileInfo(left).absoluteFilePath());
    const QString cleanRight = QDir::cleanPath(QFileInfo(right).absoluteFilePath());
#ifdef Q_OS_WIN
    return cleanLeft.compare(cleanRight, Qt::CaseInsensitive) == 0;
#else
    return cleanLeft == cleanRight;
#endif
}
}

bool AudioMediaResolver::isOnline(const QString& path)
{
    return !normalizedExistingFile(path.trimmed()).isEmpty();
}

AudioMediaResolution AudioMediaResolver::resolve(const QString& storedPath,
                                                 const QString& projectFilePath)
{
    AudioMediaResolution result;
    result.storedPath = storedPath.trimmed();
    if (result.storedPath.isEmpty()) return result;

    const QFileInfo projectInfo(projectFilePath);
    const QString projectDirectory = projectInfo.isAbsolute()
        ? projectInfo.absolutePath() : QString{};
    const QFileInfo storedInfo(result.storedPath);
    const QString fileName = storedInfo.fileName();
    const QString originalCandidate = storedInfo.isAbsolute()
        ? result.storedPath
        : (projectDirectory.isEmpty()
            ? result.storedPath
            : QDir(projectDirectory).filePath(result.storedPath));

    QStringList candidates;
    candidates.reserve(6);
    if (storedInfo.isAbsolute()) candidates.push_back(result.storedPath);
    else if (!projectDirectory.isEmpty())
        candidates.push_back(QDir(projectDirectory).filePath(result.storedPath));
    if (!projectDirectory.isEmpty() && !fileName.isEmpty()) {
        const QDir projectDir(projectDirectory);
        candidates.push_back(projectDir.filePath(fileName));
        candidates.push_back(projectDir.filePath(QStringLiteral("Audio/") + fileName));
        candidates.push_back(projectDir.filePath(QStringLiteral("Media/") + fileName));
        candidates.push_back(projectDir.filePath(QStringLiteral("Samples/") + fileName));
    }

    QSet<QString> visited;
    for (const QString& candidate : candidates) {
        const QString clean = QDir::cleanPath(candidate);
#ifdef Q_OS_WIN
        const QString key = clean.toCaseFolded();
#else
        const QString key = clean;
#endif
        if (visited.contains(key)) continue;
        visited.insert(key);
        const QString existing = normalizedExistingFile(clean);
        if (existing.isEmpty()) continue;
        result.resolvedPath = existing;
        result.online = true;
        result.relocated = !samePath(originalCandidate, existing);
        return result;
    }
    return result;
}
