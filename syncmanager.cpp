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
#include <QDesktopServices>
#include <QTcpSocket>
#include <QNetworkProxy>
#include "localsslproxy.h"
#include <QCryptographicHash>
#include <QFile>
#include <QBuffer>
#include <QDir>
#include <cntdef.h>
#include <cntitem.h>
#include <e32std.h>
#include <sys/time.h>
#include <PbkFields.hrh>
#include <CPbkFieldsInfo.h>
#include <CPbkContactItem.h>

//THIS IS AI SLOP BY GOOGLE GEMINI 3.1 PRO. PLEASE CHILL.

// Утилиты для безопасной конвертации QString <-> TDesC (Symbian Strings)
static TPtrC16 ToSymbianStr(const QString& str) {
    return TPtrC16(reinterpret_cast<const TUint16*>(str.utf16()), str.length());
}

static QString ToQString(const TDesC& des) {
    return QString::fromUtf16(des.Ptr(), des.Length());
}
#endif


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

void SyncManager::logout()
{
    QSettings settings(QDesktopServices::storageLocation(QDesktopServices::DataLocation) + "/wpgcontacts.ini", QSettings::IniFormat);
    settings.remove("refreshToken");
    settings.remove("clientId");
    settings.remove("clientSecret");
    settings.sync();
    QFile file(QDesktopServices::storageLocation(QDesktopServices::DataLocation) + "/sync_state_v2.txt");
    if (file.exists()) file.remove();
    emit authStatusChanged(false);
}

SyncThread::SyncThread(SyncManager *parent) : QThread(parent), m_parent(parent) {

}

QString SyncThread::syncWait(QNetworkReply *reply)
{
    QEventLoop loop;
    connect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
    loop.exec();

    qDebug() << "\n========== ОТВЕТ СЕРВЕРА ==========";

    // 1. Проверяем HTTP статус-код
    QVariant statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    qDebug() << "HTTP Status:" << statusCode.toInt();

    QVariant redirectUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
    if (redirectUrl.isValid()) {
        qDebug() << "ВНИМАНИЕ РЕДИРЕКТ НА:" << redirectUrl.toUrl().toString();
    }


    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "Qt Network Error Code:" << reply->error();
        qDebug() << "Qt Network Error String:" << reply->errorString();
    }

    QByteArray rawData = reply->readAll();
    qDebug() << "Bytes Read:" << rawData.length();

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
    emit m_parent->authStatusChanged(true);
    executeSync(accessToken);
}

QString SyncThread::calculateHashLocal(const LocalContact &lc)
{
    QStringList phones = lc.phones;
    for(int i=0; i < phones.size(); ++i) phones[i] = CleanPhone(phones[i]);
    phones.sort();
    QStringList emails = lc.emails;
    for(int i=0; i < emails.size(); ++i) emails[i] = emails[i].trimmed().toLower();
    emails.sort();
    QString raw = QString("%1|%2|%3|%4|%5|%6|%7")
            .arg(lc.firstName.trimmed().toLower())
            .arg(lc.lastName.trimmed().toLower())
            .arg(phones.join(","))
            .arg(emails.join(","))
            .arg(lc.company.trimmed().toLower())
            .arg(lc.jobTitle.trimmed().toLower())
            .arg(lc.birthday);
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
    QString bday = "";
    if (gc.birthday.year != 0 || gc.birthday.month != 0 || gc.birthday.day != 0) {
        bday = QString("%1-%2-%3")
                .arg(gc.birthday.year) // int
                .arg(gc.birthday.month, 2, 10, QChar('0')) // uint -> QString
                .arg(gc.birthday.day, 2, 10, QChar('0'));   // uint -> QString
    }
    QString raw = QString("%1|%2|%3|%4|%5|%6|%7")
            .arg(gc.firstName.trimmed().toLower())
            .arg(gc.lastName.trimmed().toLower())
            .arg(phones.join(","))
            .arg(emails.join(","))
            .arg(gc.company.trimmed().toLower())
            .arg(gc.jobTitle.trimmed().toLower())
            .arg(bday);
    QByteArray hash = QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(hash.toHex());
}

QString SyncThread::CleanPhone(const QString& phone)
{
    QString cleaned = phone;
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

}




