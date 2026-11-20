#include "InspectorWidget.h"
#include "AudioStretchBackend.h"
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {

int gainToFader(float gain)
{
    return gain <= 0.0f ? -700 : qBound(-700, qRound(200.0 * std::log10(gain)), 60);
}

float faderToGain(int value)
{
    return value <= -700 ? 0.0f : static_cast<float>(std::pow(10.0, value / 200.0));
}

QString faderText(int value)
{
    return value <= -700 ? QString("-inf dB") : QString("%1 dB").arg(value / 10.0, 0, 'f', 1);
}

float clipGainFromDb(double db)
{
    return db <= -70.0 ? 0.0f : static_cast<float>(std::pow(10.0, db / 20.0));
}

double clipGainToDb(float gain)
{
    return gain <= 0.0f ? -70.0 : qBound(-70.0, 20.0 * std::log10(static_cast<double>(gain)), 12.0);
}

QChar returnBusLetter(int bus)
{
    return QChar(static_cast<char16_t>(u'A' + bus));
}

QString pluginStatusText(InspectorWidget::PluginHostStatus status)
{
    switch (status) {
    case InspectorWidget::PluginHostStatus::Active: return "DSP READY";
    case InspectorWidget::PluginHostStatus::Faulted: return "DSP FAULT";
    case InspectorWidget::PluginHostStatus::Failed: return "LOAD FAILED";
    case InspectorWidget::PluginHostStatus::Unsupported: return "UNSUPPORTED";
    case InspectorWidget::PluginHostStatus::Pending: return "PENDING";
    case InspectorWidget::PluginHostStatus::Internal: return {};
    }
    return {};
}

QString pluginStatusProperty(InspectorWidget::PluginHostStatus status)
{
    switch (status) {
    case InspectorWidget::PluginHostStatus::Active: return "active";
    case InspectorWidget::PluginHostStatus::Faulted: return "faulted";
    case InspectorWidget::PluginHostStatus::Pending: return "pending";
    case InspectorWidget::PluginHostStatus::Failed: return "failed";
    case InspectorWidget::PluginHostStatus::Unsupported: return "unsupported";
    case InspectorWidget::PluginHostStatus::Internal: return "internal";
    }
    return "internal";
}

QString pluginStatusDescription(InspectorWidget::PluginHostStatus status)
{
    switch (status) {
    case InspectorWidget::PluginHostStatus::Active:
        return "x64 plugin DSP loaded for live playback and offline rendering.";
    case InspectorWidget::PluginHostStatus::Faulted:
        return "The plug-in faulted during DSP. It was bypassed immediately; use Reload to create a clean instance.";
    case InspectorWidget::PluginHostStatus::Failed:
        return "The x64 plugin failed to load. Instrument layers use the internal synth fallback; failed audio effects are bypassed.";
    case InspectorWidget::PluginHostStatus::Unsupported:
        return "This plugin format is not supported. VeraCore accepts x64 VST2, VST3, and CLAP plugins.";
    case InspectorWidget::PluginHostStatus::Pending:
        return "Plugin status will be evaluated when the audio engine is connected.";
    case InspectorWidget::PluginHostStatus::Internal:
        return "Internal VeraCore instrument.";
    }
    return {};
}

} // namespace

InspectorWidget::InspectorWidget(QWidget* parent)
    : QWidget(parent), m_currentTrack(nullptr), m_showingMidiClip(false), m_showingAudioClip(false),
      m_beatsPerBar(4.0), m_notatedBeatWidth(1.0), m_currentClipStartBeat(0.0), m_currentClipLengthBeats(0.0)
{
    setupUi();
}

