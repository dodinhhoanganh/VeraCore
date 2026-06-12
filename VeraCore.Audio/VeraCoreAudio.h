#ifndef VeraCoreAUDIO_H
#define VeraCoreAUDIO_H
#include "../3rdparty/portaudio/include/portaudio.h"
class VeraCoreAudio
{
public:
	VeraCoreAudio();
	~VeraCoreAudio();
	PaError OpenStreamASIO(double srate, int devnum);
	PaError OpenStreamWASAPI(double srate, int devnum);
	PaError CloseStream();
	PaStream* stream = nullptr;
};
#endif

