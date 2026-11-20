#pragma once
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QPoint>
#include <QColor>
#include <QString>
#include <QStringList>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include "Track.h"
#include "MidiClipModel.h"

struct AudioWarpMarker
{
    double sourceSeconds = 0.0;
    double targetBeat = 0.0;
    bool operator==(const AudioWarpMarker& other) const noexcept
    {
        return sourceSeconds == other.sourceSeconds && targetBeat == other.targetBeat;
    }
};

struct AudioGainEnvelopePoint
{
    double beat = 0.0;
    float gain = 1.0f;
    bool operator==(const AudioGainEnvelopePoint& other) const noexcept
    {
        return beat == other.beat && gain == other.gain;
    }
};

class PlaylistView : public QGraphicsView
{
    Q_OBJECT

public:
    explicit PlaylistView(QWidget* parent = nullptr);
    ~PlaylistView();

    void setPixelsPerBeat(double ppb);
    void setSnapBeats(double beats);
    double getPixelsPerBeat() const;
    void setPlayheadBeat(double beat);
    void setLoopRange(bool enabled, double startBeat, double endBeat);
    void requestLoopRangeChange(bool enabled, double startBeat, double endBeat);
    void setTimeSelection(bool enabled, double startBeat, double endBeat);
    bool timeSelection(double* startBeat = nullptr, double* endBeat = nullptr) const;
    void setPunchRange(bool enabled, double startBeat, double endBeat);
    void setTimeSignature(int numerator, int denominator);
    void setTimeSignatureMap(const std::vector<MidiTimeSignaturePoint>& points);
    int addMidiClip(int trackIndex, double startBeat, double lengthBeats, const QString& name,
                    const QString& clipId = {}, const QString& contentId = {},
                    double sourceOffsetBeats = 0.0, bool muted = false);
    int addAudioClip(int trackIndex, double startBeat, double lengthBeats, const QString& name, const QString& sourcePath,
                     double sourceTempoBpm = 0.0, double pitchShiftSemitones = 0.0, double beatsPerBar = 4.0,
                     double sourceDurationSeconds = 0.0, const QString& clipId = {},
                     const QString& contentId = {}, double sourceOffsetBeats = 0.0,
                     double warpLengthBeats = 0.0, double tempoConfidence = 0.0,
                     int sourceSignatureNumerator = 0, int sourceSignatureDenominator = 0,
                     bool tempoDetected = false,
                      const std::vector<double>& transientSeconds = {},
                      const std::vector<AudioWarpMarker>& warpMarkers = {},
                      const std::vector<AudioGainEnvelopePoint>& gainEnvelope = {},
                      bool muted = false);
    void selectClipByIndex(int clipIndex, bool opened);
    int getSelectedClipIndex() const;
    struct MidiClipInfo {
        QString clipId;
        QString contentId;
        int trackIndex;
        QString name;
        double startBeat;
        double lengthBeats;
        double sourceOffsetBeats;
        bool loopEnabled;
        double loopStartBeats;
        double loopLengthBeats;
        bool isUnique;
        bool muted;
        QColor clipColor;
        QString groupId;
        bool locked;
        QString takeGroupId;
        int takeLane = 0;
        bool takeActive = true;
    };
    struct AudioClipInfo {
        QString clipId;
        QString contentId;
        int trackIndex;
        QString name;
        double startBeat;
        double lengthBeats;
        double sourceOffsetBeats;
        bool loopEnabled;
        double loopStartBeats;
        double loopLengthBeats;
        double warpLengthBeats;
        double sourceTempoBpm;
        double tempoConfidence;
        int sourceSignatureNumerator;
        int sourceSignatureDenominator;
        bool tempoDetected;
        std::vector<double> transientSeconds;
        std::vector<AudioWarpMarker> warpMarkers;
        double pitchShiftSemitones;
        int syncMode;
        double targetBars;
        double beatsPerBar;
        double sourceDurationSeconds;
        float gain;
        float pan;
        bool normalize;
        bool reverse;
        bool reversePolarity;
        double fadeInBeats;
        double fadeOutBeats;
        bool isUnique;
        QString sourcePath;
        bool mediaOffline;
        std::vector<AudioGainEnvelopePoint> gainEnvelope;
        double fadeInCurve;
        double fadeOutCurve;
        bool muted;
        QColor clipColor;
        QString groupId;
        bool locked;
        QString takeGroupId;
        int takeLane = 0;
        bool takeActive = true;
    };
    std::vector<MidiClipInfo> getMidiClips() const;
    std::vector<AudioClipInfo> getAudioClips() const;
    void updateSelectedAudioClipSync(double sourceTempoBpm, double pitchShiftSemitones,
                                     double lengthBeats = -1.0, double tempoConfidence = 0.0,
                                     int sourceSignatureNumerator = 0,
                                     int sourceSignatureDenominator = 0,
                                     bool tempoDetected = false);
    void updateSelectedAudioClipSyncMode(int syncMode, double targetBars);
    void updateSelectedAudioClipTransients(const std::vector<double>& transientSeconds);
    bool quantizeSelectedAudioClipTransients(double gridBeats);
    bool updateSelectedAudioClipWarpMarker(int markerIndex, double targetBeat,
                                           bool captureUndo = true);
    bool toggleSelectedAudioClipWarpMarker(double sourceSeconds, double targetBeat);
    bool removeSelectedAudioClipWarpMarker(int markerIndex);
    bool updateSelectedAudioClipGainEnvelope(
        const std::vector<AudioGainEnvelopePoint>& points);
    bool clearSelectedAudioClipWarpMarkers();
    void updateSelectedAudioClipProcessing(float gain, float pan, bool normalize,
                                           bool reverse, bool reversePolarity);
    void updateSelectedAudioClipFades(double fadeInBeats, double fadeOutBeats);
    bool updateSelectedAudioClipFadeCurves(double fadeInCurve, double fadeOutCurve);
    bool updateSelectedAudioClipSourceRange(double sourceInBeats, double sourceOutBeats);
    int refreshAudioMediaStatus();
    int relinkSelectedAudioContent(const QString& sourcePath, double sourceDurationSeconds);
    QStringList missingAudioSourcePaths() const;
    void updateAudioClipLengthsForTempo(double projectTempoBpm);
    void clearAllClips();
    bool selectedClipIsAudio() const;
    QStringList selectedClipIds() const;
    bool selectedMidiClipInfo(MidiClipInfo* out) const;
    bool selectedAudioClipInfo(AudioClipInfo* out) const;
    bool renameSelectedClip(const QString& name);
    bool copySelectedClips();
    bool cutSelectedClips();
    bool pasteClips();
    bool clipboardContainsMidiContent(const QString& contentId) const;
    bool historyContainsMidiContent(const QString& contentId) const;
    bool undoArrangementEdit();
    bool redoArrangementEdit();
    void resetArrangementHistory();
    bool isArrangementClean() const;
    quint64 currentArrangementStateId() const;
    bool historyContainsArrangementState(quint64 stateId) const;
    void markArrangementCleanState();
    void beginArrangementTransaction();
    void markArrangementTransactionChanged();
    bool commitArrangementTransaction();
    void cancelArrangementTransaction();
    bool canCoalesceLastUndoState(quint64 stateId) const;
    bool discardLastUndoState(quint64 stateId);
    bool duplicateSelectedClip();
    bool setSelectedClipsMuted(bool muted);
    bool setAudioClipMutedById(const QString& clipId, bool muted);
    bool toggleSelectedClipsMuted();
    bool setSelectedClipsColor(const QColor& color);
    bool setSelectedClipsLoopEnabled(bool enabled, double loopStartBeats = -1.0,
                                     double loopLengthBeats = -1.0);
    bool toggleSelectedClipsLoopEnabled();
    bool setSelectedClipsGroupId(const QString& groupId);
    bool groupSelectedClips();
    bool ungroupSelectedClips();
    bool groupSelectedClipsAsTakes();
    bool setSelectedClipAsActiveTake();
    bool compSelectedTakeInTimeSelection();
    bool flattenSelectedTakeGroup();
    bool cycleSelectedTake(int direction);
    bool promoteSelectedClipToRecordedTake();
    void setSelectedClipTakeState(const QString& takeGroupId, int takeLane, bool active);
    void normalizeTakeGroups();
    bool setSelectedClipsLocked(bool locked);
    bool toggleSelectedClipsLocked();
    bool createCrossfadeForSelectedAudioClips();
    bool insertTimeSelection();
    bool deleteTimeSelection();
    bool splitSelectedClipAtPlayhead();
    bool deleteSelectedClip();
    bool makeSelectedAudioClipUnique(const QString& name, const QString& sourcePath);
    QString makeSelectedMidiClipUnique(const QString& name);
    int selectedClipReferenceCount() const;
    bool isClipForTrackAt(int clipIndex, int trackIndex) const;

