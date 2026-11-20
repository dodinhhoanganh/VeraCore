#ifndef VeraCoreAUDIO_H
#define VeraCoreAUDIO_H
#include "../3rdparty/portaudio/include/portaudio.h"
#include "SpatialAudioTypes.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <mutex>
#include <vector>

struct VeraCoreMidiEvent
{
	int sampleOffset = 0;
	uint8_t status = 0;
	uint8_t data1 = 0;
	uint8_t data2 = 0;
	int layerId = 0;
	float gain = 1.0f;
	float pan = 0.0f;
	bool fromTimeline = false;
	bool externalPlugin = false;
	double captureBeat = -1.0;
	bool isSlide = false;
	bool isPortamento = false;
	int glideSamples = 0;
	bool isKeyswitch = false;
	uint64_t timelineGeneration = 0;
	double captureMonotonicSeconds = -1.0;
};

struct VeraCoreBeatMidiEvent
{
	double beat = 0.0;
	uint8_t status = 0;
	uint8_t data1 = 0;
	uint8_t data2 = 0;
	int layerId = 0;
	float gain = 1.0f;
	float pan = 0.0f;
	bool externalPlugin = false;
	bool isSlide = false;
	bool isPortamento = false;
	double glideDurationBeats = 0.0;
	bool isKeyswitch = false;
};

struct VeraCoreTempoPoint
{
	double beat = 0.0;
	double bpm = 120.0;
	double seconds = 0.0;
};

struct VeraCoreTimeSignaturePoint
{
	double beat = 0.0;
	int numerator = 4;
	int denominator = 4;
	int barNumber = 0;
};

struct VeraCoreMidiLayerRoute
{
	int layerId = 0;
	bool enabled = true;
	int transpose = 0;
	int midiChannel = -1;
	float gain = 1.0f;
	float pan = 0.0f;
	bool externalPlugin = false;
};

struct VeraCoreTrackSendState
{
	int trackId = -1;
	float gain = 1.0f;
	float pan = 0.0f;
	float stereoSeparation = 0.0f;
	std::array<float, 2> levels{};
	VeraCoreSpatialPannerState spatial;
	bool audible = true;
};

struct VeraCoreReturnBusState
{
	float gain = 1.0f;
	float pan = 0.0f;
	float stereoSeparation = 0.0f;
	bool muted = false;
	bool soloed = false;
	bool preFader = false;
};

inline constexpr int kVeraCoreReturnAudioFxTrackIdBase = -1024;
inline constexpr int VeraCoreReturnAudioFxTrackId(int returnBus) noexcept
{
	return kVeraCoreReturnAudioFxTrackIdBase - returnBus;
}
// Internal endpoint used to delay the direct master path against latent return
// chains. It never identifies a user track or plug-in chain.
inline constexpr int VeraCoreMasterPdcPathId = -2048;
// Final serial insert chain after dry/return summing and before the master
// fader. This ID is never a user track and deliberately has no parallel PDC
// delay line: plug-in latency is intrinsic to the final serial output path.
inline constexpr int VeraCoreMasterAudioFxTrackId = -2049;

using VeraCorePluginOutputSink = void (*)(void* context, int destinationTrackId,
	const float* left, const float* right, unsigned long framesCount,
	float gain, float pan, bool mono);
using VeraCoreExternalMidiRenderer = void (*)(void* context, int trackId, float* interleavedStereo,
	unsigned long framesCount, const VeraCoreMidiEvent* midiEvents, std::size_t midiEventCount,
	double sampleRate, double projectBeat, double projectSeconds, double projectBpm,
	double barStartBeat, int barNumber, int timeSignatureNumerator,
	int timeSignatureDenominator, bool cycleWrappedAtStart, int sourceFrameOffset,
	VeraCorePluginOutputSink outputSink, void* outputSinkContext, int renderBatchToken);
using VeraCoreExternalAudioFxRenderer = void (*)(void* context, int trackId, float* interleavedAudio,
	unsigned long framesCount, int channelCount, VeraCoreSpeakerLayout speakerLayout,
	double sampleRate, double projectBeat, double projectSeconds,
	double projectBpm, double barStartBeat, int barNumber,
	int timeSignatureNumerator, int timeSignatureDenominator,
	bool cycleWrappedAtStart, int renderBatchToken);
