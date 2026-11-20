#include "../VeraCore.QT/AudioFileDecoder.h"
#include "../VeraCore.QT/NativeAudioCodecBackend.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QString>

#include <array>
#include <iostream>

bool NativeAudioCodecBackend::canDecodePath(const QString&) { return false; }
AudioFileProbeResult NativeAudioCodecBackend::probeFile(const QString&) { return {}; }
AudioDecodeResult NativeAudioCodecBackend::decodeFile(const QString&) { return {}; }

namespace
{
bool writeExtensibleWav(const QString& path, quint16 channels, quint32 mask)
{
    constexpr quint32 sampleRate = 48000;
    constexpr quint16 bitsPerSample = 32;
    const quint16 blockAlign = static_cast<quint16>(channels * sizeof(float));
    const quint32 dataBytes = blockAlign;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32(60u + dataBytes);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(40) << quint16(0xfffe) << channels << sampleRate
        << quint32(sampleRate * blockAlign) << blockAlign << bitsPerSample
        << quint16(22) << bitsPerSample << mask;
    static constexpr char pcmSubFormat[16] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        static_cast<char>(0x80), 0x00, 0x00, static_cast<char>(0xaa),
        0x00, 0x38, static_cast<char>(0x9b), 0x71
    };
    out.writeRawData(pcmSubFormat, 16);
    out.writeRawData("data", 4);
    out << dataBytes;
    const QByteArray silence(static_cast<qsizetype>(dataBytes), '\0');
    out.writeRawData(silence.constData(), silence.size());
    return out.status() == QDataStream::Ok;
}

bool writeLegacyPcmWav(const QString& path, quint16 channels)
{
    constexpr quint32 sampleRate = 48000;
    constexpr quint16 bitsPerSample = 16;
    const quint16 blockAlign = static_cast<quint16>(channels * sizeof(qint16));
    const quint32 dataBytes = blockAlign;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32(36u + dataBytes);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << channels << sampleRate
        << quint32(sampleRate * blockAlign) << blockAlign << bitsPerSample;
    out.writeRawData("data", 4);
    out << dataBytes;
    const QByteArray silence(static_cast<qsizetype>(dataBytes), '\0');
    out.writeRawData(silence.constData(), silence.size());
    return out.status() == QDataStream::Ok;
}

bool expectLayout(const QString& path, int channels, quint64 mask)
{
    const AudioFileProbeResult result = AudioFileDecoder::probeWavPcm(path);
    return result.success && result.sampleRate == 48000 && result.frameCount == 1 &&
        result.channelCount == channels && result.channelMask == mask;
}
}

int main(int argc, char** argv)
{
    if (argc != 2) return 10;
    const QDir output(QString::fromLocal8Bit(argv[1]));
    if (!output.exists()) return 11;
    struct Vector { const char* name; quint16 channels; quint32 mask; };
    constexpr std::array<Vector, 5> valid{{
        {"surround51.wav", 6, 0x0000060fu},
        {"surround71.wav", 8, 0x0000063fu},
        {"atmos712.wav", 10, 0x0000563fu},
        {"atmos714.wav", 12, 0x0002d63fu},
        {"quad-custom.wav", 4, 0x00000033u}
    }};
    for (const Vector& vector : valid) {
        const QString path = output.filePath(QString::fromLatin1(vector.name));
        if (!writeExtensibleWav(path, vector.channels, vector.mask) ||
            !expectLayout(path, vector.channels, vector.mask)) return 20;
    }
    const QString malformed = output.filePath("malformed-mask.wav");
    if (!writeExtensibleWav(malformed, 6, 0x3u)) return 21;
    const AudioFileProbeResult malformedProbe = AudioFileDecoder::probeWavPcm(malformed);
    if (malformedProbe.success || !malformedProbe.message.contains("speaker mask")) return 22;
    const QString stereo = output.filePath("legacy-stereo.wav");
    if (!writeLegacyPcmWav(stereo, 2) || !expectLayout(stereo, 2, 0x3u)) return 23;
    const QString ambiguous = output.filePath("legacy-quad.wav");
    if (!writeLegacyPcmWav(ambiguous, 4) || !expectLayout(ambiguous, 4, 0)) return 24;
    std::cout << "WAV_PROBE_LAYOUT_TEST=PASS VECTORS=8 CANONICAL=4 CUSTOM=1 LEGACY=2 REJECTED=1\n";
    return 0;
}
