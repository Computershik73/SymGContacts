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
#include <cntdef.h>    // Для TContactItemAttr и KContactFixed
#include <app/cntitem.h>   // Для CContactItem
#include <e32std.h>
#include <sys/time.h>

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
    // LocalSslProxy localProxy;
    // localProxy.listen(QHostAddress::LocalHost, 8080);
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
    //  localProxy.close();
}

QString SyncThread::calculateHashLocal(const LocalContact &lc)
{
    // Очищаем телефоны и email
    QStringList phones = lc.phones;
    for(int i=0; i < phones.size(); ++i) phones[i] = CleanPhone(phones[i]);
    phones.sort();

    QStringList emails = lc.emails;
    for(int i=0; i < emails.size(); ++i) emails[i] = emails[i].trimmed().toLower();
    emails.sort();

    // Хэшируем только контент
    QString raw = QString("%1|%2|%3|%4|%5|%6")
            .arg(lc.firstName.trimmed().toLower())
            .arg(lc.lastName.trimmed().toLower())
            .arg(phones.join(","))
            .arg(emails.join(","))
            .arg(lc.company.trimmed().toLower())
            .arg(lc.jobTitle.trimmed().toLower());

    // ВАЖНЫЙ ДЕБАГ: выведите это в консоль
    qDebug() << "[HASH-LOCAL] raw:" << raw;

    QByteArray hash = QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(hash.toHex());
}

QString SyncThread::calculateHash(const GoogleContact &gc)
{
    QStringList phones = gc.phones;
    for(int i=0; i < phones.size(); ++i) phones[i] = CleanPhone(phones[i]);
    phones.sort();

    QStringList emails = gc.emails;
    emails.sort();

    QString raw = QString("%1|%2|%3|%4|%5|%6")
            .arg(gc.firstName.trimmed().toLower())
            .arg(gc.lastName.trimmed().toLower())
            .arg(phones.join(","))
            .arg(emails.join(","))
            .arg(gc.company.trimmed().toLower())
            .arg(gc.jobTitle.trimmed().toLower());

    QByteArray hash = QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(hash.toHex());
}

QString SyncThread::CleanPhone(const QString& phone)
{
    QString cleaned = phone;
    // Удаляем всё, кроме цифр и плюса
    cleaned.replace(" ", "").replace("-", "").replace("-", "").replace("-", "").replace("-", "").replace("-", "").replace("(", "").replace(")", "").replace("+", "").replace(" ", "").replace(" ", "");
    return cleaned;
}

