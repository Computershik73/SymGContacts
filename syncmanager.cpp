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
#include <QCryptographicHash>
#include <QFile>
#include <QBuffer>
#include <QDir>

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
    QDir().mkpath(QDesktopServices::storageLocation(QDesktopServices::DataLocation));
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

QString SyncThread::calculateHashLocal(const LocalContact &lc)
{
    QString raw = QString("%1|%2|%3|%4|%5")
            .arg(lc.firstName.trimmed().toLower())
            .arg(lc.lastName.trimmed().toLower())
            .arg(lc.phones.join(","))
            .arg(lc.emails.join(","))
            .arg(lc.remoteId); // Включаем remoteId в хэш, чтобы привязка была жесткой

    QByteArray hash = QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(hash.toHex());
}

QString SyncThread::calculateHash(const GoogleContact &gc)
{
    QString raw = QString("%1|%2|%3|%4|%5")
            .arg(gc.firstName.trimmed().toLower())
            .arg(gc.lastName.trimmed().toLower())
            .arg(gc.phones.join(","))
            .arg(gc.emails.join(","))
            .arg(gc.id);

    QByteArray hash = QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(hash.toHex());
}

void SyncThread::applyGoogleDataToLocal(const GoogleContact &gc, LocalContact &lc)
{
    lc.firstName = gc.firstName;
    lc.lastName = gc.lastName;

    lc.phones.clear();
    lc.phones = gc.phones; // Теперь телефоны хранятся списком

    lc.emails.clear();
    lc.emails = gc.emails;

    // RemoteId должен быть уже задан в lc перед вызовом
}

void SyncThread::saveSyncState(const QMap<QString, QString>& etags, const QMap<QString, QString>& hashes)
{
    // Используем прямой путь к папке данных приложения
    QString path = QDesktopServices::storageLocation(QDesktopServices::DataLocation) + "/sync_state_v2.txt";
    QFile file(path);

    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);

        out << "[ETAGS]\n";
        QMapIterator<QString, QString> itE(etags);
        while (itE.hasNext()) {
            itE.next();
            out << itE.key() << "=" << itE.value() << "\n";
        }

        out << "[HASHES]\n";
        QMapIterator<QString, QString> itH(hashes);
        while (itH.hasNext()) {
            itH.next();
            out << itH.key() << "=" << itH.value() << "\n";
        }

        file.close();
    }
}

SyncState SyncThread::loadSyncState()
{
    SyncState state;
    QString path = QDesktopServices::storageLocation(QDesktopServices::DataLocation) + "/sync_state_v2.txt";
    QFile file(path);

    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        QString currentSection = "";

        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line == "[ETAGS]") {
                currentSection = "etags";
            } else if (line == "[HASHES]") {
                currentSection = "hashes";
            } else if (line.contains("=")) {
                QStringList parts = line.split("=");
                if (parts.size() == 2) {
                    if (currentSection == "etags") state.etags[parts[0]] = parts[1];
                    else if (currentSection == "hashes") state.hashes[parts[0]] = parts[1];
                }
            }
        }
        file.close();
    }
    return state;
}