    void setTrackCount(int count);
    int getTrackCount() const;
    void setTracks(const std::vector<std::shared_ptr<Track>>& tracks);

    void setPluginAutomationLane(int displayTrackIndex, int targetTrackIndex,
                                 bool audioFx, int slotIndex,
                                 const PluginAutomationLane& lane);
    void clearPluginAutomationLane();

    void updateSceneBounds();

signals:
    void trackLaneSelected(int trackIndex);
    void midiClipSelected(int trackIndex, const QString& name, double startBeat, double lengthBeats);
    void midiClipOpened(int trackIndex, const QString& name, double startBeat, double lengthBeats);
    void audioClipSelected(int trackIndex, const QString& name, double startBeat, double lengthBeats, const QString& sourcePath,
                           double sourceTempoBpm, double pitchShiftSemitones, int syncMode, double targetBars);
    void renameSelectedClipRequested();
    void makeSelectedClipUniqueRequested();
    void stemSeparationRequested(int stemCount);
    void selectedClipDeleted(const QString& clipId, const QString& contentId, bool audio);
    void contentChanged();
    void playheadSeekRequested(double beat);
    void loopRangeChanged(bool enabled, double startBeat, double endBeat);
    void timeSelectionChanged(bool enabled, double startBeat, double endBeat);
    void clipSelectionChanged();
    void clipClipboardChanged();
    void arrangementHistoryChanged(bool canUndo, bool canRedo);
    void arrangementCleanStateChanged(bool clean);
    void arrangementStateWillTransition(quint64 fromStateId, quint64 toStateId);
    void arrangementStateTransitioned(quint64 fromStateId, quint64 toStateId);
    void arrangementStateRestored();
    void pluginAutomationEdited(int trackIndex, bool audioFx, int slotIndex,
                                const PluginAutomationLane& lane);

protected:
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct MidiClip {
        QString clipId;
        QString contentId;
        int trackIndex;
        double startBeat;
        double lengthBeats;
        double sourceOffsetBeats;
        bool loopEnabled = false;
        double loopStartBeats = 0.0;
        double loopLengthBeats = 0.0;
        QString name;
        bool isUnique;
        bool muted = false;
        QColor clipColor;
        QString groupId;
        bool locked = false;
        QString takeGroupId;
        int takeLane = 0;
        bool takeActive = true;
    };
    struct AudioClip {
        QString clipId;
        QString contentId;
        int trackIndex;
        double startBeat;
        double lengthBeats;
        double sourceOffsetBeats;
        bool loopEnabled = false;
        double loopStartBeats = 0.0;
        double loopLengthBeats = 0.0;
        double warpLengthBeats;
        double sourceTempoBpm;
        double tempoConfidence;
        int sourceSignatureNumerator;
        int sourceSignatureDenominator;
        bool tempoDetected;
        std::vector<double> transientSeconds;
        std::vector<AudioWarpMarker> warpMarkers;
        double pitchShiftSemitones;
        int syncMode;
        double targetBars;
        double beatsPerBar;
        double sourceDurationSeconds;
        float gain;
        float pan;
        bool normalize;
        bool reverse;
        bool reversePolarity;
        double fadeInBeats;
        double fadeOutBeats;
        bool isUnique;
        QString name;
        QString sourcePath;
        bool mediaOffline = false;
        std::vector<AudioGainEnvelopePoint> gainEnvelope;
        double fadeInCurve = 0.0;
        double fadeOutCurve = 0.0;
        bool muted = false;
        QColor clipColor;
        QString groupId;
        bool locked = false;
        QString takeGroupId;
        int takeLane = 0;
        bool takeActive = true;
    };
    struct DraggedMidiClipState {
        int trackIndex;
        double startBeat;
        double lengthBeats;
        double sourceOffsetBeats;
    };
    struct DraggedAudioClipState {
        int trackIndex;
        double startBeat;
        double lengthBeats;
        double sourceOffsetBeats;
        double warpLengthBeats;
        double fadeInBeats;
        double fadeOutBeats;
        int syncMode;
        bool loopEnabled;
    };
    struct ArrangementState {
        quint64 stateId = 0;
        std::vector<MidiClip> midiClips;
        std::vector<AudioClip> audioClips;
        std::set<QString> selectedClipIds;
        QString primaryClipId;
        bool primaryIsAudio = false;
        bool timeSelectionEnabled = false;
        double timeSelectionStartBeat = 0.0;
        double timeSelectionEndBeat = 0.0;
    };