void SyncThread::applyGoogleDataToLocal(const GoogleContact &gc, LocalContact &lc)
{
    lc.firstName = gc.firstName;
    lc.lastName = gc.lastName;

    lc.phones.clear();
    lc.phones = gc.phones;

    lc.emails.clear();
    lc.emails = gc.emails;

    lc.addresses.clear();
    lc.addresses = gc.addresses;

    lc.urls.clear();
    lc.urls = gc.urls;

    lc.company = gc.company;
    lc.jobTitle = gc.jobTitle;
    lc.notes = gc.notes;

    if (gc.birthday.year != 0 || gc.birthday.month != 0 || gc.birthday.day != 0) {
        lc.birthday = QString("%1-%2-%3")
                .arg(gc.birthday.year)
                .arg(gc.birthday.month, 2, 10, QChar('0'))
                .arg(gc.birthday.day, 2, 10, QChar('0'));
    } else {
        lc.birthday = "";
    }

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

// =======================================================================
// УМНЫЕ ХЕЛПЕРЫ ДЛЯ РЕДАКТИРОВАНИЯ ПОЛЕЙ "ПО МЕСТУ"
// =======================================================================

void SyncThread::SmartSetSingleFieldL(CPbkContactItem* aItem, TInt aFieldId, const QString& aValue, const CPbkFieldsInfo& aFieldsInfo)
{
    TPbkContactItemField* field = aItem->FindField(aFieldId);

    if (aValue.isEmpty()) {
        // Если из Google пришла пустота, а поле есть - удаляем его
        if (field) {
            for (int k = 0; k < aItem->CardFields().Count(); ++k) {
                if (aItem->CardFields()[k].FieldInfo().FieldId() == aFieldId) {
                    aItem->RemoveField(k);
                    break;
                }
            }
        }
    } else {
        // Обновляем текст, если поле есть, или создаем новое
        if (field) {
            field->TextStorage()->SetTextL(ToSymbianStr(aValue));
        } else {
            CPbkFieldInfo* info = aFieldsInfo.Find(aFieldId);
            if (info) aItem->AddFieldL(*info).TextStorage()->SetTextL(ToSymbianStr(aValue));
        }
    }
}

void SyncThread::SmartSetMultiFieldL(CPbkContactItem* aItem, TInt aFieldId, const QStringList& aValues, const CPbkFieldsInfo& aFieldsInfo)
{
    // 1. Собираем индексы всех существующих полей данного типа в контакте
    QList<int> existingIndices;
    for (int k = 0; k < aItem->CardFields().Count(); ++k) {
        if (aItem->CardFields()[k].FieldInfo().FieldId() == aFieldId) {
            existingIndices.append(k);
        }
    }

    int googleCount = aValues.size();
    int symbianCount = existingIndices.size();

    // 2. ОБНОВЛЯЕМ существующие пересекающиеся поля
    int minCount = qMin(googleCount, symbianCount);
    for (int i = 0; i < minCount; ++i) {
        aItem->CardFields()[existingIndices[i]].TextStorage()->SetTextL(ToSymbianStr(aValues[i]));
    }

    // 3. ДОБАВЛЯЕМ новые поля, если в Google их больше
    if (googleCount > symbianCount) {
        CPbkFieldInfo* info = aFieldsInfo.Find(aFieldId);
        if (info) {
            for (int i = symbianCount; i < googleCount; ++i) {
                if (!aValues[i].isEmpty()) {
                    aItem->AddFieldL(*info).TextStorage()->SetTextL(ToSymbianStr(aValues[i]));
                }
            }
        }
    }

    // 4. УДАЛЯЕМ лишние поля с конца, если в Google их стало меньше
    // ВАЖНО: Удалять элементы из массива нужно строго с конца (с максимального индекса),
    // чтобы индексы остальных элементов не "съехали".
    if (symbianCount > googleCount) {
        for (int i = symbianCount - 1; i >= googleCount; --i) {
            aItem->RemoveField(existingIndices[i]);
        }
    }
}

void SyncThread::SmartSetDateFieldL(CPbkContactItem* aItem, TInt aFieldId, const QString& aDateStr, const CPbkFieldsInfo& aFieldsInfo)
{
    TPbkContactItemField* field = aItem->FindField(aFieldId);

    if (aDateStr.isEmpty()) {
        if (field) {
            for (int k = 0; k < aItem->CardFields().Count(); ++k) {
                if (aItem->CardFields()[k].FieldInfo().FieldId() == aFieldId) {
                    aItem->RemoveField(k);
                    break;
                }
            }
        }
    } else {
        QStringList parts = aDateStr.split("-");
        if (parts.size() == 3) {
            TDateTime dt(parts[0].toInt(), (TMonth)(parts[1].toInt() - 1), parts[2].toInt() - 1, 0, 0, 0, 0);
            TTime time(dt);

            if (field) {
                field->DateTimeStorage()->SetTime(time);
            } else {
                CPbkFieldInfo* info = aFieldsInfo.Find(aFieldId);
                if (info) aItem->AddFieldL(*info).DateTimeStorage()->SetTime(time);
            }
        }
    }
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
    QString json = "{\"etag\": \"" + etag + "\",";

    // Экранируем кавычки и УДАЛЯЕМ ПЕРЕНОСЫ СТРОК (чтобы не было ошибки 400)
    QString safeFirstName = localContact.firstName;
    safeFirstName.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");

    QString safeLastName = localContact.lastName;
    safeLastName.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");

    json += "\"names\":[{\"givenName\": \"" + safeFirstName + "\", \"familyName\": \"" + safeLastName + "\"}],";

    if (!localContact.phones.isEmpty()) {
        json += "\"phoneNumbers\":[";
        for (int i = 0; i < localContact.phones.size(); ++i) {
            QString p = localContact.phones[i];
            p.replace("\"", "\\\"").replace("\n", "").replace("\r", ""); // Удаляем переносы в номере
            json += "{\"value\": \"" + p + "\"}";
            if (i < localContact.phones.size() - 1) json += ",";
        }
        json += "],";
    }

    if (json.endsWith(",")) json.chop(1);
    json += "}";

    QNetworkAccessManager net;
    QString resourceId = localContact.remoteId;
    if (!resourceId.startsWith("people/")) resourceId = "people/" + resourceId;

    QString url = "https://people.googleapis.com/v1/" + resourceId + ":updateContact?updatePersonFields=names,phoneNumbers,emailAddresses,addresses,urls,organizations,biographies,birthdays";
    QUrl qurll(url);
    QNetworkRequest req(qurll);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", QString("Bearer " + accessToken).toUtf8());
    req.setRawHeader("If-Match", QString("\"" + etag + "\"").toUtf8());

    QByteArray data = json.toUtf8();
    QBuffer *buffer = new QBuffer();
    buffer->setData(data);
    buffer->open(QIODevice::ReadOnly);

    QNetworkReply *reply = net.sendCustomRequest(req, "PATCH", buffer);
    QString response = syncWait(reply);

    bool success = (reply->error() == QNetworkReply::NoError);
    if (!success) qDebug() << "[ERROR] Google PATCH:" << response;

    return success;
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
    //TRAPD(dbErr, {
    CContactDatabase* db = NULL;
    TRAPD(openErr, db = CContactDatabase::OpenL());
    if (openErr == KErrNotFound) {
        TRAPD(createErr, db = CContactDatabase::CreateL());
        if (createErr == KErrNotFound) {
            qDebug() << "Ошибка создания";
            User::LeaveIfError(createErr);
        }
    } else if (openErr != KErrNone) {
        qDebug() << "Ошибка";
        User::LeaveIfError(openErr);
    }
    CleanupStack::PushL(db);
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
    readSymbianContacts(existingContacts, db);

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
    if (!toDeleteLocally.isEmpty()) deleteSymbianContacts(toDeleteLocally, db);

    QList<LocalContact> contactsToSaveToPhone;

    // === ФАЗА 3: СИНХРОНИЗАЦИЯ И РАЗРЕШЕНИЕ КОНФЛИКТОВ ===
    for (int i = 0; i < googleContacts.size(); ++i) {
        GoogleContact &gc = googleContacts[i];

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

            // 1. Проверяем изменения
            bool cloudChanged = false;
            if (!lastEtag.isEmpty() && gc.etag != lastEtag) cloudChanged = true;

            bool localChanged = false;
            if (!lastHash.isEmpty() && localHash != lastHash) localChanged = true;

            // Если хэши идентичны — никто не менялся
            if (googleHash == localHash) {
                cloudChanged = false;
                localChanged = false;
            }

            // РАЗРЕШЕНИЕ КОНФЛИКТОВ
            if (lastHash.isEmpty()) {
                // Первая синхронизация этого контакта (связывание).
                applyGoogleDataToLocal(gc, *lc);
                newHashes[gc.id] = calculateHashLocal(*lc);

                // ВАЖНО: Добавляем в список на сохранение в телефон
                contactsToSaveToPhone.append(*lc);
            }
            else if (localChanged && !cloudChanged) {
                // Изменился ТОЛЬКО телефон -> UPLOAD (Отправляем в Google)
                qDebug() << "[^] Upload: Отправка локальных изменений в Google для" << lc->firstName;
                if (updateGoogleContact(accessToken, *lc, gc.etag)) {
                    newHashes[gc.id] = localHash;
                    uploadedCount++;
                } else {
                    newHashes[gc.id] = lastHash;
                }
            }
            else if (cloudChanged) {
                // Изменился Google -> DOWNLOAD (Сохраняем в телефон)
                qDebug() << "[!] Download: Обновление из Google для" << gc.firstName;
                applyGoogleDataToLocal(gc, *lc);
                newHashes[gc.id] = calculateHashLocal(*lc);
                downloadedCount++;

                // ВАЖНО: Добавляем в список на сохранение в телефон
                contactsToSaveToPhone.append(*lc);
            }
            else {
                // НИКТО НЕ МЕНЯЛСЯ. Мы ничего не отправляем в Google и НЕ сохраняем в телефон!
                newHashes[gc.id] = localHash;
            }

            newEtags[gc.id] = gc.etag;
        }
        else
        {
            // НОВЫЙ КОНТАКТ ИЗ GOOGLE -> DOWNLOAD (Сохраняем в телефон)
            qDebug() << "[+] Download: Создание нового контакта" << gc.firstName;
            LocalContact newLc;
            newLc.symbianId = 0;
            newLc.remoteId = gc.id;
            applyGoogleDataToLocal(gc, newLc);

            existingContacts.append(newLc);

            // ВАЖНО: Добавляем в список на сохранение в телефон
            contactsToSaveToPhone.append(newLc);

            newHashes[gc.id] = calculateHashLocal(newLc);
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
                contactsToSaveToPhone.append(existingContacts[i]);
                uploadedCount++;
            }
        }
    }

    if (!contactsToSaveToPhone.isEmpty()) {
        emit m_parent->progressUpdated("Сохранение изменений в телефон...");
        saveSymbianContacts(contactsToSaveToPhone, db);
    } else {
        qDebug() << "[SAVE] Нет изменений для сохранения в телефон.";
    }

    // Финальная запись ВСЕХ изменений в телефонную книгу
    emit m_parent->progressUpdated("Сохранение в телефон...");

    CleanupStack::PopAndDestroy(db);
    emit m_parent->progressUpdated("Сохранение состояния...");
    saveSyncState(newEtags, newHashes);
    
    emit m_parent->syncFinished(true, QString("Готово! Обновлено: %1, Выгружено: %2").arg(downloadedCount).arg(uploadedCount));
    
    //});

    /*  if (dbErr != KErrNone) {
        qDebug() << "КРИТИЧЕСКАЯ ОШИБКА CContactDatabase:" << dbErr;
    }*/
    // СОХРАНЯЕМ НОВОЕ СОСТОЯНИЕ ДЛЯ СЛЕДУЮЩЕГО РАЗА


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

