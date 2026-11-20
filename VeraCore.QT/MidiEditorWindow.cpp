#include "MidiEditorWindow.h"

#include <QAbstractScrollArea>
#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QFont>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QRectF>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QToolButton>
#include <QToolTip>
#include <QTransform>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr int kKeyboardWidth = 132;
constexpr int kNoteHeight = 18;
constexpr int kKeyswitchRows = 12;
constexpr int kKeyswitchRowHeight = 14;
constexpr int kRulerHeight = 28;
constexpr int kTopPadding = kRulerHeight + 6;
constexpr int kVelocityLaneHeight = 92;
constexpr int kBottomPadding = 20;
constexpr double kDefaultSnap = 0.25;
constexpr double kMinNoteLength = 1.0 / 480.0;
constexpr double kDefaultPixelsPerBeat = 48.0;

std::vector<MidiNote> g_noteClipboard;
double g_noteClipboardSpanBeats = 0.0;

enum class NotePropertyTarget
{
    Velocity,
    Pan,
    Release,
    ModX,
    ModY
};

enum class PianoRollTool
{
    Draw,
    Paint,
    Stamp,
    Select,
    Delete,
    Mute,
    Slice
};

QColor noteColor(int group)
{
    static const QColor colors[16] = {
        QColor("#7ee0a0"), QColor("#8bd2ff"), QColor("#ffcc66"), QColor("#ff8a7a"),
        QColor("#c39bff"), QColor("#f48fc8"), QColor("#8fe388"), QColor("#57d2c8"),
        QColor("#f6ec72"), QColor("#f0a15a"), QColor("#9aa8ff"), QColor("#d4d4d4"),
        QColor("#66b36a"), QColor("#4a9fd9"), QColor("#d46a6a"), QColor("#9e7ad8")
    };
    return colors[qBound(0, group, 15)];
}

QString propertyTargetName(NotePropertyTarget target)
{
    switch (target) {
    case NotePropertyTarget::Velocity: return "Velocity";
    case NotePropertyTarget::Pan: return "Pan";
    case NotePropertyTarget::Release: return "Release";
    case NotePropertyTarget::ModX: return "Mod X";
    case NotePropertyTarget::ModY: return "Mod Y";
    }
    return "Velocity";
}

QString noteName(int pitch)
{
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int octave = pitch / 12;
    int index = ((pitch % 12) + 12) % 12;
    return QString("%1%2").arg(names[index]).arg(octave);
}

bool isBlackKey(int pitch)
{
    switch (((pitch % 12) + 12) % 12) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10:
        return true;
    default:
        return false;
    }
}

double clampBeat(double beat)
{
    return std::isfinite(beat) && beat > 0.0 ? beat : 0.0;
}

double safeNoteLength(double length)
{
    return std::isfinite(length) && length > 0.0 ? std::max(kMinNoteLength, length) : 1.0;
}

int pianoRowCount()
{
    return kPianoRollMaxPitch - kPianoRollMinPitch + 1;
}

int clampPianoPitch(int pitch)
{
    return qBound(kPianoRollMinPitch, pitch, kPianoRollMaxPitch);
}

}

class MidiEditorWindow::PianoRollCanvas : public QAbstractScrollArea
{
public:
    explicit PianoRollCanvas(QWidget* parent = nullptr)
        : QAbstractScrollArea(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        viewport()->setMouseTracking(true);
        setFrameShape(QFrame::NoFrame);
        horizontalScrollBar()->setSingleStep(12);
        verticalScrollBar()->setSingleStep(kNoteHeight);
    }

    void setClip(MidiClipDocument* clip)
    {
        stopPreviewNote();
        m_clip = clip;
        m_editBeforeDocument.reset();
        rebuildTimeSignatureMap();
        m_selectedNote = -1;
        m_activeNote = -1;
        m_activeResize = false;
        m_creatingNote = false;
        m_createDragged = false;
        m_selecting = false;
        m_slicing = false;
        m_muting = false;
        m_mutedDragNotes.clear();
        m_createIsKeyswitch = false;
        m_editingProperty = false;
        m_propertyNote = -1;
        m_erasingRight = false;
        m_hoverExpressionRow = -1;
        m_focusedExpressionRow = -1;
        m_createCurrentBeat = 0.0;
        m_selectionStart = QPointF();
        m_selectionCurrent = QPointF();
        m_selectionAdditive = false;
        m_lastCreatedNote = -1;
        m_undoHistory.clear();
        m_redoHistory.clear();
        if (m_clip && std::isfinite(m_clip->lastNoteLengthBeats) && m_clip->lastNoteLengthBeats > 0.0) {
            m_lastNoteLengthBeats = safeNoteLength(m_clip->lastNoteLengthBeats);
        }
        m_nextGroupId = 1;
        if (m_clip) {
            for (const MidiNote& note : m_clip->notes) {
                m_nextGroupId = std::max(m_nextGroupId, note.groupId + 1);
            }
        }
        rebuildExpressionRows();
        updateScrollRanges();
        viewport()->update();
    }

    void setGhostClips(std::vector<MidiGhostClip> clips)
    {
        m_ghostClips = std::move(clips);
        viewport()->update();
    }

    void setExpressionMap(std::vector<MidiExpressionMapEntry> entries)
    {
        m_pluginExpressionMap = std::move(entries);
        rebuildExpressionRows();
        updateScrollRanges();
        viewport()->update();
    }

    void setChangeCallback(std::function<void(const MidiClipDocument&,
                                               const MidiClipDocument&,
                                               quint64)> callback)
    {
        m_changeCallback = std::move(callback);
    }

    void setUndoRequestCallback(std::function<void()> callback)
    {
        m_undoRequestCallback = std::move(callback);
    }

    void setRedoRequestCallback(std::function<void()> callback)
    {
        m_redoRequestCallback = std::move(callback);
    }

    void setSnapBeats(double snapBeats)
    {
        m_snapToLine = std::isfinite(snapBeats) && snapBeats <= 0.0;
        m_snapBeats = std::isfinite(snapBeats) && snapBeats > 0.0
            ? std::max(1.0 / 64.0, snapBeats)
            : kDefaultSnap;
        viewport()->update();
    }

    void setScaleSnap(bool enabled, int rootNote, bool minor)
    {
        m_snapToScale = enabled;
        m_scaleRoot = ((rootNote % 12) + 12) % 12;
        m_scaleMinor = minor;
        viewport()->update();
    }

    void setStampChord(std::vector<int> intervals, bool diatonic)
    {
        m_stampIntervals = std::move(intervals);
        m_stampDiatonic = diatonic;
    }

    void setCurrentColorGroup(int group)
    {
        const int safeGroup = qBound(0, group, 15);
        const bool changed = m_currentColorGroup != safeGroup;
        m_currentColorGroup = safeGroup;
        if (m_colorGroupChangedCallback) {
            m_colorGroupChangedCallback(safeGroup);
        }
        if (!changed) {
            return;
        }
        viewport()->update();
    }

    void setSlideMode(bool enabled)
    {
        const bool changed = m_slideMode != enabled;
        if (changed) {
            m_slideMode = enabled;
        }
        if (m_slideModeChangedCallback) {
            m_slideModeChangedCallback(enabled);
        }
    }

    void setPortamentoMode(bool enabled)
    {
        const bool changed = m_portamentoMode != enabled;
        if (changed) {
            m_portamentoMode = enabled;
        }
        if (m_portamentoModeChangedCallback) {
            m_portamentoModeChangedCallback(enabled);
        }
    }

    void setGhostNotesVisible(bool visible)
    {
        const bool changed = m_showGhostNotes != visible;
        if (changed) {
            m_showGhostNotes = visible;
        }
        if (m_ghostNotesVisibleChangedCallback) {
            m_ghostNotesVisibleChangedCallback(visible);
        }
        if (changed) {
            viewport()->update();
        }
    }

    void setPropertyTarget(NotePropertyTarget target)
    {
        const bool changed = m_propertyTarget != target;
        if (changed) {
            m_propertyTarget = target;
        }
        if (m_propertyTargetChangedCallback) {
            m_propertyTargetChangedCallback(target);
        }
        if (changed) {
            viewport()->update();
        }
    }

    void setToolMode(PianoRollTool toolMode)
    {
        if (m_toolMode != toolMode) {
            m_toolMode = toolMode;
            m_slicing = false;
            m_muting = false;
            m_mutedDragNotes.clear();
            viewport()->update();
        }
        if (m_toolModeChangedCallback) {
            m_toolModeChangedCallback(toolMode);
        }
    }

    void setToolModeChangedCallback(std::function<void(PianoRollTool)> callback)
    {
        m_toolModeChangedCallback = std::move(callback);
    }

    void setSlideModeChangedCallback(std::function<void(bool)> callback)
    {
        m_slideModeChangedCallback = std::move(callback);
    }

    void setPortamentoModeChangedCallback(std::function<void(bool)> callback)
    {
        m_portamentoModeChangedCallback = std::move(callback);
    }

    void setGhostNotesVisibleChangedCallback(std::function<void(bool)> callback)
    {
        m_ghostNotesVisibleChangedCallback = std::move(callback);
    }

    void setPropertyTargetChangedCallback(std::function<void(NotePropertyTarget)> callback)
    {
        m_propertyTargetChangedCallback = std::move(callback);
    }

    void setColorGroupChangedCallback(std::function<void(int)> callback)
    {
        m_colorGroupChangedCallback = std::move(callback);
    }

    void setPlayheadSeekCallback(std::function<void(double)> callback)
    {
        m_playheadSeekCallback = std::move(callback);
    }

    void setNotePreviewCallback(std::function<void(int, int, bool, bool)> callback)
    {
        m_notePreviewCallback = std::move(callback);
    }

    void setPlayheadBeat(double relativeBeat)
    {
        m_playheadBeat = std::isfinite(relativeBeat) && relativeBeat >= 0.0
            ? relativeBeat : -1.0;
        viewport()->update();
    }

    void zoomBy(double factor)
    {
        zoomAt(factor, viewport()->width() * 0.5);
    }

    void setPixelsPerBeat(double value)
    {
        m_pixelsPerBeat = std::isfinite(value) ? qBound(24.0, value, 160.0) : kDefaultPixelsPerBeat;
        updateScrollRanges();
        viewport()->update();
    }

    double pixelsPerBeat() const { return m_pixelsPerBeat; }
    PianoRollTool toolMode() const { return m_toolMode; }

    void zoomAt(double factor, double viewportX)
    {
        const int oldScroll = horizontalScrollBar()->value();
        const double safeX = qBound(0.0, viewportX, static_cast<double>(viewport()->width()));
        const double focusBeat = std::max(0.0, (oldScroll + safeX - kKeyboardWidth) / m_pixelsPerBeat);
        m_pixelsPerBeat = std::max(24.0, std::min(160.0, m_pixelsPerBeat * factor));
        updateScrollRanges();
        horizontalScrollBar()->setValue(static_cast<int>(focusBeat * m_pixelsPerBeat + kKeyboardWidth - safeX));
        viewport()->update();
    }

    void commandSelectAll()
    {
        selectAllNotes();
    }

    void commandDuplicateSelection()
    {
        duplicateSelection();
    }

    void commandCopySelection()
    {
        copySelection();
    }

    void commandCutSelection()
    {
        cutSelection();
    }

    void commandPasteSelection()
    {
        pasteSelection();
    }

    void commandQuantizeSelection()
    {
        quantizeSelection();
    }

    void commandQuickLegato()
    {
        quickLegatoSelection();
    }

protected:
    bool viewportEvent(QEvent* event) override
    {
        if (event->type() == QEvent::ToolTip && m_clip) {
            auto* helpEvent = static_cast<QHelpEvent*>(event);
            const QPointF contentPos = viewportToContent(helpEvent->pos());
            if (helpEvent->pos().x() < kKeyboardWidth && isInKeyswitchLane(contentPos)) {
                const int row = keyswitchRowFromY(contentPos.y());
                const int pitch = keyswitchPitchForRow(row);
                const int channel = keyswitchChannelForRow(row);
                const QString state = hasExpressionAtPlayhead(row)
                    ? QStringLiteral("Assigned at playhead")
                    : QStringLiteral("Not assigned at playhead");
                const QString text = QStringLiteral(
                    "<b>%1</b><br>%2 &middot; MIDI Ch %3<br>%4<br>"
                    "Click: audition  |  Shift+Click: insert  |  Right-click: remove<br>"
                    "Keys: [ / ] audition  |  Shift+Enter: insert  |  Shift+Delete: remove")
                    .arg(expressionNameForPitchChannel(pitch, channel), noteName(pitch))
                    .arg(channel + 1)
                    .arg(state);
                QToolTip::showText(helpEvent->globalPos(), text, viewport());
                event->accept();
                return true;
            }
            QToolTip::hideText();
            event->ignore();
            return true;
        }
        return QAbstractScrollArea::viewportEvent(event);
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QAbstractScrollArea::resizeEvent(event);
        updateScrollRanges();
    }

