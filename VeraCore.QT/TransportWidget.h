#pragma once
#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QElapsedTimer>
#include <cstdint>
#include <vector>
#include "MidiClipModel.h"

class QResizeEvent;

class TransportWidget : public QWidget {
    Q_OBJECT
public:
    explicit TransportWidget(QWidget* parent = nullptr);
    virtual ~TransportWidget() = default;
    double tempo() const;
    void setTempo(double bpm);
    int signatureNumerator() const;
    int signatureDenominator() const;
    bool metronomeEnabled() const;
    int countInBars() const;
    int keyRoot() const;
    bool minorScale() const;
    double snapBeats() const;
    int ppq() const;
    void setTimeSignature(int numerator, int denominator);
    void setTimeSignatureMap(const std::vector<MidiTimeSignaturePoint>& points);
    void setKeyScale(int rootNote, bool minor);
    void setSnapBeats(double beats);
    void setPpq(int ppq);
    void setPlaying(bool playing);
    void setRecording(bool recording);
    void setPunchEnabled(bool enabled);
    void setCountInBars(int bars);
    void togglePunch();
    void setPositionBeats(double beat);
    void setMeterPeaks(float left, float right, float momentaryLufs,
                       float shortTermLufs, float integratedLufs,
                       float loudnessRangeLu);
    void setAudioHealth(uint64_t callbackCount, double cpuLoad, uint64_t inputUnderflows,
        uint64_t inputOverflows, uint64_t outputUnderflows,
        uint64_t outputOverflows, uint64_t primingOutputs);
    void setAudioDeviceStatus(bool active, const QString& detail);

signals:
    void playRequested();
    void pauseRequested();
    void stopRequested();
    void recordToggled(bool enabled);
    void punchToggled(bool enabled);
    void metronomeToggled(bool enabled);
    void countInBarsChanged(int bars);
    void keyScaleChanged(int rootNote, bool minor);
    void tempoChanged(double bpm);
    void timeSignatureChanged(int numerator, int denominator);
    void snapChanged(double beats);
    void ppqChanged(int ppq);
    void audioHealthResetRequested();
    void audioRecoveryRequested();

private:
    // UI Elements
    QPushButton* m_playBtn;
    QPushButton* m_pauseBtn;
    QPushButton* m_stopBtn;
    QPushButton* m_recordBtn;
    QPushButton* m_punchBtn;

    QLabel* m_timeDisplay;
    QLabel* m_meterDisplay;
    QElapsedTimer m_meterClock;
    qint64 m_meterLastUpdateMs = 0;
    qint64 m_meterHoldUntilMs = 0;
    double m_meterHeldDb = -120.0;
    QPushButton* m_audioHealthBtn;
    bool m_audioDeviceActive = true;

    QWidget* m_tempoGroup;
    QWidget* m_signatureGroup;
    QWidget* m_keyGroup;
    QWidget* m_snapGroup;
    QWidget* m_ppqGroup;
    QWidget* m_countInGroup = nullptr;

    QDoubleSpinBox* m_tempoSpin;
    QComboBox* m_signatureNum;
    QComboBox* m_signatureDen;
    QPushButton* m_metronomeBtn;
    QComboBox* m_countInCombo = nullptr;

    QComboBox* m_keyNote;
    QComboBox* m_keyScale;
    QComboBox* m_snapCombo;
    QSpinBox* m_ppqSpin;
    std::vector<MidiTimeSignaturePoint> m_timeSignatureMap{{0.0, 4, 4}};
    double m_positionBeat = 0.0;

    void setupUi();
    void updateResponsiveLayout(int width);
    void resizeEvent(QResizeEvent* event) override;
};
