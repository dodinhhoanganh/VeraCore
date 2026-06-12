#include "PlaylistView.h"
#include <QPainter>
#include <QScrollBar>
#include <QStyleOptionGraphicsItem>
#include <qmath.h>
#include <algorithm>

PlaylistView::PlaylistView(QWidget* parent)
    : QGraphicsView(parent),
      m_pixelsPerBeat(40.0),
      m_beatsPerMeasure(4),
      m_trackHeight(60), // Match Inspector/Track Header height
      m_timelineHeight(30),
      m_trackCount(0),
      m_selectedClipIndex(-1),
      m_pendingClipDrag(false),
      m_draggingClipCreate(false),
      m_dragTrackIndex(-1),
      m_dragStartBeat(0.0),
      m_dragCurrentBeat(0.0)
{
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);
    
    setAlignment(Qt::AlignLeft | Qt::AlignTop);

    // Disable default scrollbars, we will manage them if needed, a DAW workspace
    setRenderHint(QPainter::Antialiasing, false); // For crispy grid lines
    setRenderHint(QPainter::TextAntialiasing, true);
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate); // Necessary since foreground depends on scroll
    setFrameShape(QFrame::NoFrame);
    
    // Set darker background color
    setBackgroundBrush(QColor("#1b1b1b"));
}

PlaylistView::~PlaylistView()
{
}

void PlaylistView::setPixelsPerBeat(double ppb)
{
    m_pixelsPerBeat = qMax(10.0, qMin(ppb, 300.0));
    updateSceneBounds();
    viewport()->update();
}

double PlaylistView::getPixelsPerBeat() const
{
    return m_pixelsPerBeat;
}

void PlaylistView::setTrackCount(int count)
{
    m_trackCount = count;
    updateSceneBounds();
    viewport()->update();
}

int PlaylistView::getTrackCount() const
{
    return m_trackCount;
}

void PlaylistView::setTracks(const std::vector<std::shared_ptr<Track>>& tracks)
{
    m_tracks.clear();
    m_tracks.reserve(tracks.size());
    for (const auto& track : tracks) {
        m_tracks.push_back(track.get());
    }
    setTrackCount((int)m_tracks.size());
}

void PlaylistView::updateSceneBounds()
{
    double minWidth = viewport()->width();
    double minHeight = qMax((int)viewport()->height(), m_trackCount * m_trackHeight + m_timelineHeight);
    
    QRectF itemsRect = scene()->itemsBoundingRect();
    double clipRight = 0.0;
    for (const MidiClip& clip : m_midiClips) {
        clipRight = qMax(clipRight, (clip.startBeat + clip.lengthBeats) * m_pixelsPerBeat + 120.0);
    }

    double finalWidth = qMax(qMax(minWidth, itemsRect.right()), clipRight);
    double finalHeight = qMax(minHeight, itemsRect.bottom());
    
    setSceneRect(0, 0, finalWidth, finalHeight);
}

void PlaylistView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    updateSceneBounds();
}

void PlaylistView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        // Zoom functionality
        double numDegrees = event->angleDelta().y() / 8.0;
        double numSteps = numDegrees / 15.0; // Typical mouse step is 15 degrees
        
        // Exponential zoom mapping
        double factor = qPow(1.1, numSteps);
        setPixelsPerBeat(m_pixelsPerBeat * factor);
        
        event->accept();
    } else {
        // Normal scroll
        QGraphicsView::wheelEvent(event);
    }
}

