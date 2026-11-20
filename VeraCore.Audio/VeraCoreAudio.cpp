#include "VeraCoreAudio.h"
#include <portaudio.h>
#include <string.h>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace {
std::array<float, 5> lfeLowPassCoefficients(double sampleRate, double q) noexcept
{
    constexpr double cutoffHz = 120.0;
    constexpr double twoPi = 6.28318530717958647692;
    const double rate = std::clamp(
        std::isfinite(sampleRate) ? sampleRate : 48000.0, 8000.0, 768000.0);
    const double omega = twoPi * cutoffHz / rate;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / (2.0 * q);
    const double inverseA0 = 1.0 / (1.0 + alpha);
    const float b0 = static_cast<float>(0.5 * (1.0 - cosine) * inverseA0);
    return {b0, static_cast<float>((1.0 - cosine) * inverseA0), b0,
        static_cast<float>(-2.0 * cosine * inverseA0),
        static_cast<float>((1.0 - alpha) * inverseA0)};
}

float processLfeLowPassSection(float input, const std::array<float, 5>& coefficients,
                               float& z1, float& z2) noexcept
{
    const float output = coefficients[0] * input + z1;
    z1 = coefficients[1] * input - coefficients[3] * output + z2;
    z2 = coefficients[2] * input - coefficients[4] * output;
    if (!std::isfinite(z1) || std::abs(z1) < 1.0e-20f) z1 = 0.0f;
    if (!std::isfinite(z2) || std::abs(z2) < 1.0e-20f) z2 = 0.0f;
    return std::isfinite(output) ? output : 0.0f;
}
} // namespace
#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__)
#include <xmmintrin.h>
#define VERACORE_AUDIO_HAS_MXCSR 1
#endif
#define bzero(b,len) (memset((b), '\0', (len)), (void) 0)

namespace
{
static_assert(std::atomic<double>::is_always_lock_free,
              "VeraCore requires lock-free 64-bit transport atomics.");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "VeraCore requires lock-free 64-bit audio health atomics.");
constexpr int kMinPluginCycleFrames = 16;

// Subnormal tails can turn otherwise cheap filters, meters and plug-ins into
// severe CPU spikes. Apply DAZ+FTZ for the complete real-time callback and
// restore the driver's thread state at the boundary so VeraCore does not leak
// a floating-point mode into PortAudio or another host callback.
class ScopedAudioDenormalMode
{
public:
    ScopedAudioDenormalMode() noexcept
    {
#if defined(VERACORE_AUDIO_HAS_MXCSR)
        m_previous = _mm_getcsr();
        constexpr unsigned int denormalsAreZero = 0x0040u;
        constexpr unsigned int flushToZero = 0x8000u;
        const unsigned int realtimeMode = m_previous | denormalsAreZero | flushToZero;
        if (realtimeMode != m_previous) _mm_setcsr(realtimeMode);
#endif
    }

    ~ScopedAudioDenormalMode() noexcept
    {
#if defined(VERACORE_AUDIO_HAS_MXCSR)
        _mm_setcsr(m_previous);
#endif
    }

    ScopedAudioDenormalMode(const ScopedAudioDenormalMode&) = delete;
    ScopedAudioDenormalMode& operator=(const ScopedAudioDenormalMode&) = delete;

private:
#if defined(VERACORE_AUDIO_HAS_MXCSR)
    unsigned int m_previous = 0;
#endif
};

static double noteToFrequency(double pitch)
{
    constexpr double referencePitch = 69.0;
    return 440.0 * std::pow(2.0, (pitch - referencePitch) / 12.0);
}

static double synthLayerSample(double phase, int layerId)
{
    constexpr double twoPi = 6.2831853071795864769;
    const int timbre = ((layerId % 4) + 4) % 4;
    if (timbre == 1) {
        return (phase / twoPi) * 2.0 - 1.0;
    }
    if (timbre == 2) {
        return std::sin(phase) >= 0.0 ? 1.0 : -1.0;
    }
    if (timbre == 3) {
        const double normalized = phase / twoPi;
        return 1.0 - 4.0 * std::fabs(normalized - 0.5);
    }
    return std::sin(phase);
}

static bool sameRouteIdentity(const std::vector<VeraCoreMidiLayerRoute>& a,
                              const std::vector<VeraCoreMidiLayerRoute>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].layerId != b[i].layerId ||
            a[i].enabled != b[i].enabled ||
            a[i].transpose != b[i].transpose ||
            a[i].midiChannel != b[i].midiChannel ||
            a[i].externalPlugin != b[i].externalPlugin) {
            return false;
        }
    }
    return true;
}

static int midiEventSortRank(uint8_t status, uint8_t data2)
{
    const uint8_t statusHi = status & 0xF0;
    if (statusHi == 0x80 || (statusHi == 0x90 && data2 == 0)) {
        return 0;
    }
    if (statusHi == 0xA0 || statusHi == 0xB0 || statusHi == 0xC0 ||
        statusHi == 0xD0 || statusHi == 0xE0) {
        return 1;
    }
    if (statusHi == 0x90) {
        return 2;
    }
    return 3;
}

static bool isValidChannelMidiEvent(uint8_t status, uint8_t data1, uint8_t data2)
{
    const uint8_t statusHi = status & 0xF0;
    if (statusHi < 0x80 || statusHi > 0xE0 || data1 > 127) {
        return false;
    }
    return statusHi == 0xC0 || statusHi == 0xD0 || data2 <= 127;
}

static int clampFrameCount(unsigned long framesCount)
{
    const unsigned long maxInt = static_cast<unsigned long>(std::numeric_limits<int>::max());
    return framesCount > maxInt ? std::numeric_limits<int>::max() : static_cast<int>(framesCount);
}

static int clampSampleOffset(double offset, int frameCount)
{
    if (frameCount <= 0 || !std::isfinite(offset) || offset <= 0.0) {
        return 0;
    }
    if (offset >= static_cast<double>(frameCount - 1)) {
        return frameCount - 1;
    }
    return static_cast<int>(offset);
}

static int framesUntilBeatBoundary(double beatDistance, double beatsPerSample, int frameLimit)
{
    if (frameLimit <= 1 || !std::isfinite(beatDistance) || !std::isfinite(beatsPerSample) ||
        beatDistance <= 0.0 || beatsPerSample <= 0.0) return std::max(1, frameLimit);
    const double frames = std::ceil(beatDistance / beatsPerSample);
    if (!std::isfinite(frames) || frames >= static_cast<double>(frameLimit)) return frameLimit;
    return std::max(1, static_cast<int>(frames));
}

static unsigned long controlRampFrameCount(double sampleRate, unsigned long blockFrames)
{
    if (blockFrames == 0) return 1;
    constexpr double rampSeconds = 0.005;
    const double requested = std::isfinite(sampleRate) && sampleRate > 0.0
        ? std::ceil(sampleRate * rampSeconds)
        : static_cast<double>(blockFrames);
    const unsigned long rampFrames = requested >= static_cast<double>(blockFrames)
        ? blockFrames
        : std::max(1ul, static_cast<unsigned long>(requested));
    return std::max(1ul, rampFrames);
}

static int glideSampleCount(double durationBeats, double beatsPerSample)
{
    if (!std::isfinite(durationBeats) || !std::isfinite(beatsPerSample) ||
        durationBeats <= 0.0 || beatsPerSample <= 0.0) return 1;
    const double samples = std::ceil(durationBeats / beatsPerSample);
    return samples >= static_cast<double>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : std::max(1, static_cast<int>(samples));
}

static double pitchBendToSemitones(uint8_t data1, uint8_t data2)
{
    const int value = (static_cast<int>(data2 & 0x7f) << 7) | static_cast<int>(data1 & 0x7f);
    constexpr double bendRangeSemitones = 2.0;
    return ((static_cast<double>(value) - 8192.0) / 8192.0) * bendRangeSemitones;
}

static double pressureToGain(uint8_t value)
{
    return 0.55 + (static_cast<double>(value & 0x7f) / 127.0) * 0.9;
}

static double finiteClamp(double value, double low, double high, double fallback)
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

static float finiteSample(float value)
{
    return std::isfinite(value) ? value : 0.0f;
}

static float finiteFloatSample(double value)
{
    constexpr double limit = static_cast<double>(std::numeric_limits<float>::max());
    return static_cast<float>(finiteClamp(value, -limit, limit, 0.0));
}

struct PanGains
{
    double left = 1.0;
    double right = 1.0;
};

static double normalizedPanLawDb(double centerAttenuationDb)
{
    return finiteClamp(centerAttenuationDb, -12.0, 0.0, -3.0);
}

static PanGains panGains(double gain, double pan, bool monoSource,
                         double centerAttenuationDb = -3.0)
{
    const double safeGain = finiteClamp(gain, 0.0, 2.0, 1.0);
    const double safePan = finiteClamp(pan, -1.0, 1.0, 0.0);
    if (monoSource) {
        constexpr double halfPi = 1.57079632679489661923;
        const double lawDb = normalizedPanLawDb(centerAttenuationDb);
        if (lawDb >= -1.0e-9) {
            const double left = safePan <= 0.0 ? 1.0 : std::cos(safePan * halfPi);
            const double right = safePan >= 0.0 ? 1.0 : std::cos(-safePan * halfPi);
            return {safeGain * left, safeGain * right};
        }
        constexpr double equalPowerCenterDb = -3.010299956639812;
        const double exponent = std::clamp(lawDb / equalPowerCenterDb, 0.0, 4.0);
        const double angle = (safePan + 1.0) * (halfPi * 0.5);
        const double leftBase = std::max(0.0, std::cos(angle));
        const double rightBase = std::max(0.0, std::sin(angle));
        return {safeGain * (leftBase <= 1.0e-15 ? 0.0 : std::pow(leftBase, exponent)),
                safeGain * (rightBase <= 1.0e-15 ? 0.0 : std::pow(rightBase, exponent))};
    }
    return {safeGain * (safePan <= 0.0 ? 1.0 : 1.0 - safePan),
            safeGain * (safePan >= 0.0 ? 1.0 : 1.0 + safePan)};
}

// FL-style direction: turn left (-1) for 200% side, right (+1) for mono merge.
static void applyStereoSeparation(float left, float right, double separation,
                                  float& outputLeft, float& outputRight)
{
    const double width = 1.0 - finiteClamp(separation, -1.0, 1.0, 0.0);
    const double mid = 0.5 * (static_cast<double>(left) + right);
    const double side = 0.5 * (static_cast<double>(left) - right) * width;
    outputLeft = finiteFloatSample(mid + side);
    outputRight = finiteFloatSample(mid - side);
}

static void sanitizeFiniteStereoBlock(float* out, unsigned long framesCount)
{
    if (!out || framesCount == 0) return;
    const size_t sampleCount = static_cast<size_t>(framesCount) * 2u;
    for (size_t sample = 0; sample < sampleCount; ++sample)
        out[sample] = finiteSample(out[sample]);
}

static double pitchShiftRatio(double semitones)
{
    const double safeSemitones = finiteClamp(semitones, -48.0, 48.0, 0.0);
    return std::pow(2.0, safeSemitones / 12.0);
}

static double shapedAudioFade(double progress, double curve)
{
    constexpr double halfPi = 1.57079632679489661923;
    const double base = std::sin(halfPi * std::clamp(progress, 0.0, 1.0));
    const double safeCurve = finiteClamp(curve, -1.0, 1.0, 0.0);
    if (safeCurve < 0.0)
        return base + (std::sqrt(base) - base) * -safeCurve;
    return base + (base * base - base) * safeCurve;
}

static void clampHardwareOutputBlock(float* out, unsigned long framesCount)
{
    if (!out || framesCount == 0) {
        return;
    }
    const size_t sampleCount = static_cast<size_t>(framesCount) * 2u;
    for (size_t i = 0; i < sampleCount; ++i) {
        out[i] = static_cast<float>(finiteClamp(static_cast<double>(out[i]), -1.0, 1.0, 0.0));
    }
}

static void clampHardwareOutputBlock(float* out, unsigned long framesCount, int channelCount)
{
    if (!out || framesCount == 0 || channelCount <= 0) return;
    const size_t sampleCount = static_cast<size_t>(framesCount) *
        static_cast<size_t>(channelCount);
    for (size_t sample = 0; sample < sampleCount; ++sample)
        out[sample] = static_cast<float>(finiteClamp(out[sample], -1.0, 1.0, 0.0));
}

static double combinedVoiceGain(double velocityGain, double layerGain, double pressureGain)
{
    return finiteClamp(velocityGain * layerGain * pressureGain, 0.0, 1.0, 0.0);
}

static bool controllerStateKeyLess(int leftLayerId, int leftChannel, bool leftTimeline,
	int rightLayerId, int rightChannel, bool rightTimeline) noexcept
{
	if (leftLayerId != rightLayerId) return leftLayerId < rightLayerId;
	if (leftChannel != rightChannel) return leftChannel < rightChannel;
	return leftTimeline < rightTimeline;
}

constexpr size_t kMaxCallbackMidiEvents = 8192;
constexpr size_t kMaxTempoMapPoints = 4096;
constexpr size_t kMaxTimeSignatureMapPoints = 4096;
constexpr int kMaxTempoSplitsPerCallback = 48;
constexpr double kMinTempoBpm = 20.0;
constexpr double kMaxTempoBpm = 999.0;
constexpr int kLayerIdStride = 1024;
constexpr int kPluginMixerChunkFrames = 4096;
constexpr size_t kMaxRealtimeFrames = 65536u;
constexpr size_t kTruePeakPhases = 3;
constexpr size_t kTruePeakTaps = 12;
constexpr size_t kTruePeakHistorySize = 16;
static_assert((kTruePeakHistorySize & (kTruePeakHistorySize - 1u)) == 0u);
constexpr std::array<std::array<double, kTruePeakTaps>, kTruePeakPhases>
    kTruePeakCoefficients{{
        {{-0.000060728637998627675, 0.002086005593157234,
          -0.010327505047492028, 0.033387776218619415,
          -0.09049110527107501, 0.2816103162584527,
          0.8938803894347742, -0.1507597506895705,
          0.05548871098145975, -0.019212910719945072,
          0.005025878191616285, -0.000627076311998364}},
        {{-0.0003656163281706088, 0.0046988549470318485,
          -0.020121561020860813, 0.061115066896961015,
          -0.1641083815398346, 0.6187816370448733,
          0.6187816370448733, -0.1641083815398346,
          0.061115066896961015, -0.020121561020860813,
          0.0046988549470318485, -0.0003656163281706088}},
        {{-0.0006270763119983641, 0.0050258781916162855,
          -0.019212910719945075, 0.055488710981459764,
          -0.15075975068957054, 0.8938803894347744,
          0.28161031625845273, -0.09049110527107503,
          0.03338777621861942, -0.01032750504749203,
          0.0020860055931572344, -0.00006072863799862769}}
    }};

float truePeakFromHistory(const std::array<float, kTruePeakHistorySize>& history,
                          size_t nextWrite) noexcept
{
    double peak = 0.0;
    for (const auto& phase : kTruePeakCoefficients) {
        double sample = 0.0;
        for (size_t tap = 0; tap < kTruePeakTaps; ++tap) {
            const size_t index = (nextWrite - 1u - tap) & (kTruePeakHistorySize - 1u);
            sample += static_cast<double>(history[index]) * phase[tap];
        }
        if (std::isfinite(sample)) peak = std::max(peak, std::fabs(sample));
    }
    return static_cast<float>(peak);
}
} // namespace

void VeraCoreTruePeakMeter::Reset() noexcept
{
    historyLeft.fill(0.0f);
    historyRight.fill(0.0f);
    historyWrite = 0;
    historyCount = 0;
}

VeraCoreAudioMeterState VeraCoreTruePeakMeter::ProcessBlock(
    const float* interleavedStereo, unsigned long framesCount) noexcept
{
    VeraCoreAudioMeterState result;
    if (!interleavedStereo || framesCount == 0) return result;
    for (unsigned long frame = 0; frame < framesCount; ++frame) {
        const size_t sampleIndex = static_cast<size_t>(frame) * 2u;
        const float left = std::isfinite(interleavedStereo[sampleIndex])
            ? interleavedStereo[sampleIndex] : 0.0f;
        const float right = std::isfinite(interleavedStereo[sampleIndex + 1u])
            ? interleavedStereo[sampleIndex + 1u] : 0.0f;
        result.peakLeft = std::max(result.peakLeft, std::fabs(left));
        result.peakRight = std::max(result.peakRight, std::fabs(right));
        historyLeft[historyWrite] = left;
        historyRight[historyWrite] = right;
        historyWrite = (historyWrite + 1u) & (kTruePeakHistorySize - 1u);
        if (historyCount < kTruePeakTaps) ++historyCount;
        if (historyCount >= kTruePeakTaps) {
            result.peakLeft = std::max(
                result.peakLeft, truePeakFromHistory(historyLeft, historyWrite));
            result.peakRight = std::max(
                result.peakRight, truePeakFromHistory(historyRight, historyWrite));
        }
    }
    return result;
}

VeraCoreAudioMeterState VeraCoreTruePeakMeter::Flush() noexcept
{
    constexpr unsigned long tailFrames = 6;
    const std::array<float, tailFrames * 2u> silence{};
    return ProcessBlock(silence.data(), tailFrames);
}

double VeraCoreRealtimeLoudnessMeter::StereoBiquad::Process(
    double input, size_t channel) noexcept
{
    const double output = b0 * input + z1[channel];
    z1[channel] = b1 * input - a1 * output + z2[channel];
    z2[channel] = b2 * input - a2 * output;
    return std::isfinite(output) ? output : 0.0;
}

float VeraCoreRealtimeLoudnessMeter::EnergyToLufs(double energy) noexcept
{
    return energy > 0.0 && std::isfinite(energy)
        ? static_cast<float>(-0.691 + 10.0 * std::log10(energy))
        : -std::numeric_limits<float>::infinity();
}

void VeraCoreRealtimeLoudnessMeter::PushEnergy(
    std::vector<double>& ring, size_t& writeIndex, size_t& filled,
    double& sum, double energy) noexcept
{
    if (ring.empty()) return;
    if (filled < ring.size()) {
        ring[writeIndex] = energy;
        sum += energy;
        ++filled;
    } else {
        sum += energy - ring[writeIndex];
        ring[writeIndex] = energy;
    }
    if (++writeIndex == ring.size()) writeIndex = 0;
}

void VeraCoreRealtimeLoudnessMeter::Reset(double sampleRate)
{
    const double rate = std::clamp(sampleRate, 8000.0, 384000.0);
    momentaryRing.assign(static_cast<size_t>(std::llround(rate * 0.4)), 0.0);
    shortTermRing.assign(static_cast<size_t>(std::llround(rate * 3.0)), 0.0);
    momentaryWrite = shortTermWrite = 0;
    momentaryFilled = shortTermFilled = 0;
    momentaryEnergy = shortTermEnergy = 0.0;
    integratedStepFrames = static_cast<size_t>(std::llround(rate * 0.1));
    lraStepFrames = static_cast<size_t>(std::llround(rate * 1.0));
    ResetIntegrated();
    shelf = {};
    highPass = {};

    constexpr double pi = 3.14159265358979323846;
    {
        constexpr double frequency = 1681.974450955533;
        constexpr double gainDb = 3.999843853973347;
        constexpr double q = 0.7071752369554196;
        constexpr double slope = 0.4996667741545416;
        const double k = std::tan(pi * frequency / rate);
        const double vh = std::pow(10.0, gainDb / 20.0);
        const double vb = std::pow(vh, slope);
        const double denominator = 1.0 + k / q + k * k;
        shelf.b0 = (vh + vb * k / q + k * k) / denominator;
        shelf.b1 = 2.0 * (k * k - vh) / denominator;
        shelf.b2 = (vh - vb * k / q + k * k) / denominator;
        shelf.a1 = 2.0 * (k * k - 1.0) / denominator;
        shelf.a2 = (1.0 - k / q + k * k) / denominator;
    }
    {
        constexpr double frequency = 38.13547087602444;
        constexpr double q = 0.5003270373238773;
        const double k = std::tan(pi * frequency / rate);
        const double denominator = 1.0 + k / q + k * k;
        highPass.b0 = 1.0;
        highPass.b1 = -2.0;
        highPass.b2 = 1.0;
        highPass.a1 = 2.0 * (k * k - 1.0) / denominator;
        highPass.a2 = (1.0 - k / q + k * k) / denominator;
    }
}

void VeraCoreRealtimeLoudnessMeter::ResetIntegrated() noexcept
{
    integratedEnergySums.fill(0.0);
    integratedCounts.fill(0u);
    integratedFramesSinceBlock = 0;
    integratedWarmupFrames = 0;
    integratedStarted = false;
    currentIntegratedLufs = -std::numeric_limits<float>::infinity();
    lraEnergySums.fill(0.0);
    lraCounts.fill(0u);
    lraFramesSinceBlock = 0;
    lraWarmupFrames = 0;
    lraStarted = false;
    currentLoudnessRangeLu = -std::numeric_limits<float>::infinity();
}

void VeraCoreRealtimeLoudnessMeter::AppendIntegratedBlock() noexcept
{
    if (momentaryRing.empty() || momentaryFilled != momentaryRing.size()) return;
    const double energy = momentaryEnergy / static_cast<double>(momentaryRing.size());
    const float loudness = EnergyToLufs(energy);
    if (!std::isfinite(loudness) || loudness < -70.0f) {
        currentIntegratedLufs = CalculateIntegratedLufs();
        return;
    }
    const double clamped = std::clamp(static_cast<double>(loudness), -70.0, 10.0);
    const size_t bin = std::min(kIntegratedHistogramBins - 1u,
        static_cast<size_t>(std::floor((clamped + 70.0) * 10.0)));
    integratedEnergySums[bin] += energy;
    ++integratedCounts[bin];
    currentIntegratedLufs = CalculateIntegratedLufs();
}

float VeraCoreRealtimeLoudnessMeter::CalculateIntegratedLufs() const noexcept
{
    double absoluteEnergy = 0.0;
    uint64_t absoluteCount = 0;
    for (size_t bin = 0; bin < kIntegratedHistogramBins; ++bin) {
        absoluteEnergy += integratedEnergySums[bin];
        absoluteCount += integratedCounts[bin];
    }
    if (absoluteCount == 0) return -std::numeric_limits<float>::infinity();
    const float relativeGate = EnergyToLufs(
        absoluteEnergy / static_cast<double>(absoluteCount)) - 10.0f;
    const float gate = std::max(-70.0f, relativeGate);
    double gatedEnergy = 0.0;
    uint64_t gatedCount = 0;
    for (size_t bin = 0; bin < kIntegratedHistogramBins; ++bin) {
        const float binCenter = -69.95f + static_cast<float>(bin) * 0.1f;
        if (binCenter < gate) continue;
        gatedEnergy += integratedEnergySums[bin];
        gatedCount += integratedCounts[bin];
    }
    return gatedCount > 0
        ? EnergyToLufs(gatedEnergy / static_cast<double>(gatedCount))
        : -std::numeric_limits<float>::infinity();
}

void VeraCoreRealtimeLoudnessMeter::AppendLoudnessRangeBlock() noexcept
{
    if (shortTermRing.empty() || shortTermFilled != shortTermRing.size()) return;
    const double energy = shortTermEnergy / static_cast<double>(shortTermRing.size());
    const float loudness = EnergyToLufs(energy);
    if (std::isfinite(loudness) && loudness >= -70.0f) {
        const double clamped = std::clamp(static_cast<double>(loudness), -70.0, 10.0);
        const size_t bin = std::min(kIntegratedHistogramBins - 1u,
            static_cast<size_t>(std::floor((clamped + 70.0) * 10.0)));
        lraEnergySums[bin] += energy;
        ++lraCounts[bin];
    }
    currentLoudnessRangeLu = CalculateLoudnessRangeLu();
}

float VeraCoreRealtimeLoudnessMeter::LoudnessHistogramPercentile(
    float gate, uint64_t gatedCount, double fraction) const noexcept
{
    if (gatedCount == 0) return -std::numeric_limits<float>::infinity();
    const double rank = std::clamp(fraction, 0.0, 1.0) *
        static_cast<double>(gatedCount - 1u);
    uint64_t cumulative = 0;
    for (size_t bin = 0; bin < kIntegratedHistogramBins; ++bin) {
        const float binCenter = -69.95f + static_cast<float>(bin) * 0.1f;
        if (binCenter < gate) continue;
        cumulative += lraCounts[bin];
        if (static_cast<double>(cumulative) > rank) return binCenter;
    }
    return 10.05f;
}

float VeraCoreRealtimeLoudnessMeter::CalculateLoudnessRangeLu() const noexcept
{
    double absoluteEnergy = 0.0;
    uint64_t absoluteCount = 0;
    for (size_t bin = 0; bin < kIntegratedHistogramBins; ++bin) {
        absoluteEnergy += lraEnergySums[bin];
        absoluteCount += lraCounts[bin];
    }
    if (absoluteCount == 0) return -std::numeric_limits<float>::infinity();
    const float relativeGate = EnergyToLufs(
        absoluteEnergy / static_cast<double>(absoluteCount)) - 20.0f;
    const float gate = std::max(-70.0f, relativeGate);
    uint64_t gatedCount = 0;
    for (size_t bin = 0; bin < kIntegratedHistogramBins; ++bin) {
        const float binCenter = -69.95f + static_cast<float>(bin) * 0.1f;
        if (binCenter >= gate) gatedCount += lraCounts[bin];
    }
    if (gatedCount == 0) return -std::numeric_limits<float>::infinity();
    const float low = LoudnessHistogramPercentile(gate, gatedCount, 0.10);
    const float high = LoudnessHistogramPercentile(gate, gatedCount, 0.95);
    return std::max(0.0f, high - low);
}

VeraCoreAudioMeterState VeraCoreRealtimeLoudnessMeter::ProcessBlock(
    const float* interleavedStereo, unsigned long framesCount) noexcept
{
    VeraCoreAudioMeterState result;
    if (!interleavedStereo || framesCount == 0) return result;
    for (unsigned long frame = 0; frame < framesCount; ++frame) {
        const size_t index = static_cast<size_t>(frame) * 2u;
        const double left = highPass.Process(shelf.Process(
            std::isfinite(interleavedStereo[index]) ? interleavedStereo[index] : 0.0f, 0), 0);
        const double right = highPass.Process(shelf.Process(
            std::isfinite(interleavedStereo[index + 1u]) ? interleavedStereo[index + 1u] : 0.0f, 1), 1);
        const double energy = left * left + right * right;
        PushEnergy(momentaryRing, momentaryWrite, momentaryFilled, momentaryEnergy, energy);
        PushEnergy(shortTermRing, shortTermWrite, shortTermFilled, shortTermEnergy, energy);
        if (!integratedStarted) {
            if (++integratedWarmupFrames >= momentaryRing.size() &&
                momentaryFilled == momentaryRing.size()) {
                integratedStarted = true;
                integratedFramesSinceBlock = 0;
                AppendIntegratedBlock();
            }
        } else if (++integratedFramesSinceBlock >= integratedStepFrames) {
            integratedFramesSinceBlock = 0;
            AppendIntegratedBlock();
        }
        if (!lraStarted) {
            if (++lraWarmupFrames >= shortTermRing.size() &&
                shortTermFilled == shortTermRing.size()) {
                lraStarted = true;
                lraFramesSinceBlock = 0;
                AppendLoudnessRangeBlock();
            }
        } else if (++lraFramesSinceBlock >= lraStepFrames) {
            lraFramesSinceBlock = 0;
            AppendLoudnessRangeBlock();
        }
    }
    if (momentaryFilled == momentaryRing.size())
        result.momentaryLufs = EnergyToLufs(momentaryEnergy / momentaryRing.size());
    if (shortTermFilled == shortTermRing.size())
        result.shortTermLufs = EnergyToLufs(shortTermEnergy / shortTermRing.size());
    result.integratedLufs = currentIntegratedLufs;
    result.loudnessRangeLu = currentLoudnessRangeLu;
    return result;
}

