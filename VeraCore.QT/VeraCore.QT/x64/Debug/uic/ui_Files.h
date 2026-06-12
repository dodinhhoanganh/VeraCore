/********************************************************************************
** Form generated from reading UI file 'Files.ui'
**
** Created by: Qt User Interface Compiler version 6.3.1
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_FILES_H
#define UI_FILES_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QListView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_FilesSettings
{
public:
    QGroupBox *pathList;
    QListView *listView;
    QPushButton *addBtn;
    QPushButton *removeBtn;

    void setupUi(QWidget *FilesSettings)
    {
        if (FilesSettings->objectName().isEmpty())
            FilesSettings->setObjectName(QString::fromUtf8("FilesSettings"));
        FilesSettings->resize(744, 520);
        pathList = new QGroupBox(FilesSettings);
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

        retranslateUi(FilesSettings);

        QMetaObject::connectSlotsByName(FilesSettings);
    } // setupUi

    void retranslateUi(QWidget *FilesSettings)
    {
        FilesSettings->setWindowTitle(QCoreApplication::translate("FilesSettings", "FilesSettings", nullptr));
        pathList->setTitle(QCoreApplication::translate("FilesSettings", "Path", nullptr));
        addBtn->setText(QCoreApplication::translate("FilesSettings", "Add", nullptr));
        removeBtn->setText(QCoreApplication::translate("FilesSettings", "Remove", nullptr));
    } // retranslateUi

};

namespace Ui {
    class FilesSettings: public Ui_FilesSettings {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_FILES_H
