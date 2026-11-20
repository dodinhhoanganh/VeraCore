#include "VeraCoreQT.h"
#include "Preferences.h"
#include "TrackWidget.h"
#include "PluginManager.h"
#include "MidiEditorWindow.h"
#include "AudioCodecBackend.h"
#include "AudioFileDecoder.h"
#include "AudioStretchBackend.h"
#include "ExternalPluginHost.h"
#include "ProjectJsonMerge.h"
#include "AudioMediaResolver.h"
#include "AudioTempoAnalysis.h"
#include "StemSeparationService.h"
#include <QAction>
#include <QActionGroup>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QLabel>
#include <QVector>
#include <QMessageBox>
#include <QMenu>
#include <QEventLoop>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollBar>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QSpinBox>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTemporaryFile>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QUrl>
#include <QStorageInfo>
#include <QUuid>
#include <QVBoxLayout>
#include <functional>
#include <iterator>
#include <utility>
#include <QLocale>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>
#include <new>
#include <set>
#include <tuple>

namespace
{
int returnBusFromAudioFxTrackId(int trackId) noexcept
{
    const int returnBus = kVeraCoreReturnAudioFxTrackIdBase - trackId;
    return returnBus >= 0 && returnBus < kReturnBusCount ? returnBus : -1;
}

bool validPluginParameterTrackId(int trackId, bool audioFx, std::size_t trackCount) noexcept
{
    return (trackId >= 0 && trackId < static_cast<int>(trackCount)) ||
        (audioFx && returnBusFromAudioFxTrackId(trackId) >= 0);
}

struct AudioRenderSettings
{
    int sampleRate = 44100;
    AudioEncodeOptions encode;
    int normalizationMode = 0;
    double normalizePeakDb = -1.0;
    double normalizeLufs = -14.0;
    double normalizeToleranceLu = 0.5;
};

class OfflineLoudnessMeter
{
public:
    explicit OfflineLoudnessMeter(int sampleRate, int channelCount = 2)
        : m_windowFrames(std::max(1, static_cast<int>(std::llround(sampleRate * 0.4)))),
          m_stepFrames(std::max(1, static_cast<int>(std::llround(sampleRate * 0.1)))),
          m_energyRing(static_cast<size_t>(m_windowFrames), 0.0),
          m_shortTermWindowFrames(std::max(1, static_cast<int>(std::llround(sampleRate * 3.0)))),
          m_shortTermStepFrames(std::max(1, static_cast<int>(std::llround(sampleRate * 1.0)))),
          m_shortTermEnergyRing(static_cast<size_t>(m_shortTermWindowFrames), 0.0),
          m_channelCount(std::clamp(channelCount, 1, kVeraCoreMaxSpatialChannels))
    {
        const double rate = std::clamp(static_cast<double>(sampleRate), 8000.0, 384000.0);
        configureShelf(rate);
        configureHighPass(rate);
    }

    void process(const float* samples, int channelCount)
    {
        if (!samples) return;
        const int safeChannels = std::clamp(channelCount, 1, m_channelCount);
        double energy = 0.0;
        for (int channel = 0; channel < safeChannels; ++channel) {
            const double channelWeight = bs1770ChannelWeight(channel, safeChannels);
            if (channelWeight <= 0.0) continue; // LFE is excluded by ITU-R BS.1770.
            const double input = std::isfinite(samples[channel]) ? samples[channel] : 0.0f;
            const double weighted = m_highPass.process(
                m_shelf.process(input, static_cast<size_t>(channel)),
                static_cast<size_t>(channel));
            energy += channelWeight * weighted * weighted;
        }
        updateMomentary(energy);
        updateShortTerm(energy);
    }

    void process(float left, float right)
    {
        const std::array<float, 2> samples{left, right};
        process(samples.data(), 2);
    }

    double integratedLufs() const
    {
        constexpr double kAbsoluteGateLufs = -70.0;
        std::vector<double> absoluteGated;
        absoluteGated.reserve(m_blockEnergies.size());
        for (double energy : m_blockEnergies) {
            if (blockLoudness(energy) >= kAbsoluteGateLufs) absoluteGated.push_back(energy);
        }
        if (absoluteGated.empty()) return -std::numeric_limits<double>::infinity();
        const double ungatedEnergy = meanEnergy(absoluteGated);
        const double relativeGateLufs = blockLoudness(ungatedEnergy) - 10.0;
        const double gateLufs = std::max(kAbsoluteGateLufs, relativeGateLufs);
        double gatedEnergy = 0.0;
        size_t gatedCount = 0;
        for (double energy : absoluteGated) {
            if (blockLoudness(energy) < gateLufs) continue;
            gatedEnergy += energy;
            ++gatedCount;
        }
        return gatedCount > 0
            ? blockLoudness(gatedEnergy / static_cast<double>(gatedCount))
            : -std::numeric_limits<double>::infinity();
    }

    double momentaryMaximumLufs() const
    {
        return maximumLoudness(m_blockEnergies);
    }

    double shortTermMaximumLufs() const
    {
        return maximumLoudness(m_shortTermEnergies);
    }

    double loudnessRangeLu() const
    {
        constexpr double kAbsoluteGateLufs = -70.0;
        std::vector<double> absoluteGated;
        absoluteGated.reserve(m_shortTermEnergies.size());
        for (double energy : m_shortTermEnergies) {
            if (blockLoudness(energy) >= kAbsoluteGateLufs) absoluteGated.push_back(energy);
        }
        if (absoluteGated.empty()) return -std::numeric_limits<double>::infinity();
        const double relativeGateLufs = blockLoudness(meanEnergy(absoluteGated)) - 20.0;
        const double gateLufs = std::max(kAbsoluteGateLufs, relativeGateLufs);
        std::vector<double> gatedLoudness;
        gatedLoudness.reserve(absoluteGated.size());
        for (double energy : absoluteGated) {
            const double loudness = blockLoudness(energy);
            if (loudness >= gateLufs) gatedLoudness.push_back(loudness);
        }
        if (gatedLoudness.empty()) return -std::numeric_limits<double>::infinity();
        std::sort(gatedLoudness.begin(), gatedLoudness.end());
        return std::max(0.0, percentile(gatedLoudness, 0.95) -
                             percentile(gatedLoudness, 0.10));
    }

private:
    void updateMomentary(double energy)
    {
        if (m_filledFrames < m_windowFrames) {
            m_energyRing[m_ringWrite] = energy;
            m_windowEnergy += energy;
            m_ringWrite = (m_ringWrite + 1u) % m_energyRing.size();
            ++m_filledFrames;
            if (m_filledFrames == m_windowFrames) {
                appendBlock();
                m_framesSinceBlock = 0;
            }
            return;
        }
        m_windowEnergy += energy - m_energyRing[m_ringWrite];
        m_energyRing[m_ringWrite] = energy;
        m_ringWrite = (m_ringWrite + 1u) % m_energyRing.size();
        if (++m_framesSinceBlock >= m_stepFrames) {
            appendBlock();
            m_framesSinceBlock = 0;
        }
    }

    void updateShortTerm(double energy)
    {
        if (m_shortTermFilledFrames < m_shortTermWindowFrames) {
            m_shortTermEnergyRing[m_shortTermRingWrite] = energy;
            m_shortTermWindowEnergy += energy;
            m_shortTermRingWrite = (m_shortTermRingWrite + 1u) % m_shortTermEnergyRing.size();
            ++m_shortTermFilledFrames;
            if (m_shortTermFilledFrames == m_shortTermWindowFrames) {
                appendShortTermBlock();
                m_shortTermFramesSinceBlock = 0;
            }
            return;
        }
        m_shortTermWindowEnergy += energy - m_shortTermEnergyRing[m_shortTermRingWrite];
        m_shortTermEnergyRing[m_shortTermRingWrite] = energy;
        m_shortTermRingWrite = (m_shortTermRingWrite + 1u) % m_shortTermEnergyRing.size();
        if (++m_shortTermFramesSinceBlock >= m_shortTermStepFrames) {
            appendShortTermBlock();
            m_shortTermFramesSinceBlock = 0;
        }
    }
    struct MultichannelBiquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        std::array<double, kVeraCoreMaxSpatialChannels> z1{};
        std::array<double, kVeraCoreMaxSpatialChannels> z2{};
        double process(double input, size_t channel)
        {
            const double output = b0 * input + z1[channel];
            z1[channel] = b1 * input - a1 * output + z2[channel];
            z2[channel] = b2 * input - a2 * output;
            return std::isfinite(output) ? output : 0.0;
        }
    };

    static double bs1770ChannelWeight(int channel, int channelCount) noexcept
    {
        if (channelCount > 2 && channel == 3) return 0.0; // Canonical LFE index.
        if (channelCount > 2 && channel >= 4 && channel <= 7) return 1.41;
        return 1.0;
    }

    void configureShelf(double sampleRate)
    {
        constexpr double frequency = 1681.974450955533;
        constexpr double gainDb = 3.999843853973347;
        constexpr double q = 0.7071752369554196;
        constexpr double shelfSlope = 0.4996667741545416;
        const double k = std::tan(3.14159265358979323846 * frequency / sampleRate);
        const double vh = std::pow(10.0, gainDb / 20.0);
        const double vb = std::pow(vh, shelfSlope);
        const double denominator = 1.0 + k / q + k * k;
        m_shelf.b0 = (vh + vb * k / q + k * k) / denominator;
        m_shelf.b1 = 2.0 * (k * k - vh) / denominator;
        m_shelf.b2 = (vh - vb * k / q + k * k) / denominator;
        m_shelf.a1 = 2.0 * (k * k - 1.0) / denominator;
        m_shelf.a2 = (1.0 - k / q + k * k) / denominator;
    }

    void configureHighPass(double sampleRate)
    {
        constexpr double frequency = 38.13547087602444;
        constexpr double q = 0.5003270373238773;
        const double k = std::tan(3.14159265358979323846 * frequency / sampleRate);
        const double denominator = 1.0 + k / q + k * k;
        m_highPass.b0 = 1.0;
        m_highPass.b1 = -2.0;
        m_highPass.b2 = 1.0;
        m_highPass.a1 = 2.0 * (k * k - 1.0) / denominator;
        m_highPass.a2 = (1.0 - k / q + k * k) / denominator;
    }

    static double blockLoudness(double energy)
    {
        return energy > 0.0 && std::isfinite(energy)
            ? -0.691 + 10.0 * std::log10(energy)
            : -std::numeric_limits<double>::infinity();
    }

    static double meanEnergy(const std::vector<double>& energies)
    {
        double sum = 0.0;
        for (double energy : energies) sum += energy;
        return sum / static_cast<double>(energies.size());
    }

    static double maximumLoudness(const std::vector<double>& energies)
    {
        double maximum = -std::numeric_limits<double>::infinity();
        for (double energy : energies) maximum = std::max(maximum, blockLoudness(energy));
        return maximum;
    }

    static double percentile(const std::vector<double>& sortedValues, double fraction)
    {
        if (sortedValues.empty()) return -std::numeric_limits<double>::infinity();
        const double position = std::clamp(fraction, 0.0, 1.0) *
            static_cast<double>(sortedValues.size() - 1u);
        const size_t lower = static_cast<size_t>(std::floor(position));
        const size_t upper = std::min(lower + 1u, sortedValues.size() - 1u);
        const double blend = position - static_cast<double>(lower);
        return sortedValues[lower] + (sortedValues[upper] - sortedValues[lower]) * blend;
    }

    void appendBlock()
    {
        const double energy = std::max(0.0, m_windowEnergy) /
            static_cast<double>(m_windowFrames);
        if (std::isfinite(energy)) m_blockEnergies.push_back(energy);
    }

    void appendShortTermBlock()
    {
        const double energy = std::max(0.0, m_shortTermWindowEnergy) /
            static_cast<double>(m_shortTermWindowFrames);
        if (std::isfinite(energy)) m_shortTermEnergies.push_back(energy);
    }

    MultichannelBiquad m_shelf;
    MultichannelBiquad m_highPass;
    int m_windowFrames = 1;
    int m_stepFrames = 1;
    int m_filledFrames = 0;
    int m_framesSinceBlock = 0;
    size_t m_ringWrite = 0;
    double m_windowEnergy = 0.0;
    std::vector<double> m_energyRing;
    std::vector<double> m_blockEnergies;
    int m_shortTermWindowFrames = 1;
    int m_shortTermStepFrames = 1;
    int m_shortTermFilledFrames = 0;
    int m_shortTermFramesSinceBlock = 0;
    size_t m_shortTermRingWrite = 0;
    double m_shortTermWindowEnergy = 0.0;
    std::vector<double> m_shortTermEnergyRing;
    std::vector<double> m_shortTermEnergies;
    int m_channelCount = 2;
};

static bool editAudioRenderSettings(QWidget* parent, AudioCodecFormat format,
                                    int defaultSampleRate, int masterChannels,
                                    AudioRenderSettings& result)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("AudioRender"));
    QDialog dialog(parent);
    dialog.setWindowTitle(QString("Render %1").arg(AudioCodecBackend::displayName(format)));
    dialog.setModal(true);
    dialog.setMinimumWidth(390);
    QVBoxLayout* root = new QVBoxLayout(&dialog);
    QLabel* heading = new QLabel(QString("%1 EXPORT").arg(AudioCodecBackend::displayName(format)), &dialog);
    heading->setStyleSheet("font-weight:bold; font-size:14px;");
    root->addWidget(heading);
    QFormLayout* form = new QFormLayout();
    QComboBox* sampleRate = new QComboBox(&dialog);
    const QList<int> rates{44100, 48000, 88200, 96000, 176400, 192000};
    for (int rate : rates) sampleRate->addItem(QString("%1 Hz").arg(rate), rate);
    const int storedRate = settings.value(QStringLiteral("SampleRate"), defaultSampleRate).toInt();
    int rateIndex = sampleRate->findData(storedRate);
    if (rateIndex < 0) {
        sampleRate->addItem(QString("%1 Hz").arg(defaultSampleRate), defaultSampleRate);
        rateIndex = sampleRate->count() - 1;
    }
    sampleRate->setCurrentIndex(rateIndex);
    form->addRow("Sample rate", sampleRate);

    QComboBox* mp3Bitrate = new QComboBox(&dialog);
    for (int bitrate : {128, 160, 192, 224, 256, 320})
        mp3Bitrate->addItem(QString("%1 kbps CBR").arg(bitrate), bitrate);
    mp3Bitrate->setCurrentIndex(qMax(0, mp3Bitrate->findData(
        settings.value(QStringLiteral("Mp3BitrateKbps"), 256).toInt())));
    if (format == AudioCodecFormat::Mp3) form->addRow("Bitrate", mp3Bitrate);

    QSpinBox* vorbisQuality = new QSpinBox(&dialog);
    vorbisQuality->setRange(0, 10);
    vorbisQuality->setValue(qBound(0, settings.value(QStringLiteral("VorbisQuality"), 6).toInt(), 10));
    vorbisQuality->setSuffix(" / 10 VBR");
    if (format == AudioCodecFormat::OggVorbis) form->addRow("Quality", vorbisQuality);

    QSpinBox* compression = new QSpinBox(&dialog);
    compression->setRange(0, 8);
    compression->setValue(qBound(0, settings.value(QStringLiteral("CompressionLevel"), 5).toInt(), 8));
    compression->setToolTip("Higher compression reduces file size but increases offline render time; audio stays lossless.");
    if (format == AudioCodecFormat::Flac || format == AudioCodecFormat::WavPack)
        form->addRow("Compression", compression);

    QComboBox* bitDepth = new QComboBox(&dialog);
    bitDepth->addItem("16-bit PCM", 16);
    bitDepth->addItem("24-bit PCM", 24);
    bitDepth->setCurrentIndex(qMax(0, bitDepth->findData(
        settings.value(QStringLiteral("LosslessBitDepth"), 24).toInt())));
    if (format == AudioCodecFormat::Flac || format == AudioCodecFormat::WavPack)
        form->addRow("Bit depth", bitDepth);

    QComboBox* wavEncoding = new QComboBox(&dialog);
    wavEncoding->addItem("16-bit PCM", 16);
    wavEncoding->addItem("24-bit PCM", 24);
    wavEncoding->addItem("32-bit IEEE float", 32);
    wavEncoding->setCurrentIndex(qMax(0, wavEncoding->findData(
        settings.value(QStringLiteral("WavBitDepth"), 32).toInt())));
    const bool supportsIntegerDither = format == AudioCodecFormat::Wav ||
        format == AudioCodecFormat::Flac || format == AudioCodecFormat::WavPack;
    QCheckBox* integerDither = new QCheckBox("TPDF dither", &dialog);
    integerDither->setChecked(settings.value(QStringLiteral("IntegerDither"),
        settings.value(QStringLiteral("WavDither"), false)).toBool());
    integerDither->setToolTip(
        "Add deterministic triangular dither when reducing the 32-bit float master to 16/24-bit integer audio.");
    auto updateIntegerDither = [format, wavEncoding, integerDither]() {
        integerDither->setEnabled(format != AudioCodecFormat::Wav ||
            wavEncoding->currentData().toInt() != 32);
    };
    QObject::connect(wavEncoding, qOverload<int>(&QComboBox::currentIndexChanged),
                     &dialog, updateIntegerDither);
    updateIntegerDither();
    if (format == AudioCodecFormat::Wav) {
        form->addRow("Encoding", wavEncoding);
    }
    if (supportsIntegerDither) form->addRow("Dither", integerDither);

    QComboBox* normalizationMode = new QComboBox(&dialog);
    normalizationMode->addItem("Off", 0);
    normalizationMode->addItem("True Peak", 1);
    normalizationMode->addItem("Loudness (LUFS-I + true-peak ceiling)", 2);
    const int storedNormalizeMode = settings.contains(QStringLiteral("NormalizeMode"))
        ? settings.value(QStringLiteral("NormalizeMode"), 0).toInt()
        : (settings.value(QStringLiteral("NormalizeMaster"), false).toBool() ? 1 : 0);
    normalizationMode->setCurrentIndex(qMax(0, normalizationMode->findData(
        qBound(0, storedNormalizeMode, 2))));
    normalizationMode->setToolTip(
        "True Peak reaches the dBTP target. Loudness reaches LUFS-I without exceeding the dBTP ceiling.");
    QDoubleSpinBox* normalizePeak = new QDoubleSpinBox(&dialog);
    normalizePeak->setRange(-12.0, 0.0);
    normalizePeak->setDecimals(1);
    normalizePeak->setSingleStep(0.1);
    normalizePeak->setSuffix(" dBTP");
    normalizePeak->setValue(qBound(-12.0,
        settings.value(QStringLiteral("NormalizePeakDb"), -1.0).toDouble(), 0.0));
    QDoubleSpinBox* normalizeLufs = new QDoubleSpinBox(&dialog);
    normalizeLufs->setRange(-36.0, -5.0);
    normalizeLufs->setDecimals(1);
    normalizeLufs->setSingleStep(0.5);
    normalizeLufs->setSuffix(" LUFS-I");
    normalizeLufs->setValue(qBound(-36.0,
        settings.value(QStringLiteral("NormalizeLufs"), -14.0).toDouble(), -5.0));
    QDoubleSpinBox* normalizeTolerance = new QDoubleSpinBox(&dialog);
    normalizeTolerance->setRange(0.1, 2.0);
    normalizeTolerance->setDecimals(1);
    normalizeTolerance->setSingleStep(0.1);
    normalizeTolerance->setSuffix(" LU");
    normalizeTolerance->setValue(qBound(0.1,
        settings.value(QStringLiteral("NormalizeToleranceLu"), 0.5).toDouble(), 2.0));
    normalizeTolerance->setToolTip(
        "Maximum absolute LUFS-I target deviation accepted by the post-render compliance check.");
    QComboBox* normalizePreset = new QComboBox(&dialog);
    normalizePreset->addItem("Custom", 0);
    normalizePreset->addItem("Streaming · -14 LUFS-I / -1 dBTP", 1);
    normalizePreset->addItem("Music · -16 LUFS-I / -1 dBTP", 2);
    normalizePreset->addItem("EBU R128 · -23 LUFS-I / -1 dBTP", 3);
    normalizePreset->addItem("ATSC A/85 · -24 LUFS-I / -2 dBTP", 4);
    normalizePreset->setCurrentIndex(qMax(0, normalizePreset->findData(qBound(0,
        settings.value(QStringLiteral("NormalizePreset"), 0).toInt(), 4))));
    const auto applyNormalizePreset = [normalizePreset, normalizePeak, normalizeLufs]() {
        const int preset = normalizePreset->currentData().toInt();
        if (preset == 0) return;
        double targetLufs = -14.0;
        double ceilingDbtp = -1.0;
        if (preset == 2) targetLufs = -16.0;
        else if (preset == 3) targetLufs = -23.0;
        else if (preset == 4) {
            targetLufs = -24.0;
            ceilingDbtp = -2.0;
        }
        const QSignalBlocker peakBlocker(normalizePeak);
        const QSignalBlocker loudnessBlocker(normalizeLufs);
        normalizePeak->setValue(ceilingDbtp);
        normalizeLufs->setValue(targetLufs);
    };
    QObject::connect(normalizePreset, qOverload<int>(&QComboBox::currentIndexChanged),
                     &dialog, applyNormalizePreset);
    const auto markNormalizePresetCustom = [normalizePreset]() {
        if (normalizePreset->currentData().toInt() == 0) return;
        const QSignalBlocker blocker(normalizePreset);
        normalizePreset->setCurrentIndex(0);
    };
    QObject::connect(normalizePeak, qOverload<double>(&QDoubleSpinBox::valueChanged),
                     &dialog, markNormalizePresetCustom);
    QObject::connect(normalizeLufs, qOverload<double>(&QDoubleSpinBox::valueChanged),
                     &dialog, markNormalizePresetCustom);
    applyNormalizePreset();
    auto updateNormalizationControls = [normalizationMode, normalizePreset,
                                        normalizePeak, normalizeLufs, normalizeTolerance]() {
        const int mode = normalizationMode->currentData().toInt();
        normalizePreset->setEnabled(mode == 2);
        normalizePeak->setEnabled(mode != 0);
        normalizeLufs->setEnabled(mode == 2);
        normalizeTolerance->setEnabled(mode == 2);
    };
    QObject::connect(normalizationMode, qOverload<int>(&QComboBox::currentIndexChanged),
                     &dialog, updateNormalizationControls);
    updateNormalizationControls();
    form->addRow("Normalize", normalizationMode);
    form->addRow("Loudness preset", normalizePreset);
    form->addRow("Peak target / ceiling", normalizePeak);
    form->addRow("Loudness target", normalizeLufs);
    form->addRow("Target tolerance", normalizeTolerance);

    root->addLayout(form);
    const QString renderLayout = masterChannels > 2
        ? QString("%1-channel spatial master render").arg(masterChannels)
        : QStringLiteral("Stereo master render");
    QLabel* note = new QLabel(renderLayout +
        ". Plugin delay compensation and plugin tails are included.", &dialog);
    note->setWordWrap(true);
    note->setStyleSheet("color:#8f968f;");
    root->addWidget(note);
    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("Choose Output...");
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    root->addWidget(buttons);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (accepted) {
        result.sampleRate = sampleRate->currentData().toInt();
        result.encode.lossyBitrateKbps = mp3Bitrate->currentData().toInt();
        result.encode.vorbisQuality = vorbisQuality->value();
        result.encode.compressionLevel = compression->value();
        result.encode.losslessBitDepth = bitDepth->currentData().toInt();
        result.encode.wavBitDepth = wavEncoding->currentData().toInt();
        result.encode.wavFloatingPoint = result.encode.wavBitDepth == 32;
        result.encode.dither = supportsIntegerDither && integerDither->isChecked() &&
            (format != AudioCodecFormat::Wav || result.encode.wavBitDepth != 32);
        result.normalizationMode = normalizationMode->currentData().toInt();
        result.normalizePeakDb = normalizePeak->value();
        result.normalizeLufs = normalizeLufs->value();
        result.normalizeToleranceLu = normalizeTolerance->value();
        settings.setValue(QStringLiteral("SampleRate"), result.sampleRate);
        settings.setValue(QStringLiteral("Mp3BitrateKbps"), result.encode.lossyBitrateKbps);
        settings.setValue(QStringLiteral("VorbisQuality"), result.encode.vorbisQuality);
        settings.setValue(QStringLiteral("CompressionLevel"), result.encode.compressionLevel);
        settings.setValue(QStringLiteral("LosslessBitDepth"), result.encode.losslessBitDepth);
        settings.setValue(QStringLiteral("WavBitDepth"), result.encode.wavBitDepth);
        if (supportsIntegerDither)
            settings.setValue(QStringLiteral("IntegerDither"), result.encode.dither);
        settings.setValue(QStringLiteral("NormalizeMode"), result.normalizationMode);
        settings.setValue(QStringLiteral("NormalizeMaster"), result.normalizationMode != 0);
        settings.setValue(QStringLiteral("NormalizePreset"), normalizePreset->currentData().toInt());
        settings.setValue(QStringLiteral("NormalizePeakDb"), result.normalizePeakDb);
        settings.setValue(QStringLiteral("NormalizeLufs"), result.normalizeLufs);
        settings.setValue(QStringLiteral("NormalizeToleranceLu"), result.normalizeToleranceLu);
    }
    settings.endGroup();
    return accepted;
}

struct RtMidiEvent
{
    double beat = 0.0;
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    int layerId = 0;
    float gain = 1.0f;
    float pan = 0.0f;
    bool externalPlugin = false;
    bool isSlide = false;
    bool isPortamento = false;
    double glideDurationBeats = 0.0;
    bool isKeyswitch = false;
};

constexpr int kLegacyDivision = 480;
constexpr int kDefaultProjectPpq = 960;
constexpr int kMinProjectPpq = 24;
constexpr int kMaxProjectPpq = 9600;
constexpr int kDefaultTempoBpm = 120;
constexpr size_t kMaxTempoMapPoints = 4096;
constexpr size_t kMaxTimeSignatureMapPoints = 4096;
constexpr int kLayerIdStride = 1024;
constexpr int kMaxOfflineSynthVoices = 512;
constexpr int kPluginPathRole = Qt::UserRole;
constexpr int kPluginNameRole = Qt::UserRole + 1;
constexpr int kPluginTypeRole = Qt::UserRole + 2;
constexpr int kPluginCategoryRole = Qt::UserRole + 3;
constexpr int kPluginUidRole = Qt::UserRole + 4;
constexpr int kSamplePathRole = Qt::UserRole + 10;
constexpr uint8_t kMidiCcPan = 10;
constexpr uint8_t kMidiCcModX = 16;
constexpr uint8_t kMidiCcModY = 17;
constexpr int kMaxPluginStateBytes = 16 * 1024 * 1024;
constexpr qsizetype kMaxAraArchiveBytes = 128 * 1024 * 1024;
constexpr qint64 kMaxProjectAraArchiveBytes = 256ll * 1024ll * 1024ll;
constexpr qint64 kMaxProjectFileBytes = 512ll * 1024ll * 1024ll;
constexpr int kMaxAraArchives = 64;
constexpr int kMaxAraPersistentIdBytes = 1024;
constexpr size_t kMaxDecodedCacheEntries = 16;

static QByteArray recoveryChecksum(const QString& path)
{
    QFile checksumFile(path + QStringLiteral(".sha256"));
    if (!checksumFile.open(QIODevice::ReadOnly) || checksumFile.size() > 128)
        return {};
    const QByteArray checksum = checksumFile.readAll().trimmed();
    return checksum.size() == 64 ? checksum : QByteArray{};
}

static bool recoveryPayloadMatchesChecksum(const QString& path,
                                           const QByteArray& payload)
{
    const QByteArray expected = recoveryChecksum(path);
    return !expected.isEmpty() && expected == QCryptographicHash::hash(
        payload, QCryptographicHash::Sha256).toHex();
}

static bool recoveryFileMatchesChecksum(const QString& path)
{
    QFile recoveryFile(path);
    if (!recoveryFile.open(QIODevice::ReadOnly) || recoveryFile.size() <= 0 ||
        recoveryFile.size() > kMaxProjectFileBytes) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&recoveryFile)) return false;
    const QByteArray expected = recoveryChecksum(path);
    return !expected.isEmpty() && expected == hash.result().toHex();
}

static void recordRecoveryCheckpointTelemetry(const QString& path, quint64 revision,
                                              bool succeeded,
                                              const QString& failureReason = {})
{
    QSettings settings;
    const qint64 nowUtcMs = QDateTime::currentMSecsSinceEpoch();
    settings.setValue(QStringLiteral("Recovery/LastAttemptUtcMs"), nowUtcMs);
    settings.setValue(QStringLiteral("Recovery/LastAttemptPath"), path);
    settings.setValue(QStringLiteral("Recovery/LastAttemptRevision"),
                      QVariant::fromValue<qulonglong>(revision));
    if (succeeded) {
        settings.setValue(QStringLiteral("Recovery/LastSuccessUtcMs"), nowUtcMs);
        settings.setValue(QStringLiteral("Recovery/LastSuccessPath"), path);
        settings.setValue(QStringLiteral("Recovery/LastSuccessRevision"),
                          QVariant::fromValue<qulonglong>(revision));
        settings.setValue(QStringLiteral("Recovery/ConsecutiveFailures"), 0);
        settings.setValue(QStringLiteral("Recovery/LastFailureReason"), QString());
        settings.setValue(QStringLiteral("Recovery/EscalationShown"), false);
        settings.setValue(QStringLiteral("Recovery/RetryDelaySeconds"), 0);
        settings.setValue(QStringLiteral("Recovery/NextRetryUtcMs"), 0);
    } else {
        const QString reason = failureReason.trimmed().isEmpty()
            ? QStringLiteral("Unknown recovery checkpoint failure.")
            : failureReason.trimmed().left(1024);
        const int failures = std::clamp(settings.value(
            QStringLiteral("Recovery/ConsecutiveFailures"), 0).toInt(), 0, 999999);
        settings.setValue(QStringLiteral("Recovery/ConsecutiveFailures"), failures + 1);
        settings.setValue(QStringLiteral("Recovery/LastFailureUtcMs"), nowUtcMs);
        settings.setValue(QStringLiteral("Recovery/LastFailureReason"), reason);
    }
    settings.sync();
}

static bool isManagedRecoveryPath(const QString& path)
{
    const QFileInfo info(path);
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
    if (info.absolutePath().compare(
            QDir(QStringLiteral("M:/VeraCore-Recovery")).absolutePath(), pathCase) != 0)
        return false;
    for (int slot = 0; slot < 5; ++slot) {
        if (info.fileName().compare(QStringLiteral("AutoRecovery.%1.vcp").arg(slot),
                                    pathCase) == 0) return true;
    }
    return false;
}

struct RecoveryStorageStatus
{
    bool ready = false;
    bool readOnly = true;
    bool probeAttempted = false;
    bool probeSucceeded = false;
    qint64 bytesAvailable = -1;
    QString detail;
};

static RecoveryStorageStatus inspectRecoveryStorage(qint64 requiredBytes,
                                                    bool performWriteProbe)
{
    constexpr qint64 kRecoveryFreeSpaceReserve = 64ll * 1024ll * 1024ll;
    const QString rootPath = QStringLiteral("M:/VeraCore-Recovery");
    RecoveryStorageStatus result;
    QStorageInfo storage(QDir(rootPath).rootPath());
    storage.refresh();
    if (!storage.isValid() || !storage.isReady()) {
        result.detail = QStringLiteral("Recovery volume M: is unavailable.");
        return result;
    }
    result.readOnly = storage.isReadOnly();
    result.bytesAvailable = storage.bytesAvailable();
    if (result.readOnly) {
        result.detail = QStringLiteral("Recovery volume M: is read-only.");
        return result;
    }
    const qint64 payloadBytes = std::max<qint64>(0, requiredBytes);
    const qint64 requiredWithReserve = payloadBytes >
            std::numeric_limits<qint64>::max() - kRecoveryFreeSpaceReserve
        ? std::numeric_limits<qint64>::max()
        : payloadBytes + kRecoveryFreeSpaceReserve;
    if (result.bytesAvailable < requiredWithReserve) {
        result.detail = QStringLiteral(
            "Recovery volume needs %1 more bytes of free space.")
            .arg(requiredWithReserve - result.bytesAvailable);
        return result;
    }
    result.ready = true;
    result.detail = QStringLiteral("Recovery storage preflight passed.");
    if (!performWriteProbe) return result;

    result.probeAttempted = true;
    if (!QDir().mkpath(rootPath)) {
        result.ready = false;
        result.detail = QStringLiteral("Could not create the recovery directory.");
        return result;
    }
    QTemporaryFile probe(QDir(rootPath).filePath(
        QStringLiteral(".VeraCoreRecoveryProbe.XXXXXX")));
    probe.setAutoRemove(true);
    if (!probe.open()) {
        result.ready = false;
        result.detail = QStringLiteral("Recovery write probe could not open: %1")
            .arg(probe.errorString());
        return result;
    }
    const QByteArray payload(4096, '\x5a');
    if (probe.write(payload) != payload.size() || !probe.flush() ||
        !probe.seek(0) || probe.read(payload.size()) != payload) {
        result.ready = false;
        result.detail = QStringLiteral("Recovery write/read probe failed: %1")
            .arg(probe.errorString());
        return result;
    }
    result.probeSucceeded = true;
    result.detail = QStringLiteral(
        "Recovery write/read self-test passed; temporary probe removed.");
    return result;
}

static QString projectBackupKey(const QString& projectPath)
{
    QString identity = QFileInfo(projectPath).absoluteFilePath();
#ifdef Q_OS_WIN
    identity = identity.toLower();
#endif
    return QString::fromLatin1(QCryptographicHash::hash(
        identity.toUtf8(), QCryptographicHash::Sha256).toHex());
}

static QString projectIntegrityErrorText()
{
    return QStringLiteral(
        "Project SHA-256 verification failed; the file or checksum is corrupt.");
}

static bool createPreSaveProjectBackup(const QString& projectPath,
                                       QString* errorText)
{
    const QFileInfo sourceInfo(projectPath);
    if (!sourceInfo.exists()) return true;
    if (!sourceInfo.isFile() || sourceInfo.size() <= 0 ||
        sourceInfo.size() > kMaxProjectFileBytes) {
        if (errorText) *errorText = QStringLiteral(
            "Existing project is invalid; refusing to overwrite it without a backup.");
        return false;
    }

    QDir backupDirectory(QDir(QStringLiteral("M:/VeraCore-Recovery/ProjectBackups"))
        .filePath(projectBackupKey(sourceInfo.absoluteFilePath())));
    if (!QDir().mkpath(backupDirectory.absolutePath())) {
        if (errorText) *errorText = QStringLiteral(
            "Could not create the project backup directory on M:.");
        return false;
    }

    constexpr int kProjectBackupGenerationCount = 5;
    int targetSlot = 0;
    QDateTime oldestModified;
    for (int slot = 0; slot < kProjectBackupGenerationCount; ++slot) {
        const QFileInfo candidate(backupDirectory.filePath(
            QStringLiteral("Backup.%1.vcp").arg(slot)));
        if (!candidate.exists()) {
            targetSlot = slot;
            oldestModified = {};
            break;
        }
        if (!oldestModified.isValid() || candidate.lastModified() < oldestModified) {
            oldestModified = candidate.lastModified();
            targetSlot = slot;
        }
    }

    QFile input(sourceInfo.absoluteFilePath());
    const QString backupPath = backupDirectory.filePath(
        QStringLiteral("Backup.%1.vcp").arg(targetSlot));
    QSaveFile output(backupPath);
    output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        output.cancelWriting();
        if (errorText) *errorText = QStringLiteral(
            "Could not stage the pre-save project backup.");
        return false;
    }

    constexpr qint64 kBackupCopyBlockBytes = 1024 * 1024;
    qint64 copiedBytes = 0;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (copiedBytes < sourceInfo.size()) {
        const QByteArray block = input.read(std::min(
            kBackupCopyBlockBytes, sourceInfo.size() - copiedBytes));
        if (block.isEmpty() || output.write(block) != block.size()) {
            output.cancelWriting();
            if (errorText) *errorText = QStringLiteral(
                "Could not copy the existing project into its backup generation.");
            return false;
        }
        hash.addData(block);
        copiedBytes += block.size();
    }
    if (copiedBytes != sourceInfo.size() || !input.atEnd() || !output.commit()) {
        output.cancelWriting();
        if (errorText) *errorText = QStringLiteral(
            "Could not atomically commit the pre-save project backup.");
        return false;
    }

    const QByteArray digest = hash.result().toHex();
    QSaveFile checksumFile(backupPath + QStringLiteral(".sha256"));
    checksumFile.setDirectWriteFallback(false);
    if (!checksumFile.open(QIODevice::WriteOnly) ||
        checksumFile.write(digest) != digest.size() || !checksumFile.commit()) {
        checksumFile.cancelWriting();
        if (errorText) *errorText = QStringLiteral(
            "Could not atomically commit the project-backup checksum.");
        return false;
    }

    const QByteArray sourcePathUtf8 = sourceInfo.absoluteFilePath().toUtf8();
    QSaveFile sourcePathFile(backupDirectory.filePath(QStringLiteral("SourcePath.txt")));
    sourcePathFile.setDirectWriteFallback(false);
    if (!sourcePathFile.open(QIODevice::WriteOnly) ||
        sourcePathFile.write(sourcePathUtf8) != sourcePathUtf8.size() ||
        !sourcePathFile.commit()) {
        sourcePathFile.cancelWriting();
        if (errorText) *errorText = QStringLiteral(
            "Could not record the project-backup source path.");
        return false;
    }
    return true;
}

struct ProjectBackupCandidate
{
    QString backupPath;
    QString sourcePath;
    QDateTime modifiedUtc;
    qint64 sizeBytes = 0;
};

static std::vector<ProjectBackupCandidate> discoverProjectBackups()
{
    std::vector<ProjectBackupCandidate> result;
    const QDir root(QStringLiteral("M:/VeraCore-Recovery/ProjectBackups"));
    const QFileInfoList projectDirectories = root.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
        QDir::Time | QDir::Reversed);
    constexpr int kMaxBackupDirectories = 1024;
    constexpr int kMaxBackupCandidates = 256;
    int visitedDirectories = 0;
    for (const QFileInfo& directoryInfo : projectDirectories) {
        if (++visitedDirectories > kMaxBackupDirectories ||
            static_cast<int>(result.size()) >= kMaxBackupCandidates) break;
        const QDir directory(directoryInfo.absoluteFilePath());
        QFile sourcePathFile(directory.filePath(QStringLiteral("SourcePath.txt")));
        if (!sourcePathFile.open(QIODevice::ReadOnly) ||
            sourcePathFile.size() <= 0 || sourcePathFile.size() > 32768) continue;
        const QString sourcePath = QString::fromUtf8(sourcePathFile.readAll()).trimmed();
        const QFileInfo sourceInfo(sourcePath);
        if (sourcePath.isEmpty() || !sourceInfo.isAbsolute() ||
            sourceInfo.suffix().compare(QStringLiteral("vcp"), Qt::CaseInsensitive) != 0) continue;
        if (directory.dirName().compare(projectBackupKey(sourceInfo.absoluteFilePath()),
                                        Qt::CaseInsensitive) != 0) continue;

        for (int slot = 0; slot < 5 &&
             static_cast<int>(result.size()) < kMaxBackupCandidates; ++slot) {
            const QString backupPath = directory.filePath(
                QStringLiteral("Backup.%1.vcp").arg(slot));
            const QFileInfo backupInfo(backupPath);
            if (!backupInfo.exists() || !backupInfo.isFile() ||
                !recoveryFileMatchesChecksum(backupPath)) continue;
            result.push_back({backupInfo.absoluteFilePath(),
                              sourceInfo.absoluteFilePath(),
                              backupInfo.lastModified().toUTC(),
                              backupInfo.size()});
        }
    }
    std::sort(result.begin(), result.end(),
              [](const ProjectBackupCandidate& left,
                 const ProjectBackupCandidate& right) {
                  return left.modifiedUtc > right.modifiedUtc;
              });
    return result;
}

static std::vector<ProjectBackupCandidate> validatedBackupsForProject(
    const QString& projectPath)
{
    std::vector<ProjectBackupCandidate> result;
    const QFileInfo requestedInfo(projectPath);
    if (!requestedInfo.isAbsolute() ||
        requestedInfo.suffix().compare(QStringLiteral("vcp"), Qt::CaseInsensitive) != 0)
        return result;
    const QString requestedPath = requestedInfo.absoluteFilePath();
    const QDir directory(QDir(QStringLiteral("M:/VeraCore-Recovery/ProjectBackups"))
        .filePath(projectBackupKey(requestedPath)));
    QFile sourcePathFile(directory.filePath(QStringLiteral("SourcePath.txt")));
    if (!sourcePathFile.open(QIODevice::ReadOnly) || sourcePathFile.size() <= 0 ||
        sourcePathFile.size() > 32768) return result;
    const QFileInfo storedInfo(QString::fromUtf8(sourcePathFile.readAll()).trimmed());
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sourcePathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sourcePathCase = Qt::CaseSensitive;
#endif
    if (!storedInfo.isAbsolute() || storedInfo.absoluteFilePath().compare(
            requestedPath, sourcePathCase) != 0 ||
        directory.dirName().compare(projectBackupKey(storedInfo.absoluteFilePath()),
                                    Qt::CaseInsensitive) != 0) return result;
    for (int slot = 0; slot < 5; ++slot) {
        const QString backupPath = directory.filePath(
            QStringLiteral("Backup.%1.vcp").arg(slot));
        const QFileInfo backupInfo(backupPath);
        if (!backupInfo.exists() || !backupInfo.isFile() || backupInfo.isSymLink() ||
            !recoveryFileMatchesChecksum(backupPath)) continue;
        result.push_back({backupInfo.absoluteFilePath(), requestedPath,
                          backupInfo.lastModified().toUTC(), backupInfo.size()});
    }
    std::sort(result.begin(), result.end(),
              [](const ProjectBackupCandidate& left,
                 const ProjectBackupCandidate& right) {
                  return left.modifiedUtc > right.modifiedUtc;
              });
    return result;
}

static QString projectBackupDisplayLabel(const ProjectBackupCandidate& backup)
{
    return QStringLiteral("%1 — %2 — %3 MiB — %4\n%5")
        .arg(QFileInfo(backup.sourcePath).completeBaseName(),
             QLocale().toString(backup.modifiedUtc.toLocalTime(),
                                QStringLiteral("yyyy-MM-dd HH:mm:ss")),
             QLocale().toString(static_cast<double>(backup.sizeBytes) /
                                (1024.0 * 1024.0), 'f', 2),
             QFileInfo(backup.backupPath).fileName())
        .arg(backup.sourcePath);
}

static bool deleteProjectBackupGeneration(const ProjectBackupCandidate& backup,
                                          QString* errorText)
{
    const QFileInfo backupInfo(backup.backupPath);
    const QFileInfo directoryInfo(backupInfo.absolutePath());
    const QFileInfo rootInfo(QStringLiteral("M:/VeraCore-Recovery/ProjectBackups"));
    if (!backupInfo.exists() || !backupInfo.isFile() || backupInfo.isSymLink() ||
        !directoryInfo.isDir() || directoryInfo.isSymLink() ||
        rootInfo.canonicalFilePath().isEmpty() ||
        directoryInfo.canonicalFilePath().isEmpty()) {
        if (errorText) *errorText = QStringLiteral("The selected backup path is no longer valid.");
        return false;
    }
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
    if (QFileInfo(directoryInfo.canonicalFilePath()).dir().canonicalPath().compare(
            rootInfo.canonicalFilePath(), pathCase) != 0 ||
        directoryInfo.fileName().compare(projectBackupKey(backup.sourcePath),
                                         Qt::CaseInsensitive) != 0) {
        if (errorText) *errorText = QStringLiteral("Backup path identity validation failed.");
        return false;
    }

    bool allowedName = false;
    for (int slot = 0; slot < 5; ++slot) {
        if (backupInfo.fileName() == QStringLiteral("Backup.%1.vcp").arg(slot)) {
            allowedName = true;
            break;
        }
    }
    if (!allowedName || !recoveryFileMatchesChecksum(backupInfo.absoluteFilePath())) {
        if (errorText) *errorText = QStringLiteral(
            "Backup filename or SHA-256 validation failed.");
        return false;
    }

    int validGenerations = 0;
    const QDir directory(directoryInfo.absoluteFilePath());
    for (int slot = 0; slot < 5; ++slot) {
        if (recoveryFileMatchesChecksum(directory.filePath(
                QStringLiteral("Backup.%1.vcp").arg(slot)))) ++validGenerations;
    }
    if (validGenerations < 2) {
        if (errorText) *errorText = QStringLiteral(
            "The last valid backup generation is protected from deletion.");
        return false;
    }
    if (!QFile::remove(backupInfo.absoluteFilePath())) {
        if (errorText) *errorText = QStringLiteral("Could not delete the selected backup generation.");
        return false;
    }
    QFile::remove(backupInfo.absoluteFilePath() + QStringLiteral(".sha256"));
    return true;
}

struct ProjectBackupAudit
{
    int directoriesVisited = 0;
    int invalidMetadataDirectories = 0;
    int validGenerations = 0;
    int invalidGenerations = 0;
    int orphanChecksums = 0;
    qint64 validBytes = 0;
    qint64 invalidBytes = 0;
    QDateTime newestValidUtc;
    bool scanCapped = false;
};

static ProjectBackupAudit auditProjectBackups()
{
    ProjectBackupAudit audit;
    const QDir root(QStringLiteral("M:/VeraCore-Recovery/ProjectBackups"));
    const QFileInfoList directories = root.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
        QDir::Name | QDir::IgnoreCase);
    constexpr int kMaxAuditDirectories = 1024;
    audit.scanCapped = directories.size() > kMaxAuditDirectories;
    for (const QFileInfo& directoryInfo : directories) {
        if (audit.directoriesVisited >= kMaxAuditDirectories) break;
        ++audit.directoriesVisited;
        const QDir directory(directoryInfo.absoluteFilePath());
        QFile sourcePathFile(directory.filePath(QStringLiteral("SourcePath.txt")));
        bool metadataValid = sourcePathFile.open(QIODevice::ReadOnly) &&
            sourcePathFile.size() > 0 && sourcePathFile.size() <= 32768;
        QString sourcePath;
        if (metadataValid) {
            sourcePath = QString::fromUtf8(sourcePathFile.readAll()).trimmed();
            const QFileInfo sourceInfo(sourcePath);
            metadataValid = !sourcePath.isEmpty() && sourceInfo.isAbsolute() &&
                sourceInfo.suffix().compare(QStringLiteral("vcp"), Qt::CaseInsensitive) == 0 &&
                directory.dirName().compare(projectBackupKey(sourceInfo.absoluteFilePath()),
                                            Qt::CaseInsensitive) == 0;
        }
        if (!metadataValid) ++audit.invalidMetadataDirectories;

        for (int slot = 0; slot < 5; ++slot) {
            const QString backupPath = directory.filePath(
                QStringLiteral("Backup.%1.vcp").arg(slot));
            const QFileInfo backupInfo(backupPath);
            const QFileInfo checksumInfo(backupPath + QStringLiteral(".sha256"));
            if (!backupInfo.exists()) {
                if (checksumInfo.exists() && checksumInfo.isFile()) ++audit.orphanChecksums;
                continue;
            }
            const bool valid = metadataValid && backupInfo.isFile() &&
                !backupInfo.isSymLink() && recoveryFileMatchesChecksum(backupPath);
            if (valid) {
                ++audit.validGenerations;
                audit.validBytes += std::max<qint64>(0, backupInfo.size());
                const QDateTime modified = backupInfo.lastModified().toUTC();
                if (!audit.newestValidUtc.isValid() || modified > audit.newestValidUtc)
                    audit.newestValidUtc = modified;
            } else {
                ++audit.invalidGenerations;
                if (backupInfo.isFile())
                    audit.invalidBytes += std::max<qint64>(0, backupInfo.size());
            }
        }
    }
    return audit;
}
constexpr size_t kMaxSyncCacheEntries = 8;
constexpr size_t kMaxDecodedCacheSamples = 64u * 1024u * 1024u;
constexpr size_t kMaxSyncCacheSamples = 64u * 1024u * 1024u;
constexpr qint64 kMaxProjectPluginStateBytes = 32 * 1024 * 1024;
constexpr qint64 kMaxProjectPluginAutomationPoints = 4ll * 1024ll * 1024ll;

template <typename ClipInfo>
static bool clipTakeIsActive(const ClipInfo& clip)
{
    return clip.takeGroupId.isEmpty() || clip.takeActive;
}

struct MidiNoteTickRange
{
    quint32 startTick = 0;
    quint32 durationTicks = 1;
};

static bool quantizeMidiNoteRange(const MidiNote& note, int ppq, MidiNoteTickRange& range)
{
    if (!isFiniteBeatRange(note.startBeat, note.lengthBeats) || !isPluginMidiPitch(note.pitch)) return false;
    const int safePpq = std::clamp(ppq, kMinProjectPpq, kMaxProjectPpq);
    constexpr quint32 maxTick = std::numeric_limits<quint32>::max();
    const double start = std::round(note.startBeat * safePpq);
    const double duration = std::round(note.lengthBeats * safePpq);
    if (!std::isfinite(start) || !std::isfinite(duration)) return false;
    range.startTick = static_cast<quint32>(std::clamp(start, 0.0, static_cast<double>(maxTick - 1u)));
    range.durationTicks = static_cast<quint32>(std::clamp(
        duration, 1.0, static_cast<double>(maxTick - range.startTick)));
    return true;
}

struct MidiRoundTripNote
{
    quint32 startTick = 0;
    quint32 durationTicks = 1;
    int pitch = 60;
    int colorGroup = 0;
    int velocity = 100;
    int pan = 64;
    int release = 64;
    int modX = 64;
    int modY = 64;
    int groupId = 0;
    int flags = 0;

    auto values() const
    {
        return std::tie(startTick, durationTicks, pitch, colorGroup, velocity,
                        pan, release, modX, modY, groupId, flags);
    }
    bool operator<(const MidiRoundTripNote& other) const { return values() < other.values(); }
    bool operator==(const MidiRoundTripNote& other) const { return values() == other.values(); }
};

static std::vector<MidiRoundTripNote> normalizedMidiRoundTripNotes(const MidiClipDocument& document, int ppq)
{
    std::vector<MidiRoundTripNote> result;
    result.reserve(document.notes.size());
    for (const MidiNote& note : document.notes) {
        MidiNoteTickRange range;
        if (!quantizeMidiNoteRange(note, ppq, range)) continue;
        int flags = 0;
        if (note.isKeyswitch) flags |= 1;
        if (note.isSlide) flags |= 2;
        if (note.isPortamento) flags |= 4;
        if (note.muted) flags |= 8;
        result.push_back({range.startTick,
                          range.durationTicks,
                          note.pitch,
                          clampIntValue(note.colorGroup, 0, 15),
                          clampIntValue(note.velocity, 1, 127),
                          clampIntValue(note.pan, 0, 127),
                          clampIntValue(note.release, 0, 127),
                          clampIntValue(note.modX, 0, 127),
                          clampIntValue(note.modY, 0, 127),
                          note.groupId > 0 ? note.groupId : 0,
                          flags});
    }
    std::sort(result.begin(), result.end());
    return result;
}

struct MidiRoundTripEvent
{
    quint32 tick = 0;
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;

    auto values() const { return std::tie(tick, status, data1, data2); }
    bool operator<(const MidiRoundTripEvent& other) const { return values() < other.values(); }
    bool operator==(const MidiRoundTripEvent& other) const { return values() == other.values(); }
};

static std::vector<MidiRoundTripEvent> normalizedMidiRoundTripEvents(
    const MidiClipDocument& document, int ppq, bool includeNoteProperties = true)
{
    const int safePpq = std::clamp(ppq, kMinProjectPpq, kMaxProjectPpq);
    constexpr quint32 maxTick = std::numeric_limits<quint32>::max();
    std::vector<MidiRoundTripEvent> result;
    result.reserve(document.performanceEvents.size() + document.notes.size() * 4u);
    auto pushEvent = [&](double beat, uint8_t status, uint8_t data1, uint8_t data2) {
        if (!std::isfinite(beat) || beat < 0.0 ||
            !isValidMidiChannelEvent(status, data1, data2)) return;
        const uint8_t statusHi = status & 0xF0;
        if (statusHi == 0x80 || statusHi == 0x90) return;
        const double tickValue = std::round(beat * safePpq);
        if (!std::isfinite(tickValue)) return;
        const quint32 tick = static_cast<quint32>(std::clamp(
            tickValue, 0.0, static_cast<double>(maxTick)));
        const uint8_t canonicalData2 = (statusHi == 0xC0 || statusHi == 0xD0) ? 0 : data2;
        result.push_back({tick, status, data1, canonicalData2});
    };
    if (includeNoteProperties) {
        for (const MidiNote& note : document.notes) {
            if (note.muted || note.isSlide) continue;
            MidiNoteTickRange range;
            if (!quantizeMidiNoteRange(note, safePpq, range)) continue;
            const double startBeat = static_cast<double>(range.startTick) / safePpq;
            const uint8_t channel = static_cast<uint8_t>(clampIntValue(note.colorGroup, 0, 15));
            if (note.pan != 64)
                pushEvent(startBeat, static_cast<uint8_t>(0xB0 | channel), kMidiCcPan,
                          static_cast<uint8_t>(clampIntValue(note.pan, 0, 127)));
            if (note.modX != 64)
                pushEvent(startBeat, static_cast<uint8_t>(0xB0 | channel), kMidiCcModX,
                          static_cast<uint8_t>(clampIntValue(note.modX, 0, 127)));
            if (note.modY != 64)
                pushEvent(startBeat, static_cast<uint8_t>(0xB0 | channel), kMidiCcModY,
                          static_cast<uint8_t>(clampIntValue(note.modY, 0, 127)));
        }
    }
    for (const MidiEvent& event : document.performanceEvents)
        pushEvent(event.beat, event.status, event.data1, event.data2);

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

static bool midiNotesPersistentlyEqual(const MidiNote& left, const MidiNote& right)
{
    return std::tie(left.pitch, left.startBeat, left.lengthBeats, left.velocity,
                    left.pan, left.release, left.modX, left.modY, left.colorGroup,
                    left.groupId, left.isKeyswitch, left.isSlide,
                    left.isPortamento, left.muted) ==
           std::tie(right.pitch, right.startBeat, right.lengthBeats, right.velocity,
                    right.pan, right.release, right.modX, right.modY, right.colorGroup,
                    right.groupId, right.isKeyswitch, right.isSlide,
                    right.isPortamento, right.muted);
}

static bool midiEventsPersistentlyEqual(const MidiEvent& left, const MidiEvent& right)
{
    return std::tie(left.beat, left.status, left.data1, left.data2, left.isSlide,
                    left.isPortamento, left.glideDurationBeats, left.isKeyswitch) ==
           std::tie(right.beat, right.status, right.data1, right.data2, right.isSlide,
                    right.isPortamento, right.glideDurationBeats, right.isKeyswitch);
}

static bool midiDocumentsPersistentlyEqual(const MidiClipDocument& left,
                                           const MidiClipDocument& right)
{
    if (std::tie(left.name, left.trackName, left.trackIndex, left.startBeat,
                 left.lengthBeats, left.tempoBpm, left.timeSignatureNumerator,
                 left.timeSignatureDenominator) !=
        std::tie(right.name, right.trackName, right.trackIndex, right.startBeat,
                 right.lengthBeats, right.tempoBpm, right.timeSignatureNumerator,
                 right.timeSignatureDenominator) ||
        left.tempoMap.size() != right.tempoMap.size() ||
        left.timeSignatureMap.size() != right.timeSignatureMap.size() ||
        left.notes.size() != right.notes.size() ||
        left.performanceEvents.size() != right.performanceEvents.size()) return false;

    for (size_t index = 0; index < left.tempoMap.size(); ++index) {
        if (std::tie(left.tempoMap[index].beat, left.tempoMap[index].bpm) !=
            std::tie(right.tempoMap[index].beat, right.tempoMap[index].bpm)) return false;
    }
    for (size_t index = 0; index < left.timeSignatureMap.size(); ++index) {
        if (std::tie(left.timeSignatureMap[index].beat,
                     left.timeSignatureMap[index].numerator,
                     left.timeSignatureMap[index].denominator) !=
            std::tie(right.timeSignatureMap[index].beat,
                     right.timeSignatureMap[index].numerator,
                     right.timeSignatureMap[index].denominator)) return false;
    }
    for (size_t index = 0; index < left.notes.size(); ++index)
        if (!midiNotesPersistentlyEqual(left.notes[index], right.notes[index])) return false;
    for (size_t index = 0; index < left.performanceEvents.size(); ++index)
        if (!midiEventsPersistentlyEqual(
                left.performanceEvents[index], right.performanceEvents[index])) return false;
    return true;
}

static bool decodeProjectPluginState(const QString& encoded, QByteArray& decoded)
{
    decoded.clear();
    if (encoded.isEmpty()) return true;
    const QByteArray ascii = encoded.toLatin1();
    if (ascii.size() != encoded.size() || ascii.size() % 4 != 0 ||
        ascii.size() > ((kMaxPluginStateBytes + 2) / 3) * 4) return false;
    for (int i = 0; i < ascii.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(ascii[i]);
        const bool alphabet = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '+' || c == '/';
        if (!alphabet && !(c == '=' && i >= ascii.size() - 2)) return false;
    }
    decoded = QByteArray::fromBase64(ascii);
    return decoded.size() <= kMaxPluginStateBytes && decoded.toBase64() == ascii;
}

static QJsonArray serializePluginAutomation(
    const std::vector<PluginAutomationLane>& automation)
{
    QJsonArray lanes;
    for (const PluginAutomationLane& lane : automation) {
        QJsonObject laneData;
        laneData["parameterId"] = static_cast<qint64>(lane.parameterId);
        laneData["name"] = lane.parameterName;
        laneData["enabled"] = lane.enabled;
        laneData["interpolation"] = lane.interpolation == PluginAutomationInterpolation::Hold
            ? QStringLiteral("hold")
            : (lane.interpolation == PluginAutomationInterpolation::Smooth
                ? QStringLiteral("smooth") : QStringLiteral("linear"));
        QJsonArray points;
        for (const PluginAutomationPoint& point : lane.points) {
            QJsonObject pointData;
            pointData["beat"] = point.beat;
            pointData["value"] = point.normalizedValue;
            pointData["curve"] = point.curveTension;
            if (point.curvePolynomial) {
                QJsonArray curvePolynomial;
                for (double coefficient : *point.curvePolynomial)
                    curvePolynomial.append(coefficient);
                pointData["curvePolynomial"] = curvePolynomial;
            }
            points.append(pointData);
        }
        laneData["points"] = points;
        lanes.append(laneData);
    }
    return lanes;
}

static qint64 pluginAutomationPointCount(
    const std::vector<PluginAutomationLane>& automation)
{
    qint64 count = 0;
    for (const PluginAutomationLane& lane : automation)
        count += static_cast<qint64>(lane.points.size());
    return count;
}

static bool deserializePluginAutomation(
    const QJsonValue& value, std::vector<PluginAutomationLane>& automation)
{
    automation.clear();
    if (value.isUndefined() || value.isNull()) return true;
    if (!value.isArray()) return false;
    const QJsonArray lanes = value.toArray();
    if (lanes.size() > static_cast<qsizetype>(kMaxPluginAutomationLanes)) return false;
    std::set<uint32_t> parameterIds;
    for (const QJsonValue& laneValue : lanes) {
        if (!laneValue.isObject()) return false;
        const QJsonObject laneData = laneValue.toObject();
        const double idValue = laneData.value("parameterId").toDouble(-1.0);
        if (!std::isfinite(idValue) || idValue < 0.0 ||
            idValue >= static_cast<double>(UINT32_MAX) || std::floor(idValue) != idValue) return false;
        PluginAutomationLane lane;
        lane.parameterId = static_cast<uint32_t>(idValue);
        if (!parameterIds.insert(lane.parameterId).second) return false;
        lane.parameterName = laneData.value("name").toString().left(256);
        lane.enabled = laneData.value("enabled").toBool(true);
        const QString interpolation = laneData.value("interpolation").toString("linear");
        if (interpolation == "hold") lane.interpolation = PluginAutomationInterpolation::Hold;
        else if (interpolation == "smooth") lane.interpolation = PluginAutomationInterpolation::Smooth;
        else if (interpolation != "linear") return false;
        const QJsonValue pointsValue = laneData.value("points");
        if (!pointsValue.isArray()) return false;
        const QJsonArray points = pointsValue.toArray();
        if (points.size() > static_cast<qsizetype>(kMaxPluginAutomationPointsPerLane)) return false;
        lane.points.reserve(static_cast<size_t>(points.size()));
        for (const QJsonValue& pointValue : points) {
            if (!pointValue.isObject()) return false;
            const QJsonObject pointData = pointValue.toObject();
            const double beat = pointData.value("beat").toDouble(-1.0);
            const double normalized = pointData.value("value").toDouble(-1.0);
            const double curve = pointData.value("curve").toDouble(0.0);
            if (!std::isfinite(beat) || beat < 0.0 || !std::isfinite(normalized) ||
                normalized < 0.0 || normalized > 1.0 || !std::isfinite(curve) ||
                curve < -1.0 || curve > 1.0) return false;
            PluginAutomationPoint point{beat, normalized, curve};
            const QJsonValue polynomialValue = pointData.value("curvePolynomial");
            if (!polynomialValue.isUndefined() && !polynomialValue.isNull()) {
                if (!polynomialValue.isArray()) return false;
                const QJsonArray coefficients = polynomialValue.toArray();
                PluginAutomationCurvePolynomial polynomial{};
                if (coefficients.size() != static_cast<qsizetype>(polynomial.size()))
                    return false;
                for (qsizetype index = 0; index < coefficients.size(); ++index) {
                    if (!coefficients[index].isDouble()) return false;
                    polynomial[static_cast<size_t>(index)] =
                        coefficients[index].toDouble();
                }
                if (!pluginAutomationCurvePolynomialValid(polynomial)) return false;
                point.curvePolynomial =
                    std::make_shared<const PluginAutomationCurvePolynomial>(polynomial);
            }
            lane.points.push_back(point);
        }
        automation.push_back(std::move(lane));
    }
    return true;
}

static bool decodeProjectAraArchive(const QString& encoded, QByteArray& decoded)
{
    decoded.clear();
    const QByteArray ascii = encoded.toLatin1();
    if (ascii.size() != encoded.size() || ascii.size() % 4 != 0 ||
        ascii.size() > ((kMaxAraArchiveBytes + 2) / 3) * 4) return false;
    for (qsizetype index = 0; index < ascii.size(); ++index) {
        const unsigned char value = static_cast<unsigned char>(ascii[index]);
        const bool alphabet = (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
            value == '+' || value == '/';
        if (!alphabet && !(value == '=' && index >= ascii.size() - 2)) return false;
    }
    decoded = QByteArray::fromBase64(ascii);
    return decoded.size() <= kMaxAraArchiveBytes && decoded.toBase64() == ascii;
}

static bool readUInt16BE(const QByteArray& bytes, int index, quint16& outValue)
{
    if (index < 0 || index + 1 >= bytes.size()) {
        return false;
    }
    const uint8_t hi = static_cast<uint8_t>(bytes[index]);
    const uint8_t lo = static_cast<uint8_t>(bytes[index + 1]);
    outValue = static_cast<quint16>((hi << 8) | lo);
    return true;
}

static bool readUInt32BE(const QByteArray& bytes, int index, quint32& outValue)
{
    if (index < 0 || index + 3 >= bytes.size()) {
        return false;
    }
    const uint8_t b1 = static_cast<uint8_t>(bytes[index]);
    const uint8_t b2 = static_cast<uint8_t>(bytes[index + 1]);
    const uint8_t b3 = static_cast<uint8_t>(bytes[index + 2]);
    const uint8_t b4 = static_cast<uint8_t>(bytes[index + 3]);
    outValue = (static_cast<quint32>(b1) << 24) |
        (static_cast<quint32>(b2) << 16) |
        (static_cast<quint32>(b3) << 8) |
        static_cast<quint32>(b4);
    return true;
}

static void writeUInt16BE(QIODevice& file, quint16 value)
{
    const char bytes[2] = {
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>(value & 0xff)
    };
    file.write(bytes, 2);
}

static void writeUInt32BE(QIODevice& file, quint32 value)
{
    const char bytes[4] = {
        static_cast<char>((value >> 24) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>(value & 0xff)
    };
    file.write(bytes, 4);
}

static QByteArray encodeVariableLength(quint32 value)
{
    constexpr quint32 kMaxMidiVariableLength = 0x0fffffffu;
    value = qMin(value, kMaxMidiVariableLength);
    QByteArray out;
    unsigned char byte = static_cast<unsigned char>(value & 0x7f);
    while ((value >>= 7) > 0) {
        byte |= 0x80;
        out.prepend(static_cast<char>(byte));
        byte = static_cast<unsigned char>(value & 0x7f);
    }
    out.prepend(static_cast<char>(byte));
    return out;
}

static bool readVariableLength(const QByteArray& bytes, int& index, int size, quint32& outValue)
{
    outValue = 0;
    int bytesRead = 0;
    while (index < size) {
        ++bytesRead;
        if (bytesRead > 4) {
            return false;
        }
        const unsigned char c = static_cast<unsigned char>(bytes[index++]);
        outValue = (outValue << 7) | (c & 0x7f);
        if ((c & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

static int expectedChannelDataCount(uint8_t status)
{
    const uint8_t statusHi = status & 0xF0;
    if (statusHi == 0xC0 || statusHi == 0xD0) {
        return 1;
    }
    return 2;
}

static int expectedSystemDataCount(uint8_t status)
{
    switch (status) {
    case 0xF1:
    case 0xF3:
        return 1;
    case 0xF2:
        return 2;
    case 0xF6:
    case 0xF8:
    case 0xF9:
    case 0xFA:
    case 0xFB:
    case 0xFC:
    case 0xFD:
    case 0xFE:
        return 0;
    default:
        return -1;
    }
}

static bool isMidiRealtimeStatus(uint8_t status)
{
    return status >= 0xF8 && status <= 0xFE;
}

static double noteToFrequency(int pitch)
{
    return 440.0 * std::pow(2.0, (static_cast<double>(pitch) - 69.0) / 12.0);
}

static double noteToFrequency(double pitch)
{
    return 440.0 * std::pow(2.0, (pitch - 69.0) / 12.0);
}

static double pitchBendToSemitones(uint8_t data1, uint8_t data2)
{
    const int value = (static_cast<int>(data2 & 0x7f) << 7) | static_cast<int>(data1 & 0x7f);
    constexpr double bendRangeSemitones = 2.0;
    return ((static_cast<double>(value) - 8192.0) / 8192.0) * bendRangeSemitones;
}

static double pressureToGain(uint8_t value)
{
    return 0.55 + (static_cast<double>(value & 0x7f) / 127.0) * 0.9;
}

static double finiteClamp(double value, double low, double high, double fallback)
{
    return std::isfinite(value) ? qBound(low, value, high) : fallback;
}

static double shapedAudioFade(double progress, double curve)
{
    constexpr double halfPi = 1.57079632679489661923;
    const double base = std::sin(halfPi * qBound(0.0, progress, 1.0));
    const double safeCurve = finiteClamp(curve, -1.0, 1.0, 0.0);
    if (safeCurve < 0.0)
        return base + (std::sqrt(base) - base) * -safeCurve;
    return base + (base * base - base) * safeCurve;
}

static std::vector<MidiTempoPoint> normalizedTempoMap(
    const std::vector<MidiTempoPoint>& points, double fallbackBpm)
{
    const double safeFallback = finiteClamp(fallbackBpm, 20.0, 999.0, kDefaultTempoBpm);
    std::vector<MidiTempoPoint> normalized;
    normalized.reserve(std::min(points.size(), kMaxTempoMapPoints));
    for (const MidiTempoPoint& point : points) {
        if (!std::isfinite(point.beat) || point.beat < 0.0 ||
            !std::isfinite(point.bpm) || point.bpm < 20.0 || point.bpm > 999.0) continue;
        if (normalized.size() >= kMaxTempoMapPoints) break;
        normalized.push_back(point);
    }
    std::stable_sort(normalized.begin(), normalized.end(),
        [](const MidiTempoPoint& left, const MidiTempoPoint& right) {
            return left.beat < right.beat;
        });
    std::vector<MidiTempoPoint> collapsed;
    collapsed.reserve(normalized.size());
    for (const MidiTempoPoint& point : normalized) {
        if (!collapsed.empty() && collapsed.back().beat == point.beat) collapsed.back() = point;
        else collapsed.push_back(point);
    }
    if (collapsed.empty()) collapsed.push_back({0.0, safeFallback});
    else if (collapsed.front().beat > 0.0) {
        if (collapsed.size() == kMaxTempoMapPoints) collapsed.pop_back();
        collapsed.insert(collapsed.begin(), {0.0, safeFallback});
    }
    return collapsed;
}

static double tempoAtBeat(const std::vector<MidiTempoPoint>& points, double beat, double fallbackBpm)
{
    if (points.empty() || !std::isfinite(beat))
        return finiteClamp(fallbackBpm, 20.0, 999.0, kDefaultTempoBpm);
    const auto next = std::upper_bound(points.begin(), points.end(), qMax(0.0, beat),
        [](double targetBeat, const MidiTempoPoint& point) { return targetBeat < point.beat; });
    return next == points.begin() ? points.front().bpm : (next - 1)->bpm;
}

static bool validTimeSignatureDenominator(int denominator)
{
    return denominator >= 1 && denominator <= 64 &&
        (denominator & (denominator - 1)) == 0;
}

static std::vector<MidiTimeSignaturePoint> normalizedTimeSignatureMap(
    const std::vector<MidiTimeSignaturePoint>& points, int fallbackNumerator,
    int fallbackDenominator)
{
    const int safeNumerator = qBound(1, fallbackNumerator, 32);
    const int safeDenominator = validTimeSignatureDenominator(fallbackDenominator)
        ? fallbackDenominator : 4;
    std::vector<MidiTimeSignaturePoint> normalized;
    normalized.reserve(std::min(points.size(), kMaxTimeSignatureMapPoints));
    for (const MidiTimeSignaturePoint& point : points) {
        if (!std::isfinite(point.beat) || point.beat < 0.0 ||
            point.numerator < 1 || point.numerator > 32 ||
            !validTimeSignatureDenominator(point.denominator)) continue;
        if (normalized.size() >= kMaxTimeSignatureMapPoints) break;
        normalized.push_back(point);
    }
    std::stable_sort(normalized.begin(), normalized.end(),
        [](const MidiTimeSignaturePoint& left, const MidiTimeSignaturePoint& right) {
            return left.beat < right.beat;
        });
    std::vector<MidiTimeSignaturePoint> collapsed;
    collapsed.reserve(normalized.size());
    for (const MidiTimeSignaturePoint& point : normalized) {
        if (!collapsed.empty() && collapsed.back().beat == point.beat) collapsed.back() = point;
        else collapsed.push_back(point);
    }
    if (collapsed.empty()) collapsed.push_back({0.0, safeNumerator, safeDenominator});
    else if (collapsed.front().beat > 0.0) {
        if (collapsed.size() == kMaxTimeSignatureMapPoints) collapsed.pop_back();
        collapsed.insert(collapsed.begin(), {0.0, safeNumerator, safeDenominator});
    }
    return collapsed;
}

static MidiTimeSignaturePoint timeSignatureAtBeat(
    const std::vector<MidiTimeSignaturePoint>& points, double beat,
    int fallbackNumerator, int fallbackDenominator)
{
    if (points.empty()) return {0.0, qBound(1, fallbackNumerator, 32),
        validTimeSignatureDenominator(fallbackDenominator) ? fallbackDenominator : 4};
    const auto next = std::upper_bound(points.begin(), points.end(), qMax(0.0, beat),
        [](double targetBeat, const MidiTimeSignaturePoint& point) { return targetBeat < point.beat; });
    return next == points.begin() ? points.front() : *(next - 1);
}

struct PluginMetricalPosition
{
    double barStartBeat = 0.0;
    int barNumber = 0;
    int numerator = 4;
    int denominator = 4;
};

// Signature markers begin a new bar. A partial bar before a marker therefore
// counts as one bar, matching professional DAW marker semantics.
static PluginMetricalPosition pluginMetricalPositionAtBeat(
    const std::vector<MidiTimeSignaturePoint>& points, double beat,
    int fallbackNumerator, int fallbackDenominator)
{
    const double safeBeat = std::isfinite(beat) ? qMax(0.0, beat) : 0.0;
    int numerator = qBound(1, fallbackNumerator, 32);
    int denominator = validTimeSignatureDenominator(fallbackDenominator)
        ? fallbackDenominator : 4;
    double segmentStart = 0.0;
    int64_t completedBars = 0;
    const auto beatsPerBar = [](int num, int denom) {
        return static_cast<double>(num) * 4.0 / static_cast<double>(denom);
    };
    const auto boundedBars = [](double ratio, bool roundUp) {
        if (!std::isfinite(ratio) || ratio <= 0.0) return int64_t{0};
        const double nearest = std::round(ratio);
        const double tolerance = 16.0 * std::numeric_limits<double>::epsilon() *
            qMax(1.0, std::fabs(ratio));
        const double stable = std::fabs(ratio - nearest) <= tolerance
            ? nearest : ratio;
        const double whole = roundUp ? std::ceil(stable) : std::floor(stable);
        return static_cast<int64_t>(qMin<double>(INT32_MAX, qMax(0.0, whole)));
    };
    for (const MidiTimeSignaturePoint& point : points) {
        if (!std::isfinite(point.beat) || point.beat < segmentStart ||
            point.beat > safeBeat || point.numerator < 1 || point.numerator > 32 ||
            !validTimeSignatureDenominator(point.denominator)) continue;
        const double segmentLength = point.beat - segmentStart;
        if (segmentLength > 0.0) {
            const int64_t bars = boundedBars(
                segmentLength / beatsPerBar(numerator, denominator), true);
            completedBars = std::min<int64_t>(INT32_MAX,
                completedBars + bars);
        }
        segmentStart = point.beat;
        numerator = point.numerator;
        denominator = point.denominator;
    }
    const double currentBeatsPerBar = beatsPerBar(numerator, denominator);
    const double localRatio = qMax(0.0, safeBeat - segmentStart) / currentBeatsPerBar;
    const double nearestLocal = std::round(localRatio);
    const double localTolerance = qMin(0.25,
        16.0 * std::numeric_limits<double>::epsilon() *
            qMax(1.0, std::fabs(localRatio)));
    const double stableLocal = std::fabs(localRatio - nearestLocal) <= localTolerance
        ? nearestLocal : localRatio;
    // Keep bar position in double precision even after the public cumulative
    // bar counter saturates.  Clamping this value to INT32_MAX displaced the
    // reported bar start on very long timelines.
    double barsIntoSegment = std::floor(qMax(0.0, stableLocal));
    double resolvedBarStart = segmentStart + barsIntoSegment * currentBeatsPerBar;
    if (resolvedBarStart > safeBeat && barsIntoSegment > 0.0) {
        barsIntoSegment -= 1.0;
        resolvedBarStart = segmentStart + barsIntoSegment * currentBeatsPerBar;
    }
    const int64_t barNumber = std::min<int64_t>(
        INT32_MAX, completedBars + static_cast<int64_t>(qMin<double>(
            INT32_MAX, barsIntoSegment)));
    return {resolvedBarStart,
            static_cast<int>(barNumber), numerator, denominator};
}

static double countInStartBeat(const std::vector<MidiTimeSignaturePoint>& points,
                               double recordBeat, int bars,
                               int fallbackNumerator, int fallbackDenominator)
{
    double cursor = std::isfinite(recordBeat) ? qMax(0.0, recordBeat) : 0.0;
    const int safeBars = bars == 1 || bars == 2 || bars == 4 ? bars : 0;
    for (int bar = 0; bar < safeBars && cursor > 1.0e-9; ++bar) {
        const double probe = qMax(0.0, cursor - 1.0e-9);
        const MidiTimeSignaturePoint signature = timeSignatureAtBeat(
            points, probe, fallbackNumerator, fallbackDenominator);
        const double barLength = static_cast<double>(signature.numerator) * 4.0 /
            static_cast<double>(qMax(1, signature.denominator));
        cursor = qMax(0.0, cursor - qMax(0.25, barLength));
    }
    return cursor;
}

struct TempoClockPoint
{
    double beat = 0.0;
    double bpm = 120.0;
    double seconds = 0.0;
};

struct TempoClock
{
    std::vector<TempoClockPoint> points;

    TempoClock(const std::vector<MidiTempoPoint>& source, double fallbackBpm)
    {
        const std::vector<MidiTempoPoint> normalized = normalizedTempoMap(source, fallbackBpm);
        points.reserve(normalized.size());
        double seconds = 0.0;
        double previousBeat = 0.0;
        double previousBpm = normalized.front().bpm;
        for (const MidiTempoPoint& point : normalized) {
            if (!points.empty()) seconds += (point.beat - previousBeat) * 60.0 / previousBpm;
            points.push_back({point.beat, point.bpm, seconds});
            previousBeat = point.beat;
            previousBpm = point.bpm;
        }
    }

    double secondsAtBeat(double beat) const
    {
        if (points.empty() || !std::isfinite(beat) || beat <= 0.0) return 0.0;
        const auto next = std::upper_bound(points.begin(), points.end(), beat,
            [](double targetBeat, const TempoClockPoint& point) { return targetBeat < point.beat; });
        const TempoClockPoint& point = next == points.begin() ? points.front() : *(next - 1);
        return point.seconds + (beat - point.beat) * 60.0 / point.bpm;
    }

    double beatAtSeconds(double seconds) const
    {
        if (points.empty() || !std::isfinite(seconds) || seconds <= 0.0) return 0.0;
        const auto next = std::upper_bound(points.begin(), points.end(), seconds,
            [](double targetSeconds, const TempoClockPoint& point) { return targetSeconds < point.seconds; });
        const TempoClockPoint& point = next == points.begin() ? points.front() : *(next - 1);
        return point.beat + (seconds - point.seconds) * point.bpm / 60.0;
    }

    double bpmAtBeat(double beat) const
    {
        if (points.empty()) return kDefaultTempoBpm;
        const auto next = std::upper_bound(points.begin(), points.end(), qMax(0.0, beat),
            [](double targetBeat, const TempoClockPoint& point) { return targetBeat < point.beat; });
        return next == points.begin() ? points.front().bpm : (next - 1)->bpm;
    }
};

static double automationLaneValueAt(const PluginAutomationLane& lane, double beat,
                                    double fallback)
{
    if (lane.points.empty() || !std::isfinite(beat)) return fallback;
    const auto next = std::upper_bound(lane.points.begin(), lane.points.end(), beat,
        [](double targetBeat, const PluginAutomationPoint& point) {
            return targetBeat < point.beat;
        });
    if (next == lane.points.begin()) return next->normalizedValue;
    const PluginAutomationPoint& left = *(next - 1);
    if (next == lane.points.end() ||
        lane.interpolation == PluginAutomationInterpolation::Hold)
        return left.normalizedValue;
    const double span = next->beat - left.beat;
    if (span <= 1.0e-12) return next->normalizedValue;
    double amount = std::clamp((beat - left.beat) / span, 0.0, 1.0);
    if (lane.interpolation == PluginAutomationInterpolation::Smooth)
        amount = pluginAutomationCurvePosition(amount, left);
    return left.normalizedValue +
        (next->normalizedValue - left.normalizedValue) * amount;
}

static double averageTempoForBeatRange(const TempoClock& clock, double startBeat,
                                       double lengthBeats, double fallbackTempo)
{
    const double safeStart = qMax(0.0, startBeat);
    const double safeLength = std::isfinite(lengthBeats) ? qMax(0.0, lengthBeats) : 0.0;
    const double duration = clock.secondsAtBeat(safeStart + safeLength) -
        clock.secondsAtBeat(safeStart);
    return safeLength > 0.0 && std::isfinite(duration) && duration > 0.0
        ? qBound(20.0, safeLength * 60.0 / duration, 300.0)
        : finiteClamp(clock.bpmAtBeat(safeStart), 20.0, 300.0, fallbackTempo);
}

static std::shared_ptr<const std::vector<VeraCoreAudioWarpPoint>>
audioSyncWarpPoints(const TempoClock& clock, double startBeat, double lengthBeats)
{
    const double safeStart = qMax(0.0, startBeat);
    const double safeLength = std::isfinite(lengthBeats) ? qMax(0.0, lengthBeats) : 0.0;
    const double endBeat = safeStart + safeLength;
    const double startSeconds = clock.secondsAtBeat(safeStart);
    const double durationSeconds = clock.secondsAtBeat(endBeat) - startSeconds;
    if (safeLength <= 0.0 || !std::isfinite(durationSeconds) || durationSeconds <= 0.0)
        return {};

    auto points = std::make_shared<std::vector<VeraCoreAudioWarpPoint>>();
    points->reserve(clock.points.size() + 2u);
    points->push_back({0.0, 0.0});
    for (const TempoClockPoint& point : clock.points) {
        if (point.beat <= safeStart || point.beat >= endBeat) continue;
        const double progress = (point.seconds - startSeconds) / durationSeconds;
        if (std::isfinite(progress))
            points->push_back({point.beat - safeStart, qBound(0.0, progress, 1.0)});
    }
    points->push_back({safeLength, 1.0});
    return points->size() > 2u
        ? std::shared_ptr<const std::vector<VeraCoreAudioWarpPoint>>(std::move(points))
        : std::shared_ptr<const std::vector<VeraCoreAudioWarpPoint>>{};
}

static double sourceProgressAtWarpBeat(const std::vector<AudioWarpMarker>& markers,
                                       double sourceDurationSeconds,
                                       double warpLengthBeats, double targetBeat)
{
    if (!std::isfinite(sourceDurationSeconds) || sourceDurationSeconds <= 0.0 ||
        !std::isfinite(warpLengthBeats) || warpLengthBeats <= 0.0) return 0.0;
    const double target = qBound(0.0, targetBeat, warpLengthBeats);
    AudioWarpMarker left{0.0, 0.0};
    for (const AudioWarpMarker& right : markers) {
        if (target <= right.targetBeat) {
            const double span = right.targetBeat - left.targetBeat;
            const double amount = span > 0.0 ? (target - left.targetBeat) / span : 0.0;
            const double seconds = left.sourceSeconds +
                (right.sourceSeconds - left.sourceSeconds) * amount;
            return qBound(0.0, seconds / sourceDurationSeconds, 1.0);
        }
        left = right;
    }
    const double span = warpLengthBeats - left.targetBeat;
    const double amount = span > 0.0 ? (target - left.targetBeat) / span : 0.0;
    const double seconds = left.sourceSeconds +
        (sourceDurationSeconds - left.sourceSeconds) * amount;
    return qBound(0.0, seconds / sourceDurationSeconds, 1.0);
}

static std::shared_ptr<const std::vector<VeraCoreAudioWarpPoint>>
audioUserWarpPoints(const std::vector<AudioWarpMarker>& markers,
                    double sourceDurationSeconds, double warpLengthBeats,
                    double sourceOffsetBeats, double lengthBeats)
{
    if (markers.empty() || sourceDurationSeconds <= 0.0 || warpLengthBeats <= 0.0 ||
        lengthBeats <= 0.0) return {};
    const double start = qBound(0.0, sourceOffsetBeats, warpLengthBeats);
    const double end = qBound(start, sourceOffsetBeats + lengthBeats, warpLengthBeats);
    if (end <= start) return {};
    auto points = std::make_shared<std::vector<VeraCoreAudioWarpPoint>>();
    points->reserve(markers.size() + 2u);
    points->push_back({0.0, sourceProgressAtWarpBeat(
        markers, sourceDurationSeconds, warpLengthBeats, start)});
    for (const AudioWarpMarker& marker : markers) {
        if (marker.targetBeat <= start || marker.targetBeat >= end) continue;
        points->push_back({marker.targetBeat - start,
                           qBound(0.0, marker.sourceSeconds / sourceDurationSeconds, 1.0)});
    }
    points->push_back({end - start, sourceProgressAtWarpBeat(
        markers, sourceDurationSeconds, warpLengthBeats, end)});
    return std::shared_ptr<const std::vector<VeraCoreAudioWarpPoint>>(std::move(points));
}

static std::shared_ptr<const std::vector<VeraCoreAudioWarpPoint>>
audioLoopWarpPoints(const std::vector<AudioWarpMarker>& markers,
                    double sourceDurationSeconds, double warpLengthBeats,
                    double loopStartBeats, double loopLengthBeats,
                    double sourceOffsetBeats)
{
    if (markers.empty() || sourceDurationSeconds <= 0.0 || warpLengthBeats <= 0.0 ||
        loopStartBeats < 0.0 || loopLengthBeats < 0.25) return {};
    const double loopStart = qBound(0.0, loopStartBeats,
        qMax(0.0, warpLengthBeats - 0.25));
    const double loopEnd = qBound(loopStart + 0.25,
        loopStart + loopLengthBeats, warpLengthBeats);
    const double cycleLength = loopEnd - loopStart;
    if (cycleLength < 0.25) return {};
    double phase = std::fmod(sourceOffsetBeats - loopStart, cycleLength);
    if (phase < 0.0) phase += cycleLength;
    const double sourceStart = loopStart + phase;
    auto points = std::make_shared<std::vector<VeraCoreAudioWarpPoint>>();
    points->reserve(markers.size() + 4u);
    auto appendSegment = [&](double segmentStart, double segmentEnd, double localStart) {
        points->push_back({localStart, sourceProgressAtWarpBeat(
            markers, sourceDurationSeconds, warpLengthBeats, segmentStart)});
        for (const AudioWarpMarker& marker : markers) {
            if (marker.targetBeat <= segmentStart || marker.targetBeat >= segmentEnd) continue;
            points->push_back({localStart + marker.targetBeat - segmentStart,
                qBound(0.0, marker.sourceSeconds / sourceDurationSeconds, 1.0)});
        }
        points->push_back({localStart + segmentEnd - segmentStart,
            sourceProgressAtWarpBeat(markers, sourceDurationSeconds,
                                     warpLengthBeats, segmentEnd)});
    };
    appendSegment(sourceStart, loopEnd, 0.0);
    const double firstSpan = loopEnd - sourceStart;
    if (firstSpan < cycleLength)
        appendSegment(loopStart, sourceStart, firstSpan);
    return std::shared_ptr<const std::vector<VeraCoreAudioWarpPoint>>(std::move(points));
}

static std::vector<AraTempoEntryDescriptor> buildAraTempoEntries(
    const std::vector<MidiTempoPoint>& source, const TempoClock& clock,
    double fallbackBpm, double requestedHorizonBeat)
{
    const std::vector<MidiTempoPoint> normalized = normalizedTempoMap(source, fallbackBpm);
    std::vector<AraTempoEntryDescriptor> result;
    result.reserve(normalized.size() + 1u);
    for (const MidiTempoPoint& point : normalized) {
        AraTempoEntryDescriptor entry;
        entry.timeSeconds = clock.secondsAtBeat(point.beat);
        entry.quarterPosition = point.beat;
        result.push_back(entry);
    }
    if (result.empty() || result.front().quarterPosition != 0.0) {
        AraTempoEntryDescriptor origin;
        origin.timeSeconds = 0.0;
        origin.quarterPosition = 0.0;
        result.insert(result.begin(), origin);
    }
    const double lastBeat = result.empty() ? 0.0 : result.back().quarterPosition;
    const double tempoHorizonBeat = qMax(qMax(1.0, requestedHorizonBeat), lastBeat + 1.0);
    AraTempoEntryDescriptor horizon;
    horizon.timeSeconds = clock.secondsAtBeat(tempoHorizonBeat);
    horizon.quarterPosition = tempoHorizonBeat;
    result.push_back(horizon);
    return result;
}

static std::vector<AraBarSignatureDescriptor> buildAraBarSignatures(
    const std::vector<MidiTimeSignaturePoint>& source,
    int fallbackNumerator, int fallbackDenominator)
{
    const std::vector<MidiTimeSignaturePoint> normalized = normalizedTimeSignatureMap(
        source, fallbackNumerator, fallbackDenominator);
    std::vector<AraBarSignatureDescriptor> result;
    result.reserve(normalized.size());
    for (const MidiTimeSignaturePoint& point : normalized) {
        if (!result.empty()) {
            const AraBarSignatureDescriptor& previous = result.back();
            const double barLength = static_cast<double>(previous.numerator) * 4.0 /
                static_cast<double>(previous.denominator);
            const double bars = (point.beat - previous.quarterPosition) / barLength;
            if (!std::isfinite(bars) || std::abs(bars - std::round(bars)) > 0.000001)
                continue;
        }
        result.push_back({point.beat, point.numerator, point.denominator});
    }
    return result;
}

struct AraClipRegionInput
{
    QString clipId;
    QString contentId;
    QString name;
    QString trackName;
    int trackIndex = -1;
    double startBeat = 0.0;
    double lengthBeats = 0.0;
    double sourceOffsetBeats = 0.0;
    bool loopEnabled = false;
    double loopStartBeats = 0.0;
    double loopLengthBeats = 0.0;
    double warpLengthBeats = 0.0;
    double sourceTempoBpm = 120.0;
    int syncMode = 0;
    const std::vector<AudioWarpMarker>* warpMarkers = nullptr;
};

static void appendAraClipRegions(const AraClipRegionInput& clip,
                                 double sourceFileDurationSeconds,
                                 const TempoClock& clock,
                                 std::vector<AraPlaybackRegionDescriptor>& regions)
{
    if (clip.clipId.isEmpty() || clip.contentId.isEmpty() || clip.trackIndex < 0 ||
        !isFiniteBeatRange(clip.startBeat, clip.lengthBeats) ||
        !std::isfinite(sourceFileDurationSeconds) || sourceFileDurationSeconds <= 0.0) return;
    const double sourceTempo = qBound(20.0, clip.sourceTempoBpm, 300.0);
    const std::vector<AudioWarpMarker> emptyMarkers;
    const std::vector<AudioWarpMarker>& markers = clip.warpMarkers
        ? *clip.warpMarkers : emptyMarkers;
    auto sourceSecondsAtBeat = [&](double sourceBeat) {
        if (std::isfinite(clip.warpLengthBeats) && clip.warpLengthBeats > 0.0) {
            return sourceProgressAtWarpBeat(markers, sourceFileDurationSeconds,
                clip.warpLengthBeats, sourceBeat) * sourceFileDurationSeconds;
        }
        return sourceBeat * 60.0 / sourceTempo;
    };
    int regionIndex = 0;
    auto appendRegion = [&](double localStartBeat, double localLengthBeats,
                            double sourceStartBeat, double sourceLengthBeats) {
        if (localLengthBeats <= 0.0 || sourceLengthBeats <= 0.0) return;
        double sourceStartSeconds = qBound(0.0, sourceSecondsAtBeat(sourceStartBeat),
                                           sourceFileDurationSeconds);
        const double sourceEndSeconds = qBound(sourceStartSeconds,
            sourceSecondsAtBeat(sourceStartBeat + sourceLengthBeats),
            sourceFileDurationSeconds);
        const double sourceDurationSeconds = sourceEndSeconds - sourceStartSeconds;
        const double playbackStartSeconds = clock.secondsAtBeat(
            clip.startBeat + localStartBeat);
        const double playbackDurationSeconds = clock.secondsAtBeat(
            clip.startBeat + localStartBeat + localLengthBeats) - playbackStartSeconds;
        if (sourceDurationSeconds <= 0.0 || playbackDurationSeconds <= 0.0) return;
        AraPlaybackRegionDescriptor region;
        region.persistentId = (clip.loopEnabled
            ? QString("%1-loop-%2").arg(clip.clipId).arg(regionIndex)
            : clip.clipId).toUtf8().toStdString();
        region.audioSourcePersistentId = clip.contentId.toUtf8().toStdString();
        region.regionSequencePersistentId = QString("audio-track-%1")
            .arg(clip.trackIndex).toStdString();
        region.nameUtf8 = clip.name.toUtf8().toStdString();
        region.regionSequenceNameUtf8 = clip.trackName.toUtf8().toStdString();
        region.regionSequenceOrder = clip.trackIndex;
        region.routingTrackIndex = clip.trackIndex;
        region.sourceStartSeconds = sourceStartSeconds;
        region.sourceDurationSeconds = sourceDurationSeconds;
        region.playbackStartSeconds = playbackStartSeconds;
        region.playbackDurationSeconds = playbackDurationSeconds;
        region.requestTimeStretch = clip.syncMode > 0 ||
            std::abs(sourceDurationSeconds - playbackDurationSeconds) > 0.000001;
        regions.push_back(std::move(region));
        ++regionIndex;
    };

    const bool validLoop = clip.loopEnabled && std::isfinite(clip.loopStartBeats) &&
        clip.loopStartBeats >= 0.0 && std::isfinite(clip.loopLengthBeats) &&
        clip.loopLengthBeats >= 0.25;
    if (!validLoop) {
        appendRegion(0.0, clip.lengthBeats, qMax(0.0, clip.sourceOffsetBeats),
                     clip.lengthBeats);
        return;
    }
    double phase = std::fmod(clip.sourceOffsetBeats - clip.loopStartBeats,
                             clip.loopLengthBeats);
    if (phase < 0.0) phase += clip.loopLengthBeats;
    double localBeat = 0.0;
    constexpr int kMaxAraLoopRegionsPerClip = 65536;
    while (localBeat < clip.lengthBeats - 1.0e-9 &&
           regionIndex < kMaxAraLoopRegionsPerClip) {
        const double sourceStartBeat = clip.loopStartBeats + phase;
        const double segmentLength = qMin(clip.lengthBeats - localBeat,
            clip.loopLengthBeats - phase);
        if (!std::isfinite(segmentLength) || segmentLength <= 1.0e-9) break;
        appendRegion(localBeat, segmentLength, sourceStartBeat, segmentLength);
        localBeat += segmentLength;
        phase = 0.0;
    }
}

struct PanGains
{
    double left = 1.0;
    double right = 1.0;
};

static double canonicalPanLawDb(double centerAttenuationDb)
{
    static constexpr std::array<double, 4> laws{0.0, -3.0, -4.5, -6.0};
    if (!std::isfinite(centerAttenuationDb)) return -3.0;
    return *std::min_element(laws.begin(), laws.end(), [centerAttenuationDb](double a, double b) {
        return std::fabs(a - centerAttenuationDb) < std::fabs(b - centerAttenuationDb);
    });
}

static PanGains panGains(double gain, double pan, bool monoSource,
                         double centerAttenuationDb = -3.0)
{
    const double safeGain = finiteClamp(gain, 0.0, 2.0, 1.0);
    const double safePan = finiteClamp(pan, -1.0, 1.0, 0.0);
    if (monoSource) {
        constexpr double halfPi = 1.57079632679489661923;
        const double lawDb = canonicalPanLawDb(centerAttenuationDb);
        if (lawDb >= -1.0e-9) {
            const double left = safePan <= 0.0 ? 1.0 : std::cos(safePan * halfPi);
            const double right = safePan >= 0.0 ? 1.0 : std::cos(-safePan * halfPi);
            return {safeGain * left, safeGain * right};
        }
        constexpr double equalPowerCenterDb = -3.010299956639812;
        const double exponent = std::clamp(lawDb / equalPowerCenterDb, 0.0, 4.0);
        const double angle = (safePan + 1.0) * (halfPi * 0.5);
        const double leftBase = std::max(0.0, std::cos(angle));
        const double rightBase = std::max(0.0, std::sin(angle));
        return {safeGain * (leftBase <= 1.0e-15 ? 0.0 : std::pow(leftBase, exponent)),
                safeGain * (rightBase <= 1.0e-15 ? 0.0 : std::pow(rightBase, exponent))};
    }
    return {safeGain * (safePan <= 0.0 ? 1.0 : 1.0 - safePan),
            safeGain * (safePan >= 0.0 ? 1.0 : 1.0 + safePan)};
}

static std::array<double, 2> applyStereoSeparation(
    double left, double right, double separation)
{
    const double width = 1.0 - finiteClamp(separation, -1.0, 1.0, 0.0);
    const double mid = 0.5 * (left + right);
    const double side = 0.5 * (left - right) * width;
    return {mid + side, mid - side};
}

struct OfflinePluginOutputSinkContext
{
    std::map<int, QVector<float>>* trackBlocks = nullptr;
    int frames = 0;
    double panLawDb = -3.0;
};

static void accumulateOfflinePluginOutput(
    void* opaque, int destinationTrackId, const float* left, const float* right,
    unsigned long frameCount, float gain, float pan, bool mono)
{
    auto* context = static_cast<OfflinePluginOutputSinkContext*>(opaque);
    if (!context || !left || destinationTrackId < 0 || context->frames <= 0)
        return;
    const int frames = std::min<int>(context->frames,
        frameCount > static_cast<unsigned long>(INT32_MAX)
            ? INT32_MAX : static_cast<int>(frameCount));
    if (!context->trackBlocks) return;
    const auto found = context->trackBlocks->find(destinationTrackId);
    if (found == context->trackBlocks->end()) return;
    QVector<float>* destination = &found->second;
    if (destination->size() < context->frames * 2) destination->resize(context->frames * 2);
    const PanGains gains = panGains(gain, pan, mono, context->panLawDb);
    const float* safeRight = right ? right : left;
    for (int frame = 0; frame < frames; ++frame) {
        const int sample = frame * 2;
        (*destination)[sample] += static_cast<float>(left[frame] * gains.left);
        (*destination)[sample + 1] += static_cast<float>(safeRight[frame] * gains.right);
    }
}

static double pitchShiftRatio(double semitones)
{
    const double safeSemitones = finiteClamp(semitones, -48.0, 48.0, 0.0);
    return std::pow(2.0, safeSemitones / 12.0);
}

static double linearToDb(double value)
{
    const double safeValue = std::isfinite(value) ? qMax(value, 0.0) : 0.0;
    if (safeValue <= 0.000001) {
        return -120.0;
    }
    return 20.0 * std::log10(safeValue);
}

static float peakNormalizationGain(const DecodedAudioFile& audio)
{
    double peak = 0.0;
    for (float sample : audio.stereoSamples) {
        if (std::isfinite(sample)) peak = std::max(peak, std::fabs(static_cast<double>(sample)));
    }
    if (peak <= 1.0e-9) return 1.0f;
    return static_cast<float>(std::clamp(1.0 / peak, 0.0, 64.0));
}

static double synthLayerSample(double phase, int layerId)
{
    constexpr double twoPi = 6.2831853071795864769;
    const int timbre = ((layerId % 4) + 4) % 4;
    if (timbre == 1) {
        return (phase / twoPi) * 2.0 - 1.0;
    }
    if (timbre == 2) {
        return std::sin(phase) >= 0.0 ? 1.0 : -1.0;
    }
    if (timbre == 3) {
        const double normalized = phase / twoPi;
        return 1.0 - 4.0 * std::fabs(normalized - 0.5);
    }
    return std::sin(phase);
}

static quint32 tempoBpmToMidiMicroseconds(double bpm)
{
    const double safeBpm = std::isfinite(bpm) && bpm > 0.0 ? qBound(1.0, bpm, 999.0) : kDefaultTempoBpm;
    return static_cast<quint32>(qRound(60000000.0 / safeBpm));
}

static uint8_t midiDenominatorPower(int denominator)
{
    int safeDenominator = denominator;
    if (!validTimeSignatureDenominator(safeDenominator)) {
        safeDenominator = 4;
    }

    uint8_t power = 0;
    while (safeDenominator > 1) {
        safeDenominator /= 2;
        ++power;
    }
    return power;
}

struct MidiRoundTripTempoPoint
{
    quint32 tick = 0;
    quint32 microsPerQuarter = 500000;
    auto values() const { return std::tie(tick, microsPerQuarter); }
    bool operator==(const MidiRoundTripTempoPoint& other) const { return values() == other.values(); }
};

struct MidiRoundTripTimeSignaturePoint
{
    quint32 tick = 0;
    int numerator = 4;
    int denominator = 4;
    auto values() const { return std::tie(tick, numerator, denominator); }
    bool operator==(const MidiRoundTripTimeSignaturePoint& other) const
    {
        return values() == other.values();
    }
};

static std::vector<MidiRoundTripTempoPoint> normalizedMidiRoundTripTempoMap(
    const MidiClipDocument& document, int ppq)
{
    const int safePpq = std::clamp(ppq, kMinProjectPpq, kMaxProjectPpq);
    const double fallback = std::isfinite(document.tempoBpm) && document.tempoBpm > 0.0
        ? document.tempoBpm : kDefaultTempoBpm;
    const std::vector<MidiTempoPoint> points = normalizedTempoMap(document.tempoMap, fallback);
    std::vector<MidiRoundTripTempoPoint> result;
    result.reserve(points.size());
    for (const MidiTempoPoint& point : points) {
        if (point.beat > document.lengthBeats) break;
        const double tickValue = std::round(point.beat * safePpq);
        if (!std::isfinite(tickValue)) continue;
        const quint32 tick = static_cast<quint32>(std::clamp(
            tickValue, 0.0, static_cast<double>(std::numeric_limits<quint32>::max())));
        const MidiRoundTripTempoPoint canonical{tick, tempoBpmToMidiMicroseconds(point.bpm)};
        if (!result.empty() && result.back().tick == tick) result.back() = canonical;
        else result.push_back(canonical);
    }
    return result;
}

static std::vector<MidiRoundTripTimeSignaturePoint> normalizedMidiRoundTripTimeSignatureMap(
    const MidiClipDocument& document, int ppq)
{
    const int safePpq = std::clamp(ppq, kMinProjectPpq, kMaxProjectPpq);
    const std::vector<MidiTimeSignaturePoint> points = normalizedTimeSignatureMap(
        document.timeSignatureMap, document.timeSignatureNumerator,
        document.timeSignatureDenominator);
    std::vector<MidiRoundTripTimeSignaturePoint> result;
    result.reserve(points.size());
    for (const MidiTimeSignaturePoint& point : points) {
        if (point.beat > document.lengthBeats) break;
        const double tickValue = std::round(point.beat * safePpq);
        if (!std::isfinite(tickValue)) continue;
        const quint32 tick = static_cast<quint32>(std::clamp(
            tickValue, 0.0, static_cast<double>(std::numeric_limits<quint32>::max())));
        const MidiRoundTripTimeSignaturePoint canonical{
            tick, point.numerator, point.denominator};
        if (!result.empty() && result.back().tick == tick) result.back() = canonical;
        else result.push_back(canonical);
    }
    return result;
}

static QString withRequiredSuffix(const QString& path, const QString& suffix)
{
    QFileInfo info(path);
    if (info.suffix().compare(suffix.mid(1), Qt::CaseInsensitive) == 0) {
        return path;
    }
    QString base = info.path() == "." ? info.completeBaseName() : info.path() + "/" + info.completeBaseName();
    if (base.isEmpty()) {
        base = path;
    }
    return base + suffix;
}

static QString comparableFilePath(const QString& path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

static bool sameFilePath(const QString& first, const QString& second)
{
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
    return !first.trimmed().isEmpty() && !second.trimmed().isEmpty() &&
        comparableFilePath(first).compare(comparableFilePath(second), pathCase) == 0;
}

static bool hasRenderStorageCapacity(const QString& destinationPath, int sampleRate,
                                     double durationSeconds, QString* errorText)
{
    if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0) {
        if (errorText) *errorText = "Render duration is invalid.";
        return false;
    }
    const long double frames = std::ceil(
        static_cast<long double>(durationSeconds) *
        static_cast<long double>(qBound(8000, sampleRate, 384000)));
    const long double masterBytesEstimate = frames * 2.0L * sizeof(float) + 1024.0L * 1024.0L;
    constexpr qint64 reserveBytes = 64ll * 1024ll * 1024ll;
    if (!std::isfinite(static_cast<double>(masterBytesEstimate)) ||
        masterBytesEstimate > static_cast<long double>(
            (std::numeric_limits<qint64>::max() - reserveBytes) / 3)) {
        if (errorText) *errorText = "Render is too long to estimate storage safely.";
        return false;
    }
    const qint64 masterBytes = static_cast<qint64>(std::ceil(masterBytesEstimate));
    QStorageInfo temporaryStorage(QDir::tempPath());
    QStorageInfo destinationStorage(QFileInfo(destinationPath).absolutePath());
    temporaryStorage.refresh();
    destinationStorage.refresh();

#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity storagePathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity storagePathCase = Qt::CaseSensitive;
#endif
    const bool sameStorage = temporaryStorage.isValid() && destinationStorage.isValid() &&
        ((!temporaryStorage.device().isEmpty() &&
          temporaryStorage.device() == destinationStorage.device()) ||
         temporaryStorage.rootPath().compare(
             destinationStorage.rootPath(), storagePathCase) == 0);
    const qint64 temporaryNeeded = sameStorage
        ? masterBytes * 3 + reserveBytes : masterBytes + reserveBytes;
    const qint64 destinationNeeded = masterBytes * 2 + reserveBytes;
    auto enough = [&](const QStorageInfo& storage, qint64 needed, const QString& label) {
        if (!storage.isValid() || !storage.isReady() || storage.bytesAvailable() < 0) return true;
        if (storage.bytesAvailable() >= needed) return true;
        if (errorText) {
            *errorText = QString("%1 needs about %2 GiB free; only %3 GiB is available on %4.")
                .arg(label)
                .arg(static_cast<double>(needed) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2)
                .arg(static_cast<double>(storage.bytesAvailable()) /
                     (1024.0 * 1024.0 * 1024.0), 0, 'f', 2)
                .arg(storage.rootPath());
        }
        return false;
    };
    if (!enough(temporaryStorage, temporaryNeeded,
                sameStorage ? QStringLiteral("Offline render")
                            : QStringLiteral("Temporary master render"))) return false;
    return sameStorage || enough(destinationStorage, destinationNeeded,
                                 QStringLiteral("Encoded output staging"));
}

static bool moveFileWithCopyFallback(const QString& sourcePath, const QString& destinationPath)
{
    if (sourcePath.isEmpty() || destinationPath.isEmpty() ||
        !QFileInfo::exists(sourcePath) || QFileInfo::exists(destinationPath)) {
        return false;
    }
    if (QFile::rename(sourcePath, destinationPath)) {
        return true;
    }

    const qint64 sourceSize = QFileInfo(sourcePath).size();
    if (!QFile::copy(sourcePath, destinationPath)) {
        QFile::remove(destinationPath);
        return false;
    }
    if (!QFileInfo::exists(destinationPath) || QFileInfo(destinationPath).size() != sourceSize) {
        QFile::remove(destinationPath);
        return false;
    }
    if (!QFile::remove(sourcePath)) {
        QFile::remove(destinationPath);
        return false;
    }
    return true;
}

static bool installFileAtomically(const QString& sourcePath, const QString& destinationPath,
                                  QString* errorText)
{
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile() || sourceInfo.size() <= 0) {
        if (errorText) *errorText = "Validated temporary output is missing or empty.";
        return false;
    }
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
    if (sourceInfo.absoluteFilePath().compare(
            QFileInfo(destinationPath).absoluteFilePath(), pathCase) == 0) {
        if (errorText) *errorText = "Temporary and destination paths must differ.";
        return false;
    }

    QFile input(sourcePath);
    QSaveFile output(destinationPath);
    output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        output.cancelWriting();
        if (errorText) *errorText = "Could not prepare atomic file commit.";
        return false;
    }

    constexpr qint64 kCopyBlockBytes = 1024 * 1024;
    qint64 copiedBytes = 0;
    while (copiedBytes < sourceInfo.size()) {
        const QByteArray block = input.read(std::min(kCopyBlockBytes, sourceInfo.size() - copiedBytes));
        if (block.isEmpty() || output.write(block) != block.size()) {
            output.cancelWriting();
            if (errorText) *errorText = "Could not stage atomic file output.";
            return false;
        }
        copiedBytes += block.size();
    }
    if (copiedBytes != sourceInfo.size() || !input.atEnd()) {
        output.cancelWriting();
        if (errorText) *errorText = "Temporary output changed during atomic commit.";
        return false;
    }
    if (!output.commit()) {
        if (errorText) *errorText = "Could not atomically replace the destination file.";
        return false;
    }
    return true;
}

static int midiEventSortRank(uint8_t status, uint8_t data2)
{
    const uint8_t statusHi = status & 0xF0;
    if (statusHi == 0x80 || (statusHi == 0x90 && data2 == 0)) {
        return 0;
    }
    if (statusHi == 0xA0 || statusHi == 0xB0 || statusHi == 0xC0 ||
        statusHi == 0xD0 || statusHi == 0xE0) {
        return 1;
    }
    if (statusHi == 0x90) {
        return 2;
    }
    return 3;
}

static double safeClipStartBeat(double value)
{
    return std::isfinite(value) && value >= 0.0 ? value : 0.0;
}

static double safeClipLengthBeats(double value)
{
    return std::isfinite(value) && value > 0.0 ? qMax(0.25, value) : 4.0;
}

struct RecordedNoteSegment
{
    double startBeat = 0.0;
    double lengthBeats = 0.0;
};

static std::vector<RecordedNoteSegment> recordedNoteSegments(double startBeat, double endBeat,
                                                              double clipLength, bool loopEnabled,
                                                              double loopStartBeat, double loopEndBeat)
{
    std::vector<RecordedNoteSegment> segments;
    const double safeClipLength = std::isfinite(clipLength) && clipLength > 0.0 ? clipLength : 0.0;
    if (safeClipLength <= 0.0) return segments;

    const double start = qBound(0.0, startBeat, safeClipLength);
    const double end = qBound(0.0, endBeat, safeClipLength);
    const double loopStart = qBound(0.0, loopStartBeat, safeClipLength);
    const double loopEnd = qBound(0.0, loopEndBeat, safeClipLength);
    const bool wrapped = loopEnabled && loopEnd - loopStart >= 0.25 && end < start &&
        start >= loopStart && start <= loopEnd && end >= loopStart && end <= loopEnd;

    auto append = [&](double segmentStart, double segmentLength) {
        if (!std::isfinite(segmentLength) || segmentLength <= 1.0e-9) return;
        const double available = qMax(0.0, safeClipLength - segmentStart);
        if (available <= 0.0) return;
        segments.push_back({segmentStart, qMin(available, qMax(1.0 / 64.0, segmentLength))});
    };
    if (wrapped) {
        append(start, loopEnd - start);
        append(loopStart, end - loopStart);
    } else {
        append(start, end - start);
    }
    if (segments.empty()) append(start, 1.0 / 64.0);
    return segments;
}

static RecordedNoteSegment quantizedRecordedNoteSegment(const RecordedNoteSegment& source,
                                                         double clipLength,
                                                         double gridBeats)
{
    if (!std::isfinite(gridBeats) || gridBeats <= 0.0 ||
        !std::isfinite(clipLength) || clipLength <= 0.0) return source;
    RecordedNoteSegment result = source;
    const double safeLength = qBound(1.0 / 9600.0, source.lengthBeats, clipLength);
    const double quantizedStart = std::round(source.startBeat / gridBeats) * gridBeats;
    result.startBeat = qBound(0.0, quantizedStart, qMax(0.0, clipLength - safeLength));
    result.lengthBeats = qMin(safeLength, clipLength - result.startBeat);
    return result;
}

static double normalizedMidiRecordQuantizeBeats(double value)
{
    if (!std::isfinite(value)) return 0.0;
    for (const double candidate : {0.0, 1.0, 0.5, 1.0 / 3.0,
                                   0.25, 1.0 / 6.0, 0.125}) {
        if (std::fabs(value - candidate) < 1.0e-9) return candidate;
    }
    return 0.0;
}

static bool applyMidiReplaceRange(MidiClipDocument& current,
                                  const MidiClipDocument& before,
                                  double sourceStartBeat, double sourceEndBeat,
                                  const std::vector<MidiNote>& recordedNotes,
                                  const std::vector<MidiEvent>& recordedEvents)
{
    if (!std::isfinite(sourceStartBeat) || !std::isfinite(sourceEndBeat) ||
        sourceEndBeat - sourceStartBeat <= 1.0e-9) return false;
    MidiClipDocument replaced = current;
    replaced.notes.clear();
    replaced.notes.reserve(before.notes.size() + recordedNotes.size() + 8u);
    for (const MidiNote& note : before.notes) {
        const double noteStart = note.startBeat;
        const double noteEnd = note.startBeat + note.lengthBeats;
        if (!std::isfinite(noteStart) || !std::isfinite(noteEnd) ||
            noteEnd <= sourceStartBeat + 1.0e-9 ||
            noteStart >= sourceEndBeat - 1.0e-9) {
            replaced.notes.push_back(note);
            continue;
        }
        if (noteStart < sourceStartBeat - 1.0e-9) {
            MidiNote left = note;
            left.lengthBeats = sourceStartBeat - noteStart;
            if (left.lengthBeats > 1.0e-9) replaced.notes.push_back(left);
        }
        if (noteEnd > sourceEndBeat + 1.0e-9) {
            MidiNote right = note;
            right.startBeat = sourceEndBeat;
            right.lengthBeats = noteEnd - sourceEndBeat;
            if (right.lengthBeats > 1.0e-9) replaced.notes.push_back(right);
        }
    }
    replaced.notes.insert(replaced.notes.end(), recordedNotes.begin(), recordedNotes.end());
    std::stable_sort(replaced.notes.begin(), replaced.notes.end(), [](const MidiNote& left,
                                                                      const MidiNote& right) {
        if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
        return left.pitch < right.pitch;
    });

    replaced.performanceEvents.clear();
    replaced.performanceEvents.reserve(before.performanceEvents.size() + recordedEvents.size());
    for (const MidiEvent& event : before.performanceEvents) {
        if (!std::isfinite(event.beat) || event.beat < sourceStartBeat - 1.0e-9 ||
            event.beat >= sourceEndBeat - 1.0e-9)
            replaced.performanceEvents.push_back(event);
    }
    replaced.performanceEvents.insert(replaced.performanceEvents.end(),
        recordedEvents.begin(), recordedEvents.end());
    std::stable_sort(replaced.performanceEvents.begin(), replaced.performanceEvents.end(),
        [](const MidiEvent& left, const MidiEvent& right) { return left.beat < right.beat; });
    if (midiDocumentsPersistentlyEqual(current, replaced)) return false;
    current = std::move(replaced);
    return true;
}

static int safeRenderSampleRate(double streamSampleRate, int configuredSampleRate)
{
    const int streamRate = std::isfinite(streamSampleRate) ? static_cast<int>(std::lround(streamSampleRate)) : 0;
    if (streamRate >= 8000 && streamRate <= 384000) {
        return streamRate;
    }
    if (configuredSampleRate >= 8000 && configuredSampleRate <= 384000) {
        return configuredSampleRate;
    }
    return 44100;
}

static bool applyGainToFloat32StereoWav(
    const QString& path, double gain,
    const std::function<bool(qint64, qint64)>& progress,
    QString* errorText)
{
    auto fail = [&](const QString& message) {
        if (errorText) *errorText = message;
        return false;
    };
    if (!std::isfinite(gain) || gain <= 0.0)
        return fail("Master normalization gain is invalid.");

    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return fail("Could not open the temporary master WAV for normalization.");
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
    const QByteArray riff = file.read(12);
    if (riff.size() != 12 || riff.mid(0, 4) != "RIFF" || riff.mid(8, 4) != "WAVE")
        return fail("Temporary master is not a valid RIFF/WAVE file.");

    bool float32 = false;
    quint16 wavChannels = 0;
    qint64 dataOffset = -1;
    quint32 dataBytes = 0;
    qint64 cursor = 12;
    while (cursor + 8 <= file.size()) {
        if (!file.seek(cursor)) return fail("Could not seek in the temporary master WAV.");
        const QByteArray chunkHeader = file.read(8);
        if (chunkHeader.size() != 8) return fail("Temporary master WAV has a truncated chunk header.");
        const quint32 chunkBytes = little32(chunkHeader.constData() + 4);
        const qint64 payloadOffset = cursor + 8;
        if (payloadOffset + static_cast<qint64>(chunkBytes) > file.size())
            return fail("Temporary master WAV has a truncated chunk.");
        if (chunkHeader.left(4) == "fmt " && chunkBytes >= 16) {
            if (!file.seek(payloadOffset)) return fail("Could not read the temporary master format.");
            const QByteArray format = file.read(std::min<quint32>(chunkBytes, 40u));
            if (format.size() < 16) return fail("Temporary master format is truncated.");
            const quint16 formatTag = little16(format.constData());
            wavChannels = little16(format.constData() + 2);
            const bool ieeeFloat = formatTag == 3;
            const bool extensibleFloat = formatTag == 0xFFFE && format.size() >= 40 &&
                little16(format.constData() + 16) >= 22 &&
                little32(format.constData() + 24) == 3;
            float32 = wavChannels > 0 && (ieeeFloat || extensibleFloat) &&
                little16(format.constData() + 14) == 32;
        } else if (chunkHeader.left(4) == "data") {
            dataOffset = payloadOffset;
            dataBytes = chunkBytes;
            break;
        }
        cursor = payloadOffset + static_cast<qint64>(chunkBytes) + (chunkBytes & 1u);
    }
    const quint32 bytesPerFrame = static_cast<quint32>(wavChannels) * 4u;
    if (!float32 || dataOffset < 0 || dataBytes == 0 || bytesPerFrame == 0 ||
        (dataBytes % bytesPerFrame) != 0)
        return fail("Temporary master must be 32-bit IEEE-float WAV audio.");

    constexpr qint64 kBlockBytes = 512 * 1024;
    QByteArray block;
    block.resize(static_cast<int>(kBlockBytes));
    const qint64 totalFrames = static_cast<qint64>(dataBytes / bytesPerFrame);
    qint64 processedBytes = 0;
    while (processedBytes < dataBytes) {
        if (progress && !progress(processedBytes / bytesPerFrame, totalFrames))
            return fail("Master normalization cancelled.");
        const qint64 bytesThisBlock = std::min<qint64>(
            kBlockBytes, static_cast<qint64>(dataBytes) - processedBytes);
        if (!file.seek(dataOffset + processedBytes) ||
            file.read(block.data(), bytesThisBlock) != bytesThisBlock)
            return fail("Could not read temporary master samples for normalization.");
        for (qint64 offset = 0; offset < bytesThisBlock; offset += 4) {
            quint32 bits = little32(block.constData() + offset);
            float sample = 0.0f;
            std::memcpy(&sample, &bits, sizeof(sample));
            double scaled = std::isfinite(sample) ? static_cast<double>(sample) * gain : 0.0;
            const double limit = static_cast<double>(std::numeric_limits<float>::max());
            scaled = std::clamp(scaled, -limit, limit);
            sample = static_cast<float>(scaled);
            std::memcpy(&bits, &sample, sizeof(bits));
            block[static_cast<int>(offset)] = static_cast<char>(bits & 0xffu);
            block[static_cast<int>(offset + 1)] = static_cast<char>((bits >> 8) & 0xffu);
            block[static_cast<int>(offset + 2)] = static_cast<char>((bits >> 16) & 0xffu);
            block[static_cast<int>(offset + 3)] = static_cast<char>((bits >> 24) & 0xffu);
        }
        if (!file.seek(dataOffset + processedBytes) ||
            file.write(block.constData(), bytesThisBlock) != bytesThisBlock)
            return fail("Could not write normalized master samples.");
        processedBytes += bytesThisBlock;
    }
    if (progress && !progress(totalFrames, totalFrames))
        return fail("Master normalization cancelled.");
    if (!file.flush() || file.error() != QFileDevice::NoError)
        return fail("Could not finalize the normalized master WAV.");
    return true;
}

static bool writeFloat32StereoWav(const QString& path, const std::vector<float>& samples, int sampleRate)
{
    if (samples.size() < 2 || (samples.size() % 2u) != 0 || sampleRate < 8000 || sampleRate > 384000) return false;
    const quint64 dataBytes64 = static_cast<quint64>(samples.size()) * sizeof(float);
    if (dataBytes64 > std::numeric_limits<quint32>::max() - 48u) return false;
    const quint32 dataBytes = static_cast<quint32>(dataBytes64);
    const quint32 frameCount = static_cast<quint32>(samples.size() / 2u);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setFloatingPointPrecision(QDataStream::SinglePrecision);
    auto text = [&](const char* value) { return file.write(value, 4) == 4; };
    if (!text("RIFF")) return false;
    out << quint32(48u + dataBytes);
    if (!text("WAVE") || !text("fmt ")) return false;
    out << quint32(16) << quint16(3) << quint16(2) << quint32(sampleRate)
        << quint32(sampleRate * 8) << quint16(8) << quint16(32);
    if (!text("fact")) return false;
    out << quint32(4) << frameCount;
    if (!text("data")) return false;
    out << dataBytes;
    for (float sample : samples) out << (std::isfinite(sample) ? sample : 0.0f);
    return out.status() == QDataStream::Ok && file.commit();
}

static double estimateSampleTempoBpm(double durationSeconds, double projectTempoBpm,
                                     int signatureNumerator, int signatureDenominator)
{
    return estimateAudioTempoFromDuration(durationSeconds, projectTempoBpm,
        signatureNumerator, signatureDenominator).tempoBpm;
}
static double projectBeatsPerBar(int signatureNumerator, int signatureDenominator)
{
    return static_cast<double>(qBound(1, signatureNumerator, 32)) *
        (4.0 / static_cast<double>(qBound(1, signatureDenominator, 64)));
}

static QString uniqueMidiPatternName(const PlaylistView* playlist, int trackIndex, const QString& requestedName)
{
    const QString base = requestedName.trimmed().isEmpty() ? QString("Imported MIDI") : requestedName.trimmed();
    auto exists = [&](const QString& candidate) {
        if (!playlist) {
            return false;
        }
        for (const PlaylistView::MidiClipInfo& clip : playlist->getMidiClips()) {
            if (clip.trackIndex == trackIndex && clip.name == candidate) {
                return true;
            }
        }
        return false;
    };
    if (!exists(base)) {
        return base;
    }
    for (int suffix = 2; suffix < 10000; ++suffix) {
        const QString candidate = QString("%1 %2").arg(base).arg(suffix);
        if (!exists(candidate)) {
            return candidate;
        }
    }
    return QString("%1 %2").arg(base).arg(QDateTime::currentMSecsSinceEpoch());
}

static MidiClipDocument midiDocumentForClipExport(const MidiClipDocument& source,
                                                   const PlaylistView::MidiClipInfo& clip)
{
    MidiClipDocument result = source;
    result.name = clip.name;
    result.startBeat = 0.0;
    result.lengthBeats = safeClipLengthBeats(clip.lengthBeats);
    result.notes.clear();
    result.performanceEvents.clear();

    if (clip.loopEnabled && std::isfinite(clip.loopStartBeats) &&
        clip.loopStartBeats >= 0.0 && std::isfinite(clip.loopLengthBeats) &&
        clip.loopLengthBeats >= 0.25) {
        PlaylistView::MidiClipInfo cycleClip = clip;
        cycleClip.startBeat = 0.0;
        cycleClip.sourceOffsetBeats = clip.loopStartBeats;
        cycleClip.lengthBeats = clip.loopLengthBeats;
        cycleClip.loopEnabled = false;
        const MidiClipDocument cycle = midiDocumentForClipExport(source, cycleClip);
        const double cycleLength = clip.loopLengthBeats;
        double phase = std::fmod(qMax(0.0, clip.sourceOffsetBeats) -
            clip.loopStartBeats, cycleLength);
        if (phase < 0.0) phase += cycleLength;
        const int repetitions = static_cast<int>(qBound(1.0,
            std::ceil((result.lengthBeats + phase) / cycleLength) + 1.0,
            65536.0));
        constexpr size_t maximumLoopItems = 1000000u;
        for (const MidiNote& cycleNote : cycle.notes) {
            for (int repetition = -1; repetition <= repetitions; ++repetition) {
                const double start = cycleNote.startBeat - phase +
                    static_cast<double>(repetition) * cycleLength;
                const double end = start + cycleNote.lengthBeats;
                const double visibleStart = qMax(0.0, start);
                const double visibleEnd = qMin(result.lengthBeats, end);
                if (visibleEnd <= visibleStart) continue;
                MidiNote note = cycleNote;
                note.startBeat = visibleStart;
                note.lengthBeats = visibleEnd - visibleStart;
                result.notes.push_back(std::move(note));
                if (result.notes.size() >= maximumLoopItems) break;
            }
            if (result.notes.size() >= maximumLoopItems) break;
        }
        std::map<int, MidiEvent> chasedState;
        for (const MidiEvent& cycleEvent : cycle.performanceEvents) {
            const uint8_t statusHi = cycleEvent.status & 0xF0;
            const bool noteEvent = statusHi == 0x80 || statusHi == 0x90;
            if (!noteEvent && cycleEvent.beat < phase) {
                const int stateKey = (statusHi == 0xA0 || statusHi == 0xB0)
                    ? ((static_cast<int>(cycleEvent.status) << 8) | cycleEvent.data1)
                    : (static_cast<int>(cycleEvent.status) << 8);
                chasedState[stateKey] = cycleEvent;
            }
            for (int repetition = -1; repetition <= repetitions; ++repetition) {
                const double beat = cycleEvent.beat - phase +
                    static_cast<double>(repetition) * cycleLength;
                if (beat < 0.0 || beat > result.lengthBeats) continue;
                MidiEvent event = cycleEvent;
                event.beat = beat;
                result.performanceEvents.push_back(event);
                if (result.performanceEvents.size() >= maximumLoopItems) break;
            }
            if (result.performanceEvents.size() >= maximumLoopItems) break;
        }
        for (const auto& state : chasedState) {
            MidiEvent event = state.second;
            event.beat = 0.0;
            result.performanceEvents.push_back(event);
        }
        result.tempoMap.clear();
        for (int repetition = -1; repetition <= repetitions; ++repetition) {
            for (const MidiTempoPoint& point : cycle.tempoMap) {
                const double beat = point.beat - phase +
                    static_cast<double>(repetition) * cycleLength;
                if (beat < 0.0 || beat > result.lengthBeats) continue;
                result.tempoMap.push_back({beat, point.bpm});
            }
        }
        if (result.tempoMap.empty() || result.tempoMap.front().beat > 0.0)
            result.tempoMap.insert(result.tempoMap.begin(),
                {0.0, tempoAtBeat(cycle.tempoMap, phase, source.tempoBpm)});
        result.tempoMap = normalizedTempoMap(result.tempoMap, source.tempoBpm);
        result.tempoBpm = result.tempoMap.front().bpm;
        return result;
    }

    const double sourceStart = qMax(0.0, clip.sourceOffsetBeats);
    const double sourceEnd = sourceStart + result.lengthBeats;
    const std::vector<MidiTempoPoint> sourceTempoMap = normalizedTempoMap(source.tempoMap, source.tempoBpm);
    result.tempoMap.clear();
    result.tempoMap.push_back({0.0, tempoAtBeat(sourceTempoMap, sourceStart, source.tempoBpm)});
    for (const MidiTempoPoint& point : sourceTempoMap) {
        if (point.beat <= sourceStart || point.beat > sourceEnd) continue;
        result.tempoMap.push_back({point.beat - sourceStart, point.bpm});
    }
    result.tempoBpm = result.tempoMap.front().bpm;
    for (const MidiNote& sourceNote : source.notes) {
        if (!isFiniteBeatRange(sourceNote.startBeat, sourceNote.lengthBeats)) continue;
        const double visibleStart = qMax(sourceStart, sourceNote.startBeat);
        const double visibleEnd = qMin(sourceEnd, sourceNote.startBeat + sourceNote.lengthBeats);
        if (visibleEnd <= visibleStart) continue;
        MidiNote note = sourceNote;
        note.startBeat = visibleStart - sourceStart;
        note.lengthBeats = visibleEnd - visibleStart;
        result.notes.push_back(std::move(note));
    }

    std::map<int, MidiEvent> chasedState;
    for (const MidiEvent& sourceEvent : source.performanceEvents) {
        if (!std::isfinite(sourceEvent.beat) || sourceEvent.beat < 0.0 ||
            !isValidMidiChannelEvent(sourceEvent.status, sourceEvent.data1, sourceEvent.data2)) continue;
        const uint8_t statusHi = sourceEvent.status & 0xF0;
        const bool noteOff = statusHi == 0x80 || (statusHi == 0x90 && sourceEvent.data2 == 0);
        const bool noteEvent = noteOff || statusHi == 0x90;
        if (sourceEvent.beat < sourceStart) {
            if (!noteEvent) {
                const int stateKey = (statusHi == 0xA0 || statusHi == 0xB0)
                    ? ((static_cast<int>(sourceEvent.status) << 8) | sourceEvent.data1)
                    : (static_cast<int>(sourceEvent.status) << 8);
                chasedState[stateKey] = sourceEvent;
            }
            continue;
        }
        if (sourceEvent.beat >= sourceEnd) {
            if (noteOff) {
                MidiEvent event = sourceEvent;
                event.beat = result.lengthBeats;
                result.performanceEvents.push_back(event);
            }
            continue;
        }
        MidiEvent event = sourceEvent;
        event.beat -= sourceStart;
        result.performanceEvents.push_back(event);
    }
    for (const auto& state : chasedState) {
        MidiEvent event = state.second;
        event.beat = 0.0;
        result.performanceEvents.push_back(event);
    }
    return result;
}

bool stemSourceClipMatchesSnapshot(const PlaylistView::AudioClipInfo& current,
                                   const PlaylistView::AudioClipInfo& snapshot)
{
    return current.clipId == snapshot.clipId &&
        current.contentId == snapshot.contentId &&
        current.trackIndex == snapshot.trackIndex &&
        current.name == snapshot.name &&
        current.startBeat == snapshot.startBeat &&
        current.lengthBeats == snapshot.lengthBeats &&
        current.sourceOffsetBeats == snapshot.sourceOffsetBeats &&
        current.loopEnabled == snapshot.loopEnabled &&
        current.loopStartBeats == snapshot.loopStartBeats &&
        current.loopLengthBeats == snapshot.loopLengthBeats &&
        current.warpLengthBeats == snapshot.warpLengthBeats &&
        current.sourceTempoBpm == snapshot.sourceTempoBpm &&
        current.tempoConfidence == snapshot.tempoConfidence &&
        current.sourceSignatureNumerator == snapshot.sourceSignatureNumerator &&
        current.sourceSignatureDenominator == snapshot.sourceSignatureDenominator &&
        current.tempoDetected == snapshot.tempoDetected &&
        current.transientSeconds == snapshot.transientSeconds &&
        current.warpMarkers == snapshot.warpMarkers &&
        current.pitchShiftSemitones == snapshot.pitchShiftSemitones &&
        current.syncMode == snapshot.syncMode &&
        current.targetBars == snapshot.targetBars &&
        current.beatsPerBar == snapshot.beatsPerBar &&
        current.sourceDurationSeconds == snapshot.sourceDurationSeconds &&
        current.gain == snapshot.gain &&
        current.pan == snapshot.pan &&
        current.normalize == snapshot.normalize &&
        current.reverse == snapshot.reverse &&
        current.reversePolarity == snapshot.reversePolarity &&
        current.fadeInBeats == snapshot.fadeInBeats &&
        current.fadeOutBeats == snapshot.fadeOutBeats &&
        current.sourcePath == snapshot.sourcePath &&
        current.mediaOffline == snapshot.mediaOffline &&
        current.gainEnvelope == snapshot.gainEnvelope &&
        current.fadeInCurve == snapshot.fadeInCurve &&
        current.fadeOutCurve == snapshot.fadeOutCurve &&
        current.muted == snapshot.muted;
}
} // namespace

VeraCoreQT::VeraCoreQT(QWidget *parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);
    m_commandRegistry = new CommandRegistry(this);
    m_selectionService = new SelectionService(this);
    m_mixerParameters = new MixerParameterService(this);
    m_stemSeparator = new StemSeparationService(this);
    qApp->installEventFilter(this);

    connect(m_stemSeparator, &StemSeparationService::progressChanged, this,
            [this](const QString& jobId, int percent, const QString& detail) {
        if (!m_pendingStemJobId.isEmpty() && jobId != m_pendingStemJobId) return;
        if (m_stemProgressDialog) {
            const int itemPercent = m_pendingStemInputRendered
                ? 15 + qBound(0, percent, 100) * 85 / 100
                : qBound(0, percent, 100);
            m_stemProgressDialog->setValue(stemBatchOverallProgress(itemPercent));
            if (!detail.isEmpty()) m_stemProgressDialog->setLabelText(
                m_stemBatchActive
                    ? tr("Batch %1/%2 — %3")
                        .arg(m_stemBatchProcessed + 1).arg(m_stemBatchTotal).arg(detail)
                    : detail);
        }
        if (!detail.isEmpty()) ui.statusBar->showMessage(detail);
    });
    connect(m_stemSeparator, &StemSeparationService::separationFinished, this,
            [this](const QString& jobId, const QString&, const QStringList& names,
                   const QStringList& paths) {
        if (m_stemProgressDialog) {
            m_stemProgressDialog->close();
            m_stemProgressDialog->deleteLater();
            m_stemProgressDialog = nullptr;
        }
        const bool imported = importSeparatedStems(jobId, names, paths);
        if (m_stemBatchActive) {
            ++m_stemBatchProcessed;
            if (imported) ++m_stemBatchSucceeded;
            else ++m_stemBatchFailed;
            QTimer::singleShot(0, this, &VeraCoreQT::startNextStemBatchItem);
        }
    });
    connect(m_stemSeparator, &StemSeparationService::separationFailed, this,
            [this](const QString& jobId, const QString& message) {
        if (!m_pendingStemJobId.isEmpty() && jobId != m_pendingStemJobId) return;
        if (m_stemProgressDialog) {
            m_stemProgressDialog->close();
            m_stemProgressDialog->deleteLater();
            m_stemProgressDialog = nullptr;
        }
        m_pendingStemJobId.clear();
        m_pendingStemManifestPath.clear();
        m_pendingStemInputRendered = false;
        if (m_stemBatchActive) {
            ++m_stemBatchProcessed;
            ++m_stemBatchFailed;
            ui.statusBar->showMessage(message, 5000);
            QTimer::singleShot(0, this, &VeraCoreQT::startNextStemBatchItem);
        } else {
            QMessageBox::critical(this, tr("Separate Stems"), message);
        }
    });
    connect(m_stemSeparator, &StemSeparationService::separationCancelled, this,
            [this](const QString& jobId) {
        if (!m_pendingStemJobId.isEmpty() && jobId != m_pendingStemJobId) return;
        if (m_stemProgressDialog) {
            m_stemProgressDialog->close();
            m_stemProgressDialog->deleteLater();
            m_stemProgressDialog = nullptr;
        }
        m_pendingStemJobId.clear();
        m_pendingStemManifestPath.clear();
        m_pendingStemInputRendered = false;
        if (m_stemBatchActive) {
            m_stemBatchCancelRequested = true;
            m_stemBatchQueue.clear();
            finishStemBatch();
        } else {
            ui.statusBar->showMessage(tr("Stem separation cancelled."), 3000);
        }
    });
    m_stemRecoveryAction = new QAction(tr("Stem Job Recovery..."), this);
    m_stemRecoveryAction->setEnabled(false);
    ui.menuOptions->addSeparator();
    ui.menuOptions->addAction(m_stemRecoveryAction);
    connect(m_stemRecoveryAction, &QAction::triggered,
            this, &VeraCoreQT::showStemRecoveryDialog);
    connect(ui.menuOptions, &QMenu::aboutToShow,
            this, &VeraCoreQT::refreshStemRecoveryAction);
    refreshStemRecoveryAction();

    // Register PluginInfo meta types for signals/deviceSlots across threads
    qRegisterMetaType<PluginInfo>("PluginInfo");
    qRegisterMetaType<std::vector<PluginInfo>>("std::vector<PluginInfo>");

    // Inspector docks at the bottom of the center area (Ableton-style device
    // strip under the arrangement), not as a third column.
    m_detailTabs = new QTabWidget(this);
    m_detailTabs->setObjectName(QStringLiteral("detailEditorTabs"));
    m_detailTabs->setDocumentMode(true);
    m_inspectorWidget = new InspectorWidget(m_detailTabs);
    m_audioEditorWidget = new AudioEditorWidget(m_detailTabs);
    m_detailTabs->addTab(m_inspectorWidget, tr("DEVICE / CLIP"));
    m_detailTabs->addTab(m_audioEditorWidget, tr("AUDIO EDITOR"));
    ui.centerSplitter->addWidget(m_detailTabs);
    connect(m_mixerParameters, &MixerParameterService::parametersChanged, this,
            [this](quint64, Track* track, std::uint32_t changes) {
        const bool republish = (changes & MixerParameterService::Republish) != 0;
        if (republish) {
            for (TrackWidget* widget : m_trackWidgets)
                if (widget) widget->refreshFromTrack();
        } else if (track) {
            for (TrackWidget* widget : m_trackWidgets)
                if (widget && widget->GetTrack() == track) widget->refreshFromTrack();
        }
        if (m_inspectorWidget) {
            if (republish || (changes & MixerParameterService::ReturnBus))
                m_inspectorWidget->setReturnBuses(m_mixerParameters->returnBuses());
            if (republish || (changes & MixerParameterService::MasterBus))
                m_inspectorWidget->setMasterBus(m_mixerParameters->masterBus());
            if (republish || !track || (m_selectedTrackIndex >= 0 &&
                m_selectedTrackIndex < static_cast<int>(m_tracks.size()) &&
                m_tracks[static_cast<std::size_t>(m_selectedTrackIndex)].get() == track))
                m_inspectorWidget->refreshMixerControls();
        }
        if (m_restoringProject) return;
        if (republish || (changes & (MixerParameterService::TrackFader |
            MixerParameterService::TrackSend | MixerParameterService::ReturnBus |
            MixerParameterService::MasterBus))) syncMixerRoutingToAudio();
        if (republish || (changes & MixerParameterService::TrackAudibility)) {
            syncRealtimeMidiLayersToAudio();
            syncMidiTimelineToAudio();
        }
        if (republish || (changes & (MixerParameterService::TrackAudibility |
            MixerParameterService::TrackInput))) syncAudioTimelineToAudio();
        if (changes & MixerParameterService::MasterBusFx)
            syncRealtimeMidiLayersToAudio();
        if (republish || (changes & (MixerParameterService::TrackFader |
            MixerParameterService::TrackAudibility | MixerParameterService::TrackInput)))
            syncAudioInputState();
    });
    connect(m_inspectorWidget, &InspectorWidget::openMidiEditorRequested,
            this, &VeraCoreQT::onOpenCurrentMidiEditor);
    connect(m_inspectorWidget, &InspectorWidget::addLayerRequested,
            this, &VeraCoreQT::onAddLayerRequested);
    connect(m_inspectorWidget, &InspectorWidget::layerEnabledChanged,
            this, &VeraCoreQT::onLayerEnabledChanged);
    connect(m_inspectorWidget, &InspectorWidget::layerParametersChanged,
            this, &VeraCoreQT::onLayerParametersChanged);
    connect(m_inspectorWidget, &InspectorWidget::layerOutputRoutingRequested,
            this, &VeraCoreQT::onLayerOutputRoutingRequested);
    connect(m_inspectorWidget, &InspectorWidget::pluginParametersRequested,
            this, [this](bool audioFx, int slotIndex, const QString& deviceName) {
        if (!m_pluginHost || slotIndex < 0) return;
        const int returnBus = m_inspectorWidget->currentReturnFxBus();
        const bool returnEditor = audioFx && returnBus >= 0 && returnBus < kReturnBusCount;
        if (!returnEditor && (m_selectedTrackIndex < 0 ||
            m_selectedTrackIndex >= static_cast<int>(m_tracks.size()))) return;
        const int hostTrackId = returnEditor
            ? VeraCoreReturnAudioFxTrackId(returnBus) : m_selectedTrackIndex;
        const bool editorChanged = m_parameterEditorTrackIndex != hostTrackId ||
            m_parameterEditorReturnBus != (returnEditor ? returnBus : -1) ||
            m_parameterEditorSlotIndex != slotIndex || m_parameterEditorAudioFx != audioFx;
        if (editorChanged) {
            finishAutomationWrite();
            m_hasAutomationTarget = false;
            m_automationParameterId = 0;
            m_automationParameterName.clear();
            m_inspectorWidget->clearActiveAutomationTarget();
            if (m_playlistView) m_playlistView->clearPluginAutomationLane();
        }
        m_parameterEditorTrackIndex = hostTrackId;
        m_parameterEditorReturnBus = returnEditor ? returnBus : -1;
        m_parameterEditorSlotIndex = slotIndex;
        m_parameterEditorAudioFx = audioFx;
        m_parameterEditorDeviceName = deviceName;
        refreshInspectorPluginParameters();
    });
    connect(m_inspectorWidget, &InspectorWidget::pluginAutomationTargetRequested,
            this, [this](bool audioFx, int slotIndex, uint32_t parameterId,
                         const QString& parameterName) {
        const int contextTrackId = m_parameterEditorReturnBus >= 0
            ? VeraCoreReturnAudioFxTrackId(m_parameterEditorReturnBus) : m_selectedTrackIndex;
        if (contextTrackId != m_parameterEditorTrackIndex || slotIndex < 0 ||
            slotIndex != m_parameterEditorSlotIndex || audioFx != m_parameterEditorAudioFx)
            return;
        const bool sameTarget = m_hasAutomationTarget &&
            m_automationParameterId == parameterId;
        finishAutomationWrite();
        if (sameTarget) {
            m_hasAutomationTarget = false;
            m_automationParameterId = 0;
            m_automationParameterName.clear();
            m_inspectorWidget->clearActiveAutomationTarget();
            if (m_playlistView) m_playlistView->clearPluginAutomationLane();
            return;
        }
        m_hasAutomationTarget = true;
        m_automationParameterId = parameterId;
        m_automationParameterName = parameterName.trimmed();
        if (m_parameterEditorReturnBus >= 0) {
            const QString prefix = QString("Return %1 · ").arg(
                QChar('A' + m_parameterEditorReturnBus));
            if (!m_automationParameterName.startsWith(prefix))
                m_automationParameterName.prepend(prefix);
        }
        m_inspectorWidget->setActiveAutomationTarget(audioFx, slotIndex, parameterId);
        refreshPluginAutomationLane();
        // Write is armed from transport start, so changing its target while
        // rolling must immediately resume capture for the newly selected lane.
        if (m_isPlaying &&
            m_automationWriteMode == InspectorWidget::AutomationWriteMode::Write)
            beginAutomationWriteAtCurrentValue();
    });
    connect(m_inspectorWidget, &InspectorWidget::pluginAutomationModeChanged,
            this, [this](InspectorWidget::AutomationWriteMode mode) {
        if (m_automationWriteMode != mode &&
            (m_automationWriteActive || m_automationWriteOwnsHostGesture))
            finishAutomationWrite();
        m_automationWriteMode = mode;
        if (mode == InspectorWidget::AutomationWriteMode::Write && m_isPlaying)
            beginAutomationWriteAtCurrentValue();
    });
    connect(m_inspectorWidget, &InspectorWidget::pluginParameterGestureBegin,
            this, [this](bool audioFx, int slotIndex, uint32_t parameterId) {
        const int contextTrackId = m_parameterEditorReturnBus >= 0
            ? VeraCoreReturnAudioFxTrackId(m_parameterEditorReturnBus) : m_selectedTrackIndex;
        if (!m_pluginHost || contextTrackId != m_parameterEditorTrackIndex ||
            slotIndex != m_parameterEditorSlotIndex || audioFx != m_parameterEditorAudioFx) return;
        if (m_automationWriteActive &&
            m_automationWriteMode == InspectorWidget::AutomationWriteMode::Write &&
            parameterId == m_automationParameterId)
            return;
        if (m_automationWriteActive) finishAutomationWrite();
        armAutomationWriteTarget(m_parameterEditorTrackIndex, audioFx, slotIndex,
                                 parameterId, m_automationParameterName);
        bool began = false;
        if (audioFx)
            began = m_pluginHost->beginAudioFxParameterGesture(
                m_parameterEditorTrackIndex, slotIndex, parameterId);
        else
            began = m_pluginHost->beginLayerParameterGesture(
                m_parameterEditorTrackIndex * kLayerIdStride + slotIndex, parameterId);
        if (began) {
            m_automationWriteOwnsHostGesture = true;
            ++m_parameterEditorGestureDepth;
            ++m_automationWriteGestureSerial;
            if (m_isPlaying && m_hasAutomationTarget &&
                parameterId == m_automationParameterId &&
                m_automationWriteMode != InspectorWidget::AutomationWriteMode::Read) {
                const std::vector<PluginParameterInfo> parameters = audioFx
                    ? m_pluginHost->parametersForAudioFx(m_parameterEditorTrackIndex, slotIndex)
                    : m_pluginHost->parametersForLayer(
                        m_parameterEditorTrackIndex * kLayerIdStride + slotIndex);
                const auto current = std::find_if(parameters.begin(), parameters.end(),
                    [parameterId](const auto& item) { return item.stableId == parameterId; });
                if (current != parameters.end())
                    writePluginAutomationPoint(parameterId, m_automationParameterName,
                                               m_playheadBeat, current->normalizedValue);
            }
        } else {
            finishAutomationWrite();
        }
    });
    connect(m_inspectorWidget, &InspectorWidget::pluginParameterChanged,
            this, [this](bool audioFx, int slotIndex, uint32_t parameterId, double value) {
        const int contextTrackId = m_parameterEditorReturnBus >= 0
            ? VeraCoreReturnAudioFxTrackId(m_parameterEditorReturnBus) : m_selectedTrackIndex;
        if (!m_pluginHost || contextTrackId != m_parameterEditorTrackIndex ||
            slotIndex != m_parameterEditorSlotIndex || audioFx != m_parameterEditorAudioFx) return;
        if (audioFx)
            m_pluginHost->setAudioFxParameterNormalized(
                m_parameterEditorTrackIndex, slotIndex, parameterId, value);
        else
            m_pluginHost->setLayerParameterNormalized(
                m_parameterEditorTrackIndex * kLayerIdStride + slotIndex, parameterId, value);
        const bool write = m_isPlaying && m_hasAutomationTarget &&
            parameterId == m_automationParameterId && m_parameterEditorGestureDepth > 0 &&
            m_automationWriteMode != InspectorWidget::AutomationWriteMode::Read;
        if (write) {
            writePluginAutomationPoint(parameterId, m_automationParameterName,
                                       m_playheadBeat, value);
        }
    });
    connect(m_inspectorWidget, &InspectorWidget::pluginParameterGestureEnd,
            this, [this](bool audioFx, int slotIndex, uint32_t parameterId) {
        const int contextTrackId = m_parameterEditorReturnBus >= 0
            ? VeraCoreReturnAudioFxTrackId(m_parameterEditorReturnBus) : m_selectedTrackIndex;
        if (!m_pluginHost || contextTrackId != m_parameterEditorTrackIndex ||
            slotIndex != m_parameterEditorSlotIndex || audioFx != m_parameterEditorAudioFx) return;
        if (m_automationWriteActive && m_automationWriteOwnsHostGesture &&
            parameterId != m_automationWriteParameterId)
            return;
        if (m_automationWriteActive && parameterId == m_automationParameterId) {
            if (m_automationWriteMode == InspectorWidget::AutomationWriteMode::Touch) {
                finishTouchAutomationWrite(m_playheadBeat);
                return;
            }
            if (!m_isPlaying) {
                finishAutomationWrite();
                return;
            }
            if (m_automationWriteMode == InspectorWidget::AutomationWriteMode::Write ||
                m_automationWriteMode == InspectorWidget::AutomationWriteMode::Latch)
                return;
        }
        // A target/mode change can finish the owned gesture before the widget's
        // delayed release signal arrives. Never send a second host end.
        if (m_parameterEditorGestureDepth <= 0) return;
        if (audioFx)
            m_pluginHost->endAudioFxParameterGesture(
                m_parameterEditorTrackIndex, slotIndex, parameterId);
        else
            m_pluginHost->endLayerParameterGesture(
                m_parameterEditorTrackIndex * kLayerIdStride + slotIndex, parameterId);
        if (m_parameterEditorGestureDepth > 0) --m_parameterEditorGestureDepth;
        m_automationWriteOwnsHostGesture = false;
        m_automationWriteTrackIndex = -1;
        m_automationWriteAudioFx = false;
        m_automationWriteSlotIndex = -1;
        m_automationWriteInstanceEpoch = 0;
        m_automationWriteParameterId = 0;
        m_automationWriteParameterName.clear();
    });
    connect(m_inspectorWidget, &InspectorWidget::layerReloadRequested,
            this, [this](int layerIndex) {
        if (!m_pluginHost || m_selectedTrackIndex < 0 ||
            m_selectedTrackIndex >= static_cast<int>(m_tracks.size())) return;
        const auto* midiTrack = dynamic_cast<const MidiTrack*>(m_tracks[m_selectedTrackIndex].get());
        if (!midiTrack || layerIndex < 0 ||
            layerIndex >= static_cast<int>(midiTrack->GetLayerSlots().size())) return;
        finishAutomationWrite();
        ui.statusBar->showMessage("Reloading faulted instrument DSP...", 2000);
        syncRealtimeMidiLayersToAudio();
        const int layerId = m_selectedTrackIndex * kLayerIdStride + layerIndex;
        ui.statusBar->showMessage(m_pluginHost->isLayerLoaded(layerId)
            ? "Instrument DSP reloaded."
            : "Instrument reload failed; internal fallback remains active.", 5000);
    });
    connect(m_inspectorWidget, &InspectorWidget::removeLayerRequested,
            this, &VeraCoreQT::onRemoveLayerRequested);
    connect(m_inspectorWidget, &InspectorWidget::mixerControlsEdited,
            this, &VeraCoreQT::applyTrackMixerControls);
    connect(m_inspectorWidget, &InspectorWidget::spatialPannerEdited,
            this, &VeraCoreQT::applyTrackSpatialPanner);
    connect(m_inspectorWidget, &InspectorWidget::trackSendEdited,
            this, &VeraCoreQT::applyTrackSendLevel);
    connect(m_inspectorWidget, &InspectorWidget::returnBusEdited,
            this, &VeraCoreQT::applyReturnBusControls);
    connect(m_inspectorWidget, &InspectorWidget::masterBusEdited,
            this, &VeraCoreQT::applyMasterBusControls);
    connect(m_inspectorWidget, &InspectorWidget::audioFxEnabledChanged, this, [this](int index, bool enabled) {
        if (m_selectedTrackIndex < 0 || m_selectedTrackIndex >= static_cast<int>(m_tracks.size())) return;
        const std::shared_ptr<Track> trackRef = m_tracks[m_selectedTrackIndex];
        const std::vector<AudioFxSlot>& deviceSlots = trackRef->GetAudioFxSlots();
        if (index < 0 || index >= static_cast<int>(deviceSlots.size()) || deviceSlots[index].enabled == enabled) return;
        const std::vector<AudioFxSlot> beforeSlots = deviceSlots;
        const bool captureUndo = m_playlistView && !m_restoringProject &&
            !m_suppressProjectUndoCapture;
        const quint64 beforeStateId = captureUndo
            ? m_playlistView->currentArrangementStateId() : 0;
        const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
        if (captureUndo) {
            m_playlistView->beginArrangementTransaction();
            m_playlistView->markArrangementTransactionChanged();
        }
        trackRef->SetAudioFxEnabled(index, enabled);
        const std::vector<AudioFxSlot> afterSlots = trackRef->GetAudioFxSlots();
        if (captureUndo) {
            commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
                [trackRef, beforeSlots]() { trackRef->SetAudioFxSlots(beforeSlots); },
                [trackRef, afterSlots]() { trackRef->SetAudioFxSlots(afterSlots); });
        } else {
            setProjectDirty();
        }
        m_inspectorWidget->setTrack(trackRef.get());
        syncRealtimeMidiLayersToAudio();
    });
    connect(m_inspectorWidget, &InspectorWidget::audioFxReloadRequested,
            this, [this](int index) {
        if (!m_pluginHost || m_selectedTrackIndex < 0 ||
            m_selectedTrackIndex >= static_cast<int>(m_tracks.size())) return;
        const auto& deviceSlots = m_tracks[m_selectedTrackIndex]->GetAudioFxSlots();
        if (index < 0 || index >= static_cast<int>(deviceSlots.size())) return;
        finishAutomationWrite();
        ui.statusBar->showMessage("Reloading faulted audio effect...", 2000);
        syncRealtimeMidiLayersToAudio();
        ui.statusBar->showMessage(m_pluginHost->isAudioFxLoaded(m_selectedTrackIndex, index)
            ? "Audio effect DSP reloaded."
            : "Audio effect reload failed; slot remains bypassed.", 5000);
    });
    connect(m_inspectorWidget, &InspectorWidget::removeAudioFxRequested, this, [this](int index) {
        if (m_selectedTrackIndex < 0 || m_selectedTrackIndex >= static_cast<int>(m_tracks.size())) return;
        const int trackIndex = m_selectedTrackIndex;
        const std::shared_ptr<Track> trackRef = m_tracks[trackIndex];
        if (index < 0 || index >= static_cast<int>(trackRef->GetAudioFxSlots().size())) return;
        finishAutomationWrite();
        if (m_pluginHost) {
            for (int slotIndex = index;
                 slotIndex < static_cast<int>(trackRef->GetAudioFxSlots().size()); ++slotIndex) {
                const QByteArray state = m_pluginHost->captureAudioFxState(trackIndex, slotIndex);
                if (!state.isEmpty()) trackRef->SetAudioFxPluginState(slotIndex, state);
            }
        }
        const std::vector<AudioFxSlot> beforeSlots = trackRef->GetAudioFxSlots();
        const bool captureUndo = m_playlistView && !m_restoringProject &&
            !m_suppressProjectUndoCapture;
        const quint64 beforeStateId = captureUndo
            ? m_playlistView->currentArrangementStateId() : 0;
        const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
        if (captureUndo) {
            m_playlistView->beginArrangementTransaction();
            m_playlistView->markArrangementTransactionChanged();
        }
        trackRef->RemoveAudioFx(index);
        const std::vector<AudioFxSlot> afterSlots = trackRef->GetAudioFxSlots();
        if (captureUndo) {
            commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
                [trackRef, beforeSlots]() { trackRef->SetAudioFxSlots(beforeSlots); },
                [trackRef, afterSlots]() { trackRef->SetAudioFxSlots(afterSlots); });
        } else {
            setProjectDirty();
        }
        m_inspectorWidget->setTrack(trackRef.get());
        syncRealtimeMidiLayersToAudio();
    });
    connect(m_inspectorWidget, &InspectorWidget::addAudioFxRequested, this, [this]() {
        ui.statusBar->showMessage("Double-click an Audio Plugin in the Browser to add it to this track.", 3500);
        if (ui.browserSearch) ui.browserSearch->setFocus();
    });
    connect(m_inspectorWidget, &InspectorWidget::returnAudioFxEnabledChanged, this,
            [this](int returnBus, int index, bool enabled) {
        if (returnBus < 0 || returnBus >= kReturnBusCount) return;
        const ReturnBusState before =
            m_mixerParameters->returnBuses()[static_cast<std::size_t>(returnBus)];
        if (index < 0 || index >= static_cast<int>(before.audioFxSlots.size()) ||
            before.audioFxSlots[static_cast<std::size_t>(index)].enabled == enabled) return;
        ReturnBusState after = before;
        after.audioFxSlots[static_cast<std::size_t>(index)].enabled = enabled;
        const bool captureUndo = m_playlistView && !m_restoringProject &&
            !m_suppressProjectUndoCapture;
        const quint64 beforeStateId = captureUndo
            ? m_playlistView->currentArrangementStateId() : 0;
        const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
        if (captureUndo) {
            m_playlistView->beginArrangementTransaction();
            m_playlistView->markArrangementTransactionChanged();
        }
        m_mixerParameters->setReturnBus(returnBus, after);
        if (captureUndo) {
            commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
                [this, returnBus, before]() {
                    m_mixerParameters->setReturnBus(returnBus, before);
                    syncRealtimeMidiLayersToAudio();
                },
                [this, returnBus, after]() {
                    m_mixerParameters->setReturnBus(returnBus, after);
                    syncRealtimeMidiLayersToAudio();
                });
        } else {
            setProjectDirty();
        }
        syncRealtimeMidiLayersToAudio();
    });
    connect(m_inspectorWidget, &InspectorWidget::returnAudioFxReloadRequested, this,
            [this](int returnBus, int index) {
        if (!m_pluginHost || returnBus < 0 || returnBus >= kReturnBusCount) return;
        const ReturnBusState& state =
            m_mixerParameters->returnBuses()[static_cast<std::size_t>(returnBus)];
        if (index < 0 || index >= static_cast<int>(state.audioFxSlots.size())) return;
        finishAutomationWrite();
        ui.statusBar->showMessage("Reloading return audio effect...", 2000);
        syncRealtimeMidiLayersToAudio();
        ui.statusBar->showMessage(m_pluginHost->isAudioFxLoaded(
            VeraCoreReturnAudioFxTrackId(returnBus), index)
            ? "Return audio effect DSP reloaded."
            : "Return audio effect reload failed; slot remains bypassed.", 5000);
    });
    connect(m_inspectorWidget, &InspectorWidget::removeReturnAudioFxRequested, this,
            [this](int returnBus, int index) {
        if (returnBus < 0 || returnBus >= kReturnBusCount) return;
        finishAutomationWrite();
        ReturnBusState before =
            m_mixerParameters->returnBuses()[static_cast<std::size_t>(returnBus)];
        if (index < 0 || index >= static_cast<int>(before.audioFxSlots.size())) return;
        if (m_pluginHost) {
            for (int slotIndex = index;
                 slotIndex < static_cast<int>(before.audioFxSlots.size()); ++slotIndex) {
                const QByteArray state = m_pluginHost->captureAudioFxState(
                    VeraCoreReturnAudioFxTrackId(returnBus), slotIndex);
                if (!state.isEmpty())
                    before.audioFxSlots[static_cast<std::size_t>(slotIndex)].pluginState = state;
            }
        }
        ReturnBusState after = before;
        after.audioFxSlots.erase(after.audioFxSlots.begin() + index);
        const bool captureUndo = m_playlistView && !m_restoringProject &&
            !m_suppressProjectUndoCapture;
        const quint64 beforeStateId = captureUndo
            ? m_playlistView->currentArrangementStateId() : 0;
        const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
        if (captureUndo) {
            m_playlistView->beginArrangementTransaction();
            m_playlistView->markArrangementTransactionChanged();
        }
        m_mixerParameters->setReturnBus(returnBus, after);
        if (captureUndo) {
            commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
                [this, returnBus, before]() {
                    m_mixerParameters->setReturnBus(returnBus, before);
                    syncRealtimeMidiLayersToAudio();
                },
                [this, returnBus, after]() {
                    m_mixerParameters->setReturnBus(returnBus, after);
                    syncRealtimeMidiLayersToAudio();
                });
        } else {
            setProjectDirty();
        }
        syncRealtimeMidiLayersToAudio();
    });
    connect(m_inspectorWidget, &InspectorWidget::addReturnAudioFxRequested, this,
            [this](int returnBus) {
        if (returnBus < 0 || returnBus >= kReturnBusCount) return;
        m_inspectorWidget->showReturnFxBus(returnBus);
        ui.statusBar->showMessage(QString("Double-click an Audio Plugin to add it to Return %1.")
            .arg(QChar('A' + returnBus)), 3500);
        if (ui.browserSearch) ui.browserSearch->setFocus();
    });
    connect(m_inspectorWidget, &InspectorWidget::audioClipSyncChanged, this, [this](double sourceTempo, double pitchShift) {
        if (!m_playlistView) return;
        PlaylistView::AudioClipInfo clip;
        double musicalLengthBeats = -1.0;
        AudioTempoAnalysis analysis;
        if (m_playlistView->selectedAudioClipInfo(&clip)) {
            double durationSeconds = clip.sourceDurationSeconds;
            if (durationSeconds <= 0.0) {
                const AudioFileProbeResult probe = AudioFileDecoder::probeAudio(clip.sourcePath);
                durationSeconds = probe.success ? probe.durationSeconds() : 0.0;
            }
            if (durationSeconds > 0.0) {
                const double projectTempo = m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm;
                const int numerator = m_transportWidget ? m_transportWidget->signatureNumerator() : 4;
                const int denominator = m_transportWidget ? m_transportWidget->signatureDenominator() : 4;
                analysis.timeSignatureNumerator = numerator;
                analysis.timeSignatureDenominator = denominator;
                if (!std::isfinite(sourceTempo) || sourceTempo <= 0.0) {
                    const DecodedAudioFile* decoded = cachedDecodedAudio(clip.sourcePath);
                    analysis = decoded
                        ? analyzeAudioTempo(*decoded, projectTempo, numerator, denominator)
                        : AudioTempoAnalysis{};
                    if (analysis.tempoBpm <= 0.0) {
                        std::vector<double> transientSeconds = std::move(analysis.transientSeconds);
                        analysis = estimateAudioTempoFromDuration(
                            durationSeconds, projectTempo, numerator, denominator);
                        analysis.transientSeconds = std::move(transientSeconds);
                    }
                    sourceTempo = analysis.tempoBpm;
                }
                if (clip.syncMode == 0) {
                    const double pitchRatio = std::pow(2.0, qBound(-48.0, pitchShift, 48.0) / 12.0);
                    musicalLengthBeats = durationSeconds * projectTempo / (60.0 * pitchRatio);
                } else if (clip.syncMode == 1 && sourceTempo > 0.0) {
                    musicalLengthBeats = durationSeconds * sourceTempo / 60.0;
                }
            }
        }
        const bool detected = analysis.source != AudioTempoSource::Unknown;
        m_playlistView->updateSelectedAudioClipSync(
            sourceTempo, pitchShift, musicalLengthBeats, analysis.confidence,
            analysis.timeSignatureNumerator, analysis.timeSignatureDenominator, detected);
        if (!analysis.transientSeconds.empty())
            m_playlistView->updateSelectedAudioClipTransients(analysis.transientSeconds);
    });
    connect(m_inspectorWidget, &InspectorWidget::audioClipSyncModeChanged, this, [this](int mode, double bars) {
        if (!m_playlistView) return;
        m_playlistView->updateSelectedAudioClipSyncMode(mode, bars);
        m_playlistView->updateAudioClipLengthsForTempo(m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm);
    });
    connect(m_inspectorWidget, &InspectorWidget::audioClipProcessingChanged, this,
            [this](float gain, float pan, bool normalize, bool reverse, bool reversePolarity) {
        if (!m_playlistView) return;
        m_playlistView->updateSelectedAudioClipProcessing(
            gain, pan, normalize, reverse, reversePolarity);
    });
    connect(m_inspectorWidget, &InspectorWidget::audioClipFadesChanged, this,
            [this](double fadeInBeats, double fadeOutBeats) {
        if (m_playlistView)
            m_playlistView->updateSelectedAudioClipFades(fadeInBeats, fadeOutBeats);
    });
    connect(m_inspectorWidget, &InspectorWidget::audioClipSourceRangeChanged, this,
            [this](double sourceInBeats, double sourceOutBeats) {
        if (m_playlistView)
            m_playlistView->updateSelectedAudioClipSourceRange(sourceInBeats, sourceOutBeats);
    });
    connect(m_inspectorWidget, &InspectorWidget::audioClipRelinkRequested, this, [this]() {
        if (!m_playlistView) return;
        PlaylistView::AudioClipInfo clip;
        if (!m_playlistView->selectedAudioClipInfo(&clip)) return;
        const QFileInfo oldSource(clip.sourcePath);
        const QString startDirectory = oldSource.absolutePath().isEmpty()
            ? (m_projectPath.isEmpty() ? QString{} : QFileInfo(m_projectPath).absolutePath())
            : oldSource.absolutePath();
        const QString replacement = QFileDialog::getOpenFileName(
            this, QStringLiteral("Locate Missing Audio"), startDirectory,
            QStringLiteral("Audio files (*.wav *.wave *.aif *.aiff *.flac *.ogg *.oga *.mp3 *.wv);;All files (*.*)"));
        if (replacement.isEmpty()) return;
        const AudioFileProbeResult probe = AudioFileDecoder::probeAudio(replacement);
        if (!probe.success) {
            QMessageBox::warning(this, QStringLiteral("Relink Media"),
                probe.message.isEmpty() ? QStringLiteral("The selected file is not readable audio.")
                                        : probe.message);
            return;
        }
        const int updated = m_playlistView->relinkSelectedAudioContent(
            QFileInfo(replacement).absoluteFilePath(), probe.durationSeconds());
        if (updated <= 0) return;
        m_audioDecodeCache.clear();
        m_audioSyncDecodeCache.clear();
        m_missingMediaCount = m_playlistView->refreshAudioMediaStatus();
        syncAudioTimelineToAudio();
        ui.statusBar->showMessage(QString("Relinked %1 clip instance(s) to %2.")
            .arg(updated).arg(QFileInfo(replacement).fileName()), 6000);
    });
    connect(m_inspectorWidget, &InspectorWidget::araProcessingAlgorithmChanged,
            this, [this](int algorithmIndex) {
        if (!m_pluginHost ||
            !m_pluginHost->setSelectedAraProcessingAlgorithm(algorithmIndex)) {
            ui.statusBar->showMessage(
                "ARA processing algorithm change was rejected by the plug-in.", 4000);
            refreshSelectedAraProcessingAlgorithms();
            return;
        }
        m_araDocumentDirty = true;
        setProjectDirty();
        ui.statusBar->showMessage(
            "ARA processing algorithm changed; source reanalysis requested.", 4000);
    });
    ui.actionUndo->setShortcut(QKeySequence(QKeySequence::Undo));
    ui.actionRedo->setShortcut(QKeySequence(QKeySequence::Redo));
    ui.actionUndo->setEnabled(false);
    ui.actionRedo->setEnabled(false);
    connect(ui.actionUndo, &QAction::triggered, this, [this]() {
        performGlobalUndo();
    });
    connect(ui.actionRedo, &QAction::triggered, this, [this]() {
        performGlobalRedo();
    });
    m_commandRegistry->registerAction(QStringLiteral("edit.undo"), ui.actionUndo,
        QStringLiteral("Edit"), QStringLiteral("Undo the last project command"));
    m_commandRegistry->registerAction(QStringLiteral("edit.redo"), ui.actionRedo,
        QStringLiteral("Edit"), QStringLiteral("Redo the last project command"));
    QAction* makeUniqueAction = new QAction("Make Unique", this);
    makeUniqueAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_U));
    ui.menuEdit->addAction(makeUniqueAction);
    connect(makeUniqueAction, &QAction::triggered, this, &VeraCoreQT::onMakeSelectedClipUnique);
    makeUniqueAction->setEnabled(false);
    m_commandRegistry->registerAction(QStringLiteral("clip.make-unique"), makeUniqueAction,
        QStringLiteral("Clip"), QStringLiteral("Detach selected clip content from linked instances"));
    QAction* duplicateClipAction = new QAction("Duplicate Clip", this);
    duplicateClipAction->setShortcuts({
        QKeySequence(Qt::CTRL | Qt::Key_B),
        QKeySequence(Qt::CTRL | Qt::Key_D)
    });
    ui.menuEdit->addAction(duplicateClipAction);
    connect(duplicateClipAction, &QAction::triggered, this, [this]() {
        if (!m_playlistView || !m_playlistView->duplicateSelectedClip()) {
            ui.statusBar->showMessage("Select a MIDI or audio clip to duplicate.", 3000);
            return;
        }
        syncMidiTimelineToAudio();
        syncAudioTimelineToAudio();
        ui.statusBar->showMessage("Created linked clip instance. Use Make Unique to detach it.", 3000);
    });
    duplicateClipAction->setEnabled(false);
    m_commandRegistry->registerAction(QStringLiteral("clip.duplicate-linked"), duplicateClipAction,
        QStringLiteral("Clip"), QStringLiteral("Duplicate selected clips as linked instances"));
    connect(m_selectionService, &SelectionService::clipsChanged, this, [this]() {
        const bool hasClips = m_selectionService && !m_selectionService->clipIds().isEmpty();
        m_commandRegistry->setEnabled(QStringLiteral("clip.make-unique"), hasClips);
        m_commandRegistry->setEnabled(QStringLiteral("clip.duplicate-linked"), hasClips);
    });

    // Browser column left, arrangement gets the remaining space.
    ui.mainSplitter->setStretchFactor(0, 0);
    ui.mainSplitter->setStretchFactor(1, 1);
    ui.mainSplitter->setChildrenCollapsible(false);
    ui.browserPanel->setMinimumWidth(150);
    ui.browserPanel->setMaximumWidth(520);
    QList<int> sizes;
    sizes << 240 << 1040;
    ui.mainSplitter->setSizes(sizes);
    m_lastMainSplitterSizes = sizes;

    // Arrangement above, inspector strip below.
    ui.centerSplitter->setStretchFactor(0, 1);
    ui.centerSplitter->setStretchFactor(1, 0);
    ui.centerSplitter->setChildrenCollapsible(false);
    QList<int> centerSizes;
    centerSizes << 540 << 170;
    ui.centerSplitter->setSizes(centerSizes);
    m_lastCenterSplitterSizes = centerSizes;

    ui.playlistSplitter->setChildrenCollapsible(false);
    QList<int> playlistSizes;
    playlistSizes << 214 << 826;
    ui.playlistSplitter->setSizes(playlistSizes);
    m_lastPlaylistSplitterSizes = playlistSizes;

    // FL-style instant browser filtering.
    connect(ui.browserSearch, &QLineEdit::textChanged,
            this, &VeraCoreQT::onBrowserSearchChanged);
    ui.browserTree->setIndentation(12);
    ui.browserTree->setUniformRowHeights(true);
    ui.browserTree->setAnimated(false);
    ui.browserTree->setTextElideMode(Qt::ElideMiddle);
    ui.browserTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui.browserTree->header()->setStretchLastSection(true);
    connect(ui.browserTree, &QTreeWidget::itemDoubleClicked,
            this, &VeraCoreQT::onBrowserItemActivated);

    m_transportWidget = new TransportWidget(this);
    ui.verticalLayout->insertWidget(0, m_transportWidget);
    m_playbackTimer = new QTimer(this);
    m_playbackTimer->setInterval(30);
    m_playbackTimer->setTimerType(Qt::CoarseTimer);
    m_araControlTimer = new QTimer(this);
    m_araControlTimer->setInterval(50);
    m_araControlTimer->setTimerType(Qt::CoarseTimer);
    connect(m_transportWidget, &TransportWidget::playRequested, this, &VeraCoreQT::onTransportPlay);
    connect(m_transportWidget, &TransportWidget::pauseRequested, this, &VeraCoreQT::onTransportPause);
    connect(m_transportWidget, &TransportWidget::stopRequested, this, &VeraCoreQT::onTransportStop);
    connect(m_transportWidget, &TransportWidget::audioHealthResetRequested, this, [this]() {
        if (audioGeneral) audioGeneral->audioCore.ResetAudioHealthCounters();
        if (m_transportWidget) m_transportWidget->setAudioHealth(0, 0.0, 0, 0, 0, 0, 0);
    });
    connect(m_transportWidget, &TransportWidget::audioRecoveryRequested, this, [this]() {
        if (!audioGeneral) return;
        audioGeneral->audioCore.CloseStream();
        quiesceExternalAudioForDeviceChange();
        const PaError result = audioGeneral->RestartAudioFromSettings();
        const AudioDeviceStatus status = audioGeneral->GetAudioDeviceStatus();
        resumeAudioAfterDeviceChange(status.active);
        if (result != paNoError || !status.active) {
            ui.statusBar->showMessage(status.message, 6000);
            return;
        }
        ui.statusBar->showMessage("Audio output reconnected.", 3500);
    });
    connect(m_playbackTimer, &QTimer::timeout, this, &VeraCoreQT::onPlaybackTick);
    connect(m_araControlTimer, &QTimer::timeout, this, &VeraCoreQT::processAraHostRequests);
    m_playbackTimer->start(); // Continuous master/input/plugin meter polling.
    m_araControlTimer->start();

    QAction* toggleTransportAction = new QAction("Play/Pause\tSpace", this);
    toggleTransportAction->setShortcut(QKeySequence(Qt::Key_Space));
    toggleTransportAction->setStatusTip("Start or pause playback");
    connect(toggleTransportAction, &QAction::triggered, this, [this]() {
        if (m_isPlaying) onTransportPause(); else onTransportPlay();
    });
    QAction* rewindTransportAction = new QAction("Return to Start\tHome", this);
    rewindTransportAction->setShortcut(QKeySequence(Qt::Key_Home));
    rewindTransportAction->setStatusTip("Move playhead to arrangement start");
    connect(rewindTransportAction, &QAction::triggered, this, [this]() {
        seekPlayhead(0.0);
    });
    QAction* punchTransportAction = new QAction("Punch In/Out\tCtrl+Shift+P", this);
    punchTransportAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")));
    punchTransportAction->setStatusTip("Toggle punch recording using the arrangement time selection");
    connect(punchTransportAction, &QAction::triggered, this, [this]() {
        if (m_transportWidget) m_transportWidget->togglePunch();
    });
    QAction* captureMidiAction = new QAction(
        "Capture Retrospective MIDI\tCtrl+Shift+C", this);
    captureMidiAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    captureMidiAction->setStatusTip(
        "Insert the latest unrecorded MIDI performance into the selected clip");
    connect(captureMidiAction, &QAction::triggered, this,
            [this]() { captureRetrospectiveMidi(); });
    m_midiStepInputAction = new QAction("MIDI Step Input", this);
    m_midiStepInputAction->setCheckable(true);
    m_midiStepInputAction->setStatusTip(
        "Enter notes and chords at the playhead using the current snap step");
    connect(m_midiStepInputAction, &QAction::toggled, this, [this](bool enabled) {
        if (enabled && (m_isRecordingMidi || m_isRecordingAudio)) {
            QSignalBlocker blocker(m_midiStepInputAction);
            m_midiStepInputAction->setChecked(false);
            ui.statusBar->showMessage("Stop recording before enabling MIDI step input.", 3000);
            return;
        }
        m_midiStepInputEnabled = enabled;
        if (!enabled) clearStepMidiChord();
        ui.statusBar->showMessage(enabled
            ? "MIDI step input enabled; play notes or chords."
            : "MIDI step input disabled.", 2500);
    });
    QAction* stepInputRestAction = new QAction("Step Input: Insert Rest", this);
    stepInputRestAction->setStatusTip("Advance the step-input cursor without adding notes");
    connect(stepInputRestAction, &QAction::triggered, this,
        [this]() { moveStepInputCursor(1); });
    QAction* stepInputBackAction = new QAction("Step Input: Move Back", this);
    stepInputBackAction->setStatusTip("Move the step-input cursor back by one snap step");
    connect(stepInputBackAction, &QAction::triggered, this,
        [this]() { moveStepInputCursor(-1); });
    QAction* stepInputTieAction = new QAction("Step Input: Tie Previous Notes", this);
    stepInputTieAction->setStatusTip(
        "Extend notes ending at the cursor by one snap step and advance");
    connect(stepInputTieAction, &QAction::triggered, this,
        [this]() { tieStepInputNotes(); });
    constexpr std::array<int, 6> stepInputGateValues{{25, 50, 75, 100, 150, 200}};
    constexpr std::array<int, 5> stepInputVelocityValues{{0, 32, 64, 96, 127}};
    const std::array<std::pair<int, QString>, 2> stepInputModes{{
        {0, QStringLiteral("Merge")},
        {1, QStringLiteral("Replace Step")}
    }};
    QSettings stepInputSettings;
    const auto restorePreset = [&stepInputSettings](const QString& key,
                                                     const auto& values,
                                                     int fallback) {
        const int candidate = stepInputSettings.value(key, fallback).toInt();
        return std::find(values.begin(), values.end(), candidate) != values.end()
            ? candidate : fallback;
    };
    m_stepMidiGatePercent = restorePreset(
        QStringLiteral("MidiStepInput/GatePercent"), stepInputGateValues, 100);
    m_stepMidiVelocityOverride = restorePreset(
        QStringLiteral("MidiStepInput/VelocityOverride"), stepInputVelocityValues, 0);
    m_stepMidiInsertMode = qBound(0, stepInputSettings.value(
        QStringLiteral("MidiStepInput/InsertMode"), 0).toInt(), 1);

    auto* stepInputGateGroup = new QActionGroup(this);
    stepInputGateGroup->setExclusive(true);
    QList<QAction*> stepInputGateActions;
    for (const int gatePercent : stepInputGateValues) {
        QAction* gateAction = new QAction(QString("Step Input Gate: %1%").arg(gatePercent), this);
        gateAction->setCheckable(true);
        gateAction->setChecked(gatePercent == m_stepMidiGatePercent);
        gateAction->setStatusTip(QString(
            "Set entered-note duration to %1% of the cursor advance").arg(gatePercent));
        stepInputGateGroup->addAction(gateAction);
        stepInputGateActions.push_back(gateAction);
        connect(gateAction, &QAction::triggered, this, [this, gatePercent]() {
            m_stepMidiGatePercent = gatePercent;
            QSettings().setValue(QStringLiteral("MidiStepInput/GatePercent"), gatePercent);
            ui.statusBar->showMessage(
                QString("MIDI step-input gate set to %1%.").arg(gatePercent), 2200);
        });
    }
    auto* stepInputVelocityGroup = new QActionGroup(this);
    stepInputVelocityGroup->setExclusive(true);
    QList<QAction*> stepInputVelocityActions;
    for (const int velocity : stepInputVelocityValues) {
        const QString label = velocity == 0
            ? QStringLiteral("Performed") : QString::number(velocity);
        QAction* velocityAction = new QAction(
            QString("Step Input Velocity: %1").arg(label), this);
        velocityAction->setCheckable(true);
        velocityAction->setChecked(velocity == m_stepMidiVelocityOverride);
        velocityAction->setStatusTip(velocity == 0
            ? QStringLiteral("Preserve velocity played on the MIDI controller")
            : QString("Enter every step-input note at velocity %1").arg(velocity));
        stepInputVelocityGroup->addAction(velocityAction);
        stepInputVelocityActions.push_back(velocityAction);
        connect(velocityAction, &QAction::triggered, this, [this, velocity, label]() {
            m_stepMidiVelocityOverride = velocity;
            QSettings().setValue(
                QStringLiteral("MidiStepInput/VelocityOverride"), velocity);
            ui.statusBar->showMessage(
                QString("MIDI step-input velocity: %1.").arg(label), 2200);
        });
    }
    auto* stepInputModeGroup = new QActionGroup(this);
    stepInputModeGroup->setExclusive(true);
    QList<QAction*> stepInputModeActions;
    for (const auto& mode : stepInputModes) {
        QAction* modeAction = new QAction(
            QString("Step Input Mode: %1").arg(mode.second), this);
        modeAction->setCheckable(true);
        modeAction->setChecked(mode.first == m_stepMidiInsertMode);
        modeAction->setStatusTip(mode.first == 0
            ? QStringLiteral("Add notes without removing existing notes")
            : QStringLiteral("Replace notes beginning inside the current snap step"));
        stepInputModeGroup->addAction(modeAction);
        stepInputModeActions.push_back(modeAction);
        connect(modeAction, &QAction::triggered, this, [this, mode]() {
            m_stepMidiInsertMode = mode.first;
            QSettings().setValue(QStringLiteral("MidiStepInput/InsertMode"), mode.first);
            ui.statusBar->showMessage(
                QString("MIDI step-input mode: %1.").arg(mode.second), 2200);
        });
    }
    m_commandRegistry->registerAction(QStringLiteral("transport.play-pause"), toggleTransportAction,
        QStringLiteral("Transport"), QStringLiteral("Start or pause playback"));
    m_commandRegistry->registerAction(QStringLiteral("transport.return-to-start"), rewindTransportAction,
        QStringLiteral("Transport"), QStringLiteral("Move the playhead to arrangement start"));
    m_commandRegistry->registerAction(QStringLiteral("transport.punch-in-out"), punchTransportAction,
        QStringLiteral("Transport"), QStringLiteral("Toggle punch recording from the time selection"));
    m_commandRegistry->registerAction(QStringLiteral("transport.capture-midi"), captureMidiAction,
        QStringLiteral("Transport"), QStringLiteral("Capture the latest unrecorded MIDI performance"));
    m_commandRegistry->registerAction(QStringLiteral("record.midi-step-input"),
        m_midiStepInputAction, QStringLiteral("Recording"),
        QStringLiteral("Toggle chord-aware MIDI step input"));
    m_commandRegistry->registerAction(QStringLiteral("record.midi-step-rest"),
        stepInputRestAction, QStringLiteral("Recording"),
        QStringLiteral("Advance MIDI step-input cursor by one rest"));
    m_commandRegistry->registerAction(QStringLiteral("record.midi-step-back"),
        stepInputBackAction, QStringLiteral("Recording"),
        QStringLiteral("Move MIDI step-input cursor back one step"));
    m_commandRegistry->registerAction(QStringLiteral("record.midi-step-tie"),
        stepInputTieAction, QStringLiteral("Recording"),
        QStringLiteral("Tie previous MIDI notes across one step"));
    for (int index = 0; index < static_cast<int>(stepInputGateValues.size()); ++index) {
        const int gatePercent = stepInputGateValues[static_cast<size_t>(index)];
        m_commandRegistry->registerAction(
            QStringLiteral("record.midi-step-gate-%1").arg(gatePercent),
            stepInputGateActions[index], QStringLiteral("Recording"),
            QString("Set MIDI step-input gate to %1%").arg(gatePercent));
    }
    for (int index = 0; index < static_cast<int>(stepInputVelocityValues.size()); ++index) {
        const int velocity = stepInputVelocityValues[static_cast<size_t>(index)];
        const QString token = velocity == 0
            ? QStringLiteral("performed") : QString::number(velocity);
        m_commandRegistry->registerAction(
            QStringLiteral("record.midi-step-velocity-%1").arg(token),
            stepInputVelocityActions[index], QStringLiteral("Recording"),
            velocity == 0 ? QStringLiteral("Preserve performed step-input velocity")
                          : QString("Set MIDI step-input velocity to %1").arg(velocity));
    }
    for (int index = 0; index < static_cast<int>(stepInputModes.size()); ++index) {
        const auto& mode = stepInputModes[static_cast<size_t>(index)];
        m_commandRegistry->registerAction(
            mode.first == 0 ? QStringLiteral("record.midi-step-mode-merge")
                            : QStringLiteral("record.midi-step-mode-replace"),
            stepInputModeActions[index], QStringLiteral("Recording"),
            QString("Set MIDI step-input mode to %1").arg(mode.second));
    }
    QMenu* transportMenu = new QMenu("Transport", ui.menuBar);
    ui.menuBar->insertMenu(ui.menuOptions->menuAction(), transportMenu);
    transportMenu->addAction(toggleTransportAction);
    transportMenu->addAction(rewindTransportAction);
    transportMenu->addAction(punchTransportAction);
    transportMenu->addAction(captureMidiAction);
    transportMenu->addAction(m_midiStepInputAction);
    transportMenu->addAction(stepInputRestAction);
    transportMenu->addAction(stepInputBackAction);
    transportMenu->addAction(stepInputTieAction);
    QMenu* stepInputGateMenu = transportMenu->addMenu(QStringLiteral("Step Input Gate"));
    stepInputGateMenu->addActions(stepInputGateActions);
    QMenu* stepInputVelocityMenu = transportMenu->addMenu(
        QStringLiteral("Step Input Velocity"));
    stepInputVelocityMenu->addActions(stepInputVelocityActions);
    QMenu* stepInputModeMenu = transportMenu->addMenu(QStringLiteral("Step Input Mode"));
    stepInputModeMenu->addActions(stepInputModeActions);
    QMenu* midiRecordModeMenu = transportMenu->addMenu(QStringLiteral("MIDI Record Mode"));
    auto* midiRecordModeGroup = new QActionGroup(this);
    midiRecordModeGroup->setExclusive(true);
    const std::array<std::pair<int, QString>, 2> midiRecordModes{{
        {0, QStringLiteral("Overdub")},
        {1, QStringLiteral("Replace")}
    }};
    for (const auto& mode : midiRecordModes) {
        QAction* action = midiRecordModeMenu->addAction(mode.second);
        action->setCheckable(true);
        action->setChecked(m_midiRecordMode == mode.first);
        midiRecordModeGroup->addAction(action);
        m_midiRecordModeActions.push_back({mode.first, action});
        connect(action, &QAction::triggered, this, [this, mode](bool checked) {
            if (!checked) return;
            if (m_isRecordingMidi || m_isRecordingAudio) {
                for (const auto& entry : m_midiRecordModeActions)
                    entry.second->setChecked(entry.first == m_midiRecordMode);
                ui.statusBar->showMessage("Stop recording before changing MIDI record mode.", 3000);
                return;
            }
            if (m_midiRecordMode == mode.first) return;
            m_midiRecordMode = mode.first;
            setProjectDirty();
            ui.statusBar->showMessage(QString("MIDI record mode: %1").arg(mode.second), 2200);
        });
        m_commandRegistry->registerAction(
            mode.first == 0 ? QStringLiteral("record.midi-overdub")
                            : QStringLiteral("record.midi-replace"),
            action, QStringLiteral("Recording"),
            QString("Use MIDI %1 recording mode").arg(mode.second.toLower()));
    }
    QMenu* midiRecordQuantizeMenu = transportMenu->addMenu(
        QStringLiteral("MIDI Record Quantization"));
    auto* midiRecordQuantizeGroup = new QActionGroup(this);
    midiRecordQuantizeGroup->setExclusive(true);
    const std::array<std::pair<double, QString>, 7> midiRecordQuantizeValues{{
        {0.0, QStringLiteral("Off")},
        {1.0, QStringLiteral("1/4")},
        {0.5, QStringLiteral("1/8")},
        {1.0 / 3.0, QStringLiteral("1/8 Triplet")},
        {0.25, QStringLiteral("1/16")},
        {1.0 / 6.0, QStringLiteral("1/16 Triplet")},
        {0.125, QStringLiteral("1/32")}
    }};
    int quantizeCommandIndex = 0;
    for (const auto& value : midiRecordQuantizeValues) {
        QAction* action = midiRecordQuantizeMenu->addAction(value.second);
        action->setCheckable(true);
        action->setChecked(std::fabs(m_midiRecordQuantizeBeats - value.first) < 1.0e-9);
        midiRecordQuantizeGroup->addAction(action);
        m_midiRecordQuantizeActions.push_back({value.first, action});
        connect(action, &QAction::triggered, this, [this, value](bool checked) {
            if (!checked) return;
            if (m_isRecordingMidi || m_isRecordingAudio) {
                for (const auto& entry : m_midiRecordQuantizeActions)
                    entry.second->setChecked(
                        std::fabs(entry.first - m_midiRecordQuantizeBeats) < 1.0e-9);
                ui.statusBar->showMessage(
                    "Stop recording before changing MIDI record quantization.", 3000);
                return;
            }
            if (std::fabs(m_midiRecordQuantizeBeats - value.first) < 1.0e-9) return;
            m_midiRecordQuantizeBeats = value.first;
            setProjectDirty();
            ui.statusBar->showMessage(
                QString("MIDI record quantization: %1").arg(value.second), 2200);
        });
        m_commandRegistry->registerAction(
            QString("record.midi-quantize-%1").arg(quantizeCommandIndex++), action,
            QStringLiteral("Recording"),
            QString("Set MIDI record quantization to %1").arg(value.second));
    }
    QMenu* panLawMenu = ui.menuOptions->addMenu(QStringLiteral("Pan Law"));
    auto* panLawGroup = new QActionGroup(this);
    panLawGroup->setExclusive(true);
    const std::array<std::pair<double, QString>, 4> panLaws{{
        {0.0, QStringLiteral("0 dB (constant center)")},
        {-3.0, QStringLiteral("-3 dB (equal power)")},
        {-4.5, QStringLiteral("-4.5 dB")},
        {-6.0, QStringLiteral("-6 dB")}
    }};
    for (const auto& law : panLaws) {
        QAction* action = panLawMenu->addAction(law.second);
        action->setCheckable(true);
        panLawGroup->addAction(action);
        m_panLawActions.push_back({law.first, action});
        QString commandSuffix = QString::number(std::fabs(law.first), 'f',
            std::fabs(law.first - std::round(law.first)) < 0.001 ? 0 : 1);
        commandSuffix.replace('.', '_');
        m_commandRegistry->registerAction(
            QStringLiteral("mixer.pan-law.%1db").arg(
                law.first < 0.0 ? QStringLiteral("minus") + commandSuffix : commandSuffix),
            action, QStringLiteral("Mixer"),
            QStringLiteral("Set mono center attenuation to %1 dB").arg(law.first, 0, 'g', 3));
        connect(action, &QAction::triggered, this, [this, value = law.first](bool checked) {
            if (!checked) return;
            const double next = canonicalPanLawDb(value);
            if (std::fabs(next - m_projectPanLawDb) <= 1.0e-9) return;
            const double previous = m_projectPanLawDb;
            const bool captureUndo = m_playlistView && !m_restoringProject &&
                !m_suppressProjectUndoCapture;
            const quint64 beforeStateId = captureUndo
                ? m_playlistView->currentArrangementStateId() : 0;
            const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
            if (captureUndo) {
                m_playlistView->beginArrangementTransaction();
                m_playlistView->markArrangementTransactionChanged();
            }
            applyProjectPanLawDb(next);
            if (captureUndo) {
                commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore,
                    QStringLiteral("mixer-pan-law"),
                    [this, previous]() { applyProjectPanLawDb(previous); },
                    [this, next]() { applyProjectPanLawDb(next); });
            } else {
                setProjectDirty();
            }
            ui.statusBar->showMessage(
                QStringLiteral("Mono pan law: %1 dB").arg(next, 0, 'g', 3), 2500);
        });
    }
    applyProjectPanLawDb(m_projectPanLawDb);
    QMenu* speakerLayoutMenu = ui.menuOptions->addMenu(QStringLiteral("Spatial Audio Layout"));
    auto* speakerLayoutGroup = new QActionGroup(this);
    speakerLayoutGroup->setExclusive(true);
    const std::array<std::pair<VeraCoreSpeakerLayout, QString>, 5> speakerLayouts{{
        {VeraCoreSpeakerLayout::Stereo, QStringLiteral("Stereo 2.0")},
        {VeraCoreSpeakerLayout::Surround51, QStringLiteral("Surround 5.1")},
        {VeraCoreSpeakerLayout::Surround71, QStringLiteral("Surround 7.1")},
        {VeraCoreSpeakerLayout::Atmos712, QStringLiteral("Dolby Atmos bed 7.1.2")},
        {VeraCoreSpeakerLayout::Atmos714, QStringLiteral("Dolby Atmos monitoring 7.1.4")}
    }};
    int layoutCommandIndex = 0;
    for (const auto& layout : speakerLayouts) {
        QAction* action = speakerLayoutMenu->addAction(layout.second);
        action->setCheckable(true);
        speakerLayoutGroup->addAction(action);
        m_speakerLayoutActions.push_back({layout.first, action});
        m_commandRegistry->registerAction(
            QStringLiteral("mixer.spatial-layout-%1").arg(layoutCommandIndex++), action,
            QStringLiteral("Mixer"), QStringLiteral("Set project output to %1").arg(layout.second));
        connect(action, &QAction::triggered, this,
                [this, next = layout.first, name = layout.second](bool checked) {
            if (!checked || next == m_projectSpeakerLayout) return;
            const VeraCoreSpeakerLayout previous = m_projectSpeakerLayout;
            const bool captureUndo = m_playlistView && !m_restoringProject &&
                !m_suppressProjectUndoCapture;
            const quint64 beforeStateId = captureUndo
                ? m_playlistView->currentArrangementStateId() : 0;
            const bool dirtyBefore = m_nonArrangementDirty;
            if (captureUndo) {
                m_playlistView->beginArrangementTransaction();
                m_playlistView->markArrangementTransactionChanged();
            }
            applyProjectSpeakerLayout(next);
            if (captureUndo) {
                commitProjectUndoStep(beforeStateId, dirtyBefore,
                    QStringLiteral("mixer-spatial-layout"),
                    [this, previous]() { applyProjectSpeakerLayout(previous); },
                    [this, next]() { applyProjectSpeakerLayout(next); });
            } else {
                setProjectDirty();
            }
            ui.statusBar->showMessage(
                QStringLiteral("Project spatial layout: %1").arg(name), 2500);
        });
    }
    applyProjectSpeakerLayout(m_projectSpeakerLayout);
    connect(m_transportWidget, &TransportWidget::tempoChanged, this, [this](double bpm) {
        const bool captureUndo = m_playlistView && !m_restoringProject &&
            !m_suppressProjectUndoCapture;
        const quint64 beforeStateId = captureUndo
            ? m_playlistView->currentArrangementStateId() : 0;
        const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
        const ProjectTimingState timingBefore = captureProjectTimingState();
        if (captureUndo) {
            m_playlistView->beginArrangementTransaction();
            m_playlistView->markArrangementTransactionChanged();
        } else {
            setProjectDirty();
        }
        if (!m_restoringProject) {
            m_tempoMap = normalizedTempoMap({{0.0, bpm}}, bpm);
            syncTempoMapToAudio();
        }
        syncProjectMetadataToMidiDocuments();
        if (m_playlistView) {
            m_playlistView->updateAudioClipLengthsForTempo(bpm);
        }
        syncAudioTimelineToAudio();
        if (audioGeneral && m_isPlaying) {
            const double audioPlayheadBeat = audioGeneral->audioCore.GetTransportPlayheadBeat();
            if (std::isfinite(audioPlayheadBeat) && audioPlayheadBeat >= 0.0) {
                m_playheadBeat = audioPlayheadBeat;
            }
            audioGeneral->audioCore.SetTransportState(true, bpm, m_playheadBeat);
        }
        syncPluginTransportToHost();
        if (captureUndo) {
            const ProjectTimingState timingAfter = captureProjectTimingState();
            commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore,
                QStringLiteral("transport-tempo"),
                [this, timingBefore]() { applyProjectTimingState(timingBefore); },
                [this, timingAfter]() { applyProjectTimingState(timingAfter); });
        }
    });
    connect(m_transportWidget, &TransportWidget::timeSignatureChanged, this, [this](int numerator, int denominator) {
        const bool captureUndo = m_playlistView && !m_restoringProject &&
            !m_suppressProjectUndoCapture;
        const quint64 beforeStateId = captureUndo
            ? m_playlistView->currentArrangementStateId() : 0;
        const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
        const ProjectTimingState timingBefore = captureProjectTimingState();
        const double snapBefore = m_projectSnapBeats;
        if (captureUndo) {
            m_playlistView->beginArrangementTransaction();
            m_playlistView->markArrangementTransactionChanged();
        } else {
            setProjectDirty();
        }
        if (!m_restoringProject) {
            m_timeSignatureMap = normalizedTimeSignatureMap(
                {{0.0, numerator, denominator}}, numerator, denominator);
            syncTimeSignatureMapToAudio();
        }
        if (audioGeneral) {
            audioGeneral->audioCore.SetMetronomeState(
                m_transportWidget && m_transportWidget->metronomeEnabled(), numerator, denominator);
        }
        if (m_playlistView) {
            m_playlistView->setTimeSignature(numerator, denominator);
            syncAudioTimelineToAudio();
        }
        if (m_inspectorWidget) {
            m_inspectorWidget->setTimeSignature(numerator, denominator);
        }
        syncProjectMetadataToMidiDocuments();
        syncPluginTransportToHost();
        const double snapAfter = m_transportWidget
            ? m_transportWidget->snapBeats() : m_projectSnapBeats;
        m_projectSnapBeats = snapAfter;
        if (m_playlistView) m_playlistView->setSnapBeats(snapAfter);
        if (captureUndo) {
            const ProjectTimingState timingAfter = captureProjectTimingState();
            auto applySignatureState = [this](const ProjectTimingState& timing, double snapBeats) {
                applyProjectTimingState(timing);
                m_projectSnapBeats = snapBeats;
                if (m_transportWidget) {
                    QSignalBlocker blocker(m_transportWidget);
                    m_transportWidget->setSnapBeats(snapBeats);
                }
                if (m_playlistView) m_playlistView->setSnapBeats(snapBeats);
            };
            commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore,
                QStringLiteral("transport-signature"),
                [applySignatureState, timingBefore, snapBefore]() {
                    applySignatureState(timingBefore, snapBefore);
                },
                [applySignatureState, timingAfter, snapAfter]() {
                    applySignatureState(timingAfter, snapAfter);
                });
        }
    });
    connect(m_transportWidget, &TransportWidget::ppqChanged, this, [this](int ppq) {
        const int safePpq = std::clamp(ppq, kMinProjectPpq, kMaxProjectPpq);
        if (m_projectPpq == safePpq) return;
        const bool captureUndo = m_playlistView && !m_restoringProject &&
            !m_suppressProjectUndoCapture;
        const quint64 beforeStateId = captureUndo
            ? m_playlistView->currentArrangementStateId() : 0;
        const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
        const int previousPpq = m_projectPpq;
        if (captureUndo) {
            m_playlistView->beginArrangementTransaction();
            m_playlistView->markArrangementTransactionChanged();
        }
        m_projectPpq = safePpq;
        if (captureUndo) {
            auto applyPpq = [this](int value) {
                m_projectPpq = value;
                if (m_transportWidget) {
                    QSignalBlocker blocker(m_transportWidget);
                    m_transportWidget->setPpq(value);
                }
            };
            commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore,
                QStringLiteral("transport-ppq"),
                [applyPpq, previousPpq]() { applyPpq(previousPpq); },
                [applyPpq, safePpq]() { applyPpq(safePpq); });
        } else {
            setProjectDirty();
        }
        ui.statusBar->showMessage(QString("Project resolution: %1 PPQ").arg(safePpq), 2500);
    });
    connect(m_transportWidget, &TransportWidget::metronomeToggled, this, [this](bool enabled) {
        if (audioGeneral && m_transportWidget) {
            audioGeneral->audioCore.SetMetronomeState(
                m_countInMetronomeOverride ? true : enabled,
                m_transportWidget->signatureNumerator(),
                m_transportWidget->signatureDenominator());
        }
    });
    connect(m_transportWidget, &TransportWidget::countInBarsChanged, this, [this](int bars) {
        const int safeBars = bars == 1 || bars == 2 || bars == 4 ? bars : 0;
        if (m_isRecordingAudio || m_isRecordingMidi) {
            m_transportWidget->setCountInBars(m_countInBars);
            ui.statusBar->showMessage("Stop recording before changing count-in.", 3000);
            return;
        }
        if (m_countInBars == safeBars) return;
        m_countInBars = safeBars;
        setProjectDirty();
    });
    connect(m_transportWidget, &TransportWidget::keyScaleChanged, this, [this](int rootNote, bool minor) {
        const int safeRoot = std::clamp(rootNote, 0, 11);
        if (m_projectKeyRoot == safeRoot && m_projectMinorScale == minor) return;
        const int previousRoot = m_projectKeyRoot;
        const bool previousMinor = m_projectMinorScale;
        const bool captureUndo = m_playlistView && !m_restoringProject &&
            !m_suppressProjectUndoCapture;
        const quint64 beforeStateId = captureUndo
            ? m_playlistView->currentArrangementStateId() : 0;
        const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
        if (captureUndo) {
            m_playlistView->beginArrangementTransaction();
            m_playlistView->markArrangementTransactionChanged();
        }
        m_projectKeyRoot = safeRoot;
        m_projectMinorScale = minor;
        if (m_midiEditor) {
            m_midiEditor->setProjectScale(safeRoot, minor);
        }
        if (captureUndo) {
            auto applyKeyScale = [this](int targetRoot, bool targetMinor) {
                m_projectKeyRoot = targetRoot;
                m_projectMinorScale = targetMinor;
                if (m_transportWidget) {
                    QSignalBlocker blocker(m_transportWidget);
                    m_transportWidget->setKeyScale(targetRoot, targetMinor);
                }
                if (m_midiEditor) m_midiEditor->setProjectScale(targetRoot, targetMinor);
            };
            commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
                [applyKeyScale, previousRoot, previousMinor]() {
                    applyKeyScale(previousRoot, previousMinor);
                },
                [applyKeyScale, safeRoot, minor]() { applyKeyScale(safeRoot, minor); });
        } else {
            setProjectDirty();
        }
    });
    connect(m_transportWidget, &TransportWidget::recordToggled, this, [this](bool enabled) {
        if (!enabled) {
            stopAudioRecording(true);
            stopMidiRecording(true);
            restoreCountInMetronome();
            m_recordingCountInEndBeat = -1.0;
            m_recordingCountInComplete = true;
            m_recordingStopBeatOverride = -1.0;
            return;
        }

        if (m_midiStepInputEnabled && m_midiStepInputAction)
            m_midiStepInputAction->setChecked(false);

        if (m_punchEnabled) {
            const bool validPunch = std::isfinite(m_punchInBeat) &&
                std::isfinite(m_punchOutBeat) && m_punchInBeat >= 0.0 &&
                m_punchOutBeat - m_punchInBeat >= 0.25;
            const bool loopCompatible = !m_loopEnabled ||
                (m_punchInBeat >= m_loopStartBeat - 1.0e-9 &&
                 m_punchOutBeat <= m_loopEndBeat + 1.0e-9);
            if (!validPunch || !loopCompatible) {
                m_transportWidget->setRecording(false);
                ui.statusBar->showMessage(loopCompatible
                    ? "Punch range is invalid. Select a new time range."
                    : "Punch range must be fully inside the active loop range.", 4500);
                return;
            }
            if (m_playheadBeat >= m_punchOutBeat - 1.0e-9)
                seekPlayhead(m_punchInBeat);
        }

        QString audioError;
        if (!hasActiveAudioOutput(&audioError)) {
            m_transportWidget->setRecording(false);
            m_transportWidget->setAudioDeviceStatus(false, audioError);
            ui.statusBar->showMessage(audioError, 6000);
            return;
        }

        m_recordingStopBeatOverride = -1.0;
        m_recordingAudioTracks.clear();
        for (int i = 0; i < static_cast<int>(m_tracks.size()); ++i) {
            const auto& track = m_tracks[i];
            if (track && track->GetType() == TrackType::Audio && track->IsArmed())
                m_recordingAudioTracks.push_back(i);
        }
        if (!audioGeneral || !audioGeneral->audioCore.HasAudioInput()) m_recordingAudioTracks.clear();
        MidiClipDocument* doc = getCurrentSelectedClipDocument();
        const bool canRecordMidi = doc && doc->trackIndex >= 0 &&
            doc->trackIndex < static_cast<int>(m_tracks.size()) && m_tracks[doc->trackIndex] &&
            m_tracks[doc->trackIndex]->GetType() == TrackType::Midi && m_tracks[doc->trackIndex]->IsArmed();
        if (m_recordingAudioTracks.empty() && !canRecordMidi) {
            m_transportWidget->setRecording(false);
            ui.statusBar->showMessage("Arm an audio track, or arm a MIDI track and select its clip.", 4000);
            return;
        }

        restoreCountInMetronome();
        m_recordingCountInEndBeat = -1.0;
        m_recordingCountInComplete = true;
        const double recordStartBeat = m_punchEnabled ? m_punchInBeat : m_playheadBeat;
        const int fallbackNumerator = m_transportWidget
            ? m_transportWidget->signatureNumerator() : 4;
        const int fallbackDenominator = m_transportWidget
            ? m_transportWidget->signatureDenominator() : 4;
        const double preRollBeat = countInStartBeat(m_timeSignatureMap, recordStartBeat,
            m_countInBars, fallbackNumerator, fallbackDenominator);
        if (m_countInBars > 0 && preRollBeat < recordStartBeat - 1.0e-9) {
            m_recordingCountInEndBeat = recordStartBeat;
            m_recordingCountInComplete = false;
            seekPlayhead(preRollBeat);
            if (audioGeneral) {
                const MidiTimeSignaturePoint countInSignature = timeSignatureAtBeat(
                    m_timeSignatureMap, preRollBeat, fallbackNumerator, fallbackDenominator);
                audioGeneral->audioCore.SetMetronomeState(true,
                    countInSignature.numerator, countInSignature.denominator);
                m_countInMetronomeOverride = true;
            }
        }

        if (!m_recordingAudioTracks.empty()) {
            m_recordedAudioSamples.clear();
            m_audioRecordingOverflowed = false;
            m_audioRecordingStartBeat = m_playheadBeat;
            m_audioRecordingSampleRate = audioGeneral
                ? safeRenderSampleRate(audioGeneral->audioCore.GetStreamSampleRate(), 44100)
                : 44100;
            m_isRecordingAudio = true;
            syncAudioInputState();
        }
        if (canRecordMidi) {
            m_recordedMidiStarts.clear();
            m_midiRecordingBeforeDocument = std::make_unique<MidiClipDocument>(*doc);
            m_midiRecordingContentId = m_currentMidiClipKey;
            m_midiRecordingClipStartBeat = m_currentMidiClipStartBeat;
            m_midiRecordingClipLengthBeats = m_currentMidiClipLengthBeats;
            m_midiRecordingClipSourceOffsetBeats = m_currentMidiClipSourceOffsetBeats;
            m_midiRecordingRangeStartBeat = recordStartBeat;
            m_midiRecordingSawLoopWrap = false;
            m_midiRecordingAddedNotes.clear();
            m_midiRecordingAddedEvents.clear();
            m_midiRecordingBeforeStateId = 0;
            m_midiRecordingNonArrangementDirtyBefore = false;
            m_midiRecordingChanged = false;
            m_midiRecordingUndoActive = false;
            m_isRecordingMidi = true;
        }
        if (!m_isPlaying) {
            onTransportPlay();
        }
        syncPluginTransportToHost();
        ui.statusBar->showMessage(QString("Recording%1%2")
            .arg(m_isRecordingAudio ? " audio" : "")
            .arg(m_isRecordingMidi ? QString(" MIDI into %1").arg(doc->name) : QString()) +
            (m_punchEnabled ? QString(" (punch %1-%2)")
                .arg(m_punchInBeat, 0, 'f', 2).arg(m_punchOutBeat, 0, 'f', 2) : QString()) +
            (m_recordingCountInEndBeat >= 0.0
                ? QString(" · count-in %1 bar(s)").arg(m_countInBars) : QString()) +
            (m_isRecordingMidi
                ? QString(" · %1").arg(m_midiRecordMode == 1 ? "replace" : "overdub")
                : QString()) +
            (m_isRecordingMidi && m_midiRecordQuantizeBeats > 0.0
                ? QString(" · quantize %1 beat").arg(
                    m_midiRecordQuantizeBeats, 0, 'g', 3)
                : QString()), 3000);
    });

    // Replace the default QGraphicsView with our custom PlaylistView
    m_playlistView = new PlaylistView(this);
    int playlistIndex = ui.playlistSplitter->indexOf(ui.playlistView);
    ui.playlistView->deleteLater();
    ui.playlistSplitter->insertWidget(playlistIndex, m_playlistView);
    m_playlistView->setTimeSignature(m_transportWidget->signatureNumerator(), m_transportWidget->signatureDenominator());
    m_playlistView->setTimeSignatureMap(m_timeSignatureMap);
    m_transportWidget->setTimeSignatureMap(m_timeSignatureMap);
    m_playlistView->setSnapBeats(m_transportWidget->snapBeats());
    m_audioEditorWidget->setSnapBeats(m_transportWidget->snapBeats());
    m_inspectorWidget->setTimeSignature(m_transportWidget->signatureNumerator(), m_transportWidget->signatureDenominator());
    connect(m_playlistView, &PlaylistView::trackLaneSelected,
            this, &VeraCoreQT::onPlaylistTrackSelected);
    connect(m_playlistView, &PlaylistView::pluginAutomationEdited,
            this, [this](int trackIndex, bool audioFx, int slotIndex,
                         const PluginAutomationLane& lane) {
        if (!m_hasAutomationTarget || trackIndex != m_parameterEditorTrackIndex ||
            audioFx != m_parameterEditorAudioFx || slotIndex != m_parameterEditorSlotIndex ||
            lane.parameterId != m_automationParameterId) return;
        applyPluginAutomationLane(trackIndex, audioFx, slotIndex, lane);
    });
    connect(m_transportWidget, &TransportWidget::snapChanged, this, [this](double beats) {
        const double safeBeats = std::isfinite(beats) ? std::clamp(beats, 0.0, 128.0) : 0.25;
        if (std::fabs(m_projectSnapBeats - safeBeats) <= 1.0e-6) return;
        const double previousBeats = m_projectSnapBeats;
        const bool captureUndo = m_playlistView && !m_restoringProject &&
            !m_suppressProjectUndoCapture;
        const quint64 beforeStateId = captureUndo
            ? m_playlistView->currentArrangementStateId() : 0;
        const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
        if (captureUndo) {
            m_playlistView->beginArrangementTransaction();
            m_playlistView->markArrangementTransactionChanged();
        }
        m_projectSnapBeats = safeBeats;
        if (m_playlistView) m_playlistView->setSnapBeats(safeBeats);
        if (m_audioEditorWidget) m_audioEditorWidget->setSnapBeats(safeBeats);
        if (captureUndo) {
            auto applySnap = [this](double targetBeats) {
                m_projectSnapBeats = targetBeats;
                if (m_transportWidget) {
                    QSignalBlocker blocker(m_transportWidget);
                    m_transportWidget->setSnapBeats(targetBeats);
                }
                if (m_playlistView) m_playlistView->setSnapBeats(targetBeats);
                if (m_audioEditorWidget) m_audioEditorWidget->setSnapBeats(targetBeats);
            };
            commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
                [applySnap, previousBeats]() { applySnap(previousBeats); },
                [applySnap, safeBeats]() { applySnap(safeBeats); });
        } else {
            setProjectDirty();
        }
    });
    connect(m_playlistView, &PlaylistView::midiClipSelected,
            this, &VeraCoreQT::onMidiClipSelected);
    connect(m_playlistView, &PlaylistView::midiClipOpened,
            this, &VeraCoreQT::onMidiClipOpened);
    connect(m_playlistView, &PlaylistView::audioClipSelected,
            this, &VeraCoreQT::onAudioClipSelected);
    connect(m_audioEditorWidget, &AudioEditorWidget::warpMarkerMoveRequested,
            this, [this](int markerIndex, double targetBeat) {
        if (m_playlistView)
            m_playlistView->updateSelectedAudioClipWarpMarker(markerIndex, targetBeat);
    });
    connect(m_audioEditorWidget, &AudioEditorWidget::warpMarkerToggleRequested,
            this, [this](double sourceSeconds, double targetBeat) {
        if (m_playlistView)
            m_playlistView->toggleSelectedAudioClipWarpMarker(sourceSeconds, targetBeat);
    });
    connect(m_audioEditorWidget, &AudioEditorWidget::warpMarkerRemoveRequested,
            this, [this](int markerIndex) {
        if (m_playlistView)
            m_playlistView->removeSelectedAudioClipWarpMarker(markerIndex);
    });
    connect(m_audioEditorWidget, &AudioEditorWidget::sourceRangeChangeRequested,
            this, [this](double sourceInBeats, double sourceOutBeats) {
        if (m_playlistView)
            m_playlistView->updateSelectedAudioClipSourceRange(
                sourceInBeats, sourceOutBeats);
    });
    connect(m_audioEditorWidget, &AudioEditorWidget::fadesChangeRequested,
            this, [this](double fadeInBeats, double fadeOutBeats) {
        if (m_playlistView)
            m_playlistView->updateSelectedAudioClipFades(
                fadeInBeats, fadeOutBeats);
    });
    connect(m_audioEditorWidget, &AudioEditorWidget::gainEnvelopeChangeRequested,
            this, [this](const std::vector<AudioGainEnvelopePoint>& points) {
        if (m_playlistView)
            m_playlistView->updateSelectedAudioClipGainEnvelope(points);
    });
    connect(m_audioEditorWidget, &AudioEditorWidget::fadeCurvesChangeRequested,
            this, [this](double fadeInCurve, double fadeOutCurve) {
        if (m_playlistView)
            m_playlistView->updateSelectedAudioClipFadeCurves(fadeInCurve, fadeOutCurve);
    });
    connect(m_audioEditorWidget, &AudioEditorWidget::loopRangeChangeRequested,
            this, [this](bool enabled, double startBeat, double endBeat) {
        if (m_playlistView)
            m_playlistView->requestLoopRangeChange(enabled, startBeat, endBeat);
    });
    connect(m_audioEditorWidget, &AudioEditorWidget::scrubRequested,
            this, [this](double projectBeat) { seekPlayhead(projectBeat); });
    connect(m_audioEditorWidget, &AudioEditorWidget::seekRequested,
            this, [this](double projectBeat) { seekPlayhead(projectBeat); });
    connect(m_playlistView, &PlaylistView::clipSelectionChanged, this, [this]() {
        if (!m_playlistView || !m_selectionService) return;
        QString primaryId;
        SelectionService::ClipKind kind = SelectionService::ClipKind::None;
        PlaylistView::AudioClipInfo audioClip;
        PlaylistView::MidiClipInfo midiClip;
        if (m_playlistView->selectedAudioClipInfo(&audioClip)) {
            primaryId = audioClip.clipId;
            kind = SelectionService::ClipKind::Audio;
            if (m_audioEditorWidget) m_audioEditorWidget->setClip(audioClip);
        } else if (m_playlistView->selectedMidiClipInfo(&midiClip)) {
            primaryId = midiClip.clipId;
            kind = SelectionService::ClipKind::Midi;
            if (m_audioEditorWidget) m_audioEditorWidget->clearClip();
        } else if (m_audioEditorWidget) {
            m_audioEditorWidget->clearClip();
        }
        m_selectionService->setClipSelection(
            m_playlistView->selectedClipIds(), primaryId, kind);
    });
    connect(m_playlistView, &PlaylistView::timeSelectionChanged, this,
            [this](bool enabled, double startBeat, double endBeat) {
        if (m_selectionService)
            m_selectionService->setTimeRange(enabled, startBeat, endBeat);
    });
    connect(m_transportWidget, &TransportWidget::punchToggled, this, [this](bool enabled) {
        if (m_isRecordingAudio || m_isRecordingMidi) {
            m_transportWidget->setPunchEnabled(m_punchEnabled);
            ui.statusBar->showMessage("Stop recording before changing Punch In/Out.", 3500);
            return;
        }
        double nextIn = m_punchInBeat;
        double nextOut = m_punchOutBeat;
        if (enabled && (!m_playlistView || !m_playlistView->timeSelection(&nextIn, &nextOut) ||
            !std::isfinite(nextIn) || !std::isfinite(nextOut) || nextIn < 0.0 ||
            nextOut - nextIn < 0.25)) {
            m_transportWidget->setPunchEnabled(false);
            ui.statusBar->showMessage("Select a time range before enabling Punch In/Out.", 4000);
            return;
        }

        const bool nextEnabled = enabled;
        if (m_punchEnabled == nextEnabled &&
            (!nextEnabled || (std::fabs(m_punchInBeat - nextIn) <= 1.0e-6 &&
                              std::fabs(m_punchOutBeat - nextOut) <= 1.0e-6))) return;
        const bool previousEnabled = m_punchEnabled;
        const double previousIn = m_punchInBeat;
        const double previousOut = m_punchOutBeat;
        const bool captureUndo = m_playlistView && !m_restoringProject &&
            !m_suppressProjectUndoCapture;
        const quint64 beforeStateId = captureUndo
            ? m_playlistView->currentArrangementStateId() : 0;
        const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
        if (captureUndo) {
            m_playlistView->beginArrangementTransaction();
            m_playlistView->markArrangementTransactionChanged();
        }
        auto applyPunch = [this](bool targetEnabled, double targetIn, double targetOut) {
            m_punchEnabled = targetEnabled;
            m_punchInBeat = targetIn;
            m_punchOutBeat = targetOut;
            if (m_transportWidget) m_transportWidget->setPunchEnabled(targetEnabled);
            if (m_playlistView)
                m_playlistView->setPunchRange(targetEnabled, targetIn, targetOut);
        };
        applyPunch(nextEnabled, nextIn, nextOut);
        if (captureUndo) {
            commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
                [applyPunch, previousEnabled, previousIn, previousOut]() {
                    applyPunch(previousEnabled, previousIn, previousOut);
                },
                [applyPunch, nextEnabled, nextIn, nextOut]() {
                    applyPunch(nextEnabled, nextIn, nextOut);
                });
        } else {
            setProjectDirty();
        }
        ui.statusBar->showMessage(nextEnabled
            ? QString("Punch %1 to %2 beats").arg(nextIn, 0, 'f', 2).arg(nextOut, 0, 'f', 2)
            : QString("Punch disabled"), 2500);
    });
    connect(m_playlistView, &PlaylistView::renameSelectedClipRequested, this, [this]() {
        QString currentName;
        PlaylistView::AudioClipInfo audioClip;
        PlaylistView::MidiClipInfo midiClip;
        if (m_playlistView->selectedAudioClipInfo(&audioClip)) currentName = audioClip.name;
        else if (m_playlistView->selectedMidiClipInfo(&midiClip)) currentName = midiClip.name;
        else return;

        bool accepted = false;
        const QString name = QInputDialog::getText(this, "Rename Clip", "Name:",
            QLineEdit::Normal, currentName, &accepted).trimmed();
        if (!accepted || name.isEmpty()) return;
        if (!m_playlistView->renameSelectedClip(name)) {
            ui.statusBar->showMessage("Could not rename the selected clip.", 3000);
        }
    });
    connect(m_playlistView, &PlaylistView::makeSelectedClipUniqueRequested,
            this, &VeraCoreQT::onMakeSelectedClipUnique);
    connect(m_playlistView, &PlaylistView::stemSeparationRequested,
            this, &VeraCoreQT::startStemSeparation);
    const auto pruneArrangementMidiDocuments = [this]() {
        if (!m_playlistView) return;
        std::set<QString> liveContentIds;
        for (const PlaylistView::MidiClipInfo& clip : m_playlistView->getMidiClips())
            liveContentIds.insert(clip.contentId);
        for (auto it = m_midiClipDocs.begin(); it != m_midiClipDocs.end();) {
            const bool retained = liveContentIds.find(it->first) != liveContentIds.end() ||
                m_playlistView->clipboardContainsMidiContent(it->first) ||
                m_playlistView->historyContainsMidiContent(it->first);
            if (!retained) it = m_midiClipDocs.erase(it);
            else ++it;
        }
    };
    connect(m_playlistView, &PlaylistView::clipClipboardChanged,
            this, pruneArrangementMidiDocuments);
    connect(m_playlistView, &PlaylistView::arrangementHistoryChanged, this,
            [this, pruneArrangementMidiDocuments](bool canUndo, bool canRedo) {
        ui.actionUndo->setEnabled(canUndo);
        ui.actionRedo->setEnabled(canRedo);
        pruneArrangementMidiDocuments();
        pruneArrangementProjectTransitions();
    });
    connect(m_playlistView, &PlaylistView::arrangementCleanStateChanged, this,
            [this](bool) { refreshProjectDirtyFromArrangement(false); });
    connect(m_playlistView, &PlaylistView::arrangementStateWillTransition,
            this, &VeraCoreQT::applyArrangementProjectTransition);
    connect(m_playlistView, &PlaylistView::arrangementStateTransitioned,
            this, &VeraCoreQT::finishArrangementProjectTransition);
    connect(m_playlistView, &PlaylistView::arrangementStateRestored, this, [this]() {
        if (!m_playlistView) return;
        if (m_pluginHost) m_pluginHost->setAraEditorSelection(-1, {});
        if (!m_currentMidiClipId.isEmpty()) {
            const std::vector<PlaylistView::MidiClipInfo> clips = m_playlistView->getMidiClips();
            const bool currentStillExists = std::any_of(
                clips.begin(), clips.end(), [this](const PlaylistView::MidiClipInfo& clip) {
                    return clip.clipId == m_currentMidiClipId;
                });
            if (!currentStillExists) {
                m_hasCurrentMidiClip = false;
                m_currentMidiClipId.clear();
                m_currentMidiClipKey.clear();
                if (m_midiEditor) m_midiEditor->close();
            }
        }
    });
    connect(m_playlistView, &PlaylistView::selectedClipDeleted, this,
            [this](const QString& clipId, const QString& contentId, bool audio) {
        if (audio) {
            if (m_pluginHost) m_pluginHost->setAraEditorSelection(-1, {});
            if (m_inspectorWidget)
                m_inspectorWidget->setAraProcessingAlgorithms(
                    QString(), QStringList(), -1);
        }
        if (!audio) {
            const std::vector<PlaylistView::MidiClipInfo> remainingClips = m_playlistView->getMidiClips();
            const bool contentStillUsed = std::any_of(
                remainingClips.begin(), remainingClips.end(),
                [&contentId](const PlaylistView::MidiClipInfo& clip) { return clip.contentId == contentId; });
            if (!contentStillUsed &&
                !m_playlistView->clipboardContainsMidiContent(contentId) &&
                !m_playlistView->historyContainsMidiContent(contentId))
                m_midiClipDocs.erase(buildClipDocumentKey(contentId));
            if (clipId == m_currentMidiClipId) {
                m_hasCurrentMidiClip = false;
                m_currentMidiClipId.clear();
                m_currentMidiClipKey.clear();
                if (m_midiEditor) m_midiEditor->close();
            }
        }
        ui.statusBar->showMessage("Clip deleted.", 2500);
    });
    connect(m_playlistView, &PlaylistView::contentChanged, this, [this]() {
        refreshProjectDirtyFromArrangement(true);
        syncMidiTimelineToAudio();
        syncAudioTimelineToAudio();
        refreshMidiEditorGhostClips();
    });
    connect(m_playlistView, &PlaylistView::playheadSeekRequested, this, [this](double beat) {
        seekPlayhead(beat);
    });
    connect(m_playlistView, &PlaylistView::loopRangeChanged,
            this, [this](bool enabled, double startBeat, double endBeat) {
        const bool nextEnabled = enabled && std::isfinite(startBeat) && std::isfinite(endBeat) &&
            startBeat >= 0.0 && endBeat - startBeat >= 0.25;
        const double nextStart = nextEnabled ? startBeat : 0.0;
        const double nextEnd = nextEnabled ? endBeat : 4.0;
        if (m_loopEnabled == nextEnabled &&
            std::fabs(m_loopStartBeat - nextStart) <= 1.0e-6 &&
            std::fabs(m_loopEndBeat - nextEnd) <= 1.0e-6) return;
        const bool previousEnabled = m_loopEnabled;
        const double previousStart = m_loopStartBeat;
        const double previousEnd = m_loopEndBeat;
        const bool captureUndo = !m_restoringProject && !m_suppressProjectUndoCapture;
        const quint64 beforeStateId = captureUndo
            ? m_playlistView->currentArrangementStateId() : 0;
        const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
        if (captureUndo) {
            m_playlistView->beginArrangementTransaction();
            m_playlistView->markArrangementTransactionChanged();
        }
        m_loopEnabled = nextEnabled;
        m_loopStartBeat = nextStart;
        m_loopEndBeat = nextEnd;
        if (m_audioEditorWidget)
            m_audioEditorWidget->setLoopRange(
                m_loopEnabled, m_loopStartBeat, m_loopEndBeat);
        if (audioGeneral) audioGeneral->audioCore.SetLoopRange(m_loopEnabled, m_loopStartBeat, m_loopEndBeat);
        if (captureUndo) {
            auto applyLoop = [this](bool targetEnabled, double targetStart, double targetEnd) {
                m_loopEnabled = targetEnabled;
                m_loopStartBeat = targetStart;
                m_loopEndBeat = targetEnd;
                if (m_playlistView)
                    m_playlistView->setLoopRange(targetEnabled, targetStart, targetEnd);
                if (m_audioEditorWidget)
                    m_audioEditorWidget->setLoopRange(
                        targetEnabled, targetStart, targetEnd);
                if (audioGeneral)
                    audioGeneral->audioCore.SetLoopRange(targetEnabled, targetStart, targetEnd);
            };
            commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
                [applyLoop, previousEnabled, previousStart, previousEnd]() {
                    applyLoop(previousEnabled, previousStart, previousEnd);
                },
                [applyLoop, nextEnabled, nextStart, nextEnd]() {
                    applyLoop(nextEnabled, nextStart, nextEnd);
                });
        } else {
            setProjectDirty();
        }
        ui.statusBar->showMessage(m_loopEnabled
            ? QString("Loop %1 to %2 beats").arg(m_loopStartBeat, 0, 'f', 2).arg(m_loopEndBeat, 0, 'f', 2)
            : QString("Loop disabled"), 2500);
    });

    // Sync vertical scrollbars between track headers and playlist
    connect(ui.trackHeadersScroll->verticalScrollBar(), &QScrollBar::valueChanged,
            m_playlistView->verticalScrollBar(), &QScrollBar::setValue);
    connect(m_playlistView->verticalScrollBar(), &QScrollBar::valueChanged,
            ui.trackHeadersScroll->verticalScrollBar(), &QScrollBar::setValue);

    // Set top margin to 30 to align with the Timeline Ruler (which is 30px high)
    QMargins margins = ui.trackHeadersLayout->contentsMargins();
    margins.setTop(30);
    ui.trackHeadersLayout->setContentsMargins(margins);

    // Set spacing to 0 so 60px track widgets stack exactly to 60px lanes
    ui.trackHeadersLayout->setSpacing(0);

    // Remove frame to align perfectly with the borderless PlaylistView
    ui.trackHeadersScroll->setFrameShape(QFrame::NoFrame);

    resetReturnBuses();
    initDefaultTracks();

    // Populate browser tree with previously saved plugins
    updateBrowserTree();
    ui.statusBar->showMessage(AudioCodecBackend::nativeCapabilitySummary().join(" | "), 6000);

    connect(ui.actionPreferences, &QAction::triggered, this, &VeraCoreQT::OnPreferences);
    ui.actionOpen->setShortcut(QKeySequence::Open);
    QAction* openProjectBackupAction = new QAction(
        QStringLiteral("Open Project Backup..."), this);
    openProjectBackupAction->setObjectName(QStringLiteral("actionOpenProjectBackup"));
    openProjectBackupAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+Shift+O")));
    openProjectBackupAction->setStatusTip(QStringLiteral(
        "Open a validated project generation from M:\\VeraCore-Recovery"));
    ui.menuFile->insertAction(ui.actionNew, openProjectBackupAction);
    connect(openProjectBackupAction, &QAction::triggered, this, [this]() {
        if (!confirmDiscardChanges()) return;
        const std::vector<ProjectBackupCandidate> backups = discoverProjectBackups();
        if (backups.empty()) {
            QMessageBox::information(this, QStringLiteral("Project Backups"),
                QStringLiteral("No valid project backups were found on M:."));
            return;
        }
        QStringList labels;
        labels.reserve(static_cast<qsizetype>(backups.size()));
        for (const ProjectBackupCandidate& backup : backups)
            labels.push_back(projectBackupDisplayLabel(backup));
        bool accepted = false;
        const QString selected = QInputDialog::getItem(
            this, QStringLiteral("Open Project Backup"),
            QStringLiteral("Validated generations (newest first):"),
            labels, 0, false, &accepted);
        if (!accepted) return;
        const qsizetype selectedIndex = labels.indexOf(selected);
        if (selectedIndex < 0 || selectedIndex >= static_cast<qsizetype>(backups.size())) return;
        const ProjectBackupCandidate& backup = backups[static_cast<size_t>(selectedIndex)];
        if (!recoveryFileMatchesChecksum(backup.backupPath)) {
            QMessageBox::critical(this, QStringLiteral("Open Project Backup"),
                QStringLiteral("The selected backup changed or failed its SHA-256 check."));
            return;
        }
        QString error;
        if (!loadProjectFile(backup.backupPath, &error)) {
            QMessageBox::critical(this, QStringLiteral("Open Project Backup"),
                error.isEmpty() ? QStringLiteral("Could not open the selected backup.") : error);
            return;
        }
        m_projectPath = backup.sourcePath;
        setProjectDirty(true);
        markRecoveryStateClean();
        ui.statusBar->showMessage(QStringLiteral(
            "Project backup opened. Save to restore the original project path."), 7000);
    });
    QAction* manageProjectBackupsAction = new QAction(
        QStringLiteral("Manage Project Backups..."), this);
    manageProjectBackupsAction->setObjectName(QStringLiteral("actionManageProjectBackups"));
    manageProjectBackupsAction->setStatusTip(QStringLiteral(
        "Delete one validated backup generation while preserving another"));
    ui.menuFile->insertAction(ui.actionNew, manageProjectBackupsAction);
    connect(manageProjectBackupsAction, &QAction::triggered, this, [this]() {
        const std::vector<ProjectBackupCandidate> backups = discoverProjectBackups();
        if (backups.empty()) {
            QMessageBox::information(this, QStringLiteral("Manage Project Backups"),
                QStringLiteral("No valid project backups were found on M:."));
            return;
        }
        QStringList labels;
        labels.reserve(static_cast<qsizetype>(backups.size()));
        for (const ProjectBackupCandidate& backup : backups)
            labels.push_back(projectBackupDisplayLabel(backup));
        bool accepted = false;
        const QString selected = QInputDialog::getItem(
            this, QStringLiteral("Manage Project Backups"),
            QStringLiteral("Select one validated generation to delete:"),
            labels, 0, false, &accepted);
        if (!accepted) return;
        const qsizetype selectedIndex = labels.indexOf(selected);
        if (selectedIndex < 0 || selectedIndex >= static_cast<qsizetype>(backups.size())) return;
        const ProjectBackupCandidate& backup = backups[static_cast<size_t>(selectedIndex)];
        const QString selectedKey = projectBackupKey(backup.sourcePath);
        const int validSiblingCount = static_cast<int>(std::count_if(
            backups.begin(), backups.end(), [&selectedKey](const ProjectBackupCandidate& candidate) {
                return projectBackupKey(candidate.sourcePath) == selectedKey;
            }));
        if (validSiblingCount < 2) {
            QMessageBox::information(this, QStringLiteral("Manage Project Backups"),
                QStringLiteral("The last valid generation is protected from deletion."));
            return;
        }
        const QMessageBox::StandardButton choice = QMessageBox::warning(
            this, QStringLiteral("Delete Project Backup"),
            QStringLiteral(
                "Permanently delete this backup generation?\n\n"
                "Action: Delete only the selected .vcp generation and its checksum.\n"
                "Risk: This generation cannot be recovered after deletion.\n"
                "Recovery: %1 other validated generation(s) remain; the source project is untouched.\n\n"
                "%2")
                .arg(validSiblingCount - 1)
                .arg(projectBackupDisplayLabel(backup)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes) return;
        QString error;
        if (!deleteProjectBackupGeneration(backup, &error)) {
            QMessageBox::critical(this, QStringLiteral("Manage Project Backups"),
                error.isEmpty() ? QStringLiteral("Could not delete the selected generation.") : error);
            return;
        }
        ui.statusBar->showMessage(QStringLiteral(
            "Selected backup generation deleted; another valid generation remains."), 6000);
    });
    QAction* auditProjectBackupsAction = new QAction(
        QStringLiteral("Project Backup Audit..."), this);
    auditProjectBackupsAction->setObjectName(QStringLiteral("actionAuditProjectBackups"));
    auditProjectBackupsAction->setStatusTip(QStringLiteral(
        "Read-only integrity and storage report for project backups"));
    ui.menuFile->insertAction(ui.actionNew, auditProjectBackupsAction);
    connect(auditProjectBackupsAction, &QAction::triggered, this, [this]() {
        const ProjectBackupAudit audit = auditProjectBackups();
        const QString newest = audit.newestValidUtc.isValid()
            ? QLocale().toString(audit.newestValidUtc.toLocalTime(),
                                 QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : QStringLiteral("None");
        QMessageBox::information(this, QStringLiteral("Project Backup Audit"),
            QStringLiteral(
                "Read-only report — no files changed.\n\n"
                "Root: M:\\VeraCore-Recovery\\ProjectBackups\n"
                "Directories scanned: %1\n"
                "Valid generations: %2 (%3 MiB)\n"
                "Invalid/corrupt generations: %4 (%5 MiB)\n"
                "Invalid metadata directories: %6\n"
                "Orphan checksum files: %7\n"
                "Newest valid generation: %8\n"
                "Scan capped at 1024 directories: %9")
                .arg(audit.directoriesVisited)
                .arg(audit.validGenerations)
                .arg(QLocale().toString(static_cast<double>(audit.validBytes) /
                                        (1024.0 * 1024.0), 'f', 2))
                .arg(audit.invalidGenerations)
                .arg(QLocale().toString(static_cast<double>(audit.invalidBytes) /
                                        (1024.0 * 1024.0), 'f', 2))
                .arg(audit.invalidMetadataDirectories)
                .arg(audit.orphanChecksums)
                .arg(newest)
                .arg(audit.scanCapped ? QStringLiteral("Yes") : QStringLiteral("No")));
    });
    ui.actionSave->setShortcut(QKeySequence::Save);
    ui.actionSave_As->setShortcut(QKeySequence::SaveAs);
    ui.actionNew->setShortcut(QKeySequence::New);
    connect(ui.actionOpen, &QAction::triggered, this, [this]() {
        if (!confirmDiscardChanges()) return;
        const QString path = QFileDialog::getOpenFileName(this, "Open VeraCore Project", QString(),
                                                          "VeraCore project (*.vcp)");
        if (path.isEmpty()) return;
        QString error;
        if (!loadProjectFile(path, &error)) {
            if (error == projectIntegrityErrorText()) {
                const std::vector<ProjectBackupCandidate> backups =
                    validatedBackupsForProject(path);
                if (!backups.empty()) {
                    const ProjectBackupCandidate& newest = backups.front();
                    const QMessageBox::StandardButton recover = QMessageBox::warning(
                        this, QStringLiteral("Project Integrity Failure"),
                        QStringLiteral(
                            "%1\n\n"
                            "A validated backup is available:\n%2\n\n"
                            "Open this backup now? The damaged source file will remain untouched, "
                            "and the recovered project will require Save As to a new file.")
                            .arg(error, projectBackupDisplayLabel(newest)),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                    if (recover != QMessageBox::Yes) return;
                    QString backupError;
                    if (!loadProjectFile(newest.backupPath, &backupError)) {
                        QMessageBox::critical(this, QStringLiteral("Open Project Backup"),
                            backupError.isEmpty()
                                ? QStringLiteral("The validated backup could not be opened.")
                                : backupError);
                        return;
                    }
                    m_projectPath.clear();
                    setProjectDirty(true);
                    markRecoveryStateClean();
                    ui.statusBar->showMessage(QStringLiteral(
                        "Validated backup opened after integrity failure. Use Save As; source untouched."),
                        8000);
                    return;
                }
            }
            QMessageBox::critical(this, "Open Project", error.isEmpty() ? "Could not open project." : error);
            return;
        }
        m_projectPath = path;
        setProjectDirty(m_autoRelinkedMediaCount > 0);
        markRecoveryStateClean();
        const QString integrityStatus = m_projectIntegrityState == ProjectIntegrityState::Verified
            ? QStringLiteral(" · SHA-256 verified")
            : QStringLiteral(" · Legacy/unverified; save to add SHA-256");
        if (m_missingMediaCount > 0) {
            ui.statusBar->showMessage(QString("Project loaded · %1 missing media item(s). Select an offline clip to relink.")
                .arg(m_missingMediaCount) + integrityStatus, 8000);
        } else if (m_autoRelinkedMediaCount > 0) {
            ui.statusBar->showMessage(QString("Project loaded · auto-relinked %1 media item(s). Save to keep new paths.")
                .arg(m_autoRelinkedMediaCount) + integrityStatus, 8000);
        } else {
            ui.statusBar->showMessage(QStringLiteral("Project loaded") + integrityStatus, 5000);
        }
    });
    connect(ui.actionSave_As, &QAction::triggered, this, [this]() { saveProjectInteractive(true); });
    connect(ui.actionSave, &QAction::triggered, this, [this]() { saveProjectInteractive(false); });
    connect(ui.actionNew, &QAction::triggered, this, [this]() {
        if (!confirmDiscardChanges()) return;
        m_restoringProject = true;
        resetProjectModel(true);
        m_restoringProject = false;
        m_mixerParameters->republishProject(m_tracks);
        m_projectPath.clear();
        setProjectDirty(false);
        markRecoveryStateClean();
        ui.statusBar->showMessage("New project.", 2500);
    });
    connect(ui.actionExit, &QAction::triggered, this, &QWidget::close);
    m_commandRegistry->registerAction(QStringLiteral("project.open"), ui.actionOpen,
        QStringLiteral("Project"), QStringLiteral("Open a VeraCore project"));
    m_commandRegistry->registerAction(QStringLiteral("project.open-backup"), openProjectBackupAction,
        QStringLiteral("Project"), QStringLiteral("Open a validated project backup"));
    m_commandRegistry->registerAction(QStringLiteral("project.manage-backups"), manageProjectBackupsAction,
        QStringLiteral("Project"), QStringLiteral("Manage validated project backups"));
    m_commandRegistry->registerAction(QStringLiteral("project.audit-backups"), auditProjectBackupsAction,
        QStringLiteral("Project"), QStringLiteral("Audit project backup integrity and storage"));
    m_commandRegistry->registerAction(QStringLiteral("project.save"), ui.actionSave,
        QStringLiteral("Project"), QStringLiteral("Save the current project"));
    m_commandRegistry->registerAction(QStringLiteral("project.save-as"), ui.actionSave_As,
        QStringLiteral("Project"), QStringLiteral("Save the current project to a new path"));
    m_commandRegistry->registerAction(QStringLiteral("project.new"), ui.actionNew,
        QStringLiteral("Project"), QStringLiteral("Create a new project"));
    ui.actionBrowser->setChecked(true);
    ui.actionBrowser->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+B")));
    ui.actionBrowser->setStatusTip("Show or hide Browser (Ctrl+Alt+B)");
    connect(ui.actionBrowser, &QAction::triggered, this, [this](bool checked) {
        if (checked) {
            ui.browserPanel->show();
            ui.mainSplitter->setSizes(m_lastMainSplitterSizes.isEmpty() ? QList<int>{240, 1040} : m_lastMainSplitterSizes);
            ui.browserSearch->setFocus(Qt::ShortcutFocusReason);
            return;
        }

        if (ui.browserPanel->isVisible()) {
            m_lastMainSplitterSizes = ui.mainSplitter->sizes();
        }
        ui.browserPanel->hide();
    });
    m_commandRegistry->registerAction(QStringLiteral("view.browser"), ui.actionBrowser,
        QStringLiteral("View"), QStringLiteral("Show or hide the Browser"));

    ui.actionPlaylist->setChecked(true);
    ui.actionPlaylist->setShortcut(QKeySequence(Qt::Key_F5));
    ui.actionPlaylist->setStatusTip("Show or hide Arrangement / Playlist (F5)");
    connect(ui.actionPlaylist, &QAction::triggered, this, [this](bool checked) {
        if (checked) {
            ui.playlistSplitter->show();
            ui.centerSplitter->setSizes(m_lastCenterSplitterSizes.isEmpty() ? QList<int>{540, 170} : m_lastCenterSplitterSizes);
            ui.playlistSplitter->setSizes(m_lastPlaylistSplitterSizes.isEmpty() ? QList<int>{214, 826} : m_lastPlaylistSplitterSizes);
            if (m_playlistView) m_playlistView->setFocus(Qt::ShortcutFocusReason);
            return;
        }

        if (ui.playlistSplitter->isVisible()) {
            m_lastCenterSplitterSizes = ui.centerSplitter->sizes();
            m_lastPlaylistSplitterSizes = ui.playlistSplitter->sizes();
        }
        ui.playlistSplitter->hide();
    });
    m_commandRegistry->registerAction(QStringLiteral("view.arrangement"), ui.actionPlaylist,
        QStringLiteral("View"), QStringLiteral("Show or hide the Arrangement"));

    auto setDetailPanelVisible = [this](bool visible) {
        if (visible) {
            m_detailTabs->show();
            ui.centerSplitter->setSizes(
                m_lastCenterSplitterSizes.isEmpty() ? QList<int>{540, 170} : m_lastCenterSplitterSizes);
            return;
        }
        if (m_detailTabs->isVisible()) m_lastCenterSplitterSizes = ui.centerSplitter->sizes();
        m_detailTabs->hide();
    };

    m_inspectorAction = new QAction("Device / Inspector", this);
    m_inspectorAction->setShortcut(QKeySequence(QStringLiteral("Shift+Tab")));
    m_inspectorAction->setStatusTip("Show or hide Clip / Device detail panel (Shift+Tab)");
    m_inspectorAction->setCheckable(true);
    m_inspectorAction->setChecked(true);
    ui.menuView->insertAction(ui.actionTempo_Tapper, m_inspectorAction);
    ui.actionMixer->setText("Mixer / Device Panel");
    ui.actionMixer->setShortcut(QKeySequence(Qt::Key_F9));
    ui.actionMixer->setStatusTip("Show or hide Mixer / Device panel (F9)");
    ui.actionMixer->setChecked(true);
    connect(m_inspectorAction, &QAction::triggered, this,
            [this, setDetailPanelVisible](bool checked) {
        QSignalBlocker blocker(ui.actionMixer);
        ui.actionMixer->setChecked(checked);
        setDetailPanelVisible(checked);
    });
    connect(ui.actionMixer, &QAction::triggered, this,
            [this, setDetailPanelVisible](bool checked) {
        QSignalBlocker blocker(m_inspectorAction);
        m_inspectorAction->setChecked(checked);
        setDetailPanelVisible(checked);
    });
    m_commandRegistry->registerAction(QStringLiteral("view.inspector"), m_inspectorAction,
        QStringLiteral("View"), QStringLiteral("Show or hide the Clip / Device detail panel"));
    m_commandRegistry->registerAction(QStringLiteral("view.mixer"), ui.actionMixer,
        QStringLiteral("View"), QStringLiteral("Show or hide the Mixer / Device panel"));

    ui.actionMIDI_Editor->setShortcut(QKeySequence(Qt::Key_F7));
    ui.actionMIDI_Editor->setStatusTip("Open or close Piano Roll / MIDI Editor (F7)");
    connect(ui.actionMIDI_Editor, &QAction::triggered, this, [this](bool checked) {
        if (checked) {
            onOpenCurrentMidiEditor();
            return;
        }

        if (m_midiEditor && m_midiEditor->isVisible()) {
            m_midiEditor->close();
        }
    });
    m_commandRegistry->registerAction(QStringLiteral("view.piano-roll"), ui.actionMIDI_Editor,
        QStringLiteral("View"), QStringLiteral("Open or close the Piano Roll"));

    auto restoredSizes = [](QSettings& settings, const char* key, const QList<int>& fallback) {
        const QVariantList stored = settings.value(QString::fromLatin1(key)).toList();
        if (stored.size() != fallback.size()) return fallback;
        QList<int> sizes;
        int total = 0;
        for (const QVariant& item : stored) {
            bool ok = false;
            const int value = item.toInt(&ok);
            if (!ok || value <= 0) return fallback;
            sizes.push_back(value);
            total += value;
        }
        return total > 0 ? sizes : fallback;
    };
    QSettings workspaceSettings;
    workspaceSettings.beginGroup(QStringLiteral("Workspace"));
    const QByteArray savedGeometry = workspaceSettings.value(QStringLiteral("MainWindowGeometry")).toByteArray();
    m_lastMainSplitterSizes = restoredSizes(
        workspaceSettings, "MainSplitterSizes", QList<int>{240, 1040});
    m_lastCenterSplitterSizes = restoredSizes(
        workspaceSettings, "CenterSplitterSizes", QList<int>{540, 170});
    m_lastPlaylistSplitterSizes = restoredSizes(
        workspaceSettings, "PlaylistSplitterSizes", QList<int>{214, 826});
    const bool browserVisible = workspaceSettings.value(QStringLiteral("BrowserVisible"), true).toBool();
    const bool playlistVisible = workspaceSettings.value(QStringLiteral("PlaylistVisible"), true).toBool();
    const bool inspectorVisible = workspaceSettings.value(QStringLiteral("InspectorVisible"), true).toBool();
    workspaceSettings.endGroup();

    if (!savedGeometry.isEmpty()) restoreGeometry(savedGeometry);
    ui.mainSplitter->setSizes(m_lastMainSplitterSizes);
    ui.centerSplitter->setSizes(m_lastCenterSplitterSizes);
    ui.playlistSplitter->setSizes(m_lastPlaylistSplitterSizes);
    ui.actionBrowser->setChecked(browserVisible);
    ui.browserPanel->setVisible(browserVisible);
    ui.actionPlaylist->setChecked(playlistVisible);
    ui.playlistSplitter->setVisible(playlistVisible);
    m_inspectorAction->setChecked(inspectorVisible);
    ui.actionMixer->setChecked(inspectorVisible);
    m_detailTabs->setVisible(inspectorVisible);

    m_workspaceManager = new WorkspaceManager(this);
    connect(m_workspaceManager, &WorkspaceManager::activationRequested,
            this, &VeraCoreQT::applyWorkspacePreset);
    connect(m_workspaceManager, &WorkspaceManager::currentPresetChanged,
            this, [this](const QString& id) {
        for (auto it = m_workspaceActions.begin(); it != m_workspaceActions.end(); ++it) {
            const QSignalBlocker blocker(it.value());
            it.value()->setChecked(it.key() == id);
        }
        QSettings settings;
        settings.beginGroup(QStringLiteral("Workspace"));
        settings.setValue(QStringLiteral("CurrentPreset"), id);
        settings.endGroup();
    });
    QMenu* workspaceMenu = ui.menuView->addMenu(QStringLiteral("Workspaces"));
    QActionGroup* workspaceGroup = new QActionGroup(this);
    workspaceGroup->setExclusive(true);
    int workspaceIndex = 1;
    for (const QString& id : m_workspaceManager->presetIds()) {
        const WorkspacePreset preset = m_workspaceManager->preset(id);
        QAction* action = workspaceMenu->addAction(preset.name);
        action->setCheckable(true);
        action->setToolTip(preset.description);
        action->setStatusTip(preset.description);
        action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+%1").arg(workspaceIndex)));
        workspaceGroup->addAction(action);
        m_workspaceActions.insert(id, action);
        connect(action, &QAction::triggered, this, [this, id](bool checked) {
            if (checked) m_workspaceManager->activate(id);
        });
        m_commandRegistry->registerAction(QStringLiteral("workspace.%1").arg(id), action,
            QStringLiteral("Workspace"), preset.description);
        ++workspaceIndex;
    }
    workspaceMenu->addSeparator();
    QAction* saveCustomWorkspace = workspaceMenu->addAction(
        QStringLiteral("Save Current as User Custom"));
    saveCustomWorkspace->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+Shift+8")));
    connect(saveCustomWorkspace, &QAction::triggered, this, [this]() {
        if (!m_workspaceManager->saveUserCustom(captureWorkspacePreset())) {
            ui.statusBar->showMessage(QStringLiteral("Workspace layout is invalid."), 3500);
            return;
        }
        ui.statusBar->showMessage(QStringLiteral("User Custom workspace saved."), 3000);
    });
    m_commandRegistry->registerAction(QStringLiteral("workspace.save-user-custom"),
        saveCustomWorkspace, QStringLiteral("Workspace"),
        QStringLiteral("Save the current panel layout as User Custom"));
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("Workspace"));
        const QString current = settings.value(
            QStringLiteral("CurrentPreset"), QStringLiteral("arrangement")).toString();
        settings.endGroup();
        if (!m_workspaceManager->selectCurrentWithoutActivation(current))
            m_workspaceManager->selectCurrentWithoutActivation(QStringLiteral("arrangement"));
    }

    m_keymapProfiles = new KeymapProfileManager(m_commandRegistry, this);
    QMenu* keymapMenu = ui.menuOptions->addMenu(QStringLiteral("Keymap Profile"));
    QActionGroup* keymapGroup = new QActionGroup(this);
    keymapGroup->setExclusive(true);
    connect(m_keymapProfiles, &KeymapProfileManager::profileApplied,
            this, [this](const QString& id) {
        for (auto it = m_keymapActions.begin(); it != m_keymapActions.end(); ++it) {
            const QSignalBlocker blocker(it.value());
            it.value()->setChecked(it.key() == id);
        }
        QSettings settings;
        settings.beginGroup(QStringLiteral("Interaction"));
        settings.setValue(QStringLiteral("KeymapProfile"), id);
        settings.endGroup();
        const KeymapProfile profile = m_keymapProfiles->profile(id);
        ui.statusBar->showMessage(
            QStringLiteral("Keymap: %1").arg(profile.name), 2500);
    });
    for (const QString& id : m_keymapProfiles->profileIds()) {
        const KeymapProfile profile = m_keymapProfiles->profile(id);
        QAction* action = keymapMenu->addAction(profile.name);
        action->setCheckable(true);
        action->setToolTip(profile.description);
        action->setStatusTip(profile.description);
        keymapGroup->addAction(action);
        m_keymapActions.insert(id, action);
        connect(action, &QAction::triggered, this, [this, id](bool checked) {
            if (!checked) return;
            QString error;
            if (!m_keymapProfiles->applyProfile(id, &error)) {
                ui.statusBar->showMessage(error, 5000);
            }
        });
    }
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("Interaction"));
        const QString savedProfile = settings.value(
            QStringLiteral("KeymapProfile"), QStringLiteral("veracore")).toString();
        settings.endGroup();
        QString error;
        if (!m_keymapProfiles->applyProfile(savedProfile, &error))
            m_keymapProfiles->applyProfile(QStringLiteral("veracore"), nullptr);
    }

    ui.actionMIDI_2->setText("Selected MIDI Clip...");
    QAction* exportArrangementMidiAction = new QAction("MIDI Arrangement (Type 1)...", this);
    const QList<QAction*> exportActions = ui.menuExport->actions();
    const int selectedMidiActionIndex = exportActions.indexOf(ui.actionMIDI_2);
    QAction* insertBefore = selectedMidiActionIndex >= 0 && selectedMidiActionIndex + 1 < exportActions.size()
        ? exportActions[selectedMidiActionIndex + 1]
        : nullptr;
    ui.menuExport->insertAction(insertBefore, exportArrangementMidiAction);

    connect(exportArrangementMidiAction, &QAction::triggered, this, [this]() {
        const QString destination = QFileDialog::getSaveFileName(
            this, "Export MIDI Arrangement", QString(), "MIDI (*.mid)");
        if (destination.isEmpty()) return;
        const QString normalizedDestination = withRequiredSuffix(destination, ".mid");
        const QFileInfo midiInfo(normalizedDestination);
        QTemporaryFile tempMidi(midiInfo.absolutePath() + "/.VeraCoreArrangementXXXXXX.mid");
        tempMidi.setAutoRemove(true);
        if (!tempMidi.open()) {
            QMessageBox::critical(this, "MIDI Export", "Could not create temporary arrangement MIDI file.");
            return;
        }
        const QString tempMidiPath = tempMidi.fileName();
        tempMidi.close();
        QFile::remove(tempMidiPath);

        QString errorText;
        if (!writeMidiArrangementToFile(tempMidiPath, &errorText)) {
            QFile::remove(tempMidiPath);
            QMessageBox::critical(this, "MIDI Export", errorText.isEmpty()
                ? "Could not export MIDI arrangement." : errorText);
            return;
        }

        QString installError;
        if (!installFileAtomically(tempMidiPath, normalizedDestination, &installError)) {
            QFile::remove(tempMidiPath);
            QMessageBox::critical(this, "MIDI Export", installError.isEmpty()
                ? "Could not install MIDI arrangement." : installError);
            return;
        }
        ui.statusBar->showMessage("MIDI arrangement exported as Standard MIDI File Type 1.", 4000);
    });

    connect(ui.actionMIDI, &QAction::triggered, this, [this]() {
        if (m_selectedTrackIndex < 0 ||
            m_selectedTrackIndex >= (int)m_tracks.size() ||
            m_tracks[m_selectedTrackIndex]->GetType() != TrackType::Midi) {
            QMessageBox::information(this, "MIDI Import", "Select a MIDI track before importing.");
            return;
        }

        const QString path = QFileDialog::getOpenFileName(this,
            "Import MIDI",
            QString(),
            "MIDI (*.mid *.midi)");
        if (path.isEmpty()) {
            return;
        }

        QString errorText;
        const int importedCount = importMidiFileToTracks(
            path, m_selectedTrackIndex, 0.0, true, &errorText);
        if (importedCount < 0) {
            QMessageBox::critical(this, "MIDI Import", errorText.isEmpty() ? "Could not read MIDI file." : errorText);
            return;
        }
        ui.statusBar->showMessage(QString("Imported %1 MIDI track%2.")
            .arg(importedCount).arg(importedCount == 1 ? "" : "s"), 4000);
    });
    connect(ui.actionMIDI_2, &QAction::triggered, this, [this]() {
        if (!m_hasCurrentMidiClip) {
            QMessageBox::information(this, "MIDI Export", "Select a MIDI clip before exporting.");
            return;
        }
        const QString destination = QFileDialog::getSaveFileName(
            this,
            "Export MIDI",
            QString(),
            "MIDI (*.mid)");
        if (destination.isEmpty()) {
            return;
        }

        const MidiClipDocument* doc = getCurrentSelectedClipDocument();
        if (!doc) {
            QMessageBox::critical(this, "MIDI Export", "Current clip document could not be found.");
            return;
        }
        PlaylistView::MidiClipInfo clip;
        if (!m_playlistView || !m_playlistView->selectedMidiClipInfo(&clip)) {
            QMessageBox::critical(this, "MIDI Export", "Current MIDI clip instance could not be found.");
            return;
        }
        const MidiClipDocument exportDocument = midiDocumentForClipExport(*doc, clip);

        const QString normalizedDestination = withRequiredSuffix(destination, ".mid");

        const QFileInfo midiInfo(normalizedDestination);
        QTemporaryFile tempMidi(midiInfo.absolutePath() + "/.VeraCoreMidiXXXXXX.mid");
        tempMidi.setAutoRemove(true);
        if (!tempMidi.open()) {
            QMessageBox::critical(this, "MIDI Export", "Could not create temporary MIDI file.");
            return;
        }
        const QString tempMidiPath = tempMidi.fileName();
        tempMidi.close();

        QString errorText;
        if (!writeMidiDocumentToFile(tempMidiPath, exportDocument, &errorText)) {
            QFile::remove(tempMidiPath);
            QMessageBox::critical(this, "MIDI Export", errorText.isEmpty() ? "Could not write MIDI file." : errorText);
            return;
        }
        MidiClipDocument validatedDocument;
        QString validationError;
        const std::vector<MidiRoundTripNote> expectedNotes = normalizedMidiRoundTripNotes(exportDocument, m_projectPpq);
        const std::vector<MidiRoundTripEvent> expectedEvents = normalizedMidiRoundTripEvents(exportDocument, m_projectPpq);
        const std::vector<MidiRoundTripTempoPoint> expectedTempoMap =
            normalizedMidiRoundTripTempoMap(exportDocument, m_projectPpq);
        const std::vector<MidiRoundTripTimeSignaturePoint> expectedSignatureMap =
            normalizedMidiRoundTripTimeSignatureMap(exportDocument, m_projectPpq);
        const bool readBack = importMidiDocumentFromFile(tempMidiPath, validatedDocument, &validationError);
        const bool notesMatch = readBack &&
            normalizedMidiRoundTripNotes(validatedDocument, m_projectPpq) == expectedNotes;
        const bool eventsMatch = readBack &&
            normalizedMidiRoundTripEvents(validatedDocument, m_projectPpq, false) == expectedEvents;
        const bool tempoMatch = readBack &&
            normalizedMidiRoundTripTempoMap(validatedDocument, m_projectPpq) == expectedTempoMap;
        const bool signatureMatch = readBack &&
            normalizedMidiRoundTripTimeSignatureMap(
                validatedDocument, m_projectPpq) == expectedSignatureMap;
        const bool namesMatch = readBack &&
            (exportDocument.name.trimmed().isEmpty() ||
             validatedDocument.name == exportDocument.name.trimmed()) &&
            (exportDocument.trackName.trimmed().isEmpty() ||
             validatedDocument.trackName == exportDocument.trackName.trimmed());
        if (!notesMatch || !eventsMatch || !tempoMatch || !signatureMatch || !namesMatch) {
            QFile::remove(tempMidiPath);
            if (readBack && validationError.isEmpty()) {
                QStringList mismatches;
                if (!notesMatch) mismatches.push_back(QString("notes %1/%2")
                    .arg(static_cast<qulonglong>(expectedNotes.size()))
                    .arg(static_cast<qulonglong>(validatedDocument.notes.size())));
                if (!eventsMatch) mismatches.push_back(QString("channel events %1/%2")
                    .arg(static_cast<qulonglong>(expectedEvents.size()))
                    .arg(static_cast<qulonglong>(validatedDocument.performanceEvents.size())));
                if (!tempoMatch) mismatches.push_back("tempo");
                if (!signatureMatch) mismatches.push_back("time signature");
                if (!namesMatch) mismatches.push_back("track metadata");
                validationError = QString("semantic mismatch: %1").arg(mismatches.join(", "));
            }
            QMessageBox::critical(this, "MIDI Export",
                validationError.isEmpty()
                    ? "Exported MIDI failed read-back validation."
                    : QString("Exported MIDI failed read-back validation: %1").arg(validationError));
            return;
        }
        QString installError;
        if (!installFileAtomically(tempMidiPath, normalizedDestination, &installError)) {
            QFile::remove(tempMidiPath);
            QMessageBox::critical(this, "MIDI Export", installError.isEmpty()
                ? "Could not install MIDI file." : installError);
            return;
        }
        ui.statusBar->showMessage("MIDI exported.", 3500);
    });
    auto configureRenderAction = [](QAction* action, AudioCodecFormat format) {
        if (!action) {
            return;
        }
        const bool available = AudioCodecBackend::isNativeEncoderAvailable(format);
        action->setToolTip(available
            ? QString("%1 render available.").arg(AudioCodecBackend::displayName(format))
            : AudioCodecBackend::nativeEncoderRequirement(format) + " Embedded libraries are used; no external process is launched.");
        if (format != AudioCodecFormat::Wav && !available) {
            action->setText(QString("%1 (embedded encoder missing)").arg(AudioCodecBackend::displayName(format)));
        }
    };
    configureRenderAction(ui.actionWAV, AudioCodecFormat::Wav);
    configureRenderAction(ui.actionMP3, AudioCodecFormat::Mp3);
    configureRenderAction(ui.actionFLAC, AudioCodecFormat::Flac);
    configureRenderAction(ui.actionOGG, AudioCodecFormat::OggVorbis);
    configureRenderAction(ui.actionWavPack, AudioCodecFormat::WavPack);

    connect(ui.actionWAV, &QAction::triggered, this, [this]() { renderOffline("wav"); });
    connect(ui.actionMP3, &QAction::triggered, this, [this]() { renderOffline("mp3"); });
    connect(ui.actionFLAC, &QAction::triggered, this, [this]() { renderOffline("flac"); });
    connect(ui.actionOGG, &QAction::triggered, this, [this]() { renderOffline("ogg"); });
    connect(ui.actionWavPack, &QAction::triggered, this, [this]() { renderOffline("wv"); });
    m_recoveryIdleClock.start();
    m_recoveryTimer = new QTimer(this);
    m_recoveryRetryTimer = new QTimer(this);
    m_recoveryRetryTimer->setSingleShot(true);
    connect(m_recoveryRetryTimer, &QTimer::timeout,
            this, &VeraCoreQT::scheduleRecoverySnapshot);
    constexpr std::array<int, 6> recoveryIntervals{{30, 60, 120, 300, 600, 900}};
    QSettings recoverySettings;
    const bool recoveryEnabled = recoverySettings.value(
        QStringLiteral("Recovery/Enabled"), true).toBool();
    const int savedRecoverySeconds = recoverySettings.value(
        QStringLiteral("Recovery/IntervalSeconds"), 60).toInt();
    const int recoverySeconds = std::find(recoveryIntervals.begin(), recoveryIntervals.end(),
                                          savedRecoverySeconds) != recoveryIntervals.end()
        ? savedRecoverySeconds : 60;
    if (recoverySeconds != savedRecoverySeconds)
        recoverySettings.setValue(QStringLiteral("Recovery/IntervalSeconds"), recoverySeconds);
    m_recoveryTimer->setInterval(recoverySeconds * 1000);
    connect(m_recoveryTimer, &QTimer::timeout, this, &VeraCoreQT::scheduleRecoverySnapshot);
    if (recoveryEnabled) m_recoveryTimer->start();

    QMenu* recoveryMenu = ui.menuOptions->addMenu(QStringLiteral("Recovery"));
    QAction* recoveryEnabledAction = recoveryMenu->addAction(
        QStringLiteral("Automatic Recovery"));
    recoveryEnabledAction->setObjectName(QStringLiteral("actionAutomaticRecovery"));
    recoveryEnabledAction->setCheckable(true);
    recoveryEnabledAction->setChecked(recoveryEnabled);
    recoveryEnabledAction->setStatusTip(QStringLiteral(
        "Write rotating recovery checkpoints to M:\\VeraCore-Recovery"));
    connect(recoveryEnabledAction, &QAction::toggled, this, [this](bool enabled) {
        QSettings().setValue(QStringLiteral("Recovery/Enabled"), enabled);
        if (enabled) m_recoveryTimer->start();
        else {
            m_recoveryTimer->stop();
            if (m_recoveryRetryTimer) m_recoveryRetryTimer->stop();
        }
        ui.statusBar->showMessage(enabled
            ? QStringLiteral("Automatic recovery enabled.")
            : QStringLiteral("Automatic recovery disabled."), 3500);
    });

    QMenu* intervalMenu = recoveryMenu->addMenu(QStringLiteral("Checkpoint Interval"));
    QActionGroup* intervalGroup = new QActionGroup(this);
    intervalGroup->setExclusive(true);
    for (int seconds : recoveryIntervals) {
        const QString label = seconds < 60
            ? QStringLiteral("%1 seconds").arg(seconds)
            : seconds == 60 ? QStringLiteral("1 minute")
                            : QStringLiteral("%1 minutes").arg(seconds / 60);
        QAction* action = intervalMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(seconds == recoverySeconds);
        intervalGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, recoveryEnabledAction, seconds, label](bool checked) {
            if (!checked) return;
            m_recoveryTimer->setInterval(seconds * 1000);
            QSettings().setValue(QStringLiteral("Recovery/IntervalSeconds"), seconds);
            if (recoveryEnabledAction->isChecked()) m_recoveryTimer->start();
            ui.statusBar->showMessage(
                QStringLiteral("Recovery interval: %1.").arg(label), 3500);
        });
    }
    recoveryMenu->addSeparator();
    QAction* recoveryStatusAction = recoveryMenu->addAction(
        QStringLiteral("Recovery Status..."));
    recoveryStatusAction->setObjectName(QStringLiteral("actionRecoveryStatus"));
    connect(recoveryStatusAction, &QAction::triggered, this,
            [this, recoveryEnabledAction]() {
        const QSettings settings;
        const auto formatUtc = [](qint64 utcMs) {
            return utcMs > 0
                ? QDateTime::fromMSecsSinceEpoch(utcMs, Qt::UTC)
                      .toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss t"))
                : QStringLiteral("Never");
        };
        const QString attemptPath = settings.value(
            QStringLiteral("Recovery/LastAttemptPath")).toString();
        const QString successPath = settings.value(
            QStringLiteral("Recovery/LastSuccessPath")).toString();
        const bool managedAttempt = !attemptPath.isEmpty() &&
                                    isManagedRecoveryPath(attemptPath);
        const bool checksumValid = managedAttempt &&
                                   recoveryFileMatchesChecksum(attemptPath);
        const QString lastFailureReason = settings.value(
            QStringLiteral("Recovery/LastFailureReason")).toString().trimmed();
        const RecoveryStorageStatus storage = inspectRecoveryStorage(0, false);
        const QString availableStorage = storage.bytesAvailable >= 0
            ? QStringLiteral("%1 GiB").arg(
                  static_cast<double>(storage.bytesAvailable) /
                      (1024.0 * 1024.0 * 1024.0), 0, 'f', 2)
            : QStringLiteral("Unknown");
        const int intervalSeconds = m_recoveryTimer
            ? m_recoveryTimer->interval() / 1000 : 0;
        const QString report = QStringLiteral(
            "Automatic recovery: %1\n"
            "Interval: %2 seconds\n"
            "Checkpoint pending: %3\n\n"
            "Last attempt: %4\n"
            "Attempt path: %5\n"
            "Attempt revision: %6\n"
            "Managed path: %7\n"
            "On-disk SHA-256: %8\n\n"
            "Last success: %9\n"
            "Success path: %10\n"
            "Success revision: %11\n"
            "Consecutive failures: %12\n"
            "Last failure reason: %13\n\n"
            "Recovery volume ready: %14\n"
            "Recovery volume read-only: %15\n"
            "Available storage: %16\n"
            "Storage detail: %17\n\n"
            "Retry pending: %18\n"
            "Retry in: %19 seconds")
            .arg(recoveryEnabledAction->isChecked()
                     ? QStringLiteral("Enabled") : QStringLiteral("Disabled"))
            .arg(intervalSeconds)
            .arg(m_recoveryWritePending ? QStringLiteral("Yes") : QStringLiteral("No"))
            .arg(formatUtc(settings.value(
                QStringLiteral("Recovery/LastAttemptUtcMs")).toLongLong()))
            .arg(attemptPath.isEmpty() ? QStringLiteral("None") : attemptPath)
            .arg(settings.value(QStringLiteral("Recovery/LastAttemptRevision"), 0)
                     .toULongLong())
            .arg(managedAttempt ? QStringLiteral("Valid") : QStringLiteral("Invalid/none"))
            .arg(checksumValid ? QStringLiteral("Valid") : QStringLiteral("Invalid/none"))
            .arg(formatUtc(settings.value(
                QStringLiteral("Recovery/LastSuccessUtcMs")).toLongLong()))
            .arg(successPath.isEmpty() ? QStringLiteral("None") : successPath)
            .arg(settings.value(QStringLiteral("Recovery/LastSuccessRevision"), 0)
                     .toULongLong())
            .arg(settings.value(QStringLiteral("Recovery/ConsecutiveFailures"), 0).toInt())
            .arg(lastFailureReason.isEmpty() ? QStringLiteral("None")
                                             : lastFailureReason)
            .arg(storage.ready ? QStringLiteral("Yes") : QStringLiteral("No"))
            .arg(storage.readOnly ? QStringLiteral("Yes") : QStringLiteral("No"))
            .arg(availableStorage)
            .arg(storage.detail)
            .arg(m_recoveryRetryTimer && m_recoveryRetryTimer->isActive()
                     ? QStringLiteral("Yes") : QStringLiteral("No"))
            .arg(m_recoveryRetryTimer && m_recoveryRetryTimer->isActive()
                     ? std::max(0, m_recoveryRetryTimer->remainingTime() / 1000) : 0);
        QMessageBox::information(this, QStringLiteral("Recovery Status"), report);
    });
    m_commandRegistry->registerAction(QStringLiteral("project.recovery-status"),
        recoveryStatusAction, QStringLiteral("Project"),
        QStringLiteral("Show recovery checkpoint health and telemetry"));

    QAction* recoveryStorageTestAction = recoveryMenu->addAction(
        QStringLiteral("Test Recovery Storage..."));
    recoveryStorageTestAction->setObjectName(
        QStringLiteral("actionTestRecoveryStorage"));
    connect(recoveryStorageTestAction, &QAction::triggered, this, [this]() {
        const RecoveryStorageStatus storage = inspectRecoveryStorage(0, true);
        const QString result = QStringLiteral("%1\nAvailable: %2 GiB")
            .arg(storage.detail)
            .arg(storage.bytesAvailable >= 0
                     ? static_cast<double>(storage.bytesAvailable) /
                           (1024.0 * 1024.0 * 1024.0)
                     : 0.0, 0, 'f', 2);
        if (storage.ready && storage.probeSucceeded) {
            QMessageBox::information(this, QStringLiteral("Recovery Storage Test"), result);
        } else {
            QMessageBox::critical(this, QStringLiteral("Recovery Storage Test"), result);
        }
    });
    m_commandRegistry->registerAction(QStringLiteral("project.recovery-storage-test"),
        recoveryStorageTestAction, QStringLiteral("Project"),
        QStringLiteral("Run a temporary write/read test on recovery storage"));

    QAction* checkpointNowAction = recoveryMenu->addAction(
        QStringLiteral("Create Checkpoint Now"));
    checkpointNowAction->setObjectName(QStringLiteral("actionCreateRecoveryCheckpoint"));
    connect(checkpointNowAction, &QAction::triggered, this, [this]() {
        if (!m_projectDirty) {
            ui.statusBar->showMessage(QStringLiteral(
                "Project is clean; no recovery checkpoint is needed."), 3500);
            return;
        }
        QString error;
        if (!writeRecoverySnapshotNow(&error)) {
            QMessageBox::critical(this, QStringLiteral("Recovery Checkpoint"),
                error.isEmpty() ? QStringLiteral("Could not create recovery checkpoint.") : error);
            return;
        }
        ui.statusBar->showMessage(QStringLiteral(
            "Recovery checkpoint committed with SHA-256."), 4500);
    });
    m_commandRegistry->registerAction(QStringLiteral("project.recovery-checkpoint"),
        checkpointNowAction, QStringLiteral("Project"),
        QStringLiteral("Create a recovery checkpoint immediately"));
    updateProjectWindowTitle();
}

void VeraCoreQT::updateProjectWindowTitle()
{
    const QString name = m_projectPath.isEmpty()
        ? QString("Untitled")
        : QFileInfo(m_projectPath).completeBaseName();
    QString integrity;
    if (m_projectIntegrityState == ProjectIntegrityState::Verified)
        integrity = QStringLiteral(" [SHA-256 Verified]");
    else if (m_projectIntegrityState == ProjectIntegrityState::LegacyUnverified)
        integrity = QStringLiteral(" [Legacy/Unverified]");
    setWindowTitle(QStringLiteral("VeraCore - %1%2%3")
        .arg(name, integrity, m_projectDirty ? QStringLiteral(" *") : QString{}));
}

void VeraCoreQT::setProjectDirty(bool dirty)
{
    if (m_restoringProject) return;
    if (dirty) {
        m_nonArrangementDirty = true;
        ++m_projectRevision;
    } else {
        m_araDocumentDirty = false;
        m_nonArrangementDirty = false;
        if (m_playlistView) m_playlistView->markArrangementCleanState();
    }
    const bool arrangementDirty = m_playlistView && !m_playlistView->isArrangementClean();
    const bool nextDirty = m_nonArrangementDirty || arrangementDirty;
    if (m_projectDirty == nextDirty) {
        updateProjectWindowTitle();
        return;
    }
    m_projectDirty = nextDirty;
    updateProjectWindowTitle();
}

void VeraCoreQT::refreshProjectDirtyFromArrangement(bool contentMutation)
{
    if (m_restoringProject) return;
    if (contentMutation) ++m_projectRevision;
    const bool arrangementDirty = m_playlistView && !m_playlistView->isArrangementClean();
    m_projectDirty = m_nonArrangementDirty || arrangementDirty;
    updateProjectWindowTitle();
}

bool VeraCoreQT::saveProjectInteractive(bool forceSaveAs)
{
    QString path = m_projectPath;
    if (forceSaveAs || path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, "Save VeraCore Project", path,
                                            "VeraCore project (*.vcp)");
        if (path.isEmpty()) return false;
        path = withRequiredSuffix(path, ".vcp");
    }
    QString error;
    if (!saveProjectFile(path, &error)) {
        QMessageBox::critical(this, "Save Project", error.isEmpty() ? "Could not save project." : error);
        return false;
    }
    m_projectPath = path;
    setProjectDirty(false);
    markRecoveryStateClean();
    updateProjectWindowTitle();
    ui.statusBar->showMessage("Project saved · SHA-256 verified.", 3500);
    return true;
}

bool VeraCoreQT::confirmDiscardChanges()
{
    if (!m_projectDirty) return true;
    const QMessageBox::StandardButton choice = QMessageBox::warning(
        this, "Unsaved Project", "Save changes before continuing?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (choice == QMessageBox::Cancel) return false;
    if (choice == QMessageBox::Save) return saveProjectInteractive(false);
    return choice == QMessageBox::Discard;
}

void VeraCoreQT::keyPressEvent(QKeyEvent* event)
{
    QMainWindow::keyPressEvent(event);
}

bool VeraCoreQT::eventFilter(QObject* watched, QEvent* event)
{
    if (event && (event->type() == QEvent::KeyPress ||
                  event->type() == QEvent::MouseButtonPress ||
                  event->type() == QEvent::Wheel ||
                  event->type() == QEvent::TouchBegin)) {
        m_recoveryIdleClock.restart();
    }
    if (event && event->type() == QEvent::KeyPress) {
        QWidget* activeWindow = QApplication::activeWindow();
        const bool transportContext = activeWindow == this ||
            (m_midiEditor && activeWindow == m_midiEditor);
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        QWidget* focus = QApplication::focusWidget();
        const bool editingValue = qobject_cast<QLineEdit*>(focus) ||
            qobject_cast<QAbstractSpinBox*>(focus) || qobject_cast<QComboBox*>(focus);
        if (transportContext && !keyEvent->isAutoRepeat()) {
            const QKeySequence sequence(keyEvent->keyCombination());
            const QString commandId = m_commandRegistry
                ? m_commandRegistry->shortcutOwner(sequence) : QString{};
            const bool allowedWhileEditing = commandId.startsWith(QStringLiteral("view.")) ||
                commandId.startsWith(QStringLiteral("workspace.")) ||
                commandId == QStringLiteral("project.save") ||
                commandId == QStringLiteral("project.save-as");
            if (!commandId.isEmpty() && (!editingValue || allowedWhileEditing) &&
                m_commandRegistry->trigger(commandId)) return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void VeraCoreQT::closeEvent(QCloseEvent* event)
{
    if (confirmDiscardChanges()) {
        if (ui.browserPanel->isVisible()) m_lastMainSplitterSizes = ui.mainSplitter->sizes();
        if (ui.playlistSplitter->isVisible()) m_lastPlaylistSplitterSizes = ui.playlistSplitter->sizes();
        if (ui.playlistSplitter->isVisible() && m_detailTabs->isVisible())
            m_lastCenterSplitterSizes = ui.centerSplitter->sizes();

        auto storedSizes = [](const QList<int>& sizes) {
            QVariantList stored;
            stored.reserve(sizes.size());
            for (int size : sizes) stored.push_back(size);
            return stored;
        };
        QSettings workspaceSettings;
        workspaceSettings.beginGroup(QStringLiteral("Workspace"));
        workspaceSettings.setValue(QStringLiteral("MainWindowGeometry"), saveGeometry());
        workspaceSettings.setValue(QStringLiteral("MainWindowWasMaximized"), isMaximized());
        workspaceSettings.setValue(QStringLiteral("MainSplitterSizes"), storedSizes(m_lastMainSplitterSizes));
        workspaceSettings.setValue(QStringLiteral("CenterSplitterSizes"), storedSizes(m_lastCenterSplitterSizes));
        workspaceSettings.setValue(QStringLiteral("PlaylistSplitterSizes"), storedSizes(m_lastPlaylistSplitterSizes));
        workspaceSettings.setValue(QStringLiteral("BrowserVisible"), ui.browserPanel->isVisible());
        workspaceSettings.setValue(QStringLiteral("PlaylistVisible"), ui.playlistSplitter->isVisible());
        workspaceSettings.setValue(QStringLiteral("InspectorVisible"), m_detailTabs->isVisible());
        workspaceSettings.endGroup();
        workspaceSettings.sync();
        markRecoveryStateClean();
        event->accept();
    } else {
        event->ignore();
    }
}

void VeraCoreQT::resetProjectModel(bool createDefaultTracks)
{
    onTransportStop();
    m_projectIntegrityState = ProjectIntegrityState::Unsaved;
    if (m_selectionService) m_selectionService->clearAll();
    if (m_midiEditor) m_midiEditor->close();
    m_midiClipDocs.clear();
    m_audioDecodeCache.clear();
    m_audioSyncDecodeCache.clear();
    m_retrospectiveMidiBuffer.clear();
    clearStepMidiChord();
    m_midiStepInputEnabled = false;
    if (m_midiStepInputAction) {
        QSignalBlocker blocker(m_midiStepInputAction);
        m_midiStepInputAction->setChecked(false);
    }
    m_midiRecordingBeforeDocument.reset();
    m_midiRecordingContentId.clear();
    m_midiRecordingClipStartBeat = 0.0;
    m_midiRecordingClipLengthBeats = 0.0;
    m_midiRecordingClipSourceOffsetBeats = 0.0;
    m_midiRecordingRangeStartBeat = 0.0;
    m_midiRecordingSawLoopWrap = false;
    m_midiRecordingAddedNotes.clear();
    m_midiRecordingAddedEvents.clear();
    m_midiRecordingBeforeStateId = 0;
    m_midiRecordingNonArrangementDirtyBefore = false;
    m_midiRecordingChanged = false;
    m_midiRecordingUndoActive = false;
    m_audioCacheUseCounter = 0;
    m_arrangementProjectTransitions.clear();
    m_projectUndoTransitions.clear();
    m_projectAraArchives.clear();
    m_projectAraArchivesRestored = true;
    m_projectPassthroughRoot = {};
    m_missingMediaCount = 0;
    m_autoRelinkedMediaCount = 0;
    for (TrackWidget* widget : m_trackWidgets) {
        delete widget;
    }
    m_trackWidgets.clear();
    m_tracks.clear();
    resetReturnBuses();
    if (m_playlistView) {
        m_playlistView->clearAllClips();
        m_playlistView->setTracks(m_tracks);
    }
    m_selectedTrackIndex = -1;
    m_currentMidiTrackIndex = -1;
    m_currentMidiClipKey.clear();
    m_currentMidiClipId.clear();
    m_currentMidiClipName.clear();
    m_currentMidiClipStartBeat = 0.0;
    m_currentMidiClipLengthBeats = 0.0;
    m_currentMidiClipSourceOffsetBeats = 0.0;
    m_hasCurrentMidiClip = false;
    m_loopEnabled = false;
    m_loopStartBeat = 0.0;
    m_loopEndBeat = 4.0;
    m_punchEnabled = false;
    m_punchInBeat = 0.0;
    m_punchOutBeat = 4.0;
    m_countInBars = 0;
    m_midiRecordMode = 0;
    m_midiRecordQuantizeBeats = 0.0;
    for (const auto& entry : m_midiRecordModeActions)
        entry.second->setChecked(entry.first == m_midiRecordMode);
    for (const auto& entry : m_midiRecordQuantizeActions)
        entry.second->setChecked(
            std::fabs(entry.first - m_midiRecordQuantizeBeats) < 1.0e-9);
    m_recordingCountInEndBeat = -1.0;
    m_recordingCountInComplete = true;
    restoreCountInMetronome();
    m_projectKeyRoot = 0;
    m_projectMinorScale = false;
    m_projectSnapBeats = 0.25;
    m_projectPpq = kDefaultProjectPpq;
    applyProjectPanLawDb(-3.0);
    applyProjectSpeakerLayout(VeraCoreSpeakerLayout::Stereo);
    m_tempoMap = {{0.0, m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm}};
    m_timeSignatureMap = {{0.0,
        m_transportWidget ? m_transportWidget->signatureNumerator() : 4,
        m_transportWidget ? m_transportWidget->signatureDenominator() : 4}};
    if (m_transportWidget) {
        QSignalBlocker blocker(m_transportWidget);
        m_transportWidget->setKeyScale(m_projectKeyRoot, m_projectMinorScale);
        m_transportWidget->setSnapBeats(m_projectSnapBeats);
        m_transportWidget->setPpq(m_projectPpq);
        m_transportWidget->setPunchEnabled(false);
        m_transportWidget->setCountInBars(0);
    }
    if (m_playlistView) m_playlistView->setSnapBeats(m_projectSnapBeats);
    if (m_playlistView) m_playlistView->setLoopRange(false, 0.0, 0.0);
    if (m_playlistView) m_playlistView->setPunchRange(false, 0.0, 0.0);
    if (audioGeneral) audioGeneral->audioCore.SetLoopRange(false, 0.0, 0.0);
    m_parameterEditorTrackIndex = -1;
    m_parameterEditorReturnBus = -1;
    m_parameterEditorSlotIndex = -1;
    m_parameterEditorGestureDepth = 0;
    m_parameterEditorDeviceName.clear();
    m_hasAutomationTarget = false;
    m_automationParameterId = 0;
    m_automationParameterName.clear();
    m_automationWriteActive = false;
    m_automationWriteOwnsHostGesture = false;
    m_automationWriteTrackIndex = -1;
    m_automationWriteAudioFx = false;
    m_automationWriteSlotIndex = -1;
    m_automationWriteInstanceEpoch = 0;
    m_automationWriteParameterId = 0;
    m_automationWriteParameterName.clear();
    m_automationWriteRanges.clear();
    m_automationWriteLastBeat = -1.0;
    m_automationWriteRangeOverflow = false;
    m_automationWriteBufferOverflow = false;
    m_automationWriteStreamOverflow = false;
    m_automationWriteBuffer.clear();
    if (m_playlistView) m_playlistView->clearPluginAutomationLane();
    if (m_inspectorWidget) m_inspectorWidget->clearActiveAutomationTarget();
    if (m_inspectorWidget) m_inspectorWidget->setTrack(nullptr);
    if (createDefaultTracks) initDefaultTracks();
    syncRealtimeMidiLayersToAudio();
    syncMixerRoutingToAudio();
    syncTempoMapToAudio();
    syncTimeSignatureMapToAudio();
    syncMidiTimelineToAudio();
    syncAudioTimelineToAudio();
    syncPluginTransportToHost();
}

bool VeraCoreQT::serializeProject(QByteArray& json, bool capturePluginState,
                                  bool recoverySnapshot, QString* errorText)
{
    json.clear();
    QJsonObject root;
    root["format"] = "VeraCoreProject";
    root["version"] = 16;
    if (recoverySnapshot) {
        root["recoverySnapshot"] = true;
        root["recoverySourcePath"] = m_projectPath;
        root["recoveryCreatedUtcMs"] = QDateTime::currentMSecsSinceEpoch();
    }

    if (capturePluginState && m_pluginHost &&
        (m_projectAraArchivesRestored || m_projectAraArchives.empty())) {
        std::vector<AraDocumentArchive> captured;
        if (!m_pluginHost->captureAraArchives(captured)) {
            if (errorText) *errorText = "Could not capture ARA document state.";
            return false;
        }
        try {
            std::vector<StoredAraArchive> next;
            next.reserve(captured.size());
            qint64 totalBytes = 0;
            for (const AraDocumentArchive& archive : captured) {
                if (archive.factoryPersistentId.empty() || archive.documentArchiveId.empty() ||
                    archive.factoryPersistentId.size() > kMaxAraPersistentIdBytes ||
                    archive.documentArchiveId.size() > kMaxAraPersistentIdBytes ||
                    archive.bytes.size() > static_cast<size_t>(kMaxAraArchiveBytes)) {
                    if (errorText) *errorText = "ARA archive exceeds project persistence limits.";
                    return false;
                }
                totalBytes += static_cast<qint64>(archive.bytes.size());
                if (totalBytes > kMaxProjectAraArchiveBytes) {
                    if (errorText) *errorText = "Project ARA state exceeds 256 MB.";
                    return false;
                }
                StoredAraArchive stored;
                stored.factoryPersistentIdUtf8 = QByteArray(
                    archive.factoryPersistentId.data(),
                    static_cast<qsizetype>(archive.factoryPersistentId.size()));
                stored.documentArchiveIdUtf8 = QByteArray(
                    archive.documentArchiveId.data(),
                    static_cast<qsizetype>(archive.documentArchiveId.size()));
                stored.bytes = QByteArray(reinterpret_cast<const char*>(archive.bytes.data()),
                    static_cast<qsizetype>(archive.bytes.size()));
                next.push_back(std::move(stored));
            }
            m_projectAraArchives.swap(next);
            m_projectAraArchivesRestored = true;
        } catch (...) {
            if (errorText) *errorText = "Not enough memory to capture ARA document state.";
            return false;
        }
    }

    QJsonArray araArchives;
    qint64 totalAraBytes = 0;
    if (m_projectAraArchives.size() > static_cast<size_t>(kMaxAraArchives)) {
        if (errorText) *errorText = "Project contains too many ARA archives.";
        return false;
    }
    for (const StoredAraArchive& archive : m_projectAraArchives) {
        if (archive.factoryPersistentIdUtf8.isEmpty() || archive.documentArchiveIdUtf8.isEmpty() ||
            archive.factoryPersistentIdUtf8.size() > kMaxAraPersistentIdBytes ||
            archive.documentArchiveIdUtf8.size() > kMaxAraPersistentIdBytes ||
            archive.factoryPersistentIdUtf8.contains('\0') ||
            archive.documentArchiveIdUtf8.contains('\0') ||
            archive.bytes.size() > kMaxAraArchiveBytes) {
            if (errorText) *errorText = "ARA archive cache is invalid or oversized.";
            return false;
        }
        const QString factoryId = QString::fromUtf8(archive.factoryPersistentIdUtf8);
        const QString documentArchiveId = QString::fromUtf8(archive.documentArchiveIdUtf8);
        if (factoryId.toUtf8() != archive.factoryPersistentIdUtf8 ||
            documentArchiveId.toUtf8() != archive.documentArchiveIdUtf8) {
            if (errorText) *errorText = "ARA archive IDs are not valid UTF-8.";
            return false;
        }
        totalAraBytes += archive.bytes.size();
        if (totalAraBytes > kMaxProjectAraArchiveBytes) {
            if (errorText) *errorText = "Project ARA state exceeds 256 MB.";
            return false;
        }
        QJsonObject item;
        item["factoryId"] = factoryId;
        item["archiveId"] = documentArchiveId;
        item["data"] = QString::fromLatin1(archive.bytes.toBase64());
        araArchives.append(item);
    }
    root["araArchives"] = araArchives;
    QJsonObject transport;
    const double baseProjectTempo = tempoAtBeat(
        m_tempoMap, 0.0, m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm);
    transport["tempo"] = baseProjectTempo;
    QJsonArray tempoMap;
    for (const MidiTempoPoint& point : normalizedTempoMap(
             m_tempoMap, baseProjectTempo)) {
        QJsonObject tempoPoint;
        tempoPoint["beat"] = point.beat;
        tempoPoint["bpm"] = point.bpm;
        tempoMap.push_back(tempoPoint);
    }
    transport["tempoMap"] = tempoMap;
    QJsonArray signatureMap;
    const std::vector<MidiTimeSignaturePoint> savedSignatureMap = normalizedTimeSignatureMap(
        m_timeSignatureMap,
        m_transportWidget ? m_transportWidget->signatureNumerator() : 4,
        m_transportWidget ? m_transportWidget->signatureDenominator() : 4);
    for (const MidiTimeSignaturePoint& point : savedSignatureMap) {
        QJsonObject signaturePoint;
        signaturePoint["beat"] = point.beat;
        signaturePoint["numerator"] = point.numerator;
        signaturePoint["denominator"] = point.denominator;
        signatureMap.push_back(signaturePoint);
    }
    transport["timeSignatureMap"] = signatureMap;
    transport["numerator"] = savedSignatureMap.front().numerator;
    transport["denominator"] = savedSignatureMap.front().denominator;
    transport["keyRoot"] = m_projectKeyRoot;
    transport["minor"] = m_projectMinorScale;
    transport["loopEnabled"] = m_loopEnabled;
    transport["loopStart"] = m_loopStartBeat;
    transport["loopEnd"] = m_loopEndBeat;
    transport["punchEnabled"] = m_punchEnabled;
    transport["punchIn"] = m_punchInBeat;
    transport["punchOut"] = m_punchOutBeat;
    transport["countInBars"] = m_countInBars;
    transport["midiRecordMode"] = m_midiRecordMode;
    transport["midiRecordQuantizeBeats"] = m_midiRecordQuantizeBeats;
    transport["snapBeats"] = m_projectSnapBeats;
    transport["ppq"] = m_projectPpq;
    root["transport"] = transport;

    QJsonArray tracks;
    qint64 totalPluginStateBytes = 0;
    qint64 totalPluginAutomationPoints = 0;
    for (int trackIndex = 0; trackIndex < static_cast<int>(m_tracks.size()); ++trackIndex) {
        const auto& track = m_tracks[trackIndex];
        if (!track) {
            if (errorText) *errorText = "Project contains an invalid empty track.";
            return false;
        }
        QJsonObject object;
        object["name"] = track->GetName();
        object["type"] = track->GetType() == TrackType::Midi ? "midi" : "audio";
        object["volume"] = track->GetVolume();
        object["pan"] = track->GetPan();
        object["stereoSeparation"] = track->GetStereoSeparation();
        const VeraCoreSpatialPannerState& spatialState = track->GetSpatialPanner();
        QJsonObject spatial;
        spatial["mode"] = static_cast<int>(spatialState.mode);
        spatial["azimuth"] = spatialState.azimuthDegrees;
        spatial["elevation"] = spatialState.elevationDegrees;
        spatial["distance"] = spatialState.distance;
        spatial["spread"] = spatialState.spread;
        spatial["lfeSend"] = spatialState.lfeSend;
        spatial["linkStereoPan"] = spatialState.linkStereoPan;
        object["spatial"] = spatial;
        object["muted"] = track->IsMuted();
        object["soloed"] = track->IsSoloed();
        object["armed"] = track->IsArmed();
        object["monitoring"] = track->IsMonitoring();
        QJsonArray sends;
        for (int bus = 0; bus < kReturnBusCount; ++bus) sends.append(track->GetSendLevel(bus));
        object["sends"] = sends;
        QJsonArray fx;
        const auto& audioFxSlots = track->GetAudioFxSlots();
        for (int fxIndex = 0; fxIndex < static_cast<int>(audioFxSlots.size()); ++fxIndex) {
            const AudioFxSlot& slot = audioFxSlots[fxIndex];
            QJsonObject item;
            item["name"] = slot.name;
            item["pluginPath"] = slot.pluginPath;
            item["pluginUid"] = slot.pluginUid;
            item["pluginType"] = slot.pluginType;
            item["enabled"] = slot.enabled;
            totalPluginAutomationPoints += pluginAutomationPointCount(slot.automation);
            if (totalPluginAutomationPoints > kMaxProjectPluginAutomationPoints) {
                if (errorText) *errorText = "Project plugin automation exceeds four million points.";
                return false;
            }
            item["automation"] = serializePluginAutomation(slot.automation);
            QByteArray pluginState = capturePluginState && m_pluginHost
                ? m_pluginHost->captureAudioFxState(trackIndex, fxIndex)
                : QByteArray{};
            if (pluginState.isEmpty()) pluginState = slot.pluginState;
            else track->SetAudioFxPluginState(fxIndex, pluginState);
            if (pluginState.size() > kMaxPluginStateBytes ||
                totalPluginStateBytes + pluginState.size() > kMaxProjectPluginStateBytes) {
                if (errorText) *errorText = "Plugin state exceeds project persistence limits.";
                return false;
            }
            totalPluginStateBytes += pluginState.size();
            if (!pluginState.isEmpty()) item["pluginState"] = QString::fromLatin1(pluginState.toBase64());
            fx.append(item);
        }
        object["audioFx"] = fx;
        if (auto* midi = dynamic_cast<MidiTrack*>(track.get())) {
            QJsonArray layers;
            const auto& layerSlots = midi->GetLayerSlots();
            for (int layerIndex = 0; layerIndex < static_cast<int>(layerSlots.size()); ++layerIndex) {
                const LayerSlot& layer = layerSlots[layerIndex];
                QJsonObject item;
                item["name"] = layer.name;
                item["pluginPath"] = layer.pluginPath;
                item["pluginUid"] = layer.pluginUid;
                item["pluginType"] = layer.pluginType;
                item["enabled"] = layer.enabled;
                item["gain"] = layer.gain;
                item["pan"] = layer.pan;
                item["transpose"] = layer.transpose;
                item["midiChannel"] = layer.midiChannel;
                totalPluginAutomationPoints += pluginAutomationPointCount(layer.automation);
                if (totalPluginAutomationPoints > kMaxProjectPluginAutomationPoints) {
                    if (errorText) *errorText = "Project plugin automation exceeds four million points.";
                    return false;
                }
                item["automation"] = serializePluginAutomation(layer.automation);
                QJsonArray outputRoutes;
                for (const PluginOutputRoute& route : layer.outputRoutes) {
                    QJsonObject routeData;
                    routeData["stablePortId"] = route.stablePortId;
                    routeData["name"] = route.displayName;
                    routeData["bus"] = route.outputBusIndex;
                    routeData["destinationTrack"] = route.destinationTrackIndex;
                    routeData["enabled"] = route.enabled;
                    outputRoutes.append(routeData);
                }
                item["outputRoutes"] = outputRoutes;
                QByteArray pluginState = capturePluginState && m_pluginHost
                    ? m_pluginHost->captureLayerState(trackIndex * kLayerIdStride + layerIndex)
                    : QByteArray{};
                if (pluginState.isEmpty()) pluginState = layer.pluginState;
                else midi->SetLayerSlotPluginState(layerIndex, pluginState);
                if (pluginState.size() > kMaxPluginStateBytes ||
                    totalPluginStateBytes + pluginState.size() > kMaxProjectPluginStateBytes) {
                    if (errorText) *errorText = "Plugin state exceeds project persistence limits.";
                    return false;
                }
                totalPluginStateBytes += pluginState.size();
                if (!pluginState.isEmpty()) item["pluginState"] = QString::fromLatin1(pluginState.toBase64());
                layers.append(item);
            }
            object["layers"] = layers;
        }
        tracks.append(object);
    }
    root["tracks"] = tracks;

    QJsonArray returns;
    for (int returnBus = 0; returnBus < kReturnBusCount; ++returnBus) {
        ReturnBusState bus = m_mixerParameters->returnBuses()[static_cast<std::size_t>(returnBus)];
        QJsonObject object;
        object["name"] = bus.name;
        object["volume"] = bus.volume;
        object["pan"] = bus.pan;
        object["stereoSeparation"] = bus.stereoSeparation;
        object["muted"] = bus.muted;
        object["soloed"] = bus.soloed;
        object["preFader"] = bus.preFader;
		QJsonArray fx;
		for (int fxIndex = 0; fxIndex < static_cast<int>(bus.audioFxSlots.size()); ++fxIndex) {
			AudioFxSlot& slot = bus.audioFxSlots[static_cast<std::size_t>(fxIndex)];
			QJsonObject item;
			item["name"] = slot.name;
			item["pluginPath"] = slot.pluginPath;
			item["pluginUid"] = slot.pluginUid;
			item["pluginType"] = slot.pluginType;
			item["enabled"] = slot.enabled;
			totalPluginAutomationPoints += pluginAutomationPointCount(slot.automation);
			if (totalPluginAutomationPoints > kMaxProjectPluginAutomationPoints) {
				if (errorText) *errorText = "Project plugin automation exceeds four million points.";
				return false;
			}
			item["automation"] = serializePluginAutomation(slot.automation);
			QByteArray pluginState = capturePluginState && m_pluginHost
				? m_pluginHost->captureAudioFxState(
					VeraCoreReturnAudioFxTrackId(returnBus), fxIndex)
				: QByteArray{};
			if (pluginState.isEmpty()) pluginState = slot.pluginState;
			if (pluginState.size() > kMaxPluginStateBytes ||
				totalPluginStateBytes + pluginState.size() > kMaxProjectPluginStateBytes) {
				if (errorText) *errorText = "Plugin state exceeds project persistence limits.";
				return false;
			}
			totalPluginStateBytes += pluginState.size();
			if (!pluginState.isEmpty())
				item["pluginState"] = QString::fromLatin1(pluginState.toBase64());
			fx.append(item);
		}
		object["audioFx"] = fx;
        returns.append(object);
    }
    root["returns"] = returns;
    QJsonObject master;
    master["volume"] = m_mixerParameters->masterBus().volume;
    master["pan"] = m_mixerParameters->masterBus().pan;
    master["stereoSeparation"] = m_mixerParameters->masterBus().stereoSeparation;
    master["muted"] = m_mixerParameters->masterBus().muted;
    QJsonArray masterFx;
    const auto& masterFxSlots = m_mixerParameters->masterBus().audioFxSlots;
    for (int fxIndex = 0; fxIndex < static_cast<int>(masterFxSlots.size()); ++fxIndex) {
        const AudioFxSlot& slot = masterFxSlots[static_cast<std::size_t>(fxIndex)];
        QJsonObject item;
        item["name"] = slot.name;
        item["pluginPath"] = slot.pluginPath;
        item["pluginUid"] = slot.pluginUid;
        item["pluginType"] = slot.pluginType;
        item["enabled"] = slot.enabled;
        totalPluginAutomationPoints += pluginAutomationPointCount(slot.automation);
        if (totalPluginAutomationPoints > kMaxProjectPluginAutomationPoints) {
            if (errorText) *errorText = "Project plugin automation exceeds four million points.";
            return false;
        }
        item["automation"] = serializePluginAutomation(slot.automation);
        QByteArray pluginState = capturePluginState && m_pluginHost
            ? m_pluginHost->captureAudioFxState(VeraCoreMasterAudioFxTrackId, fxIndex)
            : QByteArray{};
        if (pluginState.isEmpty()) pluginState = slot.pluginState;
        if (pluginState.size() > kMaxPluginStateBytes ||
            totalPluginStateBytes + pluginState.size() > kMaxProjectPluginStateBytes) {
            if (errorText) *errorText = "Plugin state exceeds project persistence limits.";
            return false;
        }
        totalPluginStateBytes += pluginState.size();
        if (!pluginState.isEmpty())
            item["pluginState"] = QString::fromLatin1(pluginState.toBase64());
        masterFx.append(item);
    }
    master["audioFx"] = masterFx;
    root["master"] = master;
    QJsonObject mixer;
    mixer["panLawDb"] = m_projectPanLawDb;
    mixer["speakerLayout"] = static_cast<int>(m_projectSpeakerLayout);
    root["mixer"] = mixer;

    QJsonArray midiClips;
    std::set<QString> referencedMidiContents;
    std::set<QString> serializedClipIds;
    if (m_playlistView) {
        for (const auto& clip : m_playlistView->getMidiClips()) {
            if (clip.clipId.trimmed().isEmpty() || clip.contentId.trimmed().isEmpty() ||
                !serializedClipIds.insert(clip.clipId).second) {
                if (errorText) *errorText = "MIDI clip identity is empty or duplicated.";
                return false;
            }
            QJsonObject item;
            item["clipId"] = clip.clipId;
            item["contentId"] = clip.contentId;
            item["track"] = clip.trackIndex;
            item["start"] = clip.startBeat;
            item["length"] = clip.lengthBeats;
            item["offset"] = clip.sourceOffsetBeats;
            item["loopEnabled"] = clip.loopEnabled;
            if (clip.loopEnabled) {
                item["loopStart"] = clip.loopStartBeats;
                item["loopLength"] = clip.loopLengthBeats;
            }
            item["muted"] = clip.muted;
            if (!clip.groupId.isEmpty()) item["groupId"] = clip.groupId;
            if (!clip.takeGroupId.isEmpty()) {
                item["takeGroupId"] = clip.takeGroupId;
                item["takeLane"] = clip.takeLane;
                item["takeActive"] = clip.takeActive;
            }
            item["locked"] = clip.locked;
            if (clip.clipColor.isValid()) item["color"] = clip.clipColor.name(QColor::HexRgb);
            midiClips.append(item);
            referencedMidiContents.insert(clip.contentId);
        }
    }
    root["midiClips"] = midiClips;

    QJsonArray audioClips;
    std::map<QString, QJsonObject> audioContentObjects;
    if (m_playlistView) {
        for (const auto& clip : m_playlistView->getAudioClips()) {
            if (clip.clipId.trimmed().isEmpty() || clip.contentId.trimmed().isEmpty() ||
                !serializedClipIds.insert(clip.clipId).second) {
                if (errorText) *errorText = "Audio clip identity is empty or duplicated.";
                return false;
            }
            QJsonObject item;
            item["clipId"] = clip.clipId;
            item["contentId"] = clip.contentId;
            item["track"] = clip.trackIndex;
            item["start"] = clip.startBeat;
            item["length"] = clip.lengthBeats;
            item["offset"] = clip.sourceOffsetBeats;
            item["loopEnabled"] = clip.loopEnabled;
            if (clip.loopEnabled) {
                item["loopStart"] = clip.loopStartBeats;
                item["loopLength"] = clip.loopLengthBeats;
            }
            item["warpLength"] = clip.warpLengthBeats;
            item["fadeIn"] = clip.fadeInBeats;
            item["fadeOut"] = clip.fadeOutBeats;
            item["fadeInCurve"] = clip.fadeInCurve;
            item["fadeOutCurve"] = clip.fadeOutCurve;
            item["muted"] = clip.muted;
            if (!clip.groupId.isEmpty()) item["groupId"] = clip.groupId;
            if (!clip.takeGroupId.isEmpty()) {
                item["takeGroupId"] = clip.takeGroupId;
                item["takeLane"] = clip.takeLane;
                item["takeActive"] = clip.takeActive;
            }
            item["locked"] = clip.locked;
            if (clip.clipColor.isValid()) item["color"] = clip.clipColor.name(QColor::HexRgb);
            QJsonArray gainEnvelope;
            for (const AudioGainEnvelopePoint& point : clip.gainEnvelope) {
                QJsonObject envelopePoint;
                envelopePoint["beat"] = point.beat;
                envelopePoint["gain"] = point.gain;
                gainEnvelope.append(envelopePoint);
            }
            item["gainEnvelope"] = gainEnvelope;
            audioClips.append(item);

            QJsonObject content;
            content["contentId"] = clip.contentId;
            content["name"] = clip.name;
            content["sourceTempo"] = clip.sourceTempoBpm;
            content["tempoConfidence"] = clip.tempoConfidence;
            content["sourceSignatureNumerator"] = clip.sourceSignatureNumerator;
            content["sourceSignatureDenominator"] = clip.sourceSignatureDenominator;
            content["tempoDetected"] = clip.tempoDetected;
            QJsonArray transientMarkers;
            for (double marker : clip.transientSeconds) transientMarkers.append(marker);
            content["transientSeconds"] = transientMarkers;
            QJsonArray warpMarkers;
            for (const AudioWarpMarker& marker : clip.warpMarkers) {
                QJsonObject point;
                point["sourceSeconds"] = marker.sourceSeconds;
                point["targetBeat"] = marker.targetBeat;
                warpMarkers.append(point);
            }
            content["warpMarkers"] = warpMarkers;
            content["pitch"] = clip.pitchShiftSemitones;
            content["syncMode"] = clip.syncMode;
            content["targetBars"] = clip.targetBars;
            content["beatsPerBar"] = clip.beatsPerBar;
            content["sourceDuration"] = clip.sourceDurationSeconds;
            content["gain"] = clip.gain;
            content["pan"] = clip.pan;
            content["normalize"] = clip.normalize;
            content["reverse"] = clip.reverse;
            content["reversePolarity"] = clip.reversePolarity;
            content["sourcePath"] = clip.sourcePath;
            audioContentObjects[clip.contentId] = content;
        }
    }
    root["audioClips"] = audioClips;
    QJsonArray audioContents;
    for (const auto& entry : audioContentObjects) audioContents.append(entry.second);
    root["audioContents"] = audioContents;

    QJsonArray documents;
    for (const auto& entry : m_midiClipDocs) {
        if (referencedMidiContents.find(entry.first) == referencedMidiContents.end()) continue;
        const MidiClipDocument* doc = entry.second.get();
        if (!doc) continue;
        QJsonObject object;
        object["contentId"] = entry.first;
        object["name"] = doc->name;
        object["trackName"] = doc->trackName;
        object["track"] = doc->trackIndex;
        object["start"] = doc->startBeat;
        object["length"] = doc->lengthBeats;
        object["lastNoteLength"] = doc->lastNoteLengthBeats;
        object["tempo"] = doc->tempoBpm;
        QJsonArray documentTempoMap;
        for (const MidiTempoPoint& point : normalizedTempoMap(doc->tempoMap, doc->tempoBpm)) {
            QJsonObject tempoPoint;
            tempoPoint["beat"] = point.beat;
            tempoPoint["bpm"] = point.bpm;
            documentTempoMap.push_back(tempoPoint);
        }
        object["tempoMap"] = documentTempoMap;
        QJsonArray documentSignatureMap;
        for (const MidiTimeSignaturePoint& point : normalizedTimeSignatureMap(
                 doc->timeSignatureMap, doc->timeSignatureNumerator,
                 doc->timeSignatureDenominator)) {
            QJsonObject signaturePoint;
            signaturePoint["beat"] = point.beat;
            signaturePoint["numerator"] = point.numerator;
            signaturePoint["denominator"] = point.denominator;
            documentSignatureMap.push_back(signaturePoint);
        }
        object["timeSignatureMap"] = documentSignatureMap;
        object["numerator"] = doc->timeSignatureNumerator;
        object["denominator"] = doc->timeSignatureDenominator;
        QJsonArray notes;
        for (const MidiNote& note : doc->notes) {
            QJsonObject item;
            item["pitch"] = note.pitch;
            item["start"] = note.startBeat;
            item["length"] = note.lengthBeats;
            item["velocity"] = note.velocity;
            item["pan"] = note.pan;
            item["release"] = note.release;
            item["modX"] = note.modX;
            item["modY"] = note.modY;
            item["color"] = note.colorGroup;
            item["group"] = note.groupId;
            item["keyswitch"] = note.isKeyswitch;
            item["slide"] = note.isSlide;
            item["portamento"] = note.isPortamento;
            item["muted"] = note.muted;
            notes.append(item);
        }
        object["notes"] = notes;
        QJsonArray performance;
        for (const MidiEvent& event : doc->performanceEvents) {
            QJsonObject item;
            item["beat"] = event.beat;
            item["status"] = event.status;
            item["data1"] = event.data1;
            item["data2"] = event.data2;
            performance.append(item);
        }
        object["performance"] = performance;
        documents.append(object);
    }
    if (documents.size() != static_cast<int>(referencedMidiContents.size())) {
        if (errorText) *errorText = "A MIDI clip references missing content.";
        return false;
    }
    root["midiContents"] = documents;

    root = ProjectJsonMerge::mergeForSave(m_projectPassthroughRoot, root, recoverySnapshot);
    json = QJsonDocument(root).toJson(
        recoverySnapshot ? QJsonDocument::Compact : QJsonDocument::Indented);
    if (json.isEmpty() || json.size() > kMaxProjectFileBytes) {
        if (errorText) *errorText = "Serialized project exceeds 512 MB.";
        json.clear();
        return false;
    }
    return true;
}

bool VeraCoreQT::saveProjectFile(const QString& path, QString* errorText)
{
    QByteArray json;
    if (!serializeProject(json, true, false, errorText)) return false;
    if (!createPreSaveProjectBackup(path, errorText)) return false;
    const QByteArray digest = QCryptographicHash::hash(
        json, QCryptographicHash::Sha256).toHex();
    const QString checksumPath = path + QStringLiteral(".sha256");
    QSaveFile checksumFile(checksumPath);
    checksumFile.setDirectWriteFallback(false);
    if (!checksumFile.open(QIODevice::WriteOnly) ||
        checksumFile.write(digest) != digest.size()) {
        checksumFile.cancelWriting();
        if (errorText) *errorText = "Could not stage the project integrity checksum.";
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        checksumFile.cancelWriting();
        if (errorText) *errorText = "Could not open project destination.";
        return false;
    }
    if (file.write(json) != json.size() || !file.commit()) {
        file.cancelWriting();
        checksumFile.cancelWriting();
        if (errorText) *errorText = "Could not atomically write project file.";
        return false;
    }
    if (!checksumFile.commit()) {
        QFile::remove(checksumPath); // Never leave a stale digest beside newly committed data.
        m_projectIntegrityState = ProjectIntegrityState::LegacyUnverified;
        updateProjectWindowTitle();
        if (errorText) *errorText =
            "Project data was saved, but its integrity checksum failed. Save again.";
        return false;
    }
    m_projectIntegrityState = ProjectIntegrityState::Verified;
    updateProjectWindowTitle();
    return true;
}

QString VeraCoreQT::recoveryFilePath() const
{
    return QDir(QStringLiteral("M:/VeraCore-Recovery"))
        .filePath(QStringLiteral("AutoRecovery.vcp"));
}

QString VeraCoreQT::recoveryFilePathForRevision(quint64 revision) const
{
    constexpr quint64 kRecoveryGenerationCount = 5;
    const quint64 slot = revision > 0
        ? (revision - 1u) % kRecoveryGenerationCount : 0u;
    return QDir(QStringLiteral("M:/VeraCore-Recovery"))
        .filePath(QStringLiteral("AutoRecovery.%1.vcp").arg(slot));
}

void VeraCoreQT::scheduleRecoveryRetry(const QString& reason)
{
    if (!m_recoveryTimer || !m_recoveryTimer->isActive() ||
        !m_recoveryRetryTimer) return;
    QSettings settings;
    const int failures = std::clamp(settings.value(
        QStringLiteral("Recovery/ConsecutiveFailures"), 1).toInt(), 1, 1000000);
    const int exponent = std::min(failures - 1, 6);
    const int delaySeconds = std::min(300, 5 * (1 << exponent));
    const int delayMs = delaySeconds * 1000;
    if (!m_recoveryRetryTimer->isActive() ||
        m_recoveryRetryTimer->remainingTime() > delayMs) {
        m_recoveryRetryTimer->start(delayMs);
    }
    settings.setValue(QStringLiteral("Recovery/RetryDelaySeconds"), delaySeconds);
    settings.setValue(QStringLiteral("Recovery/NextRetryUtcMs"),
                      QDateTime::currentMSecsSinceEpoch() + delayMs);

    const QString failureReason = reason.trimmed().isEmpty()
        ? QStringLiteral("Unknown recovery checkpoint failure.")
        : reason.trimmed().left(1024);
    const QString retryMessage = QStringLiteral(
        "Automatic recovery failed (%1 consecutive). Retry in %2 seconds.\n%3")
        .arg(failures).arg(delaySeconds).arg(failureReason);
    const bool escalationShown = settings.value(
        QStringLiteral("Recovery/EscalationShown"), false).toBool();
    if (failures >= 3 && !escalationShown) {
        settings.setValue(QStringLiteral("Recovery/EscalationShown"), true);
        settings.sync();
        auto* warning = new QMessageBox(
            QMessageBox::Warning, QStringLiteral("Recovery Needs Attention"),
            retryMessage + QStringLiteral(
                "\n\nOpen Options > Recovery > Recovery Status for diagnostics."),
            QMessageBox::Ok, this);
        warning->setAttribute(Qt::WA_DeleteOnClose);
        warning->open();
    } else {
        settings.sync();
        ui.statusBar->showMessage(retryMessage.simplified(), 8000);
    }
}

void VeraCoreQT::scheduleRecoverySnapshot()
{
    if (!m_projectDirty || m_restoringProject || m_offlineRenderActive || m_isPlaying || m_isRecordingMidi ||
        m_isRecordingAudio || m_recoveryWritePending ||
        m_projectRevision == m_lastRecoveryRevision ||
        !m_recoveryIdleClock.isValid() || m_recoveryIdleClock.elapsed() < 5000) {
        return;
    }

    const quint64 snapshotRevision = m_projectRevision;
    const QString path = recoveryFilePathForRevision(snapshotRevision);
    if (path.isEmpty()) return;

    QByteArray json;
    QString error;
    if (!serializeProject(json, false, true, &error) || json.isEmpty()) {
        const QString reason = error.isEmpty()
            ? QStringLiteral("Could not serialize the automatic recovery checkpoint.")
            : error;
        recordRecoveryCheckpointTelemetry(path, snapshotRevision, false, reason);
        scheduleRecoveryRetry(reason);
        return;
    }
    const RecoveryStorageStatus storage = inspectRecoveryStorage(json.size(), false);
    if (!storage.ready) {
        recordRecoveryCheckpointTelemetry(path, snapshotRevision, false, storage.detail);
        scheduleRecoveryRetry(storage.detail);
        return;
    }

    auto writeSucceeded = std::make_shared<std::atomic_bool>(false);
    auto writeError = std::make_shared<QString>();
    m_recoveryWritePending = true;
    QThread* writer = QThread::create(
            [path, json = std::move(json), writeSucceeded, writeError]() {
        const QFileInfo destination(path);
        if (!QDir().mkpath(destination.absolutePath())) {
            *writeError = QStringLiteral("Could not create the recovery directory.");
            return;
        }
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) {
            *writeError = QStringLiteral("Could not stage the recovery checkpoint.");
            return;
        }
        if (file.write(json) != json.size()) {
            file.cancelWriting();
            *writeError = QStringLiteral("Could not write the complete recovery checkpoint.");
            return;
        }
        if (!file.commit()) {
            *writeError = QStringLiteral("Could not atomically commit the recovery checkpoint.");
            return;
        }
        const QByteArray digest = QCryptographicHash::hash(
            json, QCryptographicHash::Sha256).toHex();
        QSaveFile checksumFile(path + QStringLiteral(".sha256"));
        checksumFile.setDirectWriteFallback(false);
        if (!checksumFile.open(QIODevice::WriteOnly) ||
            checksumFile.write(digest) != digest.size()) {
            checksumFile.cancelWriting();
            *writeError = QStringLiteral("Could not stage the recovery checksum.");
            return;
        }
        if (!checksumFile.commit()) {
            *writeError = QStringLiteral("Could not atomically commit the recovery checksum.");
            return;
        }
        writeSucceeded->store(true, std::memory_order_release);
    });
    connect(writer, &QThread::finished, this,
            [this, path, snapshotRevision, writeSucceeded, writeError]() {
        const bool succeeded = writeSucceeded->load(std::memory_order_acquire);
        recordRecoveryCheckpointTelemetry(path, snapshotRevision, succeeded, *writeError);
        if (succeeded) {
            m_lastRecoveryRevision = snapshotRevision;
            if (m_recoveryRetryTimer) m_recoveryRetryTimer->stop();
        } else {
            scheduleRecoveryRetry(*writeError);
        }
        m_recoveryWritePending = false;
    });
    connect(writer, &QThread::finished, writer, &QObject::deleteLater);
    writer->setObjectName(QStringLiteral("VeraCoreRecoveryWriter"));
    writer->start(QThread::LowPriority);
}

bool VeraCoreQT::writeRecoverySnapshotNow(QString* errorText)
{
    if (!m_projectDirty) return true;
    const quint64 snapshotRevision = m_projectRevision;
    const QString path = recoveryFilePathForRevision(snapshotRevision);
    if (path.isEmpty()) {
        if (errorText) *errorText = "Crash-recovery storage is unavailable.";
        return false;
    }
    if (snapshotRevision == m_lastRecoveryRevision &&
        recoveryFileMatchesChecksum(path)) {
        recordRecoveryCheckpointTelemetry(path, snapshotRevision, true);
        if (m_recoveryRetryTimer) m_recoveryRetryTimer->stop();
        return true;
    }
    if (m_recoveryWritePending) {
        if (errorText) *errorText = "An automatic recovery checkpoint is still being committed.";
        return false;
    }
    bool checkpointSucceeded = false;
    QString checkpointError;
    const auto failCheckpoint = [&](const QString& reason) {
        checkpointError = reason;
        if (errorText) *errorText = reason;
        return false;
    };
    const auto telemetryGuard = qScopeGuard([&, snapshotRevision]() {
        recordRecoveryCheckpointTelemetry(path, snapshotRevision,
                                          checkpointSucceeded, checkpointError);
    });

    QByteArray json;
    QString serializeError;
    if (!serializeProject(json, false, true, &serializeError) || json.isEmpty()) {
        return failCheckpoint(serializeError.isEmpty()
            ? QStringLiteral("Could not serialize the pre-render recovery checkpoint.")
            : serializeError);
    }
    if (json.size() > kMaxProjectFileBytes) {
        return failCheckpoint(QStringLiteral(
            "Pre-render recovery checkpoint exceeds 512 MB."));
    }
    const RecoveryStorageStatus storage = inspectRecoveryStorage(json.size(), false);
    if (!storage.ready) return failCheckpoint(storage.detail);

    const QFileInfo destination(path);
    if (!QDir().mkpath(destination.absolutePath())) {
        return failCheckpoint(QStringLiteral("Could not create the recovery directory."));
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()) {
        file.cancelWriting();
        return failCheckpoint(QStringLiteral(
            "Could not stage the pre-render recovery checkpoint."));
    }
    if (!file.commit()) {
        return failCheckpoint(QStringLiteral(
            "Could not atomically commit the pre-render recovery checkpoint."));
    }
    const QByteArray digest = QCryptographicHash::hash(
        json, QCryptographicHash::Sha256).toHex();
    QSaveFile checksumFile(path + QStringLiteral(".sha256"));
    checksumFile.setDirectWriteFallback(false);
    if (!checksumFile.open(QIODevice::WriteOnly) ||
        checksumFile.write(digest) != digest.size()) {
        checksumFile.cancelWriting();
        return failCheckpoint(QStringLiteral("Could not stage the recovery checksum."));
    }
    if (!checksumFile.commit()) {
        return failCheckpoint(QStringLiteral(
            "Could not atomically commit the recovery checksum."));
    }
    m_lastRecoveryRevision = snapshotRevision;
    checkpointSucceeded = true;
    if (m_recoveryRetryTimer) m_recoveryRetryTimer->stop();
    return true;
}

void VeraCoreQT::markRecoveryStateClean()
{
    QSettings settings;
    settings.setValue(QStringLiteral("Recovery/LastCleanUtcMs"),
                      QDateTime::currentMSecsSinceEpoch());
    settings.sync();
}

void VeraCoreQT::offerRecoveryIfPresent()
{
    const qint64 lastCleanUtcMs = QSettings().value(
        QStringLiteral("Recovery/LastCleanUtcMs"), 0).toLongLong();
    constexpr int kRecoveryGenerationCount = 5;
    QStringList candidates{recoveryFilePath()}; // Read the legacy single-file checkpoint too.
    for (int slot = 0; slot < kRecoveryGenerationCount; ++slot) {
        candidates.push_back(QDir(QStringLiteral("M:/VeraCore-Recovery"))
            .filePath(QStringLiteral("AutoRecovery.%1.vcp").arg(slot)));
    }

    QString path;
    QJsonObject root;
    qint64 createdUtcMs = 0;
    for (const QString& candidate : candidates) {
        QFile file(candidate);
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
            file.size() > kMaxProjectFileBytes) continue;
        QJsonParseError parseError;
        const QByteArray recoveryJson = file.readAll();
        file.close();
        if (candidate != recoveryFilePath() &&
            !recoveryPayloadMatchesChecksum(candidate, recoveryJson)) continue;
        const QJsonDocument document = QJsonDocument::fromJson(recoveryJson, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) continue;
        const QJsonObject candidateRoot = document.object();
        if (!candidateRoot.value(QStringLiteral("recoverySnapshot")).toBool(false) ||
            candidateRoot.value(QStringLiteral("format")).toString() !=
                QStringLiteral("VeraCoreProject")) continue;
        const qint64 candidateCreatedUtcMs = static_cast<qint64>(
            candidateRoot.value(QStringLiteral("recoveryCreatedUtcMs")).toDouble(0.0));
        if (candidateCreatedUtcMs <= lastCleanUtcMs || candidateCreatedUtcMs <= createdUtcMs)
            continue;
        path = candidate;
        root = candidateRoot;
        createdUtcMs = candidateCreatedUtcMs;
    }
    if (path.isEmpty()) return;

    const QMessageBox::StandardButton choice = QMessageBox::warning(
        this, QStringLiteral("Recover Project"),
        QStringLiteral("VeraCore found newer crash-recovery data. Recover it?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (choice != QMessageBox::Yes) {
        markRecoveryStateClean();
        return;
    }

    const QString sourcePath = root.value(QStringLiteral("recoverySourcePath")).toString();
    QString error;
    if (!loadProjectFile(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Recover Project"),
                              error.isEmpty() ? QStringLiteral("Could not recover project.") : error);
        return;
    }
    const QFileInfo sourceInfo(sourcePath);
    m_projectPath = sourcePath.isEmpty() || !sourceInfo.isAbsolute() ||
            sourceInfo.suffix().compare(QStringLiteral("vcp"), Qt::CaseInsensitive) != 0
        ? QString{}
        : sourceInfo.absoluteFilePath();
    setProjectDirty(true);
    ui.statusBar->showMessage(QStringLiteral("Recovery loaded. Save project to keep it."), 5000);
}

bool VeraCoreQT::loadProjectFile(const QString& path, QString* errorText)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
        file.size() > kMaxProjectFileBytes) {
        if (errorText) *errorText = "Project file is missing, empty, or larger than 512 MB.";
        return false;
    }
    const qint64 expectedSize = file.size();
    const QByteArray projectJson = file.readAll();
    file.close();
    if (projectJson.size() != expectedSize) {
        if (errorText) *errorText = "Could not read the complete project file.";
        return false;
    }
    const QFileInfo checksumInfo(path + QStringLiteral(".sha256"));
    if (checksumInfo.exists() && !recoveryPayloadMatchesChecksum(path, projectJson)) {
        if (errorText) *errorText = projectIntegrityErrorText();
        return false;
    }
    const ProjectIntegrityState loadedIntegrity = checksumInfo.exists()
        ? ProjectIntegrityState::Verified
        : ProjectIntegrityState::LegacyUnverified;
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(projectJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        if (errorText) *errorText = QString("Invalid project JSON: %1").arg(parseError.errorString());
        return false;
    }
    QJsonObject root = json.object();
    const int sourceVersion = root.value("version").toInt();
    if (root.value("format").toString() != "VeraCoreProject" ||
        (sourceVersion != 1 && sourceVersion != 2 && sourceVersion != 3 &&
         sourceVersion != 4 && sourceVersion != 5 && sourceVersion != 6 &&
         sourceVersion != 7 && sourceVersion != 8 && sourceVersion != 9 &&
         sourceVersion != 10 && sourceVersion != 11 && sourceVersion != 12 &&
         sourceVersion != 13 && sourceVersion != 14 && sourceVersion != 15 &&
         sourceVersion != 16)) {
        if (errorText) *errorText = "Unsupported or incomplete VeraCore project schema.";
        return false;
    }

    auto newPersistentId = []() {
        return QUuid::createUuid().toString(QUuid::WithoutBraces);
    };
    if (sourceVersion == 1) {
        if (!root.value("tracks").isArray() || !root.value("midiClips").isArray() ||
            !root.value("audioClips").isArray() || !root.value("midiDocuments").isArray()) {
            if (errorText) *errorText = "Incomplete version 1 project schema.";
            return false;
        }
        const QJsonArray legacyDocuments = root.value("midiDocuments").toArray();
        std::map<QString, QJsonObject> documentsByLegacyKey;
        for (const QJsonValue& value : legacyDocuments) {
            const QJsonObject document = value.toObject();
            const QString key = QString("%1|%2")
                .arg(document.value("track").toInt(-1))
                .arg(document.value("name").toString().trimmed());
            const auto existing = documentsByLegacyKey.find(key);
            if (existing != documentsByLegacyKey.end() &&
                QJsonDocument(existing->second).toJson(QJsonDocument::Compact) !=
                    QJsonDocument(document).toJson(QJsonDocument::Compact)) {
                if (errorText) *errorText = "Version 1 project contains conflicting MIDI pattern payloads.";
                return false;
            }
            documentsByLegacyKey[key] = document;
        }

        QJsonArray migratedMidiClips;
        QJsonArray migratedMidiContents;
        std::map<QString, QString> sharedMidiIds;
        for (const QJsonValue& value : root.value("midiClips").toArray()) {
            QJsonObject clip = value.toObject();
            const QString legacyKey = QString("%1|%2")
                .arg(clip.value("track").toInt(-1))
                .arg(clip.value("name").toString().trimmed());
            QString contentId;
            if (!clip.value("unique").toBool(true)) contentId = sharedMidiIds[legacyKey];
            if (contentId.isEmpty()) {
                contentId = newPersistentId();
                if (!clip.value("unique").toBool(true)) sharedMidiIds[legacyKey] = contentId;
                QJsonObject content = documentsByLegacyKey[legacyKey];
                content["contentId"] = contentId;
                content["track"] = clip.value("track");
                content["name"] = clip.value("name");
                if (!content.value("notes").isArray()) content["notes"] = QJsonArray{};
                if (!content.value("performance").isArray()) content["performance"] = QJsonArray{};
                migratedMidiContents.append(content);
            }
            clip.remove("unique");
            clip["clipId"] = newPersistentId();
            clip["contentId"] = contentId;
            migratedMidiClips.append(clip);
        }

        QJsonArray migratedAudioClips;
        QJsonArray migratedAudioContents;
        std::map<QByteArray, QString> sharedAudioIds;
        for (const QJsonValue& value : root.value("audioClips").toArray()) {
            QJsonObject legacy = value.toObject();
            QJsonObject content;
            for (const QString& field : {QString("name"), QString("sourceTempo"), QString("tempoConfidence"),
                                         QString("sourceSignatureNumerator"), QString("sourceSignatureDenominator"),
                                         QString("tempoDetected"), QString("transientSeconds"), QString("warpMarkers"), QString("pitch"),
                                         QString("syncMode"), QString("targetBars"), QString("beatsPerBar"),
                                         QString("sourceDuration"), QString("gain"), QString("pan"),
                                         QString("normalize"), QString("reverse"), QString("reversePolarity"),
                                         QString("sourcePath")}) {
                content[field] = legacy.value(field);
            }
            const QByteArray sharedKey = QJsonDocument(content).toJson(QJsonDocument::Compact);
            QString contentId;
            if (!legacy.value("unique").toBool(true)) contentId = sharedAudioIds[sharedKey];
            if (contentId.isEmpty()) {
                contentId = newPersistentId();
                if (!legacy.value("unique").toBool(true)) sharedAudioIds[sharedKey] = contentId;
                content["contentId"] = contentId;
                migratedAudioContents.append(content);
            }
            QJsonObject clip;
            clip["clipId"] = newPersistentId();
            clip["contentId"] = contentId;
            clip["track"] = legacy.value("track");
            clip["start"] = legacy.value("start");
            clip["length"] = legacy.value("length");
            migratedAudioClips.append(clip);
        }
        root["version"] = 2;
        root["midiClips"] = migratedMidiClips;
        root["audioClips"] = migratedAudioClips;
        root["midiContents"] = migratedMidiContents;
        root["audioContents"] = migratedAudioContents;
    }

    if (!root.value("tracks").isArray() || !root.value("midiClips").isArray() ||
        !root.value("audioClips").isArray() || !root.value("midiContents").isArray() ||
        !root.value("audioContents").isArray() ||
        (root.contains("araArchives") && !root.value("araArchives").isArray())) {
        if (errorText) *errorText = "Unsupported or incomplete VeraCore project schema.";
        return false;
    }
    const QJsonArray trackData = root.value("tracks").toArray();
    const QJsonArray midiClipData = root.value("midiClips").toArray();
    const QJsonArray audioClipData = root.value("audioClips").toArray();
    const QJsonArray documentData = root.value("midiContents").toArray();
    const QJsonArray audioContentData = root.value("audioContents").toArray();
    const QJsonArray araArchiveData = root.value("araArchives").toArray();
    const auto validOptionalSeparation = [](const QJsonObject& object) {
        if (!object.contains("stereoSeparation")) return true;
        const QJsonValue value = object.value("stereoSeparation");
        const double separation = value.toDouble(std::numeric_limits<double>::quiet_NaN());
        return value.isDouble() && std::isfinite(separation) &&
            separation >= -1.0 && separation <= 1.0;
    };
    if (root.contains("returns") &&
        (!root.value("returns").isArray() || root.value("returns").toArray().size() > kReturnBusCount)) {
        if (errorText) *errorText = "Project contains invalid return bus data.";
        return false;
    }
    if (root.contains("master") && !root.value("master").isObject()) {
        if (errorText) *errorText = "Project contains invalid master bus data.";
        return false;
    }
    if (root.contains("master") &&
        !validOptionalSeparation(root.value("master").toObject())) {
        if (errorText) *errorText = "Project contains invalid Master stereo separation.";
        return false;
    }
    if (root.contains("mixer")) {
        if (!root.value("mixer").isObject()) {
            if (errorText) *errorText = "Project contains invalid mixer settings.";
            return false;
        }
        const QJsonValue panLawValue = root.value("mixer").toObject().value("panLawDb");
        const double panLaw = panLawValue.toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!panLawValue.isDouble() || !std::isfinite(panLaw) ||
            std::fabs(canonicalPanLawDb(panLaw) - panLaw) > 1.0e-9) {
            if (errorText) *errorText = "Project contains an unsupported pan law.";
            return false;
        }
        const int speakerLayout = root.value("mixer").toObject().value("speakerLayout").toInt(0);
        if (speakerLayout < static_cast<int>(VeraCoreSpeakerLayout::Stereo) ||
            speakerLayout > static_cast<int>(VeraCoreSpeakerLayout::Atmos714)) {
            if (errorText) *errorText = "Project contains an invalid spatial speaker layout.";
            return false;
        }
    }
    for (const QJsonValue& value : root.value("returns").toArray()) {
        if (!value.isObject() || !validOptionalSeparation(value.toObject())) {
            if (errorText) *errorText = "Project contains an invalid return bus entry.";
            return false;
        }
    }
    for (const QJsonValue& value : trackData) {
        if (!value.isObject() || !validOptionalSeparation(value.toObject())) {
            if (errorText) *errorText = "Project contains invalid track stereo separation.";
            return false;
        }
    }
    if (trackData.size() > 256 || midiClipData.size() > 100000 ||
        audioClipData.size() > 100000 || documentData.size() > 10000 ||
        audioContentData.size() > 100000 || araArchiveData.size() > kMaxAraArchives) {
        if (errorText) *errorText = "Project exceeds supported track, clip, or pattern limits.";
        return false;
    }

    std::vector<StoredAraArchive> loadedAraArchives;
    qint64 loadedAraBytes = 0;
    std::set<std::pair<QByteArray, QByteArray>> loadedAraIds;
    try {
        loadedAraArchives.reserve(araArchiveData.size());
        for (const QJsonValue& value : araArchiveData) {
            if (!value.isObject()) {
                if (errorText) *errorText = "Project contains invalid ARA archive data.";
                return false;
            }
            const QJsonObject item = value.toObject();
            const QString factoryText = item.value("factoryId").toString();
            const QString archiveText = item.value("archiveId").toString();
            StoredAraArchive archive;
            archive.factoryPersistentIdUtf8 = factoryText.toUtf8();
            archive.documentArchiveIdUtf8 = archiveText.toUtf8();
            if (archive.factoryPersistentIdUtf8.isEmpty() ||
                archive.documentArchiveIdUtf8.isEmpty() ||
                archive.factoryPersistentIdUtf8.size() > kMaxAraPersistentIdBytes ||
                archive.documentArchiveIdUtf8.size() > kMaxAraPersistentIdBytes ||
                archive.factoryPersistentIdUtf8.contains('\0') ||
                archive.documentArchiveIdUtf8.contains('\0') ||
                QString::fromUtf8(archive.factoryPersistentIdUtf8) != factoryText ||
                QString::fromUtf8(archive.documentArchiveIdUtf8) != archiveText ||
                !loadedAraIds.emplace(archive.factoryPersistentIdUtf8,
                                      archive.documentArchiveIdUtf8).second ||
                !decodeProjectAraArchive(item.value("data").toString(), archive.bytes)) {
                if (errorText) *errorText = "Project contains invalid or oversized ARA archive.";
                return false;
            }
            loadedAraBytes += archive.bytes.size();
            if (loadedAraBytes > kMaxProjectAraArchiveBytes) {
                if (errorText) *errorText = "Project ARA state exceeds 256 MB.";
                return false;
            }
            loadedAraArchives.push_back(std::move(archive));
        }
    } catch (...) {
        if (errorText) *errorText = "Not enough memory to load ARA project state.";
        return false;
    }

    // Validate all bounded collections before replacing the current session.
    // Track array positions are persistent IDs, so malformed entries cannot be skipped.
    std::vector<TrackType> serializedTrackTypes;
    serializedTrackTypes.reserve(trackData.size());
    for (const QJsonValue& value : trackData) {
        if (!value.isObject()) {
            if (errorText) *errorText = "Project contains an invalid track entry.";
            return false;
        }
        const QJsonObject item = value.toObject();
        const QString type = item.value("type").toString();
        if (type != "midi" && type != "audio") {
            if (errorText) *errorText = "Project contains an unsupported track type.";
            return false;
        }
        if (item.contains("audioFx") && !item.value("audioFx").isArray()) {
            if (errorText) *errorText = "Project contains invalid audio-effect data.";
            return false;
        }
        if (item.contains("sends") &&
            (!item.value("sends").isArray() || item.value("sends").toArray().size() > kReturnBusCount)) {
            if (errorText) *errorText = "Project contains invalid track send data.";
            return false;
        }
        if (item.value("audioFx").toArray().size() > 128) {
            if (errorText) *errorText = "A track contains more than 128 audio effects.";
            return false;
        }
        for (const QJsonValue& fx : item.value("audioFx").toArray()) {
            if (!fx.isObject() && !fx.isString()) {
                if (errorText) *errorText = "A track contains invalid audio-effect data.";
                return false;
            }
        }
        if (type == "midi" && item.contains("layers") && !item.value("layers").isArray()) {
            if (errorText) *errorText = "Project contains invalid MIDI layer data.";
            return false;
        }
        if (type == "midi" && item.value("layers").isArray()) {
            const QJsonArray layers = item.value("layers").toArray();
            if (layers.size() > 128) {
                if (errorText) *errorText = "A MIDI track contains more than 128 layers.";
                return false;
            }
            for (const QJsonValue& layer : layers) {
                if (!layer.isObject()) {
                    if (errorText) *errorText = "Project contains an invalid plugin layer entry.";
                    return false;
                }
                const QJsonObject layerObject = layer.toObject();
                if (layerObject.contains("outputRoutes") &&
                    (!layerObject.value("outputRoutes").isArray() ||
                     layerObject.value("outputRoutes").toArray().size() > 64)) {
                    if (errorText) *errorText = "Project contains invalid plugin output routing.";
                    return false;
                }
                for (const QJsonValue& routeValue : layerObject.value("outputRoutes").toArray()) {
                    if (!routeValue.isObject()) {
                        if (errorText) *errorText = "Project contains an invalid plugin output route.";
                        return false;
                    }
                    const QJsonObject route = routeValue.toObject();
                    const int bus = route.value("bus").toInt(-1);
                    const int destination = route.value("destinationTrack").toInt(-2);
                    if (bus < 0 || bus >= 64 || destination < -1 || destination >= trackData.size() ||
                        destination >= 256) {
                        if (errorText) *errorText = "Project contains an out-of-range plugin output route.";
                        return false;
                    }
                }
            }
        }
        serializedTrackTypes.push_back(type == "midi" ? TrackType::Midi : TrackType::Audio);
    }

    auto validClipTrack = [&serializedTrackTypes](const QJsonObject& item, TrackType expectedType) {
        const int track = item.value("track").toInt(-1);
        return track >= 0 && track < static_cast<int>(serializedTrackTypes.size()) &&
               serializedTrackTypes[track] == expectedType;
    };
    for (const QJsonValue& value : midiClipData) {
        if (!value.isObject() || !validClipTrack(value.toObject(), TrackType::Midi)) {
            if (errorText) *errorText = "Project contains an invalid MIDI clip entry.";
            return false;
        }
    }
    for (const QJsonValue& value : audioClipData) {
        if (!value.isObject() || !validClipTrack(value.toObject(), TrackType::Audio)) {
            if (errorText) *errorText = "Project contains an invalid audio clip entry.";
            return false;
        }
        const QJsonObject clip = value.toObject();
        const double length = clip.value("length").toDouble(0.0);
        const double fadeIn = clip.value("fadeIn").toDouble(0.0);
        const double fadeOut = clip.value("fadeOut").toDouble(0.0);
        const double fadeInCurve = clip.value("fadeInCurve").toDouble(0.0);
        const double fadeOutCurve = clip.value("fadeOutCurve").toDouble(0.0);
        if (!std::isfinite(length) || length <= 0.0 || !std::isfinite(fadeIn) ||
            !std::isfinite(fadeOut) || fadeIn < 0.0 || fadeOut < 0.0 ||
            fadeIn + fadeOut > length + 0.000001 || !std::isfinite(fadeInCurve) ||
            !std::isfinite(fadeOutCurve) || fadeInCurve < -1.0 || fadeInCurve > 1.0 ||
            fadeOutCurve < -1.0 || fadeOutCurve > 1.0) {
            if (errorText) *errorText = "Project contains invalid audio clip fades.";
            return false;
        }
        if (clip.contains("gainEnvelope") && !clip.value("gainEnvelope").isArray()) {
            if (errorText) *errorText = "Project contains an invalid audio gain envelope.";
            return false;
        }
        const QJsonArray gainEnvelope = clip.value("gainEnvelope").toArray();
        if (gainEnvelope.size() > 256) {
            if (errorText) *errorText = "An audio gain envelope contains more than 256 points.";
            return false;
        }
        double previousBeat = -1.0;
        for (const QJsonValue& pointValue : gainEnvelope) {
            if (!pointValue.isObject()) {
                if (errorText) *errorText = "Project contains an invalid audio gain envelope point.";
                return false;
            }
            const QJsonObject point = pointValue.toObject();
            const double beat = point.value("beat").toDouble(std::numeric_limits<double>::quiet_NaN());
            const double gain = point.value("gain").toDouble(std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(beat) || !std::isfinite(gain) || beat < 0.0 ||
                beat > length || gain < 0.0 || gain > 4.0 || beat <= previousBeat) {
                if (errorText) *errorText = "Project contains an out-of-range audio gain envelope point.";
                return false;
            }
            previousBeat = beat;
        }
    }

    auto validId = [](const QString& id) {
        return !id.isEmpty() && id == id.trimmed() && id.size() <= 128;
    };
    std::set<QString> clipIds;
    std::set<QString> midiContentIds;
    std::set<QString> audioContentIds;
    for (const QJsonValue& value : documentData) {
        const QJsonObject item = value.toObject();
        const QString id = item.value("contentId").toString();
        if (!validId(id) || !midiContentIds.insert(id).second) {
            if (errorText) *errorText = "Project contains duplicate or invalid MIDI content IDs.";
            return false;
        }
    }
    for (const QJsonValue& value : audioContentData) {
        if (!value.isObject()) {
            if (errorText) *errorText = "Project contains invalid audio content.";
            return false;
        }
        const QJsonObject content = value.toObject();
        const QString id = content.value("contentId").toString();
        if (!validId(id) || !audioContentIds.insert(id).second) {
            if (errorText) *errorText = "Project contains duplicate or invalid audio content IDs.";
            return false;
        }
        const double gain = content.value("gain").toDouble(1.0);
        const double pan = content.value("pan").toDouble(0.0);
        const double tempoConfidence = content.value("tempoConfidence").toDouble(0.0);
        const int sourceNumerator = content.value("sourceSignatureNumerator").toInt(0);
        const int sourceDenominator = content.value("sourceSignatureDenominator").toInt(0);
        const QJsonValue transientValue = content.value("transientSeconds");
        const QJsonValue warpValue = content.value("warpMarkers");
        const bool validSourceDenominator = sourceDenominator == 0 ||
            (sourceDenominator >= 1 && sourceDenominator <= 64 &&
             (sourceDenominator & (sourceDenominator - 1)) == 0);
        if (!std::isfinite(gain) || gain < 0.0 || gain > 4.0 ||
            !std::isfinite(pan) || pan < -1.0 || pan > 1.0 ||
            (content.contains("tempoConfidence") && !content.value("tempoConfidence").isDouble()) ||
            (content.contains("sourceSignatureNumerator") && !content.value("sourceSignatureNumerator").isDouble()) ||
            (content.contains("sourceSignatureDenominator") && !content.value("sourceSignatureDenominator").isDouble()) ||
            !std::isfinite(tempoConfidence) || tempoConfidence < 0.0 || tempoConfidence > 1.0 ||
            sourceNumerator < 0 || sourceNumerator > 32 || !validSourceDenominator ||
            (content.contains("tempoDetected") && !content.value("tempoDetected").isBool())) {
            if (errorText) *errorText = "Project contains invalid audio clip processing values.";
            return false;
        }
        if (!transientValue.isUndefined()) {
            if (!transientValue.isArray() || transientValue.toArray().size() > 4096) {
                if (errorText) *errorText = "Project contains invalid audio transient markers.";
                return false;
            }
            const double duration = content.value("sourceDuration").toDouble(0.0);
            const double maximumMarker = std::isfinite(duration) && duration > 0.0
                ? duration + 0.001 : 24.0 * 60.0 * 60.0;
            double previousMarker = -1.0;
            for (const QJsonValue& markerValue : transientValue.toArray()) {
                const double marker = markerValue.toDouble(-1.0);
                if (!markerValue.isDouble() || !std::isfinite(marker) || marker < 0.0 ||
                    marker > maximumMarker || marker + 0.000001 < previousMarker) {
                    if (errorText) *errorText = "Project contains invalid audio transient markers.";
                    return false;
                }
                previousMarker = marker;
            }
        }
        if (!warpValue.isUndefined()) {
            if (!warpValue.isArray() || warpValue.toArray().size() > 4096) {
                if (errorText) *errorText = "Project contains invalid audio warp markers.";
                return false;
            }
            const double duration = content.value("sourceDuration").toDouble(0.0);
            double previousSource = -1.0;
            double previousTarget = -1.0;
            for (const QJsonValue& markerValue : warpValue.toArray()) {
                if (!markerValue.isObject()) {
                    if (errorText) *errorText = "Project contains invalid audio warp markers.";
                    return false;
                }
                const QJsonObject marker = markerValue.toObject();
                const double sourceSeconds = marker.value("sourceSeconds").toDouble(-1.0);
                const double targetBeat = marker.value("targetBeat").toDouble(-1.0);
                if (!marker.value("sourceSeconds").isDouble() || !marker.value("targetBeat").isDouble() ||
                    !std::isfinite(sourceSeconds) || !std::isfinite(targetBeat) ||
                    sourceSeconds <= 0.0 || (duration > 0.0 && sourceSeconds >= duration) ||
                    targetBeat <= 0.0 || sourceSeconds <= previousSource || targetBeat <= previousTarget) {
                    if (errorText) *errorText = "Project contains invalid audio warp markers.";
                    return false;
                }
                previousSource = sourceSeconds;
                previousTarget = targetBeat;
            }
        }
    }
    auto validateClipIds = [&](const QJsonArray& clips, const std::set<QString>& contents) {
        for (const QJsonValue& value : clips) {
            const QJsonObject item = value.toObject();
            const QString clipId = item.value("clipId").toString();
            const QString contentId = item.value("contentId").toString();
            if (!validId(clipId) || !clipIds.insert(clipId).second ||
                !validId(contentId) || contents.find(contentId) == contents.end()) return false;
        }
        return true;
    };
    if (!validateClipIds(midiClipData, midiContentIds) ||
        !validateClipIds(audioClipData, audioContentIds)) {
        if (errorText) *errorText = "Project contains duplicate clip IDs or dangling content references.";
        return false;
    }

    qint64 totalPluginStateBytes = 0;
    qint64 totalPluginAutomationPoints = 0;
    for (const QJsonValue& trackValue : trackData) {
        const QJsonObject trackObject = trackValue.toObject();
        const QJsonArray layers = trackObject.value("layers").toArray();
        for (const QJsonValue& layerValue : layers) {
            QByteArray state;
            std::vector<PluginAutomationLane> automation;
            if (!decodeProjectPluginState(layerValue.toObject().value("pluginState").toString(), state) ||
                !deserializePluginAutomation(layerValue.toObject().value("automation"), automation)) {
                if (errorText) *errorText = "Project contains invalid or oversized plugin state.";
                return false;
            }
            totalPluginAutomationPoints += pluginAutomationPointCount(automation);
            if (totalPluginAutomationPoints > kMaxProjectPluginAutomationPoints) {
                if (errorText) *errorText = "Project plugin automation exceeds four million points.";
                return false;
            }
            totalPluginStateBytes += state.size();
            if (totalPluginStateBytes > kMaxProjectPluginStateBytes) {
                if (errorText) *errorText = "Project plugin state exceeds 32 MB.";
                return false;
            }
        }
        for (const QJsonValue& fxValue : trackObject.value("audioFx").toArray()) {
            if (!fxValue.isObject()) continue;
            QByteArray state;
            std::vector<PluginAutomationLane> automation;
            if (!decodeProjectPluginState(fxValue.toObject().value("pluginState").toString(), state) ||
                !deserializePluginAutomation(fxValue.toObject().value("automation"), automation)) {
                if (errorText) *errorText = "Project contains invalid or oversized audio-effect state.";
                return false;
            }
            totalPluginAutomationPoints += pluginAutomationPointCount(automation);
            if (totalPluginAutomationPoints > kMaxProjectPluginAutomationPoints) {
                if (errorText) *errorText = "Project plugin automation exceeds four million points.";
                return false;
            }
            totalPluginStateBytes += state.size();
            if (totalPluginStateBytes > kMaxProjectPluginStateBytes) {
                if (errorText) *errorText = "Project plugin state exceeds 32 MB.";
                return false;
            }
        }
    }
    const auto validateBusEffects = [&](const QJsonArray& effects) {
        if (effects.size() > 128) return false;
        for (const QJsonValue& fxValue : effects) {
            if (!fxValue.isObject()) return false;
            QByteArray state;
            std::vector<PluginAutomationLane> automation;
            const QJsonObject effect = fxValue.toObject();
            if (!decodeProjectPluginState(effect.value("pluginState").toString(), state) ||
                !deserializePluginAutomation(effect.value("automation"), automation)) return false;
            totalPluginAutomationPoints += pluginAutomationPointCount(automation);
            totalPluginStateBytes += state.size();
            if (totalPluginAutomationPoints > kMaxProjectPluginAutomationPoints ||
                totalPluginStateBytes > kMaxProjectPluginStateBytes) return false;
        }
        return true;
    };
    for (const QJsonValue& returnValue : root.value("returns").toArray()) {
        if (!returnValue.isObject() ||
            !validateBusEffects(returnValue.toObject().value("audioFx").toArray())) {
            if (errorText) *errorText = "Project contains invalid or oversized return-effect state.";
            return false;
        }
    }
    if (!validateBusEffects(root.value("master").toObject().value("audioFx").toArray())) {
        if (errorText) *errorText = "Project contains invalid or oversized master-effect state.";
        return false;
    }

    qint64 totalNotes = 0;
    qint64 totalPerformanceEvents = 0;
    for (const QJsonValue& value : documentData) {
        if (!value.isObject()) {
            if (errorText) *errorText = "Project contains an invalid MIDI pattern entry.";
            return false;
        }
        const QJsonObject item = value.toObject();
        if (!validClipTrack(item, TrackType::Midi) || item.value("name").toString().trimmed().isEmpty() ||
            !item.value("notes").isArray() ||
            (item.contains("performance") && !item.value("performance").isArray())) {
            if (errorText) *errorText = "Project contains invalid MIDI pattern data.";
            return false;
        }
        const QJsonArray notes = item.value("notes").toArray();
        const QJsonArray performance = item.value("performance").toArray();
        const QJsonArray documentTempoMap = item.value("tempoMap").toArray();
        if (documentTempoMap.size() > static_cast<qsizetype>(kMaxTempoMapPoints)) {
            if (errorText) *errorText = "MIDI pattern tempo map exceeds 4096 points.";
            return false;
        }
        for (const QJsonValue& tempoValue : documentTempoMap) {
            if (!tempoValue.isObject()) {
                if (errorText) *errorText = "MIDI pattern contains an invalid tempo-map entry.";
                return false;
            }
            const QJsonObject tempoPoint = tempoValue.toObject();
            const double tempoBeat = tempoPoint.value("beat").toDouble(-1.0);
            const double tempoBpm = tempoPoint.value("bpm").toDouble(-1.0);
            if (!std::isfinite(tempoBeat) || tempoBeat < 0.0 ||
                !std::isfinite(tempoBpm) || tempoBpm < 20.0 || tempoBpm > 999.0) {
                if (errorText) *errorText = "MIDI pattern tempo map contains invalid timing data.";
                return false;
            }
        }
        const QJsonArray documentSignatureMap = item.value("timeSignatureMap").toArray();
        if (documentSignatureMap.size() > static_cast<qsizetype>(kMaxTimeSignatureMapPoints)) {
            if (errorText) *errorText = "MIDI pattern time-signature map exceeds 4096 points.";
            return false;
        }
        for (const QJsonValue& signatureValue : documentSignatureMap) {
            if (!signatureValue.isObject()) {
                if (errorText) *errorText = "MIDI pattern contains an invalid time-signature entry.";
                return false;
            }
            const QJsonObject point = signatureValue.toObject();
            const double beat = point.value("beat").toDouble(-1.0);
            const int numerator = point.value("numerator").toInt(0);
            const int denominator = point.value("denominator").toInt(0);
            if (!std::isfinite(beat) || beat < 0.0 || numerator < 1 || numerator > 32 ||
                !validTimeSignatureDenominator(denominator)) {
                if (errorText) *errorText = "MIDI pattern time-signature map contains invalid timing data.";
                return false;
            }
        }
        totalNotes += notes.size();
        totalPerformanceEvents += performance.size();
        if (totalNotes > 1000000 || totalPerformanceEvents > 1000000) {
            if (errorText) *errorText = "Project exceeds MIDI note or performance-event limits.";
            return false;
        }
        for (const QJsonValue& note : notes) {
            if (!note.isObject()) {
                if (errorText) *errorText = "Project contains an invalid MIDI note entry.";
                return false;
            }
        }
        for (const QJsonValue& event : performance) {
            if (!event.isObject()) {
                if (errorText) *errorText = "Project contains an invalid MIDI event entry.";
                return false;
            }
        }
    }

    const QJsonArray serializedTempoMap = root.value("transport").toObject().value("tempoMap").toArray();
    if (serializedTempoMap.size() > static_cast<qsizetype>(kMaxTempoMapPoints)) {
        if (errorText) *errorText = "Project tempo map exceeds 4096 points.";
        return false;
    }
    for (const QJsonValue& value : serializedTempoMap) {
        if (!value.isObject()) {
            if (errorText) *errorText = "Project contains an invalid tempo-map entry.";
            return false;
        }
        const QJsonObject point = value.toObject();
        const double beat = point.value("beat").toDouble(-1.0);
        const double bpm = point.value("bpm").toDouble(-1.0);
        if (!std::isfinite(beat) || beat < 0.0 || !std::isfinite(bpm) || bpm < 20.0 || bpm > 999.0) {
            if (errorText) *errorText = "Project tempo map contains an out-of-range beat or BPM.";
            return false;
        }
    }
    const QJsonArray serializedSignatureMap =
        root.value("transport").toObject().value("timeSignatureMap").toArray();
    if (serializedSignatureMap.size() > static_cast<qsizetype>(kMaxTimeSignatureMapPoints)) {
        if (errorText) *errorText = "Project time-signature map exceeds 4096 points.";
        return false;
    }
    for (const QJsonValue& value : serializedSignatureMap) {
        if (!value.isObject()) {
            if (errorText) *errorText = "Project contains an invalid time-signature entry.";
            return false;
        }
        const QJsonObject point = value.toObject();
        const double beat = point.value("beat").toDouble(-1.0);
        const int numerator = point.value("numerator").toInt(0);
        const int denominator = point.value("denominator").toInt(0);
        if (!std::isfinite(beat) || beat < 0.0 || numerator < 1 || numerator > 32 ||
            !validTimeSignatureDenominator(denominator)) {
            if (errorText) *errorText = "Project time-signature map contains invalid timing data.";
            return false;
        }
    }

    m_restoringProject = true;
    resetProjectModel(false);
    m_projectAraArchives.swap(loadedAraArchives);
    m_projectAraArchivesRestored = false;
    if (m_projectAraArchives.empty()) m_projectAraArchivesRestored = true;
    applyProjectPanLawDb(root.value("mixer").toObject().value("panLawDb").toDouble(-3.0));
    applyProjectSpeakerLayout(static_cast<VeraCoreSpeakerLayout>(
        root.value("mixer").toObject().value("speakerLayout").toInt(0)));
    const QJsonObject masterData = root.value("master").toObject();
    MasterBusState masterState;
    masterState.volume = static_cast<float>(masterData.value("volume").toDouble(1.0));
    masterState.pan = static_cast<float>(masterData.value("pan").toDouble(0.0));
    masterState.stereoSeparation = static_cast<float>(
        masterData.value("stereoSeparation").toDouble(0.0));
    masterState.muted = masterData.value("muted").toBool(false);
    const QJsonArray masterFxData = masterData.value("audioFx").toArray();
    for (int fxIndex = 0; fxIndex < masterFxData.size() && fxIndex < 128; ++fxIndex) {
        if (!masterFxData[fxIndex].isObject()) continue;
        const QJsonObject data = masterFxData[fxIndex].toObject();
        AudioFxSlot slot;
        slot.name = data.value("name").toString();
        slot.pluginPath = data.value("pluginPath").toString();
        slot.pluginUid = data.value("pluginUid").toString();
        slot.pluginType = data.value("pluginType").toString();
        slot.enabled = data.value("enabled").toBool(true);
        decodeProjectPluginState(data.value("pluginState").toString(), slot.pluginState);
        deserializePluginAutomation(data.value("automation"), slot.automation);
        if (!slot.name.trimmed().isEmpty()) masterState.audioFxSlots.push_back(std::move(slot));
    }
    m_mixerParameters->restoreMasterBus(masterState);
    const QJsonArray returnData = root.value("returns").toArray();
    for (int bus = 0; bus < returnData.size() && bus < kReturnBusCount; ++bus) {
        if (!returnData[bus].isObject()) continue;
        const QJsonObject item = returnData[bus].toObject();
        ReturnBusState state = m_mixerParameters->returnBuses()[static_cast<std::size_t>(bus)];
        state.name = item.value("name").toString(state.name);
        state.volume = static_cast<float>(item.value("volume").toDouble(1.0));
        state.pan = static_cast<float>(item.value("pan").toDouble(0.0));
        state.stereoSeparation = static_cast<float>(
            item.value("stereoSeparation").toDouble(0.0));
        state.muted = item.value("muted").toBool(false);
        state.soloed = item.value("soloed").toBool(false);
        state.preFader = item.value("preFader").toBool(false);
		state.audioFxSlots.clear();
		const QJsonArray returnFx = item.value("audioFx").toArray();
		for (int fxIndex = 0; fxIndex < returnFx.size() && fxIndex < 128; ++fxIndex) {
			if (!returnFx[fxIndex].isObject()) continue;
			const QJsonObject data = returnFx[fxIndex].toObject();
			AudioFxSlot slot;
			slot.name = data.value("name").toString();
			slot.pluginPath = data.value("pluginPath").toString();
			slot.pluginUid = data.value("pluginUid").toString();
			slot.pluginType = data.value("pluginType").toString();
			slot.enabled = data.value("enabled").toBool(true);
			decodeProjectPluginState(data.value("pluginState").toString(), slot.pluginState);
			deserializePluginAutomation(data.value("automation"), slot.automation);
			if (!slot.name.trimmed().isEmpty()) state.audioFxSlots.push_back(std::move(slot));
		}
        m_mixerParameters->restoreReturnBus(bus, state);
    }
    const QJsonObject transport = root.value("transport").toObject();
    m_projectPpq = std::clamp(
        transport.value("ppq").toInt(kLegacyDivision), kMinProjectPpq, kMaxProjectPpq);
    m_projectKeyRoot = std::clamp(transport.value("keyRoot").toInt(0), 0, 11);
    m_projectMinorScale = transport.value("minor").toBool(false);
    m_projectSnapBeats = finiteClamp(
        transport.value("snapBeats").toDouble(0.25), 0.0, 128.0, 0.25);
    if (m_transportWidget) {
        QSignalBlocker blocker(m_transportWidget);
        m_transportWidget->setTempo(transport.value("tempo").toDouble(kDefaultTempoBpm));
        m_transportWidget->setTimeSignature(transport.value("numerator").toInt(4),
                                             transport.value("denominator").toInt(4));
        m_transportWidget->setSnapBeats(m_projectSnapBeats);
        m_transportWidget->setKeyScale(m_projectKeyRoot, m_projectMinorScale);
        m_transportWidget->setPpq(m_projectPpq);
        m_projectSnapBeats = m_transportWidget->snapBeats();
    }
    if (m_playlistView) m_playlistView->setSnapBeats(m_projectSnapBeats);
    if (m_midiEditor) m_midiEditor->setProjectScale(m_projectKeyRoot, m_projectMinorScale);
    std::vector<MidiTempoPoint> loadedTempoMap;
    const QJsonArray loadedTempoPoints = transport.value("tempoMap").toArray();
    loadedTempoMap.reserve(qMin(loadedTempoPoints.size(), static_cast<qsizetype>(kMaxTempoMapPoints)));
    for (const QJsonValue& value : loadedTempoPoints) {
        if (!value.isObject() || loadedTempoMap.size() >= kMaxTempoMapPoints) continue;
        const QJsonObject point = value.toObject();
        loadedTempoMap.push_back({point.value("beat").toDouble(-1.0),
                                  point.value("bpm").toDouble(-1.0)});
    }
    m_tempoMap = normalizedTempoMap(
        loadedTempoMap, m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm);
    std::vector<MidiTimeSignaturePoint> loadedSignatureMap;
    const QJsonArray loadedSignaturePoints = transport.value("timeSignatureMap").toArray();
    loadedSignatureMap.reserve(qMin(loadedSignaturePoints.size(),
        static_cast<qsizetype>(kMaxTimeSignatureMapPoints)));
    for (const QJsonValue& value : loadedSignaturePoints) {
        if (!value.isObject() || loadedSignatureMap.size() >= kMaxTimeSignatureMapPoints) continue;
        const QJsonObject point = value.toObject();
        loadedSignatureMap.push_back({point.value("beat").toDouble(-1.0),
                                      point.value("numerator").toInt(0),
                                      point.value("denominator").toInt(0)});
    }
    m_timeSignatureMap = normalizedTimeSignatureMap(
        loadedSignatureMap,
        m_transportWidget ? m_transportWidget->signatureNumerator() : 4,
        m_transportWidget ? m_transportWidget->signatureDenominator() : 4);
    const double loopStart = transport.value("loopStart").toDouble(0.0);
    const double loopEnd = transport.value("loopEnd").toDouble(4.0);
    m_loopEnabled = transport.value("loopEnabled").toBool(false) &&
        std::isfinite(loopStart) && std::isfinite(loopEnd) && loopStart >= 0.0 && loopEnd - loopStart >= 0.25;
    m_loopStartBeat = m_loopEnabled ? loopStart : 0.0;
    m_loopEndBeat = m_loopEnabled ? loopEnd : 4.0;
    if (m_playlistView) m_playlistView->setLoopRange(m_loopEnabled, m_loopStartBeat, m_loopEndBeat);
    if (audioGeneral) audioGeneral->audioCore.SetLoopRange(m_loopEnabled, m_loopStartBeat, m_loopEndBeat);
    const double punchIn = transport.value("punchIn").toDouble(0.0);
    const double punchOut = transport.value("punchOut").toDouble(4.0);
    m_punchEnabled = transport.value("punchEnabled").toBool(false) &&
        std::isfinite(punchIn) && std::isfinite(punchOut) && punchIn >= 0.0 &&
        punchOut - punchIn >= 0.25;
    m_punchInBeat = m_punchEnabled ? punchIn : 0.0;
    m_punchOutBeat = m_punchEnabled ? punchOut : 4.0;
    const int loadedCountIn = transport.value("countInBars").toInt(0);
    m_countInBars = loadedCountIn == 1 || loadedCountIn == 2 || loadedCountIn == 4
        ? loadedCountIn : 0;
    m_midiRecordMode = transport.value("midiRecordMode").toInt(0) == 1 ? 1 : 0;
    m_midiRecordQuantizeBeats = normalizedMidiRecordQuantizeBeats(
        transport.value("midiRecordQuantizeBeats").toDouble(0.0));
    for (const auto& entry : m_midiRecordModeActions)
        entry.second->setChecked(entry.first == m_midiRecordMode);
    for (const auto& entry : m_midiRecordQuantizeActions)
        entry.second->setChecked(
            std::fabs(entry.first - m_midiRecordQuantizeBeats) < 1.0e-9);
    if (m_transportWidget) m_transportWidget->setPunchEnabled(m_punchEnabled);
    if (m_transportWidget) m_transportWidget->setCountInBars(m_countInBars);
    if (m_playlistView)
        m_playlistView->setPunchRange(m_punchEnabled, m_punchInBeat, m_punchOutBeat);

    for (const QJsonValue& value : trackData) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        const bool midi = item.value("type").toString() == "midi";
        std::shared_ptr<Track> track = midi
            ? std::static_pointer_cast<Track>(std::make_shared<MidiTrack>(item.value("name").toString("MIDI")))
            : std::static_pointer_cast<Track>(std::make_shared<AudioTrack>(item.value("name").toString("Audio")));
        const QJsonObject spatialData = item.value("spatial").toObject();
        VeraCoreSpatialPannerState spatialState;
        spatialState.mode = static_cast<VeraCoreSpatialRenderMode>(
            std::clamp(spatialData.value("mode").toInt(0), 0, 2));
        spatialState.azimuthDegrees = static_cast<float>(spatialData.value("azimuth").toDouble(
            item.value("pan").toDouble(0.0) * 30.0));
        spatialState.elevationDegrees = static_cast<float>(
            spatialData.value("elevation").toDouble(0.0));
        spatialState.distance = static_cast<float>(spatialData.value("distance").toDouble(0.0));
        spatialState.spread = static_cast<float>(spatialData.value("spread").toDouble(0.0));
        spatialState.lfeSend = static_cast<float>(spatialData.value("lfeSend").toDouble(0.0));
        spatialState.linkStereoPan = spatialData.value("linkStereoPan").toBool(true);
        m_mixerParameters->restoreTrackState(*track, {
            static_cast<float>(item.value("volume").toDouble(1.0)),
            static_cast<float>(item.value("pan").toDouble(0.0)),
            static_cast<float>(item.value("stereoSeparation").toDouble(0.0)),
            item.value("muted").toBool(false), item.value("soloed").toBool(false),
            item.value("armed").toBool(false), item.value("monitoring").toBool(false),
            spatialState});
        const QJsonArray sends = item.value("sends").toArray();
        for (int bus = 0; bus < sends.size() && bus < kReturnBusCount; ++bus)
            m_mixerParameters->restoreTrackSend(
                *track, bus, static_cast<float>(sends[bus].toDouble(0.0)));
        const QJsonArray fx = item.value("audioFx").toArray();
        for (int i = 0; i < fx.size() && i < 128; ++i) {
            AudioFxSlot slot;
            if (fx[i].isString()) {
                slot.name = fx[i].toString();
            } else {
                const QJsonObject data = fx[i].toObject();
                slot.name = data.value("name").toString();
                slot.pluginPath = data.value("pluginPath").toString();
                slot.pluginUid = data.value("pluginUid").toString();
                slot.pluginType = data.value("pluginType").toString();
                slot.enabled = data.value("enabled").toBool(true);
                decodeProjectPluginState(data.value("pluginState").toString(), slot.pluginState);
                deserializePluginAutomation(data.value("automation"), slot.automation);
            }
            if (!slot.name.trimmed().isEmpty()) track->AddAudioFx(slot);
        }
        if (auto* midiTrack = dynamic_cast<MidiTrack*>(track.get())) {
            midiTrack->ClearLayerSlots();
            const QJsonArray layers = item.value("layers").toArray();
            for (int i = 0; i < layers.size() && i < 128; ++i) {
                const QJsonObject layerData = layers[i].toObject();
                LayerSlot layer;
                layer.name = layerData.value("name").toString("Layer");
                layer.pluginPath = layerData.value("pluginPath").toString();
                layer.pluginUid = layerData.value("pluginUid").toString();
                layer.pluginType = layerData.value("pluginType").toString("Internal");
                decodeProjectPluginState(layerData.value("pluginState").toString(), layer.pluginState);
                layer.enabled = layerData.value("enabled").toBool(true);
                layer.gain = static_cast<float>(layerData.value("gain").toDouble(1.0));
                layer.pan = static_cast<float>(layerData.value("pan").toDouble(0.0));
                layer.transpose = layerData.value("transpose").toInt(0);
                layer.midiChannel = layerData.value("midiChannel").toInt(-1);
                deserializePluginAutomation(layerData.value("automation"), layer.automation);
                const QJsonArray outputRoutes = layerData.value("outputRoutes").toArray();
                for (int routeIndex = 0; routeIndex < outputRoutes.size() && routeIndex < 64; ++routeIndex) {
                    const QJsonObject routeData = outputRoutes[routeIndex].toObject();
                    PluginOutputRoute route;
                    route.stablePortId = routeData.value("stablePortId").toString().left(256);
                    route.displayName = routeData.value("name").toString().left(256);
                    route.outputBusIndex = routeData.value("bus").toInt(0);
                    route.destinationTrackIndex = routeData.value("destinationTrack").toInt(-1);
                    route.enabled = routeData.value("enabled").toBool(true);
                    layer.outputRoutes.push_back(std::move(route));
                }
                midiTrack->AddLayerSlot(layer);
            }
            if (midiTrack->GetLayerSlots().empty()) midiTrack->AddLayerSlot(LayerSlot{});
        }
        addTrack(std::move(track));
    }
    if (m_tracks.empty()) initDefaultTracks();

    for (const QJsonValue& value : documentData) {
        const QJsonObject item = value.toObject();
        auto doc = std::make_unique<MidiClipDocument>();
        doc->name = item.value("name").toString();
        doc->trackName = item.value("trackName").toString();
        doc->trackIndex = item.value("track").toInt(-1);
        if (doc->name.trimmed().isEmpty() || doc->trackIndex < 0 ||
            doc->trackIndex >= static_cast<int>(m_tracks.size()) ||
            m_tracks[doc->trackIndex]->GetType() != TrackType::Midi) continue;
        doc->startBeat = qMax(0.0, item.value("start").toDouble(0.0));
        doc->lengthBeats = qMax(0.000001, item.value("length").toDouble(4.0));
        doc->lastNoteLengthBeats = qMax(0.000001, item.value("lastNoteLength").toDouble(1.0));
        doc->tempoBpm = item.value("tempo").toDouble(kDefaultTempoBpm);
        std::vector<MidiTempoPoint> documentTempoMap;
        const QJsonArray tempoPoints = item.value("tempoMap").toArray();
        documentTempoMap.reserve(qMin(tempoPoints.size(), static_cast<qsizetype>(kMaxTempoMapPoints)));
        for (const QJsonValue& tempoValue : tempoPoints) {
            if (!tempoValue.isObject() || documentTempoMap.size() >= kMaxTempoMapPoints) continue;
            const QJsonObject tempoPoint = tempoValue.toObject();
            documentTempoMap.push_back({tempoPoint.value("beat").toDouble(-1.0),
                                        tempoPoint.value("bpm").toDouble(-1.0)});
        }
        doc->tempoMap = normalizedTempoMap(documentTempoMap, doc->tempoBpm);
        doc->timeSignatureNumerator = qBound(1, item.value("numerator").toInt(4), 32);
        const int serializedDenominator = item.value("denominator").toInt(4);
        doc->timeSignatureDenominator = validTimeSignatureDenominator(serializedDenominator)
            ? serializedDenominator : 4;
        std::vector<MidiTimeSignaturePoint> documentSignatureMap;
        const QJsonArray signaturePoints = item.value("timeSignatureMap").toArray();
        documentSignatureMap.reserve(qMin(signaturePoints.size(),
            static_cast<qsizetype>(kMaxTimeSignatureMapPoints)));
        for (const QJsonValue& signatureValue : signaturePoints) {
            if (!signatureValue.isObject() ||
                documentSignatureMap.size() >= kMaxTimeSignatureMapPoints) continue;
            const QJsonObject signaturePoint = signatureValue.toObject();
            documentSignatureMap.push_back({signaturePoint.value("beat").toDouble(-1.0),
                                            signaturePoint.value("numerator").toInt(0),
                                            signaturePoint.value("denominator").toInt(0)});
        }
        doc->timeSignatureMap = normalizedTimeSignatureMap(
            documentSignatureMap, doc->timeSignatureNumerator,
            doc->timeSignatureDenominator);
        const QJsonArray notes = item.value("notes").toArray();
        for (const QJsonValue& noteValue : notes) {
            const QJsonObject data = noteValue.toObject();
            MidiNote note;
            note.pitch = qBound(0, data.value("pitch").toInt(60), kPianoRollMaxPitch);
            note.startBeat = qMax(0.0, data.value("start").toDouble(0.0));
            note.lengthBeats = qMax(0.000001, data.value("length").toDouble(1.0));
            note.velocity = qBound(1, data.value("velocity").toInt(100), 127);
            note.pan = qBound(0, data.value("pan").toInt(64), 127);
            note.release = qBound(0, data.value("release").toInt(64), 127);
            note.modX = qBound(0, data.value("modX").toInt(64), 127);
            note.modY = qBound(0, data.value("modY").toInt(64), 127);
            note.colorGroup = qBound(0, data.value("color").toInt(0), 15);
            note.groupId = qMax(0, data.value("group").toInt(0));
            note.isKeyswitch = data.value("keyswitch").toBool(false);
            note.isSlide = data.value("slide").toBool(false);
            note.isPortamento = data.value("portamento").toBool(false);
            note.muted = data.value("muted").toBool(false);
            doc->notes.push_back(note);
        }
        const QJsonArray performance = item.value("performance").toArray();
        for (int i = 0; i < performance.size() && i < 1000000; ++i) {
            const QJsonObject data = performance[i].toObject();
            MidiEvent event;
            event.beat = qMax(0.0, data.value("beat").toDouble(0.0));
            event.status = static_cast<unsigned char>(data.value("status").toInt(0));
            event.data1 = static_cast<unsigned char>(data.value("data1").toInt(0));
            event.data2 = static_cast<unsigned char>(data.value("data2").toInt(0));
            if (isValidMidiChannelEvent(event.status, event.data1, event.data2))
                doc->performanceEvents.push_back(event);
        }
        const QString key = buildClipDocumentKey(item.value("contentId").toString());
        m_midiClipDocs[key] = std::move(doc);
    }

    {
        QSignalBlocker blocker(m_playlistView);
        std::map<QString, QString> midiNamesById;
        for (const QJsonValue& value : documentData) {
            const QJsonObject content = value.toObject();
            midiNamesById[content.value("contentId").toString()] = content.value("name").toString("Pattern");
        }
        for (const QJsonValue& value : midiClipData) {
            const QJsonObject item = value.toObject();
            const int track = item.value("track").toInt(-1);
            if (track < 0 || track >= static_cast<int>(m_tracks.size()) ||
                m_tracks[track]->GetType() != TrackType::Midi) continue;
            const QString name = midiNamesById[item.value("contentId").toString()];
            if (m_playlistView->addMidiClip(track, qMax(0.0, item.value("start").toDouble()),
                                            qMax(0.000001, item.value("length").toDouble(4.0)), name,
                                             item.value("clipId").toString(), item.value("contentId").toString(),
                                             qMax(0.0, item.value("offset").toDouble(0.0)),
                                             item.value("muted").toBool(false)) < 0) {
                continue;
            }
            m_playlistView->setSelectedClipsColor(QColor(item.value("color").toString()));
            m_playlistView->setSelectedClipsLoopEnabled(
                item.value("loopEnabled").toBool(false),
                item.value("loopStart").toDouble(-1.0),
                item.value("loopLength").toDouble(-1.0));
            m_playlistView->setSelectedClipsGroupId(item.value("groupId").toString());
            m_playlistView->setSelectedClipTakeState(
                item.value("takeGroupId").toString(), item.value("takeLane").toInt(0),
                item.value("takeActive").toBool(true));
            m_playlistView->setSelectedClipsLocked(item.value("locked").toBool(false));
        }
        std::map<QString, QJsonObject> audioContentsById;
        std::map<QString, std::vector<double>> audioTransientsById;
        std::map<QString, std::vector<AudioWarpMarker>> audioWarpMarkersById;
        for (const QJsonValue& value : audioContentData) {
            QJsonObject content = value.toObject();
            const AudioMediaResolution media = AudioMediaResolver::resolve(
                content.value("sourcePath").toString(), path);
            if (media.online) {
                content["sourcePath"] = media.resolvedPath;
                if (media.relocated) ++m_autoRelinkedMediaCount;
            }
            const QString contentId = content.value("contentId").toString();
            std::vector<double> markers;
            const QJsonArray markerData = content.value("transientSeconds").toArray();
            markers.reserve(static_cast<size_t>(markerData.size()));
            for (const QJsonValue& marker : markerData) markers.push_back(marker.toDouble());
            std::vector<AudioWarpMarker> warpMarkers;
            const QJsonArray warpData = content.value("warpMarkers").toArray();
            warpMarkers.reserve(static_cast<size_t>(warpData.size()));
            for (const QJsonValue& markerValue : warpData) {
                const QJsonObject marker = markerValue.toObject();
                warpMarkers.push_back({marker.value("sourceSeconds").toDouble(),
                                       marker.value("targetBeat").toDouble()});
            }
            audioContentsById[contentId] = content;
            audioTransientsById[contentId] = std::move(markers);
            audioWarpMarkersById[contentId] = std::move(warpMarkers);
        }
        for (const QJsonValue& value : audioClipData) {
            const QJsonObject item = value.toObject();
            const int track = item.value("track").toInt(-1);
            if (track < 0 || track >= static_cast<int>(m_tracks.size()) ||
                m_tracks[track]->GetType() != TrackType::Audio) continue;
            const QJsonObject content = audioContentsById[item.value("contentId").toString()];
            const double length = qMax(0.000001, item.value("length").toDouble(4.0));
            std::vector<AudioGainEnvelopePoint> gainEnvelope;
            const QJsonArray gainEnvelopeData = item.value("gainEnvelope").toArray();
            gainEnvelope.reserve(static_cast<size_t>(gainEnvelopeData.size()));
            for (const QJsonValue& pointValue : gainEnvelopeData) {
                const QJsonObject point = pointValue.toObject();
                gainEnvelope.push_back({point.value("beat").toDouble(),
                    static_cast<float>(point.value("gain").toDouble(1.0))});
            }
            if (m_playlistView->addAudioClip(
                    track, qMax(0.0, item.value("start").toDouble()), length,
                    content.value("name").toString("Audio"), content.value("sourcePath").toString(),
                    content.value("sourceTempo").toDouble(0.0), content.value("pitch").toDouble(0.0),
                    content.value("beatsPerBar").toDouble(4.0), content.value("sourceDuration").toDouble(0.0),
                    item.value("clipId").toString(), item.value("contentId").toString(),
                    qMax(0.0, item.value("offset").toDouble(0.0)),
                    item.value("warpLength").toDouble(length),
                    content.value("tempoConfidence").toDouble(0.0),
                    content.value("sourceSignatureNumerator").toInt(0),
                    content.value("sourceSignatureDenominator").toInt(0),
                    content.value("tempoDetected").toBool(false),
                    audioTransientsById[item.value("contentId").toString()],
                    audioWarpMarkersById[item.value("contentId").toString()],
                    gainEnvelope,
                    item.value("muted").toBool(false)) >= 0) {
                m_playlistView->updateSelectedAudioClipSync(
                    content.value("sourceTempo").toDouble(0.0), content.value("pitch").toDouble(0.0), -1.0,
                    content.value("tempoConfidence").toDouble(0.0),
                    content.value("sourceSignatureNumerator").toInt(0),
                    content.value("sourceSignatureDenominator").toInt(0),
                    content.value("tempoDetected").toBool(false));
                m_playlistView->updateSelectedAudioClipSyncMode(
                    content.value("syncMode").toInt(0), content.value("targetBars").toDouble(0.0));
                m_playlistView->updateSelectedAudioClipProcessing(
                    static_cast<float>(content.value("gain").toDouble(1.0)),
                    static_cast<float>(content.value("pan").toDouble(0.0)),
                    content.value("normalize").toBool(false),
                    content.value("reverse").toBool(false),
                    content.value("reversePolarity").toBool(false));
                m_playlistView->updateSelectedAudioClipFades(
                    item.value("fadeIn").toDouble(0.0),
                    item.value("fadeOut").toDouble(0.0));
                m_playlistView->updateSelectedAudioClipFadeCurves(
                    item.value("fadeInCurve").toDouble(0.0),
                    item.value("fadeOutCurve").toDouble(0.0));
                m_playlistView->setSelectedClipsColor(QColor(item.value("color").toString()));
                m_playlistView->setSelectedClipsLoopEnabled(
                    item.value("loopEnabled").toBool(false),
                    item.value("loopStart").toDouble(-1.0),
                    item.value("loopLength").toDouble(-1.0));
                m_playlistView->setSelectedClipsGroupId(item.value("groupId").toString());
                m_playlistView->setSelectedClipTakeState(
                    item.value("takeGroupId").toString(), item.value("takeLane").toInt(0),
                    item.value("takeActive").toBool(true));
                m_playlistView->setSelectedClipsLocked(item.value("locked").toBool(false));
            }
        }
        m_playlistView->normalizeTakeGroups();
        m_missingMediaCount = m_playlistView->refreshAudioMediaStatus();
    }
    m_playlistView->resetArrangementHistory();
    if (!m_trackWidgets.empty()) onTrackSelected(m_trackWidgets.front(), m_tracks.front().get());
    syncRealtimeMidiLayersToAudio();
    syncMixerRoutingToAudio();
    syncMidiTimelineToAudio();
    syncTempoMapToAudio();
    syncTimeSignatureMapToAudio();
    syncAudioTimelineToAudio();
    m_restoringProject = false;
    m_mixerParameters->republishProject(m_tracks);
    restoreProjectAraArchives();
    syncPluginTransportToHost();
    syncAudioInputState();
    m_projectPassthroughRoot = root;
    m_projectIntegrityState = loadedIntegrity;
    updateProjectWindowTitle();
    return true;
}

void VeraCoreQT::initDefaultTracks() {
    auto midiTrack = std::make_shared<MidiTrack>("Inst 1");
    auto audioTrack = std::make_shared<AudioTrack>("Audio 1");

    addTrack(midiTrack);
    addTrack(audioTrack);
    if (!m_trackWidgets.empty()) {
        onTrackSelected(m_trackWidgets.front(), midiTrack.get());
    }
}

QStringList VeraCoreQT::stemRecoveryRoots() const
{
    QStringList roots;
    const QString appData = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    if (!appData.isEmpty())
        roots.push_back(QDir(appData).filePath(QStringLiteral("Unsaved/Stems")));
    if (!m_projectPath.isEmpty())
        roots.push_back(QDir(QFileInfo(m_projectPath).absolutePath()).filePath(
            QStringLiteral("VeraCore Assets/Stems")));
    roots.removeDuplicates();
    return roots;
}

void VeraCoreQT::refreshStemRecoveryAction()
{
    if (!m_stemRecoveryAction) return;
    QVector<StemRecoveryJob> jobs = StemSeparationService::scanRecoverableJobs(
        stemRecoveryRoots(), 128);
    const QString activeJob = m_stemSeparator ? m_stemSeparator->activeJobId() : QString();
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [&activeJob](const StemRecoveryJob& job) {
        return !activeJob.isEmpty() && job.jobId == activeJob;
    }), jobs.end());
    m_stemRecoveryAction->setEnabled(!jobs.isEmpty());
    m_stemRecoveryAction->setText(jobs.isEmpty()
        ? tr("Stem Job Recovery...")
        : tr("Recover Interrupted Stem Jobs (%1)...").arg(jobs.size()));
}

void VeraCoreQT::showStemRecoveryDialog()
{
    QVector<StemRecoveryJob> jobs = StemSeparationService::scanRecoverableJobs(
        stemRecoveryRoots(), 128);
    const QString activeJob = m_stemSeparator ? m_stemSeparator->activeJobId() : QString();
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [&activeJob](const StemRecoveryJob& job) {
        return !activeJob.isEmpty() && job.jobId == activeJob;
    }), jobs.end());
    if (jobs.isEmpty()) {
        QMessageBox::information(this, tr("Stem Job Recovery"),
                                 tr("No interrupted stem jobs were found."));
        refreshStemRecoveryAction();
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Stem Job Recovery"));
    dialog.resize(820, 390);
    auto* layout = new QVBoxLayout(&dialog);
    auto* help = new QLabel(tr(
        "Select a completed job to import its validated outputs, or retry an interrupted job. "
        "The matching source clip must be selected in the Playlist. Old manifests remain as provenance."),
        &dialog);
    help->setWordWrap(true);
    layout->addWidget(help);
    auto* table = new QTableWidget(jobs.size(), 5, &dialog);
    table->setHorizontalHeaderLabels({tr("State"), tr("Source"), tr("Stems"),
                                      tr("Updated"), tr("Source status")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int row = 0; row < jobs.size(); ++row) {
        const StemRecoveryJob& job = jobs[row];
        auto* state = new QTableWidgetItem(job.state);
        auto* source = new QTableWidgetItem(QFileInfo(job.sourcePath).fileName());
        source->setToolTip(job.sourcePath);
        auto* stems = new QTableWidgetItem(QString::number(job.expectedStems.size()));
        const QDateTime updated = QDateTime::fromString(job.updatedUtc, Qt::ISODateWithMs);
        auto* time = new QTableWidgetItem(updated.isValid()
            ? updated.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : job.updatedUtc);
        auto* status = new QTableWidgetItem(!job.sourceValid
            ? tr("Source missing/changed")
            : (job.state == QStringLiteral("completed")
                ? (job.outputsComplete ? tr("Ready to import") : tr("Outputs incomplete"))
                : tr("Ready to retry")));
        table->setItem(row, 0, state);
        table->setItem(row, 1, source);
        table->setItem(row, 2, stems);
        table->setItem(row, 3, time);
        table->setItem(row, 4, status);
    }
    layout->addWidget(table);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QPushButton* retry = buttons->addButton(tr("Retry Selected"),
                                            QDialogButtonBox::ActionRole);
    QPushButton* openFolder = buttons->addButton(tr("Open Output Folder"),
                                                 QDialogButtonBox::ActionRole);
    retry->setEnabled(false);
    openFolder->setEnabled(false);
    connect(table, &QTableWidget::itemSelectionChanged, &dialog,
            [table, retry, openFolder, &jobs]() {
        const int row = table->currentRow();
        const bool selected = row >= 0 && row < jobs.size();
        retry->setEnabled(selected);
        openFolder->setEnabled(selected);
        if (selected) {
            const StemRecoveryJob& job = jobs[row];
            retry->setText(job.state == QStringLiteral("completed") && job.outputsComplete
                ? QObject::tr("Import Completed") : QObject::tr("Retry Selected"));
        }
    });
    connect(openFolder, &QPushButton::clicked, &dialog, [&jobs, table]() {
        const int row = table->currentRow();
        if (row < 0 || row >= jobs.size()) return;
        QDesktopServices::openUrl(QUrl::fromLocalFile(jobs[row].outputDirectory));
    });
    connect(retry, &QPushButton::clicked, &dialog, [this, &dialog, &jobs, table]() {
        const int row = table->currentRow();
        if (row < 0 || row >= jobs.size()) return;
        const StemRecoveryJob job = jobs[row];
        if (!job.sourceValid) {
            QMessageBox::warning(&dialog, tr("Stem Job Recovery"),
                                 tr("The original source is missing or changed."));
            return;
        }
        PlaylistView::AudioClipInfo selectedClip;
        if (!m_playlistView || !m_playlistView->selectedAudioClipInfo(&selectedClip)) {
            QMessageBox::information(&dialog, tr("Stem Job Recovery"),
                                     tr("Select the matching source audio clip in the Playlist first."));
            return;
        }
        auto normalizedPath = [](const QString& path) {
            const QFileInfo info(path);
            const QString canonical = info.canonicalFilePath();
            return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
        };
        if (normalizedPath(selectedClip.sourcePath).compare(
                normalizedPath(job.sourcePath), Qt::CaseInsensitive) != 0) {
            QMessageBox::information(&dialog, tr("Stem Job Recovery"),
                                     tr("The selected clip does not use this job's source audio."));
            return;
        }
        const int stemCount = job.expectedStems.size();
        dialog.accept();
        if (job.state == QStringLiteral("completed") && job.outputsComplete) {
            m_pendingStemClip = selectedClip;
            m_pendingStemJobId = job.jobId;
            m_pendingStemManifestPath = job.manifestPath;
            m_pendingStemInputRendered = job.inputWasRendered;
            importSeparatedStems(job.jobId, job.expectedStems, job.outputPaths);
            refreshStemRecoveryAction();
            return;
        }
        const bool started = startStemSeparationForClip(selectedClip, stemCount);
        if (started && m_stemSeparator && m_stemSeparator->isBusy()) {
            QString error;
            if (!StemSeparationService::markInterruptedJobRetried(
                    job.manifestPath, m_stemSeparator->activeJobId(), &error)) {
                ui.statusBar->showMessage(error, 5000);
            }
        }
        refreshStemRecoveryAction();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    table->selectRow(0);
    dialog.exec();
}

void VeraCoreQT::startStemSeparation(int stemCount)
{
    if (!m_playlistView || !m_stemSeparator) return;
    if (m_stemSeparator->isBusy() || m_stemBatchActive) {
        QMessageBox::information(this, tr("Separate Stems"),
                                 tr("A stem-separation job is already running."));
        return;
    }
    if (stemCount != 2 && stemCount != 4 && stemCount != 5) {
        QMessageBox::warning(this, tr("Separate Stems"),
                             tr("Choose a 2, 4, or 5-stem model."));
        return;
    }

    const QStringList selectedIds = m_playlistView->selectedClipIds();
    std::vector<PlaylistView::AudioClipInfo> selectedClips;
    for (const PlaylistView::AudioClipInfo& clip : m_playlistView->getAudioClips()) {
        if (selectedIds.contains(clip.clipId) &&
            std::none_of(selectedClips.begin(), selectedClips.end(),
                [&clip](const PlaylistView::AudioClipInfo& item) {
                    return item.clipId == clip.clipId;
                })) {
            selectedClips.push_back(clip);
        }
    }
    if (selectedClips.empty()) {
        PlaylistView::AudioClipInfo primary;
        if (m_playlistView->selectedAudioClipInfo(&primary))
            selectedClips.push_back(primary);
    }
    if (selectedClips.empty()) {
        QMessageBox::warning(this, tr("Separate Stems"),
                             tr("Select one or more audio clips first."));
        return;
    }
    constexpr int kMaximumStemBatchClips = 64;
    if (selectedClips.size() > static_cast<size_t>(kMaximumStemBatchClips)) {
        QMessageBox::warning(this, tr("Separate Stems"),
            tr("A stem batch is limited to %1 audio clips.").arg(kMaximumStemBatchClips));
        return;
    }
    std::sort(selectedClips.begin(), selectedClips.end(),
        [](const PlaylistView::AudioClipInfo& left,
           const PlaylistView::AudioClipInfo& right) {
            if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
            if (left.trackIndex != right.trackIndex) return left.trackIndex < right.trackIndex;
            return left.clipId < right.clipId;
        });
    const qint64 resultingTrackCount = static_cast<qint64>(m_tracks.size()) +
        static_cast<qint64>(selectedClips.size()) * stemCount;
    if (resultingTrackCount > 256) {
        QMessageBox::warning(this, tr("Separate Stems"),
                             tr("The separated tracks would exceed the 256-track limit."));
        return;
    }

    if (selectedClips.size() == 1) {
        startStemSeparationForClip(selectedClips.front(), stemCount);
        return;
    }

    m_stemBatchQueue.clear();
    for (const PlaylistView::AudioClipInfo& clip : selectedClips)
        m_stemBatchQueue.push_back({clip, stemCount});
    m_stemBatchTotal = static_cast<int>(selectedClips.size());
    m_stemBatchProcessed = 0;
    m_stemBatchSucceeded = 0;
    m_stemBatchFailed = 0;
    m_stemBatchCancelRequested = false;
    m_stemBatchActive = true;
    ui.statusBar->showMessage(tr("Starting stem batch: %1 clips.").arg(m_stemBatchTotal));
    startNextStemBatchItem();
}

int VeraCoreQT::stemBatchOverallProgress(int itemPercent) const
{
    const int boundedItemPercent = qBound(0, itemPercent, 100);
    if (!m_stemBatchActive || m_stemBatchTotal <= 0) return boundedItemPercent;
    return qBound(0, (m_stemBatchProcessed * 100 + boundedItemPercent) /
        m_stemBatchTotal, 100);
}

void VeraCoreQT::startNextStemBatchItem()
{
    if (!m_stemBatchActive) return;
    if (m_stemBatchCancelRequested || m_stemBatchQueue.empty()) {
        finishStemBatch();
        return;
    }

    const StemBatchItem item = m_stemBatchQueue.front();
    m_stemBatchQueue.pop_front();
    PlaylistView::AudioClipInfo currentClip;
    bool found = false;
    if (m_playlistView) {
        for (const PlaylistView::AudioClipInfo& clip : m_playlistView->getAudioClips()) {
            if (clip.clipId == item.clip.clipId && clip.contentId == item.clip.contentId) {
                currentClip = clip;
                found = true;
                break;
            }
        }
    }
    if (!found || currentClip.sourcePath.isEmpty() || currentClip.mediaOffline ||
        !isFiniteBeatRange(currentClip.startBeat, currentClip.lengthBeats) ||
        currentClip.trackIndex < 0 ||
        currentClip.trackIndex >= static_cast<int>(m_tracks.size())) {
        ++m_stemBatchProcessed;
        ++m_stemBatchFailed;
        ui.statusBar->showMessage(tr("Skipped missing, changed, or offline batch clip (%1/%2).")
            .arg(m_stemBatchProcessed).arg(m_stemBatchTotal), 5000);
        QTimer::singleShot(0, this, &VeraCoreQT::startNextStemBatchItem);
        return;
    }

    if (!startStemSeparationForClip(currentClip, item.stemCount)) {
        if (m_stemBatchCancelRequested) {
            finishStemBatch();
            return;
        }
        ++m_stemBatchProcessed;
        ++m_stemBatchFailed;
        QTimer::singleShot(0, this, &VeraCoreQT::startNextStemBatchItem);
    }
}

void VeraCoreQT::finishStemBatch()
{
    if (!m_stemBatchActive) return;
    const int total = m_stemBatchTotal;
    const int processed = m_stemBatchProcessed;
    const int succeeded = m_stemBatchSucceeded;
    const int failed = m_stemBatchFailed;
    const bool cancelled = m_stemBatchCancelRequested;
    m_stemBatchQueue.clear();
    m_stemBatchActive = false;
    m_stemBatchCancelRequested = false;
    m_stemBatchTotal = 0;
    m_stemBatchProcessed = 0;
    m_stemBatchSucceeded = 0;
    m_stemBatchFailed = 0;
    if (m_stemProgressDialog) {
        m_stemProgressDialog->close();
        m_stemProgressDialog->deleteLater();
        m_stemProgressDialog = nullptr;
    }
    ui.statusBar->showMessage(cancelled
        ? tr("Stem batch cancelled: %1/%2 processed, %3 imported, %4 failed.")
            .arg(processed).arg(total).arg(succeeded).arg(failed)
        : tr("Stem batch complete: %1 imported, %2 failed.")
            .arg(succeeded).arg(failed), 8000);
}

bool VeraCoreQT::startStemSeparationForClip(
    const PlaylistView::AudioClipInfo& clip, int stemCount)
{
    if (!m_playlistView || !m_stemSeparator) return false;
    auto reportError = [this](const QString& message, bool critical = true) {
        if (m_stemBatchActive) ui.statusBar->showMessage(message, 5000);
        else if (critical) QMessageBox::critical(this, tr("Separate Stems"), message);
        else QMessageBox::warning(this, tr("Separate Stems"), message);
    };
    if (m_stemSeparator->isBusy()) {
        reportError(tr("A stem-separation job is already running."), false);
        return false;
    }
    if (stemCount != 2 && stemCount != 4 && stemCount != 5) {
        reportError(tr("Choose a 2, 4, or 5-stem model."), false);
        return false;
    }
    if (clip.sourcePath.isEmpty() || clip.mediaOffline ||
        !isFiniteBeatRange(clip.startBeat, clip.lengthBeats) ||
        clip.trackIndex < 0 || clip.trackIndex >= static_cast<int>(m_tracks.size())) {
        reportError(tr("The selected audio clip is offline or invalid."), false);
        return false;
    }
    if (static_cast<int>(m_tracks.size()) + stemCount > 256) {
        reportError(tr("The separated tracks would exceed the 256-track limit."), false);
        return false;
    }

    const QString assetsRoot = m_projectPath.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
              .filePath(QStringLiteral("Unsaved/Stems"))
        : QDir(QFileInfo(m_projectPath).absolutePath())
              .filePath(QStringLiteral("VeraCore Assets/Stems"));
    const QString outputDirectory = QDir(assetsRoot).filePath(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString requestedBackend = QSettings().value(
        "Audio/StemSeparatorBackend", QStringLiteral("Auto")).toString().trimmed();
    const bool spleeterAvailable = StemSeparationService::resolveRuntime(
        nullptr, nullptr, nullptr);
    const bool demucsAvailable = StemSeparationService::resolveDemucsRuntime(
        nullptr, nullptr, nullptr);
    const bool forceSpleeter = requestedBackend.compare(
        QStringLiteral("Spleeter"), Qt::CaseInsensitive) == 0;
    const bool forceDemucs = requestedBackend.compare(
        QStringLiteral("Demucs"), Qt::CaseInsensitive) == 0;
    const bool compatibleRuntime = forceSpleeter ? spleeterAvailable
        : (forceDemucs ? (demucsAvailable && stemCount != 5)
                       : (spleeterAvailable || (demucsAvailable && stemCount != 5)));
    if (!compatibleRuntime) {
        reportError(forceDemucs && stemCount == 5
                ? tr("Demucs supports VeraCore 2/4-stem fallback; choose Spleeter for 5 stems.")
                : tr("No compatible Spleeter/Demucs runtime was found. Configure Preferences."));
        return false;
    }
    if (!QDir().mkpath(outputDirectory)) {
        reportError(tr("Could not create the stem output directory."));
        return false;
    }
    const QString renderedInputPath = QDir(outputDirectory).filePath(
        QStringLiteral("VeraCoreStemInput.wav"));

    m_pendingStemClip = clip;
    m_pendingStemJobId.clear();
    m_pendingStemManifestPath.clear();
    m_pendingStemInputRendered = false;
    m_stemProgressDialog = new QProgressDialog(
        tr("Rendering the selected clip for stem separation..."), tr("Cancel"),
        0, 100, this);
    m_stemProgressDialog->setWindowTitle(tr("Separate Stems"));
    m_stemProgressDialog->setWindowModality(Qt::NonModal);
    m_stemProgressDialog->setAutoClose(false);
    m_stemProgressDialog->setAutoReset(false);
    m_stemProgressDialog->setMinimumDuration(0);
    connect(m_stemProgressDialog, &QProgressDialog::canceled, this, [this]() {
        if (m_stemBatchActive) {
            m_stemBatchCancelRequested = true;
            m_stemBatchQueue.clear();
        }
        if (m_stemSeparator) m_stemSeparator->cancel();
    });
    m_stemProgressDialog->show();

    const int configuredSampleRate = QSettings().value("SRate", 44100).toInt();
    const int sampleRate = safeRenderSampleRate(
        audioGeneral ? audioGeneral->audioCore.GetStreamSampleRate() : 0.0,
        configuredSampleRate);
    const double projectTempo = m_transportWidget
        ? m_transportWidget->tempo() : kDefaultTempoBpm;
    const TempoClock projectClock(m_tempoMap, projectTempo);
    const double renderedDuration = projectClock.secondsAtBeat(
        clip.startBeat + clip.lengthBeats) - projectClock.secondsAtBeat(clip.startBeat);
    QString error;
    if (!hasRenderStorageCapacity(renderedInputPath, sampleRate,
            renderedDuration, &error)) {
        m_stemProgressDialog->close();
        m_stemProgressDialog->deleteLater();
        m_stemProgressDialog = nullptr;
        reportError(error);
        return false;
    }

    OfflineAudioClip renderClip;
    renderClip.trackIndex = clip.trackIndex;
    renderClip.clipId = clip.clipId;
    renderClip.contentId = clip.contentId;
    renderClip.name = clip.name;
    renderClip.sourcePath = clip.sourcePath;
    renderClip.startBeat = 0.0;
    renderClip.lengthBeats = clip.lengthBeats;
    renderClip.sourceOffsetBeats = clip.sourceOffsetBeats;
    renderClip.loopEnabled = clip.loopEnabled;
    renderClip.loopStartBeats = clip.loopStartBeats;
    renderClip.loopLengthBeats = clip.loopLengthBeats;
    renderClip.warpLengthBeats = clip.warpLengthBeats;
    renderClip.sourceDurationSeconds = clip.sourceDurationSeconds;
    renderClip.warpMarkers = clip.warpMarkers;
    renderClip.sourceTempoBpm = clip.sourceTempoBpm;
    renderClip.pitchShiftSemitones = clip.pitchShiftSemitones;
    renderClip.syncMode = clip.syncMode;
    renderClip.targetBars = clip.targetBars;
    renderClip.beatsPerBar = clip.beatsPerBar;
    renderClip.gain = clip.gain;
    renderClip.pan = clip.pan;
    renderClip.normalize = clip.normalize;
    renderClip.reverse = clip.reverse;
    renderClip.reversePolarity = clip.reversePolarity;
    renderClip.fadeInBeats = clip.fadeInBeats;
    renderClip.fadeOutBeats = clip.fadeOutBeats;
    renderClip.gainEnvelope = clip.gainEnvelope;
    renderClip.fadeInCurve = clip.fadeInCurve;
    renderClip.fadeOutCurve = clip.fadeOutCurve;
    const bool rendered = renderOfflineWavWithSynth(
        renderedInputPath, {}, {renderClip}, clip.lengthBeats, sampleRate, nullptr,
        [this, dialog = m_stemProgressDialog](quint64 completed, quint64 total) {
            QApplication::processEvents(QEventLoop::AllEvents, 5);
            if (!dialog || dialog->wasCanceled()) return false;
            const int value = total > 0
                ? static_cast<int>(qBound<quint64>(0,
                    completed * 15u / total, 15)) : 0;
            dialog->setValue(stemBatchOverallProgress(value));
            if (m_stemBatchActive) dialog->setLabelText(
                tr("Batch %1/%2 — Rendering clip...")
                    .arg(m_stemBatchProcessed + 1).arg(m_stemBatchTotal));
            return true;
        }, true, clip.startBeat);
    if (!rendered) {
        const bool cancelled = m_stemProgressDialog &&
            m_stemProgressDialog->wasCanceled();
        if (m_stemProgressDialog) {
            m_stemProgressDialog->close();
            m_stemProgressDialog->deleteLater();
            m_stemProgressDialog = nullptr;
        }
        m_pendingStemInputRendered = false;
        if (cancelled) {
            if (m_stemBatchActive) {
                m_stemBatchCancelRequested = true;
                m_stemBatchQueue.clear();
            }
            ui.statusBar->showMessage(tr("Stem separation cancelled."), 3000);
        } else {
            reportError(tr("Could not render the processed clip for stem separation."));
        }
        return false;
    }
    QString renderedHeaderError;
    const AudioFileProbeResult renderedProbe = AudioFileDecoder::probeAudio(
        renderedInputPath);
    if (!AudioCodecBackend::hasValidContainerHeader(renderedInputPath,
            AudioCodecFormat::Wav, &renderedHeaderError) ||
        !renderedProbe.success || renderedProbe.sampleRate != sampleRate ||
        renderedProbe.durationSeconds() <= 0.0) {
        if (m_stemProgressDialog) {
            m_stemProgressDialog->close();
            m_stemProgressDialog->deleteLater();
            m_stemProgressDialog = nullptr;
        }
        m_pendingStemInputRendered = false;
        reportError(renderedHeaderError.isEmpty()
                ? tr("The rendered stem input failed WAV validation.")
                : renderedHeaderError);
        return false;
    }
    m_pendingStemInputRendered = true;
    m_stemProgressDialog->setValue(stemBatchOverallProgress(15));
    m_stemProgressDialog->setLabelText(m_stemBatchActive
        ? tr("Batch %1/%2 — Starting stem backend...")
            .arg(m_stemBatchProcessed + 1).arg(m_stemBatchTotal)
        : tr("Starting stem backend..."));

    if (!m_stemSeparator->startSeparation(
            renderedInputPath, outputDirectory, stemCount, &error,
            clip.sourcePath, true)) {
        m_stemProgressDialog->close();
        m_stemProgressDialog->deleteLater();
        m_stemProgressDialog = nullptr;
        m_pendingStemInputRendered = false;
        reportError(error);
        return false;
    }
    m_pendingStemJobId = m_stemSeparator->activeJobId();
    m_pendingStemManifestPath = m_stemSeparator->activeManifestPath();
    return true;
}

bool VeraCoreQT::importSeparatedStems(const QString& jobId,
                                      const QStringList& stemNames,
                                      const QStringList& stemPaths)
{
    if (jobId.isEmpty() || jobId != m_pendingStemJobId) return false;
    auto reportError = [this](const QString& message, bool critical = true) {
        if (m_stemBatchActive) ui.statusBar->showMessage(message, 5000);
        else if (critical) QMessageBox::critical(this, tr("Separate Stems"), message);
        else QMessageBox::warning(this, tr("Separate Stems"), message);
    };
    const PlaylistView::AudioClipInfo source = m_pendingStemClip;
    const QString manifestPath = m_pendingStemManifestPath;
    const bool inputWasRendered = m_pendingStemInputRendered;
    m_pendingStemJobId.clear();
    m_pendingStemManifestPath.clear();
    m_pendingStemInputRendered = false;
    if (!m_playlistView || stemNames.isEmpty() || stemNames.size() != stemPaths.size()) {
        reportError(tr("The stem result is incomplete."));
        return false;
    }
    if (!manifestPath.isEmpty()) {
        QString sourceFreshnessError;
        if (!StemSeparationService::validateCompletedJobSource(
                manifestPath, source.sourcePath, &sourceFreshnessError)) {
            reportError(sourceFreshnessError, false);
            return false;
        }
    }
    const auto currentClips = m_playlistView->getAudioClips();
    const auto currentSource = std::find_if(currentClips.begin(), currentClips.end(),
        [&source](const PlaylistView::AudioClipInfo& clip) {
            return clip.clipId == source.clipId && clip.contentId == source.contentId;
        });
    if (currentSource == currentClips.end()) {
        reportError(tr("Separation completed, but the source clip is no longer in this project."), false);
        return false;
    }
    if (!stemSourceClipMatchesSnapshot(*currentSource, source)) {
        reportError(tr("Separation completed, but the source clip changed. The stale stems were not imported; retry separation."), false);
        return false;
    }
    if (static_cast<int>(m_tracks.size()) + stemPaths.size() > 256) {
        reportError(tr("The separated tracks would exceed the 256-track limit."), false);
        return false;
    }

    std::vector<AudioFileProbeResult> probes;
    probes.reserve(static_cast<size_t>(stemPaths.size()));
    for (const QString& path : stemPaths) {
        const AudioFileProbeResult probe = AudioFileDecoder::probeAudio(path);
        if (!probe.success || probe.durationSeconds() <= 0.0) {
            reportError(tr("A separated stem is unreadable: %1")
                .arg(QFileInfo(path).fileName()));
            return false;
        }
        probes.push_back(probe);
    }

    const int trackInsertIndex = static_cast<int>(m_tracks.size());
    const int selectedBefore = m_selectedTrackIndex;
    const bool captureUndo = !m_restoringProject && !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    m_playlistView->beginArrangementTransaction();
    m_playlistView->markArrangementTransactionChanged();

    double commonStemGain = inputWasRendered ? 1.0 : (std::isfinite(source.gain)
        ? qBound(0.0, static_cast<double>(source.gain), 4.0) : 1.0);
    if (!inputWasRendered && source.normalize) {
        const DecodedAudioFile* decodedSource = cachedDecodedAudio(source.sourcePath);
        commonStemGain *= normalizationGainForDecoded(decodedSource);
    }
    const bool normalizationGainLimited = commonStemGain > 8.0;
    const float stemTrackGain = static_cast<float>(commonStemGain > 4.0
        ? qBound(1.0, commonStemGain / 4.0, 2.0) : 1.0);
    const float stemClipGain = static_cast<float>(
        qBound(0.0, commonStemGain / stemTrackGain, 4.0));

    std::vector<std::shared_ptr<Track>> addedTracks;
    addedTracks.reserve(static_cast<size_t>(stemNames.size()));
    for (const QString& stem : stemNames) {
        QString displayName = stem;
        if (!displayName.isEmpty()) displayName[0] = displayName[0].toUpper();
        auto track = std::make_shared<AudioTrack>(
            QStringLiteral("%1 — %2").arg(source.name, displayName));
        track->SetVolume(stemTrackGain);
        addedTracks.push_back(track);
        addTrack(track);
    }

    auto rollbackStemImport = [this, &addedTracks, selectedBefore,
                               nonArrangementDirtyBefore]() {
        if (m_playlistView) m_playlistView->cancelArrangementTransaction();
        for (auto it = addedTracks.rbegin(); it != addedTracks.rend(); ++it) {
            const auto found = std::find(m_tracks.begin(), m_tracks.end(), *it);
            if (found != m_tracks.end()) m_tracks.erase(found);
        }
        rebuildTrackWidgets(selectedBefore);
        m_nonArrangementDirty = nonArrangementDirtyBefore;
        syncMixerRoutingToAudio();
        syncAudioTimelineToAudio();
        refreshProjectDirtyFromArrangement(false);
    };

    const QVector<QColor> colors = {
        QColor(QStringLiteral("#5FA8D3")), QColor(QStringLiteral("#E76F51")),
        QColor(QStringLiteral("#8AB17D")), QColor(QStringLiteral("#C77DFF")),
        QColor(QStringLiteral("#E9C46A"))
    };
    int importedClipCount = 0;
    for (int index = 0; index < stemPaths.size(); ++index) {
        QString displayName = stemNames[index];
        if (!displayName.isEmpty()) displayName[0] = displayName[0].toUpper();
        const int clipIndex = m_playlistView->addAudioClip(
            trackInsertIndex + index, source.startBeat, source.lengthBeats,
            QStringLiteral("%1 — %2").arg(source.name, displayName), stemPaths[index],
            inputWasRendered ? 0.0 : source.sourceTempoBpm,
            inputWasRendered ? 0.0 : source.pitchShiftSemitones, source.beatsPerBar,
            probes[static_cast<size_t>(index)].durationSeconds(), {}, {},
            inputWasRendered ? 0.0 : source.sourceOffsetBeats,
            inputWasRendered ? source.lengthBeats : source.warpLengthBeats,
            inputWasRendered ? 1.0 : source.tempoConfidence,
            source.sourceSignatureNumerator, source.sourceSignatureDenominator,
            inputWasRendered ? false : source.tempoDetected,
            inputWasRendered ? std::vector<double>{} : source.transientSeconds,
            inputWasRendered ? std::vector<AudioWarpMarker>{} : source.warpMarkers,
            inputWasRendered ? std::vector<AudioGainEnvelopePoint>{} : source.gainEnvelope,
            false);
        if (clipIndex < 0) {
            rollbackStemImport();
            reportError(tr("Stem import was rolled back because a destination clip could not be created."));
            return false;
        }
        m_playlistView->updateSelectedAudioClipSyncMode(
            inputWasRendered ? 0 : source.syncMode,
            inputWasRendered ? 0.0 : source.targetBars);
        m_playlistView->updateSelectedAudioClipProcessing(
            stemClipGain, inputWasRendered ? 0.0f : source.pan, false,
            inputWasRendered ? false : source.reverse,
            inputWasRendered ? false : source.reversePolarity);
        m_playlistView->updateSelectedAudioClipFades(
            inputWasRendered ? 0.0 : source.fadeInBeats,
            inputWasRendered ? 0.0 : source.fadeOutBeats);
        m_playlistView->updateSelectedAudioClipFadeCurves(
            inputWasRendered ? 0.0 : source.fadeInCurve,
            inputWasRendered ? 0.0 : source.fadeOutCurve);
        m_playlistView->setSelectedClipsLoopEnabled(
            inputWasRendered ? false : source.loopEnabled,
            inputWasRendered ? 0.0 : source.loopStartBeats,
            inputWasRendered ? 0.0 : source.loopLengthBeats);
        m_playlistView->setSelectedClipsColor(colors[index % colors.size()]);
        ++importedClipCount;
    }
    if (importedClipCount != stemPaths.size() ||
        (!source.muted && !m_playlistView->setAudioClipMutedById(source.clipId, true))) {
        rollbackStemImport();
        reportError(tr("Stem import was rolled back because the source clip changed during import."));
        return false;
    }
    const int selectedAfter = m_tracks.empty()
        ? -1 : static_cast<int>(m_tracks.size()) - 1;
    const QString stemUndoMergeKey = m_stemBatchActive
        ? QStringLiteral("gesture:stem-batch") : QString();

    if (captureUndo) {
        if (!commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, stemUndoMergeKey,
            [this, addedTracks, selectedBefore]() {
                for (auto it = addedTracks.rbegin(); it != addedTracks.rend(); ++it) {
                    const auto found = std::find(m_tracks.begin(), m_tracks.end(), *it);
                    if (found != m_tracks.end()) m_tracks.erase(found);
                }
                rebuildTrackWidgets(selectedBefore);
                syncMixerRoutingToAudio();
                syncAudioTimelineToAudio();
            },
            [this, addedTracks, trackInsertIndex, selectedAfter]() {
                int insertIndex = std::clamp(
                    trackInsertIndex, 0, static_cast<int>(m_tracks.size()));
                for (const std::shared_ptr<Track>& track : addedTracks) {
                    if (std::find(m_tracks.begin(), m_tracks.end(), track) == m_tracks.end())
                        m_tracks.insert(m_tracks.begin() + insertIndex, track);
                    ++insertIndex;
                }
                rebuildTrackWidgets(selectedAfter);
                syncMixerRoutingToAudio();
                syncAudioTimelineToAudio();
             })) {
            rollbackStemImport();
            reportError(tr("Stem import was rolled back because its project undo step could not be recorded."), false);
            return false;
        }
    } else {
        m_playlistView->commitArrangementTransaction();
        setProjectDirty();
    }
    syncMixerRoutingToAudio();
    syncAudioTimelineToAudio();
    QString manifestError;
    const bool provenanceRecorded = manifestPath.isEmpty() ||
        StemSeparationService::markCompletedJobImported(
            manifestPath, m_projectPath, source.clipId, stemPaths.size(), &manifestError);
    ui.statusBar->showMessage(!provenanceRecorded
        ? tr("Imported %1 stems; source muted. Provenance update failed: %2")
            .arg(stemPaths.size()).arg(manifestError)
        : (normalizationGainLimited
            ? tr("Imported %1 stems; source muted. Normalization gain was limited to +18 dB.")
                .arg(stemPaths.size())
            : tr("Imported %1 stems; source clip muted.").arg(stemPaths.size())), 6000);
    return true;
}

void VeraCoreQT::addTrack(std::shared_ptr<Track> track) {
    m_tracks.push_back(track);

    TrackWidget* trackWidget = new TrackWidget(track.get(), this);
    m_trackWidgets.push_back(trackWidget);

    connect(trackWidget, &TrackWidget::trackSelected, this, &VeraCoreQT::onTrackSelected);
    connect(trackWidget, &TrackWidget::mixerControlsEdited,
            this, &VeraCoreQT::applyTrackMixerControls);
    connect(trackWidget, &TrackWidget::trackSendEdited,
            this, &VeraCoreQT::applyTrackSendLevel);

    ui.trackHeadersLayout->insertWidget(ui.trackHeadersLayout->count() - 1, trackWidget);

    // Update playlist view grid lanes
    m_playlistView->setTracks(m_tracks);
    syncMixerRoutingToAudio();
}

void VeraCoreQT::rebuildTrackWidgets(int preferredTrackIndex)
{
    for (TrackWidget* widget : m_trackWidgets) delete widget;
    m_trackWidgets.clear();
    m_trackWidgets.reserve(m_tracks.size());
    for (const std::shared_ptr<Track>& track : m_tracks) {
        TrackWidget* widget = new TrackWidget(track.get(), this);
        m_trackWidgets.push_back(widget);
        connect(widget, &TrackWidget::trackSelected, this, &VeraCoreQT::onTrackSelected);
        connect(widget, &TrackWidget::mixerControlsEdited,
                this, &VeraCoreQT::applyTrackMixerControls);
        connect(widget, &TrackWidget::trackSendEdited,
                this, &VeraCoreQT::applyTrackSendLevel);
        ui.trackHeadersLayout->insertWidget(ui.trackHeadersLayout->count() - 1, widget);
    }
    if (m_playlistView) m_playlistView->setTracks(m_tracks);
    m_selectedTrackIndex = preferredTrackIndex >= 0 &&
            preferredTrackIndex < static_cast<int>(m_trackWidgets.size())
        ? preferredTrackIndex : (m_trackWidgets.empty() ? -1 : 0);
    for (int index = 0; index < static_cast<int>(m_trackWidgets.size()); ++index)
        m_trackWidgets[index]->setSelected(index == m_selectedTrackIndex);
    if (!m_hasCurrentMidiClip && m_inspectorWidget)
        m_inspectorWidget->setTrack(m_selectedTrackIndex >= 0
            ? m_tracks[m_selectedTrackIndex].get() : nullptr);
    if (m_selectionService) {
        m_selectionService->setTrackSelection(
            m_selectedTrackIndex >= 0 ? QVector<int>{m_selectedTrackIndex} : QVector<int>{},
            m_selectedTrackIndex);
    }
}

VeraCoreQT::ProjectTimingState VeraCoreQT::captureProjectTimingState() const
{
    ProjectTimingState state;
    state.tempoMap = m_tempoMap;
    state.signatureMap = m_timeSignatureMap;
    state.snapBeats = m_projectSnapBeats;
    const double fallbackTempo = m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm;
    state.tempo = tempoAtBeat(state.tempoMap, 0.0, fallbackTempo);
    const MidiTimeSignaturePoint initialSignature = timeSignatureAtBeat(
        state.signatureMap, 0.0,
        m_transportWidget ? m_transportWidget->signatureNumerator() : 4,
        m_transportWidget ? m_transportWidget->signatureDenominator() : 4);
    state.numerator = initialSignature.numerator;
    state.denominator = initialSignature.denominator;
    return state;
}

bool VeraCoreQT::projectTimingMatches(const ProjectTimingState& state) const
{
    const ProjectTimingState current = captureProjectTimingState();
    auto close = [](double left, double right) {
        return std::isfinite(left) && std::isfinite(right) && std::fabs(left - right) <= 1.0e-6;
    };
    if (!close(current.tempo, state.tempo) || !close(current.snapBeats, state.snapBeats) ||
        current.numerator != state.numerator ||
        current.denominator != state.denominator ||
        current.tempoMap.size() != state.tempoMap.size() ||
        current.signatureMap.size() != state.signatureMap.size()) return false;
    for (size_t index = 0; index < current.tempoMap.size(); ++index) {
        if (!close(current.tempoMap[index].beat, state.tempoMap[index].beat) ||
            !close(current.tempoMap[index].bpm, state.tempoMap[index].bpm)) return false;
    }
    for (size_t index = 0; index < current.signatureMap.size(); ++index) {
        if (!close(current.signatureMap[index].beat, state.signatureMap[index].beat) ||
            current.signatureMap[index].numerator != state.signatureMap[index].numerator ||
            current.signatureMap[index].denominator != state.signatureMap[index].denominator) return false;
    }
    return true;
}

void VeraCoreQT::applyProjectTimingState(const ProjectTimingState& state)
{
    const double tempo = finiteClamp(state.tempo, 20.0, 300.0, kDefaultTempoBpm);
    const int numerator = std::clamp(state.numerator, 1, 32);
    const int denominator = validTimeSignatureDenominator(state.denominator)
        ? state.denominator : 4;
    m_tempoMap = normalizedTempoMap(state.tempoMap, tempo);
    m_timeSignatureMap = normalizedTimeSignatureMap(
        state.signatureMap, numerator, denominator);
    m_projectSnapBeats = finiteClamp(state.snapBeats, 0.0, 128.0, 0.25);
    if (m_transportWidget) {
        QSignalBlocker blocker(m_transportWidget);
        m_transportWidget->setTempo(tempo);
        m_transportWidget->setTimeSignature(numerator, denominator);
        m_transportWidget->setTimeSignatureMap(m_timeSignatureMap);
        m_transportWidget->setSnapBeats(m_projectSnapBeats);
        m_projectSnapBeats = m_transportWidget->snapBeats();
    }
    if (m_playlistView) {
        m_playlistView->setTimeSignatureMap(m_timeSignatureMap);
        m_playlistView->setSnapBeats(m_projectSnapBeats);
    }
}

void VeraCoreQT::applyArrangementProjectTransition(quint64 fromStateId, quint64 toStateId)
{
    const auto transitionIt = std::find_if(
        m_arrangementProjectTransitions.rbegin(), m_arrangementProjectTransitions.rend(),
        [fromStateId, toStateId](const ArrangementProjectTransition& transition) {
            return (transition.beforeStateId == fromStateId && transition.afterStateId == toStateId) ||
                   (transition.afterStateId == fromStateId && transition.beforeStateId == toStateId);
        });
    if (transitionIt != m_arrangementProjectTransitions.rend()) {
        const ArrangementProjectTransition& transition = *transitionIt;
        const bool applyingAfter = transition.beforeStateId == fromStateId &&
            transition.afterStateId == toStateId;
        bool topologyChanged = false;
        if (applyingAfter) {
            int insertIndex = std::clamp(
                transition.trackInsertIndex, 0, static_cast<int>(m_tracks.size()));
            for (const std::shared_ptr<Track>& track : transition.addedTracks) {
                if (std::find(m_tracks.begin(), m_tracks.end(), track) == m_tracks.end()) {
                    m_tracks.insert(m_tracks.begin() + insertIndex, track);
                    topologyChanged = true;
                }
                ++insertIndex;
            }
        } else {
            for (auto trackIt = transition.addedTracks.rbegin();
                 trackIt != transition.addedTracks.rend(); ++trackIt) {
                const auto found = std::find(m_tracks.begin(), m_tracks.end(), *trackIt);
                if (found != m_tracks.end()) {
                    m_tracks.erase(found);
                    topologyChanged = true;
                }
            }
        }
        if (topologyChanged) rebuildTrackWidgets(applyingAfter
            ? transition.selectedTrackAfter : transition.selectedTrackBefore);
        if (transition.timingChanged) {
            const ProjectTimingState& expected = applyingAfter
                ? transition.timingBefore : transition.timingAfter;
            const ProjectTimingState& target = applyingAfter
                ? transition.timingAfter : transition.timingBefore;
            if (projectTimingMatches(expected)) applyProjectTimingState(target);
        }
    }

    const auto projectIt = std::find_if(
        m_projectUndoTransitions.rbegin(), m_projectUndoTransitions.rend(),
        [fromStateId, toStateId](const ProjectUndoTransition& transition) {
            return (transition.beforeStateId == fromStateId && transition.afterStateId == toStateId) ||
                   (transition.afterStateId == fromStateId && transition.beforeStateId == toStateId);
        });
    if (projectIt != m_projectUndoTransitions.rend()) {
        const bool applyingAfter = projectIt->beforeStateId == fromStateId &&
            projectIt->afterStateId == toStateId;
        const std::function<void()>& apply = applyingAfter
            ? projectIt->applyAfter : projectIt->applyBefore;
        if (apply) apply();
    }
    refreshProjectDirtyFromArrangement(false);
}

void VeraCoreQT::finishArrangementProjectTransition(quint64 fromStateId, quint64 toStateId)
{
    const bool importMatched = std::any_of(
        m_arrangementProjectTransitions.begin(), m_arrangementProjectTransitions.end(),
        [fromStateId, toStateId](const ArrangementProjectTransition& transition) {
            return (transition.beforeStateId == fromStateId && transition.afterStateId == toStateId) ||
                   (transition.afterStateId == fromStateId && transition.beforeStateId == toStateId);
        });
    const bool projectMatched = std::any_of(
        m_projectUndoTransitions.begin(), m_projectUndoTransitions.end(),
        [fromStateId, toStateId](const ProjectUndoTransition& transition) {
            return (transition.beforeStateId == fromStateId && transition.afterStateId == toStateId) ||
                   (transition.afterStateId == fromStateId && transition.beforeStateId == toStateId);
        });
    if (!importMatched && !projectMatched) return;
    m_mixerParameters->republishProject(m_tracks);
    if (m_inspectorWidget) {
        m_inspectorWidget->refreshTrackDevices();
        if (m_transportWidget) m_inspectorWidget->setTimeSignature(
            m_transportWidget->signatureNumerator(),
            m_transportWidget->signatureDenominator());
    }
    refreshInspectorPluginParameters();
    syncTempoMapToAudio();
    syncTimeSignatureMapToAudio();
    syncProjectMetadataToMidiDocuments();
    syncPluginTransportToHost();
    refreshMidiEditorGhostClips();
    refreshMidiEditorExpressionMap();
    refreshProjectDirtyFromArrangement(false);
}

void VeraCoreQT::pruneArrangementProjectTransitions()
{
    if (!m_playlistView) {
        m_arrangementProjectTransitions.clear();
        m_projectUndoTransitions.clear();
        return;
    }
    m_arrangementProjectTransitions.erase(
        std::remove_if(m_arrangementProjectTransitions.begin(),
                       m_arrangementProjectTransitions.end(),
            [this](const ArrangementProjectTransition& transition) {
                return !m_playlistView->historyContainsArrangementState(transition.beforeStateId) ||
                       !m_playlistView->historyContainsArrangementState(transition.afterStateId);
            }),
        m_arrangementProjectTransitions.end());
    m_projectUndoTransitions.erase(
        std::remove_if(m_projectUndoTransitions.begin(), m_projectUndoTransitions.end(),
            [this](const ProjectUndoTransition& transition) {
                return !m_playlistView->historyContainsArrangementState(transition.beforeStateId) ||
                       !m_playlistView->historyContainsArrangementState(transition.afterStateId);
            }),
        m_projectUndoTransitions.end());
}

void VeraCoreQT::performGlobalUndo()
{
    if (!m_playlistView || !m_playlistView->undoArrangementEdit())
        ui.statusBar->showMessage("Nothing to undo.", 2000);
}

void VeraCoreQT::performGlobalRedo()
{
    if (!m_playlistView || !m_playlistView->redoArrangementEdit())
        ui.statusBar->showMessage("Nothing to redo.", 2000);
}

void VeraCoreQT::applyMidiDocumentState(const QString& contentId,
                                        const MidiClipDocument& document)
{
    const auto found = m_midiClipDocs.find(contentId);
    if (found == m_midiClipDocs.end() || !found->second) return;
    *found->second = document;
    if (m_midiEditor && m_currentMidiClipKey == contentId)
        m_midiEditor->setClip(found->second.get());
}

bool VeraCoreQT::commitProjectUndoStep(quint64 beforeStateId,
                                       bool nonArrangementDirtyBefore,
                                       const QString& mergeKey,
                                       std::function<void()> applyBefore,
                                       std::function<void()> applyAfter)
{
    if (!m_playlistView || !m_playlistView->commitArrangementTransaction()) return false;
    const quint64 afterStateId = m_playlistView->currentArrangementStateId();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    bool merged = false;
    if (!m_projectUndoTransitions.empty()) {
        ProjectUndoTransition& previous = m_projectUndoTransitions.back();
        const bool explicitGesture = mergeKey.startsWith(QStringLiteral("gesture:"));
        const bool mergeWindow = explicitGesture || (nowMs >= previous.committedAtMs &&
            nowMs - previous.committedAtMs <= 400);
        if (!mergeKey.isEmpty() && previous.mergeKey == mergeKey &&
            previous.afterStateId == beforeStateId && mergeWindow &&
            m_playlistView->canCoalesceLastUndoState(beforeStateId) &&
            m_playlistView->discardLastUndoState(beforeStateId)) {
            std::function<void()> previousBefore = std::move(previous.applyBefore);
            std::function<void()> previousAfter = std::move(previous.applyAfter);
            std::function<void()> nextBefore = std::move(applyBefore);
            std::function<void()> nextAfter = std::move(applyAfter);
            previous.applyBefore = [previousBefore = std::move(previousBefore),
                                    nextBefore = std::move(nextBefore)]() {
                if (nextBefore) nextBefore();
                if (previousBefore) previousBefore();
            };
            previous.applyAfter = [previousAfter = std::move(previousAfter),
                                   nextAfter = std::move(nextAfter)]() {
                if (previousAfter) previousAfter();
                if (nextAfter) nextAfter();
            };
            previous.afterStateId = afterStateId;
            previous.committedAtMs = nowMs;
            merged = true;
        }
    }
    if (!merged) {
        constexpr size_t kMaxProjectUndoTransitions = 64;
        if (m_projectUndoTransitions.size() >= kMaxProjectUndoTransitions)
            m_projectUndoTransitions.erase(m_projectUndoTransitions.begin());
        m_projectUndoTransitions.push_back({beforeStateId, afterStateId, mergeKey, nowMs,
            std::move(applyBefore), std::move(applyAfter)});
    }
    m_nonArrangementDirty = nonArrangementDirtyBefore;
    ++m_projectRevision;
    refreshProjectDirtyFromArrangement(false);
    return true;
}

void VeraCoreQT::resetReturnBuses()
{
    m_mixerParameters->resetProject();
}

WorkspacePreset VeraCoreQT::captureWorkspacePreset() const
{
    WorkspacePreset preset;
    preset.id = QStringLiteral("user-custom");
    preset.name = QStringLiteral("User Custom");
    preset.description = QStringLiteral("User-saved workspace layout.");
    preset.browserVisible = ui.browserPanel->isVisible();
    preset.arrangementVisible = ui.playlistSplitter->isVisible();
    preset.detailVisible = m_inspectorWidget && m_detailTabs->isVisible();
    preset.mainSplitterSizes = preset.browserVisible
        ? ui.mainSplitter->sizes() : m_lastMainSplitterSizes;
    preset.centerSplitterSizes = (preset.arrangementVisible || preset.detailVisible)
        ? ui.centerSplitter->sizes() : m_lastCenterSplitterSizes;
    preset.playlistSplitterSizes = preset.arrangementVisible
        ? ui.playlistSplitter->sizes() : m_lastPlaylistSplitterSizes;
    preset.builtIn = false;
    return preset;
}

void VeraCoreQT::applyWorkspacePreset(const WorkspacePreset& preset)
{
    if (!WorkspaceManager::isValid(preset)) return;
    if (ui.browserPanel->isVisible()) m_lastMainSplitterSizes = ui.mainSplitter->sizes();
    if (ui.playlistSplitter->isVisible())
        m_lastPlaylistSplitterSizes = ui.playlistSplitter->sizes();
    if ((ui.playlistSplitter->isVisible() || m_detailTabs->isVisible()))
        m_lastCenterSplitterSizes = ui.centerSplitter->sizes();

    m_lastMainSplitterSizes = preset.mainSplitterSizes;
    m_lastCenterSplitterSizes = preset.centerSplitterSizes;
    m_lastPlaylistSplitterSizes = preset.playlistSplitterSizes;
    ui.mainSplitter->setSizes(m_lastMainSplitterSizes);
    ui.centerSplitter->setSizes(m_lastCenterSplitterSizes);
    ui.playlistSplitter->setSizes(m_lastPlaylistSplitterSizes);
    {
        const QSignalBlocker browserBlock(ui.actionBrowser);
        const QSignalBlocker arrangementBlock(ui.actionPlaylist);
        const QSignalBlocker inspectorBlock(m_inspectorAction);
        const QSignalBlocker mixerBlock(ui.actionMixer);
        ui.actionBrowser->setChecked(preset.browserVisible);
        ui.actionPlaylist->setChecked(preset.arrangementVisible);
        m_inspectorAction->setChecked(preset.detailVisible);
        ui.actionMixer->setChecked(preset.detailVisible);
    }
    ui.browserPanel->setVisible(preset.browserVisible);
    ui.playlistSplitter->setVisible(preset.arrangementVisible);
    m_detailTabs->setVisible(preset.detailVisible);
    if (preset.browserVisible) ui.mainSplitter->setSizes(m_lastMainSplitterSizes);
    if (preset.arrangementVisible || preset.detailVisible)
        ui.centerSplitter->setSizes(m_lastCenterSplitterSizes);
    if (preset.arrangementVisible)
        ui.playlistSplitter->setSizes(m_lastPlaylistSplitterSizes);
    if (preset.arrangementVisible && m_playlistView)
        m_playlistView->setFocus(Qt::ShortcutFocusReason);
    ui.statusBar->showMessage(
        QStringLiteral("Workspace: %1").arg(preset.name), 2500);
}

void VeraCoreQT::applyProjectPanLawDb(double centerAttenuationDb)
{
    m_projectPanLawDb = canonicalPanLawDb(centerAttenuationDb);
    for (const auto& entry : m_panLawActions) {
        if (!entry.second) continue;
        const QSignalBlocker blocker(entry.second);
        entry.second->setChecked(std::fabs(entry.first - m_projectPanLawDb) <= 1.0e-9);
    }
    if (audioGeneral)
        audioGeneral->audioCore.SetPanLawDb(static_cast<float>(m_projectPanLawDb));
    if (m_pluginHost)
        m_pluginHost->setPanLawDb(m_projectPanLawDb);
}

void VeraCoreQT::applyProjectSpeakerLayout(VeraCoreSpeakerLayout layout)
{
    const VeraCoreSpeakerLayout previousLayout = m_projectSpeakerLayout;
    switch (layout) {
    case VeraCoreSpeakerLayout::Stereo:
    case VeraCoreSpeakerLayout::Surround51:
    case VeraCoreSpeakerLayout::Surround71:
    case VeraCoreSpeakerLayout::Atmos712:
    case VeraCoreSpeakerLayout::Atmos714:
        m_projectSpeakerLayout = layout;
        break;
    default:
        m_projectSpeakerLayout = VeraCoreSpeakerLayout::Stereo;
        break;
    }
    for (const auto& entry : m_speakerLayoutActions)
        if (entry.second) entry.second->setChecked(entry.first == m_projectSpeakerLayout);
    if (audioGeneral) {
        const bool reopenStream = previousLayout != m_projectSpeakerLayout &&
            audioGeneral->audioCore.HasOpenStream();
        audioGeneral->audioCore.SetSpeakerLayout(m_projectSpeakerLayout);
        if (reopenStream) {
            const PaError result = audioGeneral->RestartAudioFromSettings();
            if (ui.statusBar) {
                const AudioDeviceStatus status = audioGeneral->GetAudioDeviceStatus();
                const int requestedChannels = VeraCoreSpeakerChannelCount(m_projectSpeakerLayout);
                ui.statusBar->showMessage(result == paNoError
                    ? (status.outputChannels == requestedChannels
                        ? QString("Spatial output reopened: %1 channels").arg(status.outputChannels)
                        : QString("Device cannot open %1-channel mode; using compatible %2-channel fallback")
                            .arg(requestedChannels).arg(status.outputChannels))
                    : QString("Spatial output reopen failed: %1")
                        .arg(QString::fromLocal8Bit(Pa_GetErrorText(result))), 3500);
            }
        }
    }
	if (m_pluginHost && audioGeneral && previousLayout != m_projectSpeakerLayout) {
		captureLivePluginStatesToProject();
		syncRealtimeMidiLayersToAudio();
	}
}

void VeraCoreQT::applyTrackMixerControls(Track* track, float volume, float pan, float stereoSeparation,
                                         bool muted, bool soloed, bool armed, bool monitoring)
{
    if (!track) return;
    const auto modelIt = std::find_if(m_tracks.begin(), m_tracks.end(),
        [track](const std::shared_ptr<Track>& candidate) { return candidate.get() == track; });
    if (modelIt == m_tracks.end()) return;

    const float safeVolume = static_cast<float>(finiteClamp(volume, 0.0, 2.0, 1.0));
    const float safePan = static_cast<float>(finiteClamp(pan, -1.0, 1.0, 0.0));
    const float safeSeparation = static_cast<float>(
        finiteClamp(stereoSeparation, -1.0, 1.0, 0.0));
    const bool volumeChanged = std::fabs(track->GetVolume() - safeVolume) > 1.0e-6f;
    const bool panChanged = std::fabs(track->GetPan() - safePan) > 1.0e-6f;
    const bool separationChanged =
        std::fabs(track->GetStereoSeparation() - safeSeparation) > 1.0e-6f;
    const bool mutedChanged = track->IsMuted() != muted;
    const bool soloedChanged = track->IsSoloed() != soloed;
    const bool armedChanged = track->IsArmed() != armed;
    const bool monitoringChanged = track->IsMonitoring() != monitoring;
    const bool faderChanged = volumeChanged || panChanged || separationChanged;
    const bool audibleSetChanged = mutedChanged || soloedChanged;
    const bool inputStateChanged = armedChanged || monitoringChanged;
    if (!faderChanged && !audibleSetChanged && !inputStateChanged) return;

    const std::shared_ptr<Track> trackRef = *modelIt;
    const float previousVolume = track->GetVolume();
    const float previousPan = track->GetPan();
    const float previousSeparation = track->GetStereoSeparation();
    const bool previousMuted = track->IsMuted();
    const bool previousSoloed = track->IsSoloed();
    const bool previousArmed = track->IsArmed();
    const bool previousMonitoring = track->IsMonitoring();
    const VeraCoreSpatialPannerState previousSpatial = track->GetSpatialPanner();
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    VeraCoreSpatialPannerState nextSpatial = previousSpatial;
    if (nextSpatial.linkStereoPan) nextSpatial.azimuthDegrees = safePan * 30.0f;
    const MixerParameterService::TrackState nextState{
        safeVolume, safePan, safeSeparation, muted, soloed, armed, monitoring,
        nextSpatial};
    m_mixerParameters->setTrackState(*track, nextState);

    if (captureUndo) {
        const MixerParameterService::TrackState previousState{
            previousVolume, previousPan, previousSeparation, previousMuted, previousSoloed,
            previousArmed, previousMonitoring, previousSpatial};
        const qulonglong trackKey = static_cast<qulonglong>(
            reinterpret_cast<quintptr>(trackRef.get()));
        QString mergeKey;
        if (volumeChanged && !panChanged && !separationChanged && !audibleSetChanged && !inputStateChanged)
            mergeKey = QStringLiteral("track-volume:%1").arg(trackKey, 0, 16);
        else if (panChanged && !volumeChanged && !separationChanged && !audibleSetChanged && !inputStateChanged)
            mergeKey = QStringLiteral("track-pan:%1").arg(trackKey, 0, 16);
        else if (separationChanged && !volumeChanged && !panChanged &&
                 !audibleSetChanged && !inputStateChanged)
            mergeKey = QStringLiteral("track-width:%1").arg(trackKey, 0, 16);
        commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, mergeKey,
            [this, trackRef, previousState]() {
                m_mixerParameters->setTrackState(*trackRef, previousState);
            },
            [this, trackRef, nextState]() {
                m_mixerParameters->setTrackState(*trackRef, nextState);
            });
    } else {
        setProjectDirty();
    }
}

void VeraCoreQT::applyTrackSpatialPanner(Track* track, VeraCoreSpatialPannerState requested)
{
    if (!track) return;
    const auto found = std::find_if(m_tracks.begin(), m_tracks.end(),
        [track](const std::shared_ptr<Track>& candidate) { return candidate.get() == track; });
    if (found == m_tracks.end()) return;
    const std::shared_ptr<Track> trackRef = *found;
    const MixerParameterService::TrackState previous =
        MixerParameterService::stateFor(*track);
    MixerParameterService::TrackState next = previous;
    next.spatial = VeraCoreNormalizeSpatialPanner(requested);
    if (next.spatial.linkStereoPan)
        next.pan = std::clamp(next.spatial.azimuthDegrees / 30.0f, -1.0f, 1.0f);
    const bool changed = next.pan != previous.pan ||
        next.spatial.mode != previous.spatial.mode ||
        next.spatial.azimuthDegrees != previous.spatial.azimuthDegrees ||
        next.spatial.elevationDegrees != previous.spatial.elevationDegrees ||
        next.spatial.distance != previous.spatial.distance ||
        next.spatial.spread != previous.spatial.spread ||
        next.spatial.lfeSend != previous.spatial.lfeSend ||
        next.spatial.linkStereoPan != previous.spatial.linkStereoPan;
    if (!changed) return;
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool dirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    m_mixerParameters->setTrackState(*track, next);
    if (captureUndo) {
        const qulonglong trackKey = static_cast<qulonglong>(
            reinterpret_cast<quintptr>(trackRef.get()));
        commitProjectUndoStep(beforeStateId, dirtyBefore,
            QStringLiteral("track-spatial:%1").arg(trackKey, 0, 16),
            [this, trackRef, previous]() {
                m_mixerParameters->setTrackState(*trackRef, previous);
            },
            [this, trackRef, next]() {
                m_mixerParameters->setTrackState(*trackRef, next);
            });
    } else {
        setProjectDirty();
    }
}

void VeraCoreQT::applyTrackSendLevel(Track* track, int returnBus, float level)
{
    if (!track || returnBus < 0 || returnBus >= kReturnBusCount) return;
    const auto modelIt = std::find_if(m_tracks.begin(), m_tracks.end(),
        [track](const std::shared_ptr<Track>& candidate) { return candidate.get() == track; });
    if (modelIt == m_tracks.end()) return;
    const float safeLevel = static_cast<float>(finiteClamp(level, 0.0, 1.0, 0.0));
    if (std::fabs(track->GetSendLevel(returnBus) - safeLevel) <= 1.0e-6f) return;
    const std::shared_ptr<Track> trackRef = *modelIt;
    const float previousLevel = track->GetSendLevel(returnBus);
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    m_mixerParameters->setTrackSend(*track, returnBus, safeLevel);
    if (captureUndo) {
        const qulonglong trackKey = static_cast<qulonglong>(
            reinterpret_cast<quintptr>(trackRef.get()));
        const QString mergeKey = QStringLiteral("track-send:%1:%2")
            .arg(trackKey, 0, 16).arg(returnBus);
        commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, mergeKey,
            [this, trackRef, returnBus, previousLevel]() {
                m_mixerParameters->setTrackSend(*trackRef, returnBus, previousLevel);
            },
            [this, trackRef, returnBus, safeLevel]() {
                m_mixerParameters->setTrackSend(*trackRef, returnBus, safeLevel);
            });
    } else {
        setProjectDirty();
    }
}

void VeraCoreQT::applyReturnBusControls(int returnBus, float volume, float pan,
                                        float stereoSeparation,
                                        bool muted, bool soloed, bool preFader)
{
    if (returnBus < 0 || returnBus >= kReturnBusCount) return;
    const ReturnBusState& bus =
        m_mixerParameters->returnBuses()[static_cast<std::size_t>(returnBus)];
    const float safeVolume = static_cast<float>(finiteClamp(volume, 0.0, 2.0, 1.0));
    const float safePan = static_cast<float>(finiteClamp(pan, -1.0, 1.0, 0.0));
    const float safeSeparation = static_cast<float>(
        finiteClamp(stereoSeparation, -1.0, 1.0, 0.0));
    const bool volumeChanged = std::fabs(bus.volume - safeVolume) > 1.0e-6f;
    const bool panChanged = std::fabs(bus.pan - safePan) > 1.0e-6f;
    const bool separationChanged =
        std::fabs(bus.stereoSeparation - safeSeparation) > 1.0e-6f;
    const bool mutedChanged = bus.muted != muted;
    const bool soloedChanged = bus.soloed != soloed;
    const bool preFaderChanged = bus.preFader != preFader;
    if (!volumeChanged && !panChanged && !separationChanged && !mutedChanged &&
        !soloedChanged && !preFaderChanged) return;
    const ReturnBusState previous = bus;
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    ReturnBusState next = bus;
    next.volume = safeVolume;
    next.pan = safePan;
    next.stereoSeparation = safeSeparation;
    next.muted = muted;
    next.soloed = soloed;
    next.preFader = preFader;
    m_mixerParameters->setReturnBus(returnBus, next);
    if (captureUndo) {
        QString mergeKey;
        if (volumeChanged && !panChanged && !separationChanged && !mutedChanged && !soloedChanged && !preFaderChanged)
            mergeKey = QStringLiteral("return-volume:%1").arg(returnBus);
        else if (panChanged && !volumeChanged && !separationChanged && !mutedChanged && !soloedChanged && !preFaderChanged)
            mergeKey = QStringLiteral("return-pan:%1").arg(returnBus);
        else if (separationChanged && !volumeChanged && !panChanged && !mutedChanged &&
                 !soloedChanged && !preFaderChanged)
            mergeKey = QStringLiteral("return-width:%1").arg(returnBus);
        commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, mergeKey,
            [this, returnBus, previous]() {
                m_mixerParameters->setReturnBus(returnBus, previous);
            },
            [this, returnBus, next]() {
                m_mixerParameters->setReturnBus(returnBus, next);
            });
    } else {
        setProjectDirty();
    }
}

void VeraCoreQT::applyMasterBusControls(float volume, float pan,
                                        float stereoSeparation, bool muted)
{
    MasterBusState next = m_mixerParameters->masterBus();
    next.volume = static_cast<float>(finiteClamp(volume, 0.0, 2.0, 1.0));
    next.pan = static_cast<float>(finiteClamp(pan, -1.0, 1.0, 0.0));
    next.stereoSeparation = static_cast<float>(
        finiteClamp(stereoSeparation, -1.0, 1.0, 0.0));
    next.muted = muted;
    const MasterBusState& current = m_mixerParameters->masterBus();
    const bool volumeChanged = std::fabs(current.volume - next.volume) > 1.0e-6f;
    const bool panChanged = std::fabs(current.pan - next.pan) > 1.0e-6f;
    const bool separationChanged =
        std::fabs(current.stereoSeparation - next.stereoSeparation) > 1.0e-6f;
    const bool mutedChanged = current.muted != next.muted;
    if (!volumeChanged && !panChanged && !separationChanged && !mutedChanged) return;
    const MasterBusState previous = current;
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    m_mixerParameters->setMasterBus(next);
    if (captureUndo) {
        QString mergeKey;
        if (volumeChanged && !panChanged && !separationChanged && !mutedChanged)
            mergeKey = QStringLiteral("master-volume");
        else if (panChanged && !volumeChanged && !separationChanged && !mutedChanged)
            mergeKey = QStringLiteral("master-pan");
        else if (separationChanged && !volumeChanged && !panChanged && !mutedChanged)
            mergeKey = QStringLiteral("master-width");
        commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, mergeKey,
            [this, previous]() { m_mixerParameters->setMasterBus(previous); },
            [this, next]() { m_mixerParameters->setMasterBus(next); });
    } else {
        setProjectDirty();
    }
}

void VeraCoreQT::onTrackSelected(TrackWidget* selectedWidget, Track* track) {
    if (m_parameterEditorReturnBus >= 0) {
        finishAutomationWrite();
        m_parameterEditorTrackIndex = -1;
        m_parameterEditorReturnBus = -1;
        m_parameterEditorSlotIndex = -1;
        m_parameterEditorGestureDepth = 0;
        m_parameterEditorDeviceName.clear();
        m_hasAutomationTarget = false;
        if (m_inspectorWidget) {
            m_inspectorWidget->clearActiveAutomationTarget();
            m_inspectorWidget->clearPluginParameters();
        }
        if (m_playlistView) m_playlistView->clearPluginAutomationLane();
    }
    if (m_selectedTrackIndex >= 0 && m_parameterEditorTrackIndex == m_selectedTrackIndex &&
        m_selectedTrackIndex < static_cast<int>(m_trackWidgets.size()) &&
        m_trackWidgets[static_cast<size_t>(m_selectedTrackIndex)] != selectedWidget)
        finishAutomationWrite();
    // Deselect all others
    for (TrackWidget* tw : m_trackWidgets) {
        if (tw != selectedWidget) {
            tw->setSelected(false);
        }
    }

    selectedWidget->setSelected(true);
    m_selectedTrackIndex = -1;
    for (int i = 0; i < (int)m_trackWidgets.size(); ++i) {
        if (m_trackWidgets[i] == selectedWidget) {
            m_selectedTrackIndex = i;
            break;
        }
    }
    if (m_selectionService) {
        m_selectionService->setTrackSelection(
            m_selectedTrackIndex >= 0 ? QVector<int>{m_selectedTrackIndex} : QVector<int>{},
            m_selectedTrackIndex);
    }
    m_hasCurrentMidiClip = false;
    m_currentMidiClipKey.clear();
    m_currentMidiClipId.clear();
    if (m_pluginHost) m_pluginHost->setAraEditorSelection(-1, {});
    m_inspectorWidget->setTrack(track);
    m_inspectorWidget->setReturnBuses(m_mixerParameters->returnBuses());
    if (m_parameterEditorTrackIndex >= 0 &&
        m_parameterEditorTrackIndex != m_selectedTrackIndex) {
        m_hasAutomationTarget = false;
        m_automationParameterId = 0;
        m_automationParameterName.clear();
        m_inspectorWidget->clearActiveAutomationTarget();
        if (m_playlistView) m_playlistView->clearPluginAutomationLane();
    }
    if (track && track->GetType() == TrackType::Midi) {
        m_currentMidiTrackIndex = m_selectedTrackIndex;
    } else {
        m_currentMidiTrackIndex = -1;
    }
    syncRealtimeMidiLayersToAudio();
}

void VeraCoreQT::onPlaylistTrackSelected(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= (int)m_trackWidgets.size()) {
        return;
    }

    onTrackSelected(m_trackWidgets[trackIndex], m_tracks[trackIndex].get());
}

void VeraCoreQT::onAddLayerRequested()
{
    if (m_selectedTrackIndex < 0 || m_selectedTrackIndex >= static_cast<int>(m_tracks.size())) {
        return;
    }

    const std::shared_ptr<Track> trackRef = m_tracks[m_selectedTrackIndex];
    MidiTrack* midiTrack = dynamic_cast<MidiTrack*>(trackRef.get());
    if (!midiTrack) {
        return;
    }

    LayerSlot layer;
    const int newLayerIndex = static_cast<int>(midiTrack->GetLayerSlots().size());
    layer.name = QString("Internal Synth %1").arg(newLayerIndex + 1);
    layer.pluginType = "Internal";
    layer.gain = 0.85f;
    if (newLayerIndex % 3 == 1) {
        layer.transpose = 12;
    } else if (newLayerIndex % 3 == 2) {
        layer.transpose = -12;
    }
    const std::vector<LayerSlot> beforeLayers = midiTrack->GetLayerSlots();
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    midiTrack->AddLayerSlot(layer);
    const std::vector<LayerSlot> afterLayers = midiTrack->GetLayerSlots();
    if (captureUndo) {
        auto redoLayers = std::make_shared<std::vector<LayerSlot>>(afterLayers);
        commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
            [this, trackRef, beforeLayers, redoLayers, newLayerIndex]() {
                if (auto* target = dynamic_cast<MidiTrack*>(trackRef.get())) {
                    const auto found = std::find(m_tracks.begin(), m_tracks.end(), trackRef);
                    if (m_pluginHost && found != m_tracks.end()) {
                        const int trackIndex = static_cast<int>(std::distance(m_tracks.begin(), found));
                        const QByteArray state = m_pluginHost->captureLayerState(
                            trackIndex * kLayerIdStride + newLayerIndex);
                        if (!state.isEmpty()) target->SetLayerSlotPluginState(newLayerIndex, state);
                    }
                    *redoLayers = target->GetLayerSlots();
                    target->SetLayerSlots(beforeLayers);
                }
            },
            [trackRef, redoLayers]() {
                if (auto* target = dynamic_cast<MidiTrack*>(trackRef.get()))
                    target->SetLayerSlots(*redoLayers);
            });
    } else {
        setProjectDirty();
    }
    m_inspectorWidget->setTrack(midiTrack);
    syncRealtimeMidiLayersToAudio();
    syncMidiTimelineToAudio();
}

void VeraCoreQT::onLayerEnabledChanged(int layerIndex, bool enabled)
{
    if (m_selectedTrackIndex < 0 || m_selectedTrackIndex >= static_cast<int>(m_tracks.size())) {
        return;
    }

    const std::shared_ptr<Track> trackRef = m_tracks[m_selectedTrackIndex];
    MidiTrack* midiTrack = dynamic_cast<MidiTrack*>(trackRef.get());
    if (!midiTrack) {
        return;
    }

    const std::vector<LayerSlot>& layers = midiTrack->GetLayerSlots();
    if (layerIndex < 0 || layerIndex >= static_cast<int>(layers.size())) {
        return;
    }
    if (layers[layerIndex].enabled == enabled) return;

    const std::vector<LayerSlot> beforeLayers = layers;
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    midiTrack->SetLayerSlotEnabled(layerIndex, enabled);
    const std::vector<LayerSlot> afterLayers = midiTrack->GetLayerSlots();
    if (captureUndo) {
        commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
            [trackRef, beforeLayers]() {
                if (auto* target = dynamic_cast<MidiTrack*>(trackRef.get()))
                    target->SetLayerSlots(beforeLayers);
            },
            [trackRef, afterLayers]() {
                if (auto* target = dynamic_cast<MidiTrack*>(trackRef.get()))
                    target->SetLayerSlots(afterLayers);
            });
    } else {
        setProjectDirty();
    }
    m_inspectorWidget->setTrack(midiTrack);
    syncRealtimeMidiLayersToAudio();
    syncMidiTimelineToAudio();
    ui.statusBar->showMessage(QString("Layer %1 %2.")
                                  .arg(layerIndex + 1)
                                  .arg(enabled ? "enabled" : "muted"),
                              2500);
}

void VeraCoreQT::onLayerParametersChanged(int layerIndex, float gain, float pan, int transpose, int midiChannel)
{
    if (m_selectedTrackIndex < 0 || m_selectedTrackIndex >= static_cast<int>(m_tracks.size())) {
        return;
    }

    const std::shared_ptr<Track> trackRef = m_tracks[m_selectedTrackIndex];
    MidiTrack* midiTrack = dynamic_cast<MidiTrack*>(trackRef.get());
    if (!midiTrack) {
        return;
    }

    const std::vector<LayerSlot>& layers = midiTrack->GetLayerSlots();
    if (layerIndex < 0 || layerIndex >= static_cast<int>(layers.size())) {
        return;
    }

    const LayerSlot& previousLayer = layers[layerIndex];
    const float safeGain = static_cast<float>(finiteClamp(gain, 0.0, 2.0, 1.0));
    const float safePan = static_cast<float>(finiteClamp(pan, -1.0, 1.0, 0.0));
    const int safeTranspose = std::clamp(transpose, -48, 48);
    const int safeMidiChannel = std::clamp(midiChannel, -1, 15);
    const bool gainChanged = std::fabs(previousLayer.gain - safeGain) > 1.0e-6f;
    const bool panChanged = std::fabs(previousLayer.pan - safePan) > 1.0e-6f;
    const bool transposeChanged = previousLayer.transpose != safeTranspose;
    const bool channelChanged = previousLayer.midiChannel != safeMidiChannel;
    if (!gainChanged && !panChanged && !transposeChanged && !channelChanged) return;
    const std::vector<LayerSlot> beforeLayers = layers;
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    midiTrack->SetLayerSlotParameters(
        layerIndex, safeGain, safePan, safeTranspose, safeMidiChannel);
    const std::vector<LayerSlot> afterLayers = midiTrack->GetLayerSlots();
    if (captureUndo) {
        const qulonglong trackKey = static_cast<qulonglong>(
            reinterpret_cast<quintptr>(trackRef.get()));
        QString mergeKey;
        if (gainChanged && !panChanged && !transposeChanged && !channelChanged)
            mergeKey = QStringLiteral("layer-gain:%1:%2").arg(trackKey, 0, 16).arg(layerIndex);
        else if (panChanged && !gainChanged && !transposeChanged && !channelChanged)
            mergeKey = QStringLiteral("layer-pan:%1:%2").arg(trackKey, 0, 16).arg(layerIndex);
        commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, mergeKey,
            [trackRef, beforeLayers]() {
                if (auto* target = dynamic_cast<MidiTrack*>(trackRef.get()))
                    target->SetLayerSlots(beforeLayers);
            },
            [trackRef, afterLayers]() {
                if (auto* target = dynamic_cast<MidiTrack*>(trackRef.get()))
                    target->SetLayerSlots(afterLayers);
            });
    } else {
        setProjectDirty();
    }
    const LayerSlot& updatedLayer = midiTrack->GetLayerSlots()[layerIndex];
    syncRealtimeMidiLayersToAudio();
    syncMidiTimelineToAudio();
    ui.statusBar->showMessage(QString("Layer %1: %2%, pan %3, T%4, %5.")
                                  .arg(layerIndex + 1)
                                  .arg(static_cast<int>(updatedLayer.gain * 100.0f))
                                  .arg(static_cast<int>(updatedLayer.pan * 100.0f))
                                  .arg(updatedLayer.transpose)
                                  .arg(updatedLayer.midiChannel < 0 ? QString("All channels")
                                                                    : QString("Ch%1").arg(updatedLayer.midiChannel + 1)),
                              2000);
}

void VeraCoreQT::onLayerOutputRoutingRequested(int layerIndex)
{
    const int sourceTrackIndex = m_selectedTrackIndex;
    if (!m_pluginHost || sourceTrackIndex < 0 ||
        sourceTrackIndex >= static_cast<int>(m_tracks.size())) return;
    const std::shared_ptr<Track> sourceTrackRef = m_tracks[sourceTrackIndex];
    MidiTrack* midiTrack = dynamic_cast<MidiTrack*>(sourceTrackRef.get());
    if (!midiTrack) return;
    const auto& layers = midiTrack->GetLayerSlots();
    if (layerIndex < 0 || layerIndex >= static_cast<int>(layers.size())) return;
    const LayerSlot layerSnapshot = layers[static_cast<size_t>(layerIndex)];
    const int layerId = sourceTrackIndex * kLayerIdStride + layerIndex;
    const std::vector<PluginAudioOutputBus> buses = m_pluginHost->outputBusesForLayer(layerId);
    if (buses.empty()) {
        QMessageBox::information(this, "Plugin Outputs",
            "No audio output buses are currently exposed by this plugin.");
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QString("Output Mapping — %1").arg(layerSnapshot.name));
    dialog.setMinimumWidth(520);
    QVBoxLayout* root = new QVBoxLayout(&dialog);
    QLabel* help = new QLabel(
        "Map plugin outputs to mixer channels. This is output routing; sidechain mapping controls plugin inputs.",
        &dialog);
    help->setWordWrap(true);
    root->addWidget(help);
    QFormLayout* form = new QFormLayout();
    std::vector<QComboBox*> destinations;
    destinations.reserve(buses.size());
    for (const PluginAudioOutputBus& bus : buses) {
        QComboBox* destination = new QComboBox(&dialog);
        destination->addItem("Instrument channel", -1);
        destination->addItem("Disabled", -2);
        if (!bus.main) destination->addItem("New mixer channel", -3);
        for (int trackIndex = 0;
             trackIndex < static_cast<int>(m_tracks.size()) && trackIndex < 256; ++trackIndex) {
            if (trackIndex == sourceTrackIndex || !m_tracks[trackIndex]) continue;
            destination->addItem(
                QString("Mixer %1 — %2").arg(trackIndex + 1).arg(m_tracks[trackIndex]->GetName()),
                trackIndex);
        }
        const auto existing = std::find_if(
            layerSnapshot.outputRoutes.begin(), layerSnapshot.outputRoutes.end(),
            [&](const PluginOutputRoute& route) {
                return (!bus.stablePortId.isEmpty() && route.stablePortId == bus.stablePortId) ||
                    route.outputBusIndex == bus.busIndex;
            });
        int value = bus.main ? -1 : -2;
        if (existing != layerSnapshot.outputRoutes.end())
            value = existing->enabled ? existing->destinationTrackIndex : -2;
        int comboIndex = destination->findData(value);
        destination->setCurrentIndex(comboIndex >= 0 ? comboIndex : destination->findData(-2));
        destination->setToolTip(QString("%1 channel%2").arg(bus.channelCount)
            .arg(bus.main ? "; main output" : "; auxiliary output"));
        form->addRow(QString("%1 (%2 ch)").arg(bus.name).arg(bus.channelCount), destination);
        destinations.push_back(destination);
    }
    root->addLayout(form);
    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QPushButton* autoMap = buttons->addButton("Auto Map Outputs", QDialogButtonBox::ActionRole);
    connect(autoMap, &QPushButton::clicked, &dialog, [&]() {
        for (size_t index = 0; index < buses.size(); ++index) {
            const int desired = buses[index].main ? -1 : -3;
            const int comboIndex = destinations[index]->findData(desired);
            if (comboIndex >= 0) destinations[index]->setCurrentIndex(comboIndex);
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    root->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;

    const int newChannelCount = static_cast<int>(std::count_if(
        destinations.begin(), destinations.end(), [](const QComboBox* destination) {
            return destination && destination->currentData().toInt() == -3;
        }));
    if (static_cast<int>(m_tracks.size()) + newChannelCount > 256) {
        QMessageBox::warning(this, "Plugin Outputs",
            "Auto Map would exceed VeraCore's 256 mixer-channel limit.");
        return;
    }

    std::vector<PluginOutputRoute> routes;
    routes.reserve(buses.size());
    std::vector<std::shared_ptr<Track>> addedTracks;
    addedTracks.reserve(static_cast<size_t>(newChannelCount));
    const int trackInsertIndex = static_cast<int>(m_tracks.size());
    for (size_t index = 0; index < buses.size(); ++index) {
        int destination = destinations[index]->currentData().toInt();
        if (destination == -3) {
            auto mixerTrack = std::make_shared<AudioTrack>(
                QString("%1 — %2").arg(layerSnapshot.name, buses[index].name));
            destination = trackInsertIndex + static_cast<int>(addedTracks.size());
            addedTracks.push_back(std::move(mixerTrack));
        }
        routes.push_back({buses[index].stablePortId, buses[index].name,
                          buses[index].busIndex, destination == -2 ? -1 : destination,
                          destination != -2});
    }
    auto routesEqual = [](const std::vector<PluginOutputRoute>& left,
                          const std::vector<PluginOutputRoute>& right) {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index) {
            const PluginOutputRoute& a = left[index];
            const PluginOutputRoute& b = right[index];
            if (a.stablePortId != b.stablePortId || a.displayName != b.displayName ||
                a.outputBusIndex != b.outputBusIndex ||
                a.destinationTrackIndex != b.destinationTrackIndex || a.enabled != b.enabled)
                return false;
        }
        return true;
    };
    if (addedTracks.empty() && routesEqual(layerSnapshot.outputRoutes, routes)) return;
    const std::vector<LayerSlot> beforeLayers = midiTrack->GetLayerSlots();
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    for (const std::shared_ptr<Track>& track : addedTracks) addTrack(track);
    midiTrack->SetLayerSlotOutputRoutes(layerIndex, routes);
    const std::vector<LayerSlot> afterLayers = midiTrack->GetLayerSlots();
    if (captureUndo) {
        commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
            [this, sourceTrackRef, beforeLayers, addedTracks, sourceTrackIndex]() {
                for (auto it = addedTracks.rbegin(); it != addedTracks.rend(); ++it) {
                    const auto found = std::find(m_tracks.begin(), m_tracks.end(), *it);
                    if (found != m_tracks.end()) m_tracks.erase(found);
                }
                if (auto* target = dynamic_cast<MidiTrack*>(sourceTrackRef.get()))
                    target->SetLayerSlots(beforeLayers);
                rebuildTrackWidgets(sourceTrackIndex);
            },
            [this, sourceTrackRef, afterLayers, addedTracks, trackInsertIndex,
             sourceTrackIndex]() {
                int insertIndex = std::clamp(
                    trackInsertIndex, 0, static_cast<int>(m_tracks.size()));
                for (const std::shared_ptr<Track>& track : addedTracks) {
                    if (std::find(m_tracks.begin(), m_tracks.end(), track) == m_tracks.end())
                        m_tracks.insert(m_tracks.begin() + insertIndex, track);
                    ++insertIndex;
                }
                if (auto* target = dynamic_cast<MidiTrack*>(sourceTrackRef.get()))
                    target->SetLayerSlots(afterLayers);
                rebuildTrackWidgets(sourceTrackIndex);
            });
    } else {
        setProjectDirty();
    }
    syncRealtimeMidiLayersToAudio();
    syncMixerRoutingToAudio();
    m_inspectorWidget->setTrack(midiTrack);
    ui.statusBar->showMessage(
        QString("Mapped %1 plugin outputs.").arg(static_cast<int>(routes.size())), 3000);
}

void VeraCoreQT::onRemoveLayerRequested(int layerIndex)
{
    if (m_selectedTrackIndex < 0 || m_selectedTrackIndex >= static_cast<int>(m_tracks.size())) {
        return;
    }

    const int trackIndex = m_selectedTrackIndex;
    const std::shared_ptr<Track> trackRef = m_tracks[trackIndex];
    MidiTrack* midiTrack = dynamic_cast<MidiTrack*>(trackRef.get());
    if (!midiTrack) {
        return;
    }

    const std::vector<LayerSlot>& layers = midiTrack->GetLayerSlots();
    if (layerIndex < 0 || layerIndex >= static_cast<int>(layers.size())) {
        return;
    }

    if (layers.size() <= 1) {
        ui.statusBar->showMessage("Keep at least one layer on a MIDI track.", 2500);
        return;
    }

    finishAutomationWrite();

    if (m_pluginHost) {
        for (int slotIndex = layerIndex;
             slotIndex < static_cast<int>(midiTrack->GetLayerSlots().size()); ++slotIndex) {
            const QByteArray state = m_pluginHost->captureLayerState(
                trackIndex * kLayerIdStride + slotIndex);
            if (!state.isEmpty()) midiTrack->SetLayerSlotPluginState(slotIndex, state);
        }
    }
    const std::vector<LayerSlot> beforeLayers = midiTrack->GetLayerSlots();
    const QString removedName = beforeLayers[layerIndex].name;
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    midiTrack->RemoveLayerSlot(layerIndex);
    const std::vector<LayerSlot> afterLayers = midiTrack->GetLayerSlots();
    if (captureUndo) {
        commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
            [trackRef, beforeLayers]() {
                if (auto* target = dynamic_cast<MidiTrack*>(trackRef.get()))
                    target->SetLayerSlots(beforeLayers);
            },
            [trackRef, afterLayers]() {
                if (auto* target = dynamic_cast<MidiTrack*>(trackRef.get()))
                    target->SetLayerSlots(afterLayers);
            });
    } else {
        setProjectDirty();
    }
    m_inspectorWidget->setTrack(midiTrack);
    syncRealtimeMidiLayersToAudio();
    syncMidiTimelineToAudio();
    ui.statusBar->showMessage(QString("Removed layer: %1").arg(removedName), 3000);
}

void VeraCoreQT::onBrowserItemActivated(QTreeWidgetItem* item, int column)
{
    (void)column;
    if (!item || item->childCount() > 0) {
        return;
    }
    const int returnFxBus = m_inspectorWidget
        ? m_inspectorWidget->currentReturnFxBus() : -1;
    const bool selectedTrackValid = m_selectedTrackIndex >= 0 &&
        m_selectedTrackIndex < static_cast<int>(m_tracks.size());
    if (!selectedTrackValid &&
        (returnFxBus < 0 || returnFxBus >= kReturnBusCount)) {
        return;
    }

    const QString samplePath = item->data(0, kSamplePathRole).toString();
    if (!samplePath.isEmpty()) {
        if (!selectedTrackValid) return;
        const QFileInfo sampleInfo(samplePath);
        if (!sampleInfo.exists() || !sampleInfo.isFile()) {
            QMessageBox::critical(this, "Add Audio Clip", "Sample file could not be found.");
            return;
        }
        const QString suffix = sampleInfo.suffix().toLower();
        if (suffix == "mid" || suffix == "midi") {
            if (!m_tracks[m_selectedTrackIndex] || m_tracks[m_selectedTrackIndex]->GetType() != TrackType::Midi) {
                QMessageBox::information(this, "Add MIDI Clip", "Select a MIDI track before adding a MIDI file.");
                return;
            }

            QString errorText;
            const int importedCount = importMidiFileToTracks(
                samplePath, m_selectedTrackIndex, m_playheadBeat, false, &errorText);
            if (importedCount < 0) {
                QMessageBox::critical(this, "Add MIDI Clip", errorText.isEmpty() ? "Could not read MIDI file." : errorText);
                return;
            }
            ui.statusBar->showMessage(QString("Added %1 MIDI track%2 at the playhead.")
                .arg(importedCount).arg(importedCount == 1 ? "" : "s"), 4000);
            return;
        }
        if (!m_tracks[m_selectedTrackIndex] || m_tracks[m_selectedTrackIndex]->GetType() != TrackType::Audio) {
            QMessageBox::information(this, "Add Audio Clip", "Select an audio track before adding a sample.");
            return;
        }
        if (!AudioFileDecoder::canDecodePath(samplePath)) {
            QMessageBox::information(this, "Add Audio Clip", AudioFileDecoder::unsupportedDecodeMessage(samplePath));
            return;
        }
        const AudioFileProbeResult probe = AudioFileDecoder::probeAudio(samplePath);
        if (!probe.success || probe.durationSeconds() <= 0.0) {
            QMessageBox::critical(this, "Add Audio Clip", probe.message.isEmpty() ? "Could not read sample duration." : probe.message);
            return;
        }
        const double insertBeat = m_playheadBeat;
        const double tempo = m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm;
        const double safeTempo = std::isfinite(tempo) && tempo > 0.0 ? qBound(1.0, tempo, 999.0) : kDefaultTempoBpm;
        const int signatureNumerator = m_transportWidget ? m_transportWidget->signatureNumerator() : 4;
        const int signatureDenominator = m_transportWidget ? m_transportWidget->signatureDenominator() : 4;
        const DecodedAudioFile* decoded = cachedDecodedAudio(samplePath);
        AudioTempoAnalysis analysis = decoded
            ? analyzeAudioTempo(*decoded, safeTempo, signatureNumerator, signatureDenominator)
            : AudioTempoAnalysis{};
        if (analysis.tempoBpm <= 0.0) {
            std::vector<double> transientSeconds = std::move(analysis.transientSeconds);
            analysis = estimateAudioTempoFromDuration(
                probe.durationSeconds(), safeTempo, signatureNumerator, signatureDenominator);
            analysis.transientSeconds = std::move(transientSeconds);
        }
        const double sourceTempo = analysis.tempoBpm;
        const double beatsPerBar = projectBeatsPerBar(signatureNumerator, signatureDenominator);
        const double clipTempo = sourceTempo > 0.0 ? sourceTempo : safeTempo;
        const double lengthBeats = qMax(0.25, probe.durationSeconds() * clipTempo / 60.0);
        const QString clipName = sampleInfo.completeBaseName().isEmpty() ? item->text(0) : sampleInfo.completeBaseName();
        const int clipIndex = m_playlistView->addAudioClip(m_selectedTrackIndex, safeClipStartBeat(insertBeat), lengthBeats, clipName,
            samplePath, sourceTempo, 0.0, beatsPerBar, probe.durationSeconds(), {}, {}, 0.0, lengthBeats,
            analysis.confidence, analysis.timeSignatureNumerator, analysis.timeSignatureDenominator,
            analysis.source != AudioTempoSource::Unknown, analysis.transientSeconds);
        if (clipIndex < 0) {
            QMessageBox::critical(this, "Add Audio Clip", "Could not add sample to the selected audio track.");
            return;
        }
        syncAudioTimelineToAudio();
        ui.statusBar->showMessage(QString("Added audio clip: %1 (%2 beats). Native audio clip playback/render enabled.")
                                      .arg(clipName)
                                      .arg(lengthBeats, 0, 'f', 2),
                                  5000);
        return;
    }

    const QString category = item->data(0, kPluginCategoryRole).toString();
    if (category != "Instrument") {
        if (returnFxBus >= 0 && returnFxBus < kReturnBusCount) {
            AudioFxSlot slot;
            slot.name = item->data(0, kPluginNameRole).toString();
            if (slot.name.isEmpty()) slot.name = item->text(0);
            slot.pluginPath = item->data(0, kPluginPathRole).toString();
            slot.pluginType = item->data(0, kPluginTypeRole).toString();
            slot.pluginUid = item->data(0, kPluginUidRole).toString();
            if (slot.pluginPath.isEmpty()) return;
            const ReturnBusState before =
                m_mixerParameters->returnBuses()[static_cast<std::size_t>(returnFxBus)];
            ReturnBusState after = before;
            const int newFxIndex = static_cast<int>(after.audioFxSlots.size());
            after.audioFxSlots.push_back(slot);
            const bool captureUndo = m_playlistView && !m_restoringProject &&
                !m_suppressProjectUndoCapture;
            const quint64 beforeStateId = captureUndo
                ? m_playlistView->currentArrangementStateId() : 0;
            const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
            if (captureUndo) {
                m_playlistView->beginArrangementTransaction();
                m_playlistView->markArrangementTransactionChanged();
            }
            m_mixerParameters->setReturnBus(returnFxBus, after);
            if (captureUndo) {
                commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
                    [this, returnFxBus, before]() {
                        m_mixerParameters->setReturnBus(returnFxBus, before);
                        syncRealtimeMidiLayersToAudio();
                    },
                    [this, returnFxBus, after]() {
                        m_mixerParameters->setReturnBus(returnFxBus, after);
                        syncRealtimeMidiLayersToAudio();
                    });
            } else {
                setProjectDirty();
            }
            syncRealtimeMidiLayersToAudio();
            m_inspectorWidget->showReturnFxBus(returnFxBus);
            const bool hostEvaluated = audioGeneral && m_pluginHost;
            const bool loaded = hostEvaluated && m_pluginHost->isAudioFxLoaded(
                VeraCoreReturnAudioFxTrackId(returnFxBus), newFxIndex);
            ui.statusBar->showMessage(QString("Added Return %1 FX: %2. %3")
                .arg(QChar('A' + returnFxBus)).arg(slot.name,
                    !hostEvaluated ? QString("DSP status pending audio engine.")
                                   : loaded ? QString("x64 DSP ready.")
                                            : QString("Plugin load failed; effect bypassed.")), 5000);
            return;
        }
        if (!selectedTrackValid) return;
        const std::shared_ptr<Track> trackRef = m_tracks[m_selectedTrackIndex];
        AudioFxSlot slot;
        slot.name = item->data(0, kPluginNameRole).toString();
        if (slot.name.isEmpty()) slot.name = item->text(0);
        slot.pluginPath = item->data(0, kPluginPathRole).toString();
        slot.pluginType = item->data(0, kPluginTypeRole).toString();
        slot.pluginUid = item->data(0, kPluginUidRole).toString();
        if (slot.pluginPath.isEmpty()) return;
        const std::vector<AudioFxSlot> beforeSlots = trackRef->GetAudioFxSlots();
        const int newFxIndex = static_cast<int>(beforeSlots.size());
        const bool captureUndo = m_playlistView && !m_restoringProject &&
            !m_suppressProjectUndoCapture;
        const quint64 beforeStateId = captureUndo
            ? m_playlistView->currentArrangementStateId() : 0;
        const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
        if (captureUndo) {
            m_playlistView->beginArrangementTransaction();
            m_playlistView->markArrangementTransactionChanged();
        }
        trackRef->AddAudioFx(slot);
        const std::vector<AudioFxSlot> afterSlots = trackRef->GetAudioFxSlots();
        if (captureUndo) {
            auto redoSlots = std::make_shared<std::vector<AudioFxSlot>>(afterSlots);
            commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
                [this, trackRef, beforeSlots, redoSlots, newFxIndex]() {
                    const auto found = std::find(m_tracks.begin(), m_tracks.end(), trackRef);
                    if (m_pluginHost && found != m_tracks.end()) {
                        const int trackIndex = static_cast<int>(std::distance(m_tracks.begin(), found));
                        const QByteArray state = m_pluginHost->captureAudioFxState(
                            trackIndex, newFxIndex);
                        if (!state.isEmpty()) trackRef->SetAudioFxPluginState(newFxIndex, state);
                    }
                    *redoSlots = trackRef->GetAudioFxSlots();
                    trackRef->SetAudioFxSlots(beforeSlots);
                },
                [trackRef, redoSlots]() { trackRef->SetAudioFxSlots(*redoSlots); });
        } else {
            setProjectDirty();
        }
        m_inspectorWidget->setTrack(trackRef.get());
        syncRealtimeMidiLayersToAudio();
        const bool hostEvaluated = audioGeneral && m_pluginHost;
        const bool loaded = hostEvaluated &&
            m_pluginHost->isAudioFxLoaded(m_selectedTrackIndex, newFxIndex);
        ui.statusBar->showMessage(QString("Added audio FX: %1. %2")
            .arg(slot.name, !hostEvaluated ? QString("DSP status pending audio engine.")
                                           : loaded ? QString("x64 DSP ready.")
                                                    : QString("Plugin load failed; effect bypassed.")), 5000);
        return;
    }

    const std::shared_ptr<Track> trackRef = m_tracks[m_selectedTrackIndex];
    MidiTrack* midiTrack = dynamic_cast<MidiTrack*>(trackRef.get());
    if (!midiTrack) {
        QMessageBox::information(this, "Add Layer", "Select a MIDI track before adding an instrument layer.");
        return;
    }

    LayerSlot layer;
    layer.name = item->data(0, kPluginNameRole).toString();
    if (layer.name.isEmpty()) {
        layer.name = item->text(0);
    }
    layer.pluginPath = item->data(0, kPluginPathRole).toString();
    layer.pluginType = item->data(0, kPluginTypeRole).toString();
    layer.pluginUid = item->data(0, kPluginUidRole).toString();
    layer.gain = 0.85f;
    const int newLayerIndex = static_cast<int>(midiTrack->GetLayerSlots().size());
    const std::vector<LayerSlot> beforeLayers = midiTrack->GetLayerSlots();
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    midiTrack->AddLayerSlot(layer);
    const std::vector<LayerSlot> afterLayers = midiTrack->GetLayerSlots();
    if (captureUndo) {
        auto redoLayers = std::make_shared<std::vector<LayerSlot>>(afterLayers);
        commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
            [this, trackRef, beforeLayers, redoLayers, newLayerIndex]() {
                if (auto* target = dynamic_cast<MidiTrack*>(trackRef.get())) {
                    const auto found = std::find(m_tracks.begin(), m_tracks.end(), trackRef);
                    if (m_pluginHost && found != m_tracks.end()) {
                        const int trackIndex = static_cast<int>(std::distance(m_tracks.begin(), found));
                        const QByteArray state = m_pluginHost->captureLayerState(
                            trackIndex * kLayerIdStride + newLayerIndex);
                        if (!state.isEmpty()) target->SetLayerSlotPluginState(newLayerIndex, state);
                    }
                    *redoLayers = target->GetLayerSlots();
                    target->SetLayerSlots(beforeLayers);
                }
            },
            [trackRef, redoLayers]() {
                if (auto* target = dynamic_cast<MidiTrack*>(trackRef.get()))
                    target->SetLayerSlots(*redoLayers);
            });
    } else {
        setProjectDirty();
    }
    m_inspectorWidget->setTrack(midiTrack);
    syncRealtimeMidiLayersToAudio();
    syncMidiTimelineToAudio();
    const int layerId = m_selectedTrackIndex * kLayerIdStride + newLayerIndex;
    const bool hostEvaluated = audioGeneral && m_pluginHost;
    const bool loaded = hostEvaluated && m_pluginHost->isLayerLoaded(layerId);
    const QString dspStatus = !hostEvaluated
        ? QString("DSP status pending audio engine.")
        : loaded
        ? QString("x64 plugin DSP ready for live playback and offline render.")
        : QString("DSP host unavailable for this format/plugin; using internal synth fallback.");
    ui.statusBar->showMessage(QString("Added %1 layer: %2. %3")
        .arg(layer.pluginType.isEmpty() ? QString("plugin") : layer.pluginType, layer.name, dspStatus), 6000);
}

void VeraCoreQT::onAudioClipSelected(int trackIndex, const QString& name, double startBeat, double lengthBeats, const QString& sourcePath,
                                     double sourceTempoBpm, double pitchShiftSemitones, int syncMode, double targetBars)
{
    if (m_isRecordingMidi) {
        stopMidiRecording(true);
        if (m_transportWidget) m_transportWidget->setRecording(false);
    }
    if (trackIndex < 0 || trackIndex >= (int)m_trackWidgets.size()) {
        return;
    }

    onTrackSelected(m_trackWidgets[trackIndex], m_tracks[trackIndex].get());
    PlaylistView::AudioClipInfo selectedClip;
    const bool hasSelectedClip = m_playlistView && m_playlistView->selectedAudioClipInfo(&selectedClip);
    if (hasSelectedClip && m_pluginHost) {
        m_pluginHost->setAraEditorSelection(trackIndex,
            {selectedClip.clipId.toUtf8().toStdString()});
    }
    m_inspectorWidget->setAudioClip(
        name, startBeat, lengthBeats, sourcePath, sourceTempoBpm, pitchShiftSemitones,
        syncMode, targetBars,
        hasSelectedClip ? selectedClip.tempoConfidence : 0.0,
        hasSelectedClip ? selectedClip.sourceSignatureNumerator : 0,
        hasSelectedClip ? selectedClip.sourceSignatureDenominator : 0,
        hasSelectedClip && selectedClip.tempoDetected,
        hasSelectedClip ? selectedClip.gain : 1.0f,
        hasSelectedClip ? selectedClip.pan : 0.0f,
        hasSelectedClip && selectedClip.normalize,
        hasSelectedClip && selectedClip.reverse,
        hasSelectedClip && selectedClip.reversePolarity,
        hasSelectedClip && selectedClip.loopEnabled
            ? selectedClip.loopStartBeats
            : (hasSelectedClip ? selectedClip.sourceOffsetBeats : 0.0),
        hasSelectedClip && selectedClip.loopEnabled
            ? selectedClip.loopLengthBeats : lengthBeats,
        hasSelectedClip ? selectedClip.warpLengthBeats : lengthBeats,
        hasSelectedClip ? selectedClip.fadeInBeats : 0.0,
        hasSelectedClip ? selectedClip.fadeOutBeats : 0.0,
        hasSelectedClip && selectedClip.mediaOffline,
        hasSelectedClip ? static_cast<int>(selectedClip.transientSeconds.size()) : 0,
        hasSelectedClip ? static_cast<int>(selectedClip.warpMarkers.size()) : 0);
    if (hasSelectedClip && m_audioEditorWidget) {
        m_audioEditorWidget->setClip(selectedClip);
        m_audioEditorWidget->setLoopRange(
            m_loopEnabled, m_loopStartBeat, m_loopEndBeat);
        if (m_detailTabs) m_detailTabs->setCurrentWidget(m_audioEditorWidget);
    }
    refreshSelectedAraProcessingAlgorithms();
    m_hasCurrentMidiClip = false;
    m_currentMidiClipId.clear();
    m_currentMidiClipSourceOffsetBeats = 0.0;
    refreshMidiEditorGhostClips();
    if (ui.actionMIDI_Editor) {
        QSignalBlocker blocker(ui.actionMIDI_Editor);
        ui.actionMIDI_Editor->setChecked(m_midiEditor && m_midiEditor->isVisible());
    }
}

int VeraCoreQT::importMidiFileToTracks(const QString& path, int firstTargetTrack,
                                       double insertionBeat, bool preserveFileStart,
                                       QString* errorText)
{
    if (firstTargetTrack < 0 || firstTargetTrack >= static_cast<int>(m_tracks.size()) ||
        !m_tracks[firstTargetTrack] || m_tracks[firstTargetTrack]->GetType() != TrackType::Midi ||
        !m_playlistView) {
        if (errorText) *errorText = "Select a MIDI track before importing.";
        return -1;
    }
    std::vector<MidiClipDocument> importedTracks;
    MidiClipDocument conductor;
    if (!importMidiTracksFromFile(path, importedTracks, conductor, errorText)) return -1;

    const bool previousProjectUndoSuppression = m_suppressProjectUndoCapture;
    m_suppressProjectUndoCapture = true;
    const auto projectUndoSuppressionGuard = qScopeGuard([this, previousProjectUndoSuppression]() {
        m_suppressProjectUndoCapture = previousProjectUndoSuppression;
    });
    const quint64 beforeStateId = m_playlistView->currentArrangementStateId();
    const int trackInsertIndex = static_cast<int>(m_tracks.size());
    const int selectedTrackBefore = m_selectedTrackIndex;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    const ProjectTimingState timingBefore = captureProjectTimingState();
    std::vector<std::shared_ptr<Track>> addedTracks;
    bool appliedTiming = false;
    m_playlistView->beginArrangementTransaction();

    if (m_transportWidget) {
        const bool tempoDiffers = conductor.tempoBpm > 0.0 &&
            std::fabs(conductor.tempoBpm - m_transportWidget->tempo()) > 0.01;
        const bool tempoMapDiffers = conductor.tempoMap.size() > 1u;
        const bool signatureMapDiffers = conductor.timeSignatureMap.size() > 1u;
        const bool signatureDiffers = conductor.timeSignatureNumerator > 0 &&
            conductor.timeSignatureDenominator > 0 &&
            (conductor.timeSignatureNumerator != m_transportWidget->signatureNumerator() ||
             conductor.timeSignatureDenominator != m_transportWidget->signatureDenominator());
        appliedTiming = (tempoDiffers || tempoMapDiffers ||
            signatureDiffers || signatureMapDiffers) &&
            QMessageBox::question(this,
                "Import MIDI Timing",
                "Apply the MIDI file tempo map and time signature to the project?",
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) == QMessageBox::Yes;
        if (appliedTiming && conductor.tempoBpm > 0.0)
            m_transportWidget->setTempo(conductor.tempoBpm);
        if (appliedTiming && conductor.timeSignatureNumerator > 0 &&
            conductor.timeSignatureDenominator > 0)
            m_transportWidget->setTimeSignature(
                conductor.timeSignatureNumerator, conductor.timeSignatureDenominator);
        if (appliedTiming) {
            m_tempoMap = normalizedTempoMap(conductor.tempoMap, conductor.tempoBpm);
            m_timeSignatureMap = normalizedTimeSignatureMap(
                conductor.timeSignatureMap,
                conductor.timeSignatureNumerator,
                conductor.timeSignatureDenominator);
            syncTempoMapToAudio();
            syncTimeSignatureMapToAudio();
        }
    }

    const QString fileBaseName = QFileInfo(path).completeBaseName();
    int importedCount = 0;
    for (size_t importIndex = 0; importIndex < importedTracks.size(); ++importIndex) {
        MidiClipDocument& importedDoc = importedTracks[importIndex];
        int targetTrack = firstTargetTrack;
        if (importIndex > 0) {
            QString trackName = importedDoc.trackName.trimmed();
            if (trackName.isEmpty()) trackName = importedDoc.name.trimmed();
            if (trackName.isEmpty()) trackName = QString("MIDI %1")
                .arg(static_cast<qulonglong>(m_tracks.size() + 1u));
            auto midiTrack = std::make_shared<MidiTrack>(trackName);
            if (midiTrack->GetLayerSlots().empty()) midiTrack->AddLayerSlot(LayerSlot{});
            addTrack(midiTrack);
            addedTracks.push_back(midiTrack);
            targetTrack = static_cast<int>(m_tracks.size()) - 1;
        }
        const QString importedName = importedDoc.name.trimmed();
        const QString fallbackName = fileBaseName.isEmpty()
            ? QString("Imported MIDI %1").arg(static_cast<qulonglong>(importIndex + 1u))
            : (importedTracks.size() == 1
                ? fileBaseName
                : QString("%1 %2").arg(fileBaseName)
                    .arg(static_cast<qulonglong>(importIndex + 1u)));
        const QString clipName = uniqueMidiPatternName(
            m_playlistView, targetTrack, importedName.isEmpty() ? fallbackName : importedName);
        const double clipStart = preserveFileStart
            ? safeClipStartBeat(importedDoc.startBeat)
            : safeClipStartBeat(insertionBeat);
        const double clipLength = safeClipLengthBeats(importedDoc.lengthBeats);
        if (m_playlistView->addMidiClip(targetTrack, clipStart, clipLength, clipName) < 0) {
            m_playlistView->cancelArrangementTransaction();
            for (const std::shared_ptr<Track>& addedTrack : addedTracks) {
                const auto found = std::find(m_tracks.begin(), m_tracks.end(), addedTrack);
                if (found != m_tracks.end()) m_tracks.erase(found);
            }
            rebuildTrackWidgets(selectedTrackBefore);
            applyProjectTimingState(timingBefore);
            m_nonArrangementDirty = nonArrangementDirtyBefore;
            syncRealtimeMidiLayersToAudio();
            syncMixerRoutingToAudio();
            syncTempoMapToAudio();
            syncTimeSignatureMapToAudio();
            syncMidiTimelineToAudio();
            syncAudioTimelineToAudio();
            syncProjectMetadataToMidiDocuments();
            syncPluginTransportToHost();
            refreshProjectDirtyFromArrangement(false);
            if (errorText) *errorText = "Could not add an imported MIDI track.";
            return -1;
        }
        finalizeImportedMidiClip(
            targetTrack, clipName, clipStart, clipLength, importedDoc, false);
        ++importedCount;
    }
    const bool committed = m_playlistView->commitArrangementTransaction();
    if (!committed) {
        for (const std::shared_ptr<Track>& addedTrack : addedTracks) {
            const auto found = std::find(m_tracks.begin(), m_tracks.end(), addedTrack);
            if (found != m_tracks.end()) m_tracks.erase(found);
        }
        rebuildTrackWidgets(selectedTrackBefore);
        applyProjectTimingState(timingBefore);
        m_nonArrangementDirty = nonArrangementDirtyBefore;
        syncRealtimeMidiLayersToAudio();
        syncMixerRoutingToAudio();
        syncTempoMapToAudio();
        syncTimeSignatureMapToAudio();
        syncMidiTimelineToAudio();
        syncAudioTimelineToAudio();
        syncProjectMetadataToMidiDocuments();
        syncPluginTransportToHost();
        refreshProjectDirtyFromArrangement(false);
        if (errorText) *errorText = "MIDI import produced no Arrangement change.";
        return -1;
    }
    if (!addedTracks.empty() || appliedTiming) {
        constexpr size_t kMaxProjectTransitions = 64;
        if (m_arrangementProjectTransitions.size() >= kMaxProjectTransitions)
            m_arrangementProjectTransitions.erase(m_arrangementProjectTransitions.begin());
        ArrangementProjectTransition transition;
        transition.beforeStateId = beforeStateId;
        transition.afterStateId = m_playlistView->currentArrangementStateId();
        transition.trackInsertIndex = trackInsertIndex;
        transition.addedTracks = std::move(addedTracks);
        transition.selectedTrackBefore = selectedTrackBefore;
        transition.selectedTrackAfter = m_selectedTrackIndex;
        transition.timingChanged = appliedTiming;
        transition.timingBefore = timingBefore;
        transition.timingAfter = captureProjectTimingState();
        m_arrangementProjectTransitions.push_back(std::move(transition));
    }
    m_nonArrangementDirty = nonArrangementDirtyBefore;
    refreshProjectDirtyFromArrangement(false);
    return importedCount;
}

void VeraCoreQT::finalizeImportedMidiClip(int trackIndex, const QString& clipName, double clipStart,
                                          double clipLength, const MidiClipDocument& importedDoc,
                                          bool offerTimingImport)
{
    if (trackIndex < 0 || trackIndex >= (int)m_tracks.size()) {
        return;
    }

    PlaylistView::MidiClipInfo clip;
    if (!m_playlistView || !m_playlistView->selectedMidiClipInfo(&clip)) {
        return;
    }
    const QString key = buildClipDocumentKey(clip.contentId);
    auto doc = std::make_unique<MidiClipDocument>(importedDoc);
    doc->name = clipName;
    doc->trackName = m_tracks[trackIndex]->GetName();
    doc->trackIndex = trackIndex;
    doc->startBeat = clipStart;
    doc->lengthBeats = clipLength;

    if (offerTimingImport && m_transportWidget) {
        const bool tempoDiffers = importedDoc.tempoBpm > 0.0 &&
            std::fabs(importedDoc.tempoBpm - m_transportWidget->tempo()) > 0.01;
        const bool tempoMapDiffers = importedDoc.tempoMap.size() > 1u;
        const bool signatureMapDiffers = importedDoc.timeSignatureMap.size() > 1u;
        const bool signatureDiffers = importedDoc.timeSignatureNumerator > 0 &&
            importedDoc.timeSignatureDenominator > 0 &&
            (importedDoc.timeSignatureNumerator != m_transportWidget->signatureNumerator() ||
             importedDoc.timeSignatureDenominator != m_transportWidget->signatureDenominator());
        const bool applyImportedTiming = (tempoDiffers || tempoMapDiffers ||
            signatureDiffers || signatureMapDiffers) &&
            QMessageBox::question(this,
                "Import MIDI Timing",
                "Apply the MIDI file tempo map and time signature to the project?",
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) == QMessageBox::Yes;
        if (applyImportedTiming && importedDoc.tempoBpm > 0.0)
            m_transportWidget->setTempo(importedDoc.tempoBpm);
        if (applyImportedTiming && importedDoc.timeSignatureNumerator > 0 &&
            importedDoc.timeSignatureDenominator > 0)
            m_transportWidget->setTimeSignature(importedDoc.timeSignatureNumerator,
                                                importedDoc.timeSignatureDenominator);
        if (applyImportedTiming) {
            m_tempoMap = normalizedTempoMap(importedDoc.tempoMap, importedDoc.tempoBpm);
            m_timeSignatureMap = normalizedTimeSignatureMap(
                importedDoc.timeSignatureMap,
                importedDoc.timeSignatureNumerator,
                importedDoc.timeSignatureDenominator);
            syncTempoMapToAudio();
            syncTimeSignatureMapToAudio();
        }
    }
    if (m_transportWidget) {
        doc->tempoBpm = m_transportWidget->tempo();
        doc->timeSignatureNumerator = m_transportWidget->signatureNumerator();
        doc->timeSignatureDenominator = m_transportWidget->signatureDenominator();
    }

    m_midiClipDocs[key] = std::move(doc);
    m_currentMidiClipKey = key;
    m_currentMidiClipId = clip.clipId;
    m_currentMidiTrackIndex = trackIndex;
    m_currentMidiClipName = clipName;
    m_currentMidiClipStartBeat = clipStart;
    m_currentMidiClipLengthBeats = clipLength;
    m_currentMidiClipSourceOffsetBeats = qMax(0.0, clip.sourceOffsetBeats);
    m_hasCurrentMidiClip = true;
    m_inspectorWidget->setMidiClip(clipName, clipStart, clipLength);
    syncMidiTimelineToAudio();
    refreshMidiEditorGhostClips();
}

void VeraCoreQT::onMakeSelectedClipUnique()
{
    if (!m_playlistView) {
        return;
    }

    auto confirmSingle = [this]() {
        return QMessageBox::question(this,
            "Make Unique",
            "This clip does not appear anywhere else. Make it unique anyway?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;
    };
    auto uniqueName = [this](const QString& baseName, bool audio) {
        const QString base = QString("%1 unique").arg(baseName.trimmed().isEmpty()
            ? (audio ? "Audio" : "MIDI")
            : baseName.trimmed());
        auto exists = [&](const QString& candidate) {
            if (audio) {
                for (const PlaylistView::AudioClipInfo& clip : m_playlistView->getAudioClips()) {
                    if (clip.name == candidate) return true;
                }
            } else {
                for (const PlaylistView::MidiClipInfo& clip : m_playlistView->getMidiClips()) {
                    if (clip.name == candidate) return true;
                }
            }
            return false;
        };
        if (!exists(base)) {
            return base;
        }
        for (int i = 2; i < 1000; ++i) {
            const QString candidate = QString("%1 %2").arg(base).arg(i);
            if (!exists(candidate)) {
                return candidate;
            }
        }
        return QString("%1 %2").arg(base).arg(QDateTime::currentMSecsSinceEpoch());
    };

    if (m_playlistView->selectedClipIsAudio()) {
        PlaylistView::AudioClipInfo clip;
        if (!m_playlistView->selectedAudioClipInfo(&clip)) {
            return;
        }
        const int instances = m_playlistView->selectedClipReferenceCount();
        if (instances <= 1 && !confirmSingle()) {
            return;
        }
        const QString newName = uniqueName(clip.name, true);
        const QFileInfo sourceInfo(clip.sourcePath);
        if (!sourceInfo.exists() || !sourceInfo.isFile() || sourceInfo.size() <= 0) {
            QMessageBox::critical(this, "Make Unique", "Audio source file is missing or empty.");
            return;
        }

        auto availableCopyPath = [&](const QString& directoryPath) {
            QDir directory(directoryPath);
            if (!directory.exists() && !directory.mkpath(".")) {
                return QString();
            }
            const QString stem = sourceInfo.completeBaseName().isEmpty() ? QString("audio") : sourceInfo.completeBaseName();
            const QString extension = sourceInfo.completeSuffix().isEmpty()
                ? QString()
                : QString(".%1").arg(sourceInfo.completeSuffix());
            for (int i = 1; i < 10000; ++i) {
                const QString number = i == 1 ? QString() : QString(" %1").arg(i);
                const QString candidate = directory.filePath(QString("%1 unique%2%3").arg(stem, number, extension));
                if (!QFileInfo::exists(candidate)) {
                    return candidate;
                }
            }
            return QString();
        };

        QString copiedPath;
        const QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString appAudioDir = QDir(appDataPath.isEmpty() ? QDir::tempPath() : appDataPath)
            .filePath("Unique Audio");
        const QStringList targetDirectories = {sourceInfo.absolutePath(), appAudioDir};
        for (const QString& directory : targetDirectories) {
            const QString candidate = availableCopyPath(directory);
            if (candidate.isEmpty() || !QFile::copy(sourceInfo.absoluteFilePath(), candidate)) {
                continue;
            }
            if (QFileInfo(candidate).size() == sourceInfo.size()) {
                copiedPath = candidate;
                break;
            }
            QFile::remove(candidate);
        }
        if (copiedPath.isEmpty()) {
            QMessageBox::critical(this, "Make Unique", "Could not clone the audio source file.");
            return;
        }
        if (!m_playlistView->makeSelectedAudioClipUnique(newName, copiedPath)) {
            QFile::remove(copiedPath);
            QMessageBox::critical(this, "Make Unique", "Could not update the selected audio clip.");
            return;
        }
        syncAudioTimelineToAudio();
        ui.statusBar->showMessage("Audio clip made unique with an independent source copy.", 3000);
        return;
    }

    PlaylistView::MidiClipInfo clip;
    if (!m_playlistView->selectedMidiClipInfo(&clip)) {
        QMessageBox::information(this, "Make Unique", "Select a MIDI or audio clip first.");
        return;
    }
    const int instances = m_playlistView->selectedClipReferenceCount();
    if (instances <= 1 && !confirmSingle()) {
        return;
    }

    const QString oldKey = buildClipDocumentKey(clip.contentId);
    auto oldIt = m_midiClipDocs.find(oldKey);
    if (oldIt == m_midiClipDocs.end() || !oldIt->second) {
        QMessageBox::critical(this, "Make Unique", "The selected MIDI content is missing.");
        return;
    }
    MidiClipDocument clonedDoc = *oldIt->second;

    const QString newName = uniqueName(clip.name, false);
    const QString newContentId = m_playlistView->makeSelectedMidiClipUnique(newName);
    if (newContentId.isEmpty()) {
        return;
    }
    const QString newKey = buildClipDocumentKey(newContentId);
    clonedDoc.name = newName;
    clonedDoc.trackIndex = clip.trackIndex;
    clonedDoc.startBeat = clip.startBeat;
    clonedDoc.lengthBeats = qMax(clonedDoc.lengthBeats,
                                 clip.sourceOffsetBeats + clip.lengthBeats);
    m_midiClipDocs[newKey] = std::make_unique<MidiClipDocument>(clonedDoc);
    if (instances <= 1 &&
        !m_playlistView->clipboardContainsMidiContent(oldKey) &&
        !m_playlistView->historyContainsMidiContent(oldKey))
        m_midiClipDocs.erase(oldKey);
    m_currentMidiClipKey = newKey;
    m_currentMidiClipId = clip.clipId;
    m_currentMidiClipName = newName;
    m_hasCurrentMidiClip = true;
    if (m_midiEditor) {
        m_midiEditor->setClip(m_midiClipDocs[newKey].get());
    }
    syncMidiTimelineToAudio();
    refreshMidiEditorGhostClips();
    ui.statusBar->showMessage("MIDI clip made unique.", 3000);
}

void VeraCoreQT::onMidiClipSelected(int trackIndex, const QString& name, double startBeat, double lengthBeats) {
    if (trackIndex < 0 || trackIndex >= (int)m_trackWidgets.size()) {
        return;
    }

    onTrackSelected(m_trackWidgets[trackIndex], m_tracks[trackIndex].get());
    m_inspectorWidget->setMidiClip(name, startBeat, lengthBeats);
    const double safeStartBeat = safeClipStartBeat(startBeat);
    const double safeLengthBeats = safeClipLengthBeats(lengthBeats);
    PlaylistView::MidiClipInfo clip;
    if (!m_playlistView || !m_playlistView->selectedMidiClipInfo(&clip)) {
        return;
    }
    const QString nextClipKey = buildClipDocumentKey(clip.contentId);
    if (m_isRecordingMidi && nextClipKey != m_currentMidiClipKey) {
        stopMidiRecording(true);
        if (m_transportWidget) m_transportWidget->setRecording(false);
    }
    m_currentMidiTrackIndex = trackIndex;
    m_currentMidiClipName = name;
    m_currentMidiClipStartBeat = safeStartBeat;
    m_currentMidiClipLengthBeats = safeLengthBeats;
    m_currentMidiClipSourceOffsetBeats = qMax(0.0, clip.sourceOffsetBeats);
    m_currentMidiClipKey = nextClipKey;
    m_currentMidiClipId = clip.clipId;
    auto& doc = m_midiClipDocs[m_currentMidiClipKey];
    if (!doc) {
        doc = std::make_unique<MidiClipDocument>();
        doc->name = name;
        doc->trackName = m_tracks[trackIndex]->GetName();
        doc->trackIndex = trackIndex;
        if (m_transportWidget) {
            doc->tempoBpm = m_transportWidget->tempo();
            doc->timeSignatureNumerator = m_transportWidget->signatureNumerator();
            doc->timeSignatureDenominator = m_transportWidget->signatureDenominator();
        }
    }
    doc->startBeat = safeStartBeat;
    doc->lengthBeats = qMax(doc->lengthBeats,
                            m_currentMidiClipSourceOffsetBeats + safeLengthBeats);
    doc->trackIndex = trackIndex;
    doc->trackName = m_tracks[trackIndex]->GetName();
    m_hasCurrentMidiClip = true;
    if (m_midiEditor) {
        m_midiEditor->setClip(doc.get());
        refreshMidiEditorExpressionMap();
        refreshMidiEditorGhostClips();
        updateMidiEditorPlayhead();
    }
}

void VeraCoreQT::onMidiClipOpened(int trackIndex, const QString& name, double startBeat, double lengthBeats) {
    onMidiClipSelected(trackIndex, name, startBeat, lengthBeats);
    openMidiEditorForClip(trackIndex, name, startBeat, lengthBeats);
}

void VeraCoreQT::onOpenCurrentMidiEditor()
{
    if (!m_hasCurrentMidiClip) {
        ui.statusBar->showMessage("Select or double-click a MIDI clip before opening the MIDI editor.", 4000);
        QSignalBlocker blocker(ui.actionMIDI_Editor);
        ui.actionMIDI_Editor->setChecked(false);
        return;
    }

    openMidiEditorForClip(m_currentMidiTrackIndex, m_currentMidiClipName,
                          m_currentMidiClipStartBeat, m_currentMidiClipLengthBeats);
}

void VeraCoreQT::openMidiEditorForClip(int trackIndex, const QString& name, double startBeat, double lengthBeats)
{
    if (trackIndex < 0 || trackIndex >= (int)m_tracks.size()) {
        QSignalBlocker blocker(ui.actionMIDI_Editor);
        ui.actionMIDI_Editor->setChecked(false);
        return;
    }
    if (m_tracks[trackIndex]->GetType() != TrackType::Midi) {
        QSignalBlocker blocker(ui.actionMIDI_Editor);
        ui.actionMIDI_Editor->setChecked(false);
        return;
    }

    const double safeStartBeat = safeClipStartBeat(startBeat);
    const double safeLengthBeats = safeClipLengthBeats(lengthBeats);
    PlaylistView::MidiClipInfo clip;
    if (!m_playlistView || !m_playlistView->selectedMidiClipInfo(&clip)) {
        QSignalBlocker blocker(ui.actionMIDI_Editor);
        ui.actionMIDI_Editor->setChecked(false);
        ui.statusBar->showMessage(
            "Select or double-click a MIDI clip before opening the MIDI editor.", 4000);
        return;
    }
    const QString key = buildClipDocumentKey(clip.contentId);
    auto& doc = m_midiClipDocs[key];
    if (!doc) {
        doc = std::make_unique<MidiClipDocument>();
        doc->name = name;
        doc->trackName = m_tracks[trackIndex]->GetName();
        doc->trackIndex = trackIndex;
        if (m_transportWidget) {
            doc->tempoBpm = m_transportWidget->tempo();
            doc->timeSignatureNumerator = m_transportWidget->signatureNumerator();
            doc->timeSignatureDenominator = m_transportWidget->signatureDenominator();
        }
    }
    doc->startBeat = safeStartBeat;
    doc->lengthBeats = qMax(doc->lengthBeats,
                            qMax(0.0, clip.sourceOffsetBeats) + safeLengthBeats);
    m_currentMidiClipKey = key;
    m_currentMidiClipId = clip.clipId;
    m_currentMidiTrackIndex = trackIndex;
    m_currentMidiClipName = name;
    m_currentMidiClipStartBeat = safeStartBeat;
    m_currentMidiClipLengthBeats = safeLengthBeats;
    m_currentMidiClipSourceOffsetBeats = qMax(0.0, clip.sourceOffsetBeats);
    m_hasCurrentMidiClip = true;

    QWidget* currentFocus = QApplication::focusWidget();
    if (currentFocus && currentFocus->window() == this) {
        m_focusBeforeMidiEditor = currentFocus;
    }

    if (!m_midiEditor) {
        m_midiEditor = new MidiEditorWindow(this);
        m_midiEditor->setCloseCallback([this]() {
            {
                QSignalBlocker blocker(ui.actionMIDI_Editor);
                ui.actionMIDI_Editor->setChecked(false);
            }
            const QPointer<QWidget> focusTarget = m_focusBeforeMidiEditor;
            m_focusBeforeMidiEditor.clear();
            QTimer::singleShot(0, this, [this, focusTarget]() {
                if (!isVisible() || QApplication::closingDown()) return;
                raise();
                activateWindow();
                if (focusTarget && focusTarget->isVisible() && focusTarget->isEnabled()) {
                    focusTarget->setFocus(Qt::ShortcutFocusReason);
                } else if (m_playlistView && m_playlistView->isVisible()) {
                    m_playlistView->setFocus(Qt::ShortcutFocusReason);
                } else if (ui.browserPanel->isVisible()) {
                    ui.browserSearch->setFocus(Qt::ShortcutFocusReason);
                }
            });
        });
        m_midiEditor->setDocumentChangedCallback(
            [this](const MidiClipDocument& beforeDocument,
                   const MidiClipDocument& afterDocument,
                   quint64 editSerial) {
            if (midiDocumentsPersistentlyEqual(beforeDocument, afterDocument)) return;
            const QString contentId = m_currentMidiClipKey;
            const auto current = m_midiClipDocs.find(contentId);
            if (contentId.isEmpty() || current == m_midiClipDocs.end() ||
                !current->second || current->second.get() != &afterDocument) return;

            const bool captureUndo = m_playlistView && !m_restoringProject &&
                !m_suppressProjectUndoCapture;
            const quint64 beforeStateId = captureUndo
                ? m_playlistView->currentArrangementStateId() : 0;
            const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
            if (captureUndo) {
                m_playlistView->beginArrangementTransaction();
                m_playlistView->markArrangementTransactionChanged();
                const QString mergeKey = QStringLiteral("gesture:midi-doc:%1:%2")
                    .arg(contentId)
                    .arg(static_cast<qulonglong>(editSerial));
                if (!commitProjectUndoStep(
                        beforeStateId, nonArrangementDirtyBefore, mergeKey,
                        [this, contentId, beforeDocument]() {
                            applyMidiDocumentState(contentId, beforeDocument);
                        },
                        [this, contentId, afterDocument]() {
                            applyMidiDocumentState(contentId, afterDocument);
                        })) {
                    setProjectDirty();
                }
            } else {
                setProjectDirty();
            }
            syncMidiTimelineToAudio();
            refreshMidiEditorGhostClips();
        });
        m_midiEditor->setUndoRequestCallback([this]() { performGlobalUndo(); });
        m_midiEditor->setRedoRequestCallback([this]() { performGlobalRedo(); });
        m_midiEditor->setPlayheadSeekCallback([this](double relativeBeat) {
            const double sourceStart = qMax(0.0, m_currentMidiClipSourceOffsetBeats);
            const double sourceEnd = sourceStart + qMax(0.0, m_currentMidiClipLengthBeats);
            const double sourceBeat = qBound(sourceStart, relativeBeat, sourceEnd);
            seekPlayhead(m_currentMidiClipStartBeat + sourceBeat - sourceStart);
        });
        m_midiEditor->setNotePreviewCallback(
            [this](int pitch, int channel, bool noteOn, bool isKeyswitch) {
            if (!audioGeneral || !isPluginMidiPitch(pitch)) return;
            VeraCoreMidiEvent event;
            event.status = static_cast<uint8_t>((noteOn ? 0x90 : 0x80) | qBound(0, channel, 15));
            event.data1 = static_cast<uint8_t>(pitch);
            event.data2 = static_cast<uint8_t>(noteOn ? 100 : 0);
            event.isKeyswitch = isKeyswitch;
            audioGeneral->audioCore.QueueRealtimeMidiEvent(event);
            audioGeneral->SendMidiToOutputs(event);
        });
        m_midiEditor->setMidiChannelChangedCallback([this](int) {
            refreshMidiEditorExpressionMap();
        });
    }
    if (m_transportWidget) {
        m_midiEditor->setProjectScale(m_transportWidget->keyRoot(), m_transportWidget->minorScale());
    }
    m_midiEditor->setClip(doc.get());
    refreshMidiEditorExpressionMap();
    refreshMidiEditorGhostClips();
    updateMidiEditorPlayhead();
    m_midiEditor->show();
    m_midiEditor->raise();
    m_midiEditor->activateWindow();
    ui.actionMIDI_Editor->setChecked(true);
    syncMidiTimelineToAudio();
}

void VeraCoreQT::syncMidiTimelineToAudio()
{
    if (!audioGeneral) {
        return;
    }

    const std::vector<VeraCoreBeatMidiEvent> events = buildTimelineEvents();
    if (m_isPlaying && m_pluginHost) {
        m_pluginHost->panic();
    }
    audioGeneral->audioCore.SetMidiTimelineEvents(events);
}

void VeraCoreQT::syncTempoMapToAudio()
{
    if (!audioGeneral) return;
    const double fallback = m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm;
    m_tempoMap = normalizedTempoMap(m_tempoMap, fallback);
    std::vector<VeraCoreTempoPoint> published;
    published.reserve(m_tempoMap.size());
    for (const MidiTempoPoint& point : m_tempoMap) published.push_back({point.beat, point.bpm});
    audioGeneral->audioCore.SetTempoMap(published);
}

void VeraCoreQT::syncTimeSignatureMapToAudio()
{
    const int fallbackNumerator = m_transportWidget ? m_transportWidget->signatureNumerator() : 4;
    const int fallbackDenominator = m_transportWidget ? m_transportWidget->signatureDenominator() : 4;
    m_timeSignatureMap = normalizedTimeSignatureMap(
        m_timeSignatureMap, fallbackNumerator, fallbackDenominator);
    if (m_transportWidget) m_transportWidget->setTimeSignatureMap(m_timeSignatureMap);
    if (m_playlistView) m_playlistView->setTimeSignatureMap(m_timeSignatureMap);
    if (!audioGeneral) return;
    std::vector<VeraCoreTimeSignaturePoint> published;
    published.reserve(m_timeSignatureMap.size());
    for (const MidiTimeSignaturePoint& point : m_timeSignatureMap)
        published.push_back({point.beat, point.numerator, point.denominator});
    audioGeneral->audioCore.SetTimeSignatureMap(published);
}

void VeraCoreQT::syncMixerRoutingToAudio()
{
    if (!audioGeneral) return;
	const bool anyTrackSolo = std::any_of(m_tracks.begin(), m_tracks.end(),
		[](const std::shared_ptr<Track>& track) { return track && track->IsSoloed(); });
    std::vector<VeraCoreTrackSendState> sends;
    sends.reserve(m_tracks.size());
    for (int trackIndex = 0; trackIndex < static_cast<int>(m_tracks.size()); ++trackIndex) {
        if (!m_tracks[trackIndex]) continue;
        VeraCoreTrackSendState state;
        state.trackId = trackIndex;
        state.gain = m_tracks[trackIndex]->GetVolume();
        state.pan = m_tracks[trackIndex]->GetPan();
		state.stereoSeparation = m_tracks[trackIndex]->GetStereoSeparation();
		state.spatial = m_tracks[trackIndex]->GetSpatialPanner();
		state.audible = !m_tracks[trackIndex]->IsMuted() &&
			(!anyTrackSolo || m_tracks[trackIndex]->IsSoloed());
        for (int bus = 0; bus < kReturnBusCount; ++bus)
            state.levels[bus] = m_tracks[trackIndex]->GetSendLevel(bus);
        sends.push_back(state);
    }
    std::array<VeraCoreReturnBusState, kReturnBusCount> returns{};
    const auto& returnBuses = m_mixerParameters->returnBuses();
    for (int bus = 0; bus < kReturnBusCount; ++bus) {
        returns[bus].gain = returnBuses[bus].volume;
        returns[bus].pan = returnBuses[bus].pan;
        returns[bus].stereoSeparation = returnBuses[bus].stereoSeparation;
        returns[bus].muted = returnBuses[bus].muted;
        returns[bus].soloed = returnBuses[bus].soloed;
        returns[bus].preFader = returnBuses[bus].preFader;
    }
    audioGeneral->audioCore.SetMixerRouting(sends, returns);
    const MasterBusState& masterBus = m_mixerParameters->masterBus();
    audioGeneral->audioCore.SetMasterState(
        masterBus.volume, masterBus.pan, masterBus.stereoSeparation, masterBus.muted);
}

void VeraCoreQT::syncAudioTimelineToAudio()
{
    if (!audioGeneral || !m_playlistView) {
        return;
    }

    const double projectTempo = m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm;
    const double safeTempo = finiteClamp(projectTempo, 20.0, 300.0, kDefaultTempoBpm);
    const TempoClock araTempoClock(m_tempoMap, safeTempo);
    const int safeSampleRate = safeRenderSampleRate(
        audioGeneral->audioCore.GetStreamSampleRate(), 44100);

    const std::vector<PlaylistView::AudioClipInfo> audioClipInfos =
        m_playlistView->getAudioClips();
    std::vector<AraAudioSourceDescriptor> araSources;
    std::set<QString> araContentIds;
    std::map<QString, double> araSourceDurations;
    araSources.reserve(audioClipInfos.size());
    for (const PlaylistView::AudioClipInfo& clip : audioClipInfos) {
        const QString contentId = clip.contentId.trimmed();
        if (contentId.isEmpty() || !araContentIds.insert(contentId).second) continue;
        const DecodedAudioFile* source = cachedDecodedAudio(clip.sourcePath);
        const auto sharedSamples = sharedPlaybackSamples(source);
        if (!source || source->sampleRate <= 0 || !sharedSamples) continue;
        AraAudioSourceDescriptor descriptor;
        descriptor.persistentId = contentId.toUtf8().toStdString();
        descriptor.nameUtf8 = clip.name.toUtf8().toStdString();
        descriptor.sampleRate = source->sampleRate;
        descriptor.channelCount = source->sourceChannels == 1 ? 1 : 2;
        descriptor.interleavedStereoSamples = sharedSamples;
        araSources.push_back(std::move(descriptor));
        araSourceDurations[contentId] = static_cast<double>(sharedSamples->size() / 2u) /
            static_cast<double>(source->sampleRate);
    }
    std::vector<AraPlaybackRegionDescriptor> araRegions;
    araRegions.reserve(audioClipInfos.size());
    for (const PlaylistView::AudioClipInfo& clip : audioClipInfos) {
        if (!clipTakeIsActive(clip)) continue;
        const QString clipId = clip.clipId.trimmed();
        const QString contentId = clip.contentId.trimmed();
        const auto duration = araSourceDurations.find(contentId);
        if (clipId.isEmpty() || duration == araSourceDurations.end() ||
            clip.trackIndex < 0 || clip.trackIndex >= static_cast<int>(m_tracks.size()) ||
            !isFiniteBeatRange(clip.startBeat, clip.lengthBeats)) continue;
        const Track* track = m_tracks[clip.trackIndex].get();
        if (!track || track->GetType() != TrackType::Audio) continue;
        AraClipRegionInput regionInput;
        regionInput.clipId = clipId;
        regionInput.contentId = contentId;
        regionInput.name = clip.name;
        regionInput.trackName = track->GetName();
        regionInput.trackIndex = clip.trackIndex;
        regionInput.startBeat = clip.startBeat;
        regionInput.lengthBeats = clip.lengthBeats;
        regionInput.sourceOffsetBeats = clip.sourceOffsetBeats;
        regionInput.loopEnabled = clip.loopEnabled;
        regionInput.loopStartBeats = clip.loopStartBeats;
        regionInput.loopLengthBeats = clip.loopLengthBeats;
        regionInput.warpLengthBeats = clip.warpLengthBeats;
        regionInput.sourceTempoBpm = std::isfinite(clip.sourceTempoBpm) &&
            clip.sourceTempoBpm > 0.0 ? clip.sourceTempoBpm : safeTempo;
        regionInput.syncMode = clip.syncMode;
        regionInput.warpMarkers = &clip.warpMarkers;
        appendAraClipRegions(regionInput, duration->second, araTempoClock, araRegions);
    }
    double tempoHorizonBeat = 1.0;
    for (const PlaylistView::AudioClipInfo& clip : audioClipInfos)
        if (isFiniteBeatRange(clip.startBeat, clip.lengthBeats))
            tempoHorizonBeat = qMax(tempoHorizonBeat, clip.startBeat + clip.lengthBeats);
    const std::vector<AraTempoEntryDescriptor> araTempoEntries = buildAraTempoEntries(
        m_tempoMap, araTempoClock, safeTempo, tempoHorizonBeat);
    const std::vector<AraBarSignatureDescriptor> araBarSignatures = buildAraBarSignatures(
        m_timeSignatureMap,
        m_transportWidget ? m_transportWidget->signatureNumerator() : 4,
        m_transportWidget ? m_transportWidget->signatureDenominator() : 4);
    if (m_pluginHost) m_pluginHost->syncAraModelGraph(araSources, araRegions, araTempoEntries, araBarSignatures);

    bool anySolo = false;
    for (const auto& track : m_tracks) {
        if (track && track->IsSoloed()) {
            anySolo = true;
            break;
        }
    }

    std::vector<VeraCoreAudioTimelineClip> clips;
    int skippedAudioClips = 0;
    QString firstSkippedAudioClip;
    for (const PlaylistView::AudioClipInfo& clip : audioClipInfos) {
        if (!clipTakeIsActive(clip) || clip.muted || clip.trackIndex < 0 ||
            clip.trackIndex >= static_cast<int>(m_tracks.size()) ||
            !isFiniteBeatRange(clip.startBeat, clip.lengthBeats)) {
            continue;
        }
        const Track* track = m_tracks[clip.trackIndex].get();
        if (!track || track->GetType() != TrackType::Audio || track->IsMuted() || track->IsMonitoring() ||
            (anySolo && !track->IsSoloed())) {
            continue;
        }

        bool processedSync = false;
        const double clipTempo = averageTempoForBeatRange(
            araTempoClock, clip.startBeat, clip.lengthBeats, safeTempo);
        const DecodedAudioFile* decoded = cachedDecodedAudioForClipSync(clip.sourcePath,
            clip.sourceTempoBpm, clip.pitchShiftSemitones, clip.syncMode, clip.targetBars,
			clip.beatsPerBar, clip.warpLengthBeats, clipTempo, safeSampleRate, &processedSync);
        if (!decoded || decoded->sampleRate <= 0 || decoded->stereoSamples.size() < 2) {
            ++skippedAudioClips;
            if (firstSkippedAudioClip.isEmpty()) {
                const QFileInfo sourceInfo(clip.sourcePath);
                firstSkippedAudioClip = sourceInfo.fileName().isEmpty() ? clip.name : sourceInfo.fileName();
            }
            continue;
        }

        VeraCoreAudioTimelineClip audioClip;
        audioClip.trackId = clip.trackIndex;
        audioClip.startBeat = clip.startBeat;
        audioClip.lengthBeats = clip.lengthBeats;
        audioClip.sourceOffsetBeats = clip.sourceOffsetBeats;
        audioClip.loopEnabled = clip.loopEnabled;
        audioClip.loopStartBeats = clip.loopStartBeats;
        audioClip.loopLengthBeats = clip.loopLengthBeats;
        audioClip.warpLengthBeats = clip.warpLengthBeats;
        audioClip.sourceTempoBpm = clip.sourceTempoBpm;
        audioClip.pitchShiftSemitones = processedSync || clip.syncMode > 0 ? 0.0 : clip.pitchShiftSemitones;
        audioClip.syncMode = clip.syncMode;
        audioClip.targetBars = clip.targetBars;
        audioClip.beatsPerBar = clip.beatsPerBar;
        audioClip.sampleRate = decoded->sampleRate;
        audioClip.monoSource = decoded->sourceChannels == 1;
        audioClip.sharedStereoSamples = sharedPlaybackSamples(decoded);
        if (!audioClip.sharedStereoSamples) {
            ++skippedAudioClips;
            if (firstSkippedAudioClip.isEmpty()) {
                const QFileInfo sourceInfo(clip.sourcePath);
                firstSkippedAudioClip = sourceInfo.fileName().isEmpty() ? clip.name : sourceInfo.fileName();
            }
            continue;
        }
        const double decodedDurationSeconds = static_cast<double>(decoded->frameCount()) /
            static_cast<double>(decoded->sampleRate);
        const double warpSourceDuration = clip.sourceDurationSeconds > 0.0
            ? clip.sourceDurationSeconds : decodedDurationSeconds;
        audioClip.sharedWarpPoints = clip.loopEnabled
            ? audioLoopWarpPoints(clip.warpMarkers, warpSourceDuration,
                clip.warpLengthBeats, clip.loopStartBeats,
                clip.loopLengthBeats, clip.sourceOffsetBeats)
            : audioUserWarpPoints(clip.warpMarkers, warpSourceDuration,
                clip.warpLengthBeats, clip.sourceOffsetBeats, clip.lengthBeats);
        if (audioClip.sharedWarpPoints) {
            audioClip.warpPointsUseAbsoluteSourceProgress = true;
        } else if (processedSync && !clip.loopEnabled) {
            audioClip.sharedWarpPoints = audioSyncWarpPoints(
                araTempoClock, clip.startBeat, clip.lengthBeats);
        }
        audioClip.gain = clip.gain;
        audioClip.pan = clip.pan;
        audioClip.normalizationGain = clip.normalize ? normalizationGainForDecoded(decoded) : 1.0f;
        audioClip.reverse = clip.reverse;
        audioClip.reversePolarity = clip.reversePolarity;
        audioClip.fadeInBeats = clip.fadeInBeats;
        audioClip.fadeOutBeats = clip.fadeOutBeats;
        audioClip.fadeInCurve = clip.fadeInCurve;
        audioClip.fadeOutCurve = clip.fadeOutCurve;
        if (!clip.gainEnvelope.empty()) {
            std::vector<VeraCoreAudioGainPoint> gainEnvelope;
            gainEnvelope.reserve(clip.gainEnvelope.size());
            for (const AudioGainEnvelopePoint& point : clip.gainEnvelope)
                gainEnvelope.push_back({point.beat, point.gain});
            audioClip.sharedGainEnvelope =
                std::make_shared<const std::vector<VeraCoreAudioGainPoint>>(std::move(gainEnvelope));
        }
        clips.push_back(std::move(audioClip));
    }

    audioGeneral->audioCore.SetAudioTimelineClips(std::move(clips));
    if (skippedAudioClips > 0) {
        ui.statusBar->showMessage(QString("Skipped %1 audio clip%2 during playback sync: %3")
            .arg(skippedAudioClips)
            .arg(skippedAudioClips == 1 ? "" : "s")
            .arg(firstSkippedAudioClip), 5000);
    }
}

void VeraCoreQT::trimDecodedAudioCache(std::map<QString, CachedDecodedAudio>& cache,
                                       size_t maxEntries, size_t maxSamples) const
{
    if (maxEntries == 0 || maxSamples == 0) {
        cache.clear();
        return;
    }
    auto sampleCount = [](const CachedDecodedAudio& entry) {
        const size_t decoded = static_cast<size_t>(entry.audio.stereoSamples.size());
        const size_t playback = entry.playbackSamples ? entry.playbackSamples->size() : 0;
        return playback > std::numeric_limits<size_t>::max() - decoded
            ? std::numeric_limits<size_t>::max()
            : decoded + playback;
    };
    size_t totalSamples = 0;
    for (const auto& entry : cache) {
        const size_t count = sampleCount(entry.second);
        totalSamples = count > std::numeric_limits<size_t>::max() - totalSamples
            ? std::numeric_limits<size_t>::max()
            : totalSamples + count;
    }
    while ((cache.size() > maxEntries || totalSamples > maxSamples) && cache.size() > 1) {
        const auto oldest = std::min_element(cache.begin(), cache.end(),
            [](const auto& left, const auto& right) {
                return left.second.lastUse < right.second.lastUse;
            });
        if (oldest == cache.end()) break;
        const size_t removedSamples = sampleCount(oldest->second);
        totalSamples = removedSamples > totalSamples ? 0 : totalSamples - removedSamples;
        cache.erase(oldest);
    }
}

std::shared_ptr<const std::vector<float>> VeraCoreQT::sharedPlaybackSamples(
    const DecodedAudioFile* decoded) const
{
    if (!decoded || decoded->stereoSamples.size() < 2) return {};
    auto createSamples = [decoded]() -> std::shared_ptr<const std::vector<float>> {
        try {
            auto samples = std::make_shared<std::vector<float>>(
                decoded->stereoSamples.begin(), decoded->stereoSamples.end());
            if ((samples->size() % 2u) != 0) samples->pop_back();
            return samples->size() >= 2 ? samples : std::shared_ptr<const std::vector<float>>{};
        } catch (const std::bad_alloc&) {
            return {};
        }
    };
    auto findOrCreate = [&](std::map<QString, CachedDecodedAudio>& cache,
                            size_t maxEntries, size_t maxSamples) {
        for (auto& entry : cache) {
            if (&entry.second.audio != decoded) continue;
            entry.second.lastUse = ++m_audioCacheUseCounter;
            if (!entry.second.playbackSamples) entry.second.playbackSamples = createSamples();
            const auto result = entry.second.playbackSamples;
            trimDecodedAudioCache(cache, maxEntries, maxSamples);
            return result;
        }
        return std::shared_ptr<const std::vector<float>>{};
    };
    if (auto samples = findOrCreate(
            m_audioDecodeCache, kMaxDecodedCacheEntries, kMaxDecodedCacheSamples)) return samples;
    if (auto samples = findOrCreate(
            m_audioSyncDecodeCache, kMaxSyncCacheEntries, kMaxSyncCacheSamples)) return samples;
    return createSamples();
}

float VeraCoreQT::normalizationGainForDecoded(const DecodedAudioFile* decoded) const
{
    if (!decoded) return 1.0f;
    for (const auto& entry : m_audioDecodeCache)
        if (&entry.second.audio == decoded) return entry.second.normalizationGain;
    for (const auto& entry : m_audioSyncDecodeCache)
        if (&entry.second.audio == decoded) return entry.second.normalizationGain;
    return peakNormalizationGain(*decoded);
}

const DecodedAudioFile* VeraCoreQT::cachedDecodedAudio(const QString& path) const
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        m_audioDecodeCache.erase(path);
        return nullptr;
    }

    const qint64 size = info.size();
    const QDateTime modified = info.lastModified();
    auto cached = m_audioDecodeCache.find(path);
    if (cached != m_audioDecodeCache.end() &&
        cached->second.fileSize == size &&
        cached->second.lastModified == modified) {
        cached->second.lastUse = ++m_audioCacheUseCounter;
        return &cached->second.audio;
    }

    AudioDecodeResult decoded = AudioFileDecoder::decodeAudio(path);
    if (!decoded.success || decoded.audio.sampleRate <= 0 || decoded.audio.stereoSamples.size() < 2) {
        m_audioDecodeCache.erase(path);
        return nullptr;
    }

    CachedDecodedAudio entry;
    entry.fileSize = size;
    entry.lastModified = modified;
    entry.audio = std::move(decoded.audio);
    entry.normalizationGain = peakNormalizationGain(entry.audio);
    entry.lastUse = ++m_audioCacheUseCounter;
    auto inserted = m_audioDecodeCache.insert_or_assign(path, std::move(entry));
    trimDecodedAudioCache(m_audioDecodeCache, kMaxDecodedCacheEntries, kMaxDecodedCacheSamples);
    return &inserted.first->second.audio;
}

const DecodedAudioFile* VeraCoreQT::cachedDecodedAudioForClipSync(const QString& path, double sourceTempoBpm, double pitchShiftSemitones,
                                                                  int syncMode, double targetBars, double beatsPerBar, double lengthBeats,
                                                                  double projectTempoBpm, int sampleRate, bool* processed) const
{
    if (processed) {
        *processed = false;
    }

    const DecodedAudioFile* source = cachedDecodedAudio(path);
    if (!source || source->sampleRate <= 0 || source->frameCount() <= 0) {
        return nullptr;
    }

    const double safePitch = std::isfinite(pitchShiftSemitones) ? qBound(-48.0, pitchShiftSemitones, 48.0) : 0.0;
    const bool needsPitch = std::fabs(safePitch) > 0.0001;
    const bool needsStretch = syncMode > 0;
    const double detectedSourceTempo = std::isfinite(sourceTempoBpm) && sourceTempoBpm > 0.0
        ? qBound(20.0, sourceTempoBpm, 300.0)
        : 0.0;
    const double safeProjectTempo = std::isfinite(projectTempoBpm) && projectTempoBpm > 0.0
        ? qBound(20.0, projectTempoBpm, 300.0)
        : 0.0;
    // Stretch Off is intentional varispeed. Processing pitch here and then
    // changing playback speed would apply the ratio twice.
    if (syncMode == 0 || (!needsPitch && !needsStretch) || safeProjectTempo <= 0.0 ||
        !AudioStretchBackend::isElastiqueAvailable()) {
        return source;
    }
    if (syncMode == 1 && detectedSourceTempo <= 0.0) {
        return source;
    }
    const double safeSourceTempo = detectedSourceTempo > 0.0 ? detectedSourceTempo : safeProjectTempo;

    double targetTempo = safeSourceTempo;
    double tempoRatioOverride = 0.0;
    if (syncMode == 1) {
        targetTempo = safeProjectTempo;
    } else if (syncMode == 2 || syncMode == 3) {
        const double safeBeatsPerBar = std::isfinite(beatsPerBar) && beatsPerBar > 0.0 ? qBound(0.25, beatsPerBar, 64.0) : 4.0;
        const double targetBeats = syncMode == 2 && targetBars > 0.0
            ? qBound(0.0, targetBars, 512.0) * safeBeatsPerBar
            : (std::isfinite(lengthBeats) && lengthBeats > 0.0 ? lengthBeats : 0.0);
        const double naturalSeconds = static_cast<double>(source->frameCount()) / static_cast<double>(source->sampleRate);
        const double targetSeconds = targetBeats > 0.0 ? targetBeats * 60.0 / safeProjectTempo : 0.0;
        tempoRatioOverride = targetSeconds > 0.0 ? naturalSeconds / targetSeconds : 1.0;
    }
    if (!std::isfinite(targetTempo) || targetTempo <= 0.0 ||
        !std::isfinite(tempoRatioOverride) || tempoRatioOverride < 0.0) {
        return source;
    }
    const double effectiveRatio = tempoRatioOverride > 0.0
        ? tempoRatioOverride
        : AudioStretchBackend::tempoRatio(safeSourceTempo, targetTempo);
    if (!std::isfinite(effectiveRatio) || effectiveRatio <= 0.0) {
        return source;
    }

    const QFileInfo info(path);
    const QString key = QString("%1|%2|%3|%4|%5|%6|%7")
        .arg(info.absoluteFilePath())
        .arg(source->frameCount())
        .arg(safeSourceTempo, 0, 'f', 4)
        .arg(effectiveRatio, 0, 'f', 8)
        .arg(safePitch, 0, 'f', 4)
        .arg(qBound(8000, sampleRate, 384000))
        .arg(syncMode);
    const qint64 size = info.size();
    const QDateTime modified = info.lastModified();
    auto cached = m_audioSyncDecodeCache.find(key);
    if (cached != m_audioSyncDecodeCache.end() &&
        cached->second.fileSize == size &&
        cached->second.lastModified == modified) {
        cached->second.lastUse = ++m_audioCacheUseCounter;
        if (processed) {
            *processed = true;
        }
        return &cached->second.audio;
    }

    QTemporaryFile temp(QDir::tempPath() + "/VeraCoreSyncXXXXXX.wav");
    temp.setAutoRemove(true);
    if (!temp.open()) {
        return source;
    }
    const QString tempPath = temp.fileName();
    temp.close();
    QFile::remove(tempPath);

    AudioStretchRequest request;
    request.sourcePath = path;
    request.destinationPath = tempPath;
    request.sourceTempoBpm = safeSourceTempo;
    request.targetTempoBpm = targetTempo;
    request.tempoRatioOverride = tempoRatioOverride;
    request.pitchShiftSemitones = safePitch;
    request.sampleRate = qBound(8000, sampleRate, 384000);
    const AudioStretchResult stretch = AudioStretchBackend::processForProjectSync(request);
    if (!stretch.success) {
        QFile::remove(tempPath);
        return source;
    }

    AudioDecodeResult decoded = AudioFileDecoder::decodeAudio(tempPath);
    QFile::remove(tempPath);
    if (!decoded.success || decoded.audio.sampleRate <= 0 || decoded.audio.stereoSamples.size() < 2) {
        return source;
    }
    decoded.audio.sourceChannels = source->sourceChannels;

    CachedDecodedAudio entry;
    entry.fileSize = size;
    entry.lastModified = modified;
    entry.audio = std::move(decoded.audio);
    entry.normalizationGain = peakNormalizationGain(entry.audio);
    entry.lastUse = ++m_audioCacheUseCounter;
    auto inserted = m_audioSyncDecodeCache.insert_or_assign(key, std::move(entry));
    trimDecodedAudioCache(m_audioSyncDecodeCache, kMaxSyncCacheEntries, kMaxSyncCacheSamples);
    if (processed) {
        *processed = true;
    }
    return &inserted.first->second.audio;
}

void VeraCoreQT::syncRealtimeMidiLayersToAudio()
{
    if (!audioGeneral) {
        refreshInspectorPluginHostStates(false);
        return;
    }
    if (m_pluginHost) {
        m_pluginHost->syncLayers(m_tracks, m_mixerParameters->returnBuses(),
                                 m_mixerParameters->masterBus(),
                                 audioGeneral->audioCore.GetStreamSampleRate(),
								 m_projectSpeakerLayout);
    }
    refreshInspectorPluginHostStates(m_pluginHost != nullptr);

    std::vector<int> midiTrackIds;
    midiTrackIds.reserve(m_tracks.size());
    for (int trackIndex = 0; trackIndex < static_cast<int>(m_tracks.size()); ++trackIndex) {
        if (m_tracks[trackIndex] && m_tracks[trackIndex]->GetType() == TrackType::Midi)
            midiTrackIds.push_back(trackIndex);
    }
    audioGeneral->audioCore.SetMidiTrackIds(midiTrackIds);

    std::vector<VeraCoreMidiLayerRoute> routes;
    if (m_selectedTrackIndex >= 0 && m_selectedTrackIndex < static_cast<int>(m_tracks.size())) {
        const Track* selectedTrack = m_tracks[m_selectedTrackIndex].get();
        bool anySolo = false;
        for (const auto& track : m_tracks) {
            if (track && track->IsSoloed()) {
                anySolo = true;
                break;
            }
        }

        const MidiTrack* midiTrack = dynamic_cast<const MidiTrack*>(selectedTrack);
        if (midiTrack && selectedTrack && !selectedTrack->IsMuted() && (!anySolo || selectedTrack->IsSoloed())) {
            const std::vector<LayerSlot>& layers = midiTrack->GetLayerSlots();
            routes.reserve(layers.empty() ? 1 : layers.size());
            if (layers.empty()) {
                VeraCoreMidiLayerRoute route;
                route.layerId = m_selectedTrackIndex * kLayerIdStride;
                route.enabled = true;
                route.gain = 1.0f;
                route.pan = 0.0f;
                routes.push_back(route);
            }
            for (int layerIndex = 0; layerIndex < static_cast<int>(layers.size()); ++layerIndex) {
                const LayerSlot& layer = layers[layerIndex];
                VeraCoreMidiLayerRoute route;
                route.layerId = m_selectedTrackIndex * kLayerIdStride + layerIndex;
                route.enabled = layer.enabled;
                route.transpose = layer.transpose;
                route.midiChannel = layer.midiChannel;
                route.externalPlugin = m_pluginHost && m_pluginHost->isLayerHosted(route.layerId);
                route.gain = static_cast<float>(finiteClamp(
                    static_cast<double>(layer.gain), 0.0, 2.0, 1.0));
                route.pan = static_cast<float>(finiteClamp(
                    static_cast<double>(layer.pan), -1.0, 1.0, 0.0));
                routes.push_back(route);
            }
        }
    }
    audioGeneral->audioCore.SetRealtimeMidiLayerRoutes(routes);
    refreshMidiEditorExpressionMap();
    restoreProjectAraArchives();
}

void VeraCoreQT::quiesceExternalAudioForDeviceChange()
{
    if (!audioGeneral) return;
    finishAutomationWrite();
    audioGeneral->audioCore.SetExternalMidiRenderer(nullptr, nullptr);
    audioGeneral->audioCore.SetExternalAudioFxRenderer(nullptr, nullptr);
	audioGeneral->audioCore.SetExternalRenderBatchCallbacks(nullptr, nullptr, nullptr);
    captureLivePluginStatesToProject();
}

void VeraCoreQT::captureLivePluginStatesToProject()
{
    if (!m_pluginHost) return;
    for (int trackIndex = 0; trackIndex < static_cast<int>(m_tracks.size()); ++trackIndex) {
        const std::shared_ptr<Track>& track = m_tracks[static_cast<size_t>(trackIndex)];
        if (!track) continue;
        const auto& effects = track->GetAudioFxSlots();
        for (int slotIndex = 0; slotIndex < static_cast<int>(effects.size()); ++slotIndex) {
            const QByteArray state = m_pluginHost->captureAudioFxState(trackIndex, slotIndex);
            if (!state.isEmpty()) track->SetAudioFxPluginState(slotIndex, state);
        }
        auto* midiTrack = dynamic_cast<MidiTrack*>(track.get());
        if (!midiTrack) continue;
        const auto& layers = midiTrack->GetLayerSlots();
        for (int layerIndex = 0; layerIndex < static_cast<int>(layers.size()); ++layerIndex) {
            const QByteArray state = m_pluginHost->captureLayerState(
                trackIndex * kLayerIdStride + layerIndex);
            if (!state.isEmpty()) midiTrack->SetLayerSlotPluginState(layerIndex, state);
        }
    }
	for (int returnBus = 0; returnBus < kReturnBusCount; ++returnBus) {
		ReturnBusState state = m_mixerParameters->returnBuses()[static_cast<std::size_t>(returnBus)];
		for (int slotIndex = 0; slotIndex < static_cast<int>(state.audioFxSlots.size()); ++slotIndex) {
			const QByteArray pluginState = m_pluginHost->captureAudioFxState(
				VeraCoreReturnAudioFxTrackId(returnBus), slotIndex);
			if (!pluginState.isEmpty())
				state.audioFxSlots[static_cast<std::size_t>(slotIndex)].pluginState = pluginState;
		}
		m_mixerParameters->restoreReturnBus(returnBus, state);
	}
	MasterBusState masterState = m_mixerParameters->masterBus();
	for (int slotIndex = 0;
		 slotIndex < static_cast<int>(masterState.audioFxSlots.size()); ++slotIndex) {
		const QByteArray pluginState = m_pluginHost->captureAudioFxState(
			VeraCoreMasterAudioFxTrackId, slotIndex);
		if (!pluginState.isEmpty())
			masterState.audioFxSlots[static_cast<std::size_t>(slotIndex)].pluginState = pluginState;
	}
	m_mixerParameters->restoreMasterBus(masterState);
}

void VeraCoreQT::resumeAudioAfterDeviceChange(bool streamActive)
{
    if (!audioGeneral) return;
    if (streamActive) {
        syncRealtimeMidiLayersToAudio();
        syncMixerRoutingToAudio();
        syncAudioInputState();
        syncPluginTransportToHost();
        syncTempoMapToAudio();
        syncTimeSignatureMapToAudio();
        syncMidiTimelineToAudio();
        syncAudioTimelineToAudio();
    }
    if (m_pluginHost) {
        audioGeneral->audioCore.SetExternalMidiRenderer(
            &ExternalPluginHost::audioCallback, m_pluginHost.get());
        audioGeneral->audioCore.SetExternalAudioFxRenderer(
            &ExternalPluginHost::audioFxCallback, m_pluginHost.get());
		audioGeneral->audioCore.SetExternalRenderBatchCallbacks(
			&ExternalPluginHost::beginRenderBatchCallback,
			&ExternalPluginHost::endRenderBatchCallback, m_pluginHost.get());
    }
    const AudioDeviceStatus status = audioGeneral->GetAudioDeviceStatus();
    if (m_transportWidget)
        m_transportWidget->setAudioDeviceStatus(status.active, status.message);
}

void VeraCoreQT::restoreProjectAraArchives()
{
    if (m_projectAraArchivesRestored || m_projectAraArchives.empty() ||
        m_restoringProject || !m_pluginHost) return;
    try {
        std::vector<AraDocumentArchive> archives;
        archives.reserve(m_projectAraArchives.size());
        for (const StoredAraArchive& stored : m_projectAraArchives) {
            AraDocumentArchive archive;
            archive.factoryPersistentId.assign(stored.factoryPersistentIdUtf8.constData(),
                static_cast<size_t>(stored.factoryPersistentIdUtf8.size()));
            archive.documentArchiveId.assign(stored.documentArchiveIdUtf8.constData(),
                static_cast<size_t>(stored.documentArchiveIdUtf8.size()));
            archive.bytes.resize(static_cast<size_t>(stored.bytes.size()));
            if (!stored.bytes.isEmpty())
                std::memcpy(archive.bytes.data(), stored.bytes.constData(),
                            static_cast<size_t>(stored.bytes.size()));
            archives.push_back(std::move(archive));
        }
        if (m_pluginHost->restoreAraArchives(archives)) {
            m_projectAraArchivesRestored = true;
            ui.statusBar->showMessage("ARA document state restored.", 3000);
        }
    } catch (...) {}
}

void VeraCoreQT::refreshInspectorPluginHostStates(bool hostEvaluated)
{
    if (!m_inspectorWidget) return;
    if (m_parameterEditorTrackIndex >= 0 &&
        m_parameterEditorTrackIndex != m_selectedTrackIndex) {
        finishAutomationWrite();
        m_parameterEditorTrackIndex = -1;
        m_parameterEditorReturnBus = -1;
        m_parameterEditorSlotIndex = -1;
        m_parameterEditorGestureDepth = 0;
        m_parameterEditorDeviceName.clear();
        m_hasAutomationTarget = false;
        m_automationParameterId = 0;
        m_automationParameterName.clear();
        m_automationWriteActive = false;
        m_automationWriteOwnsHostGesture = false;
        m_automationWriteTrackIndex = -1;
        m_automationWriteAudioFx = false;
        m_automationWriteSlotIndex = -1;
        m_automationWriteInstanceEpoch = 0;
        m_automationWriteParameterId = 0;
        m_automationWriteParameterName.clear();
        m_automationWriteRanges.clear();
        m_automationWriteLastBeat = -1.0;
        m_automationWriteRangeOverflow = false;
        m_automationWriteBufferOverflow = false;
        m_automationWriteStreamOverflow = false;
        m_automationWriteBuffer.clear();
        m_inspectorWidget->clearActiveAutomationTarget();
        if (m_playlistView) m_playlistView->clearPluginAutomationLane();
        m_inspectorWidget->clearPluginParameters();
    }
    if (m_parameterEditorTrackIndex == m_selectedTrackIndex &&
        m_parameterEditorTrackIndex >= 0 &&
        m_selectedTrackIndex < static_cast<int>(m_tracks.size()) &&
        m_tracks[m_selectedTrackIndex]) {
        bool targetValid = false;
        if (m_parameterEditorAudioFx) {
            const auto& deviceSlots = m_tracks[m_selectedTrackIndex]->GetAudioFxSlots();
            targetValid = m_parameterEditorSlotIndex >= 0 &&
                m_parameterEditorSlotIndex < static_cast<int>(deviceSlots.size()) &&
                deviceSlots[static_cast<size_t>(m_parameterEditorSlotIndex)].name ==
                    m_parameterEditorDeviceName;
        } else if (const auto* midi = dynamic_cast<const MidiTrack*>(
                       m_tracks[m_selectedTrackIndex].get())) {
            const auto& deviceSlots = midi->GetLayerSlots();
            targetValid = m_parameterEditorSlotIndex >= 0 &&
                m_parameterEditorSlotIndex < static_cast<int>(deviceSlots.size()) &&
                deviceSlots[static_cast<size_t>(m_parameterEditorSlotIndex)].name ==
                    m_parameterEditorDeviceName;
        }
        if (!targetValid) {
            finishAutomationWrite();
            m_parameterEditorTrackIndex = -1;
            m_parameterEditorReturnBus = -1;
            m_parameterEditorSlotIndex = -1;
            m_parameterEditorGestureDepth = 0;
            m_parameterEditorDeviceName.clear();
            m_hasAutomationTarget = false;
            m_automationParameterId = 0;
            m_automationParameterName.clear();
            m_automationWriteActive = false;
            m_automationWriteOwnsHostGesture = false;
            m_automationWriteTrackIndex = -1;
            m_automationWriteAudioFx = false;
            m_automationWriteSlotIndex = -1;
            m_automationWriteInstanceEpoch = 0;
            m_automationWriteParameterId = 0;
            m_automationWriteParameterName.clear();
            m_automationWriteRanges.clear();
            m_automationWriteLastBeat = -1.0;
            m_automationWriteRangeOverflow = false;
            m_automationWriteBufferOverflow = false;
            m_automationWriteStreamOverflow = false;
            m_automationWriteBuffer.clear();
            m_inspectorWidget->clearActiveAutomationTarget();
            if (m_playlistView) m_playlistView->clearPluginAutomationLane();
            m_inspectorWidget->clearPluginParameters();
        }
    }
    if (m_parameterEditorReturnBus >= 0 &&
        m_parameterEditorReturnBus < kReturnBusCount) {
        const auto& returnSlots = m_mixerParameters->returnBuses()[
            static_cast<std::size_t>(m_parameterEditorReturnBus)].audioFxSlots;
        const bool targetValid = m_parameterEditorAudioFx &&
            m_parameterEditorTrackIndex ==
                VeraCoreReturnAudioFxTrackId(m_parameterEditorReturnBus) &&
            m_parameterEditorSlotIndex >= 0 &&
            m_parameterEditorSlotIndex < static_cast<int>(returnSlots.size()) &&
            returnSlots[static_cast<std::size_t>(m_parameterEditorSlotIndex)].name ==
                m_parameterEditorDeviceName;
        if (!targetValid) {
            finishAutomationWrite();
            m_parameterEditorTrackIndex = -1;
            m_parameterEditorReturnBus = -1;
            m_parameterEditorSlotIndex = -1;
            m_parameterEditorGestureDepth = 0;
            m_parameterEditorDeviceName.clear();
            m_hasAutomationTarget = false;
            m_automationParameterId = 0;
            m_automationParameterName.clear();
            m_inspectorWidget->clearActiveAutomationTarget();
            if (m_playlistView) m_playlistView->clearPluginAutomationLane();
            m_inspectorWidget->clearPluginParameters();
        }
    }

    auto statusFor = [hostEvaluated](const QString& type, const QString& path,
                                     bool loaded, bool faulted) {
        if (type.compare("Internal", Qt::CaseInsensitive) == 0) {
            return InspectorWidget::PluginHostStatus::Internal;
        }
        const bool supported = type.compare("VST2", Qt::CaseInsensitive) == 0 ||
            type.compare("VST3", Qt::CaseInsensitive) == 0 ||
            type.compare("CLAP", Qt::CaseInsensitive) == 0;
        if (!supported || path.trimmed().isEmpty()) {
            return InspectorWidget::PluginHostStatus::Unsupported;
        }
        if (!hostEvaluated) return InspectorWidget::PluginHostStatus::Pending;
        if (faulted) return InspectorWidget::PluginHostStatus::Faulted;
        return loaded ? InspectorWidget::PluginHostStatus::Active
                      : InspectorWidget::PluginHostStatus::Failed;
    };

    std::array<std::vector<InspectorWidget::PluginHostStatus>,
               kReturnBusCount> returnFxStates{};
    for (int returnBus = 0; returnBus < kReturnBusCount; ++returnBus) {
        const auto& effects = m_mixerParameters->returnBuses()[
            static_cast<std::size_t>(returnBus)].audioFxSlots;
        auto& states = returnFxStates[static_cast<std::size_t>(returnBus)];
        states.reserve(effects.size());
        const int hostTrackId = VeraCoreReturnAudioFxTrackId(returnBus);
        for (int slotIndex = 0; slotIndex < static_cast<int>(effects.size()); ++slotIndex) {
            const AudioFxSlot& slot = effects[static_cast<std::size_t>(slotIndex)];
            const bool loaded = hostEvaluated && m_pluginHost &&
                m_pluginHost->isAudioFxLoaded(hostTrackId, slotIndex);
            const bool faulted = hostEvaluated && m_pluginHost &&
                m_pluginHost->isAudioFxFaulted(hostTrackId, slotIndex);
            states.push_back(statusFor(
                slot.pluginType, slot.pluginPath, loaded, faulted));
        }
    }
    m_inspectorWidget->setReturnPluginHostStates(std::move(returnFxStates));

    std::vector<InspectorWidget::PluginHostStatus> layerStates;
    std::vector<InspectorWidget::PluginHostStatus> audioFxStates;
    if (m_selectedTrackIndex < 0 || m_selectedTrackIndex >= static_cast<int>(m_tracks.size()) ||
        !m_tracks[m_selectedTrackIndex]) {
        m_inspectorWidget->setPluginHostStates(std::move(layerStates), std::move(audioFxStates));
        return;
    }

    const Track* track = m_tracks[m_selectedTrackIndex].get();
    const auto& effects = track->GetAudioFxSlots();
    audioFxStates.reserve(effects.size());
    for (int slotIndex = 0; slotIndex < static_cast<int>(effects.size()); ++slotIndex) {
        const AudioFxSlot& slot = effects[static_cast<size_t>(slotIndex)];
        const bool loaded = hostEvaluated && m_pluginHost &&
            m_pluginHost->isAudioFxLoaded(m_selectedTrackIndex, slotIndex);
        const bool faulted = hostEvaluated && m_pluginHost &&
            m_pluginHost->isAudioFxFaulted(m_selectedTrackIndex, slotIndex);
        audioFxStates.push_back(statusFor(slot.pluginType, slot.pluginPath, loaded, faulted));
    }

    if (const auto* midiTrack = dynamic_cast<const MidiTrack*>(track)) {
        const auto& layers = midiTrack->GetLayerSlots();
        layerStates.reserve(layers.size());
        for (int layerIndex = 0; layerIndex < static_cast<int>(layers.size()); ++layerIndex) {
            const LayerSlot& layer = layers[static_cast<size_t>(layerIndex)];
            const int layerId = m_selectedTrackIndex * kLayerIdStride + layerIndex;
            const bool loaded = hostEvaluated && m_pluginHost && m_pluginHost->isLayerLoaded(layerId);
            const bool faulted = hostEvaluated && m_pluginHost &&
                m_pluginHost->isLayerFaulted(layerId);
            layerStates.push_back(statusFor(layer.pluginType, layer.pluginPath, loaded, faulted));
        }
    }
    m_inspectorWidget->setPluginHostStates(std::move(layerStates), std::move(audioFxStates));
}

void VeraCoreQT::refreshInspectorPluginParameters()
{
    const bool returnEditor = m_parameterEditorReturnBus >= 0 &&
        m_parameterEditorReturnBus < kReturnBusCount &&
        m_parameterEditorTrackIndex ==
            VeraCoreReturnAudioFxTrackId(m_parameterEditorReturnBus);
    const bool trackEditor = m_parameterEditorReturnBus < 0 &&
        m_parameterEditorTrackIndex >= 0 &&
        m_parameterEditorTrackIndex == m_selectedTrackIndex &&
        m_selectedTrackIndex < static_cast<int>(m_tracks.size()) &&
        m_tracks[static_cast<std::size_t>(m_selectedTrackIndex)];
    if (!m_inspectorWidget || !m_pluginHost || (!returnEditor && !trackEditor) ||
        m_parameterEditorSlotIndex < 0) {
        if (m_inspectorWidget) m_inspectorWidget->clearPluginParameters();
        return;
    }
    std::vector<PluginParameterInfo> parameters;
    if (m_parameterEditorAudioFx) {
        const auto& deviceSlots = returnEditor
            ? m_mixerParameters->returnBuses()[static_cast<std::size_t>(
                m_parameterEditorReturnBus)].audioFxSlots
            : m_tracks[static_cast<std::size_t>(m_selectedTrackIndex)]->GetAudioFxSlots();
        if (m_parameterEditorSlotIndex >= static_cast<int>(deviceSlots.size())) {
            m_inspectorWidget->clearPluginParameters();
            return;
        }
        parameters = m_pluginHost->parametersForAudioFx(
            m_parameterEditorTrackIndex, m_parameterEditorSlotIndex);
    } else {
        const auto* midi = dynamic_cast<const MidiTrack*>(
            m_tracks[static_cast<std::size_t>(m_selectedTrackIndex)].get());
        if (!midi || m_parameterEditorSlotIndex >= static_cast<int>(midi->GetLayerSlots().size())) {
            m_inspectorWidget->clearPluginParameters();
            return;
        }
        parameters = m_pluginHost->parametersForLayer(
            m_selectedTrackIndex * kLayerIdStride + m_parameterEditorSlotIndex);
    }
    m_inspectorWidget->setPluginParameters(
        m_parameterEditorDeviceName, m_parameterEditorAudioFx,
        m_parameterEditorSlotIndex, std::move(parameters), true);
}

void VeraCoreQT::refreshPluginAutomationLane()
{
    const int returnBus = m_parameterEditorAudioFx
        ? returnBusFromAudioFxTrackId(m_parameterEditorTrackIndex) : -1;
    const bool returnAutomationLane = returnBus >= 0;
    const bool targetValid = validPluginParameterTrackId(
        m_parameterEditorTrackIndex, m_parameterEditorAudioFx, m_tracks.size());
    const bool targetObjectValid = m_parameterEditorTrackIndex < 0 ||
        (targetValid && m_tracks[static_cast<std::size_t>(m_parameterEditorTrackIndex)]);
    const int displayTrackIndex = m_selectedTrackIndex >= 0 &&
        m_selectedTrackIndex < static_cast<int>(m_tracks.size())
        ? m_selectedTrackIndex : (!m_tracks.empty() ? 0 : -1);
    if (!m_playlistView || !m_hasAutomationTarget || !targetValid || !targetObjectValid ||
        displayTrackIndex < 0 || m_parameterEditorSlotIndex < 0) {
        if (m_playlistView) m_playlistView->clearPluginAutomationLane();
        return;
    }
    const std::vector<PluginAutomationLane>* lanes = nullptr;
    const Track* track = m_parameterEditorTrackIndex >= 0
        ? m_tracks[static_cast<std::size_t>(m_parameterEditorTrackIndex)].get() : nullptr;
    if (returnAutomationLane) {
        const auto& deviceSlots = m_mixerParameters->returnBuses()[
            static_cast<std::size_t>(returnBus)].audioFxSlots;
        if (m_parameterEditorSlotIndex < static_cast<int>(deviceSlots.size()))
            lanes = &deviceSlots[static_cast<std::size_t>(m_parameterEditorSlotIndex)].automation;
    } else if (m_parameterEditorAudioFx) {
        const auto& deviceSlots = track->GetAudioFxSlots();
        if (m_parameterEditorSlotIndex < static_cast<int>(deviceSlots.size()))
            lanes = &deviceSlots[static_cast<size_t>(m_parameterEditorSlotIndex)].automation;
    } else if (const auto* midi = dynamic_cast<const MidiTrack*>(track)) {
        const auto& deviceSlots = midi->GetLayerSlots();
        if (m_parameterEditorSlotIndex < static_cast<int>(deviceSlots.size()))
            lanes = &deviceSlots[static_cast<size_t>(m_parameterEditorSlotIndex)].automation;
    }
    if (!lanes) {
        m_playlistView->clearPluginAutomationLane();
        return;
    }
    PluginAutomationLane lane;
    lane.parameterId = m_automationParameterId;
    lane.parameterName = m_automationParameterName;
    const auto found = std::find_if(lanes->begin(), lanes->end(), [this](const auto& candidate) {
        return candidate.parameterId == m_automationParameterId;
    });
    if (found != lanes->end()) lane = *found;
    if (lane.parameterName.isEmpty()) lane.parameterName = m_automationParameterName;
    if (returnAutomationLane) {
        const QString prefix = QString("Return %1 · ").arg(QChar('A' + returnBus));
        if (!lane.parameterName.startsWith(prefix)) lane.parameterName.prepend(prefix);
    }
    m_playlistView->setPluginAutomationLane(
        displayTrackIndex, m_parameterEditorTrackIndex, m_parameterEditorAudioFx,
        m_parameterEditorSlotIndex, lane);
}

void VeraCoreQT::applyPluginAutomationLane(int trackIndex, bool audioFx, int slotIndex,
                                           const PluginAutomationLane& lane,
                                           const QString& mergeKey)
{
    const int returnBus = audioFx ? returnBusFromAudioFxTrackId(trackIndex) : -1;
    const bool returnAutomationLane = returnBus >= 0;
    if (!validPluginParameterTrackId(trackIndex, audioFx, m_tracks.size()) ||
        slotIndex < 0) return;
    const std::shared_ptr<Track> trackRef = trackIndex >= 0
        ? m_tracks[static_cast<std::size_t>(trackIndex)] : nullptr;
    if (trackIndex >= 0 && !trackRef) return;
    std::vector<PluginAutomationLane> before;
    if (returnAutomationLane) {
        const auto& deviceSlots = m_mixerParameters->returnBuses()[
            static_cast<std::size_t>(returnBus)].audioFxSlots;
        if (slotIndex >= static_cast<int>(deviceSlots.size())) return;
        before = deviceSlots[static_cast<std::size_t>(slotIndex)].automation;
    } else if (audioFx) {
        const auto& deviceSlots = trackRef->GetAudioFxSlots();
        if (slotIndex >= static_cast<int>(deviceSlots.size())) return;
        before = deviceSlots[static_cast<size_t>(slotIndex)].automation;
    } else {
        const auto* midi = dynamic_cast<const MidiTrack*>(trackRef.get());
        if (!midi || slotIndex >= static_cast<int>(midi->GetLayerSlots().size())) return;
        before = midi->GetLayerSlots()[static_cast<size_t>(slotIndex)].automation;
    }
    std::vector<PluginAutomationLane> after = before;
    const auto found = std::find_if(after.begin(), after.end(), [&](const auto& candidate) {
        return candidate.parameterId == lane.parameterId;
    });
    if (found == after.end()) {
        if (after.size() >= kMaxPluginAutomationLanes) return;
        after.push_back(lane);
    } else {
        *found = lane;
    }
    const auto apply = [this, trackRef, audioFx, slotIndex, returnBus,
                        returnAutomationLane](
                           const std::vector<PluginAutomationLane>& automation) {
        if (returnAutomationLane) {
            ReturnBusState state = m_mixerParameters->returnBuses()[
                static_cast<std::size_t>(returnBus)];
            if (slotIndex < 0 || slotIndex >= static_cast<int>(state.audioFxSlots.size()))
                return;
            state.audioFxSlots[static_cast<std::size_t>(slotIndex)].automation = automation;
            m_mixerParameters->setReturnBus(returnBus, state);
        } else if (audioFx) {
            trackRef->SetAudioFxAutomation(slotIndex, automation);
        } else if (auto* midi = dynamic_cast<MidiTrack*>(trackRef.get())) {
            midi->SetLayerSlotAutomation(slotIndex, automation);
        }
        syncRealtimeMidiLayersToAudio();
        refreshPluginAutomationLane();
    };
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    apply(after);
    if (captureUndo) {
        const auto beforeCopy = std::make_shared<std::vector<PluginAutomationLane>>(std::move(before));
        const auto afterCopy = std::make_shared<std::vector<PluginAutomationLane>>(std::move(after));
        if (!commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, mergeKey,
                [apply, beforeCopy]() { apply(*beforeCopy); },
                [apply, afterCopy]() { apply(*afterCopy); }))
            setProjectDirty();
    } else {
        setProjectDirty();
    }
}

void VeraCoreQT::armAutomationWriteTarget(int trackIndex, bool audioFx, int slotIndex,
                                          uint32_t parameterId,
                                          const QString& parameterName,
                                          uint64_t instanceEpoch)
{
    m_automationWriteTrackIndex = trackIndex;
    m_automationWriteAudioFx = audioFx;
    m_automationWriteSlotIndex = slotIndex;
    m_automationWriteParameterId = parameterId;
    m_automationWriteParameterName = parameterName.trimmed();
    if (instanceEpoch == 0 && m_pluginHost &&
        validPluginParameterTrackId(trackIndex, audioFx, m_tracks.size()) && slotIndex >= 0) {
        instanceEpoch = audioFx
            ? m_pluginHost->instanceEpochForAudioFx(trackIndex, slotIndex)
            : m_pluginHost->instanceEpochForLayer(trackIndex * kLayerIdStride + slotIndex);
    }
    m_automationWriteInstanceEpoch = instanceEpoch;
}

bool VeraCoreQT::acceptNativeAutomationGestureSerial(
    int trackIndex, bool audioFx, int slotIndex, uint64_t instanceEpoch,
    uint32_t parameterId, uint64_t gestureSerial, bool terminal)
{
    if (!validPluginParameterTrackId(trackIndex, audioFx, m_tracks.size()) ||
        slotIndex < 0 || instanceEpoch == 0 || gestureSerial == 0)
        return true;
    constexpr size_t kMaxNativeAutomationGestureHighWater = 4096;
    const NativeAutomationGestureKey key{
        trackIndex, audioFx, slotIndex, instanceEpoch, parameterId};
    auto found = m_nativeAutomationGestureHighWater.find(key);
    ++m_nativeAutomationGestureSeenCounter;
    if (found != m_nativeAutomationGestureHighWater.end()) {
        if (gestureSerial < found->second.serial ||
            (gestureSerial == found->second.serial && found->second.closed))
            return false;
        found->second.lastSeen = m_nativeAutomationGestureSeenCounter;
        if (gestureSerial > found->second.serial) {
            found->second.serial = gestureSerial;
            found->second.closed = terminal;
        } else if (terminal) {
            found->second.closed = true;
        }
        return true;
    }
    if (m_nativeAutomationGestureHighWater.size() >=
        kMaxNativeAutomationGestureHighWater) {
        auto oldest = m_nativeAutomationGestureHighWater.end();
        for (auto candidate = m_nativeAutomationGestureHighWater.begin();
             candidate != m_nativeAutomationGestureHighWater.end(); ++candidate) {
            const NativeAutomationGestureKey& candidateKey = candidate->first;
            const bool pinned = m_automationWriteActive &&
                !m_automationWriteOwnsHostGesture &&
                candidateKey.trackIndex == m_automationWriteTrackIndex &&
                candidateKey.audioFx == m_automationWriteAudioFx &&
                candidateKey.slotIndex == m_automationWriteSlotIndex &&
                candidateKey.instanceEpoch == m_automationWriteInstanceEpoch &&
                candidateKey.parameterId == m_automationWriteParameterId;
            if (!pinned && (oldest == m_nativeAutomationGestureHighWater.end() ||
                candidate->second.lastSeen < oldest->second.lastSeen))
                oldest = candidate;
        }
        if (oldest != m_nativeAutomationGestureHighWater.end())
            m_nativeAutomationGestureHighWater.erase(oldest);
    }
    m_nativeAutomationGestureHighWater.emplace(
        key, NativeAutomationGestureState{
            gestureSerial, m_nativeAutomationGestureSeenCounter, terminal});
    return true;
}

void VeraCoreQT::beginAutomationWriteAtCurrentValue()
{
    if (!m_isPlaying || m_automationWriteActive || !m_pluginHost ||
        !m_hasAutomationTarget ||
        m_automationWriteMode != InspectorWidget::AutomationWriteMode::Write ||
        !validPluginParameterTrackId(m_parameterEditorTrackIndex,
            m_parameterEditorAudioFx, m_tracks.size()) ||
        m_parameterEditorSlotIndex < 0) return;
    armAutomationWriteTarget(m_parameterEditorTrackIndex, m_parameterEditorAudioFx,
                             m_parameterEditorSlotIndex, m_automationParameterId,
                             m_automationParameterName);
    std::vector<PluginParameterInfo> parameters = m_parameterEditorAudioFx
        ? m_pluginHost->parametersForAudioFx(
            m_parameterEditorTrackIndex, m_parameterEditorSlotIndex)
        : m_pluginHost->parametersForLayer(
            m_parameterEditorTrackIndex * kLayerIdStride + m_parameterEditorSlotIndex);
    const auto parameter = std::find_if(parameters.begin(), parameters.end(), [this](const auto& item) {
        return item.stableId == m_automationParameterId && !item.readOnly && item.automatable;
    });
    const bool began = parameter != parameters.end() && (m_parameterEditorAudioFx
        ? m_pluginHost->beginAudioFxParameterGesture(
            m_parameterEditorTrackIndex, m_parameterEditorSlotIndex,
            m_automationParameterId)
        : m_pluginHost->beginLayerParameterGesture(
            m_parameterEditorTrackIndex * kLayerIdStride + m_parameterEditorSlotIndex,
            m_automationParameterId));
    if (!began) {
        finishAutomationWrite();
        return;
    }
    m_automationWriteOwnsHostGesture = true;
    ++m_parameterEditorGestureDepth;
    ++m_automationWriteGestureSerial;
    writePluginAutomationPoint(m_automationParameterId, m_automationParameterName,
                               m_playheadBeat, parameter->normalizedValue);
}

void VeraCoreQT::writePluginAutomationPoint(uint32_t parameterId,
                                            const QString& parameterName,
                                            double beat, double normalizedValue, bool force,
                                            double curveTension,
                                            const PluginAutomationCurvePolynomial* curvePolynomial)
{
    if (parameterId != m_automationWriteParameterId ||
        !std::isfinite(beat) || !std::isfinite(normalizedValue) || beat < 0.0 ||
        !validPluginParameterTrackId(m_automationWriteTrackIndex,
            m_automationWriteAudioFx, m_tracks.size())) return;
    beat = qMax(0.0, beat);
    normalizedValue = std::clamp(normalizedValue, 0.0, 1.0);
    curveTension = std::isfinite(curveTension)
        ? std::clamp(curveTension, -1.0, 1.0) : 0.0;
    PluginAutomationPoint automationPoint{beat, normalizedValue, curveTension};
    if (curvePolynomial && pluginAutomationCurvePolynomialValid(*curvePolynomial)) {
        automationPoint.curvePolynomial =
            std::make_shared<const PluginAutomationCurvePolynomial>(*curvePolynomial);
    }
    if (!m_automationWriteActive) {
        m_automationWriteActive = true;
        m_automationWriteStartBeat = beat;
        m_automationWriteEndBeat = beat;
        m_automationWriteRanges.clear();
        m_automationWriteLastBeat = -1.0;
        m_automationWriteRangeOverflow = false;
        m_automationWriteBufferOverflow = false;
        m_automationWriteStreamOverflow = false;
        m_automationWriteBuffer.clear();
    }
    extendAutomationWriteRange(beat);
    m_automationWriteParameterName = parameterName.trimmed();
    m_automationWriteValue = normalizedValue;
    constexpr double kValueEpsilon = 1.0e-5;
    const double beatEpsilon = 1.0 / static_cast<double>(qMax(24, m_projectPpq));
    const double previousBeatDistance = m_automationWriteBuffer.empty()
        ? std::numeric_limits<double>::max()
        : std::fabs(m_automationWriteBuffer.back().beat - beat);
    if (!m_automationWriteBuffer.empty() &&
        (previousBeatDistance <= 1.0e-12 ||
         (!force && previousBeatDistance <= beatEpsilon))) {
        m_automationWriteBuffer.back() = automationPoint;
    } else if (force || m_automationWriteBuffer.empty() ||
               std::fabs(m_automationWriteBuffer.back().normalizedValue - normalizedValue) >
                   kValueEpsilon) {
        if (m_automationWriteBuffer.size() < kMaxPluginAutomationPointsPerLane)
            m_automationWriteBuffer.push_back(automationPoint);
        else
            m_automationWriteBufferOverflow = true;
    }
}

void VeraCoreQT::addAutomationWriteRange(double startBeat, double endBeat)
{
    if (!std::isfinite(startBeat) || !std::isfinite(endBeat) ||
        startBeat < 0.0 || endBeat < 0.0) return;
    constexpr size_t kMaxAutomationWriteRanges = 8;
    AutomationWriteRange added{qMin(startBeat, endBeat), qMax(startBeat, endBeat)};
    m_automationWriteRanges.push_back(added);
    std::stable_sort(m_automationWriteRanges.begin(), m_automationWriteRanges.end(),
        [](const AutomationWriteRange& left, const AutomationWriteRange& right) {
            return left.startBeat < right.startBeat;
        });
    std::vector<AutomationWriteRange> merged;
    merged.reserve(m_automationWriteRanges.size());
    for (const AutomationWriteRange& range : m_automationWriteRanges) {
        if (!merged.empty() && range.startBeat <= merged.back().endBeat + 1.0e-9)
            merged.back().endBeat = qMax(merged.back().endBeat, range.endBeat);
        else
            merged.push_back(range);
    }
    if (merged.size() > kMaxAutomationWriteRanges) {
        // Abort the eventual model write atomically; never bridge or drop an
        // interval while retaining its recorded points.
        m_automationWriteRangeOverflow = true;
        merged.resize(kMaxAutomationWriteRanges);
    }
    m_automationWriteRanges = std::move(merged);
}

void VeraCoreQT::extendAutomationWriteRange(double beat)
{
    if (!std::isfinite(beat) || beat < 0.0) return;
    beat = qMax(0.0, beat);
    if (m_automationWriteRanges.empty() || m_automationWriteLastBeat < 0.0) {
        addAutomationWriteRange(beat, beat);
        m_automationWriteStartBeat = beat;
        m_automationWriteEndBeat = beat;
        m_automationWriteLastBeat = beat;
        return;
    }
    const double loopLength = m_loopEndBeat - m_loopStartBeat;
    const bool inLoop = m_loopEnabled && loopLength > 0.0 &&
        m_automationWriteLastBeat >= m_loopStartBeat &&
        m_automationWriteLastBeat <= m_loopEndBeat &&
        beat >= m_loopStartBeat && beat <= m_loopEndBeat &&
        std::isfinite(m_automationWriteLastBeat);
    const bool wrapped = inLoop && beat + 1.0e-9 < m_automationWriteLastBeat &&
        m_automationWriteLastBeat - beat > loopLength * 0.5;
    const bool delayedPreWrap = inLoop && beat > m_automationWriteLastBeat + loopLength * 0.5;
    if (wrapped) {
        addAutomationWriteRange(m_automationWriteLastBeat, m_loopEndBeat);
        addAutomationWriteRange(m_loopStartBeat, beat);
        m_automationWriteLastBeat = beat;
    } else if (delayedPreWrap) {
        // A late pre-wrap callback belongs to the existing high-beat interval;
        // never bridge it across the untouched middle of the loop.
        addAutomationWriteRange(beat, beat);
    } else {
        addAutomationWriteRange(m_automationWriteLastBeat, beat);
        m_automationWriteLastBeat = beat;
    }
    m_automationWriteStartBeat = qMin(m_automationWriteStartBeat, beat);
    m_automationWriteEndBeat = qMax(m_automationWriteEndBeat, beat);
}

void VeraCoreQT::closeAutomationWriteAtTransportDiscontinuity(double previousBeat)
{
    const bool wrapped = m_automationWriteActive && m_loopEnabled &&
        m_loopEndBeat > m_loopStartBeat && std::isfinite(previousBeat) &&
        previousBeat >= m_loopStartBeat && previousBeat <= m_loopEndBeat &&
        m_playheadBeat >= m_loopStartBeat && m_playheadBeat <= m_loopEndBeat &&
        m_playheadBeat + 1.0e-9 < previousBeat;
    if (wrapped) {
        // Pause/stop can observe the audio-engine wrap before the UI timer.
        // Close both ranges explicitly; do not rely on the half-loop heuristic.
        addAutomationWriteRange(previousBeat, m_loopEndBeat);
        addAutomationWriteRange(m_loopStartBeat, m_playheadBeat);
        m_automationWriteLastBeat = m_playheadBeat;
        m_automationWriteStartBeat = qMin(m_automationWriteStartBeat, m_playheadBeat);
        m_automationWriteEndBeat = qMax(m_automationWriteEndBeat, previousBeat);
    }
    finishAutomationWrite(m_playheadBeat);
}

void VeraCoreQT::finishTouchAutomationWrite(double eventBeat)
{
    if (!m_automationWriteActive || !std::isfinite(eventBeat) || eventBeat < 0.0) {
        finishAutomationWrite(eventBeat);
        return;
    }
    const Track* track = m_automationWriteTrackIndex >= 0 &&
        m_automationWriteTrackIndex < static_cast<int>(m_tracks.size())
        ? m_tracks[static_cast<size_t>(m_automationWriteTrackIndex)].get() : nullptr;
    const PluginAutomationLane* originalLane = nullptr;
    const int returnBus = m_automationWriteAudioFx
        ? returnBusFromAudioFxTrackId(m_automationWriteTrackIndex) : -1;
    if (returnBus >= 0) {
        const auto& deviceSlots = m_mixerParameters->returnBuses()[
            static_cast<std::size_t>(returnBus)].audioFxSlots;
        if (m_automationWriteSlotIndex >= 0 &&
            m_automationWriteSlotIndex < static_cast<int>(deviceSlots.size())) {
            const auto& returnAutomationLane = deviceSlots[
                static_cast<std::size_t>(m_automationWriteSlotIndex)].automation;
            const auto found = std::find_if(
                returnAutomationLane.begin(), returnAutomationLane.end(),
                [this](const auto& lane) {
                    return lane.parameterId == m_automationWriteParameterId;
                });
            if (found != returnAutomationLane.end()) originalLane = &*found;
        }
    } else if (track && m_automationWriteAudioFx) {
        const auto& deviceSlots = track->GetAudioFxSlots();
        if (m_automationWriteSlotIndex >= 0 &&
            m_automationWriteSlotIndex < static_cast<int>(deviceSlots.size())) {
            const auto& lanes = deviceSlots[static_cast<size_t>(m_automationWriteSlotIndex)].automation;
            const auto found = std::find_if(lanes.begin(), lanes.end(), [this](const auto& lane) {
                return lane.parameterId == m_automationWriteParameterId;
            });
            if (found != lanes.end()) originalLane = &*found;
        }
    } else if (const auto* midi = dynamic_cast<const MidiTrack*>(track)) {
        const auto& deviceSlots = midi->GetLayerSlots();
        if (m_automationWriteSlotIndex >= 0 &&
            m_automationWriteSlotIndex < static_cast<int>(deviceSlots.size())) {
            const auto& lanes = deviceSlots[static_cast<size_t>(m_automationWriteSlotIndex)].automation;
            const auto found = std::find_if(lanes.begin(), lanes.end(), [this](const auto& lane) {
                return lane.parameterId == m_automationWriteParameterId;
            });
            if (found != lanes.end()) originalLane = &*found;
        }
    }
    if (!originalLane || originalLane->points.empty()) {
        finishAutomationWrite(eventBeat);
        return;
    }

    constexpr double kTouchReturnSeconds = 0.1;
    const double fallbackTempo = m_transportWidget
        ? m_transportWidget->tempo() : kDefaultTempoBpm;
    const TempoClock clock(m_tempoMap, fallbackTempo);
    const double eventSeconds = clock.secondsAtBeat(eventBeat);
    const double returnSeconds = eventSeconds + kTouchReturnSeconds;
    const double loopStartSeconds = clock.secondsAtBeat(m_loopStartBeat);
    const double loopEndSeconds = clock.secondsAtBeat(m_loopEndBeat);
    const double loopDurationSeconds = loopEndSeconds - loopStartSeconds;
    const bool loopWrappedReturn = m_loopEnabled && loopDurationSeconds > 0.0 &&
        eventBeat >= m_loopStartBeat && eventBeat < m_loopEndBeat &&
        returnSeconds > loopEndSeconds;
    const auto mapTouchReturnBeat = [&](double timelineSeconds) {
        if (!loopWrappedReturn || timelineSeconds <= loopEndSeconds)
            return clock.beatAtSeconds(timelineSeconds);
        double wrappedSeconds = std::fmod(
            timelineSeconds - loopEndSeconds, loopDurationSeconds);
        if (wrappedSeconds < 0.0) wrappedSeconds += loopDurationSeconds;
        return clock.beatAtSeconds(loopStartSeconds + wrappedSeconds);
    };
    const double returnBeat = mapTouchReturnBeat(returnSeconds);
    const double returnValue = automationLaneValueAt(
        *originalLane, returnBeat, m_automationWriteValue);
    double returnTension = 0.0;
    PluginAutomationCurvePolynomial returnPolynomial{};
    bool hasReturnPolynomial = false;
    if (originalLane->interpolation == PluginAutomationInterpolation::Smooth) {
        const auto next = std::upper_bound(
            originalLane->points.begin(), originalLane->points.end(), returnBeat,
            [](double target, const PluginAutomationPoint& point) {
                return target < point.beat;
            });
        if (next != originalLane->points.begin() && next != originalLane->points.end() &&
            next->beat - returnBeat > 1.0e-12 &&
            std::fabs(next->normalizedValue - returnValue) > 1.0e-12) {
            const PluginAutomationPoint& left = *(next - 1);
            const double span = next->beat - left.beat;
            const double startPosition = span > 1.0e-12
                ? std::clamp((returnBeat - left.beat) / span, 0.0, 1.0) : 0.0;
            const PluginAutomationCurvePolynomial sourcePolynomial = left.curvePolynomial
                ? *left.curvePolynomial
                : pluginAutomationCurvePolynomial(left.curveTension);
            returnPolynomial = pluginAutomationRestrictedCurve(
                sourcePolynomial, startPosition);
            hasReturnPolynomial = pluginAutomationCurvePolynomialValid(returnPolynomial);
            returnTension = left.curveTension;
        }
    }
    writePluginAutomationPoint(m_automationWriteParameterId,
                               m_automationWriteParameterName,
                               eventBeat, m_automationWriteValue, true);
    if (loopWrappedReturn) {
        constexpr int kWrappedReturnSteps = 32;
        const double touchedValue = m_automationWriteValue;
        bool wroteLoopBoundary = false;
        for (int step = 1; step <= kWrappedReturnSteps; ++step) {
            const double amount = static_cast<double>(step) /
                static_cast<double>(kWrappedReturnSteps);
            const double stepSeconds = eventSeconds + kTouchReturnSeconds * amount;
            if (!wroteLoopBoundary && stepSeconds > loopEndSeconds) {
                const double boundaryAmount = std::clamp(
                    (loopEndSeconds - eventSeconds) / kTouchReturnSeconds, 0.0, 1.0);
                const double endValue = touchedValue +
                    (returnValue - touchedValue) * boundaryAmount;
                writePluginAutomationPoint(m_automationWriteParameterId,
                                           m_automationWriteParameterName,
                                           m_loopEndBeat, endValue, true);
                const double startValue = endValue;
                writePluginAutomationPoint(m_automationWriteParameterId,
                                           m_automationWriteParameterName,
                                           m_loopStartBeat, startValue, true);
                wroteLoopBoundary = true;
            }
            const double stepBeat = mapTouchReturnBeat(stepSeconds);
            const double value = touchedValue + (returnValue - touchedValue) * amount;
            writePluginAutomationPoint(m_automationWriteParameterId,
                                       m_automationWriteParameterName,
                                       stepBeat, value, true);
        }
        if (originalLane->interpolation == PluginAutomationInterpolation::Smooth) {
            writePluginAutomationPoint(m_automationWriteParameterId,
                                       m_automationWriteParameterName,
                                       returnBeat, returnValue, true, returnTension,
                                       hasReturnPolynomial ? &returnPolynomial : nullptr);
        }
    } else if (originalLane->interpolation == PluginAutomationInterpolation::Hold) {
        // Hold has no continuous segment primitive. A bounded 32-step ramp keeps
        // Touch release perceptually smooth without changing the lane's mode.
        constexpr int kHoldReturnSteps = 32;
        const double touchedValue = m_automationWriteValue;
        for (int step = 1; step <= kHoldReturnSteps; ++step) {
            const double amount = static_cast<double>(step) /
                static_cast<double>(kHoldReturnSteps);
            const double beat = clock.beatAtSeconds(
                eventSeconds + kTouchReturnSeconds * amount);
            const double value = touchedValue + (returnValue - touchedValue) * amount;
            writePluginAutomationPoint(m_automationWriteParameterId,
                                       m_automationWriteParameterName,
                                       beat, value, true);
        }
    } else {
        writePluginAutomationPoint(m_automationWriteParameterId,
                                   m_automationWriteParameterName,
                                   returnBeat, returnValue, true, returnTension,
                                   hasReturnPolynomial ? &returnPolynomial : nullptr);
    }
    finishAutomationWrite(returnBeat);
}

void VeraCoreQT::finishAutomationWrite(double finalBeat)
{
    if (m_automationWriteActive && !m_automationWriteOwnsHostGesture &&
        validPluginParameterTrackId(m_automationWriteTrackIndex,
            m_automationWriteAudioFx, m_tracks.size()) && m_automationWriteSlotIndex >= 0 &&
        m_automationWriteInstanceEpoch != 0 && m_automationWriteGestureSerial != 0) {
        const NativeAutomationGestureKey key{
            m_automationWriteTrackIndex, m_automationWriteAudioFx,
            m_automationWriteSlotIndex, m_automationWriteInstanceEpoch,
            m_automationWriteParameterId};
        const auto found = m_nativeAutomationGestureHighWater.find(key);
        if (found != m_nativeAutomationGestureHighWater.end() &&
            found->second.serial == m_automationWriteGestureSerial)
            found->second.closed = true;
    }
    bool writeInstanceCurrent = m_automationWriteInstanceEpoch == 0;
    if (!writeInstanceCurrent && m_pluginHost &&
        validPluginParameterTrackId(m_automationWriteTrackIndex,
            m_automationWriteAudioFx, m_tracks.size()) && m_automationWriteSlotIndex >= 0) {
        const uint64_t currentEpoch = m_automationWriteAudioFx
            ? m_pluginHost->instanceEpochForAudioFx(
                m_automationWriteTrackIndex, m_automationWriteSlotIndex)
            : m_pluginHost->instanceEpochForLayer(
                m_automationWriteTrackIndex * kLayerIdStride + m_automationWriteSlotIndex);
        writeInstanceCurrent = currentEpoch != 0 &&
            currentEpoch == m_automationWriteInstanceEpoch;
    }
    if (!m_automationWriteActive) {
        if (writeInstanceCurrent && m_automationWriteOwnsHostGesture && m_pluginHost &&
            validPluginParameterTrackId(m_automationWriteTrackIndex,
                m_automationWriteAudioFx, m_tracks.size()) && m_automationWriteSlotIndex >= 0) {
            if (m_automationWriteAudioFx)
                m_pluginHost->endAudioFxParameterGesture(
                    m_automationWriteTrackIndex, m_automationWriteSlotIndex,
                    m_automationWriteParameterId);
            else
                m_pluginHost->endLayerParameterGesture(
                    m_automationWriteTrackIndex * kLayerIdStride + m_automationWriteSlotIndex,
                    m_automationWriteParameterId);
            if (m_parameterEditorGestureDepth > 0) --m_parameterEditorGestureDepth;
        }
        m_automationWriteOwnsHostGesture = false;
        m_automationWriteTrackIndex = -1;
        m_automationWriteAudioFx = false;
        m_automationWriteSlotIndex = -1;
        m_automationWriteInstanceEpoch = 0;
        m_automationWriteParameterId = 0;
        m_automationWriteParameterName.clear();
        m_automationWriteRanges.clear();
        m_automationWriteLastBeat = -1.0;
        m_automationWriteRangeOverflow = false;
        m_automationWriteBufferOverflow = false;
        m_automationWriteStreamOverflow = false;
        m_automationWriteBuffer.clear();
        return;
    }
    const double resolvedFinalBeat = std::isfinite(finalBeat) && finalBeat >= 0.0
        ? finalBeat : m_playheadBeat;
    extendAutomationWriteRange(resolvedFinalBeat);
    std::vector<PluginAutomationPoint> recorded = std::move(m_automationWriteBuffer);
    const auto hasPointAt = [&](double beat) {
        return std::any_of(recorded.begin(), recorded.end(), [beat](const auto& point) {
            return point.beat == beat;
        });
    };
    for (const AutomationWriteRange& range : m_automationWriteRanges) {
        auto first = recorded.end();
        auto last = recorded.end();
        for (auto point = recorded.begin(); point != recorded.end(); ++point) {
            if (point->beat < range.startBeat || point->beat > range.endBeat) continue;
            if (first == recorded.end() || point->beat < first->beat) first = point;
            if (last == recorded.end() || point->beat > last->beat) last = point;
        }
        const double firstValue = first != recorded.end()
            ? first->normalizedValue : m_automationWriteValue;
        const double lastValue = last != recorded.end()
            ? last->normalizedValue : m_automationWriteValue;
        if (!hasPointAt(range.startBeat)) recorded.push_back({range.startBeat,
            firstValue});
        if (!hasPointAt(range.endBeat)) recorded.push_back({range.endBeat,
            lastValue});
    }
    std::stable_sort(recorded.begin(), recorded.end(),
        [](const auto& left, const auto& right) { return left.beat < right.beat; });
    std::vector<PluginAutomationPoint> uniqueRecorded;
    uniqueRecorded.reserve(recorded.size());
    for (const PluginAutomationPoint& point : recorded) {
        if (!uniqueRecorded.empty() && uniqueRecorded.back().beat == point.beat)
            uniqueRecorded.back() = point;
        else
            uniqueRecorded.push_back(point);
    }

    const Track* track = m_automationWriteTrackIndex >= 0 &&
        m_automationWriteTrackIndex < static_cast<int>(m_tracks.size())
        ? m_tracks[static_cast<size_t>(m_automationWriteTrackIndex)].get() : nullptr;
    const std::vector<PluginAutomationLane>* lanes = nullptr;
    const int returnBus = m_automationWriteAudioFx
        ? returnBusFromAudioFxTrackId(m_automationWriteTrackIndex) : -1;
    if (returnBus >= 0) {
        const auto& deviceSlots = m_mixerParameters->returnBuses()[
            static_cast<std::size_t>(returnBus)].audioFxSlots;
        if (m_automationWriteSlotIndex >= 0 &&
            m_automationWriteSlotIndex < static_cast<int>(deviceSlots.size()))
            lanes = &deviceSlots[static_cast<std::size_t>(
                m_automationWriteSlotIndex)].automation;
    } else if (track && m_automationWriteAudioFx) {
        const auto& deviceSlots = track->GetAudioFxSlots();
        if (m_automationWriteSlotIndex >= 0 &&
            m_automationWriteSlotIndex < static_cast<int>(deviceSlots.size()))
            lanes = &deviceSlots[static_cast<size_t>(m_automationWriteSlotIndex)].automation;
    } else if (const auto* midi = dynamic_cast<const MidiTrack*>(track)) {
        const auto& deviceSlots = midi->GetLayerSlots();
        if (m_automationWriteSlotIndex >= 0 &&
            m_automationWriteSlotIndex < static_cast<int>(deviceSlots.size()))
            lanes = &deviceSlots[static_cast<size_t>(m_automationWriteSlotIndex)].automation;
    }
    if (lanes && writeInstanceCurrent) {
        PluginAutomationLane lane;
        lane.parameterId = m_automationWriteParameterId;
        lane.parameterName = m_automationWriteParameterName;
        const auto found = std::find_if(lanes->begin(), lanes->end(), [this](const auto& candidate) {
            return candidate.parameterId == m_automationWriteParameterId;
        });
        if (found != lanes->end()) lane = *found;
        bool writeRejected = m_automationWriteRangeOverflow ||
            m_automationWriteBufferOverflow || m_automationWriteStreamOverflow;
        if (writeRejected) {
            ui.statusBar->showMessage(
                m_automationWriteStreamOverflow
                    ? "Automation write skipped: native edit stream overflowed."
                    : (m_automationWriteBufferOverflow
                        ? "Automation write skipped: capture point limit exceeded."
                        : "Automation write skipped: too many disjoint capture ranges."),
                7000);
        }
        lane.enabled = true;
        if (lane.parameterName.isEmpty()) lane.parameterName = m_automationWriteParameterName;
        // Keep Smooth control points exact. Its piecewise S-curves cannot be
        // safely thinned with a finite sample audit without changing extrema.
        if (lane.interpolation == PluginAutomationInterpolation::Linear &&
            m_automationWriteRanges.size() == 1 &&
            uniqueRecorded.size() > 2) {
            std::vector<PluginAutomationPoint> thinned;
            thinned.reserve(uniqueRecorded.size());
            thinned.push_back(uniqueRecorded.front());
            for (size_t i = 1; i + 1 < uniqueRecorded.size(); ++i) {
                const auto& left = thinned.back();
                const auto& point = uniqueRecorded[i];
                const auto& right = uniqueRecorded[i + 1];
                const double span = right.beat - left.beat;
                bool removable = false;
                if (span > 1.0e-12) {
                    const double position = std::clamp(
                        (point.beat - left.beat) / span, 0.0, 1.0);
                    const double expected = left.normalizedValue +
                        (right.normalizedValue - left.normalizedValue) * position;
                    removable = !left.curvePolynomial && !point.curvePolynomial &&
                        std::fabs(point.curveTension) <= 1.0e-9 &&
                        std::fabs(expected - point.normalizedValue) <= 0.0005;
                }
                if (!removable)
                    thinned.push_back(point);
            }
            thinned.push_back(uniqueRecorded.back());
            uniqueRecorded = std::move(thinned);
        }
        std::vector<PluginAutomationPoint> preserved;
        preserved.reserve(lane.points.size());
        for (const PluginAutomationPoint& point : lane.points) {
            const bool overwritten = std::any_of(
                m_automationWriteRanges.begin(), m_automationWriteRanges.end(),
                [&point](const AutomationWriteRange& range) {
                    return point.beat >= range.startBeat && point.beat <= range.endBeat;
                });
            if (!overwritten)
                preserved.push_back(point);
        }
        const size_t available = kMaxPluginAutomationPointsPerLane > preserved.size()
            ? kMaxPluginAutomationPointsPerLane - preserved.size() : 0;
        std::vector<double> mandatoryBeats;
        mandatoryBeats.reserve(m_automationWriteRanges.size() * 2 + 1);
        auto requireBeat = [&](double beat) {
            if (std::none_of(mandatoryBeats.begin(), mandatoryBeats.end(), [beat](double existing) {
                    return existing == beat;
                })) mandatoryBeats.push_back(beat);
        };
        for (const PluginAutomationPoint& point : uniqueRecorded)
            if (point.curvePolynomial) requireBeat(point.beat);
        for (const AutomationWriteRange& range : m_automationWriteRanges) {
            requireBeat(range.startBeat);
            requireBeat(range.endBeat);
        }
        if (available < mandatoryBeats.size()) {
            // Keep the lane byte-for-byte unchanged instead of dropping a write
            // boundary and changing interpolation outside the gesture.
            writeRejected = true;
            ui.statusBar->showMessage(
                "Automation write skipped: lane point limit cannot preserve all boundaries.",
                7000);
        } else if (uniqueRecorded.size() > available && available > 0) {
            std::vector<PluginAutomationPoint> bounded;
            bounded.reserve(available);
            auto addBounded = [&](const PluginAutomationPoint& point) {
                if (bounded.size() >= available) return;
                const bool duplicate = std::any_of(
                    bounded.begin(), bounded.end(), [&point](const auto& existing) {
                        return existing.beat == point.beat;
                    });
                if (!duplicate) bounded.push_back(point);
            };
            // Exact continuation metadata is highest priority near capacity.
            for (auto point = uniqueRecorded.rbegin(); point != uniqueRecorded.rend(); ++point)
                if (point->curvePolynomial) addBounded(*point);
            // Every disjoint overwrite interval keeps both boundaries.
            for (const AutomationWriteRange& range : m_automationWriteRanges) {
                const auto end = std::find_if(uniqueRecorded.begin(), uniqueRecorded.end(),
                    [&range](const auto& point) { return point.beat == range.endBeat; });
                if (end != uniqueRecorded.end()) addBounded(*end);
                const auto start = std::find_if(uniqueRecorded.begin(), uniqueRecorded.end(),
                    [&range](const auto& point) { return point.beat == range.startBeat; });
                if (start != uniqueRecorded.end()) addBounded(*start);
            }
            for (size_t sample = 0; bounded.size() < available &&
                 sample < available; ++sample) {
                const size_t source = available == 1 ? uniqueRecorded.size() - 1
                    : (sample * (uniqueRecorded.size() - 1)) / (available - 1);
                addBounded(uniqueRecorded[source]);
            }
            for (const PluginAutomationPoint& point : uniqueRecorded) {
                if (bounded.size() >= available) break;
                addBounded(point);
            }
            std::stable_sort(bounded.begin(), bounded.end(),
                [](const auto& left, const auto& right) { return left.beat < right.beat; });
            uniqueRecorded = std::move(bounded);
        } else if (available == 0) {
            uniqueRecorded.clear();
        }
        if (!writeRejected) {
            preserved.insert(preserved.end(), uniqueRecorded.begin(), uniqueRecorded.end());
            std::stable_sort(preserved.begin(), preserved.end(),
                [](const auto& left, const auto& right) { return left.beat < right.beat; });
            lane.points = std::move(preserved);
            const QString mergeKey = QStringLiteral("gesture:automation-write:%1:%2:%3:%4:%5:%6")
                .arg(m_automationWriteTrackIndex).arg(m_automationWriteAudioFx ? 1 : 0)
                .arg(m_automationWriteSlotIndex).arg(static_cast<qulonglong>(m_automationWriteParameterId))
                .arg(static_cast<qulonglong>(m_automationWriteInstanceEpoch))
                .arg(static_cast<qulonglong>(m_automationWriteGestureSerial));
            applyPluginAutomationLane(m_automationWriteTrackIndex, m_automationWriteAudioFx,
                                      m_automationWriteSlotIndex, lane, mergeKey);
        }
    }
    if (writeInstanceCurrent && m_automationWriteOwnsHostGesture && m_pluginHost &&
        validPluginParameterTrackId(m_automationWriteTrackIndex,
            m_automationWriteAudioFx, m_tracks.size()) && m_automationWriteSlotIndex >= 0) {
        if (m_automationWriteAudioFx)
            m_pluginHost->endAudioFxParameterGesture(
                m_automationWriteTrackIndex, m_automationWriteSlotIndex,
                m_automationWriteParameterId);
        else
            m_pluginHost->endLayerParameterGesture(
                m_automationWriteTrackIndex * kLayerIdStride + m_automationWriteSlotIndex,
                m_automationWriteParameterId);
        if (m_parameterEditorGestureDepth > 0) --m_parameterEditorGestureDepth;
    }
    m_automationWriteActive = false;
    m_automationWriteOwnsHostGesture = false;
    m_automationWriteTrackIndex = -1;
    m_automationWriteAudioFx = false;
    m_automationWriteSlotIndex = -1;
    m_automationWriteInstanceEpoch = 0;
    m_automationWriteParameterId = 0;
    m_automationWriteParameterName.clear();
    m_automationWriteRanges.clear();
    m_automationWriteLastBeat = -1.0;
    m_automationWriteRangeOverflow = false;
    m_automationWriteBufferOverflow = false;
    m_automationWriteStreamOverflow = false;
    m_automationWriteBuffer.clear();
}

void VeraCoreQT::syncPluginTransportToHost()
{
    if (!m_pluginHost) {
        return;
    }
    const double fallbackTempo =
        m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm;
    const TempoClock transportClock(m_tempoMap, fallbackTempo);
    const double bpm = transportClock.bpmAtBeat(m_playheadBeat);
    const double projectSeconds = transportClock.secondsAtBeat(m_playheadBeat);
    const PluginMetricalPosition metrical = pluginMetricalPositionAtBeat(
        m_timeSignatureMap, m_playheadBeat,
        m_transportWidget ? m_transportWidget->signatureNumerator() : 4,
        m_transportWidget ? m_transportWidget->signatureDenominator() : 4);
    const bool cycleActive = m_loopEnabled &&
        std::isfinite(m_loopStartBeat) && std::isfinite(m_loopEndBeat) &&
        m_loopStartBeat >= 0.0 && m_loopEndBeat > m_loopStartBeat;
    const double cycleStartSeconds = cycleActive
        ? transportClock.secondsAtBeat(m_loopStartBeat) : 0.0;
    const double cycleEndSeconds = cycleActive
        ? transportClock.secondsAtBeat(m_loopEndBeat) : 0.0;
    const double cycleStartBpm = cycleActive
        ? transportClock.bpmAtBeat(m_loopStartBeat) : bpm;
    const PluginMetricalPosition cycleStartMetrical = pluginMetricalPositionAtBeat(
        m_timeSignatureMap, cycleActive ? m_loopStartBeat : m_playheadBeat,
        m_transportWidget ? m_transportWidget->signatureNumerator() : 4,
        m_transportWidget ? m_transportWidget->signatureDenominator() : 4);
    m_pluginHost->setTransport(m_isPlaying, m_isRecordingMidi || m_isRecordingAudio,
        bpm, m_playheadBeat,
                               metrical.numerator, metrical.denominator, projectSeconds,
                               metrical.barStartBeat, metrical.barNumber, cycleActive,
                               m_loopStartBeat, m_loopEndBeat,
                               cycleStartSeconds, cycleEndSeconds,
                               cycleStartBpm, cycleStartMetrical.barStartBeat,
                               cycleStartMetrical.barNumber,
                               cycleStartMetrical.numerator,
                               cycleStartMetrical.denominator);
}

void VeraCoreQT::syncTransportDisplayToPlayhead()
{
    if (!m_transportWidget) return;
    const double mappedTempo = tempoAtBeat(
        m_tempoMap, m_playheadBeat, m_transportWidget->tempo());
    const MidiTimeSignaturePoint mappedSignature = timeSignatureAtBeat(
        m_timeSignatureMap, m_playheadBeat,
        m_transportWidget->signatureNumerator(),
        m_transportWidget->signatureDenominator());
    QSignalBlocker blocker(m_transportWidget);
    if (std::fabs(mappedTempo - m_transportWidget->tempo()) > 1.0e-6)
        m_transportWidget->setTempo(mappedTempo);
    if (mappedSignature.numerator != m_transportWidget->signatureNumerator() ||
        mappedSignature.denominator != m_transportWidget->signatureDenominator())
        m_transportWidget->setTimeSignature(
            mappedSignature.numerator, mappedSignature.denominator);
}

void VeraCoreQT::refreshSelectedAraProcessingAlgorithms()
{
    if (!m_inspectorWidget) return;
    AraProcessingAlgorithmSelection selection;
    if (!m_pluginHost ||
        !m_pluginHost->selectedAraProcessingAlgorithms(selection) ||
        selection.algorithms.empty()) {
        m_inspectorWidget->setAraProcessingAlgorithms(QString(), QStringList(), -1);
        return;
    }

    const auto strictUtf8 = [](const std::string& value, QString& decoded) {
        if (value.empty() || value.size() > 1024u) return false;
        const QByteArray bytes(value.data(), static_cast<qsizetype>(value.size()));
        decoded = QString::fromUtf8(bytes);
        return !decoded.isEmpty() && decoded.toUtf8() == bytes;
    };
    QString pluginName;
    if (!strictUtf8(selection.pluginNameUtf8, pluginName)) {
        m_inspectorWidget->setAraProcessingAlgorithms(QString(), QStringList(), -1);
        return;
    }
    QStringList names;
    names.reserve(static_cast<qsizetype>(selection.algorithms.size()));
    for (const AraProcessingAlgorithm& algorithm : selection.algorithms) {
        QString name;
        if (!strictUtf8(algorithm.nameUtf8, name)) {
            m_inspectorWidget->setAraProcessingAlgorithms(QString(), QStringList(), -1);
            return;
        }
        names.push_back(name);
    }
    m_inspectorWidget->setAraProcessingAlgorithms(
        pluginName, names, selection.currentIndex);
}

void VeraCoreQT::processPluginParameterEdits()
{
    if (!m_pluginHost || m_restoringProject || m_offlineRenderActive) return;
    bool overflowed = false;
    const std::vector<PluginParameterEditEvent> edits =
        m_pluginHost->consumePluginParameterEdits(&overflowed);
    bool refreshSelectedParameters = false;
    const double fallbackTempo = m_transportWidget
        ? m_transportWidget->tempo() : kDefaultTempoBpm;
    const TempoClock captureClock(m_tempoMap, fallbackTempo);
    const double captureNowSeconds = captureClock.secondsAtBeat(m_playheadBeat);
    const double loopStartSeconds = captureClock.secondsAtBeat(m_loopStartBeat);
    const double loopEndSeconds = captureClock.secondsAtBeat(m_loopEndBeat);
    const double loopDurationSeconds = loopEndSeconds - loopStartSeconds;

    for (const PluginParameterEditEvent& edit : edits) {
        const bool audioFx = edit.target == PluginStateTarget::AudioFx;
        const int returnBus = audioFx
            ? returnBusFromAudioFxTrackId(edit.trackIndex) : -1;
        const bool returnAutomationWrite = returnBus >= 0;
        const bool trackTargetValid = edit.trackIndex >= 0 &&
            edit.trackIndex < static_cast<int>(m_tracks.size()) &&
            m_tracks[static_cast<std::size_t>(edit.trackIndex)];
        if ((!returnAutomationWrite && !trackTargetValid) || edit.slotIndex < 0) continue;
        Track* track = trackTargetValid
            ? m_tracks[static_cast<std::size_t>(edit.trackIndex)].get() : nullptr;
        QString deviceName;
        bool slotValid = false;
        if (returnAutomationWrite) {
            const auto& deviceSlots = m_mixerParameters->returnBuses()[
                static_cast<std::size_t>(returnBus)].audioFxSlots;
            slotValid = edit.slotIndex < static_cast<int>(deviceSlots.size());
            if (slotValid)
                deviceName = deviceSlots[static_cast<std::size_t>(edit.slotIndex)].name;
        } else if (audioFx) {
            const auto& deviceSlots = track->GetAudioFxSlots();
            slotValid = edit.slotIndex < static_cast<int>(deviceSlots.size());
            if (slotValid) deviceName = deviceSlots[static_cast<size_t>(edit.slotIndex)].name;
        } else if (auto* midi = dynamic_cast<MidiTrack*>(track)) {
            const auto& deviceSlots = midi->GetLayerSlots();
            slotValid = edit.layerId == edit.trackIndex * kLayerIdStride + edit.slotIndex &&
                edit.slotIndex < static_cast<int>(deviceSlots.size());
            if (slotValid) deviceName = deviceSlots[static_cast<size_t>(edit.slotIndex)].name;
        }
        if (!slotValid) continue;
        const bool sameActiveNativeInstance = m_automationWriteActive &&
            !m_automationWriteOwnsHostGesture &&
            m_automationWriteTrackIndex == edit.trackIndex &&
            m_automationWriteAudioFx == audioFx &&
            m_automationWriteSlotIndex == edit.slotIndex &&
            m_automationWriteInstanceEpoch != 0 && edit.instanceEpoch != 0 &&
            m_automationWriteInstanceEpoch == edit.instanceEpoch;
        if (edit.streamOverflowed) {
            if (sameActiveNativeInstance) {
                m_automationWriteStreamOverflow = true;
                finishAutomationWrite();
            }
            continue;
        }
        if (!acceptNativeAutomationGestureSerial(
                edit.trackIndex, audioFx, edit.slotIndex, edit.instanceEpoch,
                edit.parameterId, edit.gestureSerial,
                edit.phase == PluginParameterEditPhase::End))
            continue;
        const bool sameActiveNativeIdentity = m_automationWriteActive &&
            !m_automationWriteOwnsHostGesture &&
            m_automationWriteTrackIndex == edit.trackIndex &&
            m_automationWriteAudioFx == audioFx &&
            m_automationWriteSlotIndex == edit.slotIndex &&
            m_automationWriteParameterId == edit.parameterId &&
            m_automationWriteInstanceEpoch != 0 && edit.instanceEpoch != 0 &&
            m_automationWriteInstanceEpoch == edit.instanceEpoch;
        const bool exactActiveNativeGesture = sameActiveNativeIdentity &&
            edit.gestureSerial != 0 && m_automationWriteGestureSerial != 0 &&
            edit.gestureSerial == m_automationWriteGestureSerial;
        const bool newerActiveNativeGesture = sameActiveNativeIdentity &&
            edit.gestureSerial != 0 && m_automationWriteGestureSerial != 0 &&
            edit.gestureSerial > m_automationWriteGestureSerial;

        double eventBeat = m_playheadBeat;
        if (m_isPlaying && std::isfinite(edit.capturedProjectBeat) &&
            edit.capturedProjectBeat >= 0.0) {
            eventBeat = edit.capturedProjectBeat;
            if (m_loopEnabled && m_loopEndBeat > m_loopStartBeat &&
                eventBeat >= m_loopEndBeat) {
                eventBeat = m_loopStartBeat + std::fmod(
                    eventBeat - m_loopStartBeat, m_loopEndBeat - m_loopStartBeat);
            }
        } else if (m_isPlaying && std::isfinite(edit.captureAgeSeconds) &&
            edit.captureAgeSeconds > 0.0) {
            double eventSeconds = captureNowSeconds - edit.captureAgeSeconds;
            if (m_loopEnabled && loopDurationSeconds > 0.0 &&
                m_playheadBeat >= m_loopStartBeat && m_playheadBeat <= m_loopEndBeat &&
                eventSeconds < loopStartSeconds) {
                const double beforeLoopStart = loopStartSeconds - eventSeconds;
                double wrapped = std::fmod(beforeLoopStart, loopDurationSeconds);
                if (wrapped <= 1.0e-12) wrapped = loopDurationSeconds;
                eventSeconds = loopEndSeconds - wrapped;
            }
            eventBeat = captureClock.beatAtSeconds(qMax(0.0, eventSeconds));
        }
        if (newerActiveNativeGesture) {
            finishAutomationWrite(eventBeat);
            if (edit.phase == PluginParameterEditPhase::End) continue;
        }

        const bool sameWriteIdentity = m_automationWriteTrackIndex == edit.trackIndex &&
            m_automationWriteAudioFx == audioFx &&
            m_automationWriteSlotIndex == edit.slotIndex &&
            m_automationWriteParameterId == edit.parameterId &&
            (m_automationWriteInstanceEpoch == 0 || edit.instanceEpoch == 0 ||
             m_automationWriteInstanceEpoch == edit.instanceEpoch);
        const bool sameWriteTarget = sameWriteIdentity &&
            (!m_automationWriteActive ||
             (!m_automationWriteOwnsHostGesture &&
              (edit.gestureSerial == 0 || m_automationWriteGestureSerial == 0 ||
               edit.gestureSerial == m_automationWriteGestureSerial)));
        const bool exactWriteGesture = sameWriteTarget && m_automationWriteActive &&
            !m_automationWriteOwnsHostGesture &&
            edit.gestureSerial != 0 && m_automationWriteGestureSerial != 0 &&
            edit.gestureSerial == m_automationWriteGestureSerial;

        QString parameterName = sameWriteTarget ? m_automationWriteParameterName : QString();
        double currentValue = edit.normalizedValue;
        bool automatable = true;
        if (!sameWriteTarget || parameterName.isEmpty() ||
            edit.phase == PluginParameterEditPhase::Begin) {
            const std::vector<PluginParameterInfo> parameters = audioFx
                ? m_pluginHost->parametersForAudioFx(edit.trackIndex, edit.slotIndex)
                : m_pluginHost->parametersForLayer(edit.layerId);
            const auto parameter = std::find_if(parameters.begin(), parameters.end(),
                [&edit](const auto& item) { return item.stableId == edit.parameterId; });
            if (parameter == parameters.end()) continue;
            parameterName = parameter->module.isEmpty()
                ? parameter->name : parameter->module + " / " + parameter->name;
            if (returnAutomationWrite) {
                const QString prefix = QString("Return %1 · ").arg(QChar('A' + returnBus));
                if (!parameterName.startsWith(prefix)) parameterName.prepend(prefix);
            }
            currentValue = parameter->normalizedValue;
            automatable = parameter->automatable && !parameter->readOnly && !parameter->hidden;
        }

        const bool selectedEdit = returnAutomationWrite
            ? m_inspectorWidget && m_inspectorWidget->currentReturnFxBus() == returnBus
            : edit.trackIndex == m_selectedTrackIndex;
        if (selectedEdit) {
            m_parameterEditorTrackIndex = edit.trackIndex;
            m_parameterEditorReturnBus = returnAutomationWrite ? returnBus : -1;
            m_parameterEditorSlotIndex = edit.slotIndex;
            m_parameterEditorAudioFx = audioFx;
            m_parameterEditorDeviceName = deviceName;
            if (automatable) {
                m_hasAutomationTarget = true;
                m_automationParameterId = edit.parameterId;
                m_automationParameterName = parameterName;
                if (m_inspectorWidget)
                    m_inspectorWidget->setActiveAutomationTarget(
                        audioFx, edit.slotIndex, edit.parameterId);
                refreshPluginAutomationLane();
            }
            refreshSelectedParameters = true;
        }

        if (!automatable || m_automationWriteMode == InspectorWidget::AutomationWriteMode::Read ||
            !m_isPlaying) continue;

        if (edit.phase == PluginParameterEditPhase::Begin) {
            if (m_automationWriteActive && !sameWriteTarget) finishAutomationWrite();
            if (!m_automationWriteActive) {
                armAutomationWriteTarget(edit.trackIndex, audioFx, edit.slotIndex,
                                         edit.parameterId, parameterName,
                                         edit.instanceEpoch);
                m_automationWriteOwnsHostGesture = false;
                m_automationWriteGestureSerial = edit.gestureSerial != 0
                    ? edit.gestureSerial : m_automationWriteGestureSerial + 1;
                writePluginAutomationPoint(edit.parameterId, parameterName,
                                           eventBeat, currentValue);
            }
            continue;
        }

        if (edit.phase == PluginParameterEditPhase::Value) {
            if (!sameWriteTarget && m_automationWriteActive) finishAutomationWrite();
            if (!m_automationWriteActive) {
                armAutomationWriteTarget(edit.trackIndex, audioFx, edit.slotIndex,
                                         edit.parameterId, parameterName,
                                         edit.instanceEpoch);
                m_automationWriteOwnsHostGesture = false;
                m_automationWriteGestureSerial = edit.gestureSerial != 0
                    ? edit.gestureSerial : m_automationWriteGestureSerial + 1;
            }
            writePluginAutomationPoint(edit.parameterId, parameterName,
                                       eventBeat, edit.normalizedValue);
            continue;
        }

        if (edit.phase == PluginParameterEditPhase::End && exactWriteGesture &&
            m_automationWriteActive) {
            if (m_automationWriteMode == InspectorWidget::AutomationWriteMode::Touch) {
                finishTouchAutomationWrite(eventBeat);
            } else if (!m_automationWriteOwnsHostGesture) {
                const bool began = audioFx
                    ? m_pluginHost->beginAudioFxParameterGesture(
                        edit.trackIndex, edit.slotIndex, edit.parameterId)
                    : m_pluginHost->beginLayerParameterGesture(edit.layerId, edit.parameterId);
                if (began) {
                    m_automationWriteOwnsHostGesture = true;
                    ++m_parameterEditorGestureDepth;
                } else {
                    finishAutomationWrite();
                }
            }
        }
    }

    if (overflowed) {
        ui.statusBar->showMessage(
            "Plug-in editor automation queue overflow; gesture safely ended at last value.",
            7000);
    }
    if (refreshSelectedParameters && m_parameterEditorGestureDepth == 0)
        refreshInspectorPluginParameters();
}

void VeraCoreQT::processPluginStateChanges()
{
    if (!m_pluginHost || m_restoringProject || m_offlineRenderActive) return;
    processPluginParameterEdits();
    const std::vector<PluginStateChange> changes = m_pluginHost->consumePluginStateChanges();
    bool modelChanged = false;
    for (const PluginStateChange& change : changes) {
        const int returnBus = change.target == PluginStateTarget::AudioFx
            ? returnBusFromAudioFxTrackId(change.trackIndex) : -1;
        const bool returnFxStateChange = returnBus >= 0;
        const bool trackTargetValid = change.trackIndex >= 0 &&
            change.trackIndex < static_cast<int>(m_tracks.size()) &&
            m_tracks[static_cast<std::size_t>(change.trackIndex)];
        if ((!returnFxStateChange && !trackTargetValid) || change.slotIndex < 0 ||
            change.beforeState.isEmpty() ||
            change.afterState.isEmpty() || change.beforeState == change.afterState) {
            continue;
        }
        const std::shared_ptr<Track> trackRef = trackTargetValid
            ? m_tracks[static_cast<std::size_t>(change.trackIndex)] : nullptr;

        std::function<void(const QByteArray&)> applyState;
        QString targetName;
        if (returnFxStateChange) {
            const auto& returnSlots = m_mixerParameters->returnBuses()[
                static_cast<std::size_t>(returnBus)].audioFxSlots;
            if (change.slotIndex >= static_cast<int>(returnSlots.size())) continue;
            applyState = [this, returnBus, slotIndex = change.slotIndex](
                             const QByteArray& stateBytes) {
                ReturnBusState state = m_mixerParameters->returnBuses()[
                    static_cast<std::size_t>(returnBus)];
                if (slotIndex < 0 || slotIndex >= static_cast<int>(state.audioFxSlots.size()))
                    return;
                state.audioFxSlots[static_cast<std::size_t>(slotIndex)].pluginState = stateBytes;
                m_mixerParameters->setReturnBus(returnBus, state);
                syncRealtimeMidiLayersToAudio();
            };
            targetName = QStringLiteral("return-fx");
        } else if (change.target == PluginStateTarget::Layer) {
            auto* midi = dynamic_cast<MidiTrack*>(trackRef.get());
            if (!midi || change.layerId != change.trackIndex * kLayerIdStride + change.slotIndex ||
                change.slotIndex >= static_cast<int>(midi->GetLayerSlots().size())) continue;
            applyState = [trackRef, slotIndex = change.slotIndex](const QByteArray& state) {
                if (auto* target = dynamic_cast<MidiTrack*>(trackRef.get()))
                    target->SetLayerSlotPluginState(slotIndex, state);
            };
            targetName = QStringLiteral("layer");
        } else {
            if (change.slotIndex >= static_cast<int>(trackRef->GetAudioFxSlots().size())) continue;
            applyState = [trackRef, slotIndex = change.slotIndex](const QByteArray& state) {
                trackRef->SetAudioFxPluginState(slotIndex, state);
            };
            targetName = QStringLiteral("fx");
        }

        const bool captureUndo = m_playlistView && !m_suppressProjectUndoCapture;
        const quint64 beforeStateId = captureUndo
            ? m_playlistView->currentArrangementStateId() : 0;
        const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
        if (captureUndo) {
            m_playlistView->beginArrangementTransaction();
            m_playlistView->markArrangementTransactionChanged();
        }
        applyState(change.afterState);
        modelChanged = true;

        if (captureUndo) {
            const QString targetKey = returnFxStateChange
                ? QStringLiteral("return-%1").arg(returnBus)
                : QString::number(static_cast<qulonglong>(
                    reinterpret_cast<quintptr>(trackRef.get())), 16);
            const QString mergeKey = QStringLiteral("gesture:plugin-%1:%2:%3:%4:%5")
                .arg(targetName)
                .arg(targetKey)
                .arg(change.slotIndex)
                .arg(static_cast<qulonglong>(change.instanceEpoch))
                .arg(static_cast<qulonglong>(change.gestureSerial));
            const auto before = std::make_shared<QByteArray>(change.beforeState);
            const auto after = std::make_shared<QByteArray>(change.afterState);
            if (!commitProjectUndoStep(
                    beforeStateId, nonArrangementDirtyBefore, mergeKey,
                    [applyState, before]() { applyState(*before); },
                    [applyState, after]() { applyState(*after); })) {
                setProjectDirty();
            }
        } else {
            setProjectDirty();
        }
    }
    if (modelChanged && m_inspectorWidget) {
        m_inspectorWidget->refreshTrackDevices();
        if (m_parameterEditorGestureDepth == 0) refreshInspectorPluginParameters();
    }
}

void VeraCoreQT::processAraHostRequests()
{
    if (++m_audioHealthPollDivider >= 10) {
        m_audioHealthPollDivider = 0;
        if (m_transportWidget && !audioGeneral) {
            const QString message = "Audio engine unavailable.";
            m_transportWidget->setAudioDeviceStatus(false, message);
            if (m_isPlaying) {
                onTransportPause();
                m_transportWidget->setAudioDeviceStatus(false, message);
                ui.statusBar->showMessage("Playback stopped: audio engine unavailable.", 7000);
            }
        } else if (audioGeneral && m_transportWidget) {
            const AudioDeviceStatus deviceStatus = audioGeneral->GetAudioDeviceStatus();
            m_transportWidget->setAudioDeviceStatus(deviceStatus.active, deviceStatus.message);
            if (!deviceStatus.active && m_isPlaying) {
                onTransportPause();
                m_transportWidget->setAudioDeviceStatus(false, deviceStatus.message);
                ui.statusBar->showMessage(
                    QString("Playback stopped: %1").arg(deviceStatus.message), 7000);
            }
            const VeraCoreAudioHealthState state =
                audioGeneral->audioCore.GetAudioHealthState();
            m_transportWidget->setAudioHealth(
                state.callbackCount, state.cpuLoad, state.inputUnderflows, state.inputOverflows,
                state.outputUnderflows, state.outputOverflows, state.primingOutputs);
        }
		if (audioGeneral && m_inspectorWidget) {
			std::array<VeraCoreAudioMeterState, kReturnBusCount> returnMeters{};
			for (int bus = 0; bus < kReturnBusCount; ++bus)
				returnMeters[bus] = audioGeneral->audioCore.GetAndResetReturnMeterState(bus);
			m_inspectorWidget->setReturnMeters(returnMeters);
		}
    }
    if (!m_pluginHost) {
        m_pluginHealthPollDivider = 0;
        m_araAnalysisWasActive = false;
        if (m_inspectorWidget) {
            m_inspectorWidget->setAraStatus(false, false, {}, 0.0f);
            m_inspectorWidget->setAraProcessingAlgorithms(
                QString(), QStringList(), -1);
        }
        return;
    }
    if (m_restoringProject || m_offlineRenderActive) return;
    processPluginStateChanges();
    if (++m_pluginHealthPollDivider >= 10) {
        m_pluginHealthPollDivider = 0;
        refreshInspectorPluginHostStates(true);
    }

    AraHostStatus status;
    if (m_pluginHost->pollAraStatus(status)) {
        if (!status.available) {
            m_araDocumentDirty = false;
            m_araAnalysisWasActive = false;
        }
        if (status.documentDirty) {
            m_araDocumentDirty = true;
            setProjectDirty();
        }
        QString operation;
        float progress = 0.0f;
        if (status.unarchiving) {
            operation = QStringLiteral("Restoring");
            progress = status.unarchiveProgress;
        } else if (status.archiving) {
            operation = QStringLiteral("Saving");
            progress = status.archiveProgress;
        } else if (status.analysisActive) {
            operation = QStringLiteral("Analysis");
            progress = status.analysisProgress;
        }
        if (m_inspectorWidget)
            m_inspectorWidget->setAraStatus(
                status.available, m_araDocumentDirty, operation, progress);
        const bool analysisCompleted = m_araAnalysisWasActive && !status.analysisActive;
        m_araAnalysisWasActive = status.analysisActive;
        if (status.available && !status.analysisActive &&
            !status.archiving && !status.unarchiving &&
            (analysisCompleted || status.documentDirty))
            refreshSelectedAraProcessingAlgorithms();
    }

    AraTransportRequest request;
    if (!m_pluginHost->consumeAraTransportRequest(request)) return;

    const double fallbackTempo = m_transportWidget
        ? m_transportWidget->tempo() : kDefaultTempoBpm;
    const TempoClock clock(m_tempoMap, fallbackTempo);
    bool loopChanged = false;
    if (request.cycleRangeRequested) {
        const double startBeat = clock.beatAtSeconds(request.cycleStartSeconds);
        const double endBeat = clock.beatAtSeconds(
            request.cycleStartSeconds + request.cycleDurationSeconds);
        if (std::isfinite(startBeat) && std::isfinite(endBeat) &&
            startBeat >= 0.0 && endBeat - startBeat >= 0.000001) {
            if (std::fabs(m_loopStartBeat - startBeat) > 0.000001 ||
                std::fabs(m_loopEndBeat - endBeat) > 0.000001) {
                m_loopStartBeat = startBeat;
                m_loopEndBeat = endBeat;
                loopChanged = true;
            }
        }
    }
    if (request.cycleEnableRequested &&
        m_loopEnabled != request.cycleEnabled) {
        m_loopEnabled = request.cycleEnabled;
        loopChanged = true;
    }
    if (loopChanged) {
        if (m_playlistView)
            m_playlistView->setLoopRange(
                m_loopEnabled, m_loopStartBeat, m_loopEndBeat);
        if (audioGeneral)
            audioGeneral->audioCore.SetLoopRange(
                m_loopEnabled, m_loopStartBeat, m_loopEndBeat);
        setProjectDirty();
    }
    if (request.seekRequested)
        seekPlayhead(clock.beatAtSeconds(request.positionSeconds));
    if (request.stopRequested) {
        if (m_isPlaying) onTransportPause();
    } else if (request.startRequested && !m_isPlaying) {
        onTransportPlay();
    }
}

void VeraCoreQT::seekPlayhead(double beat)
{
    if (!std::isfinite(beat) || beat < 0.0) return;
    if (!m_stepMidiHeldKeys.empty() || !m_stepMidiChord.empty()) clearStepMidiChord();
    if (m_isRecordingAudio) stopAudioRecording(true);
    if (m_isRecordingMidi) {
        stopMidiRecording(true);
        if (m_transportWidget) m_transportWidget->setRecording(false);
    }
    if (m_pluginHost) m_pluginHost->panic();
    sendExternalMidiPanic();
    m_playheadBeat = beat;
    syncTransportDisplayToPlayhead();
    if (m_transportWidget) m_transportWidget->setPositionBeats(m_playheadBeat);
    if (m_playlistView) m_playlistView->setPlayheadBeat(m_playheadBeat);
    if (m_audioEditorWidget) m_audioEditorWidget->setPlayheadBeat(m_playheadBeat);
    if (audioGeneral && m_transportWidget) {
        audioGeneral->audioCore.SetTransportState(
            m_isPlaying, m_transportWidget->tempo(), m_playheadBeat);
    }
    if (m_isPlaying) sendTimelineMidiChaseToExternalOutputs(m_playheadBeat);
    syncPluginTransportToHost();
    updateMidiEditorPlayhead();
}

void VeraCoreQT::updateMidiEditorPlayhead()
{
    if (!m_midiEditor) return;
    if (!m_hasCurrentMidiClip || !std::isfinite(m_playheadBeat) ||
        m_playheadBeat < m_currentMidiClipStartBeat ||
        m_playheadBeat > m_currentMidiClipStartBeat + m_currentMidiClipLengthBeats) {
        m_midiEditor->setPlayheadBeat(-1.0);
        return;
    }
    m_midiEditor->setPlayheadBeat(m_currentMidiClipSourceOffsetBeats +
                                  m_playheadBeat - m_currentMidiClipStartBeat);
}

void VeraCoreQT::refreshMidiEditorExpressionMap()
{
    m_currentMidiExpressionMap.clear();
    if (!m_midiEditor) return;
    std::vector<MidiExpressionMapEntry> entries;
    const MidiClipDocument* doc = getCurrentSelectedClipDocument();
    if (!doc || !m_pluginHost || doc->trackIndex < 0 ||
        doc->trackIndex >= static_cast<int>(m_tracks.size())) {
        m_midiEditor->setExpressionMap(std::move(entries));
        return;
    }

    const MidiTrack* midiTrack = dynamic_cast<const MidiTrack*>(m_tracks[doc->trackIndex].get());
    if (!midiTrack) {
        m_midiEditor->setExpressionMap(std::move(entries));
        return;
    }
    const std::vector<LayerSlot>& layers = midiTrack->GetLayerSlots();
    const bool prefixLayerName = layers.size() > 1;
    for (int layerIndex = 0; layerIndex < static_cast<int>(layers.size()); ++layerIndex) {
        const LayerSlot& layer = layers[layerIndex];
        if (!layer.enabled) continue;
        const int layerId = doc->trackIndex * kLayerIdStride + layerIndex;
        const int channel = layer.midiChannel >= 0
            ? qBound(0, layer.midiChannel, 15)
            : m_midiEditor->currentMidiChannel();
        std::vector<MidiExpressionMapEntry> layerEntries =
            m_pluginHost->expressionMapForLayer(layerId, channel);
        for (MidiExpressionMapEntry& entry : layerEntries) {
            entry.pitch -= layer.transpose;
            if (!isPluginMidiPitch(entry.pitch)) continue;
            if (prefixLayerName && !layer.name.trimmed().isEmpty()) {
                entry.name = layer.name + ": " + entry.name;
                entry.shortName = layer.name + ": " + entry.shortName;
            }
            entries.push_back(std::move(entry));
        }
    }
    m_currentMidiExpressionMap = entries;
    m_midiEditor->setExpressionMap(std::move(entries));
}

void VeraCoreQT::refreshMidiEditorGhostClips()
{
    if (!m_midiEditor) return;
    std::vector<MidiGhostClip> ghosts;
    if (!m_playlistView || m_currentMidiClipId.isEmpty()) {
        m_midiEditor->setGhostClips(std::move(ghosts));
        return;
    }

    const std::vector<PlaylistView::MidiClipInfo> clips = m_playlistView->getMidiClips();
    const auto current = std::find_if(clips.begin(), clips.end(), [this](const PlaylistView::MidiClipInfo& clip) {
        return clip.clipId == m_currentMidiClipId;
    });
    if (current == clips.end() || !isFiniteBeatRange(current->startBeat, current->lengthBeats)) {
        m_midiEditor->setGhostClips(std::move(ghosts));
        return;
    }

    m_currentMidiClipStartBeat = current->startBeat;
    m_currentMidiClipLengthBeats = current->lengthBeats;
    m_currentMidiClipSourceOffsetBeats = qMax(0.0, current->sourceOffsetBeats);
    const auto currentDocument = m_midiClipDocs.find(m_currentMidiClipKey);
    if (currentDocument != m_midiClipDocs.end() && currentDocument->second) {
        currentDocument->second->startBeat = current->startBeat;
        currentDocument->second->lengthBeats = qMax(currentDocument->second->lengthBeats,
            current->sourceOffsetBeats + current->lengthBeats);
    }

    const double currentEnd = current->startBeat + current->lengthBeats;
    for (const PlaylistView::MidiClipInfo& clip : clips) {
        if (clip.clipId == current->clipId || !isFiniteBeatRange(clip.startBeat, clip.lengthBeats)) continue;
        const double clipEnd = clip.startBeat + clip.lengthBeats;
        if (clip.startBeat >= currentEnd || clipEnd <= current->startBeat) continue;
        const auto document = m_midiClipDocs.find(buildClipDocumentKey(clip.contentId));
        if (document == m_midiClipDocs.end() || !document->second) continue;

        MidiGhostClip ghost;
        ghost.clipId = clip.clipId;
        ghost.name = clip.name;
        ghost.trackIndex = clip.trackIndex;
        ghost.relativeStartBeat = current->sourceOffsetBeats + clip.startBeat - current->startBeat;
        ghost.lengthBeats = clip.lengthBeats;
        const double sourceStart = qMax(0.0, clip.sourceOffsetBeats);
        const double sourceEnd = sourceStart + clip.lengthBeats;
        for (const MidiNote& sourceNote : document->second->notes) {
            if (!isFiniteBeatRange(sourceNote.startBeat, sourceNote.lengthBeats)) continue;
            const double visibleStart = qMax(sourceStart, sourceNote.startBeat);
            const double visibleEnd = qMin(sourceEnd, sourceNote.startBeat + sourceNote.lengthBeats);
            if (visibleEnd <= visibleStart) continue;
            MidiNote note = sourceNote;
            note.startBeat = visibleStart - sourceStart;
            note.lengthBeats = visibleEnd - visibleStart;
            ghost.notes.push_back(std::move(note));
        }
        ghosts.push_back(std::move(ghost));
    }
    m_midiEditor->setGhostClips(std::move(ghosts));
    updateMidiEditorPlayhead();
}

double VeraCoreQT::arrangementLengthBeats() const
{
    double length = 4.0;
    for (const PlaylistView::MidiClipInfo& clip : m_playlistView->getMidiClips()) {
        if (clip.trackIndex < 0 || clip.trackIndex >= (int)m_tracks.size()) {
            continue;
        }
        if (m_tracks[clip.trackIndex]->GetType() != TrackType::Midi) {
            continue;
        }
        if (!isFiniteBeatRange(clip.startBeat, clip.lengthBeats)) {
            continue;
        }
        const double clipEnd = clip.startBeat + clip.lengthBeats;
        if (clipEnd > length) {
            length = clipEnd;
        }
    }
    for (const PlaylistView::AudioClipInfo& clip : m_playlistView->getAudioClips()) {
        if (clip.trackIndex < 0 || clip.trackIndex >= (int)m_tracks.size()) {
            continue;
        }
        if (m_tracks[clip.trackIndex]->GetType() != TrackType::Audio) {
            continue;
        }
        if (!isFiniteBeatRange(clip.startBeat, clip.lengthBeats)) {
            continue;
        }
        const double clipEnd = clip.startBeat + clip.lengthBeats;
        if (clipEnd > length) {
            length = clipEnd;
        }
    }
    return length;
}

QString VeraCoreQT::buildClipDocumentKey(const QString& contentId) const
{
    return contentId.trimmed();
}

void VeraCoreQT::syncProjectMetadataToMidiDocuments()
{
    if (!m_transportWidget) {
        return;
    }

    const double tempo = m_transportWidget->tempo();
    const int signatureNumerator = m_transportWidget->signatureNumerator();
    const int signatureDenominator = m_transportWidget->signatureDenominator();
    for (auto& entry : m_midiClipDocs) {
        MidiClipDocument* doc = entry.second.get();
        if (!doc) {
            continue;
        }
        doc->tempoBpm = tempo;
        doc->tempoMap = normalizedTempoMap({{0.0, tempo}}, tempo);
        doc->timeSignatureNumerator = signatureNumerator;
        doc->timeSignatureDenominator = signatureDenominator;
        doc->timeSignatureMap = normalizedTimeSignatureMap(
            {{0.0, signatureNumerator, signatureDenominator}},
            signatureNumerator, signatureDenominator);
    }

}

std::vector<VeraCoreBeatMidiEvent> VeraCoreQT::buildTimelineEvents() const
{
    std::vector<VeraCoreBeatMidiEvent> events;
    bool anySolo = false;
    for (const auto& track : m_tracks) {
        if (track && track->IsSoloed()) {
            anySolo = true;
            break;
        }
    }

    if (!m_playlistView) {
        return events;
    }
    for (const PlaylistView::MidiClipInfo& clip : m_playlistView->getMidiClips()) {
        if (!clipTakeIsActive(clip) || clip.muted || clip.trackIndex < 0 ||
            clip.trackIndex >= static_cast<int>(m_tracks.size()) ||
            !isFiniteBeatRange(clip.startBeat, clip.lengthBeats)) {
            continue;
        }
        const QString key = buildClipDocumentKey(clip.contentId);
        const auto documentIt = m_midiClipDocs.find(key);
        if (documentIt == m_midiClipDocs.end() || !documentIt->second) {
            continue;
        }
        const MidiClipDocument* doc = documentIt->second.get();
        MidiClipDocument loopedDocument;
        if (clip.loopEnabled) {
            loopedDocument = midiDocumentForClipExport(*doc, clip);
            doc = &loopedDocument;
        }
        const Track* sourceTrack = m_tracks[clip.trackIndex].get();
        if (!sourceTrack || sourceTrack->GetType() != TrackType::Midi) {
            continue;
        }
        if (sourceTrack->IsMuted() || (anySolo && !sourceTrack->IsSoloed())) {
            continue;
        }
        const MidiTrack* midiTrack = dynamic_cast<const MidiTrack*>(sourceTrack);
        if (!midiTrack) {
            continue;
        }
        const double sourceOffset = clip.loopEnabled ? 0.0 : (std::isfinite(clip.sourceOffsetBeats)
            ? qMax(0.0, clip.sourceOffsetBeats)
            : 0.0);

        auto addLayerEvents = [&](const MidiEvent& event, const LayerSlot& layer, int layerIndex) {
            if (!layer.enabled) {
                return;
            }
            const uint8_t statusHi = event.status & 0xF0;
            const int midiChannel = event.status & 0x0F;
            if (layer.midiChannel >= 0 && layer.midiChannel != midiChannel) {
                return;
            }
            uint8_t data1 = event.data1;
            if (statusHi == 0x90 || statusHi == 0x80 || statusHi == 0xA0) {
                const int transposedPitch = static_cast<int>(event.data1) + layer.transpose;
                if (!isPluginMidiPitch(transposedPitch)) {
                    return;
                }
                data1 = static_cast<uint8_t>(transposedPitch);
            }
            double localBeat = event.beat - sourceOffset;
            if (localBeat < 0.0) {
                return;
            }
            if (localBeat >= clip.lengthBeats) {
                const uint8_t eventType = event.status & 0xF0;
                const bool isNoteOff = eventType == 0x80 || (eventType == 0x90 && event.data2 == 0);
                if (!isNoteOff) {
                    return;
                }
                localBeat = clip.lengthBeats;
            }
            const double absoluteBeat = clip.startBeat + localBeat;
            if (!std::isfinite(absoluteBeat) || absoluteBeat < 0.0) {
                return;
            }
            const int layerId = clip.trackIndex * kLayerIdStride + layerIndex;
            const bool externalPlugin = m_pluginHost && m_pluginHost->isLayerHosted(layerId);
            // FL-style slide notes are native synth control events, never MIDI notes.
            if (event.isSlide && externalPlugin) {
                return;
            }
            const float eventGain = static_cast<float>(finiteClamp(
                static_cast<double>(layer.gain), 0.0, 2.0, 1.0));
            const float eventPan = static_cast<float>(finiteClamp(
                static_cast<double>(layer.pan), -1.0, 1.0, 0.0));
            events.push_back({absoluteBeat,
                              event.status,
                              data1,
                              event.data2,
                              layerId,
                              eventGain,
                              eventPan,
                              externalPlugin,
                              event.isSlide,
                              event.isPortamento && !externalPlugin,
                              event.glideDurationBeats,
                              event.isKeyswitch});
        };

        const std::vector<MidiEvent> sourceEvents = doc->buildMidiEvents();
        std::map<int, std::vector<MidiEvent>> activeNotes;
        std::map<int, MidiEvent> channelState;
        std::map<int, MidiEvent> keyswitchState;
        std::vector<MidiEvent> playbackEvents;
        playbackEvents.reserve(sourceEvents.size());
        for (const MidiEvent& sourceEvent : sourceEvents) {
            if (sourceEvent.beat > sourceOffset) {
                playbackEvents.push_back(sourceEvent);
                continue;
            }
            const uint8_t statusHi = sourceEvent.status & 0xF0;
            const int channel = static_cast<int>(sourceEvent.status & 0x0F);
            const int noteKey = (channel << 8) |
                static_cast<int>(sourceEvent.data1);
            if (sourceEvent.isKeyswitch) {
                if (statusHi == 0x90 && sourceEvent.data2 > 0)
                    keyswitchState[channel] = sourceEvent;
            } else if (sourceEvent.isSlide) {
                if (sourceEvent.beat + sourceEvent.glideDurationBeats > sourceOffset) {
                    MidiEvent chase = sourceEvent;
                    chase.beat = sourceOffset;
                    chase.glideDurationBeats = sourceEvent.beat + sourceEvent.glideDurationBeats - sourceOffset;
                    playbackEvents.push_back(chase);
                }
            } else if (statusHi == 0x90 && sourceEvent.data2 > 0) {
                activeNotes[noteKey].push_back(sourceEvent);
            } else if (statusHi == 0x80 || (statusHi == 0x90 && sourceEvent.data2 == 0)) {
                auto active = activeNotes.find(noteKey);
                if (active != activeNotes.end() && !active->second.empty()) {
                    active->second.pop_back();
                    if (active->second.empty()) activeNotes.erase(active);
                }
            } else {
                const int stateKey = (statusHi == 0xA0 || statusHi == 0xB0)
                    ? ((static_cast<int>(sourceEvent.status) << 8) | static_cast<int>(sourceEvent.data1))
                    : (static_cast<int>(sourceEvent.status) << 8);
                channelState[stateKey] = sourceEvent;
            }
        }
        for (const auto& state : channelState) {
            MidiEvent chase = state.second;
            chase.beat = sourceOffset;
            playbackEvents.push_back(chase);
        }
        for (const auto& state : keyswitchState) {
            MidiEvent chaseOn = state.second;
            chaseOn.beat = sourceOffset;
            playbackEvents.push_back(chaseOn);
            MidiEvent chaseOff = chaseOn;
            chaseOff.beat = sourceOffset + 1.0e-6;
            chaseOff.status = static_cast<uint8_t>(0x80 | state.first);
            chaseOff.data2 = 0;
            playbackEvents.push_back(chaseOff);
        }
        for (const auto& active : activeNotes) {
            for (MidiEvent chase : active.second) {
                if (chase.beat < sourceOffset) chase.isPortamento = false;
                chase.beat = sourceOffset;
                playbackEvents.push_back(chase);
            }
        }
        std::stable_sort(playbackEvents.begin(), playbackEvents.end(), [](const MidiEvent& a, const MidiEvent& b) {
            if (a.beat != b.beat) return a.beat < b.beat;
            const int aRank = midiClipEventSortRank(a.status, a.data2);
            const int bRank = midiClipEventSortRank(b.status, b.data2);
            return aRank != bRank ? aRank < bRank : a.status < b.status;
        });

        for (const MidiEvent& event : playbackEvents) {
            const uint8_t statusHi = event.status & 0xF0;
            if (!isValidMidiChannelEvent(event.status, event.data1, event.data2)) {
                continue;
            }
            if ((statusHi == 0x90 || statusHi == 0x80) && !isPluginMidiPitch(event.data1)) {
                continue;
            }

            const std::vector<LayerSlot>& layers = midiTrack->GetLayerSlots();
            if (layers.empty()) {
                LayerSlot fallback;
                fallback.name = "Internal Synth";
                addLayerEvents(event, fallback, 0);
                continue;
            }
            for (int layerIndex = 0; layerIndex < static_cast<int>(layers.size()); ++layerIndex) {
                addLayerEvents(event, layers[layerIndex], layerIndex);
            }
        }
    }

    std::stable_sort(events.begin(), events.end(), [](const VeraCoreBeatMidiEvent& a, const VeraCoreBeatMidiEvent& b) {
        if (a.beat != b.beat) return a.beat < b.beat;
        if (a.isPortamento != b.isPortamento) return a.isPortamento;
        const int aRank = midiEventSortRank(a.status, a.data2);
        const int bRank = midiEventSortRank(b.status, b.data2);
        if (aRank != bRank) return aRank < bRank;
        if (a.isSlide != b.isSlide) return !a.isSlide;
        if (a.layerId != b.layerId) return a.layerId < b.layerId;
        if (a.status != b.status) return a.status < b.status;
        return a.data1 < b.data1;
    });
    return events;
}

void VeraCoreQT::sendTimelineMidiToExternalOutputs(double startBeat, double endBeat) const
{
    if (!audioGeneral ||
        !std::isfinite(startBeat) ||
        !std::isfinite(endBeat) ||
        endBeat <= startBeat) {
        return;
    }

    const std::vector<VeraCoreBeatMidiEvent> events = buildTimelineEvents();
    for (const VeraCoreBeatMidiEvent& event : events) {
        if (event.isSlide) continue;
        if (event.beat < startBeat || event.beat >= endBeat) {
            continue;
        }

        VeraCoreMidiEvent midiEvent;
        midiEvent.status = event.status;
        midiEvent.data1 = event.data1;
        midiEvent.data2 = event.data2;
        midiEvent.layerId = event.layerId;
        midiEvent.gain = event.gain;
        midiEvent.pan = event.pan;
        midiEvent.fromTimeline = true;
        audioGeneral->SendMidiToOutputs(midiEvent);
    }
}

void VeraCoreQT::sendTimelineMidiChaseToExternalOutputs(double beat) const
{
    if (!audioGeneral || !std::isfinite(beat) || beat < 0.0) return;

    using NoteKey = std::tuple<int, int, int>;
    using StateKey = std::tuple<int, int, int>;
    struct ActiveNoteState {
        VeraCoreBeatMidiEvent event;
        int count = 0;
    };
    std::map<NoteKey, ActiveNoteState> activeNotes;
    std::map<StateKey, VeraCoreBeatMidiEvent> channelState;
    std::map<std::pair<int, int>, VeraCoreBeatMidiEvent> keyswitchState;
    for (const VeraCoreBeatMidiEvent& event : buildTimelineEvents()) {
        if (event.beat >= beat) break;
        if (event.isSlide) continue;
        const int statusHi = event.status & 0xF0;
        const int channel = event.status & 0x0F;
        const bool noteOn = statusHi == 0x90 && event.data2 > 0;
        const bool noteOff = statusHi == 0x80 || (statusHi == 0x90 && event.data2 == 0);
        if (noteOn || noteOff) {
            if (event.isKeyswitch) {
                if (noteOn) keyswitchState[{event.layerId, channel}] = event;
                continue;
            }
            ActiveNoteState& state = activeNotes[{event.layerId, channel, event.data1}];
            if (noteOn) {
                state.event = event;
                if (state.count < std::numeric_limits<int>::max()) ++state.count;
            } else if (state.count > 0) {
                --state.count;
            }
            continue;
        }
        if (statusHi >= 0xA0 && statusHi <= 0xE0) {
            const int controller = (statusHi == 0xA0 || statusHi == 0xB0) ? event.data1 : 0;
            channelState[{event.layerId, event.status, controller}] = event;
        }
    }

    constexpr size_t kMaxExternalChaseEvents = 8192;
    size_t sentEvents = 0;
    auto send = [&](const VeraCoreBeatMidiEvent& event) {
        if (sentEvents >= kMaxExternalChaseEvents) return false;
        VeraCoreMidiEvent midiEvent;
        midiEvent.status = event.status;
        midiEvent.data1 = event.data1;
        midiEvent.data2 = event.data2;
        midiEvent.layerId = event.layerId;
        midiEvent.gain = event.gain;
        midiEvent.pan = event.pan;
        midiEvent.fromTimeline = true;
        midiEvent.isKeyswitch = event.isKeyswitch;
        audioGeneral->SendMidiToOutputs(midiEvent);
        ++sentEvents;
        return true;
    };
    for (const auto& state : channelState)
        if (!send(state.second)) return;
    for (const auto& state : keyswitchState) {
        if (!send(state.second)) return;
        VeraCoreBeatMidiEvent noteOff = state.second;
        noteOff.status = static_cast<uint8_t>(0x80 | std::get<1>(state.first));
        noteOff.data2 = 0;
        if (!send(noteOff)) return;
    }
    for (const auto& active : activeNotes) {
        for (int count = 0; count < active.second.count; ++count)
            if (!send(active.second.event)) return;
    }
}

void VeraCoreQT::sendExternalMidiPanic() const
{
    if (!audioGeneral) {
        return;
    }

    for (int channel = 0; channel < 16; ++channel) {
        VeraCoreMidiEvent allSoundOff;
        allSoundOff.status = static_cast<uint8_t>(0xB0 | channel);
        allSoundOff.data1 = 120;
        allSoundOff.data2 = 0;
        allSoundOff.fromTimeline = true;
        audioGeneral->SendMidiToOutputs(allSoundOff);

        VeraCoreMidiEvent allNotesOff = allSoundOff;
        allNotesOff.data1 = 123;
        audioGeneral->SendMidiToOutputs(allNotesOff);
    }
}

const MidiClipDocument* VeraCoreQT::getCurrentSelectedClipDocument() const
{
    if (!m_hasCurrentMidiClip || m_currentMidiClipKey.isEmpty()) {
        return nullptr;
    }

    const auto it = m_midiClipDocs.find(m_currentMidiClipKey);
    if (it != m_midiClipDocs.end() && it->second) {
        return it->second.get();
    }

    return nullptr;
}

MidiClipDocument* VeraCoreQT::getCurrentSelectedClipDocument()
{
    return const_cast<MidiClipDocument*>(
        static_cast<const VeraCoreQT*>(this)->getCurrentSelectedClipDocument());
}

void VeraCoreQT::clearStepMidiChord()
{
    m_stepMidiHeldKeys.clear();
    m_stepMidiChord.clear();
    m_stepMidiContentId.clear();
    m_stepMidiProjectBeat = 0.0;
    m_stepMidiClipStartBeat = 0.0;
    m_stepMidiClipLengthBeats = 0.0;
    m_stepMidiClipSourceOffsetBeats = 0.0;
    m_stepMidiStepBeats = 0.25;
    m_stepMidiChordGatePercent = m_stepMidiGatePercent;
    m_stepMidiChordVelocityOverride = m_stepMidiVelocityOverride;
    m_stepMidiChordInsertMode = m_stepMidiInsertMode;
}

bool VeraCoreQT::moveStepInputCursor(int stepDelta)
{
    if (!m_midiStepInputEnabled) {
        ui.statusBar->showMessage("Enable MIDI step input first.", 2200);
        return false;
    }
    if (m_isPlaying || m_isRecordingMidi || m_isRecordingAudio || stepDelta == 0) {
        ui.statusBar->showMessage("Stop transport and recording before moving step input.", 2500);
        return false;
    }
    if (!m_stepMidiHeldKeys.empty() || !m_stepMidiChord.empty()) {
        ui.statusBar->showMessage("Release held MIDI notes before moving step input.", 2500);
        return false;
    }
    const MidiClipDocument* doc = getCurrentSelectedClipDocument();
    if (!doc || !isFiniteBeatRange(m_currentMidiClipStartBeat,
                                   m_currentMidiClipLengthBeats)) {
        ui.statusBar->showMessage("Select a MIDI clip before moving step input.", 2500);
        return false;
    }
    const double step = m_projectSnapBeats > 1.0e-9 ? m_projectSnapBeats : 0.25;
    const double local = qBound(0.0, m_playheadBeat - m_currentMidiClipStartBeat,
                                m_currentMidiClipLengthBeats);
    const double snapped = std::round(local / step) * step;
    const double targetLocal = qBound(0.0, snapped + stepDelta * step,
                                      m_currentMidiClipLengthBeats);
    const double targetBeat = m_currentMidiClipStartBeat + targetLocal;
    if (std::fabs(targetBeat - m_playheadBeat) <= 1.0e-9) {
        ui.statusBar->showMessage(stepDelta > 0
            ? "Step input reached the end of the MIDI clip."
            : "Step input reached the start of the MIDI clip.", 2200);
        return false;
    }
    seekPlayhead(targetBeat);
    ui.statusBar->showMessage(stepDelta > 0
        ? "Step input: rest inserted."
        : "Step input: moved back.", 1800);
    return true;
}

bool VeraCoreQT::tieStepInputNotes()
{
    if (!m_midiStepInputEnabled) {
        ui.statusBar->showMessage("Enable MIDI step input first.", 2200);
        return false;
    }
    if (m_isPlaying || m_isRecordingMidi || m_isRecordingAudio) {
        ui.statusBar->showMessage("Stop transport and recording before tying step input.", 2500);
        return false;
    }
    if (!m_stepMidiHeldKeys.empty() || !m_stepMidiChord.empty()) {
        ui.statusBar->showMessage("Release held MIDI notes before tying step input.", 2500);
        return false;
    }
    MidiClipDocument* doc = getCurrentSelectedClipDocument();
    if (!doc || !isFiniteBeatRange(m_currentMidiClipStartBeat,
                                   m_currentMidiClipLengthBeats)) {
        ui.statusBar->showMessage("Select a MIDI clip before tying step input.", 2500);
        return false;
    }

    constexpr double epsilon = 1.0e-6;
    const double step = m_projectSnapBeats > 1.0e-9 ? m_projectSnapBeats : 0.25;
    const double local = qBound(0.0, m_playheadBeat - m_currentMidiClipStartBeat,
                                m_currentMidiClipLengthBeats);
    const double cursorLocal = qBound(0.0, std::round(local / step) * step,
                                      m_currentMidiClipLengthBeats);
    if (cursorLocal <= epsilon) {
        ui.statusBar->showMessage("Step input reached the start of the MIDI clip.", 2200);
        return false;
    }
    if (cursorLocal >= m_currentMidiClipLengthBeats - epsilon) {
        ui.statusBar->showMessage("Step input reached the end of the MIDI clip.", 2200);
        return false;
    }

    const double extension = qMin(step, m_currentMidiClipLengthBeats - cursorLocal);
    const double sourceCursor = m_currentMidiClipSourceOffsetBeats + cursorLocal;
    const double sourceLimit = m_currentMidiClipSourceOffsetBeats +
        m_currentMidiClipLengthBeats;
    const MidiClipDocument before = *doc;
    MidiClipDocument after = before;
    int tied = 0;
    double lastLength = 0.0;
    for (MidiNote& note : after.notes) {
        if (!std::isfinite(note.startBeat) || !std::isfinite(note.lengthBeats) ||
            note.lengthBeats <= epsilon) continue;
        const double noteEnd = note.startBeat + note.lengthBeats;
        if (std::fabs(noteEnd - sourceCursor) > epsilon) continue;
        const double extendedEnd = qMin(sourceLimit, noteEnd + extension);
        if (extendedEnd <= noteEnd + epsilon) continue;
        note.lengthBeats = extendedEnd - note.startBeat;
        lastLength = qMax(lastLength, note.lengthBeats);
        ++tied;
    }
    if (tied == 0) {
        ui.statusBar->showMessage("No MIDI notes end at the step-input cursor.", 2500);
        return false;
    }
    after.lastNoteLengthBeats = lastLength;

    const QString contentId = m_currentMidiClipKey;
    const double nextBeat = m_currentMidiClipStartBeat + cursorLocal + extension;
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    *doc = after;
    if (m_midiEditor && m_currentMidiClipKey == contentId) m_midiEditor->setClip(doc);
    const auto applyDocument = [this, contentId](const MidiClipDocument& state) {
        applyMidiDocumentState(contentId, state);
        syncMidiTimelineToAudio();
        refreshMidiEditorGhostClips();
    };
    const bool committed = captureUndo && commitProjectUndoStep(beforeStateId,
        nonArrangementDirtyBefore, QString(),
        [applyDocument, before]() { applyDocument(before); },
        [applyDocument, after]() { applyDocument(after); });
    if (!committed) {
        if (captureUndo && m_playlistView) m_playlistView->cancelArrangementTransaction();
        setProjectDirty();
    }
    syncMidiTimelineToAudio();
    refreshMidiEditorGhostClips();
    seekPlayhead(nextBeat);
    ui.statusBar->showMessage(QString("Step input: tied %1 note%2.")
        .arg(tied).arg(tied == 1 ? "" : "s"), 2200);
    return true;
}

bool VeraCoreQT::handleStepMidiInput(const VeraCoreMidiEvent& event)
{
    if (!m_midiStepInputEnabled || m_isPlaying || m_isRecordingMidi ||
        m_isRecordingAudio || event.fromTimeline ||
        !isValidMidiChannelEvent(event.status, event.data1, event.data2)) return false;
    const uint8_t statusHi = event.status & 0xF0;
    const bool noteOn = statusHi == 0x90 && event.data2 > 0;
    const bool noteOff = statusHi == 0x80 || (statusHi == 0x90 && event.data2 == 0);
    if ((!noteOn && !noteOff) || !isPluginMidiPitch(event.data1)) return false;
    const int channel = event.status & 0x0F;
    const int key = channel * 128 + event.data1;
    if (noteOn) {
        if (m_stepMidiHeldKeys.empty()) {
            clearStepMidiChord();
            MidiClipDocument* doc = getCurrentSelectedClipDocument();
            if (!doc || !isFiniteBeatRange(m_currentMidiClipStartBeat,
                                           m_currentMidiClipLengthBeats)) {
                ui.statusBar->showMessage(
                    "Select a MIDI clip before using step input.", 3000);
                return true;
            }
            const double clipEnd = m_currentMidiClipStartBeat +
                m_currentMidiClipLengthBeats;
            const double step = m_projectSnapBeats > 1.0e-9
                ? m_projectSnapBeats : 0.25;
            const double localBeat = qBound(0.0,
                m_playheadBeat - m_currentMidiClipStartBeat,
                m_currentMidiClipLengthBeats);
            const double snappedLocal = qBound(0.0,
                std::round(localBeat / step) * step,
                m_currentMidiClipLengthBeats);
            m_stepMidiProjectBeat = m_currentMidiClipStartBeat + snappedLocal;
            if (localBeat >= m_currentMidiClipLengthBeats - 1.0e-9 ||
                m_stepMidiProjectBeat >= clipEnd - 1.0e-9) {
                ui.statusBar->showMessage("Step input reached the end of the MIDI clip.", 2500);
                return true;
            }
            m_stepMidiContentId = m_currentMidiClipKey;
            m_stepMidiClipStartBeat = m_currentMidiClipStartBeat;
            m_stepMidiClipLengthBeats = m_currentMidiClipLengthBeats;
            m_stepMidiClipSourceOffsetBeats = m_currentMidiClipSourceOffsetBeats;
            m_stepMidiStepBeats = step;
            m_stepMidiChordGatePercent = m_stepMidiGatePercent;
            m_stepMidiChordVelocityOverride = m_stepMidiVelocityOverride;
            m_stepMidiChordInsertMode = m_stepMidiInsertMode;
        }
        int& holdCount = m_stepMidiHeldKeys[key];
        if (holdCount++ == 0) {
            const bool isKeyswitch = std::any_of(
                m_currentMidiExpressionMap.begin(), m_currentMidiExpressionMap.end(),
                [channel, &event](const MidiExpressionMapEntry& entry) {
                    return entry.channel == channel && entry.pitch == event.data1;
                });
            m_stepMidiChord.push_back({channel, event.data1,
                qBound(1, static_cast<int>(event.data2), 127), isKeyswitch});
        }
        return true;
    }

    auto held = m_stepMidiHeldKeys.find(key);
    if (held == m_stepMidiHeldKeys.end()) return true;
    if (--held->second <= 0) m_stepMidiHeldKeys.erase(held);
    if (m_stepMidiHeldKeys.empty()) commitStepMidiChord();
    return true;
}

bool VeraCoreQT::commitStepMidiChord()
{
    if (m_stepMidiChord.empty() || m_stepMidiContentId.isEmpty()) {
        clearStepMidiChord();
        return false;
    }
    const auto document = m_midiClipDocs.find(m_stepMidiContentId);
    MidiClipDocument* doc = document != m_midiClipDocs.end() && document->second
        ? document->second.get() : nullptr;
    if (!doc || !isFiniteBeatRange(m_stepMidiClipStartBeat,
                                   m_stepMidiClipLengthBeats)) {
        clearStepMidiChord();
        return false;
    }
    const double step = std::isfinite(m_stepMidiStepBeats) &&
        m_stepMidiStepBeats > 1.0e-9 ? m_stepMidiStepBeats : 0.25;
    const double localBeat = qBound(0.0,
        m_stepMidiProjectBeat - m_stepMidiClipStartBeat,
        m_stepMidiClipLengthBeats);
    const int gatePercent = qBound(25, m_stepMidiChordGatePercent, 200);
    const int velocityOverride = qBound(0, m_stepMidiChordVelocityOverride, 127);
    const int insertMode = qBound(0, m_stepMidiChordInsertMode, 1);
    const double duration = qMin(step * static_cast<double>(gatePercent) / 100.0,
                                 m_stepMidiClipLengthBeats - localBeat);
    if (duration <= 1.0e-9) {
        clearStepMidiChord();
        return false;
    }
    const int articulationCount = static_cast<int>(std::count_if(
        m_stepMidiChord.begin(), m_stepMidiChord.end(),
        [](const StepMidiChordNote& note) { return note.isKeyswitch; }));
    const int playableCount = static_cast<int>(m_stepMidiChord.size()) - articulationCount;
    const double keyswitchDuration = qMin(step, m_stepMidiClipLengthBeats - localBeat);
    const MidiClipDocument before = *doc;
    MidiClipDocument after = before;
    int replaced = 0;
    if (insertMode == 1) {
        constexpr double epsilon = 1.0e-9;
        const double sourceStepStart = m_stepMidiClipSourceOffsetBeats + localBeat;
        const double sourceStepEnd = sourceStepStart +
            qMin(step, m_stepMidiClipLengthBeats - localBeat);
        const size_t previousCount = after.notes.size();
        after.notes.erase(std::remove_if(after.notes.begin(), after.notes.end(),
            [sourceStepStart, sourceStepEnd, epsilon, playableCount](const MidiNote& note) {
                const bool replaceType = playableCount > 0 || note.isKeyswitch;
                return replaceType && std::isfinite(note.startBeat) &&
                    note.startBeat >= sourceStepStart - epsilon &&
                    note.startBeat < sourceStepEnd;
            }), after.notes.end());
        replaced = static_cast<int>(previousCount - after.notes.size());
    }
    for (const StepMidiChordNote& chordNote : m_stepMidiChord) {
        MidiNote note;
        note.pitch = qBound(0, chordNote.pitch, 127);
        note.startBeat = m_stepMidiClipSourceOffsetBeats + localBeat;
        note.lengthBeats = chordNote.isKeyswitch ? keyswitchDuration : duration;
        note.velocity = velocityOverride > 0
            ? velocityOverride : qBound(1, chordNote.velocity, 127);
        note.colorGroup = qBound(0, chordNote.channel, 15);
        note.isKeyswitch = chordNote.isKeyswitch;
        after.notes.push_back(note);
    }
    after.lastNoteLengthBeats = playableCount > 0 ? duration : keyswitchDuration;
    std::stable_sort(after.notes.begin(), after.notes.end(), [](const MidiNote& left,
                                                                const MidiNote& right) {
        if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
        return left.pitch < right.pitch;
    });
    const QString contentId = m_stepMidiContentId;
    const double nextBeat = qMin(m_stepMidiClipStartBeat + m_stepMidiClipLengthBeats,
        m_stepMidiProjectBeat + (playableCount > 0 ? step : 0.0));
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    *doc = after;
    if (m_midiEditor && m_currentMidiClipKey == contentId) m_midiEditor->setClip(doc);
    const auto applyDocument = [this, contentId](const MidiClipDocument& state) {
        applyMidiDocumentState(contentId, state);
        syncMidiTimelineToAudio();
        refreshMidiEditorGhostClips();
    };
    const bool committed = captureUndo && commitProjectUndoStep(beforeStateId,
        nonArrangementDirtyBefore, QString(),
        [applyDocument, before]() { applyDocument(before); },
        [applyDocument, after]() { applyDocument(after); });
    if (!committed) {
        if (captureUndo && m_playlistView) m_playlistView->cancelArrangementTransaction();
        setProjectDirty();
    }
    syncMidiTimelineToAudio();
    refreshMidiEditorGhostClips();
    const int inserted = static_cast<int>(m_stepMidiChord.size());
    clearStepMidiChord();
    if (nextBeat > m_playheadBeat + 1.0e-9) seekPlayhead(nextBeat);
    QString result;
    if (playableCount == 0) {
        result = QString("Step input: inserted %1 articulation%2.")
            .arg(articulationCount).arg(articulationCount == 1 ? "" : "s");
    } else if (articulationCount > 0) {
        result = QString("Step input: inserted %1 note%2 and %3 articulation%4.")
            .arg(playableCount).arg(playableCount == 1 ? "" : "s")
            .arg(articulationCount).arg(articulationCount == 1 ? "" : "s");
    } else {
        result = QString("Step input: inserted %1 note%2.")
            .arg(inserted).arg(inserted == 1 ? "" : "s");
    }
    if (replaced > 0) result.prepend(QString("Replaced %1. ").arg(replaced));
    ui.statusBar->showMessage(result, 2200);
    return true;
}

void VeraCoreQT::bufferRetrospectiveMidiInput(const VeraCoreMidiEvent& event)
{
    if (m_isRecordingMidi || event.fromTimeline ||
        !isValidMidiChannelEvent(event.status, event.data1, event.data2) ||
        !std::isfinite(event.captureMonotonicSeconds) ||
        event.captureMonotonicSeconds < 0.0) return;
    const uint8_t statusHi = event.status & 0xF0;
    if (statusHi < 0x80 || statusHi > 0xE0) return;

    if (!m_retrospectiveMidiBuffer.empty()) {
        const RetrospectiveMidiCapture& previous = m_retrospectiveMidiBuffer.back();
        if (event.captureMonotonicSeconds < previous.event.captureMonotonicSeconds ||
            (m_isPlaying && previous.transportRunning &&
             std::isfinite(event.captureBeat) && std::isfinite(previous.event.captureBeat) &&
             event.captureBeat + 1.0e-6 < previous.event.captureBeat))
            m_retrospectiveMidiBuffer.clear();
    }
    m_retrospectiveMidiBuffer.push_back({event, m_isPlaying});
    constexpr size_t maximumEvents = 65536;
    constexpr double maximumAgeSeconds = 120.0;
    while (!m_retrospectiveMidiBuffer.empty() &&
           (m_retrospectiveMidiBuffer.size() > maximumEvents ||
            event.captureMonotonicSeconds -
                m_retrospectiveMidiBuffer.front().event.captureMonotonicSeconds >
                    maximumAgeSeconds))
        m_retrospectiveMidiBuffer.pop_front();
}

bool VeraCoreQT::captureRetrospectiveMidi()
{
    if (m_isRecordingMidi || m_isRecordingAudio) {
        ui.statusBar->showMessage("Stop recording before capturing retrospective MIDI.", 3000);
        return false;
    }
    MidiClipDocument* doc = getCurrentSelectedClipDocument();
    if (!doc || !isFiniteBeatRange(m_currentMidiClipStartBeat,
                                   m_currentMidiClipLengthBeats)) {
        ui.statusBar->showMessage("Select a MIDI clip before capturing MIDI.", 3000);
        return false;
    }
    if (m_retrospectiveMidiBuffer.empty()) {
        ui.statusBar->showMessage("No unrecorded MIDI performance is available.", 2500);
        return false;
    }

    size_t begin = m_retrospectiveMidiBuffer.size() - 1u;
    const bool transportRunning = m_retrospectiveMidiBuffer.back().transportRunning;
    while (begin > 0u) {
        const RetrospectiveMidiCapture& previous = m_retrospectiveMidiBuffer[begin - 1u];
        const RetrospectiveMidiCapture& current = m_retrospectiveMidiBuffer[begin];
        const double gap = current.event.captureMonotonicSeconds -
            previous.event.captureMonotonicSeconds;
        if (previous.transportRunning != transportRunning || !std::isfinite(gap) ||
            gap < 0.0 || gap > 5.0 ||
            (transportRunning && current.event.captureBeat + 1.0e-6 <
                                    previous.event.captureBeat)) break;
        --begin;
    }
    const size_t end = m_retrospectiveMidiBuffer.size();
    const double clipStart = m_currentMidiClipStartBeat;
    const double clipEnd = clipStart + m_currentMidiClipLengthBeats;
    const double firstTime = m_retrospectiveMidiBuffer[begin].event.captureMonotonicSeconds;
    const double lastTime = m_retrospectiveMidiBuffer[end - 1u].event.captureMonotonicSeconds;
    const double fallbackTempo = m_transportWidget
        ? m_transportWidget->tempo() : kDefaultTempoBpm;
    const TempoClock clock(m_tempoMap, fallbackTempo);
    const double anchor = qBound(clipStart, m_playheadBeat, clipEnd);
    const double stoppedDurationBeats = qMax(0.0, lastTime - firstTime) *
        clock.bpmAtBeat(anchor) / 60.0;
    const double stoppedBaseBeat = anchor + stoppedDurationBeats <= clipEnd
        ? anchor : qMax(clipStart, anchor - stoppedDurationBeats);
    const auto mappedBeat = [&](const RetrospectiveMidiCapture& capture) {
        if (transportRunning && std::isfinite(capture.event.captureBeat))
            return capture.event.captureBeat;
        return stoppedBaseBeat + qMax(0.0,
            capture.event.captureMonotonicSeconds - firstTime) *
                clock.bpmAtBeat(stoppedBaseBeat) / 60.0;
    };

    struct PendingStart { double beat = 0.0; int velocity = 100; };
    std::map<int, std::deque<PendingStart>> pending;
    MidiClipDocument after = *doc;
    int addedNotes = 0;
    int addedEvents = 0;
    double finalMappedBeat = clipStart;
    auto appendNote = [&](int channel, int pitch, const PendingStart& start,
                          double endBeat) {
        const double boundedStart = qBound(clipStart, start.beat, clipEnd);
        const double boundedEnd = qBound(clipStart, endBeat, clipEnd);
        if (boundedEnd - boundedStart < 1.0e-6) return;
        MidiNote note;
        note.pitch = qBound(0, pitch, 127);
        note.startBeat = m_currentMidiClipSourceOffsetBeats + boundedStart - clipStart;
        note.lengthBeats = boundedEnd - boundedStart;
        note.velocity = qBound(1, start.velocity, 127);
        note.colorGroup = qBound(0, channel, 15);
        after.lastNoteLengthBeats = note.lengthBeats;
        after.notes.push_back(note);
        ++addedNotes;
    };

    for (size_t index = begin; index < end; ++index) {
        const RetrospectiveMidiCapture& capture = m_retrospectiveMidiBuffer[index];
        const VeraCoreMidiEvent& event = capture.event;
        const double beat = mappedBeat(capture);
        if (!std::isfinite(beat)) continue;
        finalMappedBeat = qMax(finalMappedBeat, beat);
        const uint8_t statusHi = event.status & 0xF0;
        const int channel = event.status & 0x0F;
        const bool noteOn = statusHi == 0x90 && event.data2 > 0;
        const bool noteOff = statusHi == 0x80 || (statusHi == 0x90 && event.data2 == 0);
        const int noteKey = channel * 128 + event.data1;
        if (noteOn && isPluginMidiPitch(event.data1)) {
            pending[noteKey].push_back({beat, qBound(1, static_cast<int>(event.data2), 127)});
        } else if (noteOff && isPluginMidiPitch(event.data1)) {
            auto found = pending.find(noteKey);
            if (found == pending.end() || found->second.empty()) continue;
            const PendingStart start = found->second.front();
            found->second.pop_front();
            if (found->second.empty()) pending.erase(found);
            appendNote(channel, event.data1, start, beat);
        } else if (beat >= clipStart - 1.0e-9 && beat <= clipEnd + 1.0e-9) {
            after.performanceEvents.push_back({
                m_currentMidiClipSourceOffsetBeats + qBound(clipStart, beat, clipEnd) - clipStart,
                event.status, event.data1, event.data2});
            ++addedEvents;
        }
    }
    for (const auto& entry : pending) {
        const int channel = qBound(0, entry.first / 128, 15);
        const int pitch = qBound(0, entry.first % 128, 127);
        for (const PendingStart& start : entry.second) {
            const double fallbackEnd = qMax(finalMappedBeat,
                start.beat + qMax(1.0 / 960.0, after.lastNoteLengthBeats));
            appendNote(channel, pitch, start, fallbackEnd);
        }
    }
    if (addedNotes == 0 && addedEvents == 0) {
        ui.statusBar->showMessage("Captured MIDI falls outside the selected clip.", 3000);
        return false;
    }
    std::stable_sort(after.notes.begin(), after.notes.end(), [](const MidiNote& left,
                                                                const MidiNote& right) {
        if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
        return left.pitch < right.pitch;
    });
    std::stable_sort(after.performanceEvents.begin(), after.performanceEvents.end(),
        [](const MidiEvent& left, const MidiEvent& right) {
            return left.beat < right.beat;
        });

    const MidiClipDocument before = *doc;
    const QString contentId = m_currentMidiClipKey;
    const bool captureUndo = m_playlistView && !m_restoringProject &&
        !m_suppressProjectUndoCapture;
    const quint64 beforeStateId = captureUndo
        ? m_playlistView->currentArrangementStateId() : 0;
    const bool nonArrangementDirtyBefore = m_nonArrangementDirty;
    if (captureUndo) {
        m_playlistView->beginArrangementTransaction();
        m_playlistView->markArrangementTransactionChanged();
    }
    *doc = after;
    if (m_midiEditor) m_midiEditor->setClip(doc);
    if (captureUndo) {
        const auto applyDocument = [this, contentId](const MidiClipDocument& state) {
            applyMidiDocumentState(contentId, state);
            syncMidiTimelineToAudio();
            refreshMidiEditorGhostClips();
        };
        if (!commitProjectUndoStep(beforeStateId, nonArrangementDirtyBefore, QString(),
                [applyDocument, before]() { applyDocument(before); },
                [applyDocument, after]() { applyDocument(after); }))
            setProjectDirty();
    } else {
        setProjectDirty();
    }
    syncMidiTimelineToAudio();
    refreshMidiEditorGhostClips();
    m_retrospectiveMidiBuffer.erase(
        m_retrospectiveMidiBuffer.begin() + static_cast<ptrdiff_t>(begin),
        m_retrospectiveMidiBuffer.begin() + static_cast<ptrdiff_t>(end));
    ui.statusBar->showMessage(QString("Captured %1 MIDI note(s), %2 event(s).")
        .arg(addedNotes).arg(addedEvents), 3500);
    return true;
}

void VeraCoreQT::markMidiRecordingChanged()
{
    if (m_midiRecordingChanged) return;
    m_midiRecordingChanged = true;
    if (!m_playlistView || m_restoringProject || m_suppressProjectUndoCapture ||
        !m_midiRecordingBeforeDocument || m_midiRecordingContentId.isEmpty()) return;
    m_midiRecordingBeforeStateId = m_playlistView->currentArrangementStateId();
    m_midiRecordingNonArrangementDirtyBefore = m_nonArrangementDirty;
    m_playlistView->beginArrangementTransaction();
    m_playlistView->markArrangementTransactionChanged();
    m_midiRecordingUndoActive = true;
}

void VeraCoreQT::handleRecordedMidiInput(const VeraCoreMidiEvent& event)
{
    if (!m_isRecordingMidi || !isValidMidiChannelEvent(event.status, event.data1, event.data2)) {
        return;
    }
    const auto document = m_midiClipDocs.find(m_midiRecordingContentId);
    MidiClipDocument* doc = document != m_midiClipDocs.end() && document->second
        ? document->second.get() : nullptr;
    if (!doc || !isFiniteBeatRange(m_midiRecordingClipStartBeat,
                                   m_midiRecordingClipLengthBeats)) {
        stopMidiRecording(false);
        if (m_transportWidget) m_transportWidget->setRecording(false);
        return;
    }

    const double audioBeat = std::isfinite(event.captureBeat) && event.captureBeat >= 0.0
        ? event.captureBeat
        : (audioGeneral ? audioGeneral->audioCore.GetTransportPlayheadBeat() : m_playheadBeat);
    const double absoluteBeat = std::isfinite(audioBeat) ? audioBeat : m_playheadBeat;
    if (m_recordingCountInEndBeat >= 0.0 && !m_recordingCountInComplete &&
        absoluteBeat < m_recordingCountInEndBeat - 1.0e-9) return;
    if (m_punchEnabled && (absoluteBeat < m_punchInBeat - 1.0e-9 ||
                          absoluteBeat > m_punchOutBeat + 1.0e-9)) {
        return;
    }
    const double localBeat = absoluteBeat - m_midiRecordingClipStartBeat;
    if (!std::isfinite(localBeat) || localBeat < 0.0 ||
        localBeat > m_midiRecordingClipLengthBeats) {
        return;
    }

    const uint8_t statusHi = event.status & 0xF0;
    const int channel = event.status & 0x0F;
    const bool noteOn = statusHi == 0x90 && event.data2 > 0;
    const bool noteOff = statusHi == 0x80 || (statusHi == 0x90 && event.data2 == 0);
    const int noteKey = channel * 128 + event.data1;
    if (noteOn && (!m_punchEnabled || absoluteBeat < m_punchOutBeat - 1.0e-9) &&
        localBeat < m_midiRecordingClipLengthBeats && isPluginMidiPitch(event.data1)) {
        m_recordedMidiStarts[noteKey].push_back({localBeat, qBound(1, static_cast<int>(event.data2), 127)});
        return;
    }
    if (noteOff && isPluginMidiPitch(event.data1)) {
        auto found = m_recordedMidiStarts.find(noteKey);
        if (found == m_recordedMidiStarts.end() || found->second.empty()) {
            return;
        }
        const RecordedMidiStart start = found->second.front();
        found->second.erase(found->second.begin());
        if (found->second.empty()) m_recordedMidiStarts.erase(found);

        const std::vector<RecordedNoteSegment> segments = recordedNoteSegments(
            start.beat, localBeat, m_midiRecordingClipLengthBeats, m_loopEnabled,
            m_loopStartBeat - m_midiRecordingClipStartBeat,
            m_loopEndBeat - m_midiRecordingClipStartBeat);
        bool appendedNote = false;
        for (const RecordedNoteSegment& rawSegment : segments) {
            const RecordedNoteSegment segment = quantizedRecordedNoteSegment(
                rawSegment, m_midiRecordingClipLengthBeats,
                m_midiRecordQuantizeBeats);
            markMidiRecordingChanged();
            MidiNote note;
            note.pitch = event.data1;
            note.startBeat = m_midiRecordingClipSourceOffsetBeats + segment.startBeat;
            note.lengthBeats = segment.lengthBeats;
            note.velocity = start.velocity;
            note.colorGroup = channel;
            doc->lastNoteLengthBeats = note.lengthBeats;
            doc->notes.push_back(note);
            m_midiRecordingAddedNotes.push_back(note);
            appendedNote = true;
        }
        if (!appendedNote) return;
        setProjectDirty();
        std::stable_sort(doc->notes.begin(), doc->notes.end(), [](const MidiNote& a, const MidiNote& b) {
            if (a.startBeat != b.startBeat) return a.startBeat < b.startBeat;
            return a.pitch < b.pitch;
        });
        if (m_midiEditor && m_currentMidiClipKey == m_midiRecordingContentId)
            m_midiEditor->setClip(doc);
        syncMidiTimelineToAudio();
        refreshMidiEditorGhostClips();
        return;
    }

    markMidiRecordingChanged();
    const MidiEvent recordedEvent{m_midiRecordingClipSourceOffsetBeats + localBeat,
                                  event.status, event.data1, event.data2};
    doc->performanceEvents.push_back(recordedEvent);
    m_midiRecordingAddedEvents.push_back(recordedEvent);
    setProjectDirty();
    syncMidiTimelineToAudio();
}

void VeraCoreQT::stopMidiRecording(bool finalizeActiveNotes)
{
    if (!m_isRecordingMidi) {
        m_recordedMidiStarts.clear();
        m_midiRecordingBeforeDocument.reset();
        m_midiRecordingContentId.clear();
        m_midiRecordingClipStartBeat = 0.0;
        m_midiRecordingClipLengthBeats = 0.0;
        m_midiRecordingClipSourceOffsetBeats = 0.0;
        m_midiRecordingRangeStartBeat = 0.0;
        m_midiRecordingSawLoopWrap = false;
        m_midiRecordingAddedNotes.clear();
        m_midiRecordingAddedEvents.clear();
        m_midiRecordingBeforeStateId = 0;
        m_midiRecordingNonArrangementDirtyBefore = false;
        m_midiRecordingChanged = false;
        m_midiRecordingUndoActive = false;
        return;
    }
    m_isRecordingMidi = false;
    syncPluginTransportToHost();
    const auto document = m_midiClipDocs.find(m_midiRecordingContentId);
    MidiClipDocument* doc = document != m_midiClipDocs.end() && document->second
        ? document->second.get() : nullptr;
    bool finalizedNotes = false;
    if (finalizeActiveNotes && doc &&
        isFiniteBeatRange(m_midiRecordingClipStartBeat, m_midiRecordingClipLengthBeats)) {
        const double audioBeat = m_recordingStopBeatOverride >= 0.0
            ? m_recordingStopBeatOverride
            : (audioGeneral ? audioGeneral->audioCore.GetTransportPlayheadBeat() : m_playheadBeat);
        const double recordingEndBeat = m_punchEnabled
            ? qMin(std::isfinite(audioBeat) ? audioBeat : m_playheadBeat, m_punchOutBeat)
            : (std::isfinite(audioBeat) ? audioBeat : m_playheadBeat);
        const double endBeat = qBound(0.0,
            recordingEndBeat - m_midiRecordingClipStartBeat,
            m_midiRecordingClipLengthBeats);
        for (const auto& entry : m_recordedMidiStarts) {
            const int channel = qBound(0, entry.first / 128, 15);
            const int pitch = qBound(0, entry.first % 128, 127);
            for (const RecordedMidiStart& start : entry.second) {
                const std::vector<RecordedNoteSegment> segments = recordedNoteSegments(
                    start.beat, endBeat, m_midiRecordingClipLengthBeats, m_loopEnabled,
                    m_loopStartBeat - m_midiRecordingClipStartBeat,
                    m_loopEndBeat - m_midiRecordingClipStartBeat);
                for (const RecordedNoteSegment& rawSegment : segments) {
                    const RecordedNoteSegment segment = quantizedRecordedNoteSegment(
                        rawSegment, m_midiRecordingClipLengthBeats,
                        m_midiRecordQuantizeBeats);
                    markMidiRecordingChanged();
                    MidiNote note;
                    note.pitch = pitch;
                    note.startBeat = m_midiRecordingClipSourceOffsetBeats + segment.startBeat;
                    note.lengthBeats = segment.lengthBeats;
                    note.velocity = start.velocity;
                    note.colorGroup = channel;
                    doc->lastNoteLengthBeats = note.lengthBeats;
                    doc->notes.push_back(note);
                    m_midiRecordingAddedNotes.push_back(note);
                    finalizedNotes = true;
                }
            }
        }
    }
    m_recordedMidiStarts.clear();
    if (finalizedNotes && doc) {
        setProjectDirty();
        std::stable_sort(doc->notes.begin(), doc->notes.end(), [](const MidiNote& a, const MidiNote& b) {
            if (a.startBeat != b.startBeat) return a.startBeat < b.startBeat;
            return a.pitch < b.pitch;
        });
        if (m_midiEditor && m_currentMidiClipKey == m_midiRecordingContentId)
            m_midiEditor->setClip(doc);
        syncMidiTimelineToAudio();
        refreshMidiEditorGhostClips();
    }
    if (m_midiRecordMode == 1 && finalizeActiveNotes && doc &&
        m_midiRecordingBeforeDocument) {
        const double audioBeat = m_recordingStopBeatOverride >= 0.0
            ? m_recordingStopBeatOverride
            : (audioGeneral ? audioGeneral->audioCore.GetTransportPlayheadBeat() : m_playheadBeat);
        double replaceStart = m_punchEnabled ? m_punchInBeat : m_midiRecordingRangeStartBeat;
        double replaceEnd = m_punchEnabled
            ? qMin(std::isfinite(audioBeat) ? audioBeat : m_playheadBeat, m_punchOutBeat)
            : (std::isfinite(audioBeat) ? audioBeat : m_playheadBeat);
        if (!m_punchEnabled && m_loopEnabled && m_midiRecordingSawLoopWrap) {
            replaceStart = m_loopStartBeat;
            replaceEnd = m_loopEndBeat;
        }
        const double clipEndBeat = m_midiRecordingClipStartBeat +
            m_midiRecordingClipLengthBeats;
        replaceStart = qBound(m_midiRecordingClipStartBeat, replaceStart, clipEndBeat);
        replaceEnd = qBound(m_midiRecordingClipStartBeat, replaceEnd, clipEndBeat);
        const double sourceStart = m_midiRecordingClipSourceOffsetBeats +
            replaceStart - m_midiRecordingClipStartBeat;
        const double sourceEnd = m_midiRecordingClipSourceOffsetBeats +
            replaceEnd - m_midiRecordingClipStartBeat;
        MidiClipDocument replaced = *doc;
        if (applyMidiReplaceRange(replaced, *m_midiRecordingBeforeDocument,
                sourceStart, sourceEnd, m_midiRecordingAddedNotes,
                m_midiRecordingAddedEvents)) {
            markMidiRecordingChanged();
            *doc = std::move(replaced);
            setProjectDirty();
            if (m_midiEditor && m_currentMidiClipKey == m_midiRecordingContentId)
                m_midiEditor->setClip(doc);
            syncMidiTimelineToAudio();
            refreshMidiEditorGhostClips();
        }
    }
    if (m_midiRecordingChanged && doc && m_midiRecordingBeforeDocument) {
        const MidiClipDocument before = *m_midiRecordingBeforeDocument;
        const MidiClipDocument after = *doc;
        const QString contentId = m_midiRecordingContentId;
        const auto applyDocument = [this, contentId](const MidiClipDocument& state) {
            applyMidiDocumentState(contentId, state);
            syncMidiTimelineToAudio();
            refreshMidiEditorGhostClips();
        };
        if (!m_midiRecordingUndoActive ||
            !commitProjectUndoStep(m_midiRecordingBeforeStateId,
                m_midiRecordingNonArrangementDirtyBefore, QString(),
                [applyDocument, before]() { applyDocument(before); },
                [applyDocument, after]() { applyDocument(after); })) {
            if (m_midiRecordingUndoActive && m_playlistView)
                m_playlistView->cancelArrangementTransaction();
            setProjectDirty();
        }
    } else if (m_midiRecordingUndoActive && m_playlistView) {
        m_playlistView->cancelArrangementTransaction();
    }
    m_midiRecordingBeforeDocument.reset();
    m_midiRecordingContentId.clear();
    m_midiRecordingClipStartBeat = 0.0;
    m_midiRecordingClipLengthBeats = 0.0;
    m_midiRecordingClipSourceOffsetBeats = 0.0;
    m_midiRecordingRangeStartBeat = 0.0;
    m_midiRecordingSawLoopWrap = false;
    m_midiRecordingAddedNotes.clear();
    m_midiRecordingAddedEvents.clear();
    m_midiRecordingBeforeStateId = 0;
    m_midiRecordingNonArrangementDirtyBefore = false;
    m_midiRecordingChanged = false;
    m_midiRecordingUndoActive = false;
    ui.statusBar->showMessage("MIDI recording stopped.", 2500);
}

void VeraCoreQT::syncAudioInputState()
{
    if (!audioGeneral) return;
    std::vector<int> monitorTrackIds;
    monitorTrackIds.reserve(m_tracks.size());
    const bool anySolo = std::any_of(m_tracks.begin(), m_tracks.end(), [](const std::shared_ptr<Track>& track) {
        return track && track->IsSoloed();
    });
    for (int trackIndex = 0; trackIndex < static_cast<int>(m_tracks.size()); ++trackIndex) {
        const auto& track = m_tracks[trackIndex];
        if (!track || track->GetType() != TrackType::Audio || !track->IsMonitoring() || track->IsMuted() ||
            (anySolo && !track->IsSoloed())) continue;
        monitorTrackIds.push_back(trackIndex);
    }
    audioGeneral->audioCore.SetAudioInputState(monitorTrackIds, m_isRecordingAudio);
}

void VeraCoreQT::restoreCountInMetronome()
{
    if (!m_countInMetronomeOverride) return;
    m_countInMetronomeOverride = false;
    if (!audioGeneral || !m_transportWidget) return;
    const MidiTimeSignaturePoint signature = timeSignatureAtBeat(
        m_timeSignatureMap, m_playheadBeat, m_transportWidget->signatureNumerator(),
        m_transportWidget->signatureDenominator());
    audioGeneral->audioCore.SetMetronomeState(
        m_transportWidget->metronomeEnabled(), signature.numerator, signature.denominator);
}

void VeraCoreQT::drainAudioRecording()
{
    if (!audioGeneral || !m_isRecordingAudio) return;
    audioGeneral->audioCore.DrainRecordedAudio(m_recordedAudioSamples, &m_audioRecordingOverflowed);
}

void VeraCoreQT::stopAudioRecording(bool finalizeRecording)
{
    if (!m_isRecordingAudio) {
        syncAudioInputState();
        return;
    }
    m_isRecordingAudio = false;
    syncAudioInputState();
    if (audioGeneral) audioGeneral->audioCore.DrainRecordedAudio(m_recordedAudioSamples, &m_audioRecordingOverflowed);

    const std::vector<int> tracks = m_recordingAudioTracks;
    m_recordingAudioTracks.clear();
    if (!finalizeRecording || m_recordedAudioSamples.size() < 2 || tracks.empty()) {
        m_recordedAudioSamples.clear();
        return;
    }

    const int sampleRate = qMax(1, m_audioRecordingSampleRate);
    const double fallbackTempo = m_transportWidget
        ? m_transportWidget->tempo() : kDefaultTempoBpm;
    const TempoClock recordingClock(m_tempoMap, fallbackTempo);
    const size_t availableFrames = m_recordedAudioSamples.size() / 2u;
    const double captureStartSeconds = recordingClock.secondsAtBeat(m_audioRecordingStartBeat);
    const double availableEndSeconds = captureStartSeconds +
        static_cast<double>(availableFrames) / sampleRate;
    const double availableEndBeat = recordingClock.beatAtSeconds(availableEndSeconds);
    const double transportEndBeat = m_recordingStopBeatOverride >= 0.0
        ? m_recordingStopBeatOverride
        : (audioGeneral ? audioGeneral->audioCore.GetTransportPlayheadBeat() : m_playheadBeat);
    const double requestedEndBeat = std::isfinite(transportEndBeat)
        ? transportEndBeat : availableEndBeat;
    double clipStartBeat = m_punchEnabled
        ? qMax(m_audioRecordingStartBeat, m_punchInBeat) : m_audioRecordingStartBeat;
    if (m_recordingCountInEndBeat >= 0.0)
        clipStartBeat = qMax(clipStartBeat, m_recordingCountInEndBeat);
    const double targetEndBeat = m_punchEnabled
        ? qMin(requestedEndBeat, m_punchOutBeat) : availableEndBeat;
    const double clipEndBeat = qMin(targetEndBeat, availableEndBeat);
    if (!std::isfinite(clipStartBeat) || !std::isfinite(clipEndBeat) ||
        clipEndBeat - clipStartBeat < 1.0e-6) {
        m_recordedAudioSamples.clear();
        ui.statusBar->showMessage(m_recordingCountInEndBeat >= 0.0
            ? "Recording stopped before count-in completed."
            : "Punch stopped before audio entered the record range.", 3000);
        return;
    }
    const auto frameAtBeat = [&](double beat) -> size_t {
        const double frame = (recordingClock.secondsAtBeat(beat) - captureStartSeconds) * sampleRate;
        return static_cast<size_t>(qBound<qint64>(0, qRound64(frame),
            static_cast<qint64>(availableFrames)));
    };
    const size_t firstFrame = frameAtBeat(clipStartBeat);
    const size_t onePastLastFrame = frameAtBeat(clipEndBeat);
    if (onePastLastFrame <= firstFrame) {
        m_recordedAudioSamples.clear();
        ui.statusBar->showMessage("Punch range contains no captured audio frames.", 3000);
        return;
    }
    if (firstFrame > 0 || onePastLastFrame < availableFrames) {
        const size_t firstSample = firstFrame * 2u;
        const size_t onePastLastSample = onePastLastFrame * 2u;
        std::move(m_recordedAudioSamples.begin() + static_cast<ptrdiff_t>(firstSample),
                  m_recordedAudioSamples.begin() + static_cast<ptrdiff_t>(onePastLastSample),
                  m_recordedAudioSamples.begin());
        m_recordedAudioSamples.resize(onePastLastSample - firstSample);
    }

    QString root = m_projectPath.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : QFileInfo(m_projectPath).absolutePath();
    if (root.isEmpty()) root = QDir::tempPath();
    QDir directory(QDir(root).filePath("Recorded"));
    if (!directory.exists() && !directory.mkpath(".")) {
        ui.statusBar->showMessage("Audio recording failed: cannot create Recorded folder.", 5000);
        m_recordedAudioSamples.clear();
        return;
    }
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    QString path;
    for (int i = 1; i < 10000; ++i) {
        const QString suffix = i == 1 ? QString() : QString("-%1").arg(i);
        const QString candidate = directory.filePath(QString("Recording-%1%2.wav").arg(stamp, suffix));
        if (!QFileInfo::exists(candidate)) { path = candidate; break; }
    }
    if (path.isEmpty() || !writeFloat32StereoWav(path, m_recordedAudioSamples, m_audioRecordingSampleRate)) {
        ui.statusBar->showMessage("Audio recording failed while writing WAV.", 5000);
        m_recordedAudioSamples.clear();
        return;
    }

    const double frames = static_cast<double>(m_recordedAudioSamples.size() / 2u);
    const double durationSeconds = frames / sampleRate;
    const double tempo = recordingClock.bpmAtBeat(clipStartBeat);
    const double lengthBeats = qMax(1.0e-6, clipEndBeat - clipStartBeat);
    const double beatsPerBar = projectBeatsPerBar(
        m_transportWidget ? m_transportWidget->signatureNumerator() : 4,
        m_transportWidget ? m_transportWidget->signatureDenominator() : 4);
    const QString contentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString name = QFileInfo(path).completeBaseName();
    int created = 0;
    m_playlistView->beginArrangementTransaction();
    for (int trackIndex : tracks) {
        if (trackIndex < 0 || trackIndex >= static_cast<int>(m_tracks.size()) ||
            !m_tracks[trackIndex] || m_tracks[trackIndex]->GetType() != TrackType::Audio) continue;
        if (m_playlistView->addAudioClip(trackIndex, clipStartBeat, lengthBeats, name, path,
                tempo, 0.0, beatsPerBar, durationSeconds, {}, contentId) >= 0) {
            ++created;
            m_playlistView->promoteSelectedClipToRecordedTake();
        }
    }
    if (created > 0) m_playlistView->commitArrangementTransaction();
    else {
        m_playlistView->cancelArrangementTransaction();
        QFile::remove(path);
    }
    m_recordedAudioSamples.clear();
    syncAudioTimelineToAudio();
    ui.statusBar->showMessage(QString("Recorded %1 audio clip%2%3")
        .arg(created).arg(created == 1 ? "" : "s")
        .arg(m_audioRecordingOverflowed ? " (input overrun detected)" : ""), 5000);
    m_audioRecordingOverflowed = false;
}

bool VeraCoreQT::hasActiveAudioOutput(QString* reason) const
{
    if (!audioGeneral) {
        if (reason) *reason = "Audio engine unavailable. Click AUDIO OFF to reconnect.";
        return false;
    }
    const AudioDeviceStatus status = audioGeneral->GetAudioDeviceStatus();
    if (reason) *reason = status.active ? QString() : status.message;
    return status.active;
}

void VeraCoreQT::onTransportPlay()
{
    QString audioError;
    if (!hasActiveAudioOutput(&audioError)) {
        if (m_transportWidget) {
            m_transportWidget->setPlaying(false);
            m_transportWidget->setAudioDeviceStatus(false, audioError);
        }
        ui.statusBar->showMessage(audioError, 6000);
        return;
    }
    if (!m_stepMidiHeldKeys.empty() || !m_stepMidiChord.empty()) clearStepMidiChord();
    syncMidiTimelineToAudio();
    syncTempoMapToAudio();
    syncAudioTimelineToAudio();
    if (!std::isfinite(m_playheadBeat) || m_playheadBeat < 0.0) {
        m_playheadBeat = 0.0;
    }
    syncTransportDisplayToPlayhead();
    m_isPlaying = true;
    syncPluginTransportToHost();
    beginAutomationWriteAtCurrentValue();
    m_transportWidget->setPlaying(true);
    if (audioGeneral) {
        audioGeneral->audioCore.SetTransportState(true, m_transportWidget->tempo(), m_playheadBeat);
    }
    sendTimelineMidiChaseToExternalOutputs(m_playheadBeat);
    m_playbackTimer->start();
}

void VeraCoreQT::onTransportPause()
{
    stopAudioRecording(true);
    stopMidiRecording(true);
    restoreCountInMetronome();
    m_recordingCountInEndBeat = -1.0;
    m_recordingCountInComplete = true;
    if (m_transportWidget) m_transportWidget->setRecording(false);
    const double automationPreviousBeat = m_playheadBeat;
    if (audioGeneral) {
        const double audioPlayheadBeat = audioGeneral->audioCore.GetTransportPlayheadBeat();
        if (std::isfinite(audioPlayheadBeat) && audioPlayheadBeat >= 0.0)
            m_playheadBeat = audioPlayheadBeat;
    }
    syncTransportDisplayToPlayhead();
    closeAutomationWriteAtTransportDiscontinuity(automationPreviousBeat);
    m_isPlaying = false;
    syncPluginTransportToHost();
    m_transportWidget->setPlaying(false);
    m_transportWidget->setPositionBeats(m_playheadBeat);
    if (m_playlistView) m_playlistView->setPlayheadBeat(m_playheadBeat);
    if (m_audioEditorWidget) m_audioEditorWidget->setPlayheadBeat(m_playheadBeat);
    updateMidiEditorPlayhead();
    m_transportWidget->setMeterPeaks(0.0f, 0.0f,
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity());
    if (m_pluginHost) {
        m_pluginHost->panic();
    }
    sendExternalMidiPanic();
    if (audioGeneral) {
        audioGeneral->audioCore.SetTransportState(false, m_transportWidget->tempo(), m_playheadBeat);
    }
}

void VeraCoreQT::onTransportStop()
{
    stopAudioRecording(true);
    stopMidiRecording(true);
    restoreCountInMetronome();
    m_recordingCountInEndBeat = -1.0;
    m_recordingCountInComplete = true;
    if (m_transportWidget) m_transportWidget->setRecording(false);
    const double automationPreviousBeat = m_playheadBeat;
    if (audioGeneral) {
        const double audioPlayheadBeat = audioGeneral->audioCore.GetTransportPlayheadBeat();
        if (std::isfinite(audioPlayheadBeat) && audioPlayheadBeat >= 0.0)
            m_playheadBeat = audioPlayheadBeat;
    }
    closeAutomationWriteAtTransportDiscontinuity(automationPreviousBeat);
    m_isPlaying = false;
    m_playheadBeat = 0.0;
    syncTransportDisplayToPlayhead();
    syncPluginTransportToHost();
    m_transportWidget->setPlaying(false);
    m_transportWidget->setPositionBeats(m_playheadBeat);
    if (m_playlistView) m_playlistView->setPlayheadBeat(m_playheadBeat);
    if (m_audioEditorWidget) m_audioEditorWidget->setPlayheadBeat(m_playheadBeat);
    m_transportWidget->setMeterPeaks(0.0f, 0.0f,
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity());
    if (m_pluginHost) {
        m_pluginHost->panic();
    }
    sendExternalMidiPanic();
    if (audioGeneral) {
        audioGeneral->audioCore.RequestLoudnessMeterReset();
        audioGeneral->audioCore.SetTransportState(false, m_transportWidget->tempo(), m_playheadBeat);
    }
}

void VeraCoreQT::onPlaybackTick()
{
    if (audioGeneral && m_transportWidget) {
        const VeraCoreAudioMeterState meter = audioGeneral->audioCore.GetAndResetMeterState();
        m_transportWidget->setMeterPeaks(
            meter.peakLeft, meter.peakRight, meter.momentaryLufs,
            meter.shortTermLufs, meter.integratedLufs, meter.loudnessRangeLu);
    }
    if (!m_isPlaying) {
        return;
    }
    const double previousBeat = m_playheadBeat;
    drainAudioRecording();
    const double audioPlayheadBeat = audioGeneral ? audioGeneral->audioCore.GetTransportPlayheadBeat() : previousBeat;
    if (std::isfinite(audioPlayheadBeat) && audioPlayheadBeat >= 0.0) {
        m_playheadBeat = audioPlayheadBeat;
    }
    if (!std::isfinite(m_playheadBeat) || m_playheadBeat < 0.0) {
        m_playheadBeat = 0.0;
    }
    if (m_isRecordingMidi && m_loopEnabled &&
        m_playheadBeat + 1.0e-9 < previousBeat)
        m_midiRecordingSawLoopWrap = true;
    if (m_recordingCountInEndBeat >= 0.0 && !m_recordingCountInComplete &&
        previousBeat < m_recordingCountInEndBeat &&
        m_playheadBeat >= m_recordingCountInEndBeat) {
        m_recordingCountInComplete = true;
        restoreCountInMetronome();
        ui.statusBar->showMessage("Count-in complete · recording", 2000);
    }
    if (m_punchEnabled && (m_isRecordingAudio || m_isRecordingMidi)) {
        const bool wrapped = m_loopEnabled && m_playheadBeat + 1.0e-9 < previousBeat;
        const bool crossedPunchOut =
            (!wrapped && previousBeat < m_punchOutBeat &&
             m_playheadBeat >= m_punchOutBeat) ||
            (wrapped && previousBeat < m_punchOutBeat &&
             m_punchOutBeat <= m_loopEndBeat + 1.0e-9);
        if (crossedPunchOut) {
            m_recordingStopBeatOverride = m_punchOutBeat;
            stopAudioRecording(true);
            stopMidiRecording(true);
            restoreCountInMetronome();
            m_recordingCountInEndBeat = -1.0;
            m_recordingCountInComplete = true;
            if (m_transportWidget) m_transportWidget->setRecording(false);
            m_recordingStopBeatOverride = -1.0;
            ui.statusBar->showMessage("Punch recording complete; playback continues.", 3500);
        }
    }
    if (m_automationWriteActive) {
        if (m_loopEnabled && m_playheadBeat < previousBeat) {
            const double heldValue = m_automationWriteValue;
            const int writeTrack = m_automationWriteTrackIndex;
            const int writeSlot = m_automationWriteSlotIndex;
            const bool writeAudioFx = m_automationWriteAudioFx;
            const uint32_t writeParameter = m_automationWriteParameterId;
            const QString writeName = m_automationWriteParameterName;
            const uint64_t writeEpoch = m_automationWriteInstanceEpoch;
            const bool writeOwnsHostGesture = m_automationWriteOwnsHostGesture;
            const bool restartWrite = m_pluginHost &&
                m_automationWriteMode != InspectorWidget::AutomationWriteMode::Read &&
                writeTrack >= 0 && writeSlot >= 0;
            extendAutomationWriteRange(previousBeat);
            extendAutomationWriteRange(m_loopEndBeat);
            finishAutomationWrite(m_loopEndBeat);
            if (restartWrite) {
                armAutomationWriteTarget(writeTrack, writeAudioFx, writeSlot,
                                         writeParameter, writeName, writeEpoch);
                const bool began = !writeOwnsHostGesture || (writeAudioFx
                    ? m_pluginHost->beginAudioFxParameterGesture(
                        writeTrack, writeSlot, writeParameter)
                    : m_pluginHost->beginLayerParameterGesture(
                        writeTrack * kLayerIdStride + writeSlot, writeParameter));
                if (began) {
                    m_automationWriteOwnsHostGesture = writeOwnsHostGesture;
                    if (writeOwnsHostGesture) ++m_parameterEditorGestureDepth;
                    if (writeOwnsHostGesture) {
                        if (writeAudioFx)
                            m_pluginHost->setAudioFxParameterNormalized(
                                writeTrack, writeSlot, writeParameter, heldValue);
                        else
                            m_pluginHost->setLayerParameterNormalized(
                                writeTrack * kLayerIdStride + writeSlot,
                                writeParameter, heldValue);
                    }
                    writePluginAutomationPoint(writeParameter, writeName,
                                               m_loopStartBeat, heldValue, true);
                    writePluginAutomationPoint(writeParameter, writeName,
                                               m_playheadBeat, heldValue, true);
                } else {
                    finishAutomationWrite();
                }
            }
        } else {
            extendAutomationWriteRange(m_playheadBeat);
        }
    }
    syncTransportDisplayToPlayhead();
    syncPluginTransportToHost();
    if (!m_loopEnabled && m_playheadBeat >= arrangementLengthBeats()) {
        onTransportStop();
        return;
    }
    if (m_loopEnabled && m_playheadBeat < previousBeat) {
        sendTimelineMidiToExternalOutputs(previousBeat, m_loopEndBeat);
        sendExternalMidiPanic();
        sendTimelineMidiChaseToExternalOutputs(m_loopStartBeat);
        sendTimelineMidiToExternalOutputs(m_loopStartBeat, m_playheadBeat);
    } else {
        sendTimelineMidiToExternalOutputs(previousBeat, m_playheadBeat);
    }
    m_transportWidget->setPositionBeats(m_playheadBeat);
    if (m_playlistView) m_playlistView->setPlayheadBeat(m_playheadBeat);
    if (m_audioEditorWidget) m_audioEditorWidget->setPlayheadBeat(m_playheadBeat);
    updateMidiEditorPlayhead();
}

bool VeraCoreQT::encodeRenderedAudio(const QString& sourceWav, const QString& destinationPath,
                                     const QString& format, const AudioEncodeOptions& options,
                                     QString* errorText,
                                     const std::function<bool(qint64, qint64)>& progress) const
{
    const AudioCodecResult result = AudioCodecBackend::encodeFromWav(
        sourceWav,
        destinationPath,
        AudioCodecBackend::formatFromId(format),
        options,
        progress);
    if (!result.success && errorText) {
        *errorText = result.message;
    }
    return result.success;
}

bool VeraCoreQT::writeMidiDocumentToFile(const QString& path, const MidiClipDocument& doc,
                                         QString* errorText, bool includeConductorMeta) const
{
    constexpr quint32 kMaxMidiVariableLength = 0x0fffffffu;
    const int exportPpq = std::clamp(m_projectPpq, kMinProjectPpq, kMaxProjectPpq);
    QVector<RtMidiEvent> midiEvents;
    auto pushCc = [&](double beat, uint8_t channel, uint8_t controller, int value) {
        midiEvents.push_back({beat,
                              static_cast<uint8_t>(0xB0 | channel),
                              controller,
                              static_cast<uint8_t>(clampIntValue(value, 0, 127))});
    };

    for (const MidiNote& note : doc.notes) {
        if (note.muted || note.isSlide ||
            !isFiniteBeatRange(note.startBeat, note.lengthBeats) ||
            !isPluginMidiPitch(note.pitch)) {
            continue;
        }

        MidiNoteTickRange range;
        if (!quantizeMidiNoteRange(note, exportPpq, range)) continue;
        const double noteStartBeat = static_cast<double>(range.startTick) / exportPpq;
        const double noteEndBeat = static_cast<double>(range.startTick + range.durationTicks) / exportPpq;
        const uint8_t pitch = static_cast<uint8_t>(note.pitch);
        const uint8_t velocity = static_cast<uint8_t>(clampIntValue(note.velocity, 1, 127));
        const uint8_t channel = static_cast<uint8_t>(clampIntValue(note.colorGroup, 0, 15));
        if (note.pan != 64) {
            pushCc(noteStartBeat, channel, kMidiCcPan, note.pan);
        }
        if (note.modX != 64) {
            pushCc(noteStartBeat, channel, kMidiCcModX, note.modX);
        }
        if (note.modY != 64) {
            pushCc(noteStartBeat, channel, kMidiCcModY, note.modY);
        }
        midiEvents.push_back({noteStartBeat, static_cast<uint8_t>(0x90 | channel), pitch, velocity});
        midiEvents.push_back({noteEndBeat, static_cast<uint8_t>(0x80 | channel), pitch,
                              static_cast<uint8_t>(clampIntValue(note.release, 0, 127))});
    }
    for (const MidiEvent& event : doc.performanceEvents) {
        if (!std::isfinite(event.beat) || event.beat < 0.0 ||
            !isValidMidiChannelEvent(event.status, event.data1, event.data2)) {
            continue;
        }
        midiEvents.push_back({event.beat, event.status, event.data1, event.data2});
    }

    std::stable_sort(midiEvents.begin(), midiEvents.end(), [](const RtMidiEvent& a, const RtMidiEvent& b) {
        if (a.beat != b.beat) return a.beat < b.beat;
        const int aRank = midiEventSortRank(a.status, a.data2);
        const int bRank = midiEventSortRank(b.status, b.data2);
        if (aRank != bRank) return aRank < bRank;
        if (a.status != b.status) return a.status < b.status;
        return a.data1 < b.data1;
    });
    QVector<RtMidiEvent> compactEvents;
    compactEvents.reserve(midiEvents.size());
    for (const RtMidiEvent& event : midiEvents) {
        const uint8_t statusHi = event.status & 0xF0;
        const bool noteEvent = statusHi == 0x80 || statusHi == 0x90;
        const bool duplicateState = !noteEvent && !compactEvents.isEmpty() &&
            compactEvents.back().beat == event.beat &&
            compactEvents.back().status == event.status &&
            compactEvents.back().data1 == event.data1 &&
            compactEvents.back().data2 == event.data2;
        if (!duplicateState) compactEvents.push_back(event);
    }
    midiEvents.swap(compactEvents);

    QByteArray trackBytes;
    bool metadataTooLarge = false;
    auto appendMetaText = [&](uint8_t type, const QString& text) {
        const QByteArray payload = text.trimmed().toUtf8();
        if (payload.isEmpty()) {
            return;
        }
        if (static_cast<quint64>(payload.size()) > kMaxMidiVariableLength) {
            metadataTooLarge = true;
            return;
        }
        trackBytes.append(static_cast<char>(0x00));
        trackBytes.append(static_cast<char>(0xff));
        trackBytes.append(static_cast<char>(type));
        trackBytes.append(encodeVariableLength(static_cast<quint32>(payload.size())));
        trackBytes.append(payload);
    };
    appendMetaText(0x03, doc.name);
    appendMetaText(0x04, doc.trackName);
    QString groupMeta = "VeraCoreGroup:v1";
    bool hasGroupMeta = false;
    for (const MidiNote& note : doc.notes) {
        MidiNoteTickRange range;
        if (note.groupId <= 0 || !quantizeMidiNoteRange(note, exportPpq, range)) continue;
        groupMeta += QString(" %1,%2,%3,%4")
            .arg(range.startTick)
            .arg(qBound(0, note.pitch, 127))
            .arg(qBound(0, note.colorGroup, 15))
            .arg(qMax(1, note.groupId));
        hasGroupMeta = true;
    }
    if (hasGroupMeta) {
        appendMetaText(0x01, groupMeta);
    }
    QString noteFlagsMeta = "VeraCoreNoteFlags:v1";
    bool hasNoteFlagsMeta = false;
    for (const MidiNote& note : doc.notes) {
        MidiNoteTickRange range;
        if (!quantizeMidiNoteRange(note, exportPpq, range)) continue;
        int flags = 0;
        if (note.isKeyswitch) flags |= 1;
        if (note.isSlide) flags |= 2;
        if (note.isPortamento) flags |= 4;
        if (note.muted) flags |= 8;
        noteFlagsMeta += QString(" %1,%2,%3,%4,%5,%6,%7,%8,%9,%10")
            .arg(range.startTick)
            .arg(range.durationTicks)
            .arg(qBound(0, note.pitch, 127))
            .arg(qBound(0, note.colorGroup, 15))
            .arg(flags)
            .arg(qBound(1, note.velocity, 127))
            .arg(qBound(0, note.pan, 127))
            .arg(qBound(0, note.release, 127))
            .arg(qBound(0, note.modX, 127))
            .arg(qBound(0, note.modY, 127));
        hasNoteFlagsMeta = true;
    }
    if (hasNoteFlagsMeta) {
        appendMetaText(0x01, noteFlagsMeta);
    }
    if (metadataTooLarge) {
        if (errorText) *errorText = "MIDI metadata exceeds the Standard MIDI File limit.";
        return false;
    }

    std::vector<MidiTempoPoint> exportTempoMap;
    std::vector<MidiTimeSignaturePoint> exportSignatureMap;
    if (includeConductorMeta) {
        const double exportTempo = std::isfinite(doc.tempoBpm) && doc.tempoBpm > 0.0
            ? doc.tempoBpm
            : (m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm);
        exportTempoMap = normalizedTempoMap(doc.tempoMap, exportTempo);
        const quint32 tempo = tempoBpmToMidiMicroseconds(exportTempoMap.front().bpm);
        trackBytes.append(static_cast<char>(0x00));
        trackBytes.append(static_cast<char>(0xff));
        trackBytes.append(static_cast<char>(0x51));
        trackBytes.append(static_cast<char>(0x03));
        trackBytes.append(static_cast<char>((tempo >> 16) & 0xff));
        trackBytes.append(static_cast<char>((tempo >> 8) & 0xff));
        trackBytes.append(static_cast<char>(tempo & 0xff));
        exportSignatureMap = normalizedTimeSignatureMap(
            doc.timeSignatureMap,
            doc.timeSignatureNumerator > 0 ? doc.timeSignatureNumerator
                : (m_transportWidget ? m_transportWidget->signatureNumerator() : 4),
            validTimeSignatureDenominator(doc.timeSignatureDenominator)
                ? doc.timeSignatureDenominator
                : (m_transportWidget ? m_transportWidget->signatureDenominator() : 4));
        const int signatureNumerator = exportSignatureMap.front().numerator;
        const int signatureDenominator = exportSignatureMap.front().denominator;
        trackBytes.append(static_cast<char>(0x00));
        trackBytes.append(static_cast<char>(0xff));
        trackBytes.append(static_cast<char>(0x58));
        trackBytes.append(static_cast<char>(0x04));
        trackBytes.append(static_cast<char>(qBound(1, signatureNumerator, 255)));
        trackBytes.append(static_cast<char>(midiDenominatorPower(signatureDenominator)));
        trackBytes.append(static_cast<char>(24));
        trackBytes.append(static_cast<char>(8));
    }

    auto appendSpacerDeltas = [&](quint32& deltaTicks) {
        while (deltaTicks > kMaxMidiVariableLength) {
            trackBytes.append(encodeVariableLength(kMaxMidiVariableLength));
            trackBytes.append(static_cast<char>(0xff));
            trackBytes.append(static_cast<char>(0x7f));
            trackBytes.append(static_cast<char>(0x00));
            deltaTicks -= kMaxMidiVariableLength;
        }
    };
    auto appendDelta = [&](quint32 deltaTicks, uint8_t status, uint8_t data1, uint8_t data2) {
        appendSpacerDeltas(deltaTicks);
        trackBytes.append(encodeVariableLength(deltaTicks));
        trackBytes.append(static_cast<char>(status));
        trackBytes.append(static_cast<char>(data1));
        if ((status & 0xF0) != 0xC0 && (status & 0xF0) != 0xD0) {
            trackBytes.append(static_cast<char>(data2));
        }
    };
    auto appendTempoDelta = [&](quint32 deltaTicks, double bpm) {
        appendSpacerDeltas(deltaTicks);
        const quint32 tempo = tempoBpmToMidiMicroseconds(bpm);
        trackBytes.append(encodeVariableLength(deltaTicks));
        trackBytes.append(static_cast<char>(0xff));
        trackBytes.append(static_cast<char>(0x51));
        trackBytes.append(static_cast<char>(0x03));
        trackBytes.append(static_cast<char>((tempo >> 16) & 0xff));
        trackBytes.append(static_cast<char>((tempo >> 8) & 0xff));
        trackBytes.append(static_cast<char>(tempo & 0xff));
    };
    auto appendSignatureDelta = [&](quint32 deltaTicks, const MidiTimeSignaturePoint& signature) {
        appendSpacerDeltas(deltaTicks);
        trackBytes.append(encodeVariableLength(deltaTicks));
        trackBytes.append(static_cast<char>(0xff));
        trackBytes.append(static_cast<char>(0x58));
        trackBytes.append(static_cast<char>(0x04));
        trackBytes.append(static_cast<char>(qBound(1, signature.numerator, 255)));
        trackBytes.append(static_cast<char>(midiDenominatorPower(signature.denominator)));
        trackBytes.append(static_cast<char>(24));
        trackBytes.append(static_cast<char>(8));
    };

    const auto pointTick = [&](double beat) {
        const double tickDouble = qMax(0.0, beat) * exportPpq;
        return tickDouble > static_cast<double>(std::numeric_limits<quint32>::max())
            ? std::numeric_limits<quint32>::max()
            : static_cast<quint32>(std::llround(tickDouble));
    };

    quint32 prevTick = 0;
    quint32 lastEventTick = 0;
    size_t nextTempoPoint = includeConductorMeta ? 1u : exportTempoMap.size();
    size_t nextSignaturePoint = includeConductorMeta ? 1u : exportSignatureMap.size();
    auto appendConductorEventsThrough = [&](quint32 eventTick) {
        while (true) {
            const bool hasTempo = nextTempoPoint < exportTempoMap.size() &&
                exportTempoMap[nextTempoPoint].beat <= doc.lengthBeats;
            const bool hasSignature = nextSignaturePoint < exportSignatureMap.size() &&
                exportSignatureMap[nextSignaturePoint].beat <= doc.lengthBeats;
            if (!hasTempo && !hasSignature) break;
            const quint32 tempoTick = hasTempo
                ? pointTick(exportTempoMap[nextTempoPoint].beat)
                : std::numeric_limits<quint32>::max();
            const quint32 signatureTick = hasSignature
                ? pointTick(exportSignatureMap[nextSignaturePoint].beat)
                : std::numeric_limits<quint32>::max();
            const quint32 nextTick = qMin(tempoTick, signatureTick);
            if (nextTick > eventTick) break;
            const quint32 clampedTick = qMax(nextTick, prevTick);
            if (tempoTick <= signatureTick) {
                appendTempoDelta(clampedTick - prevTick, exportTempoMap[nextTempoPoint].bpm);
                ++nextTempoPoint;
            } else {
                appendSignatureDelta(clampedTick - prevTick, exportSignatureMap[nextSignaturePoint]);
                ++nextSignaturePoint;
            }
            prevTick = clampedTick;
            lastEventTick = qMax(lastEventTick, clampedTick);
        }
    };
    for (const RtMidiEvent& event : midiEvents) {
        const double tickDouble = qMax(0.0, event.beat) * exportPpq;
        quint32 eventTick = tickDouble > static_cast<double>(std::numeric_limits<quint32>::max())
            ? std::numeric_limits<quint32>::max()
            : static_cast<quint32>(std::llround(tickDouble));
        if (eventTick < prevTick) {
            eventTick = prevTick;
        }
        appendConductorEventsThrough(eventTick);
        appendDelta(eventTick - prevTick, event.status, event.data1, event.data2);
        prevTick = eventTick;
        lastEventTick = eventTick;
    }
    appendConductorEventsThrough(std::numeric_limits<quint32>::max());

    const double clipEndDouble = qMax(0.0, doc.lengthBeats) * exportPpq;
    const quint32 clipEndTick = clipEndDouble > static_cast<double>(std::numeric_limits<quint32>::max())
        ? std::numeric_limits<quint32>::max()
        : static_cast<quint32>(std::llround(clipEndDouble));
    const quint32 trackEndTick = qMax(clipEndTick, lastEventTick);
    quint32 endDelta = trackEndTick > prevTick ? trackEndTick - prevTick : 0;
    appendSpacerDeltas(endDelta);
    trackBytes.append(encodeVariableLength(endDelta));
    trackBytes.append(static_cast<char>(0xff));
    trackBytes.append(static_cast<char>(0x2f));
    trackBytes.append(static_cast<char>(0x00));

    if (trackBytes.size() < 0 ||
        static_cast<qulonglong>(trackBytes.size()) > std::numeric_limits<quint32>::max()) {
        if (errorText) {
            *errorText = "MIDI track is too large to export.";
        }
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorText) {
            *errorText = "Could not open destination MIDI file.";
        }
        return false;
    }

    auto failWrite = [&](const QString& message) {
        if (errorText) {
            *errorText = message;
        }
        file.cancelWriting();
        return false;
    };
    auto checkedWrite = [&](const char* data, qint64 size) {
        return file.write(data, size) == size;
    };
    auto checkedWriteArray = [&](const QByteArray& bytes) {
        return file.write(bytes) == bytes.size();
    };

    if (!checkedWrite("MThd", 4)) {
        return failWrite("Could not write MIDI header.");
    }
    writeUInt32BE(file, 6);
    writeUInt16BE(file, 0);
    writeUInt16BE(file, 1);
    writeUInt16BE(file, static_cast<quint16>(exportPpq));
    if (file.error() != QFileDevice::NoError || !checkedWrite("MTrk", 4)) {
        return failWrite("Could not write MIDI track header.");
    }
    writeUInt32BE(file, static_cast<quint32>(trackBytes.size()));
    if (file.error() != QFileDevice::NoError || !checkedWriteArray(trackBytes)) {
        return failWrite("Could not write MIDI track data.");
    }
    if (file.error() != QFileDevice::NoError || !file.commit()) {
        if (errorText) {
            *errorText = "Could not finalize MIDI file.";
        }
        return false;
    }
    return true;
}

bool VeraCoreQT::writeMidiArrangementToFile(const QString& path, QString* errorText) const
{
    if (!m_playlistView) {
        if (errorText) *errorText = "Playlist is unavailable.";
        return false;
    }
    const int exportPpq = std::clamp(m_projectPpq, kMinProjectPpq, kMaxProjectPpq);
    const double arrangementLength = qMax(1.0, arrangementLengthBeats());
    std::map<int, MidiClipDocument> trackDocuments;
    std::map<int, qint64> nextGroupIds;
    for (const PlaylistView::MidiClipInfo& clip : m_playlistView->getMidiClips()) {
        if (clip.trackIndex < 0 || clip.trackIndex >= static_cast<int>(m_tracks.size()) ||
            !m_tracks[clip.trackIndex] || m_tracks[clip.trackIndex]->GetType() != TrackType::Midi ||
            !isFiniteBeatRange(clip.startBeat, clip.lengthBeats)) continue;
        const auto source = m_midiClipDocs.find(buildClipDocumentKey(clip.contentId));
        if (source == m_midiClipDocs.end() || !source->second) continue;
        const MidiClipDocument visible = midiDocumentForClipExport(*source->second, clip);
        MidiClipDocument& track = trackDocuments[clip.trackIndex];
        track.name = m_tracks[clip.trackIndex]->GetName();
        track.trackName = track.name;
        track.trackIndex = clip.trackIndex;
        track.startBeat = 0.0;
        track.lengthBeats = arrangementLength;
        track.tempoBpm = m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm;
        track.timeSignatureNumerator = m_transportWidget ? m_transportWidget->signatureNumerator() : 4;
        track.timeSignatureDenominator = m_transportWidget ? m_transportWidget->signatureDenominator() : 4;
        track.timeSignatureMap = normalizedTimeSignatureMap(
            m_timeSignatureMap, track.timeSignatureNumerator,
            track.timeSignatureDenominator);
        std::map<int, int> clipGroupIds;
        for (MidiNote note : visible.notes) {
            note.startBeat += clip.startBeat;
            if (note.groupId > 0) {
                auto mapped = clipGroupIds.find(note.groupId);
                if (mapped == clipGroupIds.end()) {
                    qint64& nextGroupId = nextGroupIds[clip.trackIndex];
                    if (nextGroupId <= 0) nextGroupId = 1;
                    const int replacement = nextGroupId <= std::numeric_limits<int>::max()
                        ? static_cast<int>(nextGroupId++) : 0;
                    mapped = clipGroupIds.emplace(note.groupId, replacement).first;
                }
                note.groupId = mapped->second;
            }
            track.notes.push_back(std::move(note));
        }
        for (MidiEvent event : visible.performanceEvents) {
            event.beat += clip.startBeat;
            track.performanceEvents.push_back(event);
        }
    }
    if (trackDocuments.empty()) {
        if (errorText) *errorText = "The arrangement contains no exportable MIDI clips.";
        return false;
    }
    if (trackDocuments.size() + 1u > std::numeric_limits<quint16>::max()) {
        if (errorText) *errorText = "The arrangement has too many MIDI tracks for an SMF Type 1 file.";
        return false;
    }

    auto extractTrackChunk = [&](const MidiClipDocument& document, bool includeConductor,
                                 QByteArray& chunk) {
        QTemporaryFile temporary(QDir::tempPath() + "/VeraCoreSmfTrackXXXXXX.mid");
        if (!temporary.open()) {
            if (errorText) *errorText = "Could not create a temporary MIDI track.";
            return false;
        }
        const QString temporaryPath = temporary.fileName();
        temporary.close();
        QFile::remove(temporaryPath);
        QString trackError;
        if (!writeMidiDocumentToFile(
                temporaryPath, document, &trackError, includeConductor)) {
            QFile::remove(temporaryPath);
            if (errorText) *errorText = trackError.isEmpty()
                ? "Could not serialize a MIDI track." : trackError;
            return false;
        }

        MidiClipDocument readBackDocument;
        QString readBackError;
        const bool readBack = importMidiDocumentFromFile(
            temporaryPath, readBackDocument, &readBackError);
        const bool notesMatch = readBack &&
            normalizedMidiRoundTripNotes(readBackDocument, exportPpq) ==
            normalizedMidiRoundTripNotes(document, exportPpq);
        const bool eventsMatch = readBack &&
            normalizedMidiRoundTripEvents(readBackDocument, exportPpq, false) ==
            normalizedMidiRoundTripEvents(document, exportPpq);
        const bool namesMatch = readBack &&
            (document.name.trimmed().isEmpty() || readBackDocument.name == document.name.trimmed()) &&
            (document.trackName.trimmed().isEmpty() ||
             readBackDocument.trackName == document.trackName.trimmed());
        bool conductorMetaMatch = true;
        if (readBack && includeConductor) {
            conductorMetaMatch =
                normalizedMidiRoundTripTempoMap(readBackDocument, exportPpq) ==
                    normalizedMidiRoundTripTempoMap(document, exportPpq) &&
                normalizedMidiRoundTripTimeSignatureMap(readBackDocument, exportPpq) ==
                    normalizedMidiRoundTripTimeSignatureMap(document, exportPpq);
        }
        if (!notesMatch || !eventsMatch || !namesMatch || !conductorMetaMatch) {
            QFile::remove(temporaryPath);
            if (errorText) *errorText = readBackError.isEmpty()
                ? "A MIDI arrangement track failed semantic read-back validation."
                : readBackError;
            return false;
        }

        QFile trackFile(temporaryPath);
        if (!trackFile.open(QIODevice::ReadOnly)) {
            QFile::remove(temporaryPath);
            if (errorText) *errorText = "Could not reopen a serialized MIDI track.";
            return false;
        }
        const QByteArray bytes = trackFile.readAll();
        trackFile.close();
        QFile::remove(temporaryPath);
        quint32 headerLength = 0;
        quint16 format = 0;
        quint16 trackCount = 0;
        quint16 division = 0;
        quint32 payloadLength = 0;
        const bool valid = bytes.size() >= 22 && bytes.left(4) == "MThd" &&
            readUInt32BE(bytes, 4, headerLength) && headerLength == 6 &&
            readUInt16BE(bytes, 8, format) && format == 0 &&
            readUInt16BE(bytes, 10, trackCount) && trackCount == 1 &&
            readUInt16BE(bytes, 12, division) && division == exportPpq &&
            bytes.mid(14, 4) == "MTrk" && readUInt32BE(bytes, 18, payloadLength) &&
            static_cast<quint64>(bytes.size()) == 22ull + payloadLength;
        if (!valid) {
            if (errorText) *errorText = "Serialized MIDI track has an invalid SMF chunk layout.";
            return false;
        }
        chunk = bytes.mid(14);
        return true;
    };

    MidiClipDocument conductor;
    conductor.name = m_projectPath.isEmpty()
        ? QStringLiteral("VeraCore Arrangement")
        : QFileInfo(m_projectPath).completeBaseName();
    conductor.lengthBeats = arrangementLength;
    conductor.tempoBpm = tempoAtBeat(
        m_tempoMap, 0.0, m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm);
    conductor.tempoMap = normalizedTempoMap(m_tempoMap, conductor.tempoBpm);
    conductor.timeSignatureNumerator = m_transportWidget ? m_transportWidget->signatureNumerator() : 4;
    conductor.timeSignatureDenominator = m_transportWidget ? m_transportWidget->signatureDenominator() : 4;
    conductor.timeSignatureMap = normalizedTimeSignatureMap(
        m_timeSignatureMap, conductor.timeSignatureNumerator,
        conductor.timeSignatureDenominator);

    QVector<QByteArray> chunks;
    chunks.reserve(static_cast<qsizetype>(trackDocuments.size() + 1u));
    QByteArray conductorChunk;
    if (!extractTrackChunk(conductor, true, conductorChunk)) return false;
    chunks.push_back(std::move(conductorChunk));
    for (const auto& entry : trackDocuments) {
        QByteArray trackChunk;
        if (!extractTrackChunk(entry.second, false, trackChunk)) return false;
        chunks.push_back(std::move(trackChunk));
    }

    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        if (errorText) *errorText = "Could not open the MIDI arrangement destination.";
        return false;
    }
    if (output.write("MThd", 4) != 4) {
        if (errorText) *errorText = "Could not write the MIDI arrangement header.";
        return false;
    }
    writeUInt32BE(output, 6);
    writeUInt16BE(output, 1);
    writeUInt16BE(output, static_cast<quint16>(chunks.size()));
    writeUInt16BE(output, static_cast<quint16>(exportPpq));
    for (const QByteArray& chunk : chunks) {
        if (output.write(chunk) != chunk.size()) {
            if (errorText) *errorText = "Could not write a MIDI arrangement track.";
            return false;
        }
    }
    if (output.error() != QFileDevice::NoError || !output.commit()) {
        if (errorText) *errorText = "Could not finalize the MIDI arrangement.";
        return false;
    }

    QFile validation(path);
    if (!validation.open(QIODevice::ReadOnly)) {
        QFile::remove(path);
        if (errorText) *errorText = "Could not validate the MIDI arrangement.";
        return false;
    }
    const QByteArray header = validation.read(14);
    quint32 headerLength = 0;
    quint16 format = 0;
    quint16 trackCount = 0;
    quint16 division = 0;
    bool valid = header.size() == 14 && header.left(4) == "MThd" &&
        readUInt32BE(header, 4, headerLength) && headerLength == 6 &&
        readUInt16BE(header, 8, format) && format == 1 &&
        readUInt16BE(header, 10, trackCount) &&
        static_cast<qsizetype>(trackCount) == chunks.size() &&
        readUInt16BE(header, 12, division) && division == exportPpq;
    for (const QByteArray& chunk : chunks) {
        if (!valid || validation.read(chunk.size()) != chunk) {
            valid = false;
            break;
        }
    }
    valid = valid && validation.atEnd();
    validation.close();
    if (!valid) {
        QFile::remove(path);
        if (errorText) *errorText = "MIDI arrangement failed byte-for-byte installation validation.";
        return false;
    }
    return true;
}

bool VeraCoreQT::importMidiDocumentFromFile(const QString& path, MidiClipDocument& outDoc, QString* errorText) const
{
    outDoc = MidiClipDocument();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorText) {
            *errorText = "Could not open selected MIDI file.";
        }
        return false;
    }

    constexpr qint64 kMaxMidiDocumentBytes = 512ll * 1024ll * 1024ll;
    const qint64 sourceBytes = file.size();
    if (sourceBytes < 14 || sourceBytes > kMaxMidiDocumentBytes) {
        file.close();
        if (errorText) *errorText = "MIDI file size is outside the supported import range.";
        return false;
    }
    const QByteArray bytes = file.read(sourceBytes);
    if (file.error() != QFileDevice::NoError || bytes.size() != sourceBytes) {
        file.close();
        if (errorText) {
            *errorText = "Could not read the complete MIDI file.";
        }
        return false;
    }
    file.close();
    if (bytes.size() > std::numeric_limits<int>::max()) {
        if (errorText) *errorText = "MIDI file is too large.";
        return false;
    }
    const int size = static_cast<int>(bytes.size());
    if (size < 14) {
        if (errorText) {
            *errorText = "File is too short for a MIDI file.";
        }
        return false;
    }

    if (bytes.left(4) != "MThd") {
        if (errorText) {
            *errorText = "Invalid MIDI header.";
        }
        return false;
    }

    quint32 headerSize = 0;
    quint16 format = 0;
    quint16 trackCount = 0;
    quint16 division = 0;
    readUInt32BE(bytes, 4, headerSize);
    readUInt16BE(bytes, 8, format);
    readUInt16BE(bytes, 10, trackCount);
    readUInt16BE(bytes, 12, division);

    const quint64 firstTrackOffset64 = 8ull + static_cast<quint64>(headerSize);
    if (headerSize < 6 || firstTrackOffset64 > static_cast<quint64>(size) ||
        firstTrackOffset64 > static_cast<quint64>(std::numeric_limits<int>::max()) ||
        format == 2 || format > 2 || trackCount == 0 || trackCount > 4096 ||
        (format == 0 && trackCount != 1) || division == 0) {
        if (errorText) {
            *errorText = "Unsupported MIDI format or timing.";
        }
        return false;
    }

    const bool smpteTiming = (division & 0x8000) != 0;
    double ticksPerBeat = smpteTiming ? 0.0 : static_cast<double>(division);
    double smpteTicksPerSecond = 0.0;
    if (smpteTiming) {
        const int frameCode = static_cast<int>((division >> 8) & 0xFF) - 256;
        const int ticksPerFrame = division & 0xFF;
        double framesPerSecond = 0.0;
        if (frameCode == -24) framesPerSecond = 24.0;
        else if (frameCode == -25) framesPerSecond = 25.0;
        else if (frameCode == -29) framesPerSecond = 30000.0 / 1001.0;
        else if (frameCode == -30) framesPerSecond = 30.0;
        if (framesPerSecond <= 0.0 || ticksPerFrame <= 0) {
            if (errorText) *errorText = "Unsupported MIDI SMPTE timing division.";
            return false;
        }
        smpteTicksPerSecond = framesPerSecond * static_cast<double>(ticksPerFrame);
    }
    double importedTempoBpm = 0.0;
    struct ImportedTempoMeta
    {
        qint64 tick = 0;
        int sourceTrack = 0;
        int sequence = 0;
        double bpm = 120.0;
    };
    std::vector<ImportedTempoMeta> importedTempoMeta;
    struct ImportedTimeSignatureMeta
    {
        qint64 tick = 0;
        int sourceTrack = 0;
        int sequence = 0;
        int numerator = 4;
        int denominator = 4;
    };
    std::vector<ImportedTimeSignatureMeta> importedSignatureMeta;
    QString importedName;
    QString importedTrackName;
    struct ImportedGroupMeta {
        quint32 tick = 0;
        int pitch = 0;
        int colorGroup = 0;
        int groupId = 0;
    };
    std::vector<ImportedGroupMeta> importedGroupMeta;
    struct ImportedNoteFlagsMeta {
        quint32 tick = 0;
        quint32 durationTicks = 0;
        int pitch = 0;
        int colorGroup = 0;
        int flags = 0;
        int velocity = 100;
        int pan = 64;
        int release = 64;
        int modX = 64;
        int modY = 64;
    };
    std::vector<ImportedNoteFlagsMeta> importedNoteFlagsMeta;
    struct ParsedMidiEvent
    {
        qint64 tick = 0;
        int sourceTrack = 0;
        int sequence = 0;
        uint8_t status = 0;
        uint8_t data1 = 0;
        uint8_t data2 = 0;
    };

    std::vector<ParsedMidiEvent> parsed;
    std::vector<qint64> trackEndTicks(qMax(1, static_cast<int>(trackCount)), 0);
    int cursor = static_cast<int>(firstTrackOffset64);
    for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
        if (cursor + 8 > size || bytes.mid(cursor, 4) != "MTrk") {
            if (errorText) {
                *errorText = "Unexpected or missing MIDI track header.";
            }
            return false;
        }

        quint32 trackBytesLength = 0;
        readUInt32BE(bytes, cursor + 4, trackBytesLength);
        cursor += 8;
        const quint64 trackEnd64 = static_cast<quint64>(cursor) + static_cast<quint64>(trackBytesLength);
        if (trackEnd64 > static_cast<quint64>(size) ||
            trackEnd64 > static_cast<quint64>(std::numeric_limits<int>::max())) {
            if (errorText) {
                *errorText = "Malformed MIDI track length.";
            }
            return false;
        }
        const int trackEnd = static_cast<int>(trackEnd64);

        int trackCursor = cursor;
        qint64 tick = 0;
        uint8_t runningStatus = 0;
        bool sawEndOfTrack = false;
        while (trackCursor < trackEnd) {
            quint32 delta = 0;
            if (!readVariableLength(bytes, trackCursor, trackEnd, delta)) {
                if (errorText) {
                    *errorText = "Malformed variable-length value.";
                }
                return false;
            }
            tick += static_cast<qint64>(delta);

            if (trackCursor >= trackEnd) {
                if (errorText) *errorText = "MIDI track ends after a delta without an event.";
                return false;
            }
            uint8_t status = static_cast<uint8_t>(bytes[trackCursor++]);
            if ((status & 0x80) == 0) {
                if (runningStatus == 0) {
                    if (errorText) {
                        *errorText = "Malformed running status.";
                    }
                    return false;
                }
                uint8_t data1 = status;
                if (data1 > 127) {
                    if (errorText) {
                        *errorText = "Malformed MIDI running-status data.";
                    }
                    return false;
                }
                status = runningStatus;
                const int dataCount = expectedChannelDataCount(status);
                if (dataCount == 1) {
                    parsed.push_back({tick, trackIndex, static_cast<int>(parsed.size()), status, data1, 0});
                } else {
                    if (trackCursor >= trackEnd) {
                        if (errorText) {
                            *errorText = "Malformed running status data.";
                        }
                        return false;
                    }
                    const uint8_t data2 = static_cast<uint8_t>(bytes[trackCursor++]);
                    if (data2 > 127) {
                        if (errorText) {
                            *errorText = "Malformed MIDI running-status data.";
                        }
                        return false;
                    }
                    parsed.push_back({tick, trackIndex, static_cast<int>(parsed.size()), status, data1, data2});
                }
                continue;
            }

            if (status == 0xFF) {
                runningStatus = 0;
                if (trackCursor >= trackEnd) {
                    if (errorText) {
                        *errorText = "Malformed meta status.";
                    }
                    return false;
                }
                const uint8_t metaType = static_cast<uint8_t>(bytes[trackCursor++]);
                quint32 metaLen = 0;
                if (!readVariableLength(bytes, trackCursor, trackEnd, metaLen)) {
                    if (errorText) {
                        *errorText = "Malformed meta length.";
                    }
                    return false;
                }
                if (static_cast<quint64>(metaLen) > static_cast<quint64>(trackEnd - trackCursor)) {
                    if (errorText) {
                        *errorText = "Malformed meta payload.";
                    }
                    return false;
                }
                if (metaType == 0x2F) {
                    if (metaLen != 0 || trackCursor != trackEnd) {
                        if (errorText) *errorText = "End-of-track must be the final zero-length event.";
                        return false;
                    }
                    sawEndOfTrack = true;
                    break;
                }
                if (metaType == 0x51 && metaLen == 3) {
                    const uint8_t b1 = static_cast<uint8_t>(bytes[trackCursor]);
                    const uint8_t b2 = static_cast<uint8_t>(bytes[trackCursor + 1]);
                    const uint8_t b3 = static_cast<uint8_t>(bytes[trackCursor + 2]);
                    const quint32 microsPerQuarter =
                        (static_cast<quint32>(b1) << 16) |
                        (static_cast<quint32>(b2) << 8) |
                        static_cast<quint32>(b3);
                    if (microsPerQuarter > 0 && importedTempoMeta.size() < kMaxTempoMapPoints) {
                        const double tempoBpm = 60000000.0 / static_cast<double>(microsPerQuarter);
                        if (std::isfinite(tempoBpm) && tempoBpm >= 20.0 && tempoBpm <= 999.0) {
                            importedTempoMeta.push_back({tick, trackIndex,
                                static_cast<int>(importedTempoMeta.size()), tempoBpm});
                        }
                    }
                } else if ((metaType == 0x01 || metaType == 0x03 || metaType == 0x04) && metaLen > 0) {
                    const QString text = QString::fromUtf8(bytes.constData() + trackCursor, static_cast<int>(metaLen)).trimmed();
                    if (metaType == 0x01 && text.startsWith("VeraCoreGroup:v1")) {
                        const QStringList entries = text.split(' ', Qt::SkipEmptyParts);
                        for (int i = 1; i < entries.size(); ++i) {
                            const QStringList parts = entries[i].split(',');
                            if (parts.size() != 4) {
                                continue;
                            }
                            bool okTick = false;
                            bool okPitch = false;
                            bool okColor = false;
                            bool okGroup = false;
                            const quint32 tickValue = parts[0].toUInt(&okTick);
                            const int pitchValue = parts[1].toInt(&okPitch);
                            const int colorValue = parts[2].toInt(&okColor);
                            const int groupValue = parts[3].toInt(&okGroup);
                            if (okTick && okPitch && okColor && okGroup &&
                                isPluginMidiPitch(pitchValue) &&
                                colorValue >= 0 && colorValue < 16 &&
                                groupValue > 0) {
                                importedGroupMeta.push_back({tickValue, pitchValue, colorValue, groupValue});
                            }
                        }
                    } else if (metaType == 0x01 && text.startsWith("VeraCoreNoteFlags:v1")) {
                        const QStringList entries = text.split(' ', Qt::SkipEmptyParts);
                        for (int i = 1; i < entries.size(); ++i) {
                            const QStringList parts = entries[i].split(',');
                            if (parts.size() != 5 && parts.size() != 10) {
                                continue;
                            }
                            bool okTick = false;
                            bool okDuration = false;
                            bool okPitch = false;
                            bool okColor = false;
                            bool okFlags = false;
                            bool okVelocity = true;
                            bool okPan = true;
                            bool okRelease = true;
                            bool okModX = true;
                            bool okModY = true;
                            const quint32 tickValue = parts[0].toUInt(&okTick);
                            const quint32 durationValue = parts[1].toUInt(&okDuration);
                            const int pitchValue = parts[2].toInt(&okPitch);
                            const int colorValue = parts[3].toInt(&okColor);
                            const int flagsValue = parts[4].toInt(&okFlags);
                            int velocityValue = 100;
                            int panValue = 64;
                            int releaseValue = 64;
                            int modXValue = 64;
                            int modYValue = 64;
                            if (parts.size() == 10) {
                                velocityValue = parts[5].toInt(&okVelocity);
                                panValue = parts[6].toInt(&okPan);
                                releaseValue = parts[7].toInt(&okRelease);
                                modXValue = parts[8].toInt(&okModX);
                                modYValue = parts[9].toInt(&okModY);
                            }
                            if (okTick && okDuration && okPitch && okColor && okFlags &&
                                okVelocity && okPan && okRelease && okModX && okModY &&
                                durationValue > 0 &&
                                isPluginMidiPitch(pitchValue) &&
                                colorValue >= 0 && colorValue < 16 &&
                                flagsValue >= 0 && flagsValue <= 15) {
                                importedNoteFlagsMeta.push_back({tickValue,
                                                                 durationValue,
                                                                 pitchValue,
                                                                 colorValue,
                                                                 flagsValue,
                                                                 clampIntValue(velocityValue, 1, 127),
                                                                 clampIntValue(panValue, 0, 127),
                                                                 clampIntValue(releaseValue, 0, 127),
                                                                 clampIntValue(modXValue, 0, 127),
                                                                 clampIntValue(modYValue, 0, 127)});
                            }
                        }
                    } else if (metaType == 0x03 && importedName.isEmpty()) {
                        importedName = text;
                    } else if (metaType == 0x04 && importedTrackName.isEmpty()) {
                        importedTrackName = text;
                    }
                } else if (metaType == 0x58 && metaLen >= 2) {
                    const int numerator = static_cast<uint8_t>(bytes[trackCursor]);
                    const int denominatorPower = static_cast<uint8_t>(bytes[trackCursor + 1]);
                    const int denominator = denominatorPower >= 0 && denominatorPower <= 6
                        ? (1 << denominatorPower)
                        : 4;
                    if (numerator > 0 && numerator <= 32 &&
                        validTimeSignatureDenominator(denominator) &&
                        importedSignatureMeta.size() < kMaxTimeSignatureMapPoints) {
                        importedSignatureMeta.push_back({tick, trackIndex,
                            static_cast<int>(importedSignatureMeta.size()),
                            numerator, denominator});
                    }
                }
                trackCursor += static_cast<int>(metaLen);
                continue;
            }
            if (status == 0xF0 || status == 0xF7) {
                runningStatus = 0;
                quint32 sysexLen = 0;
                if (!readVariableLength(bytes, trackCursor, trackEnd, sysexLen)) {
                    if (errorText) {
                        *errorText = "Malformed system event.";
                    }
                    return false;
                }
                if (static_cast<quint64>(sysexLen) > static_cast<quint64>(trackEnd - trackCursor)) {
                    if (errorText) {
                        *errorText = "Malformed system payload.";
                    }
                    return false;
                }
                trackCursor += static_cast<int>(sysexLen);
                continue;
            }

            if ((status & 0xF0) >= 0x80 && (status & 0xF0) <= 0xE0) {
                runningStatus = status;
                const int dataCount = expectedChannelDataCount(status);
                if (dataCount == 1) {
                    if (trackCursor >= trackEnd) {
                        if (errorText) {
                            *errorText = "Malformed MIDI event.";
                        }
                        return false;
                    }
                    const uint8_t data1 = static_cast<uint8_t>(bytes[trackCursor++]);
                    if (data1 > 127) {
                        if (errorText) {
                            *errorText = "Malformed MIDI event data.";
                        }
                        return false;
                    }
                    parsed.push_back({tick, trackIndex, static_cast<int>(parsed.size()), status, data1, 0});
                } else {
                    if (trackCursor + 1 >= trackEnd) {
                        if (errorText) {
                            *errorText = "Malformed MIDI event.";
                        }
                        return false;
                    }
                    const uint8_t data1 = static_cast<uint8_t>(bytes[trackCursor++]);
                    const uint8_t data2 = static_cast<uint8_t>(bytes[trackCursor++]);
                    if (data1 > 127 || data2 > 127) {
                        if (errorText) {
                            *errorText = "Malformed MIDI event data.";
                        }
                        return false;
                    }
                    parsed.push_back({tick, trackIndex, static_cast<int>(parsed.size()), status, data1, data2});
                }
            } else {
                if (!isMidiRealtimeStatus(status)) {
                    runningStatus = 0;
                }
                const int systemDataCount = expectedSystemDataCount(status);
                if (systemDataCount < 0 || trackCursor + systemDataCount > trackEnd) {
                    if (errorText) {
                        *errorText = "Malformed or unsupported MIDI system event.";
                    }
                    return false;
                }
                trackCursor += systemDataCount;
                continue;
            }
        }
        if (!sawEndOfTrack) {
            if (errorText) *errorText = "MIDI track is missing its end-of-track event.";
            return false;
        }
        trackEndTicks[trackIndex] = tick;
        cursor = trackEnd;
    }
    if (cursor != size) {
        if (errorText) *errorText = "MIDI file contains trailing bytes after its declared tracks.";
        return false;
    }

    const double projectTempo = finiteClamp(
        m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm,
        20.0, 999.0, kDefaultTempoBpm);
    std::stable_sort(importedTempoMeta.begin(), importedTempoMeta.end(),
        [](const ImportedTempoMeta& left, const ImportedTempoMeta& right) {
            if (left.tick != right.tick) return left.tick < right.tick;
            if (left.sourceTrack != right.sourceTrack) return left.sourceTrack > right.sourceTrack;
            return left.sequence < right.sequence;
        });
    std::vector<ImportedTempoMeta> collapsedTempoMeta;
    collapsedTempoMeta.reserve(importedTempoMeta.size());
    for (const ImportedTempoMeta& point : importedTempoMeta) {
        if (!collapsedTempoMeta.empty() && collapsedTempoMeta.back().tick == point.tick)
            collapsedTempoMeta.back() = point;
        else
            collapsedTempoMeta.push_back(point);
    }
    struct TempoTickSegment
    {
        qint64 tick = 0;
        double beat = 0.0;
        double bpm = 120.0;
    };
    std::vector<TempoTickSegment> tempoTickSegments;
    tempoTickSegments.reserve(collapsedTempoMeta.size() + 1u);
    tempoTickSegments.push_back({0, 0.0, projectTempo});
    for (const ImportedTempoMeta& point : collapsedTempoMeta) {
        if (point.tick < 0) continue;
        double pointBeat = 0.0;
        if (smpteTiming) {
            const TempoTickSegment& previous = tempoTickSegments.back();
            pointBeat = previous.beat +
                (static_cast<double>(point.tick - previous.tick) / smpteTicksPerSecond) * previous.bpm / 60.0;
        } else {
            pointBeat = static_cast<double>(point.tick) / static_cast<double>(division);
        }
        if (!std::isfinite(pointBeat) || pointBeat < 0.0) continue;
        if (tempoTickSegments.back().tick == point.tick) {
            tempoTickSegments.back().beat = pointBeat;
            tempoTickSegments.back().bpm = point.bpm;
        } else {
            tempoTickSegments.push_back({point.tick, pointBeat, point.bpm});
        }
    }
    std::vector<MidiTempoPoint> importedTempoMap;
    importedTempoMap.reserve(tempoTickSegments.size());
    for (const TempoTickSegment& segment : tempoTickSegments)
        importedTempoMap.push_back({segment.beat, segment.bpm});
    importedTempoMap = normalizedTempoMap(importedTempoMap, projectTempo);
    importedTempoBpm = tempoAtBeat(importedTempoMap, 0.0, projectTempo);
    if (smpteTiming) ticksPerBeat = smpteTicksPerSecond * 60.0 / importedTempoBpm;
    if (!std::isfinite(ticksPerBeat) || ticksPerBeat <= 0.0) {
        if (errorText) *errorText = "Invalid MIDI timing division.";
        return false;
    }
    std::stable_sort(parsed.begin(), parsed.end(), [](const ParsedMidiEvent& a, const ParsedMidiEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        const int aRank = midiEventSortRank(a.status, a.data2);
        const int bRank = midiEventSortRank(b.status, b.data2);
        if (aRank != bRank) return aRank < bRank;
        if (a.sourceTrack != b.sourceTrack) return a.sourceTrack < b.sourceTrack;
        return a.sequence < b.sequence;
    });
    const auto tickToBeat = [&](qint64 tick) {
        if (tick <= 0) return 0.0;
        if (!smpteTiming) return static_cast<double>(tick) / static_cast<double>(division);
        const auto next = std::upper_bound(tempoTickSegments.begin(), tempoTickSegments.end(), tick,
            [](qint64 targetTick, const TempoTickSegment& segment) { return targetTick < segment.tick; });
        const TempoTickSegment& segment = next == tempoTickSegments.begin()
            ? tempoTickSegments.front() : *(next - 1);
        return segment.beat +
            (static_cast<double>(tick - segment.tick) / smpteTicksPerSecond) * segment.bpm / 60.0;
    };
    std::stable_sort(importedSignatureMeta.begin(), importedSignatureMeta.end(),
        [](const ImportedTimeSignatureMeta& left, const ImportedTimeSignatureMeta& right) {
            if (left.tick != right.tick) return left.tick < right.tick;
            if (left.sourceTrack != right.sourceTrack) return left.sourceTrack > right.sourceTrack;
            return left.sequence < right.sequence;
        });
    std::vector<MidiTimeSignaturePoint> importedSignatureMap;
    importedSignatureMap.reserve(importedSignatureMeta.size());
    qint64 previousSignatureTick = std::numeric_limits<qint64>::min();
    for (const ImportedTimeSignatureMeta& point : importedSignatureMeta) {
        const MidiTimeSignaturePoint converted{
            tickToBeat(point.tick), point.numerator, point.denominator};
        if (point.tick == previousSignatureTick && !importedSignatureMap.empty())
            importedSignatureMap.back() = converted;
        else
            importedSignatureMap.push_back(converted);
        previousSignatureTick = point.tick;
    }
    const int fallbackNumerator = m_transportWidget ? m_transportWidget->signatureNumerator() : 4;
    const int fallbackDenominator = m_transportWidget ? m_transportWidget->signatureDenominator() : 4;
    importedSignatureMap = normalizedTimeSignatureMap(
        importedSignatureMap, fallbackNumerator, fallbackDenominator);

    struct PendingNote {
        double startBeat = 0.0;
        int velocity = 100;
        int colorGroup = 0;
        int pan = 64;
        int modX = 64;
        int modY = 64;
    };
    struct ImportControllerState {
        int pan = 64;
        int modX = 64;
        int modY = 64;
    };
    std::vector<std::vector<std::vector<PendingNote>>> pendingByTrack(
        qMax(1, static_cast<int>(trackCount)),
        std::vector<std::vector<PendingNote>>(128));
    std::vector<std::vector<ImportControllerState>> controllerStateByTrack(
        qMax(1, static_cast<int>(trackCount)),
        std::vector<ImportControllerState>(16));
    std::vector<MidiNote> notes;
    std::vector<MidiEvent> performanceEvents;
    for (const ParsedMidiEvent& event : parsed) {
        const uint8_t highNibble = event.status & 0xF0;
        if (event.data1 > 127 || event.sourceTrack < 0 || event.sourceTrack >= static_cast<int>(pendingByTrack.size())) {
            continue;
        }

        const double beat = tickToBeat(event.tick);
        const int colorGroup = event.status & 0x0F;
        if (highNibble == 0xB0 && colorGroup >= 0 && colorGroup < 16) {
            ImportControllerState& state = controllerStateByTrack[event.sourceTrack][colorGroup];
            const int value = clampIntValue(static_cast<int>(event.data2), 0, 127);
            if (event.data1 == kMidiCcPan) {
                state.pan = value;
            } else if (event.data1 == kMidiCcModX) {
                state.modX = value;
            } else if (event.data1 == kMidiCcModY) {
                state.modY = value;
            }
            performanceEvents.push_back({beat, event.status, event.data1, event.data2});
            continue;
        }

        if (highNibble == 0xA0 || highNibble == 0xC0 ||
            highNibble == 0xD0 || highNibble == 0xE0) {
            performanceEvents.push_back({beat, event.status, event.data1, event.data2});
            continue;
        }

        if (highNibble == 0x90 && event.data2 > 0) {
            const ImportControllerState& state = controllerStateByTrack[event.sourceTrack][colorGroup];
            pendingByTrack[event.sourceTrack][event.data1].push_back({
                beat,
                static_cast<int>(event.data2),
                colorGroup,
                state.pan,
                state.modX,
                state.modY});
            continue;
        }

        if (highNibble == 0x80 || (highNibble == 0x90 && event.data2 == 0)) {
            auto& pendingForPitch = pendingByTrack[event.sourceTrack][event.data1];
            auto pendingIt = std::find_if(pendingForPitch.begin(), pendingForPitch.end(),
                [colorGroup](const PendingNote& note) {
                    return note.colorGroup == colorGroup;
                });
            if (pendingIt == pendingForPitch.end()) {
                continue;
            }
            const PendingNote startNote = *pendingIt;
            pendingForPitch.erase(pendingIt);
            const double noteLength = beat - startNote.startBeat;
            if (!isFiniteBeatRange(startNote.startBeat, noteLength)) {
                continue;
            }
            MidiNote note;
            note.pitch = event.data1;
            note.startBeat = startNote.startBeat;
            note.lengthBeats = noteLength;
            note.velocity = startNote.velocity;
            note.colorGroup = startNote.colorGroup;
            note.pan = startNote.pan;
            note.release = clampIntValue(static_cast<int>(event.data2), 0, 127);
            note.modX = startNote.modX;
            note.modY = startNote.modY;
            notes.push_back(note);
        }
    }

    for (int trackIndex = 0; trackIndex < static_cast<int>(pendingByTrack.size()); ++trackIndex) {
        const double trackEndBeat = trackIndex < static_cast<int>(trackEndTicks.size())
            ? tickToBeat(trackEndTicks[trackIndex])
            : 0.0;
        for (int pitch = 0; pitch < static_cast<int>(pendingByTrack[trackIndex].size()); ++pitch) {
            for (const PendingNote& startNote : pendingByTrack[trackIndex][pitch]) {
                const double endBeat = qMax(trackEndBeat, startNote.startBeat + 1.0);
                const double noteLength = endBeat - startNote.startBeat;
                if (!isFiniteBeatRange(startNote.startBeat, noteLength)) {
                    continue;
                }
                MidiNote note;
                note.pitch = pitch;
                note.startBeat = startNote.startBeat;
                note.lengthBeats = noteLength;
                note.velocity = startNote.velocity;
                note.colorGroup = startNote.colorGroup;
                note.pan = startNote.pan;
                note.modX = startNote.modX;
                note.modY = startNote.modY;
                notes.push_back(note);
            }
        }
    }

    std::stable_sort(notes.begin(), notes.end(), [](const MidiNote& a, const MidiNote& b) {
        if (a.startBeat != b.startBeat) return a.startBeat < b.startBeat;
        return a.pitch < b.pitch;
    });

    for (const ImportedGroupMeta& meta : importedGroupMeta) {
        for (MidiNote& note : notes) {
            if (note.groupId != 0 || note.pitch != meta.pitch || note.colorGroup != meta.colorGroup) {
                continue;
            }
            const double noteTickDouble = qMax(0.0, note.startBeat) * ticksPerBeat;
            if (!std::isfinite(noteTickDouble)) {
                continue;
            }
            const quint32 noteTick = noteTickDouble > static_cast<double>(std::numeric_limits<quint32>::max())
                ? std::numeric_limits<quint32>::max()
                : static_cast<quint32>(std::llround(noteTickDouble));
            if (noteTick == meta.tick) {
                note.groupId = meta.groupId;
                break;
            }
        }
    }
    std::vector<bool> noteFlagsMatched(notes.size(), false);
    for (const ImportedNoteFlagsMeta& meta : importedNoteFlagsMeta) {
        MidiNote* target = nullptr;
        int targetIndex = -1;
        for (int noteIndex = 0; noteIndex < static_cast<int>(notes.size()); ++noteIndex) {
            if (noteIndex < static_cast<int>(noteFlagsMatched.size()) && noteFlagsMatched[noteIndex]) {
                continue;
            }
            MidiNote& note = notes[noteIndex];
            if (note.pitch != meta.pitch || note.colorGroup != meta.colorGroup) {
                continue;
            }
            const double noteTickDouble = qMax(0.0, note.startBeat) * ticksPerBeat;
            if (!std::isfinite(noteTickDouble)) {
                continue;
            }
            const quint32 noteTick = noteTickDouble > static_cast<double>(std::numeric_limits<quint32>::max())
                ? std::numeric_limits<quint32>::max()
                : static_cast<quint32>(std::llround(noteTickDouble));
            const double durationTickDouble = qMax(0.0, note.lengthBeats) * ticksPerBeat;
            if (!std::isfinite(durationTickDouble)) {
                continue;
            }
            const quint32 durationTick = durationTickDouble > static_cast<double>(std::numeric_limits<quint32>::max())
                ? std::numeric_limits<quint32>::max()
                : static_cast<quint32>(std::llround(durationTickDouble));
            if (noteTick == meta.tick && durationTick == meta.durationTicks) {
                target = &note;
                targetIndex = noteIndex;
                break;
            }
        }
        if (!target) {
            MidiNote restored;
            restored.pitch = meta.pitch;
            restored.startBeat = static_cast<double>(meta.tick) / ticksPerBeat;
            restored.lengthBeats = qMax(1.0 / ticksPerBeat, static_cast<double>(meta.durationTicks) / ticksPerBeat);
            restored.colorGroup = meta.colorGroup;
            notes.push_back(restored);
            noteFlagsMatched.push_back(false);
            target = &notes.back();
            targetIndex = static_cast<int>(notes.size()) - 1;
        }
        if (targetIndex >= 0 && targetIndex < static_cast<int>(noteFlagsMatched.size())) {
            noteFlagsMatched[targetIndex] = true;
        }
        target->velocity = meta.velocity;
        target->pan = meta.pan;
        target->release = meta.release;
        target->modX = meta.modX;
        target->modY = meta.modY;
        target->isKeyswitch = (meta.flags & 1) != 0;
        target->isSlide = (meta.flags & 2) != 0;
        target->isPortamento = (meta.flags & 4) != 0;
        target->muted = (meta.flags & 8) != 0;
    }
    for (const ImportedGroupMeta& meta : importedGroupMeta) {
        for (MidiNote& note : notes) {
            if (note.groupId != 0 || note.pitch != meta.pitch || note.colorGroup != meta.colorGroup) {
                continue;
            }
            const double noteTickDouble = qMax(0.0, note.startBeat) * ticksPerBeat;
            if (!std::isfinite(noteTickDouble)) {
                continue;
            }
            const quint32 noteTick = noteTickDouble > static_cast<double>(std::numeric_limits<quint32>::max())
                ? std::numeric_limits<quint32>::max()
                : static_cast<quint32>(std::llround(noteTickDouble));
            if (noteTick == meta.tick) {
                note.groupId = meta.groupId;
                break;
            }
        }
    }
    std::stable_sort(notes.begin(), notes.end(), [](const MidiNote& a, const MidiNote& b) {
        if (a.startBeat != b.startBeat) return a.startBeat < b.startBeat;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        return a.colorGroup < b.colorGroup;
    });

    outDoc.notes = std::move(notes);
    outDoc.performanceEvents = std::move(performanceEvents);
    outDoc.name = importedName;
    outDoc.trackName = importedTrackName;
    outDoc.tempoBpm = importedTempoBpm;
    outDoc.tempoMap = std::move(importedTempoMap);
    outDoc.timeSignatureMap = std::move(importedSignatureMap);
    const MidiTimeSignaturePoint initialSignature = timeSignatureAtBeat(
        outDoc.timeSignatureMap, 0.0, fallbackNumerator, fallbackDenominator);
    outDoc.timeSignatureNumerator = initialSignature.numerator;
    outDoc.timeSignatureDenominator = initialSignature.denominator;
    const double importedBeatsPerBar = projectBeatsPerBar(
        outDoc.timeSignatureNumerator,
        outDoc.timeSignatureDenominator);
    outDoc.lengthBeats = qMax(1.0, importedBeatsPerBar);
    if (!trackEndTicks.empty()) {
        const qint64 maxTrackEndTick = *std::max_element(trackEndTicks.begin(), trackEndTicks.end());
        if (maxTrackEndTick > 0) {
            outDoc.lengthBeats = qMax(outDoc.lengthBeats,
                tickToBeat(maxTrackEndTick));
        }
    }
    for (const MidiNote& note : outDoc.notes) {
        outDoc.lengthBeats = qMax(outDoc.lengthBeats, note.startBeat + nonNegativeDouble(note.lengthBeats));
    }
    for (const MidiEvent& event : outDoc.performanceEvents) {
        if (std::isfinite(event.beat) && event.beat >= 0.0) {
            outDoc.lengthBeats = qMax(outDoc.lengthBeats, event.beat + 1.0);
        }
    }
    return true;
}

bool VeraCoreQT::importMidiTracksFromFile(const QString& path,
                                          std::vector<MidiClipDocument>& tracks,
                                          MidiClipDocument& conductor,
                                          QString* errorText) const
{
    tracks.clear();
    conductor = MidiClipDocument();
    QFile source(path);
    constexpr qint64 maxMidiImportBytes = 512ll * 1024ll * 1024ll;
    if (!source.open(QIODevice::ReadOnly)) {
        if (errorText) *errorText = "Could not open selected MIDI file.";
        return false;
    }
    if (source.size() < 14 || source.size() > maxMidiImportBytes) {
        if (errorText) *errorText = "MIDI file size is outside the supported import range.";
        return false;
    }
    const QByteArray bytes = source.readAll();
    if (source.error() != QFileDevice::NoError || bytes.size() != source.size()) {
        if (errorText) *errorText = "Could not read the complete MIDI file.";
        return false;
    }
    source.close();

    quint32 headerLength = 0;
    quint16 format = 0;
    quint16 trackCount = 0;
    quint16 division = 0;
    if (bytes.left(4) != "MThd" || !readUInt32BE(bytes, 4, headerLength) || headerLength < 6 ||
        !readUInt16BE(bytes, 8, format) || !readUInt16BE(bytes, 10, trackCount) ||
        !readUInt16BE(bytes, 12, division) || division == 0 || trackCount == 0 || trackCount > 4096) {
        if (errorText) *errorText = "Invalid or unsupported MIDI header.";
        return false;
    }
    if (format == 0) {
        MidiClipDocument document;
        if (!importMidiDocumentFromFile(path, document, errorText)) return false;
        conductor = document;
        tracks.push_back(std::move(document));
        return true;
    }
    if (format != 1) {
        if (errorText) *errorText = "Only Standard MIDI File Type 0 and Type 1 can be imported.";
        return false;
    }
    const quint64 firstTrackOffset64 = 8ull + headerLength;
    if (firstTrackOffset64 > static_cast<quint64>(bytes.size()) ||
        firstTrackOffset64 > static_cast<quint64>(std::numeric_limits<int>::max())) {
        if (errorText) *errorText = "MIDI header extends beyond the file.";
        return false;
    }
    if (!importMidiDocumentFromFile(path, conductor, errorText)) return false;

    int cursor = static_cast<int>(firstTrackOffset64);
    for (int sourceTrackIndex = 0; sourceTrackIndex < trackCount; ++sourceTrackIndex) {
        if (cursor < 0 || cursor + 8 > bytes.size() || bytes.mid(cursor, 4) != "MTrk") {
            if (errorText) *errorText = "Missing MIDI track chunk while splitting Type 1 file.";
            tracks.clear();
            return false;
        }
        quint32 payloadLength = 0;
        if (!readUInt32BE(bytes, cursor + 4, payloadLength)) {
            if (errorText) *errorText = "Invalid MIDI track length.";
            tracks.clear();
            return false;
        }
        const quint64 chunkLength = 8ull + payloadLength;
        if (chunkLength > static_cast<quint64>(std::numeric_limits<int>::max()) ||
            static_cast<quint64>(cursor) + chunkLength > static_cast<quint64>(bytes.size())) {
            if (errorText) *errorText = "MIDI track chunk extends beyond the file.";
            tracks.clear();
            return false;
        }

        QTemporaryFile trackFile(QDir::tempPath() + "/VeraCoreMidiImportXXXXXX.mid");
        if (!trackFile.open()) {
            if (errorText) *errorText = "Could not create a temporary MIDI track for import.";
            tracks.clear();
            return false;
        }
        if (trackFile.write("MThd", 4) != 4) {
            if (errorText) *errorText = "Could not write a temporary MIDI header.";
            tracks.clear();
            return false;
        }
        writeUInt32BE(trackFile, 6);
        writeUInt16BE(trackFile, 0);
        writeUInt16BE(trackFile, 1);
        writeUInt16BE(trackFile, division);
        const double sourceTempo = std::isfinite(conductor.tempoBpm) && conductor.tempoBpm > 0.0
            ? conductor.tempoBpm
            : (m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm);
        const quint32 tempo = tempoBpmToMidiMicroseconds(sourceTempo);
        const int numerator = qBound(1, conductor.timeSignatureNumerator, 255);
        const int denominator = conductor.timeSignatureDenominator;
        QByteArray timingMeta;
        timingMeta.append(static_cast<char>(0x00));
        timingMeta.append(static_cast<char>(0xff));
        timingMeta.append(static_cast<char>(0x51));
        timingMeta.append(static_cast<char>(0x03));
        timingMeta.append(static_cast<char>((tempo >> 16) & 0xff));
        timingMeta.append(static_cast<char>((tempo >> 8) & 0xff));
        timingMeta.append(static_cast<char>(tempo & 0xff));
        timingMeta.append(static_cast<char>(0x00));
        timingMeta.append(static_cast<char>(0xff));
        timingMeta.append(static_cast<char>(0x58));
        timingMeta.append(static_cast<char>(0x04));
        timingMeta.append(static_cast<char>(numerator));
        timingMeta.append(static_cast<char>(midiDenominatorPower(denominator)));
        timingMeta.append(static_cast<char>(24));
        timingMeta.append(static_cast<char>(8));
        const quint64 wrappedPayloadLength = static_cast<quint64>(payloadLength) +
            static_cast<quint64>(timingMeta.size());
        if (wrappedPayloadLength > std::numeric_limits<quint32>::max() ||
            trackFile.write("MTrk", 4) != 4) {
            if (errorText) *errorText = "MIDI track is too large to add conductor timing metadata.";
            tracks.clear();
            return false;
        }
        writeUInt32BE(trackFile, static_cast<quint32>(wrappedPayloadLength));
        const int safePayloadLength = static_cast<int>(payloadLength);
        if (trackFile.write(timingMeta) != timingMeta.size() ||
            trackFile.write(bytes.constData() + cursor + 8, safePayloadLength) != safePayloadLength ||
            trackFile.error() != QFileDevice::NoError || !trackFile.flush()) {
            if (errorText) *errorText = "Could not materialize a temporary MIDI track.";
            tracks.clear();
            return false;
        }
        const QString trackPath = trackFile.fileName();
        trackFile.close();

        MidiClipDocument document;
        QString trackError;
        if (!importMidiDocumentFromFile(trackPath, document, &trackError)) {
            if (errorText) *errorText = trackError.isEmpty()
                ? QString("Could not decode MIDI track %1.").arg(sourceTrackIndex + 1)
                : trackError;
            tracks.clear();
            return false;
        }
        if (!document.notes.empty() || !document.performanceEvents.empty()) {
            document.trackIndex = sourceTrackIndex;
            document.tempoBpm = conductor.tempoBpm;
            document.tempoMap = conductor.tempoMap;
            document.timeSignatureNumerator = conductor.timeSignatureNumerator;
            document.timeSignatureDenominator = conductor.timeSignatureDenominator;
            document.timeSignatureMap = conductor.timeSignatureMap;
            if (document.trackName.trimmed().isEmpty()) document.trackName = document.name.trimmed();
            tracks.push_back(std::move(document));
        }
        cursor += static_cast<int>(chunkLength);
    }
    if (tracks.empty()) {
        if (errorText) *errorText = "The Type 1 MIDI file contains no note or channel-event tracks.";
        return false;
    }
    return true;
}

bool VeraCoreQT::renderOfflineWavWithSynth(const QString& path, const std::vector<VeraCoreBeatMidiEvent>& events, const std::vector<OfflineAudioClip>& audioClips, double lengthBeats, int sampleRate, OfflineRenderStats* stats, const std::function<bool(quint64, quint64)>& progress, bool isolatedClipRender, double timelineBeatOrigin) const
{
    if (stats) {
        *stats = OfflineRenderStats{};
    }
    const int safeSampleRate = qBound(8000, sampleRate, 384000);
    const double arrangementLength = std::isfinite(lengthBeats) && lengthBeats > 0.0
        ? (isolatedClipRender ? qMax(0.0001, lengthBeats) : qMax(1.0, lengthBeats))
        : 1.0;
    const double renderTempo = m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm;
    const double safeTempo = std::isfinite(renderTempo) && renderTempo > 0.0
        ? qBound(1.0, renderTempo, 999.0)
        : static_cast<double>(kDefaultTempoBpm);
    const double safeBeatOrigin = isolatedClipRender && std::isfinite(timelineBeatOrigin)
        ? qMax(0.0, timelineBeatOrigin) : 0.0;
    const std::vector<MidiTempoPoint> projectTempoMap = normalizedTempoMap(
        m_tempoMap, safeTempo);
    std::vector<MidiTempoPoint> renderTempoMap;
    if (safeBeatOrigin > 0.0) {
        renderTempoMap.push_back({0.0, tempoAtBeat(
            projectTempoMap, safeBeatOrigin, safeTempo)});
        for (const MidiTempoPoint& point : projectTempoMap) {
            if (point.beat > safeBeatOrigin &&
                point.beat <= safeBeatOrigin + arrangementLength)
                renderTempoMap.push_back({point.beat - safeBeatOrigin, point.bpm});
        }
    } else {
        renderTempoMap = projectTempoMap;
    }
    const TempoClock tempoClock(renderTempoMap, safeTempo);
    const double initialTempo = tempoClock.bpmAtBeat(0.0);
    const int defaultNumerator = m_transportWidget
        ? m_transportWidget->signatureNumerator() : 4;
    const int defaultDenominator = m_transportWidget
        ? m_transportWidget->signatureDenominator() : 4;
    const std::vector<MidiTimeSignaturePoint> projectSignatureMap =
        normalizedTimeSignatureMap(m_timeSignatureMap,
            defaultNumerator, defaultDenominator);
    std::vector<MidiTimeSignaturePoint> renderSignatureMap;
    if (safeBeatOrigin > 0.0) {
        const MidiTimeSignaturePoint initial = timeSignatureAtBeat(
            projectSignatureMap, safeBeatOrigin, defaultNumerator, defaultDenominator);
        renderSignatureMap.push_back({0.0, initial.numerator, initial.denominator});
        for (const MidiTimeSignaturePoint& point : projectSignatureMap) {
            if (point.beat > safeBeatOrigin &&
                point.beat <= safeBeatOrigin + arrangementLength)
                renderSignatureMap.push_back({point.beat - safeBeatOrigin,
                    point.numerator, point.denominator});
        }
    } else {
        renderSignatureMap = projectSignatureMap;
    }
    const MidiTimeSignaturePoint initialSignature = timeSignatureAtBeat(
        renderSignatureMap, 0.0, 4, 4);
    ExternalPluginHost offlinePluginHost;
    if (!isolatedClipRender && m_pluginHost) {
        for (int trackIndex = 0; trackIndex < static_cast<int>(m_tracks.size()); ++trackIndex) {
            const auto& track = m_tracks[trackIndex];
            if (!track) continue;
            const auto& fxSlots = track->GetAudioFxSlots();
            for (int fxIndex = 0; fxIndex < static_cast<int>(fxSlots.size()); ++fxIndex) {
                const QByteArray state = m_pluginHost->captureAudioFxState(trackIndex, fxIndex);
                if (!state.isEmpty()) track->SetAudioFxPluginState(fxIndex, state);
            }
            auto* midi = dynamic_cast<MidiTrack*>(track.get());
            if (!midi) continue;
            const auto& layers = midi->GetLayerSlots();
            for (int layerIndex = 0; layerIndex < static_cast<int>(layers.size()); ++layerIndex) {
                const QByteArray state = m_pluginHost->captureLayerState(
                    trackIndex * kLayerIdStride + layerIndex);
                if (!state.isEmpty()) midi->SetLayerSlotPluginState(layerIndex, state);
            }
        }
    }
    std::vector<AraAudioSourceDescriptor> offlineAraSources;
    std::set<QString> offlineAraContentIds;
    std::map<QString, double> offlineAraSourceDurations;
    offlineAraSources.reserve(audioClips.size());
    for (const OfflineAudioClip& clip : audioClips) {
        const QString contentId = clip.contentId.trimmed();
        if (contentId.isEmpty() || !offlineAraContentIds.insert(contentId).second) continue;
        const DecodedAudioFile* source = cachedDecodedAudio(clip.sourcePath);
        const auto sharedSamples = sharedPlaybackSamples(source);
        if (!source || source->sampleRate <= 0 || !sharedSamples) continue;
        AraAudioSourceDescriptor descriptor;
        descriptor.persistentId = contentId.toUtf8().toStdString();
        descriptor.nameUtf8 = clip.name.toUtf8().toStdString();
        descriptor.sampleRate = source->sampleRate;
        descriptor.channelCount = source->sourceChannels == 1 ? 1 : 2;
        descriptor.interleavedStereoSamples = sharedSamples;
        offlineAraSources.push_back(std::move(descriptor));
        offlineAraSourceDurations[contentId] = static_cast<double>(sharedSamples->size() / 2u) /
            static_cast<double>(source->sampleRate);
    }
    std::vector<AraPlaybackRegionDescriptor> offlineAraRegions;
    offlineAraRegions.reserve(audioClips.size());
    for (const OfflineAudioClip& clip : audioClips) {
        const QString clipId = clip.clipId.trimmed();
        const QString contentId = clip.contentId.trimmed();
        const auto duration = offlineAraSourceDurations.find(contentId);
        if (clipId.isEmpty() || duration == offlineAraSourceDurations.end() ||
            clip.trackIndex < 0 || clip.trackIndex >= static_cast<int>(m_tracks.size()) ||
            !isFiniteBeatRange(clip.startBeat, clip.lengthBeats)) continue;
        const Track* track = m_tracks[clip.trackIndex].get();
        if (!track || track->GetType() != TrackType::Audio) continue;
        AraClipRegionInput regionInput;
        regionInput.clipId = clipId;
        regionInput.contentId = contentId;
        regionInput.name = clip.name;
        regionInput.trackName = track->GetName();
        regionInput.trackIndex = clip.trackIndex;
        regionInput.startBeat = clip.startBeat;
        regionInput.lengthBeats = clip.lengthBeats;
        regionInput.sourceOffsetBeats = clip.sourceOffsetBeats;
        regionInput.loopEnabled = clip.loopEnabled;
        regionInput.loopStartBeats = clip.loopStartBeats;
        regionInput.loopLengthBeats = clip.loopLengthBeats;
        regionInput.warpLengthBeats = clip.warpLengthBeats;
        regionInput.sourceTempoBpm = std::isfinite(clip.sourceTempoBpm) &&
            clip.sourceTempoBpm > 0.0 ? clip.sourceTempoBpm : safeTempo;
        regionInput.syncMode = clip.syncMode;
        regionInput.warpMarkers = &clip.warpMarkers;
        appendAraClipRegions(regionInput, duration->second, tempoClock, offlineAraRegions);
    }
    const std::vector<AraTempoEntryDescriptor> offlineAraTempoEntries = buildAraTempoEntries(
        m_tempoMap, tempoClock, safeTempo, arrangementLength);
    const std::vector<AraBarSignatureDescriptor> offlineAraBarSignatures = buildAraBarSignatures(
        renderSignatureMap, initialSignature.numerator, initialSignature.denominator);
    if (!isolatedClipRender) {
        offlinePluginHost.syncAraModelGraph(offlineAraSources, offlineAraRegions,
            offlineAraTempoEntries, offlineAraBarSignatures);
        offlinePluginHost.setOfflineProcessing(true);
        offlinePluginHost.syncLayers(m_tracks, m_mixerParameters->returnBuses(),
                                     m_mixerParameters->masterBus(), safeSampleRate,
								     m_projectSpeakerLayout);
        offlinePluginHost.setTransport(
            true, false, initialTempo, 0.0,
            initialSignature.numerator, initialSignature.denominator,
            0.0, 0.0, 0);
    }
    const uint32_t pluginTailSamples = isolatedClipRender
        ? 0u : offlinePluginHost.maxTailSamples();
    uint32_t pluginLatencySamples = 0;
    if (!isolatedClipRender) {
        for (const auto& entry : offlinePluginHost.trackLatencySamples())
            if (entry.first != VeraCoreMasterAudioFxTrackId)
                pluginLatencySamples = std::max(pluginLatencySamples, entry.second);
    }
    const VeraCoreSpeakerLayout offlineSpeakerLayout = isolatedClipRender
        ? VeraCoreSpeakerLayout::Stereo : m_projectSpeakerLayout;
    const quint16 channels = static_cast<quint16>(
        VeraCoreSpeakerChannelCount(offlineSpeakerLayout));
    const quint16 bitsPerSample = 32;
    const quint32 bytesPerFrame = channels * (bitsPerSample / 8);
    const double arrangementFrameCount = std::ceil(
        tempoClock.secondsAtBeat(arrangementLength) * safeSampleRate);
    if (!std::isfinite(arrangementFrameCount) || arrangementFrameCount <= 0.0 ||
        arrangementFrameCount > static_cast<double>(std::numeric_limits<quint32>::max())) {
        return false;
    }
    const quint64 outputFrameCount = static_cast<quint64>(arrangementFrameCount) + pluginTailSamples;
    const quint64 processingFrameCount = outputFrameCount + pluginLatencySamples;
    if (outputFrameCount == 0 || processingFrameCount > std::numeric_limits<quint32>::max() ||
        outputFrameCount > (std::numeric_limits<quint32>::max() -
            (channels > 2 ? 72u : 48u)) / bytesPerFrame) {
        return false;
    }
    const quint32 frames = static_cast<quint32>(processingFrameCount);
    const quint32 outputFrames = static_cast<quint32>(outputFrameCount);
    const quint32 latencyTrimFrames = pluginLatencySamples;
    const quint32 dataBytes = outputFrames * bytesPerFrame;
    const double safeLengthBeats = tempoClock.beatAtSeconds(
        static_cast<double>(frames) / safeSampleRate);

    struct ActiveNote {
        int pitch = 0;
        int channel = 0;
        int layerId = 0;
        double phase = 0.0;
        double amp = 0.0;
        double pressureGain = 1.0;
        double pan = 0.0;
        double panLeft = 0.7071067811865476;
        double panRight = 0.7071067811865476;
        double pitchBendSemitones = 0.0;
        double currentPitch = 0.0;
        double glideTargetPitch = 0.0;
        double glideStepPerSample = 0.0;
        int glideSamplesRemaining = 0;
        int noteOnCount = 0;
    };
    struct OfflinePitchBendState {
        int channel = 0;
        int layerId = 0;
        double semitones = 0.0;
    };
    struct OfflinePressureState {
        int channel = 0;
        int layerId = 0;
        double gain = 1.0;
    };
    struct DecodedRenderClip {
        int trackIndex = -1;
        std::shared_ptr<const std::vector<float>> samples;
        int sourceChannels = 2;
        quint32 startFrame = 0;
        quint32 lengthFrames = 0;
        double startBeat = 0.0;
        double lengthBeats = 0.0;
        bool loopEnabled = false;
        double loopLengthBeats = 0.0;
        double loopStartProgress = 0.0;
        double sourceStartProgress = 0.0;
        double sourceProgressSpan = 1.0;
        double pitchShiftSemitones = 0.0;
        float gain = 1.0f;
        float pan = 0.0f;
        float normalizationGain = 1.0f;
        bool reverse = false;
        bool reversePolarity = false;
        double fadeInBeats = 0.0;
        double fadeOutBeats = 0.0;
        std::vector<AudioGainEnvelopePoint> gainEnvelope;
        double fadeInCurve = 0.0;
        double fadeOutCurve = 0.0;
        bool timeMappedSync = false;
        std::shared_ptr<const std::vector<VeraCoreAudioWarpPoint>> userWarpPoints;
        double projectStartSeconds = 0.0;
        double projectDurationSeconds = 0.0;
    };

    std::vector<RtMidiEvent> sortedEvents;
    sortedEvents.reserve(events.size());
    for (const VeraCoreBeatMidiEvent& event : events) {
        if (!std::isfinite(event.beat) || event.beat < 0.0 || event.beat > safeLengthBeats ||
            !isValidMidiChannelEvent(event.status, event.data1, event.data2)) {
            continue;
        }
        const float gain = std::isfinite(event.gain) ? event.gain : 1.0f;
        const float pan = std::isfinite(event.pan) ? event.pan : 0.0f;
        const bool externalPlugin = offlinePluginHost.isLayerHosted(event.layerId);
        if (event.isKeyswitch && !externalPlugin) continue;
        if (event.isSlide && externalPlugin) continue;
        sortedEvents.push_back({event.beat, event.status, event.data1, event.data2, event.layerId,
                                gain, pan, externalPlugin, event.isSlide,
                                event.isPortamento && !externalPlugin, event.glideDurationBeats,
                                event.isKeyswitch});
    }
    std::stable_sort(sortedEvents.begin(), sortedEvents.end(), [](const RtMidiEvent& a, const RtMidiEvent& b) {
        if (a.beat != b.beat) return a.beat < b.beat;
        if (a.isPortamento != b.isPortamento) return a.isPortamento;
        const int aRank = midiEventSortRank(a.status, a.data2);
        const int bRank = midiEventSortRank(b.status, b.data2);
        if (aRank != bRank) return aRank < bRank;
        if (a.isSlide != b.isSlide) return !a.isSlide;
        if (a.layerId != b.layerId) return a.layerId < b.layerId;
        if (a.status != b.status) return a.status < b.status;
        return a.data1 < b.data1;
    });

    std::vector<ActiveNote> activeNotes;
    std::vector<OfflinePitchBendState> pitchBends;
    std::vector<OfflinePressureState> pressureStates;
    int nextEventIndex = 0;
    int nextEventTick = 0;
    auto toSample = [&](const RtMidiEvent& event) {
        const double sampleDouble = std::floor(
            tempoClock.secondsAtBeat(event.beat) * safeSampleRate);
        if (!std::isfinite(sampleDouble) || sampleDouble <= 0.0) {
            return 0;
        }
        const double maxSample = static_cast<double>(frames - 1);
        return static_cast<int>(qBound(0.0, sampleDouble, maxSample));
    };

    if (!sortedEvents.empty()) {
        nextEventTick = toSample(sortedEvents[0]);
    }

    struct TimedPluginEvent {
        int sample = 0;
        VeraCoreMidiEvent event;
    };
    std::vector<TimedPluginEvent> pluginEvents;
    for (const RtMidiEvent& event : sortedEvents) {
        if (!event.externalPlugin) {
            continue;
        }
        VeraCoreMidiEvent pluginEvent;
        pluginEvent.status = event.status;
        pluginEvent.data1 = event.data1;
        pluginEvent.data2 = event.data2;
        pluginEvent.layerId = event.layerId;
        pluginEvent.gain = event.gain;
        pluginEvent.pan = event.pan;
        pluginEvent.fromTimeline = true;
        pluginEvent.externalPlugin = true;
        pluginEvents.push_back({toSample(event), pluginEvent});
    }

    std::vector<DecodedRenderClip> decodedClips;
    decodedClips.reserve(audioClips.size());
    for (const OfflineAudioClip& clip : audioClips) {
        if (clip.sourcePath.trimmed().isEmpty() || !std::isfinite(clip.startBeat) ||
            clip.startBeat > safeLengthBeats) {
            return false;
        }
        bool processedSync = false;
        const double clipTempo = averageTempoForBeatRange(
            tempoClock, clip.startBeat, clip.lengthBeats, safeTempo);
        const DecodedAudioFile* decoded = cachedDecodedAudioForClipSync(clip.sourcePath,
            clip.sourceTempoBpm, clip.pitchShiftSemitones, clip.syncMode, clip.targetBars,
            clip.beatsPerBar, clip.warpLengthBeats, clipTempo, safeSampleRate, &processedSync);
        if (!decoded || decoded->frameCount() <= 0 || decoded->sampleRate <= 0) {
            return false;
        }
        const double safeClipStart = qMax(0.0, clip.startBeat);
        const double lengthBeatValue = std::isfinite(clip.lengthBeats) && clip.lengthBeats > 0.0
            ? clip.lengthBeats
            : safeLengthBeats - safeClipStart;
        const double startFrameDouble = std::floor(
            tempoClock.secondsAtBeat(safeClipStart) * safeSampleRate);
        const double lengthFrameDouble = std::ceil(
            (tempoClock.secondsAtBeat(safeClipStart + lengthBeatValue) -
             tempoClock.secondsAtBeat(safeClipStart)) * safeSampleRate);
        if (!std::isfinite(startFrameDouble) || !std::isfinite(lengthFrameDouble) ||
            startFrameDouble >= static_cast<double>(frames) || lengthFrameDouble <= 0.0) {
            continue;
        }
        DecodedRenderClip renderClip;
        renderClip.trackIndex = clip.trackIndex;
        renderClip.samples = sharedPlaybackSamples(decoded);
        renderClip.sourceChannels = decoded->sourceChannels;
        renderClip.startBeat = safeClipStart;
        renderClip.lengthBeats = lengthBeatValue;
        if (!renderClip.samples || renderClip.samples->size() < 2u) return false;
        renderClip.startFrame = static_cast<quint32>(qBound(0.0, startFrameDouble, static_cast<double>(frames - 1)));
        if (!std::isfinite(lengthFrameDouble) || lengthFrameDouble <= 0.0) {
            continue;
        }
        renderClip.lengthFrames = static_cast<quint32>(qBound(1.0, lengthFrameDouble,
            static_cast<double>(frames - renderClip.startFrame)));
        const double sourceOffset = std::isfinite(clip.sourceOffsetBeats)
            ? qMax(0.0, clip.sourceOffsetBeats)
            : 0.0;
        renderClip.loopEnabled = clip.loopEnabled &&
            std::isfinite(clip.loopStartBeats) && clip.loopStartBeats >= 0.0 &&
            std::isfinite(clip.loopLengthBeats) && clip.loopLengthBeats >= 0.25;
        renderClip.loopLengthBeats = renderClip.loopEnabled ? clip.loopLengthBeats : 0.0;
        const double requiredSourceEnd = renderClip.loopEnabled
            ? clip.loopStartBeats + clip.loopLengthBeats
            : sourceOffset + lengthBeatValue;
        const double fullWarp = std::isfinite(clip.warpLengthBeats) && clip.warpLengthBeats > 0.0
            ? qMax(clip.warpLengthBeats, requiredSourceEnd)
            : requiredSourceEnd;
        renderClip.loopStartProgress = renderClip.loopEnabled
            ? qBound(0.0, clip.loopStartBeats / fullWarp, 1.0) : 0.0;
        renderClip.sourceStartProgress = qBound(0.0, sourceOffset / fullWarp, 1.0);
        renderClip.sourceProgressSpan = renderClip.loopEnabled
            ? qBound(0.0, clip.loopLengthBeats / fullWarp,
                     1.0 - renderClip.loopStartProgress)
            : qBound(0.0, lengthBeatValue / fullWarp,
                     1.0 - renderClip.sourceStartProgress);
        const double decodedDurationSeconds = static_cast<double>(decoded->frameCount()) /
            static_cast<double>(decoded->sampleRate);
        const double warpSourceDuration = clip.sourceDurationSeconds > 0.0
            ? clip.sourceDurationSeconds : decodedDurationSeconds;
        renderClip.userWarpPoints = renderClip.loopEnabled
            ? audioLoopWarpPoints(clip.warpMarkers, warpSourceDuration, fullWarp,
                clip.loopStartBeats, clip.loopLengthBeats, sourceOffset)
            : audioUserWarpPoints(clip.warpMarkers, warpSourceDuration, fullWarp,
                sourceOffset, lengthBeatValue);
        renderClip.pitchShiftSemitones = processedSync || clip.syncMode > 0
            ? 0.0
            : (std::isfinite(clip.pitchShiftSemitones) ? qBound(-48.0, clip.pitchShiftSemitones, 48.0) : 0.0);
        renderClip.gain = std::isfinite(clip.gain) ? qBound(0.0f, clip.gain, 4.0f) : 1.0f;
        renderClip.pan = std::isfinite(clip.pan) ? qBound(-1.0f, clip.pan, 1.0f) : 0.0f;
        renderClip.normalizationGain = clip.normalize ? normalizationGainForDecoded(decoded) : 1.0f;
        renderClip.reverse = clip.reverse;
        renderClip.reversePolarity = clip.reversePolarity;
        renderClip.fadeInBeats = qBound(0.0, clip.fadeInBeats, lengthBeatValue);
        renderClip.fadeOutBeats = qBound(0.0, clip.fadeOutBeats,
                                         lengthBeatValue - renderClip.fadeInBeats);
        renderClip.gainEnvelope = clip.gainEnvelope;
        renderClip.fadeInCurve = qBound(-1.0, clip.fadeInCurve, 1.0);
        renderClip.fadeOutCurve = qBound(-1.0, clip.fadeOutCurve, 1.0);
        renderClip.timeMappedSync = processedSync;
        renderClip.projectStartSeconds = tempoClock.secondsAtBeat(safeClipStart);
        renderClip.projectDurationSeconds = tempoClock.secondsAtBeat(
            safeClipStart + lengthBeatValue) - renderClip.projectStartSeconds;
        decodedClips.push_back(std::move(renderClip));
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setFloatingPointPrecision(QDataStream::SinglePrecision);
    auto failWavWrite = [&]() {
        file.close();
        QFile::remove(path);
        return false;
    };
    auto writeText = [&](const char* text) { return file.write(text, 4) == 4; };
    auto outputOk = [&]() {
        return file.error() == QFileDevice::NoError && out.status() == QDataStream::Ok;
    };
    if (!writeText("RIFF")) return failWavWrite();
    out << quint32((channels > 2 ? 72u : 48u) + dataBytes);
    if (!writeText("WAVE")) return failWavWrite();
    if (!writeText("fmt ")) return failWavWrite();
    out << quint32(channels > 2 ? 40u : 16u);
    out << quint16(channels > 2 ? 0xFFFEu : 3u); // EXTENSIBLE or IEEE float
    out << quint16(channels);
    out << quint32(safeSampleRate);
    out << quint32(safeSampleRate * bytesPerFrame);
    out << quint16(bytesPerFrame);
    out << quint16(bitsPerSample);
    if (channels > 2) {
        out << quint16(22); // cbSize
        out << quint16(bitsPerSample); // valid bits
        out << quint32(VeraCoreWaveSpeakerMask(offlineSpeakerLayout));
        out << quint32(3) << quint16(0) << quint16(0x0010); // IEEE_FLOAT GUID
        const char guidTail[8] = {
            static_cast<char>(0x80), 0x00, 0x00, static_cast<char>(0xAA),
            0x00, 0x38, static_cast<char>(0x9B), 0x71};
        if (file.write(guidTail, 8) != 8) return failWavWrite();
    }
    if (!writeText("fact")) return failWavWrite();
    out << quint32(4);
    out << outputFrames;
    if (!writeText("data")) return failWavWrite();
    out << dataBytes;
    if (!outputOk()) {
        return failWavWrite();
    }

    const double twoPi = 6.2831853071795864769;
    std::array<VeraCoreTruePeakMeter, kVeraCoreMaxSpatialChannels / 2> offlineTruePeakMeters;
    std::array<double, kVeraCoreMaxSpatialChannels> channelTruePeaks{};
    std::array<double, kVeraCoreMaxSpatialChannels> channelPreClampPeaks{};
    std::array<double, kVeraCoreMaxSpatialChannels> channelSquareSums{};
    OfflineLoudnessMeter offlineLoudnessMeter(safeSampleRate, channels);
    qint64 clippedSamples = 0;
    const auto meterOutputFrame = [&](const double* source, int channelCount) {
        std::array<float, kVeraCoreMaxSpatialChannels> finiteFrame{};
        const int safeChannels = std::clamp(channelCount, 1, kVeraCoreMaxSpatialChannels);
        const double floatLimit = static_cast<double>(std::numeric_limits<float>::max());
        for (int channel = 0; channel < safeChannels; ++channel) {
            double value = source && std::isfinite(source[channel]) ? source[channel] : 0.0;
            channelPreClampPeaks[static_cast<size_t>(channel)] = std::max(
                channelPreClampPeaks[static_cast<size_t>(channel)], std::fabs(value));
            channelSquareSums[static_cast<size_t>(channel)] += value * value;
            if (std::fabs(value) > 1.0) ++clippedSamples;
            value = std::clamp(value, -floatLimit, floatLimit);
            finiteFrame[static_cast<size_t>(channel)] = static_cast<float>(value);
        }
        for (int pair = 0; pair * 2 < safeChannels; ++pair) {
            const int firstChannel = pair * 2;
            const std::array<float, 2> pairFrame{
                finiteFrame[static_cast<size_t>(firstChannel)],
                firstChannel + 1 < safeChannels
                    ? finiteFrame[static_cast<size_t>(firstChannel + 1)] : 0.0f};
            const VeraCoreAudioMeterState truePeak =
                offlineTruePeakMeters[static_cast<size_t>(pair)].ProcessBlock(pairFrame.data(), 1);
            channelTruePeaks[static_cast<size_t>(firstChannel)] = std::max(
                channelTruePeaks[static_cast<size_t>(firstChannel)],
                static_cast<double>(truePeak.peakLeft));
            if (firstChannel + 1 < safeChannels) {
                channelTruePeaks[static_cast<size_t>(firstChannel + 1)] = std::max(
                    channelTruePeaks[static_cast<size_t>(firstChannel + 1)],
                    static_cast<double>(truePeak.peakRight));
            }
        }
        offlineLoudnessMeter.process(finiteFrame.data(), safeChannels);
        return finiteFrame;
    };
    constexpr quint32 kOfflinePluginBlockFrames = 512;
    std::map<int, QVector<float>> trackBlocks;
	QVector<float> offlineDryMixBlock;
	std::array<QVector<float>, kReturnBusCount> offlineReturnBlocks;
    QVector<float> offlineMasterBlock;
    QVector<float> offlineSpatialBlock;
    std::vector<int> offlineMidiSourceTracks;
    for (int trackIndex = 0; trackIndex < static_cast<int>(m_tracks.size()); ++trackIndex) {
        if (m_tracks[trackIndex]) {
            trackBlocks.try_emplace(trackIndex);
        }
        if (!isolatedClipRender && m_tracks[trackIndex] &&
            m_tracks[trackIndex]->GetType() == TrackType::Midi) {
            offlineMidiSourceTracks.push_back(trackIndex);
        }
    }
    std::array<PanGains, kReturnBusCount> offlineReturnGains{};
    std::vector<PanGains> offlineTrackGains(m_tracks.size());
	std::vector<VeraCoreSpatialGains> offlineObjectGains(m_tracks.size());
	std::vector<VeraCoreSpatialGains> offlineLeftBedGains(m_tracks.size());
	std::vector<VeraCoreSpatialGains> offlineRightBedGains(m_tracks.size());
	std::vector<bool> offlineTrackAudible(m_tracks.size(), false);
	const bool anyOfflineTrackSolo = std::any_of(m_tracks.begin(), m_tracks.end(),
		[](const std::shared_ptr<Track>& track) { return track && track->IsSoloed(); });
    for (int trackIndex = 0; trackIndex < static_cast<int>(m_tracks.size()); ++trackIndex) {
        if (!m_tracks[trackIndex]) continue;
		offlineTrackAudible[trackIndex] = !m_tracks[trackIndex]->IsMuted() &&
			(!anyOfflineTrackSolo || m_tracks[trackIndex]->IsSoloed());
        offlineTrackGains[trackIndex] = panGains(
            m_tracks[trackIndex]->GetVolume(), m_tracks[trackIndex]->GetPan(), false);
		const VeraCoreSpatialPannerState spatial = m_tracks[trackIndex]->GetSpatialPanner();
		if (channels > 2 && spatial.mode == VeraCoreSpatialRenderMode::Object) {
			offlineObjectGains[trackIndex] = VeraCoreCalculateSpatialGains(
				offlineSpeakerLayout, spatial);
		} else if (channels > 2 && spatial.mode == VeraCoreSpatialRenderMode::Bed) {
			VeraCoreSpatialPannerState leftState = spatial;
			VeraCoreSpatialPannerState rightState = spatial;
			leftState.azimuthDegrees = std::clamp(spatial.azimuthDegrees - 30.0f, -180.0f, 180.0f);
			rightState.azimuthDegrees = std::clamp(spatial.azimuthDegrees + 30.0f, -180.0f, 180.0f);
			leftState.lfeSend *= 0.5f;
			rightState.lfeSend *= 0.5f;
			offlineLeftBedGains[trackIndex] = VeraCoreCalculateSpatialGains(
				offlineSpeakerLayout, leftState);
			offlineRightBedGains[trackIndex] = VeraCoreCalculateSpatialGains(
				offlineSpeakerLayout, rightState);
		}
    }
    const auto& offlineReturnBuses = m_mixerParameters->returnBuses();
    const bool anyReturnSolo = std::any_of(offlineReturnBuses.begin(), offlineReturnBuses.end(),
        [](const ReturnBusState& bus) { return bus.soloed; });
    for (int bus = 0; bus < kReturnBusCount; ++bus) {
        const bool audible = !offlineReturnBuses[bus].muted &&
            (!anyReturnSolo || offlineReturnBuses[bus].soloed);
        const double gain = audible ? offlineReturnBuses[bus].volume : 0.0;
        offlineReturnGains[bus] = panGains(gain, offlineReturnBuses[bus].pan, false);
    }
    const MasterBusState& offlineMasterBus = m_mixerParameters->masterBus();
    const PanGains offlineMasterGains = panGains(
        offlineMasterBus.muted ? 0.0 : offlineMasterBus.volume,
        offlineMasterBus.pan, false);
    std::vector<VeraCoreMidiEvent> pluginBlockEvents;
    pluginBlockEvents.reserve(2048);
    size_t nextPluginEvent = 0;
    quint32 pluginBlockStart = 0;
    quint32 pluginBlockFrames = 0;
    for (quint32 frame = 0; frame < frames; ++frame) {
        if (pluginBlockFrames == 0 || frame >= pluginBlockStart + pluginBlockFrames) {
            if (progress && !progress(frame, frames)) return failWavWrite();
            pluginBlockStart = frame;
            pluginBlockFrames = qMin(kOfflinePluginBlockFrames, frames - pluginBlockStart);
            const double provisionalBlockBeat = tempoClock.beatAtSeconds(
                static_cast<double>(pluginBlockStart) / safeSampleRate);
            const auto nextTempoBoundary = std::upper_bound(
                tempoClock.points.begin(), tempoClock.points.end(), provisionalBlockBeat,
                [](double beat, const TempoClockPoint& point) { return beat < point.beat; });
            if (nextTempoBoundary != tempoClock.points.end()) {
                const double boundaryFrameDouble = std::ceil(nextTempoBoundary->seconds * safeSampleRate);
                if (std::isfinite(boundaryFrameDouble) &&
                    boundaryFrameDouble > static_cast<double>(pluginBlockStart) &&
                    boundaryFrameDouble < static_cast<double>(pluginBlockStart + pluginBlockFrames)) {
                    pluginBlockFrames = static_cast<quint32>(boundaryFrameDouble) - pluginBlockStart;
                }
            }
            const auto nextSignatureBoundary = std::upper_bound(
                renderSignatureMap.begin(), renderSignatureMap.end(), provisionalBlockBeat,
                [](double beat, const MidiTimeSignaturePoint& point) { return beat < point.beat; });
            if (nextSignatureBoundary != renderSignatureMap.end()) {
                const double boundaryFrameDouble = std::ceil(
                    tempoClock.secondsAtBeat(nextSignatureBoundary->beat) * safeSampleRate);
                if (std::isfinite(boundaryFrameDouble) &&
                    boundaryFrameDouble > static_cast<double>(pluginBlockStart) &&
                    boundaryFrameDouble < static_cast<double>(pluginBlockStart + pluginBlockFrames)) {
                    pluginBlockFrames = static_cast<quint32>(boundaryFrameDouble) - pluginBlockStart;
                }
            }
            for (auto& trackBlock : trackBlocks)
                trackBlock.second.fill(0.0f, static_cast<int>(pluginBlockFrames * 2u));
            pluginBlockEvents.clear();
            const quint32 blockEnd = pluginBlockStart + pluginBlockFrames;
            while (nextPluginEvent < pluginEvents.size() &&
                   pluginEvents[nextPluginEvent].sample < static_cast<int>(blockEnd)) {
                if (pluginEvents[nextPluginEvent].sample >= static_cast<int>(pluginBlockStart)) {
                    VeraCoreMidiEvent event = pluginEvents[nextPluginEvent].event;
                    event.sampleOffset = pluginEvents[nextPluginEvent].sample - static_cast<int>(pluginBlockStart);
                    pluginBlockEvents.push_back(event);
                }
                ++nextPluginEvent;
            }
            const double blockBeat = tempoClock.beatAtSeconds(
                static_cast<double>(pluginBlockStart) / safeSampleRate);
            const double blockTempo = tempoClock.bpmAtBeat(blockBeat);
            const MidiTimeSignaturePoint blockSignature = timeSignatureAtBeat(
                renderSignatureMap, blockBeat,
                initialSignature.numerator, initialSignature.denominator);
            const PluginMetricalPosition blockMetrical = pluginMetricalPositionAtBeat(
                renderSignatureMap, blockBeat,
                initialSignature.numerator, initialSignature.denominator);
            offlinePluginHost.setTransport(
                true, false, blockTempo, blockBeat,
                blockSignature.numerator, blockSignature.denominator,
                static_cast<double>(pluginBlockStart) / safeSampleRate,
                blockMetrical.barStartBeat, blockMetrical.barNumber);
            OfflinePluginOutputSinkContext outputSinkContext{
                &trackBlocks,
                static_cast<int>(pluginBlockFrames), m_projectPanLawDb};
            for (int sourceTrack : offlineMidiSourceTracks) {
                auto trackBlock = trackBlocks.find(sourceTrack);
                if (trackBlock == trackBlocks.end()) continue;
                offlinePluginHost.renderTrack(sourceTrack, trackBlock->second.data(),
                    pluginBlockFrames, pluginBlockEvents, safeSampleRate, blockBeat,
                    static_cast<double>(pluginBlockStart) / safeSampleRate,
                    blockTempo, blockMetrical.barStartBeat, blockMetrical.barNumber,
                    blockMetrical.numerator, blockMetrical.denominator, false, 0,
                    &accumulateOfflinePluginOutput, &outputSinkContext);
            }
            const quint32 blockEndFrame = pluginBlockStart + pluginBlockFrames;
            for (const DecodedRenderClip& clip : decodedClips) {
                auto trackBlock = trackBlocks.find(clip.trackIndex);
                if (trackBlock == trackBlocks.end() || clip.startFrame >= blockEndFrame ||
                    clip.startFrame + clip.lengthFrames <= pluginBlockStart) continue;
                const quint32 firstFrame = qMax(pluginBlockStart, clip.startFrame);
                const quint32 lastFrame = qMin(blockEndFrame, clip.startFrame + clip.lengthFrames);
                const std::vector<float>& samples = *clip.samples;
                const int sourceFrames = static_cast<int>(samples.size() / 2u);
                if (sourceFrames <= 0) continue;
                const PanGains clipGains = panGains(
                    1.0, clip.pan, clip.sourceChannels == 1, m_projectPanLawDb);
                const double clipLevel = static_cast<double>(clip.gain) *
                    static_cast<double>(clip.normalizationGain) * (clip.reversePolarity ? -1.0 : 1.0);
                for (quint32 audioFrame = firstFrame; audioFrame < lastFrame; ++audioFrame) {
                    const double projectBeat = tempoClock.beatAtSeconds(
                        static_cast<double>(audioFrame) / safeSampleRate);
                    double progress = clip.timeMappedSync && clip.projectDurationSeconds > 0.0
                        ? qBound(0.0,
                            (static_cast<double>(audioFrame) / safeSampleRate - clip.projectStartSeconds) /
                                clip.projectDurationSeconds,
                            1.0)
                        : (clip.lengthBeats > 0.0
                            ? qBound(0.0, (projectBeat - clip.startBeat) / clip.lengthBeats, 1.0)
                            : 0.0);
                    const double localBeat = qBound(0.0, projectBeat - clip.startBeat, clip.lengthBeats);
                    const double sourceLocalBeat = clip.loopEnabled
                        ? std::fmod(localBeat, clip.loopLengthBeats) : localBeat;
                    if (clip.userWarpPoints && clip.userWarpPoints->size() >= 2u) {
                        const auto& points = *clip.userWarpPoints;
                        auto right = std::upper_bound(points.begin(), points.end(), sourceLocalBeat,
                            [](double beat, const VeraCoreAudioWarpPoint& point) {
                                return beat < point.localBeat;
                            });
                        if (right == points.begin()) progress = right->sourceProgress;
                        else if (right == points.end()) progress = points.back().sourceProgress;
                        else {
                            const VeraCoreAudioWarpPoint& left = *(right - 1);
                            const double span = right->localBeat - left.localBeat;
                            const double amount = span > 0.0
                                ? qBound(0.0, (sourceLocalBeat - left.localBeat) / span, 1.0) : 0.0;
                            progress = left.sourceProgress +
                                (right->sourceProgress - left.sourceProgress) * amount;
                        }
                    }
                    const double remainingBeat = qMax(0.0, clip.lengthBeats - localBeat);
                    double fadeEnvelope = 1.0;
                    if (clip.fadeInBeats > 0.0 && localBeat < clip.fadeInBeats)
                        fadeEnvelope *= shapedAudioFade(localBeat / clip.fadeInBeats,
                                                        clip.fadeInCurve);
                    if (clip.fadeOutBeats > 0.0 && remainingBeat < clip.fadeOutBeats)
                        fadeEnvelope *= shapedAudioFade(remainingBeat / clip.fadeOutBeats,
                                                        clip.fadeOutCurve);
                    double gainEnvelope = 1.0;
                    if (!clip.gainEnvelope.empty()) {
                        auto interpolateGain = [](double position, double leftBeat, float leftGain,
                                                  double rightBeat, float rightGain) {
                            const double span = rightBeat - leftBeat;
                            const double amount = span > 0.0
                                ? qBound(0.0, (position - leftBeat) / span, 1.0) : 0.0;
                            return static_cast<double>(leftGain) +
                                (static_cast<double>(rightGain) - leftGain) * amount;
                        };
                        const auto right = std::upper_bound(clip.gainEnvelope.begin(),
                            clip.gainEnvelope.end(), localBeat,
                            [](double beat, const AudioGainEnvelopePoint& point) {
                                return beat < point.beat;
                            });
                        const AudioGainEnvelopePoint& first = clip.gainEnvelope.front();
                        if (right == clip.gainEnvelope.begin()) {
                            gainEnvelope = first.beat <= 0.0 ? first.gain :
                                interpolateGain(localBeat, 0.0, 1.0f, first.beat, first.gain);
                        } else if (right != clip.gainEnvelope.end()) {
                            const AudioGainEnvelopePoint& left = *(right - 1);
                            gainEnvelope = interpolateGain(localBeat, left.beat, left.gain,
                                                           right->beat, right->gain);
                        } else {
                            const AudioGainEnvelopePoint& left = clip.gainEnvelope.back();
                            gainEnvelope = left.beat >= clip.lengthBeats ? left.gain :
                                interpolateGain(localBeat, left.beat, left.gain,
                                                clip.lengthBeats, 1.0f);
                        }
                    }
                    double forwardProgress = progress;
                    if (!clip.userWarpPoints) {
                        if (clip.loopEnabled && clip.sourceProgressSpan > 0.0) {
                            double phaseProgress = std::fmod(
                                clip.sourceStartProgress - clip.loopStartProgress +
                                    sourceLocalBeat * clip.sourceProgressSpan /
                                        clip.loopLengthBeats,
                                clip.sourceProgressSpan);
                            if (phaseProgress < 0.0) phaseProgress += clip.sourceProgressSpan;
                            forwardProgress = clip.loopStartProgress + phaseProgress;
                        } else {
                            forwardProgress = clip.sourceStartProgress +
                                progress * clip.sourceProgressSpan;
                        }
                    }
                    const double sourceProgress = clip.reverse ? 1.0 - forwardProgress : forwardProgress;
                    const double sourceFrame = sourceFrames > 1
                        ? sourceProgress * static_cast<double>(sourceFrames - 1)
                        : 0.0;
                    if (!std::isfinite(sourceFrame) || sourceFrame < 0.0) continue;
                    const int sourceIndex = static_cast<int>(std::floor(sourceFrame));
                    if (sourceIndex < 0 || sourceIndex >= sourceFrames) continue;
                    const int nextIndex = qMin(sourceIndex + 1, sourceFrames - 1);
                    const float fraction = static_cast<float>(sourceFrame - std::floor(sourceFrame));
                    const size_t sourceBase = static_cast<size_t>(sourceIndex) * 2u;
                    const size_t nextBase = static_cast<size_t>(nextIndex) * 2u;
                    if (sourceBase + 1u >= samples.size() || nextBase + 1u >= samples.size()) continue;
                    const float left = static_cast<float>((samples[sourceBase] +
                        (samples[nextBase] - samples[sourceBase]) * fraction) *
                        clipGains.left * clipLevel * fadeEnvelope * gainEnvelope);
                    const float right = static_cast<float>((samples[sourceBase + 1u] +
                        (samples[nextBase + 1u] - samples[sourceBase + 1u]) * fraction) *
                        clipGains.right * clipLevel * fadeEnvelope * gainEnvelope);
                    const size_t target = static_cast<size_t>(audioFrame - pluginBlockStart) * 2u;
                    trackBlock->second[static_cast<int>(target)] += left;
                    trackBlock->second[static_cast<int>(target + 1u)] += right;
                }
            }
        }
        while (nextEventIndex < (int)sortedEvents.size() && nextEventTick <= (int)frame) {
            const RtMidiEvent& event = sortedEvents[nextEventIndex];
            if (event.externalPlugin) {
                ++nextEventIndex;
                nextEventTick = nextEventIndex < static_cast<int>(sortedEvents.size())
                    ? toSample(sortedEvents[nextEventIndex])
                    : static_cast<int>(frames);
                continue;
            }
            const uint8_t statusHi = event.status & 0xF0;
            const int channel = event.status & 0x0F;
            const double glideDuration = std::isfinite(event.glideDurationBeats) &&
                    event.glideDurationBeats > 0.0
                ? event.glideDurationBeats : 0.0;
            const double glideEndBeat = event.beat + glideDuration;
            const double glideSeconds = tempoClock.secondsAtBeat(glideEndBeat) -
                tempoClock.secondsAtBeat(event.beat);
            const int eventGlideSamples = qMax(1, static_cast<int>(std::ceil(
                qBound(0.0, glideSeconds * safeSampleRate,
                       static_cast<double>(std::numeric_limits<int>::max())))));
            const bool isNoteOn = (statusHi == 0x90 && event.data2 > 0 && !event.isSlide);
            const bool isNoteOff = !event.isSlide &&
                ((statusHi == 0x80) || (statusHi == 0x90 && event.data2 == 0));
            if (event.isSlide && isPluginMidiPitch(event.data1)) {
                const int glideSamples = eventGlideSamples;
                for (ActiveNote& note : activeNotes) {
                    if (note.channel != channel || note.layerId != event.layerId) continue;
                    note.glideTargetPitch = event.data1;
                    note.glideSamplesRemaining = glideSamples;
                    note.glideStepPerSample = (note.glideTargetPitch - note.currentPitch) /
                        static_cast<double>(glideSamples);
                }
            }
            const bool isPitchBend = statusHi == 0xE0 && event.data1 <= 127 && event.data2 <= 127;
            const bool isChannelPressure = statusHi == 0xD0 && event.data1 <= 127;
            const bool isPolyPressure = statusHi == 0xA0 && event.data1 <= 127 && event.data2 <= 127;
            auto currentPitchBend = [&]() {
                const auto bendIt = std::find_if(pitchBends.begin(), pitchBends.end(),
                    [&](const OfflinePitchBendState& state) {
                        return state.channel == channel && state.layerId == event.layerId;
                    });
                return bendIt == pitchBends.end() ? 0.0 : bendIt->semitones;
            };
            auto currentPressureGain = [&]() {
                const auto pressureIt = std::find_if(pressureStates.begin(), pressureStates.end(),
                    [&](const OfflinePressureState& state) {
                        return state.channel == channel && state.layerId == event.layerId;
                    });
                return pressureIt == pressureStates.end() ? 1.0 : pressureIt->gain;
            };
            if (isPitchBend) {
                const double semitones = pitchBendToSemitones(event.data1, event.data2);
                auto bendIt = std::find_if(pitchBends.begin(), pitchBends.end(),
                    [&](const OfflinePitchBendState& state) {
                        return state.channel == channel && state.layerId == event.layerId;
                    });
                if (bendIt == pitchBends.end()) {
                    pitchBends.push_back({channel, event.layerId, semitones});
                } else {
                    bendIt->semitones = semitones;
                }
                for (ActiveNote& note : activeNotes) {
                    if (note.channel == channel && note.layerId == event.layerId) {
                        note.pitchBendSemitones = semitones;
                    }
                }
            } else if (isChannelPressure) {
                const double pressureGain = pressureToGain(event.data1);
                auto pressureIt = std::find_if(pressureStates.begin(), pressureStates.end(),
                    [&](const OfflinePressureState& state) {
                        return state.channel == channel && state.layerId == event.layerId;
                    });
                if (pressureIt == pressureStates.end()) {
                    pressureStates.push_back({channel, event.layerId, pressureGain});
                } else {
                    pressureIt->gain = pressureGain;
                }
                for (ActiveNote& note : activeNotes) {
                    if (note.channel == channel && note.layerId == event.layerId) {
                        note.pressureGain = pressureGain;
                    }
                }
            } else if (isPolyPressure) {
                const double pressureGain = pressureToGain(event.data2);
                for (ActiveNote& note : activeNotes) {
                    if (note.pitch == event.data1 &&
                        note.channel == channel &&
                        note.layerId == event.layerId) {
                        note.pressureGain = pressureGain;
                    }
                }
            }
            if (isNoteOn && isPluginMidiPitch(event.data1)) {
                const double amplitude = static_cast<double>(event.data2) / 127.0;
                const double layerGain = finiteClamp(static_cast<double>(event.gain), 0.0, 2.0, 1.0);
                const double pan = finiteClamp(static_cast<double>(event.pan), -1.0, 1.0, 0.0);
                const PanGains notePanGains = panGains(
                    1.0, pan, true, m_projectPanLawDb);
                const double bend = currentPitchBend();
                const double pressureGain = currentPressureGain();
                double startPitch = static_cast<double>(event.data1);
                if (event.isPortamento) {
                    const auto source = std::find_if(activeNotes.rbegin(), activeNotes.rend(),
                        [&](const ActiveNote& note) {
                            return note.channel == channel && note.layerId == event.layerId;
                        });
                    if (source != activeNotes.rend()) startPitch = source->currentPitch;
                }
                auto configurePitch = [&](ActiveNote& note) {
                    note.currentPitch = startPitch;
                    note.glideTargetPitch = static_cast<double>(event.data1);
                    note.glideSamplesRemaining = event.isPortamento
                        ? eventGlideSamples
                        : 0;
                    note.glideStepPerSample = note.glideSamplesRemaining > 0
                        ? (note.glideTargetPitch - note.currentPitch) /
                            static_cast<double>(note.glideSamplesRemaining)
                        : 0.0;
                    if (note.glideSamplesRemaining == 0) note.currentPitch = note.glideTargetPitch;
                };
                auto existing = std::find_if(activeNotes.begin(), activeNotes.end(),
                    [&](const ActiveNote& note) {
                        return note.pitch == event.data1 &&
                               note.channel == channel &&
                               note.layerId == event.layerId;
                    });
                if (existing != activeNotes.end()) {
                    existing->phase = 0.0;
                    existing->amp = amplitude * 0.12 * layerGain;
                    existing->pressureGain = pressureGain;
                    existing->pan = pan;
                    existing->panLeft = notePanGains.left;
                    existing->panRight = notePanGains.right;
                    existing->pitchBendSemitones = bend;
                    configurePitch(*existing);
                    existing->noteOnCount = existing->noteOnCount >= std::numeric_limits<int>::max()
                        ? std::numeric_limits<int>::max()
                        : qMax(1, existing->noteOnCount) + 1;
                } else {
                    if (activeNotes.size() >= kMaxOfflineSynthVoices) {
                        activeNotes.erase(activeNotes.begin());
                    }
                    ActiveNote note;
                    note.pitch = event.data1;
                    note.channel = channel;
                    note.layerId = event.layerId;
                    note.amp = amplitude * 0.12 * layerGain;
                    note.pressureGain = pressureGain;
                    note.pan = pan;
                    note.panLeft = notePanGains.left;
                    note.panRight = notePanGains.right;
                    note.pitchBendSemitones = bend;
                    note.noteOnCount = 1;
                    configurePitch(note);
                    activeNotes.push_back(note);
                }
            } else if (isNoteOff && isPluginMidiPitch(event.data1)) {
                for (int i = (int)activeNotes.size() - 1; i >= 0; --i) {
                    if (activeNotes[i].pitch == event.data1 &&
                        activeNotes[i].channel == channel &&
                        activeNotes[i].layerId == event.layerId) {
                        if (activeNotes[i].noteOnCount > 1) {
                            --activeNotes[i].noteOnCount;
                        } else {
                            activeNotes.erase(activeNotes.begin() + i);
                        }
                        break;
                    }
                }
            }
            ++nextEventIndex;
            if (nextEventIndex < (int)sortedEvents.size()) {
                nextEventTick = toSample(sortedEvents[nextEventIndex]);
            }
        }

        const size_t pluginSampleIndex = static_cast<size_t>(frame - pluginBlockStart) * 2u;
        for (ActiveNote& note : activeNotes) {
            const double sample = synthLayerSample(note.phase, note.layerId) * note.amp * note.pressureGain;
            const int trackIndex = note.layerId >= 0 ? note.layerId / kLayerIdStride : -1;
            auto trackBlock = trackBlocks.find(trackIndex);
            if (trackBlock != trackBlocks.end() &&
                pluginSampleIndex + 1u < static_cast<size_t>(trackBlock->second.size())) {
                trackBlock->second[static_cast<int>(pluginSampleIndex)] +=
                    static_cast<float>(sample * note.panLeft);
                trackBlock->second[static_cast<int>(pluginSampleIndex + 1u)] +=
                    static_cast<float>(sample * note.panRight);
            }
            note.phase += twoPi * noteToFrequency(note.currentPitch + note.pitchBendSemitones) /
                static_cast<double>(safeSampleRate);
            if (note.glideSamplesRemaining > 0) {
                note.currentPitch += note.glideStepPerSample;
                if (--note.glideSamplesRemaining == 0) {
                    note.currentPitch = note.glideTargetPitch;
                    note.glideStepPerSample = 0.0;
                }
            }
            if (note.phase > twoPi) {
                note.phase -= twoPi;
            }
        }

        if (frame + 1u == pluginBlockStart + pluginBlockFrames) {
            if (isolatedClipRender) {
                for (quint32 localFrame = 0; localFrame < pluginBlockFrames; ++localFrame) {
                    const int sampleIndex = static_cast<int>(localFrame * 2u);
                    double leftSample = 0.0;
                    double rightSample = 0.0;
                    for (const auto& trackBlock : trackBlocks) {
                        leftSample += trackBlock.second[sampleIndex];
                        rightSample += trackBlock.second[sampleIndex + 1];
                    }
                    const std::array<double, 2> rawFrame{leftSample, rightSample};
                    const auto meteredFrame = meterOutputFrame(rawFrame.data(), 2);
                    out << meteredFrame[0] << meteredFrame[1];
                    if (!outputOk()) return failWavWrite();
                }
                continue;
            }
            const double blockBeat = tempoClock.beatAtSeconds(
                static_cast<double>(pluginBlockStart) / safeSampleRate);
            const double blockTempo = tempoClock.bpmAtBeat(blockBeat);
            const PluginMetricalPosition blockMetrical = pluginMetricalPositionAtBeat(
                renderSignatureMap, blockBeat,
                initialSignature.numerator, initialSignature.denominator);
            for (auto& trackBlock : trackBlocks) {
                offlinePluginHost.renderAudioFx(trackBlock.first, trackBlock.second.data(),
                    pluginBlockFrames, safeSampleRate, blockBeat,
                    static_cast<double>(pluginBlockStart) / safeSampleRate,
                    blockTempo, blockMetrical.barStartBeat, blockMetrical.barNumber,
                    blockMetrical.numerator, blockMetrical.denominator, false);
            }
            const int mixSampleCount = static_cast<int>(pluginBlockFrames * 2u);
			offlineDryMixBlock.fill(0.0f, mixSampleCount);
			for (QVector<float>& returnBlock : offlineReturnBlocks)
				returnBlock.fill(0.0f, mixSampleCount);
			if (channels > 2)
				offlineSpatialBlock.fill(0.0f,
					static_cast<int>(pluginBlockFrames) * static_cast<int>(channels));
			for (const auto& trackBlock : trackBlocks) {
				const int trackIndex = trackBlock.first;
				const QVector<float>& block = trackBlock.second;
				if (trackIndex < 0 || trackIndex >= static_cast<int>(offlineTrackGains.size()) ||
					!m_tracks[trackIndex] || !offlineTrackAudible[trackIndex]) continue;
				std::array<double, kReturnBusCount> sendLevels{};
				for (int bus = 0; bus < kReturnBusCount; ++bus)
					sendLevels[bus] = m_tracks[trackIndex]->GetSendLevel(bus);
				for (quint32 localFrame = 0; localFrame < pluginBlockFrames; ++localFrame) {
					const int sampleIndex = static_cast<int>(localFrame * 2u);
					const auto separated = applyStereoSeparation(
						block[sampleIndex], block[sampleIndex + 1],
						m_tracks[trackIndex]->GetStereoSeparation());
					const double sourceLeft = separated[0];
					const double sourceRight = separated[1];
					const double left = sourceLeft * offlineTrackGains[trackIndex].left;
					const double right = sourceRight * offlineTrackGains[trackIndex].right;
					if (!anyReturnSolo) {
						if (channels == 2) {
							offlineDryMixBlock[sampleIndex] += static_cast<float>(left);
							offlineDryMixBlock[sampleIndex + 1] += static_cast<float>(right);
						} else {
							const int spatialIndex = static_cast<int>(localFrame) * channels;
							const VeraCoreSpatialRenderMode mode =
								m_tracks[trackIndex]->GetSpatialPanner().mode;
							if (mode == VeraCoreSpatialRenderMode::StereoPan) {
								offlineSpatialBlock[spatialIndex] += static_cast<float>(left);
								offlineSpatialBlock[spatialIndex + 1] += static_cast<float>(right);
							} else if (mode == VeraCoreSpatialRenderMode::Object) {
								const double mono = 0.5 * (sourceLeft + sourceRight) *
									m_tracks[trackIndex]->GetVolume();
								for (int channel = 0; channel < channels; ++channel)
									offlineSpatialBlock[spatialIndex + channel] += static_cast<float>(
										mono * offlineObjectGains[trackIndex].channels[
											static_cast<size_t>(channel)]);
							} else {
								const double level = m_tracks[trackIndex]->GetVolume();
								for (int channel = 0; channel < channels; ++channel)
									offlineSpatialBlock[spatialIndex + channel] += static_cast<float>(
										level * (sourceLeft * offlineLeftBedGains[trackIndex].channels[
											static_cast<size_t>(channel)] +
											sourceRight * offlineRightBedGains[trackIndex].channels[
											static_cast<size_t>(channel)]));
							}
						}
					}
					for (int bus = 0; bus < kReturnBusCount; ++bus) {
						const double sendLeft = offlineReturnBuses[bus].preFader ? sourceLeft : left;
						const double sendRight = offlineReturnBuses[bus].preFader ? sourceRight : right;
						offlineReturnBlocks[bus][sampleIndex] +=
							static_cast<float>(sendLeft * sendLevels[bus]);
						offlineReturnBlocks[bus][sampleIndex + 1] +=
							static_cast<float>(sendRight * sendLevels[bus]);
					}
				}
			}
			if (channels == 2)
				offlinePluginHost.renderAudioFx(
					VeraCoreMasterPdcPathId, offlineDryMixBlock.data(),
					pluginBlockFrames, safeSampleRate, blockBeat,
					static_cast<double>(pluginBlockStart) / safeSampleRate,
					blockTempo, blockMetrical.barStartBeat, blockMetrical.barNumber,
					blockMetrical.numerator, blockMetrical.denominator, false);
			for (int bus = 0; bus < kReturnBusCount; ++bus) {
				offlinePluginHost.renderAudioFx(
					VeraCoreReturnAudioFxTrackId(bus), offlineReturnBlocks[bus].data(),
					pluginBlockFrames, safeSampleRate, blockBeat,
					static_cast<double>(pluginBlockStart) / safeSampleRate,
					blockTempo, blockMetrical.barStartBeat, blockMetrical.barNumber,
					blockMetrical.numerator, blockMetrical.denominator, false);
			}
			if (channels == 2) offlineMasterBlock.fill(0.0f, mixSampleCount);
			for (quint32 localFrame = 0; localFrame < pluginBlockFrames; ++localFrame) {
				const int sampleIndex = static_cast<int>(localFrame * 2u);
				double left = channels == 2 ? offlineDryMixBlock[sampleIndex] : 0.0;
				double right = channels == 2 ? offlineDryMixBlock[sampleIndex + 1] : 0.0;
				for (int bus = 0; bus < kReturnBusCount; ++bus) {
					const auto separatedReturn = applyStereoSeparation(
						offlineReturnBlocks[bus][sampleIndex],
						offlineReturnBlocks[bus][sampleIndex + 1],
						offlineReturnBuses[bus].stereoSeparation);
					left += separatedReturn[0] * offlineReturnGains[bus].left;
					right += separatedReturn[1] * offlineReturnGains[bus].right;
				}
				if (channels == 2) {
					offlineMasterBlock[sampleIndex] = static_cast<float>(left);
					offlineMasterBlock[sampleIndex + 1] = static_cast<float>(right);
				} else {
					const int spatialIndex = static_cast<int>(localFrame) * channels;
					offlineSpatialBlock[spatialIndex] += static_cast<float>(left);
					offlineSpatialBlock[spatialIndex + 1] += static_cast<float>(right);
				}
			}
			if (channels == 2)
				offlinePluginHost.renderAudioFx(
					VeraCoreMasterAudioFxTrackId, offlineMasterBlock.data(),
					pluginBlockFrames, safeSampleRate, blockBeat,
					static_cast<double>(pluginBlockStart) / safeSampleRate,
					blockTempo, blockMetrical.barStartBeat, blockMetrical.barNumber,
					blockMetrical.numerator, blockMetrical.denominator, false);
			else
				offlinePluginHost.renderAudioFxMultichannel(
					VeraCoreMasterAudioFxTrackId, offlineSpatialBlock.data(),
					pluginBlockFrames, static_cast<int>(channels), offlineSpeakerLayout,
					safeSampleRate, blockBeat,
					static_cast<double>(pluginBlockStart) / safeSampleRate,
					blockTempo, blockMetrical.barStartBeat, blockMetrical.barNumber,
					blockMetrical.numerator, blockMetrical.denominator, false);
			for (quint32 localFrame = 0; localFrame < pluginBlockFrames; ++localFrame) {
                const quint32 processingFrame = pluginBlockStart + localFrame;
                if (processingFrame < latencyTrimFrames ||
                    processingFrame - latencyTrimFrames >= outputFrames) {
                    continue;
                }
                const size_t sampleIndex = static_cast<size_t>(localFrame) * 2u;
				if (channels > 2) {
					const size_t spatialIndex = static_cast<size_t>(localFrame) * channels;
					std::array<double, kVeraCoreMaxSpatialChannels> frameSamples{};
					for (int channel = 0; channel < channels; ++channel) {
						const double value = offlineSpatialBlock[
							static_cast<int>(spatialIndex + static_cast<size_t>(channel))];
						frameSamples[static_cast<size_t>(channel)] =
							std::isfinite(value) ? value : 0.0;
					}
					constexpr std::array<std::array<int, 2>, 5> pairs{{
						{{0, 1}}, {{4, 5}}, {{6, 7}}, {{8, 9}}, {{10, 11}}
					}};
					for (const auto& pair : pairs) {
						if (pair[1] >= channels) continue;
						const auto separated = applyStereoSeparation(
							frameSamples[static_cast<size_t>(pair[0])],
							frameSamples[static_cast<size_t>(pair[1])],
							offlineMasterBus.stereoSeparation);
						frameSamples[static_cast<size_t>(pair[0])] =
							separated[0] * offlineMasterGains.left;
						frameSamples[static_cast<size_t>(pair[1])] =
							separated[1] * offlineMasterGains.right;
					}
					const double masterCenterGain =
						0.5 * (offlineMasterGains.left + offlineMasterGains.right);
					frameSamples[2] *= masterCenterGain;
					frameSamples[3] *= offlineMasterBus.muted ? 0.0 : offlineMasterBus.volume;
					const auto meteredFrame = meterOutputFrame(frameSamples.data(), channels);
					for (int channel = 0; channel < channels; ++channel) {
						out << meteredFrame[static_cast<size_t>(channel)];
					}
					if (!outputOk()) return failWavWrite();
					continue;
				}
				const auto separatedMaster = applyStereoSeparation(
					offlineMasterBlock[static_cast<int>(sampleIndex)],
					offlineMasterBlock[static_cast<int>(sampleIndex + 1u)],
					offlineMasterBus.stereoSeparation);
				double leftSample = separatedMaster[0];
				double rightSample = separatedMaster[1];
                leftSample *= offlineMasterGains.left;
                rightSample *= offlineMasterGains.right;
                const std::array<double, 2> rawFrame{leftSample, rightSample};
                const auto meteredFrame = meterOutputFrame(rawFrame.data(), 2);
                out << meteredFrame[0] << meteredFrame[1];
                if (!outputOk()) return failWavWrite();
            }
        }
    }

    if (progress && !progress(frames, frames)) return failWavWrite();

    if (!file.flush()) {
        return failWavWrite();
    }
    if (file.error() != QFileDevice::NoError) {
        return failWavWrite();
    }
    file.close();
    if (file.error() != QFileDevice::NoError) {
        QFile::remove(path);
        return false;
    }
    for (int pair = 0; pair * 2 < channels; ++pair) {
        const int firstChannel = pair * 2;
        const VeraCoreAudioMeterState truePeakTail =
            offlineTruePeakMeters[static_cast<size_t>(pair)].Flush();
        channelTruePeaks[static_cast<size_t>(firstChannel)] = std::max(
            channelTruePeaks[static_cast<size_t>(firstChannel)],
            static_cast<double>(truePeakTail.peakLeft));
        if (firstChannel + 1 < channels) {
            channelTruePeaks[static_cast<size_t>(firstChannel + 1)] = std::max(
                channelTruePeaks[static_cast<size_t>(firstChannel + 1)],
                static_cast<double>(truePeakTail.peakRight));
        }
    }
    const double peakLeft = channelTruePeaks[0];
    const double peakRight = channels > 1 ? channelTruePeaks[1] : peakLeft;
    const double preClampPeakLeft = channelPreClampPeaks[0];
    const double preClampPeakRight = channels > 1 ? channelPreClampPeaks[1] : preClampPeakLeft;
    const double squareSumLeft = channelSquareSums[0];
    const double squareSumRight = channels > 1 ? channelSquareSums[1] : squareSumLeft;
    const double maximumTruePeak = *std::max_element(
        channelTruePeaks.begin(), channelTruePeaks.begin() + channels);
    const double maximumPreClampPeak = *std::max_element(
        channelPreClampPeaks.begin(), channelPreClampPeaks.begin() + channels);
    double maximumRms = 0.0;
    for (int channel = 0; channel < channels; ++channel) {
        maximumRms = std::max(maximumRms, outputFrames > 0
            ? std::sqrt(channelSquareSums[static_cast<size_t>(channel)] /
                        static_cast<double>(outputFrames))
            : 0.0);
    }
    if (stats) {
        stats->peakLeft = peakLeft;
        stats->peakRight = peakRight;
        stats->truePeakMaximum = maximumTruePeak;
        stats->preClampPeakLeft = preClampPeakLeft;
        stats->preClampPeakRight = preClampPeakRight;
        stats->preClampPeakMaximum = maximumPreClampPeak;
        stats->rmsLeft = outputFrames > 0
            ? std::sqrt(squareSumLeft / static_cast<double>(outputFrames)) : 0.0;
        stats->rmsRight = outputFrames > 0
            ? std::sqrt(squareSumRight / static_cast<double>(outputFrames)) : 0.0;
        stats->rmsMaximum = maximumRms;
        stats->integratedLufs = offlineLoudnessMeter.integratedLufs();
        stats->momentaryMaxLufs = offlineLoudnessMeter.momentaryMaximumLufs();
        stats->shortTermMaxLufs = offlineLoudnessMeter.shortTermMaximumLufs();
        stats->loudnessRangeLu = offlineLoudnessMeter.loudnessRangeLu();
        stats->sampleFrames = static_cast<qint64>(outputFrames);
        stats->clippedSamples = clippedSamples;
        stats->channelCount = channels;
    }
    return true;
}

bool VeraCoreQT::renderOfflineWav(const QString& path, const std::vector<VeraCoreBeatMidiEvent>& events, const std::vector<OfflineAudioClip>& audioClips, double lengthBeats, int sampleRate, OfflineRenderStats* stats, const std::function<bool(quint64, quint64)>& progress) const
{
    return renderOfflineWavWithSynth(path, events, audioClips, lengthBeats, sampleRate, stats, progress);
}

void VeraCoreQT::renderOffline(const QString& format)
{
    if (m_offlineRenderActive) {
        ui.statusBar->showMessage("An offline render is already active.", 4000);
        return;
    }
    if (m_isRecordingMidi || m_isRecordingAudio) {
        QMessageBox::information(this, "Offline Render",
            "Stop recording before starting an offline render.");
        return;
    }
    m_offlineRenderActive = true;
    bool resumeTransport = false;
    const auto renderActivityGuard = qScopeGuard([this, &resumeTransport]() {
        m_offlineRenderActive = false;
        if (resumeTransport && !m_isPlaying) onTransportPlay();
    });

    const AudioCodecFormat codecFormat = AudioCodecBackend::formatFromId(format);
    if (codecFormat == AudioCodecFormat::Unsupported) {
        QMessageBox::critical(this, "Offline Render", "Unsupported render format.");
        return;
    }
    if (codecFormat != AudioCodecFormat::Wav && !AudioCodecBackend::isNativeEncoderAvailable(codecFormat)) {
        QMessageBox::information(this,
            "Offline Render",
            AudioCodecBackend::nativeEncoderRequirement(codecFormat) + " Embedded libraries are used; no external process is launched.");
        return;
    }
    const int masterChannels = VeraCoreSpeakerChannelCount(m_projectSpeakerLayout);
    const int maximumCodecChannels = AudioCodecBackend::maximumEncoderChannels(codecFormat);
    if (masterChannels > maximumCodecChannels) {
        QMessageBox::information(this, "Spatial Master Export",
            QString("%1 supports at most %2 channels in the active embedded backend. "
                    "The current %3-channel master will not be collapsed. Choose WAV or a codec "
                    "with sufficient channel capacity.")
                .arg(AudioCodecBackend::displayName(codecFormat))
                .arg(maximumCodecChannels)
                .arg(masterChannels));
        return;
    }
    const QString filter = AudioCodecBackend::fileDialogFilter(codecFormat);
    const QString suffix = AudioCodecBackend::fileSuffix(codecFormat);
    const int configuredSampleRate = QSettings().value("SRate", 44100).toInt();
    const double streamSampleRate = audioGeneral ? audioGeneral->audioCore.GetStreamSampleRate() : 0.0;
    const int defaultSampleRate = safeRenderSampleRate(streamSampleRate, configuredSampleRate);
    AudioRenderSettings renderSettings;
    if (!editAudioRenderSettings(
            this, codecFormat, defaultSampleRate, masterChannels, renderSettings)) return;
    const int sampleRate = AudioCodecBackend::preferredEncoderSampleRate(
        codecFormat, renderSettings.sampleRate);
    QString encodingSummary;
    switch (codecFormat) {
    case AudioCodecFormat::Mp3:
        encodingSummary = QString("%1 kbps CBR").arg(renderSettings.encode.lossyBitrateKbps);
        break;
    case AudioCodecFormat::OggVorbis:
        encodingSummary = QString("quality %1 VBR").arg(renderSettings.encode.vorbisQuality);
        break;
    case AudioCodecFormat::Flac:
    case AudioCodecFormat::WavPack:
        encodingSummary = QString("%1-bit, compression %2%3")
            .arg(renderSettings.encode.losslessBitDepth)
            .arg(renderSettings.encode.compressionLevel)
            .arg(renderSettings.encode.dither ? ", TPDF dither" : "");
        break;
    case AudioCodecFormat::Wav:
        encodingSummary = renderSettings.encode.wavFloatingPoint
            ? QStringLiteral("32-bit float")
            : QString("%1-bit PCM%2").arg(renderSettings.encode.wavBitDepth)
                .arg(renderSettings.encode.dither ? ", TPDF dither" : "");
        break;
    default:
        break;
    }
    encodingSummary += QString(", %1 channels").arg(masterChannels);
    if (sampleRate != renderSettings.sampleRate) {
        ui.statusBar->showMessage(QString("%1 encoder adjusted sample rate from %2 Hz to %3 Hz.")
            .arg(AudioCodecBackend::displayName(codecFormat))
            .arg(renderSettings.sampleRate)
            .arg(sampleRate), 5000);
    }

    QSettings renderPathSettings;
    const QString initialDirectory = renderPathSettings.value(
        QStringLiteral("AudioRender/LastDirectory"), QStandardPaths::writableLocation(
            QStandardPaths::MusicLocation)).toString();
    const QString path = QFileDialog::getSaveFileName(this, "Render Audio", initialDirectory, filter);
    if (path.isEmpty()) {
        return;
    }

    const QString outPath = withRequiredSuffix(path, suffix);
    if (m_playlistView) {
        for (const PlaylistView::AudioClipInfo& clip : m_playlistView->getAudioClips()) {
            if (sameFilePath(outPath, clip.sourcePath)) {
                QMessageBox::critical(this, "Offline Render",
                    QString("Output '%1' is used by an audio clip in this project. Choose another file to preserve the source recording.")
                        .arg(QFileInfo(outPath).fileName()));
                return;
            }
        }
    }
    renderPathSettings.setValue(QStringLiteral("AudioRender/LastDirectory"),
                                QFileInfo(outPath).absolutePath());
    resumeTransport = m_isPlaying;
    if (resumeTransport) onTransportPause();
    QString recoveryError;
    if (!writeRecoverySnapshotNow(&recoveryError)) {
        QMessageBox::critical(this, "Offline Render",
            recoveryError.isEmpty()
                ? QStringLiteral("Could not create a pre-render recovery checkpoint.")
                : recoveryError);
        return;
    }
    const std::vector<VeraCoreBeatMidiEvent> events = buildTimelineEvents();
    std::vector<OfflineAudioClip> audioClips;
    bool anySolo = false;
    for (const auto& track : m_tracks) {
        if (track && track->IsSoloed()) {
            anySolo = true;
            break;
        }
    }
    for (const PlaylistView::AudioClipInfo& clip : m_playlistView->getAudioClips()) {
        if (!clipTakeIsActive(clip) || clip.muted || clip.trackIndex < 0 ||
            clip.trackIndex >= static_cast<int>(m_tracks.size())) {
            continue;
        }
        const Track* track = m_tracks[clip.trackIndex].get();
        if (!track || track->GetType() != TrackType::Audio || track->IsMuted() ||
            (anySolo && !track->IsSoloed()) || !isFiniteBeatRange(clip.startBeat, clip.lengthBeats)) {
            continue;
        }
        OfflineAudioClip renderClip;
        renderClip.trackIndex = clip.trackIndex;
        renderClip.clipId = clip.clipId;
        renderClip.contentId = clip.contentId;
        renderClip.name = clip.name;
        renderClip.sourcePath = clip.sourcePath;
        renderClip.startBeat = clip.startBeat;
        renderClip.lengthBeats = clip.lengthBeats;
        renderClip.sourceOffsetBeats = clip.sourceOffsetBeats;
        renderClip.loopEnabled = clip.loopEnabled;
        renderClip.loopStartBeats = clip.loopStartBeats;
        renderClip.loopLengthBeats = clip.loopLengthBeats;
        renderClip.warpLengthBeats = clip.warpLengthBeats;
        renderClip.sourceDurationSeconds = clip.sourceDurationSeconds;
        renderClip.warpMarkers = clip.warpMarkers;
        renderClip.sourceTempoBpm = clip.sourceTempoBpm;
        renderClip.pitchShiftSemitones = clip.pitchShiftSemitones;
        renderClip.syncMode = clip.syncMode;
        renderClip.targetBars = clip.targetBars;
        renderClip.beatsPerBar = clip.beatsPerBar;
        renderClip.gain = clip.gain;
        renderClip.pan = clip.pan;
        renderClip.normalize = clip.normalize;
        renderClip.reverse = clip.reverse;
        renderClip.reversePolarity = clip.reversePolarity;
        renderClip.fadeInBeats = clip.fadeInBeats;
        renderClip.fadeOutBeats = clip.fadeOutBeats;
        renderClip.gainEnvelope = clip.gainEnvelope;
        renderClip.fadeInCurve = clip.fadeInCurve;
        renderClip.fadeOutCurve = clip.fadeOutCurve;
        audioClips.push_back(renderClip);
    }
    for (const OfflineAudioClip& clip : audioClips) {
        if (clip.sourcePath.trimmed().isEmpty()) {
            QMessageBox::critical(this, "Offline Render", "An audio clip has no source file path.");
            return;
        }
        const AudioFileProbeResult probe = AudioFileDecoder::probeAudio(clip.sourcePath);
        if (!probe.success || probe.durationSeconds() <= 0.0) {
            const QString fileName = QFileInfo(clip.sourcePath).fileName();
            QMessageBox::critical(this,
                "Offline Render",
                QString("Cannot render audio clip '%1': %2")
                    .arg(fileName.isEmpty() ? clip.sourcePath : fileName,
                         probe.message.isEmpty() ? QString("could not read audio data.") : probe.message));
            return;
        }
    }
    const double lengthBeats = arrangementLengthBeats();
    const double fallbackTempo = m_transportWidget
        ? m_transportWidget->tempo() : kDefaultTempoBpm;
    const TempoClock renderClock(m_tempoMap, fallbackTempo);
    const double baseDurationSeconds = renderClock.secondsAtBeat(lengthBeats);
    const double pluginTailSeconds = m_pluginHost
        ? static_cast<double>(m_pluginHost->maxTailSamples()) / sampleRate : 0.0;
    QString storageError;
    if (!hasRenderStorageCapacity(outPath, sampleRate,
            baseDurationSeconds + qMax(2.0, pluginTailSeconds + 1.0), &storageError)) {
        QMessageBox::critical(this, "Offline Render", storageError);
        return;
    }
    OfflineRenderStats renderStats;
    auto meteredMessage = [&](const QString& prefix) {
        const double peak = renderStats.truePeakMaximum;
        const double rms = renderStats.rmsMaximum;
        const QString loudness = std::isfinite(renderStats.integratedLufs)
            ? QString::number(renderStats.integratedLufs, 'f', 1)
            : QStringLiteral("-inf");
        const QString momentary = std::isfinite(renderStats.momentaryMaxLufs)
            ? QString::number(renderStats.momentaryMaxLufs, 'f', 1) : QStringLiteral("n/a");
        const QString shortTerm = std::isfinite(renderStats.shortTermMaxLufs)
            ? QString::number(renderStats.shortTermMaxLufs, 'f', 1) : QStringLiteral("n/a");
        const QString loudnessRange = std::isfinite(renderStats.loudnessRangeLu)
            ? QString::number(renderStats.loudnessRangeLu, 'f', 1) : QStringLiteral("n/a");
        const QString clipText = renderStats.clippedSamples > 0
            ? QString(" Clipped samples: %1.").arg(renderStats.clippedSamples)
            : QString();
        return QString("%1 True peak %2 dBTP, %3 LUFS-I, %4 LUFS-M max, "
                       "%5 LUFS-S max, LRA %6 LU, RMS %7 dBFS.%8")
            .arg(prefix)
            .arg(linearToDb(peak), 0, 'f', 1)
            .arg(loudness)
            .arg(momentary)
            .arg(shortTerm)
            .arg(loudnessRange)
            .arg(linearToDb(rms), 0, 'f', 1)
            .arg(clipText);
    };

    QProgressDialog progressDialog("Preparing offline render...", "Cancel", 0, 1000, this);
    progressDialog.setWindowTitle("Offline Render");
    progressDialog.setWindowModality(Qt::WindowModal);
    progressDialog.setMinimumDuration(0);
    progressDialog.setAutoClose(false);
    progressDialog.setAutoReset(false);
    progressDialog.setValue(0);
    auto updateRenderProgress = [&](int stageStart, int stageEnd, const QString& label,
                                    qint64 completed, qint64 total) {
        if (progressDialog.wasCanceled()) return false;
        const double ratio = total > 0
            ? std::clamp(static_cast<double>(completed) / static_cast<double>(total), 0.0, 1.0)
            : 0.0;
        progressDialog.setLabelText(label);
        progressDialog.setValue(stageStart + static_cast<int>(
            std::llround(ratio * static_cast<double>(stageEnd - stageStart))));
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        return !progressDialog.wasCanceled();
    };
    if (!updateRenderProgress(0, 20, "Preparing sources and plugins...", 1, 1)) {
        ui.statusBar->showMessage("Render cancelled.", 5000);
        return;
    }

    QTemporaryFile tempWav;
    tempWav.setFileTemplate(QDir::tempPath() + "\\VeraCoreRenderXXXXXX.wav");
    tempWav.setAutoRemove(true);
    if (!tempWav.open()) {
        QMessageBox::critical(this, "Offline Render", "Could not create temporary WAV file.");
        return;
    }
    const QString wavPath = tempWav.fileName();
    tempWav.close();
    auto cleanupTempWav = [&]() {
        if (!wavPath.isEmpty() && QFile::exists(wavPath)) {
            QFile::remove(wavPath);
        }
    };
    auto createTempOutput = [&]() -> QString {
        const QFileInfo outInfo(outPath);
        QTemporaryFile tempOut(outInfo.absolutePath() + "/.VeraCoreOutputXXXXXX" + suffix);
        tempOut.setAutoRemove(false);
        if (!tempOut.open()) {
            return QString();
        }
        const QString tempPath = tempOut.fileName();
        tempOut.close();
        return tempPath;
    };
    auto installOutput = [&](const QString& tempPath, const QString& successMessage) {
        if (progressDialog.wasCanceled()) {
            QFile::remove(tempPath);
            ui.statusBar->showMessage("Render cancelled; existing output preserved.", 5000);
            return false;
        }
        const QFileInfo tempInfo(tempPath);
        if (tempPath.isEmpty() || !tempInfo.exists() || tempInfo.size() <= 0) {
            QFile::remove(tempPath);
            QMessageBox::critical(this, "Offline Render", "Render did not produce a valid output file.");
            return false;
        }

        QFile input(tempPath);
        QSaveFile output(outPath);
        output.setDirectWriteFallback(false);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
            output.cancelWriting();
            QMessageBox::critical(this, "Offline Render", "Could not prepare atomic output commit.");
            return false;
        }
        constexpr qint64 kCommitBlockBytes = 1024 * 1024;
        const qint64 totalBytes = input.size();
        qint64 copiedBytes = 0;
        while (copiedBytes < totalBytes) {
            if (!updateRenderProgress(980, 995, "Installing rendered output...",
                                      copiedBytes, totalBytes)) {
                output.cancelWriting();
                ui.statusBar->showMessage("Render cancelled; existing output preserved.", 5000);
                return false;
            }
            const QByteArray block = input.read(std::min(kCommitBlockBytes, totalBytes - copiedBytes));
            if (block.isEmpty() || output.write(block) != block.size()) {
                output.cancelWriting();
                QMessageBox::critical(this, "Offline Render", "Could not stage rendered output.");
                return false;
            }
            copiedBytes += block.size();
        }
        if (!updateRenderProgress(980, 995, "Installing rendered output...",
                                  totalBytes, totalBytes)) {
            output.cancelWriting();
            ui.statusBar->showMessage("Render cancelled; existing output preserved.", 5000);
            return false;
        }
        if (!output.commit()) {
            QMessageBox::critical(this, "Offline Render", "Could not atomically commit rendered output.");
            return false;
        }
        input.close();
        QFile::remove(tempPath);
        ui.statusBar->showMessage(successMessage, 5000);
        return true;
    };

    const auto renderProgress = [&](quint64 completed, quint64 total) {
        return updateRenderProgress(20, 750, "Rendering master audio...",
            static_cast<qint64>(completed), static_cast<qint64>(total));
    };
    if (!renderOfflineWavWithSynth(wavPath, events, audioClips, lengthBeats, sampleRate,
                                   &renderStats, renderProgress)) {
        cleanupTempWav();
        if (progressDialog.wasCanceled()) {
            ui.statusBar->showMessage("Render cancelled; existing output preserved.", 5000);
            return;
        }
        QMessageBox::critical(this, "Offline Render", "Could not render temporary WAV.");
        return;
    }

    int encodeStageStart = 750;
    if (renderSettings.normalizationMode != 0) {
        const double renderedPeak = renderStats.truePeakMaximum;
        if (std::isfinite(renderedPeak) && renderedPeak > 1.0e-9) {
            const double ceilingAmplitude = std::pow(10.0,
                qBound(-12.0, renderSettings.normalizePeakDb, 0.0) / 20.0);
            const double ceilingGain = ceilingAmplitude / renderedPeak;
            double loudnessGain = std::numeric_limits<double>::infinity();
            bool ceilingLimited = false;
            if (renderSettings.normalizationMode == 2) {
                if (!std::isfinite(renderStats.integratedLufs)) {
                    encodingSummary += ", loudness normalization skipped (silent master)";
                    loudnessGain = 1.0;
                } else {
                    loudnessGain = std::pow(10.0,
                        (qBound(-36.0, renderSettings.normalizeLufs, -5.0) -
                         renderStats.integratedLufs) / 20.0);
                }
            }
            const double normalizationGain = renderSettings.normalizationMode == 2
                ? qMin(loudnessGain, ceilingGain)
                : ceilingGain;
            ceilingLimited = renderSettings.normalizationMode == 2 &&
                std::isfinite(loudnessGain) && ceilingGain < loudnessGain;
            const auto normalizeProgress = [&](qint64 completed, qint64 total) {
                return updateRenderProgress(750, 810,
                    renderSettings.normalizationMode == 2
                        ? "Normalizing master loudness..."
                        : "Normalizing master true peak...",
                    completed, total);
            };
            QString normalizationError;
            if (!applyGainToFloat32StereoWav(
                    wavPath, normalizationGain, normalizeProgress, &normalizationError)) {
                cleanupTempWav();
                if (progressDialog.wasCanceled()) {
                    ui.statusBar->showMessage(
                        "Render cancelled; existing output preserved.", 5000);
                    return;
                }
                QMessageBox::critical(this, "Offline Render",
                    normalizationError.isEmpty()
                        ? QStringLiteral("Could not normalize the rendered master.")
                        : normalizationError);
                return;
            }
            auto scaleMetric = [normalizationGain](double value) {
                return std::isfinite(value) ? qMax(0.0, value * normalizationGain) : 0.0;
            };
            renderStats.peakLeft = scaleMetric(renderStats.peakLeft);
            renderStats.peakRight = scaleMetric(renderStats.peakRight);
            renderStats.truePeakMaximum = scaleMetric(renderStats.truePeakMaximum);
            renderStats.preClampPeakLeft = scaleMetric(renderStats.preClampPeakLeft);
            renderStats.preClampPeakRight = scaleMetric(renderStats.preClampPeakRight);
            renderStats.preClampPeakMaximum = scaleMetric(renderStats.preClampPeakMaximum);
            renderStats.rmsLeft = scaleMetric(renderStats.rmsLeft);
            renderStats.rmsRight = scaleMetric(renderStats.rmsRight);
            renderStats.rmsMaximum = scaleMetric(renderStats.rmsMaximum);
            if (std::isfinite(renderStats.integratedLufs))
                renderStats.integratedLufs += 20.0 * std::log10(normalizationGain);
            if (std::isfinite(renderStats.momentaryMaxLufs))
                renderStats.momentaryMaxLufs += 20.0 * std::log10(normalizationGain);
            if (std::isfinite(renderStats.shortTermMaxLufs))
                renderStats.shortTermMaxLufs += 20.0 * std::log10(normalizationGain);
            renderStats.clippedSamples = 0;
            if (renderSettings.normalizationMode == 2) {
                encodingSummary += QString(", normalized toward %1 LUFS-I, ceiling %2 dBTP%3")
                    .arg(renderSettings.normalizeLufs, 0, 'f', 1)
                    .arg(renderSettings.normalizePeakDb, 0, 'f', 1)
                    .arg(ceilingLimited ? QStringLiteral(" (ceiling-limited)") : QString());
                if (std::isfinite(renderStats.integratedLufs)) {
                    const double targetLufs = qBound(-36.0, renderSettings.normalizeLufs, -5.0);
                    const double toleranceLu = qBound(0.1, renderSettings.normalizeToleranceLu, 2.0);
                    const double loudnessDeltaLu = renderStats.integratedLufs - targetLufs;
                    const double postPeakDbtp = linearToDb(renderStats.truePeakMaximum);
                    const bool loudnessPass = std::fabs(loudnessDeltaLu) <= toleranceLu + 1.0e-9;
                    const bool truePeakPass = postPeakDbtp <=
                        qBound(-12.0, renderSettings.normalizePeakDb, 0.0) + 0.05;
                    const QString compliance = loudnessPass && truePeakPass
                        ? QStringLiteral("PASS")
                        : ceilingLimited && truePeakPass
                            ? QStringLiteral("TARGET MISS (ceiling-limited)")
                            : QStringLiteral("FAIL");
                    encodingSummary += QString(", compliance %1 (delta %2 LU, TP %3 dBTP, tolerance ±%4 LU)")
                        .arg(compliance)
                        .arg(loudnessDeltaLu, 0, 'f', 1)
                        .arg(postPeakDbtp, 0, 'f', 1)
                        .arg(toleranceLu, 0, 'f', 1);
                } else {
                    encodingSummary += ", compliance unavailable (no measurable loudness)";
                }
            } else {
                encodingSummary += QString(", normalized to %1 dBTP")
                    .arg(renderSettings.normalizePeakDb, 0, 'f', 1);
            }
            encodeStageStart = 810;
        } else {
            encodingSummary += ", normalization skipped (silent master)";
        }
    }

    const auto encodeProgress = [&](qint64 completed, qint64 total) {
        return updateRenderProgress(encodeStageStart, 940,
            QString("Encoding %1...").arg(AudioCodecBackend::displayName(codecFormat)),
            completed, total);
    };

    if (codecFormat == AudioCodecFormat::Wav) {
        const QString tempOutPath = createTempOutput();
        if (tempOutPath.isEmpty()) {
            cleanupTempWav();
            QMessageBox::critical(this, "Offline Render", "Could not create temporary output file.");
            return;
        }
        struct TempOutputGuard {
            QString path;
            ~TempOutputGuard() { if (!path.isEmpty()) QFile::remove(path); }
        } tempOutputGuard{tempOutPath};
        QFile::remove(tempOutPath);
        QString errorText;
        if (!encodeRenderedAudio(wavPath, tempOutPath, format, renderSettings.encode,
                                 &errorText, encodeProgress)) {
            cleanupTempWav();
            QFile::remove(tempOutPath);
            if (progressDialog.wasCanceled()) {
                ui.statusBar->showMessage("Render cancelled; existing output preserved.", 5000);
                return;
            }
            QMessageBox::critical(this, "Offline Render", errorText.isEmpty() ? "Could not write WAV file." : errorText);
            return;
        }
        if (!updateRenderProgress(940, 980, "Validating WAV output...", 1, 1)) {
            cleanupTempWav();
            ui.statusBar->showMessage("Render cancelled; existing output preserved.", 5000);
            return;
        }
        const QFileInfo outputInfo(tempOutPath);
        QString wavValidationError;
        const AudioFileProbeResult sourceProbe = AudioFileDecoder::probeWavPcm(wavPath);
        const AudioFileProbeResult outputProbe = AudioFileDecoder::probeWavPcm(tempOutPath);
        const bool wavComplete = outputInfo.exists() && outputInfo.size() > 44 &&
            sourceProbe.success && outputProbe.success &&
            outputProbe.sampleRate == sourceProbe.sampleRate &&
            outputProbe.frameCount == sourceProbe.frameCount &&
            outputProbe.channelCount == sourceProbe.channelCount &&
            outputProbe.channelMask == sourceProbe.channelMask &&
            AudioCodecBackend::hasValidContainerHeader(
                tempOutPath, AudioCodecFormat::Wav, &wavValidationError);
        if (!wavComplete) {
            cleanupTempWav();
            QFile::remove(tempOutPath);
            const QString probeError = !outputProbe.success ? outputProbe.message : QString();
            const QString validationMessage = !wavValidationError.isEmpty()
                ? wavValidationError
                : !probeError.isEmpty() ? probeError
                : QStringLiteral("WAV render sample rate, frame count, or channel layout does not match the master render.");
            QMessageBox::critical(this, "Offline Render", validationMessage);
            return;
        }
        cleanupTempWav();
        if (installOutput(tempOutPath, meteredMessage(QString("WAV render complete (%1 Hz, %2).")
            .arg(sampleRate).arg(encodingSummary)))) {
            progressDialog.setValue(1000);
        }
        return;
    }

    const QString tempOutPath = createTempOutput();
    if (tempOutPath.isEmpty()) {
        cleanupTempWav();
        QMessageBox::critical(this, "Offline Render", "Could not create temporary output file.");
        return;
    }
    struct TempOutputGuard {
        QString path;
        ~TempOutputGuard() { if (!path.isEmpty()) QFile::remove(path); }
    } tempOutputGuard{tempOutPath};
    QString errorText;
    if (!encodeRenderedAudio(wavPath, tempOutPath, format, renderSettings.encode,
                             &errorText, encodeProgress)) {
        cleanupTempWav();
        QFile::remove(tempOutPath);
        if (progressDialog.wasCanceled()) {
            ui.statusBar->showMessage("Render cancelled; existing output preserved.", 5000);
            return;
        }
        if (errorText.isEmpty()) {
            errorText = "Could not encode audio.";
        }
        QMessageBox::critical(this, "Offline Render", errorText);
        return;
    }

    if (!updateRenderProgress(940, 980,
            QString("Validating %1 output...").arg(AudioCodecBackend::displayName(codecFormat)), 1, 1)) {
        cleanupTempWav();
        ui.statusBar->showMessage("Render cancelled; existing output preserved.", 5000);
        return;
    }

    QString containerError;
    if (!AudioCodecBackend::hasValidContainerHeader(tempOutPath, codecFormat, &containerError)) {
        cleanupTempWav();
        QFile::remove(tempOutPath);
        QMessageBox::critical(this, "Offline Render", containerError);
        return;
    }

    const AudioFileProbeResult sourceProbe = AudioFileDecoder::probeAudio(wavPath);
    const bool decoderAvailable = AudioCodecBackend::isNativeDecoderAvailable(codecFormat);
    const AudioFileProbeResult encodedProbe = decoderAvailable
        ? AudioFileDecoder::probeAudio(tempOutPath)
        : AudioFileProbeResult{};
    const double sourceDuration = sourceProbe.durationSeconds();
    const double encodedDuration = encodedProbe.durationSeconds();
    qint64 allowedFrameDelta = 0;
    if (codecFormat == AudioCodecFormat::Mp3) {
        allowedFrameDelta = 2304; // Encoder delay plus one MPEG Layer III padding frame.
    } else if (codecFormat == AudioCodecFormat::OggVorbis) {
        allowedFrameDelta = 8192; // Maximum Vorbis overlap/block padding.
    }
    const qint64 frameDifference = static_cast<qint64>(encodedProbe.frameCount) -
        static_cast<qint64>(sourceProbe.frameCount);
    const qint64 encodedFrameDelta = decoderAvailable
        ? (frameDifference < 0 ? -frameDifference : frameDifference)
        : 0;
    const bool encodedLayoutMismatch = decoderAvailable &&
        (encodedProbe.channelCount != sourceProbe.channelCount ||
         (sourceProbe.channelCount > 2 &&
          (sourceProbe.channelMask == 0 || encodedProbe.channelMask == 0 ||
           encodedProbe.channelMask != sourceProbe.channelMask)));
    if (!sourceProbe.success || sourceDuration <= 0.0 ||
        (decoderAvailable && (!encodedProbe.success || encodedProbe.frameCount <= 0 ||
         encodedProbe.sampleRate != sampleRate ||
         encodedFrameDelta > allowedFrameDelta || encodedLayoutMismatch))) {
        cleanupTempWav();
        QFile::remove(tempOutPath);
        QString detail;
        if (!sourceProbe.success || sourceDuration <= 0.0) {
            detail = sourceProbe.message.trimmed();
        } else if (decoderAvailable && (!encodedProbe.success || encodedDuration <= 0.0)) {
            detail = encodedProbe.message.trimmed();
        } else if (decoderAvailable && encodedProbe.sampleRate != sampleRate) {
            detail = QString("expected %1 Hz, got %2 Hz").arg(sampleRate).arg(encodedProbe.sampleRate);
        } else if (encodedLayoutMismatch) {
            detail = QString("expected %1 channels/mask 0x%2, got %3 channels/mask 0x%4")
                .arg(sourceProbe.channelCount)
                .arg(QString::number(sourceProbe.channelMask, 16))
                .arg(encodedProbe.channelCount)
                .arg(QString::number(encodedProbe.channelMask, 16));
        } else {
            detail = QString("expected %1 frames, got %2 (allowed codec padding: %3 frames)")
                .arg(sourceProbe.frameCount)
                .arg(encodedProbe.frameCount)
                .arg(allowedFrameDelta);
        }
        QMessageBox::critical(this, "Offline Render",
            detail.isEmpty()
                ? "Encoded output failed validation."
                : QString("Encoded output failed validation: %1").arg(detail));
        return;
    }

    cleanupTempWav();
    if (installOutput(tempOutPath, meteredMessage(QString("Rendered %1 (%2 Hz, %3).")
        .arg(AudioCodecBackend::displayName(codecFormat))
        .arg(sampleRate)
        .arg(encodingSummary)))) {
        progressDialog.setValue(1000);
    }
}

void VeraCoreQT::onBrowserSearchChanged(const QString& text)
{
    const QString needle = text.trimmed();

    // An item stays visible when it matches or any descendant matches.
    std::function<bool(QTreeWidgetItem*)> filterItem = [&](QTreeWidgetItem* item) -> bool {
        bool anyChildVisible = false;
        for (int i = 0; i < item->childCount(); ++i)
            anyChildVisible = filterItem(item->child(i)) || anyChildVisible;

        const bool selfMatches = needle.isEmpty() ||
                                 item->text(0).contains(needle, Qt::CaseInsensitive);
        const bool visible = selfMatches || anyChildVisible;
        item->setHidden(!visible);
        if (!needle.isEmpty() && anyChildVisible)
            item->setExpanded(true);
        return visible;
    };

    for (int i = 0; i < ui.browserTree->topLevelItemCount(); ++i)
        filterItem(ui.browserTree->topLevelItem(i));
}

void VeraCoreQT::OnPreferences()
{
    Preferences* preferences = new Preferences(this);
    connect(preferences, &Preferences::audioConfigurationChangeStarted,
            this, &VeraCoreQT::quiesceExternalAudioForDeviceChange,
            Qt::DirectConnection);
    connect(preferences, &Preferences::audioConfigurationChangeFinished,
            this, &VeraCoreQT::resumeAudioAfterDeviceChange,
            Qt::DirectConnection);
    preferences->ConnectAudio(audioGeneral);
    preferences->InitPreferences();
    preferences->deleteLater();
}

void VeraCoreQT::updateBrowserTree()
{
    QList<QTreeWidgetItem*> items = ui.browserTree->findItems("Plugins", Qt::MatchExactly | Qt::MatchRecursive, 0);
    if (items.isEmpty()) return;
    QTreeWidgetItem* pluginsNode = items.first();

    QTreeWidgetItem* audioNode = nullptr;
    QTreeWidgetItem* genNode = nullptr;
    for (int i = 0; i < pluginsNode->childCount(); ++i) {
        const QString text = pluginsNode->child(i)->text(0);
        if (text == "Audio Plugins" || text == "Audio Effects") audioNode = pluginsNode->child(i);
        if (text == "Generator Plugins" || text == "Instruments") genNode = pluginsNode->child(i);
    }
    if (!audioNode) {
        audioNode = new QTreeWidgetItem(pluginsNode);
    }
    if (!genNode) {
        genNode = new QTreeWidgetItem(pluginsNode);
    }
    audioNode->setText(0, "Audio Effects");
    genNode->setText(0, "Instruments");

    auto typeLabel = [](const QString& type) {
        if (type.compare("VST2", Qt::CaseInsensitive) == 0 || type.compare("VST", Qt::CaseInsensitive) == 0)
            return QString("VST2");
        if (type.compare("VST3", Qt::CaseInsensitive) == 0)
            return QString("VST3");
        if (type.compare("CLAP", Qt::CaseInsensitive) == 0 || type.compare("Clap", Qt::CaseInsensitive) == 0)
            return QString("CLAP");
        return type;
    };

    auto ensureTypeNode = [&](QTreeWidgetItem* categoryNode, const QString& type) {
        const QString label = typeLabel(type);
        QTreeWidgetItem* typeNode = nullptr;
        for (int i = 0; i < categoryNode->childCount(); ++i) {
            if (typeLabel(categoryNode->child(i)->text(0)) == label) {
                typeNode = categoryNode->child(i);
                break;
            }
        }
        if (!typeNode) {
            typeNode = new QTreeWidgetItem(categoryNode, QStringList() << label);
        } else {
            typeNode->setText(0, label);
        }
        typeNode->setExpanded(true);
        return typeNode;
    };

    auto addPluginToNode = [&](QTreeWidgetItem* categoryNode, const PluginInfo& info) {
        if (!categoryNode) return;
        QTreeWidgetItem* typeNode = ensureTypeNode(categoryNode, info.type);
        QTreeWidgetItem* pluginLeaf = new QTreeWidgetItem(typeNode, QStringList() << info.name);
        pluginLeaf->setData(0, kPluginPathRole, info.path);
        pluginLeaf->setData(0, kPluginNameRole, info.name);
        pluginLeaf->setData(0, kPluginTypeRole, info.type);
        pluginLeaf->setData(0, kPluginCategoryRole, info.category);
        pluginLeaf->setData(0, kPluginUidRole, info.uid);
        if (!info.vendor.isEmpty()) {
            pluginLeaf->setToolTip(0, QString("%1\n%2 %3\n%4").arg(info.vendor, typeLabel(info.type), info.version, info.path));
        } else {
            pluginLeaf->setToolTip(0, info.path);
        }
    };

    auto clearLeaves = [](QTreeWidgetItem* categoryNode) {
        if (!categoryNode) return;
        for (int i = 0; i < categoryNode->childCount(); ++i) {
            QTreeWidgetItem* typeNode = categoryNode->child(i);
            qDeleteAll(typeNode->takeChildren());
        }
    };

    ensureTypeNode(audioNode, "VST3");
    ensureTypeNode(audioNode, "CLAP");
    ensureTypeNode(audioNode, "VST2");
    ensureTypeNode(genNode, "VST3");
    ensureTypeNode(genNode, "CLAP");
    ensureTypeNode(genNode, "VST2");

    clearLeaves(audioNode);
    clearLeaves(genNode);

    const auto& discovered = PluginManager::GetInstance().GetDiscoveredPlugins();
    for (const auto& info : discovered) {
        // Instruments go to Generator Plugins, everything else to Audio Plugins.
        QTreeWidgetItem* target = (info.category == "Instrument" && genNode) ? genNode : audioNode;
        addPluginToNode(target, info);
    }

    auto pruneEmptyTypeNodes = [](QTreeWidgetItem* categoryNode) {
        if (!categoryNode) return;
        for (int i = categoryNode->childCount() - 1; i >= 0; --i) {
            if (categoryNode->child(i)->childCount() == 0) {
                delete categoryNode->takeChild(i);
            }
        }
    };
    pruneEmptyTypeNodes(audioNode);
    pruneEmptyTypeNodes(genNode);

    if (audioNode->childCount() == 0) {
        QTreeWidgetItem* empty = new QTreeWidgetItem(audioNode, QStringList() << "No audio effects scanned");
        empty->setDisabled(true);
    }
    if (genNode->childCount() == 0) {
        QTreeWidgetItem* empty = new QTreeWidgetItem(genNode, QStringList() << "No instruments scanned");
        empty->setDisabled(true);
    }

    pluginsNode->setExpanded(true);
    audioNode->setExpanded(true);
    genNode->setExpanded(true);

    const QList<QTreeWidgetItem*> sampleRoots = ui.browserTree->findItems("Sample Packs", Qt::MatchExactly | Qt::MatchRecursive, 0);
    if (!sampleRoots.isEmpty()) {
        QTreeWidgetItem* sampleRoot = sampleRoots.first();
        qDeleteAll(sampleRoot->takeChildren());
        const QStringList samples = QSettings().value("SamplePackFiles").toStringList();
        const double projectTempo = m_transportWidget ? m_transportWidget->tempo() : kDefaultTempoBpm;
        const int signatureNumerator = m_transportWidget ? m_transportWidget->signatureNumerator() : 4;
        const int signatureDenominator = m_transportWidget ? m_transportWidget->signatureDenominator() : 4;
        for (const QString& samplePath : samples) {
            const QFileInfo info(samplePath);
            if (!info.exists() || !info.isFile()) {
                continue;
            }
            const QString suffix = info.suffix().toLower();
            const bool isMidiSample = suffix == "mid" || suffix == "midi";
            const bool canDecodeAudio = AudioFileDecoder::canDecodePath(info.absoluteFilePath());
            const QString samplePrefix = isMidiSample
                ? "[MIDI] "
                : (canDecodeAudio ? "[Audio] " : "[Needs embedded decoder] ");
            QTreeWidgetItem* sampleLeaf = new QTreeWidgetItem(sampleRoot, QStringList() << samplePrefix + info.fileName());
            sampleLeaf->setData(0, kSamplePathRole, info.absoluteFilePath());
            if (isMidiSample) {
                sampleLeaf->setToolTip(0, QString("%1\nMIDI import enabled; double-click on a MIDI track.")
                    .arg(info.absoluteFilePath()));
            } else if (canDecodeAudio) {
                const AudioFileProbeResult probe = AudioFileDecoder::probeAudio(info.absoluteFilePath());
                const double tempoEstimate = probe.success
                    ? estimateSampleTempoBpm(probe.durationSeconds(), projectTempo, signatureNumerator, signatureDenominator)
                    : 0.0;
                const QString tempoText = tempoEstimate > 0.0
                    ? QString("Tempo estimate: %1 BPM (%2/%3 meter)")
                          .arg(tempoEstimate, 0, 'f', 1)
                          .arg(signatureNumerator)
                          .arg(signatureDenominator)
                    : QString("Tempo estimate: unknown");
                sampleLeaf->setToolTip(0, probe.success
                    ? QString("%1\nNative audio playback/render enabled\n%2 seconds\n%3")
                          .arg(info.absoluteFilePath())
                          .arg(probe.durationSeconds(), 0, 'f', 2)
                          .arg(tempoText)
                    : QString("%1\n%2").arg(info.absoluteFilePath(), probe.message));
            } else {
                sampleLeaf->setToolTip(0, QString("%1\n%2").arg(info.absoluteFilePath(), AudioFileDecoder::unsupportedDecodeMessage(info.absoluteFilePath())));
            }
        }
        if (sampleRoot->childCount() == 0) {
            QTreeWidgetItem* empty = new QTreeWidgetItem(sampleRoot, QStringList() << "No samples scanned");
            empty->setDisabled(true);
        }
        sampleRoot->setExpanded(true);
    }
    ui.browserTree->resizeColumnToContents(0);
}

void VeraCoreQT::addSinglePluginToBrowser(const PluginInfo& info)
{
    QList<QTreeWidgetItem*> items = ui.browserTree->findItems("Plugins", Qt::MatchExactly | Qt::MatchRecursive, 0);
    if (items.isEmpty()) return;
    QTreeWidgetItem* pluginsNode = items.first();

    QTreeWidgetItem* audioNode = nullptr;
    QTreeWidgetItem* genNode = nullptr;
    for (int i = 0; i < pluginsNode->childCount(); ++i) {
        const QString text = pluginsNode->child(i)->text(0);
        if (text == "Audio Plugins" || text == "Audio Effects") audioNode = pluginsNode->child(i);
        if (text == "Generator Plugins" || text == "Instruments") genNode = pluginsNode->child(i);
    }

    QTreeWidgetItem* categoryNode = (info.category == "Instrument" && genNode) ? genNode : audioNode;
    if (!categoryNode) return;

    auto typeLabel = [](const QString& type) {
        if (type.compare("VST2", Qt::CaseInsensitive) == 0 || type.compare("VST", Qt::CaseInsensitive) == 0)
            return QString("VST2");
        if (type.compare("VST3", Qt::CaseInsensitive) == 0)
            return QString("VST3");
        if (type.compare("CLAP", Qt::CaseInsensitive) == 0 || type.compare("Clap", Qt::CaseInsensitive) == 0)
            return QString("CLAP");
        return type;
    };

    QTreeWidgetItem* typeNode = nullptr;
    const QString label = typeLabel(info.type);
    for (int i = 0; i < categoryNode->childCount(); ++i) {
        if (typeLabel(categoryNode->child(i)->text(0)) == label) {
            typeNode = categoryNode->child(i);
            break;
        }
    }
    if (!typeNode) {
        typeNode = new QTreeWidgetItem(categoryNode, QStringList() << label);
    }
    QTreeWidgetItem* pluginLeaf = new QTreeWidgetItem(typeNode, QStringList() << info.name);
    pluginLeaf->setData(0, kPluginPathRole, info.path);
    pluginLeaf->setData(0, kPluginNameRole, info.name);
    pluginLeaf->setData(0, kPluginTypeRole, info.type);
    pluginLeaf->setData(0, kPluginCategoryRole, info.category);
    pluginLeaf->setData(0, kPluginUidRole, info.uid);
    if (!info.vendor.isEmpty()) {
        pluginLeaf->setToolTip(0, QString("%1\n%2 %3\n%4").arg(info.vendor, label, info.version, info.path));
    } else {
        pluginLeaf->setToolTip(0, info.path);
    }
    pluginsNode->setExpanded(true);
    categoryNode->setExpanded(true);
    typeNode->setExpanded(true);
}

void VeraCoreQT::ConnectAudio(const shared_ptr<AudioGeneral>& audio) {
    if (audioGeneral) {
        audioGeneral->SetMidiInputCallback({});
        audioGeneral->audioCore.SetExternalMidiRenderer(nullptr, nullptr);
        audioGeneral->audioCore.SetExternalAudioFxRenderer(nullptr, nullptr);
		audioGeneral->audioCore.SetExternalRenderBatchCallbacks(nullptr, nullptr, nullptr);
    }
    this->audioGeneral = audio;
    if (!m_pluginHost) {
        m_pluginHost = std::make_unique<ExternalPluginHost>();
    }
    m_pluginHost->setPanLawDb(m_projectPanLawDb);
    if (audioGeneral) {
        audioGeneral->audioCore.SetExternalMidiRenderer(&ExternalPluginHost::audioCallback, m_pluginHost.get());
        audioGeneral->audioCore.SetExternalAudioFxRenderer(&ExternalPluginHost::audioFxCallback, m_pluginHost.get());
		audioGeneral->audioCore.SetExternalRenderBatchCallbacks(
			&ExternalPluginHost::beginRenderBatchCallback,
			&ExternalPluginHost::endRenderBatchCallback, m_pluginHost.get());
        audioGeneral->audioCore.SetLoopRange(m_loopEnabled, m_loopStartBeat, m_loopEndBeat);
        audioGeneral->audioCore.SetPanLawDb(static_cast<float>(m_projectPanLawDb));
        audioGeneral->audioCore.SetSpeakerLayout(m_projectSpeakerLayout);
        audioGeneral->SetMidiInputCallback([this](const VeraCoreMidiEvent& event) {
            QMetaObject::invokeMethod(this, [this, event]() {
                if (!handleStepMidiInput(event)) bufferRetrospectiveMidiInput(event);
                handleRecordedMidiInput(event);
            }, Qt::QueuedConnection);
        });
    }
    m_mixerParameters->republishProject(m_tracks);
    syncPluginTransportToHost();
    syncTempoMapToAudio();
    syncTimeSignatureMapToAudio();
    if (!m_recoveryPromptScheduled) {
        m_recoveryPromptScheduled = true;
        QTimer::singleShot(0, this, [this]() { offerRecoveryIfPresent(); });
    }
}

VeraCoreQT::~VeraCoreQT()
{
    if (audioGeneral) {
        audioGeneral->SetMidiInputCallback({});
        audioGeneral->audioCore.SetExternalMidiRenderer(nullptr, nullptr);
        audioGeneral->audioCore.SetExternalAudioFxRenderer(nullptr, nullptr);
		audioGeneral->audioCore.SetExternalRenderBatchCallbacks(nullptr, nullptr, nullptr);
    }
    m_pluginHost.reset();
}
