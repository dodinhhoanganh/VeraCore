#include "PlaylistView.h"
#include "AudioMediaResolver.h"
#include <QPainter>
#include <QPainterPath>
#include <QAction>
#include <QColorDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineF>
#include <QMenu>
#include <QInputDialog>
#include <QScrollBar>
#include <QSettings>
#include <QUuid>
#include <QStyleOptionGraphicsItem>
#include <qmath.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <limits>
#include <numeric>
#include <utility>

namespace {
constexpr size_t kMaximumTransientMarkers = 4096u;
constexpr size_t kMaximumWarpMarkers = 4096u;
constexpr size_t kMaximumGainEnvelopePoints = 256u;

QColor normalizedClipColor(const QColor& color)
{
    if (!color.isValid()) return {};
    QColor result = color.toRgb();
    result.setAlpha(255);
    return result;
}

QColor contrastingClipText(const QColor& background, const QColor& fallback)
{
    if (!background.isValid()) return fallback;
    const double luminance = 0.2126 * background.redF() +
        0.7152 * background.greenF() + 0.0722 * background.blueF();
    return luminance > 0.56 ? QColor("#171717") : QColor("#f7f7f7");
}

void drawClipLockBadge(QPainter* painter, const QRectF& clipRect, bool offsetForUnique)
{
    if (!painter || clipRect.width() < (offsetForUnique ? 42.0 : 24.0)) return;
    const double right = clipRect.right() - (offsetForUnique ? 38.0 : 20.0);
    const QRectF body(right, clipRect.top() + 9.0, 14.0, 10.0);
    painter->setPen(QPen(QColor("#171717"), 1.25));
    painter->setBrush(QColor("#ffd38a"));
    painter->drawRoundedRect(body, 2.0, 2.0);
    painter->setBrush(Qt::NoBrush);
    painter->drawArc(QRectF(body.left() + 3.0, body.top() - 6.0, 8.0, 9.0), 0, 180 * 16);
}

void drawClipGroupIndicator(QPainter* painter, const QRectF& clipRect,
                            const QString& groupId)
{
    if (!painter || groupId.isEmpty() || clipRect.width() < 10.0) return;
    const QColor color = QColor::fromHsv(
        static_cast<int>(qHash(groupId) % 360u), 155, 238);
    painter->setPen(QPen(color, 2.5, Qt::SolidLine, Qt::RoundCap));
    painter->setBrush(Qt::NoBrush);
    painter->drawLine(QPointF(clipRect.left() + 4.0, clipRect.bottom() - 2.5),
                      QPointF(clipRect.right() - 4.0, clipRect.bottom() - 2.5));
}

void drawClipLoopGuides(QPainter* painter, const QRectF& clipRect,
                        double loopLengthBeats, double firstBoundaryBeats,
                        double pixelsPerBeat)
{
    if (!painter || !std::isfinite(loopLengthBeats) || loopLengthBeats < 0.25 ||
        !std::isfinite(firstBoundaryBeats) || firstBoundaryBeats <= 0.0 ||
        !std::isfinite(pixelsPerBeat) || pixelsPerBeat <= 0.0) return;
    const double spacing = loopLengthBeats * pixelsPerBeat;
    const double first = firstBoundaryBeats * pixelsPerBeat;
    if (spacing < 3.0 || first >= clipRect.width() - 1.0) return;
    painter->setPen(QPen(QColor(255, 255, 255, 82), 1.0, Qt::DashLine));
    const int maximumGuides = qMin(4096,
        static_cast<int>(std::ceil(clipRect.width() / spacing)));
    for (int guide = 0; guide <= maximumGuides; ++guide) {
        const double x = clipRect.left() + first + spacing * guide;
        if (x >= clipRect.right() - 1.0) break;
        painter->drawLine(QPointF(x, clipRect.top() + 2.0),
                          QPointF(x, clipRect.bottom() - 2.0));
    }
}

void drawTakeLaneBadge(QPainter* painter, const QRectF& clipRect, int lane, bool active)
{
    if (!painter || clipRect.width() < 18.0 || clipRect.height() < 3.0) return;
    const QString label = QString("T%1%2").arg(lane + 1).arg(active ? " C" : "");
    const double width = active ? 34.0 : 20.0;
    const QRectF badge(clipRect.right() - width - 3.0, clipRect.top() + 1.0,
                       width, qMin(13.0, clipRect.height() - 2.0));
    painter->setPen(Qt::NoPen);
    painter->setBrush(active ? QColor(42, 196, 154, 220) : QColor(20, 24, 24, 175));
    painter->drawRoundedRect(badge, 2.0, 2.0);
    painter->setPen(active ? QColor("#071b15") : QColor("#d4ddd9"));
    painter->drawText(badge, Qt::AlignCenter, label);
}

double shapedAudioFade(double progress, double curve)
{
    constexpr double halfPi = 1.57079632679489661923;
    const double base = std::sin(halfPi * qBound(0.0, progress, 1.0));
    const double safeCurve = qBound(-1.0, std::isfinite(curve) ? curve : 0.0, 1.0);
    if (safeCurve < 0.0)
        return base + (std::sqrt(base) - base) * -safeCurve;
    return base + (base * base - base) * safeCurve;
}

std::vector<double> normalizedTransientSeconds(const std::vector<double>& source,
                                               double durationSeconds)
{
    const double maximum = std::isfinite(durationSeconds) && durationSeconds > 0.0
        ? durationSeconds + 0.001 : 24.0 * 60.0 * 60.0;
    std::vector<double> result;
    result.reserve(std::min(source.size(), kMaximumTransientMarkers));
    for (double marker : source) {
        if (!std::isfinite(marker) || marker < 0.0 || marker > maximum) continue;
        result.push_back(marker);
        if (result.size() >= kMaximumTransientMarkers) break;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end(), [](double left, double right) {
        return std::fabs(left - right) < 0.0005;
    }), result.end());
    return result;
}

std::vector<AudioWarpMarker> normalizedWarpMarkers(
    const std::vector<AudioWarpMarker>& source, double durationSeconds,
    double warpLengthBeats)
{
    std::vector<AudioWarpMarker> candidates;
    if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0 ||
        !std::isfinite(warpLengthBeats) || warpLengthBeats <= 0.0) return candidates;
    candidates.reserve(std::min(source.size(), kMaximumWarpMarkers));
    for (const AudioWarpMarker& marker : source) {
        if (!std::isfinite(marker.sourceSeconds) || !std::isfinite(marker.targetBeat) ||
            marker.sourceSeconds <= 0.0 || marker.sourceSeconds >= durationSeconds ||
            marker.targetBeat <= 0.0 || marker.targetBeat >= warpLengthBeats) continue;
        candidates.push_back(marker);
        if (candidates.size() >= kMaximumWarpMarkers) break;
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const AudioWarpMarker& left, const AudioWarpMarker& right) {
            return left.sourceSeconds < right.sourceSeconds;
        });
    std::vector<AudioWarpMarker> result;
    result.reserve(candidates.size());
    for (const AudioWarpMarker& marker : candidates) {
        if (!result.empty() &&
            (marker.sourceSeconds - result.back().sourceSeconds < 0.0005 ||
             marker.targetBeat - result.back().targetBeat < 1.0 / 960.0)) continue;
        result.push_back(marker);
    }
    return result;
}

std::vector<AudioGainEnvelopePoint> normalizedGainEnvelope(
    const std::vector<AudioGainEnvelopePoint>& source, double lengthBeats)
{
    std::vector<AudioGainEnvelopePoint> result;
    if (!std::isfinite(lengthBeats) || lengthBeats <= 0.0) return result;
    result.reserve(std::min(source.size(), kMaximumGainEnvelopePoints));
    for (const AudioGainEnvelopePoint& point : source) {
        if (!std::isfinite(point.beat) || !std::isfinite(point.gain) ||
            point.beat < 0.0 || point.beat > lengthBeats) continue;
        result.push_back({point.beat, qBound(0.0f, point.gain, 4.0f)});
        if (result.size() >= kMaximumGainEnvelopePoints) break;
    }
    std::stable_sort(result.begin(), result.end(),
        [](const AudioGainEnvelopePoint& left, const AudioGainEnvelopePoint& right) {
            return left.beat < right.beat;
        });
    std::vector<AudioGainEnvelopePoint> unique;
    unique.reserve(result.size());
    for (const AudioGainEnvelopePoint& point : result) {
        if (!unique.empty() && std::fabs(point.beat - unique.back().beat) < 1.0 / 960.0)
            unique.back() = point;
        else
            unique.push_back(point);
    }
    return unique;
}

double warpedBeatForSourceSeconds(double sourceSeconds, double durationSeconds,
                                  double warpLengthBeats,
                                  const std::vector<AudioWarpMarker>& markers)
{
    if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0 ||
        !std::isfinite(warpLengthBeats) || warpLengthBeats <= 0.0) return 0.0;
    const double source = qBound(0.0, sourceSeconds, durationSeconds);
    AudioWarpMarker left{0.0, 0.0};
    for (const AudioWarpMarker& right : markers) {
        if (source <= right.sourceSeconds) {
            const double span = right.sourceSeconds - left.sourceSeconds;
            const double amount = span > 0.0 ? (source - left.sourceSeconds) / span : 0.0;
            return left.targetBeat + (right.targetBeat - left.targetBeat) * amount;
        }
        left = right;
    }
    const double span = durationSeconds - left.sourceSeconds;
    const double amount = span > 0.0 ? (source - left.sourceSeconds) / span : 0.0;
    return left.targetBeat + (warpLengthBeats - left.targetBeat) * amount;
}
}

PlaylistView::PlaylistView(QWidget* parent)
    : QGraphicsView(parent),
      m_pixelsPerBeat(40.0),
      m_snapBeats(0.25),
      m_beatsPerBar(4.0),
      m_signatureNumerator(4),
      m_signatureDenominator(4),
      m_trackHeight(60), // Match Inspector/Track Header height
      m_timelineHeight(30),
      m_trackCount(0),
      m_clipboardOriginBeat(0.0),
      m_clipboardSpanBeats(0.0),
      m_clipboardPrimaryIsAudio(false),
      m_arrangementTransactionDepth(0),
      m_arrangementTransactionChanged(false),
      m_currentArrangementStateId(1),
      m_cleanArrangementStateId(1),
      m_nextArrangementStateId(2),
      m_suppressUndoCapture(false),
      m_selectedClipIndex(-1),
      m_selectedClipIsAudio(false),
      m_playheadBeat(0.0),
      m_loopEnabled(false),
      m_loopDragging(false),
      m_loopStartBeat(0.0),
      m_loopEndBeat(4.0),
      m_loopDragAnchorBeat(0.0),
      m_timeSelectionEnabled(false),
      m_timeSelectionDragging(false),
      m_timeSelectionStartBeat(0.0),
      m_timeSelectionEndBeat(0.0),
      m_timeSelectionAnchorBeat(0.0),
      m_punchEnabled(false),
      m_punchInBeat(0.0),
      m_punchOutBeat(4.0),
      m_pendingClipDrag(false),
      m_marqueeSelecting(false),
      m_panning(false),
      m_draggingClipCreate(false),
      m_editingExistingClip(false),
      m_existingClipChanged(false),
      m_resizingExistingClip(false),
      m_resizingExistingClipFromLeft(false),
      m_autoCrossfadeEnabled(false),
      m_audioFadeDrag(AudioFadeDrag::None),
      m_audioWarpDragMarker(-1),
      m_duplicateOnDrag(false),
      m_clipGrabOffsetBeat(0.0),
      m_dragTrackIndex(-1),
      m_dragStartBeat(0.0),
      m_dragCurrentBeat(0.0)
{
    m_autoCrossfadeEnabled = QSettings().value(
        QStringLiteral("Editing/AutoCrossfades"), false).toBool();
    m_timeSignatureMap.push_back({0.0, 4, 4});
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);

    setAlignment(Qt::AlignLeft | Qt::AlignTop);

    // Disable default scrollbars, we will manage them if needed, a DAW workspace
    setRenderHint(QPainter::Antialiasing, false); // For crispy grid lines
    setRenderHint(QPainter::TextAntialiasing, true);
    setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);

    // Set darker background color
    setBackgroundBrush(QColor("#1b1b1b"));
}

PlaylistView::~PlaylistView()
{
}

void PlaylistView::setPlayheadBeat(double beat)
{
    const double nextBeat = std::isfinite(beat) ? qMax(0.0, beat) : 0.0;
    if (qFuzzyCompare(1.0 + m_playheadBeat, 1.0 + nextBeat)) return;

    auto invalidatePlayhead = [this](double playheadBeat) {
        constexpr int halfWidth = 3;
        const int x = qRound(playheadBeat * m_pixelsPerBeat - horizontalScrollBar()->value());
        if (x >= -halfWidth && x <= viewport()->width() + halfWidth) {
            viewport()->update(x - halfWidth, 0, halfWidth * 2 + 1, viewport()->height());
        }
    };
    invalidatePlayhead(m_playheadBeat);
    m_playheadBeat = nextBeat;
    invalidatePlayhead(m_playheadBeat);
}

void PlaylistView::setSnapBeats(double beats)
{
    m_snapBeats = std::isfinite(beats) ? qBound(0.0, beats, 64.0) : 0.25;
}

void PlaylistView::setLoopRange(bool enabled, double startBeat, double endBeat)
{
    const bool valid = enabled && std::isfinite(startBeat) && std::isfinite(endBeat) &&
        startBeat >= 0.0 && endBeat - startBeat >= 0.25;
    m_loopEnabled = valid;
    m_loopStartBeat = valid ? startBeat : 0.0;
    m_loopEndBeat = valid ? endBeat : qMax(0.25, m_beatsPerBar);
    viewport()->update();
}

void PlaylistView::requestLoopRangeChange(bool enabled, double startBeat, double endBeat)
{
    const bool valid = enabled && std::isfinite(startBeat) && std::isfinite(endBeat) &&
        startBeat >= 0.0 && endBeat - startBeat >= 0.25;
    const double safeStart = valid ? startBeat : 0.0;
    const double safeEnd = valid ? endBeat : 4.0;
    setLoopRange(valid, safeStart, safeEnd);
    emit loopRangeChanged(valid, safeStart, safeEnd);
}

void PlaylistView::setTimeSelection(bool enabled, double startBeat, double endBeat)
{
    const bool valid = enabled && std::isfinite(startBeat) && std::isfinite(endBeat) &&
        startBeat >= 0.0 && endBeat - startBeat >= 0.25;
    m_timeSelectionEnabled = valid;
    m_timeSelectionStartBeat = valid ? startBeat : 0.0;
    m_timeSelectionEndBeat = valid ? endBeat : 0.0;
    if (!valid) m_timeSelectionDragging = false;
    viewport()->update();
}

bool PlaylistView::timeSelection(double* startBeat, double* endBeat) const
{
    if (startBeat) *startBeat = m_timeSelectionStartBeat;
    if (endBeat) *endBeat = m_timeSelectionEndBeat;
    return m_timeSelectionEnabled;
}

void PlaylistView::setPunchRange(bool enabled, double startBeat, double endBeat)
{
    const bool valid = enabled && std::isfinite(startBeat) && std::isfinite(endBeat) &&
        startBeat >= 0.0 && endBeat - startBeat >= 0.25;
    m_punchEnabled = valid;
    m_punchInBeat = valid ? startBeat : 0.0;
    m_punchOutBeat = valid ? endBeat : 4.0;
    viewport()->update();
}

void PlaylistView::setTimeSignature(int numerator, int denominator)
{
    const int safeNumerator = qBound(1, numerator, 32);
    const int boundedDenominator = qBound(1, denominator, 64);
    const int safeDenominator = (boundedDenominator & (boundedDenominator - 1)) == 0
        ? boundedDenominator : 4;
    setTimeSignatureMap({{0.0, safeNumerator, safeDenominator}});
}

void PlaylistView::setTimeSignatureMap(const std::vector<MidiTimeSignaturePoint>& points)
{
    constexpr size_t kMaxSignaturePoints = 4096;
    std::vector<MidiTimeSignaturePoint> normalized;
    normalized.reserve(std::min(points.size(), kMaxSignaturePoints));
    for (const MidiTimeSignaturePoint& point : points) {
        const bool validDenominator = point.denominator >= 1 && point.denominator <= 64 &&
            (point.denominator & (point.denominator - 1)) == 0;
        if (!std::isfinite(point.beat) || point.beat < 0.0 ||
            point.numerator < 1 || point.numerator > 32 || !validDenominator) continue;
        if (normalized.size() >= kMaxSignaturePoints) break;
        normalized.push_back(point);
    }
    std::stable_sort(normalized.begin(), normalized.end(),
        [](const MidiTimeSignaturePoint& left, const MidiTimeSignaturePoint& right) {
            return left.beat < right.beat;
        });
    std::vector<MidiTimeSignaturePoint> collapsed;
    collapsed.reserve(normalized.size());
    for (const MidiTimeSignaturePoint& point : normalized) {
        if (!collapsed.empty() && collapsed.back().beat == point.beat) collapsed.back() = point;
        else collapsed.push_back(point);
    }
    if (collapsed.empty()) collapsed.push_back({0.0, m_signatureNumerator, m_signatureDenominator});
    else if (collapsed.front().beat > 0.0) {
        if (collapsed.size() == kMaxSignaturePoints) collapsed.pop_back();
        collapsed.insert(collapsed.begin(), {0.0, m_signatureNumerator, m_signatureDenominator});
    }
    m_timeSignatureMap = std::move(collapsed);
    m_signatureNumerator = m_timeSignatureMap.front().numerator;
    m_signatureDenominator = m_timeSignatureMap.front().denominator;
    m_beatsPerBar = qBound(0.25,
        static_cast<double>(m_signatureNumerator) *
            (4.0 / static_cast<double>(m_signatureDenominator)),
        64.0);
    for (AudioClip& clip : m_audioClips) {
        const MidiTimeSignaturePoint signature = timeSignatureAtBeat(clip.startBeat);
        clip.beatsPerBar = qBound(0.25,
            static_cast<double>(signature.numerator) *
                (4.0 / static_cast<double>(signature.denominator)),
            64.0);
        if (clip.syncMode == 2 && clip.targetBars > 0.0) {
            scaleAudioClipWarp(clip, clip.targetBars * clip.beatsPerBar);
        }
    }
    updateSceneBounds();
    viewport()->update();
    if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 && m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
        const AudioClip& clip = m_audioClips[m_selectedClipIndex];
        emit audioClipSelected(clip.trackIndex, clip.name, clip.startBeat, clip.lengthBeats, clip.sourcePath,
                               clip.sourceTempoBpm, clip.pitchShiftSemitones, clip.syncMode, clip.targetBars);
    }
}

MidiTimeSignaturePoint PlaylistView::timeSignatureAtBeat(double beat) const
{
    if (m_timeSignatureMap.empty()) return {0.0, m_signatureNumerator, m_signatureDenominator};
    const auto next = std::upper_bound(m_timeSignatureMap.begin(), m_timeSignatureMap.end(),
        qMax(0.0, beat),
        [](double targetBeat, const MidiTimeSignaturePoint& point) {
            return targetBeat < point.beat;
        });
    return next == m_timeSignatureMap.begin() ? m_timeSignatureMap.front() : *(next - 1);
}

std::vector<PlaylistView::BarMarker> PlaylistView::barMarkers(
    double startBeat, double endBeat) const
{
    std::vector<BarMarker> result;
    if (m_timeSignatureMap.empty() || !std::isfinite(startBeat) ||
        !std::isfinite(endBeat) || endBeat < 0.0 || endBeat < startBeat) return result;
    const double visibleStart = qMax(0.0, startBeat);
    const double visibleEnd = qMax(visibleStart, endBeat);
    result.reserve(static_cast<size_t>(qBound(4.0,
        (visibleEnd - visibleStart) / 0.25 + 4.0, 4096.0)));
    qint64 baseBar = 1;
    for (size_t index = 0; index < m_timeSignatureMap.size(); ++index) {
        const MidiTimeSignaturePoint& point = m_timeSignatureMap[index];
        if (point.beat > visibleEnd + 1.0e-9) break;
        const double segmentStart = point.beat;
        const double segmentEnd = index + 1u < m_timeSignatureMap.size()
            ? m_timeSignatureMap[index + 1u].beat
            : std::numeric_limits<double>::infinity();
        const double beatsPerBar = qBound(0.25,
            static_cast<double>(point.numerator) *
                (4.0 / static_cast<double>(point.denominator)), 64.0);
        if (segmentEnd >= visibleStart && segmentStart <= visibleEnd) {
            const qint64 firstLocalBar = qMax<qint64>(0,
                static_cast<qint64>(std::floor((visibleStart - segmentStart) / beatsPerBar)) - 1);
            for (qint64 localBar = firstLocalBar; ; ++localBar) {
                const double barBeat = segmentStart + static_cast<double>(localBar) * beatsPerBar;
                if (barBeat > visibleEnd + 1.0e-9 || barBeat >= segmentEnd - 1.0e-9) break;
                if (barBeat >= visibleStart - 1.0e-9) {
                    const qint64 number = qMin<qint64>(
                        std::numeric_limits<int>::max(), baseBar + localBar);
                    result.push_back({barBeat, static_cast<int>(number),
                        point.numerator, point.denominator,
                        index > 0u && localBar == 0});
                    if (result.size() >= 8192u) return result;
                }
            }
        }
        if (std::isfinite(segmentEnd)) {
            const double span = qMax(0.0, segmentEnd - segmentStart);
            const qint64 segmentBars = qMax<qint64>(1,
                static_cast<qint64>(std::ceil(span / beatsPerBar - 1.0e-9)));
            baseBar = qMin<qint64>(std::numeric_limits<int>::max(), baseBar + segmentBars);
        }
    }
    return result;
}

namespace {
QString stableClipId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
}

int PlaylistView::addMidiClip(int trackIndex, double startBeat, double lengthBeats, const QString& name,
                              const QString& clipId, const QString& contentId, double sourceOffsetBeats,
                              bool muted)
{
    if (!isMidiTrack(trackIndex)) {
        return -1;
    }
    pushUndoState();
    const int clipIndex = createMidiClip(trackIndex, startBeat, lengthBeats, name, clipId, contentId,
                                         sourceOffsetBeats, muted);
    selectClip(clipIndex, false);
    return clipIndex;
}

int PlaylistView::addAudioClip(int trackIndex, double startBeat, double lengthBeats, const QString& name, const QString& sourcePath,
                               double sourceTempoBpm, double pitchShiftSemitones, double beatsPerBar, double sourceDurationSeconds,
                               const QString& clipId, const QString& contentId,
                               double sourceOffsetBeats, double warpLengthBeats, double tempoConfidence,
                               int sourceSignatureNumerator, int sourceSignatureDenominator, bool tempoDetected,
                               const std::vector<double>& transientSeconds,
                               const std::vector<AudioWarpMarker>& warpMarkers,
                               const std::vector<AudioGainEnvelopePoint>& gainEnvelope,
                               bool muted)
{
    if (!isAudioTrack(trackIndex)) {
        return -1;
    }
    pushUndoState();
    const int clipIndex = createAudioClip(trackIndex, startBeat, lengthBeats, name, sourcePath,
                                           sourceTempoBpm, pitchShiftSemitones, beatsPerBar, sourceDurationSeconds,
                                           clipId, contentId, sourceOffsetBeats, warpLengthBeats, tempoConfidence,
                                           sourceSignatureNumerator, sourceSignatureDenominator, tempoDetected,
                                           transientSeconds, warpMarkers, gainEnvelope, muted);
    selectAudioClip(clipIndex);
    return clipIndex;
}

std::vector<PlaylistView::MidiClipInfo> PlaylistView::getMidiClips() const
{
    std::vector<MidiClipInfo> clips;
    clips.reserve(m_midiClips.size());
    for (const MidiClip& clip : m_midiClips) {
        if (!isMidiTrack(clip.trackIndex)) {
            continue;
        }
        clips.push_back({clip.clipId, clip.contentId, clip.trackIndex, clip.name,
                         clip.startBeat, clip.lengthBeats, clip.sourceOffsetBeats,
                         clip.loopEnabled, clip.loopStartBeats, clip.loopLengthBeats, clip.isUnique,
                         clip.muted, clip.clipColor, clip.groupId, clip.locked,
                         clip.takeGroupId, clip.takeLane, clip.takeActive});
    }
    return clips;
}

std::vector<PlaylistView::AudioClipInfo> PlaylistView::getAudioClips() const
{
    std::vector<AudioClipInfo> clips;
    clips.reserve(m_audioClips.size());
    for (const AudioClip& clip : m_audioClips) {
        if (!isAudioTrack(clip.trackIndex)) {
            continue;
        }
        clips.push_back({clip.clipId,
                         clip.contentId,
                         clip.trackIndex,
                         clip.name,
                         clip.startBeat,
                         clip.lengthBeats,
                         clip.sourceOffsetBeats,
                         clip.loopEnabled,
                         clip.loopStartBeats,
                         clip.loopLengthBeats,
                         clip.warpLengthBeats,
                         clip.sourceTempoBpm,
                         clip.tempoConfidence,
                         clip.sourceSignatureNumerator,
                         clip.sourceSignatureDenominator,
                         clip.tempoDetected,
                         clip.transientSeconds,
                         clip.warpMarkers,
                         clip.pitchShiftSemitones,
                         clip.syncMode,
                         clip.targetBars,
                         clip.beatsPerBar,
                         clip.sourceDurationSeconds,
                         clip.gain,
                         clip.pan,
                         clip.normalize,
                         clip.reverse,
                         clip.reversePolarity,
                         clip.fadeInBeats,
                         clip.fadeOutBeats,
                         clip.isUnique,
                         clip.sourcePath,
                         clip.mediaOffline,
                         clip.gainEnvelope,
                         clip.fadeInCurve,
                         clip.fadeOutCurve,
                         clip.muted,
                         clip.clipColor,
                         clip.groupId,
                         clip.locked,
                         clip.takeGroupId,
                         clip.takeLane,
                         clip.takeActive});
    }
    return clips;
}

void PlaylistView::advanceMidiClipSourceOffset(MidiClip& clip, double deltaBeats)
{
    const double next = qMax(0.0, clip.sourceOffsetBeats +
        (std::isfinite(deltaBeats) ? deltaBeats : 0.0));
    if (!clip.loopEnabled || !std::isfinite(clip.loopLengthBeats) ||
        clip.loopLengthBeats < 0.25) {
        clip.sourceOffsetBeats = next;
        return;
    }
    clip.loopStartBeats = std::isfinite(clip.loopStartBeats)
        ? qMax(0.0, clip.loopStartBeats) : 0.0;
    double phase = std::fmod(next - clip.loopStartBeats, clip.loopLengthBeats);
    if (phase < 0.0) phase += clip.loopLengthBeats;
    clip.sourceOffsetBeats = clip.loopStartBeats + phase;
}

void PlaylistView::advanceAudioClipSourceOffset(AudioClip& clip, double deltaBeats)
{
    const double warpLength = std::isfinite(clip.warpLengthBeats)
        ? qMax(0.25, clip.warpLengthBeats) : 0.25;
    const double next = qBound(0.0, clip.sourceOffsetBeats +
        (std::isfinite(deltaBeats) ? deltaBeats : 0.0), warpLength);
    if (!clip.loopEnabled || !std::isfinite(clip.loopLengthBeats) ||
        clip.loopLengthBeats < 0.25) {
        clip.sourceOffsetBeats = next;
        return;
    }
    clip.loopStartBeats = qBound(0.0,
        std::isfinite(clip.loopStartBeats) ? clip.loopStartBeats : 0.0,
        qMax(0.0, warpLength - 0.25));
    clip.loopLengthBeats = qBound(0.25, clip.loopLengthBeats,
        qMax(0.25, warpLength - clip.loopStartBeats));
    double phase = std::fmod(next - clip.loopStartBeats, clip.loopLengthBeats);
    if (phase < 0.0) phase += clip.loopLengthBeats;
    clip.sourceOffsetBeats = clip.loopStartBeats + phase;
}

void PlaylistView::scaleAudioClipWarp(AudioClip& clip, double warpLengthBeats)
{
    const double oldWarp = std::isfinite(clip.warpLengthBeats) && clip.warpLengthBeats > 0.0
        ? qMax(0.25, clip.warpLengthBeats)
        : qMax(0.25, clip.sourceOffsetBeats + clip.lengthBeats);
    const double newWarp = std::isfinite(warpLengthBeats) && warpLengthBeats > 0.0
        ? qMax(0.25, warpLengthBeats)
        : oldWarp;
    const double startRatio = qBound(0.0, clip.sourceOffsetBeats / oldWarp, 1.0);
    const double endRatio = qBound(startRatio,
        (clip.sourceOffsetBeats + clip.lengthBeats) / oldWarp, 1.0);
    const double oldLength = clip.lengthBeats;
    clip.warpLengthBeats = newWarp;
    const double markerScale = oldWarp > 0.0 ? newWarp / oldWarp : 1.0;
    for (AudioWarpMarker& marker : clip.warpMarkers) marker.targetBeat *= markerScale;
    clip.sourceOffsetBeats = qMin(startRatio * newWarp, qMax(0.0, newWarp - 0.25));
    clip.lengthBeats = qMin(newWarp - clip.sourceOffsetBeats,
        qMax(0.25, (endRatio - startRatio) * newWarp));
    if (oldLength > 0.0 && clip.lengthBeats > 0.0) {
        const double envelopeScale = clip.lengthBeats / oldLength;
        for (AudioGainEnvelopePoint& point : clip.gainEnvelope)
            point.beat *= envelopeScale;
    }
    clip.gainEnvelope = normalizedGainEnvelope(clip.gainEnvelope, clip.lengthBeats);
    if (clip.loopEnabled) {
        clip.loopStartBeats = qBound(0.0, clip.loopStartBeats,
            qMax(0.0, newWarp - 0.25));
        clip.loopLengthBeats = qBound(0.25, clip.loopLengthBeats,
            qMax(0.25, newWarp - clip.loopStartBeats));
        advanceAudioClipSourceOffset(clip, 0.0);
    }
    clampAudioClipFades(clip);
}

void PlaylistView::clampAudioClipFades(AudioClip& clip)
{
    const double length = std::isfinite(clip.lengthBeats) ? qMax(0.0, clip.lengthBeats) : 0.0;
    clip.fadeInBeats = std::isfinite(clip.fadeInBeats) ? qBound(0.0, clip.fadeInBeats, length) : 0.0;
    clip.fadeOutBeats = std::isfinite(clip.fadeOutBeats) ? qBound(0.0, clip.fadeOutBeats, length) : 0.0;
    const double total = clip.fadeInBeats + clip.fadeOutBeats;
    if (total > length && total > 0.0) {
        const double scale = length / total;
        clip.fadeInBeats *= scale;
        clip.fadeOutBeats *= scale;
    }
}