using VeraCoreExternalRenderBatchBegin = int (*)(void* context);
using VeraCoreExternalRenderBatchEnd = void (*)(void* context, int renderBatchToken);

struct VeraCoreAudioWarpPoint
{
	double localBeat = 0.0;
	double sourceProgress = 0.0;
};

struct VeraCoreAudioGainPoint
{
	double localBeat = 0.0;
	float gain = 1.0f;
};

struct VeraCoreAudioTimelineClip
{
	int trackId = -1;
	double startBeat = 0.0;
	double lengthBeats = 0.0;
	double sourceOffsetBeats = 0.0;
	bool loopEnabled = false;
	double loopStartBeats = 0.0;
	double loopLengthBeats = 0.0;
	double warpLengthBeats = 0.0;
	double sourceTempoBpm = 0.0;
	double pitchShiftSemitones = 0.0;
	int syncMode = 0;
	double targetBars = 0.0;
	double beatsPerBar = 4.0;
	int sampleRate = 44100;
	bool monoSource = false;
	std::vector<float> stereoSamples;
	std::shared_ptr<const std::vector<float>> sharedStereoSamples;
	std::shared_ptr<const std::vector<VeraCoreAudioWarpPoint>> sharedWarpPoints;
	std::shared_ptr<const std::vector<VeraCoreAudioGainPoint>> sharedGainEnvelope;
	bool warpPointsUseAbsoluteSourceProgress = false;
	float gain = 1.0f;
	float pan = 0.0f;
	float normalizationGain = 1.0f;
	bool reverse = false;
	bool reversePolarity = false;
	double fadeInBeats = 0.0;
	double fadeOutBeats = 0.0;
	double fadeInCurve = 0.0;
	double fadeOutCurve = 0.0;
};

struct VeraCoreAudioMeterState
{
	float peakLeft = 0.0f;
	float peakRight = 0.0f;
	float momentaryLufs = -std::numeric_limits<float>::infinity();
	float shortTermLufs = -std::numeric_limits<float>::infinity();
	float integratedLufs = -std::numeric_limits<float>::infinity();
	float loudnessRangeLu = -std::numeric_limits<float>::infinity();
};

class VeraCoreRealtimeLoudnessMeter
{
public:
	void Reset(double sampleRate);
	void ResetIntegrated() noexcept;
	VeraCoreAudioMeterState ProcessBlock(const float* interleavedStereo,
	                                     unsigned long framesCount) noexcept;

private:
	struct StereoBiquad {
		double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
		std::array<double, 2> z1{};
		std::array<double, 2> z2{};
		double Process(double input, size_t channel) noexcept;
	};
	static float EnergyToLufs(double energy) noexcept;
	static void PushEnergy(std::vector<double>& ring, size_t& writeIndex,
	                      size_t& filled, double& sum, double energy) noexcept;
	void AppendIntegratedBlock() noexcept;
	float CalculateIntegratedLufs() const noexcept;
	void AppendLoudnessRangeBlock() noexcept;
	float CalculateLoudnessRangeLu() const noexcept;
	float LoudnessHistogramPercentile(float gate, uint64_t gatedCount,
	                                 double fraction) const noexcept;
	StereoBiquad shelf;
	StereoBiquad highPass;
	std::vector<double> momentaryRing;
	std::vector<double> shortTermRing;
	size_t momentaryWrite = 0;
	size_t shortTermWrite = 0;
	size_t momentaryFilled = 0;
	size_t shortTermFilled = 0;
	double momentaryEnergy = 0.0;
	double shortTermEnergy = 0.0;
	static constexpr size_t kIntegratedHistogramBins = 801;
	std::array<double, kIntegratedHistogramBins> integratedEnergySums{};
	std::array<uint64_t, kIntegratedHistogramBins> integratedCounts{};
	size_t integratedStepFrames = 1;
	size_t integratedFramesSinceBlock = 0;
	size_t integratedWarmupFrames = 0;
	bool integratedStarted = false;
	float currentIntegratedLufs = -std::numeric_limits<float>::infinity();
	std::array<double, kIntegratedHistogramBins> lraEnergySums{};
	std::array<uint64_t, kIntegratedHistogramBins> lraCounts{};
	size_t lraStepFrames = 1;
	size_t lraFramesSinceBlock = 0;
	size_t lraWarmupFrames = 0;
	bool lraStarted = false;
	float currentLoudnessRangeLu = -std::numeric_limits<float>::infinity();
};

