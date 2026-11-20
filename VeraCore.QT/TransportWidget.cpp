#include "TransportWidget.h"
#include <QFontDatabase>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>
#include <limits>

TransportWidget::TransportWidget(QWidget* parent)
    : QWidget(parent)
{
    m_meterClock.start();
    setupUi();
}

double TransportWidget::tempo() const
{
    return m_tempoSpin ? m_tempoSpin->value() : 120.0;
}

void TransportWidget::setTempo(double bpm)
{
    if (m_tempoSpin) {
        const double safeBpm = std::isfinite(bpm) ? qBound(20.0, bpm, 300.0) : 120.0;
        m_tempoSpin->setValue(safeBpm);
    }
}

int TransportWidget::signatureNumerator() const
{
    return m_signatureNum ? m_signatureNum->currentText().toInt() : 4;
}

int TransportWidget::signatureDenominator() const
{
    return m_signatureDen ? m_signatureDen->currentText().toInt() : 4;
}

bool TransportWidget::metronomeEnabled() const
{
    return m_metronomeBtn && m_metronomeBtn->isChecked();
}

int TransportWidget::countInBars() const
{
    return m_countInCombo ? m_countInCombo->currentData().toInt() : 0;
}

int TransportWidget::keyRoot() const
{
    return m_keyNote ? qBound(0, m_keyNote->currentIndex(), 11) : 0;
}

bool TransportWidget::minorScale() const
{
    return m_keyScale && m_keyScale->currentIndex() == 1;
}

double TransportWidget::snapBeats() const
{
    if (!m_snapCombo) return 0.25;
    const double value = m_snapCombo->currentData().toDouble();
    if (value >= 0.0) return value;
    return qMax(0.25, static_cast<double>(signatureNumerator()) * 4.0 /
        static_cast<double>(qMax(1, signatureDenominator())));
}

int TransportWidget::ppq() const
{
    return m_ppqSpin ? m_ppqSpin->value() : 960;
}

void TransportWidget::setSnapBeats(double beats)
{
    if (!m_snapCombo || !std::isfinite(beats)) return;
    const double barBeats = qMax(0.25, static_cast<double>(signatureNumerator()) * 4.0 /
        static_cast<double>(qMax(1, signatureDenominator())));
    int index = std::fabs(beats - barBeats) < 1.0e-6 ? m_snapCombo->findData(-1.0) : -1;
    if (index < 0) {
        for (int i = 0; i < m_snapCombo->count(); ++i) {
            if (std::fabs(m_snapCombo->itemData(i).toDouble() - beats) < 1.0e-6) {
                index = i;
                break;
            }
        }
    }
    if (index >= 0) m_snapCombo->setCurrentIndex(index);
}

void TransportWidget::setPpq(int ppqValue)
{
    if (!m_ppqSpin) return;
    m_ppqSpin->setValue(qBound(24, ppqValue, 9600));
}

void TransportWidget::setTimeSignature(int numerator, int denominator)
{
    const int previousNumerator = signatureNumerator();
    const int previousDenominator = signatureDenominator();
    const int safeNumerator = qBound(1, numerator, 32);
    const int boundedDenominator = qBound(1, denominator, 64);
    const int safeDenominator = (boundedDenominator & (boundedDenominator - 1)) == 0
        ? boundedDenominator : 4;

    if (m_signatureNum) {
        QSignalBlocker blocker(m_signatureNum);
        m_signatureNum->setCurrentText(QString::number(safeNumerator));
    }
    if (m_signatureDen) {
        QSignalBlocker blocker(m_signatureDen);
        m_signatureDen->setCurrentText(QString::number(safeDenominator));
    }
    if (safeNumerator != previousNumerator || safeDenominator != previousDenominator) {
        emit timeSignatureChanged(safeNumerator, safeDenominator);
        if (m_snapCombo && m_snapCombo->currentData().toDouble() < 0.0) emit snapChanged(snapBeats());
    }
}