void InspectorWidget::setupUi() {
    // Horizontal device strip docked under the arrangement (Ableton-style):
    // [ track tile | Instrument | Audio FX | Output ]
    this->setMinimumHeight(120);
    this->setMaximumHeight(240);
    this->setObjectName("InspectorWidget");
    this->setStyleSheet("#InspectorWidget { background-color: #232323; border-top: 1px solid #0d0d0d; }");

    QHBoxLayout* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    QScrollArea* deviceScroll = new QScrollArea(this);
    deviceScroll->setObjectName("inspectorDeviceScroll");
    deviceScroll->setWidgetResizable(true);
    deviceScroll->setFrameShape(QFrame::NoFrame);
    deviceScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    deviceScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QWidget* deviceStrip = new QWidget(deviceScroll);
    deviceStrip->setObjectName("inspectorDeviceStrip");
    QHBoxLayout* mainLayout = new QHBoxLayout(deviceStrip);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);
    deviceScroll->setWidget(deviceStrip);
    rootLayout->addWidget(deviceScroll);

    const QString groupStyle =
        "QGroupBox { color: #9a9a9a; font-size: 10px; font-weight: bold; "
        "background-color: #2a2a2a; border: 1px solid #161616; border-radius: 4px; "
        "margin-top: 16px; } "
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: left top; "
        "left: 6px; top: 2px; background: transparent; border: none; padding: 0; }";

    // Track tile: selected track identity, FL-channel-button flavour.
    QFrame* trackTile = new QFrame(this);
    trackTile->setObjectName("inspectorTrackTile");
    trackTile->setMinimumWidth(140);
    trackTile->setMaximumWidth(190);
    trackTile->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    trackTile->setStyleSheet("#inspectorTrackTile { background-color: #2a2a2a; "
                             "border: 1px solid #161616; border-radius: 4px; }");
    QVBoxLayout* tileLayout = new QVBoxLayout(trackTile);
    tileLayout->setContentsMargins(8, 8, 8, 8);
    tileLayout->setSpacing(4);

    m_trackNameLabel = new QLabel("No Track Selected", trackTile);
    m_trackNameLabel->setStyleSheet("color: #f0f0f0; font-weight: bold; font-size: 14px; "
                                    "background: transparent; border: none;");
    m_trackNameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_trackNameLabel->setWordWrap(true);
    tileLayout->addWidget(m_trackNameLabel);

    const QString mixerButtonStyle =
        "QPushButton { background:#343434; color:#aaa; border:1px solid #151515; "
        "border-radius:2px; font-size:9px; font-weight:bold; padding:1px; } "
        "QPushButton:hover { border-color:#ff9020; color:white; }";
    QHBoxLayout* mixerButtons = new QHBoxLayout();
    mixerButtons->setSpacing(3);
    m_muteBtn = new QPushButton("M", trackTile);
    m_soloBtn = new QPushButton("S", trackTile);
    m_armBtn = new QPushButton("R", trackTile);
    m_monitorBtn = new QPushButton("I", trackTile);
    for (QPushButton* button : {m_muteBtn, m_soloBtn, m_armBtn, m_monitorBtn}) {
        button->setCheckable(true);
        button->setFixedHeight(18);
        button->setStyleSheet(mixerButtonStyle);
        mixerButtons->addWidget(button);
    }
    m_muteBtn->setToolTip("Mute");
    m_soloBtn->setToolTip("Solo");
    m_armBtn->setToolTip("Arm for recording");
    m_monitorBtn->setToolTip("Input monitor");
    m_muteBtn->setStyleSheet(mixerButtonStyle +
        "QPushButton:checked { background:#ffb13b; color:#141414; }");
    m_soloBtn->setStyleSheet(mixerButtonStyle +
        "QPushButton:checked { background:#4a90d9; color:#141414; }");
    m_armBtn->setStyleSheet(mixerButtonStyle +
        "QPushButton:checked { background:#e04a3a; color:white; }");
    m_monitorBtn->setStyleSheet(mixerButtonStyle +
        "QPushButton:checked { background:#56b6c2; color:#141414; }");
    tileLayout->addLayout(mixerButtons);

    const QString mixerSliderStyle =
        "QSlider::groove:horizontal { height:4px; background:#171717; border:1px solid #111; border-radius:2px; } "
        "QSlider::sub-page:horizontal { background:#71866d; border-radius:2px; } "
        "QSlider::handle:horizontal { width:7px; margin:-4px 0; background:#c7cec3; border:1px solid #333; }";
    auto addMixerSlider = [trackTile, tileLayout, &mixerSliderStyle](const QString& label,
                                                                    int minimum, int maximum) {
        QHBoxLayout* row = new QHBoxLayout();
        row->setSpacing(4);
        QLabel* title = new QLabel(label, trackTile);
        title->setFixedWidth(10);
        title->setStyleSheet("color:#899289; font-size:9px; font-weight:bold; border:none;");
        QSlider* slider = new QSlider(Qt::Horizontal, trackTile);
        slider->setRange(minimum, maximum);
        slider->setFixedHeight(12);
        slider->setStyleSheet(mixerSliderStyle);
        row->addWidget(title);
        row->addWidget(slider, 1);
        tileLayout->addLayout(row);
        return slider;
    };
    m_volumeSlider = addMixerSlider("V", -700, 60);
    m_panSlider = addMixerSlider("P", -100, 100);
    QHBoxLayout* widthRow = new QHBoxLayout();
    widthRow->setSpacing(4);
    QLabel* widthTitle = new QLabel("W", trackTile);
    widthTitle->setFixedWidth(10);
    widthTitle->setStyleSheet("color:#899289; font-size:9px; font-weight:bold; border:none;");
    m_stereoSeparationSlider = new QDial(trackTile);
    m_stereoSeparationSlider->setRange(-100, 100);
    m_stereoSeparationSlider->setNotchesVisible(false);
    m_stereoSeparationSlider->setWrapping(false);
    m_stereoSeparationSlider->setFixedSize(28, 28);
    widthRow->addWidget(widthTitle);
    widthRow->addWidget(m_stereoSeparationSlider);
    widthRow->addStretch();
    tileLayout->addLayout(widthRow);
    m_stereoSeparationSlider->setToolTip(
        "FL-style stereo separation: -100% 200% width, 0% original, +100% mono merge");

    m_araStatusLabel = new QLabel("ARA READY", trackTile);
    m_araStatusLabel->setStyleSheet(
        "color:#98ad91; background:#202620; border:1px solid #394439; "
        "border-radius:2px; padding:2px 4px; font-size:8px; font-weight:bold;");
    m_araStatusLabel->setToolTip(
        "ARA document status. Analysis and persistence stay outside the realtime audio callback.");
    m_araStatusLabel->hide();
    tileLayout->addWidget(m_araStatusLabel);
    m_araProgressBar = new QProgressBar(trackTile);
    m_araProgressBar->setRange(0, 1000);
    m_araProgressBar->setTextVisible(false);
    m_araProgressBar->setFixedHeight(4);
    m_araProgressBar->setStyleSheet(
        "QProgressBar { background:#171717; border:0; } "
        "QProgressBar::chunk { background:#78a36b; }");
    m_araProgressBar->hide();
    tileLayout->addWidget(m_araProgressBar);

    connect(m_muteBtn, &QPushButton::toggled, this, [this](bool) { emitMixerControlsEdited(); });
    connect(m_soloBtn, &QPushButton::toggled, this, [this](bool) { emitMixerControlsEdited(); });
    connect(m_armBtn, &QPushButton::toggled, this, [this](bool) { emitMixerControlsEdited(); });
    connect(m_monitorBtn, &QPushButton::toggled, this, [this](bool) { emitMixerControlsEdited(); });
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int) { emitMixerControlsEdited(); });
    connect(m_panSlider, &QSlider::valueChanged, this, [this](int) { emitMixerControlsEdited(); });
    connect(m_stereoSeparationSlider, &QDial::valueChanged,
        this, [this](int) { emitMixerControlsEdited(); });
    tileLayout->addStretch();

    m_contextLabel = new QLabel("TRACK", trackTile);
    m_contextLabel->setStyleSheet("color: #777; font-size: 9px; font-weight: bold; "
                                  "background: transparent; border: none;");
    tileLayout->addWidget(m_contextLabel);

    mainLayout->addWidget(trackTile);

    // MIDI clip details appear when an arrangement clip is selected/opened.
    m_clipGroup = new QGroupBox("MIDI CLIP", this);
    m_clipGroup->setStyleSheet(groupStyle);
    m_clipGroup->setMinimumWidth(430);
    m_clipGroup->setMaximumWidth(560);
    m_clipGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    QGridLayout* clipLayout = new QGridLayout(m_clipGroup);
    clipLayout->setContentsMargins(8, 8, 8, 8);
    clipLayout->setSpacing(6);

    m_clipStartLabel = new QLabel("Start: 1.1", m_clipGroup);
    m_clipStartLabel->setStyleSheet("color: #ccc; background-color: #333; padding: 6px 8px; "
                                    "border: 1px solid #161616; border-radius: 3px;");
    m_clipLengthLabel = new QLabel("Length: 4 beats", m_clipGroup);
    m_clipLengthLabel->setStyleSheet(m_clipStartLabel->styleSheet());
    m_clipSyncLabel = new QLabel("Sync: none", m_clipGroup);
    m_clipSyncLabel->setStyleSheet(m_clipStartLabel->styleSheet());
    m_syncModeCombo = new QComboBox(m_clipGroup);
    m_syncModeCombo->addItem("Stretch: Off", 0);
    m_syncModeCombo->addItem("Stretch: Auto BPM", 1);
    m_syncModeCombo->addItem("Stretch: Bars", 2);
    m_syncModeCombo->addItem("Stretch: Full Sync (clip length)", 3);
    m_sourceTempoSpin = new QDoubleSpinBox(m_clipGroup);
    m_sourceTempoSpin->setRange(0.0, 300.0);
    m_sourceTempoSpin->setDecimals(1);
    m_sourceTempoSpin->setSuffix(" BPM");
    m_sourceTempoSpin->setSpecialValueText("Auto BPM");
    m_pitchShiftSpin = new QDoubleSpinBox(m_clipGroup);
    m_pitchShiftSpin->setRange(-48.0, 48.0);
    m_pitchShiftSpin->setDecimals(1);
    m_pitchShiftSpin->setSuffix(" st");
    m_targetBarsSpin = new QDoubleSpinBox(m_clipGroup);
    m_targetBarsSpin->setRange(0.0, 512.0);
    m_targetBarsSpin->setDecimals(1);
    m_targetBarsSpin->setSuffix(" bars");
    m_clipGainSpin = new QDoubleSpinBox(m_clipGroup);
    m_clipGainSpin->setRange(-70.0, 12.0);
    m_clipGainSpin->setDecimals(1);
    m_clipGainSpin->setSuffix(" dB");
    m_clipGainSpin->setSpecialValueText("-inf dB");
    m_clipGainSpin->setToolTip("Non-destructive clip gain before track effects and mixer gain");
    m_clipPanSpin = new QDoubleSpinBox(m_clipGroup);
    m_clipPanSpin->setRange(-100.0, 100.0);
    m_clipPanSpin->setDecimals(0);
    m_clipPanSpin->setSuffix(" pan");
    m_clipPanSpin->setToolTip("Clip pan before track effects and mixer pan");
    m_sourceInSpin = new QDoubleSpinBox(m_clipGroup);
    m_sourceInSpin->setDecimals(3);
    m_sourceInSpin->setSingleStep(0.25);
    m_sourceInSpin->setSuffix(" beats IN");
    m_sourceInSpin->setToolTip("Non-destructive source in point for this clip instance");
    m_sourceOutSpin = new QDoubleSpinBox(m_clipGroup);
    m_sourceOutSpin->setDecimals(3);
    m_sourceOutSpin->setSingleStep(0.25);
    m_sourceOutSpin->setSuffix(" beats OUT");
    m_sourceOutSpin->setToolTip("Non-destructive source out point for this clip instance");
    connect(m_sourceInSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        QSignalBlocker blocker(m_sourceOutSpin);
        m_sourceOutSpin->setMinimum(value + 0.25);
        if (m_sourceOutSpin->value() < value + 0.25) m_sourceOutSpin->setValue(value + 0.25);
    });
    connect(m_sourceOutSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        QSignalBlocker blocker(m_sourceInSpin);
        m_sourceInSpin->setMaximum(qMax(0.0, value - 0.25));
        if (m_sourceInSpin->value() > value - 0.25) m_sourceInSpin->setValue(qMax(0.0, value - 0.25));
    });
    auto emitSourceRange = [this]() {
        if (m_showingAudioClip)
            emit audioClipSourceRangeChanged(m_sourceInSpin->value(), m_sourceOutSpin->value());
    };
    connect(m_sourceInSpin, &QAbstractSpinBox::editingFinished, this, emitSourceRange);
    connect(m_sourceOutSpin, &QAbstractSpinBox::editingFinished, this, emitSourceRange);
    m_fadeInSpin = new QDoubleSpinBox(m_clipGroup);
    m_fadeInSpin->setRange(0.0, 512.0);
    m_fadeInSpin->setDecimals(3);
    m_fadeInSpin->setSuffix(" beats in");
    m_fadeInSpin->setToolTip("Equal-power fade-in; stored per clip instance");
    m_fadeOutSpin = new QDoubleSpinBox(m_clipGroup);
    m_fadeOutSpin->setRange(0.0, 512.0);
    m_fadeOutSpin->setDecimals(3);
    m_fadeOutSpin->setSuffix(" beats out");
    m_fadeOutSpin->setToolTip("Equal-power fade-out; stored per clip instance");
    auto emitFades = [this](double) {
        if (m_showingAudioClip)
            emit audioClipFadesChanged(m_fadeInSpin->value(), m_fadeOutSpin->value());
    };
    connect(m_fadeInSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, emitFades);
    connect(m_fadeOutSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, emitFades);
    m_normalizeBtn = new QPushButton("NORMALIZE", m_clipGroup);
    m_reverseBtn = new QPushButton("REVERSE", m_clipGroup);
    m_reversePolarityBtn = new QPushButton("POLARITY", m_clipGroup);
    for (QPushButton* button : {m_normalizeBtn, m_reverseBtn, m_reversePolarityBtn}) {
        button->setCheckable(true);
        button->setStyleSheet("QPushButton { background:#303530; color:#b9c3b9; border:1px solid #161616; "
                              "padding:4px 6px; font-size:9px; font-weight:bold; } "
                              "QPushButton:checked { background:#ff9020; color:#171717; }");
    }
    m_normalizeBtn->setToolTip("Peak-normalize this shared audio content non-destructively");
    m_reverseBtn->setToolTip("Play this shared audio content backwards");
    m_reversePolarityBtn->setToolTip("Invert waveform polarity");
    auto emitProcessing = [this]() {
        if (!m_showingAudioClip) return;
        emit audioClipProcessingChanged(
            clipGainFromDb(m_clipGainSpin->value()),
            static_cast<float>(m_clipPanSpin->value() / 100.0),
            m_normalizeBtn->isChecked(), m_reverseBtn->isChecked(),
            m_reversePolarityBtn->isChecked());
    };
    connect(m_clipGainSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [emitProcessing](double) { emitProcessing(); });
    connect(m_clipPanSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [emitProcessing](double) { emitProcessing(); });
    connect(m_normalizeBtn, &QPushButton::toggled, this, [emitProcessing](bool) { emitProcessing(); });
    connect(m_reverseBtn, &QPushButton::toggled, this, [emitProcessing](bool) { emitProcessing(); });
    connect(m_reversePolarityBtn, &QPushButton::toggled, this, [emitProcessing](bool) { emitProcessing(); });
    auto emitSync = [this](double) {
        if (m_showingAudioClip) {
            emit audioClipSyncChanged(m_sourceTempoSpin->value(), m_pitchShiftSpin->value());
        }
    };
    connect(m_sourceTempoSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, emitSync);
    connect(m_pitchShiftSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, emitSync);
    connect(m_syncModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        const int mode = m_syncModeCombo->currentData().toInt();
        m_sourceTempoSpin->setVisible(m_showingAudioClip && mode == 1);
        m_targetBarsSpin->setVisible(m_showingAudioClip && mode == 2);
        const bool independentPitchAvailable = mode == 0 || AudioStretchBackend::isElastiqueAvailable();
        m_pitchShiftSpin->setEnabled(independentPitchAvailable);
        m_pitchShiftSpin->setToolTip(mode == 0
            ? "Varispeed transpose in semitones; changing pitch also changes duration."
            : (independentPitchAvailable
                ? "Independent pitch shift in semitones (Elastique Pro)."
                : "Independent pitch shift for synced clips requires Elastique Pro; basic sync preserves full clip duration."));
        if (m_showingAudioClip) emit audioClipSyncModeChanged(m_syncModeCombo->currentData().toInt(), m_targetBarsSpin->value());
    });
    connect(m_targetBarsSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
        if (m_showingAudioClip) emit audioClipSyncModeChanged(m_syncModeCombo->currentData().toInt(), m_targetBarsSpin->value());
    });
    m_openMidiEditorBtn = new QPushButton("Open MIDI Editor", m_clipGroup);
    m_openMidiEditorBtn->setEnabled(false);
    m_openMidiEditorBtn->setToolTip("Open the selected MIDI clip in the piano roll");
    m_openMidiEditorBtn->setStyleSheet("QPushButton { text-align: center; background-color: #303530; "
                                       "color: #d8ded8; border: 1px solid #161616; padding: 6px 8px; "
                                       "border-radius: 3px; font-weight: bold; } "
                                       "QPushButton:hover:enabled { border-color: #ff9020; color: #ffffff; } "
                                       "QPushButton:disabled { color: #666; background-color: #262626; }");
    connect(m_openMidiEditorBtn, &QPushButton::clicked, this, &InspectorWidget::openMidiEditorRequested);
    m_stretchStatusLabel = new QLabel(m_clipGroup);
    m_stretchStatusLabel->setAlignment(Qt::AlignCenter);
    m_stretchStatusLabel->setStyleSheet("QLabel { color: #b9c8b9; background-color: #273027; "
                                        "border: 1px solid #161616; padding: 6px 8px; "
                                        "border-radius: 3px; font-weight: bold; }");
    m_stretchStatusLabel->hide();
    m_mediaStatusLabel = new QLabel(m_clipGroup);
    m_mediaStatusLabel->setAlignment(Qt::AlignCenter);
    m_mediaStatusLabel->setObjectName(QStringLiteral("audioMediaStatus"));
    m_mediaStatusLabel->setProperty("mediaOffline", false);
    m_relinkMediaBtn = new QPushButton(QStringLiteral("Locate Missing Media…"), m_clipGroup);
    m_relinkMediaBtn->setToolTip(
        QStringLiteral("Relink every clip instance sharing this audio content"));
    m_relinkMediaBtn->hide();
    connect(m_relinkMediaBtn, &QPushButton::clicked,
            this, &InspectorWidget::audioClipRelinkRequested);
    m_araAlgorithmCombo = new QComboBox(m_clipGroup);
    m_araAlgorithmCombo->setPlaceholderText("ARA | Analysis pending");
    m_araAlgorithmCombo->setStyleSheet(
        "QComboBox { color:#d7ddd7; background:#2b322b; border:1px solid #3e493e; "
        "padding:4px 7px; font-size:9px; font-weight:bold; } "
        "QComboBox:hover { border-color:#78a36b; } "
        "QComboBox::drop-down { border:0; width:18px; }");
    m_araAlgorithmCombo->hide();
    connect(m_araAlgorithmCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (m_showingAudioClip && index >= 0)
            emit araProcessingAlgorithmChanged(index);
    });
    clipLayout->addWidget(m_clipStartLabel, 0, 0);
    clipLayout->addWidget(m_clipLengthLabel, 0, 1);
    clipLayout->addWidget(m_clipSyncLabel, 1, 0, 1, 2);
    clipLayout->addWidget(m_syncModeCombo, 2, 0);
    clipLayout->addWidget(m_sourceTempoSpin, 2, 1);
    clipLayout->addWidget(m_pitchShiftSpin, 3, 0);
    clipLayout->addWidget(m_targetBarsSpin, 3, 1);
    clipLayout->addWidget(m_clipGainSpin, 4, 0);
    clipLayout->addWidget(m_clipPanSpin, 4, 1);
    clipLayout->addWidget(m_sourceInSpin, 5, 0);
    clipLayout->addWidget(m_sourceOutSpin, 5, 1);
    clipLayout->addWidget(m_fadeInSpin, 6, 0);
    clipLayout->addWidget(m_fadeOutSpin, 6, 1);
    QHBoxLayout* processButtons = new QHBoxLayout();
    processButtons->setSpacing(3);
    processButtons->addWidget(m_normalizeBtn);
    processButtons->addWidget(m_reverseBtn);
    processButtons->addWidget(m_reversePolarityBtn);
    clipLayout->addLayout(processButtons, 7, 0, 1, 2);
    clipLayout->addWidget(m_mediaStatusLabel, 8, 0, 1, 2);
    clipLayout->addWidget(m_relinkMediaBtn, 9, 0, 1, 2);
    clipLayout->addWidget(m_openMidiEditorBtn, 10, 0, 1, 2);
    clipLayout->addWidget(m_stretchStatusLabel, 10, 0, 1, 2);
    clipLayout->addWidget(m_araAlgorithmCombo, 11, 0, 1, 2);
    mainLayout->addWidget(m_clipGroup);

    m_spatialGroup = new QGroupBox("SPATIAL PANNER", this);
    m_spatialGroup->setStyleSheet(groupStyle);
    m_spatialGroup->setMinimumWidth(300);
    QGridLayout* spatialLayout = new QGridLayout(m_spatialGroup);
    spatialLayout->setContentsMargins(8, 8, 8, 8);
    spatialLayout->setSpacing(4);
    m_spatialModeCombo = new QComboBox(m_spatialGroup);
    m_spatialModeCombo->addItem("Stereo pan", static_cast<int>(VeraCoreSpatialRenderMode::StereoPan));
    m_spatialModeCombo->addItem("Bed", static_cast<int>(VeraCoreSpatialRenderMode::Bed));
    m_spatialModeCombo->addItem("Object", static_cast<int>(VeraCoreSpatialRenderMode::Object));
    auto makeSpatialSpin = [this](double minimum, double maximum, double step,
                                  const QString& suffix) {
        auto* spin = new QDoubleSpinBox(m_spatialGroup);
        spin->setRange(minimum, maximum);
        spin->setSingleStep(step);
        spin->setDecimals(step < 1.0 ? 2 : 1);
        spin->setSuffix(suffix);
        spin->setKeyboardTracking(false);
        return spin;
    };
    m_azimuthSpin = makeSpatialSpin(-180.0, 180.0, 1.0, QString::fromUtf8("° AZ"));
    m_elevationSpin = makeSpatialSpin(-90.0, 90.0, 1.0, QString::fromUtf8("° EL"));
    m_distanceSpin = makeSpatialSpin(0.0, 1.0, 0.01, " DIST");
    m_spreadSpin = makeSpatialSpin(0.0, 1.0, 0.01, " WIDTH");
    m_lfeSendSpin = makeSpatialSpin(0.0, 1.0, 0.01, " LFE");
    m_linkStereoPanCheck = new QCheckBox("Link PAN", m_spatialGroup);
    spatialLayout->addWidget(m_spatialModeCombo, 0, 0, 1, 2);
    spatialLayout->addWidget(m_linkStereoPanCheck, 0, 2);
    spatialLayout->addWidget(m_azimuthSpin, 1, 0);
    spatialLayout->addWidget(m_elevationSpin, 1, 1);
    spatialLayout->addWidget(m_distanceSpin, 1, 2);
    spatialLayout->addWidget(m_spreadSpin, 2, 0);
    spatialLayout->addWidget(m_lfeSendSpin, 2, 1);
    const auto emitSpatial = [this]() { emitSpatialPannerEdited(); };
    connect(m_spatialModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [emitSpatial](int) { emitSpatial(); });
    connect(m_linkStereoPanCheck, &QCheckBox::toggled,
            this, [emitSpatial](bool) { emitSpatial(); });
    for (QDoubleSpinBox* spin : {m_azimuthSpin, m_elevationSpin, m_distanceSpin,
                                 m_spreadSpin, m_lfeSendSpin})
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [emitSpatial](double) { emitSpatial(); });
    mainLayout->addWidget(m_spatialGroup);

    // MIDI layer rack: one piano roll can fan out to multiple instruments.
    m_instrumentGroup = new QGroupBox("LAYERS", this);
    m_instrumentGroup->setStyleSheet(groupStyle);
    m_instrumentGroup->setMinimumWidth(500);
    m_instrumentGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QVBoxLayout* instrumentGroupLayout = new QVBoxLayout(m_instrumentGroup);
    instrumentGroupLayout->setContentsMargins(4, 4, 4, 4);
    QScrollArea* layerScroll = new QScrollArea(m_instrumentGroup);
    layerScroll->setWidgetResizable(true);
    layerScroll->setFrameShape(QFrame::NoFrame);
    layerScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layerScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QWidget* layerContent = new QWidget(layerScroll);
    m_instrumentLayout = new QVBoxLayout(layerContent);
    m_instrumentLayout->setContentsMargins(8, 8, 8, 8);
    m_instrumentLayout->setSpacing(4);
    layerScroll->setWidget(layerContent);
    instrumentGroupLayout->addWidget(layerScroll);

    mainLayout->addWidget(m_instrumentGroup);

    // FX chain grows with available width like Ableton's device chain.
    m_fxGroup = new QGroupBox("AUDIO FX", this);
    m_fxGroup->setStyleSheet(groupStyle);
    m_fxGroup->setMinimumWidth(180);
    m_fxGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_fxLayout = new QVBoxLayout(m_fxGroup);
    m_fxLayout->setContentsMargins(8, 8, 8, 8);
    m_fxLayout->setSpacing(4);

    mainLayout->addWidget(m_fxGroup, 1);

    m_parameterGroup = new QGroupBox("PLUGIN PARAMETERS", this);
    m_parameterGroup->setStyleSheet(groupStyle);
    m_parameterGroup->setMinimumWidth(520);
    m_parameterGroup->setMaximumWidth(720);
    m_parameterGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    QVBoxLayout* parameterGroupLayout = new QVBoxLayout(m_parameterGroup);
    parameterGroupLayout->setContentsMargins(4, 4, 4, 4);
    QScrollArea* parameterScroll = new QScrollArea(m_parameterGroup);
    parameterScroll->setWidgetResizable(true);
    parameterScroll->setFrameShape(QFrame::NoFrame);
    parameterScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    parameterScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QWidget* parameterContent = new QWidget(parameterScroll);
    m_parameterLayout = new QVBoxLayout(parameterContent);
    m_parameterLayout->setContentsMargins(5, 5, 5, 5);
    m_parameterLayout->setSpacing(3);
    parameterScroll->setWidget(parameterContent);
    parameterGroupLayout->addWidget(parameterScroll);
    m_parameterGroup->hide();
    mainLayout->addWidget(m_parameterGroup);

    // Output routing.
    m_outputGroup = new QGroupBox("OUTPUT", this);
    m_outputGroup->setStyleSheet(groupStyle);
    m_outputGroup->setMinimumWidth(320);
    m_outputGroup->setMaximumWidth(560);
    m_outputGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    QGridLayout* outLayout = new QGridLayout(m_outputGroup);
    outLayout->setContentsMargins(8, 8, 8, 8);
    outLayout->setSpacing(4);
    m_outputLabel = new QLabel("Stereo Out", m_outputGroup);
    m_outputLabel->setStyleSheet("color: #ccc; background-color: #333; padding: 6px 8px; "
                                 "border: 1px solid #161616; border-radius: 3px;");
    outLayout->addWidget(m_outputLabel, 0, 0, 1, 10);
    const QString busSliderStyle =
        "QSlider::groove:horizontal { height:4px; background:#171717; border:1px solid #111; } "
        "QSlider::sub-page:horizontal { background:#788b72; } "
        "QSlider::handle:horizontal { width:7px; margin:-4px 0; background:#c7cec3; border:1px solid #333; }";
    for (int bus = 0; bus < kReturnBusCount; ++bus) {
        QLabel* busName = new QLabel(QString(returnBusLetter(bus)), m_outputGroup);
        busName->setStyleSheet("color:#ffb05a; font-weight:bold; border:none;");
        m_sendSliders[bus] = new QSlider(Qt::Horizontal, m_outputGroup);
        m_sendSliders[bus]->setRange(-700, 0);
        m_sendSliders[bus]->setStyleSheet(busSliderStyle);
        m_sendSliders[bus]->setToolTip(QString("Send %1: -inf dB").arg(returnBusLetter(bus)));
        m_returnVolumeSliders[bus] = new QSlider(Qt::Horizontal, m_outputGroup);
        m_returnVolumeSliders[bus]->setRange(-700, 60);
        m_returnVolumeSliders[bus]->setStyleSheet(busSliderStyle);
        m_returnPanSliders[bus] = new QSlider(Qt::Horizontal, m_outputGroup);
        m_returnPanSliders[bus]->setRange(-100, 100);
        m_returnPanSliders[bus]->setStyleSheet(busSliderStyle);
        m_returnSeparationSliders[bus] = new QDial(m_outputGroup);
        m_returnSeparationSliders[bus]->setRange(-100, 100);
        m_returnSeparationSliders[bus]->setNotchesVisible(false);
        m_returnSeparationSliders[bus]->setWrapping(false);
        m_returnSeparationSliders[bus]->setFixedSize(24, 24);
        m_returnMuteButtons[bus] = new QPushButton("M", m_outputGroup);
        m_returnMuteButtons[bus]->setCheckable(true);
        m_returnMuteButtons[bus]->setFixedSize(22, 18);
        m_returnMuteButtons[bus]->setToolTip(QString("Mute Return %1").arg(returnBusLetter(bus)));
        m_returnMuteButtons[bus]->setStyleSheet(
            "QPushButton { background:#343434; color:#aaa; border:1px solid #151515; font-weight:bold; } "
            "QPushButton:checked { background:#ffb13b; color:#141414; }");
        m_returnSoloButtons[bus] = new QPushButton("S", m_outputGroup);
        m_returnSoloButtons[bus]->setCheckable(true);
        m_returnSoloButtons[bus]->setFixedSize(22, 18);
        m_returnSoloButtons[bus]->setToolTip(QString("Solo Return %1 (solo-in-place)").arg(returnBusLetter(bus)));
        m_returnSoloButtons[bus]->setStyleSheet(
            "QPushButton { background:#343434; color:#aaa; border:1px solid #151515; font-weight:bold; } "
            "QPushButton:checked { background:#4a90d9; color:#141414; }");
        m_returnPreFaderButtons[bus] = new QPushButton("POST", m_outputGroup);
        m_returnPreFaderButtons[bus]->setCheckable(true);
        m_returnPreFaderButtons[bus]->setFixedSize(38, 18);
        m_returnPreFaderButtons[bus]->setToolTip(
            QString("Return %1 send tap: post-fader").arg(returnBusLetter(bus)));
		m_returnMeterBars[bus] = new QProgressBar(m_outputGroup);
		m_returnMeterBars[bus]->setOrientation(Qt::Vertical);
		m_returnMeterBars[bus]->setRange(-700, 60);
		m_returnMeterBars[bus]->setValue(-700);
		m_returnMeterBars[bus]->setTextVisible(false);
		m_returnMeterBars[bus]->setFixedSize(9, 18);
		m_returnMeterBars[bus]->setAccessibleName(
			QString("Return %1 true-peak meter").arg(returnBusLetter(bus)));
		m_returnMeterBars[bus]->setStyleSheet(
			"QProgressBar { background:#171717; border:1px solid #0d0d0d; } "
			"QProgressBar::chunk { background:#69bd78; }");
		m_returnFxButtons[bus] = new QPushButton("FX", m_outputGroup);
		m_returnFxButtons[bus]->setFixedSize(24, 18);
		m_returnFxButtons[bus]->setToolTip(
			QString("Open Return %1 insert rack").arg(returnBusLetter(bus)));
		m_returnFxButtons[bus]->setStyleSheet(
			"QPushButton { background:#343434; color:#d4a76d; border:1px solid #151515; font-weight:bold; } "
			"QPushButton:hover { border-color:#ff9020; color:#ffb05a; }");
        outLayout->addWidget(busName, bus + 1, 0);
        outLayout->addWidget(m_sendSliders[bus], bus + 1, 1);
        outLayout->addWidget(m_returnVolumeSliders[bus], bus + 1, 2);
        outLayout->addWidget(m_returnPanSliders[bus], bus + 1, 3);
        outLayout->addWidget(m_returnSeparationSliders[bus], bus + 1, 4);
        outLayout->addWidget(m_returnMuteButtons[bus], bus + 1, 5);
        outLayout->addWidget(m_returnSoloButtons[bus], bus + 1, 6);
        outLayout->addWidget(m_returnPreFaderButtons[bus], bus + 1, 7);
		outLayout->addWidget(m_returnMeterBars[bus], bus + 1, 8);
		outLayout->addWidget(m_returnFxButtons[bus], bus + 1, 9);
		connect(m_returnFxButtons[bus], &QPushButton::clicked, this,
			[this, bus]() { showReturnFxBus(bus); });
        connect(m_sendSliders[bus], &QSlider::valueChanged, this, [this, bus](int value) {
            if (m_currentTrack) emit trackSendEdited(m_currentTrack, bus, faderToGain(value));
        });
        auto emitReturn = [this, bus]() {
            emit returnBusEdited(bus, faderToGain(m_returnVolumeSliders[bus]->value()),
                static_cast<float>(m_returnPanSliders[bus]->value()) / 100.0f,
                static_cast<float>(m_returnSeparationSliders[bus]->value()) / 100.0f,
                m_returnMuteButtons[bus]->isChecked(), m_returnSoloButtons[bus]->isChecked(),
                m_returnPreFaderButtons[bus]->isChecked());
        };
        connect(m_returnVolumeSliders[bus], &QSlider::valueChanged, this, [emitReturn](int) { emitReturn(); });
        connect(m_returnPanSliders[bus], &QSlider::valueChanged, this, [emitReturn](int) { emitReturn(); });
        connect(m_returnSeparationSliders[bus], &QDial::valueChanged,
            this, [emitReturn](int) { emitReturn(); });
        connect(m_returnMuteButtons[bus], &QPushButton::toggled, this, [emitReturn](bool) { emitReturn(); });
        connect(m_returnSoloButtons[bus], &QPushButton::toggled, this, [emitReturn](bool) { emitReturn(); });
        connect(m_returnPreFaderButtons[bus], &QPushButton::toggled, this,
            [this, bus, emitReturn](bool preFader) {
                m_returnPreFaderButtons[bus]->setText(preFader ? "PRE" : "POST");
                emitReturn();
            });
    }
    QLabel* legend = new QLabel("SEND LEVEL        RETURN VOL       PAN       WIDTH", m_outputGroup);
    legend->setStyleSheet("color:#777; font-size:8px; border:none;");
    outLayout->addWidget(legend, 3, 1, 1, 3);
    QLabel* masterName = new QLabel("MASTER", m_outputGroup);
    masterName->setStyleSheet("color:#d9ddd7; font-weight:bold; border:none;");
    m_masterVolumeSlider = new QSlider(Qt::Horizontal, m_outputGroup);
    m_masterVolumeSlider->setRange(-700, 60);
    m_masterVolumeSlider->setStyleSheet(busSliderStyle);
    m_masterPanSlider = new QSlider(Qt::Horizontal, m_outputGroup);
    m_masterPanSlider->setRange(-100, 100);
    m_masterPanSlider->setStyleSheet(busSliderStyle);
    m_masterSeparationSlider = new QDial(m_outputGroup);
    m_masterSeparationSlider->setRange(-100, 100);
    m_masterSeparationSlider->setNotchesVisible(false);
    m_masterSeparationSlider->setWrapping(false);
    m_masterSeparationSlider->setFixedSize(26, 26);
    m_masterMuteButton = new QPushButton("M", m_outputGroup);
    m_masterMuteButton->setCheckable(true);
    m_masterMuteButton->setFixedSize(22, 18);
    m_masterMuteButton->setStyleSheet(
        "QPushButton { background:#343434; color:#aaa; border:1px solid #151515; font-weight:bold; } "
        "QPushButton:checked { background:#ffb13b; color:#141414; }");
    outLayout->addWidget(masterName, 4, 0);
    outLayout->addWidget(m_masterVolumeSlider, 4, 1);
    outLayout->addWidget(m_masterPanSlider, 4, 2);
    outLayout->addWidget(m_masterSeparationSlider, 4, 3, 1, 2);
    outLayout->addWidget(m_masterMuteButton, 4, 5);
    auto emitMaster = [this]() {
        emit masterBusEdited(faderToGain(m_masterVolumeSlider->value()),
            static_cast<float>(m_masterPanSlider->value()) / 100.0f,
            static_cast<float>(m_masterSeparationSlider->value()) / 100.0f,
            m_masterMuteButton->isChecked());
    };
    connect(m_masterVolumeSlider, &QSlider::valueChanged, this, [emitMaster](int) { emitMaster(); });
    connect(m_masterPanSlider, &QSlider::valueChanged, this, [emitMaster](int) { emitMaster(); });
    connect(m_masterSeparationSlider, &QDial::valueChanged,
        this, [emitMaster](int) { emitMaster(); });
    connect(m_masterMuteButton, &QPushButton::toggled, this, [emitMaster](bool) { emitMaster(); });
    mainLayout->addWidget(m_outputGroup);

    updateUi();
}