class VeraCoreTruePeakMeter
{
public:
	void Reset() noexcept;
	VeraCoreAudioMeterState ProcessBlock(const float* interleavedStereo,
	                                     unsigned long framesCount) noexcept;
	VeraCoreAudioMeterState Flush() noexcept;

private:
	std::array<float, 16> historyLeft{};
	std::array<float, 16> historyRight{};
	size_t historyWrite = 0;
	size_t historyCount = 0;
};

struct VeraCoreAudioHealthState
{
	uint64_t callbackCount = 0;
	double cpuLoad = 0.0;
	uint64_t inputUnderflows = 0;
	uint64_t inputOverflows = 0;
	uint64_t outputUnderflows = 0;
	uint64_t outputOverflows = 0;
	uint64_t primingOutputs = 0;
};

struct VeraCoreAudioStreamInfo
{
	double sampleRate = 0.0;
	unsigned long requestedBufferFrames = 0;
	unsigned long callbackBufferFrames = 0;
	double inputLatencySeconds = 0.0;
	double outputLatencySeconds = 0.0;
	int outputChannels = 0;
	VeraCoreSpeakerLayout speakerLayout = VeraCoreSpeakerLayout::Stereo;
	VeraCoreSpeakerLayout requestedSpeakerLayout = VeraCoreSpeakerLayout::Stereo;
	bool layoutFallback = false;
};

