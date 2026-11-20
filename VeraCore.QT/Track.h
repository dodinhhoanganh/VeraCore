#pragma once

#include <QByteArray>
#include <QString>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "../VeraCore.Audio/SpatialAudioTypes.h"

inline constexpr int kReturnBusCount = 2;
inline constexpr std::size_t kMaxPluginOutputRoutes = 64;
inline constexpr int kMaxMixerTrackCount = 256;

// Maps one plug-in output bus to a VeraCore mixer channel. outputBusIndex is
// the native VST3/CLAP bus index; VST2 exposes consecutive stereo pairs.
// destinationTrackIndex == -1 keeps the bus on the instrument's own channel.
struct PluginOutputRoute {
    QString stablePortId;
    QString displayName;
    int outputBusIndex = 0;
    int destinationTrackIndex = -1;
    bool enabled = true;
};

inline constexpr std::size_t kMaxPluginAutomationLanes = 132;
inline constexpr std::size_t kMaxPluginAutomationPointsPerLane = 65536;
inline constexpr int kPluginAutomationSmoothSegments = 32;

enum class PluginAutomationInterpolation : uint8_t {
    Hold,
    Linear,
    Smooth
};

using PluginAutomationCurvePolynomial = std::array<double, 7>;

struct PluginAutomationPoint {
    double beat = 0.0;
    double normalizedValue = 0.0;
    // Shapes the segment starting at this point in Smooth mode.
    // -1 = ease-out, 0 = symmetric S-curve, +1 = ease-in.
    double curveTension = 0.0;
    // Optional degree-6 normalized segment polynomial. This preserves an exact
    // Smooth sub-curve when Touch rejoins inside an existing segment.
    std::shared_ptr<const PluginAutomationCurvePolynomial> curvePolynomial;
};

double pluginAutomationCurvePosition(double position, double curveTension) noexcept;
double pluginAutomationCurvePosition(double position,
                                     const PluginAutomationPoint& point) noexcept;
PluginAutomationCurvePolynomial pluginAutomationCurvePolynomial(
    double curveTension) noexcept;
PluginAutomationCurvePolynomial pluginAutomationRestrictedCurve(
    const PluginAutomationCurvePolynomial& polynomial,
    double startPosition) noexcept;
bool pluginAutomationCurvePolynomialValid(
    const PluginAutomationCurvePolynomial& polynomial) noexcept;

// Stored with the device slot so stable plug-in parameter IDs survive project
// save/load, slot moves, Make Unique, and Arrangement undo/redo snapshots.
struct PluginAutomationLane {
    uint32_t parameterId = 0;
    QString parameterName;
    bool enabled = true;
    PluginAutomationInterpolation interpolation = PluginAutomationInterpolation::Linear;
    std::vector<PluginAutomationPoint> points;
};

struct LayerSlot {
    QString name = "Internal Synth";
    QString pluginPath;
    QString pluginUid;
    QString pluginType = "Internal";
    QByteArray pluginState;
    bool enabled = true;
    float gain = 1.0f;
    float pan = 0.0f;
    int transpose = 0;
    int midiChannel = -1; // -1 means all incoming note color/MIDI channels.
    std::vector<PluginOutputRoute> outputRoutes;
    std::vector<PluginAutomationLane> automation;
};

struct AudioFxSlot {
    QString name;
    QString pluginPath;
    QString pluginUid;
    QString pluginType;
    QByteArray pluginState;
    bool enabled = true;
    std::vector<PluginAutomationLane> automation;
};

struct MasterBusState {
    float volume = 1.0f;
    float pan = 0.0f;
    float stereoSeparation = 0.0f;
    bool muted = false;
    std::vector<AudioFxSlot> audioFxSlots;
};

struct ReturnBusState {
    QString name;
    float volume = 1.0f;
    float pan = 0.0f;
    float stereoSeparation = 0.0f;
    bool muted = false;
    bool soloed = false;
    bool preFader = false;
    std::vector<AudioFxSlot> audioFxSlots;
};

enum class TrackType {
    Audio,
    Midi
};

class Track {
public:
    Track(const QString& name, TrackType type);
    virtual ~Track() = default;

    QString GetName() const;
    TrackType GetType() const;
    void SetName(const QString& name);

    float GetVolume() const;
    void SetVolume(float volume);

    float GetPan() const;
    void SetPan(float pan);
    float GetStereoSeparation() const;
    void SetStereoSeparation(float separation);
    const VeraCoreSpatialPannerState& GetSpatialPanner() const noexcept;
    void SetSpatialPanner(const VeraCoreSpatialPannerState& state) noexcept;

    bool IsMuted() const;
    void SetMuted(bool muted);

    bool IsSoloed() const;
    void SetSoloed(bool soloed);

    bool IsArmed() const;
    void SetArmed(bool armed);

    bool IsMonitoring() const;
    void SetMonitoring(bool monitoring);

    float GetSendLevel(int returnBus) const;
    void SetSendLevel(int returnBus, float level);

    const std::vector<AudioFxSlot>& GetAudioFxSlots() const;
    void SetAudioFxSlots(const std::vector<AudioFxSlot>& audioFxSlots);
    void AddAudioFx(const AudioFxSlot& slot);
    void SetAudioFxEnabled(int index, bool enabled);
    void SetAudioFxPluginState(int index, const QByteArray& state);
    void SetAudioFxAutomation(int index, const std::vector<PluginAutomationLane>& automation);
    void RemoveAudioFx(int index);

protected:
    QString m_name;
    TrackType m_type;
    float m_volume;
    float m_pan;
    float m_stereoSeparation;
	VeraCoreSpatialPannerState m_spatialPanner;
    bool m_isMuted;
    bool m_isSoloed;
    bool m_isArmed;
    bool m_isMonitoring;
    std::array<float, kReturnBusCount> m_sendLevels{};
    std::vector<AudioFxSlot> m_audioFxSlots;
};

class AudioTrack : public Track {
public:
    AudioTrack(const QString& name);
};

class MidiTrack : public Track {
public:
    MidiTrack(const QString& name);

    QString GetInstrument() const;
    void SetInstrument(const QString& instrumentName);
    const std::vector<LayerSlot>& GetLayerSlots() const;
    void SetLayerSlots(const std::vector<LayerSlot>& layers);
    void AddLayerSlot(const LayerSlot& layer);
    void SetLayerSlotEnabled(int index, bool enabled);
    void SetLayerSlotParameters(int index, float gain, float pan, int transpose, int midiChannel);
    void SetLayerSlotPluginState(int index, const QByteArray& state);
    void SetLayerSlotAutomation(int index, const std::vector<PluginAutomationLane>& automation);
    void SetLayerSlotOutputRoutes(int index, const std::vector<PluginOutputRoute>& routes);
    void RemoveLayerSlot(int index);
    void ClearLayerSlots();

private:
    QString m_instrumentSlot;
    std::vector<LayerSlot> m_layerSlots;
};
