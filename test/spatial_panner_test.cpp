#include "../VeraCore.Audio/SpatialAudioTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>

namespace
{
bool near(float actual, float expected, float tolerance = 1.0e-4f)
{
    return std::abs(actual - expected) <= tolerance;
}

float bedEnergy(const VeraCoreSpatialGains& gains)
{
    float energy = 0.0f;
    for (int channel = 0; channel < gains.channelCount; ++channel) {
        if (channel != 3) energy += gains.channels[static_cast<std::size_t>(channel)] *
            gains.channels[static_cast<std::size_t>(channel)];
    }
    return energy;
}

bool finiteAndBounded(const VeraCoreSpatialGains& gains)
{
    if (gains.channelCount < 2 || gains.channelCount > kVeraCoreMaxSpatialChannels) return false;
    for (int channel = 0; channel < kVeraCoreMaxSpatialChannels; ++channel) {
        const float value = gains.channels[static_cast<std::size_t>(channel)];
        if (!std::isfinite(value) || value < 0.0f || value > 1.0f) return false;
        if (channel >= gains.channelCount && value != 0.0f) return false;
    }
    return true;
}
}

int main()
{
    struct LayoutVector { VeraCoreSpeakerLayout layout; int channels; std::uint32_t mask; };
    constexpr std::array<LayoutVector, 5> layouts{{
        {VeraCoreSpeakerLayout::Stereo, 2, 0x00000003u},
        {VeraCoreSpeakerLayout::Surround51, 6, 0x0000060fu},
        {VeraCoreSpeakerLayout::Surround71, 8, 0x0000063fu},
        {VeraCoreSpeakerLayout::Atmos712, 10, 0x0000563fu},
        {VeraCoreSpeakerLayout::Atmos714, 12, 0x0002d63fu}
    }};
    for (const LayoutVector& vector : layouts) {
        if (VeraCoreSpeakerChannelCount(vector.layout) != vector.channels ||
            VeraCoreWaveSpeakerMask(vector.layout) != vector.mask) return 1;
    }

    VeraCoreSpatialPannerState invalid;
    invalid.azimuthDegrees = std::numeric_limits<float>::quiet_NaN();
    invalid.elevationDegrees = 120.0f;
    invalid.distance = std::numeric_limits<float>::infinity();
    invalid.spread = -2.0f;
    invalid.lfeSend = 3.0f;
    const VeraCoreSpatialPannerState normalized = VeraCoreNormalizeSpatialPanner(invalid);
    if (normalized.azimuthDegrees != 0.0f || normalized.elevationDegrees != 90.0f ||
        normalized.distance != 0.0f || normalized.spread != 0.0f || normalized.lfeSend != 1.0f) return 2;

    VeraCoreSpatialPannerState state;
    state.azimuthDegrees = -90.0f;
    const auto stereoLeft = VeraCoreCalculateSpatialGains(VeraCoreSpeakerLayout::Stereo, state);
    state.azimuthDegrees = 0.0f;
    const auto stereoCenter = VeraCoreCalculateSpatialGains(VeraCoreSpeakerLayout::Stereo, state);
    state.azimuthDegrees = 90.0f;
    const auto stereoRight = VeraCoreCalculateSpatialGains(VeraCoreSpeakerLayout::Stereo, state);
    if (!near(stereoLeft.channels[0], 1.0f) || !near(stereoLeft.channels[1], 0.0f) ||
        !near(stereoCenter.channels[0], std::sqrt(0.5f)) ||
        !near(stereoCenter.channels[1], std::sqrt(0.5f)) ||
        !near(stereoRight.channels[0], 0.0f) || !near(stereoRight.channels[1], 1.0f)) return 3;

    state = {};
    state.azimuthDegrees = -90.0f;
    const auto surroundLeft = VeraCoreCalculateSpatialGains(VeraCoreSpeakerLayout::Surround71, state);
    state.azimuthDegrees = 90.0f;
    const auto surroundRight = VeraCoreCalculateSpatialGains(VeraCoreSpeakerLayout::Surround71, state);
    if (!(surroundLeft.channels[6] > surroundLeft.channels[7]) ||
        !(surroundRight.channels[7] > surroundRight.channels[6]) ||
        !near(surroundLeft.channels[0], surroundRight.channels[1]) ||
        !near(surroundLeft.channels[4], surroundRight.channels[5]) ||
        !near(surroundLeft.channels[6], surroundRight.channels[7])) return 4;

    state = {};
    state.azimuthDegrees = 25.0f;
    state.distance = 0.5f;
    const auto distant = VeraCoreCalculateSpatialGains(VeraCoreSpeakerLayout::Surround71, state);
    const float distanceGain = 1.0f / 1.75f;
    if (!near(bedEnergy(distant), distanceGain * distanceGain) || !finiteAndBounded(distant)) return 5;

    state.lfeSend = 0.5f;
    const auto withLfe = VeraCoreCalculateSpatialGains(VeraCoreSpeakerLayout::Surround71, state);
    if (!near(withLfe.channels[3], 0.5f * distanceGain) ||
        !near(bedEnergy(withLfe), distanceGain * distanceGain)) return 6;

    state = {};
    state.azimuthDegrees = -45.0f;
    state.elevationDegrees = 45.0f;
    const auto elevated = VeraCoreCalculateSpatialGains(VeraCoreSpeakerLayout::Atmos714, state);
    if (!(elevated.channels[8] > elevated.channels[0]) ||
        !(elevated.channels[8] > elevated.channels[9]) || !near(bedEnergy(elevated), 1.0f) ||
        !finiteAndBounded(elevated)) return 7;

    state.spread = 1.0f;
    const auto diffuse = VeraCoreCalculateSpatialGains(VeraCoreSpeakerLayout::Atmos714, state);
    int elevatedActive = 0;
    int diffuseActive = 0;
    for (int channel = 0; channel < elevated.channelCount; ++channel) {
        if (channel == 3) continue;
        elevatedActive += elevated.channels[static_cast<std::size_t>(channel)] > 1.0e-3f;
        diffuseActive += diffuse.channels[static_cast<std::size_t>(channel)] > 1.0e-3f;
    }
    if (!(diffuseActive > elevatedActive) || !near(bedEnergy(diffuse), 1.0f) ||
        !finiteAndBounded(diffuse)) return 8;

    std::cout << "SPATIAL_PANNER_TEST=PASS VECTORS=14 LAYOUTS=5 STEREO=3 SURROUND=3 ATMOS=2 NORMALIZATION=1\n";
    return 0;
}