void SyncThread::saveSyncState(const QMap<QString, QString>& etags, const QMap<QString, QString>& hashes)
{
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
            } else {
                // Ищем только ПЕРВЫЙ знак равно
                int eqIdx = line.indexOf("=");
                if (eqIdx != -1) {
                    QString key = line.left(eqIdx).trimmed();
                    QString val = line.mid(eqIdx + 1).trimmed(); // Берем всё, что после первого '='

                    if (currentSection == "etags") state.etags[key] = val;
                    else if (currentSection == "hashes") state.hashes[key] = val;
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
        QString requestUri = "https://people.googleapis.com/v1/people/me/connections?"
                "personFields=names,phoneNumbers,emailAddresses,addresses,urls,birthdays,organizations,biographies&pageSize=100";
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
            if (!root.isObject()) {
                            hasMorePages = false;
                            continue;
                        }

            if (root.property("connections").isValid()) {
                if (root.property("connections").isArray()) {
                    QScriptValue connections = root.property("connections");
                    int len = connections.property("length").toInt32();

                    for (int i = 0; i < len; ++i) {
                        QScriptValue person = connections.property(i);
                        GoogleContact gc;

                        gc.id = person.property("resourceName").toString();
                        gc.etag = person.property("etag").toString();

                        QScriptValue names = person.property("names");
                        if (names.isArray() && names.property("length").toInt32() > 0) {
                            gc.firstName = names.property(0).property("givenName").toString();
                            gc.lastName = names.property(0).property("familyName").toString();
                        }

                        QScriptValue phones = person.property("phoneNumbers");
                        if (phones.isArray()) {
                            int count = phones.property("length").toInt32();
                            for (int j = 0; j < count; ++j)
                                gc.phones.append(phones.property(j).property("value").toString());
                        }

                        QScriptValue emails = person.property("emailAddresses");
                        if (emails.isArray()) {
                            int count = emails.property("length").toInt32();
                            for (int j = 0; j < count; ++j)
                                gc.emails.append(emails.property(j).property("value").toString());
                        }

                        QScriptValue addresses = person.property("addresses");
                        if (addresses.isArray()) {
                            int count = addresses.property("length").toInt32();
                            for (int j = 0; j < count; ++j)
                                gc.addresses.append(addresses.property(j).property("formattedValue").toString());
                        }

                        QScriptValue urls = person.property("urls");
                        if (urls.isArray()) {
                            int count = urls.property("length").toInt32();
                            for (int j = 0; j < count; ++j)
                                gc.urls.append(urls.property(j).property("value").toString());
                        }

                        QScriptValue orgs = person.property("organizations");
                        if (orgs.isArray() && orgs.property("length").toInt32() > 0) {
                            QScriptValue org = orgs.property(0);
                            gc.company = org.property("name").toString();
                            gc.jobTitle = org.property("title").toString();
                        }

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

                        QScriptValue bios = person.property("biographies");
                        if (bios.isArray() && bios.property("length").toInt32() > 0) {
                            gc.notes = bios.property(0).property("value").toString();
                        }

                        if (!gc.firstName.isEmpty() || !gc.lastName.isEmpty() || !gc.phones.isEmpty() || !gc.emails.isEmpty()) {
                            contactsList.append(gc);
                        }
                    }
                }
            }

            if (root.property("nextPageToken").isValid() && !root.property("nextPageToken").isUndefined() && !root.property("nextPageToken").toString().isEmpty()) {
                nextPageToken = root.property("nextPageToken").toString();
            } else {
                hasMorePages = false;
            }
        } else {
            hasMorePages = false;
        }
    }
}

