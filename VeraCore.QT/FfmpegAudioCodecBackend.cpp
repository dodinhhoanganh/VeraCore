#include "FfmpegAudioCodecBackend.h"
#include "AudioNativeBackendConfig.h"

#include <QFile>
#include <QFileInfo>
#include <QVector>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>

#ifndef __has_include
#define __has_include(x) 0
#endif

#if VC_AUDIO_WITH_FFMPEG != 0 && VC_AUDIO_FFMPEG_READY != 0 && \
    __has_include(<libavcodec/avcodec.h>) && __has_include(<libavformat/avformat.h>) && \
    __has_include(<libavutil/channel_layout.h>) && __has_include(<libavutil/error.h>) && \
    __has_include(<libavutil/opt.h>) && __has_include(<libavutil/samplefmt.h>) && \
    __has_include(<libswresample/swresample.h>)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#define VC_AUDIO_HAS_FFMPEG 1
#else
#define VC_AUDIO_HAS_FFMPEG 0
#endif

namespace
{
#if VC_AUDIO_HAS_FFMPEG != 0
int maximumChannelsForCodec(AudioCodecFormat format) noexcept
{
    switch (format) {
    case AudioCodecFormat::Mp3: return 2;
    case AudioCodecFormat::Flac: return 8;       // FLAC stream header limit.
    case AudioCodecFormat::OggVorbis: return 8;  // Vorbis mapping family 1.
    case AudioCodecFormat::WavPack: return 32;
    default: return 0;
    }
}

class FloatWavInput
{
public:
    explicit FloatWavInput(const QString& path) : m_file(path) {}

    bool open(QString* error)
    {
        auto fail = [&](const QString& message) {
            if (error) *error = message;
            m_file.close();
            return false;
        };
        if (!m_file.open(QIODevice::ReadOnly))
            return fail("Could not open rendered WAV for embedded FFmpeg encoding.");
        const QByteArray riff = m_file.read(12);
        if (riff.size() != 12 || riff.mid(0, 4) != "RIFF" || riff.mid(8, 4) != "WAVE")
            return fail("Rendered master is not a valid RIFF/WAVE file.");

        qint64 cursor = 12;
        while (cursor + 8 <= m_file.size()) {
            if (!m_file.seek(cursor)) return fail("Could not seek in rendered WAV.");
            const QByteArray header = m_file.read(8);
            if (header.size() != 8) return fail("Rendered WAV has a truncated chunk header.");
            const quint32 chunkBytes = little32(header.constData() + 4);
            const qint64 payloadOffset = cursor + 8;
            if (payloadOffset + static_cast<qint64>(chunkBytes) > m_file.size())
                return fail("Rendered WAV has a truncated chunk.");
            if (header.left(4) == "fmt " && chunkBytes >= 16) {
                if (!m_file.seek(payloadOffset)) return fail("Could not read rendered WAV format.");
                const QByteArray format = m_file.read(std::min<quint32>(chunkBytes, 40u));
                if (format.size() < 16) return fail("Rendered WAV format is truncated.");
                const quint16 formatTag = little16(format.constData());
                m_channels = little16(format.constData() + 2);
                m_sampleRate = static_cast<int>(little32(format.constData() + 4));
                m_blockAlign = little16(format.constData() + 12);
                const quint16 bits = little16(format.constData() + 14);
                const bool ieeeFloat = formatTag == 3;
                const bool extensibleFloat = formatTag == 0xFFFE && format.size() >= 40 &&
                    little16(format.constData() + 16) >= 22 &&
                    little32(format.constData() + 24) == 3;
                if (formatTag == 0xFFFE && format.size() >= 24)
                    m_channelMask = little32(format.constData() + 20);
                m_float32 = bits == 32 && (ieeeFloat || extensibleFloat);
            } else if (header.left(4) == "data") {
                m_dataOffset = payloadOffset;
                m_dataBytes = chunkBytes;
            }
            cursor = payloadOffset + static_cast<qint64>(chunkBytes) + (chunkBytes & 1u);
        }
        const quint32 expectedBlockAlign = static_cast<quint32>(m_channels) * 4u;
        if (!m_float32 || m_channels <= 0 || m_channels > 32 || m_sampleRate < 8000 ||
            m_sampleRate > 384000 || m_blockAlign != expectedBlockAlign ||
            m_dataOffset < 0 || m_dataBytes == 0 || (m_dataBytes % expectedBlockAlign) != 0) {
            return fail("Rendered master must be interleaved 32-bit IEEE-float WAV audio.");
        }
        m_frameCount = static_cast<qint64>(m_dataBytes / expectedBlockAlign);
        return m_frameCount > 0 || fail("Rendered WAV contains no audio frames.");
    }

