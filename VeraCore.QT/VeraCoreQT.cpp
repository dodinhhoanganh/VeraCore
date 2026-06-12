#include "VeraCoreQT.h"
#include "Preferences.h"
#include "TrackWidget.h"
#include "PluginManager.h"
#include <QCloseEvent>
#include <QScrollBar>
#include <functional>

VeraCoreQT::VeraCoreQT(QWidget *parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);

    // Register PluginInfo meta types for signals/slots across threads
    qRegisterMetaType<PluginInfo>("PluginInfo");
    qRegisterMetaType<std::vector<PluginInfo>>("std::vector<PluginInfo>");
    
    // Inspector docks at the bottom of the center area (Ableton-style device
    // strip under the arrangement), not as a third column.
    m_inspectorWidget = new InspectorWidget(this);
    ui.centerSplitter->addWidget(m_inspectorWidget);

    // Browser column left, arrangement gets the remaining space.
    ui.mainSplitter->setStretchFactor(0, 0);
    ui.mainSplitter->setStretchFactor(1, 1);
    QList<int> sizes;
    sizes << 240 << 1040;
    ui.mainSplitter->setSizes(sizes);

    // Arrangement above, inspector strip below.
    ui.centerSplitter->setStretchFactor(0, 1);
    ui.centerSplitter->setStretchFactor(1, 0);
    QList<int> centerSizes;
    centerSizes << 540 << 170;
    ui.centerSplitter->setSizes(centerSizes);

    QList<int> playlistSizes;
    playlistSizes << 200 << 800;
    ui.playlistSplitter->setSizes(playlistSizes);

    // FL-style instant browser filtering.
    connect(ui.browserSearch, &QLineEdit::textChanged,
            this, &VeraCoreQT::onBrowserSearchChanged);

    m_transportWidget = new TransportWidget(this);
    ui.verticalLayout->insertWidget(0, m_transportWidget);

    // Replace the default QGraphicsView with our custom PlaylistView
    m_playlistView = new PlaylistView(this);
    int playlistIndex = ui.playlistSplitter->indexOf(ui.playlistView);
    ui.playlistView->deleteLater();
    ui.playlistSplitter->insertWidget(playlistIndex, m_playlistView);
    connect(m_playlistView, &PlaylistView::trackLaneSelected,
            this, &VeraCoreQT::onPlaylistTrackSelected);
    connect(m_playlistView, &PlaylistView::midiClipSelected,
            this, &VeraCoreQT::onMidiClipSelected);
    connect(m_playlistView, &PlaylistView::midiClipOpened,
            this, &VeraCoreQT::onMidiClipSelected);

    // Sync vertical scrollbars between track headers and playlist
    connect(ui.trackHeadersScroll->verticalScrollBar(), &QScrollBar::valueChanged,
            m_playlistView->verticalScrollBar(), &QScrollBar::setValue);
    connect(m_playlistView->verticalScrollBar(), &QScrollBar::valueChanged,
            ui.trackHeadersScroll->verticalScrollBar(), &QScrollBar::setValue);

    // Set top margin to 30 to align with the Timeline Ruler (which is 30px high)
    QMargins margins = ui.trackHeadersLayout->contentsMargins();
    margins.setTop(30);
    ui.trackHeadersLayout->setContentsMargins(margins);
    
    // Set spacing to 0 so 60px track widgets stack exactly to 60px lanes
    ui.trackHeadersLayout->setSpacing(0);
    
    // Remove frame to align perfectly with the borderless PlaylistView
    ui.trackHeadersScroll->setFrameShape(QFrame::NoFrame);

    initDefaultTracks();

    // Populate browser tree with previously saved plugins
    updateBrowserTree();

    connect(ui.actionPreferences, &QAction::triggered, this, &VeraCoreQT::OnPreferences);
}

void VeraCoreQT::initDefaultTracks() {
    auto midiTrack = std::make_shared<MidiTrack>("Inst 1");
    auto audioTrack = std::make_shared<AudioTrack>("Audio 1");

    addTrack(midiTrack);
    addTrack(audioTrack);
}

void VeraCoreQT::addTrack(std::shared_ptr<Track> track) {
    m_tracks.push_back(track);
    
    TrackWidget* trackWidget = new TrackWidget(track.get(), this);
    m_trackWidgets.push_back(trackWidget);
    
    connect(trackWidget, &TrackWidget::trackSelected, this, &VeraCoreQT::onTrackSelected);

    ui.trackHeadersLayout->insertWidget(ui.trackHeadersLayout->count() - 1, trackWidget);
    
    // Update playlist view grid lanes
    m_playlistView->setTracks(m_tracks);
}

void VeraCoreQT::onTrackSelected(TrackWidget* selectedWidget, Track* track) {
    // Deselect all others
    for (TrackWidget* tw : m_trackWidgets) {
        if (tw != selectedWidget) {
            tw->setSelected(false);
        }
    }
    
    selectedWidget->setSelected(true);
    m_inspectorWidget->setTrack(track);
}

void VeraCoreQT::onPlaylistTrackSelected(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= (int)m_trackWidgets.size()) {
        return;
    }

    onTrackSelected(m_trackWidgets[trackIndex], m_tracks[trackIndex].get());
}

void VeraCoreQT::onMidiClipSelected(int trackIndex, const QString& name, double startBeat, double lengthBeats) {
    if (trackIndex < 0 || trackIndex >= (int)m_trackWidgets.size()) {
        return;
    }

    onTrackSelected(m_trackWidgets[trackIndex], m_tracks[trackIndex].get());
    m_inspectorWidget->setMidiClip(name, startBeat, lengthBeats);
}

