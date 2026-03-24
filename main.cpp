#include <QtGui/QApplication>
#include "qmlapplicationviewer.h"

#include <QApplication>
#include <QDeclarativeView>
#include <QDeclarativeContext>
#include "syncmanager.h"
#include "filepicker.h"
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
    Q_UNUSED(type);
    Q_UNUSED(msg);
    return;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    qInstallMsgHandler(myMessageOutput);

    QTextCodec::setCodecForCStrings(QTextCodec::codecForName("UTF-8"));
    QTextCodec::setCodecForTr(QTextCodec::codecForName("UTF-8"));
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));

    app.setQuitOnLastWindowClosed(false);

    SyncManager syncManager;
    FilePicker  filePicker;

    bool isAutostart = false;
    for (int i = 0; i < argc; ++i) {
        if (QString(argv[i]) == "-background")
            isAutostart = true;
    }

    QmlApplicationViewer view;
    view.setResizeMode(QDeclarativeView::SizeRootObjectToView);
    view.rootContext()->setContextProperty("syncManager", &syncManager);
    view.rootContext()->setContextProperty("filePicker",  &filePicker);
    view.setSource(QUrl("qrc:/qml/main.qml"));

    if (!isAutostart)
        view.showFullScreen();

    QNetworkConfigurationManager manager;
    if (manager.capabilities() & QNetworkConfigurationManager::NetworkSessionRequired) {
        QNetworkConfiguration config = manager.defaultConfiguration();
        QNetworkSession *networkSession = new QNetworkSession(config);
        networkSession->open();
    }

    return app.exec();
}
