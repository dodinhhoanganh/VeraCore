#include "NativeAudioCodecBackend.h"
#include "AudioNativeBackendConfig.h"
#include "FfmpegAudioCodecBackend.h"

#include <QByteArray>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QVector>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#ifndef __has_include
#define __has_include(x) 0
#endif

#if VC_AUDIO_WITH_LAME != 0 && (VC_AUDIO_LAME_ENCODER_READY != 0 || VC_AUDIO_MP3_DECODER_READY != 0) && __has_include(<lame/lame.h>)
#include <lame/lame.h>
#define VC_AUDIO_HAS_LAME_HEADER 1
#elif VC_AUDIO_WITH_LAME != 0 && (VC_AUDIO_LAME_ENCODER_READY != 0 || VC_AUDIO_MP3_DECODER_READY != 0) && __has_include(<lame.h>)
#include <lame.h>
#define VC_AUDIO_HAS_LAME_HEADER 1
#else
#define VC_AUDIO_HAS_LAME_HEADER 0
#endif

#if VC_AUDIO_WITH_FLAC != 0 && VC_AUDIO_FLAC_CODEC_READY != 0 && __has_include(<FLAC/stream_encoder.h>) && __has_include(<FLAC/stream_decoder.h>)
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#define VC_AUDIO_HAS_FLAC_HEADERS 1
#else
#define VC_AUDIO_HAS_FLAC_HEADERS 0
#endif

#if VC_AUDIO_WITH_VORBIS != 0 && VC_AUDIO_VORBIS_CODEC_READY != 0 && __has_include(<ogg/ogg.h>) && __has_include(<vorbis/vorbisenc.h>) && __has_include(<vorbis/vorbisfile.h>)
#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>
#include <vorbis/vorbisfile.h>
#define VC_AUDIO_HAS_VORBIS_HEADERS 1
#else
#define VC_AUDIO_HAS_VORBIS_HEADERS 0
#endif

#if VC_AUDIO_WITH_WAVPACK != 0 && VC_AUDIO_WAVPACK_CODEC_READY != 0 && __has_include(<wavpack/wavpack.h>)
#include <wavpack/wavpack.h>
#define VC_AUDIO_HAS_WAVPACK_HEADER 1
#elif VC_AUDIO_WITH_WAVPACK != 0 && VC_AUDIO_WAVPACK_CODEC_READY != 0 && __has_include(<wavpack.h>)
#include <wavpack.h>
#define VC_AUDIO_HAS_WAVPACK_HEADER 1
#else
#define VC_AUDIO_HAS_WAVPACK_HEADER 0
#endif