void TransportWidget::setTimeSignatureMap(const std::vector<MidiTimeSignaturePoint>& points)
{
    constexpr size_t kMaxSignaturePoints = 4096;
    std::vector<MidiTimeSignaturePoint> normalized;
    normalized.reserve(std::min(points.size(), kMaxSignaturePoints));
    for (const MidiTimeSignaturePoint& point : points) {
        const bool validDenominator = point.denominator >= 1 && point.denominator <= 64 &&
            (point.denominator & (point.denominator - 1)) == 0;
        if (!std::isfinite(point.beat) || point.beat < 0.0 ||
            point.numerator < 1 || point.numerator > 32 || !validDenominator) continue;
        if (normalized.size() >= kMaxSignaturePoints) break;
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
    if (collapsed.empty()) collapsed.push_back({0.0, signatureNumerator(), signatureDenominator()});
    else if (collapsed.front().beat > 0.0) {
        if (collapsed.size() == kMaxSignaturePoints) collapsed.pop_back();
        collapsed.insert(collapsed.begin(), {0.0, signatureNumerator(), signatureDenominator()});
    }
    m_timeSignatureMap = std::move(collapsed);
    setPositionBeats(m_positionBeat);
}

void TransportWidget::setKeyScale(int rootNote, bool minor)
{
    const int safeRoot = qBound(0, rootNote, 11);
    const int safeScale = minor ? 1 : 0;
    const bool changed = keyRoot() != safeRoot || minorScale() != minor;
    if (m_keyNote) {
        QSignalBlocker blocker(m_keyNote);
        m_keyNote->setCurrentIndex(safeRoot);
    }
    if (m_keyScale) {
        QSignalBlocker blocker(m_keyScale);
        m_keyScale->setCurrentIndex(safeScale);
    }
    if (changed) emit keyScaleChanged(safeRoot, minor);
}

void TransportWidget::setPlaying(bool playing)
{
    if (m_playBtn) {
        m_playBtn->setChecked(playing);
    }
}

void TransportWidget::setRecording(bool recording)
{
    if (m_recordBtn) {
        QSignalBlocker blocker(m_recordBtn);
        m_recordBtn->setChecked(recording);
    }
}

void TransportWidget::setPunchEnabled(bool enabled)
{
    if (!m_punchBtn) return;
    QSignalBlocker blocker(m_punchBtn);
    m_punchBtn->setChecked(enabled);
}

void TransportWidget::setCountInBars(int bars)
{
    if (!m_countInCombo) return;
    const int safeBars = bars == 1 || bars == 2 || bars == 4 ? bars : 0;
    const int index = m_countInCombo->findData(safeBars);
    if (index < 0) return;
    QSignalBlocker blocker(m_countInCombo);
    m_countInCombo->setCurrentIndex(index);
}

void TransportWidget::togglePunch()
{
    if (m_punchBtn) m_punchBtn->toggle();
}

void TransportWidget::setPositionBeats(double beat)
{
    if (!std::isfinite(beat) || beat < 0.0) {
        beat = 0.0;
    }
    m_positionBeat = beat;
    if (m_timeSignatureMap.empty())
        m_timeSignatureMap.push_back({0.0, signatureNumerator(), signatureDenominator()});
    const auto next = std::upper_bound(m_timeSignatureMap.begin(), m_timeSignatureMap.end(), beat,
        [](double targetBeat, const MidiTimeSignaturePoint& point) {
            return targetBeat < point.beat;
        });
    const size_t activeIndex = next == m_timeSignatureMap.begin()
        ? 0u : static_cast<size_t>((next - m_timeSignatureMap.begin()) - 1);
    qint64 baseMeasure = 1;
    for (size_t index = 0; index < activeIndex; ++index) {
        const MidiTimeSignaturePoint& point = m_timeSignatureMap[index];
        const double nextBeat = m_timeSignatureMap[index + 1u].beat;
        const double barLength = static_cast<double>(point.numerator) *
            (4.0 / static_cast<double>(point.denominator));
        const qint64 bars = std::max<qint64>(1,
            static_cast<qint64>(std::ceil(
                std::max(0.0, nextBeat - point.beat) / barLength - 1.0e-9)));
        baseMeasure = std::min<qint64>(
            std::numeric_limits<int>::max(), baseMeasure + bars);
    }
    const MidiTimeSignaturePoint& signature = m_timeSignatureMap[activeIndex];
    const double notatedBeatWidth = 4.0 / static_cast<double>(signature.denominator);
    const double beatsPerMeasure = static_cast<double>(signature.numerator) * notatedBeatWidth;
    const qint64 localMeasure = std::max<qint64>(0,
        static_cast<qint64>(std::floor((beat - signature.beat) / beatsPerMeasure)));
    const int measure = static_cast<int>(std::min<qint64>(
        std::numeric_limits<int>::max(), baseMeasure + localMeasure));
    const double measureStart = signature.beat + static_cast<double>(localMeasure) * beatsPerMeasure;
    const double beatInMeasureValue = std::max(0.0, beat - measureStart);
    const double notatedBeatPosition = beatInMeasureValue / notatedBeatWidth;
    const int beatInMeasure = qFloor(notatedBeatPosition) + 1;
    const double subdivisionPosition =
        (notatedBeatPosition - std::floor(notatedBeatPosition)) * 4.0;
    const int subdivision = qBound(1, qFloor(subdivisionPosition) + 1, 4);
    const double ticksPerSubdivision =
        static_cast<double>(qMax(24, ppq())) * notatedBeatWidth / 4.0;
    const int tick = qMax(0, static_cast<int>(std::floor(
        (subdivisionPosition - std::floor(subdivisionPosition)) * ticksPerSubdivision)));
    if (m_timeDisplay) {
        m_timeDisplay->setText(QString(" %1. %2. %3. %4 ")
            .arg(measure)
            .arg(beatInMeasure)
            .arg(subdivision)
            .arg(tick, 3, 10, QChar('0')));
    }
}

void TransportWidget::setMeterPeaks(float left, float right,
                                    float momentaryLufs, float shortTermLufs,
                                    float integratedLufs, float loudnessRangeLu)
{
    if (!m_meterDisplay) {
        return;
    }

    auto toDb = [](float value) {
        const double safeValue = std::isfinite(value) ? qMax(0.0, static_cast<double>(value)) : 0.0;
        return safeValue <= 0.000001 ? -120.0 : 20.0 * std::log10(safeValue);
    };
    constexpr qint64 kPeakHoldMs = 1500;
    constexpr double kPeakDecayDbPerSecond = 20.0;
    const double peak = qBound(-120.0, qMax(toDb(left), toDb(right)), 120.0);
    if (!m_meterClock.isValid()) m_meterClock.start();
    const qint64 nowMs = m_meterClock.elapsed();
    const qint64 elapsedMs = m_meterLastUpdateMs > 0
        ? qBound<qint64>(qint64{0}, nowMs - m_meterLastUpdateMs, qint64{250}) : 0;
    m_meterLastUpdateMs = nowMs;
    if (peak >= m_meterHeldDb || m_meterHeldDb <= -119.9) {
        m_meterHeldDb = peak;
        m_meterHoldUntilMs = nowMs + kPeakHoldMs;
    } else if (nowMs > m_meterHoldUntilMs) {
        m_meterHeldDb = qMax(peak, m_meterHeldDb -
            kPeakDecayDbPerSecond * static_cast<double>(elapsedMs) / 1000.0);
    }
    const double displayedPeak = qMax(peak, m_meterHeldDb);
    const QString momentary = std::isfinite(momentaryLufs)
        ? QString::number(momentaryLufs, 'f', 1) : QStringLiteral("---");
    const QString shortTerm = std::isfinite(shortTermLufs)
        ? QString::number(shortTermLufs, 'f', 1) : QStringLiteral("---");
    const QString integrated = std::isfinite(integratedLufs)
        ? QString::number(integratedLufs, 'f', 1) : QStringLiteral("---");
    const QString loudnessRange = std::isfinite(loudnessRangeLu)
        ? QString::number(loudnessRangeLu, 'f', 1) : QStringLiteral("---");
    m_meterDisplay->setText(QString("TP %1 · M %2 · S %3 · I %4 · LRA %5")
        .arg(displayedPeak, 5, 'f', 1).arg(momentary).arg(shortTerm)
        .arg(integrated).arg(loudnessRange));
    m_meterDisplay->setStyleSheet(QString("color: %1; font-family: 'Consolas', monospace; "
                                          "font-size: 12px; font-weight: bold; "
                                          "background-color: #121212; border: 1px solid #000; "
                                          "border-radius: 3px; padding: 4px 8px;")
        .arg(displayedPeak >= -1.0 ? "#ff4d3d" :
            (displayedPeak >= -6.0 ? "#ffb347" : "#7bd66f")));
}

void TransportWidget::setAudioHealth(uint64_t callbackCount, double cpuLoad,
    uint64_t inputUnderflows,
    uint64_t inputOverflows, uint64_t outputUnderflows,
    uint64_t outputOverflows, uint64_t primingOutputs)
{
    if (!m_audioHealthBtn) return;
    if (!m_audioDeviceActive) return;
    uint64_t xrunCount = 0;
    const auto addSaturated = [&xrunCount](uint64_t value) {
        const uint64_t room = std::numeric_limits<uint64_t>::max() - xrunCount;
        xrunCount += std::min(value, room);
    };
    addSaturated(inputUnderflows);
    addSaturated(inputOverflows);
    addSaturated(outputUnderflows);
    addSaturated(outputOverflows);

    const double safeCpuLoad = std::isfinite(cpuLoad) ? std::clamp(cpuLoad, 0.0, 16.0) : 0.0;
    const int cpuPercent = static_cast<int>(std::lround(safeCpuLoad * 100.0));
    m_audioHealthBtn->setText(QString("CPU %1% · X%2")
        .arg(cpuPercent).arg(qulonglong(xrunCount)));
    m_audioHealthBtn->setToolTip(QString(
        "Audio callback health\nCPU load: %1%\nCallbacks: %2\nInput underflow: %3\nInput overflow: %4"
        "\nOutput underflow: %5\nOutput overflow: %6\nPriming output: %7"
        "\nClick to reset counters.")
        .arg(cpuPercent)
        .arg(qulonglong(callbackCount))
        .arg(qulonglong(inputUnderflows))
        .arg(qulonglong(inputOverflows))
        .arg(qulonglong(outputUnderflows))
        .arg(qulonglong(outputOverflows))
        .arg(qulonglong(primingOutputs)));
    const bool overloaded = safeCpuLoad >= 1.0;
    const bool pressured = safeCpuLoad >= 0.8;
    const QString foreground = xrunCount > 0 || overloaded
        ? "#ff6b5f" : pressured ? "#ffb347" : "#7bd66f";
    const QString border = xrunCount > 0 || overloaded
        ? "#74312c" : pressured ? "#805d2a" : "#263b28";
    m_audioHealthBtn->setStyleSheet(QString(
        "QPushButton { color: %1; font-family: 'Consolas', monospace; font-size: 11px; "
        "font-weight: bold; background-color: #121212; border: 1px solid %2; "
        "border-radius: 3px; padding: 4px 6px; } "
        "QPushButton:hover { background-color: #252525; }")
        .arg(foreground)
        .arg(border));
}

void TransportWidget::setAudioDeviceStatus(bool active, const QString& detail)
{
    if (!m_audioHealthBtn) return;
    const bool wasActive = m_audioDeviceActive;
    m_audioDeviceActive = active;
    if (m_playBtn) m_playBtn->setEnabled(active);
    if (m_recordBtn) m_recordBtn->setEnabled(active);
    if (active) {
        if (!wasActive) setAudioHealth(0, 0.0, 0, 0, 0, 0, 0);
        return;
    }
    m_audioHealthBtn->setText("AUDIO OFF");
    m_audioHealthBtn->setToolTip(detail);
    m_audioHealthBtn->setStyleSheet(
        "QPushButton { color: #ffb347; font-family: 'Consolas', monospace; font-size: 10px; "
        "font-weight: bold; background-color: #221b12; border: 1px solid #805d2a; "
        "border-radius: 3px; padding: 4px 6px; } "
        "QPushButton:hover { background-color: #362719; }");
}

void TransportWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateResponsiveLayout(event->size().width());
}

