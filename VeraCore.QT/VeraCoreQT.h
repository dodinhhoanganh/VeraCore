#pragma once

#include <QtWidgets/QMainWindow>
#include <QList>
#include <map>
#include <vector>
#include <deque>
#include <memory>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QString>
#include <QDateTime>
#include <QByteArray>
#include <QJsonObject>
#include <cstddef>
#include <functional>
#include <limits>
#include "ui_VeraCoreQT.h"
#include "PluginManager.h"
#include "AudioGeneral.h"
#include "AudioFileDecoder.h"
#include "Track.h"
#include "TransportWidget.h"
#include "InspectorWidget.h"
#include "AudioEditorWidget.h"
#include "TrackWidget.h"
#include "PlaylistView.h"
#include "MidiClipModel.h"
#include "CommandRegistry.h"
#include "SelectionService.h"
#include "MixerParameterService.h"
#include "WorkspaceManager.h"
#include "KeymapProfileManager.h"

class MidiEditorWindow;
class ExternalPluginHost;
class StemSeparationService;
class QTreeWidgetItem;
class QCloseEvent;
class QEvent;
class QKeyEvent;
class QAction;
class QTabWidget;
class QProgressDialog;
struct AudioEncodeOptions;

class VeraCoreQT : public QMainWindow
{
    Q_OBJECT

public:
    void ConnectAudio(const shared_ptr<AudioGeneral>& audio);
    VeraCoreQT(QWidget *parent = nullptr);
    ~VeraCoreQT();
    shared_ptr<AudioGeneral> audioGeneral;

    void updateBrowserTree();
    void addSinglePluginToBrowser(const PluginInfo& info);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    Ui::VeraCoreQTClass ui;
    void OnPreferences();

    std::vector<std::shared_ptr<Track>> m_tracks;
    std::vector<TrackWidget*> m_trackWidgets;
    CommandRegistry* m_commandRegistry = nullptr;
    SelectionService* m_selectionService = nullptr;
    MixerParameterService* m_mixerParameters = nullptr;
    WorkspaceManager* m_workspaceManager = nullptr;
    KeymapProfileManager* m_keymapProfiles = nullptr;
    QAction* m_inspectorAction = nullptr;
    QAction* m_midiStepInputAction = nullptr;
    QAction* m_stemRecoveryAction = nullptr;
    QHash<QString, QAction*> m_workspaceActions;
    QHash<QString, QAction*> m_keymapActions;
    enum class ProjectIntegrityState {
        Unsaved,
        LegacyUnverified,
        Verified
    };
    ProjectIntegrityState m_projectIntegrityState = ProjectIntegrityState::Unsaved;
    struct ProjectTimingState {
        double tempo = 120.0;
        int numerator = 4;
        int denominator = 4;
        double snapBeats = 0.25;
        std::vector<MidiTempoPoint> tempoMap;
        std::vector<MidiTimeSignaturePoint> signatureMap;
    };
    struct ArrangementProjectTransition {
        quint64 beforeStateId = 0;
        quint64 afterStateId = 0;
        int trackInsertIndex = 0;
        std::vector<std::shared_ptr<Track>> addedTracks;
        int selectedTrackBefore = -1;
        int selectedTrackAfter = -1;
        bool timingChanged = false;
        ProjectTimingState timingBefore;
        ProjectTimingState timingAfter;
    };
    std::vector<ArrangementProjectTransition> m_arrangementProjectTransitions;
    struct ProjectUndoTransition {
        quint64 beforeStateId = 0;
        quint64 afterStateId = 0;
        QString mergeKey;
        qint64 committedAtMs = 0;
        std::function<void()> applyBefore;
        std::function<void()> applyAfter;
    };
    std::vector<ProjectUndoTransition> m_projectUndoTransitions;
    bool m_suppressProjectUndoCapture = false;
    std::unique_ptr<ExternalPluginHost> m_pluginHost;
    StemSeparationService* m_stemSeparator = nullptr;
    QPointer<QProgressDialog> m_stemProgressDialog;
    PlaylistView::AudioClipInfo m_pendingStemClip{};
    QString m_pendingStemJobId;
    QString m_pendingStemManifestPath;
    bool m_pendingStemInputRendered = false;
    struct StemBatchItem {
        PlaylistView::AudioClipInfo clip;
        int stemCount = 0;
    };
    std::deque<StemBatchItem> m_stemBatchQueue;
    int m_stemBatchTotal = 0;
    int m_stemBatchProcessed = 0;
    int m_stemBatchSucceeded = 0;
    int m_stemBatchFailed = 0;
    bool m_stemBatchActive = false;
    bool m_stemBatchCancelRequested = false;

