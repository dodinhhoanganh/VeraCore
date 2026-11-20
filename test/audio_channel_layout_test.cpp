#include "../VeraCore.Audio/AudioChannelLayout.h"

#include <array>
#include <cstdint>
#include <iostream>

int main()
{
    struct Vector {
        int channels;
        std::uint32_t mask;
    };
    constexpr std::array<Vector, 6> canonical{{
        {1, 0x00000004u}, {2, 0x00000003u}, {6, 0x0000060fu},
        {8, 0x0000063fu}, {10, 0x0000563fu}, {12, 0x0002d63fu}
    }};
    for (const Vector& vector : canonical) {
        if (VeraCoreCanonicalWaveSpeakerMask(vector.channels) != vector.mask ||
            !VeraCoreIsWaveSpeakerMaskValid(vector.mask, vector.channels)) {
            return 1;
        }
    }
    if (!VeraCoreIsWaveSpeakerMaskValid(0x33u, 4) ||
        VeraCoreIsWaveSpeakerMaskValid(0, 2) ||
        VeraCoreIsWaveSpeakerMaskValid(0x3u, 6) ||
        VeraCoreIsWaveSpeakerMaskValid(0x100000000ull, 1) ||
        VeraCoreCanonicalWaveSpeakerMask(4) != 0) {
        return 2;
    }
    std::cout << "CHANNEL_LAYOUT_TEST=PASS VECTORS=11 CANONICAL=6 CUSTOM=1 INVALID=4\n";
    return 0;
}