void InspectorWidget::setReturnMeters(
	const std::array<VeraCoreAudioMeterState, kReturnBusCount>& meters)
{
	for (int bus = 0; bus < kReturnBusCount; ++bus) {
		QProgressBar* meter = m_returnMeterBars[bus];
		if (!meter) continue;
		const float peak = std::max(meters[bus].peakLeft, meters[bus].peakRight);
		const int fader = gainToFader(peak);
		meter->setValue(fader);
		meter->setToolTip(QString("Return %1 true peak: %2")
			.arg(returnBusLetter(bus)).arg(faderText(fader)));
		const bool overload = peak >= 1.0f;
		if (meter->property("overload").toBool() != overload) {
			meter->setProperty("overload", overload);
			meter->setStyleSheet(overload
				? "QProgressBar { background:#171717; border:1px solid #0d0d0d; } "
				  "QProgressBar::chunk { background:#e35d55; }"
				: "QProgressBar { background:#171717; border:1px solid #0d0d0d; } "
				  "QProgressBar::chunk { background:#69bd78; }");
		}
	}
}

void InspectorWidget::setTrack(Track* track) {
    m_currentReturnFxBus = -1;
    m_currentTrack = track;
    m_showingMidiClip = false;
    m_showingAudioClip = false;
    m_audioClipSourcePath.clear();
    clearPluginParameters();
    updateUi();
}

