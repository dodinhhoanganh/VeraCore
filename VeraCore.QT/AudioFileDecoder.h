#pragma once

#include <QVector>
#include <QString>

struct DecodedAudioFile
{
    int sampleRate = 0;
    int sourceChannels = 2;
    QVector<float> stereoSamples;

    int frameCount() const
    {
        return stereoSamples.size() / 2;
    }
};

struct AudioDecodeResult
{
    bool success = false;
    QString message;
    DecodedAudioFile audio;
};

struct AudioFileProbeResult
{
    bool success = false;
    QString message;
    int sampleRate = 0;
    int frameCount = 0;
    int channelCount = 0;
    quint64 channelMask = 0;

    double durationSeconds() const
    {
        return sampleRate > 0 ? static_cast<double>(frameCount) / static_cast<double>(sampleRate) : 0.0;
    }
};

class AudioFileDecoder
{
public:
    static bool canDecodePath(const QString& path);
    static QString unsupportedDecodeMessage(const QString& path);
    static AudioFileProbeResult probeAudio(const QString& path);
    static AudioDecodeResult decodeAudio(const QString& path);
    static AudioFileProbeResult probeWavPcm(const QString& path);
    static AudioDecodeResult decodeWavPcm(const QString& path);
    static AudioFileProbeResult probeAiffPcm(const QString& path);
    static AudioDecodeResult decodeAiffPcm(const QString& path);
};
