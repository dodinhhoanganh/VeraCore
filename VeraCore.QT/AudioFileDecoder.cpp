#include "AudioFileDecoder.h"
#include "NativeAudioCodecBackend.h"
#include "../VeraCore.Audio/AudioChannelLayout.h"

#include <QFile>
#include <QFileInfo>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
constexpr quint16 kWaveFormatPcm = 0x0001;
constexpr quint16 kWaveFormatIeeeFloat = 0x0003;
constexpr quint16 kWaveFormatExtensible = 0xfffe;

quint16 readLe16(const QByteArray& data, int offset)
{
    return static_cast<quint16>(static_cast<unsigned char>(data[offset])) |
        static_cast<quint16>(static_cast<unsigned char>(data[offset + 1]) << 8);
}

quint32 readLe32(const QByteArray& data, int offset)
{
    return static_cast<quint32>(static_cast<unsigned char>(data[offset])) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 1])) << 8) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 2])) << 16) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 3])) << 24);
}

quint16 readBe16(const QByteArray& data, int offset)
{
    return (static_cast<quint16>(static_cast<unsigned char>(data[offset])) << 8) |
        static_cast<quint16>(static_cast<unsigned char>(data[offset + 1]));
}

quint32 readBe32(const QByteArray& data, int offset)
{
    return (static_cast<quint32>(static_cast<unsigned char>(data[offset])) << 24) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 1])) << 16) |
        (static_cast<quint32>(static_cast<unsigned char>(data[offset + 2])) << 8) |
        static_cast<quint32>(static_cast<unsigned char>(data[offset + 3]));
}

double readAiffExtended80(const QByteArray& data, int offset)
{
    if (offset < 0 || offset + 9 >= data.size()) {
        return 0.0;
    }
    const quint16 exponentWord = readBe16(data, offset);
    const int sign = (exponentWord & 0x8000) ? -1 : 1;
    const int exponent = static_cast<int>(exponentWord & 0x7fff) - 16383;
    quint64 mantissa = 0;
    for (int i = 0; i < 8; ++i) {
        mantissa = (mantissa << 8) | static_cast<quint64>(static_cast<unsigned char>(data[offset + 2 + i]));
    }
    if (mantissa == 0 || exponentWord == 0) {
        return 0.0;
    }
    const long double normalized = static_cast<long double>(mantissa) / std::ldexp(1.0L, 63);
    return static_cast<double>(sign * normalized * std::ldexp(1.0L, exponent));
}

float clampSample(float value)
{
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, -1.0f, 1.0f);
}

float readPcmSample(const char* bytes, int bitsPerSample, quint16 formatTag)
{
    if (formatTag == kWaveFormatIeeeFloat && bitsPerSample == 32) {
        float value = 0.0f;
        memcpy(&value, bytes, sizeof(float));
        return clampSample(value);
    }
    if (formatTag == kWaveFormatIeeeFloat && bitsPerSample == 64) {
        double value = 0.0;
        memcpy(&value, bytes, sizeof(double));
        return std::isfinite(value) ? static_cast<float>(std::clamp(value, -1.0, 1.0)) : 0.0f;
    }

    if (formatTag != kWaveFormatPcm) {
        return 0.0f;
    }

    if (bitsPerSample == 8) {
        const float value = (static_cast<float>(static_cast<unsigned char>(bytes[0])) - 128.0f) / 128.0f;
        return clampSample(value);
    }
    if (bitsPerSample == 16) {
        const qint16 value = static_cast<qint16>(
            static_cast<unsigned char>(bytes[0]) |
            (static_cast<unsigned char>(bytes[1]) << 8));
        return clampSample(static_cast<float>(value) / 32768.0f);
    }
    if (bitsPerSample == 24) {
        qint32 value = static_cast<unsigned char>(bytes[0]) |
            (static_cast<unsigned char>(bytes[1]) << 8) |
            (static_cast<unsigned char>(bytes[2]) << 16);
        if (value & 0x00800000) {
            value -= 0x01000000;
        }
        return clampSample(static_cast<float>(value) / 8388608.0f);
    }
    if (bitsPerSample == 32) {
        const quint32 unsignedValue =
            static_cast<quint32>(static_cast<unsigned char>(bytes[0])) |
            (static_cast<quint32>(static_cast<unsigned char>(bytes[1])) << 8) |
            (static_cast<quint32>(static_cast<unsigned char>(bytes[2])) << 16) |
            (static_cast<quint32>(static_cast<unsigned char>(bytes[3])) << 24);
        const qint32 value = static_cast<qint32>(unsignedValue);
        return clampSample(static_cast<float>(value) / 2147483648.0f);
    }

    return 0.0f;
}