    bool readFrames(qint64 firstFrame, int frameCount, QVector<float>* samples, QString* error)
    {
        if (!samples || firstFrame < 0 || frameCount <= 0 ||
            firstFrame > m_frameCount || frameCount > m_frameCount - firstFrame) {
            if (error) *error = "Invalid rendered WAV frame range.";
            return false;
        }
        const qint64 sampleCount = static_cast<qint64>(frameCount) * m_channels;
        const qint64 byteCount = sampleCount * 4;
        if (sampleCount > std::numeric_limits<int>::max() ||
            !m_file.seek(m_dataOffset + firstFrame * m_blockAlign)) {
            if (error) *error = "Could not seek to rendered WAV samples.";
            return false;
        }
        const QByteArray raw = m_file.read(byteCount);
        if (raw.size() != byteCount) {
            if (error) *error = "Could not read rendered WAV samples.";
            return false;
        }
        samples->resize(static_cast<int>(sampleCount));
        for (int index = 0; index < samples->size(); ++index) {
            const quint32 bits = little32(raw.constData() + static_cast<qint64>(index) * 4);
            float sample = 0.0f;
            std::memcpy(&sample, &bits, sizeof(sample));
            (*samples)[index] = std::isfinite(sample) ? sample : 0.0f;
        }
        return true;
    }

    int channels() const noexcept { return m_channels; }
    int sampleRate() const noexcept { return m_sampleRate; }
    quint32 channelMask() const noexcept { return m_channelMask; }
    qint64 frameCount() const noexcept { return m_frameCount; }

private:
    static quint16 little16(const char* bytes) noexcept
    {
        return static_cast<quint16>(static_cast<unsigned char>(bytes[0])) |
            static_cast<quint16>(static_cast<unsigned char>(bytes[1]) << 8);
    }
    static quint32 little32(const char* bytes) noexcept
    {
        return static_cast<quint32>(static_cast<unsigned char>(bytes[0])) |
            (static_cast<quint32>(static_cast<unsigned char>(bytes[1])) << 8) |
            (static_cast<quint32>(static_cast<unsigned char>(bytes[2])) << 16) |
            (static_cast<quint32>(static_cast<unsigned char>(bytes[3])) << 24);
    }