VeraCoreAudio::VeraCoreAudio()
{
    static_assert((kPendingMidiQueueCapacity & (kPendingMidiQueueCapacity - 1u)) == 0u,
                  "Pending MIDI queue capacity must be a power of two.");
    callbackMidiEvents.reserve(kMaxCallbackMidiEvents);
	callbackMidiEventsByTrack.reserve(kMaxCallbackMidiEvents);
    loopChaseEvents.reserve(2048);
    midiTimelineEvents = std::make_shared<const std::vector<VeraCoreBeatMidiEvent>>();
    tempoMap = std::make_shared<const std::vector<VeraCoreTempoPoint>>();
    timeSignatureMap = std::make_shared<const std::vector<VeraCoreTimeSignaturePoint>>();
    midiSchedulerBindings[0].timelineEvents = midiTimelineEvents;
    midiSchedulerBindings[0].tempoMap = tempoMap;
    midiSchedulerBindings[0].timeSignatureMap = timeSignatureMap;
    audioInputRing.resize(384000u * 2u * 2u);
    audioTrackScratch.resize(65536u * 2u);
    callbackStereoMasterScratch.resize(kMaxRealtimeFrames * 2u);
    callbackSpatialBusScratch.resize(
        kMaxRealtimeFrames * static_cast<size_t>(kVeraCoreMaxSpatialChannels));
    masterLoudnessMeter.Reset(44100.0);
	for (VeraCoreTruePeakMeter& meter : returnTruePeakMeters)
		meter.Reset();
    pluginMixerTrackScratch.resize(
        callbackPluginMixerActive.size() * static_cast<size_t>(kPluginMixerChunkFrames) * 2u);
	for (std::vector<float>& bus : callbackReturnBusScratch)
		bus.resize(static_cast<size_t>(kPluginMixerChunkFrames) * 2u);
	for (CallbackStereoGain& gain : callbackTrackOutputGains)
		gain = {1.0f, 1.0f};
	callbackTrackAudibilityGains.fill(1.0f);
	callbackTrackDryGains.fill(1.0f);
    pitchBendStates.reserve(1024);
    pressureStates.reserve(1024);
    for (size_t i = 0; i < pendingMidiQueue.size(); ++i)
        pendingMidiQueue[i].sequence.store(static_cast<uint64_t>(i), std::memory_order_relaxed);
	ResetCallbackMixSmoothingState();
}
VeraCoreAudio::~VeraCoreAudio()
{}
static int Process(const void* inputBuffer,
    void* outputBuffer,
    unsigned long framesCount,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    const ScopedAudioDenormalMode denormalMode;
    (void)timeInfo;

    float* out = static_cast<float*>(outputBuffer);
    const float* in = static_cast<const float*>(inputBuffer);
    if (!out) {
        return paContinue;
    }

    VeraCoreAudio* audio = static_cast<VeraCoreAudio*>(userData);
    if (!audio) {
        const size_t sampleCount = static_cast<size_t>(framesCount) * 2u;
        if (in) std::copy_n(in, sampleCount, out);
        else std::fill_n(out, sampleCount, 0.0f);
        return paContinue;
    }
    audio->ReportCallbackStatus(statusFlags, framesCount);
    audio->ProcessAudioCallback(in, out, framesCount);
    return paContinue;
}

void VeraCoreAudio::ProcessAudioCallback(const float* input, float* output, unsigned long framesCount)
{
    if (!output || framesCount == 0) return;
	float* const hardwareOutput = output;
	const bool multichannel = streamOutputChannels > 2;
	if (multichannel && static_cast<size_t>(framesCount) > kMaxRealtimeFrames) {
		std::fill_n(hardwareOutput, static_cast<size_t>(framesCount) *
			static_cast<size_t>(streamOutputChannels), 0.0f);
		return;
	}
	callbackSpatialChannelCount = multichannel ? streamOutputChannels : 2;
	callbackSpeakerLayout = multichannel ? streamSpeakerLayout : VeraCoreSpeakerLayout::Stereo;
	if (multichannel) {
		std::fill_n(hardwareOutput, static_cast<size_t>(framesCount) *
			static_cast<size_t>(streamOutputChannels), 0.0f);
		output = callbackStereoMasterScratch.data();
		std::fill_n(callbackSpatialBusScratch.data(), static_cast<size_t>(framesCount) *
			static_cast<size_t>(callbackSpatialChannelCount), 0.0f);
	}
	const size_t sampleCount = static_cast<size_t>(framesCount) * 2u;
	std::fill_n(output, sampleCount, 0.0f);
	callbackPanLawDb = static_cast<float>(normalizedPanLawDb(
		panLawDb.load(std::memory_order_relaxed)));
	int rendererBindingIndex = -1;
	for (int attempt = 0; attempt < 3 && rendererBindingIndex < 0; ++attempt)
		rendererBindingIndex = AcquireExternalRendererBinding();
	int renderBatchToken = -1;
	if (rendererBindingIndex >= 0) {
		const ExternalRendererBinding& binding =
			externalRendererBindings[static_cast<size_t>(rendererBindingIndex)];
		try {
			if (binding.batchBegin) renderBatchToken = binding.batchBegin(binding.batchContext);
		} catch (...) {
			renderBatchToken = -1;
		}
	}
    if (input && streamInputChannels > 0 && audioInputRecording.load(std::memory_order_relaxed) && !audioInputRing.empty()) {
        uint64_t write = audioInputWriteIndex.load(std::memory_order_relaxed);
        const uint64_t read = audioInputReadIndex.load(std::memory_order_acquire);
        const uint64_t capacity = static_cast<uint64_t>(audioInputRing.size());
        const uint64_t used = write >= read ? std::min(capacity, write - read) : 0;
        const uint64_t available = capacity - used;
        const size_t framesToWrite = static_cast<size_t>(std::min<uint64_t>(available / 2u, framesCount));
        const size_t toWrite = framesToWrite * 2u;
        // One modulo per callback instead of two 64-bit divisions per frame.
        // The branch wraps predictably and keeps high-rate recording cheap.
        size_t ringCursor = static_cast<size_t>(write % capacity);
        const auto advanceRingCursor = [&]() noexcept {
            if (++ringCursor == static_cast<size_t>(capacity)) ringCursor = 0;
        };
        for (size_t frame = 0; frame < framesToWrite; ++frame) {
            const size_t source = frame * static_cast<size_t>(streamInputChannels);
            const float left = finiteSample(input[source]);
            const float right = streamInputChannels > 1 ? finiteSample(input[source + 1u]) : left;
            audioInputRing[ringCursor] = left;
            advanceRingCursor();
            audioInputRing[ringCursor] = right;
            advanceRingCursor();
        }
        audioInputWriteIndex.store(write + toWrite, std::memory_order_release);
        if (toWrite < sampleCount) audioInputOverflowed.store(true, std::memory_order_relaxed);
    }
    double blockStartBeat = 0.0;
    double blockEndBeat = 0.0;
    BuildCallbackMidiEvents(clampFrameCount(framesCount), &blockStartBeat, &blockEndBeat);
    ApplyPendingVoiceControl();
    std::sort(callbackMidiEvents.begin(), callbackMidiEvents.end(), [](const VeraCoreMidiEvent& a, const VeraCoreMidiEvent& b) {
        if (a.sampleOffset != b.sampleOffset) return a.sampleOffset < b.sampleOffset;
        if (a.isPortamento != b.isPortamento) return a.isPortamento;
        const int aRank = midiEventSortRank(a.status, a.data2);
        const int bRank = midiEventSortRank(b.status, b.data2);
        if (aRank != bRank) return aRank < bRank;
        if (a.isSlide != b.isSlide) return !a.isSlide;
        if (a.layerId != b.layerId) return a.layerId < b.layerId;
        if (a.status != b.status) return a.status < b.status;
    	return a.data1 < b.data1;
    });

	int timelineBindingIndex = -1;
	const bool callbackTransportActive = callbackSegmentCount > 0 &&
		callbackSegments[0].frameCount > 0 && callbackSegments[0].beatsPerSample > 0.0;
	if (callbackTransportActive) {
		for (int attempt = 0; attempt < 3 && timelineBindingIndex < 0; ++attempt)
			timelineBindingIndex = AcquireAudioTimelineBinding();
	}
	callbackMixerRoutingBindingIndex = -1;
	for (int attempt = 0; attempt < 3 && callbackMixerRoutingBindingIndex < 0; ++attempt)
		callbackMixerRoutingBindingIndex = AcquireMixerRoutingBinding();
    for (int i = 0; i < callbackSegmentCount; ++i) {
        const CallbackTransportSegment& segment = callbackSegments[static_cast<size_t>(i)];
        if (segment.frameCount <= 0) continue;
        float* segmentOutput = output + static_cast<size_t>(segment.frameOffset) * 2u;
		RenderMetronomeBlock(segmentOutput, static_cast<unsigned long>(segment.frameCount),
							 callbackMetronomeEnabled, segment.startBeat, segment.endBeat, segment.bpm,
							 segment.signatureStartBeat, segment.numerator, segment.denominator);
	}
	// Stable counting scatter turns globally time-sorted events into contiguous
	// per-track spans once per callback. Rendering then touches only one track's
	// events instead of rescanning every event for every track and chunk.
    std::array<bool, 256> callbackMidiTrackSeen{};
	int uniqueTrackCount = 0;
	for (int track = 0; track < callbackMidiTrackCount; ++track) {
		const int trackId = callbackMidiTrackIds[static_cast<size_t>(track)];
		if (trackId < 0 || trackId >= static_cast<int>(callbackMidiTrackSeen.size()) ||
			callbackMidiTrackSeen[static_cast<size_t>(trackId)]) continue;
		callbackMidiTrackSeen[static_cast<size_t>(trackId)] = true;
		callbackMidiTrackIds[static_cast<size_t>(uniqueTrackCount++)] = trackId;
	}
	callbackMidiTrackCount = uniqueTrackCount;
    for (const VeraCoreMidiEvent& event : callbackMidiEvents) {
        const int trackId = event.layerId >= 0 ? event.layerId / kLayerIdStride : -1;
        if (trackId < 0 || trackId >= static_cast<int>(callbackMidiTrackSeen.size())) continue;
        const size_t trackIndex = static_cast<size_t>(trackId);
        if (callbackMidiTrackSeen[trackIndex]) continue;
        callbackMidiTrackSeen[trackIndex] = true;
		if (callbackMidiTrackCount < static_cast<int>(callbackMidiTrackIds.size()))
			callbackMidiTrackIds[static_cast<size_t>(callbackMidiTrackCount++)] = trackId;
    }
	callbackMidiTrackEventOffsets.fill(0);
	for (const VeraCoreMidiEvent& event : callbackMidiEvents) {
		const int trackId = event.layerId >= 0 ? event.layerId / kLayerIdStride : -1;
		if (trackId >= 0 && trackId < 256)
			++callbackMidiTrackEventOffsets[static_cast<size_t>(trackId) + 1u];
	}
	for (size_t track = 1; track < callbackMidiTrackEventOffsets.size(); ++track)
		callbackMidiTrackEventOffsets[track] += callbackMidiTrackEventOffsets[track - 1u];
	callbackMidiEventsByTrack.resize(callbackMidiTrackEventOffsets.back());
	auto callbackMidiTrackWriteOffsets = callbackMidiTrackEventOffsets;
	for (const VeraCoreMidiEvent& event : callbackMidiEvents) {
		const int trackId = event.layerId >= 0 ? event.layerId / kLayerIdStride : -1;
		if (trackId < 0 || trackId >= 256) continue;
		callbackMidiEventsByTrack[
			callbackMidiTrackWriteOffsets[static_cast<size_t>(trackId)]++] = event;
	}
    const int scratchFrames = static_cast<int>(audioTrackScratch.size() / 2u);
	const int callbackFrames = clampFrameCount(framesCount);
	const int monitorTrackCount = input && streamInputChannels > 0
		? std::clamp(audioInputMonitorTrackCount.load(std::memory_order_acquire), 0,
					 static_cast<int>(audioInputMonitorTrackIds.size()))
		: 0;
	for (int monitor = 0; monitor < monitorTrackCount; ++monitor)
		callbackAudioInputMonitorTrackIds[static_cast<size_t>(monitor)] =
			audioInputMonitorTrackIds[static_cast<size_t>(monitor)].load(std::memory_order_relaxed);
	const bool hasTransportSnapshot = callbackSegmentCount > 0;
	const double callbackFallbackBpm = hasTransportSnapshot
		? callbackSegments[0].bpm : transportBpm.load(std::memory_order_relaxed);
	const int callbackFallbackNumerator = hasTransportSnapshot
		? callbackSegments[0].numerator : timeSignatureNumerator.load(std::memory_order_relaxed);
	const int callbackFallbackDenominator = hasTransportSnapshot
		? callbackSegments[0].denominator : timeSignatureDenominator.load(std::memory_order_relaxed);
	int transportSegment = 0;
    for (int chunkStart = 0; chunkStart < callbackFrames;) {
        while (transportSegment < callbackSegmentCount &&
               callbackSegments[static_cast<size_t>(transportSegment)].frameOffset +
                   callbackSegments[static_cast<size_t>(transportSegment)].frameCount <= chunkStart)
            ++transportSegment;
        int framesUntilBoundary = callbackFrames - chunkStart;
        double chunkBeat = blockStartBeat;
        double chunkSeconds = 0.0;
		double chunkBpm = callbackFallbackBpm;
		double chunkBeatsPerSample = 0.0;
        double chunkBarStartBeat = 0.0;
        int chunkBarNumber = 0;
		int chunkNumerator = callbackFallbackNumerator;
		int chunkDenominator = callbackFallbackDenominator;
        bool chunkCycleWrappedAtStart = false;
        if (transportSegment < callbackSegmentCount) {
            const CallbackTransportSegment& segment = callbackSegments[static_cast<size_t>(transportSegment)];
            if (chunkStart >= segment.frameOffset && chunkStart < segment.frameOffset + segment.frameCount) {
                framesUntilBoundary = segment.frameOffset + segment.frameCount - chunkStart;
                chunkBeat = segment.startBeat +
                    static_cast<double>(chunkStart - segment.frameOffset) * segment.beatsPerSample;
                chunkSeconds = segment.startSeconds +
                    static_cast<double>(chunkStart - segment.frameOffset) / streamSampleRate;
				chunkBpm = segment.bpm;
				chunkBeatsPerSample = segment.beatsPerSample;
                chunkBarStartBeat = segment.barStartBeat;
                chunkBarNumber = segment.barNumber;
                chunkNumerator = segment.numerator;
                chunkDenominator = segment.denominator;
                chunkCycleWrappedAtStart = segment.cycleWrappedAtStart &&
                    chunkStart == segment.frameOffset;
            }
        }
        const int chunkFrames = std::min({kPluginMixerChunkFrames, scratchFrames,
                                          callbackFrames - chunkStart, framesUntilBoundary});
        if (chunkFrames <= 0) break;
        callbackPluginMixerActive.fill(false);
		callbackPluginMixerActiveTrackCount = 0;
        callbackPluginMixerFrames = chunkFrames;
		callbackReturnMixerFrames = chunkFrames;
		for (std::vector<float>& bus : callbackReturnBusScratch)
			std::fill_n(bus.data(), static_cast<size_t>(chunkFrames) * 2u, 0.0f);
        const size_t trackStride = static_cast<size_t>(kPluginMixerChunkFrames) * 2u;
        auto activateTrackBuffer = [&](int trackId, const float* initialStereo = nullptr) -> float* {
            if (trackId < 0 || trackId >= static_cast<int>(callbackPluginMixerActive.size())) return nullptr;
            float* buffer = pluginMixerTrackScratch.data() + static_cast<size_t>(trackId) * trackStride;
            if (!callbackPluginMixerActive[static_cast<size_t>(trackId)]) {
                const size_t chunkSamples = static_cast<size_t>(chunkFrames) * 2u;
                if (initialStereo) std::copy_n(initialStereo, chunkSamples, buffer);
                else std::fill_n(buffer, chunkSamples, 0.0f);
                callbackPluginMixerActive[static_cast<size_t>(trackId)] = true;
				callbackPluginMixerActiveTrackIds[
					static_cast<size_t>(callbackPluginMixerActiveTrackCount++)] = trackId;
            }
            return buffer;
        };
		if (monitorTrackCount > 0) {
			for (int frame = 0; frame < chunkFrames; ++frame) {
				const size_t source = static_cast<size_t>(chunkStart + frame) *
					static_cast<size_t>(streamInputChannels);
				const size_t destination = static_cast<size_t>(frame) * 2u;
				const float left = finiteSample(input[source]);
				audioTrackScratch[destination] = left;
				audioTrackScratch[destination + 1u] = streamInputChannels > 1
					? finiteSample(input[source + 1u]) : left;
			}
		}
		for (int monitor = 0; monitor < monitorTrackCount; ++monitor) {
			const int trackId = callbackAudioInputMonitorTrackIds[static_cast<size_t>(monitor)];
			activateTrackBuffer(trackId, audioTrackScratch.data());
        }
		float* chunkOutput = output + static_cast<size_t>(chunkStart) * 2u;
		callbackSpatialChunkStart = chunkStart;
		if (timelineBindingIndex >= 0 && chunkBeatsPerSample > 0.0) {
			const double chunkEndBeat = chunkBeat +
				chunkBeatsPerSample * static_cast<double>(chunkFrames);
			RenderAudioTimelineBlock(
				chunkOutput, static_cast<unsigned long>(chunkFrames), chunkBeat, chunkEndBeat,
				chunkSeconds, chunkBpm, chunkBarStartBeat, chunkBarNumber,
				chunkNumerator, chunkDenominator, chunkCycleWrappedAtStart,
				rendererBindingIndex, renderBatchToken, timelineBindingIndex, true);
		}
        for (int track = 0; track < callbackMidiTrackCount; ++track) {
            const int trackId = callbackMidiTrackIds[static_cast<size_t>(track)];
            float* trackBuffer = activateTrackBuffer(trackId);
            if (!trackBuffer) continue;
			const size_t trackEventOffset = callbackMidiTrackEventOffsets[static_cast<size_t>(trackId)];
			const size_t trackEventEnd = callbackMidiTrackEventOffsets[static_cast<size_t>(trackId) + 1u];
			const auto trackBegin = callbackMidiEventsByTrack.begin() +
				static_cast<std::ptrdiff_t>(trackEventOffset);
			const auto trackEnd = callbackMidiEventsByTrack.begin() +
				static_cast<std::ptrdiff_t>(trackEventEnd);
			const auto chunkEventBegin = std::lower_bound(trackBegin, trackEnd, chunkStart,
				[](const VeraCoreMidiEvent& event, int frame) { return event.sampleOffset < frame; });
			const auto chunkEventEnd = std::lower_bound(chunkEventBegin, trackEnd,
				chunkStart + chunkFrames,
				[](const VeraCoreMidiEvent& event, int frame) { return event.sampleOffset < frame; });
			const size_t chunkEventCount = static_cast<size_t>(chunkEventEnd - chunkEventBegin);
			const VeraCoreMidiEvent* chunkEventData = chunkEventCount > 0
				? std::addressof(*chunkEventBegin) : nullptr;
            RenderExternalMidiBlock(trackBuffer, static_cast<unsigned long>(chunkFrames),
									chunkEventData, chunkEventCount, trackId, chunkBeat, chunkSeconds,
                                    chunkBpm, chunkBarStartBeat, chunkBarNumber,
                                    chunkNumerator, chunkDenominator,
                                    chunkCycleWrappedAtStart, chunkStart,
                                    &VeraCoreAudio::AccumulatePluginOutput, this,
									rendererBindingIndex, renderBatchToken);
            sanitizeFiniteStereoBlock(trackBuffer, static_cast<unsigned long>(chunkFrames));
			RenderMidiBlock(trackBuffer, static_cast<unsigned long>(chunkFrames),
								chunkEventData, chunkEventCount, trackId, chunkStart,
								callbackPanLawDb);
            sanitizeFiniteStereoBlock(trackBuffer, static_cast<unsigned long>(chunkFrames));
        }
		std::sort(callbackPluginMixerActiveTrackIds.begin(),
		          callbackPluginMixerActiveTrackIds.begin() + callbackPluginMixerActiveTrackCount);
		for (int activeTrack = 0; activeTrack < callbackPluginMixerActiveTrackCount; ++activeTrack) {
			const int trackId = callbackPluginMixerActiveTrackIds[static_cast<size_t>(activeTrack)];
            float* trackBuffer = pluginMixerTrackScratch.data() + static_cast<size_t>(trackId) * trackStride;
            RenderExternalAudioFxBlock(trackBuffer,
                                       static_cast<unsigned long>(chunkFrames), trackId,
                                       chunkBeat, chunkSeconds, chunkBpm,
                                       chunkBarStartBeat, chunkBarNumber,
                                       chunkNumerator, chunkDenominator,
									   chunkCycleWrappedAtStart, rendererBindingIndex,
									   renderBatchToken);
            sanitizeFiniteStereoBlock(trackBuffer, static_cast<unsigned long>(chunkFrames));
            MixTrackToOutput(chunkOutput, trackBuffer,
                             static_cast<unsigned long>(chunkFrames), trackId);
        }
		MixCallbackReturnsToOutput(chunkOutput, static_cast<unsigned long>(chunkFrames),
			chunkBeat, chunkSeconds, chunkBpm, chunkBarStartBeat, chunkBarNumber,
			chunkNumerator, chunkDenominator, chunkCycleWrappedAtStart,
			rendererBindingIndex, renderBatchToken);
		if (!multichannel) {
			RenderExternalAudioFxBlock(chunkOutput, static_cast<unsigned long>(chunkFrames),
				VeraCoreMasterAudioFxTrackId, chunkBeat, chunkSeconds, chunkBpm,
				chunkBarStartBeat, chunkBarNumber, chunkNumerator, chunkDenominator,
				chunkCycleWrappedAtStart, rendererBindingIndex, renderBatchToken);
		}
		sanitizeFiniteStereoBlock(chunkOutput, static_cast<unsigned long>(chunkFrames));
		if (multichannel) {
			AccumulateStereoFrontToSpatial(chunkOutput,
				static_cast<unsigned long>(chunkFrames), chunkStart);
			float* spatialChunk = callbackSpatialBusScratch.data() +
				static_cast<size_t>(chunkStart) * static_cast<size_t>(callbackSpatialChannelCount);
			RenderExternalAudioFxBlock(spatialChunk, static_cast<unsigned long>(chunkFrames),
				VeraCoreMasterAudioFxTrackId, chunkBeat, chunkSeconds, chunkBpm,
				chunkBarStartBeat, chunkBarNumber, chunkNumerator, chunkDenominator,
				chunkCycleWrappedAtStart, rendererBindingIndex, renderBatchToken,
				callbackSpatialChannelCount, callbackSpeakerLayout);
		}
		callbackReturnMixerFrames = 0;
		callbackPluginMixerFrames = 0;
		chunkStart += chunkFrames;
    }
	if (timelineBindingIndex >= 0)
		ReleaseAudioTimelineBinding(timelineBindingIndex);
	if (callbackMixerRoutingBindingIndex >= 0) {
		ReleaseMixerRoutingBinding(callbackMixerRoutingBindingIndex);
		callbackMixerRoutingBindingIndex = -1;
	}
	if (multichannel) {
		ApplyMasterToMultichannel(callbackSpatialBusScratch.data(), framesCount,
			callbackSpatialChannelCount);
		DownmixSpatialToStereo(callbackSpatialBusScratch.data(), output, framesCount,
			callbackSpatialChannelCount);
		UpdateMeterState(output, framesCount);
		clampHardwareOutputBlock(callbackSpatialBusScratch.data(), framesCount,
			callbackSpatialChannelCount);
		std::copy_n(callbackSpatialBusScratch.data(), static_cast<size_t>(framesCount) *
			static_cast<size_t>(callbackSpatialChannelCount), hardwareOutput);
	} else {
		ApplyMasterToOutput(output, framesCount);
		UpdateMeterState(output, framesCount);
		clampHardwareOutputBlock(output, framesCount);
	}
	if (rendererBindingIndex >= 0) {
		const ExternalRendererBinding& binding =
			externalRendererBindings[static_cast<size_t>(rendererBindingIndex)];
		try {
			if (binding.batchEnd && renderBatchToken >= 0)
				binding.batchEnd(binding.batchContext, renderBatchToken);
		} catch (...) {
		}
		ReleaseExternalRendererBinding(rendererBindingIndex);
	}
}

void VeraCoreAudio::ReportCallbackStatus(PaStreamCallbackFlags flags, unsigned long framesCount) noexcept
{
    callbackCount.fetch_add(1, std::memory_order_relaxed);
    callbackBufferFrames.store(framesCount, std::memory_order_relaxed);
    if ((flags & paInputUnderflow) != 0)
        inputUnderflowCount.fetch_add(1, std::memory_order_relaxed);
    if ((flags & paInputOverflow) != 0)
        inputOverflowCount.fetch_add(1, std::memory_order_relaxed);
    if ((flags & paOutputUnderflow) != 0)
        outputUnderflowCount.fetch_add(1, std::memory_order_relaxed);
    if ((flags & paOutputOverflow) != 0)
        outputOverflowCount.fetch_add(1, std::memory_order_relaxed);
    if ((flags & paPrimingOutput) != 0)
        primingOutputCount.fetch_add(1, std::memory_order_relaxed);
}

VeraCoreAudioStreamInfo VeraCoreAudio::GetStreamInfo() const noexcept
{
    VeraCoreAudioStreamInfo info;
    info.sampleRate = HasOpenStream() ? streamSampleRate : 0.0;
    info.requestedBufferFrames = requestedBufferFrames.load(std::memory_order_relaxed);
    info.callbackBufferFrames = callbackBufferFrames.load(std::memory_order_relaxed);
    info.inputLatencySeconds = streamInputLatencySeconds.load(std::memory_order_relaxed);
    info.outputLatencySeconds = streamOutputLatencySeconds.load(std::memory_order_relaxed);
    info.outputChannels = HasOpenStream() ? streamOutputChannels : 0;
    info.speakerLayout = HasOpenStream()
        ? streamSpeakerLayout : VeraCoreSpeakerLayout::Stereo;
    info.requestedSpeakerLayout = GetSpeakerLayout();
    info.layoutFallback = HasOpenStream() && info.speakerLayout != info.requestedSpeakerLayout;
    return info;
}

VeraCoreAudioHealthState VeraCoreAudio::GetAudioHealthState() const noexcept
{
    VeraCoreAudioHealthState state;
    state.callbackCount = callbackCount.load(std::memory_order_relaxed);
    if (stream) {
        const double load = Pa_GetStreamCpuLoad(stream);
        state.cpuLoad = std::isfinite(load) ? std::clamp(load, 0.0, 16.0) : 0.0;
    }
    state.inputUnderflows = inputUnderflowCount.load(std::memory_order_relaxed);
    state.inputOverflows = inputOverflowCount.load(std::memory_order_relaxed);
    state.outputUnderflows = outputUnderflowCount.load(std::memory_order_relaxed);
    state.outputOverflows = outputOverflowCount.load(std::memory_order_relaxed);
    state.primingOutputs = primingOutputCount.load(std::memory_order_relaxed);
    return state;
}

void VeraCoreAudio::ResetAudioHealthCounters() noexcept
{
    callbackCount.store(0, std::memory_order_relaxed);
    inputUnderflowCount.store(0, std::memory_order_relaxed);
    inputOverflowCount.store(0, std::memory_order_relaxed);
    outputUnderflowCount.store(0, std::memory_order_relaxed);
    outputOverflowCount.store(0, std::memory_order_relaxed);
    primingOutputCount.store(0, std::memory_order_relaxed);
}

void VeraCoreAudio::ResetCallbackMixSmoothingState() noexcept
{
	callbackMasterOutputGainInitialized = false;
	callbackTrackMixGainInitialized.fill(false);
	callbackReturnOutputGainInitialized.fill(false);
	callbackTrackStereoSeparation.fill(0.0f);
	for (auto& state : callbackTrackLfeLowPassState) state.fill(0.0f);
	callbackLfeSectionOneCoefficients = lfeLowPassCoefficients(
		streamSampleRate, 0.5411961001461970);
	callbackLfeSectionTwoCoefficients = lfeLowPassCoefficients(
		streamSampleRate, 1.3065629648763766);
	callbackReturnStereoSeparation.fill(0.0f);
	callbackMasterStereoSeparation = 0.0f;
}

unsigned long VeraCoreAudio::NormalizeBufferFrames(unsigned long frames) noexcept
{
    constexpr unsigned long minimum = 32;
    constexpr unsigned long maximum = 4096;
    frames = std::clamp(frames, minimum, maximum);
    unsigned long upper = minimum;
    while (upper < frames && upper < maximum) upper <<= 1u;
    if (upper == frames || upper == minimum) return upper;
    const unsigned long lower = upper >> 1u;
    return frames - lower <= upper - frames ? lower : upper;
}

