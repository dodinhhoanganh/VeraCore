#pragma once

#include <QtWidgets/QMainWindow>
#include <vector>
#include <memory>
#include "ui_VeraCoreQT.h"
#include "PluginManager.h"
#include "AudioGeneral.h"
#include "Track.h"
#include "TransportWidget.h"
#include "InspectorWidget.h"
#include "TrackWidget.h"
#include "PlaylistView.h"

class VeraCoreQT : public QMainWindow
{
    Q_OBJECT

public:
    void ConnectAudio(const shared_ptr<AudioGeneral>& audio);
    VeraCoreQT(QWidget *parent = nullptr);
    ~VeraCoreQT();
    shared_ptr<AudioGeneral> audioGeneral;
    
    void updateBrowserTree();
    void addSinglePluginToBrowser(const PluginInfo& info);

private:
    Ui::VeraCoreQTClass ui;
    void OnPreferences();

    std::vector<std::shared_ptr<Track>> m_tracks;
    std::vector<TrackWidget*> m_trackWidgets;

    PlaylistView* m_playlistView;

    void addTrack(std::shared_ptr<Track> track);
    void initDefaultTracks();

    TransportWidget* m_transportWidget;
    InspectorWidget* m_inspectorWidget;

private slots:
    void onTrackSelected(TrackWidget* widget, Track* track);
    void onBrowserSearchChanged(const QString& text);
    void onPlaylistTrackSelected(int trackIndex);
    void onMidiClipSelected(int trackIndex, const QString& name, double startBeat, double lengthBeats);
};
