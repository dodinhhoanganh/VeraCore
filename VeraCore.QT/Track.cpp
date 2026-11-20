#include "Track.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

float finiteClamp(float value, float low, float high, float fallback)
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

std::vector<PluginAutomationLane> normalizedAutomation(
    const std::vector<PluginAutomationLane>& source)
{
    std::vector<PluginAutomationLane> result;
    result.reserve(std::min(source.size(), kMaxPluginAutomationLanes));
    for (const PluginAutomationLane& sourceLane : source) {
        if (result.size() >= kMaxPluginAutomationLanes ||
            sourceLane.parameterId == std::numeric_limits<uint32_t>::max() ||
            std::any_of(result.begin(), result.end(), [&](const PluginAutomationLane& existing) {
                return existing.parameterId == sourceLane.parameterId;
            })) continue;
        PluginAutomationLane lane = sourceLane;
        lane.parameterName = lane.parameterName.left(256);
        lane.points.erase(std::remove_if(lane.points.begin(), lane.points.end(), [](const auto& point) {
            return !std::isfinite(point.beat) || point.beat < 0.0 ||
                !std::isfinite(point.normalizedValue);
        }), lane.points.end());
        if (lane.points.size() > kMaxPluginAutomationPointsPerLane)
            lane.points.resize(kMaxPluginAutomationPointsPerLane);
        std::stable_sort(lane.points.begin(), lane.points.end(), [](const auto& a, const auto& b) {
            return a.beat < b.beat;
        });
        for (PluginAutomationPoint& point : lane.points) {
            point.normalizedValue = std::clamp(point.normalizedValue, 0.0, 1.0);
            point.curveTension = std::isfinite(point.curveTension)
                ? std::clamp(point.curveTension, -1.0, 1.0) : 0.0;
            if (point.curvePolynomial &&
                !pluginAutomationCurvePolynomialValid(*point.curvePolynomial))
                point.curvePolynomial.reset();
        }
        std::vector<PluginAutomationPoint> unique;
        unique.reserve(lane.points.size());
        for (const PluginAutomationPoint& point : lane.points) {
            if (!unique.empty() && unique.back().beat == point.beat)
                unique.back() = point; // Last write at an exact beat wins.
            else
                unique.push_back(point);
        }
        lane.points = std::move(unique);
        // Keep an empty lane: interpolation/enabled state is authored metadata
        // and must survive before the first point is written.
        result.push_back(std::move(lane));
    }
    return result;
}

std::vector<PluginOutputRoute> normalizedOutputRoutes(
    const std::vector<PluginOutputRoute>& source)
{
    std::vector<PluginOutputRoute> result;
    result.reserve(std::min(source.size(), kMaxPluginOutputRoutes));
    for (const PluginOutputRoute& sourceRoute : source) {
        if (sourceRoute.outputBusIndex < 0 ||
            sourceRoute.outputBusIndex >= static_cast<int>(kMaxPluginOutputRoutes) ||
            sourceRoute.destinationTrackIndex < -1 ||
            sourceRoute.destinationTrackIndex >= kMaxMixerTrackCount) continue;
        PluginOutputRoute route = sourceRoute;
        route.stablePortId = route.stablePortId.trimmed().left(256);
        route.displayName = route.displayName.simplified().left(256);
        // A native output can feed only one mixer destination. Last write wins
        // for both its persistent port ID and legacy bus index, preventing a
        // malformed project from making realtime routing order-dependent.
        result.erase(std::remove_if(result.begin(), result.end(), [&](const auto& existing) {
            return existing.outputBusIndex == route.outputBusIndex ||
                (!route.stablePortId.isEmpty() &&
                 existing.stablePortId == route.stablePortId);
        }), result.end());
        if (result.size() < kMaxPluginOutputRoutes)
            result.push_back(std::move(route));
    }
    return result;
}

} // namespace

double pluginAutomationCurvePosition(double position, double curveTension) noexcept
{
    const double x = std::isfinite(position) ? std::clamp(position, 0.0, 1.0) : 0.0;
    const double tension = std::isfinite(curveTension)
        ? std::clamp(curveTension, -1.0, 1.0) : 0.0;
    const double smooth = x * x * (3.0 - 2.0 * x);
    if (tension > 1.0e-9) {
        const double easeIn = smooth * smooth;
        return smooth + (easeIn - smooth) * tension;
    }
    if (tension < -1.0e-9) {
        const double inverse = 1.0 - smooth;
        const double easeOut = 1.0 - inverse * inverse;
        return smooth + (easeOut - smooth) * -tension;
    }
    return smooth;
}