void PlaylistView::updateSelectedAudioClipSync(double sourceTempoBpm, double pitchShiftSemitones,
                                               double lengthBeats, double tempoConfidence,
                                               int sourceSignatureNumerator,
                                               int sourceSignatureDenominator,
                                               bool tempoDetected)
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 || m_selectedClipIndex >= (int)m_audioClips.size() ||
        m_audioClips[m_selectedClipIndex].locked) {
        return;
    }
    pushUndoState();
    AudioClip& selected = m_audioClips[m_selectedClipIndex];
    for (AudioClip& clip : m_audioClips) {
        if (clip.contentId != selected.contentId) continue;
        clip.sourceTempoBpm = qBound(0.0, sourceTempoBpm, 300.0);
        clip.tempoConfidence = tempoDetected && std::isfinite(tempoConfidence)
            ? qBound(0.0, tempoConfidence, 1.0) : 0.0;
        clip.sourceSignatureNumerator = qBound(0, sourceSignatureNumerator, 32);
        const bool validDenominator = sourceSignatureDenominator >= 1 &&
            sourceSignatureDenominator <= 64 &&
            (sourceSignatureDenominator & (sourceSignatureDenominator - 1)) == 0;
        clip.sourceSignatureDenominator = validDenominator ? sourceSignatureDenominator : 0;
        clip.tempoDetected = tempoDetected && clip.sourceTempoBpm > 0.0;
        clip.pitchShiftSemitones = qBound(-48.0, pitchShiftSemitones, 48.0);
    }
    if (std::isfinite(lengthBeats) && lengthBeats > 0.0) {
        const double safeLength = qMax(0.25, lengthBeats);
        for (AudioClip& clip : m_audioClips) {
            if (clip.contentId == selected.contentId && (clip.syncMode == 0 || clip.syncMode == 1)) {
                scaleAudioClipWarp(clip, safeLength);
            }
        }
    }
    const AudioClip& clip = m_audioClips[m_selectedClipIndex];
    updateSceneBounds();
    emit audioClipSelected(clip.trackIndex, clip.name, clip.startBeat, clip.lengthBeats, clip.sourcePath,
                           clip.sourceTempoBpm, clip.pitchShiftSemitones, clip.syncMode, clip.targetBars);
    viewport()->update();
    emit contentChanged();
}

void PlaylistView::updateSelectedAudioClipSyncMode(int syncMode, double targetBars)
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 || m_selectedClipIndex >= (int)m_audioClips.size() ||
        m_audioClips[m_selectedClipIndex].locked) {
        return;
    }
    pushUndoState();
    AudioClip& selected = m_audioClips[m_selectedClipIndex];
    for (AudioClip& clip : m_audioClips) {
        if (clip.contentId != selected.contentId) continue;
        clip.syncMode = qBound(0, syncMode, 3);
        clip.targetBars = qBound(0.0, targetBars, 512.0);
        if (clip.syncMode == 2 && clip.targetBars > 0.0) {
            scaleAudioClipWarp(clip, clip.targetBars * clip.beatsPerBar);
        } else {
            clip.warpLengthBeats = qMax(clip.warpLengthBeats,
                                        clip.sourceOffsetBeats + clip.lengthBeats);
        }
    }
    const AudioClip& clip = m_audioClips[m_selectedClipIndex];
    emit audioClipSelected(clip.trackIndex, clip.name, clip.startBeat, clip.lengthBeats, clip.sourcePath,
                           clip.sourceTempoBpm, clip.pitchShiftSemitones, clip.syncMode, clip.targetBars);
    viewport()->update();
    emit contentChanged();
}

void PlaylistView::updateSelectedAudioClipTransients(const std::vector<double>& transientSeconds)
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size()) ||
        m_audioClips[m_selectedClipIndex].locked) return;
    AudioClip& selected = m_audioClips[m_selectedClipIndex];
    const std::vector<double> normalized = normalizedTransientSeconds(
        transientSeconds, selected.sourceDurationSeconds);
    if (selected.transientSeconds == normalized) return;
    pushUndoState();
    for (AudioClip& clip : m_audioClips) {
        if (clip.contentId == selected.contentId) clip.transientSeconds = normalized;
    }
    viewport()->update();
    emit contentChanged();
}

bool PlaylistView::quantizeSelectedAudioClipTransients(double gridBeats)
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size()) ||
        m_audioClips[m_selectedClipIndex].locked) return false;
    AudioClip& selected = m_audioClips[m_selectedClipIndex];
    if (selected.transientSeconds.empty() || selected.sourceDurationSeconds <= 0.0 ||
        selected.warpLengthBeats <= 0.0 || !std::isfinite(gridBeats) || gridBeats <= 0.0)
        return false;
    const double grid = qBound(1.0 / 960.0, gridBeats, 64.0);
    std::vector<AudioWarpMarker> markers;
    markers.reserve(std::min(selected.transientSeconds.size(), kMaximumWarpMarkers));
    double previousTarget = 0.0;
    for (double sourceSeconds : selected.transientSeconds) {
        if (sourceSeconds <= 0.0 || sourceSeconds >= selected.sourceDurationSeconds) continue;
        const double naturalBeat = sourceSeconds / selected.sourceDurationSeconds *
            selected.warpLengthBeats;
        const double targetBeat = std::round(naturalBeat / grid) * grid;
        if (targetBeat <= previousTarget + 1.0 / 960.0 ||
            targetBeat >= selected.warpLengthBeats) continue;
        markers.push_back({sourceSeconds, targetBeat});
        previousTarget = targetBeat;
        if (markers.size() >= kMaximumWarpMarkers) break;
    }
    markers = normalizedWarpMarkers(markers, selected.sourceDurationSeconds,
                                    selected.warpLengthBeats);
    if (markers == selected.warpMarkers) return false;
    pushUndoState();
    for (AudioClip& clip : m_audioClips)
        if (clip.contentId == selected.contentId) clip.warpMarkers = markers;
    selectAudioClip(m_selectedClipIndex);
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::updateSelectedAudioClipWarpMarker(int markerIndex, double targetBeat,
                                                     bool captureUndo)
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size()) ||
        m_audioClips[m_selectedClipIndex].locked ||
        !std::isfinite(targetBeat)) return false;
    const AudioClip& selected = m_audioClips[m_selectedClipIndex];
    if (markerIndex < 0 || markerIndex >= static_cast<int>(selected.warpMarkers.size()))
        return false;
    double maximumWarp = selected.warpLengthBeats;
    for (const AudioClip& clip : m_audioClips)
        if (clip.contentId == selected.contentId)
            maximumWarp = qMin(maximumWarp, clip.warpLengthBeats);
    constexpr double minimumSpacing = 1.0 / 960.0;
    const double minimumTarget = markerIndex > 0
        ? selected.warpMarkers[static_cast<size_t>(markerIndex - 1)].targetBeat + minimumSpacing
        : minimumSpacing;
    const double maximumTarget = markerIndex + 1 < static_cast<int>(selected.warpMarkers.size())
        ? selected.warpMarkers[static_cast<size_t>(markerIndex + 1)].targetBeat - minimumSpacing
        : maximumWarp - minimumSpacing;
    if (maximumTarget < minimumTarget) return false;
    const double safeTarget = qBound(minimumTarget, targetBeat, maximumTarget);
    if (qFuzzyCompare(1.0 + selected.warpMarkers[static_cast<size_t>(markerIndex)].targetBeat,
                      1.0 + safeTarget)) return false;
    if (captureUndo) pushUndoState();
    for (AudioClip& clip : m_audioClips) {
        if (clip.contentId == selected.contentId &&
            markerIndex < static_cast<int>(clip.warpMarkers.size()))
            clip.warpMarkers[static_cast<size_t>(markerIndex)].targetBeat = safeTarget;
    }
    viewport()->update();
    if (captureUndo) {
        selectAudioClip(m_selectedClipIndex);
        emit contentChanged();
    }
    return true;
}

bool PlaylistView::clearSelectedAudioClipWarpMarkers()
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size()) ||
        m_audioClips[m_selectedClipIndex].locked ||
        m_audioClips[m_selectedClipIndex].warpMarkers.empty()) return false;
    pushUndoState();
    const QString contentId = m_audioClips[m_selectedClipIndex].contentId;
    for (AudioClip& clip : m_audioClips)
        if (clip.contentId == contentId) clip.warpMarkers.clear();
    selectAudioClip(m_selectedClipIndex);
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::toggleSelectedAudioClipWarpMarker(double sourceSeconds,
                                                     double targetBeat)
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size()) ||
        m_audioClips[m_selectedClipIndex].locked ||
        !std::isfinite(sourceSeconds) || !std::isfinite(targetBeat)) return false;
    const AudioClip& selected = m_audioClips[m_selectedClipIndex];
    if (selected.sourceDurationSeconds <= 0.0 || selected.warpLengthBeats <= 0.0 ||
        sourceSeconds <= 0.0 || sourceSeconds >= selected.sourceDurationSeconds) return false;
    std::vector<AudioWarpMarker> markers = selected.warpMarkers;
    constexpr double sourceToleranceSeconds = 0.001;
    const auto existing = std::find_if(markers.begin(), markers.end(),
        [sourceSeconds](const AudioWarpMarker& marker) {
            return std::abs(marker.sourceSeconds - sourceSeconds) <= sourceToleranceSeconds;
        });
    if (existing != markers.end()) {
        markers.erase(existing);
    } else {
        const auto insertion = std::lower_bound(markers.begin(), markers.end(), sourceSeconds,
            [](const AudioWarpMarker& marker, double source) {
                return marker.sourceSeconds < source;
            });
        constexpr double minimumSpacing = 1.0 / 960.0;
        const double minimumTarget = insertion == markers.begin()
            ? minimumSpacing : std::prev(insertion)->targetBeat + minimumSpacing;
        const double maximumTarget = insertion == markers.end()
            ? selected.warpLengthBeats - minimumSpacing
            : insertion->targetBeat - minimumSpacing;
        if (maximumTarget < minimumTarget) return false;
        markers.insert(insertion,
            {sourceSeconds, qBound(minimumTarget, targetBeat, maximumTarget)});
    }
    markers = normalizedWarpMarkers(markers, selected.sourceDurationSeconds,
                                    selected.warpLengthBeats);
    if (markers == selected.warpMarkers) return false;
    pushUndoState();
    const QString contentId = selected.contentId;
    for (AudioClip& clip : m_audioClips)
        if (clip.contentId == contentId) clip.warpMarkers = markers;
    selectAudioClip(m_selectedClipIndex);
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::removeSelectedAudioClipWarpMarker(int markerIndex)
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size()) ||
        m_audioClips[m_selectedClipIndex].locked) return false;
    const AudioClip& selected = m_audioClips[m_selectedClipIndex];
    if (markerIndex < 0 ||
        markerIndex >= static_cast<int>(selected.warpMarkers.size())) return false;
    pushUndoState();
    const QString contentId = selected.contentId;
    for (AudioClip& clip : m_audioClips) {
        if (clip.contentId != contentId ||
            markerIndex >= static_cast<int>(clip.warpMarkers.size())) continue;
        clip.warpMarkers.erase(clip.warpMarkers.begin() + markerIndex);
    }
    selectAudioClip(m_selectedClipIndex);
    viewport()->update();
    emit contentChanged();
    return true;
}

void PlaylistView::updateSelectedAudioClipProcessing(float gain, float pan, bool normalize,
                                                     bool reverse, bool reversePolarity)
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size()) ||
        m_audioClips[m_selectedClipIndex].locked) return;
    pushUndoState();
    const QString contentId = m_audioClips[m_selectedClipIndex].contentId;
    const float safeGain = std::isfinite(gain) ? qBound(0.0f, gain, 4.0f) : 1.0f;
    const float safePan = std::isfinite(pan) ? qBound(-1.0f, pan, 1.0f) : 0.0f;
    for (AudioClip& clip : m_audioClips) {
        if (clip.contentId != contentId) continue;
        clip.gain = safeGain;
        clip.pan = safePan;
        clip.normalize = normalize;
        clip.reverse = reverse;
        clip.reversePolarity = reversePolarity;
    }
    const AudioClip& clip = m_audioClips[m_selectedClipIndex];
    emit audioClipSelected(clip.trackIndex, clip.name, clip.startBeat, clip.lengthBeats,
                           clip.sourcePath, clip.sourceTempoBpm, clip.pitchShiftSemitones,
                           clip.syncMode, clip.targetBars);
    viewport()->update();
    emit contentChanged();
}

void PlaylistView::updateSelectedAudioClipFades(double fadeInBeats, double fadeOutBeats)
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size()) ||
        m_audioClips[m_selectedClipIndex].locked) return;
    pushUndoState();
    AudioClip& clip = m_audioClips[m_selectedClipIndex];
    clip.fadeInBeats = fadeInBeats;
    clip.fadeOutBeats = fadeOutBeats;
    clampAudioClipFades(clip);
    selectAudioClip(m_selectedClipIndex);
    viewport()->update();
    emit contentChanged();
}

bool PlaylistView::updateSelectedAudioClipFadeCurves(double fadeInCurve, double fadeOutCurve)
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size()) ||
        m_audioClips[m_selectedClipIndex].locked) return false;
    AudioClip& clip = m_audioClips[m_selectedClipIndex];
    const double safeIn = qBound(-1.0, std::isfinite(fadeInCurve) ? fadeInCurve : 0.0, 1.0);
    const double safeOut = qBound(-1.0, std::isfinite(fadeOutCurve) ? fadeOutCurve : 0.0, 1.0);
    if (qFuzzyCompare(clip.fadeInCurve + 2.0, safeIn + 2.0) &&
        qFuzzyCompare(clip.fadeOutCurve + 2.0, safeOut + 2.0)) return false;
    pushUndoState();
    clip.fadeInCurve = safeIn;
    clip.fadeOutCurve = safeOut;
    selectAudioClip(m_selectedClipIndex);
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::updateSelectedAudioClipGainEnvelope(
    const std::vector<AudioGainEnvelopePoint>& points)
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size()) ||
        m_audioClips[m_selectedClipIndex].locked) return false;
    AudioClip& clip = m_audioClips[m_selectedClipIndex];
    const std::vector<AudioGainEnvelopePoint> normalized =
        normalizedGainEnvelope(points, clip.lengthBeats);
    if (clip.gainEnvelope == normalized) return false;
    pushUndoState();
    clip.gainEnvelope = normalized;
    selectAudioClip(m_selectedClipIndex);
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::updateSelectedAudioClipSourceRange(double sourceInBeats, double sourceOutBeats)
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size()) ||
        m_audioClips[m_selectedClipIndex].locked) return false;

    AudioClip& clip = m_audioClips[m_selectedClipIndex];
    const double currentIn = clip.loopEnabled ? clip.loopStartBeats : clip.sourceOffsetBeats;
    const double currentEnd = clip.loopEnabled
        ? clip.loopStartBeats + clip.loopLengthBeats
        : clip.sourceOffsetBeats + clip.lengthBeats;
    const double fullSourceBeats = std::isfinite(clip.warpLengthBeats) && clip.warpLengthBeats > 0.0
        ? qMax(0.25, clip.warpLengthBeats)
        : qMax(0.25, currentEnd);
    const double requestedIn = std::isfinite(sourceInBeats) ? sourceInBeats : currentIn;
    const double safeIn = qBound(0.0, requestedIn, qMax(0.0, fullSourceBeats - 0.25));
    const double requestedOut = std::isfinite(sourceOutBeats) ? sourceOutBeats : currentEnd;
    const double safeOut = qBound(safeIn + 0.25, requestedOut, fullSourceBeats);
    if (qFuzzyCompare(currentIn + 1.0, safeIn + 1.0) &&
        qFuzzyCompare(currentEnd + 1.0, safeOut + 1.0)) return false;

    pushUndoState();
    clip.sourceOffsetBeats = safeIn;
    if (clip.loopEnabled) {
        clip.loopStartBeats = safeIn;
        clip.loopLengthBeats = safeOut - safeIn;
        advanceAudioClipSourceOffset(clip, 0.0);
    } else {
        clip.lengthBeats = safeOut - safeIn;
        clip.gainEnvelope = normalizedGainEnvelope(clip.gainEnvelope, clip.lengthBeats);
        clampAudioClipFades(clip);
    }
    updateSceneBounds();
    selectAudioClip(m_selectedClipIndex);
    viewport()->update();
    emit contentChanged();
    return true;
}

int PlaylistView::refreshAudioMediaStatus()
{
    std::map<QString, bool> offlineByContent;
    for (AudioClip& clip : m_audioClips) {
        const QString key = clip.contentId.isEmpty() ? clip.sourcePath : clip.contentId;
        const auto found = offlineByContent.find(key);
        if (found == offlineByContent.end()) {
            const bool offline = !AudioMediaResolver::isOnline(clip.sourcePath);
            offlineByContent.emplace(key, offline);
            clip.mediaOffline = offline;
        } else {
            clip.mediaOffline = found->second;
        }
    }
    if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
        const AudioClip& clip = m_audioClips[m_selectedClipIndex];
        emit audioClipSelected(clip.trackIndex, clip.name, clip.startBeat, clip.lengthBeats,
                               clip.sourcePath, clip.sourceTempoBpm, clip.pitchShiftSemitones,
                               clip.syncMode, clip.targetBars);
    }
    viewport()->update();
    return static_cast<int>(std::count_if(offlineByContent.begin(), offlineByContent.end(),
        [](const auto& entry) { return entry.second; }));
}

int PlaylistView::relinkSelectedAudioContent(const QString& sourcePath, double sourceDurationSeconds)
{
    const QString safePath = QFileInfo(sourcePath.trimmed()).absoluteFilePath();
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size()) ||
        m_audioClips[m_selectedClipIndex].locked ||
        !AudioMediaResolver::isOnline(safePath)) return 0;

    pushUndoState();
    const QString contentId = m_audioClips[m_selectedClipIndex].contentId;
    const double safeDuration = std::isfinite(sourceDurationSeconds) && sourceDurationSeconds > 0.0
        ? sourceDurationSeconds : 0.0;
    int updated = 0;
    for (AudioClip& clip : m_audioClips) {
        if (clip.contentId != contentId) continue;
        clip.sourcePath = safePath;
        clip.sourceDurationSeconds = safeDuration;
        clip.mediaOffline = false;
        ++updated;
    }
    selectAudioClip(m_selectedClipIndex);
    viewport()->update();
    emit contentChanged();
    return updated;
}

QStringList PlaylistView::missingAudioSourcePaths() const
{
    QStringList paths;
    for (const AudioClip& clip : m_audioClips) {
        if (clip.mediaOffline && !clip.sourcePath.trimmed().isEmpty() &&
            !paths.contains(clip.sourcePath, Qt::CaseInsensitive)) paths.push_back(clip.sourcePath);
    }
    return paths;
}

void PlaylistView::updateAudioClipLengthsForTempo(double projectTempoBpm)
{
    const double safeTempo = std::isfinite(projectTempoBpm) && projectTempoBpm > 0.0
        ? qBound(20.0, projectTempoBpm, 300.0)
        : 120.0;
    for (AudioClip& clip : m_audioClips) {
        if (clip.syncMode == 2 && clip.targetBars > 0.0) {
            scaleAudioClipWarp(clip, clip.targetBars * clip.beatsPerBar);
        } else if (clip.syncMode == 0 && clip.sourceDurationSeconds > 0.0) {
            const double pitchRatio = std::pow(2.0, clip.pitchShiftSemitones / 12.0);
            scaleAudioClipWarp(clip, clip.sourceDurationSeconds * safeTempo / (60.0 * pitchRatio));
        } else if (clip.syncMode == 1 && clip.sourceDurationSeconds > 0.0 && clip.sourceTempoBpm > 0.0) {
            scaleAudioClipWarp(clip, clip.sourceDurationSeconds * clip.sourceTempoBpm / 60.0);
        }
    }
    updateSceneBounds();
    viewport()->update();
    if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 && m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
        const AudioClip& clip = m_audioClips[m_selectedClipIndex];
        emit audioClipSelected(clip.trackIndex, clip.name, clip.startBeat, clip.lengthBeats, clip.sourcePath,
                               clip.sourceTempoBpm, clip.pitchShiftSemitones, clip.syncMode, clip.targetBars);
    }
    emit contentChanged();
}

void PlaylistView::clearAllClips()
{
    resetArrangementHistory();
    m_midiClips.clear();
    m_audioClips.clear();
    m_midiContentRefs.clear();
    m_audioContentRefs.clear();
    m_firstMidiContentIndex.clear();
    m_firstAudioContentIndex.clear();
    m_midiClipClipboard.clear();
    m_audioClipClipboard.clear();
    m_clipboardOriginBeat = 0.0;
    m_clipboardSpanBeats = 0.0;
    m_clipboardPrimaryClipId.clear();
    m_clipboardPrimaryIsAudio = false;
    m_selectedClipIndex = -1;
    m_selectedClipIsAudio = false;
    m_selectedClipIds.clear();
    m_timeSelectionEnabled = false;
    m_timeSelectionDragging = false;
    m_timeSelectionStartBeat = 0.0;
    m_timeSelectionEndBeat = 0.0;
    clearClipDrag();
    updateSceneBounds();
    viewport()->update();
    emit clipSelectionChanged();
    emit clipClipboardChanged();
    emit contentChanged();
}

bool PlaylistView::selectedClipIsAudio() const
{
    return m_selectedClipIsAudio &&
        m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < (int)m_audioClips.size();
}

QStringList PlaylistView::selectedClipIds() const
{
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(m_selectedClipIds.size()));
    for (const QString& id : m_selectedClipIds) ids.push_back(id);
    return ids;
}

bool PlaylistView::selectedMidiClipInfo(MidiClipInfo* out) const
{
    if (m_selectedClipIsAudio || m_selectedClipIndex < 0 || m_selectedClipIndex >= (int)m_midiClips.size() || !out) {
        return false;
    }
    const MidiClip& clip = m_midiClips[m_selectedClipIndex];
    *out = {clip.clipId, clip.contentId, clip.trackIndex, clip.name,
            clip.startBeat, clip.lengthBeats, clip.sourceOffsetBeats,
            clip.loopEnabled, clip.loopStartBeats, clip.loopLengthBeats,
            clip.isUnique, clip.muted, clip.clipColor, clip.groupId, clip.locked,
            clip.takeGroupId, clip.takeLane, clip.takeActive};
    return true;
}

bool PlaylistView::selectedAudioClipInfo(AudioClipInfo* out) const
{
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 || m_selectedClipIndex >= (int)m_audioClips.size() || !out) {
        return false;
    }
    const AudioClip& clip = m_audioClips[m_selectedClipIndex];
    *out = {clip.clipId, clip.contentId, clip.trackIndex, clip.name, clip.startBeat, clip.lengthBeats,
            clip.sourceOffsetBeats, clip.loopEnabled, clip.loopStartBeats, clip.loopLengthBeats,
            clip.warpLengthBeats,
            clip.sourceTempoBpm, clip.tempoConfidence, clip.sourceSignatureNumerator,
            clip.sourceSignatureDenominator, clip.tempoDetected,
            clip.transientSeconds,
            clip.warpMarkers,
            clip.pitchShiftSemitones, clip.syncMode, clip.targetBars,
            clip.beatsPerBar, clip.sourceDurationSeconds, clip.gain, clip.pan,
            clip.normalize, clip.reverse, clip.reversePolarity,
            clip.fadeInBeats, clip.fadeOutBeats, clip.isUnique, clip.sourcePath,
            clip.mediaOffline, clip.gainEnvelope, clip.fadeInCurve, clip.fadeOutCurve,
            clip.muted, clip.clipColor, clip.groupId, clip.locked,
            clip.takeGroupId, clip.takeLane, clip.takeActive};
    return true;
}

bool PlaylistView::renameSelectedClip(const QString& name)
{
    const QString safeName = name.trimmed();
    if (safeName.isEmpty() || m_selectedClipIndex < 0) {
        return false;
    }
    pushUndoState();
    if (m_selectedClipIsAudio) {
        if (m_selectedClipIndex >= (int)m_audioClips.size()) return false;
        const QString contentId = m_audioClips[m_selectedClipIndex].contentId;
        for (AudioClip& clip : m_audioClips)
            if (clip.contentId == contentId) clip.name = safeName;
        selectAudioClip(m_selectedClipIndex);
    } else {
        if (m_selectedClipIndex >= (int)m_midiClips.size()) return false;
        const QString contentId = m_midiClips[m_selectedClipIndex].contentId;
        for (MidiClip& clip : m_midiClips)
            if (clip.contentId == contentId) clip.name = safeName;
        selectClip(m_selectedClipIndex, false);
    }
    updateSceneBounds();
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::copySelectedClips()
{
    if (m_selectedClipIds.empty()) return false;
    double firstBeat = std::numeric_limits<double>::infinity();
    double lastBeat = 0.0;
    std::vector<MidiClip> midiClipboard;
    std::vector<AudioClip> audioClipboard;
    for (const MidiClip& clip : m_midiClips) {
        if (!isClipSelected(clip.clipId)) continue;
        midiClipboard.push_back(clip);
        firstBeat = qMin(firstBeat, clip.startBeat);
        lastBeat = qMax(lastBeat, clip.startBeat + clip.lengthBeats);
    }
    for (const AudioClip& clip : m_audioClips) {
        if (!isClipSelected(clip.clipId)) continue;
        audioClipboard.push_back(clip);
        firstBeat = qMin(firstBeat, clip.startBeat);
        lastBeat = qMax(lastBeat, clip.startBeat + clip.lengthBeats);
    }
    if (!std::isfinite(firstBeat) || lastBeat <= firstBeat ||
        (midiClipboard.empty() && audioClipboard.empty())) return false;

    m_midiClipClipboard = std::move(midiClipboard);
    m_audioClipClipboard = std::move(audioClipboard);
    m_clipboardOriginBeat = firstBeat;
    m_clipboardSpanBeats = qMax(0.25, lastBeat - firstBeat);
    m_clipboardPrimaryClipId.clear();
    m_clipboardPrimaryIsAudio = m_selectedClipIsAudio;
    if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size()))
        m_clipboardPrimaryClipId = m_audioClips[m_selectedClipIndex].clipId;
    else if (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
             m_selectedClipIndex < static_cast<int>(m_midiClips.size()))
        m_clipboardPrimaryClipId = m_midiClips[m_selectedClipIndex].clipId;
    emit clipClipboardChanged();
    return true;
}

bool PlaylistView::cutSelectedClips()
{
    if (selectionContainsLockedClip()) return false;
    return copySelectedClips() && deleteSelectedClip();
}

bool PlaylistView::pasteClips()
{
    if (m_midiClipClipboard.empty() && m_audioClipClipboard.empty()) return false;
    pushUndoState();
    const double targetBeat = m_timeSelectionEnabled
        ? m_timeSelectionStartBeat : qMax(0.0, m_playheadBeat);
    std::set<QString> pastedIds;
    QString pastedPrimaryId;
    bool pastedPrimaryAudio = false;
    std::map<QString, QString> pastedGroupIds;
    std::map<QString, QString> pastedTakeGroupIds;
    auto remapGroupId = [&pastedGroupIds](const QString& sourceGroupId) {
        if (sourceGroupId.isEmpty()) return QString{};
        QString& mapped = pastedGroupIds[sourceGroupId];
        if (mapped.isEmpty()) mapped = stableClipId();
        return mapped;
    };
    auto remapTakeGroupId = [&pastedTakeGroupIds](const QString& sourceGroupId) {
        if (sourceGroupId.isEmpty()) return QString{};
        QString& mapped = pastedTakeGroupIds[sourceGroupId];
        if (mapped.isEmpty()) mapped = stableClipId();
        return mapped;
    };
    for (const MidiClip& source : m_midiClipClipboard) {
        if (!isMidiTrack(source.trackIndex)) continue;
        MidiClip pasted = source;
        pasted.clipId = stableClipId();
        pasted.groupId = remapGroupId(source.groupId);
        pasted.takeGroupId = remapTakeGroupId(source.takeGroupId);
        pasted.startBeat = targetBeat + qMax(0.0, source.startBeat - m_clipboardOriginBeat);
        pastedIds.insert(pasted.clipId);
        if (!m_clipboardPrimaryIsAudio && source.clipId == m_clipboardPrimaryClipId) {
            pastedPrimaryId = pasted.clipId;
            pastedPrimaryAudio = false;
        }
        m_midiClips.push_back(std::move(pasted));
        addContentReference(m_midiClips.back().contentId, false);
    }
    for (const AudioClip& source : m_audioClipClipboard) {
        if (!isAudioTrack(source.trackIndex)) continue;
        AudioClip pasted = source;
        pasted.clipId = stableClipId();
        pasted.groupId = remapGroupId(source.groupId);
        pasted.takeGroupId = remapTakeGroupId(source.takeGroupId);
        pasted.startBeat = targetBeat + qMax(0.0, source.startBeat - m_clipboardOriginBeat);
        pastedIds.insert(pasted.clipId);
        if (m_clipboardPrimaryIsAudio && source.clipId == m_clipboardPrimaryClipId) {
            pastedPrimaryId = pasted.clipId;
            pastedPrimaryAudio = true;
        }
        m_audioClips.push_back(std::move(pasted));
        addContentReference(m_audioClips.back().contentId, true);
    }
    if (pastedIds.empty()) return false;

    m_selectedClipIds = std::move(pastedIds);
    normalizeTakeGroups();
    updateSceneBounds();
    restorePrimaryClipSelection(pastedPrimaryId, pastedPrimaryAudio);
    if (m_timeSelectionEnabled) {
        setTimeSelection(false, 0.0, 0.0);
        emit timeSelectionChanged(false, 0.0, 0.0);
    }
    const double nextPasteBeat = targetBeat + qMax(0.25, m_clipboardSpanBeats);
    setPlayheadBeat(nextPasteBeat);
    emit playheadSeekRequested(nextPasteBeat);
    emit contentChanged();
    return true;
}

bool PlaylistView::clipboardContainsMidiContent(const QString& contentId) const
{
    if (contentId.isEmpty()) return false;
    return std::any_of(m_midiClipClipboard.begin(), m_midiClipClipboard.end(),
        [&contentId](const MidiClip& clip) { return clip.contentId == contentId; });
}

PlaylistView::ArrangementState PlaylistView::captureArrangementState() const
{
    ArrangementState state;
    state.stateId = m_currentArrangementStateId;
    state.midiClips = m_midiClips;
    state.audioClips = m_audioClips;
    state.selectedClipIds = m_selectedClipIds;
    state.primaryIsAudio = m_selectedClipIsAudio;
    if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size()))
        state.primaryClipId = m_audioClips[m_selectedClipIndex].clipId;
    else if (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
             m_selectedClipIndex < static_cast<int>(m_midiClips.size()))
        state.primaryClipId = m_midiClips[m_selectedClipIndex].clipId;
    state.timeSelectionEnabled = m_timeSelectionEnabled;
    state.timeSelectionStartBeat = m_timeSelectionStartBeat;
    state.timeSelectionEndBeat = m_timeSelectionEndBeat;
    return state;
}

void PlaylistView::pushUndoState()
{
    if (m_suppressUndoCapture) return;
    if (m_arrangementTransactionDepth > 0) {
        m_arrangementTransactionChanged = true;
        return;
    }
    pushUndoState(captureArrangementState());
}

void PlaylistView::pushUndoState(ArrangementState state)
{
    if (m_suppressUndoCapture) return;
    constexpr size_t kMaxArrangementHistory = 64;
    const quint64 previousStateId = m_currentArrangementStateId;
    if (m_undoHistory.size() >= kMaxArrangementHistory) m_undoHistory.erase(m_undoHistory.begin());
    m_undoHistory.push_back(std::move(state));
    m_redoHistory.clear();
    m_currentArrangementStateId = m_nextArrangementStateId++;
    emit arrangementHistoryChanged(!m_undoHistory.empty(), false);
    emit arrangementCleanStateChanged(isArrangementClean());
    emit arrangementStateTransitioned(previousStateId, m_currentArrangementStateId);
}