class VeraCoreAudio
{
public:
	VeraCoreAudio();
	~VeraCoreAudio();
	PaError OpenStreamASIO(double srate, int devnum, unsigned long framesPerBuffer = 512);
	PaError OpenStreamWASAPI(double srate, int devnum, unsigned long framesPerBuffer = 512);
	PaError CloseStream();
	static unsigned long NormalizeBufferFrames(unsigned long frames) noexcept;
	void SetPendingMidiEvents(const std::vector<VeraCoreMidiEvent>& events);
	void QueueRealtimeMidiEvent(const VeraCoreMidiEvent& event);
	std::vector<VeraCoreMidiEvent> TakePendingMidiEventsForHost();
	void SetMidiTimelineEvents(const std::vector<VeraCoreBeatMidiEvent>& events);
	void SetAudioTimelineClips(std::vector<VeraCoreAudioTimelineClip> clips);
	void SetRealtimeMidiLayerRoutes(const std::vector<VeraCoreMidiLayerRoute>& routes);
	void SetMidiTrackIds(const std::vector<int>& trackIds);
	void SetMixerRouting(const std::vector<VeraCoreTrackSendState>& sends,
	                     const std::array<VeraCoreReturnBusState, 2>& returns);
	void SetMasterState(float gain, float pan, float stereoSeparation, bool muted);
	void SetPanLawDb(float centerAttenuationDb);
	void SetSpeakerLayout(VeraCoreSpeakerLayout layout) noexcept;
	VeraCoreSpeakerLayout GetSpeakerLayout() const noexcept;
	int GetSpeakerChannelCount() const noexcept;
	void SetTransportState(bool playing, double bpm, double playheadBeat);
	void SetTempoMap(const std::vector<VeraCoreTempoPoint>& points);
	void SetTimeSignatureMap(const std::vector<VeraCoreTimeSignaturePoint>& points);
	void SetLoopRange(bool enabled, double startBeat, double endBeat);
	void SetAudioInputState(const std::vector<int>& monitorTrackIds, bool recording);
	void DrainRecordedAudio(std::vector<float>& destination, bool* overflowed = nullptr);
	void SetMetronomeState(bool enabled, int numerator, int denominator);
	void SetExternalMidiRenderer(VeraCoreExternalMidiRenderer renderer, void* context);
	void SetExternalAudioFxRenderer(VeraCoreExternalAudioFxRenderer renderer, void* context);
	void SetExternalRenderBatchCallbacks(VeraCoreExternalRenderBatchBegin begin,
		VeraCoreExternalRenderBatchEnd end, void* context);
	std::vector<VeraCoreMidiEvent> BuildMidiEventsForBlock(double blockStartBeat,
	                                                       double beatsPerSample,
	                                                       int frameCount);
	std::vector<VeraCoreMidiEvent> BuildPlaybackMidiEvents(int frameCount);
	std::vector<VeraCoreMidiEvent> BuildPlaybackMidiEvents(int frameCount, double* blockStartBeat, double* blockEndBeat);
	double GetStreamSampleRate() const { return streamSampleRate; }
	bool HasAudioInput() const { return streamInputChannels > 0; }
	bool HasOpenStream() const noexcept { return stream != nullptr; }
	PaError GetStreamActivity() const noexcept;
	double GetTransportPlayheadBeat() const;
	VeraCoreAudioMeterState GetAndResetMeterState();
	VeraCoreAudioMeterState GetAndResetReturnMeterState(int returnBus);
	void RequestLoudnessMeterReset() noexcept;
	void ReportCallbackStatus(PaStreamCallbackFlags flags, unsigned long framesCount) noexcept;
	VeraCoreAudioHealthState GetAudioHealthState() const noexcept;
	VeraCoreAudioStreamInfo GetStreamInfo() const noexcept;
	void ResetAudioHealthCounters() noexcept;
	void UpdateMeterState(const float* interleavedStereo, unsigned long framesCount);
	void RenderExternalMidiBlock(float* out, unsigned long framesCount,
		const std::vector<VeraCoreMidiEvent>& midiEvents, int trackId,
		double projectBeat, double projectSeconds, double projectBpm,
		double barStartBeat, int barNumber, int timeSignatureNumerator,
		int timeSignatureDenominator, bool cycleWrappedAtStart,
		int sourceFrameOffset = 0,
		VeraCorePluginOutputSink outputSink = nullptr,
		void* outputSinkContext = nullptr,
		int rendererBindingIndex = -1, int renderBatchToken = -1);
	void RenderExternalMidiBlock(float* out, unsigned long framesCount,
		const VeraCoreMidiEvent* midiEvents, std::size_t midiEventCount, int trackId,
		double projectBeat, double projectSeconds, double projectBpm,
		double barStartBeat, int barNumber, int timeSignatureNumerator,
		int timeSignatureDenominator, bool cycleWrappedAtStart,
		int sourceFrameOffset, VeraCorePluginOutputSink outputSink,
		void* outputSinkContext, int rendererBindingIndex, int renderBatchToken);
	void RenderMidiBlock(float* out, unsigned long framesCount,
		const std::vector<VeraCoreMidiEvent>& midiEvents, int trackId, int sourceFrameOffset = 0);
	void RenderMidiBlock(float* out, unsigned long framesCount,
		const VeraCoreMidiEvent* midiEvents, std::size_t midiEventCount,
		int trackId, int sourceFrameOffset = 0,
		double centerAttenuationDb = std::numeric_limits<double>::quiet_NaN());
	void RenderAudioTimelineBlock(float* out, unsigned long framesCount);
	void RenderAudioTimelineBlock(float* out, unsigned long framesCount, double blockStartBeat,
		double blockEndBeat, double blockStartSeconds = -1.0, double projectBpm = -1.0,
		double barStartBeat = -1.0, int barNumber = -1,
		int timeSignatureNumerator = 0, int timeSignatureDenominator = 0,
		bool cycleWrappedAtStart = false, int rendererBindingIndex = -1,
		int renderBatchToken = -1, int timelineBindingIndex = -1,
		bool accumulateToCallbackMixer = false);
	void RenderMetronomeBlock(float* out, unsigned long framesCount, bool enabled,
	                         double blockStartBeat,
	                         double blockEndBeat, double bpm, double signatureStartBeat,
	                         int numerator, int denominator);
	void ProcessAudioCallback(const float* input, float* output, unsigned long framesCount);
	PaStream* stream = nullptr;

private:
	mutable std::mutex midiStateMutex;
	std::mutex realtimeRouteMutex;
	std::mutex externalRendererUpdateMutex;
	std::mutex audioTimelineUpdateMutex;
	std::mutex mixerRoutingUpdateMutex;
	std::mutex metronomeStateWriteMutex;
	std::mutex voiceCommandWriteMutex;
	std::mutex timelineVoiceSeedUpdateMutex;

