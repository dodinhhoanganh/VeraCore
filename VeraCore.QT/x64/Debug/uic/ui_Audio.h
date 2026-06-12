/********************************************************************************
** Form generated from reading UI file 'Audio.ui'
**
** Created by: Qt User Interface Compiler version 6.3.1
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_AUDIO_H
#define UI_AUDIO_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_AudioSettings
{
public:
    QGroupBox *backendGroup;
    QComboBox *deviceBackend;
    QComboBox *device;
    QPushButton *showASIODevice;
    QGroupBox *audioSettings;
    QComboBox *sampleRate;
    QLabel *sampleRateLabel;

    void setupUi(QWidget *AudioSettings)
    {
        if (AudioSettings->objectName().isEmpty())
            AudioSettings->setObjectName(QString::fromUtf8("AudioSettings"));
        AudioSettings->resize(648, 428);
        AudioSettings->setAcceptDrops(true);
        backendGroup = new QGroupBox(AudioSettings);
        backendGroup->setObjectName(QString::fromUtf8("backendGroup"));
        backendGroup->setGeometry(QRect(10, 10, 621, 111));
        deviceBackend = new QComboBox(backendGroup);
        deviceBackend->addItem(QString());
        deviceBackend->addItem(QString());
        deviceBackend->setObjectName(QString::fromUtf8("deviceBackend"));
        deviceBackend->setGeometry(QRect(10, 20, 601, 22));
        device = new QComboBox(backendGroup);
        device->setObjectName(QString::fromUtf8("device"));
        device->setGeometry(QRect(10, 50, 601, 22));
        showASIODevice = new QPushButton(backendGroup);
        showASIODevice->setObjectName(QString::fromUtf8("showASIODevice"));
        showASIODevice->setGeometry(QRect(10, 80, 111, 24));
        audioSettings = new QGroupBox(AudioSettings);
        audioSettings->setObjectName(QString::fromUtf8("audioSettings"));
        audioSettings->setGeometry(QRect(9, 140, 621, 81));
        sampleRate = new QComboBox(audioSettings);
        sampleRate->addItem(QString());
        sampleRate->addItem(QString());
        sampleRate->addItem(QString());
        sampleRate->addItem(QString());
        sampleRate->addItem(QString());
        sampleRate->setObjectName(QString::fromUtf8("sampleRate"));
        sampleRate->setGeometry(QRect(10, 40, 601, 22));
        sampleRateLabel = new QLabel(audioSettings);
        sampleRateLabel->setObjectName(QString::fromUtf8("sampleRateLabel"));
        sampleRateLabel->setGeometry(QRect(10, 20, 101, 16));

        retranslateUi(AudioSettings);

        QMetaObject::connectSlotsByName(AudioSettings);
    } // setupUi

    void retranslateUi(QWidget *AudioSettings)
    {
        AudioSettings->setWindowTitle(QCoreApplication::translate("AudioSettings", "AudioSettings", nullptr));
        backendGroup->setTitle(QCoreApplication::translate("AudioSettings", "Audio Backend", nullptr));
        deviceBackend->setItemText(0, QCoreApplication::translate("AudioSettings", "ASIO", nullptr));
        deviceBackend->setItemText(1, QCoreApplication::translate("AudioSettings", "MME", nullptr));

        showASIODevice->setText(QCoreApplication::translate("AudioSettings", "Show ASIO panel", nullptr));
        audioSettings->setTitle(QCoreApplication::translate("AudioSettings", "Audio Settings", nullptr));
        sampleRate->setItemText(0, QCoreApplication::translate("AudioSettings", "44100", nullptr));
        sampleRate->setItemText(1, QCoreApplication::translate("AudioSettings", "48000", nullptr));
        sampleRate->setItemText(2, QCoreApplication::translate("AudioSettings", "88200", nullptr));
        sampleRate->setItemText(3, QCoreApplication::translate("AudioSettings", "96000", nullptr));
        sampleRate->setItemText(4, QCoreApplication::translate("AudioSettings", "192000", nullptr));

        sampleRateLabel->setText(QCoreApplication::translate("AudioSettings", "Sample Rate (Hz):", nullptr));
    } // retranslateUi

};

namespace Ui {
    class AudioSettings: public Ui_AudioSettings {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_AUDIO_H