void PlaylistView::restoreArrangementState(ArrangementState state)
{
    m_midiClips = std::move(state.midiClips);
    m_audioClips = std::move(state.audioClips);
    m_selectedClipIds = std::move(state.selectedClipIds);
    m_selectedClipIndex = -1;
    m_selectedClipIsAudio = state.primaryIsAudio;
    m_timeSelectionEnabled = state.timeSelectionEnabled;
    m_timeSelectionStartBeat = state.timeSelectionStartBeat;
    m_timeSelectionEndBeat = state.timeSelectionEndBeat;
    clearClipDrag();
    rebuildContentReferences();
    updateSceneBounds();
    emit arrangementStateRestored();
    restorePrimaryClipSelection(state.primaryClipId, state.primaryIsAudio);
    viewport()->update();
    emit contentChanged();
}

bool PlaylistView::undoArrangementEdit()
{
    if (m_undoHistory.empty()) return false;
    constexpr size_t kMaxArrangementHistory = 64;
    const quint64 previousStateId = m_currentArrangementStateId;
    if (m_redoHistory.size() >= kMaxArrangementHistory) m_redoHistory.erase(m_redoHistory.begin());
    m_redoHistory.push_back(captureArrangementState());
    ArrangementState state = std::move(m_undoHistory.back());
    m_undoHistory.pop_back();
    m_currentArrangementStateId = state.stateId;
    emit arrangementStateWillTransition(previousStateId, m_currentArrangementStateId);
    m_suppressUndoCapture = true;
    restoreArrangementState(std::move(state));
    m_suppressUndoCapture = false;
    emit arrangementHistoryChanged(!m_undoHistory.empty(), !m_redoHistory.empty());
    emit arrangementCleanStateChanged(isArrangementClean());
    emit arrangementStateTransitioned(previousStateId, m_currentArrangementStateId);
    return true;
}

bool PlaylistView::redoArrangementEdit()
{
    if (m_redoHistory.empty()) return false;
    constexpr size_t kMaxArrangementHistory = 64;
    const quint64 previousStateId = m_currentArrangementStateId;
    if (m_undoHistory.size() >= kMaxArrangementHistory) m_undoHistory.erase(m_undoHistory.begin());
    m_undoHistory.push_back(captureArrangementState());
    ArrangementState state = std::move(m_redoHistory.back());
    m_redoHistory.pop_back();
    m_currentArrangementStateId = state.stateId;
    emit arrangementStateWillTransition(previousStateId, m_currentArrangementStateId);
    m_suppressUndoCapture = true;
    restoreArrangementState(std::move(state));
    m_suppressUndoCapture = false;
    emit arrangementHistoryChanged(!m_undoHistory.empty(), !m_redoHistory.empty());
    emit arrangementCleanStateChanged(isArrangementClean());
    emit arrangementStateTransitioned(previousStateId, m_currentArrangementStateId);
    return true;
}

void PlaylistView::resetArrangementHistory()
{
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_pendingDragUndoState.reset();
    m_transactionBaseState.reset();
    m_arrangementTransactionDepth = 0;
    m_arrangementTransactionChanged = false;
    m_currentArrangementStateId = m_nextArrangementStateId++;
    m_cleanArrangementStateId = m_currentArrangementStateId;
    emit arrangementHistoryChanged(false, false);
    emit arrangementCleanStateChanged(true);
}

bool PlaylistView::isArrangementClean() const
{
    return m_currentArrangementStateId == m_cleanArrangementStateId;
}

quint64 PlaylistView::currentArrangementStateId() const
{
    return m_currentArrangementStateId;
}

bool PlaylistView::historyContainsArrangementState(quint64 stateId) const
{
    if (stateId == m_currentArrangementStateId) return true;
    auto contains = [stateId](const ArrangementState& state) { return state.stateId == stateId; };
    if (std::any_of(m_undoHistory.begin(), m_undoHistory.end(), contains) ||
        std::any_of(m_redoHistory.begin(), m_redoHistory.end(), contains)) return true;
    return (m_pendingDragUndoState && m_pendingDragUndoState->stateId == stateId) ||
           (m_transactionBaseState && m_transactionBaseState->stateId == stateId);
}

void PlaylistView::markArrangementCleanState()
{
    m_cleanArrangementStateId = m_currentArrangementStateId;
    emit arrangementCleanStateChanged(true);
}

void PlaylistView::beginArrangementTransaction()
{
    if (m_arrangementTransactionDepth++ > 0) return;
    m_transactionBaseState = std::make_unique<ArrangementState>(captureArrangementState());
    m_arrangementTransactionChanged = false;
}

void PlaylistView::markArrangementTransactionChanged()
{
    if (m_arrangementTransactionDepth > 0) m_arrangementTransactionChanged = true;
}

bool PlaylistView::commitArrangementTransaction()
{
    if (m_arrangementTransactionDepth <= 0) return false;
    if (--m_arrangementTransactionDepth > 0) return false;
    const bool changed = m_arrangementTransactionChanged && m_transactionBaseState;
    m_arrangementTransactionChanged = false;
    if (!changed) {
        m_transactionBaseState.reset();
        return false;
    }
    ArrangementState baseState = std::move(*m_transactionBaseState);
    m_transactionBaseState.reset();
    pushUndoState(std::move(baseState));
    return true;
}

void PlaylistView::cancelArrangementTransaction()
{
    if (m_arrangementTransactionDepth <= 0) return;
    m_arrangementTransactionDepth = 0;
    m_arrangementTransactionChanged = false;
    if (!m_transactionBaseState) return;
    ArrangementState baseState = std::move(*m_transactionBaseState);
    m_transactionBaseState.reset();
    m_suppressUndoCapture = true;
    restoreArrangementState(std::move(baseState));
    m_suppressUndoCapture = false;
    emit arrangementHistoryChanged(!m_undoHistory.empty(), !m_redoHistory.empty());
    emit arrangementCleanStateChanged(isArrangementClean());
}

bool PlaylistView::canCoalesceLastUndoState(quint64 stateId) const
{
    return m_arrangementTransactionDepth == 0 && !m_pendingDragUndoState &&
        m_redoHistory.empty() && !m_undoHistory.empty() &&
        m_undoHistory.back().stateId == stateId &&
        m_cleanArrangementStateId != stateId;
}

bool PlaylistView::discardLastUndoState(quint64 stateId)
{
    if (!canCoalesceLastUndoState(stateId)) return false;
    m_undoHistory.pop_back();
    emit arrangementHistoryChanged(!m_undoHistory.empty(), false);
    emit arrangementCleanStateChanged(isArrangementClean());
    return true;
}

bool PlaylistView::historyContainsMidiContent(const QString& contentId) const
{
    if (contentId.isEmpty()) return false;
    auto stateContains = [&contentId](const ArrangementState& state) {
        return std::any_of(state.midiClips.begin(), state.midiClips.end(),
            [&contentId](const MidiClip& clip) { return clip.contentId == contentId; });
    };
    if (std::any_of(m_undoHistory.begin(), m_undoHistory.end(), stateContains) ||
        std::any_of(m_redoHistory.begin(), m_redoHistory.end(), stateContains)) return true;
    return (m_pendingDragUndoState && stateContains(*m_pendingDragUndoState)) ||
           (m_transactionBaseState && stateContains(*m_transactionBaseState));
}

bool PlaylistView::duplicateSelectedClip()
{
    if (m_selectedClipIds.empty() || selectionContainsLockedClip()) return false;

    double firstBeat = std::numeric_limits<double>::infinity();
    double lastBeat = 0.0;
    for (const MidiClip& clip : m_midiClips) {
        if (!isClipSelected(clip.clipId)) continue;
        firstBeat = std::min(firstBeat, clip.startBeat);
        lastBeat = std::max(lastBeat, clip.startBeat + clip.lengthBeats);
    }
    for (const AudioClip& clip : m_audioClips) {
        if (!isClipSelected(clip.clipId)) continue;
        firstBeat = std::min(firstBeat, clip.startBeat);
        lastBeat = std::max(lastBeat, clip.startBeat + clip.lengthBeats);
    }
    if (!std::isfinite(firstBeat) || lastBeat <= firstBeat) return false;
    pushUndoState();
    const double duplicateOffset = m_timeSelectionEnabled
        ? m_timeSelectionEndBeat - m_timeSelectionStartBeat
        : lastBeat - firstBeat;

    QString primaryId;
    if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
        primaryId = m_audioClips[m_selectedClipIndex].clipId;
    } else if (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
               m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
        primaryId = m_midiClips[m_selectedClipIndex].clipId;
    }

    std::set<QString> duplicatedIds;
    std::map<QString, QString> duplicatedGroupIds;
    std::map<QString, QString> duplicatedTakeGroupIds;
    auto remapGroupId = [&duplicatedGroupIds](const QString& sourceGroupId) {
        if (sourceGroupId.isEmpty()) return QString{};
        QString& mapped = duplicatedGroupIds[sourceGroupId];
        if (mapped.isEmpty()) mapped = stableClipId();
        return mapped;
    };
    auto remapTakeGroupId = [&duplicatedTakeGroupIds](const QString& sourceGroupId) {
        if (sourceGroupId.isEmpty()) return QString{};
        QString& mapped = duplicatedTakeGroupIds[sourceGroupId];
        if (mapped.isEmpty()) mapped = stableClipId();
        return mapped;
    };
    int primaryMidi = -1;
    int primaryAudio = -1;
    const int originalMidiCount = static_cast<int>(m_midiClips.size());
    const int originalAudioCount = static_cast<int>(m_audioClips.size());
    for (int index = 0; index < originalMidiCount; ++index) {
        if (!isClipSelected(m_midiClips[index].clipId)) continue;
        const QString sourceId = m_midiClips[index].clipId;
        MidiClip duplicate = m_midiClips[index];
        duplicate.clipId = stableClipId();
        duplicate.groupId = remapGroupId(m_midiClips[index].groupId);
        duplicate.takeGroupId = remapTakeGroupId(m_midiClips[index].takeGroupId);
        duplicate.startBeat += duplicateOffset;
        duplicatedIds.insert(duplicate.clipId);
        m_midiClips.push_back(std::move(duplicate));
        addContentReference(m_midiClips.back().contentId, false);
        if (sourceId == primaryId) primaryMidi = static_cast<int>(m_midiClips.size()) - 1;
    }
    for (int index = 0; index < originalAudioCount; ++index) {
        if (!isClipSelected(m_audioClips[index].clipId)) continue;
        const QString sourceId = m_audioClips[index].clipId;
        AudioClip duplicate = m_audioClips[index];
        duplicate.clipId = stableClipId();
        duplicate.groupId = remapGroupId(m_audioClips[index].groupId);
        duplicate.takeGroupId = remapTakeGroupId(m_audioClips[index].takeGroupId);
        duplicate.startBeat += duplicateOffset;
        duplicatedIds.insert(duplicate.clipId);
        m_audioClips.push_back(std::move(duplicate));
        addContentReference(m_audioClips.back().contentId, true);
        if (sourceId == primaryId) primaryAudio = static_cast<int>(m_audioClips.size()) - 1;
    }
    if (duplicatedIds.empty()) return false;

    normalizeTakeGroups();
    m_selectedClipIds = std::move(duplicatedIds);
    updateSceneBounds();
    if (primaryAudio >= 0) selectAudioClip(primaryAudio, true);
    else if (primaryMidi >= 0) selectClip(primaryMidi, false, true);
    else selectFirstRemainingClip();
    emit contentChanged();
    return true;
}