	struct SynthVoice
	{
		bool active = false;
		int pitch = 0;
		int channel = 0;
		int layerId = 0;
		bool fromTimeline = false;
		double phase = 0.0;
		double velocityGain = 0.0;
		double layerGain = 1.0;
		double pressureGain = 1.0;
		double gain = 0.0;
		double pan = 0.0;
		double panLeft = 0.7071067811865476;
		double panRight = 0.7071067811865476;
		double pitchBendSemitones = 0.0;
		double currentPitch = 0.0;
		double glideTargetPitch = 0.0;
		double glideStepPerSample = 0.0;
		int glideSamplesRemaining = 0;
		int noteOnCount = 0;
	};

	struct PitchBendState
	{
		int layerId = 0;
		int channel = 0;
		bool fromTimeline = false;
		double semitones = 0.0;
	};

	struct PressureState
	{
		int layerId = 0;
		int channel = 0;
		bool fromTimeline = false;
		double gain = 1.0;
	};
	struct TimelineVoiceSeed
	{
		std::array<SynthVoice, 512> voices{};
		std::array<PitchBendState, 1024> bends{};
		std::array<PressureState, 1024> pressures{};
		int voiceCount = 0;
		int bendCount = 0;
		int pressureCount = 0;
	};
	struct RealtimeVoiceRouteCommand
	{
		int layerId = -1;
		float gain = 1.0f;
		float pan = 0.0f;
	};

