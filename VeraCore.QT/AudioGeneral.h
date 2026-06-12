#pragma once
#include "../VeraCore.Audio/VeraCoreAudio.h"
#include <QMessageBox>
#include <QSettings>
#include <include/pa_asio.h>
using namespace std;
class AudioGeneral
{
public:
    PaError err;
    VeraCoreAudio audioCore;
    AudioGeneral();
    ~AudioGeneral();
};

