#include "syncmanager.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QScriptEngine>
#include <QScriptValue>

// === НАТИВНЫЕ SYMBIAN БИБЛИОТЕКИ ===
#ifdef Q_OS_SYMBIAN
#include <e32base.h>
#include <app/cpbkcontactengine.h>
#include <app/CPbkContactItem.h>
#include <app/CPbkContactIter.h>
#include <app/PbkFields.hrh>

#include <app/PbkFields.hrh>      // Константы полей (EPbkFieldId...)
#include <app/CPbkFieldsInfo.h>   // Описание полей
#include <app/CPbkContactItem.h>  // Сам контакт
#include <QDesktopServices>
#include <QTcpSocket>
#include <QNetworkProxy>
#include "localsslproxy.h"


// Утилиты для безопасной конвертации QString <-> TDesC (Symbian Strings)
static TPtrC16 ToSymbianStr(const QString& str) {
    return TPtrC16(reinterpret_cast<const TUint16*>(str.utf16()), str.length());
}

static QString ToQString(const TDesC& des) {
    return QString::fromUtf16(des.Ptr(), des.Length());
}
#endif
// ===================================

void logRequest(const QNetworkRequest &req, const QByteArray &data)
{
    qDebug() << "=== HTTP REQUEST ===";
    qDebug() << "URL:" << req.url().toString();

    QList<QByteArray> headers = req.rawHeaderList();
    foreach(QByteArray header, headers) {
        qDebug() << "Header:" << header << ":" << req.rawHeader(header);
    }

    if (!data.isEmpty()) {
        qDebug() << "Body:" << QString::fromUtf8(data);
    }
    qDebug() << "====================";
}

SyncManager::SyncManager(QObject *parent) : QObject(parent)
{
    m_thread = new SyncThread(this);

    // Инициализация фонового таймера (каждые 15 минут)
    m_bgTimer = new QTimer(this);
    connect(m_bgTimer, SIGNAL(timeout()), this, SLOT(startSyncOnly()));
    m_bgTimer->start(15 * 60 * 1000);
}

SyncManager::~SyncManager()
{
    m_bgTimer->stop();
    if (m_thread->isRunning()) m_thread->wait();
    delete m_thread;
}

void SyncManager::startAuthAndSync(const QString &clientId, const QString &clientSecret)
{
    if (m_thread->isRunning()) return;
    m_thread->clientId = clientId;
    m_thread->clientSecret = clientSecret;
    m_thread->doAuthFlow = true;
    m_thread->start();
}

void SyncManager::startSyncOnly()
{
    if (m_thread->isRunning()) return;
    QSettings settings(QDesktopServices::storageLocation(QDesktopServices::DataLocation) + "/wpgcontacts.ini", QSettings::IniFormat);
    m_thread->clientId = settings.value("clientId").toString();
    m_thread->clientSecret = settings.value("clientSecret").toString();
    m_thread->doAuthFlow = false;
    m_thread->start();
}

SyncThread::SyncThread(SyncManager *parent) : QThread(parent), m_parent(parent) {

}



QString SyncThread::syncWait(QNetworkReply *reply)
{
    QEventLoop loop;
    connect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
    loop.exec(); // Ждем завершения

    qDebug() << "\n========== ОТВЕТ СЕРВЕРА ==========";

    // 1. Проверяем HTTP статус-код
    QVariant statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    qDebug() << "HTTP Status:" << statusCode.toInt();

    // 2. Проверяем, не было ли редиректа (Qt 4 сам не переходит по редиректам!)
    QVariant redirectUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
    if (redirectUrl.isValid()) {
        qDebug() << "ВНИМАНИЕ РЕДИРЕКТ НА:" << redirectUrl.toUrl().toString();
    }

    // 3. Сетевые ошибки Qt
    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "Qt Network Error Code:" << reply->error();
        qDebug() << "Qt Network Error String:" << reply->errorString();
    }

    // 4. Читаем СЫРЫЕ байты
    QByteArray rawData = reply->readAll();
    qDebug() << "Bytes Read:" << rawData.length();

    // Выводим сам ответ (даже если это ошибка 400/401, тело всё равно будет!)
    QString response = QString::fromUtf8(rawData);
    qDebug() << "Response Body:" << response;

    qDebug() << "=====================================\n";

    reply->deleteLater();
    return response;
}

