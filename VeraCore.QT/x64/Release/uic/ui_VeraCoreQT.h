/********************************************************************************
** Form generated from reading UI file 'VeraCoreQT.ui'
**
** Created by: Qt User Interface Compiler version 6.3.1
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_VERACOREQT_H
#define UI_VERACOREQT_H

#include <QtCore/QVariant>
#include <QtGui/QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_VeraCoreQTClass
{
public:
    QAction *actionOpen;
    QAction *actionNew;
    QAction *actionSave;
    QAction *actionSave_As;
    QAction *actionExit;
    QAction *actionMIDI;
    QAction *actionWAV;
    QAction *actionFLAC;
    QAction *actionMP3;
    QAction *actionOGG;
    QAction *actionMIDI_2;
    QAction *actionZipped_Project;
    QAction *actionUndo;
    QAction *actionRedo;
    QAction *actionCopy;
    QAction *actionPaste;
    QAction *actionCut;
    QAction *actionGeneral;
    QAction *actionAudio;
    QAction *actionMIDI_3;
    QAction *actionPlugins;
    QAction *actionFiles;
    QAction *actionPlaylist;
    QAction *actionMixer;
    QAction *actionMIDI_Editor;
    QAction *actionBrowser;
    QAction *actionTempo_Tapper;
    QAction *actionAbout;
    QAction *actionHelp;
    QAction *actionCheck_For_Update;
    QAction *actionPreferences;
    QAction *actionProject_Settings;
    QWidget *centralWidget;
    QVBoxLayout *verticalLayout;
    QSplitter *mainSplitter;
    QTreeWidget *browserTree;
    QSplitter *playlistSplitter;
    QScrollArea *trackHeadersScroll;
    QWidget *trackHeadersContainer;
    QVBoxLayout *trackHeadersLayout;
    QSpacerItem *trackHeadersSpacer;
    QGraphicsView *playlistView;
    QMenuBar *menuBar;
    QMenu *menuFile;
    QMenu *menuImport;
    QMenu *menuExport;
    QMenu *menuEdit;
    QMenu *menuOptions;
    QMenu *menuView;
    QMenu *menuHelp;
    QStatusBar *statusBar;

    void setupUi(QMainWindow *VeraCoreQTClass)
    {
        if (VeraCoreQTClass->objectName().isEmpty())
            VeraCoreQTClass->setObjectName(QString::fromUtf8("VeraCoreQTClass"));
        VeraCoreQTClass->resize(853, 550);
        actionOpen = new QAction(VeraCoreQTClass);
        actionOpen->setObjectName(QString::fromUtf8("actionOpen"));
        actionNew = new QAction(VeraCoreQTClass);
        actionNew->setObjectName(QString::fromUtf8("actionNew"));
        actionSave = new QAction(VeraCoreQTClass);
        actionSave->setObjectName(QString::fromUtf8("actionSave"));
        actionSave_As = new QAction(VeraCoreQTClass);
        actionSave_As->setObjectName(QString::fromUtf8("actionSave_As"));
        actionExit = new QAction(VeraCoreQTClass);
        actionExit->setObjectName(QString::fromUtf8("actionExit"));
        QIcon icon;
        QString iconThemeName = QString::fromUtf8("application-exit");
        if (QIcon::hasThemeIcon(iconThemeName)) {
            icon = QIcon::fromTheme(iconThemeName);
        } else {
            icon.addFile(QString::fromUtf8("."), QSize(), QIcon::Normal, QIcon::Off);
        }
        actionExit->setIcon(icon);
        actionMIDI = new QAction(VeraCoreQTClass);
        actionMIDI->setObjectName(QString::fromUtf8("actionMIDI"));
        actionWAV = new QAction(VeraCoreQTClass);
        actionWAV->setObjectName(QString::fromUtf8("actionWAV"));
        actionFLAC = new QAction(VeraCoreQTClass);
        actionFLAC->setObjectName(QString::fromUtf8("actionFLAC"));
        actionMP3 = new QAction(VeraCoreQTClass);
        actionMP3->setObjectName(QString::fromUtf8("actionMP3"));
        actionOGG = new QAction(VeraCoreQTClass);
        actionOGG->setObjectName(QString::fromUtf8("actionOGG"));
        actionMIDI_2 = new QAction(VeraCoreQTClass);
        actionMIDI_2->setObjectName(QString::fromUtf8("actionMIDI_2"));
        actionZipped_Project = new QAction(VeraCoreQTClass);
        actionZipped_Project->setObjectName(QString::fromUtf8("actionZipped_Project"));
        actionUndo = new QAction(VeraCoreQTClass);
        actionUndo->setObjectName(QString::fromUtf8("actionUndo"));
        actionRedo = new QAction(VeraCoreQTClass);
        actionRedo->setObjectName(QString::fromUtf8("actionRedo"));
        actionCopy = new QAction(VeraCoreQTClass);
        actionCopy->setObjectName(QString::fromUtf8("actionCopy"));
        actionPaste = new QAction(VeraCoreQTClass);
        actionPaste->setObjectName(QString::fromUtf8("actionPaste"));
        actionCut = new QAction(VeraCoreQTClass);
        actionCut->setObjectName(QString::fromUtf8("actionCut"));
        actionGeneral = new QAction(VeraCoreQTClass);
        actionGeneral->setObjectName(QString::fromUtf8("actionGeneral"));
        actionAudio = new QAction(VeraCoreQTClass);
        actionAudio->setObjectName(QString::fromUtf8("actionAudio"));
        actionMIDI_3 = new QAction(VeraCoreQTClass);
        actionMIDI_3->setObjectName(QString::fromUtf8("actionMIDI_3"));
        actionPlugins = new QAction(VeraCoreQTClass);
        actionPlugins->setObjectName(QString::fromUtf8("actionPlugins"));
        actionFiles = new QAction(VeraCoreQTClass);
        actionFiles->setObjectName(QString::fromUtf8("actionFiles"));
        actionPlaylist = new QAction(VeraCoreQTClass);
        actionPlaylist->setObjectName(QString::fromUtf8("actionPlaylist"));
        actionPlaylist->setCheckable(true);
        actionPlaylist->setChecked(true);
        actionMixer = new QAction(VeraCoreQTClass);
        actionMixer->setObjectName(QString::fromUtf8("actionMixer"));
        actionMixer->setCheckable(true);
        actionMIDI_Editor = new QAction(VeraCoreQTClass);
        actionMIDI_Editor->setObjectName(QString::fromUtf8("actionMIDI_Editor"));
        actionMIDI_Editor->setCheckable(true);
        actionBrowser = new QAction(VeraCoreQTClass);
        actionBrowser->setObjectName(QString::fromUtf8("actionBrowser"));
        actionBrowser->setCheckable(true);
        actionBrowser->setChecked(true);
        actionTempo_Tapper = new QAction(VeraCoreQTClass);
        actionTempo_Tapper->setObjectName(QString::fromUtf8("actionTempo_Tapper"));
        actionTempo_Tapper->setCheckable(true);
        actionAbout = new QAction(VeraCoreQTClass);
        actionAbout->setObjectName(QString::fromUtf8("actionAbout"));
        actionHelp = new QAction(VeraCoreQTClass);
        actionHelp->setObjectName(QString::fromUtf8("actionHelp"));
        actionCheck_For_Update = new QAction(VeraCoreQTClass);
        actionCheck_For_Update->setObjectName(QString::fromUtf8("actionCheck_For_Update"));
        actionPreferences = new QAction(VeraCoreQTClass);
        actionPreferences->setObjectName(QString::fromUtf8("actionPreferences"));
        actionProject_Settings = new QAction(VeraCoreQTClass);
        actionProject_Settings->setObjectName(QString::fromUtf8("actionProject_Settings"));
        centralWidget = new QWidget(VeraCoreQTClass);
        centralWidget->setObjectName(QString::fromUtf8("centralWidget"));
        verticalLayout = new QVBoxLayout(centralWidget);
        verticalLayout->setSpacing(0);
        verticalLayout->setContentsMargins(11, 11, 11, 11);
        verticalLayout->setObjectName(QString::fromUtf8("verticalLayout"));
        verticalLayout->setContentsMargins(0, 0, 0, 0);
        mainSplitter = new QSplitter(centralWidget);
        mainSplitter->setObjectName(QString::fromUtf8("mainSplitter"));
        mainSplitter->setOrientation(Qt::Horizontal);
        browserTree = new QTreeWidget(mainSplitter);
        QTreeWidgetItem *__qtreewidgetitem = new QTreeWidgetItem(browserTree);
        QTreeWidgetItem *__qtreewidgetitem1 = new QTreeWidgetItem(__qtreewidgetitem);
        new QTreeWidgetItem(__qtreewidgetitem1);
        new QTreeWidgetItem(__qtreewidgetitem1);
        new QTreeWidgetItem(__qtreewidgetitem1);
        QTreeWidgetItem *__qtreewidgetitem2 = new QTreeWidgetItem(__qtreewidgetitem);
        new QTreeWidgetItem(__qtreewidgetitem2);
        new QTreeWidgetItem(__qtreewidgetitem2);
        new QTreeWidgetItem(__qtreewidgetitem2);
        new QTreeWidgetItem(browserTree);
        browserTree->setObjectName(QString::fromUtf8("browserTree"));
        browserTree->setHeaderHidden(true);
        mainSplitter->addWidget(browserTree);
        playlistSplitter = new QSplitter(mainSplitter);
        playlistSplitter->setObjectName(QString::fromUtf8("playlistSplitter"));
        playlistSplitter->setOrientation(Qt::Horizontal);
        trackHeadersScroll = new QScrollArea(playlistSplitter);
        trackHeadersScroll->setObjectName(QString::fromUtf8("trackHeadersScroll"));
        trackHeadersScroll->setWidgetResizable(true);
        trackHeadersScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        trackHeadersContainer = new QWidget();
        trackHeadersContainer->setObjectName(QString::fromUtf8("trackHeadersContainer"));
        trackHeadersLayout = new QVBoxLayout(trackHeadersContainer);
        trackHeadersLayout->setSpacing(2);
        trackHeadersLayout->setContentsMargins(11, 11, 11, 11);
        trackHeadersLayout->setObjectName(QString::fromUtf8("trackHeadersLayout"));
        trackHeadersLayout->setContentsMargins(0, 0, 0, 0);
        trackHeadersSpacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);

        trackHeadersLayout->addItem(trackHeadersSpacer);

        trackHeadersScroll->setWidget(trackHeadersContainer);
        playlistSplitter->addWidget(trackHeadersScroll);
        playlistView = new QGraphicsView(playlistSplitter);
        playlistView->setObjectName(QString::fromUtf8("playlistView"));
        playlistSplitter->addWidget(playlistView);
        mainSplitter->addWidget(playlistSplitter);

        verticalLayout->addWidget(mainSplitter);

        VeraCoreQTClass->setCentralWidget(centralWidget);
        menuBar = new QMenuBar(VeraCoreQTClass);
        menuBar->setObjectName(QString::fromUtf8("menuBar"));
        menuBar->setGeometry(QRect(0, 0, 853, 22));
        menuBar->setDefaultUp(false);
        menuFile = new QMenu(menuBar);
        menuFile->setObjectName(QString::fromUtf8("menuFile"));
        menuImport = new QMenu(menuFile);
        menuImport->setObjectName(QString::fromUtf8("menuImport"));
        menuExport = new QMenu(menuFile);
        menuExport->setObjectName(QString::fromUtf8("menuExport"));
        menuEdit = new QMenu(menuBar);
        menuEdit->setObjectName(QString::fromUtf8("menuEdit"));
        menuOptions = new QMenu(menuBar);
        menuOptions->setObjectName(QString::fromUtf8("menuOptions"));
        menuView = new QMenu(menuBar);
        menuView->setObjectName(QString::fromUtf8("menuView"));
        menuHelp = new QMenu(menuBar);
        menuHelp->setObjectName(QString::fromUtf8("menuHelp"));
        VeraCoreQTClass->setMenuBar(menuBar);
        statusBar = new QStatusBar(VeraCoreQTClass);
        statusBar->setObjectName(QString::fromUtf8("statusBar"));
        VeraCoreQTClass->setStatusBar(statusBar);

        menuBar->addAction(menuFile->menuAction());
        menuBar->addAction(menuEdit->menuAction());
        menuBar->addAction(menuOptions->menuAction());
        menuBar->addAction(menuView->menuAction());
        menuBar->addAction(menuHelp->menuAction());
        menuFile->addAction(actionOpen);
        menuFile->addAction(actionNew);
        menuFile->addSeparator();
        menuFile->addAction(actionSave);
        menuFile->addAction(actionSave_As);
        menuFile->addSeparator();
        menuFile->addAction(menuImport->menuAction());
        menuFile->addAction(menuExport->menuAction());
        menuFile->addSeparator();
        menuFile->addAction(actionExit);
        menuImport->addAction(actionMIDI);
        menuExport->addAction(actionWAV);
        menuExport->addAction(actionFLAC);
        menuExport->addAction(actionMP3);
        menuExport->addAction(actionOGG);
        menuExport->addAction(actionMIDI_2);
        menuExport->addSeparator();
        menuExport->addAction(actionZipped_Project);
        menuEdit->addAction(actionUndo);
        menuEdit->addAction(actionRedo);
        menuEdit->addSeparator();
        menuEdit->addAction(actionCopy);
        menuEdit->addAction(actionPaste);
        menuEdit->addAction(actionCut);
        menuOptions->addAction(actionPreferences);
        menuOptions->addAction(actionProject_Settings);
        menuView->addAction(actionPlaylist);
        menuView->addAction(actionMixer);
        menuView->addAction(actionMIDI_Editor);
        menuView->addAction(actionBrowser);
        menuView->addAction(actionTempo_Tapper);
        menuHelp->addAction(actionAbout);
        menuHelp->addAction(actionHelp);
        menuHelp->addSeparator();
        menuHelp->addAction(actionCheck_For_Update);

        retranslateUi(VeraCoreQTClass);
        QObject::connect(actionExit, &QAction::triggered, VeraCoreQTClass, qOverload<>(&QMainWindow::close));

        QMetaObject::connectSlotsByName(VeraCoreQTClass);
    } // setupUi

    void retranslateUi(QMainWindow *VeraCoreQTClass)
    {
        VeraCoreQTClass->setWindowTitle(QCoreApplication::translate("VeraCoreQTClass", "VeraCore", nullptr));
        actionOpen->setText(QCoreApplication::translate("VeraCoreQTClass", "Open", nullptr));
        actionNew->setText(QCoreApplication::translate("VeraCoreQTClass", "New", nullptr));
        actionSave->setText(QCoreApplication::translate("VeraCoreQTClass", "Save", nullptr));
        actionSave_As->setText(QCoreApplication::translate("VeraCoreQTClass", "Save As", nullptr));
        actionExit->setText(QCoreApplication::translate("VeraCoreQTClass", "Exit", nullptr));
        actionMIDI->setText(QCoreApplication::translate("VeraCoreQTClass", "MIDI", nullptr));
        actionWAV->setText(QCoreApplication::translate("VeraCoreQTClass", "WAV", nullptr));
        actionFLAC->setText(QCoreApplication::translate("VeraCoreQTClass", "FLAC", nullptr));
        actionMP3->setText(QCoreApplication::translate("VeraCoreQTClass", "MP3", nullptr));
        actionOGG->setText(QCoreApplication::translate("VeraCoreQTClass", "OGG", nullptr));
        actionMIDI_2->setText(QCoreApplication::translate("VeraCoreQTClass", "MIDI", nullptr));
        actionZipped_Project->setText(QCoreApplication::translate("VeraCoreQTClass", "Zipped Project", nullptr));
        actionUndo->setText(QCoreApplication::translate("VeraCoreQTClass", "Undo", nullptr));
        actionRedo->setText(QCoreApplication::translate("VeraCoreQTClass", "Redo", nullptr));
        actionCopy->setText(QCoreApplication::translate("VeraCoreQTClass", "Copy", nullptr));
        actionPaste->setText(QCoreApplication::translate("VeraCoreQTClass", "Paste", nullptr));
        actionCut->setText(QCoreApplication::translate("VeraCoreQTClass", "Cut", nullptr));
        actionGeneral->setText(QCoreApplication::translate("VeraCoreQTClass", "General", nullptr));
        actionAudio->setText(QCoreApplication::translate("VeraCoreQTClass", "Audio", nullptr));
        actionMIDI_3->setText(QCoreApplication::translate("VeraCoreQTClass", "MIDI", nullptr));
        actionPlugins->setText(QCoreApplication::translate("VeraCoreQTClass", "Plugins", nullptr));
        actionFiles->setText(QCoreApplication::translate("VeraCoreQTClass", "Files", nullptr));
        actionPlaylist->setText(QCoreApplication::translate("VeraCoreQTClass", "Playlist", nullptr));
        actionMixer->setText(QCoreApplication::translate("VeraCoreQTClass", "Mixer", nullptr));
        actionMIDI_Editor->setText(QCoreApplication::translate("VeraCoreQTClass", "MIDI Editor", nullptr));
        actionBrowser->setText(QCoreApplication::translate("VeraCoreQTClass", "Browser", nullptr));
        actionTempo_Tapper->setText(QCoreApplication::translate("VeraCoreQTClass", "Tempo Tapper", nullptr));
        actionAbout->setText(QCoreApplication::translate("VeraCoreQTClass", "About", nullptr));
        actionHelp->setText(QCoreApplication::translate("VeraCoreQTClass", "Help", nullptr));
        actionCheck_For_Update->setText(QCoreApplication::translate("VeraCoreQTClass", "Check For Update", nullptr));
        actionPreferences->setText(QCoreApplication::translate("VeraCoreQTClass", "Preferences", nullptr));
        actionProject_Settings->setText(QCoreApplication::translate("VeraCoreQTClass", "Project Settings", nullptr));
        QTreeWidgetItem *___qtreewidgetitem = browserTree->headerItem();
        ___qtreewidgetitem->setText(0, QCoreApplication::translate("VeraCoreQTClass", "Browser", nullptr));

        const bool __sortingEnabled = browserTree->isSortingEnabled();
        browserTree->setSortingEnabled(false);
        QTreeWidgetItem *___qtreewidgetitem1 = browserTree->topLevelItem(0);
        ___qtreewidgetitem1->setText(0, QCoreApplication::translate("VeraCoreQTClass", "Plugins", nullptr));
        QTreeWidgetItem *___qtreewidgetitem2 = ___qtreewidgetitem1->child(0);
        ___qtreewidgetitem2->setText(0, QCoreApplication::translate("VeraCoreQTClass", "Audio Plugins", nullptr));
        QTreeWidgetItem *___qtreewidgetitem3 = ___qtreewidgetitem2->child(0);
        ___qtreewidgetitem3->setText(0, QCoreApplication::translate("VeraCoreQTClass", "Clap", nullptr));
        QTreeWidgetItem *___qtreewidgetitem4 = ___qtreewidgetitem2->child(1);
        ___qtreewidgetitem4->setText(0, QCoreApplication::translate("VeraCoreQTClass", "VST3", nullptr));
        QTreeWidgetItem *___qtreewidgetitem5 = ___qtreewidgetitem2->child(2);
        ___qtreewidgetitem5->setText(0, QCoreApplication::translate("VeraCoreQTClass", "VST", nullptr));
        QTreeWidgetItem *___qtreewidgetitem6 = ___qtreewidgetitem1->child(1);
        ___qtreewidgetitem6->setText(0, QCoreApplication::translate("VeraCoreQTClass", "Generator Plugins", nullptr));
        QTreeWidgetItem *___qtreewidgetitem7 = ___qtreewidgetitem6->child(0);
        ___qtreewidgetitem7->setText(0, QCoreApplication::translate("VeraCoreQTClass", "Clap", nullptr));
        QTreeWidgetItem *___qtreewidgetitem8 = ___qtreewidgetitem6->child(1);
        ___qtreewidgetitem8->setText(0, QCoreApplication::translate("VeraCoreQTClass", "VST3", nullptr));
        QTreeWidgetItem *___qtreewidgetitem9 = ___qtreewidgetitem6->child(2);
        ___qtreewidgetitem9->setText(0, QCoreApplication::translate("VeraCoreQTClass", "VST", nullptr));
        QTreeWidgetItem *___qtreewidgetitem10 = browserTree->topLevelItem(1);
        ___qtreewidgetitem10->setText(0, QCoreApplication::translate("VeraCoreQTClass", "Sample Packs", nullptr));
        browserTree->setSortingEnabled(__sortingEnabled);

        menuFile->setTitle(QCoreApplication::translate("VeraCoreQTClass", "File", nullptr));
        menuImport->setTitle(QCoreApplication::translate("VeraCoreQTClass", "Import", nullptr));
        menuExport->setTitle(QCoreApplication::translate("VeraCoreQTClass", "Export", nullptr));
        menuEdit->setTitle(QCoreApplication::translate("VeraCoreQTClass", "Edit", nullptr));
        menuOptions->setTitle(QCoreApplication::translate("VeraCoreQTClass", "Options", nullptr));
        menuView->setTitle(QCoreApplication::translate("VeraCoreQTClass", "View", nullptr));
        menuHelp->setTitle(QCoreApplication::translate("VeraCoreQTClass", "Help", nullptr));
    } // retranslateUi

};

namespace Ui {
    class VeraCoreQTClass: public Ui_VeraCoreQTClass {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_VERACOREQT_H
