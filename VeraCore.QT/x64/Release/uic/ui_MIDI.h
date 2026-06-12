/********************************************************************************
** Form generated from reading UI file 'MIDI.ui'
**
** Created by: Qt User Interface Compiler version 6.3.1
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_MIDI_H
#define UI_MIDI_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_MIDISettings
{
public:
    QGroupBox *midiDevice;
    QLabel *label;
    QLabel *label_2;
    QComboBox *comboBox;
    QComboBox *comboBox_2;

    void setupUi(QWidget *MIDISettings)
    {
        if (MIDISettings->objectName().isEmpty())
            MIDISettings->setObjectName(QString::fromUtf8("MIDISettings"));
        MIDISettings->resize(792, 150);
        midiDevice = new QGroupBox(MIDISettings);
        midiDevice->setObjectName(QString::fromUtf8("midiDevice"));
        midiDevice->setGeometry(QRect(10, 10, 771, 121));
        label = new QLabel(midiDevice);
        label->setObjectName(QString::fromUtf8("label"));
        label->setGeometry(QRect(10, 20, 49, 16));
        label_2 = new QLabel(midiDevice);
        label_2->setObjectName(QString::fromUtf8("label_2"));
        label_2->setGeometry(QRect(10, 70, 49, 16));
        comboBox = new QComboBox(midiDevice);
        comboBox->setObjectName(QString::fromUtf8("comboBox"));
        comboBox->setGeometry(QRect(10, 40, 741, 22));
        comboBox_2 = new QComboBox(midiDevice);
        comboBox_2->setObjectName(QString::fromUtf8("comboBox_2"));
        comboBox_2->setGeometry(QRect(10, 90, 741, 22));

        retranslateUi(MIDISettings);

        QMetaObject::connectSlotsByName(MIDISettings);
    } // setupUi

    void retranslateUi(QWidget *MIDISettings)
    {
        MIDISettings->setWindowTitle(QCoreApplication::translate("MIDISettings", "MIDISettings", nullptr));
        midiDevice->setTitle(QCoreApplication::translate("MIDISettings", "MIDI Device", nullptr));
        label->setText(QCoreApplication::translate("MIDISettings", "MIDI In:", nullptr));
        label_2->setText(QCoreApplication::translate("MIDISettings", "MIDI Out:", nullptr));
    } // retranslateUi

};

namespace Ui {
    class MIDISettings: public Ui_MIDISettings {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_MIDI_H