bool PlaylistView::setSelectedClipsMuted(bool muted)
{
    bool hasSelected = false;
    bool changed = false;
    for (const MidiClip& clip : m_midiClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        changed = changed || clip.muted != muted;
    }
    for (const AudioClip& clip : m_audioClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        changed = changed || clip.muted != muted;
    }
    if (!hasSelected || !changed) return false;

    pushUndoState();
    for (MidiClip& clip : m_midiClips)
        if (isClipSelected(clip.clipId)) clip.muted = muted;
    for (AudioClip& clip : m_audioClips)
        if (isClipSelected(clip.clipId)) clip.muted = muted;
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::toggleSelectedClipsMuted()
{
    bool hasSelected = false;
    bool anyUnmuted = false;
    for (const MidiClip& clip : m_midiClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        anyUnmuted = anyUnmuted || !clip.muted;
    }
    for (const AudioClip& clip : m_audioClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        anyUnmuted = anyUnmuted || !clip.muted;
    }
    return hasSelected && setSelectedClipsMuted(anyUnmuted);
}

bool PlaylistView::setAudioClipMutedById(const QString& clipId, bool muted)
{
    const auto found = std::find_if(m_audioClips.begin(), m_audioClips.end(),
        [&clipId](const AudioClip& clip) { return clip.clipId == clipId; });
    if (found == m_audioClips.end() || found->locked || found->muted == muted) return false;
    pushUndoState();
    found->muted = muted;
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::setSelectedClipsColor(const QColor& color)
{
    const QColor normalized = normalizedClipColor(color);
    bool hasSelected = false;
    bool changed = false;
    for (const MidiClip& clip : m_midiClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        changed = changed || clip.clipColor != normalized;
    }
    for (const AudioClip& clip : m_audioClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        changed = changed || clip.clipColor != normalized;
    }
    if (!hasSelected || !changed) return false;

    pushUndoState();
    for (MidiClip& clip : m_midiClips)
        if (isClipSelected(clip.clipId)) clip.clipColor = normalized;
    for (AudioClip& clip : m_audioClips)
        if (isClipSelected(clip.clipId)) clip.clipColor = normalized;
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::setSelectedClipsLoopEnabled(bool enabled, double loopStartBeats,
                                               double loopLengthBeats)
{
    expandSelectionToClipGroups();
    if (selectionContainsLockedClip()) return false;
    bool hasSelected = false;
    bool changed = false;
    const bool requestedStart = std::isfinite(loopStartBeats) && loopStartBeats >= 0.0;
    const bool requestedLength = std::isfinite(loopLengthBeats) && loopLengthBeats >= 0.25;
    auto inspect = [&](const auto& clip) {
        if (!isClipSelected(clip.clipId)) return;
        hasSelected = true;
        changed = changed || clip.loopEnabled != enabled ||
            (enabled && requestedStart &&
             !qFuzzyCompare(1.0 + clip.loopStartBeats, 1.0 + loopStartBeats)) ||
            (enabled && (requestedLength
                ? !qFuzzyCompare(1.0 + clip.loopLengthBeats, 1.0 + loopLengthBeats)
                : (!std::isfinite(clip.loopLengthBeats) || clip.loopLengthBeats < 0.25)));
    };
    for (const MidiClip& clip : m_midiClips) inspect(clip);
    for (const AudioClip& clip : m_audioClips) inspect(clip);
    if (!hasSelected || !changed) return false;

    pushUndoState();
    for (MidiClip& clip : m_midiClips) {
        if (!isClipSelected(clip.clipId)) continue;
        if (enabled && (requestedStart || !std::isfinite(clip.loopStartBeats) ||
                        clip.loopStartBeats < 0.0))
            clip.loopStartBeats = requestedStart ? loopStartBeats : clip.sourceOffsetBeats;
        if (enabled && (requestedLength || !std::isfinite(clip.loopLengthBeats) ||
                        clip.loopLengthBeats < 0.25))
            clip.loopLengthBeats = requestedLength
                ? qMax(0.25, loopLengthBeats) : qMax(0.25, clip.lengthBeats);
        clip.loopEnabled = enabled;
        if (enabled) advanceMidiClipSourceOffset(clip, 0.0);
    }
    for (AudioClip& clip : m_audioClips) {
        if (!isClipSelected(clip.clipId)) continue;
        if (enabled && (requestedStart || !std::isfinite(clip.loopStartBeats) ||
                        clip.loopStartBeats < 0.0))
            clip.loopStartBeats = qBound(0.0,
                requestedStart ? loopStartBeats : clip.sourceOffsetBeats,
                qMax(0.0, clip.warpLengthBeats - 0.25));
        if (enabled && (requestedLength || !std::isfinite(clip.loopLengthBeats) ||
                        clip.loopLengthBeats < 0.25)) {
            const double available = qMax(0.25, clip.warpLengthBeats - clip.loopStartBeats);
            clip.loopLengthBeats = qBound(0.25,
                requestedLength ? loopLengthBeats : clip.lengthBeats, available);
        }
        clip.loopEnabled = enabled;
        if (enabled) advanceAudioClipSourceOffset(clip, 0.0);
    }
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::toggleSelectedClipsLoopEnabled()
{
    expandSelectionToClipGroups();
    bool hasSelected = false;
    bool anyDisabled = false;
    for (const MidiClip& clip : m_midiClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        anyDisabled = anyDisabled || !clip.loopEnabled;
    }
    for (const AudioClip& clip : m_audioClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        anyDisabled = anyDisabled || !clip.loopEnabled;
    }
    return hasSelected && setSelectedClipsLoopEnabled(anyDisabled);
}

bool PlaylistView::setSelectedClipsGroupId(const QString& groupId)
{
    const QString normalized = groupId.trimmed().left(128);
    bool hasSelected = false;
    bool changed = false;
    for (const MidiClip& clip : m_midiClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        changed = changed || clip.groupId != normalized;
    }
    for (const AudioClip& clip : m_audioClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        changed = changed || clip.groupId != normalized;
    }
    if (!hasSelected || !changed) return false;

    pushUndoState();
    for (MidiClip& clip : m_midiClips)
        if (isClipSelected(clip.clipId)) clip.groupId = normalized;
    for (AudioClip& clip : m_audioClips)
        if (isClipSelected(clip.clipId)) clip.groupId = normalized;
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::groupSelectedClips()
{
    expandSelectionToClipGroups();
    if (selectionContainsLockedClip()) return false;
    int selectedCount = 0;
    QString commonGroup;
    bool commonInitialized = false;
    bool allSameGroup = true;
    auto inspect = [&](const QString& clipId, const QString& groupId) {
        if (!isClipSelected(clipId)) return;
        ++selectedCount;
        if (!commonInitialized) {
            commonGroup = groupId;
            commonInitialized = true;
        } else if (commonGroup != groupId) {
            allSameGroup = false;
        }
    };
    for (const MidiClip& clip : m_midiClips) inspect(clip.clipId, clip.groupId);
    for (const AudioClip& clip : m_audioClips) inspect(clip.clipId, clip.groupId);
    if (selectedCount < 2 || (allSameGroup && !commonGroup.isEmpty())) return false;
    return setSelectedClipsGroupId(stableClipId());
}

bool PlaylistView::ungroupSelectedClips()
{
    expandSelectionToClipGroups();
    if (selectionContainsLockedClip()) return false;
    return setSelectedClipsGroupId(QString());
}

bool PlaylistView::groupSelectedClipsAsTakes()
{
    if (selectionContainsLockedClip() || m_selectedClipIds.size() < 2) return false;
    const QString primaryId = m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size())
        ? m_audioClips[m_selectedClipIndex].clipId
        : (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
           m_selectedClipIndex < static_cast<int>(m_midiClips.size())
            ? m_midiClips[m_selectedClipIndex].clipId : QString{});
    if (primaryId.isEmpty()) return false;

    int trackIndex = -1;
    double startBeat = -1.0;
    double lengthBeats = -1.0;
    int selectedCount = 0;
    bool mixedTypes = false;
    auto inspect = [&](int track, double start, double length, bool audio) {
        if (audio != m_selectedClipIsAudio) { mixedTypes = true; return; }
        if (trackIndex < 0) {
            trackIndex = track;
            startBeat = start;
            lengthBeats = length;
        } else if (track != trackIndex || std::fabs(start - startBeat) > 1.0e-6 ||
                   std::fabs(length - lengthBeats) > 1.0e-6) {
            mixedTypes = true;
        }
        ++selectedCount;
    };
    for (const MidiClip& clip : m_midiClips)
        if (isClipSelected(clip.clipId)) inspect(clip.trackIndex, clip.startBeat, clip.lengthBeats, false);
    for (const AudioClip& clip : m_audioClips)
        if (isClipSelected(clip.clipId)) inspect(clip.trackIndex, clip.startBeat, clip.lengthBeats, true);
    if (mixedTypes || selectedCount < 2) return false;

    pushUndoState();
    const QString group = stableClipId();
    int lane = 0;
    if (m_selectedClipIsAudio) {
        for (AudioClip& clip : m_audioClips) {
            if (!isClipSelected(clip.clipId)) continue;
            clip.takeGroupId = group;
            clip.takeLane = lane++;
            clip.takeActive = clip.clipId == primaryId;
        }
    } else {
        for (MidiClip& clip : m_midiClips) {
            if (!isClipSelected(clip.clipId)) continue;
            clip.takeGroupId = group;
            clip.takeLane = lane++;
            clip.takeActive = clip.clipId == primaryId;
        }
    }
    updateSceneBounds();
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::setSelectedClipAsActiveTake()
{
    QString group;
    QString selectedId;
    double selectedStart = 0.0;
    double selectedLength = 0.0;
    if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
        const AudioClip& selected = m_audioClips[m_selectedClipIndex];
        group = selected.takeGroupId;
        selectedId = selected.clipId;
        selectedStart = selected.startBeat;
        selectedLength = selected.lengthBeats;
    } else if (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
               m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
        const MidiClip& selected = m_midiClips[m_selectedClipIndex];
        group = selected.takeGroupId;
        selectedId = selected.clipId;
        selectedStart = selected.startBeat;
        selectedLength = selected.lengthBeats;
    }
    if (group.isEmpty() || selectedId.isEmpty()) return false;
    bool changed = false;
    if (m_selectedClipIsAudio) {
        for (const AudioClip& clip : m_audioClips)
            if (clip.takeGroupId == group && std::fabs(clip.startBeat - selectedStart) <= 1.0e-6 &&
                std::fabs(clip.lengthBeats - selectedLength) <= 1.0e-6)
                changed = changed || clip.takeActive != (clip.clipId == selectedId);
    } else {
        for (const MidiClip& clip : m_midiClips)
            if (clip.takeGroupId == group && std::fabs(clip.startBeat - selectedStart) <= 1.0e-6 &&
                std::fabs(clip.lengthBeats - selectedLength) <= 1.0e-6)
                changed = changed || clip.takeActive != (clip.clipId == selectedId);
    }
    if (!changed) return false;
    pushUndoState();
    if (m_selectedClipIsAudio) {
        for (AudioClip& clip : m_audioClips)
            if (clip.takeGroupId == group && std::fabs(clip.startBeat - selectedStart) <= 1.0e-6 &&
                std::fabs(clip.lengthBeats - selectedLength) <= 1.0e-6)
                clip.takeActive = clip.clipId == selectedId;
        refreshAudioTakeCompSeams(group);
    } else {
        for (MidiClip& clip : m_midiClips)
            if (clip.takeGroupId == group && std::fabs(clip.startBeat - selectedStart) <= 1.0e-6 &&
                std::fabs(clip.lengthBeats - selectedLength) <= 1.0e-6)
                clip.takeActive = clip.clipId == selectedId;
    }
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::compSelectedTakeInTimeSelection()
{
    if (!m_timeSelectionEnabled || m_selectedClipIndex < 0) return false;
    const double rangeStart = m_timeSelectionStartBeat;
    const double rangeEnd = m_timeSelectionEndBeat;
    if (!std::isfinite(rangeStart) || !std::isfinite(rangeEnd) ||
        rangeStart < 0.0 || rangeEnd - rangeStart < 1.0e-6) return false;

    QString group;
    int selectedLane = 0;
    if (m_selectedClipIsAudio && m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
        group = m_audioClips[m_selectedClipIndex].takeGroupId;
        selectedLane = m_audioClips[m_selectedClipIndex].takeLane;
    } else if (!m_selectedClipIsAudio &&
               m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
        group = m_midiClips[m_selectedClipIndex].takeGroupId;
        selectedLane = m_midiClips[m_selectedClipIndex].takeLane;
    }
    if (group.isEmpty()) return false;

    std::vector<std::pair<double, double>> selectedLaneCoverage;
    if (m_selectedClipIsAudio) {
        for (const AudioClip& clip : m_audioClips) {
            if (clip.takeGroupId != group) continue;
            if (clip.locked) return false;
            if (clip.takeLane == selectedLane)
                selectedLaneCoverage.push_back({clip.startBeat, clip.startBeat + clip.lengthBeats});
        }
    } else {
        for (const MidiClip& clip : m_midiClips) {
            if (clip.takeGroupId != group) continue;
            if (clip.locked) return false;
            if (clip.takeLane == selectedLane)
                selectedLaneCoverage.push_back({clip.startBeat, clip.startBeat + clip.lengthBeats});
        }
    }
    std::stable_sort(selectedLaneCoverage.begin(), selectedLaneCoverage.end());
    double coveredUntil = rangeStart;
    for (const auto& coverage : selectedLaneCoverage) {
        if (coverage.second <= coveredUntil + 1.0e-9) continue;
        if (coverage.first > coveredUntil + 1.0e-6) break;
        coveredUntil = qMax(coveredUntil, coverage.second);
        if (coveredUntil >= rangeEnd - 1.0e-6) break;
    }
    if (coveredUntil < rangeEnd - 1.0e-6) return false;

    pushUndoState();
    auto splitMidiAt = [&](double boundary) {
        std::vector<MidiClip> rightClips;
        const size_t count = m_midiClips.size();
        for (size_t index = 0; index < count; ++index) {
            MidiClip& left = m_midiClips[index];
            if (left.takeGroupId != group) continue;
            const double end = left.startBeat + left.lengthBeats;
            const double leftLength = boundary - left.startBeat;
            const double rightLength = end - boundary;
            if (leftLength <= 1.0e-6 || rightLength <= 1.0e-6) continue;
            MidiClip right = left;
            right.clipId = stableClipId();
            right.startBeat = boundary;
            right.lengthBeats = rightLength;
            advanceMidiClipSourceOffset(right, leftLength);
            left.lengthBeats = leftLength;
            rightClips.push_back(std::move(right));
        }
        for (MidiClip& right : rightClips) {
            m_midiClips.push_back(std::move(right));
            addContentReference(m_midiClips.back().contentId, false);
        }
    };
    auto splitAudioAt = [&](double boundary) {
        std::vector<AudioClip> rightClips;
        const size_t count = m_audioClips.size();
        for (size_t index = 0; index < count; ++index) {
            AudioClip& left = m_audioClips[index];
            if (left.takeGroupId != group) continue;
            const double end = left.startBeat + left.lengthBeats;
            const double leftLength = boundary - left.startBeat;
            const double rightLength = end - boundary;
            if (leftLength <= 1.0e-6 || rightLength <= 1.0e-6) continue;
            AudioClip right = left;
            right.clipId = stableClipId();
            right.startBeat = boundary;
            right.lengthBeats = rightLength;
            advanceAudioClipSourceOffset(right, leftLength);
            right.fadeInBeats = 0.0;
            left.fadeOutBeats = 0.0;
            left.lengthBeats = leftLength;
            clampAudioClipFades(left);
            clampAudioClipFades(right);
            rightClips.push_back(std::move(right));
        }
        for (AudioClip& right : rightClips) {
            m_audioClips.push_back(std::move(right));
            addContentReference(m_audioClips.back().contentId, true);
        }
    };
    if (m_selectedClipIsAudio) {
        splitAudioAt(rangeStart);
        splitAudioAt(rangeEnd);
    } else {
        splitMidiAt(rangeStart);
        splitMidiAt(rangeEnd);
    }

    int targetIndex = -1;
    if (m_selectedClipIsAudio) {
        for (int index = 0; index < static_cast<int>(m_audioClips.size()); ++index) {
            AudioClip& clip = m_audioClips[index];
            if (clip.takeGroupId != group) continue;
            const double end = clip.startBeat + clip.lengthBeats;
            if (clip.startBeat >= rangeStart - 1.0e-6 && end <= rangeEnd + 1.0e-6) {
                clip.takeActive = clip.takeLane == selectedLane;
                if (clip.takeActive && targetIndex < 0) targetIndex = index;
            }
        }
        refreshAudioTakeCompSeams(group);
    } else {
        for (int index = 0; index < static_cast<int>(m_midiClips.size()); ++index) {
            MidiClip& clip = m_midiClips[index];
            if (clip.takeGroupId != group) continue;
            const double end = clip.startBeat + clip.lengthBeats;
            if (clip.startBeat >= rangeStart - 1.0e-6 && end <= rangeEnd + 1.0e-6) {
                clip.takeActive = clip.takeLane == selectedLane;
                if (clip.takeActive && targetIndex < 0) targetIndex = index;
            }
        }
    }
    normalizeTakeGroups();
    rebuildContentReferences();
    updateSceneBounds();
    if (targetIndex >= 0) {
        if (m_selectedClipIsAudio) selectAudioClip(targetIndex);
        else selectClip(targetIndex, false);
    }
    viewport()->update();
    emit contentChanged();
    return targetIndex >= 0;
}

bool PlaylistView::flattenSelectedTakeGroup()
{
    if (m_selectedClipIndex < 0) return false;
    QString group;
    if (m_selectedClipIsAudio &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size()))
        group = m_audioClips[m_selectedClipIndex].takeGroupId;
    else if (!m_selectedClipIsAudio &&
             m_selectedClipIndex < static_cast<int>(m_midiClips.size()))
        group = m_midiClips[m_selectedClipIndex].takeGroupId;
    if (group.isEmpty()) return false;

    bool hasActive = false;
    bool hasInactive = false;
    QString targetId;
    if (m_selectedClipIsAudio) {
        for (const AudioClip& clip : m_audioClips) {
            if (clip.takeGroupId != group) continue;
            if (clip.locked) return false;
            hasActive = hasActive || clip.takeActive;
            hasInactive = hasInactive || !clip.takeActive;
            if (clip.takeActive && targetId.isEmpty()) targetId = clip.clipId;
        }
    } else {
        for (const MidiClip& clip : m_midiClips) {
            if (clip.takeGroupId != group) continue;
            if (clip.locked) return false;
            hasActive = hasActive || clip.takeActive;
            hasInactive = hasInactive || !clip.takeActive;
            if (clip.takeActive && targetId.isEmpty()) targetId = clip.clipId;
        }
    }
    if (!hasActive || !hasInactive || targetId.isEmpty()) return false;

    pushUndoState();
    int targetIndex = -1;
    if (m_selectedClipIsAudio) {
        m_audioClips.erase(std::remove_if(m_audioClips.begin(), m_audioClips.end(),
            [&](AudioClip& clip) {
                if (clip.takeGroupId != group) return false;
                if (!clip.takeActive) return true;
                clip.takeGroupId.clear();
                clip.takeLane = 0;
                clip.takeActive = true;
                return false;
            }), m_audioClips.end());
        for (int index = 0; index < static_cast<int>(m_audioClips.size()); ++index)
            if (m_audioClips[index].clipId == targetId) { targetIndex = index; break; }
    } else {
        m_midiClips.erase(std::remove_if(m_midiClips.begin(), m_midiClips.end(),
            [&](MidiClip& clip) {
                if (clip.takeGroupId != group) return false;
                if (!clip.takeActive) return true;
                clip.takeGroupId.clear();
                clip.takeLane = 0;
                clip.takeActive = true;
                return false;
            }), m_midiClips.end());
        for (int index = 0; index < static_cast<int>(m_midiClips.size()); ++index)
            if (m_midiClips[index].clipId == targetId) { targetIndex = index; break; }
    }
    rebuildContentReferences();
    updateSceneBounds();
    if (targetIndex >= 0) {
        if (m_selectedClipIsAudio) selectAudioClip(targetIndex);
        else selectClip(targetIndex, false);
    } else {
        m_selectedClipIndex = -1;
        m_selectedClipIsAudio = false;
        m_selectedClipIds.clear();
        emit clipSelectionChanged();
    }
    viewport()->update();
    emit contentChanged();
    return targetIndex >= 0;
}

bool PlaylistView::cycleSelectedTake(int direction)
{
    if (direction == 0) return false;
    QString group;
    QString selectedId;
    double selectedStart = 0.0;
    double selectedLength = 0.0;
    if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
        group = m_audioClips[m_selectedClipIndex].takeGroupId;
        selectedId = m_audioClips[m_selectedClipIndex].clipId;
        selectedStart = m_audioClips[m_selectedClipIndex].startBeat;
        selectedLength = m_audioClips[m_selectedClipIndex].lengthBeats;
    } else if (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
               m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
        group = m_midiClips[m_selectedClipIndex].takeGroupId;
        selectedId = m_midiClips[m_selectedClipIndex].clipId;
        selectedStart = m_midiClips[m_selectedClipIndex].startBeat;
        selectedLength = m_midiClips[m_selectedClipIndex].lengthBeats;
    }
    if (group.isEmpty()) return false;
    std::vector<std::pair<int, int>> members;
    if (m_selectedClipIsAudio) {
        for (int i = 0; i < static_cast<int>(m_audioClips.size()); ++i)
            if (m_audioClips[i].takeGroupId == group &&
                std::fabs(m_audioClips[i].startBeat - selectedStart) <= 1.0e-6 &&
                std::fabs(m_audioClips[i].lengthBeats - selectedLength) <= 1.0e-6)
                members.push_back({m_audioClips[i].takeLane, i});
    } else {
        for (int i = 0; i < static_cast<int>(m_midiClips.size()); ++i)
            if (m_midiClips[i].takeGroupId == group &&
                std::fabs(m_midiClips[i].startBeat - selectedStart) <= 1.0e-6 &&
                std::fabs(m_midiClips[i].lengthBeats - selectedLength) <= 1.0e-6)
                members.push_back({m_midiClips[i].takeLane, i});
    }
    if (members.size() < 2) return false;
    std::stable_sort(members.begin(), members.end());
    int current = 0;
    for (int i = 0; i < static_cast<int>(members.size()); ++i) {
        const QString id = m_selectedClipIsAudio
            ? m_audioClips[members[i].second].clipId : m_midiClips[members[i].second].clipId;
        if (id == selectedId) { current = i; break; }
    }
    const int targetMember = (current + (direction > 0 ? 1 : -1) +
        static_cast<int>(members.size())) % static_cast<int>(members.size());
    const int targetIndex = members[targetMember].second;
    pushUndoState();
    if (m_selectedClipIsAudio) {
        const QString targetId = m_audioClips[targetIndex].clipId;
        for (AudioClip& clip : m_audioClips)
            if (clip.takeGroupId == group && std::fabs(clip.startBeat - selectedStart) <= 1.0e-6 &&
                std::fabs(clip.lengthBeats - selectedLength) <= 1.0e-6)
                clip.takeActive = clip.clipId == targetId;
        refreshAudioTakeCompSeams(group);
        selectAudioClip(targetIndex);
    } else {
        const QString targetId = m_midiClips[targetIndex].clipId;
        for (MidiClip& clip : m_midiClips)
            if (clip.takeGroupId == group && std::fabs(clip.startBeat - selectedStart) <= 1.0e-6 &&
                std::fabs(clip.lengthBeats - selectedLength) <= 1.0e-6)
                clip.takeActive = clip.clipId == targetId;
        selectClip(targetIndex, false);
    }
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::promoteSelectedClipToRecordedTake()
{
    if (m_selectedClipIndex < 0 || selectionContainsLockedClip()) return false;
    const double tolerance = 1.0e-6;
    if (m_selectedClipIsAudio) {
        if (m_selectedClipIndex >= static_cast<int>(m_audioClips.size())) return false;
        AudioClip& selected = m_audioClips[m_selectedClipIndex];
        int candidateIndex = -1;
        int ungroupedCandidate = -1;
        for (int i = 0; i < static_cast<int>(m_audioClips.size()); ++i) {
            const AudioClip& candidate = m_audioClips[i];
            if (i == m_selectedClipIndex || candidate.trackIndex != selected.trackIndex ||
                std::fabs(candidate.startBeat - selected.startBeat) > tolerance ||
                std::fabs(candidate.lengthBeats - selected.lengthBeats) > tolerance) continue;
            if (!candidate.takeGroupId.isEmpty() && candidate.takeActive) {
                candidateIndex = i;
                break;
            }
            if (candidate.takeGroupId.isEmpty() && ungroupedCandidate < 0) ungroupedCandidate = i;
        }
        if (candidateIndex < 0) candidateIndex = ungroupedCandidate;
        if (candidateIndex < 0) return false;
        pushUndoState();
        QString group = m_audioClips[candidateIndex].takeGroupId;
        if (group.isEmpty()) {
            group = stableClipId();
            m_audioClips[candidateIndex].takeGroupId = group;
            m_audioClips[candidateIndex].takeLane = 0;
        }
        int maxLane = 0;
        for (AudioClip& clip : m_audioClips) {
            if (clip.takeGroupId != group) continue;
            maxLane = qMax(maxLane, clip.takeLane);
            clip.takeActive = false;
        }
        selected.takeGroupId = group;
        selected.takeLane = maxLane + 1;
        selected.takeActive = true;
    } else {
        if (m_selectedClipIndex >= static_cast<int>(m_midiClips.size())) return false;
        MidiClip& selected = m_midiClips[m_selectedClipIndex];
        int candidateIndex = -1;
        int ungroupedCandidate = -1;
        for (int i = 0; i < static_cast<int>(m_midiClips.size()); ++i) {
            const MidiClip& candidate = m_midiClips[i];
            if (i == m_selectedClipIndex || candidate.trackIndex != selected.trackIndex ||
                std::fabs(candidate.startBeat - selected.startBeat) > tolerance ||
                std::fabs(candidate.lengthBeats - selected.lengthBeats) > tolerance) continue;
            if (!candidate.takeGroupId.isEmpty() && candidate.takeActive) {
                candidateIndex = i;
                break;
            }
            if (candidate.takeGroupId.isEmpty() && ungroupedCandidate < 0) ungroupedCandidate = i;
        }
        if (candidateIndex < 0) candidateIndex = ungroupedCandidate;
        if (candidateIndex < 0) return false;
        pushUndoState();
        QString group = m_midiClips[candidateIndex].takeGroupId;
        if (group.isEmpty()) {
            group = stableClipId();
            m_midiClips[candidateIndex].takeGroupId = group;
            m_midiClips[candidateIndex].takeLane = 0;
        }
        int maxLane = 0;
        for (MidiClip& clip : m_midiClips) {
            if (clip.takeGroupId != group) continue;
            maxLane = qMax(maxLane, clip.takeLane);
            clip.takeActive = false;
        }
        selected.takeGroupId = group;
        selected.takeLane = maxLane + 1;
        selected.takeActive = true;
    }
    updateSceneBounds();
    viewport()->update();
    emit contentChanged();
    return true;
}

void PlaylistView::setSelectedClipTakeState(const QString& takeGroupId, int takeLane, bool active)
{
    const QString group = takeGroupId.trimmed().left(128);
    if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
        AudioClip& clip = m_audioClips[m_selectedClipIndex];
        clip.takeGroupId = group;
        clip.takeLane = group.isEmpty() ? 0 : qBound(0, takeLane, 127);
        clip.takeActive = group.isEmpty() || active;
    } else if (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
               m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
        MidiClip& clip = m_midiClips[m_selectedClipIndex];
        clip.takeGroupId = group;
        clip.takeLane = group.isEmpty() ? 0 : qBound(0, takeLane, 127);
        clip.takeActive = group.isEmpty() || active;
    }
}

void PlaylistView::normalizeTakeGroups()
{
    auto normalize = [](auto& clips) {
        std::map<std::tuple<QString, double, double>, std::vector<size_t>> segments;
        for (size_t i = 0; i < clips.size(); ++i) {
            clips[i].takeGroupId = clips[i].takeGroupId.trimmed().left(128);
            clips[i].takeLane = clips[i].takeGroupId.isEmpty()
                ? 0 : qBound(0, clips[i].takeLane, 127);
            if (clips[i].takeGroupId.isEmpty()) clips[i].takeActive = true;
            else segments[{clips[i].takeGroupId, clips[i].startBeat,
                           clips[i].lengthBeats}].push_back(i);
        }
        for (auto& entry : segments) {
            size_t activeIndex = entry.second.front();
            bool foundActive = false;
            for (size_t index : entry.second) {
                if (!clips[index].takeActive) continue;
                if (!foundActive || clips[index].takeLane < clips[activeIndex].takeLane) {
                    activeIndex = index;
                    foundActive = true;
                }
            }
            for (size_t index : entry.second) clips[index].takeActive = index == activeIndex;
        }
    };
    normalize(m_midiClips);
    normalize(m_audioClips);
    viewport()->update();
}

void PlaylistView::refreshAudioTakeCompSeams(const QString& takeGroupId)
{
    if (takeGroupId.isEmpty()) return;
    std::vector<int> activeIndices;
    activeIndices.reserve(m_audioClips.size());
    for (int index = 0; index < static_cast<int>(m_audioClips.size()); ++index) {
        const AudioClip& clip = m_audioClips[index];
        if (clip.takeGroupId == takeGroupId && clip.takeActive)
            activeIndices.push_back(index);
    }
    std::stable_sort(activeIndices.begin(), activeIndices.end(), [this](int left, int right) {
        const AudioClip& leftClip = m_audioClips[left];
        const AudioClip& rightClip = m_audioClips[right];
        if (leftClip.trackIndex != rightClip.trackIndex)
            return leftClip.trackIndex < rightClip.trackIndex;
        if (leftClip.startBeat != rightClip.startBeat)
            return leftClip.startBeat < rightClip.startBeat;
        return leftClip.takeLane < rightClip.takeLane;
    });

    constexpr double seamTolerance = 1.0e-6;
    constexpr double maximumSeamFadeBeats = 1.0 / 128.0;
    for (size_t position = 1; position < activeIndices.size(); ++position) {
        AudioClip& left = m_audioClips[activeIndices[position - 1]];
        AudioClip& right = m_audioClips[activeIndices[position]];
        const double leftEnd = left.startBeat + left.lengthBeats;
        if (left.trackIndex != right.trackIndex ||
            std::fabs(leftEnd - right.startBeat) > seamTolerance) continue;
        const double seamFade = qMin(maximumSeamFadeBeats,
            qMin(left.lengthBeats * 0.25, right.lengthBeats * 0.25));
        if (!std::isfinite(seamFade) || seamFade <= seamTolerance) continue;

        if (left.takeLane == right.takeLane) {
            const bool generatedSeam = std::fabs(left.fadeOutBeats - seamFade) <= seamTolerance &&
                std::fabs(right.fadeInBeats - seamFade) <= seamTolerance &&
                qFuzzyIsNull(left.fadeOutCurve) && qFuzzyIsNull(right.fadeInCurve);
            if (generatedSeam) {
                left.fadeOutBeats = 0.0;
                right.fadeInBeats = 0.0;
            }
            continue;
        }

        left.fadeOutBeats = qMax(left.fadeOutBeats, seamFade);
        right.fadeInBeats = qMax(right.fadeInBeats, seamFade);
        left.fadeOutCurve = 0.0;
        right.fadeInCurve = 0.0;
        clampAudioClipFades(left);
        clampAudioClipFades(right);
    }
}

bool PlaylistView::setSelectedClipsLocked(bool locked)
{
    bool hasSelected = false;
    bool changed = false;
    for (const MidiClip& clip : m_midiClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        changed = changed || clip.locked != locked;
    }
    for (const AudioClip& clip : m_audioClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        changed = changed || clip.locked != locked;
    }
    if (!hasSelected || !changed) return false;

    pushUndoState();
    for (MidiClip& clip : m_midiClips)
        if (isClipSelected(clip.clipId)) clip.locked = locked;
    for (AudioClip& clip : m_audioClips)
        if (isClipSelected(clip.clipId)) clip.locked = locked;
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::toggleSelectedClipsLocked()
{
    bool hasSelected = false;
    bool anyUnlocked = false;
    for (const MidiClip& clip : m_midiClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        anyUnlocked = anyUnlocked || !clip.locked;
    }
    for (const AudioClip& clip : m_audioClips) {
        if (!isClipSelected(clip.clipId)) continue;
        hasSelected = true;
        anyUnlocked = anyUnlocked || !clip.locked;
    }
    return hasSelected && setSelectedClipsLocked(anyUnlocked);
}

bool PlaylistView::selectedAudioCrossfadePair(int* leftIndex, int* rightIndex,
                                              double* overlapBeats) const
{
    int first = -1;
    int second = -1;
    for (const MidiClip& clip : m_midiClips)
        if (isClipSelected(clip.clipId)) return false;
    for (int index = 0; index < static_cast<int>(m_audioClips.size()); ++index) {
        if (!isClipSelected(m_audioClips[index].clipId)) continue;
        if (first < 0) first = index;
        else if (second < 0) second = index;
        else return false;
    }
    if (first < 0 || second < 0) return false;
    if (m_audioClips[second].startBeat < m_audioClips[first].startBeat)
        std::swap(first, second);
    const AudioClip& left = m_audioClips[first];
    const AudioClip& right = m_audioClips[second];
    const double leftEnd = left.startBeat + left.lengthBeats;
    const double rightEnd = right.startBeat + right.lengthBeats;
    constexpr double minimumCrossfade = 1.0 / 960.0;
    if (left.trackIndex != right.trackIndex ||
        right.startBeat <= left.startBeat + minimumCrossfade ||
        leftEnd <= right.startBeat + minimumCrossfade ||
        rightEnd <= leftEnd + minimumCrossfade) return false;
    const double overlap = leftEnd - right.startBeat;
    if (!std::isfinite(overlap) || overlap <= minimumCrossfade ||
        overlap > left.lengthBeats || overlap > right.lengthBeats) return false;
    if (leftIndex) *leftIndex = first;
    if (rightIndex) *rightIndex = second;
    if (overlapBeats) *overlapBeats = overlap;
    return true;
}

bool PlaylistView::createCrossfadeForSelectedAudioClips()
{
    int leftIndex = -1;
    int rightIndex = -1;
    double overlap = 0.0;
    if (selectionContainsLockedClip() ||
        !selectedAudioCrossfadePair(&leftIndex, &rightIndex, &overlap)) return false;
    pushUndoState();
    AudioClip& left = m_audioClips[leftIndex];
    AudioClip& right = m_audioClips[rightIndex];
    left.fadeInBeats = qMin(left.fadeInBeats, qMax(0.0, left.lengthBeats - overlap));
    right.fadeOutBeats = qMin(right.fadeOutBeats, qMax(0.0, right.lengthBeats - overlap));
    left.fadeOutBeats = overlap;
    right.fadeInBeats = overlap;
    left.fadeOutCurve = 0.0;
    right.fadeInCurve = 0.0;
    clampAudioClipFades(left);
    clampAudioClipFades(right);
    if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size()))
        selectAudioClip(m_selectedClipIndex, true);
    viewport()->update();
    emit contentChanged();
    return true;
}

bool PlaylistView::applyAutoCrossfadesForSelectedAudioClips()
{
    if (!m_autoCrossfadeEnabled) return false;
    struct CrossfadePair { int left; int right; double overlap; };
    std::vector<CrossfadePair> pairs;
    std::vector<int> orderedIndices(m_audioClips.size());
    std::iota(orderedIndices.begin(), orderedIndices.end(), 0);
    std::sort(orderedIndices.begin(), orderedIndices.end(), [this](int left, int right) {
        const AudioClip& leftClip = m_audioClips[left];
        const AudioClip& rightClip = m_audioClips[right];
        if (leftClip.trackIndex != rightClip.trackIndex)
            return leftClip.trackIndex < rightClip.trackIndex;
        if (leftClip.startBeat != rightClip.startBeat)
            return leftClip.startBeat < rightClip.startBeat;
        return leftClip.clipId < rightClip.clipId;
    });
    constexpr double minimumCrossfade = 1.0 / 960.0;
    for (size_t first = 0; first < orderedIndices.size(); ++first) {
        const int leftIndex = orderedIndices[first];
        const AudioClip& orderedLeft = m_audioClips[leftIndex];
        const double orderedLeftEnd = orderedLeft.startBeat + orderedLeft.lengthBeats;
        for (size_t second = first + 1; second < orderedIndices.size(); ++second) {
            const int rightIndex = orderedIndices[second];
            const AudioClip& orderedRight = m_audioClips[rightIndex];
            if (orderedRight.trackIndex != orderedLeft.trackIndex) break;
            if (orderedRight.startBeat >= orderedLeftEnd - minimumCrossfade) break;
            if (!isClipSelected(orderedLeft.clipId) &&
                !isClipSelected(orderedRight.clipId)) continue;
            const AudioClip& left = m_audioClips[leftIndex];
            const AudioClip& right = m_audioClips[rightIndex];
            if (left.locked || right.locked) continue;
            const double leftEnd = left.startBeat + left.lengthBeats;
            const double rightEnd = right.startBeat + right.lengthBeats;
            if (left.trackIndex != right.trackIndex ||
                right.startBeat <= left.startBeat + minimumCrossfade ||
                leftEnd <= right.startBeat + minimumCrossfade ||
                rightEnd <= leftEnd + minimumCrossfade) continue;
            const double overlap = leftEnd - right.startBeat;
            if (!std::isfinite(overlap) || overlap <= minimumCrossfade ||
                overlap > left.lengthBeats || overlap > right.lengthBeats) continue;
            pairs.push_back({leftIndex, rightIndex, overlap});
        }
    }
    bool changed = false;
    for (const CrossfadePair& pair : pairs) {
        AudioClip& left = m_audioClips[pair.left];
        AudioClip& right = m_audioClips[pair.right];
        changed = changed || !qFuzzyCompare(left.fadeOutBeats + 1.0, pair.overlap + 1.0) ||
            !qFuzzyCompare(right.fadeInBeats + 1.0, pair.overlap + 1.0) ||
            !qFuzzyIsNull(left.fadeOutCurve) || !qFuzzyIsNull(right.fadeInCurve);
        left.fadeInBeats = qMin(left.fadeInBeats,
            qMax(0.0, left.lengthBeats - pair.overlap));
        right.fadeOutBeats = qMin(right.fadeOutBeats,
            qMax(0.0, right.lengthBeats - pair.overlap));
        left.fadeOutBeats = pair.overlap;
        right.fadeInBeats = pair.overlap;
        left.fadeOutCurve = 0.0;
        right.fadeInCurve = 0.0;
        clampAudioClipFades(left);
        clampAudioClipFades(right);
    }
    return changed;
}

bool PlaylistView::insertTimeSelection()
{
    if (!m_timeSelectionEnabled) return false;
    const double insertBeat = m_timeSelectionStartBeat;
    const double duration = m_timeSelectionEndBeat - m_timeSelectionStartBeat;
    if (!std::isfinite(insertBeat) || !std::isfinite(duration) || duration < 0.25) return false;
    for (const MidiClip& clip : m_midiClips)
        if (clip.locked && clip.startBeat + clip.lengthBeats > insertBeat) return false;
    for (const AudioClip& clip : m_audioClips)
        if (clip.locked && clip.startBeat + clip.lengthBeats > insertBeat) return false;
    pushUndoState();

    QString primaryId;
    const bool primaryAudio = m_selectedClipIsAudio;
    if (primaryAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size()))
        primaryId = m_audioClips[m_selectedClipIndex].clipId;
    else if (!primaryAudio && m_selectedClipIndex >= 0 &&
             m_selectedClipIndex < static_cast<int>(m_midiClips.size()))
        primaryId = m_midiClips[m_selectedClipIndex].clipId;

    std::vector<MidiClip> addedMidi;
    std::vector<AudioClip> addedAudio;
    for (MidiClip& clip : m_midiClips) {
        const double clipEnd = clip.startBeat + clip.lengthBeats;
        if (clipEnd <= insertBeat) continue;
        if (clip.startBeat >= insertBeat) {
            clip.startBeat += duration;
            continue;
        }
        const double leftLength = insertBeat - clip.startBeat;
        const double rightLength = clipEnd - insertBeat;
        if (leftLength >= 0.25 && rightLength >= 0.25) {
            MidiClip right = clip;
            right.clipId = stableClipId();
            right.startBeat = insertBeat + duration;
            right.lengthBeats = rightLength;
            advanceMidiClipSourceOffset(right, leftLength);
            clip.lengthBeats = leftLength;
            if (isClipSelected(clip.clipId)) m_selectedClipIds.insert(right.clipId);
            addedMidi.push_back(std::move(right));
        } else if (rightLength >= 0.25) {
            clip.startBeat = insertBeat + duration;
            clip.lengthBeats = rightLength;
            advanceMidiClipSourceOffset(clip, leftLength);
        } else if (leftLength >= 0.25) {
            clip.lengthBeats = qMax(0.25, leftLength);
        } else {
            clip.startBeat += duration;
        }
    }
    for (AudioClip& clip : m_audioClips) {
        const double clipEnd = clip.startBeat + clip.lengthBeats;
        if (clipEnd <= insertBeat) continue;
        if (clip.startBeat >= insertBeat) {
            clip.startBeat += duration;
            continue;
        }
        const double leftLength = insertBeat - clip.startBeat;
        const double rightLength = clipEnd - insertBeat;
        if (leftLength >= 0.25 && rightLength >= 0.25) {
            AudioClip right = clip;
            right.clipId = stableClipId();
            right.startBeat = insertBeat + duration;
            right.lengthBeats = rightLength;
            advanceAudioClipSourceOffset(right, leftLength);
            right.fadeInBeats = 0.0;
            clip.lengthBeats = leftLength;
            clip.fadeOutBeats = 0.0;
            clampAudioClipFades(clip);
            clampAudioClipFades(right);
            if (isClipSelected(clip.clipId)) m_selectedClipIds.insert(right.clipId);
            addedAudio.push_back(std::move(right));
        } else if (rightLength >= 0.25) {
            clip.startBeat = insertBeat + duration;
            clip.lengthBeats = rightLength;
            advanceAudioClipSourceOffset(clip, leftLength);
            clip.fadeInBeats = 0.0;
            clampAudioClipFades(clip);
        } else if (leftLength >= 0.25) {
            clip.lengthBeats = qMax(0.25, leftLength);
            clip.fadeOutBeats = 0.0;
            clampAudioClipFades(clip);
        } else {
            clip.startBeat += duration;
        }
    }
    m_midiClips.insert(m_midiClips.end(),
        std::make_move_iterator(addedMidi.begin()), std::make_move_iterator(addedMidi.end()));
    m_audioClips.insert(m_audioClips.end(),
        std::make_move_iterator(addedAudio.begin()), std::make_move_iterator(addedAudio.end()));
    rebuildContentReferences();
    updateSceneBounds();
    restorePrimaryClipSelection(primaryId, primaryAudio);
    emit contentChanged();
    return true;
}

bool PlaylistView::deleteTimeSelection()
{
    if (!m_timeSelectionEnabled) return false;
    const double rangeStart = m_timeSelectionStartBeat;
    const double rangeEnd = m_timeSelectionEndBeat;
    const double duration = rangeEnd - rangeStart;
    if (!std::isfinite(rangeStart) || !std::isfinite(rangeEnd) || duration < 0.25) return false;
    for (const MidiClip& clip : m_midiClips)
        if (clip.locked && clip.startBeat + clip.lengthBeats > rangeStart) return false;
    for (const AudioClip& clip : m_audioClips)
        if (clip.locked && clip.startBeat + clip.lengthBeats > rangeStart) return false;
    pushUndoState();

    QString primaryId;
    const bool primaryAudio = m_selectedClipIsAudio;
    if (primaryAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size()))
        primaryId = m_audioClips[m_selectedClipIndex].clipId;
    else if (!primaryAudio && m_selectedClipIndex >= 0 &&
             m_selectedClipIndex < static_cast<int>(m_midiClips.size()))
        primaryId = m_midiClips[m_selectedClipIndex].clipId;

    struct RemovedClip {
        QString clipId;
        QString contentId;
        bool audio;
    };
    std::vector<RemovedClip> removed;
    std::vector<MidiClip> nextMidi;
    std::vector<AudioClip> nextAudio;
    nextMidi.reserve(m_midiClips.size() + 4);
    nextAudio.reserve(m_audioClips.size() + 4);

    for (const MidiClip& source : m_midiClips) {
        MidiClip clip = source;
        const double clipEnd = clip.startBeat + clip.lengthBeats;
        if (clipEnd <= rangeStart) {
            nextMidi.push_back(std::move(clip));
        } else if (clip.startBeat >= rangeEnd) {
            clip.startBeat -= duration;
            nextMidi.push_back(std::move(clip));
        } else if (clip.startBeat >= rangeStart && clipEnd <= rangeEnd) {
            removed.push_back({clip.clipId, clip.contentId, false});
        } else if (clip.startBeat < rangeStart && clipEnd > rangeEnd) {
            const double leftLength = rangeStart - clip.startBeat;
            const double rightLength = clipEnd - rangeEnd;
            if (leftLength >= 0.25) {
                MidiClip right = clip;
                clip.lengthBeats = leftLength;
                nextMidi.push_back(std::move(clip));
                if (rightLength >= 0.25) {
                    right.clipId = stableClipId();
                    right.startBeat = rangeStart;
                    right.lengthBeats = rightLength;
                    advanceMidiClipSourceOffset(right, rangeEnd - source.startBeat);
                    if (isClipSelected(source.clipId)) m_selectedClipIds.insert(right.clipId);
                    nextMidi.push_back(std::move(right));
                }
            } else if (rightLength >= 0.25) {
                clip.startBeat = rangeStart;
                clip.lengthBeats = rightLength;
                advanceMidiClipSourceOffset(clip, rangeEnd - source.startBeat);
                nextMidi.push_back(std::move(clip));
            } else {
                removed.push_back({clip.clipId, clip.contentId, false});
            }
        } else if (clip.startBeat < rangeStart) {
            const double leftLength = rangeStart - clip.startBeat;
            if (leftLength >= 0.25) {
                clip.lengthBeats = leftLength;
                nextMidi.push_back(std::move(clip));
            } else {
                removed.push_back({clip.clipId, clip.contentId, false});
            }
        } else {
            const double rightLength = clipEnd - rangeEnd;
            if (rightLength >= 0.25) {
                clip.startBeat = rangeStart;
                clip.lengthBeats = rightLength;
                advanceMidiClipSourceOffset(clip, rangeEnd - source.startBeat);
                nextMidi.push_back(std::move(clip));
            } else {
                removed.push_back({clip.clipId, clip.contentId, false});
            }
        }
    }

    for (const AudioClip& source : m_audioClips) {
        AudioClip clip = source;
        const double clipEnd = clip.startBeat + clip.lengthBeats;
        if (clipEnd <= rangeStart) {
            nextAudio.push_back(std::move(clip));
        } else if (clip.startBeat >= rangeEnd) {
            clip.startBeat -= duration;
            nextAudio.push_back(std::move(clip));
        } else if (clip.startBeat >= rangeStart && clipEnd <= rangeEnd) {
            removed.push_back({clip.clipId, clip.contentId, true});
        } else if (clip.startBeat < rangeStart && clipEnd > rangeEnd) {
            const double leftLength = rangeStart - clip.startBeat;
            const double rightLength = clipEnd - rangeEnd;
            if (leftLength >= 0.25) {
                AudioClip right = clip;
                clip.lengthBeats = leftLength;
                clip.fadeOutBeats = 0.0;
                clampAudioClipFades(clip);
                nextAudio.push_back(std::move(clip));
                if (rightLength >= 0.25) {
                    right.clipId = stableClipId();
                    right.startBeat = rangeStart;
                    right.lengthBeats = rightLength;
                    advanceAudioClipSourceOffset(right, rangeEnd - source.startBeat);
                    right.fadeInBeats = 0.0;
                    clampAudioClipFades(right);
                    if (isClipSelected(source.clipId)) m_selectedClipIds.insert(right.clipId);
                    nextAudio.push_back(std::move(right));
                }
            } else if (rightLength >= 0.25) {
                clip.startBeat = rangeStart;
                clip.lengthBeats = rightLength;
                advanceAudioClipSourceOffset(clip, rangeEnd - source.startBeat);
                clip.fadeInBeats = 0.0;
                clampAudioClipFades(clip);
                nextAudio.push_back(std::move(clip));
            } else {
                removed.push_back({clip.clipId, clip.contentId, true});
            }
        } else if (clip.startBeat < rangeStart) {
            const double leftLength = rangeStart - clip.startBeat;
            if (leftLength >= 0.25) {
                clip.lengthBeats = leftLength;
                clip.fadeOutBeats = 0.0;
                clampAudioClipFades(clip);
                nextAudio.push_back(std::move(clip));
            } else {
                removed.push_back({clip.clipId, clip.contentId, true});
            }
        } else {
            const double rightLength = clipEnd - rangeEnd;
            if (rightLength >= 0.25) {
                clip.startBeat = rangeStart;
                clip.lengthBeats = rightLength;
                advanceAudioClipSourceOffset(clip, rangeEnd - source.startBeat);
                clip.fadeInBeats = 0.0;
                clampAudioClipFades(clip);
                nextAudio.push_back(std::move(clip));
            } else {
                removed.push_back({clip.clipId, clip.contentId, true});
            }
        }
    }

    for (const RemovedClip& clip : removed) m_selectedClipIds.erase(clip.clipId);
    m_midiClips = std::move(nextMidi);
    m_audioClips = std::move(nextAudio);
    rebuildContentReferences();
    setTimeSelection(false, 0.0, 0.0);
    updateSceneBounds();
    restorePrimaryClipSelection(primaryId, primaryAudio);
    for (const RemovedClip& clip : removed)
        emit selectedClipDeleted(clip.clipId, clip.contentId, clip.audio);
    emit timeSelectionChanged(false, 0.0, 0.0);
    emit contentChanged();
    return true;
}

bool PlaylistView::splitSelectedClipAtPlayhead()
{
    if (m_selectedClipIndex < 0 || !std::isfinite(m_playheadBeat) ||
        selectionContainsLockedClip()) return false;
    if (m_selectedClipIsAudio) {
        if (m_selectedClipIndex >= static_cast<int>(m_audioClips.size())) return false;
        AudioClip& left = m_audioClips[m_selectedClipIndex];
        const double leftLength = m_playheadBeat - left.startBeat;
        const double rightLength = left.startBeat + left.lengthBeats - m_playheadBeat;
        if (leftLength < 0.25 || rightLength < 0.25) return false;
        pushUndoState();
        AudioClip right = left;
        right.clipId = stableClipId();
        right.startBeat = m_playheadBeat;
        right.lengthBeats = rightLength;
        advanceAudioClipSourceOffset(right, leftLength);
        right.fadeInBeats = 0.0;
        left.fadeOutBeats = 0.0;
        left.lengthBeats = leftLength;
        clampAudioClipFades(left);
        clampAudioClipFades(right);
        m_audioClips.push_back(std::move(right));
        addContentReference(m_audioClips.back().contentId, true);
        updateSceneBounds();
        selectAudioClip(static_cast<int>(m_audioClips.size()) - 1);
        emit contentChanged();
        return true;
    }
    if (m_selectedClipIndex >= static_cast<int>(m_midiClips.size())) return false;
    MidiClip& left = m_midiClips[m_selectedClipIndex];
    const double leftLength = m_playheadBeat - left.startBeat;
    const double rightLength = left.startBeat + left.lengthBeats - m_playheadBeat;
    if (leftLength < 0.25 || rightLength < 0.25) return false;
    pushUndoState();
    MidiClip right = left;
    right.clipId = stableClipId();
    right.startBeat = m_playheadBeat;
    right.lengthBeats = rightLength;
    advanceMidiClipSourceOffset(right, leftLength);
    left.lengthBeats = leftLength;
    m_midiClips.push_back(std::move(right));
    addContentReference(m_midiClips.back().contentId, false);
    updateSceneBounds();
    selectClip(static_cast<int>(m_midiClips.size()) - 1, false);
    emit contentChanged();
    return true;
}

