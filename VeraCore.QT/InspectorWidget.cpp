#include "InspectorWidget.h"
#include <QHBoxLayout>
#include <QFrame>

InspectorWidget::InspectorWidget(QWidget* parent)
    : QWidget(parent), m_currentTrack(nullptr), m_showingMidiClip(false)
{
    setupUi();
}

void InspectorWidget::setupUi() {
    // Horizontal device strip docked under the arrangement (Ableton-style):
    // [ track tile | Instrument | Audio FX | Output ]
    this->setMinimumHeight(120);
    this->setMaximumHeight(220);
    this->setObjectName("InspectorWidget");
    this->setStyleSheet("#InspectorWidget { background-color: #232323; border-top: 1px solid #0d0d0d; }");

    QHBoxLayout* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    const QString groupStyle =
        "QGroupBox { color: #9a9a9a; font-size: 10px; font-weight: bold; "
        "background-color: #2a2a2a; border: 1px solid #161616; border-radius: 4px; "
        "margin-top: 16px; } "
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: left top; "
        "left: 6px; top: 2px; background: transparent; border: none; padding: 0; }";

    // Track tile: selected track identity, FL-channel-button flavour.
    QFrame* trackTile = new QFrame(this);
    trackTile->setObjectName("inspectorTrackTile");
    trackTile->setFixedWidth(170);
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
    tileLayout->addStretch();

    m_contextLabel = new QLabel("TRACK", trackTile);
    m_contextLabel->setStyleSheet("color: #777; font-size: 9px; font-weight: bold; "
                                  "background: transparent; border: none;");
    tileLayout->addWidget(m_contextLabel);

    mainLayout->addWidget(trackTile);

    // MIDI clip details appear when an arrangement clip is selected/opened.
    m_clipGroup = new QGroupBox("MIDI CLIP", this);
    m_clipGroup->setStyleSheet(groupStyle);
    m_clipGroup->setFixedWidth(260);
    QVBoxLayout* clipLayout = new QVBoxLayout(m_clipGroup);
    clipLayout->setContentsMargins(8, 8, 8, 8);
    clipLayout->setSpacing(6);

    m_clipStartLabel = new QLabel("Start: 1.1", m_clipGroup);
    m_clipStartLabel->setStyleSheet("color: #ccc; background-color: #333; padding: 6px 8px; "
                                    "border: 1px solid #161616; border-radius: 3px;");
    m_clipLengthLabel = new QLabel("Length: 4 beats", m_clipGroup);
    m_clipLengthLabel->setStyleSheet(m_clipStartLabel->styleSheet());
    QPushButton* openPianoRollBtn = new QPushButton("Open MIDI Editor", m_clipGroup);
    openPianoRollBtn->setEnabled(false);
    openPianoRollBtn->setToolTip("MIDI editor integration target");
    openPianoRollBtn->setStyleSheet("QPushButton { text-align: center; background-color: #2f2f2f; "
                                    "color: #777; border: 1px solid #161616; padding: 6px 8px; "
                                    "border-radius: 3px; }");
    clipLayout->addWidget(m_clipStartLabel);
    clipLayout->addWidget(m_clipLengthLabel);
    clipLayout->addWidget(openPianoRollBtn);
    clipLayout->addStretch();
    mainLayout->addWidget(m_clipGroup);

    // Instrument slot (MIDI tracks only).
    m_instrumentGroup = new QGroupBox("INSTRUMENT", this);
    m_instrumentGroup->setStyleSheet(groupStyle);
    m_instrumentGroup->setFixedWidth(220);
    m_instrumentLayout = new QVBoxLayout(m_instrumentGroup);
    m_instrumentLayout->setContentsMargins(8, 8, 8, 8);
    m_instrumentLayout->setSpacing(4);

    m_instrumentBtn = new QPushButton("Empty Instrument", m_instrumentGroup);
    m_instrumentBtn->setStyleSheet("QPushButton { text-align: left; background-color: #333; "
                                   "color: #ddd; border: 1px solid #161616; padding: 6px 8px; "
                                   "border-radius: 3px; } "
                                   "QPushButton:hover { border-color: #ff9020; color: #fff; }");
    m_instrumentLayout->addWidget(m_instrumentBtn);
    m_instrumentLayout->addStretch();
    mainLayout->addWidget(m_instrumentGroup);

    // FX chain grows with available width like Ableton's device chain.
    m_fxGroup = new QGroupBox("AUDIO FX", this);
    m_fxGroup->setStyleSheet(groupStyle);
    m_fxLayout = new QVBoxLayout(m_fxGroup);
    m_fxLayout->setContentsMargins(8, 8, 8, 8);
    m_fxLayout->setSpacing(4);

    m_addFxBtn = new QPushButton("+ Add FX", m_fxGroup);
    m_addFxBtn->setStyleSheet("QPushButton { text-align: center; background-color: transparent; "
                              "color: #999; border: 1px dashed #4a4a4a; padding: 6px 8px; "
                              "border-radius: 3px; } "
                              "QPushButton:hover { border-color: #ff9020; color: #ff9020; }");
    m_fxLayout->addWidget(m_addFxBtn);
    m_fxLayout->addStretch();
    mainLayout->addWidget(m_fxGroup, 1);

    // Output routing.
    m_outputGroup = new QGroupBox("OUTPUT", this);
    m_outputGroup->setStyleSheet(groupStyle);
    m_outputGroup->setFixedWidth(170);
    QVBoxLayout* outLayout = new QVBoxLayout(m_outputGroup);
    outLayout->setContentsMargins(8, 8, 8, 8);
    outLayout->setSpacing(4);
    m_outputLabel = new QLabel("Stereo Out", m_outputGroup);
    m_outputLabel->setStyleSheet("color: #ccc; background-color: #333; padding: 6px 8px; "
                                 "border: 1px solid #161616; border-radius: 3px;");
    outLayout->addWidget(m_outputLabel);
    outLayout->addStretch();
    mainLayout->addWidget(m_outputGroup);

    updateUi();
}