void SyncThread::fetchGoogleContacts(const QString &accessToken, QList<GoogleContact> &contactsList)
{
    QNetworkAccessManager net;
    QString nextPageToken = "";
    bool hasMorePages = true;

    while (hasMorePages) {
        // Запрашиваем поля: имена, телефоны, email, адреса, URL, дни рождения, организации, заметки
        QString requestUri = "https://people.googleapis.com/v1/people/me/connections?"
                "personFields=names,phoneNumbers,emailAddresses,addresses,urls,birthdays,organizations,biographies&pageSize=1000";

        if (!nextPageToken.isEmpty()) {
            requestUri += "&pageToken=" + nextPageToken;
        }
        QUrl requestUrl(requestUri);
        QNetworkRequest req(requestUrl);
        req.setRawHeader("Authorization", QString("Bearer " + accessToken).toUtf8());

        QNetworkReply *reply = net.get(req);
        QString jsonStr = syncWait(reply);

        if (reply->error() == QNetworkReply::NoError) {
            QScriptEngine engine;
            QScriptValue root = engine.evaluate("(" + jsonStr + ")");

            if (root.property("connections").isArray()) {
                QScriptValue connections = root.property("connections");
                int len = connections.property("length").toInt32();

                for (int i = 0; i < len; ++i) {
                    QScriptValue person = connections.property(i);
                    GoogleContact gc;

                    gc.id = person.property("resourceName").toString();
                    gc.etag = person.property("etag").toString();

                    // 1. Имена
                    QScriptValue names = person.property("names");
                    if (names.isArray() && names.property("length").toInt32() > 0) {
                        gc.firstName = names.property(0).property("givenName").toString();
                        gc.lastName = names.property(0).property("familyName").toString();
                    }

                    // 2. Телефоны
                    QScriptValue phones = person.property("phoneNumbers");
                    if (phones.isArray()) {
                        int count = phones.property("length").toInt32();
                        for (int j = 0; j < count; ++j)
                            gc.phones.append(phones.property(j).property("value").toString());
                    }

                    // 3. Emails
                    QScriptValue emails = person.property("emailAddresses");
                    if (emails.isArray()) {
                        int count = emails.property("length").toInt32();
                        for (int j = 0; j < count; ++j)
                            gc.emails.append(emails.property(j).property("value").toString());
                    }

                    // 4. Адреса
                    QScriptValue addresses = person.property("addresses");
                    if (addresses.isArray()) {
                        int count = addresses.property("length").toInt32();
                        for (int j = 0; j < count; ++j)
                            gc.addresses.append(addresses.property(j).property("formattedValue").toString());
                    }

                    // 5. URL / Соцсети
                    QScriptValue urls = person.property("urls");
                    if (urls.isArray()) {
                        int count = urls.property("length").toInt32();
                        for (int j = 0; j < count; ++j)
                            gc.urls.append(urls.property(j).property("value").toString());
                    }

                    // 6. Организации (Компания и Должность)
                    QScriptValue orgs = person.property("organizations");
                    if (orgs.isArray() && orgs.property("length").toInt32() > 0) {
                        QScriptValue org = orgs.property(0);
                        gc.company = org.property("name").toString();
                        gc.jobTitle = org.property("title").toString();
                    }

                    // 7. День рождения
                    QScriptValue bdays = person.property("birthdays");
                    if (bdays.isArray() && bdays.property("length").toInt32() > 0) {
                        QScriptValue dateObj = bdays.property(0).property("date");
                        if (dateObj.isObject()) {
                            if (dateObj.property("year").isValid())
                                gc.birthday.year = dateObj.property("year").toInt32();

                            if (dateObj.property("month").isValid())
                                gc.birthday.month = (uint)dateObj.property("month").toInt32();

                            if (dateObj.property("day").isValid())
                                gc.birthday.day = (uint)dateObj.property("day").toInt32();
                        }
                    }

                    // 8. Заметки (biographies)
                    QScriptValue bios = person.property("biographies");
                    if (bios.isArray() && bios.property("length").toInt32() > 0) {
                        gc.notes = bios.property(0).property("value").toString();
                    }

                    // Сохраняем в список, если есть хоть что-то
                    if (!gc.firstName.isEmpty() || !gc.lastName.isEmpty() || !gc.phones.isEmpty() || !gc.emails.isEmpty()) {
                        contactsList.append(gc);
                    }
                }
            }

            // Проверка пагинации
            if (root.property("nextPageToken").isValid()) {
                nextPageToken = root.property("nextPageToken").toString();
            } else {
                hasMorePages = false;
            }
        } else {
            hasMorePages = false; // Ошибка сети, прерываем цикл
        }
    }
}