    void leaveEvent(QEvent* event) override
    {
        setHoveredExpressionRow(-1);
        viewport()->unsetCursor();
        QAbstractScrollArea::leaveEvent(event);
    }

    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event);
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillRect(viewport()->rect(), QColor("#1a1a1a"));

        if (!m_clip) {
            painter.setPen(QColor("#888"));
            painter.drawText(viewport()->rect(), Qt::AlignCenter, "No MIDI clip selected");
            return;
        }

        const int hScroll = horizontalScrollBar()->value();
        const int vScroll = verticalScrollBar()->value();
        const QRect visible = viewport()->rect();

        drawKeyboard(&painter, visible, vScroll);
        drawGrid(&painter, visible, hScroll, vScroll);
        drawNotes(&painter, visible, hScroll, vScroll);
        drawVelocityLane(&painter, visible, hScroll, vScroll);
        drawOverlay(&painter, hScroll, vScroll);
        drawPlayhead(&painter, hScroll);
        drawRuler(&painter, hScroll);

        painter.setPen(QColor("#090909"));
        painter.drawLine(kKeyboardWidth - 1, 0, kKeyboardWidth - 1, visible.height());
        painter.drawLine(0, keyswitchTop() - vScroll - 1,
                         visible.width(), keyswitchTop() - vScroll - 1);
        painter.drawLine(0, velocityTop() - vScroll - 1,
                         visible.width(), velocityTop() - vScroll - 1);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (!m_clip) {
            return;
        }

        if (event->button() == Qt::MiddleButton) {
            m_panning = true;
            m_panStartPos = event->pos();
            m_panStartH = horizontalScrollBar()->value();
            m_panStartV = verticalScrollBar()->value();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }

        if (event->button() == Qt::RightButton) {
            const QPointF contentPos = viewportToContent(event->pos());
            if (event->pos().x() < kKeyboardWidth && isInKeyswitchLane(contentPos)) {
                const int row = keyswitchRowFromY(contentPos.y());
                setFocusedExpressionRow(row, false);
                removeExpressionAtPlayhead(row);
                event->accept();
                return;
            }
            m_erasingRight = true;
            pushUndoState();
            int index = noteAt(event->pos());
            if (index >= 0) {
                deleteNote(index);
            }
            event->accept();
            return;
        }

        if (event->button() != Qt::LeftButton) {
            QAbstractScrollArea::mousePressEvent(event);
            return;
        }

        const QPointF contentPos = viewportToContent(event->pos());
        if (event->pos().y() < kRulerHeight && event->pos().x() >= kKeyboardWidth) {
            const double beat = clampBeat((contentPos.x() - kKeyboardWidth) / m_pixelsPerBeat);
            setPlayheadBeat(beat);
            if (m_playheadSeekCallback) m_playheadSeekCallback(beat);
            event->accept();
            return;
        }
        if (event->pos().x() < kKeyboardWidth &&
            contentPos.y() >= kTopPadding && contentPos.y() < keyswitchTop()) {
            startPreviewNote(pitchFromY(contentPos.y()));
            event->accept();
            return;
        }
        if (event->pos().x() < kKeyboardWidth && isInKeyswitchLane(contentPos)) {
            const int row = keyswitchRowFromY(contentPos.y());
            setFocusedExpressionRow(row, false);
            if (event->modifiers() & Qt::ShiftModifier) {
                insertExpressionAtPlayhead(row);
                event->accept();
                return;
            }
            startPreviewNote(keyswitchPitchForRow(row), keyswitchChannelForRow(row), true);
            event->accept();
            return;
        }
        if (isInEventLane(contentPos)) {
            const int propertyIndex = noteAtEventLane(contentPos);
            if (propertyIndex >= 0) {
                pushUndoState();
                selectNote(propertyIndex, false);
                m_editingProperty = true;
                m_propertyNote = propertyIndex;
                setNotePropertyFromY(m_clip->notes[propertyIndex], contentPos.y());
                viewport()->update();
                notifyChanged();
                event->accept();
                return;
            }
        }

        if (m_toolMode == PianoRollTool::Slice) {
            if (contentPos.x() >= kKeyboardWidth && contentPos.y() >= kTopPadding &&
                contentPos.y() < velocityTop()) {
                m_slicing = true;
                m_sliceStart = contentPos;
                m_sliceCurrent = contentPos;
                m_sliceBypassSnap = (event->modifiers() & Qt::AltModifier) != 0;
                viewport()->update();
            }
            event->accept();
            return;
        }

        const int index = m_toolMode == PianoRollTool::Stamp ? -1 : noteAt(event->pos());
        if (m_toolMode == PianoRollTool::Mute) {
            if (index >= 0) {
                pushUndoState();
                m_muting = true;
                m_muteValue = !m_clip->notes[index].muted;
                m_mutedDragNotes.clear();
                m_clip->notes[index].muted = m_muteValue;
                m_mutedDragNotes.push_back(index);
                viewport()->update();
            }
            event->accept();
            return;
        }
        if (m_toolMode == PianoRollTool::Delete) {
            if (index >= 0) {
                pushUndoState();
                deleteNote(index);
            }
            event->accept();
            return;
        }
        if (index >= 0) {
            pushUndoState();
            const bool additive = (event->modifiers() &
                (Qt::ShiftModifier | Qt::ControlModifier)) != Qt::NoModifier;
            selectNote(index, additive);
            int dragIndex = index;
            if ((event->modifiers() & Qt::ShiftModifier) &&
                !(event->modifiers() & Qt::ControlModifier)) {
                dragIndex = cloneSelectionForDrag(index);
            }
            if (dragIndex < 0 || dragIndex >= static_cast<int>(m_clip->notes.size())) return;
            const QRectF rect = noteRect(m_clip->notes[dragIndex]);
            const QPointF contentPos = viewportToContent(event->pos());
            const bool nearResizeEdge = std::abs(contentPos.x() - rect.right()) <= 6.0;
            m_activeNote = dragIndex;
            m_activeResize = nearResizeEdge;
            m_dragStartPoint = contentPos;
            m_dragStartBeat = m_clip->notes[dragIndex].startBeat;
            m_dragStartPitch = m_clip->notes[dragIndex].pitch;
            m_dragStartLength = m_clip->notes[dragIndex].lengthBeats;
            captureDragSelection();
            m_noteOffsetX = contentPos.x() - rect.left();
            m_noteOffsetY = contentPos.y() - rect.top();
            viewport()->update();
            return;
        }

        if (contentPos.x() < kKeyboardWidth) {
            return;
        }

        const bool inKeyswitch = isInKeyswitchLane(contentPos);
        const bool inNoteGrid = contentPos.y() >= kTopPadding && contentPos.y() < keyswitchTop();
        if (!inNoteGrid && !inKeyswitch) {
            return;
        }
        if (m_toolMode == PianoRollTool::Select ||
            (event->modifiers() & Qt::ControlModifier)) {
            m_selecting = true;
            m_selectionStart = contentPos;
            m_selectionCurrent = contentPos;
            m_selectionAdditive =
                (event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) != Qt::NoModifier;
            if (!m_selectionAdditive) {
                clearSelection();
            }
            m_selectedNote = -1;
            viewport()->update();
            event->accept();
            return;
        }

        const int keyswitchRow = inKeyswitch ? keyswitchRowFromY(contentPos.y()) : -1;
        const int pitch = inKeyswitch
            ? keyswitchPitchForRow(keyswitchRow)
            : snapPitchToScale(pitchFromY(contentPos.y()));
        const bool bypassSnap = (event->modifiers() & Qt::AltModifier) != 0;
        const double rawStartBeat = (contentPos.x() - kKeyboardWidth) / m_pixelsPerBeat;
        const double startBeat = bypassSnap ? clampBeat(rawStartBeat) : snapBeat(rawStartBeat);

        pushUndoState();
        m_creatingNote = true;
        m_createIsKeyswitch = inKeyswitch;
        m_createPitch = pitch;
        m_createChannel = inKeyswitch ? keyswitchChannelForRow(keyswitchRow) : m_currentColorGroup;
        m_createStartBeat = startBeat;
        m_createCurrentBeat = startBeat;
        m_createLengthBeats = std::max(m_lastNoteLengthBeats, kMinNoteLength);
        m_createDragged = false;
        m_dragStartPoint = contentPos;
        m_activeNote = -1;
        viewport()->update();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_previewPitch >= 0 && (event->buttons() & Qt::LeftButton)) {
            const QPointF contentPos = viewportToContent(event->pos());
            if (event->pos().x() < kKeyboardWidth &&
                contentPos.y() >= kTopPadding && contentPos.y() < keyswitchTop()) {
                startPreviewNote(pitchFromY(contentPos.y()));
            } else if (event->pos().x() < kKeyboardWidth && isInKeyswitchLane(contentPos)) {
                const int row = keyswitchRowFromY(contentPos.y());
                startPreviewNote(keyswitchPitchForRow(row), keyswitchChannelForRow(row), true);
            }
            event->accept();
            return;
        }
        if (m_panning) {
            const QPoint delta = event->pos() - m_panStartPos;
            horizontalScrollBar()->setValue(m_panStartH - delta.x());
            verticalScrollBar()->setValue(m_panStartV - delta.y());
            event->accept();
            return;
        }

        if (!m_clip) {
            return;
        }

        if (m_erasingRight && (event->buttons() & Qt::RightButton)) {
            const int index = noteAt(event->pos());
            if (index >= 0) deleteNote(index);
            event->accept();
            return;
        }

        if (m_editingProperty && m_propertyNote >= 0 &&
            m_propertyNote < static_cast<int>(m_clip->notes.size())) {
            const QPointF contentPos = viewportToContent(event->pos());
            setNotePropertyFromY(m_clip->notes[m_propertyNote], contentPos.y());
            viewport()->update();
            notifyChanged();
            event->accept();
            return;
        }

        if (m_slicing && (event->buttons() & Qt::LeftButton)) {
            m_sliceCurrent = viewportToContent(event->pos());
            viewport()->update();
            event->accept();
            return;
        }

        if (m_muting && (event->buttons() & Qt::LeftButton)) {
            const int index = noteAt(event->pos());
            if (index >= 0 && std::find(m_mutedDragNotes.begin(), m_mutedDragNotes.end(), index) ==
                    m_mutedDragNotes.end()) {
                m_clip->notes[index].muted = m_muteValue;
                m_mutedDragNotes.push_back(index);
                viewport()->update();
            }
            event->accept();
            return;
        }

        if (m_creatingNote) {
            const QPointF contentPos = viewportToContent(event->pos());
            const double rawBeat = (contentPos.x() - kKeyboardWidth) / m_pixelsPerBeat;
            const double beat = (event->modifiers() & Qt::AltModifier)
                ? clampBeat(rawBeat) : snapBeat(rawBeat);
            m_createCurrentBeat = beat;
            const double length = std::max(kMinNoteLength, std::abs(beat - m_createStartBeat));
            if (std::abs(contentPos.x() - m_dragStartPoint.x()) >= 3.0) m_createDragged = true;
            if (m_createDragged) m_createLengthBeats = length;
            viewport()->update();
            event->accept();
            return;
        }

        if (m_selecting) {
            m_selectionCurrent = viewportToContent(event->pos());
            viewport()->update();
            event->accept();
            return;
        }

        if (m_activeNote >= 0 && (event->buttons() & Qt::LeftButton)) {
            const QPointF contentPos = viewportToContent(event->pos());
            if (m_activeResize) {
                const double rawBeat = (contentPos.x() - kKeyboardWidth) / m_pixelsPerBeat;
                const double beat = (event->modifiers() & Qt::AltModifier)
                    ? clampBeat(rawBeat) : snapBeat(rawBeat);
                const double lengthDelta = std::max(kMinNoteLength, beat - m_dragStartBeat) - m_dragStartLength;
                for (const DragNoteSnapshot& snapshot : m_dragSnapshots) {
                    if (snapshot.index < 0 || snapshot.index >= static_cast<int>(m_clip->notes.size())) {
                        continue;
                    }
                    MidiNote& note = m_clip->notes[snapshot.index];
                    note.lengthBeats = safeNoteLength(snapshot.lengthBeats + lengthDelta);
                }
            } else {
                const double beatDelta = (contentPos.x() - m_dragStartPoint.x()) / m_pixelsPerBeat;
                const double targetStart = (event->modifiers() & Qt::AltModifier)
                    ? clampBeat(m_dragStartBeat + beatDelta)
                    : snapBeat(m_dragStartBeat + beatDelta);
                double snappedDelta = targetStart - m_dragStartBeat;
                double earliestBeat = m_dragStartBeat;
                for (const DragNoteSnapshot& snapshot : m_dragSnapshots)
                    earliestBeat = std::min(earliestBeat, snapshot.startBeat);
                snappedDelta = std::max(snappedDelta, -earliestBeat);
                const int pitchDelta = (m_dragStartPoint.y() - contentPos.y()) / kNoteHeight;
                const int keyswitchDelta = (m_dragStartPoint.y() - contentPos.y()) / kKeyswitchRowHeight;
                for (const DragNoteSnapshot& snapshot : m_dragSnapshots) {
                    if (snapshot.index < 0 || snapshot.index >= static_cast<int>(m_clip->notes.size())) {
                        continue;
                    }
                    MidiNote& note = m_clip->notes[snapshot.index];
                    note.startBeat = clampBeat(snapshot.startBeat + snappedDelta);
                    if (note.isKeyswitch) {
                        const int targetRow = keyswitchRowForPitchChannel(
                            snapshot.pitch, snapshot.colorGroup) + keyswitchDelta;
                        note.pitch = keyswitchPitchForRow(targetRow);
                        note.colorGroup = keyswitchChannelForRow(targetRow);
                    } else {
                        note.pitch = snapPitchToScale(clampPianoPitch(snapshot.pitch + pitchDelta));
                    }
                }
            }
            viewport()->update();
            event->accept();
            return;
        }

        const QPointF hoverContentPos = viewportToContent(event->pos());
        if (event->buttons() == Qt::NoButton && event->pos().x() < kKeyboardWidth &&
            isInKeyswitchLane(hoverContentPos)) {
            setHoveredExpressionRow(keyswitchRowFromY(hoverContentPos.y()));
            viewport()->setCursor(Qt::PointingHandCursor);
        } else {
            setHoveredExpressionRow(-1);
            viewport()->unsetCursor();
        }
        QAbstractScrollArea::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_previewPitch >= 0) {
            stopPreviewNote();
            event->accept();
            return;
        }
        if (event->button() == Qt::RightButton && m_erasingRight) {
            m_erasingRight = false;
            event->accept();
            return;
        }

        if (event->button() == Qt::MiddleButton && m_panning) {
            m_panning = false;
            unsetCursor();
            event->accept();
            return;
        }

        if (!m_clip) {
            return;
        }

        if (event->button() == Qt::LeftButton && m_creatingNote) {
            MidiNote note;
            note.isKeyswitch = m_createIsKeyswitch;
            note.pitch = m_createIsKeyswitch ? qBound(0, m_createPitch, 127)
                                             : clampPianoPitch(m_createPitch);
            const double createStart = std::min(m_createStartBeat, m_createCurrentBeat);
            const double createEnd = std::max(m_createStartBeat, m_createCurrentBeat);
            note.startBeat = clampBeat(createStart);
            note.lengthBeats = safeNoteLength(std::max(m_createLengthBeats, createEnd - createStart));
            note.velocity = 100;
            note.colorGroup = note.isKeyswitch ? m_createChannel : m_currentColorGroup;
            note.isSlide = m_slideMode && !note.isKeyswitch;
            note.isPortamento = m_portamentoMode && !note.isKeyswitch;
            note.selected = true;
            clearSelection();
            double createdNoteLength = note.lengthBeats;
            if (m_toolMode == PianoRollTool::Stamp && !note.isKeyswitch) {
                note.isSlide = false;
                note.isPortamento = false;
                const std::vector<int> pitches = stampPitches(note.pitch);
                const int groupId = pitches.size() > 1 ? m_nextGroupId++ : 0;
                for (int pitch : pitches) {
                    MidiNote stampedNote = note;
                    stampedNote.pitch = pitch;
                    stampedNote.groupId = groupId;
                    m_clip->notes.push_back(stampedNote);
                }
            } else if (m_toolMode == PianoRollTool::Paint && !note.isKeyswitch) {
                const double start = std::min(m_createStartBeat, m_createCurrentBeat);
                const double end = std::max(m_createStartBeat, m_createCurrentBeat);
                const double step = std::max(effectiveSnapBeats(), kMinNoteLength);
                const double noteLength = std::max(kMinNoteLength, std::min(m_lastNoteLengthBeats, step));
                createdNoteLength = noteLength;
                for (double beat = start; beat <= end + 0.0001; beat += step) {
                    MidiNote paintedNote = note;
                    paintedNote.startBeat = clampBeat(beat);
                    paintedNote.lengthBeats = noteLength;
                    m_clip->notes.push_back(paintedNote);
                }
            } else {
                m_clip->notes.push_back(note);
            }
            m_clip->lastNoteLengthBeats = createdNoteLength;
            m_lastNoteLengthBeats = createdNoteLength;
            m_lastCreatedNote = static_cast<int>(m_clip->notes.size()) - 1;
            m_selectedNote = m_lastCreatedNote;
            m_creatingNote = false;
            m_createDragged = false;
            m_createIsKeyswitch = false;
            m_activeNote = -1;
            sortNotes();
            updateScrollRanges();
            viewport()->update();
            notifyChanged();
            event->accept();
            return;
        }

        if (event->button() == Qt::LeftButton && m_selecting) {
            m_selectionCurrent = viewportToContent(event->pos());
            selectNotesInRect(QRectF(m_selectionStart, m_selectionCurrent).normalized(), m_selectionAdditive);
            m_selecting = false;
            viewport()->update();
            event->accept();
            return;
        }

        if (event->button() == Qt::LeftButton && m_editingProperty) {
            m_editingProperty = false;
            m_propertyNote = -1;
            event->accept();
            return;
        }

        if (event->button() == Qt::LeftButton && m_slicing) {
            m_sliceCurrent = viewportToContent(event->pos());
            m_slicing = false;
            sliceNotesAlongLine(m_sliceStart, m_sliceCurrent, m_sliceBypassSnap);
            viewport()->update();
            event->accept();
            return;
        }

        if (event->button() == Qt::LeftButton && m_muting) {
            m_muting = false;
            m_mutedDragNotes.clear();
            notifyChanged();
            event->accept();
            return;
        }

        if (event->button() == Qt::LeftButton && m_activeNote >= 0) {
            m_clip->lastNoteLengthBeats = m_clip->notes[m_activeNote].lengthBeats;
            m_lastNoteLengthBeats = m_clip->lastNoteLengthBeats;
            m_lastCreatedNote = m_activeNote;
            m_activeNote = -1;
            m_activeResize = false;
            m_dragSnapshots.clear();
            sortNotes();
            viewport()->update();
            notifyChanged();
            event->accept();
            return;
        }

        QAbstractScrollArea::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (!m_clip) {
            QAbstractScrollArea::mouseDoubleClickEvent(event);
            return;
        }

        if (event->button() == Qt::RightButton) {
            const int ghostIndex = ghostNoteAt(event->pos());
            if (ghostIndex >= 0) {
                setCurrentColorGroup(m_clip->notes[ghostIndex].colorGroup);
                event->accept();
                return;
            }
            QAbstractScrollArea::mouseDoubleClickEvent(event);
            return;
        }

        if (event->button() != Qt::LeftButton) {
            QAbstractScrollArea::mouseDoubleClickEvent(event);
            return;
        }

        int index = noteAt(event->pos());
        if (index >= 0) {
            const bool keepSelection = m_clip->notes[index].selected;
            selectNote(index, keepSelection);
            openNotePropertiesDialog(index);
            viewport()->update();
            event->accept();
            return;
        }

        QAbstractScrollArea::mouseDoubleClickEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (!m_clip) {
            return;
        }

        if (event->matches(QKeySequence::Undo)) {
            if (m_undoRequestCallback) m_undoRequestCallback();
            else undoEdit();
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::Redo)) {
            if (m_redoRequestCallback) m_redoRequestCallback();
            else redoEdit();
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::SelectAll)) {
            selectAllNotes();
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::Copy)) {
            copySelection();
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::Cut)) {
            cutSelection();
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::Paste)) {
            pasteSelection();
            event->accept();
            return;
        }
        if (event->modifiers() == Qt::NoModifier &&
            (event->key() == Qt::Key_BracketLeft || event->key() == Qt::Key_BracketRight)) {
            const int count = keyswitchRowCount();
            const int delta = event->key() == Qt::Key_BracketRight ? 1 : -1;
            const int nextRow = m_focusedExpressionRow < 0
                ? (delta > 0 ? 0 : count - 1)
                : (m_focusedExpressionRow + delta + count) % count;
            setFocusedExpressionRow(nextRow, true);
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::ShiftModifier) &&
            (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            m_focusedExpressionRow >= 0) {
            insertExpressionAtPlayhead(m_focusedExpressionRow);
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::ShiftModifier) && event->key() == Qt::Key_Delete &&
            m_focusedExpressionRow >= 0) {
            removeExpressionAtPlayhead(m_focusedExpressionRow);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
            deleteSelectedNotes();
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_D) {
            clearSelection();
            m_selectedNote = -1;
            viewport()->update();
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_B) {
            duplicateSelection();
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_Q) {
            quantizeSelection();
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::AltModifier) && event->key() == Qt::Key_C) {
            changeSelectedNotesToCurrentColor();
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_L) {
            quickLegatoSelection();
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_G) {
            glueSelectedTouchingNotes();
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::ShiftModifier) && event->key() == Qt::Key_F) {
            cyclePropertyTarget();
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::ShiftModifier) && event->key() == Qt::Key_C) {
            selectCurrentColorGroup();
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::ShiftModifier) && event->key() == Qt::Key_G) {
            groupSelectedNotes();
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::AltModifier) && event->key() == Qt::Key_G) {
            ungroupSelectedNotes();
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::ShiftModifier) &&
            (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
            const double snap = effectiveSnapBeats();
            nudgeSelection((event->key() == Qt::Key_Right ? snap : -snap), 0);
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::AltModifier) &&
            (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
            const double pixelBeat = 1.0 / std::max(1.0, m_pixelsPerBeat);
            nudgeSelection((event->key() == Qt::Key_Right ? pixelBeat : -pixelBeat), 0);
            event->accept();
            return;
        }
        if (event->modifiers() == Qt::NoModifier) {
            if (event->key() == Qt::Key_P) {
                setToolMode(PianoRollTool::Draw);
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_B) {
                setToolMode(PianoRollTool::Paint);
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_E) {
                setToolMode(PianoRollTool::Select);
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_D) {
                setToolMode(PianoRollTool::Delete);
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_T) {
                setToolMode(PianoRollTool::Mute);
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_C) {
                setToolMode(PianoRollTool::Slice);
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_S) {
                setSlideMode(!m_slideMode);
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_O) {
                setPortamentoMode(!m_portamentoMode);
                event->accept();
                return;
            }
            if (event->key() == Qt::Key_G) {
                setGhostNotesVisible(!m_showGhostNotes);
                event->accept();
                return;
            }
        }

        const bool octave = event->modifiers() & Qt::ControlModifier;
        const bool semitone = event->modifiers() & Qt::ShiftModifier;
        if ((octave || semitone) && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
            const int step = (event->key() == Qt::Key_Up) ? 1 : -1;
            const int delta = octave ? step * 12 : step;
            transposeSelection(delta);
            event->accept();
            return;
        }

        QAbstractScrollArea::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent* event) override
    {
        if ((event->key() == Qt::Key_BracketLeft || event->key() == Qt::Key_BracketRight) &&
            m_previewIsKeyswitch) {
            stopPreviewNote();
            event->accept();
            return;
        }
        QAbstractScrollArea::keyReleaseEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override
    {
        if (m_clip && (event->modifiers() & Qt::AltModifier)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const QPoint wheelPoint = event->position().toPoint();
#else
            const QPoint wheelPoint = event->pos();
#endif
            const int index = noteAt(wheelPoint);
            if (index >= 0) {
                const int step = (event->angleDelta().y() > 0 ? 1 : -1) *
                    ((event->modifiers() & Qt::ControlModifier) ? 1 : 4);
                adjustNoteProperty(index, step);
                event->accept();
                return;
            }
        }

        if (event->modifiers() & Qt::ControlModifier) {
            const double factor = event->angleDelta().y() > 0 ? 1.1 : 0.9;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            zoomAt(factor, event->position().x());
#else
            zoomAt(factor, event->pos().x());
#endif
            event->accept();
            return;
        }

        if (event->modifiers() & Qt::ShiftModifier) {
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - event->angleDelta().y());
            event->accept();
            return;
        }

        QAbstractScrollArea::wheelEvent(event);
    }