namespace
{
float nextDitherUniform(quint32& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
}

double nextTpdfDither(quint32& state)
{
    return static_cast<double>(nextDitherUniform(state) - nextDitherUniform(state));
}

short floatToPcm16(float value, double dither = 0.0)
{
    if (!std::isfinite(value)) {
        return 0;
    }
    const double clamped = std::clamp(static_cast<double>(value), -1.0, 1.0);
    const long quantized = std::clamp(
        std::lround(clamped * 32768.0 + dither), -32768l, 32767l);
    return static_cast<short>(quantized);
}

float pcm16ToFloat(short value)
{
    return std::clamp(static_cast<float>(value) / 32768.0f, -1.0f, 1.0f);
}

float finiteUnitSample(float value)
{
    return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
}

int32_t floatToPcm24(float value, double dither = 0.0)
{
    if (!std::isfinite(value)) {
        return 0;
    }
    const double clamped = std::clamp(static_cast<double>(value), -1.0, 1.0);
    const long quantized = std::clamp(
        std::lround(clamped * 8388608.0 + dither), -8388608l, 8388607l);
    return static_cast<int32_t>(quantized);
}

float flacIntToFloat(int32_t value, unsigned bitsPerSample)
{
    if (bitsPerSample == 0 || bitsPerSample > 32) {
        return 0.0f;
    }
    const double scale = std::ldexp(1.0, static_cast<int>(bitsPerSample) - 1);
    return static_cast<float>(std::clamp(static_cast<double>(value) / scale, -1.0, 1.0));
}

bool canAppendStereoFrames(const DecodedAudioFile& audio, qint64 frames)
{
    return frames >= 0 && frames <=
        (static_cast<qint64>(std::numeric_limits<int>::max()) - audio.stereoSamples.size()) / 2;
}

float wavPackSampleToFloat(int32_t value, unsigned bitsPerSample, bool floatingPoint)
{
    if (!floatingPoint) return flacIntToFloat(value, bitsPerSample);
    float sample = 0.0f;
    static_assert(sizeof(sample) == sizeof(value), "WavPack float sample size mismatch");
    std::memcpy(&sample, &value, sizeof(sample));
    return std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
}

AudioCodecResult failOutputIfFileError(const QFile& output, const QString& destinationPath, const QString& message)
{
    if (output.error() == QFileDevice::NoError) {
        return {true, QString()};
    }
    QFile::remove(destinationPath);
    return {false, message};
}

AudioCodecResult encodeMp3WithLame(const QString& sourceWav, const QString& destinationPath,
                                   const AudioEncodeOptions& options,
                                   const AudioEncodeProgress& progress)
{
#if VC_AUDIO_WITH_LAME != 0 && VC_AUDIO_LAME_ENCODER_READY != 0 && VC_AUDIO_HAS_LAME_HEADER != 0
    const AudioDecodeResult decoded = AudioFileDecoder::decodeWavPcm(sourceWav);
    if (!decoded.success) {
        return {false, decoded.message.isEmpty() ? "Could not decode source WAV for MP3 export." : decoded.message};
    }
    if (decoded.audio.sampleRate <= 0 || decoded.audio.stereoSamples.isEmpty()) {
        return {false, "Source WAV has no audio data for MP3 export."};
    }

    QFile output(destinationPath);
    if (!output.open(QIODevice::WriteOnly)) {
        return {false, "Could not open MP3 output file."};
    }

    lame_global_flags* lame = lame_init();
    if (!lame) {
        output.close();
        QFile::remove(destinationPath);
        return {false, "Could not initialize LAME encoder."};
    }

    lame_set_in_samplerate(lame, decoded.audio.sampleRate);
    lame_set_num_channels(lame, 2);
    lame_set_VBR(lame, vbr_off);
    lame_set_brate(lame, std::clamp(options.lossyBitrateKbps, 64, 320));
    lame_set_quality(lame, std::clamp(9 - options.compressionLevel, 0, 9));
    if (lame_set_bWriteVbrTag(lame, 1) < 0 || lame_init_params(lame) < 0) {
        lame_close(lame);
        output.close();
        QFile::remove(destinationPath);
        return {false, "Could not configure LAME encoder."};
    }

    constexpr int kChunkFrames = 1152;
    constexpr int kMp3BufferBytes = (kChunkFrames * 5 / 4) + 7200;
    QVector<short> pcm;
    pcm.resize(kChunkFrames * 2);
    QVector<unsigned char> mp3;
    mp3.resize(kMp3BufferBytes);

    const int totalFrames = decoded.audio.frameCount();
    for (int frame = 0; frame < totalFrames; frame += kChunkFrames) {
        if (progress && !progress(frame, totalFrames)) {
            lame_close(lame);
            output.close();
            QFile::remove(destinationPath);
            return {false, "Encoding cancelled.", true};
        }
        const int framesThisChunk = std::min(kChunkFrames, totalFrames - frame);
        for (int i = 0; i < framesThisChunk; ++i) {
            const int sourceIndex = (frame + i) * 2;
            pcm[i * 2] = floatToPcm16(decoded.audio.stereoSamples[sourceIndex]);
            pcm[i * 2 + 1] = floatToPcm16(decoded.audio.stereoSamples[sourceIndex + 1]);
        }

        const int bytesWritten = lame_encode_buffer_interleaved(
            lame,
            pcm.data(),
            framesThisChunk,
            mp3.data(),
            mp3.size());
        if (bytesWritten < 0 || output.write(reinterpret_cast<const char*>(mp3.constData()), bytesWritten) != bytesWritten) {
            lame_close(lame);
            output.close();
            QFile::remove(destinationPath);
            return {false, "LAME MP3 encode failed."};
        }
    }
    if (progress && !progress(totalFrames, totalFrames)) {
        lame_close(lame);
        output.close();
        QFile::remove(destinationPath);
        return {false, "Encoding cancelled.", true};
    }

    const int flushBytes = lame_encode_flush(lame, mp3.data(), mp3.size());
    if (flushBytes < 0 || output.write(reinterpret_cast<const char*>(mp3.constData()), flushBytes) != flushBytes) {
        lame_close(lame);
        output.close();
        QFile::remove(destinationPath);
        return {false, "LAME MP3 flush failed."};
    }

    size_t tagBytes = lame_get_lametag_frame(lame, mp3.data(), static_cast<size_t>(mp3.size()));
    if (tagBytes > static_cast<size_t>(mp3.size()) &&
        tagBytes <= static_cast<size_t>(std::numeric_limits<int>::max())) {
        mp3.resize(static_cast<int>(tagBytes));
        tagBytes = lame_get_lametag_frame(lame, mp3.data(), static_cast<size_t>(mp3.size()));
    }
    if (tagBytes > static_cast<size_t>(mp3.size()) ||
        (tagBytes > 0 && (!output.seek(0) ||
            output.write(reinterpret_cast<const char*>(mp3.constData()), static_cast<qint64>(tagBytes)) !=
                static_cast<qint64>(tagBytes)))) {
        lame_close(lame);
        output.close();
        QFile::remove(destinationPath);
        return {false, "Could not finalize LAME/Xing seek metadata."};
    }
    lame_close(lame);
    output.close();

    const AudioCodecResult fileError = failOutputIfFileError(output, destinationPath, "MP3 output write failed.");
    if (!fileError.success) {
        return fileError;
    }
    if (!QFileInfo::exists(destinationPath) || QFileInfo(destinationPath).size() <= 0) {
        QFile::remove(destinationPath);
        return {false, "LAME MP3 export produced an empty file."};
    }
    return {true, "MP3 encoded with native LAME."};
#else
    Q_UNUSED(sourceWav);
    Q_UNUSED(destinationPath);
    Q_UNUSED(options);
    Q_UNUSED(progress);
    return {false, "MP3 export needs embedded FFmpeg/libmp3lame or a directly linked LAME SDK."};
#endif
}

AudioDecodeResult decodeMp3WithLameHip(const QString& path)
{
#if VC_AUDIO_WITH_LAME != 0 && VC_AUDIO_MP3_DECODER_READY != 0 && VC_AUDIO_HAS_LAME_HEADER != 0
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {false, "Could not open MP3 input file.", {}};
    }

    hip_t hip = hip_decode_init();
    if (!hip) {
        return {false, "Could not initialize LAME HIP MP3 decoder.", {}};
    }

    DecodedAudioFile decoded;
    QVector<short> left;
    QVector<short> right;
    left.resize(1152);
    right.resize(1152);

    mp3data_struct mp3Data;
    memset(&mp3Data, 0, sizeof(mp3Data));

    bool sawAudio = false;
    while (!file.atEnd()) {
        QByteArray chunk = file.read(4096);
        if (chunk.isEmpty()) {
            break;
        }

        const int samples = hip_decode1_headers(
            hip,
            reinterpret_cast<unsigned char*>(chunk.data()),
            static_cast<size_t>(chunk.size()),
            left.data(),
            right.data(),
            &mp3Data);
        if (samples < 0) {
            hip_decode_exit(hip);
            return {false, "LAME HIP MP3 decode failed.", {}};
        }
        if (samples == 0) {
            continue;
        }

        sawAudio = true;
        if (decoded.sampleRate == 0 && mp3Data.samplerate > 0) {
            decoded.sampleRate = mp3Data.samplerate;
        } else if (decoded.sampleRate > 0 && mp3Data.samplerate > 0 && decoded.sampleRate != mp3Data.samplerate) {
            hip_decode_exit(hip);
            return {false, "MP3 streams with sample-rate changes are not supported.", {}};
        }
        const int channels = std::max(1, mp3Data.stereo);
        decoded.sourceChannels = channels;
        if (!canAppendStereoFrames(decoded, samples)) {
            hip_decode_exit(hip);
            return {false, "Decoded MP3 is too large.", {}};
        }
        decoded.stereoSamples.reserve(decoded.stereoSamples.size() + samples * 2);
        for (int i = 0; i < samples; ++i) {
            const float l = pcm16ToFloat(left[i]);
            const float r = channels > 1 ? pcm16ToFloat(right[i]) : l;
            decoded.stereoSamples.push_back(l);
            decoded.stereoSamples.push_back(r);
        }
    }