    QFile m_file;
    int m_channels = 0;
    int m_sampleRate = 0;
    quint16 m_blockAlign = 0;
    quint32 m_channelMask = 0;
    quint32 m_dataBytes = 0;
    qint64 m_dataOffset = -1;
    qint64 m_frameCount = 0;
    bool m_float32 = false;
};

void configureSourceLayout(const FloatWavInput& source, AVChannelLayout* layout)
{
    if (!layout) return;
    if (source.channelMask() != 0 &&
        av_channel_layout_from_mask(layout, source.channelMask()) >= 0 &&
        layout->nb_channels == source.channels()) {
        return;
    }
    av_channel_layout_uninit(layout);
    av_channel_layout_default(layout, source.channels());
}

QString ffmpegError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

const char* muxerName(AudioCodecFormat format)
{
    switch (format) {
    case AudioCodecFormat::Mp3: return "mp3";
    case AudioCodecFormat::Flac: return "flac";
    case AudioCodecFormat::OggVorbis: return "ogg";
    case AudioCodecFormat::WavPack: return "wv";
    default: return nullptr;
    }
}

const AVCodec* findEncoder(AudioCodecFormat format)
{
    switch (format) {
    case AudioCodecFormat::Mp3: {
        const AVCodec* codec = avcodec_find_encoder_by_name("libmp3lame");
        return codec ? codec : avcodec_find_encoder(AV_CODEC_ID_MP3);
    }
    case AudioCodecFormat::Flac:
        return avcodec_find_encoder(AV_CODEC_ID_FLAC);
    case AudioCodecFormat::OggVorbis: {
        const AVCodec* codec = avcodec_find_encoder_by_name("libvorbis");
        return codec ? codec : avcodec_find_encoder(AV_CODEC_ID_VORBIS);
    }
    case AudioCodecFormat::WavPack:
        return avcodec_find_encoder(AV_CODEC_ID_WAVPACK);
    default:
        return nullptr;
    }
}

bool encoderSupportsRate(const AVCodec* codec, int sampleRate)
{
    if (!codec || !codec->supported_samplerates)
        return true;
    for (const int* rate = codec->supported_samplerates; *rate; ++rate) {
        if (*rate == sampleRate)
            return true;
    }
    return false;
}

bool encoderSupportsSampleFormat(const AVCodec* codec, AVSampleFormat format)
{
    if (!codec || !codec->sample_fmts)
        return true;
    for (const AVSampleFormat* candidate = codec->sample_fmts;
         *candidate != AV_SAMPLE_FMT_NONE; ++candidate) {
        if (*candidate == format)
            return true;
    }
    return false;
}

AVSampleFormat encoderSampleFormat(const AVCodec* codec, AudioCodecFormat format, int losslessBitDepth)
{
    if (!codec || !codec->sample_fmts)
        return AV_SAMPLE_FMT_FLTP;
    if (format == AudioCodecFormat::Flac || format == AudioCodecFormat::WavPack) {
        if (losslessBitDepth <= 16) {
            if (encoderSupportsSampleFormat(codec, AV_SAMPLE_FMT_S16))
                return AV_SAMPLE_FMT_S16;
            if (encoderSupportsSampleFormat(codec, AV_SAMPLE_FMT_S16P))
                return AV_SAMPLE_FMT_S16P;
        }
        if (encoderSupportsSampleFormat(codec, AV_SAMPLE_FMT_S32))
            return AV_SAMPLE_FMT_S32;
        if (encoderSupportsSampleFormat(codec, AV_SAMPLE_FMT_S32P))
            return AV_SAMPLE_FMT_S32P;
    }
    return codec->sample_fmts[0];
}

void sanitizeDecodedFrames(DecodedAudioFile* output, int firstFrame, int frameCount)
{
    if (!output || firstFrame < 0 || frameCount <= 0) return;
    const size_t firstSample = static_cast<size_t>(firstFrame) * 2u;
    const size_t sampleCount = static_cast<size_t>(frameCount) * 2u;
    const size_t outputSamples = static_cast<size_t>(output->stereoSamples.size());
    if (firstSample > outputSamples || sampleCount > outputSamples - firstSample) return;
    for (size_t index = firstSample; index < firstSample + sampleCount; ++index) {
        float& sample = output->stereoSamples[index];
        sample = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
    }
}

bool appendDecodedFrame(SwrContext* converter, AVCodecContext* decoder, const AVChannelLayout& inputLayout,
                        AVFrame* frame, DecodedAudioFile* output, QString* error)
{
    const int maxFrames = av_rescale_rnd(
        swr_get_delay(converter, decoder->sample_rate) + frame->nb_samples,
        decoder->sample_rate, decoder->sample_rate, AV_ROUND_UP);
    if (maxFrames <= 0)
        return true;

    const int oldFrames = output->frameCount();
    if (maxFrames > (std::numeric_limits<int>::max() / 2) - oldFrames) {
        *error = "Decoded audio is too large.";
        return false;
    }

    output->stereoSamples.resize((oldFrames + maxFrames) * 2);
    uint8_t* destination = reinterpret_cast<uint8_t*>(output->stereoSamples.data() + oldFrames * 2);
    const int sourcePlanesCount = av_sample_fmt_is_planar(decoder->sample_fmt)
        ? std::max(1, inputLayout.nb_channels)
        : 1;
    QVector<const uint8_t*> sourcePlanes(sourcePlanesCount);
    for (int plane = 0; plane < sourcePlanesCount; ++plane)
        sourcePlanes[plane] = frame->extended_data[plane];
    const int converted = swr_convert(
        converter, &destination, maxFrames, sourcePlanes.data(), frame->nb_samples);
    if (converted < 0) {
        *error = QString("FFmpeg audio conversion failed: %1").arg(ffmpegError(converted));
        output->stereoSamples.resize(oldFrames * 2);
        return false;
    }
    output->stereoSamples.resize((oldFrames + converted) * 2);
    sanitizeDecodedFrames(output, oldFrames, converted);
    return true;
}

bool flushDecodedResampler(SwrContext* converter, AVCodecContext* decoder,
                           DecodedAudioFile* output, QString* error)
{
    while (true) {
        const int maxFrames = static_cast<int>(swr_get_delay(converter, decoder->sample_rate));
        if (maxFrames <= 0)
            return true;
        const int oldFrames = output->frameCount();
        if (maxFrames > (std::numeric_limits<int>::max() / 2) - oldFrames) {
            *error = "Decoded audio is too large.";
            return false;
        }

        output->stereoSamples.resize((oldFrames + maxFrames) * 2);
        uint8_t* destination = reinterpret_cast<uint8_t*>(output->stereoSamples.data() + oldFrames * 2);
        const int converted = swr_convert(converter, &destination, maxFrames, nullptr, 0);
        if (converted < 0) {
            *error = QString("FFmpeg resampler flush failed: %1").arg(ffmpegError(converted));
            output->stereoSamples.resize(oldFrames * 2);
            return false;
        }
        output->stereoSamples.resize((oldFrames + converted) * 2);
        sanitizeDecodedFrames(output, oldFrames, converted);
        if (converted == 0)
            return true;
    }
}

bool receiveDecodedFrames(AVCodecContext* decoder, SwrContext* converter, const AVChannelLayout& inputLayout,
                          AVFrame* frame,
                          DecodedAudioFile* output, QString* error)
{
    while (true) {
        const int result = avcodec_receive_frame(decoder, frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return true;
        if (result < 0) {
            *error = QString("FFmpeg decode failed: %1").arg(ffmpegError(result));
            return false;
        }
        if (!appendDecodedFrame(converter, decoder, inputLayout, frame, output, error))
            return false;
        av_frame_unref(frame);
    }
}

bool writePackets(AVCodecContext* encoder, AVFormatContext* output, AVStream* stream,
                  AVPacket* packet, QString* error)
{
    while (true) {
        const int result = avcodec_receive_packet(encoder, packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return true;
        if (result < 0) {
            *error = QString("FFmpeg encode failed: %1").arg(ffmpegError(result));
            return false;
        }
        av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
        packet->stream_index = stream->index;
        const int writeResult = av_interleaved_write_frame(output, packet);
        av_packet_unref(packet);
        if (writeResult < 0) {
            *error = QString("FFmpeg mux failed: %1").arg(ffmpegError(writeResult));
            return false;
        }
    }
}
#endif
} // namespace

bool FfmpegAudioCodecBackend::isAvailable()
{
    return VC_AUDIO_HAS_FFMPEG != 0;
}

bool FfmpegAudioCodecBackend::canEncode(AudioCodecFormat format)
{
#if VC_AUDIO_HAS_FFMPEG != 0
    const char* muxer = muxerName(format);
    return muxer && av_guess_format(muxer, nullptr, nullptr) && findEncoder(format);
#else
    Q_UNUSED(format);
    return false;
#endif
}

int FfmpegAudioCodecBackend::preferredEncoderSampleRate(AudioCodecFormat format, int requestedSampleRate)
{
    const int safeRate = std::clamp(requestedSampleRate, 8000, 384000);
#if VC_AUDIO_HAS_FFMPEG != 0
    const AVCodec* codec = findEncoder(format);
    if (!codec || !codec->supported_samplerates) return safeRate;
    int bestRate = 0;
    int bestDistance = std::numeric_limits<int>::max();
    for (const int* rate = codec->supported_samplerates; *rate; ++rate) {
        if (*rate < 8000 || *rate > 384000) continue;
        const int distance = std::abs(*rate - safeRate);
        if (distance < bestDistance || (distance == bestDistance && *rate > bestRate)) {
            bestRate = *rate;
            bestDistance = distance;
        }
    }
    return bestRate > 0 ? bestRate : safeRate;
#else
    Q_UNUSED(format);
    return safeRate;
#endif
}

bool FfmpegAudioCodecBackend::canDecodePath(const QString& path)
{
#if VC_AUDIO_HAS_FFMPEG != 0
    const QString suffix = QFileInfo(path).suffix().toLower();
    AVCodecID codecId = AV_CODEC_ID_NONE;
    if (suffix == "mp3") codecId = AV_CODEC_ID_MP3;
    else if (suffix == "flac") codecId = AV_CODEC_ID_FLAC;
    else if (suffix == "ogg" || suffix == "oga") codecId = AV_CODEC_ID_VORBIS;
    else if (suffix == "wv" || suffix == "wavpack") codecId = AV_CODEC_ID_WAVPACK;
    return codecId != AV_CODEC_ID_NONE && avcodec_find_decoder(codecId) != nullptr;
#else
    Q_UNUSED(path);
    return false;
#endif
}

AudioFileProbeResult FfmpegAudioCodecBackend::probeFile(const QString& path)
{
#if VC_AUDIO_HAS_FFMPEG != 0
    AVFormatContext* format = nullptr;
    const QByteArray inputPath = path.toUtf8();
    int result = avformat_open_input(&format, inputPath.constData(), nullptr, nullptr);
    if (result >= 0) {
        result = avformat_find_stream_info(format, nullptr);
    }

    const AVStream* stream = nullptr;
    if (result >= 0) {
        const int streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (streamIndex < 0) {
            result = streamIndex;
        } else {
            stream = format->streams[streamIndex];
        }
    }

    int sampleRate = 0;
    int channelCount = 0;
    quint64 channelMask = 0;
    double durationSeconds = 0.0;
    if (result >= 0 && stream && stream->codecpar) {
        sampleRate = stream->codecpar->sample_rate;
        channelCount = stream->codecpar->ch_layout.nb_channels;
        if (stream->codecpar->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) {
            channelMask = stream->codecpar->ch_layout.u.mask;
        }
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
            durationSeconds = stream->duration * av_q2d(stream->time_base);
        } else if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
            durationSeconds = static_cast<double>(format->duration) / AV_TIME_BASE;
        }
    }
    avformat_close_input(&format);