bool SyncThread::updateGoogleContact(const QString &accessToken, const LocalContact &localContact, const QString &etag)
{
    // 1. Формируем JSON (аналогично BuildGoogleContactJson)
    // Используем упрощенный подход (строки), т.к. Qt 4 не имеет удобного QJsonObject
    QString json = "{\"etag\": \"" + etag + "\",";

    // 1. Создаем копию строки
    QString safeFirstName = localContact.firstName;
    safeFirstName.replace("\"", "\\\"");

    QString safeLastName = localContact.lastName;
    safeLastName.replace("\"", "\\\"");

    // 2. Используем копии
    json += "\"givenName\": \"" + safeFirstName + "\", \"familyName\": \"" + safeLastName + "\"}],";

    if (!localContact.phones.isEmpty()) {
        json += "\"phoneNumbers\": [";
        for (int i = 0; i < localContact.phones.size(); ++i) {
            QString p = localContact.phones[i]; // Создаем временную копию
            p.replace("\"", "\\\"");            // Меняем её
            json += "{\"value\": \"" + p + "\"}"; // Используем
            if (i < localContact.phones.size() - 1) json += ",";
        }
        json += "],";
    }

    // Закрываем JSON
    if (json.endsWith(",")) json.chop(1);
    json += "}";

    // 2. Отправляем PATCH
    QNetworkAccessManager net;
    QString resourceId = localContact.remoteId;
    if (!resourceId.startsWith("people/")) resourceId = "people/" + resourceId;

    // ВАЖНО: маска полей для Google
    QString url = "https://people.googleapis.com/v1/" + resourceId + ":updateContact?updatePersonFields=names,phoneNumbers";
    QUrl qurll(url);
    QNetworkRequest req(qurll);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", QString("Bearer " + accessToken).toUtf8());
    // Если нужно строгое соответствие версии:
    req.setRawHeader("If-Match", QString("\"" + etag + "\"").toUtf8());
    QByteArray data = json.toUtf8();
    QBuffer *buffer = new QBuffer();
    buffer->setData(data);
    buffer->open(QIODevice::ReadOnly);
    QNetworkReply *reply = net.sendCustomRequest(req, "PATCH", buffer);
    QString response = syncWait(reply);

    if (reply->error() == QNetworkReply::NoError) {
        qDebug() << "[OK] Обновлено в Google:" << localContact.firstName;
        return true;
    } else {
        qDebug() << "[ERROR] Google PATCH:" << response;
        return false;
    }
}

bool SyncThread::deleteGoogleContact(const QString &accessToken, const QString &resourceName)
{
    QNetworkAccessManager net;

    // ВАЖНО: resourceName уже приходит в формате "people/c1234..."
    // URL должен быть полным
    QString url = "https://people.googleapis.com/v1/" + resourceName + ":deleteContact";
    QUrl qurll(url);
    QNetworkRequest req(qurll);

    req.setRawHeader("Authorization", QString("Bearer " + accessToken).toUtf8());

    // В Qt 4 метод DELETE делается через sendCustomRequest
    QNetworkReply *reply = net.sendCustomRequest(req, "DELETE");

    QString response = syncWait(reply);

    if (reply->error() == QNetworkReply::NoError) {
        qDebug() << "[OK] Удалено из Google:" << resourceName;
        return true;
    } else {
        qDebug() << "[ERROR] Удаление из Google:" << reply->errorString() << response;
        return false;
    }
}