void PlaylistView::drawBackground(QPainter* painter, const QRectF& rect)
{
    // Save state
    painter->save();

    // The timeline sits at the top (visually). We offset the lanes.
    // However, the scene's y=0 is at the very top. 
    // We should reserve 30px at the top of the SCENE for the timeline?
    // No, drawForeground uses viewport coordinates and ALWAYS overlays the top 30px of the viewport.
    // Therefore, the scene's y=0 to y=30 will be permanently hidden by the timeline.
    // We must offset our track backgrounds by m_timelineHeight.

    int startTrack = qFloor((rect.top() - m_timelineHeight) / m_trackHeight);
    int endTrack = qCeil((rect.bottom() - m_timelineHeight) / m_trackHeight);
    
    // Limit endTrack to m_trackCount so we don't draw lanes for non-existent tracks
    endTrack = qMin(endTrack, m_trackCount - 1);

    for (int i = qMax(0, startTrack); i <= endTrack; ++i) {
        double yPos = m_timelineHeight + i * m_trackHeight;
        QRectF laneRect(rect.left(), yPos, rect.width(), m_trackHeight);
        
        if (i % 2 == 0) {
            painter->fillRect(laneRect, QColor("#242424"));
        } else {
            painter->fillRect(laneRect, QColor("#282828"));
        }

        // Draw bottom border for lane
        painter->setPen(QColor("#151515"));
        painter->drawLine(QPointF(rect.left(), yPos + m_trackHeight - 1), QPointF(rect.right(), yPos + m_trackHeight - 1));
    }

    // 2. Draw vertical grid lines (beats and measures)
    double measureWidth = m_pixelsPerBeat * m_beatsPerMeasure;
    
    int startBeat = qFloor(rect.left() / m_pixelsPerBeat);
    int endBeat = qCeil(rect.right() / m_pixelsPerBeat);
    
    // Configure pens
    QPen measurePen(QColor("#4a4a4a"), 1);
    QPen beatPen(QColor("#303030"), 1);

    // Sub-beat dots
    QVector<qreal> dashes;
    dashes << 1 << 3;
    QPen subBeatPen(QColor("#303030"), 1);
    subBeatPen.setDashPattern(dashes);
    
    for (int b = qMax(0, startBeat); b <= endBeat; ++b) {
        double x = b * m_pixelsPerBeat;
        
        if (b % m_beatsPerMeasure == 0) {
            // Measure line (thick)
            painter->setPen(measurePen);
            painter->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
        } else {
            // Beat line (thin)
            painter->setPen(beatPen);
            painter->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
        }
        
        // Draw 16th sub-beats if zoomed in enough
        if (m_pixelsPerBeat > 60.0) {
            double subBeatWidth = m_pixelsPerBeat / 4.0;
            painter->setPen(subBeatPen);
            for (int i = 1; i < 4; ++i) {
                double subX = x + i * subBeatWidth;
                painter->drawLine(QPointF(subX, rect.top()), QPointF(subX, rect.bottom()));
            }
        }
    }

    QFont clipFont = painter->font();
    clipFont.setPointSize(8);
    clipFont.setBold(true);
    painter->setFont(clipFont);

    for (int i = 0; i < (int)m_midiClips.size(); ++i) {
        const MidiClip& clip = m_midiClips[i];
        QRectF r = clipRect(clip);
        if (!rect.intersects(r)) {
            continue;
        }

        const bool selected = (i == m_selectedClipIndex);
        painter->setPen(QPen(selected ? QColor("#ff9020") : QColor("#2d6f78"), selected ? 2 : 1));
        painter->setBrush(QColor(selected ? "#346f78" : "#255f68"));
        painter->drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);

        painter->setPen(QColor("#dff7fa"));
        painter->drawText(r.adjusted(8, 2, -6, -2), Qt::AlignLeft | Qt::AlignVCenter, clip.name);
    }

    if (m_draggingClipCreate && m_dragTrackIndex >= 0) {
        double start = qMin(m_dragStartBeat, m_dragCurrentBeat);
        double end = qMax(m_dragStartBeat, m_dragCurrentBeat);
        if (end - start < 1.0) {
            end = start + 1.0;
        }

        MidiClip preview{m_dragTrackIndex, start, end - start, "MIDI"};
        QRectF r = clipRect(preview);
        painter->setPen(QPen(QColor("#ff9020"), 1, Qt::DashLine));
        painter->setBrush(QColor(86, 182, 194, 70));
        painter->drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    }

    painter->restore();
}

