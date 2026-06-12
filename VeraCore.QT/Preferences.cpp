#include "Preferences.h"
#include <QMessageBox>
#include <QFileDialog>
#include <QStringListModel>
#include "PluginManager.h"
#include "PluginScannerWorker.h"
#include "VeraCoreQT.h"
#include <QStatusBar>

Preferences::Preferences(QObject *parent)
	: QObject(parent)
{
    audioGeneral = make_shared<AudioGeneral>();
    vstPathModel = new QStringListModel(this);
}

void Preferences::ConnectAudio(const shared_ptr<AudioGeneral>& audio) {
    this->audioGeneral = audio;
}

void Preferences::InitPreferences()
{
    settings.setupUi(&settingDialog);

    generalSettings.setupUi(&generalDialog);
    settings.tabWidget->addTab(&generalDialog, "General");

    audioSettings.setupUi(&audioDialog);
    settings.tabWidget->addTab(&audioDialog, "Audio");
    connect(audioSettings.showASIODevice, &QPushButton::clicked, [=]() {
        std::string devName = audioSettings.device->currentText().toStdString();
        if (deviceList.find(devName) != deviceList.end()) {
            int deviceIndex = deviceList.at(devName);
            int sampleRate = audioSettings.sampleRate->currentText().toInt();

            // PortAudio's PaAsio_ShowControlPanel returns paDeviceUnavailable if an ASIO stream is already open.
            // We must close the stream before requesting the control panel.
            audioGeneral->audioCore.CloseStream();

            audioGeneral->err = PaAsio_ShowControlPanel(deviceIndex, (void*)settingDialog.winId());
            if (audioGeneral->err != paNoError) {
                QMessageBox::critical(nullptr, "ASIO Panel Error", Pa_GetErrorText(audioGeneral->err));
            }

            // Re-open the stream after the user closes the panel
            audioGeneral->err = audioGeneral->audioCore.OpenStreamASIO(sampleRate, deviceIndex);
            if (audioGeneral->err != paNoError) {
                QMessageBox::critical(nullptr, "Audio Stream Error", Pa_GetErrorText(audioGeneral->err));
            }
        } else {
            QMessageBox::warning(nullptr, "ASIO Panel", "Please select a valid ASIO device first.");
        }
    });
    connect(audioSettings.deviceBackend, &QComboBox::currentIndexChanged, [=]() {
        switch (audioSettings.deviceBackend->currentIndex())
        {
        case 0:
            GetASIODevice();
            break;
        case 1:
            GetWASAPIDevice();
            break;
        default:
            break;
        }
        appPreferences.setValue("Backend", audioSettings.deviceBackend->currentIndex());
    });
    audioSettings.deviceBackend->setCurrentIndex(appPreferences.value("Backend").toInt());
    audioSettings.device->setCurrentText(appPreferences.value("DevName").toString());
    audioSettings.sampleRate->setCurrentText(appPreferences.value("SRate").toString());
    connect(audioSettings.device, qOverload<int>(&QComboBox::activated), [=]() {
        int sampleRate = audioSettings.sampleRate->currentText().toInt();
        int deviceIndex = -1;
        deviceIndex = deviceList.find(audioSettings.device->currentText().toStdString())->second;
        appPreferences.setValue("DevIndex", deviceIndex);
        appPreferences.setValue("DevName", audioSettings.device->currentText());
        appPreferences.setValue("SRate", audioSettings.sampleRate->currentText());
        QMessageBox::critical(nullptr, "test", QString::number(appPreferences.value("DevIndex").toInt()));
        if (appPreferences.value("DevIndex").toInt() != -1)
            audioGeneral->err = audioGeneral->audioCore.CloseStream();
        switch (audioSettings.deviceBackend->currentIndex())
        {
            case 0:
                if (audioGeneral->err == paNoError)
                {
                    audioGeneral->err = audioGeneral->audioCore.OpenStreamASIO(sampleRate, deviceIndex);
                    if (audioGeneral->err != paNoError)
                        QMessageBox::critical(nullptr, "Open Audio", Pa_GetErrorText(audioGeneral->err));
                }
                else QMessageBox::critical(nullptr, "Close Audio", Pa_GetErrorText(audioGeneral->err));
                break;
            case 1:
                if (audioGeneral->err == paNoError)
                {
                    audioGeneral->err = audioGeneral->audioCore.OpenStreamWASAPI(sampleRate, deviceIndex);
                    if (audioGeneral->err != paNoError)
                        QMessageBox::critical(nullptr, "Open Audio", Pa_GetErrorText(audioGeneral->err));
                }
                else QMessageBox::critical(nullptr, "Close Audio", Pa_GetErrorText(audioGeneral->err));
                break;
            default:
                break;
        }
    });

    midSettings.setupUi(&midDialog);
    settings.tabWidget->addTab(&midDialog, "MIDI");

    vstSettings.setupUi(&vstDialog);
    settings.tabWidget->addTab(&vstDialog, "Plugins");

    filesSettings.setupUi(&filesDialog);
    settings.tabWidget->addTab(&filesDialog, "Files");

    InitSetting();

    settingDialog.exec();
}

void Preferences::ApplySetting()
{
    
}

