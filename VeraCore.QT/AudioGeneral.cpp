#include "AudioGeneral.h"
#include <chrono>

namespace
{
int configuredOutputDevice(const QSettings& settings, int backend)
{
    const PaHostApiTypeId expectedHost = backend == 0 ? paASIO : paWASAPI;
    const int count = Pa_GetDeviceCount();
    if (count <= 0) return -1;
    const auto isCandidate = [expectedHost](int index) {
        const PaDeviceInfo* device = index >= 0 ? Pa_GetDeviceInfo(index) : nullptr;
        const PaHostApiInfo* host = device ? Pa_GetHostApiInfo(device->hostApi) : nullptr;
        return device && host && host->type == expectedHost && device->maxOutputChannels >= 2;
    };

    const QString preferredName = settings.value("DevName").toString();
    if (!preferredName.isEmpty() && preferredName.compare("Default", Qt::CaseInsensitive) != 0) {
        for (int index = 0; index < count; ++index) {
            const PaDeviceInfo* device = Pa_GetDeviceInfo(index);
            if (isCandidate(index) && device &&
                QString::fromUtf8(device->name) == preferredName) return index;
        }
    }
    const int configuredIndex = settings.value("DevIndex", -1).toInt();
    if (isCandidate(configuredIndex)) return configuredIndex;
    const int defaultIndex = Pa_GetDefaultOutputDevice();
    if (isCandidate(defaultIndex)) return defaultIndex;
    for (int index = 0; index < count; ++index) {
        if (isCandidate(index)) return index;
    }
    return -1;
}
}

static void CALLBACK MidiInProc(HMIDIIN, UINT msg, DWORD_PTR user, DWORD_PTR param1, DWORD_PTR)
{
    if (msg != MIM_DATA || !user) return;
    auto* audio = reinterpret_cast<AudioGeneral*>(user);
    const DWORD data = static_cast<DWORD>(param1);
    VeraCoreMidiEvent event;
    event.status = static_cast<uint8_t>(data & 0xff);
    event.data1 = static_cast<uint8_t>((data >> 8) & 0xff);
    event.data2 = static_cast<uint8_t>((data >> 16) & 0xff);
    event.captureBeat = audio->audioCore.GetTransportPlayheadBeat();
    event.captureMonotonicSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    audio->HandleMidiInput(event);
}

AudioGeneral::AudioGeneral() {
    InitFromSettings();
}

void AudioGeneral::InitFromSettings()
{
    QSettings settings;
    auto openMidiOnly = [&]() {
        OpenMidiInputs(settings.value("MidiInputsEnabled").toStringList());
        OpenMidiOutputs(settings.value("MidiOutputsEnabled").toStringList());
    };

    err = Pa_Initialize();
    portAudioInitialized = err == paNoError;
    if (err != paNoError) {
        QMessageBox::critical(nullptr, "Audio Error", Pa_GetErrorText(err));
        openMidiOnly();
        return;
    }
    err = OpenConfiguredAudioDevice();
    OpenMidiInputs(settings.value("MidiInputsEnabled").toStringList());
    OpenMidiOutputs(settings.value("MidiOutputsEnabled").toStringList());
}

AudioGeneral::~AudioGeneral()
{
    SetMidiInputCallback({});
    CloseMidiInputs();
    CloseMidiOutputs();
    audioCore.CloseStream();
    if (portAudioInitialized) Pa_Terminate();
}

PaError AudioGeneral::OpenConfiguredAudioDevice()
{
    QSettings settings;
    int backend = settings.value("Backend", 1).toInt();
    if (backend != 0 && backend != 1) backend = 1;
    int sampleRate = settings.value("SRate", 44100).toInt();
    if (sampleRate < 8000 || sampleRate > 384000) sampleRate = 44100;
    const unsigned long bufferFrames = VeraCoreAudio::NormalizeBufferFrames(
        settings.value("AudioBufferFrames", 512).toULongLong());
    const int device = configuredOutputDevice(settings, backend);
    if (device < 0) return paInvalidDevice;

    ActiveAudioConfiguration configuration;
    configuration.backend = backend;
    configuration.device = device;
    configuration.sampleRate = sampleRate;
    configuration.bufferFrames = bufferFrames;
    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    if (info && info->name) configuration.deviceName = QString::fromUtf8(info->name);
    const PaError result = OpenAudioDevice(configuration);
    if (result == paNoError) {
        settings.setValue("Backend", backend);
        settings.setValue("DevIndex", device);
        settings.setValue("SRate", sampleRate);
        settings.setValue("AudioBufferFrames", static_cast<qulonglong>(bufferFrames));
        if (!configuration.deviceName.isEmpty()) settings.setValue("DevName", configuration.deviceName);
    }
    return result;
}