void InspectorWidget::refreshTrackDevices()
{
    if (!m_currentTrack || m_showingMidiClip || m_showingAudioClip) return;
    rebuildInstrumentRack();
    rebuildAudioFxRack();
}

void InspectorWidget::setReturnBuses(const std::array<ReturnBusState, kReturnBusCount>& buses)
{
    bool rackStructureChanged = false;
    if (m_currentReturnFxBus >= 0 && m_currentReturnFxBus < kReturnBusCount) {
        const auto& before = m_returnBusView[
            static_cast<std::size_t>(m_currentReturnFxBus)].audioFxSlots;
        const auto& after = buses[
            static_cast<std::size_t>(m_currentReturnFxBus)].audioFxSlots;
        rackStructureChanged = before.size() != after.size();
        for (std::size_t index = 0; !rackStructureChanged && index < before.size(); ++index) {
            rackStructureChanged = before[index].name != after[index].name ||
                before[index].pluginPath != after[index].pluginPath ||
                before[index].pluginType != after[index].pluginType ||
                before[index].pluginUid != after[index].pluginUid ||
                before[index].enabled != after[index].enabled;
        }
    }
    m_returnBusView = buses;
    refreshMixerControls();
	if (rackStructureChanged) rebuildAudioFxRack();
}

void InspectorWidget::showReturnFxBus(int returnBus)
{
	if (returnBus < 0 || returnBus >= kReturnBusCount) return;
	m_currentReturnFxBus = returnBus;
	m_showingMidiClip = false;
	m_showingAudioClip = false;
	clearPluginParameters();
	updateUi();
}

void InspectorWidget::setMasterBus(const MasterBusState& master)
{
    m_masterBusView = master;
    refreshMixerControls();
}

void InspectorWidget::setPluginHostStates(std::vector<PluginHostStatus> layerStates,
                                          std::vector<PluginHostStatus> audioFxStates)
{
    if (m_layerHostStates == layerStates && m_audioFxHostStates == audioFxStates) return;
    m_layerHostStates = std::move(layerStates);
    m_audioFxHostStates = std::move(audioFxStates);
    if (m_currentTrack && !m_showingMidiClip && !m_showingAudioClip) {
        rebuildInstrumentRack();
        rebuildAudioFxRack();
    }
}

void InspectorWidget::setReturnPluginHostStates(
    std::array<std::vector<PluginHostStatus>, kReturnBusCount> audioFxStates)
{
    if (m_returnAudioFxHostStates == audioFxStates) return;
    m_returnAudioFxHostStates = std::move(audioFxStates);
    if (m_currentReturnFxBus >= 0 && m_currentReturnFxBus < kReturnBusCount)
        rebuildAudioFxRack();
}

void InspectorWidget::setPluginParameters(
    const QString& deviceName, bool audioFx, int slotIndex,
    std::vector<PluginParameterInfo> parameters, bool automationAvailable)
{
    m_parameterDeviceName = deviceName.trimmed();
    m_parameterAudioFx = audioFx;
    m_parameterSlotIndex = slotIndex;
    m_pluginParameters = std::move(parameters);
    m_parameterAutomationAvailable = automationAvailable;
    rebuildPluginParameterRack();
    if (m_parameterGroup) m_parameterGroup->show();
}

void InspectorWidget::clearPluginParameters()
{
    m_parameterDeviceName.clear();
    m_parameterSlotIndex = -1;
    m_pluginParameters.clear();
    m_parameterAutomationAvailable = true;
    if (m_parameterGroup) m_parameterGroup->hide();
}

void InspectorWidget::setActiveAutomationTarget(bool audioFx, int slotIndex,
                                                uint32_t parameterId)
{
    m_hasActiveAutomationTarget = true;
    m_activeAutomationAudioFx = audioFx;
    m_activeAutomationSlotIndex = slotIndex;
    m_activeAutomationParameterId = parameterId;
    rebuildPluginParameterRack();
}

void InspectorWidget::clearActiveAutomationTarget()
{
    if (!m_hasActiveAutomationTarget) return;
    m_hasActiveAutomationTarget = false;
    m_activeAutomationSlotIndex = -1;
    m_activeAutomationParameterId = 0;
    rebuildPluginParameterRack();
}

