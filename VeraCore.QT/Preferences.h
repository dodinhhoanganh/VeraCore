#pragma once

#include <QObject>
#include "ui_SettingsDialog.h"
#include "ui_General.h"
#include "ui_Audio.h"
#include "ui_MIDI.h"
#include "ui_Plugins.h"
#include "ui_Files.h"
#include <QSettings>
#include <map>
#include "AudioGeneral.h"
#include "PluginManager.h"
#include <QStringListModel>
using namespace std;

class Preferences  : public QObject
{
	Q_OBJECT

public:
	Preferences(QObject *parent);
	~Preferences();
	void InitPreferences();
	void InitSetting();
	void ApplySetting();
	void GetASIODevice();
	void GetWASAPIDevice();
	void GetFolderList();
	void GetFolderVSTList();
	void GetGeneralSetting();
	void GetAudioSetting();
	void RestartSelectedAudio();
	void RefreshAudioStatus();
	void GetMidiInDevice();
	void GetMidiOutDevice();
	void GetMidiSetting();
	void ConnectAudio(const shared_ptr<AudioGeneral>& audio);
	Ui::SettingsDialog settings;
	QDialog settingDialog;
	Ui::GeneralSettings generalSettings;
	QWidget generalDialog;
	Ui::AudioSettings audioSettings;
	QWidget audioDialog;
	Ui::MIDISettings midSettings;
	QWidget midDialog;
	Ui::PluginsSettings vstSettings;
	QWidget vstDialog;
	Ui::FilesSettings filesSettings;
	QWidget filesDialog;
	QSettings appPreferences;
	map<string, int> deviceList;
	shared_ptr<AudioGeneral> audioGeneral;
	QStringListModel* vstPathModel;
	QStringListModel* samplePathModel;

signals:
	void audioConfigurationChangeStarted();
	void audioConfigurationChangeFinished(bool streamActive);
};
