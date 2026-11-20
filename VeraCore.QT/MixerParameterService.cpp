#include "MixerParameterService.h"

#include <algorithm>
#include <cmath>

namespace {
float finiteClamp(float value, float low, float high, float fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

bool different(float left, float right) noexcept
{
    return std::fabs(left - right) > 1.0e-6f;
}

bool sameAudioFxSlots(const std::vector<AudioFxSlot>& left,
                      const std::vector<AudioFxSlot>& right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const AudioFxSlot& a = left[index];
        const AudioFxSlot& b = right[index];
        if (a.name != b.name || a.pluginPath != b.pluginPath ||
            a.pluginUid != b.pluginUid || a.pluginType != b.pluginType ||
            a.pluginState != b.pluginState || a.enabled != b.enabled ||
            a.automation.size() != b.automation.size()) return false;
        for (std::size_t laneIndex = 0; laneIndex < a.automation.size(); ++laneIndex) {
            const PluginAutomationLane& aLane = a.automation[laneIndex];
            const PluginAutomationLane& bLane = b.automation[laneIndex];
            if (aLane.parameterId != bLane.parameterId ||
                aLane.parameterName != bLane.parameterName ||
                aLane.enabled != bLane.enabled ||
                aLane.interpolation != bLane.interpolation ||
                aLane.points.size() != bLane.points.size()) return false;
            for (std::size_t pointIndex = 0; pointIndex < aLane.points.size(); ++pointIndex) {
                const PluginAutomationPoint& aPoint = aLane.points[pointIndex];
                const PluginAutomationPoint& bPoint = bLane.points[pointIndex];
                if (aPoint.beat != bPoint.beat ||
                    aPoint.normalizedValue != bPoint.normalizedValue ||
                    aPoint.curveTension != bPoint.curveTension ||
                    static_cast<bool>(aPoint.curvePolynomial) !=
                        static_cast<bool>(bPoint.curvePolynomial) ||
                    (aPoint.curvePolynomial && bPoint.curvePolynomial &&
                     *aPoint.curvePolynomial != *bPoint.curvePolynomial)) return false;
            }
        }
    }
    return true;
}
}

MixerParameterService::MixerParameterService(QObject* parent)
    : QObject(parent)
{
    m_returnBuses[0].name = QStringLiteral("A Return");
    m_returnBuses[1].name = QStringLiteral("B Return");
}

MixerParameterService::TrackState MixerParameterService::stateFor(const Track& track) noexcept
{
    return {track.GetVolume(), track.GetPan(), track.GetStereoSeparation(), track.IsMuted(), track.IsSoloed(),
            track.IsArmed(), track.IsMonitoring(), track.GetSpatialPanner()};
}

MixerParameterService::TrackState MixerParameterService::normalizedTrackState(
    const TrackState& state) noexcept
{
    TrackState result = state;
    result.volume = finiteClamp(result.volume, 0.0f, 2.0f, 1.0f);
    result.pan = finiteClamp(result.pan, -1.0f, 1.0f, 0.0f);
    result.stereoSeparation = finiteClamp(result.stereoSeparation, -1.0f, 1.0f, 0.0f);
    result.spatial = VeraCoreNormalizeSpatialPanner(result.spatial);
    if (result.spatial.linkStereoPan)
        result.spatial.azimuthDegrees = result.pan * 30.0f;
    return result;
}

ReturnBusState MixerParameterService::normalizedReturnBus(const ReturnBusState& state)
{
    ReturnBusState result = state;
    result.name = result.name.trimmed().left(64);
    result.volume = finiteClamp(result.volume, 0.0f, 2.0f, 1.0f);
    result.pan = finiteClamp(result.pan, -1.0f, 1.0f, 0.0f);
    result.stereoSeparation = finiteClamp(result.stereoSeparation, -1.0f, 1.0f, 0.0f);
    return result;
}

MasterBusState MixerParameterService::normalizedMasterBus(
    const MasterBusState& state)
{
    MasterBusState result = state;
    result.volume = finiteClamp(result.volume, 0.0f, 2.0f, 1.0f);
    result.pan = finiteClamp(result.pan, -1.0f, 1.0f, 0.0f);
    result.stereoSeparation = finiteClamp(result.stereoSeparation, -1.0f, 1.0f, 0.0f);
    return result;
}

void MixerParameterService::publish(Track* track, std::uint32_t changes)
{
    if (changes == None) return;
    ++m_revision;
    emit parametersChanged(m_revision, track, changes);
}

