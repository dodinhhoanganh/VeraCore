#include "Preferences.h"
#include <QMessageBox>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringListModel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QSignalBlocker>
#include "PluginManager.h"
#include "PluginScannerWorker.h"
#include "SampleScannerWorker.h"
#include "StemSeparationService.h"
#include "VeraCoreQT.h"
#include <QDateTime>
#include <QProcess>
#include <QSharedPointer>
#include <QStatusBar>
#include <QThread>
#include <QTimer>

Preferences::Preferences(QObject *parent)
	: QObject(parent)
{
    vstPathModel = new QStringListModel(this);
    samplePathModel = new QStringListModel(this);
}

void Preferences::ConnectAudio(const shared_ptr<AudioGeneral>& audio) {
    this->audioGeneral = audio;
}

void Preferences::InitPreferences()
{
    settings.setupUi(&settingDialog);
    auto safeSampleRate = [](const QString& text) {
        bool ok = false;
        const int value = text.toInt(&ok);
        return ok && value >= 8000 && value <= 384000 ? value : 44100;
    };

    generalSettings.setupUi(&generalDialog);
    settings.tabWidget->addTab(&generalDialog, "General");

    audioSettings.setupUi(&audioDialog);
    settings.tabWidget->addTab(&audioDialog, "Audio");
    connect(audioSettings.showASIODevice, &QPushButton::clicked, [=]() {
        std::string devName = audioSettings.device->currentText().toStdString();
        if (deviceList.find(devName) != deviceList.end()) {
            int deviceIndex = deviceList.at(devName);
            // PortAudio's PaAsio_ShowControlPanel returns paDeviceUnavailable if an ASIO stream is already open.
            // We must close the stream before requesting the control panel.
            audioGeneral->audioCore.CloseStream();
            emit audioConfigurationChangeStarted();

            audioGeneral->err = PaAsio_ShowControlPanel(deviceIndex, (void*)settingDialog.winId());
            if (audioGeneral->err != paNoError) {
                QMessageBox::critical(nullptr, "ASIO Panel Error", Pa_GetErrorText(audioGeneral->err));
            }

            // Re-open the selected stream after the user closes the panel.
            const PaError restartResult = audioGeneral->RestartAudioFromSettings();
            const bool streamActive = audioGeneral->GetAudioDeviceStatus().active;
            emit audioConfigurationChangeFinished(streamActive);
            if (restartResult != paNoError) {
                QMessageBox::critical(nullptr, "Audio Stream Error", Pa_GetErrorText(restartResult));
            }
        } else {
            QMessageBox::warning(nullptr, "ASIO Panel", "Please select a valid ASIO device first.");
        }
    });
    connect(audioSettings.deviceBackend, qOverload<int>(&QComboBox::currentIndexChanged), [=](int) {
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
    audioSettings.deviceBackend->setCurrentIndex(appPreferences.value("Backend", 1).toInt());
    audioSettings.sampleRate->setCurrentText(QString::number(safeSampleRate(appPreferences.value("SRate", 44100).toString())));
    audioSettings.bufferSize->setCurrentText(QString::number(VeraCoreAudio::NormalizeBufferFrames(
        appPreferences.value("AudioBufferFrames", 512).toULongLong())));
    const QString defaultStemModelDirectory = QDir(
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("Spleeter"));
    audioSettings.stemExecutable->setText(
        appPreferences.value("Audio/StemSeparatorExecutable").toString());
    audioSettings.stemModelDirectory->setText(
        appPreferences.value("Audio/StemSeparatorModelDirectory",
                             defaultStemModelDirectory).toString());
    const QString stemCompute = appPreferences.value(
        "Audio/StemSeparatorCompute", QStringLiteral("Auto")).toString();
    const QString computeLabel = stemCompute.compare("CPU", Qt::CaseInsensitive) == 0
        ? QStringLiteral("CPU")
        : (stemCompute.compare("GPU:1", Qt::CaseInsensitive) == 0
            ? QStringLiteral("GPU 1")
            : (stemCompute.startsWith("GPU", Qt::CaseInsensitive)
                ? QStringLiteral("GPU 0") : QStringLiteral("Auto")));
    audioSettings.stemCompute->setCurrentText(computeLabel);
    const QString stemBackend = appPreferences.value(
        "Audio/StemSeparatorBackend", QStringLiteral("Auto")).toString();
    audioSettings.stemBackend->setCurrentText(
        stemBackend.compare(QStringLiteral("Spleeter"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("Spleeter")
            : (stemBackend.compare(QStringLiteral("Demucs"), Qt::CaseInsensitive) == 0
                ? QStringLiteral("Demucs") : QStringLiteral("Auto")));
    audioSettings.stemCpuThreads->setValue(qBound(
        0, appPreferences.value("Audio/StemSeparatorCpuThreads", 0).toInt(), 256));
    const auto selectedStemRuntimeKey = [this]() -> QString {
        return audioSettings.stemBackend->currentIndex() == 2
            ? QStringLiteral("Audio/DemucsExecutable")
            : QStringLiteral("Audio/StemSeparatorExecutable");
    };
    const auto refreshSelectedStemRuntimePath = [this, selectedStemRuntimeKey]() {
        audioSettings.stemExecutable->setText(
            appPreferences.value(selectedStemRuntimeKey()).toString());
        audioSettings.stemExecutable->setToolTip(
            audioSettings.stemBackend->currentIndex() == 2
                ? QStringLiteral("Demucs executable override")
                : QStringLiteral("Spleeter executable override"));
    };
    refreshSelectedStemRuntimePath();
    const auto refreshStemRuntimeStatus = [this]() {
        QString runtime;
        QStringList prefix;
        QString description;
        const int backendIndex = audioSettings.stemBackend->currentIndex();
        if (backendIndex == 0) {
            QString demucsRuntime;
            QStringList demucsPrefix;
            QString demucsDescription;
            const bool spleeter = StemSeparationService::resolveRuntime(
                &runtime, &prefix, &description);
            const bool demucs = StemSeparationService::resolveDemucsRuntime(
                &demucsRuntime, &demucsPrefix, &demucsDescription);
            audioSettings.stemRuntimeStatus->setText(
                QStringLiteral("Auto: Spleeter %1 · Demucs %2")
                    .arg(spleeter ? QStringLiteral("ready") : QStringLiteral("missing"),
                         demucs ? QStringLiteral("ready") : QStringLiteral("missing")));
            audioSettings.stemRuntimeStatus->setStyleSheet(spleeter || demucs
                ? QStringLiteral("color:#77b893;") : QStringLiteral("color:#d67b72;"));
            return;
        }
        const bool ready = backendIndex == 2
            ? StemSeparationService::resolveDemucsRuntime(&runtime, &prefix, &description)
            : StemSeparationService::resolveRuntime(&runtime, &prefix, &description);
        if (ready) {
            audioSettings.stemRuntimeStatus->setText(
                QStringLiteral("Ready: %1").arg(description));
            audioSettings.stemRuntimeStatus->setStyleSheet(
                prefix.isEmpty() ? QStringLiteral("color:#77b893;")
                                 : QStringLiteral("color:#d6ae72;"));
            return;
        }
        audioSettings.stemRuntimeStatus->setText(QStringLiteral("Runtime not found"));
        audioSettings.stemRuntimeStatus->setStyleSheet(QStringLiteral("color:#d67b72;"));
    };
    const auto refreshStemModelStatus = [this]() {
        const StemModelCacheReport report = StemSeparationService::inspectModelCache(
            audioSettings.stemModelDirectory->text(), false, false);
        audioSettings.stemModelStatus->setText(report.message);
        audioSettings.stemModelStatus->setStyleSheet(report.installedPresetCount == 3
            ? QStringLiteral("color:#77b893;")
            : (report.installedPresetCount > 0
                ? QStringLiteral("color:#d6ae72;")
                : QStringLiteral("color:#d67b72;")));
    };
    const auto refreshStemBackendStatus = [this]() {
        QString spleeterProgram;
        QStringList spleeterPrefix;
        QString demucsProgram;
        QStringList demucsPrefix;
        const bool spleeter = StemSeparationService::resolveRuntime(
            &spleeterProgram, &spleeterPrefix, nullptr);
        const bool demucs = StemSeparationService::resolveDemucsRuntime(
            &demucsProgram, &demucsPrefix, nullptr);
        audioSettings.stemBackendStatus->setText(
            QStringLiteral("Spleeter %1  ·  Demucs %2")
                .arg(spleeter ? QStringLiteral("candidate") : QStringLiteral("missing"),
                     demucs ? QStringLiteral("candidate") : QStringLiteral("missing")));
        audioSettings.stemBackendStatus->setStyleSheet(spleeter || demucs
            ? QStringLiteral("color:#77b893;")
            : QStringLiteral("color:#d67b72;"));
    };
    refreshStemRuntimeStatus();
    refreshStemModelStatus();
    refreshStemBackendStatus();
    connect(audioSettings.browseStemExecutable, &QPushButton::clicked, this,
            [this, selectedStemRuntimeKey, refreshStemRuntimeStatus]() {
        const QString backendName = audioSettings.stemBackend->currentIndex() == 2
            ? QStringLiteral("Demucs") : QStringLiteral("Spleeter");
        const QString path = QFileDialog::getOpenFileName(
            &settingDialog, QStringLiteral("Select %1 runtime").arg(backendName),
            audioSettings.stemExecutable->text(),
            QStringLiteral("Executable (*.exe);;All files (*)"));
        if (path.isEmpty()) return;
        audioSettings.stemExecutable->setText(QFileInfo(path).absoluteFilePath());
        appPreferences.setValue(selectedStemRuntimeKey(),
                                audioSettings.stemExecutable->text());
        refreshStemRuntimeStatus();
    });
    connect(audioSettings.stemExecutable, &QLineEdit::editingFinished, this,
            [this, selectedStemRuntimeKey, refreshStemRuntimeStatus]() {
        appPreferences.setValue(selectedStemRuntimeKey(),
                                audioSettings.stemExecutable->text().trimmed());
        refreshStemRuntimeStatus();
    });
    connect(audioSettings.browseStemModelDirectory, &QPushButton::clicked, this,
            [this, refreshStemModelStatus]() {
        const QString path = QFileDialog::getExistingDirectory(
            &settingDialog, QStringLiteral("Select Spleeter model cache"),
            audioSettings.stemModelDirectory->text(), QFileDialog::ShowDirsOnly);
        if (path.isEmpty()) return;
        audioSettings.stemModelDirectory->setText(QDir(path).absolutePath());
        appPreferences.setValue("Audio/StemSeparatorModelDirectory",
                                audioSettings.stemModelDirectory->text());
        refreshStemModelStatus();
    });
    connect(audioSettings.stemModelDirectory, &QLineEdit::editingFinished, this,
            [this, refreshStemModelStatus]() {
        QString path = QDir::cleanPath(audioSettings.stemModelDirectory->text().trimmed());
        if (path.isEmpty() || path == QStringLiteral(".")) {
            path = QDir(QStandardPaths::writableLocation(
                QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("Spleeter"));
        }
        audioSettings.stemModelDirectory->setText(path);
        appPreferences.setValue("Audio/StemSeparatorModelDirectory", path);
        refreshStemModelStatus();
    });
    connect(audioSettings.stemCompute, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        const QString value = index == 1 ? QStringLiteral("CPU")
            : (index == 2 ? QStringLiteral("GPU:0")
                          : (index == 3 ? QStringLiteral("GPU:1")
                                        : QStringLiteral("Auto")));
        appPreferences.setValue("Audio/StemSeparatorCompute", value);
    });
    connect(audioSettings.stemBackend, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this, refreshSelectedStemRuntimePath,
                   refreshStemRuntimeStatus, refreshStemBackendStatus](int index) {
        const QString value = index == 1 ? QStringLiteral("Spleeter")
            : (index == 2 ? QStringLiteral("Demucs") : QStringLiteral("Auto"));
        appPreferences.setValue("Audio/StemSeparatorBackend", value);
        refreshSelectedStemRuntimePath();
        refreshStemRuntimeStatus();
        refreshStemBackendStatus();
    });
    connect(audioSettings.stemCpuThreads, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int threads) {
        appPreferences.setValue("Audio/StemSeparatorCpuThreads", qBound(0, threads, 256));
    });
    connect(audioSettings.verifyStemRuntime, &QPushButton::clicked, this, [this]() {
        QString program;
        QStringList arguments;
        QString description;
        const int backendIndex = audioSettings.stemBackend->currentIndex();
        QString backendName = backendIndex == 2
            ? QStringLiteral("Demucs") : QStringLiteral("Spleeter");
        bool resolved = backendIndex == 2
            ? StemSeparationService::resolveDemucsRuntime(
                &program, &arguments, &description)
            : StemSeparationService::resolveRuntime(
                &program, &arguments, &description);
        if (!resolved && backendIndex == 0) {
            backendName = QStringLiteral("Demucs");
            resolved = StemSeparationService::resolveDemucsRuntime(
                &program, &arguments, &description);
        }
        if (!resolved) {
            audioSettings.stemRuntimeStatus->setText(QStringLiteral("Runtime not found"));
            audioSettings.stemRuntimeStatus->setStyleSheet(
                QStringLiteral("color:#d67b72;"));
            return;
        }
        arguments.push_back(QStringLiteral("--help"));
        auto* probe = new QProcess(this);
        auto output = QSharedPointer<QByteArray>::create();
        auto* timeout = new QTimer(probe);
        timeout->setSingleShot(true);
        probe->setProcessChannelMode(QProcess::MergedChannels);
        audioSettings.verifyStemRuntime->setEnabled(false);
        audioSettings.stemRuntimeStatus->setText(QStringLiteral("Verifying..."));
        audioSettings.stemRuntimeStatus->setStyleSheet(QStringLiteral("color:#d6ae72;"));
        connect(probe, &QProcess::readyReadStandardOutput, this, [probe, output]() {
            output->append(probe->readAllStandardOutput());
            constexpr int kMaximumProbeOutput = 64 * 1024;
            if (output->size() > kMaximumProbeOutput)
                output->remove(0, output->size() - kMaximumProbeOutput);
        });
        connect(timeout, &QTimer::timeout, probe, [probe]() {
            probe->setProperty("timedOut", true);
            if (probe->state() == QProcess::NotRunning) return;
            probe->terminate();
            QTimer::singleShot(500, probe, [probe]() {
                if (probe->state() != QProcess::NotRunning) probe->kill();
            });
        });
        connect(probe, &QProcess::errorOccurred, this,
                [this, probe](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || probe->property("handled").toBool())
                return;
            probe->setProperty("handled", true);
            audioSettings.verifyStemRuntime->setEnabled(true);
            audioSettings.stemRuntimeStatus->setText(QStringLiteral("Start failed"));
            audioSettings.stemRuntimeStatus->setStyleSheet(
                QStringLiteral("color:#d67b72;"));
            probe->deleteLater();
        });
        connect(probe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, probe, timeout, output, program, description, backendName]
                (int exitCode, QProcess::ExitStatus exitStatus) {
            if (probe->property("handled").toBool()) return;
            probe->setProperty("handled", true);
            timeout->stop();
            output->append(probe->readAllStandardOutput());
            const bool timedOut = probe->property("timedOut").toBool();
            const QString text = QString::fromUtf8(*output);
            const bool verified = !timedOut && exitStatus == QProcess::NormalExit &&
                exitCode == 0 && text.contains(backendName, Qt::CaseInsensitive);
            audioSettings.verifyStemRuntime->setEnabled(true);
            audioSettings.stemRuntimeStatus->setText(verified
                ? QStringLiteral("Verified: %1").arg(description)
                : (timedOut ? QStringLiteral("Verification timed out")
                            : QStringLiteral("%1 module unavailable").arg(backendName)));
            audioSettings.stemRuntimeStatus->setStyleSheet(verified
                ? QStringLiteral("color:#77b893;")
                : QStringLiteral("color:#d67b72;"));
            if (verified) {
                const QString keyPrefix = backendName == QStringLiteral("Demucs")
                    ? QStringLiteral("Audio/DemucsRuntimeVerified")
                    : QStringLiteral("Audio/StemSeparatorRuntimeVerified");
                appPreferences.setValue(keyPrefix + QStringLiteral("Path"), program);
                appPreferences.setValue(keyPrefix + QStringLiteral("At"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            }
            probe->deleteLater();
        });
        timeout->start(8000);
        probe->start(program, arguments, QIODevice::ReadOnly);
    });
    connect(audioSettings.verifyStemModels, &QPushButton::clicked, this, [this]() {
        const QString modelRoot = QDir::cleanPath(
            audioSettings.stemModelDirectory->text().trimmed());
        auto report = QSharedPointer<StemModelCacheReport>::create();
        auto* worker = QThread::create([modelRoot, report]() {
            *report = StemSeparationService::inspectModelCache(
                modelRoot, true, true);
        });
        audioSettings.verifyStemModels->setEnabled(false);
        audioSettings.stemModelStatus->setText(QStringLiteral("Calculating SHA-256..."));
        audioSettings.stemModelStatus->setStyleSheet(QStringLiteral("color:#d6ae72;"));
        connect(worker, &QThread::finished, this, [this, report]() {
            audioSettings.verifyStemModels->setEnabled(true);
            audioSettings.stemModelStatus->setText(report->message);
            audioSettings.stemModelStatus->setStyleSheet(report->success
                ? QStringLiteral("color:#77b893;")
                : QStringLiteral("color:#d67b72;"));
            if (report->success) {
                appPreferences.setValue("Audio/StemModelChecksumVerifiedAt",
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                appPreferences.setValue("Audio/StemModelChecksumManifest",
                    report->manifestPath);
            }
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    });
    connect(audioSettings.device, qOverload<int>(&QComboBox::activated), [=]() {
        RestartSelectedAudio();
    });
    connect(audioSettings.sampleRate, qOverload<int>(&QComboBox::activated), [=](int) {
        RestartSelectedAudio();
    });
    connect(audioSettings.bufferSize, qOverload<int>(&QComboBox::activated), [=](int) {
        RestartSelectedAudio();
    });

    midSettings.setupUi(&midDialog);
    settings.tabWidget->addTab(&midDialog, "MIDI");

    vstSettings.setupUi(&vstDialog);
    settings.tabWidget->addTab(&vstDialog, "Plugins");

    filesSettings.setupUi(&filesDialog);
    settings.tabWidget->addTab(&filesDialog, "Files");

    InitSetting();

    QTimer::singleShot(120, this, &Preferences::RefreshAudioStatus);

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
    if (audioSettings.deviceBackend->currentIndex() == 0) {
        GetASIODevice();
    } else {
        GetWASAPIDevice();
    }
    audioSettings.device->setCurrentText(appPreferences.value("DevName").toString());
    RefreshAudioStatus();
}

void Preferences::RestartSelectedAudio()
{
    if (!audioGeneral) return;
    const auto found = deviceList.find(audioSettings.device->currentText().toStdString());
    if (found == deviceList.end()) {
        QMessageBox::warning(&settingDialog, "Audio Device", "Select a valid audio output device.");
        return;
    }
    bool sampleRateOk = false;
    const int parsedSampleRate = audioSettings.sampleRate->currentText().toInt(&sampleRateOk);
    const int sampleRate = sampleRateOk && parsedSampleRate >= 8000 && parsedSampleRate <= 384000
        ? parsedSampleRate : 44100;
    const unsigned long bufferFrames = VeraCoreAudio::NormalizeBufferFrames(
        audioSettings.bufferSize->currentText().toULongLong());
    appPreferences.setValue("Backend", audioSettings.deviceBackend->currentIndex());
    appPreferences.setValue("DevIndex", found->second);
    appPreferences.setValue("DevName", audioSettings.device->currentText());
    appPreferences.setValue("SRate", sampleRate);
    appPreferences.setValue("AudioBufferFrames", static_cast<qulonglong>(bufferFrames));

    // Stop first so the synchronous owner can detach plug-in render callbacks
    // without racing a live driver callback.
    audioGeneral->audioCore.CloseStream();
    emit audioConfigurationChangeStarted();
    const PaError result = audioGeneral->RestartAudioFromSettings();
    emit audioConfigurationChangeFinished(audioGeneral->GetAudioDeviceStatus().active);
    if (result != paNoError) {
        QMessageBox::critical(&settingDialog, "Audio Configuration",
            QString("Configuration rejected: %1\nPrevious working stream restored when available.")
                .arg(QString::fromLocal8Bit(Pa_GetErrorText(result))));
        QSignalBlocker rateBlocker(audioSettings.sampleRate);
        QSignalBlocker bufferBlocker(audioSettings.bufferSize);
        audioSettings.sampleRate->setCurrentText(appPreferences.value("SRate", 44100).toString());
        audioSettings.bufferSize->setCurrentText(appPreferences.value("AudioBufferFrames", 512).toString());
    }
    QTimer::singleShot(80, this, &Preferences::RefreshAudioStatus);
}

void Preferences::RefreshAudioStatus()
{
    if (!audioGeneral) {
        audioSettings.latencyStatus->setText("Audio engine unavailable");
        return;
    }
    audioSettings.latencyStatus->setText(audioGeneral->GetAudioDeviceStatus().message);
}

void Preferences::GetASIODevice()
{
    audioSettings.device->clear();
    deviceList.clear();
    const int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(i);
        const PaHostApiInfo* hostInfo = deviceInfo ? Pa_GetHostApiInfo(deviceInfo->hostApi) : nullptr;
        if (deviceInfo && hostInfo && hostInfo->type == paASIO &&
            deviceInfo->maxOutputChannels >= 2 && deviceInfo->name)
            deviceList.insert({deviceInfo->name, i});
    }
    
    for (std::map<string, int>::iterator it = deviceList.begin(); it != deviceList.end(); ++it)
        audioSettings.device->addItem(QString::fromStdString(it->first));
}

void Preferences::GetWASAPIDevice()
{
    audioSettings.device->clear();
    deviceList.clear();
    const int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(i);
        const PaHostApiInfo* hostInfo = deviceInfo ? Pa_GetHostApiInfo(deviceInfo->hostApi) : nullptr;
        if (deviceInfo && hostInfo && hostInfo->type == paWASAPI &&
            deviceInfo->maxOutputChannels >= 2 && deviceInfo->name)
            deviceList.insert({deviceInfo->name, i});
    }
    
    for (std::map<string, int>::iterator it = deviceList.begin(); it != deviceList.end(); ++it)
        audioSettings.device->addItem(QString::fromStdString(it->first));
}

//MIDI
void Preferences::GetMidiSetting()
{
    connect(midSettings.refreshMidiIn, &QPushButton::clicked, this, &Preferences::GetMidiInDevice, Qt::UniqueConnection);
    connect(midSettings.refreshMidiOut, &QPushButton::clicked, this, &Preferences::GetMidiOutDevice, Qt::UniqueConnection);
    GetMidiInDevice();
    GetMidiOutDevice();
}

void Preferences::GetMidiInDevice()
{
    const QStringList enabled = appPreferences.value("MidiInputsEnabled").toStringList();
    QSignalBlocker blocker(midSettings.midiInList);
    midSettings.midiInList->clear();
    for (const QString& name : AudioGeneral::AvailableMidiInputs()) {
        auto* item = new QListWidgetItem(name, midSettings.midiInList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(enabled.contains(name) ? Qt::Checked : Qt::Unchecked);
    }
    blocker.unblock();
    if (audioGeneral) audioGeneral->OpenMidiInputs(enabled);
    disconnect(midSettings.midiInList, &QListWidget::itemChanged, this, nullptr);
    connect(midSettings.midiInList, &QListWidget::itemChanged, this, [=]() {
        QStringList selected;
        for (int i = 0; i < midSettings.midiInList->count(); ++i) {
            QListWidgetItem* item = midSettings.midiInList->item(i);
            if (item->checkState() == Qt::Checked) selected << item->text();
        }
        appPreferences.setValue("MidiInputsEnabled", selected);
        if (audioGeneral) audioGeneral->OpenMidiInputs(selected);
    }, Qt::UniqueConnection);
}

void Preferences::GetMidiOutDevice()
{
    const QStringList enabled = appPreferences.value("MidiOutputsEnabled").toStringList();
    QSignalBlocker blocker(midSettings.midiOutList);
    midSettings.midiOutList->clear();
    for (const QString& name : AudioGeneral::AvailableMidiOutputs()) {
        auto* item = new QListWidgetItem(name, midSettings.midiOutList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(enabled.contains(name) ? Qt::Checked : Qt::Unchecked);
    }
    blocker.unblock();
    if (audioGeneral) audioGeneral->OpenMidiOutputs(enabled);
    disconnect(midSettings.midiOutList, &QListWidget::itemChanged, this, nullptr);
    connect(midSettings.midiOutList, &QListWidget::itemChanged, this, [=]() {
        QStringList selected;
        for (int i = 0; i < midSettings.midiOutList->count(); ++i) {
            QListWidgetItem* item = midSettings.midiOutList->item(i);
            if (item->checkState() == Qt::Checked) selected << item->text();
        }
        appPreferences.setValue("MidiOutputsEnabled", selected);
        if (audioGeneral) audioGeneral->OpenMidiOutputs(selected);
    }, Qt::UniqueConnection);
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

    auto refreshQuarantineStatus = [this]() {
        const int count = PluginScannerWorker::QuarantinedCount();
        vstSettings.quarantineStatus->setText(count == 0
            ? "No quarantined plug-ins"
            : QString("%1 plug-in%2 quarantined; unchanged binaries are skipped")
                .arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
        vstSettings.clearQuarantineBtn->setEnabled(count > 0);
    };
    refreshQuarantineStatus();

    auto startPluginScan = [this, refreshQuarantineStatus](bool forceRescan) {
        PluginScannerWorker* worker = new PluginScannerWorker(
            PluginManager::GetInstance().GetAllPaths(),
            PluginManager::GetInstance().GetDiscoveredPlugins(), forceRescan);
        vstSettings.scanBtn->setEnabled(false);
        vstSettings.rescanBtn->setEnabled(false);
        vstSettings.clearQuarantineBtn->setEnabled(false);
        vstSettings.scanProgress->setValue(0);
        vstSettings.scanStatus->setText(forceRescan ? "Rescanning all" : "Scanning changes");

        QMainWindow* mainWindow = nullptr;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (QMainWindow* main = qobject_cast<QMainWindow*>(widget)) {
                mainWindow = main;
                break;
            }
        }
        
        connect(worker, &PluginScannerWorker::progressUpdated, this,
            [this, mainWindow](int percent, const QString& currentPlugin) {
                vstSettings.scanProgress->setValue(percent);
                vstSettings.scanStatus->setText(currentPlugin);
                if (!mainWindow) return;
                mainWindow->statusBar()->showMessage(QString("Scanning plugins: %1% - %2").arg(percent).arg(currentPlugin));
            });

        connect(worker, &PluginScannerWorker::pluginDiscovered, this,
            [mainWindow](const PluginInfo& info) {
                if (VeraCoreQT* veraCore = qobject_cast<VeraCoreQT*>(mainWindow)) {
                    veraCore->addSinglePluginToBrowser(info);
                }
            });

        connect(worker, &PluginScannerWorker::scanFinished, this,
            [this, worker, mainWindow, refreshQuarantineStatus]
            (const std::vector<PluginInfo>& plugins) {
            PluginManager::GetInstance().SetDiscoveredPlugins(plugins);
            vstSettings.scanBtn->setEnabled(true);
            vstSettings.rescanBtn->setEnabled(true);
            vstSettings.scanProgress->setValue(100);
            vstSettings.scanStatus->setText(QString("Found %1 plugins").arg((int)plugins.size()));
            refreshQuarantineStatus();

            if (mainWindow) {
                mainWindow->statusBar()->showMessage("Plugin scan complete.", 5000);
                if (VeraCoreQT* veraCore = qobject_cast<VeraCoreQT*>(mainWindow)) {
                    veraCore->updateBrowserTree();
                }
            }
            int count = plugins.size();
            QMessageBox::information(&settingDialog, "Plugin Scan Complete",
                QString("Found %1 plugins. %2 quarantined.")
                    .arg(count).arg(PluginScannerWorker::QuarantinedCount()));
            worker->deleteLater();
        });

        worker->start();
    };

    connect(vstSettings.scanBtn, &QPushButton::clicked, this,
        [startPluginScan]() { startPluginScan(false); });
    connect(vstSettings.rescanBtn, &QPushButton::clicked, this,
        [startPluginScan]() { startPluginScan(true); });
    connect(vstSettings.clearQuarantineBtn, &QPushButton::clicked, this,
        [this, refreshQuarantineStatus]() {
            const int count = PluginScannerWorker::QuarantinedCount();
            PluginScannerWorker::ClearQuarantine();
            refreshQuarantineStatus();
            vstSettings.scanStatus->setText(
                QString("Cleared %1 quarantine entr%2")
                    .arg(count).arg(count == 1 ? QStringLiteral("y") : QStringLiteral("ies")));
        });
}

//Folder
void Preferences::GetFolderList()
{
    const QString key = "SamplePackPaths";
    samplePathModel->setStringList(appPreferences.value(key).toStringList());
    filesSettings.listView->setModel(samplePathModel);

    connect(filesSettings.addBtn, &QPushButton::clicked, [=]() {
        QString dir = QFileDialog::getExistingDirectory(nullptr, "Select Sample Pack Folder", "", QFileDialog::ShowDirsOnly);
        if (dir.isEmpty()) return;
        QStringList paths = appPreferences.value(key).toStringList();
        if (!paths.contains(dir)) {
            paths.append(dir);
            appPreferences.setValue(key, paths);
            samplePathModel->setStringList(paths);
        }
    });

    connect(filesSettings.removeBtn, &QPushButton::clicked, [=]() {
        QModelIndex index = filesSettings.listView->currentIndex();
        if (!index.isValid()) return;
        QStringList paths = appPreferences.value(key).toStringList();
        paths.removeAt(index.row());
        appPreferences.setValue(key, paths);
        samplePathModel->setStringList(paths);
    });

    connect(filesSettings.scanBtn, &QPushButton::clicked, [=]() {
        QStringList paths = appPreferences.value(key).toStringList();
        SampleScannerWorker* worker = new SampleScannerWorker(paths);
        filesSettings.scanBtn->setEnabled(false);
        filesSettings.scanProgress->setValue(0);
        filesSettings.scanStatus->setText("Scanning");

        QMainWindow* mainWindow = nullptr;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (QMainWindow* main = qobject_cast<QMainWindow*>(widget)) {
                mainWindow = main;
                break;
            }
        }

        if (mainWindow) {
            connect(worker, &SampleScannerWorker::progressUpdated, [mainWindow](int percent, const QString& file) {
                mainWindow->statusBar()->showMessage(QString("Scanning samples: %1% - %2").arg(percent).arg(file));
            });
        }
        connect(worker, &SampleScannerWorker::progressUpdated, [=](int percent, const QString& file) {
            filesSettings.scanProgress->setValue(percent);
            filesSettings.scanStatus->setText(file);
        });
        connect(worker, &SampleScannerWorker::scanFinished, [=](const QStringList& samples) {
            appPreferences.setValue("SamplePackFiles", samples);
            filesSettings.scanBtn->setEnabled(true);
            filesSettings.scanProgress->setValue(100);
            filesSettings.scanStatus->setText(QString("Found %1 files; browser marks embedded decoder status").arg(samples.size()));
            if (mainWindow) {
                mainWindow->statusBar()->showMessage("Sample scan complete. Browser labels MIDI, decodable audio, and native-decoder-pending files.", 7000);
            }
            worker->deleteLater();
        });
        worker->start();
    });
}

Preferences::~Preferences()
{}
