#include "VeraCoreAudio.h"
#include <portaudio.h>
#include <string.h>
#include <cstdint>
#include <algorithm>
#define bzero(b,len) (memset((b), '\0', (len)), (void) 0)
VeraCoreAudio::VeraCoreAudio()
{ }
VeraCoreAudio::~VeraCoreAudio()
{}
static int Process(const void* inputBuffer,
    void* outputBuffer,
    unsigned long framesCount,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    (void)timeInfo;
    (void)statusFlags;
    (void)userData;

    float* out = static_cast<float*>(outputBuffer);
    const float* in = static_cast<const float*>(inputBuffer);
    if (!out) {
        return paContinue;
    }

    const size_t sampleCount = static_cast<size_t>(framesCount) * 2;
    if (!in) {
        std::fill_n(out, sampleCount, 0.0f);
        return paContinue;
    }

    std::copy_n(in, sampleCount, out);
    return paContinue;
}
PaError VeraCoreAudio::OpenStreamASIO(double srate, int devnum) {
    PaError err;
    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(devnum);
    if (!deviceInfo) return paInvalidDevice;

    PaStreamParameters inParam;
    memset(&inParam, 0, sizeof(PaStreamParameters));
    inParam.channelCount = 2;
    inParam.device = devnum;
    inParam.sampleFormat = paFloat32;
    inParam.suggestedLatency = deviceInfo->defaultLowInputLatency;
    inParam.hostApiSpecificStreamInfo = nullptr;

    PaStreamParameters outParam;
    memset(&outParam, 0, sizeof(PaStreamParameters));
    outParam.channelCount = 2;
    outParam.device = devnum;
    outParam.sampleFormat = paFloat32;
    outParam.suggestedLatency = deviceInfo->defaultLowOutputLatency;
    outParam.hostApiSpecificStreamInfo = nullptr;

    err = Pa_OpenStream(&stream, &inParam, &outParam, srate, paFramesPerBufferUnspecified, paNoFlag, Process, this);

    if (err == paNoError) {
        Pa_StartStream(stream);
    }
    return err;
}
PaError VeraCoreAudio::OpenStreamWASAPI(double srate, int devnum) {
    PaError err;
    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(devnum);
    if (!deviceInfo) return paInvalidDevice;

    PaStreamParameters outParam;
    memset(&outParam, 0, sizeof(PaStreamParameters));
    outParam.channelCount = 2;
    outParam.device = devnum;
    outParam.sampleFormat = paFloat32;
    outParam.suggestedLatency = deviceInfo->defaultLowOutputLatency;
    outParam.hostApiSpecificStreamInfo = nullptr;

    err = Pa_OpenStream(&stream, nullptr, &outParam, srate, paFramesPerBufferUnspecified, paNoFlag, Process, this);

    if (err == paNoError) {
        Pa_StartStream(stream);
    }
    return err;
}
PaError VeraCoreAudio::CloseStream()
{
    PaError err = paNoError;
    if (stream != nullptr) {
        Pa_AbortStream(stream);
        err = Pa_CloseStream(stream);
        stream = nullptr;
    }
    return err;
}
