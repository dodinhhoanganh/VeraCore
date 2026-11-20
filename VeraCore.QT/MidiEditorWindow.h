#pragma once

#include <QWidget>
#include <functional>
#include <memory>
#include "MidiClipModel.h"

class QAbstractScrollArea;
class QCloseEvent;
class QLabel;
class QComboBox;
class QToolButton;

class MidiEditorWindow : public QWidget
{
public:
    explicit MidiEditorWindow(QWidget* parent = nullptr);

    void setClip(MidiClipDocument* clip);
    void setGhostClips(std::vector<MidiGhostClip> clips);
    void setExpressionMap(std::vector<MidiExpressionMapEntry> entries);
    int currentMidiChannel() const;
    void setMidiChannelChangedCallback(std::function<void(int)> callback);
    void setCloseCallback(std::function<void()> callback);
    void setDocumentChangedCallback(std::function<void(const MidiClipDocument&,
                                                         const MidiClipDocument&,
                                                         quint64)> callback);
    void setUndoRequestCallback(std::function<void()> callback);
    void setRedoRequestCallback(std::function<void()> callback);
    void setPlayheadSeekCallback(std::function<void(double)> callback);
    void setNotePreviewCallback(std::function<void(int, int, bool, bool)> callback);
    void setPlayheadBeat(double relativeBeat);
    void setProjectScale(int rootNote, bool minor);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    class PianoRollCanvas;

    MidiClipDocument* m_clip;
    QLabel* m_clipLabel;
    QLabel* m_trackLabel;
    QLabel* m_snapLabel;
    QComboBox* m_snapCombo;
    QComboBox* m_channelCombo;
    QComboBox* m_eventTargetCombo;
    QComboBox* m_stampCombo;
    QToolButton* m_colorButton;
    QToolButton* m_slideButton;
    QToolButton* m_portaButton;
    QToolButton* m_ghostButton;
    QToolButton* m_scaleButton;
    PianoRollCanvas* m_canvas;
    int m_scaleRoot = 0;
    bool m_scaleMinor = false;
    std::function<void()> m_closeCallback;
    std::function<void(const MidiClipDocument&, const MidiClipDocument&, quint64)>
        m_documentChangedCallback;
    std::function<void()> m_undoRequestCallback;
    std::function<void()> m_redoRequestCallback;
    std::function<void(double)> m_playheadSeekCallback;
    std::function<void(int, int, bool, bool)> m_notePreviewCallback;
    std::function<void(int)> m_midiChannelChangedCallback;

    void refreshHeader();
    void notifyDocumentChanged(const MidiClipDocument& beforeDocument,
                               const MidiClipDocument& afterDocument,
                               quint64 editSerial);
};
