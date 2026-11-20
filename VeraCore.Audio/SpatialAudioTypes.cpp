#include "SpatialAudioTypes.h"
#include "AudioChannelLayout.h"

#include <algorithm>
#include <cmath>

namespace
{
struct SpatialSpeakerPosition {
    float azimuth;
    float elevation;
    bool lfe;
};

constexpr float kSpatialPi = 3.14159265358979323846f;

std::array<SpatialSpeakerPosition, kVeraCoreMaxSpatialChannels> spatialSpeakers(
    VeraCoreSpeakerLayout layout) noexcept
{
    std::array<SpatialSpeakerPosition, kVeraCoreMaxSpatialChannels> result{};
    result[0] = {-30.0f, 0.0f, false};
    result[1] = {30.0f, 0.0f, false};
    if (layout == VeraCoreSpeakerLayout::Stereo) return result;
    result[2] = {0.0f, 0.0f, false};
    result[3] = {0.0f, 0.0f, true};
    if (layout == VeraCoreSpeakerLayout::Surround51) {
        result[4] = {-110.0f, 0.0f, false};
        result[5] = {110.0f, 0.0f, false};
        return result;
    }
    result[4] = {-150.0f, 0.0f, false};
    result[5] = {150.0f, 0.0f, false};
    result[6] = {-90.0f, 0.0f, false};
    result[7] = {90.0f, 0.0f, false};
    if (layout == VeraCoreSpeakerLayout::Surround71) return result;
    result[8] = {-45.0f, 45.0f, false};
    result[9] = {45.0f, 45.0f, false};
    if (layout == VeraCoreSpeakerLayout::Atmos712) return result;
    result[10] = {-135.0f, 45.0f, false};
    result[11] = {135.0f, 45.0f, false};
    return result;
}

std::array<float, 3> spatialUnitVector(float azimuth, float elevation) noexcept
{
    const float az = azimuth * kSpatialPi / 180.0f;
    const float el = elevation * kSpatialPi / 180.0f;
    const float cosElevation = std::cos(el);
    return {std::sin(az) * cosElevation, std::cos(az) * cosElevation, std::sin(el)};
}
}

int VeraCoreSpeakerChannelCount(VeraCoreSpeakerLayout layout) noexcept
{
    switch (layout) {
    case VeraCoreSpeakerLayout::Surround51: return 6;
    case VeraCoreSpeakerLayout::Surround71: return 8;
    case VeraCoreSpeakerLayout::Atmos712: return 10;
    case VeraCoreSpeakerLayout::Atmos714: return 12;
    default: return 2;
    }
}

std::uint32_t VeraCoreWaveSpeakerMask(VeraCoreSpeakerLayout layout) noexcept
{
    return VeraCoreCanonicalWaveSpeakerMask(VeraCoreSpeakerChannelCount(layout));
}

VeraCoreOutputLayoutPlan VeraCoreBuildOutputLayoutPlan(
    VeraCoreSpeakerLayout requestedLayout, int maximumDeviceChannels) noexcept
{
    VeraCoreOutputLayoutPlan plan;
    if (maximumDeviceChannels < 2) return plan;
    constexpr std::array<VeraCoreSpeakerLayout, 5> descending{{
        VeraCoreSpeakerLayout::Atmos714,
        VeraCoreSpeakerLayout::Atmos712,
        VeraCoreSpeakerLayout::Surround71,
        VeraCoreSpeakerLayout::Surround51,
        VeraCoreSpeakerLayout::Stereo
    }};
    int requestedIndex = static_cast<int>(descending.size()) - 1;
    for (int index = 0; index < static_cast<int>(descending.size()); ++index) {
        if (descending[static_cast<std::size_t>(index)] == requestedLayout) {
            requestedIndex = index;
            break;
        }
    }
    for (int index = requestedIndex; index < static_cast<int>(descending.size()); ++index) {
        const VeraCoreSpeakerLayout layout = descending[static_cast<std::size_t>(index)];
        const int channels = VeraCoreSpeakerChannelCount(layout);
        if (channels > maximumDeviceChannels) continue;
        plan.candidates[static_cast<std::size_t>(plan.candidateCount++)] = {layout, channels};
    }
    return plan;
}