private:
    MidiClipDocument* m_clip = nullptr;
    std::vector<MidiTimeSignaturePoint> m_signatureMap;
    std::vector<MidiGhostClip> m_ghostClips;
    std::vector<MidiExpressionMapEntry> m_pluginExpressionMap;
    std::vector<MidiExpressionMapEntry> m_expressionRows;
    std::function<void(const MidiClipDocument&, const MidiClipDocument&, quint64)>
        m_changeCallback;
    std::function<void()> m_undoRequestCallback;
    std::function<void()> m_redoRequestCallback;
    double m_pixelsPerBeat = kDefaultPixelsPerBeat;
    int m_selectedNote = -1;
    int m_activeNote = -1;
    bool m_activeResize = false;
    bool m_creatingNote = false;
    bool m_createDragged = false;
    bool m_selecting = false;
    bool m_slicing = false;
    bool m_sliceBypassSnap = false;
    bool m_muting = false;
    bool m_muteValue = false;
    bool m_createIsKeyswitch = false;
    bool m_editingProperty = false;
    int m_createPitch = 60;
    int m_createChannel = 0;
    int m_propertyNote = -1;
    double m_createStartBeat = 0.0;
    double m_createCurrentBeat = 0.0;
    double m_createLengthBeats = 1.0;
    double m_lastNoteLengthBeats = 1.0;
    int m_lastCreatedNote = -1;
    QPointF m_dragStartPoint;
    QPointF m_selectionStart;
    QPointF m_selectionCurrent;
    QPointF m_sliceStart;
    QPointF m_sliceCurrent;
    std::vector<int> m_mutedDragNotes;
    bool m_selectionAdditive = false;
    double m_dragStartBeat = 0.0;
    int m_dragStartPitch = 60;
    double m_dragStartLength = 1.0;
    struct DragNoteSnapshot
    {
        int index = -1;
        double startBeat = 0.0;
        int pitch = 60;
        int colorGroup = 0;
        double lengthBeats = 1.0;
    };
    std::vector<DragNoteSnapshot> m_dragSnapshots;
    double m_noteOffsetX = 0.0;
    double m_noteOffsetY = 0.0;
    double m_snapBeats = kDefaultSnap;
    bool m_snapToLine = false;
    int m_currentColorGroup = 0;
    int m_nextGroupId = 1;
    bool m_slideMode = false;
    bool m_portamentoMode = false;
    bool m_showGhostNotes = true;
    bool m_snapToScale = true;
    bool m_stampDiatonic = true;
    std::vector<int> m_stampIntervals{0, 4, 7};
    int m_scaleRoot = 0;
    bool m_scaleMinor = false;
    NotePropertyTarget m_propertyTarget = NotePropertyTarget::Velocity;
    PianoRollTool m_toolMode = PianoRollTool::Draw;
    std::function<void(PianoRollTool)> m_toolModeChangedCallback;
    std::function<void(bool)> m_slideModeChangedCallback;
    std::function<void(bool)> m_portamentoModeChangedCallback;
    std::function<void(bool)> m_ghostNotesVisibleChangedCallback;
    std::function<void(NotePropertyTarget)> m_propertyTargetChangedCallback;
    std::function<void(int)> m_colorGroupChangedCallback;
    std::function<void(double)> m_playheadSeekCallback;
    std::function<void(int, int, bool, bool)> m_notePreviewCallback;
    double m_playheadBeat = -1.0;
    int m_previewPitch = -1;
    int m_previewChannel = 0;
    bool m_previewIsKeyswitch = false;
    int m_hoverExpressionRow = -1;
    int m_focusedExpressionRow = -1;
    bool m_panning = false;
    bool m_erasingRight = false;
    QPoint m_panStartPos;
    int m_panStartH = 0;
    int m_panStartV = 0;
    struct EditorState
    {
        std::vector<MidiNote> notes;
        double lastNoteLengthBeats = 1.0;
    };
    std::vector<EditorState> m_undoHistory;
    std::vector<EditorState> m_redoHistory;
    std::unique_ptr<MidiClipDocument> m_editBeforeDocument;
    quint64 m_editSerial = 0;

    EditorState currentEditorState() const
    {
        return m_clip ? EditorState{m_clip->notes, m_clip->lastNoteLengthBeats} : EditorState{};
    }

    void beginDocumentEdit()
    {
        if (!m_clip) return;
        m_editBeforeDocument = std::make_unique<MidiClipDocument>(*m_clip);
        ++m_editSerial;
        if (m_editSerial == 0) ++m_editSerial;
    }

    void pushUndoState()
    {
        if (!m_clip) return;
        beginDocumentEdit();
        if (m_undoHistory.size() >= 100) m_undoHistory.erase(m_undoHistory.begin());
        m_undoHistory.push_back(currentEditorState());
        m_redoHistory.clear();
    }

    void restoreEditorState(const EditorState& state)
    {
        if (!m_clip) return;
        m_clip->notes = state.notes;
        m_clip->lastNoteLengthBeats = safeNoteLength(state.lastNoteLengthBeats);
        m_lastNoteLengthBeats = m_clip->lastNoteLengthBeats;
        m_selectedNote = -1;
        m_lastCreatedNote = -1;
        m_activeNote = -1;
        m_dragSnapshots.clear();
        m_nextGroupId = 1;
        for (const MidiNote& note : m_clip->notes)
            m_nextGroupId = std::max(m_nextGroupId, note.groupId + 1);
        rebuildExpressionRows();
        updateScrollRanges();
        viewport()->update();
        notifyChanged();
    }

    void undoEdit()
    {
        if (!m_clip || m_undoHistory.empty()) return;
        beginDocumentEdit();
        if (m_redoHistory.size() >= 100) m_redoHistory.erase(m_redoHistory.begin());
        m_redoHistory.push_back(currentEditorState());
        const EditorState state = std::move(m_undoHistory.back());
        m_undoHistory.pop_back();
        restoreEditorState(state);
    }

    void redoEdit()
    {
        if (!m_clip || m_redoHistory.empty()) return;
        beginDocumentEdit();
        if (m_undoHistory.size() >= 100) m_undoHistory.erase(m_undoHistory.begin());
        m_undoHistory.push_back(currentEditorState());
        const EditorState state = std::move(m_redoHistory.back());
        m_redoHistory.pop_back();
        restoreEditorState(state);
    }

    int contentWidth() const
    {
        const double beats = m_clip ? std::max(64.0, m_clip->lengthBeats + 16.0) : 64.0;
        return static_cast<int>(kKeyboardWidth + beats * m_pixelsPerBeat + 220.0);
    }

    int contentHeight() const
    {
        return velocityTop() + kVelocityLaneHeight + kBottomPadding;
    }

    void rebuildTimeSignatureMap()
    {
        constexpr size_t kMaxSignaturePoints = 4096;
        m_signatureMap.clear();
        if (m_clip) {
            m_signatureMap.reserve(std::min(m_clip->timeSignatureMap.size(), kMaxSignaturePoints));
            for (const MidiTimeSignaturePoint& point : m_clip->timeSignatureMap) {
                const bool validDenominator = point.denominator >= 1 && point.denominator <= 64 &&
                    (point.denominator & (point.denominator - 1)) == 0;
                if (!std::isfinite(point.beat) || point.beat < 0.0 ||
                    point.numerator < 1 || point.numerator > 32 || !validDenominator) continue;
                if (m_signatureMap.size() >= kMaxSignaturePoints) break;
                m_signatureMap.push_back(point);
            }
        }
        std::stable_sort(m_signatureMap.begin(), m_signatureMap.end(),
            [](const MidiTimeSignaturePoint& left, const MidiTimeSignaturePoint& right) {
                return left.beat < right.beat;
            });
        std::vector<MidiTimeSignaturePoint> collapsed;
        collapsed.reserve(m_signatureMap.size());
        for (const MidiTimeSignaturePoint& point : m_signatureMap) {
            if (!collapsed.empty() && collapsed.back().beat == point.beat) collapsed.back() = point;
            else collapsed.push_back(point);
        }
        const int fallbackNumerator = m_clip ? qBound(1, m_clip->timeSignatureNumerator, 32) : 4;
        const int boundedDenominator = m_clip ? qBound(1, m_clip->timeSignatureDenominator, 64) : 4;
        const int fallbackDenominator = (boundedDenominator & (boundedDenominator - 1)) == 0
            ? boundedDenominator : 4;
        if (collapsed.empty()) collapsed.push_back({0.0, fallbackNumerator, fallbackDenominator});
        else if (collapsed.front().beat > 0.0) {
            if (collapsed.size() == kMaxSignaturePoints) collapsed.pop_back();
            collapsed.insert(collapsed.begin(), {0.0, fallbackNumerator, fallbackDenominator});
        }
        m_signatureMap = std::move(collapsed);
    }

    MidiTimeSignaturePoint timeSignatureAtBeat(double beat) const
    {
        if (m_signatureMap.empty()) return {0.0, 4, 4};
        const auto next = std::upper_bound(m_signatureMap.begin(), m_signatureMap.end(),
            std::max(0.0, beat),
            [](double targetBeat, const MidiTimeSignaturePoint& point) {
                return targetBeat < point.beat;
            });
        return next == m_signatureMap.begin() ? m_signatureMap.front() : *(next - 1);
    }

    double beatsPerBarAt(double beat) const
    {
        const MidiTimeSignaturePoint signature = timeSignatureAtBeat(beat);
        return qBound(0.25, static_cast<double>(signature.numerator) *
            (4.0 / static_cast<double>(signature.denominator)), 64.0);
    }

    double beatsPerBar() const { return beatsPerBarAt(0.0); }

    struct EditorBarMarker {
        double beat = 0.0;
        int barNumber = 1;
        int numerator = 4;
        int denominator = 4;
        bool signatureChange = false;
    };

    std::vector<EditorBarMarker> barMarkers(double startBeat, double endBeat) const
    {
        std::vector<EditorBarMarker> result;
        if (m_signatureMap.empty() || !std::isfinite(startBeat) ||
            !std::isfinite(endBeat) || endBeat < startBeat) return result;
        const double visibleStart = std::max(0.0, startBeat);
        const double visibleEnd = std::max(visibleStart, endBeat);
        result.reserve(static_cast<size_t>(std::clamp(
            (visibleEnd - visibleStart) / 0.25 + 4.0, 4.0, 4096.0)));
        qint64 baseBar = 1;
        for (size_t index = 0; index < m_signatureMap.size(); ++index) {
            const MidiTimeSignaturePoint& point = m_signatureMap[index];
            if (point.beat > visibleEnd + 1.0e-9) break;
            const double segmentStart = point.beat;
            const double segmentEnd = index + 1u < m_signatureMap.size()
                ? m_signatureMap[index + 1u].beat
                : std::numeric_limits<double>::infinity();
            const double barLength = qBound(0.25,
                static_cast<double>(point.numerator) *
                    (4.0 / static_cast<double>(point.denominator)), 64.0);
            if (segmentEnd >= visibleStart && segmentStart <= visibleEnd) {
                const qint64 firstLocalBar = std::max<qint64>(0,
                    static_cast<qint64>(std::floor((visibleStart - segmentStart) / barLength)) - 1);
                for (qint64 localBar = firstLocalBar; ; ++localBar) {
                    const double barBeat = segmentStart + static_cast<double>(localBar) * barLength;
                    if (barBeat > visibleEnd + 1.0e-9 || barBeat >= segmentEnd - 1.0e-9) break;
                    if (barBeat >= visibleStart - 1.0e-9) {
                        const qint64 number = std::min<qint64>(
                            std::numeric_limits<int>::max(), baseBar + localBar);
                        result.push_back({barBeat, static_cast<int>(number),
                            point.numerator, point.denominator,
                            index > 0u && localBar == 0});
                        if (result.size() >= 8192u) return result;
                    }
                }
            }
            if (std::isfinite(segmentEnd)) {
                const qint64 count = std::max<qint64>(1,
                    static_cast<qint64>(std::ceil(
                        std::max(0.0, segmentEnd - segmentStart) / barLength - 1.0e-9)));
                baseBar = std::min<qint64>(
                    std::numeric_limits<int>::max(), baseBar + count);
            }
        }
        return result;
    }

    double effectiveSnapBeats() const
    {
        if (!m_snapToLine) {
            return std::isfinite(m_snapBeats) && m_snapBeats > 0.0 ? m_snapBeats : kDefaultSnap;
        }
        constexpr double kMinimumGridPixels = 10.0;
        double snap = 1.0 / 64.0;
        const double maximum = std::max(snap, beatsPerBar());
        while (snap < maximum && snap * m_pixelsPerBeat < kMinimumGridPixels) snap *= 2.0;
        return std::min(snap, maximum);
    }

    bool isBarLine(double beat) const
    {
        const MidiTimeSignaturePoint signature = timeSignatureAtBeat(beat);
        const double bar = beatsPerBarAt(beat);
        const double position = std::fmod(
            std::max(0.0, beat - signature.beat), bar);
        return position <= 0.0001 || std::fabs(position - bar) <= 0.0001;
    }

    bool isPitchInScale(int pitch) const
    {
        static constexpr int majorIntervals[] = {0, 2, 4, 5, 7, 9, 11};
        static constexpr int minorIntervals[] = {0, 2, 3, 5, 7, 8, 10};
        const int pitchClass = ((pitch - m_scaleRoot) % 12 + 12) % 12;
        const int* intervals = m_scaleMinor ? minorIntervals : majorIntervals;
        return std::find(intervals, intervals + 7, pitchClass) != intervals + 7;
    }

    std::vector<int> stampPitches(int rootPitch) const
    {
        std::vector<int> pitches;
        const int root = clampPianoPitch(rootPitch);
        pitches.push_back(root);
        if (m_stampDiatonic) {
            int scaleDegree = 0;
            for (int pitch = root + 1; pitch <= kPianoRollMaxPitch && pitches.size() < 3; ++pitch) {
                if (!isPitchInScale(pitch)) continue;
                ++scaleDegree;
                if (scaleDegree == 2 || scaleDegree == 4) pitches.push_back(pitch);
            }
        } else {
            for (int interval : m_stampIntervals) {
                const int pitch = root + interval;
                if (interval == 0 || pitch < kPianoRollMinPitch || pitch > kPianoRollMaxPitch) continue;
                if (std::find(pitches.begin(), pitches.end(), pitch) == pitches.end()) pitches.push_back(pitch);
            }
        }
        return pitches;
    }

    int snapPitchToScale(int pitch) const
    {
        const int safePitch = clampPianoPitch(pitch);
        if (!m_snapToScale || isPitchInScale(safePitch)) {
            return safePitch;
        }
        for (int distance = 1; distance < 12; ++distance) {
            const int lower = safePitch - distance;
            if (lower >= kPianoRollMinPitch && isPitchInScale(lower)) {
                return lower;
            }
            const int upper = safePitch + distance;
            if (upper <= kPianoRollMaxPitch && isPitchInScale(upper)) {
                return upper;
            }
        }
        return safePitch;
    }

    void rebuildExpressionRows()
    {
        m_expressionRows.clear();
        auto addEntry = [&](MidiExpressionMapEntry entry) {
            if (!isPluginMidiPitch(entry.pitch)) return;
            entry.channel = qBound(0, entry.channel, 15);
            auto existing = std::find_if(m_expressionRows.begin(), m_expressionRows.end(),
                [&](const MidiExpressionMapEntry& current) {
                    return current.pitch == entry.pitch && current.channel == entry.channel;
                });
            if (existing == m_expressionRows.end()) {
                if (entry.name.trimmed().isEmpty()) entry.name = QString("Keyswitch %1").arg(noteName(entry.pitch));
                if (entry.shortName.trimmed().isEmpty()) entry.shortName = entry.name;
                m_expressionRows.push_back(std::move(entry));
            } else if (!entry.name.trimmed().isEmpty() && !existing->name.contains(entry.name, Qt::CaseInsensitive)) {
                existing->name += " / " + entry.name;
                existing->shortName = existing->name;
            }
        };
        for (MidiExpressionMapEntry entry : m_pluginExpressionMap) addEntry(std::move(entry));
        if (m_clip) {
            for (const MidiNote& note : m_clip->notes) {
                if (!note.isKeyswitch) continue;
                const auto mapped = std::find_if(m_expressionRows.begin(), m_expressionRows.end(),
                    [&](const MidiExpressionMapEntry& entry) {
                        return entry.pitch == note.pitch && entry.channel == note.colorGroup;
                    });
                if (mapped != m_expressionRows.end()) continue;
                addEntry({note.pitch, note.colorGroup, -1,
                          QString("Keyswitch %1").arg(noteName(note.pitch)),
                          noteName(note.pitch)});
            }
        }
        if (m_expressionRows.empty()) {
            for (int pitch = 0; pitch < kKeyswitchRows; ++pitch) {
                addEntry({pitch, 0, -1, QString("Keyswitch %1").arg(noteName(pitch)), noteName(pitch)});
            }
        }
        std::stable_sort(m_expressionRows.begin(), m_expressionRows.end(),
            [](const MidiExpressionMapEntry& a, const MidiExpressionMapEntry& b) {
                return a.pitch != b.pitch ? a.pitch < b.pitch : a.channel < b.channel;
            });
    }

    std::array<int, 16> activeKeyswitchPitches() const
    {
        std::array<int, 16> pitches{};
        pitches.fill(-1);
        if (!m_clip || !std::isfinite(m_playheadBeat) || m_playheadBeat < 0.0)
            return pitches;

        std::array<double, 16> latestBeat{};
        latestBeat.fill(-std::numeric_limits<double>::infinity());
        for (const MidiNote& note : m_clip->notes) {
            if (!note.isKeyswitch || note.muted || !std::isfinite(note.startBeat) ||
                note.startBeat > m_playheadBeat + 1.0e-9) continue;
            const int channel = qBound(0, note.colorGroup, 15);
            if (note.startBeat + 1.0e-9 < latestBeat[static_cast<size_t>(channel)])
                continue;
            latestBeat[static_cast<size_t>(channel)] = note.startBeat;
            pitches[static_cast<size_t>(channel)] = qBound(0, note.pitch, 127);
        }
        return pitches;
    }

    int keyswitchRowCount() const
    {
        return std::max(1, static_cast<int>(m_expressionRows.size()));
    }

    int keyswitchPitchForRow(int row) const
    {
        const int safeRow = qBound(0, row, keyswitchRowCount() - 1);
        return m_expressionRows.empty() ? safeRow : m_expressionRows[static_cast<size_t>(safeRow)].pitch;
    }

    int keyswitchChannelForRow(int row) const
    {
        const int safeRow = qBound(0, row, keyswitchRowCount() - 1);
        return m_expressionRows.empty()
            ? m_currentColorGroup
            : qBound(0, m_expressionRows[static_cast<size_t>(safeRow)].channel, 15);
    }

    void insertExpressionAtPlayhead(int row)
    {
        if (!m_clip) return;

        const int pitch = keyswitchPitchForRow(row);
        const int channel = keyswitchChannelForRow(row);
        const double snap = std::max(effectiveSnapBeats(), kMinNoteLength);
        const double startBeat = snapBeat(std::max(0.0, m_playheadBeat));
        const double intervalEnd = startBeat + snap;
        constexpr double kOnsetEpsilon = 1.0e-9;

        pushUndoState();
        clearSelection();
        const auto replaceBegin = std::remove_if(m_clip->notes.begin(), m_clip->notes.end(),
            [channel, startBeat, intervalEnd](const MidiNote& note) {
                return note.isKeyswitch && qBound(0, note.colorGroup, 15) == channel &&
                       note.startBeat >= startBeat - kOnsetEpsilon &&
                       note.startBeat < intervalEnd - kOnsetEpsilon;
            });
        const auto replaceEnd = m_clip->notes.end();
        m_clip->notes.erase(replaceBegin, replaceEnd);

        MidiNote inserted;
        inserted.pitch = pitch;
        inserted.startBeat = startBeat;
        inserted.lengthBeats = snap;
        inserted.colorGroup = channel;
        inserted.selected = true;
        inserted.isKeyswitch = true;
        m_clip->notes.push_back(inserted);
        m_selectedNote = static_cast<int>(m_clip->notes.size()) - 1;
        m_lastCreatedNote = m_selectedNote;

        sortNotes();
        rebuildExpressionRows();
        updateScrollRanges();
        viewport()->update();
        notifyChanged();
    }

    void removeExpressionAtPlayhead(int row)
    {
        if (!m_clip) return;

        const int pitch = keyswitchPitchForRow(row);
        const int channel = keyswitchChannelForRow(row);
        const double snap = std::max(effectiveSnapBeats(), kMinNoteLength);
        const double startBeat = snapBeat(std::max(0.0, m_playheadBeat));
        const double intervalEnd = startBeat + snap;
        constexpr double kOnsetEpsilon = 1.0e-9;
        const auto matches = [pitch, channel, startBeat, intervalEnd](const MidiNote& note) {
            return note.isKeyswitch && note.pitch == pitch &&
                   qBound(0, note.colorGroup, 15) == channel &&
                   note.startBeat >= startBeat - kOnsetEpsilon &&
                   note.startBeat < intervalEnd - kOnsetEpsilon;
        };
        const auto found = std::find_if(m_clip->notes.begin(), m_clip->notes.end(), matches);
        if (found == m_clip->notes.end()) return;

        const auto remapIndex = [&](int index) {
            if (index < 0 || index >= static_cast<int>(m_clip->notes.size())) return -1;
            if (matches(m_clip->notes[static_cast<size_t>(index)])) return -1;
            return index - static_cast<int>(std::count_if(
                m_clip->notes.begin(), m_clip->notes.begin() + index, matches));
        };
        const int selectedAfterErase = remapIndex(m_selectedNote);
        const int lastCreatedAfterErase = remapIndex(m_lastCreatedNote);

        pushUndoState();
        m_clip->notes.erase(std::remove_if(m_clip->notes.begin(), m_clip->notes.end(), matches),
                            m_clip->notes.end());
        m_selectedNote = selectedAfterErase;
        m_lastCreatedNote = lastCreatedAfterErase;
        rebuildExpressionRows();
        updateScrollRanges();
        viewport()->update();
        notifyChanged();
    }

    bool hasExpressionAtPlayhead(int row) const
    {
        if (!m_clip) return false;
        const int pitch = keyswitchPitchForRow(row);
        const int channel = keyswitchChannelForRow(row);
        const double snap = std::max(effectiveSnapBeats(), kMinNoteLength);
        const double startBeat = snapBeat(std::max(0.0, m_playheadBeat));
        const double intervalEnd = startBeat + snap;
        constexpr double kOnsetEpsilon = 1.0e-9;
        return std::any_of(m_clip->notes.begin(), m_clip->notes.end(),
            [pitch, channel, startBeat, intervalEnd](const MidiNote& note) {
                return note.isKeyswitch && note.pitch == pitch &&
                       qBound(0, note.colorGroup, 15) == channel &&
                       note.startBeat >= startBeat - kOnsetEpsilon &&
                       note.startBeat < intervalEnd - kOnsetEpsilon;
            });
    }

    QRect expressionRowViewportRect(int row) const
    {
        if (row < 0 || row >= keyswitchRowCount()) return QRect();
        const int y = keyswitchTop() + row * kKeyswitchRowHeight - verticalScrollBar()->value();
        return QRect(0, y, kKeyboardWidth, kKeyswitchRowHeight);
    }

    void setHoveredExpressionRow(int row)
    {
        const int safeRow = row >= 0 && row < keyswitchRowCount() ? row : -1;
        if (safeRow == m_hoverExpressionRow) return;
        const int previousRow = m_hoverExpressionRow;
        m_hoverExpressionRow = safeRow;
        if (previousRow >= 0) viewport()->update(expressionRowViewportRect(previousRow));
        if (m_hoverExpressionRow >= 0)
            viewport()->update(expressionRowViewportRect(m_hoverExpressionRow));
    }

    void setFocusedExpressionRow(int row, bool audition)
    {
        const int safeRow = row >= 0 && row < keyswitchRowCount() ? row : -1;
        const int previousRow = m_focusedExpressionRow;
        m_focusedExpressionRow = safeRow;
        if (previousRow != m_focusedExpressionRow) {
            if (previousRow >= 0) viewport()->update(expressionRowViewportRect(previousRow));
            if (m_focusedExpressionRow >= 0)
                viewport()->update(expressionRowViewportRect(m_focusedExpressionRow));
        }
        if (audition && m_focusedExpressionRow >= 0) {
            startPreviewNote(keyswitchPitchForRow(m_focusedExpressionRow),
                             keyswitchChannelForRow(m_focusedExpressionRow), true);
        }
    }

    int keyswitchRowForPitchChannel(int pitch, int channel) const
    {
        for (int row = 0; row < static_cast<int>(m_expressionRows.size()); ++row) {
            const MidiExpressionMapEntry& entry = m_expressionRows[static_cast<size_t>(row)];
            if (entry.pitch == pitch && entry.channel == channel) return row;
        }
        for (int row = 0; row < static_cast<int>(m_expressionRows.size()); ++row) {
            if (m_expressionRows[static_cast<size_t>(row)].pitch == pitch) return row;
        }
        return qBound(0, pitch, keyswitchRowCount() - 1);
    }

    QString expressionNameForPitchChannel(int pitch, int channel, bool shortName = false) const
    {
        const auto found = std::find_if(m_expressionRows.begin(), m_expressionRows.end(),
            [pitch, channel](const MidiExpressionMapEntry& entry) {
                return entry.pitch == pitch && entry.channel == channel;
            });
        if (found == m_expressionRows.end()) return QString("KS %1").arg(noteName(pitch));
        return shortName && !found->shortName.isEmpty() ? found->shortName : found->name;
    }

    int expressionChannelForPitch(int pitch, int preferredChannel) const
    {
        const auto found = std::find_if(m_expressionRows.begin(), m_expressionRows.end(),
            [pitch, preferredChannel](const MidiExpressionMapEntry& entry) {
                return entry.pitch == pitch && entry.channel == preferredChannel;
            });
        if (found != m_expressionRows.end()) return qBound(0, found->channel, 15);
        const auto samePitch = std::find_if(m_expressionRows.begin(), m_expressionRows.end(),
            [pitch](const MidiExpressionMapEntry& entry) { return entry.pitch == pitch; });
        return samePitch == m_expressionRows.end()
            ? qBound(0, preferredChannel, 15)
            : qBound(0, samePitch->channel, 15);
    }

    int keyswitchTop() const
    {
        return kTopPadding + pianoRowCount() * kNoteHeight + 10;
    }

    int velocityTop() const
    {
        return keyswitchTop() + keyswitchRowCount() * kKeyswitchRowHeight + 12;
    }

    QPointF viewportToContent(const QPoint& point) const
    {
        return QPointF(point.x() + horizontalScrollBar()->value(),
                       point.y() + verticalScrollBar()->value());
    }

    void updateScrollRanges()
    {
        horizontalScrollBar()->setRange(0, std::max(0, contentWidth() - viewport()->width()));
        horizontalScrollBar()->setPageStep(viewport()->width());
        verticalScrollBar()->setRange(0, std::max(0, contentHeight() - viewport()->height()));
        verticalScrollBar()->setPageStep(viewport()->height());
    }

    void drawKeyboard(QPainter* painter, const QRect& visible, int vScroll)
    {
        Q_UNUSED(visible);
        painter->save();
        painter->setClipRect(0, 0, kKeyboardWidth, viewport()->height());
        painter->fillRect(QRect(0, 0, kKeyboardWidth, viewport()->height()), QColor("#1f2320"));

        const int topRow = std::clamp((vScroll - kTopPadding) / kNoteHeight, 0, pianoRowCount() - 1);
        const int bottomRow = std::clamp((vScroll + viewport()->height() - kTopPadding) / kNoteHeight + 1, 0, pianoRowCount() - 1);
        for (int row = topRow; row <= bottomRow; ++row) {
            const int pitch = kPianoRollMaxPitch - row;
            const int y = kTopPadding + row * kNoteHeight - vScroll;
            QRect r(0, y, kKeyboardWidth, kNoteHeight);
            painter->fillRect(r, isBlackKey(pitch) ? QColor("#171a18") : QColor("#2f3430"));
            painter->setPen(QColor("#0f0f0f"));
            painter->drawRect(r.adjusted(0, 0, -1, -1));
            if (pitch % 12 == 0) {
                painter->setPen(QColor("#d8e0d8"));
                painter->drawText(r.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, noteName(pitch));
            }
        }

        const int ksTop = keyswitchTop() - vScroll;
        QRect ksHeader(0, ksTop - 18, kKeyboardWidth, 18);
        painter->fillRect(ksHeader, QColor("#262b27"));
        painter->setPen(QColor("#ff9020"));
        painter->drawText(ksHeader.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                          "EXPRESSION MAP");
        QFont expressionFont = painter->font();
        expressionFont.setPointSizeF(8.0);
        painter->setFont(expressionFont);
        const std::array<int, 16> activePitches = activeKeyswitchPitches();
        for (int row = 0; row < keyswitchRowCount(); ++row) {
            const int y = ksTop + row * kKeyswitchRowHeight;
            QRect rowRect(0, y, kKeyboardWidth, kKeyswitchRowHeight);
            if (rowRect.bottom() < 0 || rowRect.top() > viewport()->height()) continue;
            const int pitch = keyswitchPitchForRow(row);
            const int channel = keyswitchChannelForRow(row);
            const bool active = activePitches[static_cast<size_t>(channel)] == pitch;
            const bool previewing = m_previewIsKeyswitch && m_previewPitch == pitch &&
                m_previewChannel == channel;
            const bool hovered = row == m_hoverExpressionRow;
            const bool focused = row == m_focusedExpressionRow;
            painter->fillRect(rowRect, previewing
                ? QColor("#75471f")
                : active
                ? QColor("#5a3518")
                : hovered
                ? QColor("#303a31")
                : (row % 2 ? QColor("#202521") : QColor("#252b26")));
            if (active || previewing)
                painter->fillRect(rowRect.adjusted(0, 0, -rowRect.width() + 3, 0),
                                  previewing ? QColor("#ffd080") : QColor("#ff9020"));
            painter->setPen(QColor("#526052"));
            painter->drawLine(rowRect.bottomLeft(), rowRect.bottomRight());
            if (focused) {
                painter->setPen(QPen(QColor("#ff9020"), 1));
                painter->setBrush(Qt::NoBrush);
                painter->drawRect(rowRect.adjusted(1, 1, -2, -2));
            }
            const QString label = QString("%1%2  %3")
                .arg(previewing ? QStringLiteral("▶ ")
                                : (active ? QStringLiteral("● ") : QString()), noteName(pitch),
                     expressionNameForPitchChannel(pitch, channel, true));
            QFont rowFont = expressionFont;
            rowFont.setBold(active || previewing || hovered || focused);
            painter->setFont(rowFont);
            painter->setPen(previewing ? QColor("#fff1d5")
                                       : (active ? QColor("#ffd2a0")
                                                 : ((hovered || focused) ? QColor("#ffffff") : QColor("#d8ded8"))));
            painter->drawText(rowRect.adjusted(5, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                              painter->fontMetrics().elidedText(label, Qt::ElideRight, rowRect.width() - 9));
        }

        const int velTop = velocityTop() - vScroll;
        QRect velHeader(0, velTop, kKeyboardWidth, kVelocityLaneHeight);
        painter->fillRect(velHeader, QColor("#202520"));
        painter->setPen(QColor("#93b36f"));
        painter->drawText(velHeader.adjusted(6, 6, -4, 0), Qt::AlignTop | Qt::AlignLeft, "VEL");
        painter->restore();
    }

    void drawGrid(QPainter* painter, const QRect& visible, int hScroll, int vScroll)
    {
        painter->save();
        painter->setClipRect(QRect(kKeyboardWidth, 0, viewport()->width() - kKeyboardWidth, viewport()->height()));
        painter->fillRect(QRect(kKeyboardWidth, 0, viewport()->width() - kKeyboardWidth, viewport()->height()), QColor("#151815"));

        const int topRow = std::clamp((vScroll - kTopPadding) / kNoteHeight, 0, pianoRowCount() - 1);
        const int bottomRow = std::clamp((vScroll + viewport()->height() - kTopPadding) / kNoteHeight + 1, 0, pianoRowCount() - 1);
        const int gridTop = std::max(0, kTopPadding - vScroll);
        const int gridBottom = std::min(viewport()->height(), keyswitchTop() - vScroll);
        for (int row = topRow; row <= bottomRow; ++row) {
            const int pitch = kPianoRollMaxPitch - row;
            const int y = kTopPadding + row * kNoteHeight - vScroll;
            const bool scaleLane = m_snapToScale && isPitchInScale(pitch);
            const QColor rowColor = scaleLane
                ? (isBlackKey(pitch) ? QColor("#172018") : QColor("#202920"))
                : (isBlackKey(pitch) ? QColor("#101310") : QColor("#181c18"));
            painter->fillRect(QRect(kKeyboardWidth, y, viewport()->width() - kKeyboardWidth, kNoteHeight), rowColor);
            painter->setPen((pitch % 12 == 0) ? QColor("#343934") : QColor("#242824"));
            painter->drawLine(kKeyboardWidth, y, contentWidth() - hScroll, y);
        }

        const double gridSnap = effectiveSnapBeats();
        const int startStep = std::max(0, static_cast<int>(std::floor((hScroll - kKeyboardWidth) / (m_pixelsPerBeat * gridSnap))) - 1);
        const int endStep = static_cast<int>(std::ceil((hScroll + viewport()->width() - kKeyboardWidth) / (m_pixelsPerBeat * gridSnap))) + 1;
        for (int step = startStep; step <= endStep; ++step) {
            const double beatValue = step * gridSnap;
            const int x = kKeyboardWidth + static_cast<int>(beatValue * m_pixelsPerBeat) - hScroll;
            if (isBarLine(beatValue)) {
                painter->setPen(QColor("#4d554d"));
            } else if (std::fabs(beatValue - std::round(beatValue)) <= 0.0001) {
                painter->setPen(QColor("#343a34"));
            } else {
                painter->setPen(QColor("#202520"));
            }
            if (gridBottom > gridTop) {
                painter->drawLine(x, gridTop, x, gridBottom);
            }
        }

        const int startBeat = std::max(0, static_cast<int>(std::floor((hScroll - kKeyboardWidth) / m_pixelsPerBeat)) - 1);
        const int endBeat = static_cast<int>(std::ceil((hScroll + viewport()->width() - kKeyboardWidth) / m_pixelsPerBeat)) + 1;
        for (int beat = startBeat; beat <= endBeat; ++beat) {
            const int x = kKeyboardWidth + static_cast<int>(beat * m_pixelsPerBeat) - hScroll;
            painter->setPen(isBarLine(static_cast<double>(beat)) ? QColor("#586158") : QColor("#3a413a"));
            if (gridBottom > gridTop) {
                painter->drawLine(x, gridTop, x, gridBottom);
            }
        }

        const double visibleStartBeat = std::max(0.0,
            (hScroll - kKeyboardWidth) / m_pixelsPerBeat);
        const double visibleEndBeat =
            (hScroll + viewport()->width() - kKeyboardWidth) / m_pixelsPerBeat;
        const std::vector<EditorBarMarker> markers = barMarkers(
            visibleStartBeat - 64.0, visibleEndBeat + 64.0);
        painter->setPen(QColor("#586158"));
        for (const EditorBarMarker& marker : markers) {
            const int x = kKeyboardWidth + static_cast<int>(marker.beat * m_pixelsPerBeat) - hScroll;
            if (gridBottom > gridTop) {
                painter->drawLine(x, gridTop, x, gridBottom);
            }
            const double notatedBeatWidth = 4.0 / static_cast<double>(marker.denominator);
            if (notatedBeatWidth * m_pixelsPerBeat >= 8.0) {
                const auto nextSignature = std::upper_bound(
                    m_signatureMap.begin(), m_signatureMap.end(), marker.beat,
                    [](double beat, const MidiTimeSignaturePoint& point) {
                        return beat < point.beat;
                    });
                const double signatureEnd = nextSignature == m_signatureMap.end()
                    ? std::numeric_limits<double>::infinity() : nextSignature->beat;
                painter->setPen(QColor("#3a413a"));
                for (int beat = 1; beat < marker.numerator; ++beat) {
                    const double position = marker.beat + beat * notatedBeatWidth;
                    if (position >= signatureEnd - 1.0e-9) break;
                    const int beatX = kKeyboardWidth +
                        static_cast<int>(position * m_pixelsPerBeat) - hScroll;
                    if (gridBottom > gridTop) painter->drawLine(beatX, gridTop, beatX, gridBottom);
                }
                painter->setPen(QColor("#586158"));
            }
        }

        const int ksTop = keyswitchTop() - vScroll;
        const std::array<int, 16> activePitches = activeKeyswitchPitches();
        for (int row = 0; row < keyswitchRowCount(); ++row) {
            const int channel = keyswitchChannelForRow(row);
            if (activePitches[static_cast<size_t>(channel)] != keyswitchPitchForRow(row)) continue;
            const int y = ksTop + row * kKeyswitchRowHeight;
            painter->fillRect(QRect(kKeyboardWidth, y,
                std::max(0, viewport()->width() - kKeyboardWidth), kKeyswitchRowHeight),
                QColor(255, 144, 32, 28));
        }
        for (int row = 0; row <= keyswitchRowCount(); ++row) {
            const int y = ksTop + row * kKeyswitchRowHeight;
            painter->setPen(QColor("#2f352f"));
            painter->drawLine(kKeyboardWidth, y, contentWidth() - hScroll, y);
        }

        painter->setPen(QColor("#596159"));
        painter->drawLine(kKeyboardWidth, ksTop - 1, contentWidth() - hScroll, ksTop - 1);
        painter->restore();
    }

    QRectF noteRect(const MidiNote& note) const
    {
        const int x = kKeyboardWidth + static_cast<int>(note.startBeat * m_pixelsPerBeat);
        const int pitch = clampPianoPitch(note.pitch);
        const int y = note.isKeyswitch
            ? keyswitchTop() + keyswitchRowForPitchChannel(note.pitch, note.colorGroup) * kKeyswitchRowHeight
            : kTopPadding + (kPianoRollMaxPitch - pitch) * kNoteHeight;
        const int h = note.isKeyswitch ? kKeyswitchRowHeight : kNoteHeight;
        return QRectF(x, y, std::max(1.0, note.lengthBeats * m_pixelsPerBeat), h);
    }

    void drawNotes(QPainter* painter, const QRect& visible, int hScroll, int vScroll)
    {
        painter->save();
        painter->setClipRect(QRect(kKeyboardWidth, 0, viewport()->width() - kKeyboardWidth, viewport()->height()));
        if (m_showGhostNotes) {
            for (const MidiGhostClip& ghostClip : m_ghostClips) {
                QColor outline = noteColor(((ghostClip.trackIndex % 16) + 16) % 16);
                outline.setAlpha(145);
                painter->setPen(QPen(outline, 1, Qt::DashLine));
                painter->setBrush(Qt::NoBrush);
                for (const MidiNote& sourceNote : ghostClip.notes) {
                    if (sourceNote.isKeyswitch || sourceNote.muted) continue;
                    MidiNote note = sourceNote;
                    note.startBeat += ghostClip.relativeStartBeat;
                    const double ghostEnd = ghostClip.relativeStartBeat + ghostClip.lengthBeats;
                    const double noteEnd = std::min(note.startBeat + note.lengthBeats, ghostEnd);
                    note.startBeat = std::max(0.0, note.startBeat);
                    note.lengthBeats = noteEnd - note.startBeat;
                    if (note.lengthBeats <= 0.0) continue;
                    QRectF rect = noteRect(note);
                    rect.translate(-hScroll, -vScroll);
                    if (!rect.intersects(visible.adjusted(kKeyboardWidth, 0, 0, 0))) continue;
                    painter->drawRoundedRect(rect.adjusted(1.0, 2.0, -1.0, -2.0), 2, 2);
                }
            }
        }
        for (int i = 0; i < static_cast<int>(m_clip->notes.size()); ++i) {
            const MidiNote& note = m_clip->notes[i];
            const bool ghost = !note.isKeyswitch && note.colorGroup != m_currentColorGroup;
            if (ghost && !m_showGhostNotes) {
                continue;
            }
            QRectF rect = noteRect(note);
            rect.translate(-hScroll, -vScroll);
            if (!rect.intersects(visible.adjusted(kKeyboardWidth, 0, 0, 0))) {
                continue;
            }

            const bool highlighted = note.selected || i == m_selectedNote;
            QColor fill = note.isKeyswitch ? QColor("#7c4dff") : noteColor(note.colorGroup);
            QColor border = note.isKeyswitch ? QColor("#c5a6ff") : fill.darker(210);
            if (ghost) {
                fill = QColor(fill.red(), fill.green(), fill.blue(), 70);
                border = QColor("#7f9292");
            }
            if (note.muted) {
                fill = QColor("#4a524a");
                border = QColor("#252b25");
            }
            if (highlighted) {
                fill = note.isKeyswitch ? QColor("#8c60ff") : fill.lighter(118);
                border = QColor("#ff9020");
            }

            painter->setPen(QPen(border, highlighted ? 2 : 1));
            painter->setBrush(fill);
            painter->drawRoundedRect(rect.adjusted(0.5, 1.5, -0.5, -1.5), 2, 2);
            painter->fillRect(QRectF(rect.left() + 2, rect.top() + 3, 3, rect.height() - 6),
                              highlighted ? QColor("#ffffff") : fill.lighter(145));
            painter->fillRect(QRectF(rect.right() - 4, rect.top() + 3, 2, rect.height() - 6), QColor("#2b4828"));
            if (note.isSlide) {
                QPolygonF triangle;
                triangle << QPointF(rect.left() + 5, rect.bottom() - 4)
                         << QPointF(rect.left() + 17, rect.bottom() - 4)
                         << QPointF(rect.left() + 17, rect.top() + 5);
                painter->setBrush(QColor("#f6fff2"));
                painter->setPen(Qt::NoPen);
                painter->drawPolygon(triangle);
                painter->setPen(QPen(border, highlighted ? 2 : 1));
            }
            if (note.isPortamento) {
                painter->setPen(QPen(QColor("#f6fff2"), 2));
                painter->drawArc(QRectF(rect.right() - 17, rect.top() + 4, 12, rect.height() - 8), 220 * 16, 120 * 16);
            }

            if (rect.width() > 34.0) {
                painter->setPen(QColor("#0d1a0c"));
                painter->drawText(rect.adjusted(7, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                                  note.isKeyswitch
                                      ? expressionNameForPitchChannel(note.pitch, note.colorGroup)
                                      : noteName(note.pitch));
            }
        }
        painter->restore();
    }

    void drawVelocityLane(QPainter* painter, const QRect& visible, int hScroll, int vScroll)
    {
        if (!m_clip) {
            return;
        }

        const int top = velocityTop() - vScroll;
        QRect laneRect(kKeyboardWidth, top, viewport()->width() - kKeyboardWidth, kVelocityLaneHeight);
        if (!laneRect.intersects(visible)) {
            return;
        }

        painter->save();
        painter->setClipRect(laneRect);
        painter->fillRect(laneRect, QColor("#111511"));
        painter->setPen(QColor("#2b332b"));
        for (int i = 1; i < 4; ++i) {
            const int y = top + i * (kVelocityLaneHeight / 4);
            painter->drawLine(kKeyboardWidth, y, viewport()->width(), y);
        }

        for (const MidiNote& note : m_clip->notes) {
            if (note.isKeyswitch) {
                continue;
            }
            const bool ghost = note.colorGroup != m_currentColorGroup;
            if (ghost && !m_showGhostNotes) {
                continue;
            }
            const int x = kKeyboardWidth + static_cast<int>(note.startBeat * m_pixelsPerBeat) - hScroll;
            const int w = std::max(3, static_cast<int>(note.lengthBeats * m_pixelsPerBeat));
            const int value = notePropertyValue(note);
            const int h = std::max(4, (qBound(0, value, 127) * (kVelocityLaneHeight - 16)) / 127);
            const int lineX = x + std::min(std::max(4, w / 2), 18);
            const int yTop = top + kVelocityLaneHeight - h - 6;
            QColor color = note.selected ? QColor("#ff9020") : noteColor(note.colorGroup);
            if (ghost) {
                color.setAlpha(90);
            }
            painter->setPen(QPen(color, 2));
            painter->drawLine(lineX, top + kVelocityLaneHeight - 6, lineX, yTop);
            painter->drawLine(lineX, yTop, x + std::max(14, std::min(w, 46)), yTop);
            painter->setBrush(color);
            painter->drawEllipse(QPointF(lineX, yTop), 4, 4);
        }

        painter->setPen(QColor("#596159"));
        painter->drawLine(kKeyboardWidth, top, viewport()->width(), top);
        painter->setPen(QColor("#93b36f"));
        painter->drawText(QRect(kKeyboardWidth + 10, top + 6, 160, 20), Qt::AlignLeft | Qt::AlignVCenter,
                          propertyTargetName(m_propertyTarget));
        painter->restore();
    }

    void drawRuler(QPainter* painter, int hScroll)
    {
        painter->save();
        painter->setTransform(QTransform());
        QRect ruler(0, 0, viewport()->width(), kRulerHeight);
        painter->fillRect(ruler, QColor("#242a24"));
        painter->fillRect(QRect(0, 0, kKeyboardWidth, kRulerHeight), QColor("#1b201b"));
        painter->setPen(QColor("#0a0c0a"));
        painter->drawLine(0, kRulerHeight - 1, viewport()->width(), kRulerHeight - 1);

        const double visibleStartBeat = std::max(0.0, (hScroll - kKeyboardWidth) / m_pixelsPerBeat);
        const double visibleEndBeat = (hScroll + viewport()->width() - kKeyboardWidth) / m_pixelsPerBeat;
        const int startBeat = std::max(0, static_cast<int>(std::floor(visibleStartBeat)) - 1);
        const int endBeat = static_cast<int>(std::ceil(visibleEndBeat)) + 1;
        QFont font = painter->font();
        font.setPointSize(8);
        font.setBold(true);
        painter->setFont(font);

        for (int beat = startBeat; beat <= endBeat; ++beat) {
            const int x = kKeyboardWidth + static_cast<int>(beat * m_pixelsPerBeat) - hScroll;
            painter->setPen(QColor("#8a928a"));
            painter->drawLine(x, 14, x, kRulerHeight - 1);
        }

        const std::vector<EditorBarMarker> markers = barMarkers(
            visibleStartBeat - 64.0, visibleEndBeat + 64.0);
        painter->setPen(QColor("#ff9020"));
        for (const EditorBarMarker& marker : markers) {
            const int x = kKeyboardWidth + static_cast<int>(marker.beat * m_pixelsPerBeat) - hScroll;
            painter->drawLine(x, 6, x, kRulerHeight - 1);
            painter->drawText(QRect(x + 5, 2, 52, kRulerHeight - 6), Qt::AlignLeft | Qt::AlignVCenter,
                              QString::number(marker.barNumber));
            if (marker.signatureChange) {
                painter->setPen(QColor("#f2c078"));
                painter->drawText(QRect(x + 5, 0, 54, 12), Qt::AlignLeft | Qt::AlignTop,
                    QString("%1/%2").arg(marker.numerator).arg(marker.denominator));
                painter->setPen(QColor("#ff9020"));
            }
        }

        painter->setPen(QColor("#809080"));
        painter->drawText(QRect(8, 0, kKeyboardWidth - 14, kRulerHeight), Qt::AlignVCenter | Qt::AlignLeft, "PIANO");
        painter->restore();
    }

    void drawPlayhead(QPainter* painter, int hScroll)
    {
        if (m_playheadBeat < 0.0) return;
        const int x = kKeyboardWidth + static_cast<int>(m_playheadBeat * m_pixelsPerBeat) - hScroll;
        if (x < kKeyboardWidth || x >= viewport()->width()) return;
        painter->save();
        painter->setPen(QPen(QColor("#ff593d"), 1));
        painter->drawLine(x, kRulerHeight, x, viewport()->height());
        painter->restore();
    }

    void startPreviewNote(int pitch, int channel, bool isKeyswitch)
    {
        const int safePitch = clampPianoPitch(pitch);
        const int safeChannel = qBound(0, channel, 15);
        if (m_previewPitch == safePitch && m_previewChannel == safeChannel &&
            m_previewIsKeyswitch == isKeyswitch) return;
        stopPreviewNote();
        m_previewPitch = safePitch;
        m_previewChannel = safeChannel;
        m_previewIsKeyswitch = isKeyswitch;
        if (m_notePreviewCallback)
            m_notePreviewCallback(m_previewPitch, m_previewChannel, true, m_previewIsKeyswitch);
        viewport()->update();
    }

    void startPreviewNote(int pitch)
    {
        startPreviewNote(pitch, m_currentColorGroup, false);
    }

    void stopPreviewNote()
    {
        if (m_previewPitch < 0) return;
        if (m_notePreviewCallback)
            m_notePreviewCallback(m_previewPitch, m_previewChannel, false, m_previewIsKeyswitch);
        m_previewPitch = -1;
        m_previewIsKeyswitch = false;
        viewport()->update();
    }

    void drawOverlay(QPainter* painter, int hScroll, int vScroll)
    {
        if (!m_creatingNote && !m_selecting && !m_slicing) {
            return;
        }
        painter->save();
        painter->setClipRect(QRect(kKeyboardWidth, 0, viewport()->width() - kKeyboardWidth, viewport()->height()));
        if (m_selecting) {
            QRectF rect(m_selectionStart, m_selectionCurrent);
            rect = rect.normalized();
            rect.translate(-hScroll, -vScroll);
            painter->setPen(QPen(QColor("#ff9020"), 1, Qt::DashLine));
            painter->setBrush(QColor(255, 144, 32, 45));
            painter->drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        }
        if (m_creatingNote) {
            MidiNote preview;
            preview.pitch = m_createPitch;
            preview.colorGroup = m_createChannel;
            const double createStart = std::min(m_createStartBeat, m_createCurrentBeat);
            const double createEnd = std::max(m_createStartBeat, m_createCurrentBeat);
            preview.startBeat = clampBeat(createStart);
            preview.lengthBeats = safeNoteLength(std::max(m_createLengthBeats, createEnd - createStart));
            preview.isKeyswitch = m_createIsKeyswitch;
            QRectF rect = noteRect(preview);
            rect.translate(-hScroll, -vScroll);
            painter->setPen(QPen(QColor("#ff9020"), 1, Qt::DashLine));
            painter->setBrush(QColor(123, 195, 66, 100));
            painter->drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        }
        if (m_slicing) {
            const QPointF start = m_sliceStart - QPointF(hScroll, vScroll);
            const QPointF current = m_sliceCurrent - QPointF(hScroll, vScroll);
            painter->setPen(QPen(QColor("#ff6a45"), 2, Qt::DashLine));
            painter->setBrush(Qt::NoBrush);
            painter->drawLine(start, current);
        }
        painter->restore();
    }

    int noteAt(const QPoint& pos) const
    {
        if (!m_clip) {
            return -1;
        }
        const QPointF contentPos = viewportToContent(pos);
        for (int i = static_cast<int>(m_clip->notes.size()) - 1; i >= 0; --i) {
            const MidiNote& note = m_clip->notes[i];
            if (!note.isKeyswitch && note.colorGroup != m_currentColorGroup) {
                continue;
            }
            if (noteRect(note).contains(contentPos)) {
                return i;
            }
        }
        return -1;
    }

    int ghostNoteAt(const QPoint& pos) const
    {
        if (!m_clip || !m_showGhostNotes) {
            return -1;
        }
        const QPointF contentPos = viewportToContent(pos);
        for (int i = static_cast<int>(m_clip->notes.size()) - 1; i >= 0; --i) {
            const MidiNote& note = m_clip->notes[i];
            if (note.isKeyswitch || note.colorGroup == m_currentColorGroup) {
                continue;
            }
            if (noteRect(note).contains(contentPos)) {
                return i;
            }
        }
        return -1;
    }

    bool isInKeyswitchLane(const QPointF& contentPos) const
    {
        return contentPos.y() >= keyswitchTop() &&
               contentPos.y() < keyswitchTop() + keyswitchRowCount() * kKeyswitchRowHeight;
    }

    bool isInEventLane(const QPointF& contentPos) const
    {
        return contentPos.y() >= velocityTop() &&
               contentPos.y() < velocityTop() + kVelocityLaneHeight &&
               contentPos.x() >= kKeyboardWidth;
    }

    int noteAtEventLane(const QPointF& contentPos) const
    {
        if (!m_clip || !isInEventLane(contentPos)) {
            return -1;
        }

        int bestIndex = -1;
        double bestDistance = 999999.0;
        for (int i = static_cast<int>(m_clip->notes.size()) - 1; i >= 0; --i) {
            const MidiNote& note = m_clip->notes[i];
            if (note.isKeyswitch || note.colorGroup != m_currentColorGroup) {
                continue;
            }
            const double noteX = kKeyboardWidth + note.startBeat * m_pixelsPerBeat;
            const double noteWidth = std::max(12.0, note.lengthBeats * m_pixelsPerBeat);
            if (contentPos.x() < noteX - 4.0 || contentPos.x() > noteX + noteWidth + 4.0) {
                continue;
            }
            const double distance = std::abs(contentPos.x() - noteX);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = i;
            }
        }
        return bestIndex;
    }

    void setNotePropertyFromY(MidiNote& note, double contentY)
    {
        const double laneBottom = velocityTop() + kVelocityLaneHeight - 6.0;
        const double laneTop = velocityTop() + 8.0;
        const double normalized = (laneBottom - qBound(laneTop, contentY, laneBottom)) /
                                  std::max(1.0, laneBottom - laneTop);
        setNotePropertyValue(note, static_cast<int>(std::round(normalized * 127.0)));
    }

    int pitchFromY(double y) const
    {
        const double relative = y - kTopPadding;
        const int row = static_cast<int>(std::floor(relative / kNoteHeight));
        return clampPianoPitch(kPianoRollMaxPitch - row);
    }

    int keyswitchRowFromY(double y) const
    {
        return qBound(0,
                      static_cast<int>(std::floor((y - keyswitchTop()) / kKeyswitchRowHeight)),
                      keyswitchRowCount() - 1);
    }

    double snapBeat(double beat) const
    {
        const double safeSnap = effectiveSnapBeats();
        return clampBeat(std::round(beat / safeSnap) * safeSnap);
    }

    int notePropertyValue(const MidiNote& note) const
    {
        switch (m_propertyTarget) {
        case NotePropertyTarget::Velocity: return note.velocity;
        case NotePropertyTarget::Pan: return note.pan;
        case NotePropertyTarget::Release: return note.release;
        case NotePropertyTarget::ModX: return note.modX;
        case NotePropertyTarget::ModY: return note.modY;
        }
        return note.velocity;
    }

    void setNotePropertyValue(MidiNote& note, int value)
    {
        const int safeValue = qBound(0, value, 127);
        switch (m_propertyTarget) {
        case NotePropertyTarget::Velocity:
            note.velocity = qBound(1, safeValue, 127);
            break;
        case NotePropertyTarget::Pan:
            note.pan = safeValue;
            break;
        case NotePropertyTarget::Release:
            note.release = safeValue;
            break;
        case NotePropertyTarget::ModX:
            note.modX = safeValue;
            break;
        case NotePropertyTarget::ModY:
            note.modY = safeValue;
            break;
        }
    }

    void adjustNoteProperty(int index, int delta)
    {
        if (!m_clip || index < 0 || index >= static_cast<int>(m_clip->notes.size())) {
            return;
        }
        pushUndoState();
        const bool adjustSelection = m_clip->notes[index].selected;
        for (int noteIndex = 0; noteIndex < static_cast<int>(m_clip->notes.size()); ++noteIndex) {
            MidiNote& note = m_clip->notes[noteIndex];
            if (noteIndex != index && (!adjustSelection || !note.selected)) continue;
            setNotePropertyValue(note, notePropertyValue(note) + delta);
        }
        viewport()->update();
        notifyChanged();
    }

    std::vector<int> selectedNoteIndices() const
    {
        std::vector<int> indices;
        if (!m_clip) {
            return indices;
        }
        for (int i = 0; i < static_cast<int>(m_clip->notes.size()); ++i) {
            if (m_clip->notes[i].selected) {
                indices.push_back(i);
            }
        }
        return indices;
    }

    int cloneSelectionForDrag(int anchorIndex)
    {
        if (!m_clip || anchorIndex < 0 || anchorIndex >= static_cast<int>(m_clip->notes.size()))
            return -1;
        if (!m_clip->notes[anchorIndex].selected) selectNote(anchorIndex, false);
        const std::vector<int> targets = selectedNoteIndices();
        if (targets.empty()) return -1;

        std::vector<std::pair<int, int>> clonedGroups;
        const int firstNewIndex = static_cast<int>(m_clip->notes.size());
        int clonedAnchor = -1;
        for (int index : targets) {
            if (index < 0 || index >= firstNewIndex) continue;
            MidiNote& original = m_clip->notes[index];
            MidiNote copy = original;
            original.selected = false;
            copy.selected = true;
            if (copy.groupId > 0) {
                auto group = std::find_if(clonedGroups.begin(), clonedGroups.end(),
                    [&](const std::pair<int, int>& entry) { return entry.first == copy.groupId; });
                if (group == clonedGroups.end()) {
                    clonedGroups.push_back({copy.groupId, m_nextGroupId++});
                    copy.groupId = clonedGroups.back().second;
                } else {
                    copy.groupId = group->second;
                }
            }
            m_clip->notes.push_back(copy);
            if (index == anchorIndex) clonedAnchor = static_cast<int>(m_clip->notes.size()) - 1;
        }
        return clonedAnchor >= 0 ? clonedAnchor : firstNewIndex;
    }

    void captureDragSelection()
    {
        m_dragSnapshots.clear();
        if (!m_clip || m_activeNote < 0 || m_activeNote >= static_cast<int>(m_clip->notes.size())) {
            return;
        }

        std::vector<int> targets = selectedNoteIndices();
        if (targets.empty()) {
            targets.push_back(m_activeNote);
        }
        m_dragSnapshots.reserve(targets.size());
        for (int index : targets) {
            if (index < 0 || index >= static_cast<int>(m_clip->notes.size())) {
                continue;
            }
            const MidiNote& note = m_clip->notes[index];
            m_dragSnapshots.push_back(
                {index, note.startBeat, note.pitch, note.colorGroup, note.lengthBeats});
        }
    }

    std::vector<int> selectedOrAllEditableNoteIndices() const
    {
        std::vector<int> indices = selectedNoteIndices();
        if (!m_clip || !indices.empty()) {
            return indices;
        }
        for (int i = 0; i < static_cast<int>(m_clip->notes.size()); ++i) {
            const MidiNote& note = m_clip->notes[i];
            if (note.isKeyswitch || note.colorGroup == m_currentColorGroup) {
                indices.push_back(i);
            }
        }
        return indices;
    }

    void openNotePropertiesDialog(int anchorIndex)
    {
        if (!m_clip || anchorIndex < 0 || anchorIndex >= static_cast<int>(m_clip->notes.size())) {
            return;
        }

        std::vector<int> targets = selectedNoteIndices();
        if (targets.empty()) {
            targets.push_back(anchorIndex);
        }

        MidiNote& anchor = m_clip->notes[anchorIndex];
        const bool multiEdit = targets.size() > 1;

        QDialog dialog(this);
        dialog.setWindowTitle(multiEdit ? "Note Properties - Selection" : "Note Properties");
        auto* layout = new QVBoxLayout(&dialog);
        auto* form = new QFormLayout();
        layout->addLayout(form);

        QSpinBox* pitch = nullptr;
        QDoubleSpinBox* start = nullptr;
        QDoubleSpinBox* duration = nullptr;
        if (!multiEdit) {
            pitch = new QSpinBox(&dialog);
            pitch->setRange(anchor.isKeyswitch ? 0 : kPianoRollMinPitch,
                            anchor.isKeyswitch ? 127 : kPianoRollMaxPitch);
            pitch->setValue(anchor.isKeyswitch
                ? qBound(0, anchor.pitch, 127)
                : clampPianoPitch(anchor.pitch));
            pitch->setToolTip(anchor.isKeyswitch ? "Expression Map MIDI trigger" : "MIDI pitch");
            form->addRow(anchor.isKeyswitch ? "Keyswitch" : "Pitch", pitch);

            start = new QDoubleSpinBox(&dialog);
            start->setRange(0.0, 9999.0);
            start->setDecimals(6);
            start->setSingleStep(effectiveSnapBeats());
            start->setValue(clampBeat(anchor.startBeat));
            form->addRow("Start beat", start);

            duration = new QDoubleSpinBox(&dialog);
            duration->setRange(kMinNoteLength, 9999.0);
            duration->setDecimals(6);
            duration->setSingleStep(effectiveSnapBeats());
            duration->setValue(safeNoteLength(anchor.lengthBeats));
            form->addRow("Duration", duration);
        } else {
            QLabel* selectionHint = new QLabel(
                QString("%1 notes selected. Time and pitch are unchanged for multi-edit.").arg(targets.size()),
                &dialog);
            selectionHint->setWordWrap(true);
            selectionHint->setStyleSheet("color: #999;");
            layout->addWidget(selectionHint);
        }

        auto makeLevel = [&](int value) {
            QSpinBox* spin = new QSpinBox(&dialog);
            spin->setRange(0, 127);
            spin->setValue(qBound(0, value, 127));
            return spin;
        };

        QSpinBox* velocity = makeLevel(anchor.velocity);
        velocity->setRange(1, 127);
        form->addRow("Velocity", velocity);
        QSpinBox* pan = makeLevel(anchor.pan);
        form->addRow("Pan", pan);
        QSpinBox* release = makeLevel(anchor.release);
        form->addRow("Release", release);
        QSpinBox* modX = makeLevel(anchor.modX);
        form->addRow("Mod X", modX);
        QSpinBox* modY = makeLevel(anchor.modY);
        form->addRow("Mod Y", modY);

        QComboBox* color = new QComboBox(&dialog);
        for (int group = 0; group < 16; ++group) {
            color->addItem(QString("Color %1 / MIDI Ch %1").arg(group + 1), group);
        }
        color->setCurrentIndex(qBound(0, anchor.colorGroup, 15));
        form->addRow("Color group", color);

        QCheckBox* slide = new QCheckBox("Slide note (control marker; does not export as a note)", &dialog);
        slide->setChecked(anchor.isSlide);
        form->addRow("Slide", slide);
        QCheckBox* porta = new QCheckBox("Portamento flag (native synth glide; plugins receive a normal MIDI note)", &dialog);
        porta->setChecked(anchor.isPortamento);
        form->addRow("Portamento", porta);
        QCheckBox* muted = new QCheckBox("Muted", &dialog);
        muted->setChecked(anchor.muted);
        form->addRow("Mute", muted);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        if (dialog.exec() != QDialog::Accepted) {
            return;
        }

        pushUndoState();
        for (int index : targets) {
            if (index < 0 || index >= static_cast<int>(m_clip->notes.size())) {
                continue;
            }
            MidiNote& note = m_clip->notes[index];
            if (!multiEdit) {
                note.pitch = note.isKeyswitch
                    ? qBound(0, pitch->value(), 127)
                    : clampPianoPitch(pitch->value());
                note.startBeat = clampBeat(start->value());
                note.lengthBeats = safeNoteLength(duration->value());
                m_clip->lastNoteLengthBeats = note.lengthBeats;
                m_lastNoteLengthBeats = note.lengthBeats;
                m_lastCreatedNote = index;
            }
            note.velocity = qBound(1, velocity->value(), 127);
            note.pan = qBound(0, pan->value(), 127);
            note.release = qBound(0, release->value(), 127);
            note.modX = qBound(0, modX->value(), 127);
            note.modY = qBound(0, modY->value(), 127);
            note.colorGroup = qBound(0, color->currentData().toInt(), 15);
            if (!multiEdit && note.isKeyswitch) {
                note.colorGroup = expressionChannelForPitch(note.pitch, note.colorGroup);
            }
            if (!note.isKeyswitch) {
                note.isSlide = slide->isChecked();
                note.isPortamento = porta->isChecked();
            }
            note.muted = muted->isChecked();
            note.selected = true;
        }

        sortNotes();
        rebuildExpressionRows();
        updateScrollRanges();
        viewport()->update();
        notifyChanged();
    }

    void clearSelection()
    {
        if (!m_clip) {
            return;
        }
        for (auto& note : m_clip->notes) {
            note.selected = false;
        }
    }

    void selectNote(int index, bool additive)
    {
        if (!m_clip || index < 0 || index >= static_cast<int>(m_clip->notes.size())) {
            return;
        }
        if (!additive) {
            clearSelection();
        }
        m_clip->notes[index].selected = true;
        const int groupId = m_clip->notes[index].groupId;
        if (groupId > 0) {
            for (MidiNote& note : m_clip->notes) {
                if (note.groupId == groupId) {
                    note.selected = true;
                }
            }
        }
        m_selectedNote = index;
        m_lastCreatedNote = index;
    }

    void selectAllNotes()
    {
        if (!m_clip) {
            return;
        }
        for (MidiNote& note : m_clip->notes) {
            note.selected = true;
        }
        m_selectedNote = m_clip->notes.empty() ? -1 : 0;
        viewport()->update();
    }

    void selectNotesInRect(const QRectF& selectionRect, bool additive)
    {
        if (!m_clip) {
            return;
        }
        if (!additive) {
            clearSelection();
        }

        m_selectedNote = -1;
        for (int i = 0; i < static_cast<int>(m_clip->notes.size()); ++i) {
            MidiNote& note = m_clip->notes[i];
            if (!note.isKeyswitch && note.colorGroup != m_currentColorGroup) {
                continue;
            }
            if (noteRect(note).intersects(selectionRect)) {
                note.selected = true;
                if (m_selectedNote < 0) {
                    m_selectedNote = i;
                }
            }
        }
        m_lastCreatedNote = m_selectedNote;
    }

    void selectCurrentColorGroup()
    {
        if (!m_clip) {
            return;
        }

        clearSelection();
        for (int i = 0; i < static_cast<int>(m_clip->notes.size()); ++i) {
            MidiNote& note = m_clip->notes[i];
            if (note.isKeyswitch || note.colorGroup != m_currentColorGroup) {
                continue;
            }
            note.selected = true;
            if (m_selectedNote < 0) {
                m_selectedNote = i;
            }
        }
        m_lastCreatedNote = m_selectedNote;
        viewport()->update();
    }

    void groupSelectedNotes()
    {
        if (!m_clip) {
            return;
        }
        const std::vector<int> targets = selectedNoteIndices();
        if (targets.size() < 2) {
            return;
        }
        pushUndoState();
        const int groupId = m_nextGroupId++;
        for (int index : targets) {
            if (index < 0 || index >= static_cast<int>(m_clip->notes.size())) {
                continue;
            }
            m_clip->notes[index].groupId = groupId;
        }
        viewport()->update();
        notifyChanged();
    }

    void glueSelectedTouchingNotes()
    {
        if (!m_clip) {
            return;
        }

        std::vector<int> targets = selectedNoteIndices();
        if (targets.size() < 2) {
            return;
        }
        pushUndoState();

        std::stable_sort(targets.begin(), targets.end(), [this](int left, int right) {
            const MidiNote& a = m_clip->notes[left];
            const MidiNote& b = m_clip->notes[right];
            if (a.isKeyswitch != b.isKeyswitch) return a.isKeyswitch < b.isKeyswitch;
            if (a.pitch != b.pitch) return a.pitch < b.pitch;
            if (a.colorGroup != b.colorGroup) return a.colorGroup < b.colorGroup;
            if (a.isSlide != b.isSlide) return a.isSlide < b.isSlide;
            if (a.isPortamento != b.isPortamento) return a.isPortamento < b.isPortamento;
            return a.startBeat < b.startBeat;
        });

        auto sameGlueLane = [this](int left, int right) {
            const MidiNote& a = m_clip->notes[left];
            const MidiNote& b = m_clip->notes[right];
            return a.isKeyswitch == b.isKeyswitch &&
                a.pitch == b.pitch &&
                a.colorGroup == b.colorGroup &&
                a.isSlide == b.isSlide &&
                a.isPortamento == b.isPortamento;
        };

        constexpr double kTouchEpsilon = 0.0001;
        std::vector<bool> remove(m_clip->notes.size(), false);
        bool changed = false;
        int chainBase = targets.front();
        double chainEnd = m_clip->notes[chainBase].startBeat + safeNoteLength(m_clip->notes[chainBase].lengthBeats);

        for (int i = 1; i < static_cast<int>(targets.size()); ++i) {
            const int index = targets[i];
            MidiNote& note = m_clip->notes[index];
            const bool canGlue = sameGlueLane(chainBase, index) && note.startBeat <= chainEnd + kTouchEpsilon;
            if (!canGlue) {
                chainBase = index;
                chainEnd = note.startBeat + safeNoteLength(note.lengthBeats);
                continue;
            }

            const double noteEnd = note.startBeat + safeNoteLength(note.lengthBeats);
            MidiNote& base = m_clip->notes[chainBase];
            base.lengthBeats = safeNoteLength(std::max(chainEnd, noteEnd) - base.startBeat);
            chainEnd = base.startBeat + base.lengthBeats;
            remove[index] = true;
            changed = true;
        }

        if (!changed) {
            return;
        }

        const MidiNote* noteBase = m_clip->notes.data();
        m_clip->notes.erase(std::remove_if(m_clip->notes.begin(), m_clip->notes.end(),
            [&](const MidiNote& note) {
                return remove[static_cast<size_t>(&note - noteBase)];
            }), m_clip->notes.end());

        m_selectedNote = -1;
        for (int i = 0; i < static_cast<int>(m_clip->notes.size()); ++i) {
            if (m_clip->notes[i].selected) {
                m_selectedNote = i;
                break;
            }
        }
        m_lastCreatedNote = m_selectedNote;
        if (m_selectedNote >= 0) {
            m_lastNoteLengthBeats = safeNoteLength(m_clip->notes[m_selectedNote].lengthBeats);
            m_clip->lastNoteLengthBeats = m_lastNoteLengthBeats;
        }

        sortNotes();
        updateScrollRanges();
        viewport()->update();
        notifyChanged();
    }

    void ungroupSelectedNotes()
    {
        if (!m_clip) {
            return;
        }
        pushUndoState();
        bool changed = false;
        for (MidiNote& note : m_clip->notes) {
            if (note.selected && note.groupId != 0) {
                note.groupId = 0;
                changed = true;
            }
        }
        if (changed) {
            viewport()->update();
            notifyChanged();
        }
    }

    void deleteSelectedNotes()
    {
        if (!m_clip) {
            return;
        }
        const bool canDelete = std::any_of(m_clip->notes.begin(), m_clip->notes.end(),
            [](const MidiNote& note) { return note.selected; }) ||
            (m_selectedNote >= 0 && m_selectedNote < static_cast<int>(m_clip->notes.size()));
        if (!canDelete) {
            return;
        }
        pushUndoState();
        const auto before = m_clip->notes.size();
        m_clip->notes.erase(std::remove_if(m_clip->notes.begin(), m_clip->notes.end(),
            [](const MidiNote& note) { return note.selected; }), m_clip->notes.end());
        if (m_clip->notes.size() == before && m_selectedNote >= 0 &&
            m_selectedNote < static_cast<int>(m_clip->notes.size())) {
            m_clip->notes.erase(m_clip->notes.begin() + m_selectedNote);
        }
        m_selectedNote = -1;
        m_lastCreatedNote = -1;
        rebuildExpressionRows();
        updateScrollRanges();
        viewport()->update();
        notifyChanged();
    }

    bool copySelection()
    {
        if (!m_clip) {
            return false;
        }

        const std::vector<int> targets = selectedNoteIndices();
        if (targets.empty()) {
            return false;
        }

        double firstBeat = std::numeric_limits<double>::max();
        double lastBeat = 0.0;
        for (int index : targets) {
            if (index < 0 || index >= static_cast<int>(m_clip->notes.size())) {
                continue;
            }
            const MidiNote& note = m_clip->notes[index];
            firstBeat = std::min(firstBeat, note.startBeat);
            lastBeat = std::max(lastBeat, note.startBeat + nonNegativeDouble(note.lengthBeats));
        }
        if (firstBeat == std::numeric_limits<double>::max()) {
            return false;
        }

        g_noteClipboard.clear();
        g_noteClipboard.reserve(targets.size());
        for (int index : targets) {
            if (index < 0 || index >= static_cast<int>(m_clip->notes.size())) {
                continue;
            }
            MidiNote copy = m_clip->notes[index];
            copy.startBeat = clampBeat(copy.startBeat - firstBeat);
            copy.selected = false;
            g_noteClipboard.push_back(copy);
        }
        g_noteClipboardSpanBeats = std::max(kMinNoteLength, lastBeat - firstBeat);
        return !g_noteClipboard.empty();
    }

    void cutSelection()
    {
        if (copySelection()) {
            deleteSelectedNotes();
        }
    }

    void pasteSelection()
    {
        if (!m_clip || g_noteClipboard.empty()) {
            return;
        }
        pushUndoState();

        double pasteBeat = 0.0;
        const std::vector<int> selected = selectedNoteIndices();
        if (!selected.empty()) {
            for (int index : selected) {
                if (index < 0 || index >= static_cast<int>(m_clip->notes.size())) {
                    continue;
                }
                const MidiNote& note = m_clip->notes[index];
                pasteBeat = std::max(pasteBeat, note.startBeat + nonNegativeDouble(note.lengthBeats));
            }
        } else {
            for (const MidiNote& note : m_clip->notes) {
                pasteBeat = std::max(pasteBeat, note.startBeat + nonNegativeDouble(note.lengthBeats));
            }
        }
        pasteBeat = snapBeat(pasteBeat);

        clearSelection();
        const int firstNewIndex = static_cast<int>(m_clip->notes.size());
        for (const MidiNote& source : g_noteClipboard) {
            MidiNote pasted = source;
            pasted.startBeat = clampBeat(pasteBeat + source.startBeat);
            pasted.lengthBeats = safeNoteLength(source.lengthBeats);
            pasted.selected = true;
            m_clip->notes.push_back(pasted);
        }

        m_selectedNote = firstNewIndex < static_cast<int>(m_clip->notes.size()) ? firstNewIndex : -1;
        m_lastCreatedNote = m_selectedNote;
        if (g_noteClipboardSpanBeats > 0.0) {
            m_clip->lastNoteLengthBeats = safeNoteLength(g_noteClipboard.front().lengthBeats);
            m_lastNoteLengthBeats = m_clip->lastNoteLengthBeats;
        }
        sortNotes();
        rebuildExpressionRows();
        updateScrollRanges();
        viewport()->update();
        notifyChanged();
    }

    void duplicateSelection()
    {
        if (!m_clip) {
            return;
        }

        const std::vector<int> targets = selectedOrAllEditableNoteIndices();
        if (targets.empty()) {
            return;
        }

        double firstBeat = std::numeric_limits<double>::max();
        double lastBeat = 0.0;
        for (int index : targets) {
            if (index < 0 || index >= static_cast<int>(m_clip->notes.size())) {
                continue;
            }
            const MidiNote& note = m_clip->notes[index];
            firstBeat = std::min(firstBeat, note.startBeat);
            lastBeat = std::max(lastBeat, note.startBeat + nonNegativeDouble(note.lengthBeats));
        }
        if (firstBeat == std::numeric_limits<double>::max()) {
            return;
        }

        pushUndoState();
        const double safeSnap = std::max(effectiveSnapBeats(), kMinNoteLength);
        const double span = std::max(kMinNoteLength, lastBeat - firstBeat);
        const double repeatLength = std::max(safeSnap, std::ceil(span / safeSnap) * safeSnap);
        clearSelection();
        const int firstNewIndex = static_cast<int>(m_clip->notes.size());
        for (int index : targets) {
            if (index < 0 || index >= firstNewIndex) {
                continue;
            }
            MidiNote copy = m_clip->notes[index];
            copy.startBeat = clampBeat(copy.startBeat + repeatLength);
            copy.lengthBeats = safeNoteLength(copy.lengthBeats);
            copy.selected = true;
            m_clip->notes.push_back(copy);
        }

        m_selectedNote = firstNewIndex < static_cast<int>(m_clip->notes.size()) ? firstNewIndex : -1;
        m_lastCreatedNote = m_selectedNote;
        sortNotes();
        updateScrollRanges();
        viewport()->update();
        notifyChanged();
    }

    void quantizeSelection()
    {
        if (!m_clip) {
            return;
        }

        const std::vector<int> targets = selectedOrAllEditableNoteIndices();
        if (targets.empty()) {
            return;
        }
        pushUndoState();

        bool changed = false;
        for (int index : targets) {
            if (index < 0 || index >= static_cast<int>(m_clip->notes.size())) {
                continue;
            }
            MidiNote& note = m_clip->notes[index];
            const double startBeat = snapBeat(note.startBeat);
            const double endBeat = snapBeat(note.startBeat + note.lengthBeats);
            const double length = std::max(kMinNoteLength, endBeat - startBeat);
            if (note.startBeat != startBeat || note.lengthBeats != length) {
                note.startBeat = startBeat;
                note.lengthBeats = length;
                note.selected = true;
                changed = true;
            }
        }

        if (changed) {
            sortNotes();
            updateScrollRanges();
            viewport()->update();
            notifyChanged();
        }
    }

    void quickLegatoSelection()
    {
        if (!m_clip) {
            return;
        }

        const std::vector<int> targets = selectedOrAllEditableNoteIndices();
        if (targets.empty()) {
            return;
        }
        pushUndoState();

        std::vector<int> editable;
        editable.reserve(targets.size());
        for (int index : targets) {
            if (index >= 0 && index < static_cast<int>(m_clip->notes.size()) &&
                !m_clip->notes[index].isKeyswitch) {
                editable.push_back(index);
            }
        }
        std::stable_sort(editable.begin(), editable.end(), [this](int a, int b) {
            const MidiNote& left = m_clip->notes[a];
            const MidiNote& right = m_clip->notes[b];
            if (left.colorGroup != right.colorGroup) return left.colorGroup < right.colorGroup;
            if (left.pitch != right.pitch) return left.pitch < right.pitch;
            return left.startBeat < right.startBeat;
        });

        bool changed = false;
        for (int i = 0; i + 1 < static_cast<int>(editable.size()); ++i) {
            MidiNote& note = m_clip->notes[editable[i]];
            const MidiNote& next = m_clip->notes[editable[i + 1]];
            if (note.pitch != next.pitch || note.colorGroup != next.colorGroup ||
                next.startBeat <= note.startBeat) {
                continue;
            }
            const double newLength = std::max(kMinNoteLength, next.startBeat - note.startBeat);
            if (note.lengthBeats != newLength) {
                note.lengthBeats = newLength;
                note.selected = true;
                changed = true;
            }
        }

        if (changed) {
            sortNotes();
            updateScrollRanges();
            viewport()->update();
            notifyChanged();
        }
    }

    void nudgeSelection(double beatDelta, int pitchDelta)
    {
        if (!m_clip) {
            return;
        }
        const bool hasTarget = std::any_of(m_clip->notes.begin(), m_clip->notes.end(),
            [](const MidiNote& note) { return note.selected; }) ||
            (m_lastCreatedNote >= 0 && m_lastCreatedNote < static_cast<int>(m_clip->notes.size()));
        if (!hasTarget) {
            return;
        }
        pushUndoState();
        double earliestBeat = std::numeric_limits<double>::max();
        for (const MidiNote& note : m_clip->notes)
            if (note.selected) earliestBeat = std::min(earliestBeat, note.startBeat);
        if (earliestBeat != std::numeric_limits<double>::max())
            beatDelta = std::max(beatDelta, -earliestBeat);
        bool changed = false;
        for (MidiNote& note : m_clip->notes) {
            if (!note.selected) {
                continue;
            }
            note.startBeat = clampBeat(note.startBeat + beatDelta);
            if (!note.isKeyswitch && pitchDelta != 0) {
                note.pitch = snapPitchToScale(clampPianoPitch(note.pitch + pitchDelta));
            }
            changed = true;
        }
        if (!changed && m_lastCreatedNote >= 0 && m_lastCreatedNote < static_cast<int>(m_clip->notes.size())) {
            MidiNote& note = m_clip->notes[m_lastCreatedNote];
            note.startBeat = clampBeat(note.startBeat + beatDelta);
            if (!note.isKeyswitch && pitchDelta != 0) {
                note.pitch = snapPitchToScale(clampPianoPitch(note.pitch + pitchDelta));
            }
            changed = true;
        }
        if (changed) {
            sortNotes();
            viewport()->update();
            notifyChanged();
        }
    }

    void cyclePropertyTarget()
    {
        const int next = (static_cast<int>(m_propertyTarget) + 1) % 5;
        setPropertyTarget(static_cast<NotePropertyTarget>(next));
    }

    void changeSelectedNotesToCurrentColor()
    {
        if (!m_clip) {
            return;
        }

        pushUndoState();
        bool changed = false;
        bool hadSelection = false;
        for (MidiNote& note : m_clip->notes) {
            if (!note.selected) {
                continue;
            }
            hadSelection = true;
            if (!note.isKeyswitch && note.colorGroup != m_currentColorGroup) {
                note.colorGroup = m_currentColorGroup;
                changed = true;
            }
        }
        if (!hadSelection &&
            m_lastCreatedNote >= 0 &&
            m_lastCreatedNote < static_cast<int>(m_clip->notes.size())) {
            MidiNote& note = m_clip->notes[m_lastCreatedNote];
            if (!note.isKeyswitch && note.colorGroup != m_currentColorGroup) {
                note.colorGroup = m_currentColorGroup;
                note.selected = true;
                m_selectedNote = m_lastCreatedNote;
                changed = true;
            }
        }
        if (changed) {
            viewport()->update();
            notifyChanged();
        }
    }

    void deleteNote(int index)
    {
        if (!m_clip || index < 0 || index >= static_cast<int>(m_clip->notes.size())) {
            return;
        }
        m_clip->notes.erase(m_clip->notes.begin() + index);
        if (m_selectedNote == index) {
            m_selectedNote = -1;
        } else if (m_selectedNote > index) {
            --m_selectedNote;
        }
        if (m_lastCreatedNote == index) {
            m_lastCreatedNote = -1;
        } else if (m_lastCreatedNote > index) {
            --m_lastCreatedNote;
        }
        rebuildExpressionRows();
        updateScrollRanges();
        viewport()->update();
        notifyChanged();
    }

    void transposeSelection(int delta)
    {
        if (!m_clip) {
            return;
        }

        std::vector<int> targets;
        for (int i = 0; i < static_cast<int>(m_clip->notes.size()); ++i) {
            if (m_clip->notes[i].selected) {
                targets.push_back(i);
            }
        }
        if (targets.empty() && m_lastCreatedNote >= 0 && m_lastCreatedNote < static_cast<int>(m_clip->notes.size())) {
            targets.push_back(m_lastCreatedNote);
        }
        if (targets.empty()) {
            return;
        }
        pushUndoState();

        int minimumPitch = kPianoRollMaxPitch;
        int maximumPitch = kPianoRollMinPitch;
        bool hasPitchedNotes = false;
        for (int index : targets) {
            const MidiNote& note = m_clip->notes[index];
            if (note.isKeyswitch) continue;
            minimumPitch = std::min(minimumPitch, note.pitch);
            maximumPitch = std::max(maximumPitch, note.pitch);
            hasPitchedNotes = true;
        }
        if (hasPitchedNotes)
            delta = qBound(kPianoRollMinPitch - minimumPitch, delta,
                           kPianoRollMaxPitch - maximumPitch);
        for (int index : targets) {
            MidiNote& note = m_clip->notes[index];
            if (note.isKeyswitch) {
                continue;
            }
            note.pitch = snapPitchToScale(clampPianoPitch(note.pitch + delta));
        }
        viewport()->update();
        notifyChanged();
    }

    void sliceNotesAlongLine(const QPointF& start, const QPointF& end, bool bypassSnap)
    {
        if (!m_clip || m_clip->notes.empty()) return;
        struct CutPoint
        {
            int noteIndex = -1;
            double beat = 0.0;
        };
        std::vector<CutPoint> cuts;
        const double minimumY = std::min(start.y(), end.y());
        const double maximumY = std::max(start.y(), end.y());
        const double deltaY = end.y() - start.y();
        const double deltaX = end.x() - start.x();
        const int originalCount = static_cast<int>(m_clip->notes.size());
        cuts.reserve(static_cast<size_t>(originalCount));
        for (int index = 0; index < originalCount; ++index) {
            const MidiNote& note = m_clip->notes[index];
            const QRectF rect = noteRect(note);
            if (maximumY < rect.top() || minimumY > rect.bottom()) continue;

            double cutX = end.x();
            if (std::fabs(deltaY) >= 1.0) {
                const double targetY = std::clamp(rect.center().y(), minimumY, maximumY);
                const double linePosition = (targetY - start.y()) / deltaY;
                if (linePosition < 0.0 || linePosition > 1.0) continue;
                cutX = start.x() + deltaX * linePosition;
            }
            if (cutX <= rect.left() || cutX >= rect.right()) continue;
            const double rawBeat = (cutX - kKeyboardWidth) / m_pixelsPerBeat;
            const double cutBeat = bypassSnap ? clampBeat(rawBeat) : snapBeat(rawBeat);
            const double noteEnd = note.startBeat + note.lengthBeats;
            if (cutBeat - note.startBeat < kMinNoteLength ||
                noteEnd - cutBeat < kMinNoteLength) continue;
            cuts.push_back({index, cutBeat});
        }
        if (cuts.empty()) return;

        pushUndoState();
        for (const CutPoint& cut : cuts) {
            MidiNote& left = m_clip->notes[cut.noteIndex];
            MidiNote right = left;
            const double originalEnd = left.startBeat + left.lengthBeats;
            left.lengthBeats = safeNoteLength(cut.beat - left.startBeat);
            right.startBeat = cut.beat;
            right.lengthBeats = safeNoteLength(originalEnd - cut.beat);
            m_clip->notes.push_back(right);
        }
        sortNotes();
        updateScrollRanges();
        notifyChanged();
    }

    void notifyChanged()
    {
        if (m_changeCallback && m_clip && m_editBeforeDocument) {
            m_changeCallback(*m_editBeforeDocument, *m_clip, m_editSerial);
        }
    }

    void sortNotes()
    {
        // Sorting reorders the vector, so index-based references must be
        // remapped afterwards or selection/transpose target the wrong note.
        const int noteCount = static_cast<int>(m_clip->notes.size());
        const bool hadSelected = m_selectedNote >= 0 && m_selectedNote < noteCount;
        const bool hadLastCreated = m_lastCreatedNote >= 0 && m_lastCreatedNote < noteCount;
        const MidiNote selectedCopy = hadSelected ? m_clip->notes[m_selectedNote] : MidiNote{};
        const MidiNote lastCreatedCopy = hadLastCreated ? m_clip->notes[m_lastCreatedNote] : MidiNote{};

        std::stable_sort(m_clip->notes.begin(), m_clip->notes.end(), [](const MidiNote& a, const MidiNote& b) {
            if (a.startBeat != b.startBeat) return a.startBeat < b.startBeat;
            if (a.pitch != b.pitch) return a.pitch < b.pitch;
            return a.lengthBeats < b.lengthBeats;
        });

        auto findNote = [this](const MidiNote& target) -> int {
            for (int i = 0; i < static_cast<int>(m_clip->notes.size()); ++i) {
                const MidiNote& note = m_clip->notes[i];
                if (note.pitch == target.pitch && note.startBeat == target.startBeat &&
                    note.lengthBeats == target.lengthBeats && note.velocity == target.velocity &&
                    note.pan == target.pan && note.release == target.release &&
                    note.modX == target.modX && note.modY == target.modY &&
                    note.colorGroup == target.colorGroup && note.groupId == target.groupId &&
                    note.selected == target.selected &&
                    note.isKeyswitch == target.isKeyswitch && note.isSlide == target.isSlide &&
                    note.isPortamento == target.isPortamento && note.muted == target.muted) {
                    return i;
                }
            }
            return -1;
        };
        if (hadSelected) {
            m_selectedNote = findNote(selectedCopy);
        }
        if (hadLastCreated) {
            m_lastCreatedNote = findNote(lastCreatedCopy);
        }
    }
};

