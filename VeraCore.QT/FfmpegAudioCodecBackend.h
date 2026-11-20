#pragma once

#include "AudioCodecBackend.h"
#include "AudioFileDecoder.h"

#include <QString>

class FfmpegAudioCodecBackend
{
public:
    static bool isAvailable();
    static bool canEncode(AudioCodecFormat format);
    static int preferredEncoderSampleRate(AudioCodecFormat format, int requestedSampleRate);
    static bool canDecodePath(const QString& path);
    static AudioFileProbeResult probeFile(const QString& path);
    static AudioCodecResult encodeFromWav(const QString& sourceWav, const QString& destinationPath,
                                          AudioCodecFormat format, const AudioEncodeOptions& options = {},
                                          const AudioEncodeProgress& progress = {});
    static AudioDecodeResult decodeFile(const QString& path);
};
