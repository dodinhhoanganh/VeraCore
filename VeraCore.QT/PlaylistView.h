#pragma once
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QWheelEvent>

class PlaylistView : public QGraphicsView
{
    Q_OBJECT

public:
    explicit PlaylistView(QWidget* parent = nullptr);
    ~PlaylistView();

    void setPixelsPerBeat(double ppb);
    double getPixelsPerBeat() const;

    void setTrackCount(int count);
    int getTrackCount() const;

    void updateSceneBounds();

protected:
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    double m_pixelsPerBeat;
    int m_beatsPerMeasure;
    int m_trackHeight;
    int m_timelineHeight;
    int m_trackCount;
    QGraphicsScene* m_scene;
};