void InspectorWidget::rebuildPluginParameterRack()
{
    if (!m_parameterLayout || !m_parameterGroup) return;
    while (QLayoutItem* item = m_parameterLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
    m_parameterGroup->setTitle(m_parameterDeviceName.isEmpty()
        ? QStringLiteral("PLUGIN PARAMETERS")
        : QString("PARAMETERS  ·  %1").arg(m_parameterDeviceName));
    QFrame* automationBar = new QFrame(m_parameterGroup);
    automationBar->setStyleSheet("QFrame { background:#242424; border:1px solid #171717; border-radius:3px; }");
    QHBoxLayout* automationLayout = new QHBoxLayout(automationBar);
    automationLayout->setContentsMargins(6, 3, 6, 3);
    QLabel* automationTitle = new QLabel("AUTOMATION", automationBar);
    automationTitle->setStyleSheet("color:#b8b8b8; font-size:8px; font-weight:bold; border:0;");
    automationLayout->addWidget(automationTitle);
    automationLayout->addStretch();
    QComboBox* mode = new QComboBox(automationBar);
    mode->addItems({"Read", "Write", "Touch", "Latch"});
    mode->setCurrentIndex(static_cast<int>(m_automationWriteMode));
    mode->setFixedWidth(82);
    mode->setToolTip("Automation write mode");
    mode->setEnabled(m_parameterAutomationAvailable);
    automationLayout->addWidget(mode);
    connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_automationWriteMode = static_cast<AutomationWriteMode>(qBound(0, index, 3));
        emit pluginAutomationModeChanged(m_automationWriteMode);
    });
    m_parameterLayout->addWidget(automationBar);
    int visibleCount = 0;
    for (const PluginParameterInfo& parameter : m_pluginParameters) {
        if (parameter.hidden) continue;
        ++visibleCount;
        QFrame* row = new QFrame(m_parameterGroup);
        row->setStyleSheet("QFrame { background:#303030; border:1px solid #171717; border-radius:3px; } "
                           "QLabel { background:transparent; border:0; }");
        QHBoxLayout* layout = new QHBoxLayout(row);
        layout->setContentsMargins(5, 2, 5, 2);
        layout->setSpacing(5);
        QString qualifiedName = parameter.name;
        if (!parameter.module.isEmpty()) qualifiedName = parameter.module + " / " + qualifiedName;
        QLabel* name = new QLabel(qualifiedName, row);
        name->setMinimumWidth(155);
        name->setMaximumWidth(220);
        name->setToolTip(qualifiedName);
        name->setStyleSheet(parameter.readOnly ? "color:#777; font-size:9px;" : "color:#d8d8d8; font-size:9px;");
        layout->addWidget(name);

        QSlider* slider = new QSlider(Qt::Horizontal, row);
        const int sliderMaximum = parameter.stepCount > 0 && parameter.stepCount <= 10000
            ? parameter.stepCount : 10000;
        slider->setRange(0, sliderMaximum);
        slider->setValue(qRound(std::clamp(parameter.normalizedValue, 0.0, 1.0) * sliderMaximum));
        slider->setEnabled(!parameter.readOnly);
        slider->setToolTip(parameter.automatable
            ? "Automatable plug-in parameter" : "Plug-in parameter");
        layout->addWidget(slider, 1);

        QString display = parameter.displayValue;
        if (display.isEmpty()) display = QString::number(parameter.normalizedValue * 100.0, 'f', 1) + "%";
        if (!parameter.unit.isEmpty() && !display.contains(parameter.unit, Qt::CaseInsensitive))
            display += " " + parameter.unit;
        QLabel* value = new QLabel(display, row);
        value->setFixedWidth(86);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setStyleSheet("color:#9eaa9a; font-family:Consolas; font-size:9px;");
        layout->addWidget(value);

        if (parameter.automatable && m_parameterAutomationAvailable) {
            QPushButton* automation = new QPushButton("A", row);
            automation->setFixedSize(20, 18);
            automation->setCheckable(true);
            automation->setChecked(m_hasActiveAutomationTarget &&
                m_activeAutomationAudioFx == m_parameterAudioFx &&
                m_activeAutomationSlotIndex == m_parameterSlotIndex &&
                m_activeAutomationParameterId == parameter.stableId);
            automation->setToolTip("Show this parameter as Arrangement automation");
            automation->setStyleSheet(
                "QPushButton { color:#ff9d3d; background:#292929; border:1px solid #171717; font-size:8px; font-weight:bold; } "
                "QPushButton:hover { color:#fff; border-color:#ff9020; } "
                "QPushButton:checked { color:#1b1b1b; background:#ff9d3d; border-color:#ffc078; }");
            layout->addWidget(automation);
            const bool automationAudioFx = m_parameterAudioFx;
            const int automationSlotIndex = m_parameterSlotIndex;
            const uint32_t automationParameterId = parameter.stableId;
            connect(automation, &QPushButton::clicked, this,
                    [this, automationAudioFx, automationSlotIndex,
                     automationParameterId, qualifiedName]() {
                emit pluginAutomationTargetRequested(
                    automationAudioFx, automationSlotIndex,
                    automationParameterId, qualifiedName);
            });
        }
        QPushButton* reset = new QPushButton("R", row);
        reset->setFixedSize(20, 18);
        reset->setEnabled(!parameter.readOnly);
        reset->setToolTip("Reset to plug-in default");
        reset->setStyleSheet("QPushButton { color:#999; background:#292929; border:1px solid #171717; font-size:8px; } "
                             "QPushButton:hover:enabled { color:#fff; border-color:#ff9020; }");
        layout->addWidget(reset);

        const bool audioFx = m_parameterAudioFx;
        const int slotIndex = m_parameterSlotIndex;
        const uint32_t parameterId = parameter.stableId;
        connect(slider, &QSlider::sliderPressed, this,
                [this, audioFx, slotIndex, parameterId]() {
            emit pluginParameterGestureBegin(audioFx, slotIndex, parameterId);
        });
        connect(slider, &QSlider::valueChanged, this,
                [this, value, sliderMaximum, audioFx, slotIndex, parameterId](int sliderValue) {
            const double normalized = static_cast<double>(sliderValue) /
                static_cast<double>(std::max(1, sliderMaximum));
            value->setText(QString::number(normalized * 100.0, 'f', 1) + "%");
            emit pluginParameterChanged(audioFx, slotIndex, parameterId, normalized);
        });
        connect(slider, &QSlider::sliderReleased, this,
                [this, audioFx, slotIndex, parameterId]() {
            emit pluginParameterGestureEnd(audioFx, slotIndex, parameterId);
        });
        connect(reset, &QPushButton::clicked, this,
                [this, slider, sliderMaximum, parameter, audioFx, slotIndex, parameterId]() {
            emit pluginParameterGestureBegin(audioFx, slotIndex, parameterId);
            slider->setValue(qRound(std::clamp(
                parameter.defaultNormalizedValue, 0.0, 1.0) * sliderMaximum));
            emit pluginParameterGestureEnd(audioFx, slotIndex, parameterId);
        });
        m_parameterLayout->addWidget(row);
    }
    if (visibleCount == 0) {
        QLabel* empty = new QLabel("No exposed parameters", m_parameterGroup);
        empty->setStyleSheet("color:#777; background:transparent; border:0; padding:8px;");
        m_parameterLayout->addWidget(empty);
    }
    m_parameterLayout->addStretch();
}

void InspectorWidget::setAraStatus(bool available, bool dirty,
                                   const QString& operation, float progress)
{
    if (!m_araStatusLabel || !m_araProgressBar) return;
    m_araStatusLabel->setVisible(available);
    if (!available) {
        m_araProgressBar->hide();
        return;
    }
    const bool busy = !operation.isEmpty();
    const float boundedProgress = qBound(0.0f, progress, 1.0f);
    m_araStatusLabel->setText(busy
        ? QString("ARA  %1  %2%").arg(operation.toUpper()).arg(qRound(boundedProgress * 100.0f))
        : (dirty ? QStringLiteral("ARA  MODIFIED") : QStringLiteral("ARA  READY")));
    m_araStatusLabel->setStyleSheet(dirty && !busy
        ? "color:#ffb05a; background:#2d261d; border:1px solid #5a4327; border-radius:2px; padding:2px 4px; font-size:8px; font-weight:bold;"
        : "color:#98ad91; background:#202620; border:1px solid #394439; border-radius:2px; padding:2px 4px; font-size:8px; font-weight:bold;");
    m_araProgressBar->setVisible(busy);
    if (busy) m_araProgressBar->setValue(qRound(boundedProgress * 1000.0f));
}

void InspectorWidget::setAraProcessingAlgorithms(
    const QString& pluginName, const QStringList& algorithmNames, int currentIndex)
{
    if (!m_araAlgorithmCombo) return;
    const QSignalBlocker blocker(m_araAlgorithmCombo);
    m_araAlgorithmCombo->clear();
    for (const QString& name : algorithmNames)
        m_araAlgorithmCombo->addItem(QString("ARA | %1").arg(name));
    const bool available = m_showingAudioClip && !algorithmNames.isEmpty();
    if (currentIndex >= 0 && currentIndex < algorithmNames.size())
        m_araAlgorithmCombo->setCurrentIndex(currentIndex);
    else
        m_araAlgorithmCombo->setCurrentIndex(-1);
    m_araAlgorithmCombo->setToolTip(available
        ? QString("%1 processing algorithm. Changing it requests source reanalysis.")
              .arg(pluginName.isEmpty() ? QStringLiteral("ARA") : pluginName)
        : QString());
    m_araAlgorithmCombo->setVisible(available);
}

void InspectorWidget::emitMixerControlsEdited()
{
    if (!m_currentTrack) return;
    emit mixerControlsEdited(m_currentTrack,
        faderToGain(m_volumeSlider->value()),
        static_cast<float>(m_panSlider->value()) / 100.0f,
        static_cast<float>(m_stereoSeparationSlider->value()) / 100.0f,
        m_muteBtn->isChecked(), m_soloBtn->isChecked(),
        m_armBtn->isChecked(), m_monitorBtn->isChecked());
}

void InspectorWidget::emitSpatialPannerEdited()
{
    if (!m_currentTrack || !m_spatialModeCombo) return;
    VeraCoreSpatialPannerState state;
    state.mode = static_cast<VeraCoreSpatialRenderMode>(
        std::clamp(m_spatialModeCombo->currentData().toInt(), 0, 2));
    state.azimuthDegrees = static_cast<float>(m_azimuthSpin->value());
    state.elevationDegrees = static_cast<float>(m_elevationSpin->value());
    state.distance = static_cast<float>(m_distanceSpin->value());
    state.spread = static_cast<float>(m_spreadSpin->value());
    state.lfeSend = static_cast<float>(m_lfeSendSpin->value());
    state.linkStereoPan = m_linkStereoPanCheck->isChecked();
    emit spatialPannerEdited(m_currentTrack, VeraCoreNormalizeSpatialPanner(state));
}