void Preferences::InitSetting()
{
    GetGeneralSetting();
    GetAudioSetting();
    GetMidiSetting();
    GetFolderVSTList();
    GetFolderList();
}

//General
void Preferences::GetGeneralSetting()
{

}

//Audio
void Preferences::GetAudioSetting()
{
    GetASIODevice();
}

void Preferences::GetASIODevice()
{
    const PaDeviceInfo* deviceInfo;
    audioSettings.device->clear();
    deviceList.clear();
    for (long i = 0; i < Pa_GetDeviceCount(); i++)
    {
        deviceInfo = Pa_GetDeviceInfo(i);
        if (Pa_GetHostApiInfo(deviceInfo->hostApi)->type == paASIO)
            deviceList.insert({ deviceInfo->name, i });     
    }
    
    for (std::map<string, int>::iterator it = deviceList.begin(); it != deviceList.end(); ++it)
        audioSettings.device->addItem(QString::fromStdString(it->first));
}

void Preferences::GetWASAPIDevice()
{
    const PaDeviceInfo* deviceInfo;
    audioSettings.device->clear();
    deviceList.clear();
    for (long i = 0; i < Pa_GetDeviceCount(); i++)
    {
        deviceInfo = Pa_GetDeviceInfo(i);
        if (Pa_GetHostApiInfo(deviceInfo->hostApi)->type == paWASAPI)
            deviceList.insert({ deviceInfo->name, i });
    }
    
    for (std::map<string, int>::iterator it = deviceList.begin(); it != deviceList.end(); ++it)
        audioSettings.device->addItem(QString::fromStdString(it->first));
}

//MIDI
void Preferences::GetMidiSetting()
{

}

void Preferences::GetMidiInDevice()
{

}

void Preferences::GetMidiOutDevice()
{

}

//VST
void Preferences::GetFolderVSTList()
{
    // Update the Model
    vstPathModel->setStringList(PluginManager::GetInstance().GetCustomPaths());
    vstSettings.listView->setModel(vstPathModel);

    // Add Button
    connect(vstSettings.addBtn, &QPushButton::clicked, [=]() {
        QString dir = QFileDialog::getExistingDirectory(nullptr, "Select Plugin Folder", "", QFileDialog::ShowDirsOnly);
        if (!dir.isEmpty()) {
            QStringList currentPaths = PluginManager::GetInstance().GetCustomPaths();
            if (!currentPaths.contains(dir)) {
                currentPaths.append(dir);
                PluginManager::GetInstance().SetCustomPaths(currentPaths);
                vstPathModel->setStringList(PluginManager::GetInstance().GetCustomPaths());
            }
        }
    });

    // Remove Button
    connect(vstSettings.removeBtn, &QPushButton::clicked, [=]() {
        QModelIndex index = vstSettings.listView->currentIndex();
        if (index.isValid()) {
            QStringList currentPaths = PluginManager::GetInstance().GetCustomPaths();
            currentPaths.removeAt(index.row());
            PluginManager::GetInstance().SetCustomPaths(currentPaths);
            vstPathModel->setStringList(PluginManager::GetInstance().GetCustomPaths());
        }
    });

    // Scan Button
    connect(vstSettings.scanBtn, &QPushButton::clicked, [=]() {
        PluginScannerWorker* worker = new PluginScannerWorker(PluginManager::GetInstance().GetAllPaths(), PluginManager::GetInstance().GetDiscoveredPlugins());
        
        // Disable button
        vstSettings.scanBtn->setEnabled(false);
        
        // To show status in VeraCoreQT, we can find the QMainWindow
        QMainWindow* mainWindow = nullptr;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (QMainWindow* main = qobject_cast<QMainWindow*>(widget)) {
                mainWindow = main;
                break;
            }
        }
        
        if (mainWindow) {
            connect(worker, &PluginScannerWorker::progressUpdated, [mainWindow](int percent, const QString& currentPlugin) {
                mainWindow->statusBar()->showMessage(QString("Scanning plugins: %1% - %2").arg(percent).arg(currentPlugin));
            });
            
            connect(worker, &PluginScannerWorker::pluginDiscovered, [mainWindow](const PluginInfo& info) {
                if (VeraCoreQT* veraCore = qobject_cast<VeraCoreQT*>(mainWindow)) {
                    veraCore->addSinglePluginToBrowser(info);
                }
            });
        }
        
        connect(worker, &PluginScannerWorker::scanFinished, [=](const std::vector<PluginInfo>& plugins) {
            PluginManager::GetInstance().SetDiscoveredPlugins(plugins);
            vstSettings.scanBtn->setEnabled(true);
            
            if (mainWindow) {
                mainWindow->statusBar()->showMessage("Plugin scan complete.", 5000);
                if (VeraCoreQT* veraCore = qobject_cast<VeraCoreQT*>(mainWindow)) {
                    veraCore->updateBrowserTree();
                }
            }
            int count = plugins.size();
            QMessageBox::information(nullptr, "Plugin Scan Complete", QString("Found %1 plugins.").arg(count));
            worker->deleteLater();
        });
        
        worker->start();
    });
}

//Folder
void Preferences::GetFolderList()
{

}

Preferences::~Preferences()
{}