PluginAutomationCurvePolynomial pluginAutomationCurvePolynomial(
    double curveTension) noexcept
{
    const double tension = std::isfinite(curveTension)
        ? std::clamp(curveTension, -1.0, 1.0) : 0.0;
    PluginAutomationCurvePolynomial polynomial{};
    polynomial[2] = 3.0 * (1.0 - tension);
    polynomial[3] = -2.0 * (1.0 - tension);
    polynomial[4] = 9.0 * tension;
    polynomial[5] = -12.0 * tension;
    polynomial[6] = 4.0 * tension;
    return polynomial;
}

namespace {

constexpr int kBinomial[7][7] = {
    {1, 0, 0, 0, 0, 0, 0},
    {1, 1, 0, 0, 0, 0, 0},
    {1, 2, 1, 0, 0, 0, 0},
    {1, 3, 3, 1, 0, 0, 0},
    {1, 4, 6, 4, 1, 0, 0},
    {1, 5, 10, 10, 5, 1, 0},
    {1, 6, 15, 20, 15, 6, 1}
};

double evaluateCurvePolynomial(const PluginAutomationCurvePolynomial& polynomial,
                               double position) noexcept
{
    double value = polynomial.back();
    for (size_t index = polynomial.size() - 1; index > 0; --index)
        value = value * position + polynomial[index - 1];
    return value;
}

} // namespace

double pluginAutomationCurvePosition(double position,
                                     const PluginAutomationPoint& point) noexcept
{
    const double x = std::isfinite(position) ? std::clamp(position, 0.0, 1.0) : 0.0;
    if (!point.curvePolynomial)
        return pluginAutomationCurvePosition(x, point.curveTension);
    return std::clamp(evaluateCurvePolynomial(*point.curvePolynomial, x), 0.0, 1.0);
}

bool pluginAutomationCurvePolynomialValid(
    const PluginAutomationCurvePolynomial& polynomial) noexcept
{
    for (double coefficient : polynomial)
        if (!std::isfinite(coefficient) || std::fabs(coefficient) > 4096.0) return false;
    constexpr double kEndpointTolerance = 1.0e-7;
    if (std::fabs(evaluateCurvePolynomial(polynomial, 0.0)) > kEndpointTolerance ||
        std::fabs(evaluateCurvePolynomial(polynomial, 1.0) - 1.0) > kEndpointTolerance)
        return false;
    // A polynomial is a convex combination of its Bernstein coefficients.
    // Requiring every degree-5 Bernstein coefficient of P' to be non-negative
    // proves monotonicity over the whole interval, not merely at sample points.
    double derivativePower[6]{};
    for (size_t degree = 0; degree < 6; ++degree)
        derivativePower[degree] = static_cast<double>(degree + 1) * polynomial[degree + 1];
    static constexpr int degreeFiveBinomial[6] = {1, 5, 10, 10, 5, 1};
    for (size_t bernsteinDegree = 0; bernsteinDegree < 6; ++bernsteinDegree) {
        double coefficient = 0.0;
        for (size_t power = 0; power <= bernsteinDegree; ++power) {
            coefficient += static_cast<double>(kBinomial[bernsteinDegree][power]) /
                static_cast<double>(degreeFiveBinomial[power]) * derivativePower[power];
        }
        if (coefficient < -1.0e-8 || !std::isfinite(coefficient)) return false;
    }
    return true;
}

PluginAutomationCurvePolynomial pluginAutomationRestrictedCurve(
    const PluginAutomationCurvePolynomial& polynomial,
    double startPosition) noexcept
{
    const double start = std::isfinite(startPosition)
        ? std::clamp(startPosition, 0.0, 1.0) : 0.0;
    const double remainder = 1.0 - start;
    PluginAutomationCurvePolynomial expanded{};
    double startPowers[7]{1.0};
    double remainderPowers[7]{1.0};
    for (size_t power = 1; power < 7; ++power) {
        startPowers[power] = startPowers[power - 1] * start;
        remainderPowers[power] = remainderPowers[power - 1] * remainder;
    }
    for (size_t degree = 0; degree < 7; ++degree) {
        for (size_t outputDegree = 0; outputDegree <= degree; ++outputDegree) {
            expanded[outputDegree] += polynomial[degree] *
                static_cast<double>(kBinomial[degree][outputDegree]) *
                startPowers[degree - outputDegree] * remainderPowers[outputDegree];
        }
    }
    const double denominator = evaluateCurvePolynomial(polynomial, 1.0) - expanded[0];
    if (!(denominator > 1.0e-12) || !std::isfinite(denominator)) return {};
    PluginAutomationCurvePolynomial restricted{};
    for (size_t degree = 1; degree < 7; ++degree)
        restricted[degree] = expanded[degree] / denominator;
    return pluginAutomationCurvePolynomialValid(restricted)
        ? restricted : PluginAutomationCurvePolynomial{};
}