void InspectorWidget::refreshMixerControls()
{
    const QSignalBlocker masterVolumeBlock(m_masterVolumeSlider);
    const QSignalBlocker masterPanBlock(m_masterPanSlider);
    const QSignalBlocker masterSeparationBlock(m_masterSeparationSlider);
    const QSignalBlocker masterMuteBlock(m_masterMuteButton);
    m_masterVolumeSlider->setValue(gainToFader(m_masterBusView.volume));
    m_masterPanSlider->setValue(qRound(m_masterBusView.pan * 100.0f));
    m_masterSeparationSlider->setValue(qRound(m_masterBusView.stereoSeparation * 100.0f));
    m_masterMuteButton->setChecked(m_masterBusView.muted);
    m_masterVolumeSlider->setToolTip(QString("Master volume: %1")
        .arg(faderText(m_masterVolumeSlider->value())));
    m_masterPanSlider->setToolTip(QString("Master pan: %1").arg(m_masterPanSlider->value()));
    m_masterSeparationSlider->setToolTip(QString("Master stereo separation: %1%").arg(
        m_masterSeparationSlider->value()));
    m_masterMuteButton->setToolTip("Mute Master output");
    const bool enabled = m_currentTrack != nullptr;
    m_spatialGroup->setEnabled(enabled);
    QWidget* controls[] = {m_muteBtn, m_soloBtn, m_armBtn,
                           m_monitorBtn, m_volumeSlider, m_panSlider,
                           m_stereoSeparationSlider};
    for (QWidget* control : controls) {
        control->setEnabled(enabled);
    }
    if (!enabled) return;
    const QSignalBlocker muteBlock(m_muteBtn);
    const QSignalBlocker soloBlock(m_soloBtn);
    const QSignalBlocker armBlock(m_armBtn);
    const QSignalBlocker monitorBlock(m_monitorBtn);
    const QSignalBlocker volumeBlock(m_volumeSlider);
    const QSignalBlocker panBlock(m_panSlider);
    const QSignalBlocker separationBlock(m_stereoSeparationSlider);
    const QSignalBlocker spatialModeBlock(m_spatialModeCombo);
    const QSignalBlocker azimuthBlock(m_azimuthSpin);
    const QSignalBlocker elevationBlock(m_elevationSpin);
    const QSignalBlocker distanceBlock(m_distanceSpin);
    const QSignalBlocker spreadBlock(m_spreadSpin);
    const QSignalBlocker lfeBlock(m_lfeSendSpin);
    const QSignalBlocker linkBlock(m_linkStereoPanCheck);
    m_muteBtn->setChecked(m_currentTrack->IsMuted());
    m_soloBtn->setChecked(m_currentTrack->IsSoloed());
    m_armBtn->setChecked(m_currentTrack->IsArmed());
    m_monitorBtn->setChecked(m_currentTrack->IsMonitoring());
    m_volumeSlider->setValue(gainToFader(m_currentTrack->GetVolume()));
    m_panSlider->setValue(qRound(m_currentTrack->GetPan() * 100.0f));
    m_stereoSeparationSlider->setValue(
        qRound(m_currentTrack->GetStereoSeparation() * 100.0f));
    const VeraCoreSpatialPannerState& spatial = m_currentTrack->GetSpatialPanner();
    m_spatialModeCombo->setCurrentIndex(std::max(0, m_spatialModeCombo->findData(
        static_cast<int>(spatial.mode))));
    m_azimuthSpin->setValue(spatial.azimuthDegrees);
    m_elevationSpin->setValue(spatial.elevationDegrees);
    m_distanceSpin->setValue(spatial.distance);
    m_spreadSpin->setValue(spatial.spread);
    m_lfeSendSpin->setValue(spatial.lfeSend);
    m_linkStereoPanCheck->setChecked(spatial.linkStereoPan);
    m_volumeSlider->setToolTip(QString("Volume: %1").arg(faderText(m_volumeSlider->value())));
    m_panSlider->setToolTip(QString("Pan: %1").arg(m_panSlider->value()));
    m_stereoSeparationSlider->setToolTip(QString("Stereo separation: %1%").arg(
        m_stereoSeparationSlider->value()));
    for (int bus = 0; bus < kReturnBusCount; ++bus) {
        const QSignalBlocker sendBlock(m_sendSliders[bus]);
        const QSignalBlocker returnVolumeBlock(m_returnVolumeSliders[bus]);
        const QSignalBlocker returnPanBlock(m_returnPanSliders[bus]);
        const QSignalBlocker returnSeparationBlock(m_returnSeparationSliders[bus]);
        const QSignalBlocker returnMuteBlock(m_returnMuteButtons[bus]);
        const QSignalBlocker returnSoloBlock(m_returnSoloButtons[bus]);
        const QSignalBlocker returnPreFaderBlock(m_returnPreFaderButtons[bus]);
        m_sendSliders[bus]->setValue(gainToFader(m_currentTrack->GetSendLevel(bus)));
        m_returnVolumeSliders[bus]->setValue(gainToFader(m_returnBusView[bus].volume));
        m_returnPanSliders[bus]->setValue(qRound(m_returnBusView[bus].pan * 100.0f));
        m_returnSeparationSliders[bus]->setValue(
            qRound(m_returnBusView[bus].stereoSeparation * 100.0f));
        m_returnMuteButtons[bus]->setChecked(m_returnBusView[bus].muted);
        m_returnSoloButtons[bus]->setChecked(m_returnBusView[bus].soloed);
        m_returnPreFaderButtons[bus]->setChecked(m_returnBusView[bus].preFader);
        m_returnPreFaderButtons[bus]->setText(m_returnBusView[bus].preFader ? "PRE" : "POST");
        m_sendSliders[bus]->setToolTip(QString("Send %1: %2 (%3-fader)")
            .arg(returnBusLetter(bus))
            .arg(faderText(m_sendSliders[bus]->value()))
            .arg(m_returnBusView[bus].preFader ? "pre" : "post"));
        m_returnVolumeSliders[bus]->setToolTip(QString("%1 volume: %2")
            .arg(m_returnBusView[bus].name).arg(faderText(m_returnVolumeSliders[bus]->value())));
        m_returnPanSliders[bus]->setToolTip(QString("%1 pan: %2")
            .arg(m_returnBusView[bus].name).arg(m_returnPanSliders[bus]->value()));
        m_returnSeparationSliders[bus]->setToolTip(QString("%1 stereo separation: %2%")
            .arg(m_returnBusView[bus].name).arg(m_returnSeparationSliders[bus]->value()));
        m_returnPreFaderButtons[bus]->setToolTip(QString("%1 send tap: %2-fader")
            .arg(m_returnBusView[bus].name)
            .arg(m_returnBusView[bus].preFader ? "pre" : "post"));
    }
}

void InspectorWidget::setTimeSignature(int numerator, int denominator)
{
    const int boundedDenominator = qBound(1, denominator, 64);
    const int safeDenominator = (boundedDenominator & (boundedDenominator - 1)) == 0
        ? boundedDenominator : 4;
    m_timeSignatureNumerator = qBound(1, numerator, 32);
    m_timeSignatureDenominator = safeDenominator;
    m_notatedBeatWidth = qBound(0.25, 4.0 / static_cast<double>(safeDenominator), 4.0);
    const double beatsPerBar = static_cast<double>(qBound(1, numerator, 32)) * m_notatedBeatWidth;
    m_beatsPerBar = qBound(0.25, beatsPerBar, 64.0);
    updateClipPositionLabels();
}

void InspectorWidget::updateClipPositionLabels()
{
    if (!m_showingMidiClip && !m_showingAudioClip) {
        return;
    }
    const double safeBeatsPerBar = qBound(0.25, m_beatsPerBar, 64.0);
    const double safeStartBeat = std::isfinite(m_currentClipStartBeat) ? std::max(0.0, m_currentClipStartBeat) : 0.0;
    const int startMeasure = static_cast<int>(std::floor(safeStartBeat / safeBeatsPerBar)) + 1;
    const double positionInMeasure = std::fmod(safeStartBeat, safeBeatsPerBar) / qMax(0.25, m_notatedBeatWidth);
    const int startBeatInMeasure = static_cast<int>(std::floor(positionInMeasure)) + 1;
    m_clipStartLabel->setText(QString("Start: %1.%2").arg(startMeasure).arg(startBeatInMeasure));
    const double safeLength = std::isfinite(m_currentClipLengthBeats) ? std::max(0.0, m_currentClipLengthBeats) : 0.0;
    m_clipLengthLabel->setText(QString("Length: %1 beats / %2 bars")
        .arg(safeLength, 0, 'g', 4)
        .arg(safeLength / safeBeatsPerBar, 0, 'g', 4));
}

void InspectorWidget::setMidiClip(const QString& name, double startBeat, double lengthBeats) {
    clearPluginParameters();
    m_showingMidiClip = true;
    m_showingAudioClip = false;
    m_audioClipSourcePath.clear();
    m_currentClipStartBeat = startBeat;
    m_currentClipLengthBeats = lengthBeats;
    m_clipGroup->setTitle("MIDI CLIP");
    m_trackNameLabel->setText(name);

    updateClipPositionLabels();
    m_clipSyncLabel->setText("MIDI: color groups map to channels");
    m_syncModeCombo->hide();
    m_sourceTempoSpin->hide();
    m_pitchShiftSpin->hide();
    m_targetBarsSpin->hide();
    m_clipGainSpin->hide();
    m_clipPanSpin->hide();
    m_sourceInSpin->hide();
    m_sourceOutSpin->hide();
    m_fadeInSpin->hide();
    m_fadeOutSpin->hide();
    m_normalizeBtn->hide();
    m_reverseBtn->hide();
    m_reversePolarityBtn->hide();
    m_mediaStatusLabel->hide();
    m_relinkMediaBtn->hide();
    m_araAlgorithmCombo->hide();

    updateUi();
}

void InspectorWidget::setAudioClip(const QString& name, double startBeat, double lengthBeats, const QString& sourcePath,
                                   double sourceTempoBpm, double pitchShiftSemitones, int syncMode, double targetBars,
                                   double tempoConfidence, int sourceSignatureNumerator,
                                   int sourceSignatureDenominator, bool tempoDetected,
                                   float gain, float pan, bool normalize, bool reverse, bool reversePolarity,
                                   double sourceOffsetBeats, double sourceRangeLengthBeats,
                                   double warpLengthBeats,
                                   double fadeInBeats, double fadeOutBeats, bool mediaOffline,
                                   int transientCount, int warpMarkerCount) {
    m_showingMidiClip = false;
    m_showingAudioClip = true;
    m_audioClipSourcePath = sourcePath;
    m_currentClipStartBeat = startBeat;
    m_currentClipLengthBeats = lengthBeats;
    m_clipGroup->setTitle("AUDIO CLIP");
    m_trackNameLabel->setText(name);
    m_araAlgorithmCombo->hide();

    updateClipPositionLabels();
    const QString tempoText = sourceTempoBpm > 0.0
        ? QString("%1 BPM").arg(sourceTempoBpm, 0, 'f', 1)
        : QString("Auto");
    const QString modeText = syncMode == 3
        ? QString("Full Sync")
        : (syncMode == 2 ? QString("Bars") : (syncMode == 1 ? QString("Auto BPM") : QString("Off")));
    const bool validSourceMeter = sourceSignatureNumerator >= 1 && sourceSignatureNumerator <= 32 &&
        sourceSignatureDenominator >= 1 && sourceSignatureDenominator <= 64 &&
        (sourceSignatureDenominator & (sourceSignatureDenominator - 1)) == 0;
    const QString meterText = validSourceMeter
        ? QString("%1/%2").arg(sourceSignatureNumerator).arg(sourceSignatureDenominator)
        : QString("Project %1/%2").arg(m_timeSignatureNumerator).arg(m_timeSignatureDenominator);
    const QString analysisText = tempoDetected && sourceTempoBpm > 0.0
        ? QString("Detected %1%").arg(qRound(qBound(0.0, tempoConfidence, 1.0) * 100.0))
        : (sourceTempoBpm > 0.0 ? QStringLiteral("Manual") : QStringLiteral("Unknown"));
    m_clipSyncLabel->setText(QString("Sync: %1 · %2 · %3 · %4 · pitch %5 st · %6 transients · %7 warp")
        .arg(modeText)
        .arg(tempoText)
        .arg(analysisText)
        .arg(meterText)
        .arg(pitchShiftSemitones, 0, 'f', 1)
        .arg(qMax(0, transientCount))
        .arg(qMax(0, warpMarkerCount)));
    m_clipSyncLabel->setToolTip(tempoDetected
        ? QStringLiteral("Tempo and meter analyzed from onset periodicity; confidence is stored with shared audio content.")
        : QStringLiteral("Manual/unknown tempo uses the project time signature as the meter fallback."));
    {
        QSignalBlocker tempoBlock(m_sourceTempoSpin);
        QSignalBlocker pitchBlock(m_pitchShiftSpin);
        QSignalBlocker modeBlock(m_syncModeCombo);
        QSignalBlocker barsBlock(m_targetBarsSpin);
        QSignalBlocker gainBlock(m_clipGainSpin);
        QSignalBlocker panBlock(m_clipPanSpin);
        QSignalBlocker sourceInBlock(m_sourceInSpin);
        QSignalBlocker sourceOutBlock(m_sourceOutSpin);
        QSignalBlocker normalizeBlock(m_normalizeBtn);
        QSignalBlocker reverseBlock(m_reverseBtn);
        QSignalBlocker polarityBlock(m_reversePolarityBtn);
        QSignalBlocker fadeInBlock(m_fadeInSpin);
        QSignalBlocker fadeOutBlock(m_fadeOutSpin);
        m_syncModeCombo->setCurrentIndex(qBound(0, syncMode, 3));
        m_sourceTempoSpin->setValue(qBound(0.0, sourceTempoBpm, 300.0));
        m_pitchShiftSpin->setValue(qBound(-48.0, pitchShiftSemitones, 48.0));
        m_targetBarsSpin->setValue(qBound(0.0, targetBars, 512.0));
        m_clipGainSpin->setValue(clipGainToDb(std::isfinite(gain) ? qBound(0.0f, gain, 4.0f) : 1.0f));
        m_clipPanSpin->setValue((std::isfinite(pan) ? qBound(-1.0f, pan, 1.0f) : 0.0f) * 100.0);
        const double sourceLength = qMax(0.25,
            std::isfinite(warpLengthBeats) && warpLengthBeats > 0.0
                ? warpLengthBeats : sourceOffsetBeats + lengthBeats);
        const double sourceIn = qBound(0.0, sourceOffsetBeats, qMax(0.0, sourceLength - 0.25));
        const double rangeLength = std::isfinite(sourceRangeLengthBeats) &&
            sourceRangeLengthBeats >= 0.25 ? sourceRangeLengthBeats : lengthBeats;
        const double sourceOut = qBound(sourceIn + 0.25,
            sourceOffsetBeats + rangeLength, sourceLength);
        m_sourceInSpin->setRange(0.0, qMax(0.0, sourceOut - 0.25));
        m_sourceOutSpin->setRange(sourceIn + 0.25, sourceLength);
        m_sourceInSpin->setValue(sourceIn);
        m_sourceOutSpin->setValue(sourceOut);
        m_normalizeBtn->setChecked(normalize);
        m_reverseBtn->setChecked(reverse);
        m_reversePolarityBtn->setChecked(reversePolarity);
        m_fadeInSpin->setMaximum(qMax(0.0, lengthBeats));
        m_fadeOutSpin->setMaximum(qMax(0.0, lengthBeats));
        m_fadeInSpin->setValue(qBound(0.0, fadeInBeats, qMax(0.0, lengthBeats)));
        m_fadeOutSpin->setValue(qBound(0.0, fadeOutBeats, qMax(0.0, lengthBeats)));
    }
    m_syncModeCombo->show();
    m_sourceTempoSpin->setVisible(m_syncModeCombo->currentData().toInt() == 1);
    m_pitchShiftSpin->show();
    m_pitchShiftSpin->setEnabled(syncMode == 0 || AudioStretchBackend::isElastiqueAvailable());
    m_pitchShiftSpin->setToolTip(syncMode == 0
        ? "Varispeed transpose in semitones; changing pitch also changes duration."
        : (m_pitchShiftSpin->isEnabled()
            ? "Independent pitch shift in semitones (Elastique Pro)."
            : "Independent pitch shift for synced clips requires Elastique Pro; basic sync preserves full clip duration."));
    m_targetBarsSpin->setVisible(m_syncModeCombo->currentData().toInt() == 2);
    m_clipGainSpin->show();
    m_clipPanSpin->show();
    m_sourceInSpin->show();
    m_sourceOutSpin->show();
    m_fadeInSpin->show();
    m_fadeOutSpin->show();
    m_normalizeBtn->show();
    m_reverseBtn->show();
    m_reversePolarityBtn->show();
    m_mediaStatusLabel->setProperty("mediaOffline", mediaOffline);
    m_mediaStatusLabel->setText(mediaOffline ? QStringLiteral("MEDIA OFFLINE")
                                             : QStringLiteral("MEDIA ONLINE"));
    m_mediaStatusLabel->setToolTip(mediaOffline
        ? QStringLiteral("The source file is unavailable. Editing remains intact; playback is silent until relinked.")
        : QFileInfo(sourcePath).absoluteFilePath());
    m_mediaStatusLabel->style()->unpolish(m_mediaStatusLabel);
    m_mediaStatusLabel->style()->polish(m_mediaStatusLabel);
    m_mediaStatusLabel->show();
    m_relinkMediaBtn->setVisible(mediaOffline);

    updateUi();
}