void PlaylistView::drawForeground(QPainter* painter, const QRectF& rect)
{
    painter->save();

    // Reset transform to draw in VIEWPORT coordinates so timeline stays fixed at the top
    painter->setTransform(QTransform());

    // We only care about the top section of the viewport
    QRect viewportRect = viewport()->rect();
    QRect timelineRect(0, 0, viewportRect.width(), m_timelineHeight);

    // Fill background for timeline
    painter->fillRect(timelineRect, QColor("#202020"));

    // Draw bottom border
    painter->setPen(QColor("#000000"));
    painter->drawLine(0, m_timelineHeight - 1, viewportRect.width(), m_timelineHeight - 1);

    // Calculate scroll offset to map viewport X to scene X
    double scrollX = horizontalScrollBar()->value();
    
    int startBeat = qFloor(scrollX / m_pixelsPerBeat);
    int endBeat = qCeil((scrollX + viewportRect.width()) / m_pixelsPerBeat);
    
    painter->setPen(QColor("#888888"));
    QFont font = painter->font();
    font.setPointSize(8);
    painter->setFont(font);

    for (int b = qMax(0, startBeat); b <= endBeat; ++b) {
        double sceneX = b * m_pixelsPerBeat;
        double viewX = sceneX - scrollX;
        
        if (b % m_beatsPerMeasure == 0) {
            // Measure tick
            int measureNumber = (b / m_beatsPerMeasure) + 1;
            painter->drawLine(QPointF(viewX, m_timelineHeight - 10), QPointF(viewX, m_timelineHeight));
            painter->drawText(QRectF(viewX + 4, 0, 50, m_timelineHeight - 5), Qt::AlignLeft | Qt::AlignBottom, QString::number(measureNumber));
        } else {
            // Beat tick
            painter->drawLine(QPointF(viewX, m_timelineHeight - 5), QPointF(viewX, m_timelineHeight));
            
            // Draw detailed beat numbers (1.2, 1.3, etc) if there is enough space
            if (m_pixelsPerBeat > 30.0) {
                int measureNumber = (b / m_beatsPerMeasure) + 1;
                int beatNumber = (b % m_beatsPerMeasure) + 1;
                painter->drawText(QRectF(viewX + 4, 0, 50, m_timelineHeight - 5), Qt::AlignLeft | Qt::AlignBottom, QString("%1.%2").arg(measureNumber).arg(beatNumber));
            }
        }
        
        // Draw 16th sub-beats if zoomed in enough
        if (m_pixelsPerBeat > 60.0) {
            double subBeatWidth = m_pixelsPerBeat / 4.0;
            for (int i = 1; i < 4; ++i) {
                double subViewX = viewX + i * subBeatWidth;
                painter->drawLine(QPointF(subViewX, m_timelineHeight - 3), QPointF(subViewX, m_timelineHeight));
            }
        }
    }

    painter->restore();
}

void PlaylistView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QGraphicsView::mousePressEvent(event);
        return;
    }

    const QPointF scenePos = mapToScene(event->pos());
    const int clipIndex = clipAt(scenePos);
    if (clipIndex >= 0) {
        selectClip(clipIndex, false);
        event->accept();
        return;
    }

    const int trackIndex = trackIndexAt(scenePos);
    if (trackIndex >= 0) {
        m_selectedClipIndex = -1;
        emit trackLaneSelected(trackIndex);

        if (isMidiTrack(trackIndex)) {
            m_pendingClipDrag = true;
            m_draggingClipCreate = false;
            m_dragTrackIndex = trackIndex;
            m_dragStartBeat = snapBeat(beatAtX(scenePos.x()));
            m_dragCurrentBeat = m_dragStartBeat;
            m_dragStartPos = event->pos();
        }

        viewport()->update();
        event->accept();
        return;
    }

    clearClipDrag();
    QGraphicsView::mousePressEvent(event);
}

void PlaylistView::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_pendingClipDrag || !(event->buttons() & Qt::LeftButton)) {
        QGraphicsView::mouseMoveEvent(event);
        return;
    }

    if ((event->pos() - m_dragStartPos).manhattanLength() > 3) {
        m_draggingClipCreate = true;
    }

    if (m_draggingClipCreate) {
        m_dragCurrentBeat = snapBeat(beatAtX(mapToScene(event->pos()).x()));
        viewport()->update();
    }

    event->accept();
}

