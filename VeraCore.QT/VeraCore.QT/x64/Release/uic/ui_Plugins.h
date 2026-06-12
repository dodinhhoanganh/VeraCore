/********************************************************************************
** Form generated from reading UI file 'Plugins.ui'
**
** Created by: Qt User Interface Compiler version 6.3.1
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_PLUGINS_H
#define UI_PLUGINS_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QListView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_PluginsSettings
{
public:
    QGroupBox *pathList;
    QListView *listView;
    QPushButton *addBtn;
    QPushButton *removeBtn;
    QPushButton *scanBtn;

    void setupUi(QWidget *PluginsSettings)
    {
        if (PluginsSettings->objectName().isEmpty())
            PluginsSettings->setObjectName(QString::fromUtf8("PluginsSettings"));
        PluginsSettings->resize(739, 524);
        pathList = new QGroupBox(PluginsSettings);
        pathList->setObjectName(QString::fromUtf8("pathList"));
        pathList->setGeometry(QRect(10, 10, 721, 331));
        listView = new QListView(pathList);
        listView->setObjectName(QString::fromUtf8("listView"));
        listView->setGeometry(QRect(10, 30, 701, 261));
        addBtn = new QPushButton(pathList);
        addBtn->setObjectName(QString::fromUtf8("addBtn"));
        addBtn->setGeometry(QRect(560, 300, 75, 24));
        removeBtn = new QPushButton(pathList);
        removeBtn->setObjectName(QString::fromUtf8("removeBtn"));
        removeBtn->setGeometry(QRect(640, 300, 75, 24));
        scanBtn = new QPushButton(pathList);
        scanBtn->setObjectName(QString::fromUtf8("scanBtn"));
        scanBtn->setGeometry(QRect(10, 300, 75, 24));

        retranslateUi(PluginsSettings);

        QMetaObject::connectSlotsByName(PluginsSettings);
    } // setupUi

    void retranslateUi(QWidget *PluginsSettings)
    {
        PluginsSettings->setWindowTitle(QCoreApplication::translate("PluginsSettings", "PluginsSettings", nullptr));
        pathList->setTitle(QCoreApplication::translate("PluginsSettings", "Path", nullptr));
        addBtn->setText(QCoreApplication::translate("PluginsSettings", "Add", nullptr));
        removeBtn->setText(QCoreApplication::translate("PluginsSettings", "Remove", nullptr));
        scanBtn->setText(QCoreApplication::translate("PluginsSettings", "Scan", nullptr));
    } // retranslateUi

};

namespace Ui {
    class PluginsSettings: public Ui_PluginsSettings {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_PLUGINS_H