void InspectorWidget::updateUi() {
    refreshMixerControls();
	if (m_currentReturnFxBus >= 0 && m_currentReturnFxBus < kReturnBusCount) {
		const ReturnBusState& bus = m_returnBusView[static_cast<std::size_t>(m_currentReturnFxBus)];
		m_trackNameLabel->setText(bus.name.isEmpty()
			? QString("Return %1").arg(returnBusLetter(m_currentReturnFxBus)) : bus.name);
		m_contextLabel->setText("RETURN");
		m_clipGroup->setVisible(false);
		m_instrumentGroup->setVisible(false);
		m_fxGroup->setVisible(true);
		m_outputGroup->setVisible(true);
		m_openMidiEditorBtn->setEnabled(false);
		m_openMidiEditorBtn->hide();
		m_stretchStatusLabel->hide();
		m_syncModeCombo->hide();
		m_sourceTempoSpin->hide();
		m_pitchShiftSpin->hide();
		m_targetBarsSpin->hide();
		m_clipGainSpin->hide();
		m_clipPanSpin->hide();
		m_sourceInSpin->hide();
		m_sourceOutSpin->hide();
		m_fadeInSpin->hide();
		m_fadeOutSpin->hide();
		m_normalizeBtn->hide();
		m_reverseBtn->hide();
		m_reversePolarityBtn->hide();
		m_mediaStatusLabel->hide();
		m_relinkMediaBtn->hide();
		m_araAlgorithmCombo->hide();
		rebuildAudioFxRack();
		return;
	}
    if (!m_currentTrack) {
        m_trackNameLabel->setText("No Track Selected");
        m_contextLabel->setText("TRACK");
        m_clipGroup->setVisible(false);
        m_instrumentGroup->setVisible(false);
        m_fxGroup->setVisible(false);
        m_outputGroup->setVisible(false);
        m_openMidiEditorBtn->setEnabled(false);
        m_stretchStatusLabel->hide();
        m_syncModeCombo->hide();
        m_sourceTempoSpin->hide();
        m_pitchShiftSpin->hide();
        m_targetBarsSpin->hide();
        m_clipGainSpin->hide();
        m_clipPanSpin->hide();
        m_sourceInSpin->hide();
        m_sourceOutSpin->hide();
        m_fadeInSpin->hide();
        m_fadeOutSpin->hide();
        m_normalizeBtn->hide();
        m_reverseBtn->hide();
        m_reversePolarityBtn->hide();
        m_mediaStatusLabel->hide();
        m_relinkMediaBtn->hide();
        m_araAlgorithmCombo->hide();
        return;
    }

    if (m_showingMidiClip || m_showingAudioClip) {
        m_contextLabel->setText("CLIP");
        m_clipGroup->setVisible(true);
        m_openMidiEditorBtn->setEnabled(m_showingMidiClip);
        if (m_showingMidiClip) {
            m_mediaStatusLabel->hide();
            m_relinkMediaBtn->hide();
            m_openMidiEditorBtn->show();
            m_stretchStatusLabel->hide();
            m_openMidiEditorBtn->setText("Open MIDI Editor");
            m_openMidiEditorBtn->setToolTip("Open the selected MIDI clip in the piano roll");
        } else {
            m_mediaStatusLabel->show();
            const QString sourceName = QFileInfo(m_audioClipSourcePath).fileName();
            m_openMidiEditorBtn->hide();
            m_stretchStatusLabel->show();
            m_stretchStatusLabel->setText(AudioStretchBackend::isElastiqueAvailable()
                ? "Elastique Pro: independent time + pitch"
                : "Basic sync: resample / varispeed");
            m_stretchStatusLabel->setToolTip(QString("%1\nElastique Pro preserves independent time-stretch and pitch-shift. Basic sync resamples the source without launching an external process.")
                .arg(sourceName.isEmpty() ? m_audioClipSourcePath : sourceName));
        }
        m_instrumentGroup->setVisible(false);
        m_fxGroup->setVisible(false);
        m_outputGroup->setVisible(false);
        return;
    }

    m_trackNameLabel->setText(m_currentTrack->GetName());
    m_contextLabel->setText("TRACK");
    m_clipGroup->setVisible(false);
    m_openMidiEditorBtn->setEnabled(false);
    m_openMidiEditorBtn->show();
    m_stretchStatusLabel->hide();
    m_openMidiEditorBtn->setText("Open MIDI Editor");
    m_openMidiEditorBtn->setToolTip("Open the selected MIDI clip in the piano roll");
    m_clipSyncLabel->setText("Sync: none");
    m_syncModeCombo->hide();
    m_sourceTempoSpin->hide();
    m_pitchShiftSpin->hide();
    m_targetBarsSpin->hide();
    m_clipGainSpin->hide();
    m_clipPanSpin->hide();
    m_sourceInSpin->hide();
    m_sourceOutSpin->hide();
    m_fadeInSpin->hide();
    m_fadeOutSpin->hide();
    m_normalizeBtn->hide();
    m_reverseBtn->hide();
    m_reversePolarityBtn->hide();
    m_araAlgorithmCombo->hide();

    m_instrumentGroup->setVisible(m_currentTrack->GetType() == TrackType::Midi);
    rebuildInstrumentRack();
    rebuildAudioFxRack();
    m_fxGroup->setVisible(true);
    m_outputGroup->setVisible(true);
}