    for (;;) {
        const int samples = hip_decode1_headers(hip, nullptr, 0, left.data(), right.data(), &mp3Data);
        if (samples < 0) {
            hip_decode_exit(hip);
            return {false, "LAME HIP MP3 decode flush failed.", {}};
        }
        if (samples == 0) {
            break;
        }
        sawAudio = true;
        if (decoded.sampleRate == 0 && mp3Data.samplerate > 0) {
            decoded.sampleRate = mp3Data.samplerate;
        } else if (decoded.sampleRate > 0 && mp3Data.samplerate > 0 && decoded.sampleRate != mp3Data.samplerate) {
            hip_decode_exit(hip);
            return {false, "MP3 streams with sample-rate changes are not supported.", {}};
        }
        const int channels = std::max(1, mp3Data.stereo);
        decoded.sourceChannels = channels;
        if (!canAppendStereoFrames(decoded, samples)) {
            hip_decode_exit(hip);
            return {false, "Decoded MP3 is too large.", {}};
        }
        for (int i = 0; i < samples; ++i) {
            const float l = pcm16ToFloat(left[i]);
            const float r = channels > 1 ? pcm16ToFloat(right[i]) : l;
            decoded.stereoSamples.push_back(l);
            decoded.stereoSamples.push_back(r);
        }
    }

    hip_decode_exit(hip);
    if (!sawAudio || decoded.sampleRate <= 0 || decoded.stereoSamples.isEmpty()) {
        return {false, "MP3 file has no decoded audio.", {}};
    }
    return {true, QFileInfo(path).fileName(), decoded};
#else
    Q_UNUSED(path);
    return {false, "MP3 import needs embedded FFmpeg or a directly linked LAME HIP decoder.", {}};
#endif
}

AudioCodecResult encodeFlacWithLibFlac(const QString& sourceWav, const QString& destinationPath,
                                       const AudioEncodeOptions& options,
                                       const AudioEncodeProgress& progress)
{
#if VC_AUDIO_WITH_FLAC != 0 && VC_AUDIO_FLAC_CODEC_READY != 0 && VC_AUDIO_HAS_FLAC_HEADERS != 0
    const AudioDecodeResult decoded = AudioFileDecoder::decodeWavPcm(sourceWav);
    if (!decoded.success) {
        return {false, decoded.message.isEmpty() ? "Could not decode source WAV for FLAC export." : decoded.message};
    }
    if (decoded.audio.sampleRate <= 0 || decoded.audio.stereoSamples.isEmpty()) {
        return {false, "Source WAV has no audio data for FLAC export."};
    }

    FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
    if (!encoder) {
        return {false, "Could not initialize libFLAC encoder."};
    }

    FLAC__stream_encoder_set_verify(encoder, true);
    const int bitDepth = options.losslessBitDepth <= 16 ? 16 : 24;
    FLAC__stream_encoder_set_compression_level(encoder, std::clamp(options.compressionLevel, 0, 8));
    FLAC__stream_encoder_set_channels(encoder, 2);
    FLAC__stream_encoder_set_bits_per_sample(encoder, bitDepth);
    FLAC__stream_encoder_set_sample_rate(encoder, decoded.audio.sampleRate);
    FLAC__stream_encoder_set_total_samples_estimate(encoder, decoded.audio.frameCount());
    auto flacEncoderStateText = [&]() {
        return QString("state %1").arg(static_cast<int>(FLAC__stream_encoder_get_state(encoder)));
    };

    const QByteArray outputPath = QFile::encodeName(destinationPath);
    const FLAC__StreamEncoderInitStatus initStatus =
        FLAC__stream_encoder_init_file(encoder, outputPath.constData(), nullptr, nullptr);
    if (initStatus != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(encoder);
        QFile::remove(destinationPath);
        return {false, "Could not open FLAC output file."};
    }

    constexpr int kChunkFrames = 4096;
    QVector<FLAC__int32> pcm;
    pcm.resize(kChunkFrames * 2);
    quint32 ditherState = 0x4f1bbcd9u;
    auto dither = [&]() {
        return options.dither ? nextTpdfDither(ditherState) : 0.0;
    };

    const int totalFrames = decoded.audio.frameCount();
    for (int frame = 0; frame < totalFrames; frame += kChunkFrames) {
        if (progress && !progress(frame, totalFrames)) {
            FLAC__stream_encoder_finish(encoder);
            FLAC__stream_encoder_delete(encoder);
            QFile::remove(destinationPath);
            return {false, "Encoding cancelled.", true};
        }
        const int framesThisChunk = std::min(kChunkFrames, totalFrames - frame);
        for (int i = 0; i < framesThisChunk; ++i) {
            const int sourceIndex = (frame + i) * 2;
            pcm[i * 2] = bitDepth == 16
                ? floatToPcm16(decoded.audio.stereoSamples[sourceIndex], dither())
                : floatToPcm24(decoded.audio.stereoSamples[sourceIndex], dither());
            pcm[i * 2 + 1] = bitDepth == 16
                ? floatToPcm16(decoded.audio.stereoSamples[sourceIndex + 1], dither())
                : floatToPcm24(decoded.audio.stereoSamples[sourceIndex + 1], dither());
        }
        if (!FLAC__stream_encoder_process_interleaved(encoder, pcm.constData(), framesThisChunk)) {
            const QString stateText = flacEncoderStateText();
            FLAC__stream_encoder_finish(encoder);
            FLAC__stream_encoder_delete(encoder);
            QFile::remove(destinationPath);
            return {false, QString("libFLAC encode failed (%1).").arg(stateText)};
        }
    }
    if (progress && !progress(totalFrames, totalFrames)) {
        FLAC__stream_encoder_finish(encoder);
        FLAC__stream_encoder_delete(encoder);
        QFile::remove(destinationPath);
        return {false, "Encoding cancelled.", true};
    }

    if (!FLAC__stream_encoder_finish(encoder)) {
        const QString stateText = flacEncoderStateText();
        FLAC__stream_encoder_delete(encoder);
        QFile::remove(destinationPath);
        return {false, QString("libFLAC finalize failed (%1).").arg(stateText)};
    }
    FLAC__stream_encoder_delete(encoder);

    if (!QFileInfo::exists(destinationPath) || QFileInfo(destinationPath).size() <= 0) {
        QFile::remove(destinationPath);
        return {false, "FLAC export produced an empty file."};
    }
    return {true, QString("FLAC encoded with native libFLAC%1.")
        .arg(options.dither ? " and TPDF dither" : "")};
#else
    Q_UNUSED(sourceWav);
    Q_UNUSED(destinationPath);
    Q_UNUSED(options);
    Q_UNUSED(progress);
    return {false, "FLAC export needs embedded FFmpeg or a directly linked libFLAC SDK."};
#endif
}

