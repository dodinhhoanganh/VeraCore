#include "SelectionService.h"

#include <algorithm>
#include <cmath>

SelectionService::SelectionService(QObject* parent)
    : QObject(parent)
{
}

void SelectionService::setTrackSelection(QVector<int> trackIndices, int primaryTrackIndex)
{
    trackIndices.erase(std::remove_if(trackIndices.begin(), trackIndices.end(),
        [](int index) { return index < 0; }), trackIndices.end());
    std::sort(trackIndices.begin(), trackIndices.end());
    trackIndices.erase(std::unique(trackIndices.begin(), trackIndices.end()), trackIndices.end());
    if (!trackIndices.contains(primaryTrackIndex))
        primaryTrackIndex = trackIndices.isEmpty() ? -1 : trackIndices.front();
    if (m_trackIndices == trackIndices && m_primaryTrackIndex == primaryTrackIndex) return;
    m_trackIndices = std::move(trackIndices);
    m_primaryTrackIndex = primaryTrackIndex;
    publish(true, false, false);
}

void SelectionService::setClipSelection(QStringList clipIds, const QString& primaryClipId,
                                        ClipKind primaryClipKind)
{
    for (QString& id : clipIds) id = id.trimmed();
    clipIds.removeAll(QString());
    clipIds.removeDuplicates();
    QString primary = primaryClipId.trimmed();
    if (!clipIds.contains(primary)) primary.clear();
    if (primary.isEmpty() && !clipIds.isEmpty()) primary = clipIds.front();
    if (primary.isEmpty()) primaryClipKind = ClipKind::None;
    if (m_clipIds == clipIds && m_primaryClipId == primary &&
        m_primaryClipKind == primaryClipKind) return;
    m_clipIds = std::move(clipIds);
    m_primaryClipId = std::move(primary);
    m_primaryClipKind = primaryClipKind;
    publish(false, true, false);
}

void SelectionService::setTimeRange(bool enabled, double startBeat, double endBeat)
{
    const bool finite = std::isfinite(startBeat) && std::isfinite(endBeat);
    if (finite && endBeat < startBeat) std::swap(startBeat, endBeat);
    enabled = enabled && finite && startBeat >= 0.0 && endBeat > startBeat;
    if (!enabled) {
        startBeat = 0.0;
        endBeat = 0.0;
    }
    if (m_hasTimeRange == enabled && m_timeRangeStartBeat == startBeat &&
        m_timeRangeEndBeat == endBeat) return;
    m_hasTimeRange = enabled;
    m_timeRangeStartBeat = startBeat;
    m_timeRangeEndBeat = endBeat;
    publish(false, false, true);
}

void SelectionService::clearClips()
{
    setClipSelection({}, {}, ClipKind::None);
}

void SelectionService::clearAll()
{
    const bool tracks = !m_trackIndices.isEmpty() || m_primaryTrackIndex >= 0;
    const bool clips = !m_clipIds.isEmpty() || !m_primaryClipId.isEmpty();
    const bool timeRange = m_hasTimeRange;
    if (!tracks && !clips && !timeRange) return;
    m_trackIndices.clear();
    m_primaryTrackIndex = -1;
    m_clipIds.clear();
    m_primaryClipId.clear();
    m_primaryClipKind = ClipKind::None;
    m_hasTimeRange = false;
    m_timeRangeStartBeat = 0.0;
    m_timeRangeEndBeat = 0.0;
    publish(tracks, clips, timeRange);
}

void SelectionService::publish(bool tracks, bool clips, bool timeRange)
{
    ++m_revision;
    if (tracks) emit tracksChanged();
    if (clips) emit clipsChanged();
    if (timeRange) emit timeRangeChanged();
    emit selectionChanged(m_revision);
}
