#pragma once

#include <QString>
#include <vector>

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

    bool IsMuted() const;
    void SetMuted(bool muted);

    bool IsSoloed() const;
    void SetSoloed(bool soloed);

    bool IsArmed() const;
    void SetArmed(bool armed);

    bool IsMonitoring() const;
    void SetMonitoring(bool monitoring);

    std::vector<QString> GetAudioFxSlots() const;
    void AddAudioFx(const QString& fxName);

protected:
    QString m_name;
    TrackType m_type;
    float m_volume;
    float m_pan;
    bool m_isMuted;
    bool m_isSoloed;
    bool m_isArmed;
    bool m_isMonitoring;
    std::vector<QString> m_audioFxSlots;
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

private:
    QString m_instrumentSlot;
};