bool SyncThread::updateGoogleContact(const QString &accessToken, const LocalContact &localContact, const QString &etag)
{
    QStringList jsonParts;
    jsonParts.append("\"etag\": \"" + etag + "\"");

    QString safeFirstName = localContact.firstName; safeFirstName.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");
    QString safeLastName = localContact.lastName; safeLastName.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");
    jsonParts.append("\"names\":[{\"givenName\": \"" + safeFirstName + "\", \"familyName\": \"" + safeLastName + "\"}]");

    if (!localContact.phones.isEmpty()) {
        QStringList phones;
        for (int i = 0; i < localContact.phones.size(); ++i) {
            QString p = localContact.phones[i]; p.replace("\"", "\\\"").replace("\n", "").replace("\r", "");
            phones.append("{\"value\": \"" + p + "\"}");
        }
        jsonParts.append("\"phoneNumbers\":[" + phones.join(",") + "]");
    }

    if (!localContact.emails.isEmpty()) {
        QStringList emails;
        for (int i = 0; i < localContact.emails.size(); ++i) {
            QString e = localContact.emails[i]; e.replace("\"", "\\\"").replace("\n", "").replace("\r", "");
            emails.append("{\"value\": \"" + e + "\"}");
        }
        jsonParts.append("\"emailAddresses\":[" + emails.join(",") + "]");
    }

    if (!localContact.company.isEmpty() || !localContact.jobTitle.isEmpty()) {
        QString safeCompany = localContact.company; safeCompany.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");
        QString safeTitle = localContact.jobTitle; safeTitle.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");
        jsonParts.append("\"organizations\":[{\"name\": \"" + safeCompany + "\", \"title\": \"" + safeTitle + "\"}]");
    }

    if (!localContact.addresses.isEmpty()) {
        QStringList addresses;
        for (int i = 0; i < localContact.addresses.size(); ++i) {
            QString a = localContact.addresses[i]; a.replace("\"", "\\\"").replace("\n", " ").replace("\r", " ");
            addresses.append("{\"streetAddress\": \"" + a + "\"}");
        }
        jsonParts.append("\"addresses\":[" + addresses.join(",") + "]");
    }

    if (!localContact.birthday.isEmpty()) {
        QStringList parts = localContact.birthday.split("-");
        if (parts.size() == 3) {
            // Превращаем "01" в "1" с помощью .toInt()
            QString bdayJson = QString("\"birthdays\":[{\"date\": {\"year\": %1, \"month\": %2, \"day\": %3}}]")
                    .arg(parts[0].toInt())
                    .arg(parts[1].toInt())
                    .arg(parts[2].toInt());
            jsonParts.append(bdayJson);
        }
    }

    if (!localContact.notes.isEmpty()) {
        QString safeNotes = localContact.notes; safeNotes.replace("\"", "\\\"").replace("\n", "\\n").replace("\r", "");
        jsonParts.append("\"biographies\":[{\"value\": \"" + safeNotes + "\"}]");
    }

    QString json = "{" + jsonParts.join(",") + "}";

    QNetworkAccessManager net;
    QString resourceId = localContact.remoteId;
    if (!resourceId.startsWith("people/")) resourceId = "people/" + resourceId;

    QString url = "https://people.googleapis.com/v1/" + resourceId +
            ":updateContact?updatePersonFields=names,phoneNumbers,emailAddresses,addresses,organizations,birthdays,biographies";
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

    bool ok = (reply->error() == QNetworkReply::NoError);
    reply->deleteLater();
    buffer->deleteLater();
    return ok;
}

bool SyncThread::deleteGoogleContact(const QString &accessToken, const QString &resourceName)
{
    QNetworkAccessManager net;

    QString url = "https://people.googleapis.com/v1/" + resourceName + ":deleteContact";
    QUrl qurll(url);
    QNetworkRequest req(qurll);

    req.setRawHeader("Authorization", QString("Bearer " + accessToken).toUtf8());

    QNetworkReply *reply = net.sendCustomRequest(req, "DELETE");

    QString response = syncWait(reply);

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() == QNetworkReply::NoError || statusCode == 404) {
        qDebug() << "[OK] Удалено из Google (или уже отсутствовало):" << resourceName;
        return true; // Возвращаем true, чтобы контакт стерся из нашей "памяти"
    } else {
        qDebug() << "[ERROR] Удаление из Google:" << reply->errorString() << response;
        return false;
    }
}

// =======================================================================
// ПАКЕТНАЯ (BATCH) ВЫГРУЗКА В GOOGLE API 
// =======================================================================

QString SyncThread::buildPersonJson(const LocalContact &lc, const QString &etag)
{
    QStringList jsonParts;
    if (!etag.isEmpty()) {
        jsonParts.append("\"etag\": \"" + etag + "\"");
    }

    QString safeFirstName = lc.firstName; safeFirstName.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");
    QString safeLastName = lc.lastName; safeLastName.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");
    jsonParts.append("\"names\":[{\"givenName\": \"" + safeFirstName + "\", \"familyName\": \"" + safeLastName + "\"}]");

    if (!lc.phones.isEmpty()) {
        QStringList phones;
        for (int i = 0; i < lc.phones.size(); ++i) {
            QString p = lc.phones[i]; p.replace("\"", "\\\"").replace("\n", "").replace("\r", "");
            phones.append("{\"value\": \"" + p + "\"}");
        }
        jsonParts.append("\"phoneNumbers\":[" + phones.join(",") + "]");
    }

    if (!lc.emails.isEmpty()) {
        QStringList emails;
        for (int i = 0; i < lc.emails.size(); ++i) {
            QString e = lc.emails[i]; e.replace("\"", "\\\"").replace("\n", "").replace("\r", "");
            emails.append("{\"value\": \"" + e + "\"}");
        }
        jsonParts.append("\"emailAddresses\":[" + emails.join(",") + "]");
    }

    if (!lc.company.isEmpty() || !lc.jobTitle.isEmpty()) {
        QString safeCompany = lc.company; safeCompany.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");
        QString safeTitle = lc.jobTitle; safeTitle.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");
        jsonParts.append("\"organizations\":[{\"name\": \"" + safeCompany + "\", \"title\": \"" + safeTitle + "\"}]");
    }

    if (!lc.addresses.isEmpty()) {
        QStringList addresses;
        for (int i = 0; i < lc.addresses.size(); ++i) {
            QString a = lc.addresses[i]; a.replace("\"", "\\\"").replace("\n", " ").replace("\r", " ");
            addresses.append("{\"streetAddress\": \"" + a + "\"}");
        }
        jsonParts.append("\"addresses\":[" + addresses.join(",") + "]");
    }

    if (!lc.birthday.isEmpty()) {
        QStringList parts = lc.birthday.split("-");
        if (parts.size() == 3) {
            QString bdayJson = QString("\"birthdays\":[{\"date\": {\"year\": %1, \"month\": %2, \"day\": %3}}]")
                    .arg(parts[0].toInt()).arg(parts[1].toInt()).arg(parts[2].toInt());
            jsonParts.append(bdayJson);
        }
    }

    if (!lc.notes.isEmpty()) {
        QString safeNotes = lc.notes; safeNotes.replace("\"", "\\\"").replace("\n", "\\n").replace("\r", "");
        jsonParts.append("\"biographies\":[{\"value\": \"" + safeNotes + "\"}]");
    }

    return "{" + jsonParts.join(",") + "}";
}