void SyncThread::executeSync(const QString &accessToken)
{
    emit m_parent->progressUpdated("Загрузка состояния...");

    // 1. ЗАГРУЖАЕМ "ПАМЯТЬ" (состояние на момент конца прошлой синхронизации)
    SyncState state;
    TRAPD(err_load, state = loadSyncState()); // TRAPD на случай ошибки чтения файла

    // Готовим словари для сохранения нового состояния
    QMap<QString, QString> newEtags;
    QMap<QString, QString> newHashes;

    // 2. СКАЧИВАЕМ ДАННЫЕ ИЗ GOOGLE
    emit m_parent->progressUpdated("Скачивание контактов из Google...");
    QList<GoogleContact> googleContacts;
    fetchGoogleContacts(accessToken, googleContacts);
    if (googleContacts.isEmpty()) {
        emit m_parent->syncFinished(false, "Не удалось получить контакты из Google.");
        return;
    }

    // Заполняем карту текущих облачных ETag-ов для удобства
    QMap<QString, QString> currentCloudEtags;
    for(int i=0; i<googleContacts.size(); ++i) {
        currentCloudEtags[googleContacts[i].id] = googleContacts[i].etag;
    }

    // 3. ЧИТАЕМ ДАННЫЕ С ТЕЛЕФОНА
    emit m_parent->progressUpdated("Чтение контактов с телефона...");
    QList<LocalContact> existingContacts;
    readSymbianContacts(existingContacts);

    int uploadedCount = 0, downloadedCount = 0, localDeletedCount = 0, cloudDeletedCount = 0;

    // === ФАЗА 1: УДАЛЕНИЕ ИЗ GOOGLE (Если контакт удален локально) ===
    emit m_parent->progressUpdated("Проверка удаленных контактов (1/4)...");
    QMapIterator<QString, QString> it(state.etags);
    while (it.hasNext()) {
        it.next();
        QString remoteId = it.key();
        bool foundLocally = false;
        for (int i = 0; i < existingContacts.size(); ++i) {
            if (existingContacts[i].remoteId == remoteId) {
                foundLocally = true;
                break;
            }
        }

        if (!foundLocally) {
            qDebug() << "[--] Удаление из Google (удален локально): ID" << remoteId;
            if (deleteGoogleContact(accessToken, remoteId)) {
                cloudDeletedCount++;
            }
        }
    }

    // === ФАЗА 2: УДАЛЕНИЕ ИЗ ТЕЛЕФОНА (Если контакт удален в Google) ===
    emit m_parent->progressUpdated("Проверка удаленных контактов (2/4)...");
    QList<long> toDeleteLocally;
    for (int i = 0; i < existingContacts.size(); ++i) {
        if (!existingContacts[i].remoteId.isEmpty() && !currentCloudEtags.contains(existingContacts[i].remoteId)) {
            qDebug() << "[-] Удаление локально (пропал из Google):" << existingContacts[i].firstName;
            toDeleteLocally.append(existingContacts[i].symbianId);
            localDeletedCount++;
        }
    }
    if (!toDeleteLocally.isEmpty()) deleteSymbianContacts(toDeleteLocally);

    // === ФАЗА 3: СИНХРОНИЗАЦИЯ И РАЗРЕШЕНИЕ КОНФЛИКТОВ ===
    for (int i = 0; i < googleContacts.size(); ++i) {
        GoogleContact &gc = googleContacts[i];
        emit m_parent->progressUpdated(QString("Синхронизация... %1/%2").arg(i+1).arg(googleContacts.size()));

        LocalContact* lc = NULL;
        for (int j = 0; j < existingContacts.size(); ++j) {
            if (existingContacts[j].remoteId == gc.id) {
                lc = &existingContacts[j];
                break;
            }
        }

        QString googleHash = calculateHash(gc);

        if (lc) // Контакт есть и там, и там
        {
            QString localHash = calculateHashLocal(*lc);
            QString lastHash = state.hashes.value(gc.id, "");
            QString lastEtag = state.etags.value(gc.id, "");

            bool isFirstSync = lastHash.isEmpty();
            bool cloudChanged = !isFirstSync && gc.etag != lastEtag;
            bool localChanged = !isFirstSync && localHash != lastHash;

            if (isFirstSync || cloudChanged) {
                // Приоритет у Google (или первая синхронизация)
                qDebug() << "[!] Download: Обновление из Google для" << gc.firstName;
                applyGoogleDataToLocal(gc, *lc);
                newHashes[gc.id] = googleHash;
            } else if (localChanged) {
                // Изменился только телефон
                qDebug() << "[^] Upload: Отправка изменений в Google для" << lc->firstName;
                if (updateGoogleContact(accessToken, *lc, gc.etag)) {
                    newHashes[gc.id] = localHash;
                    uploadedCount++;
                } else {
                    newHashes[gc.id] = lastHash; // Оставляем старый, если ошибка
                }
            } else {
                // Ничего не изменилось
                newHashes[gc.id] = localHash;
            }
            newEtags[gc.id] = gc.etag;
        }
        else // Новый контакт из Google, которого нет на телефоне
        {
            qDebug() << "[+] Download: Создание нового контакта" << gc.firstName;
            LocalContact newLc;
            newLc.symbianId = 0;
            newLc.remoteId = gc.id;
            applyGoogleDataToLocal(gc, newLc);

            // Вносим в список для однократной записи в базу
            existingContacts.append(newLc);

            newHashes[gc.id] = googleHash;
            newEtags[gc.id] = gc.etag;
            downloadedCount++;
        }
    }

    // === ФАЗА 4: ВЫГРУЗКА НОВЫХ ЛОКАЛЬНЫХ КОНТАКТОВ ===
    emit m_parent->progressUpdated("Выгрузка новых контактов в Google...");
    QList<LocalContact> toCreateInGoogle;
    for(int i=0; i<existingContacts.size(); ++i) {
        if (existingContacts[i].remoteId.isEmpty()) {
            qDebug() << "[^] Upload: Новый локальный контакт" << existingContacts[i].firstName;
            QString newId = createGoogleContact(existingContacts[i], accessToken);
            if (!newId.isEmpty()) {
                existingContacts[i].remoteId = newId;
                newHashes[newId] = calculateHashLocal(existingContacts[i]);
                uploadedCount++;
            }
        }
    }

    // Финальная запись ВСЕХ изменений в телефонную книгу
    emit m_parent->progressUpdated("Сохранение в телефон...");
    saveSymbianContacts(existingContacts);

    // СОХРАНЯЕМ НОВОЕ СОСТОЯНИЕ ДЛЯ СЛЕДУЮЩЕГО РАЗА
    emit m_parent->progressUpdated("Сохранение состояния...");
    saveSyncState(newEtags, newHashes);

    emit m_parent->syncFinished(true, QString("Готово! Обновлено: %1, Выгружено: %2").arg(downloadedCount).arg(uploadedCount));
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
            emit m_parent->syncFinished(true, "Авторизация успешна!");
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

        for (int i = 0; i < item->CardFields().Count(); i++) {
            TPbkContactItemField& field = item->CardFields()[i];
            TInt fId = field.FieldInfo().FieldId();

            if (fId == EPbkFieldIdFirstName) lc.firstName = ToQString(field.TextStorage()->Text());
            else if (fId == EPbkFieldIdLastName) lc.lastName = ToQString(field.TextStorage()->Text());
            else if (fId == EPbkFieldIdCompanyName) lc.company = ToQString(field.TextStorage()->Text());
            else if (fId == EPbkFieldIdJobTitle) lc.jobTitle = ToQString(field.TextStorage()->Text());
            else if (fId == EPbkFieldIdPostalAddress) lc.addresses.append(ToQString(field.TextStorage()->Text()));
            else if (fId == EPbkFieldIdURL) lc.urls.append(ToQString(field.TextStorage()->Text()));
            else if (fId == EPbkFieldIdEmailAddress) lc.emails.append(ToQString(field.TextStorage()->Text()));
            else if (fId == EPbkFieldIdPhoneNumberGeneral || fId == EPbkFieldIdPhoneNumberMobile ||
                     fId == EPbkFieldIdPhoneNumberHome || fId == EPbkFieldIdPhoneNumberWork) {
                lc.phones.append(ToQString(field.TextStorage()->Text()));
            }
            else if (fId == EPbkFieldIdDate) {
                TTime time = field.DateTimeStorage()->Time();
                TDateTime dt = time.DateTime();
                // Формируем строку YYYY-MM-DD
                lc.birthday = QString("%1-%2-%3")
                        .arg(dt.Year())
                        .arg(dt.Month() + 1, 2, 10, QChar('0'))
                        .arg(dt.Day() + 1, 2, 10, QChar('0'));
            }
            else if (fId == EPbkFieldIdNote) {
                QString noteText = ToQString(field.TextStorage()->Text());
                int start = noteText.indexOf("[GID:");
                if (start != -1) {
                    int end = noteText.indexOf("]", start);
                    if (end != -1) lc.remoteId = noteText.mid(start + 5, end - start - 5);
                } else {
                    lc.notes = noteText;
                }
            }
        }
        list.append(lc);
        CleanupStack::PopAndDestroy(item);
    }
    CleanupStack::PopAndDestroy(2, db); // iter, db
});
#endif
}