    double m_pixelsPerBeat;
    double m_snapBeats;
    double m_beatsPerBar;
    int m_signatureNumerator;
    int m_signatureDenominator;
    std::vector<MidiTimeSignaturePoint> m_timeSignatureMap;
    int m_trackHeight;
    int m_timelineHeight;
    int m_trackCount;
    QGraphicsScene* m_scene;
    std::vector<Track*> m_tracks;
    std::vector<MidiClip> m_midiClips;
    std::vector<AudioClip> m_audioClips;
    std::vector<MidiClip> m_midiClipClipboard;
    std::vector<AudioClip> m_audioClipClipboard;
    double m_clipboardOriginBeat;
    double m_clipboardSpanBeats;
    QString m_clipboardPrimaryClipId;
    bool m_clipboardPrimaryIsAudio;
    std::vector<ArrangementState> m_undoHistory;
    std::vector<ArrangementState> m_redoHistory;
    std::unique_ptr<ArrangementState> m_pendingDragUndoState;
    std::unique_ptr<ArrangementState> m_transactionBaseState;
    int m_arrangementTransactionDepth;
    bool m_arrangementTransactionChanged;
    quint64 m_currentArrangementStateId;
    quint64 m_cleanArrangementStateId;
    quint64 m_nextArrangementStateId;
    bool m_suppressUndoCapture;
    std::map<QString, int> m_midiContentRefs;
    std::map<QString, int> m_audioContentRefs;
    std::map<QString, int> m_firstMidiContentIndex;
    std::map<QString, int> m_firstAudioContentIndex;
    int m_selectedClipIndex;
    bool m_selectedClipIsAudio;
    std::set<QString> m_selectedClipIds;
    double m_playheadBeat;
    bool m_loopEnabled;
    bool m_loopDragging;
    double m_loopStartBeat;
    double m_loopEndBeat;
    double m_loopDragAnchorBeat;
    bool m_timeSelectionEnabled;
    bool m_timeSelectionDragging;
    double m_timeSelectionStartBeat;
    double m_timeSelectionEndBeat;
    double m_timeSelectionAnchorBeat;
    bool m_punchEnabled;
    double m_punchInBeat;
    double m_punchOutBeat;