#if VC_AUDIO_WITH_FLAC != 0 && VC_AUDIO_FLAC_CODEC_READY != 0 && VC_AUDIO_HAS_FLAC_HEADERS != 0
struct FlacDecodeState
{
    DecodedAudioFile audio;
    QString error;
};

FLAC__StreamDecoderWriteStatus flacWriteCallback(
    const FLAC__StreamDecoder*,
    const FLAC__Frame* frame,
    const FLAC__int32* const buffer[],
    void* clientData)
{
    FlacDecodeState* state = static_cast<FlacDecodeState*>(clientData);
    if (!state || !frame || !buffer || !buffer[0]) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    const unsigned channels = frame->header.channels;
    const unsigned bits = frame->header.bits_per_sample;
    const unsigned blockSize = frame->header.blocksize;
    if (channels == 0 || channels > 8 || bits == 0 || bits > 32 || blockSize == 0) {
        state->error = "Unsupported FLAC frame layout.";
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    state->audio.sourceChannels = static_cast<int>(channels);
    if (state->audio.sampleRate == 0 && frame->header.sample_rate > 0) {
        state->audio.sampleRate = static_cast<int>(frame->header.sample_rate);
    } else if (frame->header.sample_rate == 0 ||
               state->audio.sampleRate != static_cast<int>(frame->header.sample_rate)) {
        state->error = "FLAC streams with sample-rate changes are not supported.";
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    if (!canAppendStereoFrames(state->audio, blockSize)) {
        state->error = "Decoded FLAC is too large.";
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    state->audio.stereoSamples.reserve(state->audio.stereoSamples.size() + static_cast<int>(blockSize * 2));
    for (unsigned i = 0; i < blockSize; ++i) {
        const float left = flacIntToFloat(buffer[0][i], bits);
        const float right = channels > 1 && buffer[1] ? flacIntToFloat(buffer[1][i], bits) : left;
        state->audio.stereoSamples.push_back(left);
        state->audio.stereoSamples.push_back(right);
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void flacMetadataCallback(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* metadata, void* clientData)
{
    FlacDecodeState* state = static_cast<FlacDecodeState*>(clientData);
    if (!state || !metadata || metadata->type != FLAC__METADATA_TYPE_STREAMINFO) {
        return;
    }
    state->audio.sampleRate = static_cast<int>(metadata->data.stream_info.sample_rate);
    const FLAC__uint64 totalSamples = metadata->data.stream_info.total_samples;
    if (totalSamples > 0 && totalSamples <= static_cast<FLAC__uint64>(std::numeric_limits<int>::max() / 2)) {
        state->audio.stereoSamples.reserve(static_cast<int>(totalSamples * 2));
    }
}

void flacErrorCallback(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status, void* clientData)
{
    FlacDecodeState* state = static_cast<FlacDecodeState*>(clientData);
    if (state) {
        state->error = QString("libFLAC decode error %1.").arg(static_cast<int>(status));
    }
}
#endif

AudioDecodeResult decodeFlacWithLibFlac(const QString& path)
{
#if VC_AUDIO_WITH_FLAC != 0 && VC_AUDIO_FLAC_CODEC_READY != 0 && VC_AUDIO_HAS_FLAC_HEADERS != 0
    FLAC__StreamDecoder* decoder = FLAC__stream_decoder_new();
    if (!decoder) {
        return {false, "Could not initialize libFLAC decoder.", {}};
    }

    FlacDecodeState state;
    const QByteArray inputPath = QFile::encodeName(path);
    const FLAC__StreamDecoderInitStatus initStatus = FLAC__stream_decoder_init_file(
        decoder,
        inputPath.constData(),
        flacWriteCallback,
        flacMetadataCallback,
        flacErrorCallback,
        &state);
    if (initStatus != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        FLAC__stream_decoder_delete(decoder);
        return {false, "Could not open FLAC input file.", {}};
    }

    const bool ok = FLAC__stream_decoder_process_until_end_of_stream(decoder);
    FLAC__stream_decoder_finish(decoder);
    FLAC__stream_decoder_delete(decoder);

    if (!ok || !state.error.isEmpty()) {
        return {false, state.error.isEmpty() ? "libFLAC decode failed." : state.error, {}};
    }
    if (state.audio.sampleRate <= 0 || state.audio.stereoSamples.isEmpty()) {
        return {false, "FLAC file has no decoded audio.", {}};
    }
    return {true, QFileInfo(path).fileName(), state.audio};
#else
    Q_UNUSED(path);
    return {false, "FLAC import needs embedded FFmpeg or a directly linked libFLAC SDK.", {}};
#endif
}

#if VC_AUDIO_WITH_VORBIS != 0 && VC_AUDIO_VORBIS_CODEC_READY != 0 && VC_AUDIO_HAS_VORBIS_HEADERS != 0
struct MemoryVorbisReader
{
    QByteArray data;
    qint64 offset = 0;
};

size_t vorbisReadCallback(void* ptr, size_t size, size_t nmemb, void* datasource)
{
    MemoryVorbisReader* reader = static_cast<MemoryVorbisReader*>(datasource);
    if (!reader || !ptr || size == 0 || nmemb == 0 || reader->offset >= reader->data.size()) {
        return 0;
    }

    const qint64 availableBytes = reader->data.size() - reader->offset;
    if (availableBytes <= 0) return 0;
    const size_t available = static_cast<size_t>(availableBytes);
    const size_t items = std::min(nmemb, available / size);
    if (items == 0 || items > std::numeric_limits<size_t>::max() / size) return 0;
    const size_t bytes = items * size;
    memcpy(ptr, reader->data.constData() + reader->offset, bytes);
    reader->offset += static_cast<qint64>(bytes);
    return items;
}

int vorbisSeekCallback(void* datasource, ogg_int64_t offset, int whence)
{
    MemoryVorbisReader* reader = static_cast<MemoryVorbisReader*>(datasource);
    if (!reader) {
        return -1;
    }

    auto addOffset = [](qint64 base, ogg_int64_t rawOffset, qint64& result) {
        const qint64 delta = static_cast<qint64>(rawOffset);
        if (static_cast<ogg_int64_t>(delta) != rawOffset ||
            (delta > 0 && base > std::numeric_limits<qint64>::max() - delta) ||
            (delta < 0 && base < std::numeric_limits<qint64>::min() - delta)) {
            return false;
        }
        result = base + delta;
        return true;
    };
    qint64 target = 0;
    if (whence == SEEK_SET) {
        if (!addOffset(0, offset, target)) return -1;
    } else if (whence == SEEK_CUR) {
        if (!addOffset(reader->offset, offset, target)) return -1;
    } else if (whence == SEEK_END) {
        if (!addOffset(reader->data.size(), offset, target)) return -1;
    } else {
        return -1;
    }

    if (target < 0 || target > reader->data.size()) {
        return -1;
    }
    reader->offset = target;
    return 0;
}

int vorbisCloseCallback(void*)
{
    return 0;
}

long vorbisTellCallback(void* datasource)
{
    MemoryVorbisReader* reader = static_cast<MemoryVorbisReader*>(datasource);
    if (!reader || reader->offset < 0 ||
        reader->offset > static_cast<qint64>(std::numeric_limits<long>::max())) return -1;
    return static_cast<long>(reader->offset);
}
#endif

AudioCodecResult encodeOggVorbisWithLibVorbis(const QString& sourceWav, const QString& destinationPath,
                                              const AudioEncodeOptions& options,
                                              const AudioEncodeProgress& progress)
{
#if VC_AUDIO_WITH_VORBIS != 0 && VC_AUDIO_VORBIS_CODEC_READY != 0 && VC_AUDIO_HAS_VORBIS_HEADERS != 0
    const AudioDecodeResult decoded = AudioFileDecoder::decodeWavPcm(sourceWav);
    if (!decoded.success) {
        return {false, decoded.message.isEmpty() ? "Could not decode source WAV for OGG Vorbis export." : decoded.message};
    }
    if (decoded.audio.sampleRate <= 0 || decoded.audio.stereoSamples.isEmpty()) {
        return {false, "Source WAV has no audio data for OGG Vorbis export."};
    }

    QFile output(destinationPath);
    if (!output.open(QIODevice::WriteOnly)) {
        return {false, "Could not open OGG Vorbis output file."};
    }

    vorbis_info vi;
    vorbis_info_init(&vi);
    const float quality = static_cast<float>(std::clamp(options.vorbisQuality, 0, 10)) / 10.0f;
    if (vorbis_encode_init_vbr(&vi, 2, decoded.audio.sampleRate, quality) != 0) {
        vorbis_info_clear(&vi);
        output.close();
        QFile::remove(destinationPath);
        return {false, "Could not configure libvorbis encoder."};
    }

    vorbis_comment vc;
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "ENCODER", "VeraCore native libvorbis");

    vorbis_dsp_state vd;
    vorbis_block vb;
    ogg_stream_state os;
    bool dspReady = false;
    bool blockReady = false;
    bool streamReady = false;
    if (vorbis_analysis_init(&vd, &vi) == 0) {
        dspReady = true;
    }
    if (dspReady && vorbis_block_init(&vd, &vb) == 0) {
        blockReady = true;
    }
    if (blockReady && ogg_stream_init(&os, 0x56435243) == 0) {
        streamReady = true;
    }
    if (!streamReady) {
        if (blockReady) {
            vorbis_block_clear(&vb);
        }
        if (dspReady) {
            vorbis_dsp_clear(&vd);
        }
        vorbis_comment_clear(&vc);
        vorbis_info_clear(&vi);
        output.close();
        QFile::remove(destinationPath);
        return {false, "Could not initialize libvorbis encoder state."};
    }

    ogg_packet header;
    ogg_packet headerComment;
    ogg_packet headerCode;
    bool writeOk = vorbis_analysis_headerout(
        &vd, &vc, &header, &headerComment, &headerCode) == 0;
    if (writeOk) writeOk = ogg_stream_packetin(&os, &header) == 0;
    if (writeOk) writeOk = ogg_stream_packetin(&os, &headerComment) == 0;
    if (writeOk) writeOk = ogg_stream_packetin(&os, &headerCode) == 0;

    ogg_page page;
    bool sawEosPage = false;
    auto writePage = [&]() {
        if (output.write(reinterpret_cast<const char*>(page.header), page.header_len) != page.header_len ||
            output.write(reinterpret_cast<const char*>(page.body), page.body_len) != page.body_len) {
            writeOk = false;
        } else if (ogg_page_eos(&page) != 0) {
            sawEosPage = true;
        }
    };
    while (writeOk && ogg_stream_flush(&os, &page) == 1) {
        writePage();
    }

    auto drainAnalysis = [&]() {
        int blockResult = 0;
        while (writeOk && (blockResult = vorbis_analysis_blockout(&vd, &vb)) == 1) {
            if (vorbis_analysis(&vb, nullptr) != 0 || vorbis_bitrate_addblock(&vb) != 0) {
                writeOk = false;
                break;
            }

            ogg_packet packet;
            int packetResult = 0;
            while (writeOk && (packetResult = vorbis_bitrate_flushpacket(&vd, &packet)) == 1) {
                if (ogg_stream_packetin(&os, &packet) != 0) {
                    writeOk = false;
                    break;
                }
                while (writeOk && ogg_stream_pageout(&os, &page) == 1) {
                    writePage();
                }
            }
            if (packetResult < 0) writeOk = false;
        }
        if (blockResult < 0) writeOk = false;
    };

    constexpr int kChunkFrames = 1024;
    const int totalFrames = decoded.audio.frameCount();
    bool cancelled = false;
    for (int frame = 0; writeOk && frame < totalFrames; frame += kChunkFrames) {
        if (progress && !progress(frame, totalFrames)) {
            cancelled = true;
            break;
        }
        const int framesThisChunk = std::min(kChunkFrames, totalFrames - frame);
        float** buffer = vorbis_analysis_buffer(&vd, framesThisChunk);
        if (!buffer || !buffer[0] || !buffer[1]) {
            writeOk = false;
            break;
        }
        for (int i = 0; i < framesThisChunk; ++i) {
            const int sourceIndex = (frame + i) * 2;
            buffer[0][i] = finiteUnitSample(decoded.audio.stereoSamples[sourceIndex]);
            buffer[1][i] = finiteUnitSample(decoded.audio.stereoSamples[sourceIndex + 1]);
        }
        if (vorbis_analysis_wrote(&vd, framesThisChunk) != 0) {
            writeOk = false;
            break;
        }
        drainAnalysis();
    }

    if (!cancelled && progress && !progress(totalFrames, totalFrames))
        cancelled = true;

    if (!cancelled && writeOk && vorbis_analysis_wrote(&vd, 0) != 0) {
        writeOk = false;
    }
    if (!cancelled && writeOk) drainAnalysis();
    while (!cancelled && writeOk && ogg_stream_flush(&os, &page) == 1) {
        writePage();
    }
    if (!cancelled && writeOk && !sawEosPage) writeOk = false;

    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    output.close();

    const AudioCodecResult fileError = failOutputIfFileError(output, destinationPath, "OGG Vorbis output write failed.");
    if (!fileError.success) {
        return fileError;
    }
    if (cancelled) {
        QFile::remove(destinationPath);
        return {false, "Encoding cancelled.", true};
    }
    if (!writeOk) {
        QFile::remove(destinationPath);
        return {false, "OGG Vorbis output write failed."};
    }
    if (!QFileInfo::exists(destinationPath) || QFileInfo(destinationPath).size() <= 0) {
        QFile::remove(destinationPath);
        return {false, "OGG Vorbis export produced an empty file."};
    }
    return {true, "OGG Vorbis encoded with native libogg/libvorbis."};
#else
    Q_UNUSED(sourceWav);
    Q_UNUSED(destinationPath);
    Q_UNUSED(options);
    Q_UNUSED(progress);
    return {false, "OGG Vorbis export needs embedded FFmpeg/libvorbis or directly linked Vorbis SDKs."};
#endif
}

AudioDecodeResult decodeOggVorbisWithLibVorbis(const QString& path)
{
#if VC_AUDIO_WITH_VORBIS != 0 && VC_AUDIO_VORBIS_CODEC_READY != 0 && VC_AUDIO_HAS_VORBIS_HEADERS != 0
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {false, "Could not open OGG Vorbis input file.", {}};
    }

    MemoryVorbisReader reader;
    reader.data = file.readAll();
    if (reader.data.isEmpty()) {
        return {false, "OGG Vorbis file is empty.", {}};
    }

    ov_callbacks callbacks;
    callbacks.read_func = vorbisReadCallback;
    callbacks.seek_func = vorbisSeekCallback;
    callbacks.close_func = vorbisCloseCallback;
    callbacks.tell_func = vorbisTellCallback;

    OggVorbis_File vf;
    if (ov_open_callbacks(&reader, &vf, nullptr, 0, callbacks) < 0) {
        return {false, "Could not parse OGG Vorbis stream.", {}};
    }

    vorbis_info* info = ov_info(&vf, -1);
    if (!info || info->rate <= 0 || info->channels <= 0) {
        ov_clear(&vf);
        return {false, "Invalid OGG Vorbis stream info.", {}};
    }

    DecodedAudioFile decoded;
    decoded.sampleRate = static_cast<int>(info->rate);
    decoded.sourceChannels = std::max(1, info->channels);
    const ogg_int64_t totalFrames = ov_pcm_total(&vf, -1);
    if (totalFrames > 0 && totalFrames <= static_cast<ogg_int64_t>(std::numeric_limits<int>::max() / 2)) {
        decoded.stereoSamples.reserve(static_cast<int>(totalFrames * 2));
    }

    int bitstream = 0;
    while (true) {
        float** pcm = nullptr;
        const long frames = ov_read_float(&vf, &pcm, 4096, &bitstream);
        if (frames == 0) {
            break;
        }
        if (frames < 0 || !pcm || !pcm[0]) {
            ov_clear(&vf);
            return {false, "libvorbis decode failed.", {}};
        }

        const vorbis_info* streamInfo = ov_info(&vf, bitstream);
        if (!streamInfo || streamInfo->rate <= 0 || streamInfo->channels <= 0) {
            ov_clear(&vf);
            return {false, "Invalid chained OGG Vorbis stream info.", {}};
        }
        if (static_cast<int>(streamInfo->rate) != decoded.sampleRate) {
            ov_clear(&vf);
            return {false, "Chained OGG Vorbis streams with sample-rate changes are not supported.", {}};
        }
        const int channels = std::max(1, streamInfo->channels);
        decoded.sourceChannels = std::max(decoded.sourceChannels, channels);
        if (!canAppendStereoFrames(decoded, frames)) {
            ov_clear(&vf);
            return {false, "Decoded OGG Vorbis is too large.", {}};
        }
        for (long i = 0; i < frames; ++i) {
            const float left = finiteUnitSample(pcm[0][i]);
            const float right = channels > 1 && pcm[1] ? finiteUnitSample(pcm[1][i]) : left;
            decoded.stereoSamples.push_back(left);
            decoded.stereoSamples.push_back(right);
        }
    }

    ov_clear(&vf);
    if (decoded.stereoSamples.isEmpty()) {
        return {false, "OGG Vorbis file has no decoded audio.", {}};
    }
    return {true, QFileInfo(path).fileName(), decoded};
#else
    Q_UNUSED(path);
    return {false, "OGG Vorbis import needs embedded FFmpeg or directly linked Vorbis SDKs.", {}};
#endif
}

#if VC_AUDIO_WITH_WAVPACK != 0 && VC_AUDIO_WAVPACK_CODEC_READY != 0 && VC_AUDIO_HAS_WAVPACK_HEADER != 0
int wavPackWriteCallback(void* id, void* data, int32_t byteCount)
{
    QFile* output = static_cast<QFile*>(id);
    if (!output || !data || byteCount < 0) {
        return 0;
    }
    return output->write(static_cast<const char*>(data), byteCount) == byteCount ? 1 : 0;
}
#endif

AudioCodecResult encodeWavPackWithSdk(const QString& sourceWav, const QString& destinationPath,
                                      const AudioEncodeOptions& options,
                                      const AudioEncodeProgress& progress)
{
#if VC_AUDIO_WITH_WAVPACK != 0 && VC_AUDIO_WAVPACK_CODEC_READY != 0 && VC_AUDIO_HAS_WAVPACK_HEADER != 0
    const AudioDecodeResult decoded = AudioFileDecoder::decodeWavPcm(sourceWav);
    if (!decoded.success) {
        return {false, decoded.message.isEmpty() ? "Could not decode source WAV for WavPack export." : decoded.message};
    }
    if (decoded.audio.sampleRate <= 0 || decoded.audio.stereoSamples.isEmpty()) {
        return {false, "Source WAV has no audio data for WavPack export."};
    }

    QFile output(destinationPath);
    if (!output.open(QIODevice::WriteOnly)) {
        return {false, "Could not open WavPack output file."};
    }

    WavpackContext* context = WavpackOpenFileOutput(wavPackWriteCallback, &output, nullptr);
    if (!context) {
        output.close();
        QFile::remove(destinationPath);
        return {false, "Could not initialize WavPack encoder."};
    }

    WavpackConfig config;
    memset(&config, 0, sizeof(config));
    const int bitDepth = options.losslessBitDepth <= 16 ? 16 : 24;
    config.bytes_per_sample = bitDepth / 8;
    config.bits_per_sample = bitDepth;
    config.num_channels = 2;
    config.channel_mask = 0x3;
    config.sample_rate = decoded.audio.sampleRate;
    config.flags = options.compressionLevel >= 5 ? CONFIG_HIGH_FLAG : 0;

    if (!WavpackSetConfiguration(context, &config, decoded.audio.frameCount()) ||
        !WavpackPackInit(context)) {
        WavpackCloseFile(context);
        output.close();
        QFile::remove(destinationPath);
        return {false, "Could not configure WavPack encoder."};
    }

    constexpr int kChunkFrames = 4096;
    QVector<int32_t> pcm;
    pcm.resize(kChunkFrames * 2);
    quint32 ditherState = 0x9e3779b9u;
    auto dither = [&]() {
        return options.dither ? nextTpdfDither(ditherState) : 0.0;
    };
    const int totalFrames = decoded.audio.frameCount();
    for (int frame = 0; frame < totalFrames; frame += kChunkFrames) {
        if (progress && !progress(frame, totalFrames)) {
            WavpackCloseFile(context);
            output.close();
            QFile::remove(destinationPath);
            return {false, "Encoding cancelled.", true};
        }
        const int framesThisChunk = std::min(kChunkFrames, totalFrames - frame);
        for (int i = 0; i < framesThisChunk; ++i) {
            const int sourceIndex = (frame + i) * 2;
            pcm[i * 2] = bitDepth == 16
                ? floatToPcm16(decoded.audio.stereoSamples[sourceIndex], dither())
                : floatToPcm24(decoded.audio.stereoSamples[sourceIndex], dither());
            pcm[i * 2 + 1] = bitDepth == 16
                ? floatToPcm16(decoded.audio.stereoSamples[sourceIndex + 1], dither())
                : floatToPcm24(decoded.audio.stereoSamples[sourceIndex + 1], dither());
        }
        if (!WavpackPackSamples(context, pcm.data(), framesThisChunk)) {
            WavpackCloseFile(context);
            output.close();
            QFile::remove(destinationPath);
            return {false, "WavPack encode failed."};
        }
    }
    if (progress && !progress(totalFrames, totalFrames)) {
        WavpackCloseFile(context);
        output.close();
        QFile::remove(destinationPath);
        return {false, "Encoding cancelled.", true};
    }

    if (!WavpackFlushSamples(context)) {
        WavpackCloseFile(context);
        output.close();
        QFile::remove(destinationPath);
        return {false, "WavPack finalize failed."};
    }
    WavpackCloseFile(context);
    output.close();

    const AudioCodecResult fileError = failOutputIfFileError(output, destinationPath, "WavPack output write failed.");
    if (!fileError.success) {
        return fileError;
    }
    if (!QFileInfo::exists(destinationPath) || QFileInfo(destinationPath).size() <= 0) {
        QFile::remove(destinationPath);
        return {false, "WavPack export produced an empty file."};
    }
    return {true, QString("WavPack encoded with native WavPack SDK%1.")
        .arg(options.dither ? " and TPDF dither" : "")};
#else
    Q_UNUSED(sourceWav);
    Q_UNUSED(destinationPath);
    Q_UNUSED(options);
    Q_UNUSED(progress);
    return {false, "WavPack export needs embedded FFmpeg or a directly linked WavPack SDK."};
#endif
}

AudioDecodeResult decodeWavPackWithSdk(const QString& path)
{
#if VC_AUDIO_WITH_WAVPACK != 0 && VC_AUDIO_WAVPACK_CODEC_READY != 0 && VC_AUDIO_HAS_WAVPACK_HEADER != 0
    char error[80] = {};
    const QByteArray inputPath = QFile::encodeName(path);
    WavpackContext* context = WavpackOpenFileInput(inputPath.constData(), error, OPEN_WVC | OPEN_TAGS, 0);
    if (!context) {
        return {false, QString("Could not open WavPack input file: %1").arg(error), {}};
    }

    const int sampleRate = WavpackGetSampleRate(context);
    const int channels = WavpackGetNumChannels(context);
    const int bits = WavpackGetBitsPerSample(context);
    const bool floatingPoint = (WavpackGetMode(context) & MODE_FLOAT) != 0;
    const uint32_t totalSamples = WavpackGetNumSamples(context);
    if (sampleRate <= 0 || channels <= 0 || channels > 8 || bits <= 0 || bits > 32) {
        WavpackCloseFile(context);
        return {false, "Unsupported WavPack stream layout.", {}};
    }

    DecodedAudioFile decoded;
    decoded.sampleRate = sampleRate;
    decoded.sourceChannels = channels;
    if (totalSamples != static_cast<uint32_t>(-1) &&
        totalSamples > 0 &&
        totalSamples <= static_cast<uint32_t>(std::numeric_limits<int>::max() / 2)) {
        decoded.stereoSamples.reserve(static_cast<int>(totalSamples * 2));
    }

    constexpr uint32_t kChunkFrames = 4096;
    QVector<int32_t> pcm;
    pcm.resize(static_cast<int>(kChunkFrames) * channels);
    while (true) {
        const uint32_t frames = WavpackUnpackSamples(context, pcm.data(), kChunkFrames);
        if (frames == 0) {
            break;
        }
        if (!canAppendStereoFrames(decoded, frames)) {
            WavpackCloseFile(context);
            return {false, "Decoded WavPack is too large.", {}};
        }
        for (uint32_t frame = 0; frame < frames; ++frame) {
            const int index = static_cast<int>(frame) * channels;
            const float left = wavPackSampleToFloat(pcm[index], static_cast<unsigned>(bits), floatingPoint);
            const float right = channels > 1
                ? wavPackSampleToFloat(pcm[index + 1], static_cast<unsigned>(bits), floatingPoint)
                : left;
            decoded.stereoSamples.push_back(left);
            decoded.stereoSamples.push_back(right);
        }
    }

    if (WavpackGetNumErrors(context) > 0) {
        WavpackCloseFile(context);
        return {false, "WavPack decode reported stream errors.", {}};
    }

    WavpackCloseFile(context);
    if (decoded.stereoSamples.isEmpty()) {
        return {false, "WavPack file has no decoded audio.", {}};
    }
    return {true, QFileInfo(path).fileName(), decoded};
#else
    Q_UNUSED(path);
    return {false, "WavPack import needs embedded FFmpeg or a directly linked WavPack SDK.", {}};
#endif
}
} // namespace

bool NativeAudioCodecBackend::canEncode(AudioCodecFormat format)
{
    if (FfmpegAudioCodecBackend::canEncode(format)) {
        return true;
    }
    switch (format) {
    case AudioCodecFormat::Mp3:
        return VC_AUDIO_WITH_LAME != 0 && VC_AUDIO_LAME_ENCODER_READY != 0 && VC_AUDIO_HAS_LAME_HEADER != 0;
    case AudioCodecFormat::Flac:
        return VC_AUDIO_WITH_FLAC != 0 && VC_AUDIO_FLAC_CODEC_READY != 0 && VC_AUDIO_HAS_FLAC_HEADERS != 0;
    case AudioCodecFormat::OggVorbis:
        return VC_AUDIO_WITH_VORBIS != 0 && VC_AUDIO_VORBIS_CODEC_READY != 0 && VC_AUDIO_HAS_VORBIS_HEADERS != 0;
    case AudioCodecFormat::WavPack:
        return VC_AUDIO_WITH_WAVPACK != 0 && VC_AUDIO_WAVPACK_CODEC_READY != 0 && VC_AUDIO_HAS_WAVPACK_HEADER != 0;
    default:
        return false;
    }
}

int NativeAudioCodecBackend::maximumEncodeChannels(AudioCodecFormat format)
{
    if (FfmpegAudioCodecBackend::canEncode(format)) {
        switch (format) {
        case AudioCodecFormat::Mp3: return 2;
        case AudioCodecFormat::Flac: return 8;
        case AudioCodecFormat::OggVorbis: return 8;
        case AudioCodecFormat::WavPack: return 32;
        default: return 0;
        }
    }
    // Direct SDK wrappers currently consume the application's stereo decode view.
    return canEncode(format) ? 2 : 0;
}

int NativeAudioCodecBackend::preferredEncoderSampleRate(AudioCodecFormat format, int requestedSampleRate)
{
    const int safeRate = std::clamp(requestedSampleRate, 8000, 384000);
    if (FfmpegAudioCodecBackend::canEncode(format))
        return FfmpegAudioCodecBackend::preferredEncoderSampleRate(format, safeRate);
    if (format != AudioCodecFormat::Mp3) return safeRate;
    constexpr std::array<int, 9> mp3Rates = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    return *std::min_element(mp3Rates.begin(), mp3Rates.end(), [safeRate](int a, int b) {
        return std::abs(a - safeRate) < std::abs(b - safeRate);
    });
}

bool NativeAudioCodecBackend::canDecodePath(const QString& path)
{
    if (FfmpegAudioCodecBackend::canDecodePath(path)) {
        return true;
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "mp3") {
        return VC_AUDIO_WITH_LAME != 0 && VC_AUDIO_MP3_DECODER_READY != 0 && VC_AUDIO_HAS_LAME_HEADER != 0;
    }
    if (suffix == "flac") {
        return VC_AUDIO_WITH_FLAC != 0 && VC_AUDIO_FLAC_CODEC_READY != 0 && VC_AUDIO_HAS_FLAC_HEADERS != 0;
    }
    if (suffix == "ogg" || suffix == "oga") {
        return VC_AUDIO_WITH_VORBIS != 0 && VC_AUDIO_VORBIS_CODEC_READY != 0 && VC_AUDIO_HAS_VORBIS_HEADERS != 0;
    }
    if (suffix == "wv" || suffix == "wavpack") {
        return VC_AUDIO_WITH_WAVPACK != 0 && VC_AUDIO_WAVPACK_CODEC_READY != 0 && VC_AUDIO_HAS_WAVPACK_HEADER != 0;
    }
    return false;
}

AudioFileProbeResult NativeAudioCodecBackend::probeFile(const QString& path)
{
    if (FfmpegAudioCodecBackend::canDecodePath(path)) {
        const AudioFileProbeResult probe = FfmpegAudioCodecBackend::probeFile(path);
        if (probe.success) return probe;
    }
    const AudioDecodeResult decoded = decodeFile(path);
    if (!decoded.success || decoded.audio.sampleRate <= 0 || decoded.audio.frameCount() <= 0) {
        return {false, decoded.message, 0, 0};
    }
    return {true, decoded.message, decoded.audio.sampleRate,
            decoded.audio.frameCount(), decoded.audio.sourceChannels};
}

AudioCodecResult NativeAudioCodecBackend::encodeFromWav(const QString& sourceWav, const QString& destinationPath,
                                                         AudioCodecFormat format, const AudioEncodeOptions& options,
                                                         const AudioEncodeProgress& progress)
{
    if (FfmpegAudioCodecBackend::canEncode(format)) {
        return FfmpegAudioCodecBackend::encodeFromWav(sourceWav, destinationPath, format, options, progress);
    }
    if (format == AudioCodecFormat::Mp3) {
        return encodeMp3WithLame(sourceWav, destinationPath, options, progress);
    }
    if (format == AudioCodecFormat::Flac) {
        return encodeFlacWithLibFlac(sourceWav, destinationPath, options, progress);
    }
    if (format == AudioCodecFormat::OggVorbis) {
        return encodeOggVorbisWithLibVorbis(sourceWav, destinationPath, options, progress);
    }
    if (format == AudioCodecFormat::WavPack) {
        return encodeWavPackWithSdk(sourceWav, destinationPath, options, progress);
    }
    return {false, "Native compressed encoder wrapper for this format is not wired yet."};
}

AudioDecodeResult NativeAudioCodecBackend::decodeFile(const QString& path)
{
    AudioDecodeResult ffmpegFailure;
    bool triedFfmpeg = false;
    if (FfmpegAudioCodecBackend::canDecodePath(path)) {
        triedFfmpeg = true;
        AudioDecodeResult result = FfmpegAudioCodecBackend::decodeFile(path);
        if (result.success) {
            return result;
        }
        ffmpegFailure = std::move(result);
    }

    const QString suffix = QFileInfo(path).suffix().toLower();
    AudioDecodeResult directResult;
    if (suffix == "mp3") {
        directResult = decodeMp3WithLameHip(path);
    } else if (suffix == "flac") {
        directResult = decodeFlacWithLibFlac(path);
    } else if (suffix == "ogg" || suffix == "oga") {
        directResult = decodeOggVorbisWithLibVorbis(path);
    } else if (suffix == "wv" || suffix == "wavpack") {
        directResult = decodeWavPackWithSdk(path);
    } else {
        return {false, "Native compressed decoder wrapper is not wired yet.", {}};
    }

    if (directResult.success || !triedFfmpeg) {
        return directResult;
    }
    return ffmpegFailure;
}
