#include "localsslproxy.h"
#include <QUrl>
#include <QStringList>
#include <QDebug>

// Заголовки mbedTLS
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include <QFile>
#include <QDateTime>

LocalSslProxy::LocalSslProxy(QObject *parent) : QTcpServer(parent) {}

LocalSslProxy::~LocalSslProxy() {
    close();
}

void LogToDisk(const QString &message)
{
    /*QFile file("c:/data/proxy_debug.log");
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        out << QDateTime::currentDateTime().toString("hh:mm:ss") << ": " << message << "\n";
        file.close();
    }*/
    qDebug() << "[PROXY] " << message;
}

bool LocalSslProxy::startProxy(quint16 port) {
    return listen(QHostAddress::LocalHost, port);
}

void LocalSslProxy::incomingConnection(int socketDescriptor)
{
    // Вместо запуска ProxyWorker напрямую, мы должны сначала прочитать
    // заголовки и проверить, не пришел ли метод CONNECT
    QTcpSocket client;
    client.setSocketDescriptor(socketDescriptor);

    if (client.waitForReadyRead(5000)) {
        QByteArray req = client.readAll();
        if (req.startsWith("CONNECT")) {
            // Отвечаем "200 Connection Established"
            client.write("HTTP/1.1 200 Connection Established\r\n\r\n");
            client.waitForBytesWritten();

            // Теперь, когда труба установлена, запускаем наш ProxyWorker
            // который будет работать с mbedTLS
            ProxyWorker *worker = new ProxyWorker(socketDescriptor, this);
            worker->start();
        } else {
            // Обычный GET/POST (если запрос всё же пришел как http)
            ProxyWorker *worker = new ProxyWorker(socketDescriptor, this);
            worker->start();
        }
    }
}


// --- РАБОЧИЙ ПОТОК ПРОКСИ ---

ProxyWorker::ProxyWorker(int socketDescriptor, QObject *parent)
    : QThread(parent), m_socketDescriptor(socketDescriptor) {}

void ProxyWorker::run()
{
    QTcpSocket client;
    if (!client.setSocketDescriptor(m_socketDescriptor)) return;

    if (!client.waitForReadyRead(5000)) return;

    QByteArray requestData = client.readAll();

    // ЛОГ 1: Что пришло от Qt
    LogToDisk("--- INCOMING REQUEST FROM APP ---\n" + QString::fromUtf8(requestData) + "\n---------------------------------");

    int headerEnd = requestData.indexOf("\r\n\r\n");
    if (headerEnd == -1) return;

    QByteArray headersPart = requestData.left(headerEnd);
    QByteArray bodyPart = requestData.mid(headerEnd + 4);

    QList<QByteArray> headerLines = headersPart.split('\n');
    if (headerLines.isEmpty()) return;

    // 1. Извлекаем метод, URL и версию
    QList<QByteArray> requestLineParts = headerLines.first().trimmed().split(' ');
    if (requestLineParts.size() < 3) return;

    QByteArray method = requestLineParts[0]; // POST
    QUrl rawUrl(QString::fromUtf8(requestLineParts[1])); // http://oauth2.googleapis.com...
    QByteArray httpVersion = requestLineParts[2]; // HTTP/1.1

    QString host = rawUrl.host();
    if (host.isEmpty()) host = "oauth2.googleapis.com"; // Фолбэк

    // 2. Формируем новую "первую строку" (относительный путь)
    QByteArray pathWithQuery = rawUrl.toEncoded(QUrl::RemoveScheme | QUrl::RemoveAuthority);
    if (pathWithQuery.isEmpty()) pathWithQuery = "/";
    QByteArray newRequestLine = method + " " + pathWithQuery + " " + httpVersion + "\r\n";

    // 3. Формируем остальные заголовки
    QByteArray rewrittenHeaders = newRequestLine;
    for (int i = 1; i < headerLines.size(); ++i) {
        QByteArray line = headerLines[i].trimmed();
        if (line.isEmpty()) continue;
        if (line.toLower().startsWith("proxy-connection:")) continue;
        rewrittenHeaders += line + "\r\n";
    }
    // Обязательно добавляем Host
    rewrittenHeaders += "Host: " + host.toUtf8() + "\r\n";
    // И Content-Length для тела POST
    rewrittenHeaders += "Content-Length: " + QByteArray::number(bodyPart.size()) + "\r\n";
    rewrittenHeaders += "Connection: close\r\n\r\n";

    // 4. Склеиваем всё вместе
    QByteArray toSendToGoogle = rewrittenHeaders + bodyPart;

    // ЛОГ 2: Что уходит в Google
    LogToDisk("--- SENDING TO GOOGLE (via mbedTLS) ---\n" + QString::fromUtf8(toSendToGoogle) + "\n-----------------------------------");

    // 2. ИНИЦИАЛИЗАЦИЯ mbedTLS
    mbedtls_net_context server_fd;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;

    mbedtls_net_init(&server_fd);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"sym_proxy", 9);

    const char* charles_ip = "192.168.1.183"; // IP-адрес компьютера, где запущен Charles
    const char* charles_port = "8890";        // Стандартный порт Charles
    // Подключаемся к Google на 443 порт
    if (mbedtls_net_connect(&server_fd, host.toUtf8().constData(), "443", MBEDTLS_NET_PROTO_TCP) != 0) {
        LogToDisk("ERROR: Connection to Charles Proxy failed.");

        goto cleanup;
    }


    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);

    // Отключаем строгую проверку сертификатов (стандарт для Symbian-патчей)
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    mbedtls_ssl_setup(&ssl, &conf);
    mbedtls_ssl_set_hostname(&ssl, host.toUtf8().constData());
    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    // SSL Handshake
    if (mbedtls_ssl_handshake(&ssl) != 0) {
        LogToDisk("ERROR: TLS Handshake failed.");
        goto cleanup;
    }
    LogToDisk("INFO: SSL Handshake success.");


    // 3. ОТПРАВЛЯЕМ ЗАПРОС В GOOGLE
    {
        int written = 0;
        while (written < toSendToGoogle.length()) {
            int ret = mbedtls_ssl_write(&ssl, (const unsigned char*)toSendToGoogle.constData() + written, toSendToGoogle.length() - written);
            if (ret <= 0) break;
            written += ret;
        }
        LogToDisk(QString("INFO: Bytes written to Google: %1").arg(written));
    }


    // 4. ТРАНСЛИРУЕМ ТРАФИК (Google <-> mbedTLS <-> QTcpSocket)
    unsigned char buf[4096];
    while (client.state() == QAbstractSocket::ConnectedState)
    {
        // Читаем из приложения и шлем в Google (если приложение досылает куски POST-тела)
        /*if (client.waitForReadyRead(10)) {
            QByteArray extra = client.readAll();
            mbedtls_ssl_write(&ssl, (const unsigned char*)extra.constData(), extra.length());
        }*/
        int ret = mbedtls_ssl_read(&ssl, buf, sizeof(buf));
        // Читаем из Google и отдаем приложению
        if (ret > 0) {
            LogToDisk(QString("INFO: Bytes received from Google: %1").arg(ret));
            client.write((const char*)buf, ret);
        } else if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ) {
            LogToDisk("ERROR: SSL Read failed.");
            break;
        }


    }

cleanup:
    // 5. ОЧИСТКА ПАМЯТИ И ЗАКРЫТИЕ СОЕДИНЕНИЙ
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&server_fd);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    client.disconnectFromHost();
}
