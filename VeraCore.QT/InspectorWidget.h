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

private:
    Track* m_currentTrack;

    QLabel* m_trackNameLabel;
    
    QGroupBox* m_instrumentGroup;
    QVBoxLayout* m_instrumentLayout;
    QPushButton* m_instrumentBtn;

    QGroupBox* m_fxGroup;
    QVBoxLayout* m_fxLayout;
    QPushButton* m_addFxBtn;

    QGroupBox* m_outputGroup;
    QLabel* m_outputLabel;

    void setupUi();
    void updateUi();
};
