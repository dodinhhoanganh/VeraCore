#include "VeraCoreQT.h"
#include "DarkStyle.h"
#include <QtWidgets/QApplication>
#include <QSplashScreen>
#include <QSettings>
#include "AudioGeneral.h"
shared_ptr<AudioGeneral> audioGeneral;
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setStyle(new DarkStyle);
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
        settings.setValue("DevIndex", Pa_GetDefaultOutputDevice());
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
    w.showMaximized();
    return a.exec();
}
