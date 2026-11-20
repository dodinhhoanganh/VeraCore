#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

class SelectionService final : public QObject
{
    Q_OBJECT

public:
    enum class ClipKind {
        None,
        Midi,
        Audio
    };
    Q_ENUM(ClipKind)

    explicit SelectionService(QObject* parent = nullptr);

    quint64 revision() const noexcept { return m_revision; }
    const QVector<int>& trackIndices() const noexcept { return m_trackIndices; }
    int primaryTrackIndex() const noexcept { return m_primaryTrackIndex; }
    const QStringList& clipIds() const noexcept { return m_clipIds; }
    const QString& primaryClipId() const noexcept { return m_primaryClipId; }
    ClipKind primaryClipKind() const noexcept { return m_primaryClipKind; }
    bool hasTimeRange() const noexcept { return m_hasTimeRange; }
    double timeRangeStartBeat() const noexcept { return m_timeRangeStartBeat; }
    double timeRangeEndBeat() const noexcept { return m_timeRangeEndBeat; }

    void setTrackSelection(QVector<int> trackIndices, int primaryTrackIndex);
    void setClipSelection(QStringList clipIds, const QString& primaryClipId,
                          ClipKind primaryClipKind);
    void setTimeRange(bool enabled, double startBeat, double endBeat);
    void clearClips();
    void clearAll();

signals:
    void tracksChanged();
    void clipsChanged();
    void timeRangeChanged();
    void selectionChanged(quint64 revision);

private:
    void publish(bool tracks, bool clips, bool timeRange);

    QVector<int> m_trackIndices;
    int m_primaryTrackIndex = -1;
    QStringList m_clipIds;
    QString m_primaryClipId;
    ClipKind m_primaryClipKind = ClipKind::None;
    bool m_hasTimeRange = false;
    double m_timeRangeStartBeat = 0.0;
    double m_timeRangeEndBeat = 0.0;
    quint64 m_revision = 0;
};
