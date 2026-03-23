#include <QtGui/QApplication>
#include "qmlapplicationviewer.h"

#include <QApplication>
#include <QDeclarativeView>
#include <QDeclarativeContext>
#include "syncmanager.h"
#include <QTextCodec>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QDesktopServices>
#include "localsslproxy.h"
#include <QFile>
#include <QTextStream>
#include <QtNetwork/QNetworkConfigurationManager>
#include <QtNetwork/QNetworkSession>

void myMessageOutput(QtMsgType type, const char *msg)
{
    // Можно раскомментировать для логирования в файл на устройстве

    /*QFile file("c:/data/glog.txt");
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        out << msg << "\n";
    }*/

    return;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    qInstallMsgHandler(myMessageOutput);

    QTextCodec::setCodecForCStrings(QTextCodec::codecForName("UTF-8"));
    QTextCodec::setCodecForTr(QTextCodec::codecForName("UTF-8"));
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));

    qDebug() << QDesktopServices::storageLocation(QDesktopServices::DataLocation).toUtf8() << "/wpgcontacts.ini";
    app.setQuitOnLastWindowClosed(false);

    SyncManager syncManager;

    bool isAutostart = false;
    for (int i = 0; i < argc; ++i) {
        if (QString(argv[i]) == "-background") {
            isAutostart = true;
        }
    }

    QmlApplicationViewer view;
    view.setResizeMode(QDeclarativeView::SizeRootObjectToView);
    view.rootContext()->setContextProperty("syncManager", &syncManager);
    view.setSource(QUrl("qrc:/qml/main.qml"));

    if (!isAutostart) {
        view.showFullScreen();
    }

    QNetworkConfigurationManager manager;
    if (manager.capabilities() & QNetworkConfigurationManager::NetworkSessionRequired) {
        QNetworkConfiguration config = manager.defaultConfiguration();
        QNetworkSession* networkSession = new QNetworkSession(config);
        networkSession->open();
    }

    return app.exec();
}
