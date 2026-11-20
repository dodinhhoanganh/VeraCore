#include "SampleScannerWorker.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>
#include <algorithm>

SampleScannerWorker::SampleScannerWorker(const QStringList& searchPaths, QObject* parent)
    : QThread(parent)
    , m_searchPaths(searchPaths)
{
}

void SampleScannerWorker::run()
{
    QStringList samples;
    QStringList files;
    const QSet<QString> sampleExtensions = {
        "wav", "wave", "aif", "aiff", "aifc", "flac", "mp3", "ogg", "oga", "wv", "wavpack",
        "mid", "midi", "rex", "rx2"
    };

    for (const QString& searchPath : m_searchPaths) {
        QDir dir(searchPath);
        if (!dir.exists()) {
            continue;
        }

        QDirIterator it(dir.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo info = it.fileInfo();
            if (sampleExtensions.contains(info.suffix().toLower())) {
                files.push_back(info.absoluteFilePath());
            }
        }
    }

    if (files.isEmpty()) {
        emit progressUpdated(100, "No samples found");
        emit scanFinished(samples);
        return;
    }

    files.removeDuplicates();
    std::sort(files.begin(), files.end(), [](const QString& a, const QString& b) {
        return QString::localeAwareCompare(a, b) < 0;
    });

    const int total = files.size();
    for (int i = 0; i < total; ++i) {
        samples.push_back(files[i]);
        emit progressUpdated((i * 100) / total, QFileInfo(files[i]).fileName());
    }

    emit progressUpdated(100, "Done");
    emit scanFinished(samples);
}
