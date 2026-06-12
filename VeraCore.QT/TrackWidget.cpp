#include "TrackWidget.h"
#include <QMouseEvent>
#include <QVariant>

TrackWidget::TrackWidget(Track* track, QWidget* parent)
    : QWidget(parent)
    , m_track(track)
{
    setupUi();
}

void TrackWidget::setupUi() {
    // Styling the track widget
    this->setFixedHeight(60);
    this->setMinimumWidth(150);
    this->setObjectName("TrackWidget");
    this->setStyleSheet("#TrackWidget { border: 1px solid #444; border-bottom: 1px solid #111; background-color: #333333; }");

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(5, 5, 5, 5);
    mainLayout->setSpacing(2);

    // --- Header Section: Name & Controls ---
    QHBoxLayout* headerLayout = new QHBoxLayout();
    
    m_nameLabel = new QLabel(m_track->GetName(), this);
    m_nameLabel->setStyleSheet("color: #E0E0E0; font-weight: bold; font-size: 12px;");
    headerLayout->addWidget(m_nameLabel);

    headerLayout->addStretch();

    m_muteBtn = new QPushButton("M", this);
    m_muteBtn->setFixedSize(18, 18);
    m_muteBtn->setCheckable(true);
    m_muteBtn->setStyleSheet("QPushButton { background-color: #444; color: white; border-radius: 2px; font-size: 10px; } QPushButton:checked { background-color: #d1b22e; color: black; font-weight: bold; }");

    m_soloBtn = new QPushButton("S", this);
    m_soloBtn->setFixedSize(18, 18);
    m_soloBtn->setCheckable(true);
    m_soloBtn->setStyleSheet("QPushButton { background-color: #444; color: white; border-radius: 2px; font-size: 10px; } QPushButton:checked { background-color: #2e8b57; color: black; font-weight: bold; }");

    m_recBtn = new QPushButton("R", this);
    m_recBtn->setFixedSize(18, 18);
    m_recBtn->setCheckable(true);
    m_recBtn->setStyleSheet("QPushButton { background-color: #444; color: white; border-radius: 2px; font-size: 10px; } QPushButton:checked { background-color: #b22222; color: white; font-weight: bold; }");

    headerLayout->addWidget(m_muteBtn);
    headerLayout->addWidget(m_soloBtn);
    headerLayout->addWidget(m_recBtn);

    mainLayout->addLayout(headerLayout);

    // --- Volume Slider ---
    QHBoxLayout* volLayout = new QHBoxLayout();
    QLabel* volIcon = new QLabel("Vol", this);
    volIcon->setStyleSheet("color: #888; font-size: 10px;");
    m_volSlider = new QSlider(Qt::Horizontal, this);
    m_volSlider->setRange(0, 100);
    m_volSlider->setValue((int)(m_track->GetVolume() * 100));
    m_volSlider->setFixedHeight(12);
    m_volSlider->setStyleSheet("QSlider::groove:horizontal { border: 1px solid #222; height: 4px; background: #222; margin: 2px 0; } QSlider::handle:horizontal { background: #777; border: 1px solid #555; width: 8px; margin: -4px 0; border-radius: 2px; }");
    
    volLayout->addWidget(volIcon);
    volLayout->addWidget(m_volSlider);
    mainLayout->addLayout(volLayout);

    mainLayout->addStretch();
}

void TrackWidget::setSelected(bool selected) {
    m_selected = selected;
    if (m_selected) {
        this->setStyleSheet("#TrackWidget { border: 1px solid #888; border-bottom: 1px solid #111; background-color: #4a4a4a; }");
    } else {
        this->setStyleSheet("#TrackWidget { border: 1px solid #444; border-bottom: 1px solid #111; background-color: #333333; }");
    }
}

void TrackWidget::mousePressEvent(QMouseEvent* event) {
    emit trackSelected(this, m_track);
    QWidget::mousePressEvent(event);
}
