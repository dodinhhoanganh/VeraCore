#pragma once

#include "../VeraCore.Audio/VeraCoreAudio.h"
#include "Track.h"
#include "MidiClipModel.h"
#include "AraHost.h"

#include <memory>
#include <cstdint>
#include <map>
#include <vector>
#include <QByteArray>
#include <QString>

// Snapshot produced on the control thread. It is safe to read without asking a
// plugin from the realtime callback. "ARA ready" is intentionally stricter
// than merely finding an ARA factory: all required model/render roles must be
// bound before the host reports the integration as usable.
struct PluginTechnologyCapabilities
{
    QString format;
    bool x64 = true;
    bool audioPorts = false;
    bool notePorts = false;
    bool parameters = false;
    bool persistentState = false;
    bool nativeGui = false;
    bool latency = false;
    bool tail = false;
    bool offlineRender = false;
    bool noteNames = false;
    bool noteExpression = false;
    bool keyswitches = false;
    bool physicalUiMapping = false;
    bool unitPrograms = false;
    bool processContextRequirements = false;
    bool prefetchableSupport = false;
    bool voiceInfo = false;
    bool remoteControls = false;
    bool trackInfo = false;
    bool contextMenu = false;
    bool presetLoad = false;
    bool timerSupport = false;
    bool threadPool = false;
    bool araSdkCompiled = false;
    bool araFactory = false;
    bool araDocumentController = false;
    bool araPlaybackRenderer = false;
    bool araEditorRenderer = false;
    bool araEditorView = false;
    bool araContentAccess = false;

    bool araReady() const noexcept
    {
        return araSdkCompiled && araFactory && araDocumentController &&
            araPlaybackRenderer && araEditorRenderer && araEditorView && araContentAccess;
    }
};

struct PluginAudioOutputBus
{
    QString stablePortId;
    QString name;
    int busIndex = 0;
    int channelCount = 0;
    bool main = false;
};

struct PluginParameterInfo
{
    uint32_t stableId = 0;
    QString name;
    QString module;
    QString unit;
    QString displayValue;
    double normalizedValue = 0.0;
    double defaultNormalizedValue = 0.0;
    int stepCount = 0;
    bool automatable = false;
    bool readOnly = false;
    bool hidden = false;
    bool bypass = false;
};

enum class PluginStateTarget
{
    Layer,
    AudioFx
};

// A control-thread snapshot pair produced after a plug-in reports an edit.
// Full state is never captured from the realtime callback.  gestureSerial is
// stable across the begin/perform/end sequence so the project history can
// coalesce a long editor gesture into one undo step.
struct PluginStateChange
{
    PluginStateTarget target = PluginStateTarget::Layer;
    int layerId = -1;
    int trackIndex = -1;
    int slotIndex = -1;
    uint64_t instanceEpoch = 0;
    uint64_t gestureSerial = 0;
    QByteArray beforeState;
    QByteArray afterState;
};

enum class PluginParameterEditPhase : uint8_t
{
    Begin,
    Value,
    End
};

// Bounded native-editor event copied from VST2/VST3/CLAP callbacks. Mapping to
// a Layer/Audio-FX slot happens on the control thread; callbacks never allocate
// or take the host graph mutex.
struct PluginParameterEditEvent
{
    PluginStateTarget target = PluginStateTarget::Layer;
    PluginParameterEditPhase phase = PluginParameterEditPhase::Value;
    int layerId = -1;
    int trackIndex = -1;
    int slotIndex = -1;
    uint64_t instanceEpoch = 0;
    uint64_t gestureSerial = 0;
    double captureAgeSeconds = 0.0;
    double capturedProjectBeat = -1.0;
    bool streamOverflowed = false;
    uint64_t captureCounter = 0;
    uint32_t parameterId = 0;
    double normalizedValue = 0.0;
};

class ExternalPluginHost
{
public:
    ExternalPluginHost();
    ~ExternalPluginHost();

