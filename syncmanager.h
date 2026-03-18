#ifndef SYNCMANAGER_H
#define SYNCMANAGER_H

#include <QObject>
#include <QThread>
#include <QSettings>
#include <QStringList>
#include <QTimer>

// Qt-структура для хранения данных контакта
struct LocalContact {
    long symbianId;
    QString remoteId;
    QString firstName;
    QString lastName;
    QStringList phones;
    QStringList emails; // <-- Добавить это
};

class SyncManager;

class SyncThread : public QThread {
    Q_OBJECT
public:
    SyncThread(SyncManager *parent);
    void run();

    QString clientId;
    QString clientSecret;
    bool doAuthFlow;
    SyncManager *m_parent;

private:
    QString syncWait(class QNetworkReply *reply);
    QString getAccessToken(const QString &refreshToken);
    bool doDeviceAuthFlow();
    void executeSync(const QString &accessToken);

    // Мосты для нативного Symbian API
    void readSymbianContacts(QList<LocalContact> &list);
    void saveSymbianContacts(const QList<LocalContact> &toSave);
    void deleteSymbianContacts(const QList<long> &toDelete);
};

class SyncManager : public QObject
{
    Q_OBJECT
friend class SyncThread;


public:
    explicit SyncManager(QObject *parent = 0);
    ~SyncManager();

    Q_INVOKABLE void startAuthAndSync(const QString &clientId, const QString &clientSecret);

public slots:
    Q_INVOKABLE void startSyncOnly();

signals:
    void progressUpdated(const QString &message);
    void authCodeReceived(const QString &userCode, const QString &verificationUrl);
    void syncFinished(bool success, const QString &message);

private:


    SyncThread *m_thread;
    QTimer *m_bgTimer; // Таймер для фоновой синхронизации
};



#endif // SYNCMANAGER_H