    PlaylistView* m_playlistView = nullptr;

    void addTrack(std::shared_ptr<Track> track);
    void startStemSeparation(int stemCount);
    bool startStemSeparationForClip(const PlaylistView::AudioClipInfo& clip,
                                    int stemCount);
    void startNextStemBatchItem();
    void finishStemBatch();
    int stemBatchOverallProgress(int itemPercent) const;
    bool importSeparatedStems(const QString& jobId, const QStringList& stemNames,
                              const QStringList& stemPaths);
    QStringList stemRecoveryRoots() const;
    void refreshStemRecoveryAction();
    void showStemRecoveryDialog();
    void rebuildTrackWidgets(int preferredTrackIndex);
    ProjectTimingState captureProjectTimingState() const;
    bool projectTimingMatches(const ProjectTimingState& state) const;
    void applyProjectTimingState(const ProjectTimingState& state);
    void applyArrangementProjectTransition(quint64 fromStateId, quint64 toStateId);
    void finishArrangementProjectTransition(quint64 fromStateId, quint64 toStateId);
    void pruneArrangementProjectTransitions();
    void performGlobalUndo();
    void performGlobalRedo();
    void applyMidiDocumentState(const QString& contentId,
                                const MidiClipDocument& document);
    bool commitProjectUndoStep(quint64 beforeStateId, bool nonArrangementDirtyBefore,
                               const QString& mergeKey,
                               std::function<void()> applyBefore,
                               std::function<void()> applyAfter);
    void initDefaultTracks();
    void applyTrackMixerControls(Track* track, float volume, float pan, float stereoSeparation,
                                 bool muted, bool soloed, bool armed, bool monitoring);
    void applyTrackSpatialPanner(Track* track, VeraCoreSpatialPannerState state);
    void applyTrackSendLevel(Track* track, int returnBus, float level);
    void applyReturnBusControls(int returnBus, float volume, float pan, float stereoSeparation,
                                bool muted, bool soloed, bool preFader);
    void applyMasterBusControls(float volume, float pan, float stereoSeparation, bool muted);
    void applyProjectPanLawDb(double centerAttenuationDb);
    void applyProjectSpeakerLayout(VeraCoreSpeakerLayout layout);
    void resetReturnBuses();
    WorkspacePreset captureWorkspacePreset() const;
    void applyWorkspacePreset(const WorkspacePreset& preset);

    TransportWidget* m_transportWidget;
    InspectorWidget* m_inspectorWidget;
    AudioEditorWidget* m_audioEditorWidget = nullptr;
    QTabWidget* m_detailTabs = nullptr;