    if (result < 0 || sampleRate <= 0 || !std::isfinite(durationSeconds) || durationSeconds <= 0.0) {
        const int errorCode = result < 0 ? result : AVERROR_INVALIDDATA;
        return {false, QString("FFmpeg probe failed: %1").arg(ffmpegError(errorCode)), 0, 0};
    }
    const double frameCountValue = std::round(durationSeconds * sampleRate);
    if (!std::isfinite(frameCountValue) || frameCountValue <= 0.0 ||
        frameCountValue > static_cast<double>(std::numeric_limits<int>::max())) {
        return {false, "Compressed audio duration is outside the supported range.", 0, 0};
    }
    return {true,
            QString("%1 probed with embedded FFmpeg").arg(QFileInfo(path).fileName()),
            sampleRate,
            static_cast<int>(frameCountValue),
            channelCount,
            channelMask};
#else
    Q_UNUSED(path);
    return {false, "Embedded FFmpeg probe is not linked.", 0, 0};
#endif
}

AudioDecodeResult FfmpegAudioCodecBackend::decodeFile(const QString& path)
{
#if VC_AUDIO_HAS_FFMPEG != 0
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    SwrContext* converter = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVChannelLayout inputLayout{};
    AVChannelLayout outputLayout{};
    QString error;
    DecodedAudioFile decoded;

    const QByteArray inputPath = path.toUtf8();
    int result = avformat_open_input(&format, inputPath.constData(), nullptr, nullptr);
    if (result >= 0)
        result = avformat_find_stream_info(format, nullptr);

    const AVCodec* codec = nullptr;
    int streamIndex = -1;
    if (result >= 0) {
        streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (streamIndex < 0)
            result = streamIndex;
    }
    if (result >= 0) {
        decoder = avcodec_alloc_context3(codec);
        if (!decoder)
            result = AVERROR(ENOMEM);
    }
    if (result >= 0)
        result = avcodec_parameters_to_context(decoder, format->streams[streamIndex]->codecpar);
    if (result >= 0)
        result = avcodec_open2(decoder, codec, nullptr);
    if (result >= 0 && decoder->sample_rate <= 0)
        result = AVERROR_INVALIDDATA;

    if (result >= 0) {
        decoded.sourceChannels = decoder->ch_layout.nb_channels > 0
            ? decoder->ch_layout.nb_channels
            : 2;
        if (decoder->ch_layout.nb_channels > 0)
            result = av_channel_layout_copy(&inputLayout, &decoder->ch_layout);
        else
            av_channel_layout_default(&inputLayout, 2);
        av_channel_layout_default(&outputLayout, 2);
    }
    if (result >= 0) {
        result = swr_alloc_set_opts2(&converter, &outputLayout, AV_SAMPLE_FMT_FLT,
            decoder->sample_rate, &inputLayout, decoder->sample_fmt, decoder->sample_rate, 0, nullptr);
    }
    if (result >= 0)
        result = swr_init(converter);
    if (result >= 0) {
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (!packet || !frame)
            result = AVERROR(ENOMEM);
    }

    if (result >= 0) {
        decoded.sampleRate = decoder->sample_rate;
        while ((result = av_read_frame(format, packet)) >= 0) {
            if (packet->stream_index == streamIndex) {
                const int sendResult = avcodec_send_packet(decoder, packet);
                if (sendResult < 0 || !receiveDecodedFrames(
                        decoder, converter, inputLayout, frame, &decoded, &error)) {
                    result = sendResult < 0 ? sendResult : AVERROR_INVALIDDATA;
                    av_packet_unref(packet);
                    break;
                }
            }
            av_packet_unref(packet);
        }
        if (result == AVERROR_EOF) {
            result = avcodec_send_packet(decoder, nullptr);
            if (result >= 0 && !receiveDecodedFrames(
                    decoder, converter, inputLayout, frame, &decoded, &error))
                result = AVERROR_INVALIDDATA;
        }
        if (result >= 0 && !flushDecodedResampler(converter, decoder, &decoded, &error))
            result = AVERROR_INVALIDDATA;
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&converter);
    av_channel_layout_uninit(&inputLayout);
    av_channel_layout_uninit(&outputLayout);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);

    if (result < 0 || decoded.sampleRate <= 0 || decoded.stereoSamples.isEmpty()) {
        if (error.isEmpty())
            error = QString("FFmpeg decode failed: %1").arg(ffmpegError(result));
        return {false, error, {}};
    }
    return {true, QString("%1 decoded with embedded FFmpeg").arg(QFileInfo(path).fileName()), decoded};