void SyncThread::readSymbianContacts(QList<LocalContact> &list, CContactDatabase* aDb)
{
#ifdef Q_OS_SYMBIAN
    qDebug() << "[READ] === СТАРТ ЧТЕНИЯ БАЗЫ SYMBIAN ===";

    TRAPD(dbErr, {
          qDebug() << "[READ] Открытие CContactDatabase...";
            //  CContactDatabase* db = NULL;
            /* TRAPD(openErr, db = CContactDatabase::OpenL());
                            if (openErr == KErrNotFound) {
                                qDebug() << "[READ] База не найдена, создаем новую...";
                                db = CContactDatabase::CreateL();
                            } else {
                                User::LeaveIfError(openErr);
                            }
                            CleanupStack::PushL(db);*/

            qDebug() << "[READ] Создание итератора...";
    TContactIter iter(*aDb);
    //CleanupStack::PushL(&iter);

    TContactItemId id;
    for (id = iter.FirstL(); id != KNullContactId; id = iter.NextL()) {
        TRAPD(itemErr, {
              qDebug() << "[READ] -------------------------------------";
                qDebug() << "[READ] Чтение контакта с ID:" << id;

        CContactItem* item = aDb->ReadContactL(id);
        CleanupStack::PushL(item);
        if (item->Type() != KUidContactCard) {
            CleanupStack::PopAndDestroy(item);
            //continue;
        } else {

            LocalContact lc;
            lc.symbianId = (long)id;

            CContactItemFieldSet& fieldSet = item->CardFields();
            qDebug() << "[READ] Найдено полей:" << fieldSet.Count();

            for (int k = 0; k < fieldSet.Count(); ++k) {
                CContactItemField& field = fieldSet[k];

                // ГЕНИАЛЬНЫЙ ХАК GAMMU: Вызываем методы цепочкой, избегая CContentType&
                if (field.ContentType().ContainsFieldType(KUidContactFieldGivenName)) {
                    lc.firstName = ToQString(field.TextStorage()->Text());
                    qDebug() << "[READ] Имя:" << lc.firstName;
                }
                else if (field.ContentType().ContainsFieldType(KUidContactFieldFamilyName)) {
                    lc.lastName = ToQString(field.TextStorage()->Text());
                    qDebug() << "[READ] Фамилия:" << lc.lastName;
                }
                else if (field.ContentType().ContainsFieldType(KUidContactFieldPhoneNumber)) {
                    QString phone = ToQString(field.TextStorage()->Text());
                    lc.phones.append(phone);
                    qDebug() << "[READ] Телефон:" << phone;
                }
                else if (field.ContentType().ContainsFieldType(KUidContactFieldEMail)) {
                    QString email = ToQString(field.TextStorage()->Text());
                    lc.emails.append(email);
                    qDebug() << "[READ] Email:" << email;
                }
                else if (field.ContentType().ContainsFieldType(KUidContactFieldPostOffice)) {
                    QString addr = ToQString(field.TextStorage()->Text());
                    lc.addresses.append(addr);
                    qDebug() << "[READ] Адрес:" << addr;
                }
                else if (field.ContentType().ContainsFieldType(KUidContactFieldUrl)) {
                    QString url = ToQString(field.TextStorage()->Text());
                    lc.urls.append(url);
                    qDebug() << "[READ] URL:" << url;
                }
                else if (field.ContentType().ContainsFieldType(KUidContactFieldCompanyName)) {
                    lc.company = ToQString(field.TextStorage()->Text());
                    qDebug() << "[READ] Компания:" << lc.company;
                }
                else if (field.ContentType().ContainsFieldType(KUidContactFieldJobTitle)) {
                    lc.jobTitle = ToQString(field.TextStorage()->Text());
                    qDebug() << "[READ] Должность:" << lc.jobTitle;
                }
                else if (field.ContentType().ContainsFieldType(KUidContactFieldBirthday)) {
                    TTime time = field.DateTimeStorage()->Time();
                    TDateTime dt = time.DateTime();
                    lc.birthday = QString("%1-%2-%3")
                            .arg(dt.Year())
                            .arg(dt.Month() + 1, 2, 10, QChar('0'))
                            .arg(dt.Day() + 1, 2, 10, QChar('0'));
                    qDebug() << "[READ] День рождения:" << lc.birthday;
                }
                else if (field.ContentType().ContainsFieldType(KUidContactFieldNote)) {
                    QString noteText = ToQString(field.TextStorage()->Text());
                    int start = noteText.indexOf("[GID:");
                    if (start != -1) {
                        int end = noteText.indexOf("]", start);
                        if (end != -1) {
                            lc.remoteId = noteText.mid(start + 5, end - start - 5);
                            qDebug() << "[READ] Скрытый Google ID:" << lc.remoteId;
                        }
                    } else {
                        lc.notes = noteText;
                        qDebug() << "[READ] Заметка:" << lc.notes;
                    }
                }
            }
            lc.phones.sort();
            lc.emails.sort();
            lc.addresses.sort();
            lc.urls.sort();

            list.append(lc);
            CleanupStack::PopAndDestroy(item);
            qDebug() << "[READ] Контакт успешно прочитан.";
        }
    });

    if (itemErr != KErrNone) {
        qDebug() << "[READ] [ОШИБКА] Сбой чтения контакта ID" << id << "Код:" << itemErr;
    }
}

//CleanupStack::PopAndDestroy(1);
//CleanupStack::PopAndDestroy(db);
qDebug() << "[READ] === ЧТЕНИЕ УСПЕШНО ЗАВЕРШЕНО ===";
});

if (dbErr != KErrNone) {
    qDebug() << "[READ] [КРИТИЧЕСКАЯ ОШИБКА] Сбой базы контактов:" << dbErr;
}
#endif
}