int SyncThread::batchCreateGoogleContacts(const QString &accessToken, QList<LocalContact> &contactsList, const QList<int> &indices)
{
    if (indices.isEmpty()) return 0;
    int successCount = 0;
    int batchSize = 100;

    QNetworkAccessManager net;
    QNetworkRequest req(QUrl("https://people.googleapis.com/v1/people:batchCreateContacts"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", QString("Bearer " + accessToken).toUtf8());
    req.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, false);

    for (int i = 0; i < indices.size(); i += batchSize) {
        int end = qMin(i + batchSize, indices.size());
        QStringList contactsJsonArray;

        for (int j = i; j < end; ++j) {
            LocalContact &lc = contactsList[indices[j]];
            contactsJsonArray.append("{\"contactPerson\": " + buildPersonJson(lc) + "}");
        }

        // ВАЖНО: Добавляем readMask! Без него Google вернет {}
        QString reqJson = "{"
                "\"contacts\": [" + contactsJsonArray.join(",") + "],"
                "\"readMask\": \"names,phoneNumbers,emailAddresses,addresses,organizations,birthdays,biographies\""
                "}";

        QByteArray data = reqJson.toUtf8();
        QBuffer *buffer = new QBuffer();
        buffer->setData(data);
        buffer->open(QIODevice::ReadOnly);

        QNetworkReply *reply = net.post(req, buffer);
        QString response = syncWait(reply);
        buffer->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            QScriptEngine engine;
            QScriptValue root = engine.evaluate("(" + response + ")");
            QScriptValue createdPeople = root.property("createdPeople");

            if (createdPeople.isArray()) {
                int len = createdPeople.property("length").toInt32();
                for (int j = 0; j < len; ++j) {
                    QScriptValue personItem = createdPeople.property(j).property("person");
                    if (personItem.isValid()) {
                        QString newId = personItem.property("resourceName").toString();
                        if (!newId.isEmpty()) {
                            // Присваиваем новые ID обратно
                            contactsList[indices[i + j]].remoteId = newId;
                            successCount++;
                        }
                    }
                }
            } else {
                qDebug() << "[ERROR] createdPeople не является массивом. Ответ сервера:" << response;
            }
        } else {
            qDebug() << "[ERROR] Batch Create Failed:" << response;
        }
        reply->deleteLater();
    }
    return successCount;
}