Track::Track(const QString& name, TrackType type)
    : m_name(name)
    , m_type(type)
    , m_volume(1.0f)
    , m_pan(0.0f)
    , m_stereoSeparation(0.0f)
    , m_isMuted(false)
    , m_isSoloed(false)
    , m_isArmed(false)
    , m_isMonitoring(false)
{
}

QString Track::GetName() const { return m_name; }
TrackType Track::GetType() const { return m_type; }
void Track::SetName(const QString& name) { m_name = name; }

float Track::GetVolume() const { return m_volume; }
void Track::SetVolume(float volume)
{
    m_volume = finiteClamp(volume, 0.0f, 2.0f, 1.0f);
}

float Track::GetPan() const { return m_pan; }
void Track::SetPan(float pan)
{
    m_pan = finiteClamp(pan, -1.0f, 1.0f, 0.0f);
    if (m_spatialPanner.linkStereoPan)
        m_spatialPanner.azimuthDegrees = m_pan * 30.0f;
}

float Track::GetStereoSeparation() const { return m_stereoSeparation; }
void Track::SetStereoSeparation(float separation)
{
    m_stereoSeparation = finiteClamp(separation, -1.0f, 1.0f, 0.0f);
}

const VeraCoreSpatialPannerState& Track::GetSpatialPanner() const noexcept
{
    return m_spatialPanner;
}

void Track::SetSpatialPanner(const VeraCoreSpatialPannerState& state) noexcept
{
    m_spatialPanner = VeraCoreNormalizeSpatialPanner(state);
    if (m_spatialPanner.linkStereoPan)
        m_pan = std::clamp(m_spatialPanner.azimuthDegrees / 30.0f, -1.0f, 1.0f);
}

bool Track::IsMuted() const { return m_isMuted; }
void Track::SetMuted(bool muted) { m_isMuted = muted; }

bool Track::IsSoloed() const { return m_isSoloed; }
void Track::SetSoloed(bool soloed) { m_isSoloed = soloed; }

bool Track::IsArmed() const { return m_isArmed; }
void Track::SetArmed(bool armed) { m_isArmed = armed; }

bool Track::IsMonitoring() const { return m_isMonitoring; }
void Track::SetMonitoring(bool monitoring) { m_isMonitoring = monitoring; }

float Track::GetSendLevel(int returnBus) const
{
    return returnBus >= 0 && returnBus < kReturnBusCount
        ? m_sendLevels[static_cast<std::size_t>(returnBus)] : 0.0f;
}

void Track::SetSendLevel(int returnBus, float level)
{
    if (returnBus < 0 || returnBus >= kReturnBusCount) return;
    m_sendLevels[static_cast<std::size_t>(returnBus)] = finiteClamp(level, 0.0f, 1.0f, 0.0f);
}

const std::vector<AudioFxSlot>& Track::GetAudioFxSlots() const { return m_audioFxSlots; }
void Track::SetAudioFxSlots(const std::vector<AudioFxSlot>& audioFxSlots)
{
    m_audioFxSlots = audioFxSlots;
    for (AudioFxSlot& slot : m_audioFxSlots)
        slot.automation = normalizedAutomation(slot.automation);
}
void Track::AddAudioFx(const AudioFxSlot& slot)
{
    AudioFxSlot normalized = slot;
    normalized.automation = normalizedAutomation(slot.automation);
    m_audioFxSlots.push_back(std::move(normalized));
}
void Track::SetAudioFxEnabled(int index, bool enabled)
{
    if (index >= 0 && index < static_cast<int>(m_audioFxSlots.size())) m_audioFxSlots[index].enabled = enabled;
}
void Track::SetAudioFxPluginState(int index, const QByteArray& state)
{
    if (index >= 0 && index < static_cast<int>(m_audioFxSlots.size())) m_audioFxSlots[index].pluginState = state;
}
void Track::SetAudioFxAutomation(int index, const std::vector<PluginAutomationLane>& automation)
{
    if (index >= 0 && index < static_cast<int>(m_audioFxSlots.size()))
        m_audioFxSlots[index].automation = normalizedAutomation(automation);
}
void Track::RemoveAudioFx(int index)
{
    if (index >= 0 && index < static_cast<int>(m_audioFxSlots.size()))
        m_audioFxSlots.erase(m_audioFxSlots.begin() + index);
}