void SyncThread::run()
{
    QSettings settings(QDesktopServices::storageLocation(QDesktopServices::DataLocation) + "/wpgcontacts.ini", QSettings::IniFormat);
    if (doAuthFlow) {
        if (!doDeviceAuthFlow()) {
            emit m_parent->syncFinished(false, "Ошибка авторизации.");
            return;
        }
    }
    LocalSslProxy localProxy;
    localProxy.listen(QHostAddress::LocalHost, 8080);
    QString refreshToken = settings.value("refreshToken").toString();
    if (refreshToken.isEmpty()) {
        emit m_parent->syncFinished(false, "Нет токена. Требуется авторизация.");
        return;
    }

    emit m_parent->progressUpdated("Получение токена доступа...");
    QString accessToken = getAccessToken(refreshToken);
    if (accessToken.isEmpty()) {
        emit m_parent->syncFinished(false, "Не удалось обновить токен Google.");
        return;
    }

    // Запускаем основную логику
    executeSync(accessToken);
    localProxy.close();
}

void SyncThread::executeSync(const QString &accessToken)
{
    emit m_parent->progressUpdated("Чтение Google Контактов...");

    QNetworkAccessManager net;

    QNetworkProxy proxy;
    proxy.setType(QNetworkProxy::HttpProxy);
    proxy.setHostName("127.0.0.1");
    proxy.setPort(8080);
    proxy.setCapabilities(QNetworkProxy::TunnelingCapability);

    net.setProxy(proxy); // СТРОГО навязываем прокси

    qDebug() << "Proxy set to:" << net.proxy().hostName() << ":" << net.proxy().port();

    QNetworkRequest req(QUrl("https://people.googleapis.com/v1/people/me/connections?personFields=names,phoneNumbers&pageSize=1000"));
    req.setRawHeader("Authorization", QString("Bearer " + accessToken).toUtf8());
    req.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, false);
    QTcpSocket testSocket;
    testSocket.connectToHost("127.0.0.1", 8080);
    if (testSocket.waitForConnected(3000)) {
        qDebug() << "DEBUG: Соединение с локальным прокси УСПЕШНО!";
        testSocket.disconnectFromHost();
    } else {
        qDebug() << "DEBUG: Соединение с локальным прокси ПРОВАЛИЛОСЬ!";
    }

    QNetworkReply *reply = net.get(req);


    QString googleJsonStr = syncWait(reply);
    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "!!! ОШИБКА СЕТИ !!!";
        qDebug() << "Error Code:" << reply->error();
        qDebug() << "Error String:" << reply->errorString();

        // Получаем тело ошибки от Google (если есть)
        QByteArray errorData = reply->readAll();
        qDebug() << "Google Error Data:" << QString::fromUtf8(errorData);
    } else {
        qDebug() << "--- УСПЕШНО ---";
        qDebug() << "Data length:" << googleJsonStr.length();
        // Логируем первые 500 символов, чтобы не забить память Symbian
        qDebug() << "First 500 chars:" << googleJsonStr.left(500);
    }


    QScriptEngine engine;
    QScriptValue root = engine.evaluate("(" + googleJsonStr + ")");
    QScriptValue connections = root.property("connections");

    emit m_parent->progressUpdated("Чтение контактов Symbian...");

    // ЧИТАЕМ СИСТЕМНУЮ КНИГУ ЧЕРЕЗ НАТИВНЫЙ API
    QList<LocalContact> symbianContacts;
    readSymbianContacts(symbianContacts);

    QList<LocalContact> toSave;
    QList<long> toDelete;

    if (connections.isArray()) {
        int length = connections.property("length").toInt32();

        for (int i = 0; i < length; ++i) {
            QScriptValue person = connections.property(i);
            QString remoteId = person.property("resourceName").toString();

            QString firstName, lastName, phoneVal;
            QScriptValue names = person.property("names");
            if (names.isArray() && names.property("length").toInt32() > 0) {
                firstName = names.property(0).property("givenName").toString();
                lastName = names.property(0).property("familyName").toString();
            }

            QScriptValue phones = person.property("phoneNumbers");
            if (phones.isArray() && phones.property("length").toInt32() > 0) {
                phoneVal = phones.property(0).property("value").toString();
            }

            LocalContact match;
            match.symbianId = 0; // 0 означает, что это новый контакт

            // Ищем контакт по ID
            for (int j = 0; j < symbianContacts.size(); ++j) {
                if (symbianContacts[j].remoteId == remoteId) {
                    match = symbianContacts[j];
                    break;
                }
            }

            // Заполняем данные для сохранения
            match.remoteId = remoteId;
            match.firstName = firstName;
            match.lastName = lastName;
            match.phones.clear();
            if (!phoneVal.isEmpty()) match.phones.append(phoneVal);

            toSave.append(match);
        }
    }

    emit m_parent->progressUpdated("Запись в телефонную книгу...");

    // ВЫПОЛНЯЕМ ЗАПИСЬ В БАЗУ ДАННЫХ SYMBIAN
    saveSymbianContacts(toSave);
    deleteSymbianContacts(toDelete);

    emit m_parent->syncFinished(true, "Синхронизация в фоне завершена успешно!");
}



