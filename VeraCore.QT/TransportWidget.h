#pragma once
#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>

class TransportWidget : public QWidget {
    Q_OBJECT
public:
    explicit TransportWidget(QWidget* parent = nullptr);
    virtual ~TransportWidget() = default;

private:
    // UI Elements
    QPushButton* m_playBtn;
    QPushButton* m_pauseBtn;
    QPushButton* m_stopBtn;
    QPushButton* m_recordBtn;

    QLabel* m_timeDisplay;

    QDoubleSpinBox* m_tempoSpin;
    QComboBox* m_signatureNum;
    QComboBox* m_signatureDen;
    QPushButton* m_metronomeBtn;
    
    QComboBox* m_keyNote;
    QComboBox* m_keyScale;

    void setupUi();
};
