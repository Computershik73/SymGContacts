#ifndef LOCALSSLPROXY_H
#define LOCALSSLPROXY_H

#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

class LocalSslProxy : public QTcpServer
{
    Q_OBJECT
public:
    explicit LocalSslProxy(QObject *parent = 0);
    ~LocalSslProxy();

    bool startProxy(quint16 port = 8080);

protected:
    void incomingConnection(int socketDescriptor);
};

// Рабочий поток для обработки одного HTTP-запроса
class ProxyWorker : public QThread
{
    Q_OBJECT
public:
    explicit ProxyWorker(int socketDescriptor, QObject *parent = 0);
    void run();

private:
    int m_socketDescriptor;
};

#endif // LOCALSSLPROXY_H
