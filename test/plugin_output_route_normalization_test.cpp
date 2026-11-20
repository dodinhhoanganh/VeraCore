#include "../VeraCore.QT/Track.h"

#include <iostream>
#include <string>

namespace {
bool require(bool condition, const char* message)
{
    if (condition) return true;
    std::cerr << "FAIL=" << message << '\n';
    return false;
}
}

int main()
{
    MidiTrack track(QStringLiteral("Instrument"));
    std::vector<PluginOutputRoute> routes = {
        {QStringLiteral(" main "), QStringLiteral("  Main   Out  "), 0, -1, true},
        {QStringLiteral("aux-a"), QStringLiteral("Aux A"), 1, 4, true},
        {QStringLiteral("aux-a"), QStringLiteral("Aux A moved"), 2, 5, true},
        {QStringLiteral("aux-b"), QStringLiteral("Aux B old"), 3, 6, true},
        {QStringLiteral("aux-c"), QStringLiteral("Aux C"), 3, 7, false},
        {QStringLiteral("bad-low"), QStringLiteral("Bad"), -1, 0, true},
        {QStringLiteral("bad-high"), QStringLiteral("Bad"), 64, 0, true},
        {QStringLiteral("bad-destination"), QStringLiteral("Bad"), 4, 256, true}
    };
    track.SetLayerSlotOutputRoutes(0, routes);
    const auto normalized = track.GetLayerSlots().front().outputRoutes;
    if (!require(normalized.size() == 3, "invalid-or-duplicate-routes-not-removed") ||
        !require(normalized[0].stablePortId == QStringLiteral("main"), "stable-id-not-trimmed") ||
        !require(normalized[0].displayName == QStringLiteral("Main Out"), "name-not-normalized") ||
        !require(normalized[1].stablePortId == QStringLiteral("aux-a") &&
                 normalized[1].outputBusIndex == 2 &&
                 normalized[1].destinationTrackIndex == 5, "stable-id-last-write-not-wins") ||
        !require(normalized[2].stablePortId == QStringLiteral("aux-c") &&
                 normalized[2].outputBusIndex == 3 && !normalized[2].enabled,
                 "bus-last-write-not-wins")) return 1;

    LayerSlot imported;
    imported.name = QStringLiteral("Imported");
    imported.outputRoutes = routes;
    track.AddLayerSlot(imported);
    const auto& importedRoutes = track.GetLayerSlots().back().outputRoutes;
    if (!require(importedRoutes.size() == normalized.size(), "import-path-not-normalized"))
        return 1;

    std::vector<PluginOutputRoute> maximum;
    for (int bus = 0; bus < 64; ++bus)
        maximum.push_back({QStringLiteral("port-%1").arg(bus), {}, bus, bus, true});
    track.SetLayerSlotOutputRoutes(0, maximum);
    const auto& bounded = track.GetLayerSlots().front().outputRoutes;
    if (!require(bounded.size() == kMaxPluginOutputRoutes, "valid-64-route-map-not-preserved") ||
        !require(bounded.back().destinationTrackIndex == 63, "destination-corrupted")) return 1;

    std::cout << "PLUGIN_OUTPUT_ROUTE_NORMALIZATION_TEST=PASS VECTORS=10 "
                 "MAX_ROUTES=64 MAX_TRACKS=256 DUPLICATE_POLICY=LAST_WRITE\n";
    return 0;
}