MidiEditorWindow::MidiEditorWindow(QWidget* parent)
    : QWidget(parent)
    , m_clip(nullptr)
    , m_clipLabel(nullptr)
    , m_trackLabel(nullptr)
    , m_snapLabel(nullptr)
    , m_snapCombo(nullptr)
    , m_channelCombo(nullptr)
    , m_eventTargetCombo(nullptr)
    , m_stampCombo(nullptr)
    , m_colorButton(nullptr)
    , m_slideButton(nullptr)
    , m_portaButton(nullptr)
    , m_ghostButton(nullptr)
    , m_scaleButton(nullptr)
    , m_canvas(nullptr)
{
    setWindowTitle("MIDI Editor");
    setWindowFlags(windowFlags() | Qt::Window);
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1280, 760);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* topBar = new QWidget(this);
    topBar->setObjectName("MidiEditorTopBar");
    topBar->setStyleSheet("#MidiEditorTopBar { background-color: #1d221d; border-bottom: 1px solid #0d0d0d; }");
    auto* topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(10, 8, 10, 8);
    topLayout->setSpacing(5);

    m_clipLabel = new QLabel("No clip", topBar);
    m_trackLabel = new QLabel("Track", topBar);
    m_snapLabel = new QLabel("Snap", topBar);
    for (QLabel* label : {m_clipLabel, m_trackLabel}) {
        label->setStyleSheet("color: #dfe8dc; font-weight: bold; padding: 3px 8px; "
                             "background-color: #262c26; border: 1px solid #101410; border-radius: 3px;");
    }

    const QString toolButtonStyle =
        "QToolButton { color: #dfe8dc; background: #2b332b; border: 1px solid #101410; "
        "border-radius: 3px; min-width: 26px; min-height: 24px; font-weight: bold; } "
        "QToolButton:hover { border-color: #ff9020; color: #ffffff; } "
        "QToolButton:focus { border-color: #ffb05a; } "
        "QToolButton:pressed { background: #171c17; } "
        "QToolButton:checked { background: #ff9020; color: #101410; } "
        "QToolButton:checked:hover { background: #ffad57; } "
        "QToolButton:disabled { color: #697269; background: #242924; border-color: #181c18; }";
    const QString comboStyle =
        "QComboBox { color: #dfe8dc; background: #262c26; border: 1px solid #101410; "
        "border-radius: 3px; min-height: 24px; padding: 2px 8px; } "
        "QComboBox:hover { border-color: #ff9020; } "
        "QComboBox:focus { border-color: #ffb05a; } "
        "QComboBox:disabled { color: #697269; background: #242924; border-color: #181c18; }";
    auto makeTool = [&](const QString& text, const QString& tip, bool checkable = false) {
        auto* button = new QToolButton(topBar);
        button->setText(text);
        button->setToolTip(tip);
        button->setStatusTip(tip);
        button->setAccessibleName(tip);
        button->setCheckable(checkable);
        button->setStyleSheet(toolButtonStyle);
        return button;
    };

    auto* mainMenu = makeTool("v", "Piano roll menu");
    auto* menu = new QMenu(mainMenu);
    QAction* quickQuantizeAction = menu->addAction("Quick quantize");
    quickQuantizeAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Q")));
    QAction* quickLegatoAction = menu->addAction("Quick legato");
    quickLegatoAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    QAction* duplicateAction = menu->addAction("Duplicate selection");
    duplicateAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
    QAction* copyAction = menu->addAction("Copy");
    copyAction->setShortcut(QKeySequence(QKeySequence::Copy));
    QAction* cutAction = menu->addAction("Cut");
    cutAction->setShortcut(QKeySequence(QKeySequence::Cut));
    QAction* pasteAction = menu->addAction("Paste");
    pasteAction->setShortcut(QKeySequence(QKeySequence::Paste));
    QAction* selectAllAction = menu->addAction("Select all");
    selectAllAction->setShortcut(QKeySequence(QKeySequence::SelectAll));
    mainMenu->setMenu(menu);
    mainMenu->setPopupMode(QToolButton::InstantPopup);

    auto* drawTool = makeTool("P", "Draw tool (P)", true);
    drawTool->setChecked(true);
    auto* paintTool = makeTool("B", "Paint tool (B)", true);
    auto* stampTool = makeTool("Stamp", "Chord Stamp: click to insert a grouped chord", true);
    auto* selectTool = makeTool("E", "Select tool (E)", true);
    auto* deleteTool = makeTool("D", "Delete tool (D)", true);
    auto* muteTool = makeTool("T", "Mute tool (T): click or drag across notes", true);
    auto* sliceTool = makeTool("C", "Slice tool (C): drag a cut line through notes; hold Alt to bypass snap", true);

    m_stampCombo = new QComboBox(topBar);
    m_stampCombo->setToolTip("Chord shape; Automatic builds a diatonic triad from selected scale");
    m_stampCombo->setAccessibleName("Chord stamp shape");
    m_stampCombo->setStyleSheet(comboStyle + "QComboBox { min-width: 104px; }");
    const char* stampNames[] = {"Automatic", "Major", "Minor", "Diminished", "Augmented",
                                "Sus 2", "Sus 4", "Major 7", "Minor 7", "Dominant 7", "Octave"};
    for (const char* name : stampNames) m_stampCombo->addItem(QString::fromLatin1(name));

    m_channelCombo = new QComboBox(topBar);
    m_channelCombo->setToolTip("Target channel / color group");
    m_channelCombo->setAccessibleName("Target MIDI channel and note color group");
    m_channelCombo->setStyleSheet(comboStyle + "QComboBox { min-width: 82px; }");
    for (int i = 0; i < 16; ++i) {
        m_channelCombo->addItem(QString("Ch %1").arg(i + 1), i);
    }

    m_snapCombo = new QComboBox(topBar);
    m_snapCombo->setToolTip("Piano roll snap resolution");
    m_snapCombo->setAccessibleName("Piano roll snap resolution");
    m_snapCombo->setStyleSheet(comboStyle + "QComboBox { min-width: 86px; }");
    m_snapCombo->addItem("Line", 0.0);
    m_snapCombo->addItem("1/2 beat", 0.5);
    m_snapCombo->addItem("1/4 beat", 0.25);
    m_snapCombo->addItem("1/8 beat", 0.125);
    m_snapCombo->addItem("1/16 beat", 0.0625);
    m_snapCombo->addItem("Bar", 4.0);
    m_snapCombo->setCurrentIndex(2);
    m_snapLabel->setText(QString("Snap %1").arg(m_snapCombo->currentText()));

    m_eventTargetCombo = new QComboBox(topBar);
    m_eventTargetCombo->setToolTip("Event editor target");
    m_eventTargetCombo->setAccessibleName("Event editor property target");
    m_eventTargetCombo->setStyleSheet(comboStyle + "QComboBox { min-width: 96px; }");
    const char* eventTargets[] = {"Velocity", "Pan", "Release", "Mod X", "Mod Y"};
    for (const char* name : eventTargets) {
        m_eventTargetCombo->addItem(QString::fromLatin1(name));
    }

    m_colorButton = makeTool("C", "Note color group");
    auto updateColorButton = [this]() {
        const QColor color = noteColor(m_channelCombo ? m_channelCombo->currentIndex() : 0);
        m_colorButton->setStyleSheet(QString(
            "QToolButton { color: #101410; background: %1; border: 1px solid #101410; "
            "border-radius: 3px; min-width: 26px; min-height: 24px; font-weight: bold; } "
            "QToolButton:hover { border-color: #ffffff; }").arg(color.name()));
    };
    auto* colorMenu = new QMenu(m_colorButton);
    for (int i = 0; i < 16; ++i) {
        QAction* action = colorMenu->addAction(QString("Color %1 / MIDI Ch %1").arg(i + 1));
        action->setData(i);
    }
    m_colorButton->setMenu(colorMenu);
    m_colorButton->setPopupMode(QToolButton::InstantPopup);
    updateColorButton();

    m_slideButton = makeTool("S", "Slide note mode: glides overlapping native synth notes; skipped from MIDI/plugin output", true);
    m_portaButton = makeTool("O", "Portamento flag: glides native synth pitch into this note; plugins receive normal MIDI", true);
    m_ghostButton = makeTool("G", "Show ghost notes from overlapping MIDI clips and other color groups", true);
    m_ghostButton->setChecked(true);
    m_scaleButton = makeTool("C Maj", "Snap new and moved notes to project key/scale", true);
    m_scaleButton->setChecked(true);

    auto* zoomOut = new QToolButton(topBar);
    zoomOut->setText("-");
    zoomOut->setToolTip("Zoom out");
    zoomOut->setAccessibleName("Zoom out");
    auto* zoomIn = new QToolButton(topBar);
    zoomIn->setText("+");
    zoomIn->setToolTip("Zoom in");
    zoomIn->setAccessibleName("Zoom in");
    auto* closeButton = new QToolButton(topBar);
    closeButton->setText("X");
    closeButton->setToolTip("Close editor");
    closeButton->setAccessibleName("Close MIDI editor");
    zoomOut->setStyleSheet(toolButtonStyle);
    zoomIn->setStyleSheet(toolButtonStyle);
    closeButton->setStyleSheet(toolButtonStyle + "QToolButton:hover { background: #5a2620; }");
    connect(closeButton, &QToolButton::clicked, this, &QWidget::close);
    connect(zoomOut, &QToolButton::clicked, this, [this]() {
        if (m_canvas) {
            m_canvas->zoomBy(0.85);
        }
    });
    connect(zoomIn, &QToolButton::clicked, this, [this]() {
        if (m_canvas) {
            m_canvas->zoomBy(1.15);
        }
    });

    connect(m_channelCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, updateColorButton](int index) {
        updateColorButton();
        if (m_canvas) {
            m_canvas->setCurrentColorGroup(index);
        }
        if (m_midiChannelChangedCallback) m_midiChannelChangedCallback(qBound(0, index, 15));
    });
    connect(colorMenu, &QMenu::triggered, this, [this, updateColorButton](QAction* action) {
        const int group = qBound(0, action->data().toInt(), 15);
        m_channelCombo->setCurrentIndex(group);
        updateColorButton();
        if (m_canvas) {
            m_canvas->setCurrentColorGroup(group);
        }
    });
    connect(m_snapCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_canvas) {
            m_canvas->setSnapBeats(m_snapCombo->itemData(index).toDouble());
        }
        m_snapLabel->setText(QString("Snap %1").arg(m_snapCombo->currentText()));
    });
    connect(m_stampCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (!m_canvas) return;
        const std::vector<std::vector<int>> chords = {
            {0, 4, 7}, {0, 4, 7}, {0, 3, 7}, {0, 3, 6}, {0, 4, 8},
            {0, 2, 7}, {0, 5, 7}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 4, 7, 10}, {0, 12}
        };
        const int safeIndex = qBound(0, index, static_cast<int>(chords.size()) - 1);
        m_canvas->setStampChord(chords[static_cast<size_t>(safeIndex)], safeIndex == 0);
    });
    connect(m_eventTargetCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_canvas) {
            m_canvas->setPropertyTarget(static_cast<NotePropertyTarget>(qBound(0, index, 4)));
        }
    });
    connect(m_slideButton, &QToolButton::toggled, this, [this](bool checked) {
        if (m_canvas) {
            m_canvas->setSlideMode(checked);
        }
    });
    connect(m_portaButton, &QToolButton::toggled, this, [this](bool checked) {
        if (m_canvas) {
            m_canvas->setPortamentoMode(checked);
        }
    });
    connect(m_ghostButton, &QToolButton::toggled, this, [this](bool checked) {
        if (m_canvas) {
            m_canvas->setGhostNotesVisible(checked);
        }
    });
    connect(m_scaleButton, &QToolButton::toggled, this, [this](bool checked) {
        if (m_canvas) {
            m_canvas->setScaleSnap(checked, m_scaleRoot, m_scaleMinor);
        }
    });
    auto syncToolButtons = [drawTool, paintTool, stampTool, selectTool, deleteTool,
                            muteTool, sliceTool](PianoRollTool mode) {
        drawTool->setChecked(mode == PianoRollTool::Draw);
        paintTool->setChecked(mode == PianoRollTool::Paint);
        stampTool->setChecked(mode == PianoRollTool::Stamp);
        selectTool->setChecked(mode == PianoRollTool::Select);
        deleteTool->setChecked(mode == PianoRollTool::Delete);
        muteTool->setChecked(mode == PianoRollTool::Mute);
        sliceTool->setChecked(mode == PianoRollTool::Slice);
    };
    auto setToolMode = [this, syncToolButtons](PianoRollTool mode) {
        syncToolButtons(mode);
        if (m_canvas) {
            m_canvas->setToolMode(mode);
        }
    };
    connect(drawTool, &QToolButton::clicked, this, [setToolMode]() { setToolMode(PianoRollTool::Draw); });
    connect(paintTool, &QToolButton::clicked, this, [setToolMode]() { setToolMode(PianoRollTool::Paint); });
    connect(stampTool, &QToolButton::clicked, this, [setToolMode]() { setToolMode(PianoRollTool::Stamp); });
    connect(selectTool, &QToolButton::clicked, this, [setToolMode]() { setToolMode(PianoRollTool::Select); });
    connect(deleteTool, &QToolButton::clicked, this, [setToolMode]() { setToolMode(PianoRollTool::Delete); });
    connect(muteTool, &QToolButton::clicked, this, [setToolMode]() { setToolMode(PianoRollTool::Mute); });
    connect(sliceTool, &QToolButton::clicked, this, [setToolMode]() { setToolMode(PianoRollTool::Slice); });
    connect(quickQuantizeAction, &QAction::triggered, this, [this]() {
        if (m_canvas) {
            m_canvas->commandQuantizeSelection();
        }
    });
    connect(quickLegatoAction, &QAction::triggered, this, [this]() {
        if (m_canvas) {
            m_canvas->commandQuickLegato();
        }
    });
    connect(duplicateAction, &QAction::triggered, this, [this]() {
        if (m_canvas) {
            m_canvas->commandDuplicateSelection();
        }
    });
    connect(copyAction, &QAction::triggered, this, [this]() {
        if (m_canvas) {
            m_canvas->commandCopySelection();
        }
    });
    connect(cutAction, &QAction::triggered, this, [this]() {
        if (m_canvas) {
            m_canvas->commandCutSelection();
        }
    });
    connect(pasteAction, &QAction::triggered, this, [this]() {
        if (m_canvas) {
            m_canvas->commandPasteSelection();
        }
    });
    connect(selectAllAction, &QAction::triggered, this, [this]() {
        if (m_canvas) {
            m_canvas->commandSelectAll();
        }
    });

    m_snapLabel->setStyleSheet("color: #9ca89c; background: transparent; border: none; font-weight: bold;");
    auto addToolbarDivider = [topLayout, topBar]() {
        auto* divider = new QFrame(topBar);
        divider->setObjectName("MidiToolbarDivider");
        divider->setFixedSize(1, 22);
        topLayout->addWidget(divider);
    };
    topLayout->addWidget(m_clipLabel);
    topLayout->addWidget(m_trackLabel);
    addToolbarDivider();
    topLayout->addWidget(mainMenu);
    topLayout->addWidget(drawTool);
    topLayout->addWidget(paintTool);
    topLayout->addWidget(stampTool);
    topLayout->addWidget(m_stampCombo);
    topLayout->addWidget(selectTool);
    topLayout->addWidget(deleteTool);
    topLayout->addWidget(muteTool);
    topLayout->addWidget(sliceTool);
    addToolbarDivider();
    topLayout->addWidget(m_channelCombo);
    topLayout->addWidget(m_colorButton);
    topLayout->addWidget(m_slideButton);
    topLayout->addWidget(m_portaButton);
    topLayout->addWidget(m_ghostButton);
    topLayout->addWidget(m_scaleButton);
    addToolbarDivider();
    topLayout->addWidget(m_snapLabel);
    topLayout->addWidget(m_snapCombo);
    topLayout->addWidget(m_eventTargetCombo);
    topLayout->addStretch();
    topLayout->addWidget(zoomOut);
    topLayout->addWidget(zoomIn);
    topLayout->addWidget(closeButton);
    root->addWidget(topBar);

    PianoRollTool restoredTool = PianoRollTool::Draw;
    double restoredPixelsPerBeat = kDefaultPixelsPerBeat;
    QByteArray savedGeometry;
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("PianoRoll"));
        savedGeometry = settings.value(QStringLiteral("Geometry")).toByteArray();
        m_snapCombo->setCurrentIndex(qBound(0, settings.value(QStringLiteral("SnapIndex"), 2).toInt(),
                                             m_snapCombo->count() - 1));
        m_channelCombo->setCurrentIndex(qBound(0, settings.value(QStringLiteral("ColorGroup"), 0).toInt(),
                                                m_channelCombo->count() - 1));
        m_eventTargetCombo->setCurrentIndex(qBound(0, settings.value(QStringLiteral("EventTarget"), 0).toInt(),
                                                    m_eventTargetCombo->count() - 1));
        m_stampCombo->setCurrentIndex(qBound(0, settings.value(QStringLiteral("StampIndex"), 0).toInt(),
                                              m_stampCombo->count() - 1));
        m_ghostButton->setChecked(settings.value(QStringLiteral("GhostNotesVisible"), true).toBool());
        m_scaleButton->setChecked(settings.value(QStringLiteral("ScaleSnapEnabled"), true).toBool());
        restoredTool = static_cast<PianoRollTool>(qBound(
            static_cast<int>(PianoRollTool::Draw), settings.value(QStringLiteral("ToolMode"), 0).toInt(),
            static_cast<int>(PianoRollTool::Slice)));
        restoredPixelsPerBeat = settings.value(
            QStringLiteral("PixelsPerBeat"), kDefaultPixelsPerBeat).toDouble();
        settings.endGroup();
    }

    m_canvas = new PianoRollCanvas(this);
    m_canvas->setChangeCallback([this](const MidiClipDocument& beforeDocument,
                                       const MidiClipDocument& afterDocument,
                                       quint64 editSerial) {
        notifyDocumentChanged(beforeDocument, afterDocument, editSerial);
    });
    m_canvas->setToolModeChangedCallback(syncToolButtons);
    m_canvas->setSlideModeChangedCallback([this](bool enabled) {
        if (m_slideButton) {
            m_slideButton->setChecked(enabled);
        }
    });
    m_canvas->setPortamentoModeChangedCallback([this](bool enabled) {
        if (m_portaButton) {
            m_portaButton->setChecked(enabled);
        }
    });
    m_canvas->setGhostNotesVisibleChangedCallback([this](bool visible) {
        if (m_ghostButton) {
            m_ghostButton->setChecked(visible);
        }
    });
    m_canvas->setPropertyTargetChangedCallback([this](NotePropertyTarget target) {
        if (m_eventTargetCombo) {
            m_eventTargetCombo->setCurrentIndex(static_cast<int>(target));
        }
    });
    m_canvas->setColorGroupChangedCallback([this, updateColorButton](int group) {
        if (m_channelCombo && m_channelCombo->currentIndex() != group) {
            m_channelCombo->setCurrentIndex(group);
        }
        updateColorButton();
    });
    const std::vector<std::vector<int>> initialChords = {
        {0, 4, 7}, {0, 4, 7}, {0, 3, 7}, {0, 3, 6}, {0, 4, 8},
        {0, 2, 7}, {0, 5, 7}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 4, 7, 10}, {0, 12}
    };
    const int stampIndex = m_stampCombo ? qBound(0, m_stampCombo->currentIndex(),
        static_cast<int>(initialChords.size()) - 1) : 0;
    m_canvas->setStampChord(initialChords[static_cast<size_t>(stampIndex)], stampIndex == 0);
    m_canvas->setCurrentColorGroup(m_channelCombo ? m_channelCombo->currentIndex() : 0);
    m_canvas->setSnapBeats(m_snapCombo ? m_snapCombo->currentData().toDouble() : kDefaultSnap);
    m_canvas->setPropertyTarget(m_eventTargetCombo ? static_cast<NotePropertyTarget>(m_eventTargetCombo->currentIndex())
                                                   : NotePropertyTarget::Velocity);
    m_canvas->setSlideMode(m_slideButton && m_slideButton->isChecked());
    m_canvas->setPortamentoMode(m_portaButton && m_portaButton->isChecked());
    m_canvas->setGhostNotesVisible(!m_ghostButton || m_ghostButton->isChecked());
    m_canvas->setScaleSnap(!m_scaleButton || m_scaleButton->isChecked(), m_scaleRoot, m_scaleMinor);
    m_canvas->setPixelsPerBeat(restoredPixelsPerBeat);
    m_canvas->setToolMode(restoredTool);
    root->addWidget(m_canvas, 1);
    if (!savedGeometry.isEmpty()) restoreGeometry(savedGeometry);
}

