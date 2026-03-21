#ifndef SYNCMANAGER_H
#define SYNCMANAGER_H

#include <QObject>
#include <QThread>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <QDesktopServices>
#ifdef Q_OS_SYMBIAN
#include <e32std.h> // Базовые типы Symbian (TInt, TDesC и т.д.)
#endif

// "Слепок" состояния одного контакта после последней синхронизации
struct SyncStateEntry {
    QString etag;
    QString hash;
    // Сохраняем списки полей как строки через запятую
    QString phones;
    QString emails;
    QString addresses;
    QString urls;
};


class SyncState {
public:
    QMap<QString, QString> etags;
    QMap<QString, QString> hashes;
};

struct GDate {
    int year;
    int month;
    int day;
    GDate() : year(0), month(0), day(0) {}
    // Вспомогательный метод для проверки, пустая ли дата (если Google прислал только месяц и день)
    bool isEmpty() const {
        return (year == 0 && month == 0 && day == 0);
    }
};

// Qt-структура для хранения данных контакта
struct LocalContact {
    long symbianId;
    QString remoteId;
    QString firstName;
    QString lastName;
    QStringList phones;
    QStringList emails;
    QStringList addresses;
    QStringList urls;
    QString company;
    QString jobTitle;
    QString birthday;
    QString notes;
};

struct GoogleContact {
    QString id;
    QString etag;
    QString firstName;
    QString lastName;
    QStringList phones;
    QStringList emails;
    QStringList addresses;
    QStringList urls;
    QString company;
    QString jobTitle;
    GDate birthday;
    QString notes;
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
    QString CleanPhone(const QString& phone);

    // Мосты для нативного Symbian API
    void readSymbianContacts(QList<LocalContact> &list, class CContactDatabase* aDb);
    void saveSymbianContacts(const QList<LocalContact> &toSave, class CContactDatabase* aDb);
    void deleteSymbianContacts(const QList<long> &toDelete, class CContactDatabase* aDb);
    QString createGoogleContact(const LocalContact &lc, const QString &accessToken);
    QString calculateHashLocal(const LocalContact &lc);
    QString calculateHash(const GoogleContact &gc);
    void applyGoogleDataToLocal(const GoogleContact &gc, LocalContact &lc);
    void saveSyncState(const QMap<QString, QString>& etags, const QMap<QString, QString>& hashes);
    SyncState loadSyncState();
    void fetchGoogleContacts(const QString &accessToken, QList<GoogleContact> &contactsList);
    bool deleteGoogleContact(const QString &accessToken, const QString &resourceName);
    bool updateGoogleContact(const QString &accessToken, const LocalContact &localContact, const QString &etag);
};

class SyncManager : public QObject
{
    Q_OBJECT
    friend class SyncThread;


public:
    explicit SyncManager(QObject *parent = 0);
    ~SyncManager();
    Q_INVOKABLE bool hasToken() {
        QSettings settings(QDesktopServices::storageLocation(QDesktopServices::DataLocation) + "/wpgcontacts.ini", QSettings::IniFormat);
        return !settings.value("refreshToken").toString().isEmpty();
    }
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
