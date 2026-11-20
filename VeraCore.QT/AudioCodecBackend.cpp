#include "AudioCodecBackend.h"
#include "AudioFileDecoder.h"
#include "AudioStretchBackend.h"
#include "NativeAudioCodecBackend.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>

namespace
{
void appendLe16(QByteArray& bytes, quint16 value)
{
    bytes.append(static_cast<char>(value & 0xffu));
    bytes.append(static_cast<char>((value >> 8) & 0xffu));
}

void appendLe32(QByteArray& bytes, quint32 value)
{
    bytes.append(static_cast<char>(value & 0xffu));
    bytes.append(static_cast<char>((value >> 8) & 0xffu));
    bytes.append(static_cast<char>((value >> 16) & 0xffu));
    bytes.append(static_cast<char>((value >> 24) & 0xffu));
}

int wavChannelCount(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    const QByteArray riff = file.read(12);
    if (riff.size() != 12 || riff.mid(0, 4) != "RIFF" || riff.mid(8, 4) != "WAVE")
        return 0;
    auto little16 = [](const char* bytes) {
        return static_cast<quint16>(static_cast<unsigned char>(bytes[0])) |
            static_cast<quint16>(static_cast<unsigned char>(bytes[1]) << 8);
    };
    auto little32 = [](const char* bytes) {
        return static_cast<quint32>(static_cast<unsigned char>(bytes[0])) |
            (static_cast<quint32>(static_cast<unsigned char>(bytes[1])) << 8) |
            (static_cast<quint32>(static_cast<unsigned char>(bytes[2])) << 16) |
            (static_cast<quint32>(static_cast<unsigned char>(bytes[3])) << 24);
    };
    qint64 cursor = 12;
    while (cursor + 8 <= file.size()) {
        if (!file.seek(cursor)) return 0;
        const QByteArray header = file.read(8);
        if (header.size() != 8) return 0;
        const quint32 chunkBytes = little32(header.constData() + 4);
        const qint64 payloadOffset = cursor + 8;
        if (payloadOffset + static_cast<qint64>(chunkBytes) > file.size()) return 0;
        if (header.left(4) == "fmt " && chunkBytes >= 16) {
            if (!file.seek(payloadOffset)) return 0;
            const QByteArray format = file.read(16);
            return format.size() == 16 ? little16(format.constData() + 2) : 0;
        }
        cursor = payloadOffset + static_cast<qint64>(chunkBytes) + (chunkBytes & 1u);
    }
    return 0;
}

float nextDitherUniform(quint32& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
}

AudioCodecResult writeIntegerPcmWav(const QString& sourceWav, const QString& destinationPath,
                                    int bitDepth, bool dither, const AudioEncodeProgress& progress)
{
    if (bitDepth != 16 && bitDepth != 24)
        return {false, "Integer WAV export supports 16-bit or 24-bit PCM."};

    const auto little16 = [](const char* bytes) {
        return static_cast<quint16>(static_cast<unsigned char>(bytes[0])) |
            static_cast<quint16>(static_cast<unsigned char>(bytes[1]) << 8);
    };
    const auto little32 = [](const char* bytes) {
        return static_cast<quint32>(static_cast<unsigned char>(bytes[0])) |
            (static_cast<quint32>(static_cast<unsigned char>(bytes[1])) << 8) |
            (static_cast<quint32>(static_cast<unsigned char>(bytes[2])) << 16) |
            (static_cast<quint32>(static_cast<unsigned char>(bytes[3])) << 24);
    };
    QFile input(sourceWav);
    if (!input.open(QIODevice::ReadOnly))
        return {false, "Could not open temporary WAV."};
    const QByteArray riff = input.read(12);
    if (riff.size() != 12 || riff.mid(0, 4) != "RIFF" || riff.mid(8, 4) != "WAVE")
        return {false, "Temporary master is not a valid RIFF/WAVE file."};

    quint16 channels = 0;
    quint16 sourceBlockAlign = 0;
    quint16 sourceBits = 0;
    quint32 sampleRate = 0;
    quint32 speakerMask = 0;
    bool sourceFloat32 = false;
    qint64 dataOffset = -1;
    quint32 sourceDataBytes = 0;
    qint64 cursor = 12;
    while (cursor + 8 <= input.size()) {
        if (!input.seek(cursor)) return {false, "Could not seek in temporary WAV."};
        const QByteArray chunkHeader = input.read(8);
        if (chunkHeader.size() != 8) return {false, "Temporary WAV has a truncated chunk header."};
        const quint32 chunkBytes = little32(chunkHeader.constData() + 4);
        const qint64 payloadOffset = cursor + 8;
        if (payloadOffset + static_cast<qint64>(chunkBytes) > input.size())
            return {false, "Temporary WAV has a truncated chunk."};
        if (chunkHeader.left(4) == "fmt " && chunkBytes >= 16) {
            if (!input.seek(payloadOffset)) return {false, "Could not read temporary WAV format."};
            const QByteArray format = input.read(std::min<quint32>(chunkBytes, 40u));
            if (format.size() < 16) return {false, "Temporary WAV format is truncated."};
            const quint16 formatTag = little16(format.constData());
            channels = little16(format.constData() + 2);
            sampleRate = little32(format.constData() + 4);
            sourceBlockAlign = little16(format.constData() + 12);
            sourceBits = little16(format.constData() + 14);
            const bool ieeeFloat = formatTag == 3;
            const bool extensibleFloat = formatTag == 0xFFFE && format.size() >= 40 &&
                little16(format.constData() + 16) >= 22 &&
                little32(format.constData() + 24) == 3;
            if (formatTag == 0xFFFE && format.size() >= 24)
                speakerMask = little32(format.constData() + 20);
            sourceFloat32 = (ieeeFloat || extensibleFloat) && sourceBits == 32;
        } else if (chunkHeader.left(4) == "data") {
            dataOffset = payloadOffset;
            sourceDataBytes = chunkBytes;
        }
        cursor = payloadOffset + static_cast<qint64>(chunkBytes) + (chunkBytes & 1u);
    }
    const quint32 expectedSourceBlockAlign = static_cast<quint32>(channels) * 4u;
    if (!sourceFloat32 || channels == 0 || channels > 64 || sampleRate < 8000 ||
        sampleRate > 384000 || sourceBlockAlign != expectedSourceBlockAlign ||
        dataOffset < 0 || sourceDataBytes == 0 ||
        (sourceDataBytes % expectedSourceBlockAlign) != 0) {
        return {false, "Temporary master must be interleaved 32-bit IEEE-float WAV audio."};
    }

    const quint16 bytesPerSample = static_cast<quint16>(bitDepth / 8);
    const quint16 blockAlign = channels * bytesPerSample;
    const qint64 frameCount = sourceDataBytes / expectedSourceBlockAlign;
    const quint64 dataBytes64 = static_cast<quint64>(frameCount) * blockAlign;
    const bool extensible = channels > 2;
    if (extensible && speakerMask == 0) {
        switch (channels) {
        case 6: speakerMask = 0x0000060Fu; break;  // 5.1
        case 8: speakerMask = 0x0000063Fu; break;  // 7.1
        case 10: speakerMask = 0x0000563Fu; break; // 7.1.2
        case 12: speakerMask = 0x0002D63Fu; break; // 7.1.4
        default: break;
        }
    }
    const quint32 riffOverhead = extensible ? 60u : 36u;
    if (dataBytes64 > std::numeric_limits<quint32>::max() - riffOverhead)
        return {false, "WAV exceeds the RIFF 4 GiB size limit."};
    const quint32 dataBytes = static_cast<quint32>(dataBytes64);

    QSaveFile output(destinationPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
        return {false, "Could not create WAV output."};

    QByteArray header;
    header.reserve(extensible ? 68 : 44);
    header.append("RIFF", 4);
    appendLe32(header, riffOverhead + dataBytes);
    header.append("WAVEfmt ", 8);
    appendLe32(header, extensible ? 40u : 16u);
    appendLe16(header, extensible ? 0xFFFEu : 1u);
    appendLe16(header, channels);
    appendLe32(header, sampleRate);
    appendLe32(header, sampleRate * blockAlign);
    appendLe16(header, blockAlign);
    appendLe16(header, static_cast<quint16>(bitDepth));
    if (extensible) {
        appendLe16(header, 22u);
        appendLe16(header, static_cast<quint16>(bitDepth));
        appendLe32(header, speakerMask);
        appendLe32(header, 1u); // KSDATAFORMAT_SUBTYPE_PCM
        appendLe16(header, 0u);
        appendLe16(header, 0x0010u);
        const char guidTail[8] = {
            static_cast<char>(0x80), 0x00, 0x00, static_cast<char>(0xAA),
            0x00, 0x38, static_cast<char>(0x9B), 0x71};
        header.append(guidTail, 8);
    }
    header.append("data", 4);
    appendLe32(header, dataBytes);
    if (output.write(header) != header.size())
        return {false, "Could not write WAV header."};

    constexpr int framesPerBlock = 4096;
    QByteArray pcm;
    pcm.reserve(framesPerBlock * blockAlign);
    quint32 randomState = 0x6d2b79f5u;
    const qint64 minimum = bitDepth == 16 ? -32768 : -8388608;
    const qint64 maximum = bitDepth == 16 ? 32767 : 8388607;
    const double scale = bitDepth == 16 ? 32768.0 : 8388608.0;
    for (qint64 frameBase = 0; frameBase < frameCount; frameBase += framesPerBlock) {
        if (progress && !progress(frameBase, frameCount)) {
            output.cancelWriting();
            return {false, "Encoding cancelled.", true};
        }
        pcm.clear();
        const qint64 blockEnd = std::min<qint64>(frameCount, frameBase + framesPerBlock);
        const qint64 sourceBytes = (blockEnd - frameBase) * sourceBlockAlign;
        if (!input.seek(dataOffset + frameBase * sourceBlockAlign)) {
            output.cancelWriting();
            return {false, "Could not seek in temporary WAV sample data."};
        }
        const QByteArray sourceBlock = input.read(sourceBytes);
        if (sourceBlock.size() != sourceBytes) {
            output.cancelWriting();
            return {false, "Could not read temporary WAV sample data."};
        }
        for (qint64 frame = frameBase; frame < blockEnd; ++frame) {
            for (int channel = 0; channel < channels; ++channel) {
                const qint64 localFrame = frame - frameBase;
                const qint64 sourceOffset = localFrame * sourceBlockAlign + channel * 4;
                const quint32 sampleBits = little32(sourceBlock.constData() + sourceOffset);
                float sample = 0.0f;
                std::memcpy(&sample, &sampleBits, sizeof(sample));
                double noise = 0.0;
                if (dither) {
                    const float first = nextDitherUniform(randomState);
                    const float second = nextDitherUniform(randomState);
                    noise = static_cast<double>(first - second);
                }
                const double finiteSample = std::isfinite(sample)
                    ? std::clamp(static_cast<double>(sample), -1.0, 1.0)
                    : 0.0;
                const qint64 quantized = std::clamp<qint64>(
                    static_cast<qint64>(std::llround(finiteSample * scale + noise)), minimum, maximum);
                const quint32 packed = static_cast<quint32>(quantized);
                pcm.append(static_cast<char>(packed & 0xffu));
                pcm.append(static_cast<char>((packed >> 8) & 0xffu));
                if (bitDepth == 24)
                    pcm.append(static_cast<char>((packed >> 16) & 0xffu));
            }
        }
        if (output.write(pcm) != pcm.size())
            return {false, "Could not write WAV sample data."};
    }
    if (progress && !progress(frameCount, frameCount)) {
        output.cancelWriting();
        return {false, "Encoding cancelled.", true};
    }
    if (!output.commit())
        return {false, "Could not finalize WAV output."};
    return {true, QString("%1-bit PCM WAV encoded, %2 channels%3.")
        .arg(bitDepth).arg(channels).arg(dither ? " with TPDF dither" : "")};
}

AudioCodecResult copyWavWithProgress(const QString& sourcePath, const QString& destinationPath,
                                     const AudioEncodeProgress& progress)
{
    QFile input(sourcePath);
    if (!input.open(QIODevice::ReadOnly))
        return {false, "Could not open temporary WAV."};
    QSaveFile output(destinationPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
        return {false, "Could not write 32-bit float WAV file."};

    constexpr qint64 kCopyBlockBytes = 1024 * 1024;
    const qint64 totalBytes = input.size();
    qint64 copiedBytes = 0;
    while (copiedBytes < totalBytes) {
        if (progress && !progress(copiedBytes, totalBytes)) {
            output.cancelWriting();
            return {false, "Encoding cancelled.", true};
        }
        const QByteArray block = input.read(std::min(kCopyBlockBytes, totalBytes - copiedBytes));
        if (block.isEmpty() || output.write(block) != block.size()) {
            output.cancelWriting();
            return {false, "Could not write 32-bit float WAV file."};
        }
        copiedBytes += block.size();
    }
    if (progress && !progress(totalBytes, totalBytes)) {
        output.cancelWriting();
        return {false, "Encoding cancelled.", true};
    }
    if (!output.commit())
        return {false, "Could not finalize 32-bit float WAV file."};
    return {true, "32-bit float WAV encoded."};
}

AudioCodecResult publishValidatedFile(const QString& stagedPath, const QString& destinationPath)
{
    QFile input(stagedPath);
    if (!input.open(QIODevice::ReadOnly) || input.size() <= 0)
        return {false, "Validated render staging file is missing or empty."};
    QSaveFile output(destinationPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
        return {false, "Could not open the final render destination."};
    constexpr qint64 kPublishBlockBytes = 1024 * 1024;
    while (!input.atEnd()) {
        const QByteArray block = input.read(kPublishBlockBytes);
        if (block.isEmpty() || output.write(block) != block.size()) {
            output.cancelWriting();
            return {false, "Could not publish the validated render."};
        }
    }
    if (!output.commit())
        return {false, "Could not atomically publish the validated render."};
    return {true, "Validated render published."};
}

using Byte = unsigned char;

bool containsBytes(quint64 offset, quint64 count, quint64 size)
{
    return offset <= size && count <= size - offset;
}

quint16 readLe16(const Byte* bytes)
{
    return static_cast<quint16>(bytes[0]) |
        static_cast<quint16>(static_cast<quint16>(bytes[1]) << 8);
}

quint32 readLe32(const Byte* bytes)
{
    return static_cast<quint32>(bytes[0]) |
        (static_cast<quint32>(bytes[1]) << 8) |
        (static_cast<quint32>(bytes[2]) << 16) |
        (static_cast<quint32>(bytes[3]) << 24);
}

quint64 readLe64(const Byte* bytes)
{
    return static_cast<quint64>(readLe32(bytes)) |
        (static_cast<quint64>(readLe32(bytes + 4)) << 32);
}

quint32 readBe24(const Byte* bytes)
{
    return (static_cast<quint32>(bytes[0]) << 16) |
        (static_cast<quint32>(bytes[1]) << 8) |
        static_cast<quint32>(bytes[2]);
}

quint32 readBe32(const Byte* bytes)
{
    return (static_cast<quint32>(bytes[0]) << 24) |
        (static_cast<quint32>(bytes[1]) << 16) |
        (static_cast<quint32>(bytes[2]) << 8) |
        static_cast<quint32>(bytes[3]);
}

bool fourCc(const Byte* bytes, const char* expected)
{
    return std::memcmp(bytes, expected, 4) == 0;
}

bool validateWavStructure(const Byte* bytes, quint64 size)
{
    if (!containsBytes(0, 12, size) ||
        (!fourCc(bytes, "RIFF") && !fourCc(bytes, "RF64")) ||
        !fourCc(bytes + 8, "WAVE")) return false;

    const bool rf64 = fourCc(bytes, "RF64");
    quint64 riffEnd = 0;
    quint64 rf64DataBytes = 0;
    if (rf64) {
        if (readLe32(bytes + 4) != 0xffffffffu || !containsBytes(12, 36, size) ||
            !fourCc(bytes + 12, "ds64") || readLe32(bytes + 16) < 28u) return false;
        const quint64 riffPayloadBytes = readLe64(bytes + 20);
        rf64DataBytes = readLe64(bytes + 28);
        if (riffPayloadBytes > std::numeric_limits<quint64>::max() - 8u) return false;
        riffEnd = riffPayloadBytes + 8u;
    } else {
        riffEnd = static_cast<quint64>(readLe32(bytes + 4)) + 8u;
    }
    if (riffEnd != size || riffEnd < 12u) return false;

    bool foundFormat = false;
    bool foundData = false;
    quint16 blockAlign = 0;
    quint64 offset = 12;
    while (offset < riffEnd) {
        if (!containsBytes(offset, 8, riffEnd)) return false;
        const Byte* chunk = bytes + offset;
        quint64 chunkBytes = readLe32(chunk + 4);
        if (rf64 && fourCc(chunk, "data") && chunkBytes == 0xffffffffu)
            chunkBytes = rf64DataBytes;
        const quint64 payload = offset + 8u;
        if (!containsBytes(payload, chunkBytes, riffEnd)) return false;

        if (fourCc(chunk, "fmt ")) {
            if (foundFormat || chunkBytes < 16u) return false;
            const quint16 formatTag = readLe16(bytes + payload);
            const quint16 channels = readLe16(bytes + payload + 2u);
            const quint32 sampleRate = readLe32(bytes + payload + 4u);
            const quint32 byteRate = readLe32(bytes + payload + 8u);
            blockAlign = readLe16(bytes + payload + 12u);
            const quint16 bitsPerSample = readLe16(bytes + payload + 14u);
            if ((formatTag != 1u && formatTag != 3u && formatTag != 0xfffeu) ||
                channels != 2u || sampleRate == 0u || byteRate == 0u || blockAlign == 0u ||
                bitsPerSample == 0u || bitsPerSample > 64u ||
                static_cast<quint64>(sampleRate) * blockAlign != byteRate) return false;
            foundFormat = true;
        } else if (fourCc(chunk, "data")) {
            if (!foundFormat || foundData || chunkBytes == 0u || blockAlign == 0u ||
                chunkBytes % blockAlign != 0u) return false;
            foundData = true;
        }

        const quint64 paddedBytes = chunkBytes + (chunkBytes & 1u);
        if (paddedBytes < chunkBytes || !containsBytes(payload, paddedBytes, riffEnd)) return false;
        offset = payload + paddedBytes;
    }
    return offset == riffEnd && foundFormat && foundData;
}

bool validateMp3Structure(const Byte* bytes, quint64 size)
{
    quint64 offset = 0;
    if (containsBytes(0, 10, size) && std::memcmp(bytes, "ID3", 3) == 0) {
        if (bytes[3] < 2u || bytes[3] > 4u || (bytes[6] | bytes[7] | bytes[8] | bytes[9]) >= 0x80u)
            return false;
        const quint64 tagBytes = (static_cast<quint64>(bytes[6]) << 21) |
            (static_cast<quint64>(bytes[7]) << 14) |
            (static_cast<quint64>(bytes[8]) << 7) | static_cast<quint64>(bytes[9]);
        const quint64 footerBytes = (bytes[5] & 0x10u) != 0u ? 10u : 0u;
        if (tagBytes > std::numeric_limits<quint64>::max() - 10u - footerBytes) return false;
        offset = 10u + tagBytes + footerBytes;
        if (offset > size) return false;
    }

    quint64 audioEnd = size;
    if (size >= offset + 128u && std::memcmp(bytes + size - 128u, "TAG", 3) == 0)
        audioEnd -= 128u;

    static constexpr int mpeg1Bitrates[15] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320
    };
    static constexpr int mpeg2Bitrates[15] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160
    };
    static constexpr int sampleRates[3] = {44100, 48000, 32000};
    quint64 frameCount = 0;
    while (offset < audioEnd) {
        if (!containsBytes(offset, 4, audioEnd)) return false;
        const quint32 header = readBe32(bytes + offset);
        const int version = static_cast<int>((header >> 19) & 0x3u);
        const int layer = static_cast<int>((header >> 17) & 0x3u);
        const int bitrateIndex = static_cast<int>((header >> 12) & 0xfu);
        const int sampleRateIndex = static_cast<int>((header >> 10) & 0x3u);
        if ((header & 0xffe00000u) != 0xffe00000u || version == 1 || layer != 1 ||
            bitrateIndex <= 0 || bitrateIndex >= 15 || sampleRateIndex >= 3) return false;
        int sampleRate = sampleRates[sampleRateIndex];
        if (version == 2) sampleRate /= 2;
        else if (version == 0) sampleRate /= 4;
        const int bitrate = version == 3 ? mpeg1Bitrates[bitrateIndex] : mpeg2Bitrates[bitrateIndex];
        const quint64 coefficient = version == 3 ? 144000u : 72000u;
        const quint64 frameBytes = coefficient * static_cast<quint64>(bitrate) /
            static_cast<quint64>(sampleRate) + ((header >> 9) & 1u);
        if (frameBytes < 4u || !containsBytes(offset, frameBytes, audioEnd)) return false;
        offset += frameBytes;
        ++frameCount;
    }
    return frameCount > 0u && offset == audioEnd;
}

bool validateFlacStructure(const Byte* bytes, quint64 size)
{
    if (!containsBytes(0, 4, size) || !fourCc(bytes, "fLaC")) return false;
    quint64 offset = 4;
    bool firstBlock = true;
    bool foundLastBlock = false;
    quint64 totalSamples = 0;
    while (!foundLastBlock) {
        if (!containsBytes(offset, 4, size)) return false;
        const Byte blockHeader = bytes[offset];
        const int blockType = blockHeader & 0x7fu;
        const quint64 blockBytes = readBe24(bytes + offset + 1u);
        offset += 4u;
        if (!containsBytes(offset, blockBytes, size) || blockType == 127) return false;
        if (firstBlock) {
            if (blockType != 0 || blockBytes != 34u) return false;
            const Byte* streamInfo = bytes + offset;
            quint64 packed = 0;
            for (int index = 0; index < 8; ++index)
                packed = (packed << 8) | streamInfo[10 + index];
            totalSamples = packed & 0x0000000fffffffffull;
            if (totalSamples == 0u) return false;
            firstBlock = false;
        }
        offset += blockBytes;
        foundLastBlock = (blockHeader & 0x80u) != 0u;
    }
    return !firstBlock && foundLastBlock && containsBytes(offset, 2, size) &&
        bytes[offset] == 0xffu && (bytes[offset + 1u] & 0xfcu) == 0xf8u;
}

quint32 oggCrc(const Byte* page, quint64 pageBytes)
{
    quint32 crc = 0;
    for (quint64 index = 0; index < pageBytes; ++index) {
        const Byte value = index >= 22u && index < 26u ? 0u : page[index];
        crc ^= static_cast<quint32>(value) << 24;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80000000u) != 0u ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

bool validateOggVorbisStructure(const Byte* bytes, quint64 size)
{
    quint64 offset = 0;
    quint32 serial = 0;
    quint32 expectedSequence = 0;
    quint64 pageCount = 0;
    bool foundIdentification = false;
    bool foundEos = false;
    while (offset < size) {
        if (foundEos || !containsBytes(offset, 27, size) || !fourCc(bytes + offset, "OggS") ||
            bytes[offset + 4u] != 0u) return false;
        const Byte headerType = bytes[offset + 5u];
        const quint32 pageSerial = readLe32(bytes + offset + 14u);
        const quint32 sequence = readLe32(bytes + offset + 18u);
        const quint32 storedCrc = readLe32(bytes + offset + 22u);
        const quint64 segmentCount = bytes[offset + 26u];
        if (!containsBytes(offset + 27u, segmentCount, size)) return false;
        quint64 bodyBytes = 0;
        for (quint64 segment = 0; segment < segmentCount; ++segment)
            bodyBytes += bytes[offset + 27u + segment];
        const quint64 pageBytes = 27u + segmentCount + bodyBytes;
        if (!containsBytes(offset, pageBytes, size) || oggCrc(bytes + offset, pageBytes) != storedCrc)
            return false;

        if (pageCount == 0u) {
            if ((headerType & 0x02u) == 0u || (headerType & 0x01u) != 0u || sequence != 0u)
                return false;
            serial = pageSerial;
            expectedSequence = 0u;
            const quint64 packetOffset = offset + 27u + segmentCount;
            if (bodyBytes < 7u || bytes[packetOffset] != 1u ||
                std::memcmp(bytes + packetOffset + 1u, "vorbis", 6) != 0) return false;
            foundIdentification = true;
        } else if (pageSerial != serial || sequence != expectedSequence) {
            return false;
        }
        expectedSequence = sequence + 1u;
        foundEos = (headerType & 0x04u) != 0u;
        offset += pageBytes;
        ++pageCount;
    }
    return offset == size && pageCount > 0u && foundIdentification && foundEos;
}

bool validateWavPackStructure(const Byte* bytes, quint64 size)
{
    quint64 offset = 0;
    quint32 expectedTotalSamples = 0;
    bool foundAudioBlock = false;
    quint64 blockCount = 0;
    while (offset < size) {
        if (!containsBytes(offset, 32, size) || !fourCc(bytes + offset, "wvpk")) return false;
        const quint32 chunkSize = readLe32(bytes + offset + 4u);
        const quint64 blockBytes = static_cast<quint64>(chunkSize) + 8u;
        const quint16 version = readLe16(bytes + offset + 8u);
        const quint32 totalSamples = readLe32(bytes + offset + 12u);
        const quint32 blockSamples = readLe32(bytes + offset + 20u);
        if (chunkSize < 24u || !containsBytes(offset, blockBytes, size) ||
            version < 0x0402u || version > 0x0410u ||
            totalSamples == 0u || totalSamples == 0xffffffffu) return false;
        if (blockCount == 0u) expectedTotalSamples = totalSamples;
        else if (totalSamples != expectedTotalSamples) return false;
        if (blockSamples > 0u) foundAudioBlock = true;
        offset += blockBytes;
        ++blockCount;
    }
    return offset == size && blockCount > 0u && foundAudioBlock;
}
}

AudioCodecFormat AudioCodecBackend::formatFromId(const QString& format)
{
    const QString id = format.trimmed().toLower();
    if (id == "wav" || id == "wave") {
        return AudioCodecFormat::Wav;
    }
    if (id == "mp3") {
        return AudioCodecFormat::Mp3;
    }
    if (id == "flac") {
        return AudioCodecFormat::Flac;
    }
    if (id == "ogg" || id == "oga" || id == "vorbis") {
        return AudioCodecFormat::OggVorbis;
    }
    if (id == "wv" || id == "wavpack") {
        return AudioCodecFormat::WavPack;
    }
    return AudioCodecFormat::Unsupported;
}

QString AudioCodecBackend::displayName(AudioCodecFormat format)
{
    switch (format) {
    case AudioCodecFormat::Wav:
        return "WAV";
    case AudioCodecFormat::Mp3:
        return "MP3";
    case AudioCodecFormat::Flac:
        return "FLAC";
    case AudioCodecFormat::OggVorbis:
        return "OGG Vorbis";
    case AudioCodecFormat::WavPack:
        return "WavPack";
    default:
        return "Unsupported";
    }
}

QString AudioCodecBackend::fileSuffix(AudioCodecFormat format)
{
    switch (format) {
    case AudioCodecFormat::Wav:
        return ".wav";
    case AudioCodecFormat::Mp3:
        return ".mp3";
    case AudioCodecFormat::Flac:
        return ".flac";
    case AudioCodecFormat::OggVorbis:
        return ".ogg";
    case AudioCodecFormat::WavPack:
        return ".wv";
    default:
        return QString();
    }
}

QString AudioCodecBackend::fileDialogFilter(AudioCodecFormat format)
{
    switch (format) {
    case AudioCodecFormat::Wav:
        return "WAV audio (*.wav)";
    case AudioCodecFormat::Mp3:
        return "MP3 audio (*.mp3)";
    case AudioCodecFormat::Flac:
        return "FLAC audio (*.flac)";
    case AudioCodecFormat::OggVorbis:
        return "OGG Vorbis (*.ogg *.oga)";
    case AudioCodecFormat::WavPack:
        return "WavPack audio (*.wv)";
    default:
        return QString();
    }
}

QStringList AudioCodecBackend::nativeCapabilitySummary()
{
    auto line = [&](AudioCodecFormat format) {
        const QString importStatus = isNativeDecoderAvailable(format) ? "import on" : "import missing";
        const QString exportStatus = isNativeEncoderAvailable(format) ? "export on" : "export missing";
        return QString("%1: %2, %3").arg(displayName(format), importStatus, exportStatus);
    };

    return {
        "WAV: import/export on",
        line(AudioCodecFormat::Mp3),
        line(AudioCodecFormat::Flac),
        line(AudioCodecFormat::OggVorbis),
        line(AudioCodecFormat::WavPack),
        QString("Elastique Pro: audio sync %1").arg(AudioStretchBackend::isElastiqueAvailable() ? "on" : "missing")
    };
}

bool AudioCodecBackend::isNativeDecoderAvailable(AudioCodecFormat format)
{
    switch (format) {
    case AudioCodecFormat::Wav:
        return true;
    case AudioCodecFormat::Mp3:
        return NativeAudioCodecBackend::canDecodePath("probe.mp3");
    case AudioCodecFormat::Flac:
        return NativeAudioCodecBackend::canDecodePath("probe.flac");
    case AudioCodecFormat::OggVorbis:
        return NativeAudioCodecBackend::canDecodePath("probe.ogg") ||
            NativeAudioCodecBackend::canDecodePath("probe.oga");
    case AudioCodecFormat::WavPack:
        return NativeAudioCodecBackend::canDecodePath("probe.wv");
    default:
        return false;
    }
}

bool AudioCodecBackend::isNativeEncoderAvailable(AudioCodecFormat format)
{
    switch (format) {
    case AudioCodecFormat::Wav:
        return true;
    case AudioCodecFormat::Mp3:
    case AudioCodecFormat::Flac:
    case AudioCodecFormat::OggVorbis:
    case AudioCodecFormat::WavPack:
        return NativeAudioCodecBackend::canEncode(format);
    default:
        return false;
    }
}

int AudioCodecBackend::maximumEncoderChannels(AudioCodecFormat format)
{
    if (format == AudioCodecFormat::Wav) return 64;
    return NativeAudioCodecBackend::maximumEncodeChannels(format);
}

int AudioCodecBackend::preferredEncoderSampleRate(AudioCodecFormat format, int requestedSampleRate)
{
    const int safeRate = std::clamp(requestedSampleRate, 8000, 384000);
    if (format == AudioCodecFormat::Wav) return safeRate;
    return NativeAudioCodecBackend::preferredEncoderSampleRate(format, safeRate);
}

bool AudioCodecBackend::hasValidContainerHeader(const QString& path, AudioCodecFormat format,
                                                QString* errorText)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0) {
        if (errorText) *errorText = "Encoded output is missing or empty.";
        return false;
    }
    const qint64 fileBytes = file.size();
    uchar* mapped = file.map(0, fileBytes);
    if (!mapped) {
        if (errorText) *errorText = "Encoded output could not be mapped for validation.";
        return false;
    }
    const Byte* bytes = reinterpret_cast<const Byte*>(mapped);
    const quint64 size = static_cast<quint64>(fileBytes);
    bool valid = false;
    switch (format) {
    case AudioCodecFormat::Wav:
        valid = validateWavStructure(bytes, size);
        break;
    case AudioCodecFormat::Mp3:
        valid = validateMp3Structure(bytes, size);
        break;
    case AudioCodecFormat::Flac:
        valid = validateFlacStructure(bytes, size);
        break;
    case AudioCodecFormat::OggVorbis:
        valid = validateOggVorbisStructure(bytes, size);
        break;
    case AudioCodecFormat::WavPack:
        valid = validateWavPackStructure(bytes, size);
        break;
    default:
        break;
    }
    file.unmap(mapped);
    if (!valid && errorText)
        *errorText = QString("Output does not contain a complete valid %1 stream.").arg(displayName(format));
    return valid;
}