bool PlaylistView::deleteSelectedClip()
{
    if (m_selectedClipIds.empty() || selectionContainsLockedClip()) return false;
    pushUndoState();

    struct RemovedClip {
        QString clipId;
        QString contentId;
        bool audio = false;
    };
    std::vector<RemovedClip> removed;
    m_audioClips.erase(std::remove_if(m_audioClips.begin(), m_audioClips.end(),
        [&](const AudioClip& clip) {
            if (!isClipSelected(clip.clipId)) return false;
            removed.push_back({clip.clipId, clip.contentId, true});
            return true;
        }), m_audioClips.end());
    m_midiClips.erase(std::remove_if(m_midiClips.begin(), m_midiClips.end(),
        [&](const MidiClip& clip) {
            if (!isClipSelected(clip.clipId)) return false;
            removed.push_back({clip.clipId, clip.contentId, false});
            return true;
        }), m_midiClips.end());
    if (removed.empty()) return false;

    rebuildContentReferences();
    normalizeTakeGroups();

    m_selectedClipIndex = -1;
    m_selectedClipIsAudio = false;
    m_selectedClipIds.clear();
    clearClipDrag();
    updateSceneBounds();
    viewport()->update();
    emit clipSelectionChanged();
    for (const RemovedClip& clip : removed)
        emit selectedClipDeleted(clip.clipId, clip.contentId, clip.audio);
    emit contentChanged();
    return true;
}

bool PlaylistView::makeSelectedAudioClipUnique(const QString& name, const QString& sourcePath)
{
    const QString safeName = name.trimmed();
    const QString safePath = sourcePath.trimmed();
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size()) ||
        m_audioClips[m_selectedClipIndex].locked ||
        safeName.isEmpty() || safePath.isEmpty()) {
        return false;
    }

    pushUndoState();
    AudioClip& clip = m_audioClips[m_selectedClipIndex];
    const QString oldContentId = clip.contentId;
    clip.name = safeName;
    clip.sourcePath = safePath;
    clip.mediaOffline = !AudioMediaResolver::isOnline(safePath);
    clip.contentId = stableClipId();
    removeContentReference(oldContentId, true);
    clip.isUnique = true;
    m_audioContentRefs[clip.contentId] = 1;
    m_firstAudioContentIndex[clip.contentId] = m_selectedClipIndex;
    selectAudioClip(m_selectedClipIndex);
    updateSceneBounds();
    viewport()->update();
    emit contentChanged();
    return true;
}

QString PlaylistView::makeSelectedMidiClipUnique(const QString& name)
{
    const QString safeName = name.trimmed();
    if (m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_midiClips.size()) ||
        m_midiClips[m_selectedClipIndex].locked || safeName.isEmpty()) {
        return {};
    }
    pushUndoState();
    MidiClip& clip = m_midiClips[m_selectedClipIndex];
    const QString oldContentId = clip.contentId;
    clip.name = safeName;
    clip.contentId = stableClipId();
    removeContentReference(oldContentId, false);
    clip.isUnique = true;
    m_midiContentRefs[clip.contentId] = 1;
    m_firstMidiContentIndex[clip.contentId] = m_selectedClipIndex;
    selectClip(m_selectedClipIndex, false);
    updateSceneBounds();
    viewport()->update();
    emit contentChanged();
    return clip.contentId;
}

int PlaylistView::contentReferenceCount(const QString& contentId, bool audio) const
{
    if (contentId.isEmpty()) return 0;
    const auto& refs = audio ? m_audioContentRefs : m_midiContentRefs;
    const auto it = refs.find(contentId);
    return it == refs.end() ? 0 : it->second;
}

int PlaylistView::selectedClipReferenceCount() const
{
    if (m_selectedClipIndex < 0) return 0;
    if (m_selectedClipIsAudio) {
        return m_selectedClipIndex < static_cast<int>(m_audioClips.size())
            ? contentReferenceCount(m_audioClips[m_selectedClipIndex].contentId, true) : 0;
    }
    return m_selectedClipIndex < static_cast<int>(m_midiClips.size())
        ? contentReferenceCount(m_midiClips[m_selectedClipIndex].contentId, false) : 0;
}

void PlaylistView::addContentReference(const QString& contentId, bool audio)
{
    auto& refs = audio ? m_audioContentRefs : m_midiContentRefs;
    auto& firstIndexes = audio ? m_firstAudioContentIndex : m_firstMidiContentIndex;
    const int count = ++refs[contentId];
    if (audio) {
        const int newIndex = static_cast<int>(m_audioClips.size()) - 1;
        if (count == 1) firstIndexes[contentId] = newIndex;
        if (count == 2) m_audioClips[firstIndexes[contentId]].isUnique = false;
        m_audioClips[newIndex].isUnique = count <= 1;
    } else {
        const int newIndex = static_cast<int>(m_midiClips.size()) - 1;
        if (count == 1) firstIndexes[contentId] = newIndex;
        if (count == 2) m_midiClips[firstIndexes[contentId]].isUnique = false;
        m_midiClips[newIndex].isUnique = count <= 1;
    }
}

void PlaylistView::removeContentReference(const QString& contentId, bool audio)
{
    auto& refs = audio ? m_audioContentRefs : m_midiContentRefs;
    auto& firstIndexes = audio ? m_firstAudioContentIndex : m_firstMidiContentIndex;
    auto it = refs.find(contentId);
    if (it == refs.end()) return;
    const int count = --it->second;
    if (count <= 0) {
        refs.erase(it);
        firstIndexes.erase(contentId);
    }
    if (audio) {
        for (int i = 0; i < static_cast<int>(m_audioClips.size()); ++i) {
            AudioClip& clip = m_audioClips[i];
            if (clip.contentId != contentId) continue;
            clip.isUnique = count <= 1;
            if (count == 1) firstIndexes[contentId] = i;
        }
    } else {
        for (int i = 0; i < static_cast<int>(m_midiClips.size()); ++i) {
            MidiClip& clip = m_midiClips[i];
            if (clip.contentId != contentId) continue;
            clip.isUnique = count <= 1;
            if (count == 1) firstIndexes[contentId] = i;
        }
    }
}

void PlaylistView::rebuildContentReferences()
{
    m_midiContentRefs.clear();
    m_audioContentRefs.clear();
    m_firstMidiContentIndex.clear();
    m_firstAudioContentIndex.clear();

    for (int i = 0; i < static_cast<int>(m_midiClips.size()); ++i) {
        const QString& id = m_midiClips[i].contentId;
        if (m_midiContentRefs[id]++ == 0) m_firstMidiContentIndex[id] = i;
    }
    for (int i = 0; i < static_cast<int>(m_audioClips.size()); ++i) {
        const QString& id = m_audioClips[i].contentId;
        if (m_audioContentRefs[id]++ == 0) m_firstAudioContentIndex[id] = i;
    }
    for (MidiClip& clip : m_midiClips) clip.isUnique = m_midiContentRefs[clip.contentId] == 1;
    for (AudioClip& clip : m_audioClips) clip.isUnique = m_audioContentRefs[clip.contentId] == 1;
}

void PlaylistView::captureSelectedClipDragState()
{
    m_draggedMidiClipStates.clear();
    m_draggedAudioClipStates.clear();
    for (const MidiClip& clip : m_midiClips) {
        if (!isClipSelected(clip.clipId)) continue;
        m_draggedMidiClipStates.emplace(clip.clipId, DraggedMidiClipState{
            clip.trackIndex, clip.startBeat, clip.lengthBeats, clip.sourceOffsetBeats});
    }
    for (const AudioClip& clip : m_audioClips) {
        if (!isClipSelected(clip.clipId)) continue;
        m_draggedAudioClipStates.emplace(clip.clipId, DraggedAudioClipState{
            clip.trackIndex, clip.startBeat, clip.lengthBeats, clip.sourceOffsetBeats,
            clip.warpLengthBeats, clip.fadeInBeats, clip.fadeOutBeats, clip.syncMode,
            clip.loopEnabled});
    }
}

bool PlaylistView::moveSelectedClipsFromDrag(const QPointF& scenePos,
                                             Qt::KeyboardModifiers modifiers)
{
    if (selectionContainsLockedClip()) return false;
    int primaryTrack = -1;
    double primaryStart = 0.0;
    const bool primaryAudio = m_selectedClipIsAudio;
    if (primaryAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
        const auto state = m_draggedAudioClipStates.find(m_audioClips[m_selectedClipIndex].clipId);
        if (state == m_draggedAudioClipStates.end()) return false;
        primaryTrack = state->second.trackIndex;
        primaryStart = state->second.startBeat;
    } else if (!primaryAudio && m_selectedClipIndex >= 0 &&
               m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
        const auto state = m_draggedMidiClipStates.find(m_midiClips[m_selectedClipIndex].clipId);
        if (state == m_draggedMidiClipStates.end()) return false;
        primaryTrack = state->second.trackIndex;
        primaryStart = state->second.startBeat;
    } else {
        return false;
    }

    const double desiredPrimaryStart = snapBeatUnlessAlt(
        beatAtX(scenePos.x()) - m_clipGrabOffsetBeat, modifiers);
    double earliestStart = primaryStart;
    for (const auto& entry : m_draggedMidiClipStates)
        earliestStart = qMin(earliestStart, entry.second.startBeat);
    for (const auto& entry : m_draggedAudioClipStates)
        earliestStart = qMin(earliestStart, entry.second.startBeat);
    const double beatDelta = qMax(desiredPrimaryStart - primaryStart, -earliestStart);

    int trackDelta = 0;
    const int pointerTrack = trackIndexAt(scenePos);
    const bool pointerMatchesPrimary = primaryAudio
        ? isAudioTrack(pointerTrack) : isMidiTrack(pointerTrack);
    if (pointerMatchesPrimary) {
        const int candidate = pointerTrack - primaryTrack;
        bool valid = true;
        for (const auto& entry : m_draggedMidiClipStates)
            valid = valid && isMidiTrack(entry.second.trackIndex + candidate);
        for (const auto& entry : m_draggedAudioClipStates)
            valid = valid && isAudioTrack(entry.second.trackIndex + candidate);
        if (valid) trackDelta = candidate;
    }

    for (MidiClip& clip : m_midiClips) {
        const auto state = m_draggedMidiClipStates.find(clip.clipId);
        if (state == m_draggedMidiClipStates.end()) continue;
        clip.startBeat = state->second.startBeat + beatDelta;
        clip.trackIndex = state->second.trackIndex + trackDelta;
    }
    for (AudioClip& clip : m_audioClips) {
        const auto state = m_draggedAudioClipStates.find(clip.clipId);
        if (state == m_draggedAudioClipStates.end()) continue;
        clip.startBeat = state->second.startBeat + beatDelta;
        clip.trackIndex = state->second.trackIndex + trackDelta;
    }
    return std::fabs(beatDelta) > 1.0e-9 || trackDelta != 0;
}

bool PlaylistView::resizeSelectedClipsFromDrag(double pointerBeat)
{
    if (selectionContainsLockedClip()) return false;
    double primaryStart = 0.0;
    double primaryLength = 0.0;
    if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
        const auto state = m_draggedAudioClipStates.find(m_audioClips[m_selectedClipIndex].clipId);
        if (state == m_draggedAudioClipStates.end()) return false;
        primaryStart = state->second.startBeat;
        primaryLength = state->second.lengthBeats;
    } else if (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
               m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
        const auto state = m_draggedMidiClipStates.find(m_midiClips[m_selectedClipIndex].clipId);
        if (state == m_draggedMidiClipStates.end()) return false;
        primaryStart = state->second.startBeat;
        primaryLength = state->second.lengthBeats;
    } else {
        return false;
    }

    double minimumDelta = -std::numeric_limits<double>::infinity();
    double maximumDelta = std::numeric_limits<double>::infinity();
    if (m_resizingExistingClipFromLeft) {
        for (const auto& entry : m_draggedMidiClipStates) {
            const DraggedMidiClipState& state = entry.second;
            minimumDelta = qMax(minimumDelta, -qMin(state.startBeat, state.sourceOffsetBeats));
            maximumDelta = qMin(maximumDelta, state.lengthBeats - 0.25);
        }
        for (const auto& entry : m_draggedAudioClipStates) {
            const DraggedAudioClipState& state = entry.second;
            minimumDelta = qMax(minimumDelta, -qMin(state.startBeat, state.sourceOffsetBeats));
            maximumDelta = qMin(maximumDelta, state.lengthBeats - 0.25);
        }
    } else {
        for (const auto& entry : m_draggedMidiClipStates)
            minimumDelta = qMax(minimumDelta, 0.25 - entry.second.lengthBeats);
        for (const auto& entry : m_draggedAudioClipStates) {
            const DraggedAudioClipState& state = entry.second;
            minimumDelta = qMax(minimumDelta, 0.25 - state.lengthBeats);
            if (state.syncMode == 3 || state.loopEnabled) continue;
            const double fullWarp = std::isfinite(state.warpLengthBeats) && state.warpLengthBeats > 0.0
                ? state.warpLengthBeats : state.sourceOffsetBeats + state.lengthBeats;
            maximumDelta = qMin(maximumDelta,
                qMax(0.25, fullWarp - state.sourceOffsetBeats) - state.lengthBeats);
        }
    }

    const double requestedDelta = m_resizingExistingClipFromLeft
        ? pointerBeat - primaryStart
        : pointerBeat - (primaryStart + primaryLength);
    const double delta = qBound(minimumDelta, requestedDelta, maximumDelta);
    for (MidiClip& clip : m_midiClips) {
        const auto state = m_draggedMidiClipStates.find(clip.clipId);
        if (state == m_draggedMidiClipStates.end()) continue;
        clip.startBeat = state->second.startBeat;
        clip.lengthBeats = state->second.lengthBeats;
        clip.sourceOffsetBeats = state->second.sourceOffsetBeats;
        if (m_resizingExistingClipFromLeft) {
            clip.startBeat += delta;
            clip.lengthBeats -= delta;
            advanceMidiClipSourceOffset(clip, delta);
        } else {
            clip.lengthBeats += delta;
        }
    }
    for (AudioClip& clip : m_audioClips) {
        const auto state = m_draggedAudioClipStates.find(clip.clipId);
        if (state == m_draggedAudioClipStates.end()) continue;
        const DraggedAudioClipState& original = state->second;
        clip.startBeat = original.startBeat;
        clip.lengthBeats = original.lengthBeats;
        clip.sourceOffsetBeats = original.sourceOffsetBeats;
        clip.warpLengthBeats = original.warpLengthBeats;
        clip.fadeInBeats = original.fadeInBeats;
        clip.fadeOutBeats = original.fadeOutBeats;
        if (m_resizingExistingClipFromLeft) {
            clip.startBeat += delta;
            clip.lengthBeats -= delta;
            advanceAudioClipSourceOffset(clip, delta);
        } else {
            clip.lengthBeats += delta;
            if (original.syncMode == 3) {
                const double oldWarp = qMax(0.25, original.warpLengthBeats);
                const double sourceStartRatio = qBound(
                    0.0, original.sourceOffsetBeats / oldWarp, 0.999999);
                clip.warpLengthBeats = clip.lengthBeats / (1.0 - sourceStartRatio);
                clip.sourceOffsetBeats = sourceStartRatio * clip.warpLengthBeats;
            }
        }
        clampAudioClipFades(clip);
    }
    return std::fabs(delta) > 1.0e-9;
}

void PlaylistView::updateMarqueeSelection()
{
    const QRect viewRect = QRect(m_marqueeAnchorPos, m_marqueeCurrentPos).normalized();
    const QRectF sceneRect = QRectF(
        mapToScene(viewRect.topLeft()), mapToScene(viewRect.bottomRight())).normalized();
    m_selectedClipIds = m_marqueeBaseSelection;
    std::set<QString> hitGroups;
    std::set<QString> hitUngrouped;
    auto collectHit = [&sceneRect, &hitGroups, &hitUngrouped](
                          const QString& clipId, const QString& groupId,
                          const QRectF& clipBounds) {
        if (!sceneRect.intersects(clipBounds)) return;
        if (groupId.isEmpty()) hitUngrouped.insert(clipId);
        else hitGroups.insert(groupId);
    };
    for (const MidiClip& clip : m_midiClips)
        collectHit(clip.clipId, clip.groupId, clipRect(clip));
    for (const AudioClip& clip : m_audioClips)
        collectHit(clip.clipId, clip.groupId, clipRect(clip));
    for (const QString& clipId : hitUngrouped) {
        if (m_marqueeBaseSelection.find(clipId) == m_marqueeBaseSelection.end())
            m_selectedClipIds.insert(clipId);
        else
            m_selectedClipIds.erase(clipId);
    }
    std::set<QString> selectedBaseGroups;
    for (const MidiClip& clip : m_midiClips)
        if (!clip.groupId.isEmpty() &&
            m_marqueeBaseSelection.find(clip.clipId) != m_marqueeBaseSelection.end())
            selectedBaseGroups.insert(clip.groupId);
    for (const AudioClip& clip : m_audioClips)
        if (!clip.groupId.isEmpty() &&
            m_marqueeBaseSelection.find(clip.clipId) != m_marqueeBaseSelection.end())
            selectedBaseGroups.insert(clip.groupId);
    auto applyGroupToggle = [this, &hitGroups, &selectedBaseGroups](
                                const QString& clipId, const QString& groupId) {
        if (groupId.isEmpty() || hitGroups.find(groupId) == hitGroups.end()) return;
        if (selectedBaseGroups.find(groupId) != selectedBaseGroups.end())
            m_selectedClipIds.erase(clipId);
        else
            m_selectedClipIds.insert(clipId);
    };
    for (const MidiClip& clip : m_midiClips)
        applyGroupToggle(clip.clipId, clip.groupId);
    for (const AudioClip& clip : m_audioClips)
        applyGroupToggle(clip.clipId, clip.groupId);
}

bool PlaylistView::nudgeSelectedClip(double beatDelta, int trackDirection, bool bypassSnap)
{
    if (m_selectedClipIds.empty() || selectionContainsLockedClip()) return false;

    auto targetTrack = [&](int current, bool audio) {
        if (trackDirection == 0) return current;
        for (int track = current + trackDirection;
             track >= 0 && track < static_cast<int>(m_tracks.size()); track += trackDirection) {
            if (audio ? isAudioTrack(track) : isMidiTrack(track)) return track;
        }
        return current;
    };

    double earliestBeat = std::numeric_limits<double>::infinity();
    for (const MidiClip& clip : m_midiClips)
        if (isClipSelected(clip.clipId)) earliestBeat = std::min(earliestBeat, clip.startBeat);
    for (const AudioClip& clip : m_audioClips)
        if (isClipSelected(clip.clipId)) earliestBeat = std::min(earliestBeat, clip.startBeat);
    if (!std::isfinite(earliestBeat)) return false;
    double effectiveBeatDelta = beatDelta;
    if (!bypassSnap && m_snapBeats > 0.0)
        effectiveBeatDelta = snapBeat(earliestBeat + beatDelta) - earliestBeat;
    effectiveBeatDelta = std::max(effectiveBeatDelta, -earliestBeat);

    ArrangementState undoState = captureArrangementState();
    bool changed = false;
    for (AudioClip& clip : m_audioClips) {
        if (!isClipSelected(clip.clipId)) continue;
        const double nextBeat = clip.startBeat + effectiveBeatDelta;
        const int nextTrack = targetTrack(clip.trackIndex, true);
        changed = changed || nextBeat != clip.startBeat || nextTrack != clip.trackIndex;
        clip.startBeat = nextBeat;
        clip.trackIndex = nextTrack;
    }
    for (MidiClip& clip : m_midiClips) {
        if (!isClipSelected(clip.clipId)) continue;
        const double nextBeat = clip.startBeat + effectiveBeatDelta;
        const int nextTrack = targetTrack(clip.trackIndex, false);
        changed = changed || nextBeat != clip.startBeat || nextTrack != clip.trackIndex;
        clip.startBeat = nextBeat;
        clip.trackIndex = nextTrack;
    }
    if (!changed) return false;
    pushUndoState(std::move(undoState));
    updateSceneBounds();
    if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
        m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
        selectAudioClip(m_selectedClipIndex, true);
    } else if (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
               m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
        selectClip(m_selectedClipIndex, false, true);
    } else {
        selectFirstRemainingClip();
    }
    emit contentChanged();
    return true;
}

void PlaylistView::selectClipByIndex(int clipIndex, bool opened)
{
    selectClip(clipIndex, opened);
}

int PlaylistView::getSelectedClipIndex() const
{
    return m_selectedClipIndex;
}

bool PlaylistView::isClipForTrackAt(int clipIndex, int trackIndex) const
{
    if (clipIndex < 0 || clipIndex >= (int)m_midiClips.size()) {
        return false;
    }
    return m_midiClips[clipIndex].trackIndex == trackIndex;
}

void PlaylistView::setPixelsPerBeat(double ppb)
{
    m_pixelsPerBeat = qMax(10.0, qMin(ppb, 300.0));
    updateSceneBounds();
    viewport()->update();
}

double PlaylistView::getPixelsPerBeat() const
{
    return m_pixelsPerBeat;
}

void PlaylistView::setTrackCount(int count)
{
    m_trackCount = count;
    updateSceneBounds();
    viewport()->update();
}

int PlaylistView::getTrackCount() const
{
    return m_trackCount;
}

void PlaylistView::setTracks(const std::vector<std::shared_ptr<Track>>& tracks)
{
    m_tracks.clear();
    m_tracks.reserve(tracks.size());
    for (const auto& track : tracks) {
        m_tracks.push_back(track.get());
    }
    setTrackCount((int)m_tracks.size());
}

void PlaylistView::setPluginAutomationLane(int displayTrackIndex, int targetTrackIndex,
                                           bool audioFx, int slotIndex,
                                           const PluginAutomationLane& lane)
{
    if (displayTrackIndex < 0 || displayTrackIndex >= m_trackCount || slotIndex < 0) {
        clearPluginAutomationLane();
        return;
    }
    m_pluginAutomationVisible = true;
    m_pluginAutomationTrackIndex = displayTrackIndex;
    m_pluginAutomationTargetTrackIndex = targetTrackIndex;
    m_pluginAutomationAudioFx = audioFx;
    m_pluginAutomationSlotIndex = slotIndex;
    m_pluginAutomationLane = lane;
    m_pluginAutomationDragPoint = -1;
    m_pluginAutomationDragChanged = false;
    updateSceneBounds();
    viewport()->update();
}

void PlaylistView::clearPluginAutomationLane()
{
    m_pluginAutomationVisible = false;
    m_pluginAutomationTrackIndex = -1;
    m_pluginAutomationTargetTrackIndex = -1;
    m_pluginAutomationSlotIndex = -1;
    m_pluginAutomationDragPoint = -1;
    m_pluginAutomationDragChanged = false;
    m_pluginAutomationLane = {};
    viewport()->update();
}

void PlaylistView::updateSceneBounds()
{
    double minWidth = viewport()->width();
    double minHeight = qMax((int)viewport()->height(), m_trackCount * m_trackHeight + m_timelineHeight);
    
    QRectF itemsRect = scene()->itemsBoundingRect();
    double clipRight = 0.0;
    for (const MidiClip& clip : m_midiClips) {
        clipRight = qMax(clipRight, (clip.startBeat + clip.lengthBeats) * m_pixelsPerBeat + 120.0);
    }
    for (const AudioClip& clip : m_audioClips) {
        clipRight = qMax(clipRight, (clip.startBeat + clip.lengthBeats) * m_pixelsPerBeat + 120.0);
    }
    if (m_pluginAutomationVisible && !m_pluginAutomationLane.points.empty()) {
        clipRight = qMax(clipRight,
            m_pluginAutomationLane.points.back().beat * m_pixelsPerBeat + 120.0);
    }

    double finalWidth = qMax(qMax(minWidth, itemsRect.right()), clipRight);
    double finalHeight = qMax(minHeight, itemsRect.bottom());
    
    setSceneRect(0, 0, finalWidth, finalHeight);
}

void PlaylistView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    updateSceneBounds();
}

void PlaylistView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const double cursorBeat = (horizontalScrollBar()->value() + event->position().x()) / m_pixelsPerBeat;
        const double steps = event->angleDelta().y() / 120.0;
        setPixelsPerBeat(m_pixelsPerBeat * qPow(1.1, steps));
        horizontalScrollBar()->setValue(qMax(0, static_cast<int>(
            std::lround(cursorBeat * m_pixelsPerBeat - event->position().x()))));
        event->accept();
    } else if (event->modifiers() & Qt::ShiftModifier) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - event->angleDelta().y());
        event->accept();
    } else {
        QGraphicsView::wheelEvent(event);
    }
}

