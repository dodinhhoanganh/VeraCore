#include "TrackWidget.h"
#include <QSizePolicy>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QVariant>
#include <cmath>

namespace {

// Track identity color shown as a left strip (MIDI teal, audio orange).
QString trackColor(const Track* track)
{
    return (track && track->GetType() == TrackType::Midi) ? "#39d5c7" : "#ff9b35";
}

QString trackTypeText(const Track* track)
{
    return (track && track->GetType() == TrackType::Midi) ? "MIDI" : "AUDIO";
}

QString tileStyle(const QString& color, bool selected)
{
    const QString bg = selected ? "#30362f" : "#252825";
    const QString border = selected ? color : "#111411";
    return QString("#TrackWidget { background-color: %1; border: 1px solid %2; "
                   "border-bottom: 1px solid #0d100d; border-left: 4px solid %3; }")
        .arg(bg, border, color);
}

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

QChar returnBusLetter(int bus)
{
    return QChar(static_cast<char16_t>(u'A' + bus));
}

} // namespace

TrackWidget::TrackWidget(Track* track, QWidget* parent)
    : QWidget(parent)
    , m_track(track)
{
    setupUi();
}

void TrackWidget::setupUi() {
    // Height matches the 60px playlist lanes exactly.
    this->setFixedHeight(60);
    this->setMinimumWidth(214);
    this->setObjectName("TrackWidget");
    this->setStyleSheet(tileStyle(trackColor(m_track), false));

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(9, 4, 6, 4);
    mainLayout->setSpacing(2);

    // --- Header: name + fast channel controls ---
    QHBoxLayout* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(4);

    m_typeLabel = new QLabel(trackTypeText(m_track), this);
    m_typeLabel->setFixedWidth(34);
    m_typeLabel->setAlignment(Qt::AlignCenter);
    m_typeLabel->setStyleSheet(QString("color: #0d100d; background-color: %1; border-radius: 2px; "
                                       "font-size: 8px; font-weight: bold; padding: 1px 0px;")
                                   .arg(trackColor(m_track)));
    headerLayout->addWidget(m_typeLabel);

    m_nameLabel = new QLabel(m_track->GetName(), this);
    m_nameLabel->setStyleSheet("color: #edf3ed; font-weight: bold; font-size: 11px; "
                               "background: transparent; border: none;");
    m_nameLabel->setMinimumWidth(42);
    m_nameLabel->setToolTip(m_track->GetName());
    m_nameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    headerLayout->addWidget(m_nameLabel, 1);

    const QString smallBtn =
        "QPushButton { background-color: #303530; color: #bfc7bf; border: 1px solid #111411; "
        "border-radius: 2px; font-size: 9px; font-weight: bold; padding: 0px; } "
        "QPushButton:hover { border-color: #ff9020; color: #ffffff; } ";

    m_muteBtn = new QPushButton("M", this);
    m_muteBtn->setFixedSize(19, 18);
    m_muteBtn->setCheckable(true);
    m_muteBtn->setChecked(m_track->IsMuted());
    m_muteBtn->setToolTip("Mute");
    m_muteBtn->setStyleSheet(smallBtn +
        "QPushButton:checked { background-color: #ffb13b; color: #141414; font-weight: bold; }");

    m_soloBtn = new QPushButton("S", this);
    m_soloBtn->setFixedSize(19, 18);
    m_soloBtn->setCheckable(true);
    m_soloBtn->setChecked(m_track->IsSoloed());
    m_soloBtn->setToolTip("Solo");
    m_soloBtn->setStyleSheet(smallBtn +
        "QPushButton:checked { background-color: #4a90d9; color: #141414; font-weight: bold; }");

    m_recBtn = new QPushButton("R", this);
    m_recBtn->setFixedSize(19, 18);
    m_recBtn->setCheckable(true);
    m_recBtn->setChecked(m_track->IsArmed());
    m_recBtn->setToolTip("Arm for recording");
    m_recBtn->setStyleSheet(smallBtn +
        "QPushButton:checked { background-color: #e04a3a; color: white; font-weight: bold; }");

    m_monitorBtn = new QPushButton("I", this);
    m_monitorBtn->setFixedSize(19, 18);
    m_monitorBtn->setCheckable(true);
    m_monitorBtn->setChecked(m_track->IsMonitoring());
    m_monitorBtn->setToolTip("Input monitor");
    m_monitorBtn->setStyleSheet(smallBtn +
        "QPushButton:checked { background-color: #56b6c2; color: #141414; font-weight: bold; }");

    m_deviceBtn = new QPushButton("FX", this);
    m_deviceBtn->setFixedSize(23, 18);
    m_deviceBtn->setToolTip(m_track->GetType() == TrackType::Midi ? "Open instrument and devices" : "Open audio devices");
    m_deviceBtn->setStyleSheet(smallBtn + "QPushButton { font-size: 8px; color: #d6ded6; }");

    headerLayout->addWidget(m_muteBtn);
    headerLayout->addWidget(m_soloBtn);
    headerLayout->addWidget(m_recBtn);
    headerLayout->addWidget(m_monitorBtn);
    headerLayout->addWidget(m_deviceBtn);

    mainLayout->addLayout(headerLayout);

    // --- Volume and pan sliders ---
    QHBoxLayout* controlLayout = new QHBoxLayout();
    controlLayout->setSpacing(5);

    QLabel* volIcon = new QLabel("V", this);
    volIcon->setFixedWidth(8);
    volIcon->setAlignment(Qt::AlignCenter);
    volIcon->setStyleSheet("color: #8e9a8e; font-size: 9px; font-weight: bold; background: transparent; border: none;");
    m_volSlider = new QSlider(Qt::Horizontal, this);
    m_volSlider->setRange(-700, 60);
    m_volSlider->setValue(gainToFader(m_track->GetVolume()));
    m_volSlider->setToolTip(QString("Volume: %1").arg(faderText(m_volSlider->value())));
    m_volSlider->setFixedHeight(12);
    m_volSlider->setStyleSheet(
        "QSlider::groove:horizontal { border: 1px solid #111411; height: 4px; "
        "background: #171a17; margin: 2px 0; border-radius: 2px; } "
        "QSlider::sub-page:horizontal { background: #6f8569; border-radius: 2px; } "
        "QSlider::handle:horizontal { background: #c0c8bc; border: 1px solid #333b32; "
        "width: 7px; margin: -4px 0; border-radius: 2px; } "
        "QSlider::handle:horizontal:hover { background: #ff9020; }");

    QLabel* panIcon = new QLabel("P", this);
    panIcon->setFixedWidth(8);
    panIcon->setAlignment(Qt::AlignCenter);
    panIcon->setStyleSheet(volIcon->styleSheet());
    m_panSlider = new QSlider(Qt::Horizontal, this);
    m_panSlider->setRange(-100, 100);
    m_panSlider->setValue((int)(m_track->GetPan() * 100));
    m_panSlider->setToolTip(QString("Pan: %1").arg(m_panSlider->value()));
    m_panSlider->setFixedHeight(12);
    m_panSlider->setStyleSheet(m_volSlider->styleSheet());

    QLabel* widthIcon = new QLabel("W", this);
    widthIcon->setFixedWidth(8);
    widthIcon->setAlignment(Qt::AlignCenter);
    widthIcon->setStyleSheet(volIcon->styleSheet());
    m_stereoSeparationSlider = new QDial(this);
    m_stereoSeparationSlider->setRange(-100, 100);
    m_stereoSeparationSlider->setValue(qRound(m_track->GetStereoSeparation() * 100.0f));
    m_stereoSeparationSlider->setNotchesVisible(false);
    m_stereoSeparationSlider->setWrapping(false);
    m_stereoSeparationSlider->setFixedSize(22, 22);
    m_stereoSeparationSlider->setStyleSheet(
        "QDial { background:#1b1d1b; }");
    m_stereoSeparationSlider->setToolTip(
        QString("Stereo separation: %1%").arg(m_stereoSeparationSlider->value()));

    controlLayout->addWidget(volIcon);
    controlLayout->addWidget(m_volSlider, 1);
    controlLayout->addWidget(panIcon);
    controlLayout->addWidget(m_panSlider, 1);
    controlLayout->addWidget(widthIcon);
    controlLayout->addWidget(m_stereoSeparationSlider, 1);
    for (int bus = 0; bus < kReturnBusCount; ++bus) {
        QLabel* sendIcon = new QLabel(QString(returnBusLetter(bus)), this);
        sendIcon->setFixedWidth(8);
        sendIcon->setAlignment(Qt::AlignCenter);
        sendIcon->setStyleSheet(volIcon->styleSheet());
        m_sendSliders[bus] = new QSlider(Qt::Horizontal, this);
        m_sendSliders[bus]->setRange(-700, 0);
        m_sendSliders[bus]->setValue(gainToFader(m_track->GetSendLevel(bus)));
        m_sendSliders[bus]->setFixedWidth(24);
        m_sendSliders[bus]->setFixedHeight(12);
        m_sendSliders[bus]->setStyleSheet(m_volSlider->styleSheet());
        m_sendSliders[bus]->setToolTip(QString("Send %1: %2")
            .arg(returnBusLetter(bus)).arg(faderText(m_sendSliders[bus]->value())));
        controlLayout->addWidget(sendIcon);
        controlLayout->addWidget(m_sendSliders[bus]);
        connect(m_sendSliders[bus], &QSlider::valueChanged, this, [this, bus](int value) {
            m_sendSliders[bus]->setToolTip(QString("Send %1: %2")
                .arg(returnBusLetter(bus)).arg(faderText(value)));
            emit trackSendEdited(m_track, bus, faderToGain(value));
        });
    }
    mainLayout->addLayout(controlLayout);

    connect(m_muteBtn, &QPushButton::toggled, this, [this](bool checked) {
        Q_UNUSED(checked);
        emitMixerControlsEdited();
    });
    connect(m_soloBtn, &QPushButton::toggled, this, [this](bool checked) {
        Q_UNUSED(checked);
        emitMixerControlsEdited();
    });
    connect(m_recBtn, &QPushButton::toggled, this, [this](bool checked) {
        Q_UNUSED(checked);
        emitMixerControlsEdited();
    });
    connect(m_monitorBtn, &QPushButton::toggled, this, [this](bool checked) {
        Q_UNUSED(checked);
        emitMixerControlsEdited();
    });
    connect(m_volSlider, &QSlider::valueChanged, this, [this](int value) {
        m_volSlider->setToolTip(QString("Volume: %1").arg(faderText(value)));
        emitMixerControlsEdited();
    });
    connect(m_panSlider, &QSlider::valueChanged, this, [this](int value) {
        m_panSlider->setToolTip(QString("Pan: %1").arg(value));
        emitMixerControlsEdited();
    });
    connect(m_stereoSeparationSlider, &QDial::valueChanged, this, [this](int value) {
        m_stereoSeparationSlider->setToolTip(QString("Stereo separation: %1%").arg(value));
        emitMixerControlsEdited();
    });
    connect(m_deviceBtn, &QPushButton::clicked, this, [this]() {
        emit trackSelected(this, m_track);
    });

    mainLayout->addStretch();
}