void InspectorWidget::rebuildAudioFxRack()
{
    clearPluginParameters();
    while (QLayoutItem* item = m_fxLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
    const bool returnRack =
        m_currentReturnFxBus >= 0 && m_currentReturnFxBus < kReturnBusCount;
    const std::vector<AudioFxSlot>* audioFxSlots = returnRack
        ? &m_returnBusView[static_cast<std::size_t>(m_currentReturnFxBus)].audioFxSlots
        : m_currentTrack ? &m_currentTrack->GetAudioFxSlots() : nullptr;
    if (audioFxSlots) {
        for (int i = 0; i < static_cast<int>(audioFxSlots->size()); ++i) {
            const AudioFxSlot& slot = (*audioFxSlots)[static_cast<std::size_t>(i)];
            QFrame* row = new QFrame(m_fxGroup);
            row->setStyleSheet("QFrame { background:#333; border:1px solid #161616; border-radius:3px; }");
            QHBoxLayout* layout = new QHBoxLayout(row);
            layout->setContentsMargins(5, 3, 5, 3);
            QCheckBox* enabled = new QCheckBox(row);
            enabled->setChecked(slot.enabled);
            enabled->setToolTip("Bypass/enable effect");
            QLabel* name = new QLabel(slot.name, row);
            name->setToolTip(QString("%1\n%2").arg(slot.pluginType, slot.pluginPath));
            name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            const auto& hostStates = returnRack
                ? m_returnAudioFxHostStates[static_cast<std::size_t>(m_currentReturnFxBus)]
                : m_audioFxHostStates;
            const PluginHostStatus status = i < static_cast<int>(hostStates.size())
                ? hostStates[static_cast<std::size_t>(i)] : PluginHostStatus::Pending;
            QPushButton* remove = new QPushButton("X", row);
            remove->setFixedWidth(22);
            layout->addWidget(enabled);
            layout->addWidget(name, 1);
            if (status != PluginHostStatus::Internal) {
                QLabel* statusBadge = new QLabel(pluginStatusText(status), row);
                statusBadge->setObjectName("pluginStatusBadge");
                statusBadge->setProperty("hostStatus", pluginStatusProperty(status));
                statusBadge->setToolTip(pluginStatusDescription(status));
                statusBadge->setAccessibleName(QString("Plugin status: %1").arg(pluginStatusText(status)));
                layout->addWidget(statusBadge);
            }
            if (status == PluginHostStatus::Faulted) {
                QPushButton* reload = new QPushButton("Reload", row);
                reload->setFixedWidth(48);
                reload->setToolTip(returnRack
                    ? "Reload return-bus effect DSP"
                    : "Discard faulted DSP instance and load a clean instance");
                connect(reload, &QPushButton::clicked, this, [this, i, returnRack]() {
                    if (returnRack) emit returnAudioFxReloadRequested(m_currentReturnFxBus, i);
                    else emit audioFxReloadRequested(i);
                });
                layout->addWidget(reload);
            }
            if (status != PluginHostStatus::Internal) {
                QPushButton* parameters = new QPushButton("PARAM", row);
                parameters->setFixedWidth(44);
                parameters->setEnabled(status == PluginHostStatus::Active);
                parameters->setToolTip("Open generic plug-in parameter controls");
                connect(parameters, &QPushButton::clicked, this,
                        [this, i, deviceName = slot.name]() {
                    emit pluginParametersRequested(true, i, deviceName);
                });
                layout->addWidget(parameters);
            }
            layout->addWidget(remove);
            connect(enabled, &QCheckBox::toggled, this, [this, i, returnRack](bool value) {
                if (returnRack) emit returnAudioFxEnabledChanged(m_currentReturnFxBus, i, value);
                else emit audioFxEnabledChanged(i, value);
            });
            connect(remove, &QPushButton::clicked, this, [this, i, returnRack]() {
                if (returnRack) emit removeReturnAudioFxRequested(m_currentReturnFxBus, i);
                else emit removeAudioFxRequested(i);
            });
            m_fxLayout->addWidget(row);
        }
    }
    m_addFxBtn = new QPushButton("+ Add FX from Browser", m_fxGroup);
    m_addFxBtn->setStyleSheet("QPushButton { text-align:center; background:transparent; color:#999; "
        "border:1px dashed #4a4a4a; padding:6px 8px; border-radius:3px; } "
        "QPushButton:hover { border-color:#ff9020; color:#ff9020; }");
    connect(m_addFxBtn, &QPushButton::clicked, this, [this, returnRack]() {
        if (returnRack) emit addReturnAudioFxRequested(m_currentReturnFxBus);
        else emit addAudioFxRequested();
    });
    m_fxLayout->addWidget(m_addFxBtn);
    m_fxLayout->addStretch();
}

void InspectorWidget::rebuildInstrumentRack()
{
    while (QLayoutItem* item = m_instrumentLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    MidiTrack* midiTrack = nullptr;
    if (m_currentTrack && m_currentTrack->GetType() == TrackType::Midi) {
        midiTrack = dynamic_cast<MidiTrack*>(m_currentTrack);
    }

    const QString rowStyle =
        "QFrame { background-color: #333; border: 1px solid #161616; border-radius: 3px; } "
        "QLabel { background: transparent; border: none; }";
    const QString miniButtonStyle =
        "QPushButton { background-color: #2a2a2a; color: #aaa; border: 1px solid #161616; "
        "border-radius: 3px; padding: 1px 5px; font-size: 9px; font-weight: bold; } "
        "QPushButton:hover:enabled { color: #fff; border-color: #ff9020; } "
        "QPushButton:disabled { color: #555; background-color: #272727; }";
    const QString compactControlStyle =
        "QDoubleSpinBox, QSpinBox, QComboBox { background-color: #262626; color: #cfcfcf; "
        "border: 1px solid #161616; border-radius: 3px; padding: 1px 3px; font-size: 9px; } "
        "QDoubleSpinBox:hover, QSpinBox:hover, QComboBox:hover { border-color: #ff9020; }";

    if (!midiTrack) {
        QLabel* empty = new QLabel("No MIDI track selected", m_instrumentGroup);
        empty->setStyleSheet("color: #777; background: transparent; border: none;");
        m_instrumentLayout->addWidget(empty);
    } else {
        const std::vector<LayerSlot>& layers = midiTrack->GetLayerSlots();
        if (layers.empty()) {
            QLabel* empty = new QLabel("No layers", m_instrumentGroup);
            empty->setStyleSheet("color: #777; background: transparent; border: none;");
            m_instrumentLayout->addWidget(empty);
        }

        for (int i = 0; i < static_cast<int>(layers.size()); ++i) {
            const LayerSlot& layer = layers[i];
            QFrame* row = new QFrame(m_instrumentGroup);
            row->setStyleSheet(rowStyle);
            QHBoxLayout* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(7, 4, 7, 4);
            rowLayout->setSpacing(6);

            QCheckBox* enabled = new QCheckBox(row);
            enabled->setChecked(layer.enabled);
            enabled->setToolTip(layer.enabled ? "Layer enabled" : "Layer muted");
            enabled->setStyleSheet("QCheckBox { background: transparent; border: none; } "
                                   "QCheckBox::indicator { width: 12px; height: 12px; }");
            connect(enabled, &QCheckBox::toggled, this, [this, i](bool checked) {
                emit layerEnabledChanged(i, checked);
            });
            rowLayout->addWidget(enabled);

            QLabel* index = new QLabel(QString::number(i + 1), row);
            index->setFixedWidth(18);
            index->setAlignment(Qt::AlignCenter);
            index->setStyleSheet(layer.enabled ? "color: #ff9020;" : "color: #666;");
            rowLayout->addWidget(index);

            const PluginHostStatus status = i < static_cast<int>(m_layerHostStates.size())
                ? m_layerHostStates[static_cast<size_t>(i)] : PluginHostStatus::Pending;
            QLabel* name = new QLabel(layer.name, row);
            name->setStyleSheet(layer.enabled ? "color: #e6e6e6; font-weight: bold;" : "color: #777;");
            const QString layerSource = layer.pluginPath.isEmpty() ? layer.pluginType : layer.pluginPath;
            name->setToolTip(QString("%1\n%2").arg(layerSource, pluginStatusDescription(status)));
            rowLayout->addWidget(name, 1);
            if (status != PluginHostStatus::Internal) {
                QLabel* statusBadge = new QLabel(pluginStatusText(status), row);
                statusBadge->setObjectName("pluginStatusBadge");
                statusBadge->setProperty("hostStatus", pluginStatusProperty(status));
                statusBadge->setToolTip(pluginStatusDescription(status));
                statusBadge->setAccessibleName(QString("Plugin status: %1").arg(pluginStatusText(status)));
                rowLayout->addWidget(statusBadge);
            }
            if (status == PluginHostStatus::Faulted) {
                QPushButton* reload = new QPushButton("Reload", row);
                reload->setFixedWidth(48);
                reload->setToolTip("Discard faulted DSP instance and load a clean instance");
                reload->setStyleSheet(miniButtonStyle);
                connect(reload, &QPushButton::clicked, this, [this, i]() {
                    emit layerReloadRequested(i);
                });
                rowLayout->addWidget(reload);
            }

            auto emitParameters = [this, i, row]() {
                QDoubleSpinBox* gain = row->findChild<QDoubleSpinBox*>("layerGain");
                QDoubleSpinBox* pan = row->findChild<QDoubleSpinBox*>("layerPan");
                QSpinBox* transpose = row->findChild<QSpinBox*>("layerTranspose");
                QComboBox* channel = row->findChild<QComboBox*>("layerChannel");
                if (!gain || !pan || !transpose || !channel) {
                    return;
                }
                emit layerParametersChanged(i,
                                            static_cast<float>(gain->value() / 100.0),
                                            static_cast<float>(pan->value() / 100.0),
                                            transpose->value(),
                                            channel->currentData().toInt());
            };

            const QString transposeText = layer.transpose == 0
                ? QString()
                : QString(" T%1%2").arg(layer.transpose > 0 ? "+" : "").arg(layer.transpose);
            const QString channelText = layer.midiChannel < 0
                ? QString(" All")
                : QString(" Ch%1").arg(layer.midiChannel + 1);
            const float displayGain = std::isfinite(layer.gain) ? std::clamp(layer.gain, 0.0f, 2.0f) : 1.0f;
            const float displayPan = std::isfinite(layer.pan) ? std::clamp(layer.pan, -1.0f, 1.0f) : 0.0f;
            const QString metaText = QString("%1  %2%3%4%5")
                                         .arg(layer.pluginType.isEmpty() ? "Internal" : layer.pluginType)
                                         .arg(static_cast<int>(displayGain * 100.0f))
                                         .arg("%")
                                         .arg(transposeText)
                                         .arg(channelText);
            QLabel* meta = new QLabel(metaText, row);
            meta->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            meta->setStyleSheet("color: #9a9a9a; font-size: 9px;");
            rowLayout->addWidget(meta);

            QDoubleSpinBox* gain = new QDoubleSpinBox(row);
            gain->setObjectName("layerGain");
            gain->setRange(0.0, 200.0);
            gain->setDecimals(0);
            gain->setSuffix("%");
            gain->setValue(displayGain * 100.0f);
            gain->setFixedWidth(56);
            gain->setToolTip("Layer gain");
            gain->setStyleSheet(compactControlStyle);
            connect(gain, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [emitParameters](double) {
                emitParameters();
            });
            rowLayout->addWidget(gain);

            QDoubleSpinBox* pan = new QDoubleSpinBox(row);
            pan->setObjectName("layerPan");
            pan->setRange(-100.0, 100.0);
            pan->setDecimals(0);
            pan->setValue(displayPan * 100.0f);
            pan->setFixedWidth(54);
            pan->setToolTip("Layer pan: -100 left, 100 right");
            pan->setStyleSheet(compactControlStyle);
            connect(pan, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [emitParameters](double) {
                emitParameters();
            });
            rowLayout->addWidget(pan);

            QSpinBox* transpose = new QSpinBox(row);
            transpose->setObjectName("layerTranspose");
            transpose->setRange(-48, 48);
            transpose->setValue(layer.transpose);
            transpose->setPrefix("T");
            transpose->setFixedWidth(50);
            transpose->setToolTip("Layer transpose in semitones");
            transpose->setStyleSheet(compactControlStyle);
            connect(transpose, qOverload<int>(&QSpinBox::valueChanged), this, [emitParameters](int) {
                emitParameters();
            });
            rowLayout->addWidget(transpose);

            QComboBox* channel = new QComboBox(row);
            channel->setObjectName("layerChannel");
            channel->addItem("All", -1);
            for (int ch = 0; ch < 16; ++ch) {
                channel->addItem(QString("Ch%1").arg(ch + 1), ch);
            }
            channel->setCurrentIndex(qMax(0, layer.midiChannel + 1));
            channel->setFixedWidth(56);
            channel->setToolTip("MIDI channel/color group routed to this layer");
            channel->setStyleSheet(compactControlStyle);
            connect(channel, qOverload<int>(&QComboBox::currentIndexChanged), this, [emitParameters](int) {
                emitParameters();
            });
            rowLayout->addWidget(channel);

            if (layer.pluginType.compare("Internal", Qt::CaseInsensitive) != 0) {
                QPushButton* parameters = new QPushButton("PARAM", row);
                parameters->setFixedWidth(42);
                parameters->setEnabled(status == PluginHostStatus::Active);
                parameters->setToolTip("Open generic plug-in parameter controls");
                parameters->setStyleSheet(miniButtonStyle);
                connect(parameters, &QPushButton::clicked, this,
                        [this, i, deviceName = layer.name]() {
                    emit pluginParametersRequested(false, i, deviceName);
                });
                rowLayout->addWidget(parameters);

                QPushButton* outputs = new QPushButton("OUT", row);
                outputs->setFixedWidth(34);
                outputs->setToolTip("Map this plug-in's output buses to VeraCore mixer channels");
                outputs->setStyleSheet(miniButtonStyle);
                connect(outputs, &QPushButton::clicked, this, [this, i]() {
                    emit layerOutputRoutingRequested(i);
                });
                rowLayout->addWidget(outputs);
            }

            QPushButton* remove = new QPushButton("X", row);
            remove->setFixedWidth(22);
            remove->setEnabled(layers.size() > 1);
            remove->setToolTip(layers.size() > 1 ? "Remove this layer" : "Keep at least one layer on a MIDI track");
            remove->setStyleSheet(miniButtonStyle);
            connect(remove, &QPushButton::clicked, this, [this, i]() {
                emit removeLayerRequested(i);
            });
            rowLayout->addWidget(remove);

            m_instrumentLayout->addWidget(row);
        }
    }

    m_addLayerBtn = new QPushButton("+ Add Internal Layer", m_instrumentGroup);
    m_addLayerBtn->setToolTip("Adds another internal sound layer driven by the same MIDI clip. Drag an instrument from the browser to add a plugin layer placeholder.");
    m_addLayerBtn->setStyleSheet("QPushButton { text-align: center; background-color: transparent; "
                                 "color: #999; border: 1px dashed #4a4a4a; padding: 5px 8px; "
                                 "border-radius: 3px; } "
                                 "QPushButton:hover { border-color: #ff9020; color: #ff9020; }");
    connect(m_addLayerBtn, &QPushButton::clicked, this, &InspectorWidget::addLayerRequested);
    m_instrumentLayout->addWidget(m_addLayerBtn);
    m_instrumentLayout->addStretch();
}