void PlaylistView::drawBackground(QPainter* painter, const QRectF& rect)
{
    // Save state
    painter->save();

    // The timeline sits at the top (visually). We offset the lanes.
    // However, the scene's y=0 is at the very top. 
    // We should reserve 30px at the top of the SCENE for the timeline?
    // No, drawForeground uses viewport coordinates and ALWAYS overlays the top 30px of the viewport.
    // Therefore, the scene's y=0 to y=30 will be permanently hidden by the timeline.
    // We must offset our track backgrounds by m_timelineHeight.

    int startTrack = qFloor((rect.top() - m_timelineHeight) / m_trackHeight);
    int endTrack = qCeil((rect.bottom() - m_timelineHeight) / m_trackHeight);
    
    // Limit endTrack to m_trackCount so we don't draw lanes for non-existent tracks
    endTrack = qMin(endTrack, m_trackCount - 1);

    for (int i = qMax(0, startTrack); i <= endTrack; ++i) {
        double yPos = m_timelineHeight + i * m_trackHeight;
        QRectF laneRect(rect.left(), yPos, rect.width(), m_trackHeight);

        if (i % 2 == 0) {
            painter->fillRect(laneRect, QColor("#242424"));
        } else {
            painter->fillRect(laneRect, QColor("#282828"));
        }

        // Draw bottom border for lane
        painter->setPen(QColor("#151515"));
        painter->drawLine(QPointF(rect.left(), yPos + m_trackHeight - 1), QPointF(rect.right(), yPos + m_trackHeight - 1));
    }

    // 2. Draw vertical grid lines (beats and measures)
    int startBeat = qFloor(rect.left() / m_pixelsPerBeat);
    int endBeat = qCeil(rect.right() / m_pixelsPerBeat);
    
    // Configure pens
    QPen measurePen(QColor("#4a4a4a"), 1);
    QPen beatPen(QColor("#303030"), 1);

    // Sub-beat dots
    QVector<qreal> dashes;
    dashes << 1 << 3;
    QPen subBeatPen(QColor("#303030"), 1);
    subBeatPen.setDashPattern(dashes);
    
    for (int b = qMax(0, startBeat); b <= endBeat; ++b) {
        double x = b * m_pixelsPerBeat;

        // Beat line (thin). Bar lines are drawn separately so odd meters like
        // 5/8 can place bars between quarter-note beats.
        painter->setPen(beatPen);
        painter->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
        
        // Draw 16th sub-beats if zoomed in enough
        if (m_pixelsPerBeat > 60.0) {
            double subBeatWidth = m_pixelsPerBeat / 4.0;
            painter->setPen(subBeatPen);
            for (int i = 1; i < 4; ++i) {
                double subX = x + i * subBeatWidth;
                painter->drawLine(QPointF(subX, rect.top()), QPointF(subX, rect.bottom()));
            }
        }
    }

    const std::vector<BarMarker> visibleBars = barMarkers(
        rect.left() / m_pixelsPerBeat - 64.0,
        rect.right() / m_pixelsPerBeat + 64.0);
    for (const BarMarker& marker : visibleBars) {
        const double notatedBeatWidth = 4.0 / static_cast<double>(marker.denominator);
        const auto nextSignature = std::upper_bound(
            m_timeSignatureMap.begin(), m_timeSignatureMap.end(), marker.beat,
            [](double beat, const MidiTimeSignaturePoint& point) { return beat < point.beat; });
        const double signatureEnd = nextSignature == m_timeSignatureMap.end()
            ? std::numeric_limits<double>::infinity() : nextSignature->beat;
        painter->setPen(beatPen);
        if (notatedBeatWidth * m_pixelsPerBeat >= 8.0) {
            for (int beat = 1; beat < marker.numerator; ++beat) {
                const double beatPosition = marker.beat + static_cast<double>(beat) * notatedBeatWidth;
                if (beatPosition >= signatureEnd - 1.0e-9) break;
                const double x = beatPosition * m_pixelsPerBeat;
                painter->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
            }
        }
        painter->setPen(measurePen);
        const double x = marker.beat * m_pixelsPerBeat;
        painter->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
    }

    QFont clipFont = painter->font();
    clipFont.setPointSize(8);
    clipFont.setBold(true);
    painter->setFont(clipFont);

    for (int i = 0; i < (int)m_midiClips.size(); ++i) {
        const MidiClip& clip = m_midiClips[i];
        QRectF r = clipRect(clip);
        if (!rect.intersects(r)) {
            continue;
        }

        painter->save();
        if (clip.muted) painter->setOpacity(0.42);
        else if (!clip.takeGroupId.isEmpty() && !clip.takeActive) painter->setOpacity(0.28);
        const bool selected = isClipSelected(clip.clipId);
        QColor fill = clip.clipColor.isValid() ? clip.clipColor : QColor("#255f68");
        if (selected) fill = fill.lighter(116);
        const QColor border = selected ? QColor("#ff9020")
            : (!clip.takeGroupId.isEmpty() && clip.takeActive
                ? QColor("#35d2a3") : fill.lighter(125));
        painter->setPen(QPen(border, selected ? 2 : 1));
        painter->setBrush(fill);
        painter->drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        if (clip.loopEnabled && std::isfinite(clip.loopLengthBeats) &&
            clip.loopLengthBeats >= 0.25) {
            double phase = std::fmod(clip.sourceOffsetBeats - clip.loopStartBeats,
                                     clip.loopLengthBeats);
            if (phase < 0.0) phase += clip.loopLengthBeats;
            const double firstBoundary = phase > 1.0e-9
                ? clip.loopLengthBeats - phase : clip.loopLengthBeats;
            drawClipLoopGuides(painter, r, clip.loopLengthBeats,
                               firstBoundary, m_pixelsPerBeat);
        }

        painter->setPen(contrastingClipText(fill, QColor("#dff7fa")));
        painter->drawText(r.adjusted(8, 2, -6, -2), Qt::AlignLeft | Qt::AlignVCenter,
                          clip.loopEnabled ? QString("%1  [LOOP]").arg(clip.name) : clip.name);
        if (clip.sourceOffsetBeats > 0.000001) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#ffb14e"));
            painter->drawRect(QRectF(r.left() + 2.0, r.top() + 4.0, 3.0, r.height() - 8.0));
        }
        if (clip.isUnique) {
            const QRectF badge(r.right() - 20.0, r.top() + 5.0, 14.0, 14.0);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#ff9020"));
            painter->drawEllipse(badge);
            painter->setPen(QColor("#1b1b1b"));
            painter->drawText(badge, Qt::AlignCenter, "U");
        }
        drawClipGroupIndicator(painter, r, clip.groupId);
        if (!clip.takeGroupId.isEmpty())
            drawTakeLaneBadge(painter, r, clip.takeLane, clip.takeActive);
        if (clip.locked) drawClipLockBadge(painter, r, clip.isUnique);
        painter->restore();
        if (clip.muted) {
            painter->setPen(QPen(QColor(210, 220, 218, 150), 1.0));
            painter->setBrush(QBrush(QColor(20, 24, 24, 70), Qt::BDiagPattern));
            painter->drawRoundedRect(r.adjusted(1.5, 1.5, -1.5, -1.5), 3, 3);
            painter->drawText(r.adjusted(8, 2, -6, -2), Qt::AlignRight | Qt::AlignVCenter, "MUTED");
        }
    }

    for (int i = 0; i < (int)m_audioClips.size(); ++i) {
        const AudioClip& clip = m_audioClips[i];
        QRectF r = clipRect(clip);
        if (!rect.intersects(r)) {
            continue;
        }

        painter->save();
        if (clip.muted) painter->setOpacity(0.42);
        else if (!clip.takeGroupId.isEmpty() && !clip.takeActive) painter->setOpacity(0.28);
        const bool selected = isClipSelected(clip.clipId);
        QColor fill = clip.clipColor.isValid() ? clip.clipColor : QColor("#6f5522");
        if (selected) fill = fill.lighter(116);
        const QColor border = clip.mediaOffline ? QColor("#ef7165")
            : (selected ? QColor("#ff9020")
                : (!clip.takeGroupId.isEmpty() && clip.takeActive
                    ? QColor("#35d2a3") : fill.lighter(125)));
        painter->setPen(QPen(border, selected ? 2 : 1));
        painter->setBrush(clip.mediaOffline
            ? QBrush(QColor("#3a2523"), Qt::BDiagPattern)
            : QBrush(fill));
        painter->drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        if (clip.loopEnabled && std::isfinite(clip.loopLengthBeats) &&
            clip.loopLengthBeats >= 0.25) {
            double phase = std::fmod(clip.sourceOffsetBeats - clip.loopStartBeats,
                                     clip.loopLengthBeats);
            if (phase < 0.0) phase += clip.loopLengthBeats;
            const double firstBoundary = phase > 1.0e-9
                ? clip.loopLengthBeats - phase : clip.loopLengthBeats;
            drawClipLoopGuides(painter, r, clip.loopLengthBeats,
                               firstBoundary, m_pixelsPerBeat);
        }

        if (!clip.mediaOffline && clip.sourceDurationSeconds > 0.0 &&
            clip.warpLengthBeats > 0.0 && !clip.transientSeconds.empty()) {
            painter->save();
            painter->setClipRect(r.adjusted(1.0, 1.0, -1.0, -1.0));
            painter->setPen(QPen(selected ? QColor(255, 210, 138, 145)
                                         : QColor(255, 226, 168, 100), 1.0));
            for (double markerSeconds : clip.transientSeconds) {
                double markerBeat = warpedBeatForSourceSeconds(
                    markerSeconds, clip.sourceDurationSeconds,
                    clip.warpLengthBeats, clip.warpMarkers);
                if (clip.reverse) markerBeat = clip.warpLengthBeats - markerBeat;
                if (markerBeat < clip.sourceOffsetBeats ||
                    markerBeat > clip.sourceOffsetBeats + clip.lengthBeats) continue;
                const double x = r.left() +
                    (markerBeat - clip.sourceOffsetBeats) * m_pixelsPerBeat;
                if (x < r.left() + 1.0 || x > r.right() - 1.0) continue;
                painter->drawLine(QPointF(x, r.top() + 18.0),
                                  QPointF(x, r.bottom() - 2.0));
            }
            painter->setPen(QPen(QColor("#ff9f2f"), 1.5));
            painter->setBrush(QColor("#ff9f2f"));
            for (const AudioWarpMarker& marker : clip.warpMarkers) {
                double markerBeat = clip.reverse
                    ? clip.warpLengthBeats - marker.targetBeat : marker.targetBeat;
                if (markerBeat < clip.sourceOffsetBeats ||
                    markerBeat > clip.sourceOffsetBeats + clip.lengthBeats) continue;
                const double x = r.left() +
                    (markerBeat - clip.sourceOffsetBeats) * m_pixelsPerBeat;
                painter->drawLine(QPointF(x, r.top() + 14.0),
                                  QPointF(x, r.bottom() - 2.0));
                QPolygonF anchor;
                anchor << QPointF(x - 4.0, r.top() + 14.0)
                       << QPointF(x + 4.0, r.top() + 14.0)
                       << QPointF(x, r.top() + 20.0);
                painter->drawPolygon(anchor);
            }
            painter->restore();
        }

        painter->setPen(clip.mediaOffline ? QColor("#ffd3cf")
                                          : contrastingClipText(fill, QColor("#fff4d6")));
        QString processing;
        if (clip.normalize) processing += " N";
        if (clip.reverse) processing += " R";
        if (clip.reversePolarity) processing += " P";
        if (clip.loopEnabled) processing += " L";
        const QString displayName = clip.mediaOffline ? QString("OFFLINE · %1").arg(clip.name) : clip.name;
        painter->drawText(r.adjusted(8, 2, -24, -2), Qt::AlignLeft | Qt::AlignVCenter,
                          processing.isEmpty() ? displayName : QString("%1  [%2]").arg(displayName, processing.trimmed()));
        const double fadeInWidth = qBound(0.0, clip.fadeInBeats * m_pixelsPerBeat, r.width());
        const double fadeOutWidth = qBound(0.0, clip.fadeOutBeats * m_pixelsPerBeat, r.width() - fadeInWidth);
        if (fadeInWidth > 0.5 || fadeOutWidth > 0.5) {
            QPainterPath envelope;
            envelope.moveTo(r.left() + 1.0, r.bottom() - 2.0);
            if (fadeInWidth > 0.5) {
                constexpr int fadeSamples = 16;
                for (int sample = 1; sample <= fadeSamples; ++sample) {
                    const double progress = static_cast<double>(sample) / fadeSamples;
                    envelope.lineTo(r.left() + fadeInWidth * progress,
                        r.bottom() - 2.0 - shapedAudioFade(progress, clip.fadeInCurve) *
                            (r.height() - 9.0));
                }
            } else {
                envelope.lineTo(r.left() + 1.0, r.top() + 7.0);
            }
            envelope.lineTo(r.right() - fadeOutWidth, r.top() + 7.0);
            if (fadeOutWidth > 0.5) {
                constexpr int fadeSamples = 16;
                for (int sample = 1; sample <= fadeSamples; ++sample) {
                    const double progress = static_cast<double>(sample) / fadeSamples;
                    envelope.lineTo(r.right() - fadeOutWidth + fadeOutWidth * progress,
                        r.bottom() - 2.0 - shapedAudioFade(1.0 - progress, clip.fadeOutCurve) *
                            (r.height() - 9.0));
                }
            } else {
                envelope.lineTo(r.right() - 1.0, r.bottom() - 2.0);
            }
            painter->setPen(QPen(QColor("#ffd38a"), 1.25));
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(envelope);
        }
        if (selected) {
            const QPointF fadeInHandle(r.left() + fadeInWidth, r.top() + 7.0);
            const QPointF fadeOutHandle(r.right() - fadeOutWidth, r.top() + 7.0);
            painter->setPen(QPen(QColor("#20170c"), 1.0));
            painter->setBrush(QColor("#ffd38a"));
            painter->drawEllipse(fadeInHandle, 3.5, 3.5);
            painter->drawEllipse(fadeOutHandle, 3.5, 3.5);
        }
        if (clip.sourceOffsetBeats > 0.000001) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#ffb14e"));
            painter->drawRect(QRectF(r.left() + 2.0, r.top() + 4.0, 3.0, r.height() - 8.0));
        }
        if (clip.isUnique) {
            const QRectF badge(r.right() - 20.0, r.top() + 5.0, 14.0, 14.0);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#ff9020"));
            painter->drawEllipse(badge);
            painter->setPen(QColor("#1b1b1b"));
            painter->drawText(badge, Qt::AlignCenter, "U");
        }
        drawClipGroupIndicator(painter, r, clip.groupId);
        if (!clip.takeGroupId.isEmpty())
            drawTakeLaneBadge(painter, r, clip.takeLane, clip.takeActive);
        if (clip.locked) drawClipLockBadge(painter, r, clip.isUnique);
        painter->restore();
        if (clip.muted) {
            painter->setPen(QPen(QColor(232, 220, 195, 150), 1.0));
            painter->setBrush(QBrush(QColor(22, 20, 16, 70), Qt::BDiagPattern));
            painter->drawRoundedRect(r.adjusted(1.5, 1.5, -1.5, -1.5), 3, 3);
            painter->drawText(r.adjusted(8, 2, -6, -2), Qt::AlignRight | Qt::AlignVCenter, "MUTED");
        }
    }

    if (m_pluginAutomationVisible) {
        const QRectF lane = pluginAutomationRect();
        if (rect.intersects(lane)) {
            const QRectF graph = lane.adjusted(0.0, 10.0, 0.0, -3.0);
            painter->fillRect(lane.intersected(rect), QColor(12, 12, 12, 112));
            painter->setPen(QColor("#ffb14e"));
            painter->drawText(QRectF(rect.left() + 6.0, lane.top(), rect.width() - 12.0, 10.0),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QString("A  %1  ·  %2").arg(
                                  m_pluginAutomationLane.parameterName.isEmpty()
                                      ? QString("Parameter %1").arg(m_pluginAutomationLane.parameterId)
                                      : m_pluginAutomationLane.parameterName,
                                  m_pluginAutomationLane.interpolation == PluginAutomationInterpolation::Hold
                                      ? QStringLiteral("Hold")
                                      : (m_pluginAutomationLane.interpolation ==
                                             PluginAutomationInterpolation::Smooth
                                          ? QStringLiteral("Smooth · Shift+RMB curve")
                                          : QStringLiteral("Linear"))));
            const auto& points = m_pluginAutomationLane.points;
            if (points.empty()) {
                painter->setPen(QColor("#78634d"));
                painter->drawText(graph.intersected(rect), Qt::AlignCenter,
                                  "Click to add automation point");
            } else {
                auto pointPosition = [&](const PluginAutomationPoint& point) {
                    return QPointF(point.beat * m_pixelsPerBeat,
                        graph.bottom() - point.normalizedValue * graph.height());
                };
                auto valueAtBeat = [&](double beat) {
                    const auto next = std::upper_bound(points.begin(), points.end(), beat,
                        [](double target, const PluginAutomationPoint& point) {
                            return target < point.beat;
                        });
                    if (next == points.begin()) return next->normalizedValue;
                    const PluginAutomationPoint& left = *(next - 1);
                    if (next == points.end() || m_pluginAutomationLane.interpolation ==
                        PluginAutomationInterpolation::Hold) return left.normalizedValue;
                    const double span = next->beat - left.beat;
                    if (span <= 1.0e-12) return next->normalizedValue;
                    double position = std::clamp((beat - left.beat) / span, 0.0, 1.0);
                    if (m_pluginAutomationLane.interpolation ==
                        PluginAutomationInterpolation::Smooth)
                        position = pluginAutomationCurvePosition(position, left);
                    return left.normalizedValue +
                        (next->normalizedValue - left.normalizedValue) * position;
                };
                QPainterPath envelope;
                const double visibleLeft = qMax(graph.left(), rect.left());
                const double visibleRight = qMin(graph.right(), rect.right());
                if (m_pluginAutomationLane.interpolation ==
                    PluginAutomationInterpolation::Hold) {
                    const double leftBeat = m_pixelsPerBeat > 0.0
                        ? visibleLeft / m_pixelsPerBeat : 0.0;
                    double y = graph.bottom() - valueAtBeat(leftBeat) * graph.height();
                    envelope.moveTo(visibleLeft, y);
                    for (const PluginAutomationPoint& point : points) {
                        const double x = point.beat * m_pixelsPerBeat;
                        if (x <= visibleLeft) continue;
                        if (x > visibleRight) break;
                        envelope.lineTo(x, y);
                        y = graph.bottom() - point.normalizedValue * graph.height();
                        envelope.lineTo(x, y);
                    }
                    envelope.lineTo(visibleRight, y);
                } else {
                    const int visualSegments = std::clamp(
                        static_cast<int>(std::ceil((visibleRight - visibleLeft) / 2.0)),
                        1, 4096);
                    for (int segment = 0; segment <= visualSegments; ++segment) {
                        const double x = visibleLeft + (visibleRight - visibleLeft) *
                            static_cast<double>(segment) / static_cast<double>(visualSegments);
                        const double beat = m_pixelsPerBeat > 0.0 ? x / m_pixelsPerBeat : 0.0;
                        const double y = graph.bottom() - valueAtBeat(beat) * graph.height();
                        if (segment == 0) envelope.moveTo(x, y);
                        else envelope.lineTo(x, y);
                    }
                }
                painter->setBrush(Qt::NoBrush);
                painter->setPen(QPen(QColor("#ff9d3d"), 1.6));
                painter->drawPath(envelope);
                painter->setPen(QPen(QColor("#26180c"), 1.0));
                painter->setBrush(QColor("#ffc078"));
                const double firstVisibleBeat = m_pixelsPerBeat > 0.0
                    ? qMax(0.0, (visibleLeft - 5.0) / m_pixelsPerBeat) : 0.0;
                const double lastVisibleBeat = m_pixelsPerBeat > 0.0
                    ? qMax(firstVisibleBeat, (visibleRight + 5.0) / m_pixelsPerBeat)
                    : std::numeric_limits<double>::max();
                auto firstPoint = std::lower_bound(points.begin(), points.end(), firstVisibleBeat,
                    [](const PluginAutomationPoint& point, double beat) { return point.beat < beat; });
                const auto lastPoint = std::upper_bound(firstPoint, points.end(), lastVisibleBeat,
                    [](double beat, const PluginAutomationPoint& point) { return beat < point.beat; });
                const size_t visiblePoints = static_cast<size_t>(std::distance(firstPoint, lastPoint));
                const size_t markerStride = std::max<size_t>(1, (visiblePoints + 2047) / 2048);
                size_t markerIndex = 0;
                for (auto point = firstPoint; point != lastPoint; ++point, ++markerIndex) {
                    if (markerIndex % markerStride != 0 && std::next(point) != lastPoint) continue;
                    const QPointF center = pointPosition(*point);
                    if (rect.adjusted(-5.0, -5.0, 5.0, 5.0).contains(center))
                        painter->drawEllipse(center, 4.0, 4.0);
                }
            }
        }
    }

    if (m_draggingClipCreate && m_dragTrackIndex >= 0) {
        double start = qMin(m_dragStartBeat, m_dragCurrentBeat);
        double end = qMax(m_dragStartBeat, m_dragCurrentBeat);
        if (end - start < 1.0) {
            end = start + 1.0;
        }

        MidiClip preview;
        preview.trackIndex = m_dragTrackIndex;
        preview.startBeat = start;
        preview.lengthBeats = end - start;
        preview.name = "MIDI";
        QRectF r = clipRect(preview);
        painter->setPen(QPen(QColor("#ff9020"), 1, Qt::DashLine));
        painter->setBrush(QColor(86, 182, 194, 70));
        painter->drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    }

    painter->restore();
}

void PlaylistView::drawForeground(QPainter* painter, const QRectF& rect)
{
    painter->save();

    // Reset transform to draw in VIEWPORT coordinates so timeline stays fixed at the top
    painter->setTransform(QTransform());

    // We only care about the top section of the viewport
    QRect viewportRect = viewport()->rect();
    QRect timelineRect(0, 0, viewportRect.width(), m_timelineHeight);

    // Fill background for timeline
    painter->fillRect(timelineRect, QColor("#202020"));

    // Draw bottom border
    painter->setPen(QColor("#000000"));
    painter->drawLine(0, m_timelineHeight - 1, viewportRect.width(), m_timelineHeight - 1);

    // Calculate scroll offset to map viewport X to scene X
    double scrollX = horizontalScrollBar()->value();

    if (m_loopEnabled) {
        const double left = m_loopStartBeat * m_pixelsPerBeat - scrollX;
        const double right = m_loopEndBeat * m_pixelsPerBeat - scrollX;
        const QRectF loopRect(left, 0.0, right - left, viewportRect.height());
        painter->fillRect(loopRect.intersected(QRectF(viewportRect)), QColor(255, 144, 32, 24));
        painter->setPen(QPen(QColor(255, 144, 32, 180), 1.0));
        painter->drawLine(QPointF(left, 0.0), QPointF(left, viewportRect.height()));
        painter->drawLine(QPointF(right, 0.0), QPointF(right, viewportRect.height()));
    }
    if (m_timeSelectionEnabled) {
        const double left = m_timeSelectionStartBeat * m_pixelsPerBeat - scrollX;
        const double right = m_timeSelectionEndBeat * m_pixelsPerBeat - scrollX;
        const QRectF selectionRect(left, 0.0, right - left, viewportRect.height());
        painter->fillRect(selectionRect.intersected(QRectF(viewportRect)), QColor(77, 182, 255, 28));
        painter->fillRect(QRectF(left, 0.0, right - left, 4.0), QColor(77, 182, 255, 180));
        painter->setPen(QPen(QColor(77, 182, 255, 210), 1.0));
        painter->drawLine(QPointF(left, 0.0), QPointF(left, viewportRect.height()));
        painter->drawLine(QPointF(right, 0.0), QPointF(right, viewportRect.height()));
    }
    if (m_punchEnabled) {
        const double left = m_punchInBeat * m_pixelsPerBeat - scrollX;
        const double right = m_punchOutBeat * m_pixelsPerBeat - scrollX;
        const QRectF punchRect(left, 0.0, right - left, viewportRect.height());
        painter->fillRect(punchRect.intersected(QRectF(viewportRect)), QColor(255, 82, 82, 18));
        painter->fillRect(QRectF(left, 4.0, right - left, 3.0), QColor(255, 82, 82, 210));
        painter->setPen(QPen(QColor(255, 82, 82, 230), 1.5));
        painter->drawLine(QPointF(left, 0.0), QPointF(left, viewportRect.height()));
        painter->drawLine(QPointF(right, 0.0), QPointF(right, viewportRect.height()));
        painter->drawText(QRectF(left + 4.0, 7.0, 54.0, 16.0),
                          Qt::AlignLeft | Qt::AlignVCenter, "PUNCH");
    }

    int startBeat = qFloor(scrollX / m_pixelsPerBeat);
    int endBeat = qCeil((scrollX + viewportRect.width()) / m_pixelsPerBeat);

    painter->setPen(QColor("#888888"));
    QFont font = painter->font();
    font.setPointSize(8);
    painter->setFont(font);

    for (int b = qMax(0, startBeat); b <= endBeat; ++b) {
        double sceneX = b * m_pixelsPerBeat;
        double viewX = sceneX - scrollX;

        // Quarter-note grid tick.
        painter->drawLine(QPointF(viewX, m_timelineHeight - 5), QPointF(viewX, m_timelineHeight));

        // Draw 16th sub-beats if zoomed in enough
        if (m_pixelsPerBeat > 60.0) {
            double subBeatWidth = m_pixelsPerBeat / 4.0;
            for (int i = 1; i < 4; ++i) {
                double subViewX = viewX + i * subBeatWidth;
                painter->drawLine(QPointF(subViewX, m_timelineHeight - 3), QPointF(subViewX, m_timelineHeight));
            }
        }
    }

    const std::vector<BarMarker> visibleBars = barMarkers(
        scrollX / m_pixelsPerBeat - 64.0,
        (scrollX + viewportRect.width()) / m_pixelsPerBeat + 64.0);
    for (const BarMarker& marker : visibleBars) {
        const double notatedBeatWidth = 4.0 / static_cast<double>(marker.denominator);
        const auto nextSignature = std::upper_bound(
            m_timeSignatureMap.begin(), m_timeSignatureMap.end(), marker.beat,
            [](double beat, const MidiTimeSignaturePoint& point) { return beat < point.beat; });
        const double signatureEnd = nextSignature == m_timeSignatureMap.end()
            ? std::numeric_limits<double>::infinity() : nextSignature->beat;
        for (int beat = 0; beat < marker.numerator; ++beat) {
            const double beatPosition = marker.beat + static_cast<double>(beat) * notatedBeatWidth;
            if (beatPosition >= signatureEnd - 1.0e-9) break;
            const double sceneX = beatPosition * m_pixelsPerBeat;
            const double viewX = sceneX - scrollX;
            if (viewX < -60.0 || viewX > viewportRect.width() + 60.0) {
                continue;
            }
            const bool isBar = beat == 0;
            painter->drawLine(QPointF(viewX, m_timelineHeight - (isBar ? 10 : 7)), QPointF(viewX, m_timelineHeight));
            if (isBar || notatedBeatWidth * m_pixelsPerBeat > 28.0) {
                const QString label = isBar && notatedBeatWidth * m_pixelsPerBeat <= 28.0
                    ? QString::number(marker.barNumber)
                    : QString("%1.%2").arg(marker.barNumber).arg(beat + 1);
                painter->drawText(QRectF(viewX + 4, 0, 50, m_timelineHeight - 5), Qt::AlignLeft | Qt::AlignBottom, label);
            }
        }
        if (marker.signatureChange) {
            const double viewX = marker.beat * m_pixelsPerBeat - scrollX;
            painter->setPen(QColor("#ff9020"));
            painter->drawText(QRectF(viewX + 4, 0, 48, 12), Qt::AlignLeft | Qt::AlignTop,
                QString("%1/%2").arg(marker.numerator).arg(marker.denominator));
            painter->setPen(QColor("#888888"));
        }
    }

    const double playheadX = m_playheadBeat * m_pixelsPerBeat - scrollX;
    if (playheadX >= 0.0 && playheadX <= viewportRect.width()) {
        painter->setPen(QPen(QColor("#ff9020"), 2.0));
        painter->drawLine(QPointF(playheadX, 0.0), QPointF(playheadX, viewportRect.height()));
    }

    if (m_marqueeSelecting) {
        QRect marqueeRect(m_marqueeAnchorPos, m_marqueeCurrentPos);
        marqueeRect = marqueeRect.normalized().intersected(
            viewportRect.adjusted(0, m_timelineHeight, 0, 0));
        painter->setPen(QPen(QColor(255, 176, 78, 220), 1.0, Qt::DashLine));
        painter->setBrush(QColor(255, 144, 32, 34));
        painter->drawRect(marqueeRect.adjusted(0, 0, -1, -1));
    }

    painter->restore();
}