void MidiEditorWindow::setClip(MidiClipDocument* clip)
{
    m_clip = clip;
    if (m_canvas) {
        m_canvas->setClip(clip);
    }
    refreshHeader();
}

void MidiEditorWindow::setGhostClips(std::vector<MidiGhostClip> clips)
{
    if (m_canvas) m_canvas->setGhostClips(std::move(clips));
}

void MidiEditorWindow::setExpressionMap(std::vector<MidiExpressionMapEntry> entries)
{
    if (m_canvas) m_canvas->setExpressionMap(std::move(entries));
}

int MidiEditorWindow::currentMidiChannel() const
{
    return m_channelCombo ? qBound(0, m_channelCombo->currentIndex(), 15) : 0;
}

void MidiEditorWindow::setMidiChannelChangedCallback(std::function<void(int)> callback)
{
    m_midiChannelChangedCallback = std::move(callback);
}

void MidiEditorWindow::setCloseCallback(std::function<void()> callback)
{
    m_closeCallback = std::move(callback);
}

void MidiEditorWindow::setDocumentChangedCallback(
    std::function<void(const MidiClipDocument&, const MidiClipDocument&, quint64)> callback)
{
    m_documentChangedCallback = std::move(callback);
}

void MidiEditorWindow::setUndoRequestCallback(std::function<void()> callback)
{
    m_undoRequestCallback = std::move(callback);
    if (m_canvas) m_canvas->setUndoRequestCallback(m_undoRequestCallback);
}