    // Piano-roll editor: one floating window reused for all clips; note data
    // lives in documents owned here, keyed by stable content ID, so
    // closing the editor never destroys notes.
    MidiEditorWindow* m_midiEditor = nullptr;
    QPointer<QWidget> m_focusBeforeMidiEditor;
    std::map<QString, std::unique_ptr<MidiClipDocument>> m_midiClipDocs;
    std::vector<MidiTempoPoint> m_tempoMap{{0.0, 120.0}};
    std::vector<MidiTimeSignaturePoint> m_timeSignatureMap{{0.0, 4, 4}};
    int m_selectedTrackIndex = -1;
    int m_currentMidiTrackIndex = -1;
    QString m_currentMidiClipKey;
    QString m_currentMidiClipId;
    QString m_projectPath;
    QJsonObject m_projectPassthroughRoot;
    int m_missingMediaCount = 0;
    int m_autoRelinkedMediaCount = 0;
    bool m_projectDirty = false;
    bool m_nonArrangementDirty = false;
    bool m_araDocumentDirty = false;
    bool m_araAnalysisWasActive = false;
    bool m_restoringProject = false;
    QString m_currentMidiClipName;
    double m_currentMidiClipStartBeat = 0.0;
    double m_currentMidiClipLengthBeats = 0.0;
    double m_currentMidiClipSourceOffsetBeats = 0.0;
    bool m_hasCurrentMidiClip = false;
    bool m_isPlaying = false;
    bool m_isRecordingMidi = false;
    bool m_isRecordingAudio = false;
    bool m_offlineRenderActive = false;
    std::vector<int> m_recordingAudioTracks;
    std::vector<float> m_recordedAudioSamples;
    double m_audioRecordingStartBeat = 0.0;
    int m_audioRecordingSampleRate = 44100;
    bool m_audioRecordingOverflowed = false;
    double m_recordingStopBeatOverride = -1.0;
    double m_recordingCountInEndBeat = -1.0;
    bool m_recordingCountInComplete = true;
    bool m_countInMetronomeOverride = false;
    double m_playheadBeat = 0.0;
    bool m_loopEnabled = false;
    double m_loopStartBeat = 0.0;
    double m_loopEndBeat = 4.0;
    bool m_punchEnabled = false;
    double m_punchInBeat = 0.0;
    double m_punchOutBeat = 4.0;
    int m_projectKeyRoot = 0;
    bool m_projectMinorScale = false;
    double m_projectSnapBeats = 0.25;
    int m_projectPpq = 960;
    int m_countInBars = 0;
    int m_midiRecordMode = 0;
    double m_midiRecordQuantizeBeats = 0.0;
    double m_projectPanLawDb = -3.0;
    VeraCoreSpeakerLayout m_projectSpeakerLayout = VeraCoreSpeakerLayout::Stereo;
    std::vector<std::pair<double, QAction*>> m_panLawActions;
    std::vector<std::pair<VeraCoreSpeakerLayout, QAction*>> m_speakerLayoutActions;
    std::vector<std::pair<int, QAction*>> m_midiRecordModeActions;
    std::vector<std::pair<double, QAction*>> m_midiRecordQuantizeActions;
    double m_lastSyncedBeat = 0.0;
    QTimer* m_playbackTimer = nullptr;
    QTimer* m_araControlTimer = nullptr;
    int m_pluginHealthPollDivider = 0;
    int m_audioHealthPollDivider = 0;
    QTimer* m_recoveryTimer = nullptr;
    QTimer* m_recoveryRetryTimer = nullptr;
    bool m_recoveryWritePending = false;
    bool m_recoveryPromptScheduled = false;
    quint64 m_projectRevision = 0;
    quint64 m_lastRecoveryRevision = 0;
    QElapsedTimer m_recoveryIdleClock;
    struct StoredAraArchive {
        QByteArray factoryPersistentIdUtf8;
        QByteArray documentArchiveIdUtf8;
        QByteArray bytes;
    };
    std::vector<StoredAraArchive> m_projectAraArchives;
    bool m_projectAraArchivesRestored = true;
    struct RecordedMidiStart {
        double beat = 0.0;
        int velocity = 100;
    };
    struct RetrospectiveMidiCapture {
        VeraCoreMidiEvent event;
        bool transportRunning = false;
    };
    std::map<int, std::vector<RecordedMidiStart>> m_recordedMidiStarts;
    std::deque<RetrospectiveMidiCapture> m_retrospectiveMidiBuffer;
    std::unique_ptr<MidiClipDocument> m_midiRecordingBeforeDocument;
    QString m_midiRecordingContentId;
    double m_midiRecordingClipStartBeat = 0.0;
    double m_midiRecordingClipLengthBeats = 0.0;
    double m_midiRecordingClipSourceOffsetBeats = 0.0;
    double m_midiRecordingRangeStartBeat = 0.0;
    bool m_midiRecordingSawLoopWrap = false;
    std::vector<MidiNote> m_midiRecordingAddedNotes;
    std::vector<MidiEvent> m_midiRecordingAddedEvents;
    quint64 m_midiRecordingBeforeStateId = 0;
    bool m_midiRecordingNonArrangementDirtyBefore = false;
    bool m_midiRecordingChanged = false;
    bool m_midiRecordingUndoActive = false;
    struct StepMidiChordNote {
        int channel = 0;
        int pitch = 60;
        int velocity = 100;
        bool isKeyswitch = false;
    };
    bool m_midiStepInputEnabled = false;
    std::map<int, int> m_stepMidiHeldKeys;
    std::vector<StepMidiChordNote> m_stepMidiChord;
    std::vector<MidiExpressionMapEntry> m_currentMidiExpressionMap;
    QString m_stepMidiContentId;
    double m_stepMidiProjectBeat = 0.0;
    double m_stepMidiClipStartBeat = 0.0;
    double m_stepMidiClipLengthBeats = 0.0;
    double m_stepMidiClipSourceOffsetBeats = 0.0;
    double m_stepMidiStepBeats = 0.25;
    int m_stepMidiGatePercent = 100;
    int m_stepMidiChordGatePercent = 100;
    int m_stepMidiVelocityOverride = 0;
    int m_stepMidiChordVelocityOverride = 0;
    int m_stepMidiInsertMode = 0;
    int m_stepMidiChordInsertMode = 0;
    QList<int> m_lastMainSplitterSizes;
    QList<int> m_lastCenterSplitterSizes;
    QList<int> m_lastPlaylistSplitterSizes;

