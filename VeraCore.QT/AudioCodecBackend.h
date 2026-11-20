#pragma once

#include <QString>
#include <QStringList>
#include <functional>

enum class AudioCodecFormat
{
    Wav,
    Mp3,
    Flac,
    OggVorbis,
    WavPack,
    Unsupported
};

struct AudioCodecResult
{
    bool success = false;
    QString message;
    bool cancelled = false;
};

using AudioEncodeProgress = std::function<bool(qint64 completedFrames, qint64 totalFrames)>;

struct AudioEncodeOptions
{
    int lossyBitrateKbps = 256;
    int vorbisQuality = 6;
    int compressionLevel = 5;
    int losslessBitDepth = 24;
    int wavBitDepth = 32;
    bool wavFloatingPoint = true;
    bool dither = false;
};

class AudioCodecBackend
{
public:
    static AudioCodecFormat formatFromId(const QString& format);
    static QString displayName(AudioCodecFormat format);
    static QString fileSuffix(AudioCodecFormat format);
    static QString fileDialogFilter(AudioCodecFormat format);
    static QStringList nativeCapabilitySummary();
    static bool isNativeDecoderAvailable(AudioCodecFormat format);
    static QString nativeEncoderRequirement(AudioCodecFormat format);
    static bool isNativeEncoderAvailable(AudioCodecFormat format);
    static int maximumEncoderChannels(AudioCodecFormat format);
    static int preferredEncoderSampleRate(AudioCodecFormat format, int requestedSampleRate);
    static bool hasValidContainerHeader(const QString& path, AudioCodecFormat format,
                                        QString* errorText = nullptr);
    static AudioCodecResult encodeFromWav(const QString& sourceWav, const QString& destinationPath,
                                          AudioCodecFormat format, const AudioEncodeOptions& options = {},
                                          const AudioEncodeProgress& progress = {});
};
