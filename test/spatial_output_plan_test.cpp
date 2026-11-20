#include "../VeraCore.Audio/SpatialAudioTypes.h"

#include <array>
#include <iostream>

namespace
{
template <std::size_t Size>
bool expectPlan(VeraCoreSpeakerLayout requested, int maximumChannels,
                const std::array<VeraCoreSpeakerLayout, Size>& expected)
{
    const VeraCoreOutputLayoutPlan plan = VeraCoreBuildOutputLayoutPlan(requested, maximumChannels);
    if (plan.candidateCount != static_cast<int>(Size)) return false;
    for (int index = 0; index < plan.candidateCount; ++index) {
        const VeraCoreSpeakerLayout layout = expected[static_cast<std::size_t>(index)];
        const VeraCoreOutputLayoutCandidate actual = plan.candidates[static_cast<std::size_t>(index)];
        if (actual.layout != layout || actual.channelCount != VeraCoreSpeakerChannelCount(layout) ||
            actual.channelCount > maximumChannels) return false;
        if (index > 0 && actual.channelCount >=
            plan.candidates[static_cast<std::size_t>(index - 1)].channelCount) return false;
    }
    return true;
}
}

int main()
{
    using L = VeraCoreSpeakerLayout;
    if (!expectPlan(L::Stereo, 1, std::array<L, 0>{}) ||
        !expectPlan(L::Stereo, 2, std::array<L, 1>{L::Stereo}) ||
        !expectPlan(L::Surround51, 2, std::array<L, 1>{L::Stereo}) ||
        !expectPlan(L::Surround51, 6, std::array<L, 2>{L::Surround51, L::Stereo}) ||
        !expectPlan(L::Surround71, 6, std::array<L, 2>{L::Surround51, L::Stereo}) ||
        !expectPlan(L::Surround71, 8, std::array<L, 3>{L::Surround71, L::Surround51, L::Stereo}) ||
        !expectPlan(L::Atmos712, 8, std::array<L, 3>{L::Surround71, L::Surround51, L::Stereo}) ||
        !expectPlan(L::Atmos712, 10, std::array<L, 4>{L::Atmos712, L::Surround71, L::Surround51, L::Stereo}) ||
        !expectPlan(L::Atmos714, 10, std::array<L, 4>{L::Atmos712, L::Surround71, L::Surround51, L::Stereo}) ||
        !expectPlan(L::Atmos714, 12, std::array<L, 5>{L::Atmos714, L::Atmos712, L::Surround71, L::Surround51, L::Stereo})) {
        return 1;
    }
    std::cout << "SPATIAL_OUTPUT_PLAN_TEST=PASS VECTORS=10 ASIO_WASAPI_SHARED=TRUE FALLBACK=12>10>8>6>2\n";
    return 0;
}