void MidiEditorWindow::setRedoRequestCallback(std::function<void()> callback)
{
    m_redoRequestCallback = std::move(callback);
    if (m_canvas) m_canvas->setRedoRequestCallback(m_redoRequestCallback);
}

void MidiEditorWindow::setPlayheadSeekCallback(std::function<void(double)> callback)
{
    m_playheadSeekCallback = std::move(callback);
    m_canvas->setPlayheadSeekCallback(m_playheadSeekCallback);
}

void MidiEditorWindow::setNotePreviewCallback(std::function<void(int, int, bool, bool)> callback)
{
    m_notePreviewCallback = std::move(callback);
    m_canvas->setNotePreviewCallback(m_notePreviewCallback);
}

void MidiEditorWindow::setPlayheadBeat(double relativeBeat)
{
    m_canvas->setPlayheadBeat(relativeBeat);
}

void MidiEditorWindow::setProjectScale(int rootNote, bool minor)
{
    static const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    m_scaleRoot = ((rootNote % 12) + 12) % 12;
    m_scaleMinor = minor;
    if (m_scaleButton) {
        m_scaleButton->setText(QString("%1 %2").arg(QString::fromLatin1(noteNames[m_scaleRoot]),
                                                     minor ? QString("Min") : QString("Maj")));
        m_scaleButton->setToolTip(QString("Snap new and moved notes to %1 %2")
            .arg(QString::fromLatin1(noteNames[m_scaleRoot]), minor ? QString("Minor") : QString("Major")));
    }
    if (m_canvas) {
        m_canvas->setScaleSnap(!m_scaleButton || m_scaleButton->isChecked(), m_scaleRoot, m_scaleMinor);
    }
}

