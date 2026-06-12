#include "Track.h"

Track::Track(const QString& name, TrackType type)
    : m_name(name)
    , m_type(type)
    , m_volume(1.0f)
    , m_pan(0.0f)
    , m_isMuted(false)
    , m_isSoloed(false)
    , m_isArmed(false)
    , m_isMonitoring(false)
{
}

QString Track::GetName() const { return m_name; }
TrackType Track::GetType() const { return m_type; }
void Track::SetName(const QString& name) { m_name = name; }

float Track::GetVolume() const { return m_volume; }
void Track::SetVolume(float volume) { m_volume = volume; }

float Track::GetPan() const { return m_pan; }
void Track::SetPan(float pan) { m_pan = pan; }

bool Track::IsMuted() const { return m_isMuted; }
void Track::SetMuted(bool muted) { m_isMuted = muted; }

bool Track::IsSoloed() const { return m_isSoloed; }
void Track::SetSoloed(bool soloed) { m_isSoloed = soloed; }

bool Track::IsArmed() const { return m_isArmed; }
void Track::SetArmed(bool armed) { m_isArmed = armed; }

bool Track::IsMonitoring() const { return m_isMonitoring; }
void Track::SetMonitoring(bool monitoring) { m_isMonitoring = monitoring; }

std::vector<QString> Track::GetAudioFxSlots() const { return m_audioFxSlots; }
void Track::AddAudioFx(const QString& fxName) { m_audioFxSlots.push_back(fxName); }


// AudioTrack
AudioTrack::AudioTrack(const QString& name)
    : Track(name, TrackType::Audio)
{
}

// MidiTrack
MidiTrack::MidiTrack(const QString& name)
    : Track(name, TrackType::Midi)
{
}

QString MidiTrack::GetInstrument() const { return m_instrumentSlot; }
void MidiTrack::SetInstrument(const QString& instrumentName) { m_instrumentSlot = instrumentName; }