PaError AudioGeneral::OpenAudioDevice(const ActiveAudioConfiguration& configuration)
{
    const PaError result = configuration.backend == 0
        ? audioCore.OpenStreamASIO(configuration.sampleRate, configuration.device, configuration.bufferFrames)
        : audioCore.OpenStreamWASAPI(configuration.sampleRate, configuration.device, configuration.bufferFrames);
    if (result == paNoError) {
        activeAudioConfiguration = configuration;
        activeAudioConfiguration.valid = true;
    }
    return result;
}

AudioDeviceStatus AudioGeneral::GetAudioDeviceStatus() const
{
    AudioDeviceStatus status;
    status.initialized = portAudioInitialized;
    status.streamOpen = audioCore.HasOpenStream();
    const VeraCoreAudioStreamInfo streamInfo = audioCore.GetStreamInfo();
    status.sampleRate = streamInfo.sampleRate;
    status.bufferFrames = streamInfo.callbackBufferFrames != 0
        ? streamInfo.callbackBufferFrames : streamInfo.requestedBufferFrames;
    status.inputLatencyMs = streamInfo.inputLatencySeconds * 1000.0;
    status.outputLatencyMs = streamInfo.outputLatencySeconds * 1000.0;
    status.outputChannels = streamInfo.outputChannels;
    status.speakerLayout = streamInfo.speakerLayout;
    status.requestedSpeakerLayout = streamInfo.requestedSpeakerLayout;
    status.layoutFallback = streamInfo.layoutFallback;
    if (!status.initialized) {
        status.activity = err;
        status.message = QString("Audio engine initialization failed: %1")
            .arg(QString::fromLocal8Bit(Pa_GetErrorText(err)));
        return status;
    }
    status.activity = audioCore.GetStreamActivity();
    status.active = status.activity == 1;
    if (status.active) {
        status.message = QString("Audio active: %1 Hz / %2 samples / %3 ch / %4 ms output")
            .arg(status.sampleRate, 0, 'f', 0)
            .arg(status.bufferFrames)
            .arg(status.outputChannels)
             .arg(status.outputLatencyMs, 0, 'f', 1);
        if (status.layoutFallback) {
            status.message += QString(" / compatible fallback from %1 ch")
                .arg(VeraCoreSpeakerChannelCount(status.requestedSpeakerLayout));
        }
        if (audioCore.HasAudioInput()) {
            status.message += QString(" / %1 ms round trip")
                .arg(status.inputLatencyMs + status.outputLatencyMs, 0, 'f', 1);
        }
        status.message += ".";
    } else if (status.streamOpen && status.activity == 0) {
        status.message = "Audio output stream stopped. Click to reconnect.";
    } else if (status.activity < 0 && status.activity != paBadStreamPtr) {
        status.message = QString("Audio output error: %1. Click to reconnect.")
            .arg(QString::fromLocal8Bit(Pa_GetErrorText(status.activity)));
    } else if (err != paNoError) {
        status.message = QString("Audio output unavailable: %1. Click to reconnect.")
            .arg(QString::fromLocal8Bit(Pa_GetErrorText(err)));
    } else {
        status.message = "No active audio output. Click to reconnect.";
    }
    return status;
}