int SyncThread::batchUpdateGoogleContacts(const QString &accessToken, QList<LocalContact> &contactsList, const QList<int> &indices, const QMap<QString, QString> &etagsMap)
{
    if (indices.isEmpty()) return 0;
    int successCount = 0;
    int batchSize = 100;

    QNetworkAccessManager net;
    QNetworkRequest req(QUrl("https://people.googleapis.com/v1/people:batchUpdateContacts"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", QString("Bearer " + accessToken).toUtf8());
    req.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, false);

    for (int i = 0; i < indices.size(); i += batchSize) {
        int end = qMin(i + batchSize, indices.size());
        QStringList contactsJsonMap;

        for (int j = i; j < end; ++j) {
            LocalContact &lc = contactsList[indices[j]];
            QString etag = etagsMap.value(lc.remoteId, "");
            QString personJson = buildPersonJson(lc, etag);

            QString resId = lc.remoteId;
            if (!resId.startsWith("people/")) resId = "people/" + resId;

            contactsJsonMap.append("\"" + resId + "\": " + personJson);
        }

        // ВАЖНО: Для update нужна и updateMask, и readMask (если мы хотим получить ответ)
        QString reqJson = "{"
                "\"contacts\": {" + contactsJsonMap.join(",") + "},"
                "\"updateMask\": \"names,phoneNumbers,emailAddresses,addresses,organizations,birthdays,biographies\","
                "\"readMask\": \"names,phoneNumbers\"" // Чтобы ответ не был пустым {}
                "}";

        QByteArray data = reqJson.toUtf8();
        QBuffer *buffer = new QBuffer();
        buffer->setData(data);
        buffer->open(QIODevice::ReadOnly);

        QNetworkReply *reply = net.post(req, buffer);
        QString response = syncWait(reply);
        buffer->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            successCount += (end - i);
        } else {
            qDebug() << "[ERROR] Batch Update Failed:" << response;
        }
        reply->deleteLater();
    }
    return successCount;
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

    // =====================================================================
    // ФАЗА 0: ПОИСК И ОБЪЕДИНЕНИЕ ЛОКАЛЬНЫХ ДУБЛИКАТОВ (DEDUPLICATION)
    // =====================================================================

    emit m_parent->progressUpdated("Поиск дубликатов...");
    QList<LocalContact> deduplicatedContacts;
    QList<long> duplicateSymbianIdsToDelete;
    QList<QString> cloudDuplicatesToDelete;
    QList<LocalContact> mergedContactsToSave; // НОВЫЙ СПИСОК: контакты, которые нужно сохранить

    for (int i = 0; i < existingContacts.size(); ++i) {
        LocalContact current = existingContacts[i];
        if (current.symbianId == -1) continue;

        bool wasMerged = false; // Флаг: склеивали ли мы этот контакт?

        for (int j = i + 1; j < existingContacts.size(); ++j) {
            LocalContact &other = existingContacts[j];
            if (other.symbianId == -1) continue;

            if ((!current.firstName.isEmpty() || !current.lastName.isEmpty()) &&
                    current.firstName.compare(other.firstName, Qt::CaseInsensitive) == 0 &&
                    current.lastName.compare(other.lastName, Qt::CaseInsensitive) == 0)
            {
                qDebug() << "[MERGE] Объединяем дубликат:" << current.firstName << current.lastName;
                wasMerged = true;

                // 1. Разрешение RemoteId
                if (current.remoteId.isEmpty() && !other.remoteId.isEmpty()) {
                    current.remoteId = other.remoteId;
                }
                else if (!current.remoteId.isEmpty() && !other.remoteId.isEmpty() && current.remoteId != other.remoteId) {
                    // Удаляем лишний клон из Google
                    cloudDuplicatesToDelete.append(other.remoteId);
                }

                // 2. Объединение списков (БЕЗ ПОТЕРЬ)
                foreach(QString p, other.phones) { if (!current.phones.contains(p)) current.phones.append(p); }
                foreach(QString e, other.emails) { if (!current.emails.contains(e)) current.emails.append(e); }
                foreach(QString a, other.addresses) { if (!current.addresses.contains(a)) current.addresses.append(a); }
                foreach(QString u, other.urls) { if (!current.urls.contains(u)) current.urls.append(u); }

                // 3. Объединение одиночных полей
                if (current.company.isEmpty()) current.company = other.company;
                if (current.jobTitle.isEmpty()) current.jobTitle = other.jobTitle;
                if (current.birthday.isEmpty()) current.birthday = other.birthday;

                if (current.notes.isEmpty()) {
                    current.notes = other.notes;
                } else if (!other.notes.isEmpty() && current.notes != other.notes) {
                    current.notes += "\n---\n" + other.notes;
                }

                duplicateSymbianIdsToDelete.append(other.symbianId);
                other.symbianId = -1; // Метка "Удален"
            }
        }

        // Если контакт вобрал в себя данные дубликатов, его ОБЯЗАТЕЛЬНО нужно обновить в телефоне
        if (wasMerged) {
            mergedContactsToSave.append(current);
        }

        deduplicatedContacts.append(current);
    }

    existingContacts = deduplicatedContacts;

    // Сначала удаляем клоны из телефона
    if (!duplicateSymbianIdsToDelete.isEmpty()) {
        qDebug() << "[MERGE] Удаление" << duplicateSymbianIdsToDelete.size() << "дубликатов из телефона...";
        deleteSymbianContacts(duplicateSymbianIdsToDelete, db);
    }

    // ЗАТЕМ СОХРАНЯЕМ РЕЗУЛЬТАТ СКЛЕЙКИ В ТЕЛЕФОН (Именно этого не хватало!)
    if (!mergedContactsToSave.isEmpty()) {
        qDebug() << "[MERGE] Сохранение объединенных контактов в базу телефона...";
        saveSymbianContacts(mergedContactsToSave, db);
    }

    // Удаляем дубликаты из Google (как было)
    if (!cloudDuplicatesToDelete.isEmpty()) {
        emit m_parent->progressUpdated("Очистка дубликатов в облаке...");
        for (int i = 0; i < cloudDuplicatesToDelete.size(); ++i) {
            QString dupId = cloudDuplicatesToDelete[i];
            deleteGoogleContact(accessToken, dupId);

            for (int k = 0; k < googleContacts.size(); ++k) {
                if (googleContacts[k].id == dupId) {
                    googleContacts.removeAt(k);
                    break;
                }
            }
        }
    }
    // =====================================================================



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
    emit m_parent->progressUpdated("Проверка удаленных контактов (2/4)...");
    QList<long> toDeleteLocally;

    // Идем с конца в начало, так как мы будем удалять элементы из списка existingContacts
    for (int i = existingContacts.size() - 1; i >= 0; --i) {
        QString rId = existingContacts[i].remoteId;

        if (!rId.isEmpty() && !currentCloudEtags.contains(rId)) {
            // Контакта нет в текущем Google-аккаунте.
            // Проверяем: а мы его вообще когда-нибудь видели в ЭТОМ аккаунте?
            if (state.etags.contains(rId)) {
                // Да, он был в нашей "памяти", но теперь в облаке его нет -> Удален.
                qDebug() << "[-] Удаление локально (пропал из Google):" << existingContacts[i].firstName;
                toDeleteLocally.append(existingContacts[i].symbianId);
                existingContacts.removeAt(i); // Убираем из массива, чтобы Фаза 3 его не трогала
                localDeletedCount++;
            } else {
                // Нет, в памяти этого аккаунта его нет!
                // Это чужой бэкап (или контакты из другого аккаунта).
                // Стираем ему старый GID, чтобы Фаза 4 загрузила его в новый аккаунт.
                qDebug() << "[*] Обнаружен чужой бэкап. Сброс GID для:" << existingContacts[i].firstName;
                existingContacts[i].remoteId = "";
                // Мы оставляем его в existingContacts. В Фазе 4 он будет обработан как новый.
            }
        }
    }
    if (!toDeleteLocally.isEmpty()) deleteSymbianContacts(toDeleteLocally, db);

    QList<LocalContact> contactsToSaveToPhone;

    QList<int> indicesToUpdateInGoogle;
    QList<int> indicesToCreateInGoogle;

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
                qDebug() << "[!] Download: Обновление из Google для" << gc.firstName;
                applyGoogleDataToLocal(gc, *lc);
                newHashes[gc.id] = calculateHashLocal(*lc);

                contactsToSaveToPhone.append(*lc); // <--- ВАЖНО: Добавляем в очередь на сохранение
            } else if (localChanged) {
                qDebug() << "[^] Upload: Отправка изменений в Google для" << lc->firstName;

                indicesToUpdateInGoogle.append(i);
            } else {
                newHashes[gc.id] = localHash; // Ничего не менялось
            }
            newEtags[gc.id] = gc.etag;
        }
        else
        {
            // НОВЫЙ КОНТАКТ ИЗ GOOGLE
            qDebug() << "[+] Download: Создание нового контакта" << gc.firstName;
            LocalContact newLc;
            newLc.symbianId = 0;
            newLc.remoteId = gc.id;
            applyGoogleDataToLocal(gc, newLc);

            existingContacts.append(newLc);
            contactsToSaveToPhone.append(newLc); // Добавляем в очередь на создание

            newHashes[gc.id] = calculateHashLocal(newLc);
            newEtags[gc.id] = gc.etag;
            downloadedCount++;
        }
    }

    // === ФАЗА 4: ВЫГРУЗКА НОВЫХ ЛОКАЛЬНЫХ КОНТАКТОВ ===

    emit m_parent->progressUpdated("Выгрузка новых контактов в Google...");
    for(int i=0; i<existingContacts.size(); ++i) {
        if (existingContacts[i].remoteId.isEmpty()) {
            indicesToCreateInGoogle.append(i);
        }
    }

    if (!indicesToUpdateInGoogle.isEmpty()) {
        emit m_parent->progressUpdated(QString("Пакетное обновление %1 контактов в Google...").arg(indicesToUpdateInGoogle.size()));
        uploadedCount += batchUpdateGoogleContacts(accessToken, existingContacts, indicesToUpdateInGoogle, state.etags);

        // После успешного обновления, записываем новые хеши в память
        foreach(int idx, indicesToUpdateInGoogle) {
            newHashes[existingContacts[idx].remoteId] = calculateHashLocal(existingContacts[idx]);
        }
    }

    if (!indicesToCreateInGoogle.isEmpty()) {
        emit m_parent->progressUpdated(QString("Пакетное создание %1 новых контактов в Google...").arg(indicesToCreateInGoogle.size()));
        int createdCount = batchCreateGoogleContacts(accessToken, existingContacts, indicesToCreateInGoogle);
        uploadedCount += createdCount;

        // Если контакты успешно созданы, у них появился RemoteId.
        // Мы должны сохранить их обратно в базу Symbian (чтобы прописать [GID:...])
        foreach(int idx, indicesToCreateInGoogle) {
            if (!existingContacts[idx].remoteId.isEmpty()) {
                contactsToSaveToPhone.append(existingContacts[idx]);
                newHashes[existingContacts[idx].remoteId] = calculateHashLocal(existingContacts[idx]);
            }
        }
    }

    // Финальная запись ТОЛЬКО ИЗМЕНЕНИЙ в телефонную книгу
    if (!contactsToSaveToPhone.isEmpty()) {
        emit m_parent->progressUpdated(QString("Сохранение в телефон (%1 шт)...").arg(contactsToSaveToPhone.size()));
        saveSymbianContacts(contactsToSaveToPhone, db);
    } else {
        qDebug() << "[SAVE] Нет изменений для сохранения в телефон.";
    }

    CleanupStack::PopAndDestroy(db);
    emit m_parent->progressUpdated("Сохранение состояния...");
    saveSyncState(newEtags, newHashes);
    
    emit m_parent->syncFinished(true, QString("Готово! Обновлено: %1, Выгружено: %2").arg(downloadedCount).arg(uploadedCount));
    
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
            qDebug() << "[READ] Создание итератора...";
    TContactIter iter(*aDb);
    TContactItemId id;
    for (id = iter.FirstL(); id != KNullContactId; id = iter.NextL()) {
        TRAPD(itemErr, {
              qDebug() << "[READ] -------------------------------------";
                qDebug() << "[READ] Чтение контакта с ID:" << id;

        CContactItem* item = aDb->ReadContactL(id);
        CleanupStack::PushL(item);
        if (item->Type() != KUidContactCard) {
            CleanupStack::PopAndDestroy(item);
        } else {
            LocalContact lc;
            lc.symbianId = (long)id;

            CContactItemFieldSet& fieldSet = item->CardFields();
            qDebug() << "[READ] Найдено полей:" << fieldSet.Count();

            for (int k = 0; k < fieldSet.Count(); ++k) {
                CContactItemField& field = fieldSet[k];

                // Вызываем методы цепочкой, избегая CContentType&
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

qDebug() << "[READ] === ЧТЕНИЕ УСПЕШНО ЗАВЕРШЕНО ===";
});

#endif
}

QString SyncThread::createGoogleContact(const LocalContact &lc, const QString &accessToken)
{
    QStringList jsonParts;

    // ИМЕНА
    QString safeFirstName = lc.firstName; safeFirstName.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");
    QString safeLastName = lc.lastName; safeLastName.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");
    jsonParts.append("\"names\":[{\"givenName\": \"" + safeFirstName + "\", \"familyName\": \"" + safeLastName + "\"}]");

    // ТЕЛЕФОНЫ
    if (!lc.phones.isEmpty()) {
        QStringList phones;
        for (int i = 0; i < lc.phones.size(); ++i) {
            QString p = lc.phones[i]; p.replace("\"", "\\\"").replace("\n", "").replace("\r", "");
            phones.append("{\"value\": \"" + p + "\"}");
        }
        jsonParts.append("\"phoneNumbers\":[" + phones.join(",") + "]");
    }

    // EMAIL (ДОБАВЛЕНО!)
    if (!lc.emails.isEmpty()) {
        QStringList emails;
        for (int i = 0; i < lc.emails.size(); ++i) {
            QString e = lc.emails[i]; e.replace("\"", "\\\"").replace("\n", "").replace("\r", "");
            emails.append("{\"value\": \"" + e + "\"}");
        }
        jsonParts.append("\"emailAddresses\":[" + emails.join(",") + "]");
    }

    // КОМПАНИЯ И ДОЛЖНОСТЬ (ДОБАВЛЕНО!)
    if (!lc.company.isEmpty() || !lc.jobTitle.isEmpty()) {
        QString safeCompany = lc.company; safeCompany.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");
        QString safeTitle = lc.jobTitle; safeTitle.replace("\"", "\\\"").replace("\n", " ").replace("\r", "");
        jsonParts.append("\"organizations\":[{\"name\": \"" + safeCompany + "\", \"title\": \"" + safeTitle + "\"}]");
    }

    // АДРЕСА (ДОБАВЛЕНО!)
    if (!lc.addresses.isEmpty()) {
        QStringList addresses;
        for (int i = 0; i < lc.addresses.size(); ++i) {
            QString a = lc.addresses[i]; a.replace("\"", "\\\"").replace("\n", " ").replace("\r", " ");
            addresses.append("{\"streetAddress\": \"" + a + "\"}");
        }
        jsonParts.append("\"addresses\":[" + addresses.join(",") + "]");
    }

    // ДЕНЬ РОЖДЕНИЯ (ДОБАВЛЕНО!)
    if (!lc.birthday.isEmpty()) {
        QStringList parts = lc.birthday.split("-");
        if (parts.size() == 3) {
            QString bdayJson = QString("\"birthdays\":[{\"date\": {\"year\": %1, \"month\": %2, \"day\": %3}}]")
                    .arg(parts[0]).arg(parts[1]).arg(parts[2]);
            jsonParts.append(bdayJson);
        }
    }

    // ЗАМЕТКИ (ДОБАВЛЕНО!)
    if (!lc.notes.isEmpty()) {
        QString safeNotes = lc.notes; safeNotes.replace("\"", "\\\"").replace("\n", "\\n").replace("\r", "");
        jsonParts.append("\"biographies\":[{\"value\": \"" + safeNotes + "\"}]");
    }

    // Собираем всё в одну строку
    QString json = "{" + jsonParts.join(",") + "}";

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


#define SET_SINGLE_FIELD(FieldUid, VcardMap, Value) \
{ \
    CContactItemField* targetField = NULL; \
    int firstFoundIdx = -1; \
    /* 1. Ищем существующее поле и удаляем дубликаты, если старая прога их наплодила */ \
    for (int k = fieldSet.Count() - 1; k >= 0; --k) { \
        if (fieldSet[k].ContentType().ContainsFieldType(FieldUid)) { \
            if (firstFoundIdx == -1) { \
                firstFoundIdx = k; \
                targetField = &fieldSet[k]; \
            } else { \
                /* Если нашли второе такое же поле (дубликат имени/заметки) - удаляем его */ \
                item->RemoveField(k); \
            } \
        } \
    } \
    if (Value.isEmpty()) { \
        if (targetField) item->RemoveField(firstFoundIdx); \
    } else { \
        if (targetField) { \
            targetField->TextStorage()->SetTextL(ToSymbianStr(Value)); \
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
    /* 1. Собираем индексы существующих полей этого типа */ \
    for (int k = 0; k < fieldSet.Count(); ++k) { \
        if (fieldSet[k].ContentType().ContainsFieldType(FieldUid)) { \
            existIdx.append(k); \
        } \
    } \
    int minC = qMin(existIdx.size(), ValuesList.size()); \
    /* 2. Обновляем существующие поля */ \
    for (int i = 0; i < minC; ++i) { \
        fieldSet[existIdx[i]].TextStorage()->SetTextL(ToSymbianStr(ValuesList[i])); \
    } \
    /* 3. Если в новом списке значений больше - добавляем новые поля */ \
    for (int i = minC; i < ValuesList.size(); ++i) { \
        CContactItemField* f = CContactItemField::NewLC(KStorageTypeText); \
        f->AddFieldTypeL(FieldUid); \
        f->SetMapping(VcardMap); \
        f->TextStorage()->SetTextL(ToSymbianStr(ValuesList[i])); \
        item->AddFieldL(*f); \
        CleanupStack::Pop(f); \
    } \
    /* 4. Если в телефоне полей БОЛЬШЕ, чем пришло сейчас - УДАЛЯЕМ лишние (а не нуллим) */ \
    /* Удаляем с конца, чтобы не поплыли индексы массива */ \
    if (existIdx.size() > ValuesList.size()) { \
        for (int i = existIdx.size() - 1; i >= minC; --i) { \
            item->RemoveField(existIdx[i]); \
        } \
    } \
}

// =======================================================================
// ГЛАВНАЯ ФУНКЦИЯ СОХРАНЕНИЯ
// =======================================================================

void SyncThread::saveSymbianContacts(const QList<LocalContact> &toSave, CContactDatabase* aDb)
{
#ifdef Q_OS_SYMBIAN
    qDebug() << "[SAVE] === СТАРТ ЗАПИСИ БАЗЫ SYMBIAN ===";

    for (int i = 0; i < toSave.size(); ++i) {
        const LocalContact &lc = toSave[i];

        qDebug() << "[SAVE] -------------------------------------";
        qDebug() << "[SAVE] Обработка контакта:" << lc.firstName << lc.lastName << "(ID:" << lc.symbianId << ")";

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
        qDebug() << "[SAVE] Добавление Дня рождения...";
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
                // CleanupStack::Pop(bdayField);
            }
        }

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


qDebug() << "[SAVE] === ЗАПИСЬ УСПЕШНО ЗАВЕРШЕНА ===";

#endif
}

void SyncThread::deleteSymbianContacts(const QList<long> &toDelete, CContactDatabase *aDb)
{
#ifdef Q_OS_SYMBIAN

    for (int i = 0; i < toDelete.size(); ++i) {
        // Удаляем контакт по его Symbian ID
        TRAPD(delErr, aDb->DeleteContactL((TContactItemId)toDelete[i]));
        if (delErr != KErrNone && delErr != KErrNotFound) {
            // Игнорируем KErrNotFound (если контакт уже был удален пользователем)
        }
    }

#endif
}