	std::shared_ptr<const std::vector<VeraCoreBeatMidiEvent>> midiTimelineEvents;
	std::shared_ptr<const std::vector<VeraCoreTempoPoint>> tempoMap;
	std::shared_ptr<const std::vector<VeraCoreTimeSignaturePoint>> timeSignatureMap;
	std::vector<VeraCoreMidiLayerRoute> realtimeLayerRoutes;
	std::vector<PitchBendState> pitchBendStates;
	std::vector<PressureState> pressureStates;
	std::vector<VeraCoreMidiEvent> callbackMidiEvents;
	std::vector<VeraCoreMidiEvent> callbackMidiEventsByTrack;
	std::array<std::size_t, 257> callbackMidiTrackEventOffsets{};
	std::vector<VeraCoreBeatMidiEvent> loopChaseEvents;
	std::vector<float> audioInputRing;
	std::vector<float> audioTrackScratch;
	std::vector<float> callbackStereoMasterScratch;
	std::vector<float> callbackSpatialBusScratch;
	std::vector<float> pluginMixerTrackScratch;
	std::array<std::vector<float>, 2> callbackReturnBusScratch;
	std::array<bool, 256> callbackPluginMixerActive{};
	std::array<int, 256> callbackPluginMixerActiveTrackIds{};
	int callbackPluginMixerActiveTrackCount = 0;
	int callbackPluginMixerFrames = 0;
	std::array<int, 256> midiTrackIds{};
	int midiTrackIdCount = 0;
	struct CallbackTrackMixState
	{
		float gain = 1.0f;
		float pan = 0.0f;
		float stereoSeparation = 0.0f;
		std::array<float, 2> sends{};
		VeraCoreSpatialPannerState spatial;
		bool audible = true;
		bool active = false;
	};
	struct MixerRoutingBinding
	{
		std::array<CallbackTrackMixState, 256> tracks{};
		std::array<VeraCoreReturnBusState, 2> returns{};
	};
	std::array<MixerRoutingBinding, 2> mixerRoutingBindings{};
	std::array<std::atomic<uint32_t>, 2> mixerRoutingReaders{};
	std::atomic<int> activeMixerRoutingBinding{0};
	int callbackMixerRoutingBindingIndex = -1;
	struct CallbackStereoGain
	{
		float left = 0.0f;
		float right = 0.0f;
	};
	std::array<CallbackStereoGain, 256> callbackTrackOutputGains{};
	std::array<std::array<CallbackStereoGain, 2>, 256> callbackTrackReturnGains{};
	std::array<std::array<float, 2>, 256> callbackTrackSendGains{};
	std::array<float, 256> callbackTrackAudibilityGains{};
	std::array<float, 256> callbackTrackDryGains{};
	std::array<bool, 256> callbackTrackMixGainInitialized{};
	std::array<float, 256> callbackTrackStereoSeparation{};
	// Two cascaded biquads per track: z1/z2 for each section.
	std::array<std::array<float, 4>, 256> callbackTrackLfeLowPassState{};
	std::array<float, 5> callbackLfeSectionOneCoefficients{};
	std::array<float, 5> callbackLfeSectionTwoCoefficients{};
	std::array<CallbackStereoGain, 2> callbackReturnOutputGains{};
	std::array<bool, 2> callbackReturnOutputGainInitialized{};
	std::array<float, 2> callbackReturnStereoSeparation{};
	int callbackReturnMixerFrames = 0;
	std::atomic<uint64_t> audioInputWriteIndex{0};
	std::atomic<uint64_t> audioInputReadIndex{0};
	std::atomic<bool> audioInputRecording{false};
	std::atomic<bool> audioInputOverflowed{false};
	std::array<std::atomic<int>, 256> audioInputMonitorTrackIds{};
	std::atomic<int> audioInputMonitorTrackCount{0};
	std::array<int, 256> callbackAudioInputMonitorTrackIds{};
	std::atomic<float> masterGain{1.0f};
	std::atomic<float> masterPan{0.0f};
	std::atomic<float> masterStereoSeparation{0.0f};
	std::atomic<bool> masterMuted{false};
	std::atomic<float> panLawDb{-3.0f};
	std::atomic<VeraCoreSpeakerLayout> speakerLayout{VeraCoreSpeakerLayout::Stereo};
	float callbackPanLawDb = -3.0f;
	std::atomic<uint64_t> masterMixGeneration{0};
	float callbackMasterGain = 1.0f;
	float callbackMasterPan = 0.0f;
	float callbackMasterStereoSeparation = 0.0f;
	bool callbackMasterMuted = false;
	CallbackStereoGain callbackMasterOutputGain{1.0f, 1.0f};
	bool callbackMasterOutputGainInitialized = false;
	struct CallbackTransportSegment {
		int frameOffset = 0;
		int frameCount = 0;
		double startBeat = 0.0;
		double endBeat = 0.0;
		double beatsPerSample = 0.0;
		double bpm = 120.0;
		double startSeconds = 0.0;
		double signatureStartBeat = 0.0;
		double barStartBeat = 0.0;
		int barNumber = 0;
		int numerator = 4;
		int denominator = 4;
		bool cycleWrappedAtStart = false;
	};
	// 4096-frame realtime blocks can contain 256 minimum-quantum loop spans;
	// retain additional room for coincident tempo/signature boundaries.
	std::array<CallbackTransportSegment, 320> callbackSegments{};
	int callbackSegmentCount = 0;
	bool callbackMetronomeEnabled = false;
	int callbackTimeSignatureNumerator = 4;
	int callbackTimeSignatureDenominator = 4;
	std::array<int, 256> callbackMidiTrackIds{};
	int callbackMidiTrackCount = 0;
	std::atomic<float> meterPeakLeft{0.0f};
	std::atomic<float> meterPeakRight{0.0f};
	std::atomic<float> meterMomentaryLufs{-std::numeric_limits<float>::infinity()};
	std::atomic<float> meterShortTermLufs{-std::numeric_limits<float>::infinity()};
	std::atomic<float> meterIntegratedLufs{-std::numeric_limits<float>::infinity()};
	std::atomic<float> meterLoudnessRangeLu{-std::numeric_limits<float>::infinity()};
	std::array<std::atomic<float>, 2> returnMeterPeakLeft{};
	std::array<std::atomic<float>, 2> returnMeterPeakRight{};
	std::atomic<bool> loudnessMeterResetRequested{false};
	VeraCoreTruePeakMeter masterTruePeakMeter;
	std::array<VeraCoreTruePeakMeter, 2> returnTruePeakMeters{};
	VeraCoreRealtimeLoudnessMeter masterLoudnessMeter;
	std::atomic<uint64_t> callbackCount{0};
	std::atomic<uint64_t> inputUnderflowCount{0};
	std::atomic<uint64_t> inputOverflowCount{0};
	std::atomic<uint64_t> outputUnderflowCount{0};
	std::atomic<uint64_t> outputOverflowCount{0};
	std::atomic<uint64_t> primingOutputCount{0};
	std::atomic<unsigned long> requestedBufferFrames{0};
	std::atomic<unsigned long> callbackBufferFrames{0};
	std::atomic<double> streamInputLatencySeconds{0.0};
	std::atomic<double> streamOutputLatencySeconds{0.0};
	bool realtimeLayerRoutingConfigured = false;
	std::atomic<bool> transportPlaying{false};
	std::atomic<double> transportBpm{120.0};
	std::atomic<double> transportPlayheadBeat{0.0};
	std::atomic<bool> requestedTransportPlaying{false};
	std::atomic<double> requestedTransportBpm{120.0};
	std::atomic<double> requestedTransportPlayheadBeat{0.0};
	std::atomic<uint64_t> transportCommandGeneration{0};
	uint64_t appliedTransportCommandGeneration = 0;
	std::atomic<bool> transportLoopEnabled{false};
	std::atomic<double> transportLoopStartBeat{0.0};
	std::atomic<double> transportLoopEndBeat{4.0};
	std::atomic<bool> metronomeEnabled{false};
	std::atomic<int> timeSignatureNumerator{4};
	std::atomic<int> timeSignatureDenominator{4};
	std::atomic<uint64_t> metronomeStateGeneration{0};
	double streamSampleRate = 44100.0;
	int streamInputChannels = 0;
	int streamOutputChannels = 2;
	VeraCoreSpeakerLayout streamSpeakerLayout = VeraCoreSpeakerLayout::Stereo;
	int callbackSpatialChunkStart = 0;
	int callbackSpatialChannelCount = 2;
	VeraCoreSpeakerLayout callbackSpeakerLayout = VeraCoreSpeakerLayout::Stereo;
	std::array<SynthVoice, 512> activeVoices{};
	std::array<TimelineVoiceSeed, 2> timelineVoiceSeeds{};
	std::array<std::atomic<uint32_t>, 2> timelineVoiceSeedReaders{};
	std::atomic<int> activeTimelineVoiceSeed{0};
	std::atomic<uint64_t> timelineVoiceControlGeneration{0};
	std::atomic<bool> timelineVoicesRequested{false};
	uint64_t appliedTimelineVoiceControlGeneration = 0;
	std::array<RealtimeVoiceRouteCommand, 2048> realtimeVoiceCommands{};
	std::atomic<uint64_t> realtimeVoiceCommandWrite{0};
	std::atomic<uint64_t> realtimeVoiceCommandRead{0};
	std::atomic<bool> clearAllVoicesRequested{false};
	std::atomic<bool> clearRealtimeVoicesRequested{false};
	struct PendingMidiQueueCell
	{
		std::atomic<uint64_t> sequence{0};
		VeraCoreMidiEvent event{};
	};
	static constexpr size_t kPendingMidiQueueCapacity = 8192;
	std::array<PendingMidiQueueCell, kPendingMidiQueueCapacity> pendingMidiQueue{};
	std::atomic<uint64_t> pendingMidiEnqueuePosition{0};
	std::atomic<uint64_t> pendingMidiDequeuePosition{0};
	std::atomic<uint64_t> externalTimelineGeneration{0};
	struct MidiSchedulerBinding
	{
		std::shared_ptr<const std::vector<VeraCoreBeatMidiEvent>> timelineEvents;
		std::shared_ptr<const std::vector<VeraCoreTempoPoint>> tempoMap;
		std::shared_ptr<const std::vector<VeraCoreTimeSignaturePoint>> timeSignatureMap;
		std::vector<VeraCoreBeatMidiEvent> loopChaseEvents;
		std::array<int, 256> trackIds{};
		int trackIdCount = 0;
		bool loopEnabled = false;
		double loopStartBeat = 0.0;
		double loopEndBeat = 4.0;
	};
	std::array<MidiSchedulerBinding, 2> midiSchedulerBindings{};
	std::array<std::atomic<uint32_t>, 2> midiSchedulerReaders{};
	std::atomic<int> activeMidiSchedulerBinding{0};
	struct ExternalRendererBinding
	{
		VeraCoreExternalMidiRenderer midiRenderer = nullptr;
		VeraCoreExternalAudioFxRenderer audioFxRenderer = nullptr;
		VeraCoreExternalRenderBatchBegin batchBegin = nullptr;
		VeraCoreExternalRenderBatchEnd batchEnd = nullptr;
		void* midiContext = nullptr;
		void* audioFxContext = nullptr;
		void* batchContext = nullptr;
	};
	std::array<ExternalRendererBinding, 2> externalRendererBindings{};
	std::array<std::atomic<uint32_t>, 2> externalRendererReaders{};
	std::atomic<int> activeExternalRendererBinding{0};
	struct AudioTimelineBinding
	{
		struct TrackSpan
		{
			int trackId = -1;
			std::size_t firstClip = 0;
			std::size_t clipCount = 0;
		};
		std::vector<VeraCoreAudioTimelineClip> clips;
		std::vector<double> clipPrefixMaxEndBeats;
		std::vector<TrackSpan> trackSpans;
	};
	std::array<AudioTimelineBinding, 2> audioTimelineBindings{};
	std::array<std::atomic<uint32_t>, 2> audioTimelineReaders{};
	std::atomic<int> activeAudioTimelineBinding{0};
	int AcquireExternalRendererBinding();
	void ReleaseExternalRendererBinding(int bindingIndex);
	void PublishExternalRendererBinding(const ExternalRendererBinding& binding);
	void RenderExternalAudioFxBlock(float* out, unsigned long framesCount,
		int trackId, double projectBeat, double projectSeconds, double projectBpm,
		double barStartBeat, int barNumber, int timeSignatureNumerator,
		int timeSignatureDenominator, bool cycleWrappedAtStart,
		int rendererBindingIndex = -1, int renderBatchToken = -1,
		int channelCount = 2,
		VeraCoreSpeakerLayout speakerLayout = VeraCoreSpeakerLayout::Stereo);
	int AcquireAudioTimelineBinding();
	void ReleaseAudioTimelineBinding(int bindingIndex);
	void PublishAudioTimelineBinding(std::vector<VeraCoreAudioTimelineClip> clips);
	bool QueueRealtimeVoiceRouteCommand(const VeraCoreMidiLayerRoute& route);
	int AcquireTimelineVoiceSeed();
	void ReleaseTimelineVoiceSeed(int seedIndex);
	void PublishTimelineVoiceSeed(const TimelineVoiceSeed& seed);
	void RequestClearAllVoices();
	void ApplyPendingVoiceControl();
	bool TryEnqueuePendingMidiEvent(const VeraCoreMidiEvent& event);
	bool TryDequeuePendingMidiEvent(VeraCoreMidiEvent& event);
	int AcquireMidiSchedulerBinding();
	void ReleaseMidiSchedulerBinding(int bindingIndex);
	void PublishMidiSchedulerBindingLocked();
	void ApplyPendingTransportCommand();
	void SeedTimelineVoicesAtPlayheadLocked(double playheadBeat);
	void SeedExternalTimelineAtPlayheadLocked(double playheadBeat);
	void QueueExternalTimelinePanicLocked();
	void BuildCallbackMidiEvents(int frameCount, double* blockStartBeat, double* blockEndBeat);
	void RebuildLoopChaseEventsLocked();
	void MixTrackToOutput(float* output, const float* trackOutput,
	                     unsigned long framesCount, int trackId);
	void MixCallbackReturnsToOutput(float* output, unsigned long framesCount,
		double projectBeat, double projectSeconds, double projectBpm,
		double barStartBeat, int barNumber, int timeSignatureNumerator,
		int timeSignatureDenominator, bool cycleWrappedAtStart,
		int rendererBindingIndex, int renderBatchToken);
	int AcquireMixerRoutingBinding() noexcept;
	void ReleaseMixerRoutingBinding(int bindingIndex) noexcept;
	void ResetCallbackMixSmoothingState() noexcept;
	void ApplyMasterToOutput(float* output, unsigned long framesCount);
	void ApplyMasterToMultichannel(float* output, unsigned long framesCount,
		int channelCount);
	void AccumulateStereoFrontToSpatial(const float* stereo, unsigned long framesCount,
		int destinationFrameOffset) noexcept;
	void DownmixSpatialToStereo(const float* spatial, float* stereo,
		unsigned long framesCount, int channelCount) noexcept;
	void AccumulateTimelineOutput(int destinationTrackId, const float* interleaved,
		unsigned long framesCount) noexcept;
	static void AccumulatePluginOutput(void* context, int destinationTrackId,
		const float* left, const float* right, unsigned long framesCount,
		float gain, float pan, bool mono);
};
#endif
