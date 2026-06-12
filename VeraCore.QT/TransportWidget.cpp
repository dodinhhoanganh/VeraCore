#include "TransportWidget.h"
#include <QFontDatabase>

TransportWidget::TransportWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
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
    m_stopBtn->setStyleSheet(btnStyle + "QPushButton { color: #c8c8c8; font-size: 13px; }");

    m_playBtn = new QPushButton(QString::fromUtf8("\xE2\x96\xB6"), this); // BLACK RIGHT TRIANGLE
    m_playBtn->setToolTip("Play");
    m_playBtn->setStyleSheet(btnStyle + "QPushButton { color: #6fcf73; font-size: 15px; } "
                                        "QPushButton:hover { color: #8fef93; }");

    m_pauseBtn = new QPushButton(QString::fromUtf8("\xE2\x9D\x9A\xE2\x9D\x9A"), this); // HEAVY VERTICAL BARS
    m_pauseBtn->setToolTip("Pause");
    m_pauseBtn->setStyleSheet(btnStyle + "QPushButton { color: #ffc107; font-size: 11px; }");

    m_recordBtn = new QPushButton(QString::fromUtf8("\xE2\x97\x8F"), this); // BLACK CIRCLE
    m_recordBtn->setToolTip("Record");
    m_recordBtn->setCheckable(true);
    m_recordBtn->setStyleSheet(btnStyle +
        "QPushButton { color: #e04a3a; border-radius: 17px; font-size: 14px; } "
        "QPushButton:checked { background-color: #e04a3a; color: white; }");

    transportLayout->addWidget(m_stopBtn);
    transportLayout->addWidget(m_playBtn);
    transportLayout->addWidget(m_pauseBtn);
    transportLayout->addWidget(m_recordBtn);

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

    // 3. Project settings (tempo, signature, metronome, key)
    QHBoxLayout* settingsLayout = new QHBoxLayout();
    settingsLayout->setSpacing(10);

    const QString comboStyle =
        "QComboBox, QDoubleSpinBox { background-color: #2a2a2a; color: #e8e8e8; "
        "border: 1px solid #161616; border-radius: 3px; padding: 2px 5px; height: 24px; } "
        "QComboBox:hover, QDoubleSpinBox:hover { border-color: #ff9020; }";
    const QString labelStyle = "color: #9a9a9a; font-size: 9px; font-weight: bold;";

    // Tempo
    QHBoxLayout* tempoLayout = new QHBoxLayout();
    tempoLayout->setSpacing(4);
    QLabel* tempoLbl = new QLabel("TEMPO", this);
    tempoLbl->setStyleSheet(labelStyle);
    m_tempoSpin = new QDoubleSpinBox(this);
    m_tempoSpin->setRange(20.0, 300.0);
    m_tempoSpin->setValue(120.00);
    m_tempoSpin->setDecimals(2);
    m_tempoSpin->setStyleSheet(comboStyle + "QDoubleSpinBox { width: 64px; }");
    tempoLayout->addWidget(tempoLbl);
    tempoLayout->addWidget(m_tempoSpin);
    settingsLayout->addLayout(tempoLayout);

    // Signature
    QHBoxLayout* sigLayout = new QHBoxLayout();
    sigLayout->setSpacing(4);
    QLabel* sigLbl = new QLabel("SIG", this);
    sigLbl->setStyleSheet(labelStyle);

    m_signatureNum = new QComboBox(this);
    for (int i = 1; i <= 16; ++i) m_signatureNum->addItem(QString::number(i));
    m_signatureNum->setCurrentText("4");
    m_signatureNum->setStyleSheet(comboStyle + "QComboBox { width: 34px; }");

    QLabel* slashLbl = new QLabel("/", this);
    slashLbl->setStyleSheet("color: #777;");

    m_signatureDen = new QComboBox(this);
    m_signatureDen->addItems({"2", "4", "8", "16", "32"});
    m_signatureDen->setCurrentText("4");
    m_signatureDen->setStyleSheet(comboStyle + "QComboBox { width: 34px; }");

    sigLayout->addWidget(sigLbl);
    sigLayout->addWidget(m_signatureNum);
    sigLayout->addWidget(slashLbl);
    sigLayout->addWidget(m_signatureDen);
    settingsLayout->addLayout(sigLayout);

    // Metronome
    m_metronomeBtn = new QPushButton("Click", this);
    m_metronomeBtn->setCheckable(true);
    m_metronomeBtn->setToolTip("Metronome");
    m_metronomeBtn->setStyleSheet(
        "QPushButton { background-color: #2a2a2a; border: 1px solid #161616; border-radius: 3px; "
        "color: #9a9a9a; padding: 2px 10px; height: 24px; } "
        "QPushButton:hover { background-color: #3a3a3a; } "
        "QPushButton:checked { background-color: #ff9020; color: #141414; font-weight: bold; }");
    settingsLayout->addWidget(m_metronomeBtn);

    // Key / Scale
    QHBoxLayout* keyLayout = new QHBoxLayout();
    keyLayout->setSpacing(4);
    QLabel* keyLbl = new QLabel("KEY", this);
    keyLbl->setStyleSheet(labelStyle);

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

    // Single, centered assembly. Each layout is added exactly once.
    mainLayout->addStretch();
    mainLayout->addLayout(transportLayout);
    mainLayout->addSpacing(24);
    mainLayout->addWidget(timeFrame);
    mainLayout->addSpacing(24);
    mainLayout->addLayout(settingsLayout);
    mainLayout->addStretch();
}