float readAiffSample(const char* bytes, int bitsPerSample, bool littleEndian, bool floatingPoint)
{
    if (floatingPoint && bitsPerSample == 32) {
        char ordered[4];
        if (littleEndian) {
            ordered[0] = bytes[0];
            ordered[1] = bytes[1];
            ordered[2] = bytes[2];
            ordered[3] = bytes[3];
        } else {
            ordered[0] = bytes[3];
            ordered[1] = bytes[2];
            ordered[2] = bytes[1];
            ordered[3] = bytes[0];
        }
        float value = 0.0f;
        memcpy(&value, ordered, sizeof(float));
        return clampSample(value);
    }

    if (bitsPerSample == 8) {
        const qint8 value = static_cast<qint8>(bytes[0]);
        return clampSample(static_cast<float>(value) / 128.0f);
    }

    auto byteAt = [&](int index) {
        return static_cast<quint32>(static_cast<unsigned char>(bytes[littleEndian ? index : (bitsPerSample / 8 - 1 - index)]));
    };
    if (bitsPerSample == 16) {
        const qint16 value = static_cast<qint16>(byteAt(0) | (byteAt(1) << 8));
        return clampSample(static_cast<float>(value) / 32768.0f);
    }
    if (bitsPerSample == 24) {
        qint32 value = static_cast<qint32>(byteAt(0) | (byteAt(1) << 8) | (byteAt(2) << 16));
        if (value & 0x00800000) {
            value -= 0x01000000;
        }
        return clampSample(static_cast<float>(value) / 8388608.0f);
    }
    if (bitsPerSample == 32) {
        const quint32 unsignedValue = byteAt(0) | (byteAt(1) << 8) | (byteAt(2) << 16) | (byteAt(3) << 24);
        const qint32 value = static_cast<qint32>(unsignedValue);
        return clampSample(static_cast<float>(value) / 2147483648.0f);
    }
    return 0.0f;
}

bool isWavPath(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == "wav" || suffix == "wave";
}

bool isAiffPath(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == "aif" || suffix == "aiff" || suffix == "aifc";
}
} // namespace

bool AudioFileDecoder::canDecodePath(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "wav" || suffix == "wave") {
        return probeWavPcm(path).success;
    }
    if (suffix == "aif" || suffix == "aiff" || suffix == "aifc") {
        return probeAiffPcm(path).success;
    }
    if (suffix == "mp3" || suffix == "flac" || suffix == "ogg" || suffix == "oga" ||
        suffix == "wv" || suffix == "wavpack") {
        return NativeAudioCodecBackend::canDecodePath(path);
    }
    return false;
}

QString AudioFileDecoder::unsupportedDecodeMessage(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "wav" || suffix == "wave") {
        return "WAV clip playback/render needs PCM, extensible PCM, or 32/64-bit float WAV audio.";
    }
    if (suffix == "mp3") {
        return "MP3 clip playback/render needs embedded FFmpeg or a linked LAME HIP decoder.";
    }
    if (suffix == "flac") {
        return "FLAC clip playback/render needs embedded FFmpeg or a linked libFLAC decoder.";
    }
    if (suffix == "ogg" || suffix == "oga") {
        return "OGG Vorbis clip playback/render needs embedded FFmpeg or linked libogg/libvorbis decoders.";
    }
    if (suffix == "wv" || suffix == "wavpack") {
        return "WavPack clip playback/render needs embedded FFmpeg or a linked WavPack decoder.";
    }
    if (suffix == "aif" || suffix == "aiff" || suffix == "aifc") {
        return "AIFF/AIFC clip playback/render needs uncompressed PCM, little-endian sowt PCM, or 32-bit float AIFF.";
    }
    if (suffix == "rex" || suffix == "rx2") {
        return "REX/RX2 clip playback/render is not wired yet; use WAV or an enabled native compressed backend.";
    }
    if (suffix == "mid" || suffix == "midi") {
        return "MIDI files import on MIDI tracks, not audio tracks.";
    }
    return "This audio format is not supported by the currently linked embedded decoder.";
}