void PlaylistView::mouseReleaseEvent(QMouseEvent* event)
{
    if (!m_pendingClipDrag || event->button() != Qt::LeftButton) {
        QGraphicsView::mouseReleaseEvent(event);
        return;
    }

    if (m_draggingClipCreate && m_dragTrackIndex >= 0) {
        double start = qMin(m_dragStartBeat, m_dragCurrentBeat);
        double end = qMax(m_dragStartBeat, m_dragCurrentBeat);
        if (end - start < 1.0) {
            end = start + 1.0;
        }

        const int clipIndex = createMidiClip(m_dragTrackIndex, start, end - start);
        selectClip(clipIndex, false);
    }

    clearClipDrag();
    event->accept();
}

void PlaylistView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QGraphicsView::mouseDoubleClickEvent(event);
        return;
    }

    const QPointF scenePos = mapToScene(event->pos());
    const int clipIndex = clipAt(scenePos);
    if (clipIndex >= 0) {
        selectClip(clipIndex, true);
        event->accept();
        return;
    }

    const int trackIndex = trackIndexAt(scenePos);
    if (isMidiTrack(trackIndex)) {
        const double startBeat = snapBeat(beatAtX(scenePos.x()));
        const int newClipIndex = createMidiClip(trackIndex, startBeat, 4.0);
        selectClip(newClipIndex, true);
        event->accept();
        return;
    }

    QGraphicsView::mouseDoubleClickEvent(event);
}

int PlaylistView::trackIndexAt(const QPointF& scenePos) const
{
    const double laneY = scenePos.y() - m_timelineHeight;
    if (laneY < 0.0) {
        return -1;
    }

    const int trackIndex = qFloor(laneY / m_trackHeight);
    return (trackIndex >= 0 && trackIndex < m_trackCount) ? trackIndex : -1;
}

bool PlaylistView::isMidiTrack(int trackIndex) const
{
    return trackIndex >= 0 &&
           trackIndex < (int)m_tracks.size() &&
           m_tracks[trackIndex] &&
           m_tracks[trackIndex]->GetType() == TrackType::Midi;
}

double PlaylistView::beatAtX(double sceneX) const
{
    return qMax(0.0, sceneX / m_pixelsPerBeat);
}

double PlaylistView::snapBeat(double beat) const
{
    return qMax(0.0, static_cast<double>(qRound(beat)));
}

QRectF PlaylistView::clipRect(const MidiClip& clip) const
{
    const double x = clip.startBeat * m_pixelsPerBeat;
    const double y = m_timelineHeight + clip.trackIndex * m_trackHeight + 7;
    const double w = qMax(1.0, clip.lengthBeats * m_pixelsPerBeat);
    const double h = m_trackHeight - 14;
    return QRectF(x, y, w, h);
}

int PlaylistView::clipAt(const QPointF& scenePos) const
{
    for (int i = (int)m_midiClips.size() - 1; i >= 0; --i) {
        if (clipRect(m_midiClips[i]).contains(scenePos)) {
            return i;
        }
    }
    return -1;
}

int PlaylistView::createMidiClip(int trackIndex, double startBeat, double lengthBeats)
{
    if (!isMidiTrack(trackIndex)) {
        return -1;
    }

    MidiClip clip;
    clip.trackIndex = trackIndex;
    clip.startBeat = qMax(0.0, startBeat);
    clip.lengthBeats = qMax(1.0, lengthBeats);
    clip.name = QString("MIDI %1").arg((int)m_midiClips.size() + 1);
    m_midiClips.push_back(clip);

    updateSceneBounds();
    viewport()->update();
    return (int)m_midiClips.size() - 1;
}

void PlaylistView::selectClip(int clipIndex, bool opened)
{
    if (clipIndex < 0 || clipIndex >= (int)m_midiClips.size()) {
        return;
    }

    m_selectedClipIndex = clipIndex;
    const MidiClip& clip = m_midiClips[clipIndex];
    emit trackLaneSelected(clip.trackIndex);
    emit midiClipSelected(clip.trackIndex, clip.name, clip.startBeat, clip.lengthBeats);
    if (opened) {
        emit midiClipOpened(clip.trackIndex, clip.name, clip.startBeat, clip.lengthBeats);
    }
    viewport()->update();
}

void PlaylistView::clearClipDrag()
{
    m_pendingClipDrag = false;
    m_draggingClipCreate = false;
    m_dragTrackIndex = -1;
    m_dragStartBeat = 0.0;
    m_dragCurrentBeat = 0.0;
}