void TransportWidget::updateResponsiveLayout(int width)
{
    if (m_countInGroup) m_countInGroup->setVisible(width >= 1160);
    if (m_ppqGroup) m_ppqGroup->setVisible(width >= 1040);
    if (m_keyGroup) m_keyGroup->setVisible(width >= 900);
    if (m_snapGroup) m_snapGroup->setVisible(width >= 820);
    if (m_signatureGroup) m_signatureGroup->setVisible(width >= 720);
    if (m_audioHealthBtn) m_audioHealthBtn->setVisible(width >= 680);
    if (m_meterDisplay) m_meterDisplay->setVisible(width >= 590);
    if (m_tempoGroup) m_tempoGroup->setVisible(width >= 460);
    if (m_metronomeBtn) m_metronomeBtn->setVisible(width >= 390);
    if (layout()) {
        const int sideMargin = width < 590 ? 6 : 12;
        layout()->setContentsMargins(sideMargin, 6, sideMargin, 6);
    }

    if (m_timeDisplay) {
        m_timeDisplay->setStyleSheet(QString(
            "color: #ffa040; font-family: 'Consolas', monospace; font-size: %1px; "
            "font-weight: bold; border: none;").arg(width < 590 ? 16 : 20));
    }
}

void TransportWidget::setupUi() {
    this->setFixedHeight(56);
    this->setObjectName("TransportWidget");
    this->setStyleSheet("#TransportWidget { background-color: #1f1f1f; border-bottom: 1px solid #000; }");

    QHBoxLayout* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(12, 6, 12, 6);
    mainLayout->setSpacing(0);

    // 1. Transport controls
    QHBoxLayout* transportLayout = new QHBoxLayout();
    transportLayout->setSpacing(4);

    const QString btnStyle =
        "QPushButton { background-color: #2a2a2a; border: 1px solid #161616; "
        "border-radius: 4px; color: #eee; font-weight: bold; width: 34px; height: 34px; } "
        "QPushButton:hover { background-color: #3a3a3a; } "
        "QPushButton:pressed { background-color: #161616; }";

    // Glyphs are UTF-8 hex escapes so the file stays codepage-proof under MSVC.
    m_stopBtn = new QPushButton(QString::fromUtf8("\xE2\x96\xA0"), this); // BLACK SQUARE
    m_stopBtn->setToolTip("Stop");
    m_stopBtn->setAccessibleName("Stop playback");
    m_stopBtn->setStyleSheet(btnStyle + "QPushButton { color: #c8c8c8; font-size: 13px; }");

    m_playBtn = new QPushButton(QString::fromUtf8("\xE2\x96\xB6"), this); // BLACK RIGHT TRIANGLE
    m_playBtn->setToolTip("Play (Space)");
    m_playBtn->setAccessibleName("Play");
    m_playBtn->setCheckable(true);
    m_playBtn->setStyleSheet(btnStyle + "QPushButton { color: #6fcf73; font-size: 15px; } "
                                        "QPushButton:hover { color: #8fef93; }");

    m_pauseBtn = new QPushButton(QString::fromUtf8("\xE2\x9D\x9A\xE2\x9D\x9A"), this); // HEAVY VERTICAL BARS
    m_pauseBtn->setToolTip("Pause (Space)");
    m_pauseBtn->setAccessibleName("Pause");
    m_pauseBtn->setStyleSheet(btnStyle + "QPushButton { color: #ffc107; font-size: 11px; }");

    m_recordBtn = new QPushButton(QString::fromUtf8("\xE2\x97\x8F"), this); // BLACK CIRCLE
    m_recordBtn->setToolTip("Record");
    m_recordBtn->setAccessibleName("Toggle recording");
    m_recordBtn->setCheckable(true);
    m_recordBtn->setStyleSheet(btnStyle +
        "QPushButton { color: #e04a3a; border-radius: 17px; font-size: 14px; } "
        "QPushButton:checked { background-color: #e04a3a; color: white; }");

    m_punchBtn = new QPushButton("P", this);
    m_punchBtn->setToolTip("Punch In/Out (Ctrl+Shift+P) - uses the arrangement time selection");
    m_punchBtn->setAccessibleName("Toggle punch in and out recording");
    m_punchBtn->setCheckable(true);
    m_punchBtn->setStyleSheet(btnStyle +
        "QPushButton { color: #ffb74d; font-size: 12px; } "
        "QPushButton:checked { background-color: #b85f00; color: white; border-color: #ffb74d; }");

    transportLayout->addWidget(m_stopBtn);
    transportLayout->addWidget(m_playBtn);
    transportLayout->addWidget(m_pauseBtn);
    transportLayout->addWidget(m_recordBtn);
    transportLayout->addWidget(m_punchBtn);
    connect(m_playBtn, &QPushButton::clicked, this, &TransportWidget::playRequested);
    connect(m_pauseBtn, &QPushButton::clicked, this, &TransportWidget::pauseRequested);
    connect(m_stopBtn, &QPushButton::clicked, this, &TransportWidget::stopRequested);
    connect(m_recordBtn, &QPushButton::toggled, this, &TransportWidget::recordToggled);
    connect(m_punchBtn, &QPushButton::toggled, this, &TransportWidget::punchToggled);

    // 2. Time display, FL-style LCD with the orange accent
    QFrame* timeFrame = new QFrame(this);
    timeFrame->setStyleSheet("QFrame { background-color: #121212; border: 1px solid #000; "
                             "border-radius: 4px; padding: 2px 12px; }");
    QHBoxLayout* timeLayout = new QHBoxLayout(timeFrame);
    timeLayout->setContentsMargins(0, 0, 0, 0);

    m_timeDisplay = new QLabel(" 1. 1. 1. 00 ", timeFrame);
    m_timeDisplay->setStyleSheet("color: #ffa040; font-family: 'Consolas', monospace; "
                                 "font-size: 20px; font-weight: bold; border: none;");
    timeLayout->addWidget(m_timeDisplay);

    m_meterDisplay = new QLabel("TP -inf · M --- · S --- · I --- · LRA ---", this);
    m_meterDisplay->setToolTip(
        "Post-master 4x true peak · BS.1770 momentary/short-term/integrated loudness · EBU loudness range");
    m_meterDisplay->setFixedWidth(350);
    setMeterPeaks(0.0f, 0.0f,
                  -std::numeric_limits<float>::infinity(),
                  -std::numeric_limits<float>::infinity(),
                  -std::numeric_limits<float>::infinity(),
                  -std::numeric_limits<float>::infinity());

    m_audioHealthBtn = new QPushButton("CPU 0% · X0", this);
    m_audioHealthBtn->setFixedWidth(104);
    m_audioHealthBtn->setAccessibleName("Audio CPU load and dropout counter");
    setAudioHealth(0, 0.0, 0, 0, 0, 0, 0);
    connect(m_audioHealthBtn, &QPushButton::clicked, this, [this]() {
        if (m_audioDeviceActive) emit audioHealthResetRequested();
        else emit audioRecoveryRequested();
    });

    // 3. Project settings (tempo, signature, metronome, key)
    QHBoxLayout* settingsLayout = new QHBoxLayout();
    settingsLayout->setSpacing(10);

    const QString comboStyle =
        "QComboBox, QDoubleSpinBox, QSpinBox { background-color: #2a2a2a; color: #e8e8e8; "
        "border: 1px solid #161616; border-radius: 3px; padding: 2px 5px; height: 24px; } "
        "QComboBox:hover, QDoubleSpinBox:hover, QSpinBox:hover { border-color: #ff9020; }";
    const QString labelStyle = "color: #9a9a9a; font-size: 9px; font-weight: bold;";

    // Tempo
    m_tempoGroup = new QWidget(this);
    QHBoxLayout* tempoLayout = new QHBoxLayout(m_tempoGroup);
    tempoLayout->setContentsMargins(0, 0, 0, 0);
    tempoLayout->setSpacing(4);
    QLabel* tempoLbl = new QLabel("TEMPO", this);
    tempoLbl->setStyleSheet(labelStyle);
    m_tempoSpin = new QDoubleSpinBox(this);
    m_tempoSpin->setRange(20.0, 300.0);
    m_tempoSpin->setValue(120.00);
    m_tempoSpin->setDecimals(2);
    m_tempoSpin->setStyleSheet(comboStyle + "QDoubleSpinBox { width: 64px; }");
    connect(m_tempoSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &TransportWidget::tempoChanged);
    tempoLayout->addWidget(tempoLbl);
    tempoLayout->addWidget(m_tempoSpin);
    settingsLayout->addWidget(m_tempoGroup);

    m_snapGroup = new QWidget(this);
    QHBoxLayout* snapLayout = new QHBoxLayout(m_snapGroup);
    snapLayout->setContentsMargins(0, 0, 0, 0);
    snapLayout->setSpacing(4);
    QLabel* snapLbl = new QLabel("SNAP", this);
    snapLbl->setStyleSheet(labelStyle);
    m_snapCombo = new QComboBox(this);
    m_snapCombo->addItem("None", 0.0);
    m_snapCombo->addItem("1/16", 0.25);
    m_snapCombo->addItem("1/8", 0.5);
    m_snapCombo->addItem("1/4", 1.0);
    m_snapCombo->addItem("1/2", 2.0);
    m_snapCombo->addItem("Bar", -1.0);
    m_snapCombo->setCurrentIndex(1);
    m_snapCombo->setStyleSheet(comboStyle + "QComboBox { width: 54px; }");
    m_snapCombo->setToolTip("Playlist grid snap; hold Alt to bypass temporarily");
    connect(m_snapCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this](int) { emit snapChanged(snapBeats()); });
    snapLayout->addWidget(snapLbl);
    snapLayout->addWidget(m_snapCombo);
    settingsLayout->addWidget(m_snapGroup);

    // Signature
    m_signatureGroup = new QWidget(this);
    QHBoxLayout* sigLayout = new QHBoxLayout(m_signatureGroup);
    sigLayout->setContentsMargins(0, 0, 0, 0);
    sigLayout->setSpacing(4);
    QLabel* sigLbl = new QLabel("SIG", this);
    sigLbl->setStyleSheet(labelStyle);

    m_signatureNum = new QComboBox(this);
    for (int i = 1; i <= 32; ++i) m_signatureNum->addItem(QString::number(i));
    m_signatureNum->setCurrentText("4");
    m_signatureNum->setStyleSheet(comboStyle + "QComboBox { width: 34px; }");

    QLabel* slashLbl = new QLabel("/", this);
    slashLbl->setStyleSheet("color: #777;");

    m_signatureDen = new QComboBox(this);
    m_signatureDen->addItems({"1", "2", "4", "8", "16", "32", "64"});
    m_signatureDen->setCurrentText("4");
    m_signatureDen->setStyleSheet(comboStyle + "QComboBox { width: 34px; }");
    const auto emitSignatureChanged = [this](const QString&) {
        emit timeSignatureChanged(signatureNumerator(), signatureDenominator());
        if (m_snapCombo && m_snapCombo->currentData().toDouble() < 0.0) emit snapChanged(snapBeats());
    };
    connect(m_signatureNum, &QComboBox::currentTextChanged, this, emitSignatureChanged);
    connect(m_signatureDen, &QComboBox::currentTextChanged, this, emitSignatureChanged);

    sigLayout->addWidget(sigLbl);
    sigLayout->addWidget(m_signatureNum);
    sigLayout->addWidget(slashLbl);
    sigLayout->addWidget(m_signatureDen);
    settingsLayout->addWidget(m_signatureGroup);

    m_ppqGroup = new QWidget(this);
    QHBoxLayout* ppqLayout = new QHBoxLayout(m_ppqGroup);
    ppqLayout->setContentsMargins(0, 0, 0, 0);
    ppqLayout->setSpacing(4);
    QLabel* ppqLabel = new QLabel("PPQ", this);
    ppqLabel->setStyleSheet(labelStyle);
    m_ppqSpin = new QSpinBox(this);
    m_ppqSpin->setRange(24, 9600);
    m_ppqSpin->setSingleStep(24);
    m_ppqSpin->setValue(960);
    m_ppqSpin->setStyleSheet(comboStyle + "QSpinBox { width: 62px; }");
    m_ppqSpin->setToolTip("Project pulses per quarter note; changing PPQ preserves musical positions");
    m_ppqSpin->setAccessibleName("Project PPQ resolution");
    connect(m_ppqSpin, qOverload<int>(&QSpinBox::valueChanged), this, &TransportWidget::ppqChanged);
    ppqLayout->addWidget(ppqLabel);
    ppqLayout->addWidget(m_ppqSpin);
    settingsLayout->addWidget(m_ppqGroup);

    // Metronome
    m_metronomeBtn = new QPushButton("Click", this);
    m_metronomeBtn->setCheckable(true);
    m_metronomeBtn->setToolTip("Metronome");
    m_metronomeBtn->setStyleSheet(
        "QPushButton { background-color: #2a2a2a; border: 1px solid #161616; border-radius: 3px; "
        "color: #9a9a9a; padding: 2px 10px; height: 24px; } "
        "QPushButton:hover { background-color: #3a3a3a; } "
        "QPushButton:checked { background-color: #ff9020; color: #141414; font-weight: bold; }");
    connect(m_metronomeBtn, &QPushButton::toggled, this, &TransportWidget::metronomeToggled);
    settingsLayout->addWidget(m_metronomeBtn);

    m_countInGroup = new QWidget(this);
    QHBoxLayout* countInLayout = new QHBoxLayout(m_countInGroup);
    countInLayout->setContentsMargins(0, 0, 0, 0);
    countInLayout->setSpacing(4);
    QLabel* countInLabel = new QLabel("COUNT", this);
    countInLabel->setStyleSheet(labelStyle);
    m_countInCombo = new QComboBox(this);
    m_countInCombo->addItem("Off", 0);
    m_countInCombo->addItem("1 bar", 1);
    m_countInCombo->addItem("2 bars", 2);
    m_countInCombo->addItem("4 bars", 4);
    m_countInCombo->setStyleSheet(comboStyle + "QComboBox { width: 62px; }");
    m_countInCombo->setToolTip("Metronome count-in before recording; follows project time signatures");
    connect(m_countInCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this](int) { emit countInBarsChanged(countInBars()); });
    countInLayout->addWidget(countInLabel);
    countInLayout->addWidget(m_countInCombo);
    settingsLayout->addWidget(m_countInGroup);

    // Key / Scale
    m_keyGroup = new QWidget(this);
    QHBoxLayout* keyLayout = new QHBoxLayout(m_keyGroup);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->setSpacing(4);
    QLabel* keyLbl = new QLabel("KEY", this);
    keyLbl->setStyleSheet(labelStyle);

    m_keyNote = new QComboBox(this);
    m_keyNote->addItems({"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});
    m_keyNote->setStyleSheet(comboStyle);

    m_keyScale = new QComboBox(this);
    m_keyScale->addItems({"Major", "Minor"});
    m_keyScale->setStyleSheet(comboStyle);
    const auto emitKeyScale = [this](int) {
        emit keyScaleChanged(keyRoot(), minorScale());
    };
    connect(m_keyNote, qOverload<int>(&QComboBox::currentIndexChanged), this, emitKeyScale);
    connect(m_keyScale, qOverload<int>(&QComboBox::currentIndexChanged), this, emitKeyScale);

    keyLayout->addWidget(keyLbl);
    keyLayout->addWidget(m_keyNote);
    keyLayout->addWidget(m_keyScale);
    settingsLayout->addWidget(m_keyGroup);

    // Single, centered assembly. Each layout is added exactly once.
    mainLayout->addStretch();
    mainLayout->addLayout(transportLayout);
    mainLayout->addSpacing(12);
    mainLayout->addWidget(timeFrame);
    mainLayout->addSpacing(10);
    mainLayout->addWidget(m_meterDisplay);
    mainLayout->addSpacing(6);
    mainLayout->addWidget(m_audioHealthBtn);
    mainLayout->addSpacing(12);
    mainLayout->addLayout(settingsLayout);
    mainLayout->addStretch();
    updateResponsiveLayout(width());
}