void PlaylistView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_lastPanPos = event->pos();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    if (m_pluginAutomationVisible && event->pos().y() >= m_timelineHeight) {
        const QPointF automationPos = mapToScene(event->pos());
        const QRectF automationLane = pluginAutomationRect();
        if (event->button() == Qt::RightButton && automationLane.contains(automationPos)) {
            const int pointIndex = pluginAutomationPointAt(automationPos);
            if (pointIndex >= 0) {
                if (event->modifiers() & Qt::ShiftModifier) {
                    QMenu curveMenu(this);
                    const bool curveEditable = m_pluginAutomationLane.interpolation ==
                        PluginAutomationInterpolation::Smooth && pointIndex + 1 <
                        static_cast<int>(m_pluginAutomationLane.points.size());
                    if (!curveEditable) {
                        QAction* info = curveMenu.addAction(
                            "Curve applies to a non-final point in Smooth mode");
                        info->setEnabled(false);
                        curveMenu.exec(event->globalPos());
                        event->accept();
                        return;
                    }
                    QAction* easeOut = curveMenu.addAction("Curve: Ease out");
                    QAction* neutral = curveMenu.addAction("Curve: Symmetric");
                    QAction* easeIn = curveMenu.addAction("Curve: Ease in");
                    QAction* custom = curveMenu.addAction("Curve: Custom…");
                    easeOut->setCheckable(true);
                    neutral->setCheckable(true);
                    easeIn->setCheckable(true);
                    const PluginAutomationPoint& currentPoint =
                        m_pluginAutomationLane.points[static_cast<size_t>(pointIndex)];
                    const double currentTension = currentPoint.curveTension;
                    easeOut->setChecked(!currentPoint.curvePolynomial && currentTension < -0.05);
                    neutral->setChecked(!currentPoint.curvePolynomial &&
                                        std::fabs(currentTension) <= 0.05);
                    easeIn->setChecked(!currentPoint.curvePolynomial && currentTension > 0.05);
                    if (currentPoint.curvePolynomial) {
                        curveMenu.addSeparator();
                        QAction* exact = curveMenu.addAction(
                            "Exact Touch continuation · choose a curve to replace");
                        exact->setEnabled(false);
                    }
                    QAction* selected = curveMenu.exec(event->globalPos());
                    if (selected) {
                        PluginAutomationPoint& point =
                            m_pluginAutomationLane.points[static_cast<size_t>(pointIndex)];
                        bool changed = true;
                        if (selected == custom) {
                            bool accepted = false;
                            const double value = QInputDialog::getDouble(
                                this, "Automation curve tension",
                                "Tension (-1 ease-out, +1 ease-in):",
                                point.curveTension, -1.0, 1.0, 2, &accepted,
                                Qt::WindowFlags{}, 0.05);
                            if (accepted) point.curveTension = value;
                            else changed = false;
                        } else {
                            point.curveTension = selected == easeOut ? -0.65
                                : (selected == easeIn ? 0.65 : 0.0);
                        }
                        if (changed) {
                            point.curvePolynomial.reset();
                        }
                        if (changed) {
                            emitPluginAutomationEdit();
                            viewport()->update();
                        }
                    }
                } else {
                    m_pluginAutomationLane.points.erase(
                        m_pluginAutomationLane.points.begin() + pointIndex);
                    thinPluginAutomationPoints();
                    emitPluginAutomationEdit();
                    updateSceneBounds();
                    viewport()->update();
                }
            } else {
                QMenu menu(this);
                QAction* hold = menu.addAction("Hold interpolation");
                QAction* linear = menu.addAction("Linear interpolation");
                QAction* smooth = menu.addAction("Smooth interpolation");
                hold->setCheckable(true);
                linear->setCheckable(true);
                smooth->setCheckable(true);
                hold->setChecked(m_pluginAutomationLane.interpolation ==
                                 PluginAutomationInterpolation::Hold);
                linear->setChecked(m_pluginAutomationLane.interpolation ==
                                   PluginAutomationInterpolation::Linear);
                smooth->setChecked(m_pluginAutomationLane.interpolation ==
                                   PluginAutomationInterpolation::Smooth);
                QAction* selected = menu.exec(event->globalPos());
                if (selected == hold || selected == linear || selected == smooth) {
                    m_pluginAutomationLane.interpolation = selected == hold
                        ? PluginAutomationInterpolation::Hold
                        : (selected == smooth ? PluginAutomationInterpolation::Smooth
                                             : PluginAutomationInterpolation::Linear);
                    thinPluginAutomationPoints();
                    emitPluginAutomationEdit();
                    viewport()->update();
                }
            }
            event->accept();
            return;
        }
        const QRectF automationGraph = automationLane.adjusted(0.0, 10.0, 0.0, -3.0);
        if (event->button() == Qt::LeftButton && automationGraph.contains(automationPos)) {
            m_pluginAutomationDragPoint = pluginAutomationPointAt(automationPos);
            m_pluginAutomationDragChanged = false;
            if (m_pluginAutomationDragPoint < 0 &&
                m_pluginAutomationLane.points.size() < kMaxPluginAutomationPointsPerLane) {
                m_pluginAutomationLane.points.push_back({});
                m_pluginAutomationDragPoint = movePluginAutomationPoint(
                    static_cast<int>(m_pluginAutomationLane.points.size()) - 1,
                    automationPos, event->modifiers());
                m_pluginAutomationDragChanged = m_pluginAutomationDragPoint >= 0;
            }
            if (m_pluginAutomationDragPoint >= 0) {
                viewport()->setCursor(Qt::CrossCursor);
                viewport()->update();
                event->accept();
                return;
            }
        }
    }

    if (event->button() == Qt::RightButton && event->pos().y() < m_timelineHeight) {
        if (m_timeSelectionEnabled) {
            setTimeSelection(false, 0.0, 0.0);
            emit timeSelectionChanged(false, 0.0, 0.0);
        } else {
            setLoopRange(false, 0.0, 0.0);
            emit loopRangeChanged(false, 0.0, 0.0);
        }
        event->accept();
        return;
    }

    const bool commandModifier = event->modifiers().testFlag(Qt::ControlModifier) ||
        event->modifiers().testFlag(Qt::MetaModifier);
    if (event->button() == Qt::LeftButton && event->pos().y() < m_timelineHeight &&
        commandModifier) {
        clearClipDrag();
        m_timeSelectionDragging = true;
        m_timeSelectionAnchorBeat = snapBeatUnlessAlt(
            beatAtX(mapToScene(event->pos()).x()), event->modifiers());
        m_timeSelectionStartBeat = m_timeSelectionAnchorBeat;
        m_timeSelectionEndBeat = m_timeSelectionAnchorBeat + 0.25;
        m_timeSelectionEnabled = true;
        viewport()->update();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && event->pos().y() < m_timelineHeight &&
        (event->modifiers() & Qt::ShiftModifier)) {
        m_loopDragging = true;
        m_loopDragAnchorBeat = snapBeatUnlessAlt(beatAtX(mapToScene(event->pos()).x()), event->modifiers());
        m_loopStartBeat = m_loopDragAnchorBeat;
        m_loopEndBeat = m_loopStartBeat + 0.25;
        m_loopEnabled = true;
        viewport()->update();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && event->pos().y() < m_timelineHeight) {
        const double beat = snapBeatUnlessAlt(beatAtX(mapToScene(event->pos()).x()), event->modifiers());
        setPlayheadBeat(beat);
        emit playheadSeekRequested(beat);
        event->accept();
        return;
    }

    if (event->button() == Qt::RightButton) {
        const QPointF scenePos = mapToScene(event->pos());
        const int audioIndex = audioClipAt(scenePos);
        const int midiIndex = clipAt(scenePos);
        if (audioIndex >= 0 || midiIndex >= 0) {
            if (audioIndex >= 0) {
                selectAudioClip(audioIndex, isClipSelected(m_audioClips[audioIndex].clipId));
            } else {
                selectClip(midiIndex, false, isClipSelected(m_midiClips[midiIndex].clipId));
            }
            QMenu menu(this);
            QAction* rename = menu.addAction("Rename...");
            bool hasSelectedClip = false;
            bool selectedClipsAllMuted = true;
            bool selectedClipsAllLooped = true;
            bool selectedClipsAllLocked = true;
            bool selectedClipsAnyLocked = false;
            bool selectedClipsAnyGrouped = false;
            bool selectedClipsAnyTake = false;
            int selectedClipCount = 0;
            for (const MidiClip& clip : m_midiClips) {
                if (!isClipSelected(clip.clipId)) continue;
                hasSelectedClip = true;
                ++selectedClipCount;
                selectedClipsAllMuted = selectedClipsAllMuted && clip.muted;
                selectedClipsAllLooped = selectedClipsAllLooped && clip.loopEnabled;
                selectedClipsAllLocked = selectedClipsAllLocked && clip.locked;
                selectedClipsAnyLocked = selectedClipsAnyLocked || clip.locked;
                selectedClipsAnyGrouped = selectedClipsAnyGrouped || !clip.groupId.isEmpty();
                selectedClipsAnyTake = selectedClipsAnyTake || !clip.takeGroupId.isEmpty();
            }
            for (const AudioClip& clip : m_audioClips) {
                if (!isClipSelected(clip.clipId)) continue;
                hasSelectedClip = true;
                ++selectedClipCount;
                selectedClipsAllMuted = selectedClipsAllMuted && clip.muted;
                selectedClipsAllLooped = selectedClipsAllLooped && clip.loopEnabled;
                selectedClipsAllLocked = selectedClipsAllLocked && clip.locked;
                selectedClipsAnyLocked = selectedClipsAnyLocked || clip.locked;
                selectedClipsAnyGrouped = selectedClipsAnyGrouped || !clip.groupId.isEmpty();
                selectedClipsAnyTake = selectedClipsAnyTake || !clip.takeGroupId.isEmpty();
            }
            QAction* muteClips = menu.addAction(
                selectedClipsAllMuted ? "Unmute Clips\t0" : "Mute Clips\t0");
            muteClips->setEnabled(hasSelectedClip);
            QAction* loopClips = menu.addAction(
                selectedClipsAllLooped ? "Disable Clip Loop\tCtrl+Alt+L"
                                       : "Enable Clip Loop\tCtrl+Alt+L");
            loopClips->setEnabled(hasSelectedClip && !selectedClipsAnyLocked);
            QAction* setClipColor = menu.addAction("Set Clip Color...");
            bool selectedHasCustomColor = false;
            for (const MidiClip& clip : m_midiClips)
                if (isClipSelected(clip.clipId) && clip.clipColor.isValid())
                    selectedHasCustomColor = true;
            for (const AudioClip& clip : m_audioClips)
                if (isClipSelected(clip.clipId) && clip.clipColor.isValid())
                    selectedHasCustomColor = true;
            QAction* resetClipColor = menu.addAction("Reset Clip Color");
            resetClipColor->setEnabled(selectedHasCustomColor);
            QAction* lockClips = menu.addAction("Lock Clips\tCtrl+Shift+L");
            QAction* unlockClips = menu.addAction("Unlock Clips");
            lockClips->setEnabled(hasSelectedClip && !selectedClipsAllLocked);
            unlockClips->setEnabled(hasSelectedClip && selectedClipsAnyLocked);
            QAction* groupClips = menu.addAction("Group Clips\tCtrl+G");
            QAction* ungroupClips = menu.addAction("Ungroup Clips\tCtrl+Shift+G");
            groupClips->setEnabled(selectedClipCount >= 2 && !selectedClipsAnyLocked);
            ungroupClips->setEnabled(selectedClipsAnyGrouped && !selectedClipsAnyLocked);
            QMenu* takesMenu = menu.addMenu("Take Lanes / Comp");
            QAction* createTakes = takesMenu->addAction("Group Selection as Takes");
            QAction* activateTake = takesMenu->addAction("Set as Active Comp\tShift+Enter");
            QAction* compSelection = takesMenu->addAction(
                "Comp Selected Take into Time Selection\tCtrl+Shift+Enter");
            QAction* flattenTakes = takesMenu->addAction(
                "Flatten Take Lanes to Active Clips\tCtrl+Alt+Enter");
            takesMenu->addSeparator();
            QAction* previousTake = takesMenu->addAction("Previous Take\tShift+Up");
            QAction* nextTake = takesMenu->addAction("Next Take\tShift+Down");
            createTakes->setEnabled(selectedClipCount >= 2 && !selectedClipsAnyLocked);
            activateTake->setEnabled(selectedClipsAnyTake && !selectedClipsAnyLocked);
            compSelection->setEnabled(selectedClipsAnyTake && m_timeSelectionEnabled &&
                                      !selectedClipsAnyLocked);
            flattenTakes->setEnabled(selectedClipsAnyTake && !selectedClipsAnyLocked);
            previousTake->setEnabled(selectedClipsAnyTake && !selectedClipsAnyLocked);
            nextTake->setEnabled(selectedClipsAnyTake && !selectedClipsAnyLocked);
            menu.addSeparator();
            QAction* cut = menu.addAction("Cut\tCtrl+X");
            QAction* copy = menu.addAction("Copy\tCtrl+C");
            QAction* paste = menu.addAction("Paste\tCtrl+V");
            paste->setEnabled(!m_midiClipClipboard.empty() || !m_audioClipClipboard.empty());
            menu.addSeparator();
            QAction* duplicate = menu.addAction("Duplicate\tCtrl+B / Ctrl+D");
            QAction* split = menu.addAction("Split at Playhead\tCtrl+E");
            cut->setEnabled(!selectedClipsAnyLocked);
            duplicate->setEnabled(!selectedClipsAnyLocked);
            QAction* crossfade = audioIndex >= 0
                ? menu.addAction("Crossfade Selected Clips\tX") : nullptr;
            QAction* autoCrossfade = audioIndex >= 0
                ? menu.addAction("Auto Crossfades\tCtrl+Alt+X") : nullptr;
            if (autoCrossfade) {
                autoCrossfade->setCheckable(true);
                autoCrossfade->setChecked(m_autoCrossfadeEnabled);
            }
            int crossfadeLeft = -1;
            int crossfadeRight = -1;
            double crossfadeBeats = 0.0;
            if (crossfade)
                crossfade->setEnabled(selectedAudioCrossfadePair(
                    &crossfadeLeft, &crossfadeRight, &crossfadeBeats) &&
                    !selectedClipsAnyLocked);
            QMenu* warpMenu = audioIndex >= 0 ? menu.addMenu("Warp Transients") : nullptr;
            QAction* warpQuarter = warpMenu ? warpMenu->addAction("Quantize to 1/4") : nullptr;
            QAction* warpEighth = warpMenu ? warpMenu->addAction("Quantize to 1/8") : nullptr;
            QAction* warpSixteenth = warpMenu ? warpMenu->addAction("Quantize to 1/16") : nullptr;
            QAction* warpThirtySecond = warpMenu ? warpMenu->addAction("Quantize to 1/32") : nullptr;
            QAction* resetWarp = warpMenu ? warpMenu->addAction("Reset Warp Markers") : nullptr;
            if (warpMenu) {
                const bool hasTransients = !m_audioClips[audioIndex].transientSeconds.empty();
                warpQuarter->setEnabled(hasTransients && !selectedClipsAnyLocked);
                warpEighth->setEnabled(hasTransients && !selectedClipsAnyLocked);
                warpSixteenth->setEnabled(hasTransients && !selectedClipsAnyLocked);
                warpThirtySecond->setEnabled(hasTransients && !selectedClipsAnyLocked);
                resetWarp->setEnabled(!m_audioClips[audioIndex].warpMarkers.empty() &&
                                      !selectedClipsAnyLocked);
            }
            QAction* resetFades = audioIndex >= 0 ? menu.addAction("Reset Fades") : nullptr;
            if (resetFades) resetFades->setEnabled(!selectedClipsAnyLocked);
            QMenu* stemsMenu = audioIndex >= 0 ? menu.addMenu("Separate Stems") : nullptr;
            QAction* separateTwoStems = stemsMenu
                ? stemsMenu->addAction("Vocals + Accompaniment (2 stems)") : nullptr;
            QAction* separateFourStems = stemsMenu
                ? stemsMenu->addAction("Vocals + Drums + Bass + Other (4 stems)") : nullptr;
            QAction* separateFiveStems = stemsMenu
                ? stemsMenu->addAction("Vocals + Drums + Bass + Piano + Other (5 stems)") : nullptr;
            if (stemsMenu) stemsMenu->setEnabled(!selectedClipsAnyLocked);
            const double clipStart = audioIndex >= 0
                ? m_audioClips[audioIndex].startBeat
                : m_midiClips[midiIndex].startBeat;
            const double clipEnd = audioIndex >= 0
                ? clipStart + m_audioClips[audioIndex].lengthBeats
                : clipStart + m_midiClips[midiIndex].lengthBeats;
            split->setEnabled(std::isfinite(m_playheadBeat) &&
                              m_playheadBeat - clipStart >= 0.25 &&
                              clipEnd - m_playheadBeat >= 0.25 &&
                              !selectedClipsAnyLocked);
            menu.addSeparator();
            QAction* makeUnique = menu.addAction("Make Unique");
            makeUnique->setEnabled(!selectedClipsAnyLocked);
            menu.addSeparator();
            QAction* remove = menu.addAction("Delete\tDel / Backspace");
            remove->setEnabled(!selectedClipsAnyLocked);
            QAction* selectedAction = menu.exec(event->globalPos());
            if (selectedAction == rename) {
                emit renameSelectedClipRequested();
            } else if (selectedAction == muteClips) {
                setSelectedClipsMuted(!selectedClipsAllMuted);
            } else if (selectedAction == loopClips) {
                setSelectedClipsLoopEnabled(!selectedClipsAllLooped);
            } else if (selectedAction == setClipColor) {
                const QColor initial = audioIndex >= 0
                    ? m_audioClips[audioIndex].clipColor
                    : m_midiClips[midiIndex].clipColor;
                const QColor chosen = QColorDialog::getColor(
                    initial.isValid() ? initial : QColor("#4f7d88"), this,
                    QStringLiteral("Set Clip Color"));
                if (chosen.isValid()) setSelectedClipsColor(chosen);
            } else if (selectedAction == resetClipColor) {
                setSelectedClipsColor(QColor());
            } else if (selectedAction == lockClips) {
                setSelectedClipsLocked(true);
            } else if (selectedAction == unlockClips) {
                setSelectedClipsLocked(false);
            } else if (selectedAction == groupClips) {
                groupSelectedClips();
            } else if (selectedAction == ungroupClips) {
                ungroupSelectedClips();
            } else if (selectedAction == createTakes) {
                groupSelectedClipsAsTakes();
            } else if (selectedAction == activateTake) {
                setSelectedClipAsActiveTake();
            } else if (selectedAction == compSelection) {
                compSelectedTakeInTimeSelection();
            } else if (selectedAction == flattenTakes) {
                flattenSelectedTakeGroup();
            } else if (selectedAction == previousTake) {
                cycleSelectedTake(-1);
            } else if (selectedAction == nextTake) {
                cycleSelectedTake(1);
            } else if (selectedAction == cut) {
                cutSelectedClips();
            } else if (selectedAction == copy) {
                copySelectedClips();
            } else if (selectedAction == paste) {
                pasteClips();
            } else if (selectedAction == duplicate) {
                duplicateSelectedClip();
            } else if (selectedAction == split) {
                splitSelectedClipAtPlayhead();
            } else if (crossfade && selectedAction == crossfade) {
                createCrossfadeForSelectedAudioClips();
            } else if (autoCrossfade && selectedAction == autoCrossfade) {
                m_autoCrossfadeEnabled = autoCrossfade->isChecked();
                QSettings().setValue(QStringLiteral("Editing/AutoCrossfades"),
                                     m_autoCrossfadeEnabled);
            } else if (warpQuarter && selectedAction == warpQuarter) {
                quantizeSelectedAudioClipTransients(1.0);
            } else if (warpEighth && selectedAction == warpEighth) {
                quantizeSelectedAudioClipTransients(0.5);
            } else if (warpSixteenth && selectedAction == warpSixteenth) {
                quantizeSelectedAudioClipTransients(0.25);
            } else if (warpThirtySecond && selectedAction == warpThirtySecond) {
                quantizeSelectedAudioClipTransients(0.125);
            } else if (resetWarp && selectedAction == resetWarp) {
                clearSelectedAudioClipWarpMarkers();
            } else if (resetFades && selectedAction == resetFades) {
                pushUndoState();
                AudioClip& clip = m_audioClips[audioIndex];
                clip.fadeInBeats = 0.0;
                clip.fadeOutBeats = 0.0;
                clip.fadeInCurve = 0.0;
                clip.fadeOutCurve = 0.0;
                selectAudioClip(audioIndex);
                emit contentChanged();
            } else if (separateTwoStems && selectedAction == separateTwoStems) {
                emit stemSeparationRequested(2);
            } else if (separateFourStems && selectedAction == separateFourStems) {
                emit stemSeparationRequested(4);
            } else if (separateFiveStems && selectedAction == separateFiveStems) {
                emit stemSeparationRequested(5);
            } else if (selectedAction == makeUnique) {
                emit makeSelectedClipUniqueRequested();
            } else if (selectedAction == remove) {
                deleteSelectedClip();
            }
            event->accept();
            return;
        }
    }

    if (event->button() != Qt::LeftButton) {
        QGraphicsView::mousePressEvent(event);
        return;
    }

    const QPointF scenePos = mapToScene(event->pos());
    const bool selectionModifier = event->modifiers().testFlag(Qt::ControlModifier) ||
        event->modifiers().testFlag(Qt::MetaModifier);
    if (selectionModifier) {
        const int additiveAudioIndex = audioClipAt(scenePos);
        const int additiveMidiIndex = additiveAudioIndex < 0 ? clipAt(scenePos) : -1;
        if (additiveAudioIndex >= 0 || additiveMidiIndex >= 0) {
            const bool audio = additiveAudioIndex >= 0;
            const int index = audio ? additiveAudioIndex : additiveMidiIndex;
            const QString clipId = audio ? m_audioClips[index].clipId : m_midiClips[index].clipId;
            const QString groupId = audio ? m_audioClips[index].groupId : m_midiClips[index].groupId;
            const int trackIndex = audio ? m_audioClips[index].trackIndex : m_midiClips[index].trackIndex;
            const bool primary = m_selectedClipIndex == index && m_selectedClipIsAudio == audio;
            if (isClipSelected(clipId)) {
                if (groupId.isEmpty()) m_selectedClipIds.erase(clipId);
                else removeClipGroupFromSelection(groupId);
                if (primary || !groupId.isEmpty()) selectFirstRemainingClip();
                else viewport()->update();
                emit clipSelectionChanged();
                if (m_selectedClipIds.empty()) emit trackLaneSelected(trackIndex);
            } else if (audio) {
                selectAudioClip(index, true);
            } else {
                selectClip(index, false, true);
            }
            clearClipDrag();
            event->accept();
            return;
        }
    }
    if (selectionModifier && trackIndexAt(scenePos) >= 0) {
        clearClipDrag();
        m_marqueeSelecting = true;
        m_marqueeAnchorPos = event->pos();
        m_marqueeCurrentPos = event->pos();
        m_marqueeBaseSelection = m_selectedClipIds;
        viewport()->update();
        event->accept();
        return;
    }
    int warpMarkerIndex = -1;
    const int warpClipIndex = audioWarpMarkerAt(scenePos, &warpMarkerIndex);
    if (warpClipIndex >= 0 && warpMarkerIndex >= 0) {
        selectAudioClip(warpClipIndex, true);
        if (m_audioClips[warpClipIndex].locked) {
            clearClipDrag();
            viewport()->unsetCursor();
            event->accept();
            return;
        }
        m_editingExistingClip = true;
        m_existingClipChanged = false;
        m_resizingExistingClip = false;
        m_resizingExistingClipFromLeft = false;
        m_audioFadeDrag = AudioFadeDrag::None;
        m_audioWarpDragMarker = warpMarkerIndex;
        m_audioWarpDragContentId = m_audioClips[warpClipIndex].contentId;
        m_duplicateOnDrag = false;
        m_dragStartPos = event->pos();
        m_pendingDragUndoState = std::make_unique<ArrangementState>(captureArrangementState());
        viewport()->setCursor(Qt::SizeHorCursor);
        event->accept();
        return;
    }
    AudioFadeDrag fadeDrag = AudioFadeDrag::None;
    const int fadeIndex = audioFadeHandleAt(scenePos, &fadeDrag);
    if (fadeIndex >= 0) {
        selectAudioClip(fadeIndex, true);
        if (m_audioClips[fadeIndex].locked) {
            clearClipDrag();
            viewport()->unsetCursor();
            event->accept();
            return;
        }
        m_editingExistingClip = true;
        m_existingClipChanged = false;
        m_resizingExistingClip = false;
        m_resizingExistingClipFromLeft = false;
        m_audioFadeDrag = fadeDrag;
        m_audioWarpDragMarker = -1;
        m_audioWarpDragContentId.clear();
        m_duplicateOnDrag = false;
        m_dragStartPos = event->pos();
        captureSelectedClipDragState();
        m_pendingDragUndoState = std::make_unique<ArrangementState>(captureArrangementState());
        viewport()->setCursor(Qt::SizeHorCursor);
        event->accept();
        return;
    }
    const int audioIndex = audioClipAt(scenePos);
    if (audioIndex >= 0) {
        selectAudioClip(audioIndex, isClipSelected(m_audioClips[audioIndex].clipId));
        if (selectionContainsLockedClip()) {
            clearClipDrag();
            viewport()->unsetCursor();
            event->accept();
            return;
        }
        m_editingExistingClip = true;
        m_existingClipChanged = false;
        const QRectF audioRect = clipRect(m_audioClips[audioIndex]);
        const double leftDistance = std::fabs(scenePos.x() - audioRect.left());
        const double rightDistance = std::fabs(scenePos.x() - audioRect.right());
        const bool canResizeRight = rightDistance <= 8.0;
        m_resizingExistingClip = leftDistance <= 8.0 || canResizeRight;
        m_resizingExistingClipFromLeft = m_resizingExistingClip &&
            (leftDistance <= 8.0 && (!canResizeRight || leftDistance < rightDistance));
        m_audioFadeDrag = AudioFadeDrag::None;
        m_audioWarpDragMarker = -1;
        m_audioWarpDragContentId.clear();
        m_duplicateOnDrag = !m_resizingExistingClip && (event->modifiers() & Qt::ShiftModifier);
        m_clipGrabOffsetBeat = beatAtX(scenePos.x()) - m_audioClips[audioIndex].startBeat;
        m_dragStartPos = event->pos();
        captureSelectedClipDragState();
        m_pendingDragUndoState = std::make_unique<ArrangementState>(captureArrangementState());
        viewport()->setCursor(m_resizingExistingClip ? Qt::SizeHorCursor : Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    const int clipIndex = clipAt(scenePos);
    if (clipIndex >= 0) {
        selectClip(clipIndex, false, isClipSelected(m_midiClips[clipIndex].clipId));
        if (selectionContainsLockedClip()) {
            clearClipDrag();
            viewport()->unsetCursor();
            event->accept();
            return;
        }
        m_editingExistingClip = true;
        m_existingClipChanged = false;
        const QRectF midiRect = clipRect(m_midiClips[clipIndex]);
        const double leftDistance = std::fabs(scenePos.x() - midiRect.left());
        const double rightDistance = std::fabs(scenePos.x() - midiRect.right());
        m_resizingExistingClip = qMin(leftDistance, rightDistance) <= 8.0;
        m_resizingExistingClipFromLeft = m_resizingExistingClip && leftDistance < rightDistance;
        m_audioFadeDrag = AudioFadeDrag::None;
        m_audioWarpDragMarker = -1;
        m_audioWarpDragContentId.clear();
        m_duplicateOnDrag = !m_resizingExistingClip && (event->modifiers() & Qt::ShiftModifier);
        m_clipGrabOffsetBeat = beatAtX(scenePos.x()) - m_midiClips[clipIndex].startBeat;
        m_dragStartPos = event->pos();
        captureSelectedClipDragState();
        m_pendingDragUndoState = std::make_unique<ArrangementState>(captureArrangementState());
        viewport()->setCursor(m_resizingExistingClip ? Qt::SizeHorCursor : Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    const int trackIndex = trackIndexAt(scenePos);
    if (trackIndex >= 0) {
        m_selectedClipIndex = -1;
        m_selectedClipIsAudio = false;
        m_selectedClipIds.clear();
        emit clipSelectionChanged();
        emit trackLaneSelected(trackIndex);

        if (isMidiTrack(trackIndex)) {
            m_pendingClipDrag = true;
            m_draggingClipCreate = false;
            m_dragTrackIndex = trackIndex;
            m_dragStartBeat = snapBeatUnlessAlt(beatAtX(scenePos.x()), event->modifiers());
            m_dragCurrentBeat = m_dragStartBeat;
            m_dragStartPos = event->pos();
        }

        viewport()->update();
        event->accept();
        return;
    }

    clearClipDrag();
    QGraphicsView::mousePressEvent(event);
}

void PlaylistView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_pluginAutomationDragPoint >= 0 && (event->buttons() & Qt::LeftButton)) {
        m_pluginAutomationDragPoint = movePluginAutomationPoint(
            m_pluginAutomationDragPoint, mapToScene(event->pos()), event->modifiers());
        m_pluginAutomationDragChanged = m_pluginAutomationDragPoint >= 0;
        updateSceneBounds();
        viewport()->update();
        event->accept();
        return;
    }
    if (m_timeSelectionDragging && (event->buttons() & Qt::LeftButton)) {
        const double beat = snapBeatUnlessAlt(
            beatAtX(mapToScene(event->pos()).x()), event->modifiers());
        if (beat < m_timeSelectionAnchorBeat) {
            m_timeSelectionStartBeat = qMax(0.0,
                qMin(beat, m_timeSelectionAnchorBeat - 0.25));
            m_timeSelectionEndBeat = qMax(
                m_timeSelectionAnchorBeat, m_timeSelectionStartBeat + 0.25);
        } else {
            m_timeSelectionStartBeat = m_timeSelectionAnchorBeat;
            m_timeSelectionEndBeat = qMax(beat, m_timeSelectionAnchorBeat + 0.25);
        }
        m_timeSelectionEnabled = true;
        viewport()->update();
        event->accept();
        return;
    }
    if (m_marqueeSelecting && (event->buttons() & Qt::LeftButton)) {
        m_marqueeCurrentPos = event->pos();
        updateMarqueeSelection();
        viewport()->update();
        event->accept();
        return;
    }
    if (m_editingExistingClip && (event->buttons() & Qt::LeftButton)) {
        if ((event->pos() - m_dragStartPos).manhattanLength() <= 3) {
            event->accept();
            return;
        }
        if (m_duplicateOnDrag && !m_resizingExistingClip &&
            m_audioFadeDrag == AudioFadeDrag::None && m_audioWarpDragMarker < 0) {
            double originalStartBeat = 0.0;
            if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
                m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
                originalStartBeat = m_audioClips[m_selectedClipIndex].startBeat;
            } else if (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
                       m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
                originalStartBeat = m_midiClips[m_selectedClipIndex].startBeat;
            }
            const bool previousSuppressUndo = m_suppressUndoCapture;
            m_suppressUndoCapture = true;
            const bool duplicated = duplicateSelectedClip();
            m_suppressUndoCapture = previousSuppressUndo;
            if (duplicated) {
                double duplicatedPrimaryStart = originalStartBeat;
                if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
                    m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
                    duplicatedPrimaryStart = m_audioClips[m_selectedClipIndex].startBeat;
                } else if (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
                           m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
                    duplicatedPrimaryStart = m_midiClips[m_selectedClipIndex].startBeat;
                }
                const double restoreDelta = originalStartBeat - duplicatedPrimaryStart;
                for (AudioClip& clip : m_audioClips)
                    if (isClipSelected(clip.clipId)) clip.startBeat += restoreDelta;
                for (MidiClip& clip : m_midiClips)
                    if (isClipSelected(clip.clipId)) clip.startBeat += restoreDelta;
                captureSelectedClipDragState();
            }
            m_duplicateOnDrag = false;
        }
        const QPointF scenePos = mapToScene(event->pos());
        if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
            m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
            AudioClip& clip = m_audioClips[m_selectedClipIndex];
            if (m_audioWarpDragMarker >= 0 &&
                clip.contentId == m_audioWarpDragContentId) {
                const QRectF rect = clipRect(clip);
                double displayedBeat = clip.sourceOffsetBeats +
                    (scenePos.x() - rect.left()) / m_pixelsPerBeat;
                double targetBeat = clip.reverse
                    ? clip.warpLengthBeats - displayedBeat : displayedBeat;
                targetBeat = snapBeatUnlessAlt(targetBeat, event->modifiers());
                if (updateSelectedAudioClipWarpMarker(
                        m_audioWarpDragMarker, targetBeat, false))
                    m_existingClipChanged = true;
            } else if (m_audioFadeDrag != AudioFadeDrag::None) {
                const double pointerBeat = snapBeatUnlessAlt(beatAtX(scenePos.x()), event->modifiers());
                if (m_audioFadeDrag == AudioFadeDrag::FadeIn) {
                    clip.fadeInBeats = qBound(0.0, pointerBeat - clip.startBeat,
                                              clip.lengthBeats - clip.fadeOutBeats);
                } else {
                    const double clipEnd = clip.startBeat + clip.lengthBeats;
                    clip.fadeOutBeats = qBound(0.0, clipEnd - pointerBeat,
                                               clip.lengthBeats - clip.fadeInBeats);
                }
                clampAudioClipFades(clip);
                m_existingClipChanged = true;
            } else if (m_resizingExistingClip) {
                resizeSelectedClipsFromDrag(
                    snapBeatUnlessAlt(beatAtX(scenePos.x()), event->modifiers()));
                m_existingClipChanged = true;
            } else {
                moveSelectedClipsFromDrag(scenePos, event->modifiers());
                m_existingClipChanged = true;
            }
        } else if (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
                   m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
            MidiClip& clip = m_midiClips[m_selectedClipIndex];
            if (m_resizingExistingClip) {
                resizeSelectedClipsFromDrag(
                    snapBeatUnlessAlt(beatAtX(scenePos.x()), event->modifiers()));
            } else {
                moveSelectedClipsFromDrag(scenePos, event->modifiers());
            }
            m_existingClipChanged = true;
        }
        updateSceneBounds();
        viewport()->update();
        event->accept();
        return;
    }

    if (m_loopDragging && (event->buttons() & Qt::LeftButton)) {
        const double beat = snapBeatUnlessAlt(beatAtX(mapToScene(event->pos()).x()), event->modifiers());
        const double anchor = m_loopDragAnchorBeat;
        m_loopStartBeat = qMin(anchor, beat);
        m_loopEndBeat = qMax(anchor, beat);
        if (m_loopEndBeat - m_loopStartBeat < 0.25) m_loopEndBeat = m_loopStartBeat + 0.25;
        viewport()->update();
        event->accept();
        return;
    }

    if (m_panning && (event->buttons() & Qt::MiddleButton)) {
        const QPoint delta = event->pos() - m_lastPanPos;
        m_lastPanPos = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        event->accept();
        return;
    }

    if (!m_pendingClipDrag || !(event->buttons() & Qt::LeftButton)) {
        if (event->buttons() == Qt::NoButton) {
            const QPointF scenePos = mapToScene(event->pos());
            AudioFadeDrag fadeHover = AudioFadeDrag::None;
            const bool audioFade = audioFadeHandleAt(scenePos, &fadeHover) >= 0;
            int warpHoverMarker = -1;
            const bool audioWarp = audioWarpMarkerAt(scenePos, &warpHoverMarker) >= 0;
            const int audioIndex = audioClipAt(scenePos);
            const int midiIndex = clipAt(scenePos);
            const bool audioResize = audioIndex >= 0 &&
                !m_audioClips[audioIndex].locked &&
                (std::fabs(scenePos.x() - clipRect(m_audioClips[audioIndex]).left()) <= 8.0 ||
                 std::fabs(scenePos.x() - clipRect(m_audioClips[audioIndex]).right()) <= 8.0);
            const bool midiResize = midiIndex >= 0 &&
                !m_midiClips[midiIndex].locked &&
                qMin(std::fabs(scenePos.x() - clipRect(m_midiClips[midiIndex]).left()),
                     std::fabs(scenePos.x() - clipRect(m_midiClips[midiIndex]).right())) <= 8.0;
            if (audioWarp || audioFade || audioResize || midiResize)
                viewport()->setCursor(Qt::SizeHorCursor);
            else viewport()->unsetCursor();
        }
        QGraphicsView::mouseMoveEvent(event);
        return;
    }

    if ((event->pos() - m_dragStartPos).manhattanLength() > 3) {
        m_draggingClipCreate = true;
    }

    if (m_draggingClipCreate) {
        m_dragCurrentBeat = snapBeatUnlessAlt(beatAtX(mapToScene(event->pos()).x()), event->modifiers());
        viewport()->update();
    }

    event->accept();
}

void PlaylistView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_pluginAutomationDragPoint >= 0) {
        const bool changed = m_pluginAutomationDragChanged;
        m_pluginAutomationDragPoint = -1;
        m_pluginAutomationDragChanged = false;
        viewport()->unsetCursor();
        if (changed) {
            thinPluginAutomationPoints();
            emitPluginAutomationEdit();
            updateSceneBounds();
            viewport()->update();
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_timeSelectionDragging) {
        m_timeSelectionDragging = false;
        setTimeSelection(true, m_timeSelectionStartBeat, m_timeSelectionEndBeat);
        emit timeSelectionChanged(
            m_timeSelectionEnabled, m_timeSelectionStartBeat, m_timeSelectionEndBeat);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_marqueeSelecting) {
        m_marqueeCurrentPos = event->pos();
        updateMarqueeSelection();
        m_marqueeSelecting = false;
        m_marqueeBaseSelection.clear();
        bool primarySelected = false;
        if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
            m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
            primarySelected = isClipSelected(m_audioClips[m_selectedClipIndex].clipId);
        } else if (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
                   m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
            primarySelected = isClipSelected(m_midiClips[m_selectedClipIndex].clipId);
        }
        if (!primarySelected) selectFirstRemainingClip();
        emit clipSelectionChanged();
        if (m_selectedClipIds.empty()) {
            const int trackIndex = trackIndexAt(mapToScene(m_marqueeAnchorPos));
            if (trackIndex >= 0) emit trackLaneSelected(trackIndex);
        }
        viewport()->update();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_editingExistingClip) {
        const bool changed = m_existingClipChanged;
        const bool structuralAudioEdit = m_selectedClipIsAudio &&
            m_audioFadeDrag == AudioFadeDrag::None && m_audioWarpDragMarker < 0;
        m_editingExistingClip = false;
        m_existingClipChanged = false;
        m_resizingExistingClip = false;
        m_resizingExistingClipFromLeft = false;
        m_audioFadeDrag = AudioFadeDrag::None;
        m_audioWarpDragMarker = -1;
        m_audioWarpDragContentId.clear();
        m_duplicateOnDrag = false;
        viewport()->unsetCursor();
        if (changed) {
            if (structuralAudioEdit) applyAutoCrossfadesForSelectedAudioClips();
            if (m_pendingDragUndoState)
                pushUndoState(std::move(*m_pendingDragUndoState));
            if (m_selectedClipIsAudio) selectAudioClip(m_selectedClipIndex, true);
            else selectClip(m_selectedClipIndex, false, true);
            emit contentChanged();
        }
        m_draggedMidiClipStates.clear();
        m_draggedAudioClipStates.clear();
        m_pendingDragUndoState.reset();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_loopDragging) {
        m_loopDragging = false;
        setLoopRange(true, m_loopStartBeat, m_loopEndBeat);
        emit loopRangeChanged(m_loopEnabled, m_loopStartBeat, m_loopEndBeat);
        event->accept();
        return;
    }

    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        viewport()->unsetCursor();
        event->accept();
        return;
    }

    if (!m_pendingClipDrag || event->button() != Qt::LeftButton) {
        QGraphicsView::mouseReleaseEvent(event);
        return;
    }

    if (m_draggingClipCreate && m_dragTrackIndex >= 0) {
        double start = qMin(m_dragStartBeat, m_dragCurrentBeat);
        double end = qMax(m_dragStartBeat, m_dragCurrentBeat);
        if (end - start < 1.0) {
            end = start + 1.0;
        }

        pushUndoState();
        const int clipIndex = createMidiClip(m_dragTrackIndex, start, end - start, QString());
        selectClip(clipIndex, false);
    }

    clearClipDrag();
    event->accept();
}

