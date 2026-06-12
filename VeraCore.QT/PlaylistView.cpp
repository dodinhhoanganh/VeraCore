#include "PlaylistView.h"
#include <QPainter>
#include <QScrollBar>
#include <QStyleOptionGraphicsItem>
#include <qmath.h>

PlaylistView::PlaylistView(QWidget* parent)
    : QGraphicsView(parent),
      m_pixelsPerBeat(40.0),
      m_beatsPerMeasure(4),
      m_trackHeight(60), // Match Inspector/Track Header height
      m_timelineHeight(30),
      m_trackCount(0)
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
    setBackgroundBrush(QColor("#1e1e1e"));
}

PlaylistView::~PlaylistView()
{
}

void PlaylistView::setPixelsPerBeat(double ppb)
{
    m_pixelsPerBeat = qMax(10.0, qMin(ppb, 300.0));
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

void PlaylistView::updateSceneBounds()
{
    double minWidth = viewport()->width();
    double minHeight = qMax((int)viewport()->height(), m_trackCount * m_trackHeight + m_timelineHeight);
    
    QRectF itemsRect = scene()->itemsBoundingRect();
    double finalWidth = qMax(minWidth, itemsRect.right());
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
            painter->fillRect(laneRect, QColor("#222222"));
        } else {
            painter->fillRect(laneRect, QColor("#2a2a2a"));
        }
        
        // Draw bottom border for lane
        painter->setPen(QColor("#111111"));
        painter->drawLine(QPointF(rect.left(), yPos + m_trackHeight - 1), QPointF(rect.right(), yPos + m_trackHeight - 1));
    }

    // 2. Draw vertical grid lines (beats and measures)
    double measureWidth = m_pixelsPerBeat * m_beatsPerMeasure;
    
    int startBeat = qFloor(rect.left() / m_pixelsPerBeat);
    int endBeat = qCeil(rect.right() / m_pixelsPerBeat);
    
    // Configure pens
    QPen measurePen(QColor("#555555"), 1);
    QPen beatPen(QColor("#333333"), 1);
    
    // Sub-beat dots
    QVector<qreal> dashes;
    dashes << 1 << 3;
    QPen subBeatPen(QColor("#333333"), 1);
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
    painter->fillRect(timelineRect, QColor("#1e1e1e"));
    
    // Draw bottom border
    painter->setPen(QColor("#444444"));
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