    bool m_pendingClipDrag;
    bool m_marqueeSelecting;
    QPoint m_marqueeAnchorPos;
    QPoint m_marqueeCurrentPos;
    std::set<QString> m_marqueeBaseSelection;
    bool m_panning;
    QPoint m_lastPanPos;
    bool m_draggingClipCreate;
    bool m_editingExistingClip;
    bool m_existingClipChanged;
    bool m_resizingExistingClip;
    bool m_resizingExistingClipFromLeft;
    bool m_autoCrossfadeEnabled;
    enum class AudioFadeDrag { None, FadeIn, FadeOut };
    AudioFadeDrag m_audioFadeDrag;
    int m_audioWarpDragMarker;
    QString m_audioWarpDragContentId;
    bool m_duplicateOnDrag;
    double m_clipGrabOffsetBeat;
    std::map<QString, DraggedMidiClipState> m_draggedMidiClipStates;
    std::map<QString, DraggedAudioClipState> m_draggedAudioClipStates;
    int m_dragTrackIndex;
    double m_dragStartBeat;
    double m_dragCurrentBeat;
    QPoint m_dragStartPos;

    bool m_pluginAutomationVisible = false;
    int m_pluginAutomationTrackIndex = -1;
    int m_pluginAutomationTargetTrackIndex = -1;
    bool m_pluginAutomationAudioFx = false;
    int m_pluginAutomationSlotIndex = -1;
    PluginAutomationLane m_pluginAutomationLane;
    int m_pluginAutomationDragPoint = -1;
    bool m_pluginAutomationDragChanged = false;