#else
    Q_UNUSED(path);
    return {false, "Embedded FFmpeg decoder is not linked.", {}};
#endif
}

AudioCodecResult FfmpegAudioCodecBackend::encodeFromWav(
    const QString& sourceWav, const QString& destinationPath, AudioCodecFormat format,
    const AudioEncodeOptions& options, const AudioEncodeProgress& progress)
{
#if VC_AUDIO_HAS_FFMPEG != 0
    FloatWavInput source(sourceWav);
    QString sourceError;
    if (!source.open(&sourceError))
        return {false, sourceError};
    const int maximumChannels = maximumChannelsForCodec(format);
    if (maximumChannels <= 0 || source.channels() > maximumChannels) {
        return {false, QString("%1 supports at most %2 channels; source master has %3 channels.")
            .arg(AudioCodecBackend::displayName(format))
            .arg(maximumChannels)
            .arg(source.channels())};
    }

    const char* outputName = muxerName(format);
    const AVCodec* codec = findEncoder(format);
    if (!outputName || !codec)
        return {false, "Requested FFmpeg encoder is unavailable."};
    if (!encoderSupportsRate(codec, source.sampleRate()))
        return {false, QString("FFmpeg encoder does not support %1 Hz.").arg(source.sampleRate())};

    QFile::remove(destinationPath);
    AVFormatContext* output = nullptr;
    AVCodecContext* encoder = nullptr;
    SwrContext* converter = nullptr;
    AVPacket* packet = nullptr;
    AVStream* stream = nullptr;
    AVChannelLayout sourceLayout{};
    QString error;
    bool fileOpened = false;
    bool headerWritten = false;

    const QByteArray outputPath = destinationPath.toUtf8();
    int result = avformat_alloc_output_context2(&output, nullptr, outputName, outputPath.constData());
    if (result >= 0 && !output)
        result = AVERROR_UNKNOWN;
    if (result >= 0) {
        encoder = avcodec_alloc_context3(codec);
        stream = avformat_new_stream(output, nullptr);
        if (!encoder || !stream)
            result = AVERROR(ENOMEM);
    }

    configureSourceLayout(source, &sourceLayout);
    const int losslessBitDepth = options.losslessBitDepth <= 16 ? 16 : 24;
    if (result >= 0) {
        encoder->sample_rate = source.sampleRate();
        encoder->sample_fmt = encoderSampleFormat(codec, format, losslessBitDepth);
        encoder->time_base = AVRational{1, encoder->sample_rate};
        encoder->bit_rate = format == AudioCodecFormat::Mp3
            ? static_cast<int64_t>(std::clamp(options.lossyBitrateKbps, 64, 320)) * 1000
            : 0;
        if (encoder->codec_id == AV_CODEC_ID_VORBIS) {
            encoder->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
            encoder->flags |= AV_CODEC_FLAG_QSCALE;
            encoder->global_quality = std::clamp(options.vorbisQuality, 0, 10) * FF_QP2LAMBDA;
        }
        if (format == AudioCodecFormat::Flac || format == AudioCodecFormat::WavPack) {
            encoder->compression_level = std::clamp(options.compressionLevel, 0, 8);
            encoder->bits_per_raw_sample =
                (encoder->sample_fmt == AV_SAMPLE_FMT_S32 || encoder->sample_fmt == AV_SAMPLE_FMT_S32P)
                ? losslessBitDepth
                : 16;
        }
        result = av_channel_layout_copy(&encoder->ch_layout, &sourceLayout);
        if (output->oformat->flags & AVFMT_GLOBALHEADER)
            encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (result >= 0)
        result = avcodec_open2(encoder, codec, nullptr);
    if (result >= 0) {
        stream->time_base = encoder->time_base;
        result = avcodec_parameters_from_context(stream->codecpar, encoder);
    }
    if (result >= 0) {
        result = swr_alloc_set_opts2(&converter, &encoder->ch_layout, encoder->sample_fmt,
            encoder->sample_rate, &sourceLayout, AV_SAMPLE_FMT_FLT, source.sampleRate(), 0, nullptr);
    }
    if (result >= 0 && options.dither &&
        (format == AudioCodecFormat::Flac || format == AudioCodecFormat::WavPack)) {
        result = av_opt_set_int(converter, "dither_method", SWR_DITHER_TRIANGULAR, 0);
    }
    if (result >= 0)
        result = swr_init(converter);
    if (result >= 0 && !(output->oformat->flags & AVFMT_NOFILE)) {
        result = avio_open(&output->pb, outputPath.constData(), AVIO_FLAG_WRITE);
        fileOpened = result >= 0;
    }
    if (result >= 0) {
        result = avformat_write_header(output, nullptr);
        headerWritten = result >= 0;
    }
    if (result >= 0) {
        packet = av_packet_alloc();
        if (!packet)
            result = AVERROR(ENOMEM);
    }

    qint64 pts = 0;
    qint64 sourceFrame = 0;
    const qint64 totalFrames = source.frameCount();
    QVector<float> sourceSamples;
    bool cancelled = false;
    const bool variableFrames = (codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) != 0;
    const bool supportsSmallLastFrame = (codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0;
    const int codecFrameSize = encoder && encoder->frame_size > 0 ? encoder->frame_size : 1024;

    while (result >= 0 && sourceFrame < totalFrames) {
        if (progress && !progress(sourceFrame, totalFrames)) {
            cancelled = true;
            break;
        }
        const int inputFrames = static_cast<int>(std::min<qint64>(
            codecFrameSize, totalFrames - sourceFrame));
        const bool isLastPartialFrame = inputFrames < codecFrameSize && sourceFrame + inputFrames == totalFrames;
        const int outputFrames = variableFrames || (supportsSmallLastFrame && isLastPartialFrame)
            ? inputFrames
            : codecFrameSize;
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            result = AVERROR(ENOMEM);
            break;
        }
        frame->format = encoder->sample_fmt;
        frame->sample_rate = encoder->sample_rate;
        frame->nb_samples = outputFrames;
        result = av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
        if (result >= 0)
            result = av_frame_get_buffer(frame, 0);
        if (result >= 0) {
            if (!source.readFrames(sourceFrame, inputFrames, &sourceSamples, &sourceError)) {
                error = sourceError;
                result = AVERROR_INVALIDDATA;
            }
        }
        if (result >= 0) {
            const uint8_t* input[] = {
                reinterpret_cast<const uint8_t*>(sourceSamples.constData())
            };
            const int converted = swr_convert(converter, frame->data, outputFrames, input, inputFrames);
            if (converted < 0) {
                result = converted;
            } else {
                if (converted < outputFrames) {
                    av_samples_set_silence(frame->data, converted, outputFrames - converted,
                        encoder->ch_layout.nb_channels, encoder->sample_fmt);
                }
                frame->pts = pts;
                pts += outputFrames;
                result = avcodec_send_frame(encoder, frame);
                if (result >= 0 && !writePackets(encoder, output, stream, packet, &error))
                    result = AVERROR_INVALIDDATA;
            }
        }
        av_frame_free(&frame);
        sourceFrame += inputFrames;
    }

    if (!cancelled && progress && !progress(totalFrames, totalFrames))
        cancelled = true;

    if (!cancelled && result >= 0) {
        result = avcodec_send_frame(encoder, nullptr);
        if (result >= 0 && !writePackets(encoder, output, stream, packet, &error))
            result = AVERROR_INVALIDDATA;
    }
    if (!cancelled && result >= 0) {
        const int trailerResult = av_write_trailer(output);
        if (trailerResult < 0)
            result = trailerResult;
    }

    av_packet_free(&packet);
    swr_free(&converter);
    av_channel_layout_uninit(&sourceLayout);
    avcodec_free_context(&encoder);
    if (output) {
        if (fileOpened && output->pb) {
            const int closeResult = avio_closep(&output->pb);
            if (result >= 0 && closeResult < 0)
                result = closeResult;
        }
        avformat_free_context(output);
    }

    if (cancelled) {
        QFile::remove(destinationPath);
        return {false, "Encoding cancelled.", true};
    }
    if (result < 0 || !headerWritten || !QFileInfo::exists(destinationPath) || QFileInfo(destinationPath).size() <= 0) {
        QFile::remove(destinationPath);
        if (error.isEmpty())
            error = QString("FFmpeg encode failed: %1").arg(ffmpegError(result));
        return {false, error};
    }
    return {true, QString("%1 encoded with embedded FFmpeg, %2 channels%3.")
        .arg(AudioCodecBackend::displayName(format))
        .arg(source.channels())
        .arg(options.dither &&
             (format == AudioCodecFormat::Flac || format == AudioCodecFormat::WavPack)
            ? " and TPDF dither" : "")};
#else
    Q_UNUSED(sourceWav);
    Q_UNUSED(destinationPath);
    Q_UNUSED(format);
    Q_UNUSED(options);
    Q_UNUSED(progress);
    return {false, "Embedded FFmpeg encoder is not linked."};
#endif
}