    void syncLayers(const std::vector<std::shared_ptr<Track>>& tracks,
                   const std::array<ReturnBusState, kReturnBusCount>& returns,
                   const MasterBusState& master,
                   double sampleRate,
				   VeraCoreSpeakerLayout speakerLayout = VeraCoreSpeakerLayout::Stereo);
    void syncAraModelGraph(const std::vector<AraAudioSourceDescriptor>& sources,
                           const std::vector<AraPlaybackRegionDescriptor>& regions,
                           const std::vector<AraTempoEntryDescriptor>& tempoEntries,
                           const std::vector<AraBarSignatureDescriptor>& barSignatures);
    void setAraEditorSelection(int trackIndex,
                               const std::vector<std::string>& clipIds);
    bool captureAraArchives(std::vector<AraDocumentArchive>& archives);
    bool restoreAraArchives(const std::vector<AraDocumentArchive>& archives);
    bool consumeAraTransportRequest(AraTransportRequest& request) noexcept;
    bool pollAraStatus(AraHostStatus& status) noexcept;
    bool selectedAraProcessingAlgorithms(
        AraProcessingAlgorithmSelection& selection);
    bool setSelectedAraProcessingAlgorithm(int algorithmIndex);
    void setOfflineProcessing(bool offline);
    void setPanLawDb(double centerAttenuationDb) noexcept;
    void setTransport(bool playing, bool recording, double bpm, double projectBeat,
                      int timeSignatureNumerator, int timeSignatureDenominator,
                      double projectSeconds = -1.0, double barStartBeat = -1.0,
                      int barNumber = -1, bool cycleActive = false,
                      double cycleStartBeat = 0.0, double cycleEndBeat = 0.0,
                      double cycleStartSeconds = 0.0, double cycleEndSeconds = 0.0,
                      double cycleStartBpm = 120.0,
                      double cycleStartBarStartBeat = 0.0,
                      int cycleStartBarNumber = 0,
                      int cycleStartNumerator = 4,
                      int cycleStartDenominator = 4);
    bool isLayerHosted(int layerId) const;
    bool isLayerLoaded(int layerId) const;
    bool isLayerFaulted(int layerId) const;
    bool isAudioFxLoaded(int trackIndex, int slotIndex) const;
    bool isAudioFxFaulted(int trackIndex, int slotIndex) const;
    uint64_t instanceEpochForLayer(int layerId) const;
    uint64_t instanceEpochForAudioFx(int trackIndex, int slotIndex) const;
    PluginTechnologyCapabilities capabilitiesForLayer(int layerId) const;
    PluginTechnologyCapabilities capabilitiesForAudioFx(int trackIndex, int slotIndex) const;
    std::vector<PluginAudioOutputBus> outputBusesForLayer(int layerId) const;
    std::vector<MidiExpressionMapEntry> expressionMapForLayer(int layerId, int midiChannel = 0) const;
    std::vector<PluginParameterInfo> parametersForLayer(int layerId) const;
    std::vector<PluginParameterInfo> parametersForAudioFx(int trackIndex, int slotIndex) const;
    bool beginLayerParameterGesture(int layerId, uint32_t parameterId);
    bool setLayerParameterNormalized(int layerId, uint32_t parameterId, double value);
    bool endLayerParameterGesture(int layerId, uint32_t parameterId);
    bool beginAudioFxParameterGesture(int trackIndex, int slotIndex, uint32_t parameterId);
    bool setAudioFxParameterNormalized(int trackIndex, int slotIndex,
                                       uint32_t parameterId, double value);
    bool endAudioFxParameterGesture(int trackIndex, int slotIndex, uint32_t parameterId);
    QByteArray captureLayerState(int layerId) const;
    QByteArray captureAudioFxState(int trackIndex, int slotIndex) const;
    std::vector<PluginStateChange> consumePluginStateChanges();
    std::vector<PluginParameterEditEvent> consumePluginParameterEdits(
        bool* overflowed = nullptr);
    std::map<int, uint32_t> trackLatencySamples() const;
    uint32_t maxTailSamples(double maxTailSeconds = 30.0) const;
    void panic();
    void render(float* interleavedStereo, unsigned long framesCount,
                const std::vector<VeraCoreMidiEvent>& midiEvents, double sampleRate);
    void renderTrack(int trackId, float* interleavedStereo, unsigned long framesCount,
                     const std::vector<VeraCoreMidiEvent>& midiEvents, double sampleRate,
                     double projectBeat, double projectSeconds = -1.0,
                     double projectBpm = -1.0, double barStartBeat = -1.0,
                     int barNumber = -1, int timeSignatureNumerator = 0,
                     int timeSignatureDenominator = 0,
                     bool cycleWrappedAtStart = false,
                     int sourceFrameOffset = 0,
                     VeraCorePluginOutputSink outputSink = nullptr,
                     void* outputSinkContext = nullptr);
    void renderTrack(int trackId, float* interleavedStereo, unsigned long framesCount,
                     const VeraCoreMidiEvent* midiEvents, std::size_t midiEventCount,
                     double sampleRate, double projectBeat, double projectSeconds,
                     double projectBpm, double barStartBeat, int barNumber,
                     int timeSignatureNumerator, int timeSignatureDenominator,
                     bool cycleWrappedAtStart, int sourceFrameOffset,
                     VeraCorePluginOutputSink outputSink, void* outputSinkContext,
                     int renderBatchToken = -1);
    void renderAudioFx(int trackId, float* interleavedStereo, unsigned long framesCount,
                       double sampleRate, double projectBeat, double projectSeconds = -1.0,
                       double projectBpm = -1.0, double barStartBeat = -1.0,
                       int barNumber = -1, int timeSignatureNumerator = 0,
                       int timeSignatureDenominator = 0,
                       bool cycleWrappedAtStart = false, int renderBatchToken = -1);
	void renderAudioFxMultichannel(int trackId, float* interleavedAudio,
		unsigned long framesCount, int channelCount,
		VeraCoreSpeakerLayout speakerLayout, double sampleRate,
		double projectBeat, double projectSeconds = -1.0,
		double projectBpm = -1.0, double barStartBeat = -1.0,
		int barNumber = -1, int timeSignatureNumerator = 0,
		int timeSignatureDenominator = 0,
		bool cycleWrappedAtStart = false, int renderBatchToken = -1);
	int beginRenderBatch() noexcept;
	void endRenderBatch(int renderBatchToken) noexcept;

    static void audioCallback(void* context, int trackId, float* interleavedStereo, unsigned long framesCount,
							  const VeraCoreMidiEvent* midiEvents, std::size_t midiEventCount,
                              double sampleRate, double projectBeat, double projectSeconds,
                              double projectBpm, double barStartBeat, int barNumber,
                              int timeSignatureNumerator, int timeSignatureDenominator,
                              bool cycleWrappedAtStart, int sourceFrameOffset,
							  VeraCorePluginOutputSink outputSink, void* outputSinkContext,
							  int renderBatchToken);
    static void audioFxCallback(void* context, int trackId, float* interleavedAudio,
								unsigned long framesCount, int channelCount,
								VeraCoreSpeakerLayout speakerLayout,
								double sampleRate, double projectBeat,
                                double projectSeconds, double projectBpm, double barStartBeat,
                                int barNumber, int timeSignatureNumerator,
								int timeSignatureDenominator, bool cycleWrappedAtStart,
								int renderBatchToken);
	static int beginRenderBatchCallback(void* context) noexcept;
	static void endRenderBatchCallback(void* context, int renderBatchToken) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