QString AudioCodecBackend::nativeEncoderRequirement(AudioCodecFormat format)
{
    switch (format) {
    case AudioCodecFormat::Wav:
        return "WAV export is built in.";
    case AudioCodecFormat::Mp3:
        return "MP3 export needs embedded FFmpeg/libmp3lame or a linked LAME SDK.";
    case AudioCodecFormat::Flac:
        return "FLAC export needs embedded FFmpeg FLAC support or a linked libFLAC SDK.";
    case AudioCodecFormat::OggVorbis:
        return "OGG Vorbis export needs embedded FFmpeg Vorbis support or linked libogg/libvorbis SDKs.";
    case AudioCodecFormat::WavPack:
        return "WavPack export needs embedded FFmpeg WavPack support or a linked WavPack SDK.";
    default:
        return "Unsupported render format.";
    }
}

AudioCodecResult AudioCodecBackend::encodeFromWav(const QString& sourceWav, const QString& destinationPath,
                                                   AudioCodecFormat format, const AudioEncodeOptions& options,
                                                   const AudioEncodeProgress& progress)
{
    const QFileInfo sourceInfo(sourceWav);
    if (!sourceInfo.exists() || sourceInfo.size() <= 0) {
        return {false, "Source WAV is missing or empty."};
    }
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
    const bool samePath = sourceInfo.absoluteFilePath().compare(
        QFileInfo(destinationPath).absoluteFilePath(), pathCase) == 0;

    if (format == AudioCodecFormat::Wav) {
        const bool float32 = options.wavFloatingPoint && options.wavBitDepth == 32;
        if (samePath) {
            if (!float32)
                return {false, "Integer WAV output path must differ from the source WAV."};
            QString validationError;
            return hasValidContainerHeader(sourceWav, format, &validationError)
                ? AudioCodecResult{true, "WAV already rendered."}
                : AudioCodecResult{false, validationError};
        }
        QTemporaryFile stagedFile(
            QFileInfo(destinationPath).absolutePath() + "/.veracore-export-XXXXXX.wav");
        stagedFile.setAutoRemove(true);
        if (!stagedFile.open())
            return {false, "Could not create WAV validation staging file."};
        const QString stagedPath = stagedFile.fileName();
        stagedFile.close();
        QFile::remove(stagedPath);
        AudioCodecResult wavResult;
        if (float32) {
            wavResult = copyWavWithProgress(sourceWav, stagedPath, progress);
            if (!wavResult.success) return wavResult;
        } else {
            wavResult = writeIntegerPcmWav(
                sourceWav, stagedPath, options.wavBitDepth, options.dither, progress);
            if (!wavResult.success)
                return wavResult;
        }
        QString validationError;
        if (!hasValidContainerHeader(stagedPath, format, &validationError)) {
            QFile::remove(stagedPath);
            return {false, validationError};
        }
        const AudioCodecResult publishResult = publishValidatedFile(stagedPath, destinationPath);
        if (!publishResult.success) return publishResult;
        return wavResult;
    }

    if (format == AudioCodecFormat::Mp3 || format == AudioCodecFormat::Flac ||
        format == AudioCodecFormat::OggVorbis || format == AudioCodecFormat::WavPack) {
        if (samePath) return {false, "Compressed output path must differ from source WAV."};
        const int sourceChannels = wavChannelCount(sourceWav);
        const AudioFileProbeResult sourceProbe = AudioFileDecoder::probeWavPcm(sourceWav);
        if (!sourceProbe.success || sourceChannels <= 0) {
            return {false, sourceProbe.message.isEmpty()
                ? QStringLiteral("Could not probe rendered WAV before encoding.")
                : sourceProbe.message};
        }
        if (sourceChannels > 2 && sourceProbe.channelMask == 0) {
            return {false, QString("Rendered WAV has %1 channels but no speaker mask; refusing ambiguous spatial export.")
                .arg(sourceChannels)};
        }
        const int maximumChannels = maximumEncoderChannels(format);
        if (sourceChannels > maximumChannels) {
            return {false, QString("%1 supports at most %2 channels in the active embedded backend; "
                                   "refusing to collapse a %3-channel spatial master.")
                .arg(displayName(format)).arg(maximumChannels).arg(sourceChannels)};
        }
        if (!NativeAudioCodecBackend::canEncode(format)) {
            return {false, nativeEncoderRequirement(format) + " Embedded libraries are used; no external process is launched."};
        }
        QTemporaryFile stagedFile(
            QFileInfo(destinationPath).absolutePath() + "/.veracore-export-XXXXXX" + fileSuffix(format));
        stagedFile.setAutoRemove(true);
        if (!stagedFile.open())
            return {false, "Could not create compressed validation staging file."};
        const QString stagedPath = stagedFile.fileName();
        stagedFile.close();
        QFile::remove(stagedPath);
        AudioCodecResult result = NativeAudioCodecBackend::encodeFromWav(
            sourceWav, stagedPath, format, options, progress);
        if (!result.success) return result;
        QString validationError;
        if (!hasValidContainerHeader(stagedPath, format, &validationError)) {
            QFile::remove(stagedPath);
            return {false, validationError};
        }
        const AudioFileProbeResult encodedProbe = NativeAudioCodecBackend::probeFile(stagedPath);
        const bool spatialMaskMismatch = sourceChannels > 2 &&
            (encodedProbe.channelMask == 0 || encodedProbe.channelMask != sourceProbe.channelMask);
        if (!encodedProbe.success || encodedProbe.channelCount != sourceChannels ||
            encodedProbe.sampleRate != sourceProbe.sampleRate || spatialMaskMismatch) {
            QFile::remove(stagedPath);
            return {false, QString("Encoded %1 layout verification failed: expected %2 channels, mask 0x%3 at %4 Hz; "
                                   "received %5 channels, mask 0x%6 at %7 Hz.")
                .arg(displayName(format))
                .arg(sourceChannels)
                .arg(QString::number(sourceProbe.channelMask, 16))
                .arg(sourceProbe.sampleRate)
                .arg(encodedProbe.channelCount)
                .arg(QString::number(encodedProbe.channelMask, 16))
                .arg(encodedProbe.sampleRate)};
        }
        const AudioCodecResult publishResult = publishValidatedFile(stagedPath, destinationPath);
        if (!publishResult.success) return publishResult;
        return result;
    }

    return {false, "Unsupported render format."};
}