AudioFileProbeResult AudioFileDecoder::probeAudio(const QString& path)
{
    if (isWavPath(path)) {
        return probeWavPcm(path);
    }
    if (isAiffPath(path)) {
        return probeAiffPcm(path);
    }
    if (!canDecodePath(path)) {
        return {false, unsupportedDecodeMessage(path), 0, 0};
    }

    return NativeAudioCodecBackend::probeFile(path);
}

AudioDecodeResult AudioFileDecoder::decodeAudio(const QString& path)
{
    if (isWavPath(path)) {
        return decodeWavPcm(path);
    }
    if (isAiffPath(path)) {
        return decodeAiffPcm(path);
    }
    if (!canDecodePath(path)) {
        return {false, unsupportedDecodeMessage(path), {}};
    }
    return NativeAudioCodecBackend::decodeFile(path);
}

AudioDecodeResult AudioFileDecoder::decodeWavPcm(const QString& path)
{
    AudioFileProbeResult probe = probeWavPcm(path);
    if (!probe.success) {
        return {false, probe.message, {}};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {false, "Could not open WAV file.", {}};
    }

    const QByteArray data = file.readAll();
    quint16 formatTag = 0;
    quint16 channels = 0;
    quint16 bitsPerSample = 0;
    int dataOffset = -1;
    quint32 dataSize = 0;

    qint64 offset = 12;
    while (offset + 8 <= data.size()) {
        const QByteArray chunkId = data.mid(offset, 4);
        const quint32 chunkSize = readLe32(data, static_cast<int>(offset + 4));
        const qint64 chunkDataOffset = offset + 8;
        if (chunkDataOffset + static_cast<qint64>(chunkSize) > data.size() ||
            chunkDataOffset > static_cast<qint64>(std::numeric_limits<int>::max())) {
            break;
        }
        const int chunkDataIndex = static_cast<int>(chunkDataOffset);

        if (chunkId == "fmt " && chunkSize >= 16) {
            formatTag = readLe16(data, chunkDataIndex);
            channels = readLe16(data, chunkDataIndex + 2);
            bitsPerSample = readLe16(data, chunkDataIndex + 14);
            if (formatTag == kWaveFormatExtensible && chunkSize >= 40) {
                const quint16 subFormatTag = readLe16(data, chunkDataIndex + 24);
                if (subFormatTag == kWaveFormatPcm || subFormatTag == kWaveFormatIeeeFloat) {
                    formatTag = subFormatTag;
                }
            }
        } else if (chunkId == "data") {
            dataOffset = chunkDataIndex;
            dataSize = chunkSize;
        }

        offset = chunkDataOffset + static_cast<qint64>(chunkSize) + (chunkSize % 2);
    }

    const int bytesPerSample = bitsPerSample / 8;
    const int bytesPerFrame = bytesPerSample * channels;
    if ((formatTag != kWaveFormatPcm && formatTag != kWaveFormatIeeeFloat) ||
        dataOffset < 0 || dataSize == 0 ||
        bytesPerSample <= 0 || bytesPerFrame <= 0) {
        return {false, "Invalid WAV data layout.", {}};
    }

    const qint64 availableBytes = static_cast<qint64>(data.size()) - static_cast<qint64>(dataOffset);
    const qint64 requestedBytes = static_cast<qint64>(probe.frameCount) * static_cast<qint64>(bytesPerFrame);
    if (availableBytes < requestedBytes || static_cast<qint64>(dataSize) < requestedBytes) {
        return {false, "WAV data chunk is shorter than expected.", {}};
    }

    DecodedAudioFile decoded;
    decoded.sampleRate = probe.sampleRate;
    decoded.sourceChannels = channels;
    decoded.stereoSamples.reserve(static_cast<size_t>(probe.frameCount) * 2u);

    for (int frame = 0; frame < probe.frameCount; ++frame) {
        const qint64 frameOffset64 = static_cast<qint64>(dataOffset) +
            static_cast<qint64>(frame) * static_cast<qint64>(bytesPerFrame);
        if (frameOffset64 < 0 ||
            frameOffset64 + static_cast<qint64>(bytesPerFrame) > static_cast<qint64>(data.size()) ||
            frameOffset64 > static_cast<qint64>(std::numeric_limits<int>::max())) {
            return {false, "WAV sample offset is outside the decoded buffer.", {}};
        }
        const int frameOffset = static_cast<int>(frameOffset64);
        const float left = readPcmSample(data.constData() + frameOffset, bitsPerSample, formatTag);
        const float right = channels > 1
            ? readPcmSample(data.constData() + frameOffset + bytesPerSample, bitsPerSample, formatTag)
            : left;
        decoded.stereoSamples.push_back(left);
        decoded.stereoSamples.push_back(right);
    }

    if (decoded.stereoSamples.isEmpty()) {
        return {false, "WAV file has no audio frames.", {}};
    }
    return {true, QFileInfo(path).fileName(), decoded};
}

