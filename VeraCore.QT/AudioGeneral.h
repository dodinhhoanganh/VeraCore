#pragma once
#include "../VeraCore.Audio/VeraCoreAudio.h"
#include <QMessageBox>
#include <QSettings>
#include <QStringList>
#include <include/pa_asio.h>
#include <windows.h>
#include <mmsystem.h>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>
using namespace std;

struct AudioDeviceStatus
{
    bool initialized = false;
    bool streamOpen = false;
    bool active = false;
    PaError activity = paBadStreamPtr;
    double sampleRate = 0.0;
    unsigned long bufferFrames = 0;
    double inputLatencyMs = 0.0;
    double outputLatencyMs = 0.0;
    int outputChannels = 0;
    VeraCoreSpeakerLayout speakerLayout = VeraCoreSpeakerLayout::Stereo;
    VeraCoreSpeakerLayout requestedSpeakerLayout = VeraCoreSpeakerLayout::Stereo;
    bool layoutFallback = false;
    QString message;
};

class AudioGeneral
{
public:
    PaError err = paNoError;
    VeraCoreAudio audioCore;
    AudioGeneral();
    ~AudioGeneral();
    void InitFromSettings();
    void OpenMidiInputs(const QStringList& inputNames);
    void OpenMidiOutputs(const QStringList& outputNames);
    void CloseMidiInputs();
    void CloseMidiOutputs();
    void SendMidiToOutputs(const VeraCoreMidiEvent& event);
    void SetMidiInputCallback(std::function<void(const VeraCoreMidiEvent&)> callback);
    void HandleMidiInput(const VeraCoreMidiEvent& event);
    AudioDeviceStatus GetAudioDeviceStatus() const;
    PaError RestartAudioFromSettings();
    static QStringList AvailableMidiInputs();
    static QStringList AvailableMidiOutputs();

private:
    struct ActiveAudioConfiguration
    {
        int backend = 1;
        int device = -1;
        int sampleRate = 44100;
        unsigned long bufferFrames = 512;
        QString deviceName;
        bool valid = false;
    };
    vector<HMIDIIN> midiInputs;
    vector<HMIDIOUT> midiOutputs;
    std::mutex midiCallbackMutex;
    std::function<void(const VeraCoreMidiEvent&)> midiInputCallback;
    bool portAudioInitialized = false;
    ActiveAudioConfiguration activeAudioConfiguration;
    PaError OpenConfiguredAudioDevice();
    PaError OpenAudioDevice(const ActiveAudioConfiguration& configuration);
};