void VeraCoreQT::onBrowserSearchChanged(const QString& text)
{
    const QString needle = text.trimmed();

    // An item stays visible when it matches or any descendant matches.
    std::function<bool(QTreeWidgetItem*)> filterItem = [&](QTreeWidgetItem* item) -> bool {
        bool anyChildVisible = false;
        for (int i = 0; i < item->childCount(); ++i)
            anyChildVisible = filterItem(item->child(i)) || anyChildVisible;

        const bool selfMatches = needle.isEmpty() ||
                                 item->text(0).contains(needle, Qt::CaseInsensitive);
        const bool visible = selfMatches || anyChildVisible;
        item->setHidden(!visible);
        if (!needle.isEmpty() && anyChildVisible)
            item->setExpanded(true);
        return visible;
    };

    for (int i = 0; i < ui.browserTree->topLevelItemCount(); ++i)
        filterItem(ui.browserTree->topLevelItem(i));
}

void VeraCoreQT::OnPreferences()
{
    Preferences* preferences = new Preferences(this);
    preferences->ConnectAudio(audioGeneral);
    preferences->InitPreferences();
}

void VeraCoreQT::updateBrowserTree()
{
    QList<QTreeWidgetItem*> items = ui.browserTree->findItems("Plugins", Qt::MatchExactly | Qt::MatchRecursive, 0);
    if (items.isEmpty()) return;
    QTreeWidgetItem* pluginsNode = items.first();
    
    QTreeWidgetItem* audioNode = nullptr;
    QTreeWidgetItem* genNode = nullptr;
    for (int i = 0; i < pluginsNode->childCount(); ++i) {
        if (pluginsNode->child(i)->text(0) == "Audio Plugins") audioNode = pluginsNode->child(i);
        if (pluginsNode->child(i)->text(0) == "Generator Plugins") genNode = pluginsNode->child(i);
    }
    
    auto addPluginToNode = [](QTreeWidgetItem* categoryNode, const PluginInfo& info) {
        if (!categoryNode) return;
        QTreeWidgetItem* typeNode = nullptr;
        for (int i = 0; i < categoryNode->childCount(); ++i) {
            if (categoryNode->child(i)->text(0).contains(info.type, Qt::CaseInsensitive)) {
                typeNode = categoryNode->child(i);
                break;
            }
        }
        if (!typeNode) {
            typeNode = new QTreeWidgetItem(categoryNode, QStringList() << info.type);
        }
        QTreeWidgetItem* pluginLeaf = new QTreeWidgetItem(typeNode, QStringList() << info.name);
        pluginLeaf->setData(0, Qt::UserRole, info.path);
        if (!info.vendor.isEmpty()) {
            pluginLeaf->setToolTip(0, QString("%1\n%2 %3").arg(info.vendor, info.type, info.version));
        }
    };

    auto clearLeaves = [](QTreeWidgetItem* categoryNode) {
        if (!categoryNode) return;
        for (int i = 0; i < categoryNode->childCount(); ++i) {
            QTreeWidgetItem* typeNode = categoryNode->child(i);
            qDeleteAll(typeNode->takeChildren());
        }
    };

    clearLeaves(audioNode);
    clearLeaves(genNode);

    const auto& discovered = PluginManager::GetInstance().GetDiscoveredPlugins();
    for (const auto& info : discovered) {
        // Instruments go to Generator Plugins, everything else to Audio Plugins.
        QTreeWidgetItem* target = (info.category == "Instrument" && genNode) ? genNode : audioNode;
        addPluginToNode(target, info);
    }
}

void VeraCoreQT::addSinglePluginToBrowser(const PluginInfo& info)
{
    QList<QTreeWidgetItem*> items = ui.browserTree->findItems("Plugins", Qt::MatchExactly | Qt::MatchRecursive, 0);
    if (items.isEmpty()) return;
    QTreeWidgetItem* pluginsNode = items.first();
    
    QTreeWidgetItem* audioNode = nullptr;
    QTreeWidgetItem* genNode = nullptr;
    for (int i = 0; i < pluginsNode->childCount(); ++i) {
        if (pluginsNode->child(i)->text(0) == "Audio Plugins") audioNode = pluginsNode->child(i);
        if (pluginsNode->child(i)->text(0) == "Generator Plugins") genNode = pluginsNode->child(i);
    }

    QTreeWidgetItem* categoryNode = (info.category == "Instrument" && genNode) ? genNode : audioNode;
    if (!categoryNode) return;

    QTreeWidgetItem* typeNode = nullptr;
    for (int i = 0; i < categoryNode->childCount(); ++i) {
        if (categoryNode->child(i)->text(0).contains(info.type, Qt::CaseInsensitive)) {
            typeNode = categoryNode->child(i);
            break;
        }
    }
    if (!typeNode) {
        typeNode = new QTreeWidgetItem(categoryNode, QStringList() << info.type);
    }
    QTreeWidgetItem* pluginLeaf = new QTreeWidgetItem(typeNode, QStringList() << info.name);
    pluginLeaf->setData(0, Qt::UserRole, info.path);
    if (!info.vendor.isEmpty()) {
        pluginLeaf->setToolTip(0, QString("%1\n%2 %3").arg(info.vendor, info.type, info.version));
    }
}

void VeraCoreQT::ConnectAudio(const shared_ptr<AudioGeneral>& audio) {
    this->audioGeneral = audio;
}

VeraCoreQT::~VeraCoreQT()
{
}