QString SyncThread::getAccessToken(const QString &refreshToken)
{
    QNetworkAccessManager net;
    QNetworkRequest req(QUrl("https://oauth2.googleapis.com/token"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    //req.setRawHeader("Accept-Encoding", "identity");
   // QString body = QString("client_id=%1&client_secret=%2&refresh_token=%3&grant_type=refresh_token")
    //        .arg(clientId).arg(clientSecret).arg(refreshToken);

    QList<QPair<QString, QString> > queryItems;
    QByteArray data;

    queryItems.append(qMakePair(QString("client_id"), clientId));
    queryItems.append(qMakePair(QString("client_secret"), clientSecret));
    queryItems.append(qMakePair(QString("refresh_token"), refreshToken));
    queryItems.append(qMakePair(QString("grant_type"), QString("refresh_token")));

    QUrl tempUrl;
    tempUrl.setQueryItems(queryItems);
    data = tempUrl.encodedQuery();


    QNetworkReply *reply = net.post(req, data);
    QString jsonStr = syncWait(reply);

    QScriptEngine engine;
    QScriptValue json = engine.evaluate("(" + jsonStr + ")");
    return json.property("access_token").toString();
}

bool SyncThread::doDeviceAuthFlow()
{
    QNetworkAccessManager net;
    QNetworkRequest req(QUrl("https://oauth2.googleapis.com/device/code"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    // req.setRawHeader("Accept-Encoding", "identity");
    QString body = QString("client_id=%1&scope=https://www.googleapis.com/auth/contacts").arg(clientId);

    QList<QPair<QString, QString> > queryItems;
    QByteArray data;

    queryItems.append(qMakePair(QString("client_id"), clientId));
    queryItems.append(qMakePair(QString("scope"), QString("https://www.googleapis.com/auth/contacts")));


    QUrl tempUrl;
    tempUrl.setQueryItems(queryItems);
    data = tempUrl.encodedQuery();


    logRequest(req, data);

    QNetworkReply *reply = net.post(req, data);

    QString jsonStr = syncWait(reply);
    QScriptEngine engine;
    QScriptValue json = engine.evaluate("(" + jsonStr + ")");

    QString deviceCode = json.property("device_code").toString();
    QString userCode = json.property("user_code").toString();
    QString verUrl = json.property("verification_url").toString();

    if (deviceCode.isEmpty()) return false;

    // Передаем код в UI для отображения пользователю
    emit m_parent->authCodeReceived(userCode, verUrl);
    emit m_parent->progressUpdated("Ожидание авторизации в браузере...");

    // Поллинг токена (каждые 5 сек)
    bool waiting = true;
    while (waiting) {
        QThread::sleep(5); // Спим 5 секунд

        QNetworkRequest reqToken(QUrl("https://oauth2.googleapis.com/token"));
        reqToken.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
        QString bodyToken = QString("client_id=%1&client_secret=%2&device_code=%3&grant_type=urn:ietf:params:oauth:grant-type:device_code")
                .arg(clientId).arg(clientSecret).arg(deviceCode);

        QNetworkReply *replyToken = net.post(reqToken, bodyToken.toUtf8());
        QString tokenJsonStr = syncWait(replyToken);
        QScriptValue tokenJson = engine.evaluate("(" + tokenJsonStr + ")");

        if (tokenJson.property("refresh_token").isValid()) {
            QSettings settings(QDesktopServices::storageLocation(QDesktopServices::DataLocation) + "/wpgcontacts.ini", QSettings::IniFormat);
            //QSettings settings("Computershik", "WPGContacts");
            settings.setValue("clientId", clientId);
            settings.setValue("clientSecret", clientSecret);
            settings.setValue("refreshToken", tokenJson.property("refresh_token").toString());
            settings.sync();
            return true;
        } else {
            QString error = tokenJson.property("error").toString();
            if (error != "authorization_pending" && error != "slow_down") {
                return false; // Истекло время или другая ошибка
            }
        }
    }
    return false;
}

// =======================================================================
// ИЗОЛИРОВАННЫЕ ВЫЗОВЫ К NATIVE SYMBIAN API (ОБЕРНУТЫ В TRAPD)
// =======================================================================

void SyncThread::readSymbianContacts(QList<LocalContact> &list)
{
#ifdef Q_OS_SYMBIAN
    TRAPD(err, {
          CPbkContactEngine* db = CPbkContactEngine::NewL();
            CleanupStack::PushL(db);

    CPbkContactIter* iter = CPbkContactIter::NewL(*db);
    CleanupStack::PushL(iter);

    TContactItemId id;
    while ((id = iter->NextL()) != KNullContactId) {
        CPbkContactItem* item = db->ReadContactLC(id);
        LocalContact lc;
        lc.symbianId = (long)id;

        // Читаем Имя
        TPbkContactItemField* fnField = item->FindField(EPbkFieldIdFirstName);
        if (fnField) lc.firstName = ToQString(fnField->TextStorage()->Text());

        // Читаем Фамилию
        TPbkContactItemField* lnField = item->FindField(EPbkFieldIdLastName);
        if (lnField) lc.lastName = ToQString(lnField->TextStorage()->Text());

        // Читаем Телефоны (собираем все номера)
        for (int i = 0; i < item->CardFields().Count(); i++) {
            // ИСПРАВЛЕНИЕ: Используем оператор[] вместо FieldAt(i)
            TPbkContactItemField& field = item->CardFields()[i];

            if (field.FieldInfo().FieldId() == EPbkFieldIdPhoneNumberGeneral  ||
                    field.FieldInfo().FieldId() == EPbkFieldIdPhoneNumberGeneral) {
                lc.phones.append(ToQString(field.TextStorage()->Text()));
            }
            // Читаем Email
            else if (field.FieldInfo().FieldId() == EPbkFieldIdEmailAddress) {
                lc.emails.append(ToQString(field.TextStorage()->Text()));
            }
        }

        // Читаем наш спрятанный RemoteId из Заметок
        TPbkContactItemField* noteField = item->FindField(EPbkFieldIdNote);
        if (noteField) {
            QString noteText = ToQString(noteField->TextStorage()->Text());
            int start = noteText.indexOf("[GID:");
            if (start != -1) {
                int end = noteText.indexOf("]", start);
                if (end != -1) lc.remoteId = noteText.mid(start + 5, end - start - 5);
            }
        }

        list.append(lc);
        CleanupStack::PopAndDestroy(item); // Удаляем контакт из оперативки
    }
    CleanupStack::PopAndDestroy(iter);
    CleanupStack::PopAndDestroy(db);
});

if (err != KErrNone) {
    qDebug("Ошибка чтения базы Symbian: %d", err);
}
#endif
}

void SyncThread::saveSymbianContacts(const QList<LocalContact> &toSave)
{
#ifdef Q_OS_SYMBIAN
    TRAPD(err, {
          CPbkContactEngine* db = CPbkContactEngine::NewL();
            CleanupStack::PushL(db);

    const CPbkFieldsInfo& fieldsInfo = db->FieldsInfo();

    for (int i = 0; i < toSave.size(); ++i) {
        const LocalContact &lc = toSave[i];
        CPbkContactItem* item = NULL;
        bool isNew = (lc.symbianId == 0);

        if (isNew) {
            item = db->CreateEmptyContactL();
            CleanupStack::PushL(item);
        } else {
            item = db->ReadContactLC((TContactItemId)lc.symbianId);
        }

        // === ИМЯ ===
        TPbkContactItemField* fnField = item->FindField(EPbkFieldIdFirstName);
        if (!fnField) {
            // ИСПРАВЛЕНИЕ: Используем Find() вместо FieldInfo()
            CPbkFieldInfo* info = fieldsInfo.Find(EPbkFieldIdFirstName);
            if (info) fnField = &(item->AddFieldL(*info)); // AddFieldL сразу возвращает созданное поле
        }
        if (fnField) fnField->TextStorage()->SetTextL(ToSymbianStr(lc.firstName));

        // === ФАМИЛИЯ ===
        TPbkContactItemField* lnField = item->FindField(EPbkFieldIdLastName);
        if (!lnField) {
            CPbkFieldInfo* info = fieldsInfo.Find(EPbkFieldIdLastName);
            if (info) lnField = &(item->AddFieldL(*info));
        }
        if (lnField) lnField->TextStorage()->SetTextL(ToSymbianStr(lc.lastName));

        // === ТЕЛЕФОН ===
        if (!lc.phones.isEmpty()) {
            TPbkContactItemField* phField = item->FindField(EPbkFieldIdPhoneNumberGeneral);
            if (!phField) {
                CPbkFieldInfo* info = fieldsInfo.Find(EPbkFieldIdPhoneNumberGeneral);
                if (info) phField = &(item->AddFieldL(*info));
            }
            if (phField) phField->TextStorage()->SetTextL(ToSymbianStr(lc.phones.first()));
        }

        // === EMAIL ===
        if (!lc.emails.isEmpty()) {
            TPbkContactItemField* emField = item->FindField(EPbkFieldIdEmailAddress);
            if (!emField) {
                CPbkFieldInfo* info = fieldsInfo.Find(EPbkFieldIdEmailAddress);
                if (info) emField = &(item->AddFieldL(*info));
            }
            if (emField) emField->TextStorage()->SetTextL(ToSymbianStr(lc.emails.first()));
        }

        // === ЗАМЕТКИ (Скрытый Google ID) ===
        QString noteStr = QString("[GID:%1]").arg(lc.remoteId);
        TPbkContactItemField* noteField = item->FindField(EPbkFieldIdNote);
        if (!noteField) {
            CPbkFieldInfo* info = fieldsInfo.Find(EPbkFieldIdNote);
            if (info) noteField = &(item->AddFieldL(*info));
        }
        if (noteField) noteField->TextStorage()->SetTextL(ToSymbianStr(noteStr));

        // Сохраняем в базу Symbian
        if (isNew) db->AddNewContactL(*item);
        else db->CommitContactL(*item);

        CleanupStack::PopAndDestroy(item);
    }
    CleanupStack::PopAndDestroy(db);
});

if (err != KErrNone) {
    qDebug("Ошибка сохранения в базу Symbian: %d", err);
}
#endif
}

void SyncThread::deleteSymbianContacts(const QList<long> &toDelete)
{
#ifdef Q_OS_SYMBIAN
    TRAPD(err, {
          CPbkContactEngine* db = CPbkContactEngine::NewL();
            CleanupStack::PushL(db);

    for (int i = 0; i < toDelete.size(); ++i) {
        // Удаляем контакт по его Symbian ID
        TRAPD(delErr, db->DeleteContactL((TContactItemId)toDelete[i]));
        if (delErr != KErrNone && delErr != KErrNotFound) {
            // Игнорируем KErrNotFound (если контакт уже был удален пользователем)
        }
    }

    CleanupStack::PopAndDestroy(db);
});
#endif
}