bool MixerParameterService::setTrackState(Track& track, const TrackState& requested)
{
    const TrackState state = normalizedTrackState(requested);
    std::uint32_t changes = None;
    if (different(track.GetVolume(), state.volume) || different(track.GetPan(), state.pan) ||
        different(track.GetStereoSeparation(), state.stereoSeparation))
        changes |= TrackFader;
    if (track.IsMuted() != state.muted || track.IsSoloed() != state.soloed)
        changes |= TrackAudibility;
    if (track.IsArmed() != state.armed || track.IsMonitoring() != state.monitoring)
        changes |= TrackInput;
    const VeraCoreSpatialPannerState& currentSpatial = track.GetSpatialPanner();
    if (currentSpatial.mode != state.spatial.mode ||
        currentSpatial.azimuthDegrees != state.spatial.azimuthDegrees ||
        currentSpatial.elevationDegrees != state.spatial.elevationDegrees ||
        currentSpatial.distance != state.spatial.distance ||
        currentSpatial.spread != state.spatial.spread ||
        currentSpatial.lfeSend != state.spatial.lfeSend ||
        currentSpatial.linkStereoPan != state.spatial.linkStereoPan)
        changes |= TrackFader;
    if (changes == None) return false;
    track.SetVolume(state.volume);
    track.SetPan(state.pan);
    track.SetStereoSeparation(state.stereoSeparation);
    track.SetMuted(state.muted);
    track.SetSoloed(state.soloed);
    track.SetArmed(state.armed);
    track.SetMonitoring(state.monitoring);
    track.SetSpatialPanner(state.spatial);
    publish(&track, changes);
    return true;
}

bool MixerParameterService::setTrackSend(Track& track, int returnBus, float level)
{
    if (returnBus < 0 || returnBus >= kReturnBusCount) return false;
    const float safeLevel = finiteClamp(level, 0.0f, 1.0f, 0.0f);
    if (!different(track.GetSendLevel(returnBus), safeLevel)) return false;
    track.SetSendLevel(returnBus, safeLevel);
    publish(&track, TrackSend);
    return true;
}

bool MixerParameterService::setReturnBus(int returnBus, const ReturnBusState& requested)
{
    if (returnBus < 0 || returnBus >= kReturnBusCount) return false;
    const ReturnBusState state = normalizedReturnBus(requested);
    const ReturnBusState& current = m_returnBuses[static_cast<std::size_t>(returnBus)];
    if (current.name == state.name && !different(current.volume, state.volume) &&
        !different(current.pan, state.pan) &&
        !different(current.stereoSeparation, state.stereoSeparation) && current.muted == state.muted &&
        current.soloed == state.soloed && current.preFader == state.preFader &&
        sameAudioFxSlots(current.audioFxSlots, state.audioFxSlots)) return false;
    m_returnBuses[static_cast<std::size_t>(returnBus)] = state;
    publish(nullptr, ReturnBus);
    return true;
}

bool MixerParameterService::setMasterBus(const MasterBusState& requested)
{
    const MasterBusState state = normalizedMasterBus(requested);
    const bool fxChanged = !sameAudioFxSlots(m_masterBus.audioFxSlots, state.audioFxSlots);
    if (!different(m_masterBus.volume, state.volume) &&
        !different(m_masterBus.pan, state.pan) &&
        !different(m_masterBus.stereoSeparation, state.stereoSeparation) &&
        m_masterBus.muted == state.muted &&
        !fxChanged) return false;
    m_masterBus = state;
    publish(nullptr, MasterBus | (fxChanged ? MasterBusFx : None));
    return true;
}

void MixerParameterService::restoreTrackState(Track& track, const TrackState& requested) noexcept
{
    const TrackState state = normalizedTrackState(requested);
    track.SetVolume(state.volume);
    track.SetPan(state.pan);
    track.SetStereoSeparation(state.stereoSeparation);
    track.SetMuted(state.muted);
    track.SetSoloed(state.soloed);
    track.SetArmed(state.armed);
    track.SetMonitoring(state.monitoring);
    track.SetSpatialPanner(state.spatial);
}

void MixerParameterService::restoreTrackSend(Track& track, int returnBus, float level) noexcept
{
    if (returnBus >= 0 && returnBus < kReturnBusCount)
        track.SetSendLevel(returnBus, finiteClamp(level, 0.0f, 1.0f, 0.0f));
}

void MixerParameterService::restoreReturnBus(int returnBus, const ReturnBusState& state)
{
    if (returnBus >= 0 && returnBus < kReturnBusCount)
        m_returnBuses[static_cast<std::size_t>(returnBus)] = normalizedReturnBus(state);
}

void MixerParameterService::restoreMasterBus(const MasterBusState& state)
{
    m_masterBus = normalizedMasterBus(state);
}

void MixerParameterService::resetProject()
{
    m_returnBuses = {};
    m_returnBuses[0].name = QStringLiteral("A Return");
    m_returnBuses[1].name = QStringLiteral("B Return");
    m_masterBus = {};
    publish(nullptr, ReturnBus | MasterBus | Republish);
}

void MixerParameterService::republishProject(
    const std::vector<std::shared_ptr<Track>>& tracks)
{
    for (const std::shared_ptr<Track>& track : tracks) {
        if (track) {
            // Re-normalize externally restored project data before it reaches
            // any UI or realtime subscriber.
            restoreTrackState(*track, stateFor(*track));
            for (int bus = 0; bus < kReturnBusCount; ++bus)
                restoreTrackSend(*track, bus, track->GetSendLevel(bus));
        }
    }
    publish(nullptr, All);
}