VeraCoreSpatialPannerState VeraCoreNormalizeSpatialPanner(
    const VeraCoreSpatialPannerState& state) noexcept
{
    VeraCoreSpatialPannerState result = state;
    result.azimuthDegrees = std::isfinite(result.azimuthDegrees)
        ? std::clamp(result.azimuthDegrees, -180.0f, 180.0f) : 0.0f;
    result.elevationDegrees = std::isfinite(result.elevationDegrees)
        ? std::clamp(result.elevationDegrees, -90.0f, 90.0f) : 0.0f;
    result.distance = std::isfinite(result.distance)
        ? std::clamp(result.distance, 0.0f, 1.0f) : 0.0f;
    result.spread = std::isfinite(result.spread)
        ? std::clamp(result.spread, 0.0f, 1.0f) : 0.0f;
    result.lfeSend = std::isfinite(result.lfeSend)
        ? std::clamp(result.lfeSend, 0.0f, 1.0f) : 0.0f;
    return result;
}

VeraCoreSpatialGains VeraCoreCalculateSpatialGains(
    VeraCoreSpeakerLayout layout, const VeraCoreSpatialPannerState& requested) noexcept
{
    const VeraCoreSpatialPannerState state = VeraCoreNormalizeSpatialPanner(requested);
    VeraCoreSpatialGains result;
    result.channelCount = VeraCoreSpeakerChannelCount(layout);
    const float distanceGain = 1.0f / (1.0f + 3.0f * state.distance * state.distance);
    if (layout == VeraCoreSpeakerLayout::Stereo) {
        const float pan = std::clamp(state.azimuthDegrees / 90.0f, -1.0f, 1.0f);
        const float angle = (pan + 1.0f) * kSpatialPi * 0.25f;
        result.channels[0] = std::cos(angle) * distanceGain;
        result.channels[1] = std::sin(angle) * distanceGain;
        return result;
    }

    const auto speakers = spatialSpeakers(layout);
    const auto source = spatialUnitVector(state.azimuthDegrees, state.elevationDegrees);
    const float exponent = 12.0f - state.spread * 10.0f;
    float energy = 0.0f;
    int nonLfeCount = 0;
    for (int channel = 0; channel < result.channelCount; ++channel) {
        if (speakers[static_cast<std::size_t>(channel)].lfe) continue;
        const auto speaker = spatialUnitVector(
            speakers[static_cast<std::size_t>(channel)].azimuth,
            speakers[static_cast<std::size_t>(channel)].elevation);
        const float dot = std::max(0.0f, source[0] * speaker[0] +
            source[1] * speaker[1] + source[2] * speaker[2]);
        const float focused = std::pow(dot, exponent);
        const float diffuse = state.spread * 0.15f;
        const float weight = focused + diffuse;
        result.channels[static_cast<std::size_t>(channel)] = weight;
        energy += weight * weight;
        ++nonLfeCount;
    }
    if (!(energy > 1.0e-12f) && nonLfeCount > 0) {
        const float uniform = 1.0f / std::sqrt(static_cast<float>(nonLfeCount));
        for (int channel = 0; channel < result.channelCount; ++channel)
            if (!speakers[static_cast<std::size_t>(channel)].lfe)
                result.channels[static_cast<std::size_t>(channel)] = uniform * distanceGain;
    } else {
        const float scale = distanceGain / std::sqrt(std::max(energy, 1.0e-12f));
        for (int channel = 0; channel < result.channelCount; ++channel)
            if (!speakers[static_cast<std::size_t>(channel)].lfe)
                result.channels[static_cast<std::size_t>(channel)] *= scale;
    }
    result.channels[3] = state.lfeSend * distanceGain;
    return result;
}