void MidiEditorWindow::closeEvent(QCloseEvent* event)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("PianoRoll"));
    settings.setValue(QStringLiteral("Geometry"), saveGeometry());
    settings.setValue(QStringLiteral("SnapIndex"), m_snapCombo ? m_snapCombo->currentIndex() : 2);
    settings.setValue(QStringLiteral("ColorGroup"), m_channelCombo ? m_channelCombo->currentIndex() : 0);
    settings.setValue(QStringLiteral("EventTarget"), m_eventTargetCombo ? m_eventTargetCombo->currentIndex() : 0);
    settings.setValue(QStringLiteral("StampIndex"), m_stampCombo ? m_stampCombo->currentIndex() : 0);
    settings.setValue(QStringLiteral("GhostNotesVisible"), !m_ghostButton || m_ghostButton->isChecked());
    settings.setValue(QStringLiteral("ScaleSnapEnabled"), !m_scaleButton || m_scaleButton->isChecked());
    settings.setValue(QStringLiteral("ToolMode"),
        m_canvas ? static_cast<int>(m_canvas->toolMode()) : static_cast<int>(PianoRollTool::Draw));
    settings.setValue(QStringLiteral("PixelsPerBeat"),
        m_canvas ? m_canvas->pixelsPerBeat() : kDefaultPixelsPerBeat);
    settings.endGroup();
    settings.sync();
    if (m_closeCallback) {
        m_closeCallback();
    }
    QWidget::closeEvent(event);
}