AudioFileProbeResult AudioFileDecoder::probeWavPcm(const QString& path)
{
    if (!isWavPath(path)) {
        return {false, "probeWavPcm only accepts WAV/WAVE files.", 0, 0};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {false, "Could not open WAV file.", 0, 0};
    }

    const qint64 fileSize = file.size();
    const QByteArray riffHeader = file.read(12);
    if (fileSize < 44 || riffHeader.size() != 12 ||
        riffHeader.mid(0, 4) != "RIFF" || riffHeader.mid(8, 4) != "WAVE") {
        return {false, "Unsupported WAV header.", 0, 0};
    }

    quint16 formatTag = 0;
    quint16 channels = 0;
    quint32 sampleRate = 0;
    quint16 bitsPerSample = 0;
    quint64 channelMask = 0;
    int dataOffset = -1;
    quint32 dataSize = 0;

    qint64 offset = 12;
    while (offset + 8 <= fileSize) {
        if (!file.seek(offset)) {
            break;
        }
        const QByteArray chunkHeader = file.read(8);
        if (chunkHeader.size() != 8) {
            break;
        }
        const QByteArray chunkId = chunkHeader.mid(0, 4);
        const quint32 chunkSize = readLe32(chunkHeader, 4);
        const qint64 chunkDataOffset = offset + 8;
        if (chunkDataOffset + static_cast<qint64>(chunkSize) > fileSize) {
            break;
        }
        const bool needsPayload = chunkId == "fmt ";
        const qint64 payloadBytesToRead = needsPayload ? qMin<qint64>(static_cast<qint64>(chunkSize), 40) : 0;
        const QByteArray chunkData = needsPayload ? file.read(payloadBytesToRead) : QByteArray();

        if (chunkId == "fmt " && chunkSize >= 16 && chunkData.size() >= 16) {
            formatTag = readLe16(chunkData, 0);
            channels = readLe16(chunkData, 2);
            sampleRate = readLe32(chunkData, 4);
            bitsPerSample = readLe16(chunkData, 14);
            if (formatTag == kWaveFormatExtensible && chunkSize >= 40) {
                if (chunkData.size() < 40) {
                    break;
                }
                channelMask = readLe32(chunkData, 20);
                const quint16 subFormatTag = readLe16(chunkData, 24);
                if (subFormatTag == kWaveFormatPcm || subFormatTag == kWaveFormatIeeeFloat) {
                    formatTag = subFormatTag;
                }
            }
        } else if (chunkId == "data") {
            if (chunkDataOffset > static_cast<qint64>(std::numeric_limits<int>::max())) {
                break;
            }
            dataOffset = static_cast<int>(chunkDataOffset);
            dataSize = chunkSize;
        }

        offset = chunkDataOffset + static_cast<qint64>(chunkSize) + (chunkSize % 2);
    }

    if ((formatTag != kWaveFormatPcm && formatTag != kWaveFormatIeeeFloat) || channels == 0 || sampleRate == 0 ||
        dataOffset < 0 || dataSize == 0) {
        return {false, "WAV must be PCM, extensible PCM, or 32-bit float with a data chunk.", 0, 0};
    }
    const bool supportedPcmBits = formatTag == kWaveFormatPcm &&
        (bitsPerSample == 8 || bitsPerSample == 16 || bitsPerSample == 24 || bitsPerSample == 32);
    const bool supportedFloatBits = formatTag == kWaveFormatIeeeFloat &&
        (bitsPerSample == 32 || bitsPerSample == 64);
    if (!supportedPcmBits && !supportedFloatBits) {
        return {false, "Only 8/16/24/32-bit PCM and 32/64-bit float WAV samples are supported.", 0, 0};
    }
    if (channelMask != 0 && !VeraCoreIsWaveSpeakerMaskValid(channelMask, channels)) {
        return {false, "WAV speaker mask does not match its channel count.", 0, 0};
    }

    const int bytesPerSample = bitsPerSample / 8;
    const int bytesPerFrame = bytesPerSample * channels;
    if (bytesPerFrame <= 0) {
        return {false, "Invalid WAV channel layout.", 0, 0};
    }

    const qint64 frameCount = static_cast<qint64>(dataSize) / static_cast<qint64>(bytesPerFrame);
    if (frameCount <= 0) {
        return {false, "WAV file has no audio frames.", 0, 0};
    }
    if (frameCount > static_cast<qint64>(std::numeric_limits<int>::max())) {
        return {false, "WAV file is too large for the current decoder.", 0, 0};
    }
    if (channelMask == 0) {
        channelMask = VeraCoreCanonicalWaveSpeakerMask(channels);
    }
    return {true, QFileInfo(path).fileName(), static_cast<int>(sampleRate),
            static_cast<int>(frameCount), static_cast<int>(channels), channelMask};
}

