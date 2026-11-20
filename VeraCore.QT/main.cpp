#include "VeraCoreQT.h"
#include "DarkStyle.h"
#include "PluginLoader.h"
#include "DawTheme.h"
#include <QtWidgets/QApplication>
#include <QCoreApplication>
#include <QByteArray>
#include <QFont>
#include <QFontDatabase>
#include <QSplashScreen>
#include <QSettings>
#include "AudioGeneral.h"
shared_ptr<AudioGeneral> audioGeneral;
int main(int argc, char *argv[])
{
    if (argc >= 2 && QByteArray(argv[1]) == "--veracore-scan-helper") {
        QCoreApplication helper(argc, argv);
        return PluginLoader::RunScanHelper(helper.arguments());
    }
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication a(argc, argv);
    QCoreApplication::setOrganizationName("VeraCore");
    QCoreApplication::setApplicationName("VeraCore");
    QApplication::setStyle(new DarkStyle);
    QApplication::setFont(DawTheme::preferredUiFont(QFontDatabase::families()));
    a.setProperty("veracoreTheme", DawTheme::professionalDark().id);
    QPixmap pixmap("images\\splash.png");
    QPixmap splashPixmap = pixmap.scaled(500, 500);
    Qt::Alignment bottomLeft = Qt::AlignLeft | Qt::AlignBottom;
    QSplashScreen splash(splashPixmap, Qt::WindowStaysOnTopHint);
    splash.show();
    a.processEvents();
    splash.showMessage("Initializing Settings...", bottomLeft, Qt::white);
    QSettings settings;
    if (!settings.value("DevIndex").isValid())
    {
        settings.setValue("DevIndex", -1);
        settings.setValue("DevName", "Default");
        settings.setValue("Backend", 1);
        settings.setValue("SRate", 44100);
    }
    splash.showMessage("Initializing Audio Core...", bottomLeft, Qt::white);
    audioGeneral = make_shared<AudioGeneral>();
    splash.showMessage("Initializing Plugins...", bottomLeft, Qt::white);

    splash.showMessage("Initializing Files...", bottomLeft, Qt::white);

    splash.showMessage("Initializing...", bottomLeft, Qt::white);
    VeraCoreQT w;
    w.ConnectAudio(audioGeneral);
    splash.finish(&w);
    if (settings.value("Workspace/MainWindowWasMaximized", true).toBool()) {
        w.showMaximized();
    } else {
        w.show();
    }
    return a.exec();
}
