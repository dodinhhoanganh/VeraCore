#pragma once
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QPoint>
#include <QString>
#include <memory>
#include <vector>
#include "Track.h"

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
    void setTracks(const std::vector<std::shared_ptr<Track>>& tracks);

    void updateSceneBounds();

signals:
    void trackLaneSelected(int trackIndex);
    void midiClipSelected(int trackIndex, const QString& name, double startBeat, double lengthBeats);
    void midiClipOpened(int trackIndex, const QString& name, double startBeat, double lengthBeats);

protected:
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    struct MidiClip {
        int trackIndex;
        double startBeat;
        double lengthBeats;
        QString name;
    };

    double m_pixelsPerBeat;
    int m_beatsPerMeasure;
    int m_trackHeight;
    int m_timelineHeight;
    int m_trackCount;
    QGraphicsScene* m_scene;
    std::vector<Track*> m_tracks;
    std::vector<MidiClip> m_midiClips;
    int m_selectedClipIndex;

    bool m_pendingClipDrag;
    bool m_draggingClipCreate;
    int m_dragTrackIndex;
    double m_dragStartBeat;
    double m_dragCurrentBeat;
    QPoint m_dragStartPos;

    int trackIndexAt(const QPointF& scenePos) const;
    bool isMidiTrack(int trackIndex) const;
    double beatAtX(double sceneX) const;
    double snapBeat(double beat) const;
    QRectF clipRect(const MidiClip& clip) const;
    int clipAt(const QPointF& scenePos) const;
    int createMidiClip(int trackIndex, double startBeat, double lengthBeats);
    void selectClip(int clipIndex, bool opened);
    void clearClipDrag();
};
