#pragma once

#include <QString>
#include <algorithm>
#include <cmath>
#include <vector>

constexpr int kMidiMinPitch = 0;
constexpr int kMidiMaxPitch = 127;
constexpr int kPianoRollMinPitch = 0;   // Displayed as C0.
constexpr int kPianoRollMaxPitch = kMidiMaxPitch;

inline bool isPluginMidiPitch(int pitch)
{
    return pitch >= kMidiMinPitch && pitch <= kMidiMaxPitch;
}

inline bool isValidMidiChannelEvent(unsigned char status, unsigned char data1, unsigned char data2)
{
    const unsigned char statusHi = status & 0xF0;
    if (statusHi < 0x80 || statusHi > 0xE0 || data1 > 127) {
        return false;
    }
    return statusHi == 0xC0 || statusHi == 0xD0 || data2 <= 127;
}

inline int clampIntValue(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

inline double nonNegativeDouble(double value)
{
    return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

inline bool isFiniteBeatRange(double startBeat, double lengthBeats)
{
    return std::isfinite(startBeat) && std::isfinite(lengthBeats) &&
           startBeat >= 0.0 && lengthBeats > 0.0;
}

inline int midiSampleOffset(double beat, double blockStartBeat, double beatsPerSample, int frameCount)
{
    if (frameCount <= 0 || beatsPerSample <= 0.0 ||
        !std::isfinite(beat) || !std::isfinite(blockStartBeat) || !std::isfinite(beatsPerSample)) {
        return 0;
    }

    const double offset = std::floor((beat - blockStartBeat) / beatsPerSample);
    if (!std::isfinite(offset) || offset <= 0.0) {
        return 0;
    }
    if (offset >= static_cast<double>(frameCount - 1)) {
        return frameCount - 1;
    }
    return static_cast<int>(offset);
}

inline int midiClipEventSortRank(unsigned char status, unsigned char data2)
{
    const unsigned char statusHi = status & 0xF0;
    if (statusHi == 0x80 || (statusHi == 0x90 && data2 == 0)) {
        return 0;
    }
    if (statusHi == 0xA0 || statusHi == 0xB0 || statusHi == 0xC0 ||
        statusHi == 0xD0 || statusHi == 0xE0) {
        return 1;
    }
    if (statusHi == 0x90) {
        return 2;
    }
    return 3;
}

constexpr unsigned char kMidiClipCcPan = 10;
constexpr unsigned char kMidiClipCcModX = 16;
constexpr unsigned char kMidiClipCcModY = 17;

struct MidiNote
{
    int pitch = 60;
    double startBeat = 0.0;
    double lengthBeats = 1.0;
    int velocity = 100;
    int pan = 64;
    int release = 64;
    int modX = 64;
    int modY = 64;
    int colorGroup = 0;
    int groupId = 0;
    bool selected = false;
    bool isKeyswitch = false;
    bool isSlide = false;
    bool isPortamento = false;
    bool muted = false;
};

struct MidiEvent
{
    double beat = 0.0;
    unsigned char status = 0;
    unsigned char data1 = 0;
    unsigned char data2 = 0;
    bool isSlide = false;
    bool isPortamento = false;
    double glideDurationBeats = 0.0;
    bool isKeyswitch = false;
};

struct PluginMidiEvent
{
    int sampleOffset = 0;
    unsigned char status = 0;
    unsigned char data1 = 0;
    unsigned char data2 = 0;
};

struct MidiGhostClip
{
    QString clipId;
    QString name;
    int trackIndex = -1;
    double relativeStartBeat = 0.0;
    double lengthBeats = 0.0;
    std::vector<MidiNote> notes;
};

struct MidiExpressionMapEntry
{
    int pitch = 0;
    int channel = 0;
    int layerId = -1;
    QString name;
    QString shortName;
};

struct MidiTempoPoint
{
    double beat = 0.0;
    double bpm = 120.0;
};

struct MidiTimeSignaturePoint
{
    double beat = 0.0;
    int numerator = 4;
    int denominator = 4;
};

struct MidiClipDocument
{
    QString name;
    QString trackName;
    int trackIndex = -1;
    double startBeat = 0.0;
    double lengthBeats = 4.0;
    double lastNoteLengthBeats = 1.0;
    double tempoBpm = 0.0;
    int timeSignatureNumerator = 4;
    int timeSignatureDenominator = 4;
    std::vector<MidiTempoPoint> tempoMap;
    std::vector<MidiTimeSignaturePoint> timeSignatureMap;
    std::vector<MidiNote> notes;
    std::vector<MidiEvent> performanceEvents;

    int noteCount() const { return static_cast<int>(notes.size()); }

    std::vector<MidiEvent> buildMidiEvents() const
    {
        std::vector<MidiEvent> events;
        auto pushCc = [&](double beat, unsigned char channel, unsigned char controller, int value) {
            events.push_back({beat,
                              static_cast<unsigned char>(0xB0 | channel),
                              controller,
                              static_cast<unsigned char>(clampIntValue(value, 0, 127))});
        };

        for (const MidiEvent& event : performanceEvents) {
            if (!std::isfinite(event.beat) || event.beat < 0.0 ||
                !isValidMidiChannelEvent(event.status, event.data1, event.data2)) {
                continue;
            }
            events.push_back(event);
        }

        for (const MidiNote& note : notes) {
            if (note.muted ||
                !isFiniteBeatRange(note.startBeat, note.lengthBeats) ||
                !isPluginMidiPitch(note.pitch)) {
                continue;
            }

            const unsigned char pitch = static_cast<unsigned char>(note.pitch);
            const unsigned char velocity = static_cast<unsigned char>(clampIntValue(note.velocity, 1, 127));
            const unsigned char channel = static_cast<unsigned char>(clampIntValue(note.colorGroup, 0, 15));
            if (note.isKeyswitch) {
                events.push_back({note.startBeat, static_cast<unsigned char>(0x90 | channel), pitch,
                                  velocity, false, false, 0.0, true});
                events.push_back({note.startBeat + nonNegativeDouble(note.lengthBeats),
                                  static_cast<unsigned char>(0x80 | channel), pitch,
                                  static_cast<unsigned char>(clampIntValue(note.release, 0, 127)),
                                  false, false, 0.0, true});
                continue;
            }
            if (note.isSlide) {
                events.push_back({note.startBeat, static_cast<unsigned char>(0x90 | channel), pitch,
                                  velocity, true, false, note.lengthBeats});
                continue;
            }
            if (note.pan != 64) {
                pushCc(note.startBeat, channel, kMidiClipCcPan, note.pan);
            }
            if (note.modX != 64) {
                pushCc(note.startBeat, channel, kMidiClipCcModX, note.modX);
            }
            if (note.modY != 64) {
                pushCc(note.startBeat, channel, kMidiClipCcModY, note.modY);
            }
            const double portamentoBeats = note.isPortamento
                ? std::min(0.25, note.lengthBeats)
                : 0.0;
            events.push_back({note.startBeat, static_cast<unsigned char>(0x90 | channel), pitch,
                              velocity, false, note.isPortamento, portamentoBeats});
            events.push_back({note.startBeat + nonNegativeDouble(note.lengthBeats),
                              static_cast<unsigned char>(0x80 | channel), pitch,
                              static_cast<unsigned char>(clampIntValue(note.release, 0, 127))});
        }

        std::stable_sort(events.begin(), events.end(), [](const MidiEvent& a, const MidiEvent& b) {
            if (a.beat != b.beat) return a.beat < b.beat;
            if (a.isPortamento != b.isPortamento) return a.isPortamento;
            const int aRank = midiClipEventSortRank(a.status, a.data2);
            const int bRank = midiClipEventSortRank(b.status, b.data2);
            if (aRank != bRank) return aRank < bRank;
            if (a.isSlide != b.isSlide) return !a.isSlide;
            return a.status < b.status;
        });
        std::vector<MidiEvent> compactEvents;
        compactEvents.reserve(events.size());
        for (const MidiEvent& event : events) {
            const unsigned char statusHi = event.status & 0xF0;
            const bool noteEvent = statusHi == 0x80 || statusHi == 0x90;
            const bool duplicateState = !noteEvent && !compactEvents.empty() &&
                compactEvents.back().beat == event.beat &&
                compactEvents.back().status == event.status &&
                compactEvents.back().data1 == event.data1 &&
                compactEvents.back().data2 == event.data2 &&
                compactEvents.back().isSlide == event.isSlide &&
                compactEvents.back().isPortamento == event.isPortamento &&
                compactEvents.back().glideDurationBeats == event.glideDurationBeats &&
                compactEvents.back().isKeyswitch == event.isKeyswitch;
            if (!duplicateState) compactEvents.push_back(event);
        }
        events.swap(compactEvents);
        return events;
    }

    std::vector<PluginMidiEvent> buildPluginMidiEvents(double blockStartBeat,
                                                       double beatsPerSample,
                                                       int frameCount) const
    {
        std::vector<PluginMidiEvent> pluginEvents;
        if (beatsPerSample <= 0.0 || frameCount <= 0 ||
            !std::isfinite(blockStartBeat) || !std::isfinite(beatsPerSample)) {
            return pluginEvents;
        }

        const double blockEndBeat = blockStartBeat + beatsPerSample * frameCount;
        for (const MidiEvent& event : buildMidiEvents()) {
            if (event.isSlide) {
                continue;
            }
            if (event.beat < blockStartBeat || event.beat >= blockEndBeat) {
                continue;
            }

            const int sampleOffset = midiSampleOffset(event.beat, blockStartBeat, beatsPerSample, frameCount);
            pluginEvents.push_back({sampleOffset, event.status, event.data1, event.data2});
        }

        std::stable_sort(pluginEvents.begin(), pluginEvents.end(), [](const PluginMidiEvent& a,
                                                                      const PluginMidiEvent& b) {
            if (a.sampleOffset != b.sampleOffset) return a.sampleOffset < b.sampleOffset;
            const int aRank = midiClipEventSortRank(a.status, a.data2);
            const int bRank = midiClipEventSortRank(b.status, b.data2);
            if (aRank != bRank) return aRank < bRank;
            return a.status < b.status;
        });
        return pluginEvents;
    }
};
