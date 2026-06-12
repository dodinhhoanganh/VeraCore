#pragma once
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
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

signals:
    void trackSelected(TrackWidget* widget, Track* track);

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    Track* m_track;
    bool m_selected = false;

    // UI Elements
    QLabel* m_nameLabel;
    QPushButton* m_muteBtn;
    QPushButton* m_soloBtn;
    QPushButton* m_recBtn;
    QSlider* m_volSlider;
    
    // Slots areas (Moved to Inspector panel in the future)
    void setupUi();
};