QString SyncThread::createGoogleContact(const LocalContact &lc, const QString &accessToken)
{
    QString safeFirstName = lc.firstName;
    QString safeLastName = lc.lastName;

    safeFirstName.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");
    safeLastName.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");

    QString json = "{";
    json += "\"names\":[{\"givenName\": \"" + safeFirstName + "\", \"familyName\": \"" + safeLastName + "\"}],";

    if (!lc.phones.isEmpty()) {
        json += "\"phoneNumbers\":[";
        for (int i=0; i<lc.phones.size(); ++i) {
            QString p = lc.phones[i];
            p.replace("\"", "\\\"").replace("\n", "").replace("\r", "");
            json += "{\"value\": \"" + p + "\"}";
            if (i < lc.phones.size()-1) json += ",";
        }
        json += "],";
    }

    if (json.endsWith(",")) json.chop(1);
    json += "}";

    QNetworkAccessManager net;
    QNetworkRequest req(QUrl("https://people.googleapis.com/v1/people:createContact"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", QString("Bearer " + accessToken).toUtf8());
    req.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, false);

    QByteArray data = json.toUtf8();
    QBuffer *buffer = new QBuffer();
    buffer->setData(data);
    buffer->open(QIODevice::ReadOnly);

    QNetworkReply *reply = net.post(req, buffer);
    QString response = syncWait(reply);

    QScriptEngine engine;
    QScriptValue root = engine.evaluate("(" + response + ")");
    return root.property("resourceName").toString();
}



// =======================================================================
// УНИВЕРСАЛЬНЫЕ ХЕЛПЕРЫ ДЛЯ ЗАПИСИ ПОЛЕЙ В SYMBIAN
// =======================================================================

void SyncThread::SetSingleFieldL(CPbkContactItem* aItem, TInt aFieldId, const TDesC& aValue, const CPbkFieldsInfo& aFieldsInfo)
{
    if (aValue.Length() == 0) return;

    TPbkContactItemField* field = aItem->FindField(aFieldId);
    if (field) {
        field->TextStorage()->SetTextL(aValue);
    } else {
        CPbkFieldInfo* info = aFieldsInfo.Find(aFieldId);
        if (info) {
            aItem->AddFieldL(*info).TextStorage()->SetTextL(aValue);
        }
    }
}

void SyncThread::SetMultiFieldL(CPbkContactItem* aItem, TInt aFieldId, const QStringList& aValues, const CPbkFieldsInfo& aFieldsInfo)
{
    // 1. Очищаем все старые поля этого типа, чтобы избежать ошибки -14 и дублей
    for (int k = aItem->CardFields().Count() - 1; k >= 0; --k) {
        if (aItem->CardFields()[k].FieldInfo().FieldId() == aFieldId) {
            aItem->RemoveField(k);
        }
    }

    // 2. Добавляем новые из списка
    CPbkFieldInfo* info = aFieldsInfo.Find(aFieldId);
    if (info) {
        for (int i = 0; i < aValues.size(); ++i) {
            if (!aValues[i].isEmpty()) {
                aItem->AddFieldL(*info).TextStorage()->SetTextL(ToSymbianStr(aValues[i]));
            }
        }
    }
}

void SyncThread::SetDateFieldL(CPbkContactItem* aItem, TInt aFieldId, const QString& aDateStr, const CPbkFieldsInfo& aFieldsInfo)
{
    if (aDateStr.isEmpty()) return;

    QStringList parts = aDateStr.split("-");
    if (parts.size() == 3) {
        // Symbian использует месяцы 0-11 и дни 0-30
        TDateTime dt(parts[0].toInt(), (TMonth)(parts[1].toInt() - 1), parts[2].toInt() - 1, 0, 0, 0, 0);
        TTime time(dt);

        TPbkContactItemField* field = aItem->FindField(aFieldId);
        if (field) {
            field->DateTimeStorage()->SetTime(time);
        } else {
            CPbkFieldInfo* info = aFieldsInfo.Find(aFieldId);
            if (info) {
                aItem->AddFieldL(*info).DateTimeStorage()->SetTime(time);
            }
        }
    }
}

#define SET_SINGLE_FIELD(FieldUid, VcardMap, Value) \
{ \
    int fIdx = -1; \
    for (int k = 0; k < fieldSet.Count(); ++k) { \
    const CContentType* cType = &fieldSet[k].ContentType(); \
    if (cType->ContainsFieldType(FieldUid)) { fIdx = k; break; } \
    } \
    if (Value.isEmpty()) { \
    if (fIdx != -1) fieldSet[fIdx].TextStorage()->SetTextL(KNullDesC); \
    } else { \
    if (fIdx != -1) { \
    fieldSet[fIdx].TextStorage()->SetTextL(ToSymbianStr(Value)); \
    } else { \
    CContactItemField* f = CContactItemField::NewLC(KStorageTypeText); \
    f->AddFieldTypeL(FieldUid); \
    f->SetMapping(VcardMap); \
    f->TextStorage()->SetTextL(ToSymbianStr(Value)); \
    item->AddFieldL(*f); \
    CleanupStack::Pop(f); \
    } \
    } \
    }

#define SET_MULTI_FIELD(FieldUid, VcardMap, ValuesList) \
{ \
    QList<int> existIdx; \
    for (int k = 0; k < fieldSet.Count(); ++k) { \
    const CContentType* cType = &fieldSet[k].ContentType(); \
    if (cType->ContainsFieldType(FieldUid)) { existIdx.append(k); } \
    } \
    int minC = qMin(existIdx.size(), ValuesList.size()); \
    for (int i = 0; i < minC; ++i) { \
    fieldSet[existIdx[i]].TextStorage()->SetTextL(ToSymbianStr(ValuesList[i])); \
    } \
    for (int i = minC; i < ValuesList.size(); ++i) { \
    CContactItemField* f = CContactItemField::NewLC(KStorageTypeText); \
    f->AddFieldTypeL(FieldUid); \
    f->SetMapping(VcardMap); \
    f->TextStorage()->SetTextL(ToSymbianStr(ValuesList[i])); \
    item->AddFieldL(*f); \
    CleanupStack::Pop(f); \
    } \
    for (int i = minC; i < existIdx.size(); ++i) { \
    fieldSet[existIdx[i]].TextStorage()->SetTextL(KNullDesC); \
    } \
    }


// =======================================================================
// ГЛАВНАЯ ФУНКЦИЯ СОХРАНЕНИЯ
// =======================================================================

void SyncThread::saveSymbianContacts(const QList<LocalContact> &toSave, CContactDatabase* aDb)
{
#ifdef Q_OS_SYMBIAN
    qDebug() << "[SAVE] === СТАРТ ЗАПИСИ БАЗЫ SYMBIAN ===";
    /*TRAPD(dbErr, {
          qDebug() << "[SAVE] Открытие CContactDatabase...";
            CContactDatabase* db = NULL;
    TRAPD(openErr, db = CContactDatabase::OpenL());
    if (openErr == KErrNotFound) {
        db = CContactDatabase::CreateL();
    } else {
        User::LeaveIfError(openErr);
    }
    CleanupStack::PushL(db);*/

    for (int i = 0; i < toSave.size(); ++i) {
        const LocalContact &lc = toSave[i];

        qDebug() << "[SAVE] -------------------------------------";
        qDebug() << "[SAVE] Обработка контакта:" << lc.firstName << lc.lastName << "(ID:" << lc.symbianId << ")";

        /*if (lc.symbianId == 1 || lc.symbianId == 2) {
            qDebug() << "[SAVE] Пропуск системного контакта (SIM).";
            continue;
        }*/
        TInt itemErr = KErrNone;
        TRAP(itemErr, {
             CContactItem* item = NULL;
                bool isNew = (lc.symbianId == 0);

        if (isNew) {
            qDebug() << "[SAVE] Создание новой пустой карточки CContactCard...";
            item = CContactCard::NewL();
            CleanupStack::PushL(item);
        } else {
            qDebug() << "[SAVE] Чтение существующей карточки из БД...";
            item = aDb->OpenContactL((TContactItemId)lc.symbianId);
            CleanupStack::PushL(item);
        }

        CContactItemFieldSet& fieldSet = item->CardFields();

        SET_SINGLE_FIELD(KUidContactFieldGivenName, KUidContactFieldVCardMapUnusedN, lc.firstName);
        SET_SINGLE_FIELD(KUidContactFieldFamilyName, KUidContactFieldVCardMapUnusedN, lc.lastName);
        SET_SINGLE_FIELD(KUidContactFieldCompanyName, KUidContactFieldVCardMapORG, lc.company);
        SET_SINGLE_FIELD(KUidContactFieldJobTitle, KUidContactFieldVCardMapTITLE, lc.jobTitle);

        QString fullNote = QString("[GID:%1]\n%2").arg(lc.remoteId).arg(lc.notes).trimmed();
        SET_SINGLE_FIELD(KUidContactFieldNote, KUidContactFieldVCardMapNOTE, fullNote);

        // 2. ОБНОВЛЕНИЕ СПИСКОВЫХ ПОЛЕЙ (Телефоны, Email, Адреса, URL)
        SET_MULTI_FIELD(KUidContactFieldPhoneNumber, KUidContactFieldVCardMapTEL, lc.phones);
        SET_MULTI_FIELD(KUidContactFieldEMail, KUidContactFieldVCardMapEMAILINTERNET, lc.emails);
        SET_MULTI_FIELD(KUidContactFieldPostOffice, KUidContactFieldVCardMapADR, lc.addresses);
        SET_MULTI_FIELD(KUidContactFieldUrl, KUidContactFieldVCardMapURL, lc.urls);

        // 10. ДЕНЬ РОЖДЕНИЯ
        /* qDebug() << "[SAVE] Добавление Дня рождения...";
        if (!lc.birthday.isEmpty()) {
            QStringList parts = lc.birthday.split("-");
            if (parts.size() == 3) {
                TDateTime dt(parts[0].toInt(), (TMonth)(parts[1].toInt() - 1), parts[2].toInt() - 1, 0, 0, 0, 0);
                TTime time(dt);
                CContactItemField* bdayField = CContactItemField::NewL(KStorageTypeDateTime);
                bdayField->AddFieldTypeL(KUidContactFieldBirthday);
                bdayField->SetMapping(KUidContactFieldVCardMapBDAY);
                bdayField->DateTimeStorage()->SetTime(time);
                item->AddFieldL(*bdayField);
                //CleanupStack::PopAndDestroy(bdayField);
            }
        }*/

        // --- СОХРАНЕНИЕ ---
        if (isNew) {
            qDebug() << "[SAVE] Запись нового контакта в базу (AddNewContactL)...";
            aDb->AddNewContactL(*item);
            CleanupStack::PopAndDestroy(item);
            qDebug() << "[SAVE] Контакт успешно сохранен!";
        } else {
            qDebug() << "[SAVE] Запись изменений в базу (CommitContactL)...";
            if (item == NULL) {
                qDebug() << "[SAVE] ОШИБКА: item == NULL перед Commit!";

                
            } else {
                qDebug() << "[SAVE] Попытка Commit для контакта с ID:" << lc.symbianId;
                aDb->CommitContactL(*item);
                CleanupStack::PopAndDestroy(item);
                qDebug() << "[SAVE] Контакт успешно сохранен!";
            }
        }

        
    });



    if (itemErr != KErrNone) {
        qDebug() << "[SAVE][ОШИБКА] Сбой при сохранении контакта" << lc.firstName << "Код:" << itemErr;
        // Снимаем блокировку с базы данных, если сохранение прервалось!
        if (lc.symbianId != 0) {
            TRAP_IGNORE(aDb->CloseContactL((TContactItemId)lc.symbianId));
        }
    }

    // Даем базе Symbian 20мс на сброс файловых буферов
    User::After(20000);
}

//CleanupStack::PopAndDestroy(db);
qDebug() << "[SAVE] === ЗАПИСЬ УСПЕШНО ЗАВЕРШЕНА ===";
//});

/*if (dbErr != KErrNone) {
    qDebug() << "[SAVE][КРИТИЧЕСКАЯ ОШИБКА] Сбой CContactDatabase:" << dbErr;
}*/
#endif
}

void SyncThread::deleteSymbianContacts(const QList<long> &toDelete, CContactDatabase *aDb)
{
#ifdef Q_OS_SYMBIAN
    /*TRAPD(err, {
          CPbkContactEngine* db = CPbkContactEngine::NewL();
            CleanupStack::PushL(db);*/

    for (int i = 0; i < toDelete.size(); ++i) {
        // Удаляем контакт по его Symbian ID
        TRAPD(delErr, aDb->DeleteContactL((TContactItemId)toDelete[i]));
        if (delErr != KErrNone && delErr != KErrNotFound) {
            // Игнорируем KErrNotFound (если контакт уже был удален пользователем)
        }
    }

    //  CleanupStack::PopAndDestroy(db);
    //});
#endif
}

