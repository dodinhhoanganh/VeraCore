#include "../VeraCore.QT/FfmpegAudioCodecBackend.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QString>

#include <array>
#include <iostream>

QString AudioCodecBackend::displayName(AudioCodecFormat format)
{
    switch (format) {
    case AudioCodecFormat::Mp3: return "MP3";
    case AudioCodecFormat::Flac: return "FLAC";
    case AudioCodecFormat::OggVorbis: return "Ogg Vorbis";
    case AudioCodecFormat::WavPack: return "WavPack";
    default: return "Audio";
    }
}

namespace
{
bool writeSpatialFloatWav(const QString& path, quint16 channels, quint32 mask)
{
    constexpr quint32 sampleRate = 48000;
    constexpr quint16 bitsPerSample = 32;
    constexpr quint32 frames = 4096;
    const quint16 blockAlign = static_cast<quint16>(channels * sizeof(float));
    const quint32 dataBytes = frames * blockAlign;
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
    static constexpr char floatSubFormat[16] = {
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        static_cast<char>(0x80), 0x00, 0x00, static_cast<char>(0xaa),
        0x00, 0x38, static_cast<char>(0x9b), 0x71
    };
    out.writeRawData(floatSubFormat, 16);
    out.writeRawData("data", 4);
    out << dataBytes;
    for (quint32 frame = 0; frame < frames; ++frame) {
        const float polarity = (frame & 1u) == 0 ? 1.0f : -1.0f;
        for (quint16 channel = 0; channel < channels; ++channel) {
            const float sample = polarity * 0.01f * static_cast<float>(channel + 1);
            out.writeRawData(reinterpret_cast<const char*>(&sample), sizeof(sample));
        }
    }
    return out.status() == QDataStream::Ok;
}

bool encodeAndVerify(const QString& source, const QString& destination,
                     AudioCodecFormat format, int channels, quint64 mask)
{
    if (!FfmpegAudioCodecBackend::canEncode(format)) return false;
    AudioEncodeOptions options;
    options.lossyBitrateKbps = 320;
    options.vorbisQuality = 8;
    options.compressionLevel = 5;
    const AudioCodecResult encoded = FfmpegAudioCodecBackend::encodeFromWav(
        source, destination, format, options);
    if (!encoded.success) {
        std::cerr << encoded.message.toStdString() << '\n';
        return false;
    }
    const AudioFileProbeResult probe = FfmpegAudioCodecBackend::probeFile(destination);
    if (!probe.success) std::cerr << probe.message.toStdString() << '\n';
    return probe.success && probe.sampleRate == 48000 &&
        probe.channelCount == channels && probe.channelMask == mask;
}
}

int main(int argc, char** argv)
{
    if (argc != 2 || !FfmpegAudioCodecBackend::isAvailable()) return 10;
    const QDir output(QString::fromLocal8Bit(argv[1]));
    if (!output.exists()) return 11;
    const QString source51 = output.filePath("source-51.wav");
    const QString source71 = output.filePath("source-71.wav");
    if (!writeSpatialFloatWav(source51, 6, 0x60fu) ||
        !writeSpatialFloatWav(source71, 8, 0x63fu)) return 12;
    if (!encodeAndVerify(source51, output.filePath("layout-51.flac"),
                        AudioCodecFormat::Flac, 6, 0x60fu)) return 20;
    if (!encodeAndVerify(source71, output.filePath("layout-71.ogg"),
                        AudioCodecFormat::OggVorbis, 8, 0x63fu)) return 21;
    if (!encodeAndVerify(source71, output.filePath("layout-71.wv"),
                        AudioCodecFormat::WavPack, 8, 0x63fu)) return 22;
    const AudioCodecResult mp3 = FfmpegAudioCodecBackend::encodeFromWav(
        source51, output.filePath("must-reject.mp3"), AudioCodecFormat::Mp3);
    if (mp3.success || !mp3.message.contains("at most 2 channels")) return 23;
    std::cout << "COMPRESSED_LAYOUT_TEST=PASS VECTORS=4 FLAC_51=PASS VORBIS_71=PASS WAVPACK_71=PASS MP3_SPATIAL=REJECTED\n";
    return 0;
}