AudioFileProbeResult AudioFileDecoder::probeAiffPcm(const QString& path)
{
    if (!isAiffPath(path)) {
        return {false, "probeAiffPcm only accepts AIFF/AIFC files.", 0, 0};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {false, "Could not open AIFF file.", 0, 0};
    }

    const QByteArray data = file.readAll();
    if (data.size() < 54 || data.mid(0, 4) != "FORM" ||
        (data.mid(8, 4) != "AIFF" && data.mid(8, 4) != "AIFC")) {
        return {false, "Unsupported AIFF header.", 0, 0};
    }

    quint16 channels = 0;
    quint32 frameCount = 0;
    quint16 bitsPerSample = 0;
    int sampleRate = 0;
    bool hasSsnd = false;
    bool supportedEncoding = data.mid(8, 4) == "AIFF";
    bool floatingPoint = false;

    qint64 offset = 12;
    while (offset + 8 <= data.size()) {
        const QByteArray chunkId = data.mid(offset, 4);
        const quint32 chunkSize = readBe32(data, static_cast<int>(offset + 4));
        const qint64 chunkDataOffset = offset + 8;
        if (chunkDataOffset + static_cast<qint64>(chunkSize) > data.size() ||
            chunkDataOffset > static_cast<qint64>(std::numeric_limits<int>::max())) {
            break;
        }
        const int chunkDataIndex = static_cast<int>(chunkDataOffset);
        if (chunkId == "COMM" && chunkSize >= 18) {
            channels = readBe16(data, chunkDataIndex);
            frameCount = readBe32(data, chunkDataIndex + 2);
            bitsPerSample = readBe16(data, chunkDataIndex + 6);
            sampleRate = static_cast<int>(std::lround(readAiffExtended80(data, chunkDataIndex + 8)));
            if (data.mid(8, 4) == "AIFC" && chunkSize >= 22) {
                const QByteArray compression = data.mid(chunkDataIndex + 18, 4);
                floatingPoint = compression == "fl32" || compression == "FL32";
                supportedEncoding = compression == "NONE" || compression == "twos" ||
                    compression == "sowt" || floatingPoint;
            }
        } else if (chunkId == "SSND" && chunkSize >= 8) {
            hasSsnd = true;
        }
        offset = chunkDataOffset + static_cast<qint64>(chunkSize) + (chunkSize % 2);
    }

    if (!supportedEncoding || channels == 0 || frameCount == 0 || sampleRate <= 0 || !hasSsnd) {
        return {false, "AIFF must be uncompressed PCM, little-endian sowt PCM, or 32-bit float with COMM and SSND chunks.", 0, 0};
    }
    if (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) {
        return {false, "Only 8/16/24/32-bit AIFF samples are supported.", 0, 0};
    }
    if (floatingPoint && bitsPerSample != 32) {
        return {false, "Only 32-bit float AIFF/AIFC samples are supported.", 0, 0};
    }
    if (frameCount > static_cast<quint32>(std::numeric_limits<int>::max())) {
        return {false, "AIFF file is too large for the current decoder.", 0, 0};
    }
    return {true, QFileInfo(path).fileName(), sampleRate,
            static_cast<int>(frameCount), static_cast<int>(channels)};
}

