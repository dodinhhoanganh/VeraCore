#pragma once

#include <QObject>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include "Track.h"

class MixerParameterService final : public QObject
{
    Q_OBJECT

public:
    enum Change : std::uint32_t {
        None = 0,
        TrackFader = 1u << 0,
        TrackAudibility = 1u << 1,
        TrackInput = 1u << 2,
        TrackSend = 1u << 3,
        ReturnBus = 1u << 4,
        MasterBus = 1u << 5,
        Republish = 1u << 6,
        MasterBusFx = 1u << 7,
        All = TrackFader | TrackAudibility | TrackInput | TrackSend |
              ReturnBus | MasterBus | Republish | MasterBusFx
    };

    struct TrackState {
        float volume = 1.0f;
        float pan = 0.0f;
        float stereoSeparation = 0.0f;
        bool muted = false;
        bool soloed = false;
        bool armed = false;
        bool monitoring = false;
        VeraCoreSpatialPannerState spatial;
    };

    explicit MixerParameterService(QObject* parent = nullptr);

    quint64 revision() const noexcept { return m_revision; }
    const std::array<ReturnBusState, kReturnBusCount>& returnBuses() const noexcept
        { return m_returnBuses; }
    const MasterBusState& masterBus() const noexcept { return m_masterBus; }

    static TrackState stateFor(const Track& track) noexcept;
    bool setTrackState(Track& track, const TrackState& state);
    bool setTrackSend(Track& track, int returnBus, float level);
    bool setReturnBus(int returnBus, const ReturnBusState& state);
    bool setMasterBus(const MasterBusState& state);

    // Restore paths normalize the same canonical model without publishing each
    // field. republishProject() then emits one revision for load/undo/recovery.
    void restoreTrackState(Track& track, const TrackState& state) noexcept;
    void restoreTrackSend(Track& track, int returnBus, float level) noexcept;
    void restoreReturnBus(int returnBus, const ReturnBusState& state);
    void restoreMasterBus(const MasterBusState& state);
    void resetProject();
    void republishProject(const std::vector<std::shared_ptr<Track>>& tracks);

signals:
    void parametersChanged(quint64 revision, Track* track, std::uint32_t changes);

private:
    static TrackState normalizedTrackState(const TrackState& state) noexcept;
    static ReturnBusState normalizedReturnBus(const ReturnBusState& state);
    static MasterBusState normalizedMasterBus(const MasterBusState& state);
    void publish(Track* track, std::uint32_t changes);

    quint64 m_revision = 0;
    std::array<ReturnBusState, kReturnBusCount> m_returnBuses{};
    MasterBusState m_masterBus{};
};
