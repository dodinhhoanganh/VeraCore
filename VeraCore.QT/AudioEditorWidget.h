#pragma once
#include <QVector>
#include <QWidget>
#include <QHash>
#include <QElapsedTimer>
#include <QMutex>
#include <QStringList>
#include <atomic>
#include "PlaylistView.h"

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class QContextMenuEvent;

class AudioEditorWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit AudioEditorWidget(QWidget* parent = nullptr);
    ~AudioEditorWidget() override;
    void setClip(const PlaylistView::AudioClipInfo& clip);
    void clearClip();
    void setSnapBeats(double beats);
    void setPlayheadBeat(double projectBeat);
    void setLoopRange(bool enabled, double startBeat, double endBeat);

signals:
    void warpMarkerMoveRequested(int markerIndex, double targetBeat);
    void warpMarkerToggleRequested(double sourceSeconds, double targetBeat);
    void warpMarkerRemoveRequested(int markerIndex);
    void sourceRangeChangeRequested(double sourceInBeats, double sourceOutBeats);
    void fadesChangeRequested(double fadeInBeats, double fadeOutBeats);
    void fadeCurvesChangeRequested(double fadeInCurve, double fadeOutCurve);
    void gainEnvelopeChangeRequested(const std::vector<AudioGainEnvelopePoint>& points);
    void loopRangeChangeRequested(bool enabled, double startBeat, double endBeat);
    void scrubRequested(double projectBeat);
    void seekRequested(double projectBeat);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    struct WavePeak {
        float leftMinimum = 0.0f;
        float leftMaximum = 0.0f;
        float rightMinimum = 0.0f;
        float rightMaximum = 0.0f;
    };
    enum class WaveformMode { Stereo, Overlay, Mono };
    PlaylistView::AudioClipInfo m_clip{};
    bool m_hasClip = false;
    QVector<WavePeak> m_wavePeaks;
    QString m_waveformError;
    QString m_loadedPath;
    std::atomic<quint64> m_loadGeneration{0};
    double m_snapBeats = 0.25;
    double m_playheadBeat = -1.0;
    int m_selectedWarpMarker = -1;
    int m_selectedTransient = -1;
    bool m_draggingWarpMarker = false;
    double m_dragPreviewBeat = 0.0;
    enum class DragMode {
        None, WarpMarker, SourceIn, SourceOut, FadeIn, FadeOut,
        LoopRange, Scrub, Pan, GainEnvelope
    };
    DragMode m_dragMode = DragMode::None;
    double m_previewSourceIn = 0.0;
    double m_previewSourceOut = 0.0;
    double m_previewFadeIn = 0.0;
    double m_previewFadeOut = 0.0;
    double m_viewStartBeat = 0.0;
    double m_viewSpanBeats = 1.0;
    QPointF m_lastPanPosition;
    WaveformMode m_waveformMode = WaveformMode::Stereo;
    bool m_loopEnabled = false;
    double m_loopStartBeat = 0.0;
    double m_loopEndBeat = 4.0;
    double m_previewLoopStartBeat = 0.0;
    double m_previewLoopEndBeat = 0.0;
    double m_loopDragAnchorBeat = 0.0;
    QElapsedTimer m_scrubThrottle;
    int m_selectedGainPoint = -1;
    std::vector<AudioGainEnvelopePoint> m_previewGainEnvelope;

    static QMutex s_waveCacheMutex;
    static QHash<QString, QVector<WavePeak>> s_waveCache;
    static QStringList s_waveCacheOrder;

    QRectF waveformRect() const;
    double beatAtX(double x) const;
    double xForBeat(double beat) const;
    double targetBeatForSource(double sourceSeconds) const;
    int warpMarkerAt(const QPointF& position) const;
    int transientAt(const QPointF& position) const;
    double snappedBeat(double beat, Qt::KeyboardModifiers modifiers) const;
    QString waveformCacheKey(const QString& path) const;
    void requestWaveform(const QString& path);
    void toggleSelectedTransient();
    void resetView();
    void clampView();
    DragMode dragModeAt(const QPointF& position) const;
    void updateDragPreview(double beat, Qt::KeyboardModifiers modifiers);
    double projectBeatForEditorBeat(double beat) const;
    void setWaveformMode(WaveformMode mode);
    int gainEnvelopePointAt(const QPointF& position) const;
    double gainEnvelopeY(float gain) const;
    float gainEnvelopeAtY(double y) const;
    float gainEnvelopeValueAt(double localBeat,
                              const std::vector<AudioGainEnvelopePoint>& points) const;
    static double shapedFade(double progress, double curve);
};