PaError VeraCoreAudio::OpenStreamASIO(double srate, int devnum, unsigned long framesPerBuffer) {
	if (stream != nullptr) return paStreamIsNotStopped;
    PaError err;
    framesPerBuffer = NormalizeBufferFrames(framesPerBuffer);
    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(devnum);
    const PaHostApiInfo* hostInfo = deviceInfo ? Pa_GetHostApiInfo(deviceInfo->hostApi) : nullptr;
    if (!deviceInfo || !hostInfo || hostInfo->type != paASIO || deviceInfo->maxOutputChannels < 2)
        return paInvalidDevice;

    PaStreamParameters inParam;
    memset(&inParam, 0, sizeof(PaStreamParameters));
    inParam.channelCount = std::min(2, deviceInfo->maxInputChannels);
    inParam.device = devnum;
    inParam.sampleFormat = paFloat32;
    inParam.suggestedLatency = deviceInfo->defaultLowInputLatency;
    inParam.hostApiSpecificStreamInfo = nullptr;

    PaStreamParameters outParam;
    memset(&outParam, 0, sizeof(PaStreamParameters));
    const VeraCoreSpeakerLayout requestedLayout = GetSpeakerLayout();
    const VeraCoreOutputLayoutPlan outputPlan = VeraCoreBuildOutputLayoutPlan(
        requestedLayout, deviceInfo->maxOutputChannels);
    if (outputPlan.candidateCount == 0) return paInvalidChannelCount;
    outParam.channelCount = outputPlan.candidates[0].channelCount;
    outParam.device = devnum;
    outParam.sampleFormat = paFloat32;
    outParam.suggestedLatency = deviceInfo->defaultLowOutputLatency;
    outParam.hostApiSpecificStreamInfo = nullptr;

    const PaStreamParameters* input = inParam.channelCount > 0 ? &inParam : nullptr;
    auto tryOpen = [&](int outputChannels, bool useInput) {
        outParam.channelCount = outputChannels;
        streamInputChannels = useInput && input ? inParam.channelCount : 0;
        return Pa_OpenStream(&stream, useInput ? input : nullptr, &outParam,
            srate, framesPerBuffer, paNoFlag, Process, this);
    };
    int openedOutputChannels = 0;
    VeraCoreSpeakerLayout openedLayout = VeraCoreSpeakerLayout::Stereo;
    err = paInvalidChannelCount;
    for (int index = 0; index < outputPlan.candidateCount; ++index) {
        const VeraCoreOutputLayoutCandidate candidate =
            outputPlan.candidates[static_cast<std::size_t>(index)];
        err = tryOpen(candidate.channelCount, input != nullptr);
        if (err != paNoError && input) err = tryOpen(candidate.channelCount, false);
        if (err == paNoError) {
            openedOutputChannels = candidate.channelCount;
            openedLayout = candidate.layout;
            break;
        }
    }

    if (err == paNoError) {
        streamOutputChannels = openedOutputChannels;
        streamSpeakerLayout = openedLayout;
        streamSampleRate = srate;
        masterTruePeakMeter.Reset();
        masterLoudnessMeter.Reset(srate);
        requestedBufferFrames.store(framesPerBuffer, std::memory_order_relaxed);
        callbackBufferFrames.store(0, std::memory_order_relaxed);
        const PaStreamInfo* info = Pa_GetStreamInfo(stream);
        streamInputLatencySeconds.store(info ? info->inputLatency : 0.0, std::memory_order_relaxed);
        streamOutputLatencySeconds.store(info ? info->outputLatency : 0.0, std::memory_order_relaxed);
        ResetAudioHealthCounters();
		ResetCallbackMixSmoothingState();
        err = Pa_StartStream(stream);
        if (err != paNoError) {
            Pa_CloseStream(stream);
            stream = nullptr;
            streamInputChannels = 0;
            streamOutputChannels = 2;
            streamSpeakerLayout = VeraCoreSpeakerLayout::Stereo;
            requestedBufferFrames.store(0, std::memory_order_relaxed);
            streamInputLatencySeconds.store(0.0, std::memory_order_relaxed);
            streamOutputLatencySeconds.store(0.0, std::memory_order_relaxed);
        }
    }
    return err;
}
PaError VeraCoreAudio::OpenStreamWASAPI(double srate, int devnum, unsigned long framesPerBuffer) {
	if (stream != nullptr) return paStreamIsNotStopped;
    PaError err;
    framesPerBuffer = NormalizeBufferFrames(framesPerBuffer);
    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(devnum);
    const PaHostApiInfo* hostInfo = deviceInfo ? Pa_GetHostApiInfo(deviceInfo->hostApi) : nullptr;
    if (!deviceInfo || !hostInfo || hostInfo->type != paWASAPI || deviceInfo->maxOutputChannels < 2)
        return paInvalidDevice;

    PaStreamParameters outParam;
    memset(&outParam, 0, sizeof(PaStreamParameters));
    const VeraCoreSpeakerLayout requestedLayout = GetSpeakerLayout();
    const VeraCoreOutputLayoutPlan outputPlan = VeraCoreBuildOutputLayoutPlan(
        requestedLayout, deviceInfo->maxOutputChannels);
    if (outputPlan.candidateCount == 0) return paInvalidChannelCount;
    outParam.channelCount = outputPlan.candidates[0].channelCount;
    outParam.device = devnum;
    outParam.sampleFormat = paFloat32;
    outParam.suggestedLatency = deviceInfo->defaultLowOutputLatency;
    outParam.hostApiSpecificStreamInfo = nullptr;

    int inputDevice = Pa_GetDefaultInputDevice();
    const PaDeviceInfo* inputInfo = inputDevice >= 0 ? Pa_GetDeviceInfo(inputDevice) : nullptr;
    if (!inputInfo || inputInfo->hostApi != deviceInfo->hostApi || inputInfo->maxInputChannels <= 0) {
        inputDevice = -1;
        const int count = Pa_GetDeviceCount();
        for (int i = 0; i < count; ++i) {
            const PaDeviceInfo* candidate = Pa_GetDeviceInfo(i);
            if (candidate && candidate->hostApi == deviceInfo->hostApi && candidate->maxInputChannels > 0) {
                inputDevice = i;
                inputInfo = candidate;
                break;
            }
        }
    }
    PaStreamParameters inParam{};
    if (inputDevice >= 0 && inputInfo) {
        inParam.channelCount = std::min(2, inputInfo->maxInputChannels);
        inParam.device = inputDevice;
        inParam.sampleFormat = paFloat32;
        inParam.suggestedLatency = inputInfo->defaultLowInputLatency;
    }
    const PaStreamParameters* input = inParam.channelCount > 0 ? &inParam : nullptr;
    auto tryOpen = [&](int outputChannels, bool useInput) {
        outParam.channelCount = outputChannels;
        streamInputChannels = useInput && input ? inParam.channelCount : 0;
        return Pa_OpenStream(&stream, useInput ? input : nullptr, &outParam,
            srate, framesPerBuffer, paNoFlag, Process, this);
    };
    int openedOutputChannels = 0;
    VeraCoreSpeakerLayout openedLayout = VeraCoreSpeakerLayout::Stereo;
    err = paInvalidChannelCount;
    for (int index = 0; index < outputPlan.candidateCount; ++index) {
        const VeraCoreOutputLayoutCandidate candidate =
            outputPlan.candidates[static_cast<std::size_t>(index)];
        err = tryOpen(candidate.channelCount, input != nullptr);
        if (err != paNoError && input) err = tryOpen(candidate.channelCount, false);
        if (err == paNoError) {
            openedOutputChannels = candidate.channelCount;
            openedLayout = candidate.layout;
            break;
        }
    }

    if (err == paNoError) {
        streamOutputChannels = openedOutputChannels;
        streamSpeakerLayout = openedLayout;
        streamSampleRate = srate;
        masterTruePeakMeter.Reset();
        masterLoudnessMeter.Reset(srate);
        requestedBufferFrames.store(framesPerBuffer, std::memory_order_relaxed);
        callbackBufferFrames.store(0, std::memory_order_relaxed);
        const PaStreamInfo* info = Pa_GetStreamInfo(stream);
        streamInputLatencySeconds.store(info ? info->inputLatency : 0.0, std::memory_order_relaxed);
        streamOutputLatencySeconds.store(info ? info->outputLatency : 0.0, std::memory_order_relaxed);
        ResetAudioHealthCounters();
		ResetCallbackMixSmoothingState();
        err = Pa_StartStream(stream);
        if (err != paNoError) {
            Pa_CloseStream(stream);
            stream = nullptr;
            streamInputChannels = 0;
            streamOutputChannels = 2;
            streamSpeakerLayout = VeraCoreSpeakerLayout::Stereo;
            requestedBufferFrames.store(0, std::memory_order_relaxed);
            streamInputLatencySeconds.store(0.0, std::memory_order_relaxed);
            streamOutputLatencySeconds.store(0.0, std::memory_order_relaxed);
        }
    }
    return err;
}
PaError VeraCoreAudio::CloseStream()
{
    PaError err = paNoError;
    if (stream != nullptr) {
        Pa_AbortStream(stream);
        err = Pa_CloseStream(stream);
        stream = nullptr;
        streamInputChannels = 0;
        streamOutputChannels = 2;
        streamSpeakerLayout = VeraCoreSpeakerLayout::Stereo;
        requestedBufferFrames.store(0, std::memory_order_relaxed);
        callbackBufferFrames.store(0, std::memory_order_relaxed);
        streamInputLatencySeconds.store(0.0, std::memory_order_relaxed);
        streamOutputLatencySeconds.store(0.0, std::memory_order_relaxed);
    }
    masterTruePeakMeter.Reset();
    masterLoudnessMeter.Reset(streamSampleRate);
    meterPeakLeft.store(0.0f, std::memory_order_relaxed);
    meterPeakRight.store(0.0f, std::memory_order_relaxed);
    meterMomentaryLufs.store(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
    meterShortTermLufs.store(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
    meterIntegratedLufs.store(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
    meterLoudnessRangeLu.store(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
    loudnessMeterResetRequested.store(false, std::memory_order_relaxed);
    return err;
}

PaError VeraCoreAudio::GetStreamActivity() const noexcept
{
    PaStream* const currentStream = stream;
    return currentStream ? static_cast<PaError>(Pa_IsStreamActive(currentStream)) : paBadStreamPtr;
}

bool VeraCoreAudio::TryEnqueuePendingMidiEvent(const VeraCoreMidiEvent& event)
{
    uint64_t position = pendingMidiEnqueuePosition.load(std::memory_order_relaxed);
    for (;;) {
        PendingMidiQueueCell& cell = pendingMidiQueue[
            static_cast<size_t>(position & (kPendingMidiQueueCapacity - 1u))];
        const uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
        const intptr_t difference = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position);
        if (difference == 0) {
            if (pendingMidiEnqueuePosition.compare_exchange_weak(
                    position, position + 1u, std::memory_order_relaxed)) {
                cell.event = event;
                cell.sequence.store(position + 1u, std::memory_order_release);
                return true;
            }
        } else if (difference < 0) {
            return false;
        } else {
            position = pendingMidiEnqueuePosition.load(std::memory_order_relaxed);
        }
    }
}

bool VeraCoreAudio::TryDequeuePendingMidiEvent(VeraCoreMidiEvent& event)
{
    uint64_t position = pendingMidiDequeuePosition.load(std::memory_order_relaxed);
    for (;;) {
        PendingMidiQueueCell& cell = pendingMidiQueue[
            static_cast<size_t>(position & (kPendingMidiQueueCapacity - 1u))];
        const uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
        const intptr_t difference = static_cast<intptr_t>(sequence) -
            static_cast<intptr_t>(position + 1u);
        if (difference == 0) {
            if (pendingMidiDequeuePosition.compare_exchange_weak(
                    position, position + 1u, std::memory_order_relaxed)) {
                event = cell.event;
                cell.sequence.store(position + kPendingMidiQueueCapacity, std::memory_order_release);
                return true;
            }
        } else if (difference < 0) {
            return false;
        } else {
            position = pendingMidiDequeuePosition.load(std::memory_order_relaxed);
        }
    }
}

int VeraCoreAudio::AcquireMidiSchedulerBinding()
{
    const int bindingIndex = activeMidiSchedulerBinding.load(std::memory_order_seq_cst);
    midiSchedulerReaders[static_cast<size_t>(bindingIndex)].fetch_add(1, std::memory_order_seq_cst);
    if (bindingIndex != activeMidiSchedulerBinding.load(std::memory_order_seq_cst)) {
        midiSchedulerReaders[static_cast<size_t>(bindingIndex)].fetch_sub(1, std::memory_order_seq_cst);
        return -1;
    }
    return bindingIndex;
}

void VeraCoreAudio::ReleaseMidiSchedulerBinding(int bindingIndex)
{
    if (bindingIndex < 0 || bindingIndex >= static_cast<int>(midiSchedulerReaders.size())) return;
    midiSchedulerReaders[static_cast<size_t>(bindingIndex)].fetch_sub(1, std::memory_order_seq_cst);
}

void VeraCoreAudio::PublishMidiSchedulerBindingLocked()
{
    const int previousIndex = activeMidiSchedulerBinding.load(std::memory_order_seq_cst);
    const int nextIndex = 1 - previousIndex;
    while (midiSchedulerReaders[static_cast<size_t>(nextIndex)].load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    MidiSchedulerBinding& next = midiSchedulerBindings[static_cast<size_t>(nextIndex)];
    next.timelineEvents = midiTimelineEvents;
    next.tempoMap = tempoMap;
    next.timeSignatureMap = timeSignatureMap;
    next.loopChaseEvents = loopChaseEvents;
    next.trackIds = midiTrackIds;
    next.trackIdCount = midiTrackIdCount;
    next.loopEnabled = transportLoopEnabled.load(std::memory_order_acquire);
    next.loopStartBeat = transportLoopStartBeat.load(std::memory_order_relaxed);
    next.loopEndBeat = transportLoopEndBeat.load(std::memory_order_relaxed);
    activeMidiSchedulerBinding.store(nextIndex, std::memory_order_seq_cst);
    while (midiSchedulerReaders[static_cast<size_t>(previousIndex)].load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    MidiSchedulerBinding& retired = midiSchedulerBindings[static_cast<size_t>(previousIndex)];
    retired.timelineEvents.reset();
    retired.tempoMap.reset();
    retired.timeSignatureMap.reset();
    retired.loopChaseEvents.clear();
}

void VeraCoreAudio::ApplyPendingTransportCommand()
{
    const uint64_t generation = transportCommandGeneration.load(std::memory_order_acquire);
    if ((generation & 1u) != 0u || generation == appliedTransportCommandGeneration) return;
    const double bpm = requestedTransportBpm.load(std::memory_order_relaxed);
    const double playheadBeat = requestedTransportPlayheadBeat.load(std::memory_order_relaxed);
    const bool playing = requestedTransportPlaying.load(std::memory_order_relaxed);
    if (generation != transportCommandGeneration.load(std::memory_order_acquire)) return;
    transportBpm.store(bpm, std::memory_order_relaxed);
    transportPlayheadBeat.store(playheadBeat, std::memory_order_relaxed);
    transportPlaying.store(playing, std::memory_order_release);
    appliedTransportCommandGeneration = generation;
}

void VeraCoreAudio::SetPendingMidiEvents(const std::vector<VeraCoreMidiEvent>& events)
{
    VeraCoreMidiEvent discarded;
    while (TryDequeuePendingMidiEvent(discarded)) {
    }
    for (VeraCoreMidiEvent event : events) {
        const uint8_t statusHi = event.status & 0xF0;
        const bool isChannelEvent = (statusHi >= 0x80 && statusHi <= 0xE0);
        if (isChannelEvent &&
            (event.data1 > 127 || ((statusHi != 0xC0 && statusHi != 0xD0) && event.data2 > 127))) {
            continue;
        }
        event.gain = static_cast<float>(finiteClamp(static_cast<double>(event.gain), 0.0, 2.0, 1.0));
        event.pan = static_cast<float>(finiteClamp(static_cast<double>(event.pan), -1.0, 1.0, 0.0));
        if (event.fromTimeline && event.externalPlugin)
            event.timelineGeneration = externalTimelineGeneration.load(std::memory_order_acquire);
        if (!TryEnqueuePendingMidiEvent(event)) break;
    }
}

void VeraCoreAudio::QueueRealtimeMidiEvent(const VeraCoreMidiEvent& event)
{
    std::lock_guard<std::mutex> lock(realtimeRouteMutex);
    const uint8_t statusHi = event.status & 0xF0;
    const int channel = event.status & 0x0F;
    const bool isPitchBearingEvent = (statusHi == 0x90 || statusHi == 0x80 || statusHi == 0xA0);
    const bool isChannelEvent = (statusHi >= 0x80 && statusHi <= 0xE0);
    if (isChannelEvent &&
        (event.data1 > 127 || ((statusHi != 0xC0 && statusHi != 0xD0) && event.data2 > 127))) {
        return;
    }
    if (!realtimeLayerRoutingConfigured || !isChannelEvent) {
        VeraCoreMidiEvent safeEvent = event;
        safeEvent.gain = static_cast<float>(finiteClamp(static_cast<double>(safeEvent.gain), 0.0, 2.0, 1.0));
        safeEvent.pan = static_cast<float>(finiteClamp(static_cast<double>(safeEvent.pan), -1.0, 1.0, 0.0));
        TryEnqueuePendingMidiEvent(safeEvent);
        return;
    }
    if (realtimeLayerRoutes.empty()) {
        return;
    }

    for (const VeraCoreMidiLayerRoute& route : realtimeLayerRoutes) {
        if (!route.enabled) {
            continue;
        }

        if (route.midiChannel >= 0 && route.midiChannel != channel) {
            continue;
        }

        VeraCoreMidiEvent routedEvent = event;
        if (isPitchBearingEvent) {
            const int transposedPitch = static_cast<int>(event.data1) + route.transpose;
            if (transposedPitch < 0 || transposedPitch > 127) {
                continue;
            }
            routedEvent.data1 = static_cast<uint8_t>(transposedPitch);
        }
        routedEvent.layerId = route.layerId;
        routedEvent.gain = static_cast<float>(finiteClamp(static_cast<double>(route.gain), 0.0, 2.0, 1.0));
        routedEvent.pan = static_cast<float>(finiteClamp(static_cast<double>(route.pan), -1.0, 1.0, 0.0));
        routedEvent.externalPlugin = route.externalPlugin;
        if (!TryEnqueuePendingMidiEvent(routedEvent)) break;
    }
}

std::vector<VeraCoreMidiEvent> VeraCoreAudio::TakePendingMidiEventsForHost()
{
    std::vector<VeraCoreMidiEvent> events;
	events.reserve(kMaxCallbackMidiEvents);
    VeraCoreMidiEvent event;
    while (events.size() < kMaxCallbackMidiEvents && TryDequeuePendingMidiEvent(event))
        events.push_back(event);
	return events;
}

void VeraCoreAudio::SetExternalMidiRenderer(VeraCoreExternalMidiRenderer renderer, void* context)
{
    std::lock_guard<std::mutex> lock(externalRendererUpdateMutex);
    ExternalRendererBinding binding = externalRendererBindings[
        static_cast<size_t>(activeExternalRendererBinding.load(std::memory_order_seq_cst))];
    binding.midiRenderer = renderer;
    binding.midiContext = context;
    PublishExternalRendererBinding(binding);
}

void VeraCoreAudio::SetExternalAudioFxRenderer(VeraCoreExternalAudioFxRenderer renderer, void* context)
{
    std::lock_guard<std::mutex> lock(externalRendererUpdateMutex);
    ExternalRendererBinding binding = externalRendererBindings[
        static_cast<size_t>(activeExternalRendererBinding.load(std::memory_order_seq_cst))];
    binding.audioFxRenderer = renderer;
    binding.audioFxContext = context;
    PublishExternalRendererBinding(binding);
}

void VeraCoreAudio::SetExternalRenderBatchCallbacks(VeraCoreExternalRenderBatchBegin begin,
	VeraCoreExternalRenderBatchEnd end, void* context)
{
	std::lock_guard<std::mutex> lock(externalRendererUpdateMutex);
	ExternalRendererBinding binding = externalRendererBindings[
		static_cast<size_t>(activeExternalRendererBinding.load(std::memory_order_seq_cst))];
	binding.batchBegin = begin;
	binding.batchEnd = end;
	binding.batchContext = context;
	PublishExternalRendererBinding(binding);
}

int VeraCoreAudio::AcquireExternalRendererBinding()
{
    const int bindingIndex = activeExternalRendererBinding.load(std::memory_order_seq_cst);
    externalRendererReaders[static_cast<size_t>(bindingIndex)].fetch_add(1, std::memory_order_seq_cst);
    if (bindingIndex != activeExternalRendererBinding.load(std::memory_order_seq_cst)) {
        externalRendererReaders[static_cast<size_t>(bindingIndex)].fetch_sub(1, std::memory_order_seq_cst);
        return -1;
    }
    return bindingIndex;
}

void VeraCoreAudio::ReleaseExternalRendererBinding(int bindingIndex)
{
    if (bindingIndex < 0 || bindingIndex >= static_cast<int>(externalRendererReaders.size())) return;
    externalRendererReaders[static_cast<size_t>(bindingIndex)].fetch_sub(1, std::memory_order_seq_cst);
}

void VeraCoreAudio::PublishExternalRendererBinding(const ExternalRendererBinding& binding)
{
    const int previousIndex = activeExternalRendererBinding.load(std::memory_order_seq_cst);
    const int nextIndex = 1 - previousIndex;
    while (externalRendererReaders[static_cast<size_t>(nextIndex)].load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    externalRendererBindings[static_cast<size_t>(nextIndex)] = binding;
    activeExternalRendererBinding.store(nextIndex, std::memory_order_seq_cst);
    while (externalRendererReaders[static_cast<size_t>(previousIndex)].load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
}

void VeraCoreAudio::RenderExternalAudioFxBlock(float* out, unsigned long framesCount,
                                                int trackId, double projectBeat,
                                                double projectSeconds, double projectBpm,
                                                double barStartBeat, int barNumber,
                                                int timeSignatureNumerator,
                                                 int timeSignatureDenominator,
                                                 bool cycleWrappedAtStart,
												int rendererBindingIndex,
												int renderBatchToken,
												int channelCount,
												VeraCoreSpeakerLayout speakerLayout)
{
    if (!out || framesCount == 0 || channelCount < 1 || channelCount > 12) return;
	const bool ownsBinding = rendererBindingIndex < 0;
	const int bindingIndex = ownsBinding ? AcquireExternalRendererBinding() : rendererBindingIndex;
	if (bindingIndex < 0 || bindingIndex >= static_cast<int>(externalRendererBindings.size())) return;
    const ExternalRendererBinding& binding = externalRendererBindings[static_cast<size_t>(bindingIndex)];
    try {
        if (binding.audioFxRenderer)
            binding.audioFxRenderer(binding.audioFxContext, trackId, out, framesCount,
                                    channelCount, speakerLayout, streamSampleRate,
                                    projectBeat, projectSeconds,
                                    projectBpm, barStartBeat, barNumber,
                                    timeSignatureNumerator, timeSignatureDenominator,
									cycleWrappedAtStart, renderBatchToken);
    } catch (...) {
		if (ownsBinding) ReleaseExternalRendererBinding(bindingIndex);
        return;
    }
	if (ownsBinding) ReleaseExternalRendererBinding(bindingIndex);
}

void VeraCoreAudio::RenderExternalMidiBlock(float* out, unsigned long framesCount,
                                             const std::vector<VeraCoreMidiEvent>& midiEvents,
                                             int trackId, double projectBeat, double projectSeconds,
                                             double projectBpm, double barStartBeat, int barNumber,
                                             int timeSignatureNumerator, int timeSignatureDenominator,
                                             bool cycleWrappedAtStart,
                                             int sourceFrameOffset,
                                             VeraCorePluginOutputSink outputSink,
                                             void* outputSinkContext,
											 int rendererBindingIndex,
											 int renderBatchToken)

{
	RenderExternalMidiBlock(out, framesCount, midiEvents.data(), midiEvents.size(), trackId,
		projectBeat, projectSeconds, projectBpm, barStartBeat, barNumber,
		timeSignatureNumerator, timeSignatureDenominator, cycleWrappedAtStart,
		sourceFrameOffset, outputSink, outputSinkContext, rendererBindingIndex,
		renderBatchToken);
}

void VeraCoreAudio::RenderExternalMidiBlock(float* out, unsigned long framesCount,
											 const VeraCoreMidiEvent* midiEvents,
											 std::size_t midiEventCount,
											 int trackId, double projectBeat, double projectSeconds,
											 double projectBpm, double barStartBeat, int barNumber,
											 int timeSignatureNumerator, int timeSignatureDenominator,
											 bool cycleWrappedAtStart, int sourceFrameOffset,
											 VeraCorePluginOutputSink outputSink,
											 void* outputSinkContext,
											 int rendererBindingIndex,
											 int renderBatchToken)
{
    if (!out || framesCount == 0) {
        return;
    }
	const bool ownsBinding = rendererBindingIndex < 0;
	const int bindingIndex = ownsBinding ? AcquireExternalRendererBinding() : rendererBindingIndex;
	if (bindingIndex < 0 || bindingIndex >= static_cast<int>(externalRendererBindings.size())) return;
    const ExternalRendererBinding& binding = externalRendererBindings[static_cast<size_t>(bindingIndex)];
    try {
        if (binding.midiRenderer)
            binding.midiRenderer(binding.midiContext, trackId, out, framesCount,
								 midiEvents, midiEventCount, streamSampleRate, projectBeat, projectSeconds,
                                 projectBpm, barStartBeat, barNumber,
                                 timeSignatureNumerator, timeSignatureDenominator,
                                 cycleWrappedAtStart,
                                 sourceFrameOffset,
								 outputSink, outputSinkContext, renderBatchToken);
    } catch (...) {
		if (ownsBinding) ReleaseExternalRendererBinding(bindingIndex);
        return;
    }
	if (ownsBinding) ReleaseExternalRendererBinding(bindingIndex);
}

void VeraCoreAudio::RenderMidiBlock(float* out, unsigned long framesCount,
                                   const std::vector<VeraCoreMidiEvent>& midiEvents,
                                   int trackId, int sourceFrameOffset)

{
	RenderMidiBlock(out, framesCount, midiEvents.data(), midiEvents.size(), trackId,
		sourceFrameOffset);
}

void VeraCoreAudio::RenderMidiBlock(float* out, unsigned long framesCount,
								   const VeraCoreMidiEvent* midiEvents,
								   std::size_t midiEventCount,
								   int trackId, int sourceFrameOffset,
								   double centerAttenuationDb)
{
	constexpr double twoPi = 6.2831853071795864769;
	const double renderPanLawDb = std::isfinite(centerAttenuationDb)
		? normalizedPanLawDb(centerAttenuationDb)
		: normalizedPanLawDb(panLawDb.load(std::memory_order_relaxed));
    const double phaseStepScale = streamSampleRate > 0.0 ? (twoPi / streamSampleRate) : 0.0;
    size_t eventIndex = 0;
    const int clampedFrameCount = clampFrameCount(framesCount);
    const int safeSourceOffset = std::max(0, sourceFrameOffset);
	std::array<uint16_t, 512> trackVoiceIndices{};
	std::array<bool, 512> trackVoiceSeen{};
	size_t trackVoiceCount = 0;
	auto addTrackVoice = [&](SynthVoice& voice) noexcept {
		const size_t voiceIndex = static_cast<size_t>(std::addressof(voice) - activeVoices.data());
		if (voiceIndex >= activeVoices.size() || trackVoiceSeen[voiceIndex] || !voice.active ||
			voice.layerId < 0 || voice.layerId / kLayerIdStride != trackId) return;
		trackVoiceSeen[voiceIndex] = true;
		size_t insertOffset = trackVoiceCount;
		while (insertOffset > 0 && trackVoiceIndices[insertOffset - 1u] > voiceIndex) {
			trackVoiceIndices[insertOffset] = trackVoiceIndices[insertOffset - 1u];
			--insertOffset;
		}
		trackVoiceIndices[insertOffset] = static_cast<uint16_t>(voiceIndex);
		++trackVoiceCount;
	};
	for (SynthVoice& voice : activeVoices) addTrackVoice(voice);
	auto trackVoiceAt = [&](size_t voiceOffset) noexcept -> SynthVoice& {
		return activeVoices[static_cast<size_t>(trackVoiceIndices[voiceOffset])];
	};
    while (eventIndex < midiEventCount && midiEvents[eventIndex].sampleOffset < safeSourceOffset)
        ++eventIndex;
	int nextEventOffset = eventIndex < midiEventCount
        ? std::max(0, midiEvents[eventIndex].sampleOffset)
        : safeSourceOffset + clampedFrameCount;

    for (unsigned long frame = 0; frame < framesCount; ++frame) {
        const int localFrame = frame > static_cast<unsigned long>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(frame);
        const int frameIndex = localFrame > std::numeric_limits<int>::max() - safeSourceOffset
            ? std::numeric_limits<int>::max() : safeSourceOffset + localFrame;
		while (eventIndex < midiEventCount && nextEventOffset <= frameIndex) {
            const VeraCoreMidiEvent& event = midiEvents[eventIndex];
            if (event.externalPlugin || event.isKeyswitch ||
                event.layerId < 0 || event.layerId / kLayerIdStride != trackId) {
                ++eventIndex;
				nextEventOffset = eventIndex < midiEventCount
                    ? std::max(0, midiEvents[eventIndex].sampleOffset)
                    : safeSourceOffset + clampedFrameCount;
                continue;
            }
            const uint8_t statusHi = event.status & 0xF0;
            const int channel = event.status & 0x0F;
            const bool isNoteOn = (statusHi == 0x90 && event.data2 > 0 && !event.isSlide);
            const bool isNoteOff = !event.isSlide &&
                ((statusHi == 0x80) || (statusHi == 0x90 && event.data2 == 0));
            if (event.isSlide && event.data1 <= 127) {
                const int glideSamples = std::max(1, event.glideSamples);
				for (size_t voiceOffset = 0; voiceOffset < trackVoiceCount; ++voiceOffset) {
					SynthVoice& voice = trackVoiceAt(voiceOffset);
                    if (!voice.active || voice.channel != channel || voice.layerId != event.layerId ||
                        voice.fromTimeline != event.fromTimeline) continue;
                    voice.glideTargetPitch = event.data1;
                    voice.glideSamplesRemaining = glideSamples;
                    voice.glideStepPerSample = (voice.glideTargetPitch - voice.currentPitch) /
                        static_cast<double>(glideSamples);
                }
            }
            const bool isPitchBend = statusHi == 0xE0 && event.data1 <= 127 && event.data2 <= 127;
            const bool isChannelPressure = statusHi == 0xD0 && event.data1 <= 127;
            const bool isPolyPressure = statusHi == 0xA0 && event.data1 <= 127 && event.data2 <= 127;
            const bool isAllNotesOff = statusHi == 0xB0 && (event.data1 == 120 || event.data1 == 123);

			auto findPitchBendState = [&]() {
				return std::lower_bound(pitchBendStates.begin(), pitchBendStates.end(), event.layerId,
					[&](const PitchBendState& state, int layerId) {
						return controllerStateKeyLess(state.layerId, state.channel, state.fromTimeline,
							layerId, channel, event.fromTimeline);
					});
			};
			auto findPressureState = [&]() {
				return std::lower_bound(pressureStates.begin(), pressureStates.end(), event.layerId,
					[&](const PressureState& state, int layerId) {
						return controllerStateKeyLess(state.layerId, state.channel, state.fromTimeline,
							layerId, channel, event.fromTimeline);
					});
			};
			auto matchesControllerState = [&](int layerId, int stateChannel, bool fromTimeline) {
				return layerId == event.layerId && stateChannel == channel &&
					fromTimeline == event.fromTimeline;
			};
			auto currentPitchBend = [&]() {
				const auto stateIt = findPitchBendState();
				return stateIt != pitchBendStates.end() &&
					matchesControllerState(stateIt->layerId, stateIt->channel, stateIt->fromTimeline)
					? stateIt->semitones : 0.0;
			};
			auto currentPressureGain = [&]() {
				const auto stateIt = findPressureState();
				return stateIt != pressureStates.end() &&
					matchesControllerState(stateIt->layerId, stateIt->channel, stateIt->fromTimeline)
					? stateIt->gain : 1.0;
			};

            if (isPitchBend) {
                const double semitones = pitchBendToSemitones(event.data1, event.data2);
				auto stateIt = findPitchBendState();
				if (stateIt == pitchBendStates.end() ||
					!matchesControllerState(stateIt->layerId, stateIt->channel, stateIt->fromTimeline)) {
                    if (pitchBendStates.size() < 1024u)
						pitchBendStates.insert(stateIt,
							{event.layerId, channel, event.fromTimeline, semitones});
                } else {
                    stateIt->semitones = semitones;
                }

				for (size_t voiceOffset = 0; voiceOffset < trackVoiceCount; ++voiceOffset) {
					SynthVoice& voice = trackVoiceAt(voiceOffset);
                    if (voice.active &&
                        voice.channel == channel &&
                        voice.layerId == event.layerId &&
                        voice.fromTimeline == event.fromTimeline) {
                        voice.pitchBendSemitones = semitones;
                    }
                }
            }

            if (isAllNotesOff) {
				for (size_t voiceOffset = 0; voiceOffset < trackVoiceCount; ++voiceOffset) {
					SynthVoice& voice = trackVoiceAt(voiceOffset);
                    if (voice.active && voice.channel == channel && voice.layerId == event.layerId &&
                        voice.fromTimeline == event.fromTimeline) voice = {};
                }
            }

            if (isChannelPressure) {
                const double pressureGain = pressureToGain(event.data1);
				auto stateIt = findPressureState();
				if (stateIt == pressureStates.end() ||
					!matchesControllerState(stateIt->layerId, stateIt->channel, stateIt->fromTimeline)) {
                    if (pressureStates.size() < 1024u)
						pressureStates.insert(stateIt,
							{event.layerId, channel, event.fromTimeline, pressureGain});
                } else {
                    stateIt->gain = pressureGain;
                }

				for (size_t voiceOffset = 0; voiceOffset < trackVoiceCount; ++voiceOffset) {
					SynthVoice& voice = trackVoiceAt(voiceOffset);
                    if (voice.active &&
                        voice.channel == channel &&
                        voice.layerId == event.layerId &&
                        voice.fromTimeline == event.fromTimeline) {
                        voice.pressureGain = pressureGain;
                        voice.gain = combinedVoiceGain(voice.velocityGain, voice.layerGain, voice.pressureGain);
                    }
                }
            } else if (isPolyPressure) {
                const double pressureGain = pressureToGain(event.data2);
				for (size_t voiceOffset = 0; voiceOffset < trackVoiceCount; ++voiceOffset) {
					SynthVoice& voice = trackVoiceAt(voiceOffset);
                    if (voice.active &&
                        voice.pitch == event.data1 &&
                        voice.channel == channel &&
                        voice.layerId == event.layerId &&
                        voice.fromTimeline == event.fromTimeline) {
                        voice.pressureGain = pressureGain;
                        voice.gain = combinedVoiceGain(voice.velocityGain, voice.layerGain, voice.pressureGain);
                    }
                }
            }

            if ((isNoteOn || isNoteOff) && event.data1 <= 127) {
                if (isNoteOn) {
                    bool claimed = false;
                    double startPitch = static_cast<double>(event.data1);
                    if (event.isPortamento) {
						for (size_t voiceOffset = trackVoiceCount; voiceOffset > 0; --voiceOffset) {
							SynthVoice& voice = trackVoiceAt(voiceOffset - 1u);
							if (voice.active && voice.channel == channel &&
								voice.layerId == event.layerId &&
								voice.fromTimeline == event.fromTimeline) {
								startPitch = voice.currentPitch;
                                break;
                            }
                        }
                    }
                    auto configurePitch = [&](SynthVoice& voice) {
                        voice.currentPitch = startPitch;
                        voice.glideTargetPitch = static_cast<double>(event.data1);
                        voice.glideSamplesRemaining = event.isPortamento
                            ? std::max(1, event.glideSamples)
                            : 0;
                        voice.glideStepPerSample = voice.glideSamplesRemaining > 0
                            ? (voice.glideTargetPitch - voice.currentPitch) /
                                static_cast<double>(voice.glideSamplesRemaining)
                            : 0.0;
                        if (voice.glideSamplesRemaining == 0) voice.currentPitch = voice.glideTargetPitch;
                    };
                    const double layerGain = finiteClamp(static_cast<double>(event.gain), 0.0, 2.0, 1.0);
                    const double pressureGain = currentPressureGain();
                    const double velocityGain = static_cast<double>(event.data2) / 127.0 * 0.12;
                    const double voiceGain = combinedVoiceGain(velocityGain, layerGain, pressureGain);
                    const double voicePan = finiteClamp(static_cast<double>(event.pan), -1.0, 1.0, 0.0);
					const PanGains voicePanGains = panGains(
						1.0, voicePan, true, renderPanLawDb);
					for (size_t voiceOffset = 0; voiceOffset < trackVoiceCount; ++voiceOffset) {
						SynthVoice& voice = trackVoiceAt(voiceOffset);
                        if (voice.active &&
                            voice.pitch == event.data1 &&
                            voice.channel == channel &&
                            voice.layerId == event.layerId &&
                            voice.fromTimeline == event.fromTimeline) {
                            voice.velocityGain = velocityGain;
                            voice.layerGain = layerGain;
                            voice.pressureGain = pressureGain;
                            voice.gain = voiceGain;
                            voice.pan = voicePan;
                            voice.panLeft = voicePanGains.left;
                            voice.panRight = voicePanGains.right;
                            voice.pitchBendSemitones = currentPitchBend();
                            configurePitch(voice);
                            voice.phase = 0.0;
                            voice.noteOnCount = voice.noteOnCount >= std::numeric_limits<int>::max()
                                ? std::numeric_limits<int>::max()
                                : std::max(1, voice.noteOnCount) + 1;
                            claimed = true;
                            break;
                        }
                    }

                    if (!claimed) {
                        for (auto& voice : activeVoices) {
                            if (!voice.active) {
                                voice.active = true;
                                voice.pitch = event.data1;
                                voice.channel = channel;
                                voice.layerId = event.layerId;
                                voice.fromTimeline = event.fromTimeline;
                                voice.phase = 0.0;
                                voice.velocityGain = velocityGain;
                                voice.layerGain = layerGain;
                                voice.pressureGain = pressureGain;
                                voice.gain = voiceGain;
                                voice.pan = voicePan;
                                voice.panLeft = voicePanGains.left;
                                voice.panRight = voicePanGains.right;
                                voice.pitchBendSemitones = currentPitchBend();
                                configurePitch(voice);
                                voice.noteOnCount = 1;
								addTrackVoice(voice);
                                claimed = true;
                                break;
                            }
                        }
                    }

                    if (!claimed) {
                        auto& voice = activeVoices.front();
                        voice.active = true;
                        voice.pitch = event.data1;
                        voice.channel = channel;
                        voice.layerId = event.layerId;
                        voice.fromTimeline = event.fromTimeline;
                        voice.phase = 0.0;
                        voice.velocityGain = velocityGain;
                        voice.layerGain = layerGain;
                        voice.pressureGain = pressureGain;
                        voice.gain = voiceGain;
                        voice.pan = voicePan;
                        voice.panLeft = voicePanGains.left;
                        voice.panRight = voicePanGains.right;
                        voice.pitchBendSemitones = currentPitchBend();
                        configurePitch(voice);
                        voice.noteOnCount = 1;
						addTrackVoice(voice);
                    }
                } else if (isNoteOff) {
					for (size_t voiceOffset = 0; voiceOffset < trackVoiceCount; ++voiceOffset) {
						SynthVoice& voice = trackVoiceAt(voiceOffset);
                        if (voice.active &&
                            voice.pitch == event.data1 &&
                            voice.channel == channel &&
                            voice.layerId == event.layerId &&
                            voice.fromTimeline == event.fromTimeline) {
                            if (voice.noteOnCount > 1) {
                                --voice.noteOnCount;
                            } else {
                                voice = {};
                            }
                            break;
                        }
                    }
                }
            }

            ++eventIndex;
			if (eventIndex < midiEventCount) {
                nextEventOffset = std::max(0, midiEvents[eventIndex].sampleOffset);
            } else {
                nextEventOffset = safeSourceOffset + clampedFrameCount;
            }
        }

        double leftSample = 0.0;
        double rightSample = 0.0;
		for (size_t voiceOffset = 0; voiceOffset < trackVoiceCount; ++voiceOffset) {
			SynthVoice& voice = trackVoiceAt(voiceOffset);
            if (!voice.active || voice.layerId < 0 || voice.layerId / kLayerIdStride != trackId) {
                continue;
            }
            const double sample = synthLayerSample(voice.phase, voice.layerId) * voice.gain;
            leftSample += sample * voice.panLeft;
            rightSample += sample * voice.panRight;
            const double freq = noteToFrequency(voice.currentPitch + voice.pitchBendSemitones);
            voice.phase += freq * phaseStepScale;
            if (voice.glideSamplesRemaining > 0) {
                voice.currentPitch += voice.glideStepPerSample;
                if (--voice.glideSamplesRemaining == 0) {
                    voice.currentPitch = voice.glideTargetPitch;
                    voice.glideStepPerSample = 0.0;
                }
            }
            if (voice.phase > twoPi) {
                voice.phase -= twoPi;
            }
        }

        // Preserve floating-point track headroom until inserts and master summing.
        const float left = finiteFloatSample(leftSample);
        const float right = finiteFloatSample(rightSample);
        const size_t sampleIndex = static_cast<size_t>(frame) * 2;
        out[sampleIndex] = finiteSample(finiteSample(out[sampleIndex]) + left);
        out[sampleIndex + 1] = finiteSample(finiteSample(out[sampleIndex + 1]) + right);
    }
}

void VeraCoreAudio::QueueExternalTimelinePanicLocked()
{
    const uint64_t generation = externalTimelineGeneration.fetch_add(1, std::memory_order_acq_rel) + 1u;
    std::set<std::pair<int, int>> destinations;
    const std::vector<VeraCoreBeatMidiEvent>& timeline = *midiTimelineEvents;
    for (const VeraCoreBeatMidiEvent& event : timeline) {
        if (event.externalPlugin && event.layerId >= 0)
            destinations.insert({event.layerId, event.status & 0x0F});
    }
    for (const auto& destination : destinations) {
        for (const uint8_t controller : {uint8_t(120), uint8_t(123)}) {
            VeraCoreMidiEvent event{0,
                static_cast<uint8_t>(0xB0 | destination.second), controller, 0,
                destination.first, 1.0f, 0.0f, true, true};
            event.timelineGeneration = generation;
            if (!TryEnqueuePendingMidiEvent(event)) return;
        }
    }
}

void VeraCoreAudio::SetMidiTimelineEvents(const std::vector<VeraCoreBeatMidiEvent>& events)
{
    std::vector<VeraCoreBeatMidiEvent> sanitized;
    sanitized.reserve(events.size());
    for (VeraCoreBeatMidiEvent event : events) {
        if (!std::isfinite(event.beat) || event.beat < 0.0 ||
            !isValidChannelMidiEvent(event.status, event.data1, event.data2)) {
            continue;
        }
        event.gain = static_cast<float>(finiteClamp(static_cast<double>(event.gain), 0.0, 2.0, 1.0));
        event.pan = static_cast<float>(finiteClamp(static_cast<double>(event.pan), -1.0, 1.0, 0.0));
        event.glideDurationBeats = std::isfinite(event.glideDurationBeats) && event.glideDurationBeats > 0.0
            ? event.glideDurationBeats : 0.0;
        sanitized.push_back(event);
    }
    std::stable_sort(sanitized.begin(), sanitized.end(), [](const VeraCoreBeatMidiEvent& a, const VeraCoreBeatMidiEvent& b) {
        if (a.beat != b.beat) return a.beat < b.beat;
        if (a.isPortamento != b.isPortamento) return a.isPortamento;
        const int aRank = midiEventSortRank(a.status, a.data2);
        const int bRank = midiEventSortRank(b.status, b.data2);
        if (aRank != bRank) return aRank < bRank;
        if (a.isSlide != b.isSlide) return !a.isSlide;
        if (a.layerId != b.layerId) return a.layerId < b.layerId;
        if (a.status != b.status) return a.status < b.status;
        return a.data1 < b.data1;
    });
    auto timeline = std::make_shared<const std::vector<VeraCoreBeatMidiEvent>>(std::move(sanitized));

    std::lock_guard<std::mutex> lock(midiStateMutex);
    const bool playing = requestedTransportPlaying.load(std::memory_order_relaxed);
    if (playing) QueueExternalTimelinePanicLocked();
    midiTimelineEvents = std::move(timeline);
    RebuildLoopChaseEventsLocked();
    PublishMidiSchedulerBindingLocked();

    if (playing) {
        const bool transportApplied = transportPlaying.load(std::memory_order_acquire);
        const double playheadBeat = transportApplied
            ? transportPlayheadBeat.load(std::memory_order_relaxed)
            : requestedTransportPlayheadBeat.load(std::memory_order_relaxed);
        SeedExternalTimelineAtPlayheadLocked(playheadBeat);
        SeedTimelineVoicesAtPlayheadLocked(playheadBeat);
    }
}

void VeraCoreAudio::SetAudioTimelineClips(std::vector<VeraCoreAudioTimelineClip> clips)
{
    std::vector<VeraCoreAudioTimelineClip> sanitized;
    sanitized.reserve(clips.size());
    for (VeraCoreAudioTimelineClip& clip : clips) {
        const bool hasSharedSamples = static_cast<bool>(clip.sharedStereoSamples);
        const size_t sharedSampleCount = hasSharedSamples ? clip.sharedStereoSamples->size() : 0u;
        if (!std::isfinite(clip.startBeat) || clip.startBeat < 0.0 ||
            !std::isfinite(clip.lengthBeats) || clip.lengthBeats <= 0.0 ||
            clip.sampleRate <= 0 ||
            (hasSharedSamples && (sharedSampleCount < 2u || (sharedSampleCount % 2u) != 0u)) ||
            (!hasSharedSamples && clip.stereoSamples.size() < 2u)) {
            continue;
        }
        if (hasSharedSamples) {
            clip.stereoSamples.clear();
        } else if ((clip.stereoSamples.size() % 2u) != 0u) {
            clip.stereoSamples.pop_back();
        }
        clip.sourceTempoBpm = std::isfinite(clip.sourceTempoBpm) && clip.sourceTempoBpm > 0.0
            ? finiteClamp(clip.sourceTempoBpm, 20.0, 300.0, 0.0)
            : 0.0;
        clip.pitchShiftSemitones = std::isfinite(clip.pitchShiftSemitones)
            ? finiteClamp(clip.pitchShiftSemitones, -48.0, 48.0, 0.0)
            : 0.0;
        clip.sourceOffsetBeats = std::isfinite(clip.sourceOffsetBeats)
            ? std::max(0.0, clip.sourceOffsetBeats)
            : 0.0;
        clip.loopEnabled = clip.loopEnabled && std::isfinite(clip.loopStartBeats) &&
            clip.loopStartBeats >= 0.0 && std::isfinite(clip.loopLengthBeats) &&
            clip.loopLengthBeats >= 0.25;
        if (!clip.loopEnabled) {
            clip.loopStartBeats = clip.sourceOffsetBeats;
            clip.loopLengthBeats = clip.lengthBeats;
        }
        const double requiredSourceEnd = clip.loopEnabled
            ? clip.loopStartBeats + clip.loopLengthBeats
            : clip.sourceOffsetBeats + clip.lengthBeats;
        clip.warpLengthBeats = std::isfinite(clip.warpLengthBeats) && clip.warpLengthBeats > 0.0
            ? std::max(clip.warpLengthBeats, requiredSourceEnd)
            : requiredSourceEnd;
        if (clip.loopEnabled) {
            clip.loopStartBeats = finiteClamp(clip.loopStartBeats, 0.0,
                std::max(0.0, clip.warpLengthBeats - 0.25), 0.0);
            clip.loopLengthBeats = finiteClamp(clip.loopLengthBeats, 0.25,
                std::max(0.25, clip.warpLengthBeats - clip.loopStartBeats), 0.25);
            double phase = std::fmod(clip.sourceOffsetBeats - clip.loopStartBeats,
                                     clip.loopLengthBeats);
            if (phase < 0.0) phase += clip.loopLengthBeats;
            clip.sourceOffsetBeats = clip.loopStartBeats + phase;
        }
        clip.syncMode = std::clamp(clip.syncMode, 0, 3);
        clip.targetBars = std::isfinite(clip.targetBars)
            ? finiteClamp(clip.targetBars, 0.0, 512.0, 0.0)
            : 0.0;
        clip.beatsPerBar = std::isfinite(clip.beatsPerBar)
            ? finiteClamp(clip.beatsPerBar, 0.25, 64.0, 4.0)
            : 4.0;
        clip.gain = static_cast<float>(finiteClamp(static_cast<double>(clip.gain), 0.0, 4.0, 1.0));
        clip.pan = static_cast<float>(finiteClamp(static_cast<double>(clip.pan), -1.0, 1.0, 0.0));
        clip.normalizationGain = static_cast<float>(
            finiteClamp(static_cast<double>(clip.normalizationGain), 0.0, 64.0, 1.0));
        clip.fadeInBeats = std::isfinite(clip.fadeInBeats)
            ? finiteClamp(clip.fadeInBeats, 0.0, clip.lengthBeats, 0.0) : 0.0;
        clip.fadeOutBeats = std::isfinite(clip.fadeOutBeats)
            ? finiteClamp(clip.fadeOutBeats, 0.0, clip.lengthBeats, 0.0) : 0.0;
        clip.fadeInCurve = finiteClamp(clip.fadeInCurve, -1.0, 1.0, 0.0);
        clip.fadeOutCurve = finiteClamp(clip.fadeOutCurve, -1.0, 1.0, 0.0);
        const double fadeTotal = clip.fadeInBeats + clip.fadeOutBeats;
        if (fadeTotal > clip.lengthBeats && fadeTotal > 0.0) {
            const double fadeScale = clip.lengthBeats / fadeTotal;
            clip.fadeInBeats *= fadeScale;
            clip.fadeOutBeats *= fadeScale;
        }
        if (clip.sharedWarpPoints) {
            std::vector<VeraCoreAudioWarpPoint> warpPoints;
            warpPoints.reserve(std::min(clip.sharedWarpPoints->size(), kMaxTempoMapPoints + 2u));
            bool validWarp = clip.sharedWarpPoints->size() <= kMaxTempoMapPoints + 2u;
            for (const VeraCoreAudioWarpPoint& point : *clip.sharedWarpPoints) {
                if (!validWarp || !std::isfinite(point.localBeat) ||
                    !std::isfinite(point.sourceProgress) || point.localBeat < 0.0 ||
                    point.localBeat > clip.lengthBeats || point.sourceProgress < 0.0 ||
                    point.sourceProgress > 1.0 ||
                    (!warpPoints.empty() &&
                     (point.localBeat <= warpPoints.back().localBeat ||
                      point.sourceProgress < warpPoints.back().sourceProgress))) {
                    validWarp = false;
                    break;
                }
                warpPoints.push_back(point);
            }
            validWarp = validWarp && warpPoints.size() >= 2u &&
                warpPoints.front().localBeat == 0.0 &&
                warpPoints.back().localBeat == clip.lengthBeats &&
                (clip.warpPointsUseAbsoluteSourceProgress ||
                 (warpPoints.front().sourceProgress == 0.0 &&
                  warpPoints.back().sourceProgress == 1.0));
            clip.sharedWarpPoints = validWarp
                ? std::make_shared<const std::vector<VeraCoreAudioWarpPoint>>(std::move(warpPoints))
                : std::shared_ptr<const std::vector<VeraCoreAudioWarpPoint>>{};
            if (!clip.sharedWarpPoints) clip.warpPointsUseAbsoluteSourceProgress = false;
        } else {
            clip.warpPointsUseAbsoluteSourceProgress = false;
        }
        if (clip.sharedGainEnvelope) {
            std::vector<VeraCoreAudioGainPoint> envelope;
            envelope.reserve(std::min<size_t>(clip.sharedGainEnvelope->size(), 256u));
            bool validEnvelope = clip.sharedGainEnvelope->size() <= 256u;
            for (const VeraCoreAudioGainPoint& point : *clip.sharedGainEnvelope) {
                if (!validEnvelope || !std::isfinite(point.localBeat) ||
                    !std::isfinite(point.gain) || point.localBeat < 0.0 ||
                    point.localBeat > clip.lengthBeats || point.gain < 0.0f ||
                    point.gain > 4.0f || (!envelope.empty() &&
                    point.localBeat <= envelope.back().localBeat)) {
                    validEnvelope = false;
                    break;
                }
                envelope.push_back(point);
            }
            clip.sharedGainEnvelope = validEnvelope
                ? std::make_shared<const std::vector<VeraCoreAudioGainPoint>>(std::move(envelope))
                : std::shared_ptr<const std::vector<VeraCoreAudioGainPoint>>{};
        }
        sanitized.push_back(std::move(clip));
    }
    std::stable_sort(sanitized.begin(), sanitized.end(),
        [](const VeraCoreAudioTimelineClip& a, const VeraCoreAudioTimelineClip& b) {
            if (a.trackId != b.trackId) return a.trackId < b.trackId;
            return a.startBeat < b.startBeat;
        });
    PublishAudioTimelineBinding(std::move(sanitized));
}

int VeraCoreAudio::AcquireAudioTimelineBinding()
{
    const int bindingIndex = activeAudioTimelineBinding.load(std::memory_order_seq_cst);
    audioTimelineReaders[static_cast<size_t>(bindingIndex)].fetch_add(1, std::memory_order_seq_cst);
    if (bindingIndex != activeAudioTimelineBinding.load(std::memory_order_seq_cst)) {
        audioTimelineReaders[static_cast<size_t>(bindingIndex)].fetch_sub(1, std::memory_order_seq_cst);
        return -1;
    }
    return bindingIndex;
}

void VeraCoreAudio::ReleaseAudioTimelineBinding(int bindingIndex)
{
    if (bindingIndex < 0 || bindingIndex >= static_cast<int>(audioTimelineReaders.size())) return;
    audioTimelineReaders[static_cast<size_t>(bindingIndex)].fetch_sub(1, std::memory_order_seq_cst);
}

void VeraCoreAudio::PublishAudioTimelineBinding(std::vector<VeraCoreAudioTimelineClip> clips)
{
    std::vector<AudioTimelineBinding::TrackSpan> trackSpans;
	std::vector<double> clipPrefixMaxEndBeats(clips.size());
    trackSpans.reserve(std::min<size_t>(clips.size(), 256u));
    for (size_t clipIndex = 0; clipIndex < clips.size();) {
        const int trackId = clips[clipIndex].trackId;
        const size_t firstClip = clipIndex;
		double prefixMaxEndBeat = 0.0;
        while (clipIndex < clips.size() && clips[clipIndex].trackId == trackId) {
			prefixMaxEndBeat = std::max(
				prefixMaxEndBeat, clips[clipIndex].startBeat + clips[clipIndex].lengthBeats);
			clipPrefixMaxEndBeats[clipIndex] = prefixMaxEndBeat;
            ++clipIndex;
        }
        trackSpans.push_back({trackId, firstClip, clipIndex - firstClip});
    }

    std::lock_guard<std::mutex> lock(audioTimelineUpdateMutex);
    const int previousIndex = activeAudioTimelineBinding.load(std::memory_order_seq_cst);
    const int nextIndex = 1 - previousIndex;
    while (audioTimelineReaders[static_cast<size_t>(nextIndex)].load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    AudioTimelineBinding& next = audioTimelineBindings[static_cast<size_t>(nextIndex)];
    next.clips = std::move(clips);
	next.clipPrefixMaxEndBeats = std::move(clipPrefixMaxEndBeats);
    next.trackSpans = std::move(trackSpans);
    activeAudioTimelineBinding.store(nextIndex, std::memory_order_seq_cst);
    while (audioTimelineReaders[static_cast<size_t>(previousIndex)].load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    AudioTimelineBinding& retired = audioTimelineBindings[static_cast<size_t>(previousIndex)];
    retired.clips.clear();
	retired.clipPrefixMaxEndBeats.clear();
    retired.trackSpans.clear();
}

void VeraCoreAudio::SetRealtimeMidiLayerRoutes(const std::vector<VeraCoreMidiLayerRoute>& routes)
{
    std::lock_guard<std::mutex> lock(realtimeRouteMutex);
    const bool routeIdentityChanged =
        !realtimeLayerRoutingConfigured || !sameRouteIdentity(realtimeLayerRoutes, routes);
    realtimeLayerRoutes = routes;
    realtimeLayerRoutingConfigured = true;

    if (routeIdentityChanged) {
        clearRealtimeVoicesRequested.store(true, std::memory_order_release);
        return;
    }

    for (const VeraCoreMidiLayerRoute& route : routes) {
        if (route.enabled && !QueueRealtimeVoiceRouteCommand(route)) {
            clearRealtimeVoicesRequested.store(true, std::memory_order_release);
            break;
        }
    }
}

bool VeraCoreAudio::QueueRealtimeVoiceRouteCommand(const VeraCoreMidiLayerRoute& route)
{
    std::lock_guard<std::mutex> lock(voiceCommandWriteMutex);
    const uint64_t write = realtimeVoiceCommandWrite.load(std::memory_order_relaxed);
    const uint64_t read = realtimeVoiceCommandRead.load(std::memory_order_acquire);
    if (write - read >= realtimeVoiceCommands.size()) return false;
    RealtimeVoiceRouteCommand& command = realtimeVoiceCommands[
        static_cast<size_t>(write % realtimeVoiceCommands.size())];
    command.layerId = route.layerId;
    command.gain = static_cast<float>(finiteClamp(static_cast<double>(route.gain), 0.0, 2.0, 1.0));
    command.pan = static_cast<float>(finiteClamp(static_cast<double>(route.pan), -1.0, 1.0, 0.0));
    realtimeVoiceCommandWrite.store(write + 1u, std::memory_order_release);
    return true;
}

void VeraCoreAudio::SetMidiTrackIds(const std::vector<int>& trackIds)
{
    std::lock_guard<std::mutex> lock(midiStateMutex);
    midiTrackIdCount = 0;
    for (int trackId : trackIds) {
        if (trackId < 0 || midiTrackIdCount >= static_cast<int>(midiTrackIds.size())) continue;
        bool duplicate = false;
        for (int i = 0; i < midiTrackIdCount; ++i)
            duplicate = duplicate || midiTrackIds[static_cast<size_t>(i)] == trackId;
        if (!duplicate) midiTrackIds[static_cast<size_t>(midiTrackIdCount++)] = trackId;
    }
    PublishMidiSchedulerBindingLocked();
}

void VeraCoreAudio::SetMixerRouting(const std::vector<VeraCoreTrackSendState>& sends,
                                    const std::array<VeraCoreReturnBusState, 2>& returns)
{
	MixerRoutingBinding next{};
	for (const VeraCoreTrackSendState& source : sends) {
		if (source.trackId < 0 || source.trackId >= static_cast<int>(next.tracks.size())) continue;
		CallbackTrackMixState& destination = next.tracks[static_cast<size_t>(source.trackId)];
		destination.gain = static_cast<float>(finiteClamp(source.gain, 0.0, 2.0, 1.0));
		destination.pan = static_cast<float>(finiteClamp(source.pan, -1.0, 1.0, 0.0));
		destination.stereoSeparation = static_cast<float>(
			finiteClamp(source.stereoSeparation, -1.0, 1.0, 0.0));
		destination.spatial = VeraCoreNormalizeSpatialPanner(source.spatial);
		for (size_t bus = 0; bus < destination.sends.size(); ++bus)
			destination.sends[bus] = static_cast<float>(
				finiteClamp(source.levels[bus], 0.0, 1.0, 0.0));
		destination.audible = source.audible;
		destination.active = true;
	}
	for (size_t bus = 0; bus < next.returns.size(); ++bus) {
		next.returns[bus].gain = static_cast<float>(
			finiteClamp(returns[bus].gain, 0.0, 2.0, 1.0));
		next.returns[bus].pan = static_cast<float>(
			finiteClamp(returns[bus].pan, -1.0, 1.0, 0.0));
		next.returns[bus].stereoSeparation = static_cast<float>(
			finiteClamp(returns[bus].stereoSeparation, -1.0, 1.0, 0.0));
		next.returns[bus].muted = returns[bus].muted;
		next.returns[bus].soloed = returns[bus].soloed;
		next.returns[bus].preFader = returns[bus].preFader;
	}
	std::lock_guard<std::mutex> lock(mixerRoutingUpdateMutex);
	const int previousIndex = activeMixerRoutingBinding.load(std::memory_order_seq_cst);
	const int nextIndex = 1 - previousIndex;
	while (mixerRoutingReaders[static_cast<size_t>(nextIndex)].load(std::memory_order_seq_cst) != 0)
		std::this_thread::yield();
	mixerRoutingBindings[static_cast<size_t>(nextIndex)] = next;
	activeMixerRoutingBinding.store(nextIndex, std::memory_order_seq_cst);
	while (mixerRoutingReaders[static_cast<size_t>(previousIndex)].load(std::memory_order_seq_cst) != 0)
		std::this_thread::yield();
}

int VeraCoreAudio::AcquireMixerRoutingBinding() noexcept
{
	const int bindingIndex = activeMixerRoutingBinding.load(std::memory_order_seq_cst);
	mixerRoutingReaders[static_cast<size_t>(bindingIndex)].fetch_add(1, std::memory_order_seq_cst);
	if (bindingIndex != activeMixerRoutingBinding.load(std::memory_order_seq_cst)) {
		mixerRoutingReaders[static_cast<size_t>(bindingIndex)].fetch_sub(1, std::memory_order_seq_cst);
		return -1;
	}
	return bindingIndex;
}

void VeraCoreAudio::ReleaseMixerRoutingBinding(int bindingIndex) noexcept
{
	if (bindingIndex < 0 || bindingIndex >= static_cast<int>(mixerRoutingReaders.size())) return;
	mixerRoutingReaders[static_cast<size_t>(bindingIndex)].fetch_sub(1, std::memory_order_seq_cst);
}

void VeraCoreAudio::SetMasterState(float gain, float pan, float stereoSeparation, bool muted)
{
	const float nextGain = static_cast<float>(finiteClamp(gain, 0.0, 2.0, 1.0));
	const float nextPan = static_cast<float>(finiteClamp(pan, -1.0, 1.0, 0.0));
	const float nextSeparation = static_cast<float>(
		finiteClamp(stereoSeparation, -1.0, 1.0, 0.0));
	if (masterGain.load(std::memory_order_relaxed) == nextGain &&
		masterPan.load(std::memory_order_relaxed) == nextPan &&
		masterStereoSeparation.load(std::memory_order_relaxed) == nextSeparation &&
		masterMuted.load(std::memory_order_relaxed) == muted) return;
	masterMixGeneration.fetch_add(1, std::memory_order_acq_rel);
	masterGain.store(nextGain, std::memory_order_relaxed);
	masterPan.store(nextPan, std::memory_order_relaxed);
	masterStereoSeparation.store(nextSeparation, std::memory_order_relaxed);
	masterMuted.store(muted, std::memory_order_relaxed);
	masterMixGeneration.fetch_add(1, std::memory_order_release);
}

void VeraCoreAudio::SetPanLawDb(float centerAttenuationDb)
{
    panLawDb.store(static_cast<float>(normalizedPanLawDb(centerAttenuationDb)),
                   std::memory_order_release);
}

void VeraCoreAudio::SetSpeakerLayout(VeraCoreSpeakerLayout layout) noexcept
{
    switch (layout) {
    case VeraCoreSpeakerLayout::Stereo:
    case VeraCoreSpeakerLayout::Surround51:
    case VeraCoreSpeakerLayout::Surround71:
    case VeraCoreSpeakerLayout::Atmos712:
    case VeraCoreSpeakerLayout::Atmos714:
        speakerLayout.store(layout, std::memory_order_release);
        break;
    default:
        speakerLayout.store(VeraCoreSpeakerLayout::Stereo, std::memory_order_release);
        break;
    }
}

VeraCoreSpeakerLayout VeraCoreAudio::GetSpeakerLayout() const noexcept
{
    return speakerLayout.load(std::memory_order_acquire);
}

int VeraCoreAudio::GetSpeakerChannelCount() const noexcept
{
    return VeraCoreSpeakerChannelCount(GetSpeakerLayout());
}

void VeraCoreAudio::ApplyMasterToOutput(float* output, unsigned long framesCount)
{
    if (!output || framesCount == 0) return;
	float separationTarget = callbackMasterStereoSeparation;
	for (int attempt = 0; attempt < 3; ++attempt) {
		const uint64_t generation = masterMixGeneration.load(std::memory_order_acquire);
		if ((generation & 1u) != 0u) continue;
		const float nextGain = masterGain.load(std::memory_order_relaxed);
		const float nextPan = masterPan.load(std::memory_order_relaxed);
		const float nextSeparation = masterStereoSeparation.load(std::memory_order_relaxed);
		const bool nextMuted = masterMuted.load(std::memory_order_relaxed);
		if (generation != masterMixGeneration.load(std::memory_order_acquire)) continue;
		callbackMasterGain = nextGain;
		callbackMasterPan = nextPan;
		separationTarget = nextSeparation;
		callbackMasterMuted = nextMuted;
		break;
	}
	const double gain = callbackMasterMuted ? 0.0 : callbackMasterGain;
	const PanGains target = panGains(gain, callbackMasterPan, false, callbackPanLawDb);
	if (!callbackMasterOutputGainInitialized) {
		callbackMasterOutputGain = {
			static_cast<float>(target.left), static_cast<float>(target.right)};
		callbackMasterOutputGainInitialized = true;
	}
	const unsigned long rampFrames = controlRampFrameCount(streamSampleRate, framesCount);
	const double inverseFrames = 1.0 / static_cast<double>(rampFrames);
	const double masterLeftStep = (target.left - callbackMasterOutputGain.left) * inverseFrames;
	const double masterRightStep = (target.right - callbackMasterOutputGain.right) * inverseFrames;
	const double separationStep =
		(separationTarget - callbackMasterStereoSeparation) * inverseFrames;
	double masterLeft = callbackMasterOutputGain.left;
	double masterRight = callbackMasterOutputGain.right;
	double separation = callbackMasterStereoSeparation;
	for (unsigned long frame = 0; frame < framesCount; ++frame) {
		if (frame < rampFrames) {
			masterLeft += masterLeftStep;
			masterRight += masterRightStep;
			separation += separationStep;
		}
		const size_t sample = static_cast<size_t>(frame) * 2u;
		float separatedLeft = 0.0f;
		float separatedRight = 0.0f;
		applyStereoSeparation(finiteSample(output[sample]), finiteSample(output[sample + 1u]),
			separation, separatedLeft, separatedRight);
		output[sample] = finiteSample(static_cast<float>(separatedLeft * masterLeft));
		output[sample + 1u] = finiteSample(static_cast<float>(separatedRight * masterRight));
	}
	callbackMasterOutputGain = {
		static_cast<float>(target.left), static_cast<float>(target.right)};
	callbackMasterStereoSeparation = separationTarget;
}

void VeraCoreAudio::ApplyMasterToMultichannel(float* output, unsigned long framesCount,
                                               int channelCount)
{
    if (!output || framesCount == 0 || channelCount < 2) return;
    float separationTarget = callbackMasterStereoSeparation;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t generation = masterMixGeneration.load(std::memory_order_acquire);
        if ((generation & 1u) != 0u) continue;
        const float nextGain = masterGain.load(std::memory_order_relaxed);
        const float nextPan = masterPan.load(std::memory_order_relaxed);
        const float nextSeparation = masterStereoSeparation.load(std::memory_order_relaxed);
        const bool nextMuted = masterMuted.load(std::memory_order_relaxed);
        if (generation != masterMixGeneration.load(std::memory_order_acquire)) continue;
        callbackMasterGain = nextGain;
        callbackMasterPan = nextPan;
        separationTarget = nextSeparation;
        callbackMasterMuted = nextMuted;
        break;
    }
    const double gain = callbackMasterMuted ? 0.0 : callbackMasterGain;
    const PanGains target = panGains(gain, callbackMasterPan, false, callbackPanLawDb);
    if (!callbackMasterOutputGainInitialized) {
        callbackMasterOutputGain = {
            static_cast<float>(target.left), static_cast<float>(target.right)};
        callbackMasterStereoSeparation = separationTarget;
        callbackMasterOutputGainInitialized = true;
    }
    const unsigned long rampFrames = controlRampFrameCount(streamSampleRate, framesCount);
    const double inverseFrames = 1.0 / static_cast<double>(rampFrames);
    const double leftStep = (target.left - callbackMasterOutputGain.left) * inverseFrames;
    const double rightStep = (target.right - callbackMasterOutputGain.right) * inverseFrames;
    const double separationStep =
        (separationTarget - callbackMasterStereoSeparation) * inverseFrames;
    double leftGain = callbackMasterOutputGain.left;
    double rightGain = callbackMasterOutputGain.right;
    double separation = callbackMasterStereoSeparation;
    constexpr std::array<std::array<int, 2>, 5> pairs{{
        {{0, 1}}, {{4, 5}}, {{6, 7}}, {{8, 9}}, {{10, 11}}
    }};
    for (unsigned long frame = 0; frame < framesCount; ++frame) {
        if (frame < rampFrames) {
            leftGain += leftStep;
            rightGain += rightStep;
            separation += separationStep;
        }
        float* frameSamples = output + static_cast<size_t>(frame) *
            static_cast<size_t>(channelCount);
        for (const auto& pair : pairs) {
            if (pair[1] >= channelCount) continue;
            float left = 0.0f;
            float right = 0.0f;
            applyStereoSeparation(finiteSample(frameSamples[pair[0]]),
                finiteSample(frameSamples[pair[1]]), separation, left, right);
            frameSamples[pair[0]] = finiteSample(static_cast<float>(left * leftGain));
            frameSamples[pair[1]] = finiteSample(static_cast<float>(right * rightGain));
        }
        if (channelCount > 2) {
            const double centerGain = 0.5 * (leftGain + rightGain);
            frameSamples[2] = finiteSample(static_cast<float>(
                finiteSample(frameSamples[2]) * centerGain));
        }
        if (channelCount > 3)
            frameSamples[3] = finiteSample(static_cast<float>(
                finiteSample(frameSamples[3]) * gain));
    }
    callbackMasterOutputGain = {
        static_cast<float>(target.left), static_cast<float>(target.right)};
    callbackMasterStereoSeparation = separationTarget;
}

void VeraCoreAudio::AccumulateStereoFrontToSpatial(
    const float* stereo, unsigned long framesCount, int destinationFrameOffset) noexcept
{
    if (!stereo || callbackSpatialChannelCount < 2 || destinationFrameOffset < 0) return;
    for (unsigned long frame = 0; frame < framesCount; ++frame) {
        const size_t source = static_cast<size_t>(frame) * 2u;
        const size_t destination = (static_cast<size_t>(destinationFrameOffset) + frame) *
            static_cast<size_t>(callbackSpatialChannelCount);
        callbackSpatialBusScratch[destination] = finiteSample(
            callbackSpatialBusScratch[destination] + finiteSample(stereo[source]));
        callbackSpatialBusScratch[destination + 1u] = finiteSample(
            callbackSpatialBusScratch[destination + 1u] + finiteSample(stereo[source + 1u]));
    }
}

void VeraCoreAudio::DownmixSpatialToStereo(
    const float* spatial, float* stereo, unsigned long framesCount, int channelCount) noexcept
{
    if (!spatial || !stereo || channelCount < 2) return;
    for (unsigned long frame = 0; frame < framesCount; ++frame) {
        const float* source = spatial + static_cast<size_t>(frame) *
            static_cast<size_t>(channelCount);
        double left = finiteSample(source[0]);
        double right = finiteSample(source[1]);
        if (channelCount > 2) {
            const double center = finiteSample(source[2]) * 0.7071067811865476;
            left += center;
            right += center;
        }
        if (channelCount > 3) {
            const double lfe = finiteSample(source[3]) * 0.5;
            left += lfe;
            right += lfe;
        }
        for (int channel = 4; channel < channelCount; ++channel) {
            const double contribution = finiteSample(source[channel]) * 0.5;
            if ((channel & 1) == 0) left += contribution;
            else right += contribution;
        }
        const size_t destination = static_cast<size_t>(frame) * 2u;
        stereo[destination] = finiteFloatSample(left);
        stereo[destination + 1u] = finiteFloatSample(right);
    }
}

void VeraCoreAudio::AccumulatePluginOutput(
    void* context, int destinationTrackId, const float* left, const float* right,
    unsigned long framesCount, float gain, float pan, bool mono)
{
    auto* self = static_cast<VeraCoreAudio*>(context);
    if (!self || !left || destinationTrackId < 0 ||
        destinationTrackId >= static_cast<int>(self->callbackPluginMixerActive.size()) ||
        self->callbackPluginMixerFrames <= 0) return;
    const int frames = std::min<int>(
        self->callbackPluginMixerFrames,
        framesCount > static_cast<unsigned long>(INT32_MAX)
            ? INT32_MAX : static_cast<int>(framesCount));
    const size_t trackStride = static_cast<size_t>(kPluginMixerChunkFrames) * 2u;
    float* destination = self->pluginMixerTrackScratch.data() +
        static_cast<size_t>(destinationTrackId) * trackStride;
    if (!self->callbackPluginMixerActive[static_cast<size_t>(destinationTrackId)]) {
        std::fill_n(destination, static_cast<size_t>(self->callbackPluginMixerFrames) * 2u, 0.0f);
        self->callbackPluginMixerActive[static_cast<size_t>(destinationTrackId)] = true;
		self->callbackPluginMixerActiveTrackIds[
			static_cast<size_t>(self->callbackPluginMixerActiveTrackCount++)] = destinationTrackId;
    }
	const PanGains gains = panGains(
		gain, pan, mono, self->callbackPanLawDb);
    const float* safeRight = right ? right : left;
    for (int frame = 0; frame < frames; ++frame) {
        const size_t sample = static_cast<size_t>(frame) * 2u;
        destination[sample] = finiteFloatSample(
            static_cast<double>(destination[sample]) +
            static_cast<double>(finiteSample(left[frame])) * gains.left);
        destination[sample + 1u] = finiteFloatSample(
            static_cast<double>(destination[sample + 1u]) +
            static_cast<double>(finiteSample(safeRight[frame])) * gains.right);
    }
}

void VeraCoreAudio::AccumulateTimelineOutput(
	int destinationTrackId, const float* interleaved, unsigned long framesCount) noexcept
{
	if (!interleaved || destinationTrackId < 0 ||
		destinationTrackId >= static_cast<int>(callbackPluginMixerActive.size()) ||
		callbackPluginMixerFrames <= 0) return;
	const int frames = std::min<int>(
		callbackPluginMixerFrames,
		framesCount > static_cast<unsigned long>(INT32_MAX)
			? INT32_MAX : static_cast<int>(framesCount));
	const size_t sampleCount = static_cast<size_t>(frames) * 2u;
	const size_t trackStride = static_cast<size_t>(kPluginMixerChunkFrames) * 2u;
	float* destination = pluginMixerTrackScratch.data() +
		static_cast<size_t>(destinationTrackId) * trackStride;
	if (!callbackPluginMixerActive[static_cast<size_t>(destinationTrackId)]) {
		std::copy_n(interleaved, sampleCount, destination);
		callbackPluginMixerActive[static_cast<size_t>(destinationTrackId)] = true;
		callbackPluginMixerActiveTrackIds[
			static_cast<size_t>(callbackPluginMixerActiveTrackCount++)] = destinationTrackId;
		return;
	}
	for (size_t sample = 0; sample < sampleCount; ++sample)
		destination[sample] = finiteSample(destination[sample] + interleaved[sample]);
}

void VeraCoreAudio::MixTrackToOutput(float* output, const float* trackOutput,
                                     unsigned long framesCount, int trackId)
{
    if (!output || !trackOutput || framesCount == 0) return;
	std::array<float, 2> sends{};
	float trackGain = 1.0f;
	float trackPan = 0.0f;
	float trackSeparation = 0.0f;
	VeraCoreSpatialPannerState trackSpatial;
	bool trackAudible = true;
	std::array<VeraCoreReturnBusState, 2> returns{};
	const bool ownsMixerBinding = callbackMixerRoutingBindingIndex < 0;
	int mixerBindingIndex = callbackMixerRoutingBindingIndex;
	for (int attempt = 0; ownsMixerBinding && attempt < 3 && mixerBindingIndex < 0; ++attempt)
		mixerBindingIndex = AcquireMixerRoutingBinding();
	const MixerRoutingBinding* mixerBinding =
		mixerBindingIndex >= 0 && mixerBindingIndex < static_cast<int>(mixerRoutingBindings.size())
			? &mixerRoutingBindings[static_cast<size_t>(mixerBindingIndex)] : nullptr;
	if (mixerBinding) {
		returns = mixerBinding->returns;
		if (trackId >= 0 && trackId < static_cast<int>(mixerBinding->tracks.size())) {
			const CallbackTrackMixState& route = mixerBinding->tracks[static_cast<size_t>(trackId)];
			if (route.active) {
				trackGain = route.gain;
				trackPan = route.pan;
				trackSeparation = route.stereoSeparation;
				trackSpatial = route.spatial;
				sends = route.sends;
				trackAudible = route.audible;
			}
		}
	}
	const bool deferReturns = callbackReturnMixerFrames > 0 &&
		framesCount <= static_cast<unsigned long>(callbackReturnMixerFrames);
	std::array<PanGains, 2> returnTargets{};
	const PanGains trackTarget = panGains(trackGain, trackPan, false, callbackPanLawDb);
    const bool anyReturnSolo = std::any_of(returns.begin(), returns.end(),
        [](const VeraCoreReturnBusState& state) { return state.soloed; });
	const double audibilityTarget = trackAudible ? 1.0 : 0.0;
	const double dryTarget = anyReturnSolo ? 0.0 : 1.0;
    for (size_t bus = 0; bus < returns.size(); ++bus) {
        const bool audible = !returns[bus].muted && (!anyReturnSolo || returns[bus].soloed);
		const double gain = !deferReturns && audible ? returns[bus].gain * sends[bus] : 0.0;
		returnTargets[bus] = panGains(gain, returns[bus].pan, false, callbackPanLawDb);
    }
	CallbackStereoGain transientTrackGain{
		static_cast<float>(trackTarget.left), static_cast<float>(trackTarget.right)};
	std::array<CallbackStereoGain, 2> transientReturnGains{};
	std::array<float, 2> transientSendGains = sends;
	float transientAudibilityGain = static_cast<float>(audibilityTarget);
	float transientDryGain = static_cast<float>(dryTarget);
	float transientSeparation = trackSeparation;
	CallbackStereoGain* currentTrackGain = &transientTrackGain;
	std::array<CallbackStereoGain, 2>* currentReturnGains = &transientReturnGains;
	std::array<float, 2>* currentSendGains = &transientSendGains;
	float* currentAudibilityGain = &transientAudibilityGain;
	float* currentDryGain = &transientDryGain;
	float* currentSeparation = &transientSeparation;
	std::array<float, 4> transientLfeLowPassState{};
	std::array<float, 4>* currentLfeLowPassState = &transientLfeLowPassState;
	if (trackId >= 0 && trackId < static_cast<int>(callbackTrackOutputGains.size())) {
		const size_t trackIndex = static_cast<size_t>(trackId);
		currentTrackGain = &callbackTrackOutputGains[trackIndex];
		currentReturnGains = &callbackTrackReturnGains[trackIndex];
		currentSendGains = &callbackTrackSendGains[trackIndex];
		currentAudibilityGain = &callbackTrackAudibilityGains[trackIndex];
		currentDryGain = &callbackTrackDryGains[trackIndex];
		currentSeparation = &callbackTrackStereoSeparation[trackIndex];
		currentLfeLowPassState = &callbackTrackLfeLowPassState[trackIndex];
		if (!callbackTrackMixGainInitialized[trackIndex]) {
			*currentTrackGain = {
				static_cast<float>(trackTarget.left), static_cast<float>(trackTarget.right)};
			for (size_t bus = 0; bus < returnTargets.size(); ++bus) {
				(*currentSendGains)[bus] = sends[bus];
				(*currentReturnGains)[bus] = {
					static_cast<float>(returnTargets[bus].left),
					static_cast<float>(returnTargets[bus].right)};
			}
			*currentAudibilityGain = static_cast<float>(audibilityTarget);
			*currentDryGain = static_cast<float>(dryTarget);
			*currentSeparation = trackSeparation;
			currentLfeLowPassState->fill(0.0f);
			callbackTrackMixGainInitialized[trackIndex] = true;
		}
	}
	const unsigned long rampFrames = controlRampFrameCount(streamSampleRate, framesCount);
	const double inverseFrames = 1.0 / static_cast<double>(rampFrames);
	const double trackLeftStep = (trackTarget.left - currentTrackGain->left) * inverseFrames;
	const double trackRightStep = (trackTarget.right - currentTrackGain->right) * inverseFrames;
	const double audibilityStep = (audibilityTarget - *currentAudibilityGain) * inverseFrames;
	const double dryStep = (dryTarget - *currentDryGain) * inverseFrames;
	const double separationStep = (trackSeparation - *currentSeparation) * inverseFrames;
	std::array<double, 2> returnLeftSteps{};
	std::array<double, 2> returnRightSteps{};
	std::array<double, 2> sendSteps{};
	for (size_t bus = 0; bus < returns.size(); ++bus) {
		sendSteps[bus] = (sends[bus] - (*currentSendGains)[bus]) * inverseFrames;
		returnLeftSteps[bus] =
			(returnTargets[bus].left - (*currentReturnGains)[bus].left) * inverseFrames;
		returnRightSteps[bus] =
			(returnTargets[bus].right - (*currentReturnGains)[bus].right) * inverseFrames;
	}
	double trackLeftGain = currentTrackGain->left;
	double trackRightGain = currentTrackGain->right;
	double audibilityGain = *currentAudibilityGain;
	double dryGain = *currentDryGain;
	double separation = *currentSeparation;
	std::array<double, 2> returnLeftGains{};
	std::array<double, 2> returnRightGains{};
	std::array<double, 2> sendGains{};
	VeraCoreSpatialGains spatialObjectGains;
	VeraCoreSpatialGains spatialLeftGains;
	VeraCoreSpatialGains spatialRightGains;
	const bool spatialDry = callbackSpatialChannelCount > 2;
	if (spatialDry && trackSpatial.mode != VeraCoreSpatialRenderMode::StereoPan) {
		if (trackSpatial.mode == VeraCoreSpatialRenderMode::Object) {
			spatialObjectGains = VeraCoreCalculateSpatialGains(
				callbackSpeakerLayout, trackSpatial);
		} else {
			VeraCoreSpatialPannerState leftState = trackSpatial;
			VeraCoreSpatialPannerState rightState = trackSpatial;
			leftState.azimuthDegrees = std::clamp(
				trackSpatial.azimuthDegrees - 30.0f, -180.0f, 180.0f);
			rightState.azimuthDegrees = std::clamp(
				trackSpatial.azimuthDegrees + 30.0f, -180.0f, 180.0f);
			leftState.lfeSend *= 0.5f;
			rightState.lfeSend *= 0.5f;
			spatialLeftGains = VeraCoreCalculateSpatialGains(callbackSpeakerLayout, leftState);
			spatialRightGains = VeraCoreCalculateSpatialGains(callbackSpeakerLayout, rightState);
		}
	}
	const bool lfeFilterActive = spatialDry &&
		trackSpatial.mode != VeraCoreSpatialRenderMode::StereoPan &&
		trackSpatial.lfeSend > 0.0f;
	if (!lfeFilterActive) currentLfeLowPassState->fill(0.0f);
	// Dolby/ITU LFE feeds are band-limited. Cascaded Butterworth sections form
	// a fourth-order 120 Hz low-pass without allocation or locks in the callback.
	for (size_t bus = 0; bus < returns.size(); ++bus) {
		sendGains[bus] = (*currentSendGains)[bus];
		returnLeftGains[bus] = (*currentReturnGains)[bus].left;
		returnRightGains[bus] = (*currentReturnGains)[bus].right;
	}
    for (unsigned long frame = 0; frame < framesCount; ++frame) {
		if (frame < rampFrames) {
			trackLeftGain += trackLeftStep;
			trackRightGain += trackRightStep;
			audibilityGain += audibilityStep;
			dryGain += dryStep;
			separation += separationStep;
			for (size_t bus = 0; bus < returns.size(); ++bus) {
				sendGains[bus] += sendSteps[bus];
				returnLeftGains[bus] += returnLeftSteps[bus];
				returnRightGains[bus] += returnRightSteps[bus];
			}
		}
		const size_t sample = static_cast<size_t>(frame) * 2u;
        float sourceLeft = 0.0f;
        float sourceRight = 0.0f;
        applyStereoSeparation(finiteSample(trackOutput[sample]),
            finiteSample(trackOutput[sample + 1u]), separation, sourceLeft, sourceRight);
        const float left = finiteSample(
            static_cast<float>(sourceLeft * trackLeftGain * audibilityGain));
		const float right = finiteSample(
			static_cast<float>(sourceRight * trackRightGain * audibilityGain));
		double filteredLfeMono = 0.0;
		if (lfeFilterActive) {
			const float lfeInput = finiteSample(0.5f * (sourceLeft + sourceRight));
			float filtered = processLfeLowPassSection(lfeInput,
				callbackLfeSectionOneCoefficients,
				(*currentLfeLowPassState)[0], (*currentLfeLowPassState)[1]);
			filtered = processLfeLowPassSection(filtered,
				callbackLfeSectionTwoCoefficients,
				(*currentLfeLowPassState)[2], (*currentLfeLowPassState)[3]);
			filteredLfeMono = filtered;
		}
		double mixedLeft = left * dryGain;
        double mixedRight = right * dryGain;
		if (spatialDry) {
			const size_t spatialFrame =
				(static_cast<size_t>(callbackSpatialChunkStart) + frame) *
				static_cast<size_t>(callbackSpatialChannelCount);
			if (trackSpatial.mode == VeraCoreSpatialRenderMode::StereoPan) {
				callbackSpatialBusScratch[spatialFrame] = finiteFloatSample(
					static_cast<double>(callbackSpatialBusScratch[spatialFrame]) + mixedLeft);
				callbackSpatialBusScratch[spatialFrame + 1u] = finiteFloatSample(
					static_cast<double>(callbackSpatialBusScratch[spatialFrame + 1u]) + mixedRight);
			} else {
				const double spatialLevel = std::max(trackLeftGain, trackRightGain) *
					audibilityGain * dryGain;
				if (trackSpatial.mode == VeraCoreSpatialRenderMode::Object) {
					const double mono = 0.5 * (sourceLeft + sourceRight);
					for (int channel = 0; channel < callbackSpatialChannelCount; ++channel)
						callbackSpatialBusScratch[spatialFrame + static_cast<size_t>(channel)] =
							finiteFloatSample(static_cast<double>(callbackSpatialBusScratch[
								spatialFrame + static_cast<size_t>(channel)]) +
								(channel == 3 ? filteredLfeMono : mono) *
								spatialObjectGains.channels[static_cast<size_t>(channel)] * spatialLevel);
				} else {
					for (int channel = 0; channel < callbackSpatialChannelCount; ++channel) {
						const size_t index = spatialFrame + static_cast<size_t>(channel);
						const double routed = channel == 3
							? spatialLevel * filteredLfeMono *
								(spatialLeftGains.channels[3] + spatialRightGains.channels[3])
							: spatialLevel *
								(sourceLeft * spatialLeftGains.channels[static_cast<size_t>(channel)] +
								 sourceRight * spatialRightGains.channels[static_cast<size_t>(channel)]);
						callbackSpatialBusScratch[index] = finiteFloatSample(
							static_cast<double>(callbackSpatialBusScratch[index]) + routed);
					}
				}
			}
			mixedLeft = 0.0;
			mixedRight = 0.0;
		}
        for (size_t bus = 0; bus < returnTargets.size(); ++bus) {
            const float sendLeft = returns[bus].preFader
				? static_cast<float>(sourceLeft * audibilityGain) : left;
            const float sendRight = returns[bus].preFader
				? static_cast<float>(sourceRight * audibilityGain) : right;
			if (deferReturns) {
				std::vector<float>& returnBuffer = callbackReturnBusScratch[bus];
				returnBuffer[sample] = finiteSample(
					returnBuffer[sample] + static_cast<float>(sendLeft * sendGains[bus]));
				returnBuffer[sample + 1u] = finiteSample(
					returnBuffer[sample + 1u] + static_cast<float>(sendRight * sendGains[bus]));
			} else {
				mixedLeft += sendLeft * returnLeftGains[bus];
				mixedRight += sendRight * returnRightGains[bus];
			}
        }
        output[sample] = finiteSample(finiteSample(output[sample]) + static_cast<float>(mixedLeft));
        output[sample + 1u] = finiteSample(finiteSample(output[sample + 1u]) + static_cast<float>(mixedRight));
    }
	*currentTrackGain = {
		static_cast<float>(trackTarget.left), static_cast<float>(trackTarget.right)};
	*currentAudibilityGain = static_cast<float>(audibilityTarget);
	*currentDryGain = static_cast<float>(dryTarget);
	*currentSeparation = trackSeparation;
	for (size_t bus = 0; bus < returns.size(); ++bus) {
		(*currentSendGains)[bus] = sends[bus];
		(*currentReturnGains)[bus] = {
			static_cast<float>(returnTargets[bus].left),
			static_cast<float>(returnTargets[bus].right)};
	}
	if (ownsMixerBinding && mixerBindingIndex >= 0)
		ReleaseMixerRoutingBinding(mixerBindingIndex);
}

void VeraCoreAudio::MixCallbackReturnsToOutput(
	float* output, unsigned long framesCount, double projectBeat,
	double projectSeconds, double projectBpm, double barStartBeat, int barNumber,
	int timeSignatureNumerator, int timeSignatureDenominator,
	bool cycleWrappedAtStart, int rendererBindingIndex, int renderBatchToken)
{
	if (!output || framesCount == 0 || callbackReturnMixerFrames <= 0) return;
	const unsigned long frames = std::min<unsigned long>(
		framesCount, static_cast<unsigned long>(callbackReturnMixerFrames));
	const bool ownsMixerBinding = callbackMixerRoutingBindingIndex < 0;
	int mixerBindingIndex = callbackMixerRoutingBindingIndex;
	for (int attempt = 0; ownsMixerBinding && attempt < 3 && mixerBindingIndex < 0; ++attempt)
		mixerBindingIndex = AcquireMixerRoutingBinding();
	std::array<VeraCoreReturnBusState, 2> returns{};
	if (mixerBindingIndex >= 0 && mixerBindingIndex < static_cast<int>(mixerRoutingBindings.size()))
		returns = mixerRoutingBindings[static_cast<size_t>(mixerBindingIndex)].returns;
	if (callbackSpatialChannelCount <= 2)
		RenderExternalAudioFxBlock(output, frames, VeraCoreMasterPdcPathId,
			projectBeat, projectSeconds, projectBpm, barStartBeat, barNumber,
			timeSignatureNumerator, timeSignatureDenominator,
			cycleWrappedAtStart, rendererBindingIndex, renderBatchToken);
	sanitizeFiniteStereoBlock(output, frames);
	for (size_t bus = 0; bus < returns.size(); ++bus) {
		std::vector<float>& returnBlock = callbackReturnBusScratch[bus];
		RenderExternalAudioFxBlock(returnBlock.data(), frames,
			VeraCoreReturnAudioFxTrackId(static_cast<int>(bus)), projectBeat,
			projectSeconds, projectBpm, barStartBeat, barNumber,
			timeSignatureNumerator, timeSignatureDenominator,
			cycleWrappedAtStart, rendererBindingIndex, renderBatchToken);
		sanitizeFiniteStereoBlock(returnBlock.data(), frames);
	}
	const bool anyReturnSolo = std::any_of(returns.begin(), returns.end(),
		[](const VeraCoreReturnBusState& state) { return state.soloed; });
	const unsigned long rampFrames = controlRampFrameCount(streamSampleRate, frames);
	const double inverseFrames = 1.0 / static_cast<double>(rampFrames);
	std::array<PanGains, 2> targets{};
	std::array<double, 2> leftGains{};
	std::array<double, 2> rightGains{};
	std::array<double, 2> leftSteps{};
	std::array<double, 2> rightSteps{};
	std::array<double, 2> separations{};
	std::array<double, 2> separationSteps{};
	for (size_t bus = 0; bus < returns.size(); ++bus) {
		const bool audible = !returns[bus].muted && (!anyReturnSolo || returns[bus].soloed);
		targets[bus] = panGains(
			audible ? returns[bus].gain : 0.0, returns[bus].pan, false, callbackPanLawDb);
		if (!callbackReturnOutputGainInitialized[bus]) {
			callbackReturnOutputGains[bus] = {
				static_cast<float>(targets[bus].left), static_cast<float>(targets[bus].right)};
			callbackReturnStereoSeparation[bus] = returns[bus].stereoSeparation;
			callbackReturnOutputGainInitialized[bus] = true;
		}
		leftGains[bus] = callbackReturnOutputGains[bus].left;
		rightGains[bus] = callbackReturnOutputGains[bus].right;
		leftSteps[bus] = (targets[bus].left - leftGains[bus]) * inverseFrames;
		rightSteps[bus] = (targets[bus].right - rightGains[bus]) * inverseFrames;
		separations[bus] = callbackReturnStereoSeparation[bus];
		separationSteps[bus] =
			(returns[bus].stereoSeparation - separations[bus]) * inverseFrames;
	}
	for (unsigned long frame = 0; frame < frames; ++frame) {
		const size_t sample = static_cast<size_t>(frame) * 2u;
		double mixedLeft = finiteSample(output[sample]);
		double mixedRight = finiteSample(output[sample + 1u]);
		for (size_t bus = 0; bus < returns.size(); ++bus) {
			if (frame < rampFrames) {
				leftGains[bus] += leftSteps[bus];
				rightGains[bus] += rightSteps[bus];
				separations[bus] += separationSteps[bus];
			}
			std::vector<float>& returnBlock = callbackReturnBusScratch[bus];
			float separatedLeft = 0.0f;
			float separatedRight = 0.0f;
			applyStereoSeparation(returnBlock[sample], returnBlock[sample + 1u],
				separations[bus], separatedLeft, separatedRight);
			const float returnLeft = finiteSample(static_cast<float>(separatedLeft * leftGains[bus]));
			const float returnRight = finiteSample(static_cast<float>(separatedRight * rightGains[bus]));
			returnBlock[sample] = returnLeft;
			returnBlock[sample + 1u] = returnRight;
			mixedLeft += returnLeft;
			mixedRight += returnRight;
		}
		output[sample] = finiteFloatSample(mixedLeft);
		output[sample + 1u] = finiteFloatSample(mixedRight);
	}
	auto accumulatePeak = [](std::atomic<float>& destination, float peak) noexcept {
		float current = destination.load(std::memory_order_relaxed);
		while (peak > current && !destination.compare_exchange_weak(
			current, peak, std::memory_order_release, std::memory_order_relaxed)) {
		}
	};
	for (size_t bus = 0; bus < returns.size(); ++bus) {
		const VeraCoreAudioMeterState measured = returnTruePeakMeters[bus].ProcessBlock(
			callbackReturnBusScratch[bus].data(), frames);
		accumulatePeak(returnMeterPeakLeft[bus], measured.peakLeft);
		accumulatePeak(returnMeterPeakRight[bus], measured.peakRight);
	}
	for (size_t bus = 0; bus < returns.size(); ++bus)
		callbackReturnOutputGains[bus] = {
			static_cast<float>(targets[bus].left), static_cast<float>(targets[bus].right)};
	for (size_t bus = 0; bus < returns.size(); ++bus)
		callbackReturnStereoSeparation[bus] = returns[bus].stereoSeparation;
	if (ownsMixerBinding && mixerBindingIndex >= 0)
		ReleaseMixerRoutingBinding(mixerBindingIndex);
}

void VeraCoreAudio::SeedTimelineVoicesAtPlayheadLocked(double playheadBeat)
{
    struct ActiveTimelineNote {
        const VeraCoreBeatMidiEvent* latestEvent = nullptr;
        double currentPitch = 0.0;
        int noteOnCount = 0;
    };
    std::vector<ActiveTimelineNote> activeEvents;
    std::vector<PitchBendState> timelineBends;
    std::vector<PressureState> timelinePressures;
    const std::vector<VeraCoreBeatMidiEvent>& timeline = *midiTimelineEvents;
    for (const VeraCoreBeatMidiEvent& event : timeline) {
        if (event.beat >= playheadBeat) {
            break;
        }
        if (event.externalPlugin) {
            continue;
        }
        const uint8_t statusHi = event.status & 0xF0;
        const int channel = event.status & 0x0F;
        if (event.isSlide) {
            for (ActiveTimelineNote& active : activeEvents) {
                const VeraCoreBeatMidiEvent* note = active.latestEvent;
                if (note && note->layerId == event.layerId && (note->status & 0x0F) == channel)
                    active.currentPitch = event.data1;
            }
            continue;
        }
        const bool isNoteOn = statusHi == 0x90 && event.data2 > 0;
        const bool isNoteOff = statusHi == 0x80 || (statusHi == 0x90 && event.data2 == 0);
        const bool isPitchBend = statusHi == 0xE0 && event.data1 <= 127 && event.data2 <= 127;
        const bool isChannelPressure = statusHi == 0xD0 && event.data1 <= 127;
        if ((!isNoteOn && !isNoteOff && !isPitchBend && !isChannelPressure) || event.data1 > 127) {
            continue;
        }

        if (isPitchBend) {
            auto bendIt = std::find_if(timelineBends.begin(), timelineBends.end(),
                [&](const PitchBendState& state) {
                    return state.layerId == event.layerId && state.channel == channel;
                });
            const double semitones = pitchBendToSemitones(event.data1, event.data2);
            if (bendIt == timelineBends.end()) {
                timelineBends.push_back({event.layerId, channel, true, semitones});
            } else {
                bendIt->semitones = semitones;
            }
            continue;
        }
        if (isChannelPressure) {
            auto pressureIt = std::find_if(timelinePressures.begin(), timelinePressures.end(),
                [&](const PressureState& state) {
                    return state.layerId == event.layerId && state.channel == channel;
                });
            const double pressureGain = pressureToGain(event.data1);
            if (pressureIt == timelinePressures.end()) {
                timelinePressures.push_back({event.layerId, channel, true, pressureGain});
            } else {
                pressureIt->gain = pressureGain;
            }
            continue;
        }

        auto matching = std::find_if(activeEvents.begin(), activeEvents.end(),
            [&](const ActiveTimelineNote& activeNote) {
                const VeraCoreBeatMidiEvent* activeEvent = activeNote.latestEvent;
                return activeEvent && activeEvent->layerId == event.layerId &&
                       activeEvent->data1 == event.data1 &&
                       (activeEvent->status & 0x0F) == channel;
            });
        if (isNoteOn) {
            if (matching == activeEvents.end()) {
                activeEvents.push_back({&event, static_cast<double>(event.data1), 1});
            } else {
                matching->latestEvent = &event;
                matching->currentPitch = event.data1;
                if (matching->noteOnCount < std::numeric_limits<int>::max()) {
                    ++matching->noteOnCount;
                }
            }
        } else if (matching != activeEvents.end()) {
            if (matching->noteOnCount > 1) {
                --matching->noteOnCount;
            } else {
                activeEvents.erase(matching);
            }
        }
    }

    TimelineVoiceSeed seed;
    seed.bendCount = static_cast<int>(std::min(timelineBends.size(), seed.bends.size()));
    for (int i = 0; i < seed.bendCount; ++i) seed.bends[static_cast<size_t>(i)] = timelineBends[static_cast<size_t>(i)];
    seed.pressureCount = static_cast<int>(std::min(timelinePressures.size(), seed.pressures.size()));
    for (int i = 0; i < seed.pressureCount; ++i)
        seed.pressures[static_cast<size_t>(i)] = timelinePressures[static_cast<size_t>(i)];

    for (const ActiveTimelineNote& activeNote : activeEvents) {
        const VeraCoreBeatMidiEvent* event = activeNote.latestEvent;
        if (!event || seed.voiceCount >= static_cast<int>(seed.voices.size())) break;
        const int channel = event->status & 0x0F;
        const auto pressureIt = std::find_if(timelinePressures.begin(), timelinePressures.end(),
            [&](const PressureState& state) {
                return state.layerId == event->layerId && state.channel == channel;
            });
        const auto bendIt = std::find_if(timelineBends.begin(), timelineBends.end(),
            [&](const PitchBendState& state) {
                return state.layerId == event->layerId && state.channel == channel;
            });
        SynthVoice& voice = seed.voices[static_cast<size_t>(seed.voiceCount++)];
        voice.active = true;
        voice.pitch = event->data1;
        voice.channel = channel;
        voice.layerId = event->layerId;
        voice.fromTimeline = true;
        voice.velocityGain = static_cast<double>(event->data2) / 127.0 * 0.12;
        voice.layerGain = finiteClamp(static_cast<double>(event->gain), 0.0, 2.0, 1.0);
        voice.pressureGain = pressureIt == timelinePressures.end() ? 1.0 : pressureIt->gain;
        voice.gain = combinedVoiceGain(voice.velocityGain, voice.layerGain, voice.pressureGain);
        voice.pan = finiteClamp(static_cast<double>(event->pan), -1.0, 1.0, 0.0);
		const PanGains gains = panGains(
			1.0, voice.pan, true, panLawDb.load(std::memory_order_relaxed));
        voice.panLeft = gains.left;
        voice.panRight = gains.right;
        voice.pitchBendSemitones = bendIt == timelineBends.end() ? 0.0 : bendIt->semitones;
        voice.currentPitch = activeNote.currentPitch;
        voice.glideTargetPitch = activeNote.currentPitch;
        voice.noteOnCount = std::max(1, activeNote.noteOnCount);
    }
    PublishTimelineVoiceSeed(seed);
}

int VeraCoreAudio::AcquireTimelineVoiceSeed()
{
    const int seedIndex = activeTimelineVoiceSeed.load(std::memory_order_seq_cst);
    timelineVoiceSeedReaders[static_cast<size_t>(seedIndex)].fetch_add(1, std::memory_order_seq_cst);
    if (seedIndex != activeTimelineVoiceSeed.load(std::memory_order_seq_cst)) {
        timelineVoiceSeedReaders[static_cast<size_t>(seedIndex)].fetch_sub(1, std::memory_order_seq_cst);
        return -1;
    }
    return seedIndex;
}

void VeraCoreAudio::ReleaseTimelineVoiceSeed(int seedIndex)
{
    if (seedIndex < 0 || seedIndex >= static_cast<int>(timelineVoiceSeedReaders.size())) return;
    timelineVoiceSeedReaders[static_cast<size_t>(seedIndex)].fetch_sub(1, std::memory_order_seq_cst);
}

void VeraCoreAudio::PublishTimelineVoiceSeed(const TimelineVoiceSeed& seed)
{
    std::lock_guard<std::mutex> lock(timelineVoiceSeedUpdateMutex);
    const int previousIndex = activeTimelineVoiceSeed.load(std::memory_order_seq_cst);
    const int nextIndex = 1 - previousIndex;
    while (timelineVoiceSeedReaders[static_cast<size_t>(nextIndex)].load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    timelineVoiceSeeds[static_cast<size_t>(nextIndex)] = seed;
    activeTimelineVoiceSeed.store(nextIndex, std::memory_order_seq_cst);
    timelineVoicesRequested.store(true, std::memory_order_release);
    timelineVoiceControlGeneration.fetch_add(1, std::memory_order_release);
}

void VeraCoreAudio::RequestClearAllVoices()
{
    clearAllVoicesRequested.store(true, std::memory_order_release);
    timelineVoicesRequested.store(false, std::memory_order_release);
    timelineVoiceControlGeneration.fetch_add(1, std::memory_order_release);
}

void VeraCoreAudio::ApplyPendingVoiceControl()
{
    if (clearAllVoicesRequested.exchange(false, std::memory_order_acq_rel)) {
        for (SynthVoice& voice : activeVoices) voice = {};
        pitchBendStates.clear();
        pressureStates.clear();
    }
    const uint64_t generation = timelineVoiceControlGeneration.load(std::memory_order_acquire);
    if (generation != appliedTimelineVoiceControlGeneration) {
        if (!timelineVoicesRequested.load(std::memory_order_acquire)) {
            for (SynthVoice& voice : activeVoices) voice = {};
            pitchBendStates.clear();
            pressureStates.clear();
            appliedTimelineVoiceControlGeneration = generation;
        } else {
            const int seedIndex = AcquireTimelineVoiceSeed();
            if (seedIndex >= 0) {
                const TimelineVoiceSeed& seed = timelineVoiceSeeds[static_cast<size_t>(seedIndex)];
                for (SynthVoice& voice : activeVoices)
                    if (voice.fromTimeline) voice = {};
                pitchBendStates.erase(std::remove_if(pitchBendStates.begin(), pitchBendStates.end(),
                    [](const PitchBendState& state) { return state.fromTimeline; }), pitchBendStates.end());
                pressureStates.erase(std::remove_if(pressureStates.begin(), pressureStates.end(),
                    [](const PressureState& state) { return state.fromTimeline; }), pressureStates.end());
                for (int i = 0; i < seed.bendCount && pitchBendStates.size() < 1024u; ++i)
                    pitchBendStates.push_back(seed.bends[static_cast<size_t>(i)]);
                for (int i = 0; i < seed.pressureCount && pressureStates.size() < 1024u; ++i)
                    pressureStates.push_back(seed.pressures[static_cast<size_t>(i)]);
				std::sort(pitchBendStates.begin(), pitchBendStates.end(),
					[](const PitchBendState& left, const PitchBendState& right) {
						return controllerStateKeyLess(left.layerId, left.channel, left.fromTimeline,
							right.layerId, right.channel, right.fromTimeline);
					});
				std::sort(pressureStates.begin(), pressureStates.end(),
					[](const PressureState& left, const PressureState& right) {
						return controllerStateKeyLess(left.layerId, left.channel, left.fromTimeline,
							right.layerId, right.channel, right.fromTimeline);
					});
                size_t freeVoiceIndex = 0;
                for (int i = 0; i < seed.voiceCount; ++i) {
                    while (freeVoiceIndex < activeVoices.size() && activeVoices[freeVoiceIndex].active)
                        ++freeVoiceIndex;
                    if (freeVoiceIndex >= activeVoices.size()) break;
                    activeVoices[freeVoiceIndex++] = seed.voices[static_cast<size_t>(i)];
                }
                ReleaseTimelineVoiceSeed(seedIndex);
                appliedTimelineVoiceControlGeneration = generation;
            }
        }
    }

    if (clearRealtimeVoicesRequested.exchange(false, std::memory_order_acq_rel)) {
        for (SynthVoice& voice : activeVoices)
            if (!voice.fromTimeline) voice = {};
        pitchBendStates.erase(std::remove_if(pitchBendStates.begin(), pitchBendStates.end(),
            [](const PitchBendState& state) { return !state.fromTimeline; }), pitchBendStates.end());
        pressureStates.erase(std::remove_if(pressureStates.begin(), pressureStates.end(),
            [](const PressureState& state) { return !state.fromTimeline; }), pressureStates.end());
    }

    uint64_t read = realtimeVoiceCommandRead.load(std::memory_order_relaxed);
    const uint64_t write = realtimeVoiceCommandWrite.load(std::memory_order_acquire);
    while (read < write) {
        const RealtimeVoiceRouteCommand command = realtimeVoiceCommands[
            static_cast<size_t>(read % realtimeVoiceCommands.size())];
        for (SynthVoice& voice : activeVoices) {
            if (!voice.active || voice.fromTimeline || voice.layerId != command.layerId) continue;
            voice.layerGain = command.gain;
            voice.gain = combinedVoiceGain(voice.velocityGain, voice.layerGain, voice.pressureGain);
            voice.pan = command.pan;
			const PanGains gains = panGains(
				1.0, voice.pan, true, callbackPanLawDb);
            voice.panLeft = gains.left;
            voice.panRight = gains.right;
        }
        ++read;
    }
    realtimeVoiceCommandRead.store(read, std::memory_order_release);
}

void VeraCoreAudio::SeedExternalTimelineAtPlayheadLocked(double playheadBeat)
{
    using StateKey = std::tuple<int, int, int>;
    struct ActivePluginNote {
        const VeraCoreBeatMidiEvent* latestEvent = nullptr;
        int noteOnCount = 0;
    };
    std::vector<ActivePluginNote> activeNotes;
    std::map<StateKey, const VeraCoreBeatMidiEvent*> channelState;
    std::map<std::pair<int, int>, const VeraCoreBeatMidiEvent*> keyswitchState;
    const std::vector<VeraCoreBeatMidiEvent>& timeline = *midiTimelineEvents;
    for (const VeraCoreBeatMidiEvent& event : timeline) {
        if (event.beat >= playheadBeat) {
            break;
        }
        if (!event.externalPlugin) {
            continue;
        }
        const uint8_t statusHi = event.status & 0xF0;
        const bool noteOn = statusHi == 0x90 && event.data2 > 0;
        const bool noteOff = statusHi == 0x80 || (statusHi == 0x90 && event.data2 == 0);
        if (statusHi >= 0xA0 && statusHi <= 0xE0) {
            const int controller = (statusHi == 0xA0 || statusHi == 0xB0) ? event.data1 : 0;
            channelState[{event.layerId, event.status, controller}] = &event;
            continue;
        }
        if ((!noteOn && !noteOff) || event.data1 > 127) {
            continue;
        }
        const int channel = event.status & 0x0F;
        if (event.isKeyswitch) {
            if (noteOn) keyswitchState[{event.layerId, channel}] = &event;
            continue;
        }
        auto matching = std::find_if(activeNotes.begin(), activeNotes.end(),
            [&](const ActivePluginNote& active) {
                return active.latestEvent && active.latestEvent->layerId == event.layerId &&
                    active.latestEvent->data1 == event.data1 &&
                    (active.latestEvent->status & 0x0F) == channel;
            });
        if (noteOn) {
            if (matching == activeNotes.end()) {
                activeNotes.push_back({&event, 1});
            } else {
                matching->latestEvent = &event;
                if (matching->noteOnCount < std::numeric_limits<int>::max()) {
                    ++matching->noteOnCount;
                }
            }
        } else if (matching != activeNotes.end()) {
            if (matching->noteOnCount > 1) {
                --matching->noteOnCount;
            } else {
                activeNotes.erase(matching);
            }
        }
    }

    const uint64_t generation = externalTimelineGeneration.load(std::memory_order_acquire);
    auto enqueueChasedEvent = [&](const VeraCoreBeatMidiEvent& source) {
        VeraCoreMidiEvent event{0, source.status, source.data1, source.data2, source.layerId,
                                source.gain, source.pan, true, true, -1.0,
                                false, false, 0, source.isKeyswitch, generation};
        return TryEnqueuePendingMidiEvent(event);
    };
    for (const auto& state : channelState) {
        if (state.second && !enqueueChasedEvent(*state.second)) return;
    }
    for (const auto& state : keyswitchState) {
        if (!state.second) continue;
        if (!enqueueChasedEvent(*state.second)) return;
        VeraCoreBeatMidiEvent noteOff = *state.second;
        noteOff.status = static_cast<uint8_t>(0x80 | std::get<1>(state.first));
        noteOff.data2 = 0;
        VeraCoreMidiEvent release{1, noteOff.status, noteOff.data1, noteOff.data2,
                                  noteOff.layerId, noteOff.gain, noteOff.pan,
                                  true, true, -1.0, false, false, 0, true, generation};
        if (!TryEnqueuePendingMidiEvent(release)) return;
    }
    for (const ActivePluginNote& active : activeNotes) {
        if (!active.latestEvent) {
            continue;
        }
        for (int count = 0; count < active.noteOnCount; ++count) {
            if (!enqueueChasedEvent(*active.latestEvent)) return;
        }
    }
}

void VeraCoreAudio::SetTransportState(bool playing, double bpm, double playheadBeat)
{
    std::lock_guard<std::mutex> lock(midiStateMutex);
    const bool wasPlaying = requestedTransportPlaying.load(std::memory_order_relaxed);
    const double previousPlayheadBeat = requestedTransportPlayheadBeat.load(std::memory_order_relaxed);
    const double safePlayheadBeat = std::isfinite(playheadBeat) && playheadBeat >= 0.0 ? playheadBeat : 0.0;
    const double safeBpm = std::isfinite(bpm) && bpm > 0.0 ? bpm : 120.0;
    const bool timelineDiscontinuity = wasPlaying &&
        (!playing || std::fabs(previousPlayheadBeat - safePlayheadBeat) > 1.0e-6);
    const bool shouldSeedTimelineVoices =
        playing && (!wasPlaying || std::fabs(previousPlayheadBeat - safePlayheadBeat) > 1.0e-6);

    if (timelineDiscontinuity) QueueExternalTimelinePanicLocked();

    transportCommandGeneration.fetch_add(1, std::memory_order_acq_rel);
    requestedTransportBpm.store(safeBpm, std::memory_order_relaxed);
    requestedTransportPlayheadBeat.store(safePlayheadBeat, std::memory_order_relaxed);
    requestedTransportPlaying.store(playing, std::memory_order_relaxed);
    transportCommandGeneration.fetch_add(1, std::memory_order_release);
    if (!playing) {
        RequestClearAllVoices();
        return;
    }

    if (!shouldSeedTimelineVoices) {
        return;
    }
    SeedExternalTimelineAtPlayheadLocked(safePlayheadBeat);
    SeedTimelineVoicesAtPlayheadLocked(safePlayheadBeat);
}

void VeraCoreAudio::SetTempoMap(const std::vector<VeraCoreTempoPoint>& points)
{
    std::vector<VeraCoreTempoPoint> sanitized;
    sanitized.reserve(std::min(points.size(), kMaxTempoMapPoints));
    for (const VeraCoreTempoPoint& point : points) {
        if (!std::isfinite(point.beat) || point.beat < 0.0 ||
            !std::isfinite(point.bpm) || point.bpm < kMinTempoBpm || point.bpm > kMaxTempoBpm) continue;
        if (sanitized.size() >= kMaxTempoMapPoints) break;
        sanitized.push_back(point);
    }
    std::stable_sort(sanitized.begin(), sanitized.end(),
        [](const VeraCoreTempoPoint& left, const VeraCoreTempoPoint& right) {
            return left.beat < right.beat;
        });
    std::vector<VeraCoreTempoPoint> collapsed;
    collapsed.reserve(std::min(sanitized.size(), kMaxTempoMapPoints));
    for (const VeraCoreTempoPoint& point : sanitized) {
        if (!collapsed.empty() && collapsed.back().beat == point.beat) {
            collapsed.back() = point;
        } else if (collapsed.size() < kMaxTempoMapPoints) {
            collapsed.push_back(point);
        }
    }
    if (!collapsed.empty() && collapsed.front().beat > 0.0) {
        const double fallbackBpm = finiteClamp(requestedTransportBpm.load(std::memory_order_acquire),
                                               kMinTempoBpm, kMaxTempoBpm, 120.0);
        if (collapsed.size() == kMaxTempoMapPoints) collapsed.pop_back();
        collapsed.insert(collapsed.begin(), VeraCoreTempoPoint{0.0, fallbackBpm});
    }

    double accumulatedSeconds = 0.0;
    for (size_t index = 0; index < collapsed.size(); ++index) {
        if (index > 0) {
            const VeraCoreTempoPoint& previous = collapsed[index - 1];
            accumulatedSeconds += (collapsed[index].beat - previous.beat) *
                60.0 / previous.bpm;
        }
        collapsed[index].seconds = accumulatedSeconds;
    }

    auto published = std::make_shared<const std::vector<VeraCoreTempoPoint>>(std::move(collapsed));
    std::lock_guard<std::mutex> lock(midiStateMutex);
    tempoMap = std::move(published);
    PublishMidiSchedulerBindingLocked();
}

void VeraCoreAudio::SetTimeSignatureMap(const std::vector<VeraCoreTimeSignaturePoint>& points)
{
    auto validDenominator = [](int denominator) {
        return denominator >= 1 && denominator <= 64 &&
            (denominator & (denominator - 1)) == 0;
    };
    std::vector<VeraCoreTimeSignaturePoint> sanitized;
    sanitized.reserve(std::min(points.size(), kMaxTimeSignatureMapPoints));
    for (const VeraCoreTimeSignaturePoint& point : points) {
        if (!std::isfinite(point.beat) || point.beat < 0.0 ||
            point.numerator < 1 || point.numerator > 32 ||
            !validDenominator(point.denominator)) continue;
        if (sanitized.size() >= kMaxTimeSignatureMapPoints) break;
        sanitized.push_back(point);
    }
    std::stable_sort(sanitized.begin(), sanitized.end(),
        [](const VeraCoreTimeSignaturePoint& left, const VeraCoreTimeSignaturePoint& right) {
            return left.beat < right.beat;
        });
    std::vector<VeraCoreTimeSignaturePoint> collapsed;
    collapsed.reserve(sanitized.size());
    for (const VeraCoreTimeSignaturePoint& point : sanitized) {
        if (!collapsed.empty() && collapsed.back().beat == point.beat) collapsed.back() = point;
        else collapsed.push_back(point);
    }
    if (collapsed.empty()) {
        collapsed.push_back({0.0,
            timeSignatureNumerator.load(std::memory_order_acquire),
            timeSignatureDenominator.load(std::memory_order_acquire)});
    } else if (collapsed.front().beat > 0.0) {
        if (collapsed.size() == kMaxTimeSignatureMapPoints) collapsed.pop_back();
        collapsed.insert(collapsed.begin(), {0.0,
            timeSignatureNumerator.load(std::memory_order_acquire),
            timeSignatureDenominator.load(std::memory_order_acquire)});
    }
    int64_t completedBars = 0;
    if (!collapsed.empty()) collapsed.front().barNumber = 0;
    for (size_t index = 1; index < collapsed.size(); ++index) {
        const VeraCoreTimeSignaturePoint& previous = collapsed[index - 1];
        const double beatsPerBar = static_cast<double>(previous.numerator) * 4.0 /
            static_cast<double>(previous.denominator);
        const double ratio = (collapsed[index].beat - previous.beat) / beatsPerBar;
        const double nearest = std::round(ratio);
        const double tolerance = std::min(0.25,
            16.0 * std::numeric_limits<double>::epsilon() *
                std::max(1.0, std::fabs(ratio)));
        const double stable = std::fabs(ratio - nearest) <= tolerance ? nearest : ratio;
        const int64_t bars = static_cast<int64_t>(std::min<double>(
            INT32_MAX, std::max(0.0, std::ceil(stable))));
        completedBars = std::min<int64_t>(INT32_MAX, completedBars + bars);
        collapsed[index].barNumber = static_cast<int>(completedBars);
    }
    auto published = std::make_shared<const std::vector<VeraCoreTimeSignaturePoint>>(
        std::move(collapsed));
    std::lock_guard<std::mutex> lock(midiStateMutex);
    timeSignatureMap = std::move(published);
    PublishMidiSchedulerBindingLocked();
}

void VeraCoreAudio::SetLoopRange(bool enabled, double startBeat, double endBeat)
{
    std::lock_guard<std::mutex> lock(midiStateMutex);
    const bool valid = enabled && std::isfinite(startBeat) && std::isfinite(endBeat) &&
        startBeat >= 0.0 && endBeat - startBeat >= 0.25;
    transportLoopStartBeat.store(valid ? startBeat : 0.0, std::memory_order_relaxed);
    transportLoopEndBeat.store(valid ? endBeat : 4.0, std::memory_order_relaxed);
    transportLoopEnabled.store(valid, std::memory_order_release);
    RebuildLoopChaseEventsLocked();
    PublishMidiSchedulerBindingLocked();
}

void VeraCoreAudio::SetAudioInputState(const std::vector<int>& monitorTrackIds, bool recording)
{
    const bool wasRecording = audioInputRecording.load(std::memory_order_acquire);
    if (!recording) audioInputRecording.store(false, std::memory_order_release);
    audioInputMonitorTrackCount.store(0, std::memory_order_release);
    int monitorCount = 0;
    for (int trackId : monitorTrackIds) {
        if (trackId < 0 || monitorCount >= static_cast<int>(audioInputMonitorTrackIds.size())) continue;
        bool duplicate = false;
        for (int i = 0; i < monitorCount; ++i)
            duplicate = duplicate || audioInputMonitorTrackIds[static_cast<size_t>(i)].load(
                std::memory_order_relaxed) == trackId;
        if (duplicate) continue;
        audioInputMonitorTrackIds[static_cast<size_t>(monitorCount++)].store(
            trackId, std::memory_order_relaxed);
    }
    audioInputMonitorTrackCount.store(monitorCount, std::memory_order_release);
    if (recording && !wasRecording) {
        audioInputRecording.store(false, std::memory_order_release);
        audioInputReadIndex.store(0, std::memory_order_release);
        audioInputWriteIndex.store(0, std::memory_order_release);
        audioInputOverflowed.store(false, std::memory_order_relaxed);
    }
    audioInputRecording.store(recording, std::memory_order_release);
}

void VeraCoreAudio::DrainRecordedAudio(std::vector<float>& destination, bool* overflowed)
{
    const uint64_t capacity = static_cast<uint64_t>(audioInputRing.size());
    if (capacity == 0) return;
    uint64_t read = audioInputReadIndex.load(std::memory_order_relaxed);
    const uint64_t write = audioInputWriteIndex.load(std::memory_order_acquire);
    const uint64_t count = write >= read ? std::min(capacity, write - read) : 0;
    if (count > 0) {
        destination.reserve(destination.size() + static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            destination.push_back(audioInputRing[static_cast<size_t>((read + i) % capacity)]);
        }
        read += count;
        audioInputReadIndex.store(read, std::memory_order_release);
    }
    if (overflowed && audioInputOverflowed.exchange(false, std::memory_order_acq_rel)) *overflowed = true;
}

void VeraCoreAudio::RebuildLoopChaseEventsLocked()
{
    loopChaseEvents.clear();
    const bool loopEnabled = transportLoopEnabled.load(std::memory_order_acquire);
    const double loopStartBeat = transportLoopStartBeat.load(std::memory_order_relaxed);
    const double loopEndBeat = transportLoopEndBeat.load(std::memory_order_relaxed);
    if (!loopEnabled) return;

    using NoteKey = std::tuple<int, int, int, bool>;
    using StateKey = std::tuple<int, int, int, bool>;
    struct ActiveNoteState {
        VeraCoreBeatMidiEvent event;
        int count = 0;
    };
    std::map<NoteKey, ActiveNoteState> activeNotes;
    std::map<StateKey, VeraCoreBeatMidiEvent> channelState;
    std::map<std::tuple<int, int, bool>, VeraCoreBeatMidiEvent> keyswitchState;
    std::set<std::tuple<int, int, bool>> loopChannels;
    const std::vector<VeraCoreBeatMidiEvent>& timeline = *midiTimelineEvents;
    for (const VeraCoreBeatMidiEvent& event : timeline) {
        if (event.beat >= loopEndBeat) break;
        loopChannels.insert({event.layerId, event.status & 0x0F, event.externalPlugin});
    }
    for (const VeraCoreBeatMidiEvent& event : timeline) {
        if (event.beat >= loopStartBeat) break;
        if (event.isSlide) continue;
        const int statusHi = event.status & 0xF0;
        const int channel = event.status & 0x0F;
        const bool noteOn = statusHi == 0x90 && event.data2 > 0;
        const bool noteOff = statusHi == 0x80 || (statusHi == 0x90 && event.data2 == 0);
        if (noteOn || noteOff) {
            if (event.isKeyswitch) {
                if (noteOn)
                    keyswitchState[{event.layerId, channel, event.externalPlugin}] = event;
                continue;
            }
            const NoteKey key{event.layerId, channel, event.data1, event.externalPlugin};
            auto& state = activeNotes[key];
            if (noteOn) {
                state.event = event;
                if (state.count < std::numeric_limits<int>::max()) ++state.count;
            } else if (state.count > 0) {
                --state.count;
            }
            continue;
        }
        if (statusHi >= 0xA0 && statusHi <= 0xE0) {
            const int controller = (statusHi == 0xA0 || statusHi == 0xB0) ? event.data1 : 0;
            channelState[{event.layerId, event.status, controller, event.externalPlugin}] = event;
        }
    }

    for (const auto& channel : loopChannels) {
        VeraCoreBeatMidiEvent reset;
        reset.beat = loopStartBeat;
        reset.status = static_cast<uint8_t>(0xB0 | std::get<1>(channel));
        reset.data1 = 123;
        reset.data2 = 0;
        reset.layerId = std::get<0>(channel);
        reset.externalPlugin = std::get<2>(channel);
        loopChaseEvents.push_back(reset);
    }
    for (const auto& entry : channelState) {
        VeraCoreBeatMidiEvent event = entry.second;
        event.beat = loopStartBeat;
        loopChaseEvents.push_back(event);
    }
    for (const auto& entry : keyswitchState) {
        VeraCoreBeatMidiEvent noteOn = entry.second;
        noteOn.beat = loopStartBeat;
        loopChaseEvents.push_back(noteOn);
        VeraCoreBeatMidiEvent noteOff = noteOn;
        noteOff.beat = std::nextafter(loopStartBeat,
                                      std::numeric_limits<double>::infinity());
        noteOff.status = static_cast<uint8_t>(0x80 | std::get<1>(entry.first));
        noteOff.data2 = 0;
        loopChaseEvents.push_back(noteOff);
    }
    for (const auto& entry : activeNotes) {
        if (entry.second.count <= 0) continue;
        VeraCoreBeatMidiEvent event = entry.second.event;
        event.beat = loopStartBeat;
        loopChaseEvents.push_back(event);
    }
    std::sort(loopChaseEvents.begin(), loopChaseEvents.end(), [](const VeraCoreBeatMidiEvent& a,
                                                                 const VeraCoreBeatMidiEvent& b) {
        if (a.beat != b.beat) return a.beat < b.beat;
        const int aRank = midiEventSortRank(a.status, a.data2);
        const int bRank = midiEventSortRank(b.status, b.data2);
        if (aRank != bRank) return aRank < bRank;
        if (a.layerId != b.layerId) return a.layerId < b.layerId;
        if (a.status != b.status) return a.status < b.status;
        return a.data1 < b.data1;
    });
}

void VeraCoreAudio::SetMetronomeState(bool enabled, int numerator, int denominator)
{
	std::lock_guard<std::mutex> lock(metronomeStateWriteMutex);
	metronomeStateGeneration.fetch_add(1, std::memory_order_acq_rel);
    timeSignatureNumerator.store(std::clamp(numerator, 1, 32), std::memory_order_relaxed);
    timeSignatureDenominator.store(
        (denominator == 2 || denominator == 4 || denominator == 8 ||
         denominator == 16 || denominator == 32) ? denominator : 4,
        std::memory_order_relaxed);
	metronomeEnabled.store(enabled, std::memory_order_relaxed);
	metronomeStateGeneration.fetch_add(1, std::memory_order_release);
}

std::vector<VeraCoreMidiEvent> VeraCoreAudio::BuildMidiEventsForBlock(double blockStartBeat,
                                                                      double beatsPerSample,
                                                                      int frameCount)
{
    std::vector<VeraCoreMidiEvent> blockEvents;
    if (!std::isfinite(blockStartBeat) || !std::isfinite(beatsPerSample) ||
        beatsPerSample <= 0.0 || frameCount <= 0) {
        return blockEvents;
    }

    const double blockEndBeat = blockStartBeat + beatsPerSample * frameCount;
    if (!std::isfinite(blockEndBeat)) {
        return blockEvents;
    }
    const int bindingIndex = AcquireMidiSchedulerBinding();
    if (bindingIndex < 0) return blockEvents;
    const MidiSchedulerBinding& binding = midiSchedulerBindings[static_cast<size_t>(bindingIndex)];
    const std::vector<VeraCoreBeatMidiEvent>& timeline = *binding.timelineEvents;
    const auto firstEvent = std::lower_bound(timeline.begin(), timeline.end(), blockStartBeat,
        [](const VeraCoreBeatMidiEvent& event, double beat) { return event.beat < beat; });
    for (auto eventIt = firstEvent; eventIt != timeline.end() && eventIt->beat < blockEndBeat; ++eventIt) {
        const VeraCoreBeatMidiEvent& event = *eventIt;

        const int sampleOffset = clampSampleOffset((event.beat - blockStartBeat) / beatsPerSample, frameCount);
        blockEvents.push_back({sampleOffset, event.status, event.data1, event.data2, event.layerId,
                               event.gain, event.pan, true, event.externalPlugin, -1.0,
                               event.isSlide, event.isPortamento,
                               glideSampleCount(event.glideDurationBeats, beatsPerSample),
                               event.isKeyswitch});
    }
    ReleaseMidiSchedulerBinding(bindingIndex);
    return blockEvents;
}

std::vector<VeraCoreMidiEvent> VeraCoreAudio::BuildPlaybackMidiEvents(int frameCount)
{
    return BuildPlaybackMidiEvents(frameCount, nullptr, nullptr);
}

void VeraCoreAudio::BuildCallbackMidiEvents(int frameCount, double* outBlockStartBeat, double* outBlockEndBeat)
{
    callbackMidiEvents.clear();
    callbackSegmentCount = 0;
    callbackMidiTrackCount = 0;
    if (outBlockStartBeat) *outBlockStartBeat = 0.0;
	if (outBlockEndBeat) *outBlockEndBeat = 0.0;
	ApplyPendingTransportCommand();
	for (int attempt = 0; attempt < 3; ++attempt) {
		const uint64_t generation = metronomeStateGeneration.load(std::memory_order_acquire);
		if ((generation & 1u) != 0u) continue;
		const bool enabled = metronomeEnabled.load(std::memory_order_relaxed);
		const int numerator = timeSignatureNumerator.load(std::memory_order_relaxed);
		const int denominator = timeSignatureDenominator.load(std::memory_order_relaxed);
		if (generation != metronomeStateGeneration.load(std::memory_order_acquire)) continue;
		callbackMetronomeEnabled = enabled;
		callbackTimeSignatureNumerator = numerator;
		callbackTimeSignatureDenominator = denominator;
		break;
	}

    const uint64_t externalGeneration = externalTimelineGeneration.load(std::memory_order_acquire);
    VeraCoreMidiEvent pendingEvent;
    while (callbackMidiEvents.size() < kMaxCallbackMidiEvents && TryDequeuePendingMidiEvent(pendingEvent)) {
        const bool isExternalPanic = (pendingEvent.status & 0xF0) == 0xB0 &&
            (pendingEvent.data1 == 120 || pendingEvent.data1 == 123);
        if (pendingEvent.fromTimeline && pendingEvent.externalPlugin &&
            !isExternalPanic && pendingEvent.timelineGeneration != externalGeneration) continue;
        callbackMidiEvents.push_back(pendingEvent);
    }

    int bindingIndex = -1;
    for (int attempt = 0; attempt < 3 && bindingIndex < 0; ++attempt)
        bindingIndex = AcquireMidiSchedulerBinding();
    if (bindingIndex < 0) return;
    const MidiSchedulerBinding& binding = midiSchedulerBindings[static_cast<size_t>(bindingIndex)];
    for (int i = 0; i < binding.trackIdCount; ++i)
        callbackMidiTrackIds[static_cast<size_t>(callbackMidiTrackCount++)] =
            binding.trackIds[static_cast<size_t>(i)];
    const bool playing = transportPlaying.load(std::memory_order_acquire);
    const double playheadBeat = transportPlayheadBeat.load(std::memory_order_relaxed);
    const double bpm = transportBpm.load(std::memory_order_relaxed);
    const bool loopEnabled = binding.loopEnabled;
    const double loopStartBeat = binding.loopStartBeat;
    const double loopEndBeat = binding.loopEndBeat;
    if (frameCount <= 0 ||
        !std::isfinite(playheadBeat) || !std::isfinite(bpm) ||
        !std::isfinite(streamSampleRate) || bpm <= 0.0 || streamSampleRate <= 0.0) {
        ReleaseMidiSchedulerBinding(bindingIndex);
        return;
    }

    const double fallbackBeatsPerSample = bpm / 60.0 / streamSampleRate;
    if (!std::isfinite(fallbackBeatsPerSample) || fallbackBeatsPerSample <= 0.0) {
        ReleaseMidiSchedulerBinding(bindingIndex);
        return;
    }
    const std::vector<VeraCoreBeatMidiEvent>& timeline = *binding.timelineEvents;
    const std::vector<VeraCoreTempoPoint>* tempoPoints = binding.tempoMap.get();
    const std::vector<VeraCoreTimeSignaturePoint>* signaturePoints = binding.timeSignatureMap.get();
    const auto secondsAtBeat = [tempoPoints, bpm](double beat) noexcept {
        const double safeBeat = std::isfinite(beat) ? std::max(0.0, beat) : 0.0;
        if (!tempoPoints || tempoPoints->empty()) return safeBeat * 60.0 / bpm;
        const auto next = std::upper_bound(
            tempoPoints->begin(), tempoPoints->end(), safeBeat,
            [](double target, const VeraCoreTempoPoint& point) { return target < point.beat; });
        const VeraCoreTempoPoint& active = next == tempoPoints->begin()
            ? tempoPoints->front() : *(next - 1);
        return active.seconds + (safeBeat - active.beat) * 60.0 / active.bpm;
    };

    if (!playing) {
        CallbackTransportSegment& segment = callbackSegments[0];
        segment = {};
        segment.frameCount = frameCount;
        segment.startBeat = playheadBeat;
        segment.endBeat = playheadBeat;
        segment.startSeconds = secondsAtBeat(playheadBeat);
        segment.bpm = bpm;
        if (tempoPoints && !tempoPoints->empty()) {
            const auto next = std::upper_bound(
                tempoPoints->begin(), tempoPoints->end(), playheadBeat,
                [](double beat, const VeraCoreTempoPoint& point) { return beat < point.beat; });
            segment.bpm = (next == tempoPoints->begin()
                ? tempoPoints->front() : *(next - 1)).bpm;
        }
		segment.numerator = callbackTimeSignatureNumerator;
		segment.denominator = callbackTimeSignatureDenominator;
        if (signaturePoints && !signaturePoints->empty()) {
            const auto next = std::upper_bound(
                signaturePoints->begin(), signaturePoints->end(), playheadBeat,
                [](double beat, const VeraCoreTimeSignaturePoint& point) {
                    return beat < point.beat;
                });
            const VeraCoreTimeSignaturePoint& active = next == signaturePoints->begin()
                ? signaturePoints->front() : *(next - 1);
            segment.signatureStartBeat = active.beat;
            segment.numerator = active.numerator;
            segment.denominator = active.denominator;
            segment.barNumber = active.barNumber;
        }
        const double beatsPerBar = static_cast<double>(segment.numerator) * 4.0 /
            static_cast<double>(segment.denominator);
        const double localBars = std::floor(std::max(0.0,
            (playheadBeat - segment.signatureStartBeat) / beatsPerBar));
        segment.barStartBeat = segment.signatureStartBeat + localBars * beatsPerBar;
        segment.barNumber = static_cast<int>(std::min<int64_t>(INT32_MAX,
            static_cast<int64_t>(segment.barNumber) +
            static_cast<int64_t>(std::min<double>(INT32_MAX, localBars))));
        callbackSegmentCount = 1;
        if (outBlockStartBeat) *outBlockStartBeat = playheadBeat;
        if (outBlockEndBeat) *outBlockEndBeat = playheadBeat;
        ReleaseMidiSchedulerBinding(bindingIndex);
        return;
    }

    double currentBeat = playheadBeat;
    bool chaseAtSegmentStart = false;
    if (loopEnabled && currentBeat >= loopEndBeat) {
        const double loopLength = loopEndBeat - loopStartBeat;
        currentBeat = loopLength > 0.0
            ? loopStartBeat + std::fmod(currentBeat - loopStartBeat, loopLength)
            : loopStartBeat;
        chaseAtSegmentStart = true;
    }
    const double loopDurationSeconds = loopEnabled
        ? secondsAtBeat(loopEndBeat) - secondsAtBeat(loopStartBeat) : 0.0;
    const bool loopSplittable = loopEnabled && std::isfinite(loopDurationSeconds) &&
        static_cast<long double>(loopDurationSeconds) *
            static_cast<long double>(streamSampleRate) >= kMinPluginCycleFrames;
    const double firstBeat = currentBeat;
    int remainingFrames = frameCount;
    int frameOffset = 0;
    int tempoSplitCount = 0;
    int signatureSplitCount = 0;
    while (remainingFrames > 0 && callbackSegmentCount < static_cast<int>(callbackSegments.size())) {
        double segmentBpm = bpm;
        const VeraCoreTempoPoint* nextTempoPoint = nullptr;
        if (tempoPoints && !tempoPoints->empty()) {
            const auto nextTempo = std::upper_bound(tempoPoints->begin(), tempoPoints->end(), currentBeat,
                [](double beat, const VeraCoreTempoPoint& point) { return beat < point.beat; });
            if (nextTempo != tempoPoints->begin()) segmentBpm = (nextTempo - 1)->bpm;
            if (nextTempo != tempoPoints->end()) nextTempoPoint = &*nextTempo;
        }
        const double beatsPerSample = segmentBpm / 60.0 / streamSampleRate;
        if (!std::isfinite(beatsPerSample) || beatsPerSample <= 0.0) break;
		int segmentNumerator = callbackTimeSignatureNumerator;
		int segmentDenominator = callbackTimeSignatureDenominator;
        double signatureStartBeat = 0.0;
        int signatureStartBarNumber = 0;
        const VeraCoreTimeSignaturePoint* nextSignaturePoint = nullptr;
        if (signaturePoints && !signaturePoints->empty()) {
            const auto nextSignature = std::upper_bound(
                signaturePoints->begin(), signaturePoints->end(), currentBeat,
                [](double beat, const VeraCoreTimeSignaturePoint& point) { return beat < point.beat; });
            if (nextSignature != signaturePoints->begin()) {
                const VeraCoreTimeSignaturePoint& active = *(nextSignature - 1);
                segmentNumerator = active.numerator;
                segmentDenominator = active.denominator;
                signatureStartBeat = active.beat;
                signatureStartBarNumber = active.barNumber;
            }
            if (nextSignature != signaturePoints->end()) nextSignaturePoint = &*nextSignature;
        }
        int segmentFrames = remainingFrames;
        bool splitAtTempo = false;
        bool splitAtSignature = false;
        if (nextTempoPoint && tempoSplitCount + signatureSplitCount < kMaxTempoSplitsPerCallback) {
            const double beatsUntilTempo = nextTempoPoint->beat - currentBeat;
            if (beatsUntilTempo > 0.0) {
                const int framesUntilTempo = framesUntilBeatBoundary(
                    beatsUntilTempo, beatsPerSample, segmentFrames);
                if (framesUntilTempo < segmentFrames) {
                    segmentFrames = framesUntilTempo;
                    splitAtTempo = true;
                }
            }
        }
        if (nextSignaturePoint && tempoSplitCount + signatureSplitCount < kMaxTempoSplitsPerCallback) {
            const double beatsUntilSignature = nextSignaturePoint->beat - currentBeat;
            if (beatsUntilSignature > 0.0) {
                const int framesUntilSignature = framesUntilBeatBoundary(
                    beatsUntilSignature, beatsPerSample, remainingFrames);
                if (framesUntilSignature < segmentFrames) {
                    segmentFrames = framesUntilSignature;
                    splitAtTempo = false;
                    splitAtSignature = true;
                } else if (framesUntilSignature == segmentFrames && splitAtTempo) {
                    splitAtSignature = true;
                }
            }
        }
        if (loopSplittable) {
            const double beatsUntilLoopEnd = loopEndBeat - currentBeat;
            if (beatsUntilLoopEnd <= 0.0) {
                currentBeat = loopStartBeat;
                chaseAtSegmentStart = true;
                continue;
            }
            const int framesUntilLoopEnd = framesUntilBeatBoundary(
                beatsUntilLoopEnd, beatsPerSample, remainingFrames);
            if (framesUntilLoopEnd < segmentFrames) {
                segmentFrames = framesUntilLoopEnd;
                splitAtTempo = false;
                splitAtSignature = false;
            }
        }

        CallbackTransportSegment& segment = callbackSegments[static_cast<size_t>(callbackSegmentCount++)];
        segment.frameOffset = frameOffset;
        segment.frameCount = segmentFrames;
        segment.startBeat = currentBeat;
        segment.endBeat = currentBeat + beatsPerSample * segmentFrames;
        segment.beatsPerSample = beatsPerSample;
        segment.bpm = segmentBpm;
        segment.startSeconds = secondsAtBeat(currentBeat);
        segment.cycleWrappedAtStart = chaseAtSegmentStart;
        segment.signatureStartBeat = signatureStartBeat;
        const double beatsPerBar = static_cast<double>(segmentNumerator) * 4.0 /
            static_cast<double>(segmentDenominator);
        const double localRatio = std::max(0.0, currentBeat - signatureStartBeat) / beatsPerBar;
        const double nearestBar = std::round(localRatio);
        const double barTolerance = std::min(0.25,
            16.0 * std::numeric_limits<double>::epsilon() *
                std::max(1.0, std::fabs(localRatio)));
        const double stableBar = std::fabs(localRatio - nearestBar) <= barTolerance
            ? nearestBar : localRatio;
        double localBars = std::floor(std::max(0.0, stableBar));
        segment.barStartBeat = signatureStartBeat + localBars * beatsPerBar;
        if (segment.barStartBeat > currentBeat && localBars > 0.0) {
            localBars -= 1.0;
            segment.barStartBeat = signatureStartBeat + localBars * beatsPerBar;
        }
        segment.barNumber = static_cast<int>(std::min<int64_t>(INT32_MAX,
            static_cast<int64_t>(signatureStartBarNumber) +
            static_cast<int64_t>(std::min<double>(INT32_MAX, localBars))));
        segment.numerator = segmentNumerator;
        segment.denominator = segmentDenominator;

        if (chaseAtSegmentStart) {
            for (const VeraCoreBeatMidiEvent& event : binding.loopChaseEvents) {
                if (callbackMidiEvents.size() >= kMaxCallbackMidiEvents) break;
                callbackMidiEvents.push_back({frameOffset, event.status, event.data1, event.data2, event.layerId,
                                              event.gain, event.pan, true, event.externalPlugin, -1.0,
                                              event.isSlide, event.isPortamento,
                                              glideSampleCount(event.glideDurationBeats, beatsPerSample),
                                              event.isKeyswitch});
            }
            chaseAtSegmentStart = false;
        }
        const double eventEndBeat = loopEnabled
            ? std::min(segment.endBeat, loopEndBeat)
            : segment.endBeat;
        const auto firstEvent = std::lower_bound(timeline.begin(), timeline.end(), segment.startBeat,
            [](const VeraCoreBeatMidiEvent& event, double beat) { return event.beat < beat; });
        for (auto eventIt = firstEvent;
             eventIt != timeline.end() && eventIt->beat < eventEndBeat; ++eventIt) {
            if (callbackMidiEvents.size() >= kMaxCallbackMidiEvents) break;
            const VeraCoreBeatMidiEvent& event = *eventIt;
            const int localOffset = clampSampleOffset(
                (event.beat - segment.startBeat) / beatsPerSample, segmentFrames);
            callbackMidiEvents.push_back({frameOffset + localOffset, event.status, event.data1, event.data2,
                                          event.layerId, event.gain, event.pan, true, event.externalPlugin, -1.0,
                                          event.isSlide, event.isPortamento,
                                          glideSampleCount(event.glideDurationBeats, beatsPerSample),
                                          event.isKeyswitch});
        }

        remainingFrames -= segmentFrames;
        frameOffset += segmentFrames;
        currentBeat = segment.endBeat;
        if (splitAtTempo && nextTempoPoint && currentBeat >= nextTempoPoint->beat - beatsPerSample * 0.5)
            ++tempoSplitCount;
        if (splitAtSignature && nextSignaturePoint &&
            currentBeat >= nextSignaturePoint->beat - beatsPerSample * 0.5)
            ++signatureSplitCount;
        if (loopEnabled && currentBeat >= loopEndBeat - beatsPerSample * 0.5) {
            const double loopLength = loopEndBeat - loopStartBeat;
            currentBeat = loopLength > 0.0
                ? loopStartBeat + std::fmod(
                    std::max(0.0, currentBeat - loopStartBeat), loopLength)
                : loopStartBeat;
            chaseAtSegmentStart = true;
        }
    }
    if (remainingFrames > 0) {
        CallbackTransportSegment& segment = callbackSegments.back();
        segment.frameCount += remainingFrames;
        segment.endBeat += segment.beatsPerSample * remainingFrames;
        currentBeat = segment.endBeat;
    }
    if (outBlockStartBeat) *outBlockStartBeat = firstBeat;
    if (outBlockEndBeat) *outBlockEndBeat = currentBeat;
    transportPlayheadBeat.store(currentBeat, std::memory_order_release);
    ReleaseMidiSchedulerBinding(bindingIndex);
}

std::vector<VeraCoreMidiEvent> VeraCoreAudio::BuildPlaybackMidiEvents(int frameCount, double* outBlockStartBeat, double* outBlockEndBeat)
{
    std::vector<VeraCoreMidiEvent> events;
    if (outBlockStartBeat) {
        *outBlockStartBeat = 0.0;
    }
    if (outBlockEndBeat) {
        *outBlockEndBeat = 0.0;
    }
    if (frameCount <= 0) {
        return events;
    }

    const int bindingIndex = AcquireMidiSchedulerBinding();
    if (bindingIndex < 0) return events;
    const MidiSchedulerBinding& binding = midiSchedulerBindings[static_cast<size_t>(bindingIndex)];
    const bool playing = transportPlaying.load(std::memory_order_acquire);
    const double playheadBeat = transportPlayheadBeat.load(std::memory_order_relaxed);
    const double bpm = transportBpm.load(std::memory_order_relaxed);
    if (!playing ||
        !std::isfinite(playheadBeat) ||
        !std::isfinite(bpm) ||
        !std::isfinite(streamSampleRate) ||
        bpm <= 0.0 ||
        streamSampleRate <= 0.0) {
        ReleaseMidiSchedulerBinding(bindingIndex);
        return events;
    }

    const double beatsPerSample = bpm / 60.0 / streamSampleRate;
    const double blockStartBeat = playheadBeat;
    const double blockEndBeat = blockStartBeat + beatsPerSample * frameCount;
    if (!std::isfinite(beatsPerSample) || !std::isfinite(blockEndBeat)) {
        ReleaseMidiSchedulerBinding(bindingIndex);
        return events;
    }
    if (outBlockStartBeat) {
        *outBlockStartBeat = blockStartBeat;
    }
    if (outBlockEndBeat) {
        *outBlockEndBeat = blockEndBeat;
    }
    const std::vector<VeraCoreBeatMidiEvent>& timeline = *binding.timelineEvents;
    const auto firstEvent = std::lower_bound(timeline.begin(), timeline.end(), blockStartBeat,
        [](const VeraCoreBeatMidiEvent& event, double beat) { return event.beat < beat; });
    for (auto eventIt = firstEvent; eventIt != timeline.end() && eventIt->beat < blockEndBeat; ++eventIt) {
        const VeraCoreBeatMidiEvent& event = *eventIt;
        const int sampleOffset = clampSampleOffset((event.beat - blockStartBeat) / beatsPerSample, frameCount);
        events.push_back({sampleOffset, event.status, event.data1, event.data2, event.layerId,
                          event.gain, event.pan, true, event.externalPlugin, -1.0,
                          event.isSlide, event.isPortamento,
                          glideSampleCount(event.glideDurationBeats, beatsPerSample),
                          event.isKeyswitch});
    }
    ReleaseMidiSchedulerBinding(bindingIndex);
    return events;
}

double VeraCoreAudio::GetTransportPlayheadBeat() const
{
    return transportPlayheadBeat.load(std::memory_order_acquire);
}

VeraCoreAudioMeterState VeraCoreAudio::GetAndResetMeterState()
{
    return {
        meterPeakLeft.exchange(0.0f, std::memory_order_acq_rel),
        meterPeakRight.exchange(0.0f, std::memory_order_acq_rel),
        meterMomentaryLufs.load(std::memory_order_acquire),
        meterShortTermLufs.load(std::memory_order_acquire),
        meterIntegratedLufs.load(std::memory_order_acquire),
        meterLoudnessRangeLu.load(std::memory_order_acquire)
    };
}

VeraCoreAudioMeterState VeraCoreAudio::GetAndResetReturnMeterState(int returnBus)
{
	if (returnBus < 0 || returnBus >= static_cast<int>(returnMeterPeakLeft.size())) return {};
	const size_t bus = static_cast<size_t>(returnBus);
	VeraCoreAudioMeterState state;
	state.peakLeft = returnMeterPeakLeft[bus].exchange(0.0f, std::memory_order_acq_rel);
	state.peakRight = returnMeterPeakRight[bus].exchange(0.0f, std::memory_order_acq_rel);
	return state;
}

void VeraCoreAudio::RequestLoudnessMeterReset() noexcept
{
    loudnessMeterResetRequested.store(true, std::memory_order_release);
}

void VeraCoreAudio::UpdateMeterState(const float* interleavedStereo, unsigned long framesCount)
{
    if (loudnessMeterResetRequested.exchange(false, std::memory_order_acq_rel)) {
        masterLoudnessMeter.ResetIntegrated();
        meterIntegratedLufs.store(
            -std::numeric_limits<float>::infinity(), std::memory_order_release);
        meterLoudnessRangeLu.store(
            -std::numeric_limits<float>::infinity(), std::memory_order_release);
    }
    const VeraCoreAudioMeterState measured =
        masterTruePeakMeter.ProcessBlock(interleavedStereo, framesCount);
    const VeraCoreAudioMeterState loudness =
        masterLoudnessMeter.ProcessBlock(interleavedStereo, framesCount);

    auto accumulatePeak = [](std::atomic<float>& destination, float peak) {
        float current = destination.load(std::memory_order_relaxed);
        while (peak > current && !destination.compare_exchange_weak(
                   current, peak, std::memory_order_release, std::memory_order_relaxed)) {
        }
    };
    accumulatePeak(meterPeakLeft, measured.peakLeft);
    accumulatePeak(meterPeakRight, measured.peakRight);
    meterMomentaryLufs.store(loudness.momentaryLufs, std::memory_order_release);
    meterShortTermLufs.store(loudness.shortTermLufs, std::memory_order_release);
    meterIntegratedLufs.store(loudness.integratedLufs, std::memory_order_release);
    meterLoudnessRangeLu.store(loudness.loudnessRangeLu, std::memory_order_release);
}

void VeraCoreAudio::RenderAudioTimelineBlock(float* out, unsigned long framesCount)
{
    if (!out || framesCount == 0) {
        return;
    }

    const bool playing = transportPlaying.load(std::memory_order_acquire);
    const double blockEndBeat = transportPlayheadBeat.load(std::memory_order_acquire);
    const double bpm = transportBpm.load(std::memory_order_relaxed);
    if (!playing || !std::isfinite(blockEndBeat) || !std::isfinite(bpm) ||
        !std::isfinite(streamSampleRate) || bpm <= 0.0 || streamSampleRate <= 0.0) return;
    const int frameCount = clampFrameCount(framesCount);
    const double beatsPerSample = bpm / 60.0 / streamSampleRate;
    const double blockStartBeat = blockEndBeat - beatsPerSample * frameCount;
    RenderAudioTimelineBlock(out, framesCount, blockStartBeat, blockEndBeat, -1.0);
}

void VeraCoreAudio::RenderAudioTimelineBlock(float* out, unsigned long framesCount,
                                              double blockStartBeat, double blockEndBeat,
                                              double blockStartSeconds, double projectBpm,
                                              double barStartBeat, int barNumber,
                                              int projectNumerator, int projectDenominator,
                                              bool cycleWrappedAtStart,
											  int rendererBindingIndex,
											  int renderBatchToken,
											  int timelineBindingIndex,
											  bool accumulateToCallbackMixer)
{
    if (!out || framesCount == 0) {
        return;
    }

    const int scratchFrames = static_cast<int>(audioTrackScratch.size() / 2u);
    if (scratchFrames > 0 && framesCount > static_cast<unsigned long>(scratchFrames)) {
        const double beatsPerFrame = (blockEndBeat - blockStartBeat) / static_cast<double>(framesCount);
        unsigned long offset = 0;
        while (offset < framesCount) {
            const unsigned long chunk = std::min<unsigned long>(scratchFrames, framesCount - offset);
            RenderAudioTimelineBlock(out + static_cast<size_t>(offset) * 2u, chunk,
                blockStartBeat + beatsPerFrame * offset,
                blockStartBeat + beatsPerFrame * (offset + chunk),
                std::isfinite(blockStartSeconds) && blockStartSeconds >= 0.0
                    ? blockStartSeconds + static_cast<double>(offset) / streamSampleRate
                    : -1.0, projectBpm, barStartBeat, barNumber,
				projectNumerator, projectDenominator,
				cycleWrappedAtStart && offset == 0, rendererBindingIndex,
				renderBatchToken, timelineBindingIndex, accumulateToCallbackMixer);
            offset += chunk;
        }
        return;
    }

	const int frameCount = clampFrameCount(framesCount);
	if (frameCount <= 0 || !std::isfinite(blockStartBeat) ||
		!std::isfinite(blockEndBeat) || blockEndBeat <= blockStartBeat) return;
	const bool ownsTimelineBinding = timelineBindingIndex < 0;
	if (ownsTimelineBinding && !transportPlaying.load(std::memory_order_acquire)) return;
	const double safeTempo = std::isfinite(projectBpm) && projectBpm > 0.0
		? projectBpm : transportBpm.load(std::memory_order_relaxed);
    if (!std::isfinite(safeTempo) || safeTempo <= 0.0) return;
    const double beatsPerSample = (blockEndBeat - blockStartBeat) / static_cast<double>(frameCount);
    if (!std::isfinite(beatsPerSample) || beatsPerSample <= 0.0) return;

	if (ownsTimelineBinding) timelineBindingIndex = AcquireAudioTimelineBinding();
	if (timelineBindingIndex < 0 ||
		timelineBindingIndex >= static_cast<int>(audioTimelineBindings.size())) return;
    const AudioTimelineBinding& timelineBinding =
        audioTimelineBindings[static_cast<size_t>(timelineBindingIndex)];
    const std::vector<VeraCoreAudioTimelineClip>& audioTimelineClips = timelineBinding.clips;
	const std::vector<double>& clipPrefixMaxEndBeats = timelineBinding.clipPrefixMaxEndBeats;
	if (audioTimelineClips.empty()) {
		if (ownsTimelineBinding) ReleaseAudioTimelineBinding(timelineBindingIndex);
        return;
    }

    auto mixClip = [&](const VeraCoreAudioTimelineClip& clip, float* target) {
        const std::vector<float>& samples = clip.sharedStereoSamples
            ? *clip.sharedStereoSamples
            : clip.stereoSamples;
        const size_t sourceFrames = samples.size() / 2u;
        if (sourceFrames == 0) {
            return;
        }
        const double naturalBeats = clip.sampleRate > 0 && safeTempo > 0.0
            ? (static_cast<double>(sourceFrames) / static_cast<double>(clip.sampleRate)) * safeTempo / 60.0
            : clip.lengthBeats;
        const double pitchRatio = pitchShiftRatio(clip.pitchShiftSemitones);
        const double fallbackWarpLength = clip.syncMode == 0
            ? naturalBeats / pitchRatio
            : (clip.syncMode == 2 && clip.targetBars > 0.0
                ? clip.targetBars * clip.beatsPerBar
                : (clip.loopEnabled
                    ? clip.loopStartBeats + clip.loopLengthBeats
                    : clip.sourceOffsetBeats + clip.lengthBeats));
        const double playbackLengthBeats = clip.warpLengthBeats > 0.0
            ? clip.warpLengthBeats
            : fallbackWarpLength;
        const double clipEndBeat = clip.startBeat + clip.lengthBeats;
        if (clipEndBeat <= blockStartBeat || clip.startBeat >= blockEndBeat) {
            return;
        }
        if (!std::isfinite(playbackLengthBeats) || playbackLengthBeats <= 0.0) {
            return;
        }
		const auto frameAtOrAfterBeat = [&](double boundaryBeat) noexcept {
			int first = 0;
			int last = frameCount;
			while (first < last) {
				const int middle = first + (last - first) / 2;
				const double beat = blockStartBeat + beatsPerSample * middle;
				if (beat < boundaryBeat) first = middle + 1;
				else last = middle;
			}
			return first;
		};
		const int firstFrame = frameAtOrAfterBeat(clip.startBeat);
		const int endFrame = frameAtOrAfterBeat(clipEndBeat);
		if (firstFrame >= endFrame) return;
		const PanGains gains = panGains(
			1.0, clip.pan, clip.monoSource,
			callbackPanLawDb);
        const double clipLevel = static_cast<double>(clip.gain) *
            static_cast<double>(clip.normalizationGain) * (clip.reversePolarity ? -1.0 : 1.0);
        const std::vector<VeraCoreAudioWarpPoint>* warpPoints = clip.sharedWarpPoints.get();
        const std::vector<VeraCoreAudioGainPoint>* gainPoints = clip.sharedGainEnvelope.get();
		const auto pointIndexAt = [](const auto& points, double position) noexcept {
			const auto next = std::upper_bound(
				points.begin(), points.end(), position,
				[](double target, const auto& point) { return target < point.localBeat; });
			return next == points.begin()
				? size_t{0}
				: static_cast<size_t>((next - points.begin()) - 1);
		};
		const double firstLocalBeat = std::max(
			0.0, blockStartBeat + beatsPerSample * firstFrame - clip.startBeat);
		const double firstSourceLocalBeat = clip.loopEnabled
			? std::fmod(firstLocalBeat, clip.loopLengthBeats)
			: firstLocalBeat;
		size_t warpPointIndex = warpPoints && !warpPoints->empty()
			? pointIndexAt(*warpPoints, firstSourceLocalBeat) : 0;
		size_t gainPointIndex = gainPoints && !gainPoints->empty()
			? pointIndexAt(*gainPoints, firstLocalBeat) : 0;
		for (int frame = firstFrame; frame < endFrame; ++frame) {
            const double beat = blockStartBeat + beatsPerSample * frame;
            const double localBeat = beat - clip.startBeat;
            const double sourceLocalBeat = clip.loopEnabled
                ? std::fmod(localBeat, clip.loopLengthBeats) : localBeat;
            const double remainingBeat = clip.lengthBeats - localBeat;
            double fadeEnvelope = 1.0;
            if (clip.fadeInBeats > 0.0 && localBeat < clip.fadeInBeats)
                fadeEnvelope *= shapedAudioFade(localBeat / clip.fadeInBeats, clip.fadeInCurve);
            if (clip.fadeOutBeats > 0.0 && remainingBeat < clip.fadeOutBeats)
                fadeEnvelope *= shapedAudioFade(remainingBeat / clip.fadeOutBeats, clip.fadeOutCurve);
            double gainEnvelope = 1.0;
            if (gainPoints && !gainPoints->empty()) {
                auto interpolateGain = [](double position, double leftBeat, float leftGain,
                                          double rightBeat, float rightGain) {
                    const double span = rightBeat - leftBeat;
                    const double amount = span > 0.0
                        ? std::clamp((position - leftBeat) / span, 0.0, 1.0) : 0.0;
                    return static_cast<double>(leftGain) +
                        (static_cast<double>(rightGain) - leftGain) * amount;
                };
                while (gainPointIndex + 1u < gainPoints->size() &&
                       (*gainPoints)[gainPointIndex + 1u].localBeat <= localBeat)
                    ++gainPointIndex;
                const VeraCoreAudioGainPoint& first = gainPoints->front();
                const VeraCoreAudioGainPoint& left = (*gainPoints)[gainPointIndex];
                if (localBeat <= first.localBeat) {
                    gainEnvelope = first.localBeat <= 0.0 ? first.gain :
                        interpolateGain(localBeat, 0.0, 1.0f, first.localBeat, first.gain);
                } else if (gainPointIndex + 1u < gainPoints->size()) {
                    const VeraCoreAudioGainPoint& right = (*gainPoints)[gainPointIndex + 1u];
                    gainEnvelope = interpolateGain(localBeat, left.localBeat, left.gain,
                                                    right.localBeat, right.gain);
                } else {
                    gainEnvelope = left.localBeat >= clip.lengthBeats ? left.gain :
                        interpolateGain(localBeat, left.localBeat, left.gain,
                                        clip.lengthBeats, 1.0f);
                }
            }
            double localSourceProgress = clip.lengthBeats > 0.0
                ? localBeat / clip.lengthBeats
                : 0.0;
            if (warpPoints && warpPoints->size() >= 2u) {
                if (sourceLocalBeat < (*warpPoints)[warpPointIndex].localBeat)
					warpPointIndex = pointIndexAt(*warpPoints, sourceLocalBeat);
                while (warpPointIndex + 1u < warpPoints->size() &&
                       (*warpPoints)[warpPointIndex + 1u].localBeat <= sourceLocalBeat) {
                    ++warpPointIndex;
                }
                const size_t nextWarpPoint = std::min(warpPointIndex + 1u, warpPoints->size() - 1u);
                const VeraCoreAudioWarpPoint& left = (*warpPoints)[warpPointIndex];
                const VeraCoreAudioWarpPoint& right = (*warpPoints)[nextWarpPoint];
                const double span = right.localBeat - left.localBeat;
                const double amount = span > 0.0
                    ? std::clamp((sourceLocalBeat - left.localBeat) / span, 0.0, 1.0)
                    : 0.0;
                localSourceProgress = left.sourceProgress +
                    (right.sourceProgress - left.sourceProgress) * amount;
            }
            double forwardProgress = 0.0;
            if (warpPoints && clip.warpPointsUseAbsoluteSourceProgress) {
                forwardProgress = localSourceProgress;
            } else if (clip.loopEnabled) {
                double phaseBeat = std::fmod(
                    clip.sourceOffsetBeats - clip.loopStartBeats + sourceLocalBeat,
                    clip.loopLengthBeats);
                if (phaseBeat < 0.0) phaseBeat += clip.loopLengthBeats;
                forwardProgress = (clip.loopStartBeats + phaseBeat) / playbackLengthBeats;
            } else {
                forwardProgress = (clip.sourceOffsetBeats +
                    localSourceProgress * clip.lengthBeats) / playbackLengthBeats;
            }
            const double sourceProgress = clip.reverse ? 1.0 - forwardProgress : forwardProgress;
            if (!std::isfinite(sourceProgress) || sourceProgress < 0.0 || sourceProgress > 1.0) {
                continue;
            }
            const double sourceFrame = sourceFrames > 1u
                ? sourceProgress * static_cast<double>(sourceFrames - 1u)
                : 0.0;
            if (!std::isfinite(sourceFrame) || sourceFrame < 0.0) {
                continue;
            }
            const size_t sourceIndex = static_cast<size_t>(std::floor(sourceFrame));
            if (sourceIndex >= sourceFrames) {
                continue;
            }
            const size_t nextIndex = std::min(sourceIndex + 1u, sourceFrames - 1u);
            const float fraction = static_cast<float>(sourceFrame - std::floor(sourceFrame));
            const size_t sourceBase = sourceIndex * 2u;
            const size_t nextBase = nextIndex * 2u;
            if (sourceBase + 1u >= samples.size() ||
                nextBase + 1u >= samples.size()) {
                continue;
            }
            const float leftA = samples[sourceBase];
            const float rightA = samples[sourceBase + 1u];
            const float leftB = samples[nextBase];
            const float rightB = samples[nextBase + 1u];
            const double sampleLeft = leftA + (leftB - leftA) * fraction;
            const double sampleRight = rightA + (rightB - rightA) * fraction;
            const size_t sampleIndex = static_cast<size_t>(frame) * 2;
            target[sampleIndex] = finiteSample(
                target[sampleIndex] + static_cast<float>(sampleLeft * gains.left * clipLevel * fadeEnvelope * gainEnvelope));
            target[sampleIndex + 1] = finiteSample(
                target[sampleIndex + 1] + static_cast<float>(sampleRight * gains.right * clipLevel * fadeEnvelope * gainEnvelope));
        }
    };

    for (const AudioTimelineBinding::TrackSpan& span : timelineBinding.trackSpans) {
        if (span.trackId < 0 || span.clipCount == 0 || span.firstClip >= audioTimelineClips.size()) {
            continue;
        }
        std::fill_n(audioTrackScratch.begin(), static_cast<size_t>(frameCount) * 2u, 0.0f);
        const size_t endClip = std::min(audioTimelineClips.size(), span.firstClip + span.clipCount);
		size_t firstCandidate = span.firstClip;
		if (clipPrefixMaxEndBeats.size() == audioTimelineClips.size()) {
			firstCandidate = static_cast<size_t>(std::upper_bound(
				clipPrefixMaxEndBeats.begin() + static_cast<std::ptrdiff_t>(span.firstClip),
				clipPrefixMaxEndBeats.begin() + static_cast<std::ptrdiff_t>(endClip),
				blockStartBeat) - clipPrefixMaxEndBeats.begin());
		}
		for (size_t clipIndex = firstCandidate; clipIndex < endClip; ++clipIndex) {
            const VeraCoreAudioTimelineClip& clip = audioTimelineClips[clipIndex];
            if (clip.startBeat >= blockEndBeat) break;
            if (clip.startBeat + clip.lengthBeats > blockStartBeat)
                mixClip(clip, audioTrackScratch.data());
        }
        sanitizeFiniteStereoBlock(audioTrackScratch.data(), framesCount);
		if (accumulateToCallbackMixer && span.trackId >= 0 &&
			span.trackId < static_cast<int>(callbackPluginMixerActive.size())) {
			AccumulateTimelineOutput(span.trackId, audioTrackScratch.data(), framesCount);
			continue;
		}
        RenderExternalAudioFxBlock(audioTrackScratch.data(), framesCount, span.trackId,
                                   blockStartBeat, blockStartSeconds, projectBpm,
                                   barStartBeat, barNumber,
                                   projectNumerator, projectDenominator,
								   cycleWrappedAtStart, rendererBindingIndex,
								   renderBatchToken);
        sanitizeFiniteStereoBlock(audioTrackScratch.data(), framesCount);
        MixTrackToOutput(out, audioTrackScratch.data(), framesCount, span.trackId);
    }
	if (ownsTimelineBinding) ReleaseAudioTimelineBinding(timelineBindingIndex);
}

void VeraCoreAudio::RenderMetronomeBlock(float* out, unsigned long framesCount, bool enabled,
                                         double blockStartBeat, double blockEndBeat, double bpm,
                                         double signatureStartBeat, int numerator, int denominator)
{
    if (!out || framesCount == 0 || !std::isfinite(blockStartBeat) ||
        !std::isfinite(blockEndBeat) || blockEndBeat <= blockStartBeat) {
        return;
    }

	if (!enabled || !std::isfinite(bpm) || bpm <= 0.0 ||
        !std::isfinite(signatureStartBeat) || numerator < 1 || denominator < 1) {
        return;
    }

    constexpr double twoPi = 6.2831853071795864769;
    constexpr double clickDurationSeconds = 0.035;
    const double pulseWidthBeats = 4.0 / static_cast<double>(denominator);
    const double beatsPerFrame = (blockEndBeat - blockStartBeat) / static_cast<double>(framesCount);
    for (unsigned long frame = 0; frame < framesCount; ++frame) {
        const double beat = blockStartBeat + beatsPerFrame * static_cast<double>(frame);
        const double pulsePosition = (beat - signatureStartBeat) / pulseWidthBeats;
        const double pulseIndexValue = std::floor(pulsePosition + 1.0e-9);
        const double elapsedBeats = (pulsePosition - pulseIndexValue) * pulseWidthBeats;
        const double elapsedSeconds = elapsedBeats * 60.0 / bpm;
        if (elapsedSeconds < 0.0 || elapsedSeconds >= clickDurationSeconds) {
            continue;
        }
        const long long pulseIndex = static_cast<long long>(pulseIndexValue);
        const int beatInBar = static_cast<int>((pulseIndex % numerator + numerator) % numerator);
        const double frequency = beatInBar == 0 ? 1760.0 : 1100.0;
        const double gain = (beatInBar == 0 ? 0.22 : 0.14) *
            std::exp(-elapsedSeconds * 75.0);
        const float click = static_cast<float>(std::sin(twoPi * frequency * elapsedSeconds) * gain);
        const size_t index = static_cast<size_t>(frame) * 2u;
        out[index] = finiteSample(finiteSample(out[index]) + click);
        out[index + 1u] = finiteSample(finiteSample(out[index + 1u]) + click);
    }
}
