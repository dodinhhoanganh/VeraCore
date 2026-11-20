#include "AudioEditorWidget.h"
#include "AudioFileDecoder.h"
#include <QContextMenuEvent>
#include <QDateTime>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineF>
#include <QMetaObject>
#include <QMenu>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QThreadPool>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int kWaveformBins = 4096;
constexpr double kMinimumMarkerSpacing = 1.0 / 960.0;
constexpr int kWaveCacheEntries = 16;
}

QMutex AudioEditorWidget::s_waveCacheMutex;
QHash<QString, QVector<AudioEditorWidget::WavePeak>> AudioEditorWidget::s_waveCache;
QStringList AudioEditorWidget::s_waveCacheOrder;

AudioEditorWidget::AudioEditorWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("audioEditorWidget"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumHeight(180);
    setAccessibleName(tr("Audio clip waveform editor"));
    setAccessibleDescription(tr("Arrow keys nudge warp markers; Alt fine-nudges; Delete removes; Enter toggles a transient anchor."));
}

AudioEditorWidget::~AudioEditorWidget() { ++m_loadGeneration; }

double AudioEditorWidget::shapedFade(double progress, double curve)
{
    constexpr double halfPi = 1.57079632679489661923;
    const double base = std::sin(halfPi * std::clamp(progress, 0.0, 1.0));
    const double safeCurve = std::clamp(std::isfinite(curve) ? curve : 0.0, -1.0, 1.0);
    if (safeCurve < 0.0)
        return base + (std::sqrt(base) - base) * -safeCurve;
    return base + (base * base - base) * safeCurve;
}

void AudioEditorWidget::setClip(const PlaylistView::AudioClipInfo& clip)
{
    const bool newContent = !m_hasClip || m_clip.contentId != clip.contentId;
    const bool newPath = m_loadedPath != clip.sourcePath;
    m_clip = clip;
    m_hasClip = true;
    if (newContent) {
        m_selectedWarpMarker = -1;
        m_selectedTransient = -1;
        m_selectedGainPoint = -1;
        m_draggingWarpMarker = false;
        m_dragMode = DragMode::None;
        resetView();
    } else if (m_selectedWarpMarker >= static_cast<int>(m_clip.warpMarkers.size())) {
        m_selectedWarpMarker = -1;
    }
    if (m_selectedGainPoint >= static_cast<int>(m_clip.gainEnvelope.size()))
        m_selectedGainPoint = -1;
    if (newPath) requestWaveform(clip.sourcePath);
    update();
}

void AudioEditorWidget::clearClip()
{
    ++m_loadGeneration;
    m_hasClip = false;
    m_clip = {};
    m_wavePeaks.clear();
    m_waveformError.clear();
    m_loadedPath.clear();
    m_selectedWarpMarker = -1;
    m_selectedTransient = -1;
    m_selectedGainPoint = -1;
    m_draggingWarpMarker = false;
    m_dragMode = DragMode::None;
    m_previewGainEnvelope.clear();
    m_viewStartBeat = 0.0;
    m_viewSpanBeats = 1.0;
    update();
}

void AudioEditorWidget::setSnapBeats(double beats)
{
    m_snapBeats = std::isfinite(beats) ? std::clamp(beats, 0.0, 128.0) : 0.25;
}

void AudioEditorWidget::setPlayheadBeat(double projectBeat)
{
    if (m_playheadBeat == projectBeat) return;
    const double previousBeat = m_playheadBeat;
    m_playheadBeat = projectBeat;
    const QRectF area = waveformRect();
    const auto invalidate = [this, area](double projectPosition) {
        if (!m_hasClip || projectPosition < m_clip.startBeat ||
            projectPosition > m_clip.startBeat + m_clip.lengthBeats) return;
        const double local = projectPosition - m_clip.startBeat + m_clip.sourceOffsetBeats;
        const int x = qRound(xForBeat(local));
        update(QRect(x - 3, qFloor(area.top()), 7, qCeil(area.height())));
    };
    invalidate(previousBeat);
    invalidate(m_playheadBeat);
}

void AudioEditorWidget::setLoopRange(bool enabled, double startBeat, double endBeat)
{
    const bool valid = enabled && std::isfinite(startBeat) && std::isfinite(endBeat) &&
        startBeat >= 0.0 && endBeat - startBeat >= 0.25;
    m_loopEnabled = valid;
    m_loopStartBeat = valid ? startBeat : 0.0;
    m_loopEndBeat = valid ? endBeat : 4.0;
    update();
}

double AudioEditorWidget::projectBeatForEditorBeat(double beat) const
{
    return std::max(0.0, m_clip.startBeat + beat - m_clip.sourceOffsetBeats);
}

void AudioEditorWidget::setWaveformMode(WaveformMode mode)
{
    if (m_waveformMode == mode) return;
    m_waveformMode = mode;
    const QString modeName = mode == WaveformMode::Stereo ? tr("stereo split") :
        mode == WaveformMode::Overlay ? tr("stereo overlay") : tr("mono sum");
    setAccessibleDescription(tr("Waveform mode: %1. Arrow keys edit warp markers; Ctrl-drag sets loop; Alt-drag scrubs.").arg(modeName));
    update();
}

float AudioEditorWidget::gainEnvelopeValueAt(
    double localBeat, const std::vector<AudioGainEnvelopePoint>& points) const
{
    const double length = std::max(0.0, m_clip.lengthBeats);
    const double beat = std::clamp(localBeat, 0.0, length);
    if (points.empty() || length <= 0.0) return 1.0f;
    auto interpolate = [](double position, double leftBeat, float leftGain,
                          double rightBeat, float rightGain) {
        const double span = rightBeat - leftBeat;
        const double amount = span > 0.0
            ? std::clamp((position - leftBeat) / span, 0.0, 1.0) : 0.0;
        return static_cast<float>(leftGain + (rightGain - leftGain) * amount);
    };
    if (beat <= points.front().beat)
        return points.front().beat <= 0.0 ? points.front().gain :
            interpolate(beat, 0.0, 1.0f, points.front().beat, points.front().gain);
    const auto right = std::upper_bound(points.begin(), points.end(), beat,
        [](double position, const AudioGainEnvelopePoint& point) {
            return position < point.beat;
        });
    if (right == points.end())
        return points.back().beat >= length ? points.back().gain :
            interpolate(beat, points.back().beat, points.back().gain, length, 1.0f);
    const AudioGainEnvelopePoint& left = *(right - 1);
    return interpolate(beat, left.beat, left.gain, right->beat, right->gain);
}

