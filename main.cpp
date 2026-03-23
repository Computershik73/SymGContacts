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

    QFile file("c:/data/glog.txt");
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        out << msg << "\n";
    }

    return;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);


    //qInstallMsgHandler(myMessageOutput);

    QTextCodec::setCodecForCStrings(QTextCodec::codecForName("UTF-8"));
    QTextCodec::setCodecForTr(QTextCodec::codecForName("UTF-8"));
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));

    /*QNetworkProxy proxy;
    proxy.setType(QNetworkProxy::HttpProxy); // Или Socks5Proxy
    proxy.setHostName("192.168.1.183");      // IP вашего прокси-сервера
    proxy.setPort(8890);     */                // Порт
    qDebug() << QDesktopServices::storageLocation(QDesktopServices::DataLocation).toUtf8() << "/wpgcontacts.ini";

    // 2. Устанавливаем прокси как глобальный стандарт для всех менеджеров сети
//    QNetworkProxy::setApplicationProxy(proxy);

    // ВАЖНО: Разрешаем приложению работать в фоне, даже если интерфейс закрыт!
    app.setQuitOnLastWindowClosed(false);


   /* LocalSslProxy* proxyServer = new LocalSslProxy(&app);
        quint16 proxyPort = 8080;

        if (!proxyServer->startProxy(proxyPort)) {
            qDebug() << "Could not start local proxy server!";
        } else {
            qDebug() << "Local SSL Proxy started on port" << proxyPort;
        }

        if (!proxyServer->isListening()) {
            qDebug() << "Proxy server is not listening! Starting...";
            proxyServer->startProxy(8080);
        }*/

        // 2. Настраиваем QNetworkProxy ГЛОБАЛЬНО
        // Теперь любой QNetworkAccessManager в приложении будет слать запросы сюда
     /*   QNetworkProxy proxy;
        proxy.setType(QNetworkProxy::HttpProxy);
        proxy.setHostName("127.0.0.1");
        proxy.setPort(proxyPort);
        proxy.setCapabilities(QNetworkProxy::TunnelingCapability);
        QNetworkProxy::setApplicationProxy(proxy);*/

    SyncManager syncManager;

    // Проверяем, запущен ли процесс системой при старте телефона
    bool isAutostart = false;
    for (int i = 0; i < argc; ++i) {
        if (QString(argv[i]) == "-background") {
            isAutostart = true;
        }
    }


    QmlApplicationViewer view;
    // Создаем интерфейс только если приложение запустил пользователь вручную
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
