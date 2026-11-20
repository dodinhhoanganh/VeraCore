#pragma once
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QDial>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include "Track.h"

class TrackWidget : public QWidget {
    Q_OBJECT
public:
    explicit TrackWidget(Track* track, QWidget* parent = nullptr);
    virtual ~TrackWidget() = default;

    Track* GetTrack() const { return m_track; }
    void setSelected(bool selected);
    void refreshFromTrack();

signals:
    void trackSelected(TrackWidget* widget, Track* track);
    void mixerControlsEdited(Track* track, float volume, float pan, float stereoSeparation,
                             bool muted, bool soloed, bool armed, bool monitoring);
    void trackSendEdited(Track* track, int returnBus, float level);

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    Track* m_track;
    bool m_selected = false;

    // UI Elements
    QLabel* m_nameLabel;
    QLabel* m_typeLabel;
    QPushButton* m_muteBtn;
    QPushButton* m_soloBtn;
    QPushButton* m_recBtn;
    QPushButton* m_monitorBtn;
    QPushButton* m_deviceBtn;
    QSlider* m_volSlider;
    QSlider* m_panSlider;
    QDial* m_stereoSeparationSlider;
    std::array<QSlider*, kReturnBusCount> m_sendSliders{};
    
    // Slots areas (Moved to Inspector panel in the future)
    void setupUi();
    void emitMixerControlsEdited();
};
