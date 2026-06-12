#include "InspectorWidget.h"
#include <QScrollArea>

InspectorWidget::InspectorWidget(QWidget* parent)
    : QWidget(parent), m_currentTrack(nullptr)
{
    setupUi();
}

void InspectorWidget::setupUi() {
    this->setMinimumWidth(180);
    this->setMaximumWidth(250);
    this->setObjectName("InspectorWidget");
    this->setStyleSheet("#InspectorWidget { background-color: #2b2b2b; border-right: 1px solid #111; }");

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(5, 5, 5, 5);
    mainLayout->setSpacing(10);

    // Track Name
    m_trackNameLabel = new QLabel("No Track Selected", this);
    m_trackNameLabel->setStyleSheet("color: #E0E0E0; font-weight: bold; font-size: 14px; padding: 5px; background-color: #333; border: 1px solid #444; border-radius: 3px; text-align: center;");
    m_trackNameLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_trackNameLabel);

    // Instrument Group
    m_instrumentGroup = new QGroupBox("Instrument", this);
    m_instrumentGroup->setStyleSheet("QGroupBox { color: #888; font-size: 11px; border: 1px solid #444; border-radius: 3px; margin-top: 15px;} QGroupBox::title { subcontrol-origin: margin; left: 5px; top: -5px; }");
    m_instrumentLayout = new QVBoxLayout(m_instrumentGroup);
    m_instrumentLayout->setContentsMargins(5, 15, 5, 5);
    
    m_instrumentBtn = new QPushButton("Empty Instrument", this);
    m_instrumentBtn->setStyleSheet("text-align: left; background-color: #3b3b3b; color: #ddd; border: 1px solid #222; padding: 4px; border-radius: 2px;");
    m_instrumentLayout->addWidget(m_instrumentBtn);
    mainLayout->addWidget(m_instrumentGroup);

    // Audio FX Group
    m_fxGroup = new QGroupBox("Audio FX", this);
    m_fxGroup->setStyleSheet("QGroupBox { color: #888; font-size: 11px; border: 1px solid #444; border-radius: 3px; margin-top: 15px;} QGroupBox::title { subcontrol-origin: margin; left: 5px; top: -5px; }");
    m_fxLayout = new QVBoxLayout(m_fxGroup);
    m_fxLayout->setContentsMargins(5, 15, 5, 5);
    
    m_addFxBtn = new QPushButton("+ Add FX", this);
    m_addFxBtn->setStyleSheet("text-align: center; background-color: transparent; color: #aaa; border: 1px dashed #555; padding: 4px; border-radius: 2px;");
    m_fxLayout->addWidget(m_addFxBtn);
    mainLayout->addWidget(m_fxGroup);

    // Output Group
    m_outputGroup = new QGroupBox("Output", this);
    m_outputGroup->setStyleSheet("QGroupBox { color: #888; font-size: 11px; border: 1px solid #444; border-radius: 3px; margin-top: 15px;} QGroupBox::title { subcontrol-origin: margin; left: 5px; top: -5px; }");
    QVBoxLayout* outLayout = new QVBoxLayout(m_outputGroup);
    outLayout->setContentsMargins(5, 15, 5, 5);
    m_outputLabel = new QLabel("Stereo Out", this);
    m_outputLabel->setStyleSheet("color: #ccc; background-color: #333; padding: 4px; border: 1px solid #222; border-radius: 2px;");
    outLayout->addWidget(m_outputLabel);
    mainLayout->addWidget(m_outputGroup);

    mainLayout->addStretch();

    updateUi();
}

void InspectorWidget::setTrack(Track* track) {
    m_currentTrack = track;
    updateUi();
}

void InspectorWidget::updateUi() {
    if (!m_currentTrack) {
        m_trackNameLabel->setText("No Track Selected");
        m_instrumentGroup->setVisible(false);
        m_fxGroup->setVisible(false);
        m_outputGroup->setVisible(false);
        return;
    }

    m_trackNameLabel->setText(m_currentTrack->GetName());
    
    if (m_currentTrack->GetType() == TrackType::Midi) {
        m_instrumentGroup->setVisible(true);
    } else {
        m_instrumentGroup->setVisible(false);
    }

    m_fxGroup->setVisible(true);
    m_outputGroup->setVisible(true);
}
