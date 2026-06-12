#include "AudioGeneral.h"
AudioGeneral::AudioGeneral() {
    QSettings settings;
    int dev = settings.value("DevIndex").toInt();
    int audioBackend = settings.value("Backend").toInt();
    int audioSRate = settings.value("SRate").toInt();
    err = Pa_Initialize();
    if (err != paNoError) QMessageBox::critical(nullptr, "Error", "Error: " + err);
    switch (audioBackend)
    {
    case 0:
        audioCore.OpenStreamASIO(audioSRate, dev);
        break;
    case 1:
        audioCore.OpenStreamWASAPI(audioSRate, dev);
        break;
    default:
        break;
    }
}

AudioGeneral::~AudioGeneral() {}