// AudioTrack
AudioTrack::AudioTrack(const QString& name)
    : Track(name, TrackType::Audio)
{
}

// MidiTrack
MidiTrack::MidiTrack(const QString& name)
    : Track(name, TrackType::Midi)
{
    LayerSlot layer;
    layer.name = "Internal Synth";
    layer.pluginType = "Internal";
    m_layerSlots.push_back(layer);
    m_instrumentSlot = layer.name;
}

QString MidiTrack::GetInstrument() const
{
    if (!m_layerSlots.empty()) {
        return m_layerSlots.front().name;
    }
    return m_instrumentSlot;
}

void MidiTrack::SetInstrument(const QString& instrumentName)
{
    m_instrumentSlot = instrumentName;
    if (m_layerSlots.empty()) {
        LayerSlot layer;
        layer.name = instrumentName.isEmpty() ? QString("Internal Synth") : instrumentName;
        layer.pluginType = "Internal";
        m_layerSlots.push_back(layer);
    } else {
        m_layerSlots.front().name = instrumentName;
    }
}

const std::vector<LayerSlot>& MidiTrack::GetLayerSlots() const
{
    return m_layerSlots;
}

void MidiTrack::SetLayerSlots(const std::vector<LayerSlot>& layers)
{
    m_layerSlots.clear();
    m_instrumentSlot.clear();
    for (const LayerSlot& layer : layers) AddLayerSlot(layer);
}

void MidiTrack::AddLayerSlot(const LayerSlot& layer)
{
    LayerSlot normalized = layer;
    if (normalized.name.isEmpty()) {
        normalized.name = "Layer";
    }
    if (normalized.pluginType.isEmpty()) {
        normalized.pluginType = "Internal";
    }
    normalized.gain = finiteClamp(normalized.gain, 0.0f, 2.0f, 1.0f);
    normalized.pan = finiteClamp(normalized.pan, -1.0f, 1.0f, 0.0f);
    normalized.transpose = std::clamp(normalized.transpose, -48, 48);
    normalized.midiChannel = std::clamp(normalized.midiChannel, -1, 15);
    normalized.outputRoutes = normalizedOutputRoutes(normalized.outputRoutes);
    normalized.automation = normalizedAutomation(normalized.automation);
    m_layerSlots.push_back(normalized);
    if (m_instrumentSlot.isEmpty()) {
        m_instrumentSlot = normalized.name;
    }
}

void MidiTrack::SetLayerSlotEnabled(int index, bool enabled)
{
    if (index < 0 || index >= static_cast<int>(m_layerSlots.size())) {
        return;
    }
    m_layerSlots[index].enabled = enabled;
}

void MidiTrack::SetLayerSlotParameters(int index, float gain, float pan, int transpose, int midiChannel)
{
    if (index < 0 || index >= static_cast<int>(m_layerSlots.size())) {
        return;
    }
    LayerSlot& layer = m_layerSlots[index];
    layer.gain = finiteClamp(gain, 0.0f, 2.0f, 1.0f);
    layer.pan = finiteClamp(pan, -1.0f, 1.0f, 0.0f);
    layer.transpose = std::clamp(transpose, -48, 48);
    layer.midiChannel = std::clamp(midiChannel, -1, 15);
}

void MidiTrack::SetLayerSlotPluginState(int index, const QByteArray& state)
{
    if (index < 0 || index >= static_cast<int>(m_layerSlots.size())) {
        return;
    }
    m_layerSlots[index].pluginState = state;
}

void MidiTrack::SetLayerSlotAutomation(
    int index, const std::vector<PluginAutomationLane>& automation)
{
    if (index >= 0 && index < static_cast<int>(m_layerSlots.size()))
        m_layerSlots[index].automation = normalizedAutomation(automation);
}

void MidiTrack::SetLayerSlotOutputRoutes(int index, const std::vector<PluginOutputRoute>& routes)
{
    if (index < 0 || index >= static_cast<int>(m_layerSlots.size())) return;
    m_layerSlots[static_cast<std::size_t>(index)].outputRoutes =
        normalizedOutputRoutes(routes);
}

void MidiTrack::RemoveLayerSlot(int index)
{
    if (index < 0 || index >= static_cast<int>(m_layerSlots.size())) {
        return;
    }
    m_layerSlots.erase(m_layerSlots.begin() + index);
    m_instrumentSlot = m_layerSlots.empty() ? QString() : m_layerSlots.front().name;
}

void MidiTrack::ClearLayerSlots()
{
    m_layerSlots.clear();
    m_instrumentSlot.clear();
}
