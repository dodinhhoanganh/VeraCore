#pragma once

#include <array>
#include <cstdint>

inline constexpr int kVeraCoreMaxSpatialChannels = 12;

// Canonical internal order follows WAVEFORMATEXTENSIBLE beds:
// FL, FR, FC, LFE, BL, BR, SL, SR, TFL, TFR, TBL, TBR.
enum class VeraCoreSpeakerLayout : std::uint8_t {
    Stereo = 0,
    Surround51,
    Surround71,
    Atmos712,
    Atmos714
};

enum class VeraCoreSpatialRenderMode : std::uint8_t {
    StereoPan = 0,
    Bed,
    Object
};

struct VeraCoreSpatialPannerState {
    VeraCoreSpatialRenderMode mode = VeraCoreSpatialRenderMode::StereoPan;
    float azimuthDegrees = 0.0f;
    float elevationDegrees = 0.0f;
    float distance = 0.0f;
    float spread = 0.0f;
    float lfeSend = 0.0f;
    bool linkStereoPan = true;
};

struct VeraCoreSpatialGains {
    std::array<float, kVeraCoreMaxSpatialChannels> channels{};
    int channelCount = 2;
};

struct VeraCoreOutputLayoutCandidate {
    VeraCoreSpeakerLayout layout = VeraCoreSpeakerLayout::Stereo;
    int channelCount = 2;
};

struct VeraCoreOutputLayoutPlan {
    std::array<VeraCoreOutputLayoutCandidate, 5> candidates{};
    int candidateCount = 0;
};

int VeraCoreSpeakerChannelCount(VeraCoreSpeakerLayout layout) noexcept;
std::uint32_t VeraCoreWaveSpeakerMask(VeraCoreSpeakerLayout layout) noexcept;
VeraCoreOutputLayoutPlan VeraCoreBuildOutputLayoutPlan(
    VeraCoreSpeakerLayout requestedLayout, int maximumDeviceChannels) noexcept;
VeraCoreSpatialPannerState VeraCoreNormalizeSpatialPanner(
    const VeraCoreSpatialPannerState& state) noexcept;
VeraCoreSpatialGains VeraCoreCalculateSpatialGains(
    VeraCoreSpeakerLayout layout,
    const VeraCoreSpatialPannerState& state) noexcept;
