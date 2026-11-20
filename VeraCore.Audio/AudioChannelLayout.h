#pragma once

#include <cstdint>

inline constexpr std::uint32_t kVeraCoreWaveMaskMono = 0x00000004u;
inline constexpr std::uint32_t kVeraCoreWaveMaskStereo = 0x00000003u;
inline constexpr std::uint32_t kVeraCoreWaveMaskSurround51 = 0x0000060fu;
inline constexpr std::uint32_t kVeraCoreWaveMaskSurround71 = 0x0000063fu;
inline constexpr std::uint32_t kVeraCoreWaveMaskAtmos712 = 0x0000563fu;
inline constexpr std::uint32_t kVeraCoreWaveMaskAtmos714 = 0x0002d63fu;

constexpr int VeraCoreSpeakerCountFromMask(std::uint64_t mask) noexcept
{
    int count = 0;
    while (mask != 0) {
        count += static_cast<int>(mask & 1u);
        mask >>= 1u;
    }
    return count;
}

constexpr std::uint32_t VeraCoreCanonicalWaveSpeakerMask(int channels) noexcept
{
    switch (channels) {
    case 1: return kVeraCoreWaveMaskMono;
    case 2: return kVeraCoreWaveMaskStereo;
    case 6: return kVeraCoreWaveMaskSurround51;
    case 8: return kVeraCoreWaveMaskSurround71;
    case 10: return kVeraCoreWaveMaskAtmos712;
    case 12: return kVeraCoreWaveMaskAtmos714;
    default: return 0;
    }
}

constexpr bool VeraCoreIsWaveSpeakerMaskValid(
    std::uint64_t mask, int channels) noexcept
{
    return channels > 0 && mask != 0 && mask <= 0xffffffffull &&
        VeraCoreSpeakerCountFromMask(mask) == channels;
}

static_assert(VeraCoreSpeakerCountFromMask(kVeraCoreWaveMaskMono) == 1);
static_assert(VeraCoreSpeakerCountFromMask(kVeraCoreWaveMaskStereo) == 2);
static_assert(VeraCoreSpeakerCountFromMask(kVeraCoreWaveMaskSurround51) == 6);
static_assert(VeraCoreSpeakerCountFromMask(kVeraCoreWaveMaskSurround71) == 8);
static_assert(VeraCoreSpeakerCountFromMask(kVeraCoreWaveMaskAtmos712) == 10);
static_assert(VeraCoreSpeakerCountFromMask(kVeraCoreWaveMaskAtmos714) == 12);
