#pragma once
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QPushButton>
#include "Track.h"

class InspectorWidget : public QWidget {
    Q_OBJECT
public:
    explicit InspectorWidget(QWidget* parent = nullptr);
    virtual ~InspectorWidget() = default;

    void setTrack(Track* track);
    void setMidiClip(const QString& name, double startBeat, double lengthBeats);

private:
    Track* m_currentTrack;
    bool m_showingMidiClip;

    QLabel* m_trackNameLabel;
    QLabel* m_contextLabel;
    
    QGroupBox* m_instrumentGroup;
    QVBoxLayout* m_instrumentLayout;
    QPushButton* m_instrumentBtn;

    QGroupBox* m_fxGroup;
    QVBoxLayout* m_fxLayout;
    QPushButton* m_addFxBtn;

    QGroupBox* m_outputGroup;
    QLabel* m_outputLabel;

    QGroupBox* m_clipGroup;
    QLabel* m_clipStartLabel;
    QLabel* m_clipLengthLabel;

    void setupUi();
    void updateUi();
};
