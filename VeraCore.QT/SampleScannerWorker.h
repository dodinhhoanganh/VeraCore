#pragma once

#include <QThread>
#include <QStringList>

class SampleScannerWorker : public QThread
{
    Q_OBJECT

public:
    explicit SampleScannerWorker(const QStringList& searchPaths, QObject* parent = nullptr);

signals:
    void progressUpdated(int percent, const QString& currentFile);
    void scanFinished(const QStringList& samples);

protected:
    void run() override;

private:
    QStringList m_searchPaths;
};