double AudioEditorWidget::gainEnvelopeY(float gain) const
{
    constexpr double minimumDb = -60.0;
    constexpr double maximumDb = 12.0;
    const double safeGain = std::clamp(static_cast<double>(gain), 0.0, 4.0);
    const double db = safeGain <= 0.001 ? minimumDb :
        std::clamp(20.0 * std::log10(safeGain), minimumDb, maximumDb);
    const QRectF area = waveformRect();
    return area.bottom() - (db - minimumDb) / (maximumDb - minimumDb) * area.height();
}

float AudioEditorWidget::gainEnvelopeAtY(double y) const
{
    constexpr double minimumDb = -60.0;
    constexpr double maximumDb = 12.0;
    const QRectF area = waveformRect();
    const double amount = std::clamp((area.bottom() - y) / std::max(1.0, area.height()), 0.0, 1.0);
    const double db = minimumDb + amount * (maximumDb - minimumDb);
    return db <= minimumDb + 0.05 ? 0.0f :
        static_cast<float>(std::clamp(std::pow(10.0, db / 20.0), 0.0, 4.0));
}

int AudioEditorWidget::gainEnvelopePointAt(const QPointF& position) const
{
    if (!m_hasClip || !waveformRect().contains(position)) return -1;
    const auto& points = m_dragMode == DragMode::GainEnvelope && !m_previewGainEnvelope.empty()
        ? m_previewGainEnvelope : m_clip.gainEnvelope;
    for (int index = static_cast<int>(points.size()) - 1; index >= 0; --index) {
        const AudioGainEnvelopePoint& point = points[static_cast<size_t>(index)];
        const QPointF center(xForBeat(m_clip.sourceOffsetBeats + point.beat),
                             gainEnvelopeY(point.gain));
        if (QLineF(center, position).length() <= 8.0) return index;
    }
    return -1;
}

QRectF AudioEditorWidget::waveformRect() const
{
    return QRectF(18.0, 42.0, std::max(1, width() - 36), std::max(1, height() - 62));
}

double AudioEditorWidget::beatAtX(double x) const
{
    const QRectF area = waveformRect();
    return m_viewStartBeat +
        std::clamp((x - area.left()) / area.width(), 0.0, 1.0) * m_viewSpanBeats;
}

double AudioEditorWidget::xForBeat(double beat) const
{
    const QRectF area = waveformRect();
    return area.left() + (beat - m_viewStartBeat) /
        std::max(kMinimumMarkerSpacing, m_viewSpanBeats) * area.width();
}

void AudioEditorWidget::resetView()
{
    m_viewStartBeat = 0.0;
    m_viewSpanBeats = std::max(kMinimumMarkerSpacing, m_clip.warpLengthBeats);
}

void AudioEditorWidget::clampView()
{
    const double full = std::max(kMinimumMarkerSpacing, m_clip.warpLengthBeats);
    m_viewSpanBeats = std::clamp(m_viewSpanBeats,
        std::min(full, 0.125), full);
    m_viewStartBeat = std::clamp(m_viewStartBeat, 0.0,
        std::max(0.0, full - m_viewSpanBeats));
}

double AudioEditorWidget::targetBeatForSource(double sourceSeconds) const
{
    if (m_clip.sourceDurationSeconds <= 0.0 || m_clip.warpLengthBeats <= 0.0) return 0.0;
    const double source = std::clamp(sourceSeconds, 0.0, m_clip.sourceDurationSeconds);
    double previousSource = 0.0;
    double previousTarget = 0.0;
    for (const AudioWarpMarker& marker : m_clip.warpMarkers) {
        if (source <= marker.sourceSeconds) {
            const double span = marker.sourceSeconds - previousSource;
            const double ratio = span > 0.0 ? (source - previousSource) / span : 0.0;
            return previousTarget + ratio * (marker.targetBeat - previousTarget);
        }
        previousSource = marker.sourceSeconds;
        previousTarget = marker.targetBeat;
    }
    const double span = m_clip.sourceDurationSeconds - previousSource;
    const double ratio = span > 0.0 ? (source - previousSource) / span : 0.0;
    return previousTarget + ratio * (m_clip.warpLengthBeats - previousTarget);
}

int AudioEditorWidget::warpMarkerAt(const QPointF& position) const
{
    if (!waveformRect().adjusted(-7.0, -8.0, 7.0, 8.0).contains(position)) return -1;
    int closest = -1;
    double distance = 8.0;
    for (int index = 0; index < static_cast<int>(m_clip.warpMarkers.size()); ++index) {
        const double current = std::abs(position.x() -
            xForBeat(m_clip.warpMarkers[static_cast<size_t>(index)].targetBeat));
        if (current < distance) { distance = current; closest = index; }
    }
    return closest;
}

int AudioEditorWidget::transientAt(const QPointF& position) const
{
    if (!waveformRect().contains(position)) return -1;
    int closest = -1;
    double distance = 6.0;
    for (int index = 0; index < static_cast<int>(m_clip.transientSeconds.size()); ++index) {
        const double current = std::abs(position.x() - xForBeat(targetBeatForSource(
            m_clip.transientSeconds[static_cast<size_t>(index)])));
        if (current < distance) { distance = current; closest = index; }
    }
    return closest;
}

