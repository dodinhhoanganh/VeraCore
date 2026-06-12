#include "TransportWidget.h"
#include <QFontDatabase>

TransportWidget::TransportWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

void TransportWidget::setupUi() {
    this->setFixedHeight(50);
    this->setObjectName("TransportWidget");
    this->setStyleSheet("#TransportWidget { background-color: #2b2b2b; border-bottom: 1px solid #111; }");

    QHBoxLayout* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(10, 5, 10, 5);
    mainLayout->setSpacing(20);

    // 1. Transport Controls (Play, Pause, Stop, Record)
    QHBoxLayout* transportLayout = new QHBoxLayout();
    transportLayout->setSpacing(5);

    QString btnStyle = "QPushButton { background-color: #3a3a3a; border: 1px solid #222; border-radius: 3px; color: #eee; font-weight: bold; width: 30px; height: 30px; } "
                       "QPushButton:hover { background-color: #4a4a4a; } "
                       "QPushButton:pressed { background-color: #222; }";

    m_playBtn = new QPushButton(">", this);
    m_playBtn->setStyleSheet(btnStyle + "QPushButton { color: #4CAF50; font-size: 16px; }");
    
    m_pauseBtn = new QPushButton("||", this);
    m_pauseBtn->setStyleSheet(btnStyle + "QPushButton { color: #FFC107; }");
    
    m_stopBtn = new QPushButton("[]", this);
    m_stopBtn->setStyleSheet(btnStyle + "QPushButton { color: #F44336; }");

    m_recordBtn = new QPushButton("O", this);
    m_recordBtn->setStyleSheet(btnStyle + "QPushButton { color: #F44336; border-radius: 15px; font-size: 14px; } QPushButton:checked { background-color: #F44336; color: white; }");
    m_recordBtn->setCheckable(true);

    transportLayout->addWidget(m_stopBtn);
    transportLayout->addWidget(m_playBtn);
    transportLayout->addWidget(m_pauseBtn);
    transportLayout->addWidget(m_recordBtn);

    mainLayout->addLayout(transportLayout);

    // 2. Time Display (LCD style)
    QFrame* timeFrame = new QFrame(this);
    timeFrame->setStyleSheet("QFrame { background-color: #1a1a1a; border: 1px inset #111; border-radius: 3px; padding: 2px 10px; }");
    QHBoxLayout* timeLayout = new QHBoxLayout(timeFrame);
    timeLayout->setContentsMargins(0, 0, 0, 0);

    m_timeDisplay = new QLabel(" 1. 1. 1. 00 ", timeFrame);
    m_timeDisplay->setStyleSheet("color: #00FFCC; font-family: 'Consolas', monospace; font-size: 20px; font-weight: bold;");
    timeLayout->addWidget(m_timeDisplay);

    mainLayout->addWidget(timeFrame);

    // 3. Project Settings (Tempo, Signature, Metronome, Key)
    QHBoxLayout* settingsLayout = new QHBoxLayout();
    settingsLayout->setSpacing(10);
    
    QString comboStyle = "QComboBox, QDoubleSpinBox { background-color: #333; color: white; border: 1px solid #222; border-radius: 2px; padding: 2px 5px; height: 24px; }";

    // Tempo
    QHBoxLayout* tempoLayout = new QHBoxLayout();
    tempoLayout->setSpacing(2);
    QLabel* tempoLbl = new QLabel("TEMPO", this);
    tempoLbl->setStyleSheet("color: #aaa; font-size: 9px; font-weight: bold;");
    m_tempoSpin = new QDoubleSpinBox(this);
    m_tempoSpin->setRange(20.0, 300.0);
    m_tempoSpin->setValue(120.00);
    m_tempoSpin->setDecimals(2);
    m_tempoSpin->setStyleSheet(comboStyle + "QDoubleSpinBox { width: 60px; }");
    tempoLayout->addWidget(tempoLbl);
    tempoLayout->addWidget(m_tempoSpin);
    settingsLayout->addLayout(tempoLayout);

    // Signature
    QHBoxLayout* sigLayout = new QHBoxLayout();
    sigLayout->setSpacing(2);
    QLabel* sigLbl = new QLabel("SIGNATURE", this);
    sigLbl->setStyleSheet("color: #aaa; font-size: 9px; font-weight: bold;");
    
    m_signatureNum = new QComboBox(this);
    for (int i=1; i<=16; ++i) m_signatureNum->addItem(QString::number(i));
    m_signatureNum->setCurrentText("4");
    m_signatureNum->setStyleSheet(comboStyle + "QComboBox { width: 30px; }");

    QLabel* slashLbl = new QLabel("/", this);
    slashLbl->setStyleSheet("color: #888;");

    m_signatureDen = new QComboBox(this);
    m_signatureDen->addItems({"2", "4", "8", "16", "32"});
    m_signatureDen->setCurrentText("4");
    m_signatureDen->setStyleSheet(comboStyle + "QComboBox { width: 30px; }");

    sigLayout->addWidget(sigLbl);
    sigLayout->addWidget(m_signatureNum);
    sigLayout->addWidget(slashLbl);
    sigLayout->addWidget(m_signatureDen);
    settingsLayout->addLayout(sigLayout);

    // Metronome
    m_metronomeBtn = new QPushButton("Click", this);
    m_metronomeBtn->setCheckable(true);
    m_metronomeBtn->setStyleSheet("QPushButton { background-color: #3a3a3a; border: 1px solid #222; border-radius: 2px; color: #aaa; padding: 2px 8px; height: 24px; } "
                                  "QPushButton:checked { background-color: #4CAF50; color: white; font-weight: bold; }");
    settingsLayout->addWidget(m_metronomeBtn);

    // Key / Scale
    QHBoxLayout* keyLayout = new QHBoxLayout();
    keyLayout->setSpacing(2);
    QLabel* keyLbl = new QLabel("KEY", this);
    keyLbl->setStyleSheet("color: #aaa; font-size: 9px; font-weight: bold;");
    
    m_keyNote = new QComboBox(this);
    m_keyNote->addItems({"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});
    m_keyNote->setStyleSheet(comboStyle);
    
    m_keyScale = new QComboBox(this);
    m_keyScale->addItems({"Major", "Minor"});
    m_keyScale->setStyleSheet(comboStyle);

    keyLayout->addWidget(keyLbl);
    keyLayout->addWidget(m_keyNote);
    keyLayout->addWidget(m_keyScale);
    settingsLayout->addLayout(keyLayout);

    // Center everything
    mainLayout->addStretch();
    mainLayout->addLayout(transportLayout);
    mainLayout->addSpacing(30);
    mainLayout->addWidget(timeFrame);
    mainLayout->addSpacing(30);
    mainLayout->addLayout(settingsLayout);
    mainLayout->addStretch();
}
