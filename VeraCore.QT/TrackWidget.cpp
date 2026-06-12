#include "TrackWidget.h"
#include <QMouseEvent>
#include <QVariant>

namespace {

// Track identity color shown as a left strip (MIDI teal, audio orange).
QString trackColor(const Track* track)
{
    return (track && track->GetType() == TrackType::Midi) ? "#56b6c2" : "#cf8a4a";
}

QString tileStyle(const QString& color, bool selected)
{
    const QString bg = selected ? "#3d3d3d" : "#2d2d2d";
    const QString border = selected ? "#5a5a5a" : "#1c1c1c";
    return QString("#TrackWidget { background-color: %1; border: 1px solid %2; "
                   "border-bottom: 1px solid #141414; border-left: 3px solid %3; }")
        .arg(bg, border, color);
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
    this->setMinimumWidth(190);
    this->setObjectName("TrackWidget");
    this->setStyleSheet(tileStyle(trackColor(m_track), false));

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 5, 5, 5);
    mainLayout->setSpacing(2);

    // --- Header: name + fast channel controls ---
    QHBoxLayout* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(3);

    m_nameLabel = new QLabel(m_track->GetName(), this);
    m_nameLabel->setStyleSheet("color: #e8e8e8; font-weight: bold; font-size: 12px; "
                               "background: transparent; border: none;");
    m_nameLabel->setMinimumWidth(36);
    headerLayout->addWidget(m_nameLabel);

    headerLayout->addStretch();

    const QString smallBtn =
        "QPushButton { background-color: #3a3a3a; color: #bbb; border: none; "
        "border-radius: 2px; font-size: 10px; } "
        "QPushButton:hover { background-color: #4a4a4a; } ";

    m_muteBtn = new QPushButton("M", this);
    m_muteBtn->setFixedSize(18, 18);
    m_muteBtn->setCheckable(true);
    m_muteBtn->setChecked(m_track->IsMuted());
    m_muteBtn->setToolTip("Mute");
    m_muteBtn->setStyleSheet(smallBtn +
        "QPushButton:checked { background-color: #ffb13b; color: #141414; font-weight: bold; }");

    m_soloBtn = new QPushButton("S", this);
    m_soloBtn->setFixedSize(18, 18);
    m_soloBtn->setCheckable(true);
    m_soloBtn->setChecked(m_track->IsSoloed());
    m_soloBtn->setToolTip("Solo");
    m_soloBtn->setStyleSheet(smallBtn +
        "QPushButton:checked { background-color: #4a90d9; color: #141414; font-weight: bold; }");

    m_recBtn = new QPushButton("R", this);
    m_recBtn->setFixedSize(18, 18);
    m_recBtn->setCheckable(true);
    m_recBtn->setChecked(m_track->IsArmed());
    m_recBtn->setToolTip("Arm for recording");
    m_recBtn->setStyleSheet(smallBtn +
        "QPushButton:checked { background-color: #e04a3a; color: white; font-weight: bold; }");

    m_monitorBtn = new QPushButton("I", this);
    m_monitorBtn->setFixedSize(18, 18);
    m_monitorBtn->setCheckable(true);
    m_monitorBtn->setChecked(m_track->IsMonitoring());
    m_monitorBtn->setToolTip("Input monitor");
    m_monitorBtn->setStyleSheet(smallBtn +
        "QPushButton:checked { background-color: #56b6c2; color: #141414; font-weight: bold; }");

    m_deviceBtn = new QPushButton("Dev", this);
    m_deviceBtn->setFixedSize(28, 18);
    m_deviceBtn->setToolTip(m_track->GetType() == TrackType::Midi ? "Open instrument and devices" : "Open audio devices");
    m_deviceBtn->setStyleSheet(smallBtn + "QPushButton { font-size: 9px; }");

    headerLayout->addWidget(m_muteBtn);
    headerLayout->addWidget(m_soloBtn);
    headerLayout->addWidget(m_recBtn);
    headerLayout->addWidget(m_monitorBtn);
    headerLayout->addWidget(m_deviceBtn);

    mainLayout->addLayout(headerLayout);

    // --- Volume and pan sliders ---
    QHBoxLayout* controlLayout = new QHBoxLayout();
    controlLayout->setSpacing(4);

    QLabel* volIcon = new QLabel("Vol", this);
    volIcon->setStyleSheet("color: #888; font-size: 10px; background: transparent; border: none;");
    m_volSlider = new QSlider(Qt::Horizontal, this);
    m_volSlider->setRange(0, 100);
    m_volSlider->setValue((int)(m_track->GetVolume() * 100));
    m_volSlider->setFixedHeight(12);
    m_volSlider->setStyleSheet(
        "QSlider::groove:horizontal { border: 1px solid #141414; height: 4px; "
        "background: #1c1c1c; margin: 2px 0; border-radius: 2px; } "
        "QSlider::sub-page:horizontal { background: #6a6a6a; border-radius: 2px; } "
        "QSlider::handle:horizontal { background: #9a9a9a; border: 1px solid #555; "
        "width: 8px; margin: -4px 0; border-radius: 2px; } "
        "QSlider::handle:horizontal:hover { background: #ff9020; }");

    QLabel* panIcon = new QLabel("Pan", this);
    panIcon->setStyleSheet("color: #888; font-size: 10px; background: transparent; border: none;");
    m_panSlider = new QSlider(Qt::Horizontal, this);
    m_panSlider->setRange(-100, 100);
    m_panSlider->setValue((int)(m_track->GetPan() * 100));
    m_panSlider->setFixedHeight(12);
    m_panSlider->setStyleSheet(m_volSlider->styleSheet());

    controlLayout->addWidget(volIcon);
    controlLayout->addWidget(m_volSlider, 1);
    controlLayout->addWidget(panIcon);
    controlLayout->addWidget(m_panSlider, 1);
    mainLayout->addLayout(controlLayout);

    connect(m_muteBtn, &QPushButton::toggled, this, [this](bool checked) {
        m_track->SetMuted(checked);
    });
    connect(m_soloBtn, &QPushButton::toggled, this, [this](bool checked) {
        m_track->SetSoloed(checked);
    });
    connect(m_recBtn, &QPushButton::toggled, this, [this](bool checked) {
        m_track->SetArmed(checked);
    });
    connect(m_monitorBtn, &QPushButton::toggled, this, [this](bool checked) {
        m_track->SetMonitoring(checked);
    });
    connect(m_volSlider, &QSlider::valueChanged, this, [this](int value) {
        m_track->SetVolume(static_cast<float>(value) / 100.0f);
    });
    connect(m_panSlider, &QSlider::valueChanged, this, [this](int value) {
        m_track->SetPan(static_cast<float>(value) / 100.0f);
    });
    connect(m_deviceBtn, &QPushButton::clicked, this, [this]() {
        emit trackSelected(this, m_track);
    });

    mainLayout->addStretch();
}

void TrackWidget::setSelected(bool selected) {
    m_selected = selected;
    this->setStyleSheet(tileStyle(trackColor(m_track), m_selected));
}

void TrackWidget::mousePressEvent(QMouseEvent* event) {
    emit trackSelected(this, m_track);
    QWidget::mousePressEvent(event);
}