void InspectorWidget::setTrack(Track* track) {
    m_currentTrack = track;
    m_showingMidiClip = false;
    updateUi();
}

void InspectorWidget::setMidiClip(const QString& name, double startBeat, double lengthBeats) {
    m_showingMidiClip = true;
    m_trackNameLabel->setText(name);

    const int startMeasure = (int)(startBeat / 4.0) + 1;
    const int startBeatInMeasure = ((int)startBeat % 4) + 1;
    m_clipStartLabel->setText(QString("Start: %1.%2").arg(startMeasure).arg(startBeatInMeasure));
    m_clipLengthLabel->setText(QString("Length: %1 beats").arg(lengthBeats, 0, 'f', 0));

    updateUi();
}

void InspectorWidget::updateUi() {
    if (!m_currentTrack) {
        m_trackNameLabel->setText("No Track Selected");
        m_contextLabel->setText("TRACK");
        m_clipGroup->setVisible(false);
        m_instrumentGroup->setVisible(false);
        m_fxGroup->setVisible(false);
        m_outputGroup->setVisible(false);
        return;
    }

    if (m_showingMidiClip) {
        m_contextLabel->setText("CLIP");
        m_clipGroup->setVisible(true);
        m_instrumentGroup->setVisible(false);
        m_fxGroup->setVisible(false);
        m_outputGroup->setVisible(false);
        return;
    }

    m_trackNameLabel->setText(m_currentTrack->GetName());
    m_contextLabel->setText("TRACK");
    m_clipGroup->setVisible(false);

    m_instrumentGroup->setVisible(m_currentTrack->GetType() == TrackType::Midi);
    m_fxGroup->setVisible(true);
    m_outputGroup->setVisible(true);
}