    struct OfflineRenderStats {
        double peakLeft = 0.0;
        double peakRight = 0.0;
        double truePeakMaximum = 0.0;
        double preClampPeakLeft = 0.0;
        double preClampPeakRight = 0.0;
        double preClampPeakMaximum = 0.0;
        double rmsLeft = 0.0;
        double rmsRight = 0.0;
        double rmsMaximum = 0.0;
        double integratedLufs = -std::numeric_limits<double>::infinity();
        double momentaryMaxLufs = -std::numeric_limits<double>::infinity();
        double shortTermMaxLufs = -std::numeric_limits<double>::infinity();
        double loudnessRangeLu = -std::numeric_limits<double>::infinity();
        qint64 sampleFrames = 0;
        qint64 clippedSamples = 0;
        int channelCount = 2;
    };
    struct OfflineAudioClip {
        int trackIndex = -1;
        QString clipId;
        QString contentId;
        QString name;
        QString sourcePath;
        double startBeat = 0.0;
        double lengthBeats = 0.0;
        double sourceOffsetBeats = 0.0;
        bool loopEnabled = false;
        double loopStartBeats = 0.0;
        double loopLengthBeats = 0.0;
        double warpLengthBeats = 0.0;
        double sourceDurationSeconds = 0.0;
        std::vector<AudioWarpMarker> warpMarkers;
        double sourceTempoBpm = 0.0;
        double pitchShiftSemitones = 0.0;
        int syncMode = 0;
        double targetBars = 0.0;
        double beatsPerBar = 4.0;
        float gain = 1.0f;
        float pan = 0.0f;
        bool normalize = false;
        bool reverse = false;
        bool reversePolarity = false;
        double fadeInBeats = 0.0;
        double fadeOutBeats = 0.0;
        std::vector<AudioGainEnvelopePoint> gainEnvelope;
        double fadeInCurve = 0.0;
        double fadeOutCurve = 0.0;
    };
    struct CachedDecodedAudio {
        qint64 fileSize = -1;
        QDateTime lastModified;
        DecodedAudioFile audio;
        std::shared_ptr<const std::vector<float>> playbackSamples;
        float normalizationGain = 1.0f;
        quint64 lastUse = 0;
    };
    mutable std::map<QString, CachedDecodedAudio> m_audioDecodeCache;
    mutable std::map<QString, CachedDecodedAudio> m_audioSyncDecodeCache;
    mutable quint64 m_audioCacheUseCounter = 0;
    int m_parameterEditorTrackIndex = -1;
    int m_parameterEditorReturnBus = -1;
    int m_parameterEditorSlotIndex = -1;
    bool m_parameterEditorAudioFx = false;
    int m_parameterEditorGestureDepth = 0;
    QString m_parameterEditorDeviceName;
    InspectorWidget::AutomationWriteMode m_automationWriteMode =
        InspectorWidget::AutomationWriteMode::Read;
    uint32_t m_automationParameterId = 0;
    QString m_automationParameterName;
    bool m_hasAutomationTarget = false;
    bool m_automationWriteActive = false;
    bool m_automationWriteOwnsHostGesture = false;
    int m_automationWriteTrackIndex = -1;
    int m_automationWriteSlotIndex = -1;
    bool m_automationWriteAudioFx = false;
    uint64_t m_automationWriteInstanceEpoch = 0;
    uint32_t m_automationWriteParameterId = 0;
    QString m_automationWriteParameterName;
    double m_automationWriteValue = 0.0;
    double m_automationWriteStartBeat = 0.0;
    double m_automationWriteEndBeat = 0.0;
    double m_automationWriteLastBeat = -1.0;
    bool m_automationWriteRangeOverflow = false;
    bool m_automationWriteBufferOverflow = false;
    bool m_automationWriteStreamOverflow = false;
    quint64 m_automationWriteGestureSerial = 0;
    struct AutomationWriteRange {
        double startBeat = 0.0;
        double endBeat = 0.0;
    };
    struct NativeAutomationGestureKey {
        int trackIndex;
        bool audioFx;
        int slotIndex;
        uint64_t instanceEpoch;
        uint32_t parameterId;
        bool operator<(const NativeAutomationGestureKey& other) const noexcept
        {
            if (trackIndex != other.trackIndex) return trackIndex < other.trackIndex;
            if (audioFx != other.audioFx) return audioFx < other.audioFx;
            if (slotIndex != other.slotIndex) return slotIndex < other.slotIndex;
            if (instanceEpoch != other.instanceEpoch) return instanceEpoch < other.instanceEpoch;
            return parameterId < other.parameterId;
        }
    };
    struct NativeAutomationGestureState {
        uint64_t serial;
        uint64_t lastSeen;
        bool closed;
    };
    std::vector<AutomationWriteRange> m_automationWriteRanges;
    std::vector<PluginAutomationPoint> m_automationWriteBuffer;
    std::map<NativeAutomationGestureKey, NativeAutomationGestureState>
        m_nativeAutomationGestureHighWater;
    uint64_t m_nativeAutomationGestureSeenCounter = 0;