double AudioEditorWidget::snappedBeat(double beat, Qt::KeyboardModifiers modifiers) const
{
    const double maximum = std::max(kMinimumMarkerSpacing,
        m_clip.warpLengthBeats - kMinimumMarkerSpacing);
    const double bounded = std::clamp(beat, kMinimumMarkerSpacing, maximum);
    if ((modifiers & Qt::AltModifier) || m_snapBeats <= 0.0) return bounded;
    return std::clamp(std::round(bounded / m_snapBeats) * m_snapBeats,
                      kMinimumMarkerSpacing, maximum);
}

QString AudioEditorWidget::waveformCacheKey(const QString& path) const
{
    const QFileInfo info(path);
    return info.canonicalFilePath() + QLatin1Char('|') +
        QString::number(info.size()) + QLatin1Char('|') +
        QString::number(info.lastModified().toMSecsSinceEpoch());
}

void AudioEditorWidget::requestWaveform(const QString& path)
{
    const quint64 generation = ++m_loadGeneration;
    m_loadedPath = path;
    m_wavePeaks.clear();
    m_waveformError.clear();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        m_waveformError = path.isEmpty() ? tr("No audio source") : tr("Media offline");
        update();
        return;
    }
    const QString cacheKey = waveformCacheKey(path);
    {
        QMutexLocker locker(&s_waveCacheMutex);
        const auto cached = s_waveCache.constFind(cacheKey);
        if (cached != s_waveCache.cend()) {
            m_wavePeaks = cached.value();
            s_waveCacheOrder.removeAll(cacheKey);
            s_waveCacheOrder.push_back(cacheKey);
            update();
            return;
        }
    }
    m_waveformError = tr("Building waveform...");
    update();
    const QPointer<AudioEditorWidget> guard(this);
    QThreadPool::globalInstance()->start([guard, generation, path, cacheKey]() {
        const AudioDecodeResult result = AudioFileDecoder::decodeAudio(path);
        QVector<WavePeak> peaks;
        QString error;
        if (!result.success || result.audio.frameCount() <= 0) {
            error = result.message.isEmpty() ? QObject::tr("Waveform unavailable") : result.message;
        } else {
            const int frames = result.audio.frameCount();
            const int bins = std::max(1, std::min(kWaveformBins, frames));
            peaks.resize(bins);
            for (int bin = 0; bin < bins; ++bin) {
                const int first = static_cast<int>((static_cast<qint64>(bin) * frames) / bins);
                const int last = std::max(first + 1,
                    static_cast<int>((static_cast<qint64>(bin + 1) * frames) / bins));
                float leftMinimum = std::numeric_limits<float>::max();
                float leftMaximum = std::numeric_limits<float>::lowest();
                float rightMinimum = std::numeric_limits<float>::max();
                float rightMaximum = std::numeric_limits<float>::lowest();
                for (int frame = first; frame < std::min(last, frames); ++frame) {
                    const float left = result.audio.stereoSamples[frame * 2];
                    const float right = result.audio.stereoSamples[frame * 2 + 1];
                    leftMinimum = std::min(leftMinimum, left);
                    leftMaximum = std::max(leftMaximum, left);
                    rightMinimum = std::min(rightMinimum, right);
                    rightMaximum = std::max(rightMaximum, right);
                }
                peaks[bin] = {leftMinimum, leftMaximum, rightMinimum, rightMaximum};
            }
        }
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, generation, path, cacheKey,
                                  peaks = std::move(peaks), error]() mutable {
            if (!guard || guard->m_loadGeneration.load() != generation ||
                guard->m_loadedPath != path) return;
            if (!peaks.isEmpty()) {
                QMutexLocker locker(&s_waveCacheMutex);
                s_waveCache.insert(cacheKey, peaks);
                s_waveCacheOrder.removeAll(cacheKey);
                s_waveCacheOrder.push_back(cacheKey);
                while (s_waveCacheOrder.size() > kWaveCacheEntries) {
                    s_waveCache.remove(s_waveCacheOrder.takeFirst());
                }
            }
            guard->m_wavePeaks = std::move(peaks);
            guard->m_waveformError = error;
            guard->update();
        }, Qt::QueuedConnection);
    });
}

void AudioEditorWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(20, 22, 25));
    painter.setPen(QColor(224, 228, 232));
    QFont heading = painter.font();
    heading.setBold(true);
    painter.setFont(heading);
    painter.drawText(QRectF(18, 10, width() - 36, 22), Qt::AlignVCenter,
                     m_hasClip ? m_clip.name.toUpper() : tr("AUDIO CLIP"));
    heading.setBold(false);
    painter.setFont(heading);
    painter.setPen(QColor(135, 143, 152));
    const QString detail = m_hasClip
        ? tr("%1  |  %2 warp  |  %3 transient  |  %4 gain  |  %5  |  %6")
            .arg(QFileInfo(m_clip.sourcePath).fileName())
            .arg(m_clip.warpMarkers.size()).arg(m_clip.transientSeconds.size())
            .arg(m_clip.gainEnvelope.size())
            .arg(m_clip.mediaOffline ? tr("OFFLINE") : tr("READY"))
            .arg(m_waveformMode == WaveformMode::Stereo ? tr("STEREO") :
                 m_waveformMode == WaveformMode::Overlay ? tr("OVERLAY") : tr("MONO"))
        : tr("Select audio clip: waveform, transients, IN/OUT, fades, warp anchors.");
    painter.drawText(QRectF(180, 10, width() - 198, 22),
                     Qt::AlignRight | Qt::AlignVCenter, detail);

    const QRectF area = waveformRect();
    painter.fillRect(area, QColor(27, 30, 34));
    painter.setPen(QColor(48, 53, 59));
    const int firstBeat = static_cast<int>(std::floor(m_viewStartBeat));
    const int lastBeat = static_cast<int>(std::ceil(m_viewStartBeat + m_viewSpanBeats));
    for (int beat = firstBeat; beat <= lastBeat; ++beat) {
        const double x = xForBeat(static_cast<double>(beat));
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
    painter.setPen(QColor(64, 70, 77));
    painter.drawLine(QPointF(area.left(), area.center().y()), QPointF(area.right(), area.center().y()));
    if (m_waveformMode == WaveformMode::Stereo) {
        const double leftCenter = area.top() + area.height() * 0.25;
        const double rightCenter = area.top() + area.height() * 0.75;
        painter.setPen(QColor(54, 60, 66));
        painter.drawLine(QPointF(area.left(), leftCenter), QPointF(area.right(), leftCenter));
        painter.drawLine(QPointF(area.left(), rightCenter), QPointF(area.right(), rightCenter));
        painter.setPen(QColor(130, 138, 146));
        painter.drawText(QPointF(area.left() + 4.0, leftCenter - 4.0), tr("L"));
        painter.drawText(QPointF(area.left() + 4.0, rightCenter - 4.0), tr("R"));
    }
    if (!m_hasClip) return;

    if (!m_wavePeaks.isEmpty()) {
        QPainterPath leftWaveform;
        QPainterPath rightWaveform;
        const double leftCenter = m_waveformMode == WaveformMode::Stereo
            ? area.top() + area.height() * 0.25 : area.center().y();
        const double rightCenter = m_waveformMode == WaveformMode::Stereo
            ? area.top() + area.height() * 0.75 : area.center().y();
        const double amplitude = area.height() *
            (m_waveformMode == WaveformMode::Stereo ? 0.22 : 0.46);
        for (int index = 0; index < m_wavePeaks.size(); ++index) {
            const double sourceSeconds = m_clip.sourceDurationSeconds *
                (static_cast<double>(index) + 0.5) / m_wavePeaks.size();
            const double x = xForBeat(targetBeatForSource(sourceSeconds));
            if (x < area.left() || x > area.right()) continue;
            const WavePeak& peak = m_wavePeaks[index];
            if (m_waveformMode == WaveformMode::Mono) {
                const float minimum = (peak.leftMinimum + peak.rightMinimum) * 0.5f;
                const float maximum = (peak.leftMaximum + peak.rightMaximum) * 0.5f;
                leftWaveform.moveTo(x, leftCenter - maximum * amplitude);
                leftWaveform.lineTo(x, leftCenter - minimum * amplitude);
            } else {
                leftWaveform.moveTo(x, leftCenter - peak.leftMaximum * amplitude);
                leftWaveform.lineTo(x, leftCenter - peak.leftMinimum * amplitude);
                rightWaveform.moveTo(x, rightCenter - peak.rightMaximum * amplitude);
                rightWaveform.lineTo(x, rightCenter - peak.rightMinimum * amplitude);
            }
        }
        painter.setPen(QPen(QColor(94, 199, 177), 1.0));
        painter.drawPath(leftWaveform);
        if (m_waveformMode != WaveformMode::Mono) {
            painter.setPen(QPen(QColor(100, 158, 234,
                m_waveformMode == WaveformMode::Overlay ? 180 : 255), 1.0));
            painter.drawPath(rightWaveform);
        }
    } else if (!m_waveformError.isEmpty()) {
        painter.setPen(QColor(150, 157, 165));
        painter.drawText(area, Qt::AlignCenter, m_waveformError);
    }

    const bool previewRange = m_dragMode == DragMode::SourceIn ||
        m_dragMode == DragMode::SourceOut || m_dragMode == DragMode::FadeIn ||
        m_dragMode == DragMode::FadeOut;
    const double sourceIn = previewRange ? m_previewSourceIn :
        std::clamp(m_clip.sourceOffsetBeats, 0.0, m_clip.warpLengthBeats);
    const double sourceOut = previewRange ? m_previewSourceOut :
        std::clamp(sourceIn + m_clip.lengthBeats, sourceIn, m_clip.warpLengthBeats);
    const double fadeIn = previewRange ? m_previewFadeIn : m_clip.fadeInBeats;
    const double fadeOut = previewRange ? m_previewFadeOut : m_clip.fadeOutBeats;
    const double sourceInX = std::clamp(xForBeat(sourceIn), area.left(), area.right());
    const double sourceOutX = std::clamp(xForBeat(sourceOut), area.left(), area.right());
    painter.fillRect(QRectF(area.left(), area.top(), std::max(0.0, sourceInX - area.left()),
                           area.height()), QColor(8, 9, 11, 150));
    painter.fillRect(QRectF(sourceOutX, area.top(),
                           std::max(0.0, area.right() - sourceOutX),
                           area.height()), QColor(8, 9, 11, 150));

    const bool previewLoop = m_dragMode == DragMode::LoopRange;
    const bool loopEnabled = previewLoop || m_loopEnabled;
    const double loopStartProject = previewLoop ? m_previewLoopStartBeat : m_loopStartBeat;
    const double loopEndProject = previewLoop ? m_previewLoopEndBeat : m_loopEndBeat;
    if (loopEnabled) {
        const double loopStartEditor = loopStartProject - m_clip.startBeat +
            m_clip.sourceOffsetBeats;
        const double loopEndEditor = loopEndProject - m_clip.startBeat +
            m_clip.sourceOffsetBeats;
        const double visibleStart = std::max(m_viewStartBeat, loopStartEditor);
        const double visibleEnd = std::min(m_viewStartBeat + m_viewSpanBeats, loopEndEditor);
        if (visibleEnd > visibleStart) {
            const QRectF loopRect(xForBeat(visibleStart), area.top(),
                xForBeat(visibleEnd) - xForBeat(visibleStart), area.height());
            painter.fillRect(loopRect, QColor(244, 188, 72, 24));
            painter.fillRect(QRectF(loopRect.left(), area.top(), loopRect.width(), 7.0),
                             QColor(244, 188, 72, 190));
        }
    }

    for (int index = 0; index < static_cast<int>(m_clip.transientSeconds.size()); ++index) {
        const double x = xForBeat(targetBeatForSource(
            m_clip.transientSeconds[static_cast<size_t>(index)]));
        if (x < area.left() || x > area.right()) continue;
        painter.setPen(QPen(index == m_selectedTransient ? QColor(255, 198, 92) :
                           QColor(100, 109, 118), 1.0, Qt::DashLine));
        painter.drawLine(QPointF(x, area.top() + 12.0), QPointF(x, area.bottom()));
    }
    for (int index = 0; index < static_cast<int>(m_clip.warpMarkers.size()); ++index) {
        const double target = m_draggingWarpMarker && index == m_selectedWarpMarker
            ? m_dragPreviewBeat : m_clip.warpMarkers[static_cast<size_t>(index)].targetBeat;
        const double x = xForBeat(target);
        if (x < area.left() || x > area.right()) continue;
        const QColor color = index == m_selectedWarpMarker ?
            QColor(255, 185, 68) : QColor(99, 211, 188);
        painter.setPen(QPen(color, 2.0));
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawPolygon(QPolygonF() << QPointF(x, area.top()) <<
            QPointF(x - 5, area.top() + 7) << QPointF(x + 5, area.top() + 7));
    }

    painter.setPen(QPen(QColor(120, 190, 255), 2.0));
    painter.drawLine(QPointF(sourceInX, area.top()), QPointF(sourceInX, area.bottom()));
    painter.drawLine(QPointF(sourceOutX, area.top()), QPointF(sourceOutX, area.bottom()));
    painter.drawText(QPointF(sourceInX + 4, area.top() + 14), tr("IN"));
    painter.drawText(QPointF(sourceOutX - 26, area.top() + 14), tr("OUT"));
    if (fadeIn > 0.0) {
        painter.setPen(QPen(QColor(202, 137, 255), 1.5));
        QPainterPath fadePath;
        fadePath.moveTo(sourceInX, area.bottom());
        constexpr int fadeSamples = 24;
        for (int sample = 1; sample <= fadeSamples; ++sample) {
            const double progress = static_cast<double>(sample) / fadeSamples;
            fadePath.lineTo(xForBeat(sourceIn + fadeIn * progress),
                area.bottom() - shapedFade(progress, m_clip.fadeInCurve) * area.height());
        }
        painter.drawPath(fadePath);
    }
    if (fadeOut > 0.0) {
        painter.setPen(QPen(QColor(202, 137, 255), 1.5));
        QPainterPath fadePath;
        fadePath.moveTo(xForBeat(sourceOut - fadeOut), area.top());
        constexpr int fadeSamples = 24;
        for (int sample = 1; sample <= fadeSamples; ++sample) {
            const double progress = static_cast<double>(sample) / fadeSamples;
            fadePath.lineTo(xForBeat(sourceOut - fadeOut + fadeOut * progress),
                area.bottom() - shapedFade(1.0 - progress, m_clip.fadeOutCurve) * area.height());
        }
        painter.drawPath(fadePath);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(120, 190, 255));
    painter.drawRect(QRectF(sourceInX - 4.0, area.center().y() - 5.0, 8.0, 10.0));
    painter.drawRect(QRectF(sourceOutX - 4.0, area.center().y() - 5.0, 8.0, 10.0));
    painter.setBrush(QColor(202, 137, 255));
    painter.drawEllipse(QPointF(xForBeat(sourceIn + fadeIn), area.top() + 2.0), 4.0, 4.0);
    painter.drawEllipse(QPointF(xForBeat(sourceOut - fadeOut), area.top() + 2.0), 4.0, 4.0);

    const auto& envelope = m_dragMode == DragMode::GainEnvelope
        ? m_previewGainEnvelope : m_clip.gainEnvelope;
    QPainterPath gainPath;
    gainPath.moveTo(xForBeat(sourceIn), gainEnvelopeY(
        gainEnvelopeValueAt(0.0, envelope)));
    for (const AudioGainEnvelopePoint& point : envelope)
        gainPath.lineTo(xForBeat(sourceIn + point.beat), gainEnvelopeY(point.gain));
    gainPath.lineTo(xForBeat(sourceOut), gainEnvelopeY(
        gainEnvelopeValueAt(m_clip.lengthBeats, envelope)));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 214, 92, 220), 1.6));
    painter.drawPath(gainPath);
    for (int index = 0; index < static_cast<int>(envelope.size()); ++index) {
        const AudioGainEnvelopePoint& point = envelope[static_cast<size_t>(index)];
        const QPointF center(xForBeat(sourceIn + point.beat), gainEnvelopeY(point.gain));
        painter.setBrush(index == m_selectedGainPoint ? QColor(255, 244, 182) : QColor(255, 214, 92));
        painter.setPen(QPen(QColor(28, 30, 33), 1.0));
        painter.drawEllipse(center, 4.5, 4.5);
    }
    const double localPlayhead = m_playheadBeat - m_clip.startBeat + m_clip.sourceOffsetBeats;
    if (m_playheadBeat >= m_clip.startBeat &&
        m_playheadBeat <= m_clip.startBeat + m_clip.lengthBeats) {
        const double x = xForBeat(localPlayhead);
        painter.setPen(QPen(QColor(255, 100, 92), 1.5));
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
    if (hasFocus()) {
        painter.setPen(QPen(QColor(118, 177, 255), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(area.adjusted(0.5, 0.5, -0.5, -0.5));
    }
}

AudioEditorWidget::DragMode AudioEditorWidget::dragModeAt(const QPointF& position) const
{
    if (!m_hasClip || !waveformRect().adjusted(-8.0, -8.0, 8.0, 8.0).contains(position))
        return DragMode::None;
    const QRectF area = waveformRect();
    if (gainEnvelopePointAt(position) >= 0) return DragMode::GainEnvelope;
    const double sourceIn = m_clip.sourceOffsetBeats;
    const double sourceOut = sourceIn + m_clip.lengthBeats;
    if (std::abs(position.y() - (area.top() + 2.0)) <= 9.0) {
        if (std::abs(position.x() - xForBeat(sourceIn + m_clip.fadeInBeats)) <= 8.0)
            return DragMode::FadeIn;
        if (std::abs(position.x() - xForBeat(sourceOut - m_clip.fadeOutBeats)) <= 8.0)
            return DragMode::FadeOut;
    }
    if (std::abs(position.y() - area.center().y()) <= 14.0) {
        if (std::abs(position.x() - xForBeat(sourceIn)) <= 8.0)
            return DragMode::SourceIn;
        if (std::abs(position.x() - xForBeat(sourceOut)) <= 8.0)
            return DragMode::SourceOut;
    }
    return warpMarkerAt(position) >= 0 ? DragMode::WarpMarker : DragMode::None;
}

void AudioEditorWidget::updateDragPreview(double beat, Qt::KeyboardModifiers modifiers)
{
    double target = m_dragMode == DragMode::WarpMarker
        ? snappedBeat(beat, modifiers)
        : std::clamp(beat, 0.0, m_clip.warpLengthBeats);
    if (m_dragMode != DragMode::WarpMarker &&
        !(modifiers & Qt::AltModifier) && m_snapBeats > 0.0) {
        target = std::clamp(std::round(target / m_snapBeats) * m_snapBeats,
                            0.0, m_clip.warpLengthBeats);
    }
    constexpr double minimumLength = 0.25;
    if (m_dragMode == DragMode::SourceIn) {
        m_previewSourceIn = std::clamp(target, 0.0,
            std::max(0.0, m_previewSourceOut - minimumLength));
    } else if (m_dragMode == DragMode::SourceOut) {
        m_previewSourceOut = std::clamp(target,
            std::min(m_clip.warpLengthBeats, m_previewSourceIn + minimumLength),
            m_clip.warpLengthBeats);
    } else if (m_dragMode == DragMode::FadeIn) {
        m_previewFadeIn = std::clamp(target - m_previewSourceIn,
            0.0, m_previewSourceOut - m_previewSourceIn);
    } else if (m_dragMode == DragMode::FadeOut) {
        m_previewFadeOut = std::clamp(m_previewSourceOut - target,
            0.0, m_previewSourceOut - m_previewSourceIn);
    } else if (m_dragMode == DragMode::WarpMarker) {
        m_dragPreviewBeat = target;
    }
    const double length = m_previewSourceOut - m_previewSourceIn;
    m_previewFadeIn = std::clamp(m_previewFadeIn, 0.0, length);
    m_previewFadeOut = std::clamp(m_previewFadeOut, 0.0, length);
    if (m_previewFadeIn + m_previewFadeOut > length && length > 0.0) {
        const double scale = length / (m_previewFadeIn + m_previewFadeOut);
        m_previewFadeIn *= scale;
        m_previewFadeOut *= scale;
    }
}

void AudioEditorWidget::mousePressEvent(QMouseEvent* event)
{
    if (!m_hasClip) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::MiddleButton) {
        m_dragMode = DragMode::Pan;
        m_lastPanPosition = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragMode = dragModeAt(event->position());
    if (m_dragMode == DragMode::None && waveformRect().contains(event->position())) {
        if (event->modifiers() & Qt::ControlModifier) {
            m_dragMode = DragMode::LoopRange;
            double editorBeat = std::clamp(beatAtX(event->position().x()),
                0.0, m_clip.warpLengthBeats);
            if (!(event->modifiers() & Qt::AltModifier) && m_snapBeats > 0.0)
                editorBeat = std::round(editorBeat / m_snapBeats) * m_snapBeats;
            m_loopDragAnchorBeat = projectBeatForEditorBeat(editorBeat);
            m_previewLoopStartBeat = m_loopDragAnchorBeat;
            m_previewLoopEndBeat = m_loopDragAnchorBeat;
        } else if (event->modifiers() & Qt::AltModifier) {
            m_dragMode = DragMode::Scrub;
            m_scrubThrottle.restart();
            emit scrubRequested(projectBeatForEditorBeat(
                beatAtX(event->position().x())));
        }
    }
    m_previewSourceIn = m_clip.sourceOffsetBeats;
    m_previewSourceOut = m_clip.sourceOffsetBeats + m_clip.lengthBeats;
    m_previewFadeIn = m_clip.fadeInBeats;
    m_previewFadeOut = m_clip.fadeOutBeats;
    m_previewGainEnvelope = m_clip.gainEnvelope;
    if (m_dragMode == DragMode::WarpMarker) {
        m_selectedWarpMarker = warpMarkerAt(event->position());
        m_selectedTransient = -1;
        m_draggingWarpMarker = true;
        m_dragPreviewBeat = m_clip.warpMarkers[static_cast<size_t>(m_selectedWarpMarker)].targetBeat;
        setCursor(Qt::SizeHorCursor);
    } else if (m_dragMode == DragMode::GainEnvelope) {
        m_selectedGainPoint = gainEnvelopePointAt(event->position());
        m_selectedWarpMarker = -1;
        m_selectedTransient = -1;
        setCursor(Qt::SizeAllCursor);
    } else if (m_dragMode != DragMode::None) {
        m_selectedWarpMarker = -1;
        m_selectedTransient = -1;
        setCursor(m_dragMode == DragMode::Scrub ? Qt::CrossCursor :
                  m_dragMode == DragMode::LoopRange ? Qt::CrossCursor :
                  Qt::SizeHorCursor);
    } else {
        m_selectedWarpMarker = -1;
        m_selectedTransient = transientAt(event->position());
    }
    if (m_dragMode == DragMode::None && m_selectedTransient < 0 &&
        waveformRect().contains(event->position())) {
        emit seekRequested(std::max(0.0, m_clip.startBeat +
            beatAtX(event->position().x()) - m_clip.sourceOffsetBeats));
    }
    update();
    event->accept();
}

void AudioEditorWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragMode == DragMode::Pan) {
        const double deltaPixels = event->position().x() - m_lastPanPosition.x();
        m_viewStartBeat -= deltaPixels / std::max(1.0, waveformRect().width()) * m_viewSpanBeats;
        m_lastPanPosition = event->position();
        clampView();
        update();
        event->accept();
        return;
    }
    if (m_dragMode == DragMode::LoopRange) {
        double editorBeat = std::clamp(beatAtX(event->position().x()),
            0.0, m_clip.warpLengthBeats);
        if (!(event->modifiers() & Qt::AltModifier) && m_snapBeats > 0.0)
            editorBeat = std::round(editorBeat / m_snapBeats) * m_snapBeats;
        const double projectBeat = projectBeatForEditorBeat(editorBeat);
        m_previewLoopStartBeat = std::min(m_loopDragAnchorBeat, projectBeat);
        m_previewLoopEndBeat = std::max(m_loopDragAnchorBeat, projectBeat);
        update();
        event->accept();
        return;
    }
    if (m_dragMode == DragMode::Scrub) {
        if (!m_scrubThrottle.isValid() || m_scrubThrottle.elapsed() >= 33) {
            emit scrubRequested(projectBeatForEditorBeat(
                beatAtX(event->position().x())));
            m_scrubThrottle.restart();
        }
        event->accept();
        return;
    }
    if (m_dragMode == DragMode::GainEnvelope && m_selectedGainPoint >= 0 &&
        m_selectedGainPoint < static_cast<int>(m_previewGainEnvelope.size())) {
        AudioGainEnvelopePoint& point = m_previewGainEnvelope[static_cast<size_t>(m_selectedGainPoint)];
        double localBeat = std::clamp(beatAtX(event->position().x()) - m_clip.sourceOffsetBeats,
                                      0.0, m_clip.lengthBeats);
        if (!(event->modifiers() & Qt::AltModifier) && m_snapBeats > 0.0)
            localBeat = std::round(localBeat / m_snapBeats) * m_snapBeats;
        const double leftLimit = m_selectedGainPoint > 0
            ? m_previewGainEnvelope[static_cast<size_t>(m_selectedGainPoint - 1)].beat + kMinimumMarkerSpacing
            : 0.0;
        const double rightLimit = m_selectedGainPoint + 1 < static_cast<int>(m_previewGainEnvelope.size())
            ? m_previewGainEnvelope[static_cast<size_t>(m_selectedGainPoint + 1)].beat - kMinimumMarkerSpacing
            : m_clip.lengthBeats;
        point.beat = std::clamp(localBeat, leftLimit, std::max(leftLimit, rightLimit));
        point.gain = gainEnvelopeAtY(event->position().y());
        update();
        event->accept();
        return;
    }
    if (m_dragMode != DragMode::None) {
        updateDragPreview(beatAtX(event->position().x()), event->modifiers());
        update();
        event->accept();
        return;
    }
    setCursor(dragModeAt(event->position()) != DragMode::None ?
        Qt::SizeHorCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(event);
}

void AudioEditorWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragMode == DragMode::Pan && event->button() == Qt::MiddleButton) {
        m_dragMode = DragMode::None;
        unsetCursor();
        event->accept();
        return;
    }
    if (m_dragMode != DragMode::None && event->button() == Qt::LeftButton) {
        const DragMode finishedMode = m_dragMode;
        m_dragMode = DragMode::None;
        m_draggingWarpMarker = false;
        unsetCursor();
        if (finishedMode == DragMode::WarpMarker)
            emit warpMarkerMoveRequested(m_selectedWarpMarker, m_dragPreviewBeat);
        else if (finishedMode == DragMode::SourceIn ||
                 finishedMode == DragMode::SourceOut)
            emit sourceRangeChangeRequested(m_previewSourceIn, m_previewSourceOut);
        else if (finishedMode == DragMode::FadeIn ||
                 finishedMode == DragMode::FadeOut)
            emit fadesChangeRequested(m_previewFadeIn, m_previewFadeOut);
        else if (finishedMode == DragMode::LoopRange &&
                 m_previewLoopEndBeat - m_previewLoopStartBeat >= 0.25)
            emit loopRangeChangeRequested(true, m_previewLoopStartBeat,
                                          m_previewLoopEndBeat);
        else if (finishedMode == DragMode::Scrub)
            emit scrubRequested(projectBeatForEditorBeat(
                beatAtX(event->position().x())));
        else if (finishedMode == DragMode::GainEnvelope)
            emit gainEnvelopeChangeRequested(m_previewGainEnvelope);
        update();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void AudioEditorWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (m_hasClip && event->button() == Qt::LeftButton) {
        m_selectedTransient = transientAt(event->position());
        if (m_selectedTransient >= 0) {
            m_selectedWarpMarker = -1;
            m_selectedGainPoint = -1;
            m_dragMode = DragMode::None;
            toggleSelectedTransient();
            event->accept();
            return;
        }
        const int gainPoint = gainEnvelopePointAt(event->position());
        if (gainPoint >= 0 || waveformRect().contains(event->position())) {
            std::vector<AudioGainEnvelopePoint> points = m_clip.gainEnvelope;
            if (gainPoint >= 0) {
                points.erase(points.begin() + gainPoint);
                m_selectedGainPoint = -1;
            } else {
                double beat = std::clamp(beatAtX(event->position().x()) - m_clip.sourceOffsetBeats,
                                         0.0, m_clip.lengthBeats);
                if (!(event->modifiers() & Qt::AltModifier) && m_snapBeats > 0.0)
                    beat = std::round(beat / m_snapBeats) * m_snapBeats;
                points.push_back({beat, gainEnvelopeAtY(event->position().y())});
                std::stable_sort(points.begin(), points.end(),
                    [](const AudioGainEnvelopePoint& left, const AudioGainEnvelopePoint& right) {
                        return left.beat < right.beat;
                    });
            }
            m_dragMode = DragMode::None;
            m_previewGainEnvelope.clear();
            emit gainEnvelopeChangeRequested(points);
            event->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void AudioEditorWidget::toggleSelectedTransient()
{
    if (!m_hasClip || m_selectedTransient < 0 ||
        m_selectedTransient >= static_cast<int>(m_clip.transientSeconds.size())) return;
    const double sourceSeconds = m_clip.transientSeconds[static_cast<size_t>(m_selectedTransient)];
    emit warpMarkerToggleRequested(sourceSeconds,
        snappedBeat(targetBeatForSource(sourceSeconds), Qt::NoModifier));
}

void AudioEditorWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_1) {
        setWaveformMode(WaveformMode::Stereo);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_2) {
        setWaveformMode(WaveformMode::Overlay);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_3) {
        setWaveformMode(WaveformMode::Mono);
        event->accept();
        return;
    }
    if (m_hasClip && event->key() == Qt::Key_L) {
        if (event->modifiers() & Qt::ShiftModifier)
            emit loopRangeChangeRequested(false, 0.0, 4.0);
        else
            emit loopRangeChangeRequested(true, m_clip.startBeat,
                                          m_clip.startBeat + m_clip.lengthBeats);
        event->accept();
        return;
    }
    if (m_hasClip && event->key() == Qt::Key_Home) {
        resetView();
        update();
        event->accept();
        return;
    }
    if (m_hasClip && m_selectedWarpMarker >= 0 &&
        (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        const double step = (event->modifiers() & Qt::AltModifier)
            ? kMinimumMarkerSpacing : (m_snapBeats > 0.0 ? m_snapBeats : 0.25);
        emit warpMarkerMoveRequested(m_selectedWarpMarker,
            m_clip.warpMarkers[static_cast<size_t>(m_selectedWarpMarker)].targetBeat +
            (event->key() == Qt::Key_Left ? -step : step));
        event->accept();
        return;
    }
    if (m_hasClip && m_selectedWarpMarker >= 0 && event->key() == Qt::Key_Delete) {
        emit warpMarkerRemoveRequested(m_selectedWarpMarker);
        m_selectedWarpMarker = -1;
        event->accept();
        return;
    }
    if (m_hasClip && m_selectedGainPoint >= 0 && event->key() == Qt::Key_Delete) {
        std::vector<AudioGainEnvelopePoint> points = m_clip.gainEnvelope;
        if (m_selectedGainPoint < static_cast<int>(points.size()))
            points.erase(points.begin() + m_selectedGainPoint);
        m_selectedGainPoint = -1;
        emit gainEnvelopeChangeRequested(points);
        event->accept();
        return;
    }
    if (m_hasClip && m_selectedTransient >= 0 &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        toggleSelectedTransient();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void AudioEditorWidget::wheelEvent(QWheelEvent* event)
{
    if (!m_hasClip || event->angleDelta().y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    const double direction = event->angleDelta().y() > 0 ? 1.0 : -1.0;
    if (event->modifiers() & Qt::ShiftModifier) {
        m_viewStartBeat -= direction * m_viewSpanBeats * 0.1;
        clampView();
    } else {
        const QRectF area = waveformRect();
        const double anchorRatio = std::clamp(
            (event->position().x() - area.left()) / area.width(), 0.0, 1.0);
        const double anchorBeat = m_viewStartBeat + anchorRatio * m_viewSpanBeats;
        m_viewSpanBeats *= direction > 0.0 ? 0.8 : 1.25;
        clampView();
        m_viewStartBeat = anchorBeat - anchorRatio * m_viewSpanBeats;
        clampView();
    }
    update();
    event->accept();
}

void AudioEditorWidget::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu menu(this);
    QAction* stereo = menu.addAction(tr("Stereo split [1]"));
    QAction* overlay = menu.addAction(tr("Stereo overlay [2]"));
    QAction* mono = menu.addAction(tr("Mono sum [3]"));
    stereo->setCheckable(true);
    overlay->setCheckable(true);
    mono->setCheckable(true);
    stereo->setChecked(m_waveformMode == WaveformMode::Stereo);
    overlay->setChecked(m_waveformMode == WaveformMode::Overlay);
    mono->setChecked(m_waveformMode == WaveformMode::Mono);
    menu.addSeparator();
    QAction* loopClip = menu.addAction(tr("Loop selected clip [L]"));
    QAction* disableLoop = menu.addAction(tr("Disable project loop [Shift+L]"));
    loopClip->setEnabled(m_hasClip);
    disableLoop->setEnabled(m_loopEnabled);
    menu.addSeparator();
    QMenu* fadeInCurve = menu.addMenu(tr("Fade-in curve"));
    QAction* fadeInFast = fadeInCurve->addAction(tr("Fast"));
    QAction* fadeInEqual = fadeInCurve->addAction(tr("Equal power"));
    QAction* fadeInSlow = fadeInCurve->addAction(tr("Slow"));
    QMenu* fadeOutCurve = menu.addMenu(tr("Fade-out curve"));
    QAction* fadeOutFast = fadeOutCurve->addAction(tr("Fast"));
    QAction* fadeOutEqual = fadeOutCurve->addAction(tr("Equal power"));
    QAction* fadeOutSlow = fadeOutCurve->addAction(tr("Slow"));
    fadeInCurve->setEnabled(m_hasClip);
    fadeOutCurve->setEnabled(m_hasClip);
    QAction* selected = menu.exec(event->globalPos());
    if (selected == stereo) setWaveformMode(WaveformMode::Stereo);
    else if (selected == overlay) setWaveformMode(WaveformMode::Overlay);
    else if (selected == mono) setWaveformMode(WaveformMode::Mono);
    else if (selected == loopClip)
        emit loopRangeChangeRequested(true, m_clip.startBeat,
                                      m_clip.startBeat + m_clip.lengthBeats);
    else if (selected == disableLoop)
        emit loopRangeChangeRequested(false, 0.0, 4.0);
    else if (selected == fadeInFast)
        emit fadeCurvesChangeRequested(-1.0, m_clip.fadeOutCurve);
    else if (selected == fadeInEqual)
        emit fadeCurvesChangeRequested(0.0, m_clip.fadeOutCurve);
    else if (selected == fadeInSlow)
        emit fadeCurvesChangeRequested(1.0, m_clip.fadeOutCurve);
    else if (selected == fadeOutFast)
        emit fadeCurvesChangeRequested(m_clip.fadeInCurve, -1.0);
    else if (selected == fadeOutEqual)
        emit fadeCurvesChangeRequested(m_clip.fadeInCurve, 0.0);
    else if (selected == fadeOutSlow)
        emit fadeCurvesChangeRequested(m_clip.fadeInCurve, 1.0);
    event->accept();
}