    int trackIndexAt(const QPointF& scenePos) const;
    bool isMidiTrack(int trackIndex) const;
    bool isAudioTrack(int trackIndex) const;
    double beatAtX(double sceneX) const;
    double snapBeat(double beat) const;
    double snapBeatUnlessAlt(double beat, Qt::KeyboardModifiers modifiers) const;
    struct BarMarker {
        double beat = 0.0;
        int barNumber = 1;
        int numerator = 4;
        int denominator = 4;
        bool signatureChange = false;
    };
    MidiTimeSignaturePoint timeSignatureAtBeat(double beat) const;
    std::vector<BarMarker> barMarkers(double startBeat, double endBeat) const;
    QRectF clipRect(const MidiClip& clip) const;
    QRectF clipRect(const AudioClip& clip) const;
    QRectF takeLaneRect(double startBeat, double lengthBeats, int trackIndex,
                        const QString& takeGroupId, int takeLane, bool audio) const;
    int clipAt(const QPointF& scenePos) const;
    int audioClipAt(const QPointF& scenePos) const;
    int audioFadeHandleAt(const QPointF& scenePos, AudioFadeDrag* drag) const;
    bool selectedAudioCrossfadePair(int* leftIndex, int* rightIndex,
                                    double* overlapBeats) const;
    bool applyAutoCrossfadesForSelectedAudioClips();
    int audioWarpMarkerAt(const QPointF& scenePos, int* markerIndex) const;
    int createMidiClip(int trackIndex, double startBeat, double lengthBeats, const QString& clipName,
                       const QString& clipId = {}, const QString& contentId = {},
                       double sourceOffsetBeats = 0.0, bool muted = false);
    int createAudioClip(int trackIndex, double startBeat, double lengthBeats, const QString& clipName, const QString& sourcePath,
                        double sourceTempoBpm, double pitchShiftSemitones, double beatsPerBar, double sourceDurationSeconds,
                        const QString& clipId = {}, const QString& contentId = {},
                        double sourceOffsetBeats = 0.0, double warpLengthBeats = 0.0,
                        double tempoConfidence = 0.0, int sourceSignatureNumerator = 0,
                        int sourceSignatureDenominator = 0, bool tempoDetected = false,
                        const std::vector<double>& transientSeconds = {},
                        const std::vector<AudioWarpMarker>& warpMarkers = {},
                        const std::vector<AudioGainEnvelopePoint>& gainEnvelope = {},
                        bool muted = false);
    void scaleAudioClipWarp(AudioClip& clip, double warpLengthBeats);
    static void advanceMidiClipSourceOffset(MidiClip& clip, double deltaBeats);
    static void advanceAudioClipSourceOffset(AudioClip& clip, double deltaBeats);
    static void clampAudioClipFades(AudioClip& clip);
    void refreshAudioTakeCompSeams(const QString& takeGroupId);
    int contentReferenceCount(const QString& contentId, bool audio) const;
    void addContentReference(const QString& contentId, bool audio);
    void removeContentReference(const QString& contentId, bool audio);
    void rebuildContentReferences();
    bool isClipSelected(const QString& clipId) const;
    void addClipGroupToSelection(const QString& groupId);
    void removeClipGroupFromSelection(const QString& groupId);
    void expandSelectionToClipGroups();
    bool selectionContainsLockedClip() const;
    void selectFirstRemainingClip();
    void restorePrimaryClipSelection(const QString& clipId, bool audio);
    ArrangementState captureArrangementState() const;
    void restoreArrangementState(ArrangementState state);
    void pushUndoState();
    void pushUndoState(ArrangementState state);
    void captureSelectedClipDragState();
    bool moveSelectedClipsFromDrag(const QPointF& scenePos, Qt::KeyboardModifiers modifiers);
    bool resizeSelectedClipsFromDrag(double pointerBeat);
    void updateMarqueeSelection();
    bool nudgeSelectedClip(double beatDelta, int trackDirection, bool bypassSnap = false);
    void selectClip(int clipIndex, bool opened, bool preserveSelection = false);
    void selectAudioClip(int clipIndex, bool preserveSelection = false);
    void clearClipDrag();
    QRectF pluginAutomationRect() const;
    int pluginAutomationPointAt(const QPointF& scenePos) const;
    int movePluginAutomationPoint(int pointIndex, const QPointF& scenePos,
                                  Qt::KeyboardModifiers modifiers);
    void thinPluginAutomationPoints();
    void emitPluginAutomationEdit();
};