    void openMidiEditorForClip(int trackIndex, const QString& name, double startBeat, double lengthBeats);
    void syncMidiTimelineToAudio();
    void syncTempoMapToAudio();
    void syncTimeSignatureMapToAudio();
    void syncAudioTimelineToAudio();
    void syncRealtimeMidiLayersToAudio();
    void captureLivePluginStatesToProject();
    void quiesceExternalAudioForDeviceChange();
    void resumeAudioAfterDeviceChange(bool streamActive);
    void restoreProjectAraArchives();
    void processPluginParameterEdits();
    void processPluginStateChanges();
    void processAraHostRequests();
    void refreshSelectedAraProcessingAlgorithms();
    void refreshInspectorPluginHostStates(bool hostEvaluated);
    void refreshInspectorPluginParameters();
    void refreshPluginAutomationLane();
    void applyPluginAutomationLane(int trackIndex, bool audioFx, int slotIndex,
                                   const PluginAutomationLane& lane,
                                   const QString& mergeKey = {});
    void beginAutomationWriteAtCurrentValue();
    void armAutomationWriteTarget(int trackIndex, bool audioFx, int slotIndex,
                                  uint32_t parameterId, const QString& parameterName,
                                  uint64_t instanceEpoch = 0);
    void extendAutomationWriteRange(double beat);
    void addAutomationWriteRange(double startBeat, double endBeat);
    void closeAutomationWriteAtTransportDiscontinuity(double previousBeat);
    bool acceptNativeAutomationGestureSerial(int trackIndex, bool audioFx, int slotIndex,
                                             uint64_t instanceEpoch, uint32_t parameterId,
                                             uint64_t gestureSerial, bool terminal);
    void writePluginAutomationPoint(uint32_t parameterId, const QString& parameterName,
                                    double beat, double normalizedValue, bool force = false,
                                    double curveTension = 0.0,
                                    const PluginAutomationCurvePolynomial* curvePolynomial = nullptr);
    void finishTouchAutomationWrite(double eventBeat);
    void finishAutomationWrite(double finalBeat = -1.0);
    void syncMixerRoutingToAudio();
    void syncPluginTransportToHost();
    void syncTransportDisplayToPlayhead();
    bool hasActiveAudioOutput(QString* reason = nullptr) const;
    void seekPlayhead(double beat);
    void updateMidiEditorPlayhead();
    void refreshMidiEditorGhostClips();
    void refreshMidiEditorExpressionMap();
    void syncProjectMetadataToMidiDocuments();
    void resetProjectModel(bool createDefaultTracks);
    bool saveProjectFile(const QString& path, QString* errorText = nullptr);
    bool serializeProject(QByteArray& json, bool capturePluginState,
                          bool recoverySnapshot, QString* errorText = nullptr);
    bool loadProjectFile(const QString& path, QString* errorText = nullptr);
    bool saveProjectInteractive(bool forceSaveAs);
    bool confirmDiscardChanges();
    void setProjectDirty(bool dirty = true);
    void refreshProjectDirtyFromArrangement(bool contentMutation);
    void updateProjectWindowTitle();
    QString recoveryFilePath() const;
    QString recoveryFilePathForRevision(quint64 revision) const;
    void scheduleRecoverySnapshot();
    void scheduleRecoveryRetry(const QString& reason);
    bool writeRecoverySnapshotNow(QString* errorText = nullptr);
    void offerRecoveryIfPresent();
    void markRecoveryStateClean();
    void sendTimelineMidiToExternalOutputs(double startBeat, double endBeat) const;
    void sendTimelineMidiChaseToExternalOutputs(double beat) const;
    void sendExternalMidiPanic() const;
    QString buildClipDocumentKey(const QString& contentId) const;
    int importMidiFileToTracks(const QString& path, int firstTargetTrack,
                               double insertionBeat, bool preserveFileStart,
                               QString* errorText);
    void finalizeImportedMidiClip(int trackIndex, const QString& clipName, double clipStart,
                                  double clipLength, const MidiClipDocument& importedDoc,
                                  bool offerTimingImport = true);
    std::vector<VeraCoreBeatMidiEvent> buildTimelineEvents() const;
    const MidiClipDocument* getCurrentSelectedClipDocument() const;
    MidiClipDocument* getCurrentSelectedClipDocument();
    void handleRecordedMidiInput(const VeraCoreMidiEvent& event);
    void bufferRetrospectiveMidiInput(const VeraCoreMidiEvent& event);
    bool captureRetrospectiveMidi();
    bool handleStepMidiInput(const VeraCoreMidiEvent& event);
    bool commitStepMidiChord();
    void clearStepMidiChord();
    bool moveStepInputCursor(int stepDelta);
    bool tieStepInputNotes();
    void markMidiRecordingChanged();
    void stopMidiRecording(bool finalizeActiveNotes);
    void syncAudioInputState();
    void drainAudioRecording();
    void stopAudioRecording(bool finalizeRecording);
    void restoreCountInMetronome();
    bool encodeRenderedAudio(const QString& sourceWav, const QString& destinationPath, const QString& format,
                             const AudioEncodeOptions& options, QString* errorText,
                             const std::function<bool(qint64, qint64)>& progress = {}) const;
    bool writeMidiDocumentToFile(const QString& path, const MidiClipDocument& doc, QString* errorText,
                                 bool includeConductorMeta = true) const;
    bool writeMidiArrangementToFile(const QString& path, QString* errorText) const;
    bool importMidiTracksFromFile(const QString& path, std::vector<MidiClipDocument>& tracks,
                                  MidiClipDocument& conductor, QString* errorText) const;
    bool importMidiDocumentFromFile(const QString& path, MidiClipDocument& outDoc, QString* errorText) const;
    bool renderOfflineWavWithSynth(const QString& path, const std::vector<VeraCoreBeatMidiEvent>& events, const std::vector<OfflineAudioClip>& audioClips, double lengthBeats, int sampleRate, OfflineRenderStats* stats = nullptr, const std::function<bool(quint64, quint64)>& progress = {}, bool isolatedClipRender = false, double timelineBeatOrigin = 0.0) const;
    double arrangementLengthBeats() const;
    bool renderOfflineWav(const QString& path, const std::vector<VeraCoreBeatMidiEvent>& events, const std::vector<OfflineAudioClip>& audioClips, double lengthBeats, int sampleRate, OfflineRenderStats* stats = nullptr, const std::function<bool(quint64, quint64)>& progress = {}) const;
    void renderOffline(const QString& format);
    const DecodedAudioFile* cachedDecodedAudio(const QString& path) const;
    const DecodedAudioFile* cachedDecodedAudioForClipSync(const QString& path, double sourceTempoBpm, double pitchShiftSemitones,
                                                          int syncMode, double targetBars, double beatsPerBar, double lengthBeats,
                                                          double projectTempoBpm, int sampleRate, bool* processed) const;
    void trimDecodedAudioCache(std::map<QString, CachedDecodedAudio>& cache,
                               size_t maxEntries, size_t maxSamples) const;
    std::shared_ptr<const std::vector<float>> sharedPlaybackSamples(
        const DecodedAudioFile* decoded) const;
    float normalizationGainForDecoded(const DecodedAudioFile* decoded) const;

private slots:
    void onTrackSelected(TrackWidget* widget, Track* track);
    void onBrowserSearchChanged(const QString& text);
    void onPlaylistTrackSelected(int trackIndex);
    void onMidiClipSelected(int trackIndex, const QString& name, double startBeat, double lengthBeats);
    void onMidiClipOpened(int trackIndex, const QString& name, double startBeat, double lengthBeats);
    void onAudioClipSelected(int trackIndex, const QString& name, double startBeat, double lengthBeats, const QString& sourcePath,
                             double sourceTempoBpm, double pitchShiftSemitones, int syncMode, double targetBars);
    void onMakeSelectedClipUnique();
    void onOpenCurrentMidiEditor();
    void onTransportPlay();
    void onTransportPause();
    void onTransportStop();
    void onPlaybackTick();
    void onAddLayerRequested();
    void onLayerEnabledChanged(int layerIndex, bool enabled);
    void onLayerParametersChanged(int layerIndex, float gain, float pan, int transpose, int midiChannel);
    void onLayerOutputRoutingRequested(int layerIndex);
    void onRemoveLayerRequested(int layerIndex);
    void onBrowserItemActivated(QTreeWidgetItem* item, int column);
};