void PlaylistView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QGraphicsView::mouseDoubleClickEvent(event);
        return;
    }

    const QPointF scenePos = mapToScene(event->pos());
    const int clipIndex = clipAt(scenePos);
    if (clipIndex >= 0) {
        selectClip(clipIndex, true);
        event->accept();
        return;
    }

    const int trackIndex = trackIndexAt(scenePos);
    if (isMidiTrack(trackIndex)) {
        const double startBeat = snapBeat(beatAtX(scenePos.x()));
        pushUndoState();
        const int newClipIndex = createMidiClip(trackIndex, startBeat, qMax(1.0, m_beatsPerBar), QString());
        selectClip(newClipIndex, true);
        event->accept();
        return;
    }

    QGraphicsView::mouseDoubleClickEvent(event);
}

void PlaylistView::keyPressEvent(QKeyEvent* event)
{
    const bool commandModifier = event->modifiers() == Qt::ControlModifier ||
        event->modifiers() == Qt::MetaModifier;
    if (event->matches(QKeySequence::Undo) && undoArrangementEdit()) {
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Redo) && redoArrangementEdit()) {
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Copy) && copySelectedClips()) {
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Cut) && cutSelectedClips()) {
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Paste) && pasteClips()) {
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_0 && event->modifiers() == Qt::NoModifier &&
        toggleSelectedClipsMuted()) {
        event->accept();
        return;
    }
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const bool commandShift = modifiers.testFlag(Qt::ShiftModifier) &&
        !modifiers.testFlag(Qt::AltModifier) &&
        (modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::MetaModifier));
    if (event->key() == Qt::Key_G && commandShift && ungroupSelectedClips()) {
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_G && commandModifier && groupSelectedClips()) {
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_L && commandShift && toggleSelectedClipsLocked()) {
        event->accept();
        return;
    }
    const bool commandAlt = modifiers.testFlag(Qt::AltModifier) &&
        !modifiers.testFlag(Qt::ShiftModifier) &&
        (modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::MetaModifier));
    if (event->key() == Qt::Key_L && commandAlt && toggleSelectedClipsLoopEnabled()) {
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_X && commandAlt) {
        m_autoCrossfadeEnabled = !m_autoCrossfadeEnabled;
        QSettings().setValue(QStringLiteral("Editing/AutoCrossfades"),
                             m_autoCrossfadeEnabled);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_X && event->modifiers() == Qt::NoModifier &&
        createCrossfadeForSelectedAudioClips()) {
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        commandAlt && flattenSelectedTakeGroup()) {
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        commandShift && compSelectedTakeInTimeSelection()) {
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        event->modifiers() == Qt::ShiftModifier && setSelectedClipAsActiveTake()) {
        event->accept();
        return;
    }
    if (event->modifiers() == Qt::ShiftModifier &&
        (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) &&
        cycleSelectedTake(event->key() == Qt::Key_Up ? -1 : 1)) {
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        commandModifier && !m_selectedClipIds.empty()) {
        double firstBeat = std::numeric_limits<double>::infinity();
        double lastBeat = 0.0;
        for (const MidiClip& clip : m_midiClips) {
            if (!isClipSelected(clip.clipId)) continue;
            firstBeat = qMin(firstBeat, clip.startBeat);
            lastBeat = qMax(lastBeat, clip.startBeat + clip.lengthBeats);
        }
        for (const AudioClip& clip : m_audioClips) {
            if (!isClipSelected(clip.clipId)) continue;
            firstBeat = qMin(firstBeat, clip.startBeat);
            lastBeat = qMax(lastBeat, clip.startBeat + clip.lengthBeats);
        }
        if (std::isfinite(firstBeat) && lastBeat - firstBeat >= 0.25) {
            setTimeSelection(true, firstBeat, lastBeat);
            emit timeSelectionChanged(true, firstBeat, lastBeat);
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier &&
        m_timeSelectionEnabled) {
        setTimeSelection(false, 0.0, 0.0);
        emit timeSelectionChanged(false, 0.0, 0.0);
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        m_selectedClipIds.clear();
        for (const MidiClip& clip : m_midiClips) m_selectedClipIds.insert(clip.clipId);
        for (const AudioClip& clip : m_audioClips) m_selectedClipIds.insert(clip.clipId);
        if (!m_selectedClipIds.empty()) {
            if (m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
                m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
                selectAudioClip(m_selectedClipIndex, true);
            } else if (!m_selectedClipIsAudio && m_selectedClipIndex >= 0 &&
                       m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
                selectClip(m_selectedClipIndex, false, true);
            } else {
                selectFirstRemainingClip();
            }
        } else {
            m_selectedClipIndex = -1;
            m_selectedClipIsAudio = false;
            viewport()->update();
            emit clipSelectionChanged();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier &&
        m_selectedClipIndex >= 0) {
        int trackIndex = -1;
        if (m_selectedClipIsAudio && m_selectedClipIndex < static_cast<int>(m_audioClips.size())) {
            trackIndex = m_audioClips[m_selectedClipIndex].trackIndex;
        } else if (!m_selectedClipIsAudio &&
                   m_selectedClipIndex < static_cast<int>(m_midiClips.size())) {
            trackIndex = m_midiClips[m_selectedClipIndex].trackIndex;
        }
        m_selectedClipIndex = -1;
        m_selectedClipIsAudio = false;
        m_selectedClipIds.clear();
        clearClipDrag();
        viewport()->update();
        emit clipSelectionChanged();
        if (trackIndex >= 0) emit trackLaneSelected(trackIndex);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_E && event->modifiers() == Qt::ControlModifier &&
        splitSelectedClipAtPlayhead()) {
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Insert && commandModifier && insertTimeSelection()) {
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete && commandModifier && deleteTimeSelection()) {
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
        event->modifiers() == Qt::NoModifier && deleteSelectedClip()) {
        event->accept();
        return;
    }
    if (event->modifiers() == Qt::NoModifier) {
        const double horizontalStep = m_snapBeats > 0.0
            ? m_snapBeats : 1.0 / qMax(10.0, m_pixelsPerBeat);
        bool changed = false;
        if (event->key() == Qt::Key_Left) changed = nudgeSelectedClip(-horizontalStep, 0);
        else if (event->key() == Qt::Key_Right) changed = nudgeSelectedClip(horizontalStep, 0);
        else if (event->key() == Qt::Key_Up) changed = nudgeSelectedClip(0.0, -1);
        else if (event->key() == Qt::Key_Down) changed = nudgeSelectedClip(0.0, 1);
        if (changed) {
            event->accept();
            return;
        }
    }
    if (event->modifiers() == Qt::AltModifier &&
        (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        const double pixelBeat = 1.0 / qMax(10.0, m_pixelsPerBeat);
        if (nudgeSelectedClip(event->key() == Qt::Key_Left ? -pixelBeat : pixelBeat, 0, true)) {
            event->accept();
            return;
        }
    }
    QGraphicsView::keyPressEvent(event);
}

int PlaylistView::trackIndexAt(const QPointF& scenePos) const
{
    const double laneY = scenePos.y() - m_timelineHeight;
    if (laneY < 0.0) {
        return -1;
    }

    const int trackIndex = qFloor(laneY / m_trackHeight);
    return (trackIndex >= 0 && trackIndex < m_trackCount) ? trackIndex : -1;
}

bool PlaylistView::isMidiTrack(int trackIndex) const
{
    return trackIndex >= 0 &&
           trackIndex < (int)m_tracks.size() &&
           m_tracks[trackIndex] &&
           m_tracks[trackIndex]->GetType() == TrackType::Midi;
}

bool PlaylistView::isAudioTrack(int trackIndex) const
{
    return trackIndex >= 0 &&
           trackIndex < (int)m_tracks.size() &&
           m_tracks[trackIndex] &&
           m_tracks[trackIndex]->GetType() == TrackType::Audio;
}

double PlaylistView::beatAtX(double sceneX) const
{
    return qMax(0.0, sceneX / m_pixelsPerBeat);
}

double PlaylistView::snapBeat(double beat) const
{
    if (m_snapBeats <= 0.0) return qMax(0.0, beat);
    return qMax(0.0, static_cast<double>(qRound(beat / m_snapBeats)) * m_snapBeats);
}

double PlaylistView::snapBeatUnlessAlt(double beat, Qt::KeyboardModifiers modifiers) const
{
    return (modifiers & Qt::AltModifier) ? qMax(0.0, beat) : snapBeat(beat);
}

QRectF PlaylistView::pluginAutomationRect() const
{
    if (!m_pluginAutomationVisible || m_pluginAutomationTrackIndex < 0 ||
        m_pluginAutomationTrackIndex >= m_trackCount) return {};
    const double y = m_timelineHeight + m_pluginAutomationTrackIndex * m_trackHeight +
        qFloor(static_cast<double>(m_trackHeight) * 0.53);
    return QRectF(0.0, y, qMax(sceneRect().width(), static_cast<qreal>(viewport()->width())),
                  qMax(20.0, static_cast<double>(m_trackHeight) * 0.47 - 3.0));
}

int PlaylistView::pluginAutomationPointAt(const QPointF& scenePos) const
{
    const QRectF lane = pluginAutomationRect();
    if (!lane.contains(scenePos)) return -1;
    const QRectF graph = lane.adjusted(0.0, 10.0, 0.0, -3.0);
    constexpr double radius = 7.0;
    for (int i = static_cast<int>(m_pluginAutomationLane.points.size()) - 1; i >= 0; --i) {
        const PluginAutomationPoint& point = m_pluginAutomationLane.points[static_cast<size_t>(i)];
        const QPointF center(point.beat * m_pixelsPerBeat,
            graph.bottom() - point.normalizedValue * graph.height());
        if (QLineF(center, scenePos).length() <= radius) return i;
    }
    return -1;
}

int PlaylistView::movePluginAutomationPoint(int pointIndex, const QPointF& scenePos,
                                            Qt::KeyboardModifiers modifiers)
{
    if (pointIndex < 0 || pointIndex >= static_cast<int>(m_pluginAutomationLane.points.size()))
        return -1;
    const QRectF graph = pluginAutomationRect().adjusted(0.0, 10.0, 0.0, -3.0);
    PluginAutomationPoint point =
        m_pluginAutomationLane.points[static_cast<size_t>(pointIndex)];
    point.beat = snapBeatUnlessAlt(beatAtX(scenePos.x()), modifiers);
    point.normalizedValue = graph.height() > 0.0
        ? std::clamp((graph.bottom() - scenePos.y()) / graph.height(), 0.0, 1.0) : 0.0;
    auto& points = m_pluginAutomationLane.points;
    points.erase(points.begin() + pointIndex);
    const auto position = std::upper_bound(points.begin(), points.end(), point.beat,
        [](double beat, const PluginAutomationPoint& candidate) { return beat < candidate.beat; });
    const int nextIndex = static_cast<int>(std::distance(points.begin(), position));
    points.insert(position, point);
    return nextIndex;
}

void PlaylistView::thinPluginAutomationPoints()
{
    auto& points = m_pluginAutomationLane.points;
    if (points.empty()) return;
    std::vector<PluginAutomationPoint> unique;
    unique.reserve(std::min(points.size(), kMaxPluginAutomationPointsPerLane));
    for (const PluginAutomationPoint& point : points) {
        if (!unique.empty() && std::fabs(unique.back().beat - point.beat) <= 1.0e-9)
            unique.back() = point;
        else if (unique.size() < kMaxPluginAutomationPointsPerLane)
            unique.push_back(point);
    }
    // Smooth segments are non-compositional: removing a point can alter extrema
    // between audit samples. Preserve every authored Smooth point exactly.
    if (m_pluginAutomationLane.interpolation == PluginAutomationInterpolation::Linear &&
        unique.size() > 2) {
        std::vector<PluginAutomationPoint> thinned;
        thinned.reserve(unique.size());
        thinned.push_back(unique.front());
        for (size_t i = 1; i + 1 < unique.size(); ++i) {
            const PluginAutomationPoint& left = thinned.back();
            const PluginAutomationPoint& point = unique[i];
            const PluginAutomationPoint& right = unique[i + 1];
            const double span = right.beat - left.beat;
            bool removable = false;
            if (span > 1.0e-12) {
                const double position = std::clamp(
                    (point.beat - left.beat) / span, 0.0, 1.0);
                const double expected = left.normalizedValue +
                    (right.normalizedValue - left.normalizedValue) * position;
                removable = !left.curvePolynomial && !point.curvePolynomial &&
                    std::fabs(point.curveTension) <= 1.0e-9 &&
                    std::fabs(expected - point.normalizedValue) <= 1.0e-5;
            }
            if (!removable)
                thinned.push_back(point);
        }
        thinned.push_back(unique.back());
        unique = std::move(thinned);
    }
    points = std::move(unique);
}

void PlaylistView::emitPluginAutomationEdit()
{
    emit pluginAutomationEdited(m_pluginAutomationTargetTrackIndex, m_pluginAutomationAudioFx,
                                m_pluginAutomationSlotIndex, m_pluginAutomationLane);
}

QRectF PlaylistView::clipRect(const MidiClip& clip) const
{
    return takeLaneRect(clip.startBeat, clip.lengthBeats, clip.trackIndex,
                        clip.takeGroupId, clip.takeLane, false);
}

QRectF PlaylistView::clipRect(const AudioClip& clip) const
{
    return takeLaneRect(clip.startBeat, clip.lengthBeats, clip.trackIndex,
                        clip.takeGroupId, clip.takeLane, true);
}

QRectF PlaylistView::takeLaneRect(double startBeat, double lengthBeats, int trackIndex,
                                  const QString& takeGroupId, int takeLane, bool audio) const
{
    const double x = startBeat * m_pixelsPerBeat;
    const double w = qMax(1.0, lengthBeats * m_pixelsPerBeat);
    const double baseY = m_timelineHeight + trackIndex * m_trackHeight + 7;
    const double availableHeight = qMax(2.0, static_cast<double>(m_trackHeight - 14));
    int laneCount = 1;
    if (!takeGroupId.isEmpty()) {
        if (audio) {
            for (const AudioClip& item : m_audioClips)
                if (item.takeGroupId == takeGroupId) laneCount = qMax(laneCount, item.takeLane + 1);
        } else {
            for (const MidiClip& item : m_midiClips)
                if (item.takeGroupId == takeGroupId) laneCount = qMax(laneCount, item.takeLane + 1);
        }
    }
    laneCount = qBound(1, laneCount, 16);
    const int lane = qBound(0, takeLane, laneCount - 1);
    const double laneHeight = availableHeight / laneCount;
    const double y = baseY + lane * laneHeight;
    const double h = qMax(2.0, laneHeight - (laneCount > 1 ? 1.0 : 0.0));
    return QRectF(x, y, w, h);
}

int PlaylistView::clipAt(const QPointF& scenePos) const
{
    for (int i = (int)m_midiClips.size() - 1; i >= 0; --i) {
        if (clipRect(m_midiClips[i]).contains(scenePos)) {
            return i;
        }
    }
    return -1;
}

int PlaylistView::audioClipAt(const QPointF& scenePos) const
{
    for (int i = (int)m_audioClips.size() - 1; i >= 0; --i) {
        if (clipRect(m_audioClips[i]).contains(scenePos)) {
            return i;
        }
    }
    return -1;
}

int PlaylistView::audioFadeHandleAt(const QPointF& scenePos, AudioFadeDrag* drag) const
{
    if (drag) *drag = AudioFadeDrag::None;
    constexpr double hitRadius = 7.0;
    for (int i = static_cast<int>(m_audioClips.size()) - 1; i >= 0; --i) {
        if (!m_selectedClipIsAudio || i != m_selectedClipIndex) continue;
        const AudioClip& clip = m_audioClips[i];
        const QRectF rect = clipRect(clip);
        if (scenePos.y() < rect.top() - hitRadius || scenePos.y() > rect.top() + 14.0 ||
            scenePos.x() < rect.left() - hitRadius || scenePos.x() > rect.right() + hitRadius) continue;
        const QPointF fadeInPoint(rect.left() + clip.fadeInBeats * m_pixelsPerBeat, rect.top() + 7.0);
        const QPointF fadeOutPoint(rect.right() - clip.fadeOutBeats * m_pixelsPerBeat, rect.top() + 7.0);
        const double inDistance = std::hypot(scenePos.x() - fadeInPoint.x(), scenePos.y() - fadeInPoint.y());
        const double outDistance = std::hypot(scenePos.x() - fadeOutPoint.x(), scenePos.y() - fadeOutPoint.y());
        if (qMin(inDistance, outDistance) > hitRadius) continue;
        if (drag) *drag = inDistance <= outDistance ? AudioFadeDrag::FadeIn : AudioFadeDrag::FadeOut;
        return i;
    }
    return -1;
}

int PlaylistView::audioWarpMarkerAt(const QPointF& scenePos, int* markerIndex) const
{
    if (markerIndex) *markerIndex = -1;
    if (!m_selectedClipIsAudio || m_selectedClipIndex < 0 ||
        m_selectedClipIndex >= static_cast<int>(m_audioClips.size())) return -1;
    const AudioClip& clip = m_audioClips[m_selectedClipIndex];
    const QRectF rect = clipRect(clip);
    constexpr double hitRadius = 7.0;
    if (scenePos.y() < rect.top() + 10.0 - hitRadius ||
        scenePos.y() > rect.top() + 22.0 + hitRadius ||
        scenePos.x() < rect.left() - hitRadius || scenePos.x() > rect.right() + hitRadius)
        return -1;
    double bestDistance = hitRadius + 1.0;
    int bestMarker = -1;
    for (int index = 0; index < static_cast<int>(clip.warpMarkers.size()); ++index) {
        const AudioWarpMarker& marker = clip.warpMarkers[static_cast<size_t>(index)];
        const double displayedBeat = clip.reverse
            ? clip.warpLengthBeats - marker.targetBeat : marker.targetBeat;
        if (displayedBeat < clip.sourceOffsetBeats ||
            displayedBeat > clip.sourceOffsetBeats + clip.lengthBeats) continue;
        const double x = rect.left() +
            (displayedBeat - clip.sourceOffsetBeats) * m_pixelsPerBeat;
        const double distance = std::hypot(scenePos.x() - x,
                                           scenePos.y() - (rect.top() + 17.0));
        if (distance < bestDistance) {
            bestDistance = distance;
            bestMarker = index;
        }
    }
    if (bestMarker < 0 || bestDistance > hitRadius) return -1;
    if (markerIndex) *markerIndex = bestMarker;
    return m_selectedClipIndex;
}

int PlaylistView::createMidiClip(int trackIndex, double startBeat, double lengthBeats, const QString& clipName,
                                 const QString& requestedClipId, const QString& requestedContentId,
                                 double sourceOffsetBeats, bool muted)
{
    if (!isMidiTrack(trackIndex)) {
        return -1;
    }

    MidiClip clip;
    clip.clipId = requestedClipId.trimmed().isEmpty() ? stableClipId() : requestedClipId.trimmed();
    clip.contentId = requestedContentId.trimmed().isEmpty() ? stableClipId() : requestedContentId.trimmed();
    clip.trackIndex = trackIndex;
    clip.startBeat = qMax(0.0, startBeat);
    clip.lengthBeats = qMax(0.25, lengthBeats);
    clip.sourceOffsetBeats = std::isfinite(sourceOffsetBeats) ? qMax(0.0, sourceOffsetBeats) : 0.0;
    clip.isUnique = true;
    clip.muted = muted;
    if (clipName.isEmpty()) {
        clip.name = QString("MIDI %1").arg((int)m_midiClips.size() + 1);
    } else {
        clip.name = clipName;
    }
    m_midiClips.push_back(clip);
    addContentReference(clip.contentId, false);

    updateSceneBounds();
    viewport()->update();
    emit contentChanged();
    return (int)m_midiClips.size() - 1;
}

int PlaylistView::createAudioClip(int trackIndex, double startBeat, double lengthBeats, const QString& clipName, const QString& sourcePath,
                                  double sourceTempoBpm, double pitchShiftSemitones, double beatsPerBar, double sourceDurationSeconds,
                                  const QString& requestedClipId, const QString& requestedContentId,
                                  double sourceOffsetBeats, double warpLengthBeats, double tempoConfidence,
                                  int sourceSignatureNumerator, int sourceSignatureDenominator, bool tempoDetected,
                                  const std::vector<double>& transientSeconds,
                                  const std::vector<AudioWarpMarker>& warpMarkers,
                                  const std::vector<AudioGainEnvelopePoint>& gainEnvelope,
                                  bool muted)
{
    if (!isAudioTrack(trackIndex)) {
        return -1;
    }

    AudioClip clip;
    clip.clipId = requestedClipId.trimmed().isEmpty() ? stableClipId() : requestedClipId.trimmed();
    clip.contentId = requestedContentId.trimmed().isEmpty() ? stableClipId() : requestedContentId.trimmed();
    clip.trackIndex = trackIndex;
    clip.startBeat = qMax(0.0, startBeat);
    clip.lengthBeats = qMax(0.25, lengthBeats);
    clip.sourceOffsetBeats = std::isfinite(sourceOffsetBeats) ? qMax(0.0, sourceOffsetBeats) : 0.0;
    const double requestedWarp = std::isfinite(warpLengthBeats) && warpLengthBeats > 0.0
        ? warpLengthBeats
        : clip.sourceOffsetBeats + clip.lengthBeats;
    clip.warpLengthBeats = qMax(clip.sourceOffsetBeats + clip.lengthBeats, requestedWarp);
    clip.sourceTempoBpm = qBound(0.0, sourceTempoBpm, 300.0);
    clip.tempoConfidence = tempoDetected && std::isfinite(tempoConfidence)
        ? qBound(0.0, tempoConfidence, 1.0) : 0.0;
    clip.sourceSignatureNumerator = qBound(0, sourceSignatureNumerator, 32);
    const bool validSourceDenominator = sourceSignatureDenominator >= 1 &&
        sourceSignatureDenominator <= 64 &&
        (sourceSignatureDenominator & (sourceSignatureDenominator - 1)) == 0;
    clip.sourceSignatureDenominator = validSourceDenominator ? sourceSignatureDenominator : 0;
    clip.tempoDetected = tempoDetected && clip.sourceTempoBpm > 0.0;
    clip.transientSeconds = normalizedTransientSeconds(transientSeconds, sourceDurationSeconds);
    clip.warpMarkers = normalizedWarpMarkers(warpMarkers, sourceDurationSeconds,
                                             clip.warpLengthBeats);
    clip.pitchShiftSemitones = qBound(-48.0, pitchShiftSemitones, 48.0);
    clip.syncMode = clip.sourceTempoBpm > 0.0 ? 1 : 0;
    clip.targetBars = 0.0;
    clip.beatsPerBar = qBound(0.25, beatsPerBar, 64.0);
    clip.sourceDurationSeconds = std::isfinite(sourceDurationSeconds) && sourceDurationSeconds > 0.0
        ? sourceDurationSeconds
        : 0.0;
    clip.gain = 1.0f;
    clip.pan = 0.0f;
    clip.normalize = false;
    clip.reverse = false;
    clip.reversePolarity = false;
    clip.fadeInBeats = 0.0;
    clip.fadeOutBeats = 0.0;
    clip.gainEnvelope = normalizedGainEnvelope(gainEnvelope, clip.lengthBeats);
    clip.isUnique = true;
    clip.muted = muted;
    clip.sourcePath = sourcePath;
    clip.mediaOffline = !AudioMediaResolver::isOnline(sourcePath);
    clip.name = clipName.isEmpty() ? QFileInfo(sourcePath).completeBaseName() : clipName;
    if (clip.name.isEmpty()) {
        clip.name = QString("Audio %1").arg((int)m_audioClips.size() + 1);
    }
    for (const AudioClip& existing : m_audioClips) {
        if (existing.contentId != clip.contentId) continue;
        clip.sourceTempoBpm = existing.sourceTempoBpm;
        clip.tempoConfidence = existing.tempoConfidence;
        clip.sourceSignatureNumerator = existing.sourceSignatureNumerator;
        clip.sourceSignatureDenominator = existing.sourceSignatureDenominator;
        clip.tempoDetected = existing.tempoDetected;
        clip.transientSeconds = existing.transientSeconds;
        clip.warpMarkers = existing.warpMarkers;
        clip.pitchShiftSemitones = existing.pitchShiftSemitones;
        clip.syncMode = existing.syncMode;
        clip.targetBars = existing.targetBars;
        clip.beatsPerBar = existing.beatsPerBar;
        clip.sourceDurationSeconds = existing.sourceDurationSeconds;
        clip.sourcePath = existing.sourcePath;
        clip.mediaOffline = existing.mediaOffline;
        clip.name = existing.name;
        break;
    }
    m_audioClips.push_back(clip);
    addContentReference(clip.contentId, true);

    updateSceneBounds();
    viewport()->update();
    emit contentChanged();
    return (int)m_audioClips.size() - 1;
}

bool PlaylistView::isClipSelected(const QString& clipId) const
{
    return !clipId.isEmpty() && m_selectedClipIds.find(clipId) != m_selectedClipIds.end();
}

void PlaylistView::addClipGroupToSelection(const QString& groupId)
{
    if (groupId.isEmpty()) return;
    for (const MidiClip& clip : m_midiClips)
        if (clip.groupId == groupId) m_selectedClipIds.insert(clip.clipId);
    for (const AudioClip& clip : m_audioClips)
        if (clip.groupId == groupId) m_selectedClipIds.insert(clip.clipId);
}

void PlaylistView::removeClipGroupFromSelection(const QString& groupId)
{
    if (groupId.isEmpty()) return;
    for (const MidiClip& clip : m_midiClips)
        if (clip.groupId == groupId) m_selectedClipIds.erase(clip.clipId);
    for (const AudioClip& clip : m_audioClips)
        if (clip.groupId == groupId) m_selectedClipIds.erase(clip.clipId);
}

void PlaylistView::expandSelectionToClipGroups()
{
    std::set<QString> groups;
    for (const MidiClip& clip : m_midiClips)
        if (isClipSelected(clip.clipId) && !clip.groupId.isEmpty()) groups.insert(clip.groupId);
    for (const AudioClip& clip : m_audioClips)
        if (isClipSelected(clip.clipId) && !clip.groupId.isEmpty()) groups.insert(clip.groupId);
    if (groups.empty()) return;
    for (const MidiClip& clip : m_midiClips)
        if (groups.find(clip.groupId) != groups.end()) m_selectedClipIds.insert(clip.clipId);
    for (const AudioClip& clip : m_audioClips)
        if (groups.find(clip.groupId) != groups.end()) m_selectedClipIds.insert(clip.clipId);
}

bool PlaylistView::selectionContainsLockedClip() const
{
    for (const MidiClip& clip : m_midiClips)
        if (clip.locked && isClipSelected(clip.clipId)) return true;
    for (const AudioClip& clip : m_audioClips)
        if (clip.locked && isClipSelected(clip.clipId)) return true;
    return false;
}

void PlaylistView::selectFirstRemainingClip()
{
    for (int index = 0; index < static_cast<int>(m_midiClips.size()); ++index) {
        if (!isClipSelected(m_midiClips[index].clipId)) continue;
        selectClip(index, false, true);
        return;
    }
    for (int index = 0; index < static_cast<int>(m_audioClips.size()); ++index) {
        if (!isClipSelected(m_audioClips[index].clipId)) continue;
        selectAudioClip(index, true);
        return;
    }
    m_selectedClipIndex = -1;
    m_selectedClipIsAudio = false;
    m_selectedClipIds.clear();
    viewport()->update();
    emit clipSelectionChanged();
}

void PlaylistView::restorePrimaryClipSelection(const QString& clipId, bool audio)
{
    if (!clipId.isEmpty() && isClipSelected(clipId)) {
        if (audio) {
            for (int index = 0; index < static_cast<int>(m_audioClips.size()); ++index) {
                if (m_audioClips[index].clipId != clipId) continue;
                selectAudioClip(index, true);
                return;
            }
        } else {
            for (int index = 0; index < static_cast<int>(m_midiClips.size()); ++index) {
                if (m_midiClips[index].clipId != clipId) continue;
                selectClip(index, false, true);
                return;
            }
        }
    }
    selectFirstRemainingClip();
}

void PlaylistView::selectClip(int clipIndex, bool opened, bool preserveSelection)
{
    if (clipIndex < 0 || clipIndex >= (int)m_midiClips.size()) {
        return;
    }

    if (!preserveSelection) m_selectedClipIds.clear();
    m_selectedClipIndex = clipIndex;
    m_selectedClipIsAudio = false;
    const MidiClip& clip = m_midiClips[clipIndex];
    m_selectedClipIds.insert(clip.clipId);
    addClipGroupToSelection(clip.groupId);
    emit trackLaneSelected(clip.trackIndex);
    emit midiClipSelected(clip.trackIndex, clip.name, clip.startBeat, clip.lengthBeats);
    if (opened) {
        emit midiClipOpened(clip.trackIndex, clip.name, clip.startBeat, clip.lengthBeats);
    }
    emit clipSelectionChanged();
    viewport()->update();
}

void PlaylistView::selectAudioClip(int clipIndex, bool preserveSelection)
{
    if (clipIndex < 0 || clipIndex >= (int)m_audioClips.size()) {
        return;
    }

    if (!preserveSelection) m_selectedClipIds.clear();
    m_selectedClipIndex = clipIndex;
    m_selectedClipIsAudio = true;
    const AudioClip& clip = m_audioClips[clipIndex];
    m_selectedClipIds.insert(clip.clipId);
    addClipGroupToSelection(clip.groupId);
    emit trackLaneSelected(clip.trackIndex);
    emit audioClipSelected(clip.trackIndex, clip.name, clip.startBeat, clip.lengthBeats, clip.sourcePath,
                           clip.sourceTempoBpm, clip.pitchShiftSemitones, clip.syncMode, clip.targetBars);
    emit clipSelectionChanged();
    viewport()->update();
}

void PlaylistView::clearClipDrag()
{
    m_pendingClipDrag = false;
    m_timeSelectionDragging = false;
    m_marqueeSelecting = false;
    m_marqueeBaseSelection.clear();
    m_draggingClipCreate = false;
    m_dragTrackIndex = -1;
    m_dragStartBeat = 0.0;
    m_dragCurrentBeat = 0.0;
    m_editingExistingClip = false;
    m_existingClipChanged = false;
    m_resizingExistingClip = false;
    m_resizingExistingClipFromLeft = false;
    m_audioFadeDrag = AudioFadeDrag::None;
    m_audioWarpDragMarker = -1;
    m_audioWarpDragContentId.clear();
    m_duplicateOnDrag = false;
    m_clipGrabOffsetBeat = 0.0;
    m_draggedMidiClipStates.clear();
    m_draggedAudioClipStates.clear();
    m_pendingDragUndoState.reset();
}