void TrackWidget::emitMixerControlsEdited()
{
    emit mixerControlsEdited(m_track,
        faderToGain(m_volSlider->value()),
        static_cast<float>(m_panSlider->value()) / 100.0f,
        static_cast<float>(m_stereoSeparationSlider->value()) / 100.0f,
        m_muteBtn->isChecked(), m_soloBtn->isChecked(),
        m_recBtn->isChecked(), m_monitorBtn->isChecked());
}

void TrackWidget::refreshFromTrack()
{
    if (!m_track) return;
    const QSignalBlocker muteBlock(m_muteBtn);
    const QSignalBlocker soloBlock(m_soloBtn);
    const QSignalBlocker recBlock(m_recBtn);
    const QSignalBlocker monitorBlock(m_monitorBtn);
    const QSignalBlocker volumeBlock(m_volSlider);
    const QSignalBlocker panBlock(m_panSlider);
    const QSignalBlocker separationBlock(m_stereoSeparationSlider);
    m_muteBtn->setChecked(m_track->IsMuted());
    m_soloBtn->setChecked(m_track->IsSoloed());
    m_recBtn->setChecked(m_track->IsArmed());
    m_monitorBtn->setChecked(m_track->IsMonitoring());
    m_volSlider->setValue(gainToFader(m_track->GetVolume()));
    m_panSlider->setValue(qRound(m_track->GetPan() * 100.0f));
    m_stereoSeparationSlider->setValue(qRound(m_track->GetStereoSeparation() * 100.0f));
    m_volSlider->setToolTip(QString("Volume: %1").arg(faderText(m_volSlider->value())));
    m_panSlider->setToolTip(QString("Pan: %1").arg(m_panSlider->value()));
    m_stereoSeparationSlider->setToolTip(
        QString("Stereo separation: %1%").arg(m_stereoSeparationSlider->value()));
    for (int bus = 0; bus < kReturnBusCount; ++bus) {
        const QSignalBlocker sendBlock(m_sendSliders[bus]);
        m_sendSliders[bus]->setValue(gainToFader(m_track->GetSendLevel(bus)));
        m_sendSliders[bus]->setToolTip(QString("Send %1: %2")
            .arg(returnBusLetter(bus)).arg(faderText(m_sendSliders[bus]->value())));
    }
}

void TrackWidget::setSelected(bool selected) {
    m_selected = selected;
    this->setStyleSheet(tileStyle(trackColor(m_track), m_selected));
}

void TrackWidget::mousePressEvent(QMouseEvent* event) {
    emit trackSelected(this, m_track);
    QWidget::mousePressEvent(event);
}