QString SyncThread::createGoogleContact(const LocalContact &lc, const QString &accessToken)
{
    // Простая сборка JSON для Qt 4
    QString safeFirstName = lc.firstName;
    QString safeLastName = lc.lastName;

    // Теперь .replace() не вызовет ошибку, так как мы меняем временную переменную, а не константный объект
    safeFirstName.replace("\"", "\\\"");
    safeLastName.replace("\"", "\\\"");

    QString json = "{";
    json += "\"names\":[{\"givenName\": \"" + safeFirstName + "\", \"familyName\": \"" + safeLastName + "\"}],";

    if (!lc.phones.isEmpty()) {
        json += "\"phoneNumbers\":[";
        for (int i=0; i<lc.phones.size(); ++i) {
            QString p = lc.phones[i]; // Временная копия
            p.replace("\"", "\\\"");  // Безопасно меняем копию
            json += "{\"value\": \"" + p + "\"}";
            if (i < lc.phones.size()-1) json += ",";
        }
        json += "],";
    }

    // Убираем последнюю запятую
    if (json.endsWith(",")) json.chop(1);
    json += "}";

    QNetworkAccessManager net;
    QNetworkRequest req(QUrl("https://people.googleapis.com/v1/people:createContact"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", QString("Bearer " + accessToken).toUtf8());
    req.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, false);

    QNetworkReply *reply = net.post(req, json.toUtf8());
    QString response = syncWait(reply);

    QScriptEngine engine;
    QScriptValue root = engine.evaluate("(" + response + ")");
    return root.property("resourceName").toString();
}

void SyncThread::saveSymbianContacts(const QList<LocalContact> &toSave)
{
#ifdef Q_OS_SYMBIAN
    TRAPD(dbErr, {
          CPbkContactEngine* db = CPbkContactEngine::NewL();
            CleanupStack::PushL(db);
    const CPbkFieldsInfo& fieldsInfo = db->FieldsInfo();

    for (int i = 0; i < toSave.size(); ++i) {
        // Внутренний TRAPD: если один контакт сломается (например SIM), остальные сохранятся!
        TRAPD(itemErr, {
              const LocalContact &lc = toSave[i];
                CPbkContactItem* item = NULL;
        bool isNew = (lc.symbianId == 0);

        if (isNew) {
            item = db->CreateEmptyContactL();
        } else {
            item = db->ReadContactLC((TContactItemId)lc.symbianId);
            CleanupStack::Pop(item); // Вынимаем, чтобы вручную положить ниже
        }
        CleanupStack::PushL(item);

        // 1. ОЧИСТКА СТАРЫХ ПОЛЕЙ (кроме имени), ЧТОБЫ ИЗБЕЖАТЬ ДУБЛИКАТОВ
        if (!isNew) {
            for (int k = item->CardFields().Count() - 1; k >= 0; --k) {
                TInt fId = item->CardFields()[k].FieldInfo().FieldId();
                if (fId == EPbkFieldIdPhoneNumberMobile || fId == EPbkFieldIdPhoneNumberGeneral ||
                        fId == EPbkFieldIdEmailAddress || fId == EPbkFieldIdPostalAddress ||
                        fId == EPbkFieldIdURL || fId == EPbkFieldIdCompanyName ||
                        fId == EPbkFieldIdJobTitle || fId == EPbkFieldIdDate || fId == EPbkFieldIdNote) {
                    item->RemoveField(k);
                }
            }
        }

        // 2. ЗАПИСЬ НОВЫХ ДАННЫХ
        // Имя
        TPbkContactItemField* fnField = item->FindField(EPbkFieldIdFirstName);
        if (!fnField) { CPbkFieldInfo* i = fieldsInfo.Find(EPbkFieldIdFirstName); if (i) fnField = &(item->AddFieldL(*i)); }
        if (fnField) fnField->TextStorage()->SetTextL(ToSymbianStr(lc.firstName));

        // Фамилия
        TPbkContactItemField* lnField = item->FindField(EPbkFieldIdLastName);
        if (!lnField) { CPbkFieldInfo* i = fieldsInfo.Find(EPbkFieldIdLastName); if (i) lnField = &(item->AddFieldL(*i)); }
        if (lnField) lnField->TextStorage()->SetTextL(ToSymbianStr(lc.lastName));

        // Массивы (Телефоны, Email, Адреса, URL)
        for (int p = 0; p < lc.phones.size(); ++p) {
            CPbkFieldInfo* i = fieldsInfo.Find(EPbkFieldIdPhoneNumberMobile);
            if (i) item->AddFieldL(*i).TextStorage()->SetTextL(ToSymbianStr(lc.phones[p]));
        }
        for (int e = 0; e < lc.emails.size(); ++e) {
            CPbkFieldInfo* i = fieldsInfo.Find(EPbkFieldIdEmailAddress);
            if (i) item->AddFieldL(*i).TextStorage()->SetTextL(ToSymbianStr(lc.emails[e]));
        }
        for (int a = 0; a < lc.addresses.size(); ++a) {
            CPbkFieldInfo* i = fieldsInfo.Find(EPbkFieldIdPostalAddress);
            if (i) item->AddFieldL(*i).TextStorage()->SetTextL(ToSymbianStr(lc.addresses[a]));
        }
        for (int u = 0; u < lc.urls.size(); ++u) {
            CPbkFieldInfo* i = fieldsInfo.Find(EPbkFieldIdURL);
            if (i) item->AddFieldL(*i).TextStorage()->SetTextL(ToSymbianStr(lc.urls[u]));
        }

        // Организация
        if (!lc.company.isEmpty()) {
            CPbkFieldInfo* i = fieldsInfo.Find(EPbkFieldIdCompanyName);
            if (i) item->AddFieldL(*i).TextStorage()->SetTextL(ToSymbianStr(lc.company));
        }
        if (!lc.jobTitle.isEmpty()) {
            CPbkFieldInfo* i = fieldsInfo.Find(EPbkFieldIdJobTitle);
            if (i) item->AddFieldL(*i).TextStorage()->SetTextL(ToSymbianStr(lc.jobTitle));
        }

        // День рождения
        if (!lc.birthday.isEmpty()) {
            QStringList parts = lc.birthday.split("-");
            if (parts.size() == 3) {
                int year = parts[0].toInt();
                int month = parts[1].toInt();
                int day = parts[2].toInt();

                TDateTime dt(year, (TMonth)(month - 1), day - 1, 0, 0, 0, 0);
                TTime time(dt);

                CPbkFieldInfo* i = fieldsInfo.Find(EPbkFieldIdDate);
                if (i) {
                    item->AddFieldL(*i).DateTimeStorage()->SetTime(time);
                }
            }
        }

        // Заметки (Google ID + Оригинальная заметка)
        QString fullNote = QString("[GID:%1]\n%2").arg(lc.remoteId).arg(lc.notes).trimmed();
        CPbkFieldInfo* iNote = fieldsInfo.Find(EPbkFieldIdNote);
        if (iNote) item->AddFieldL(*iNote).TextStorage()->SetTextL(ToSymbianStr(fullNote));

        // СОХРАНЯЕМ
        if (isNew) db->AddNewContactL(*item);
        else db->CommitContactL(*item);

        CleanupStack::PopAndDestroy(item);
    });

    if (itemErr != KErrNone) {
        qDebug() << "Ошибка сохранения контакта! Symbian Error:" << itemErr;
    }
}
CleanupStack::PopAndDestroy(db);
});
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