AudioDecodeResult AudioFileDecoder::decodeAiffPcm(const QString& path)
{
    AudioFileProbeResult probe = probeAiffPcm(path);
    if (!probe.success) {
        return {false, probe.message, {}};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {false, "Could not open AIFF file.", {}};
    }

    const QByteArray data = file.readAll();
    quint16 channels = 0;
    quint16 bitsPerSample = 0;
    bool littleEndian = false;
    bool floatingPoint = false;
    int soundOffset = -1;
    quint32 soundSize = 0;
    quint32 blockOffset = 0;

    qint64 offset = 12;
    while (offset + 8 <= data.size()) {
        const QByteArray chunkId = data.mid(offset, 4);
        const quint32 chunkSize = readBe32(data, static_cast<int>(offset + 4));
        const qint64 chunkDataOffset = offset + 8;
        if (chunkDataOffset + static_cast<qint64>(chunkSize) > data.size() ||
            chunkDataOffset > static_cast<qint64>(std::numeric_limits<int>::max())) {
            break;
        }
        const int chunkDataIndex = static_cast<int>(chunkDataOffset);
        if (chunkId == "COMM" && chunkSize >= 18) {
            channels = readBe16(data, chunkDataIndex);
            bitsPerSample = readBe16(data, chunkDataIndex + 6);
            if (data.mid(8, 4) == "AIFC" && chunkSize >= 22) {
                const QByteArray compression = data.mid(chunkDataIndex + 18, 4);
                littleEndian = compression == "sowt";
                floatingPoint = compression == "fl32" || compression == "FL32";
            }
        } else if (chunkId == "SSND" && chunkSize >= 8) {
            blockOffset = readBe32(data, chunkDataIndex);
            soundOffset = chunkDataIndex + 8;
            soundSize = chunkSize - 8;
        }
        offset = chunkDataOffset + static_cast<qint64>(chunkSize) + (chunkSize % 2);
    }

    const int bytesPerSample = bitsPerSample / 8;
    const int bytesPerFrame = bytesPerSample * channels;
    if (soundOffset < 0 || bytesPerSample <= 0 || bytesPerFrame <= 0) {
        return {false, "Invalid AIFF data layout.", {}};
    }
    const qint64 audioStart = static_cast<qint64>(soundOffset) + static_cast<qint64>(blockOffset);
    const qint64 requestedBytes = static_cast<qint64>(probe.frameCount) * static_cast<qint64>(bytesPerFrame);
    if (audioStart < 0 || audioStart + requestedBytes > data.size() ||
        static_cast<qint64>(blockOffset) + requestedBytes > static_cast<qint64>(soundSize)) {
        return {false, "AIFF sound chunk is shorter than expected.", {}};
    }

    DecodedAudioFile decoded;
    decoded.sampleRate = probe.sampleRate;
    decoded.sourceChannels = channels;
    decoded.stereoSamples.reserve(static_cast<size_t>(probe.frameCount) * 2u);
    for (int frame = 0; frame < probe.frameCount; ++frame) {
        const qint64 frameOffset64 = audioStart + static_cast<qint64>(frame) * static_cast<qint64>(bytesPerFrame);
        if (frameOffset64 < 0 ||
            frameOffset64 + static_cast<qint64>(bytesPerFrame) > static_cast<qint64>(data.size()) ||
            frameOffset64 > static_cast<qint64>(std::numeric_limits<int>::max())) {
            return {false, "AIFF sample offset is outside the decoded buffer.", {}};
        }
        const int frameOffset = static_cast<int>(frameOffset64);
        const float left = readAiffSample(data.constData() + frameOffset, bitsPerSample, littleEndian, floatingPoint);
        const float right = channels > 1
            ? readAiffSample(data.constData() + frameOffset + bytesPerSample, bitsPerSample, littleEndian, floatingPoint)
            : left;
        decoded.stereoSamples.push_back(left);
        decoded.stereoSamples.push_back(right);
    }

    if (decoded.stereoSamples.isEmpty()) {
        return {false, "AIFF file has no audio frames.", {}};
    }
    return {true, QFileInfo(path).fileName(), decoded};
}