PaError AudioGeneral::RestartAudioFromSettings()
{
    const ActiveAudioConfiguration previous = activeAudioConfiguration;
    audioCore.CloseStream();
    if (!portAudioInitialized) {
        err = Pa_Initialize();
        portAudioInitialized = err == paNoError;
        if (!portAudioInitialized) return err;
    }
    const PaError requestedResult = OpenConfiguredAudioDevice();
    if (requestedResult == paNoError) {
        err = paNoError;
        return paNoError;
    }

    if (previous.valid) {
        const PaError rollbackResult = OpenAudioDevice(previous);
        if (rollbackResult == paNoError) {
            QSettings settings;
            settings.setValue("Backend", previous.backend);
            settings.setValue("DevIndex", previous.device);
            settings.setValue("DevName", previous.deviceName);
            settings.setValue("SRate", previous.sampleRate);
            settings.setValue("AudioBufferFrames", static_cast<qulonglong>(previous.bufferFrames));
            err = paNoError;
            return requestedResult;
        }
    }
    activeAudioConfiguration.valid = false;
    err = requestedResult;
    return requestedResult;
}

void AudioGeneral::SetMidiInputCallback(std::function<void(const VeraCoreMidiEvent&)> callback)
{
    std::lock_guard<std::mutex> lock(midiCallbackMutex);
    midiInputCallback = std::move(callback);
}

void AudioGeneral::HandleMidiInput(const VeraCoreMidiEvent& event)
{
    audioCore.QueueRealtimeMidiEvent(event);
    SendMidiToOutputs(event);

    std::function<void(const VeraCoreMidiEvent&)> callback;
    {
        std::lock_guard<std::mutex> lock(midiCallbackMutex);
        callback = midiInputCallback;
    }
    if (callback) {
        callback(event);
    }
}

QStringList AudioGeneral::AvailableMidiInputs()
{
    QStringList devices;
    const UINT count = midiInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            devices << QString::fromWCharArray(caps.szPname);
    }
    return devices;
}

QStringList AudioGeneral::AvailableMidiOutputs()
{
    QStringList devices;
    const UINT count = midiOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            devices << QString::fromWCharArray(caps.szPname);
    }
    return devices;
}

void AudioGeneral::OpenMidiInputs(const QStringList& inputNames)
{
    CloseMidiInputs();
    const UINT count = midiInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        QString name = QString::fromWCharArray(caps.szPname);
        if (!inputNames.contains(name)) continue;
        HMIDIIN handle = nullptr;
        if (midiInOpen(&handle, i, reinterpret_cast<DWORD_PTR>(&MidiInProc),
                       reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
            if (midiInStart(handle) == MMSYSERR_NOERROR) {
                midiInputs.push_back(handle);
            } else {
                midiInClose(handle);
            }
        }
    }
}

void AudioGeneral::OpenMidiOutputs(const QStringList& outputNames)
{
    CloseMidiOutputs();
    const UINT count = midiOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        QString name = QString::fromWCharArray(caps.szPname);
        if (!outputNames.contains(name)) continue;
        HMIDIOUT handle = nullptr;
        if (midiOutOpen(&handle, i, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
            midiOutputs.push_back(handle);
        }
    }
}

void AudioGeneral::CloseMidiInputs()
{
    for (HMIDIIN handle : midiInputs) {
        midiInStop(handle);
        midiInReset(handle);
        midiInClose(handle);
    }
    midiInputs.clear();
}

void AudioGeneral::CloseMidiOutputs()
{
    for (HMIDIOUT handle : midiOutputs) {
        midiOutReset(handle);
        midiOutClose(handle);
    }
    midiOutputs.clear();
}

void AudioGeneral::SendMidiToOutputs(const VeraCoreMidiEvent& event)
{
    if (midiOutputs.empty()) {
        return;
    }
    const uint8_t statusHi = event.status & 0xF0;
    const bool isChannelEvent = statusHi >= 0x80 && statusHi <= 0xE0;
    if (!isChannelEvent || event.data1 > 127 ||
        ((statusHi != 0xC0 && statusHi != 0xD0) && event.data2 > 127)) {
        return;
    }

    const DWORD packed = static_cast<DWORD>(event.status) |
        (static_cast<DWORD>(event.data1) << 8) |
        (static_cast<DWORD>((statusHi == 0xC0 || statusHi == 0xD0) ? 0 : event.data2) << 16);
    for (HMIDIOUT handle : midiOutputs) {
        if (handle) {
            midiOutShortMsg(handle, packed);
        }
    }
}