void MidiEditorWindow::refreshHeader()
{
    if (!m_clip) {
        setWindowTitle("MIDI Editor");
        m_clipLabel->setText("No clip");
        m_trackLabel->setText("Track");
        return;
    }

    setWindowTitle(QString("MIDI Editor - %1").arg(m_clip->name));
    m_clipLabel->setText(m_clip->name);
    m_trackLabel->setText(m_clip->trackName);
    if (m_snapCombo) {
        const double beatsPerBar = static_cast<double>(qBound(1, m_clip->timeSignatureNumerator, 32)) *
            (4.0 / static_cast<double>(qBound(1, m_clip->timeSignatureDenominator, 64)));
        const int barIndex = m_snapCombo->findText("Bar");
        if (barIndex >= 0) {
            m_snapCombo->setItemData(barIndex, qBound(0.25, beatsPerBar, 64.0));
            if (m_snapCombo->currentIndex() == barIndex && m_canvas) {
                m_canvas->setSnapBeats(m_snapCombo->itemData(barIndex).toDouble());
            }
        }
        m_snapLabel->setText(QString("Snap %1").arg(m_snapCombo->currentText()));
    }
}

void MidiEditorWindow::notifyDocumentChanged(const MidiClipDocument& beforeDocument,
                                             const MidiClipDocument& afterDocument,
                                             quint64 editSerial)
{
    if (m_clip && m_documentChangedCallback && m_clip == &afterDocument) {
        m_documentChangedCallback(beforeDocument, afterDocument, editSerial);
    }
}
