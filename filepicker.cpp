#include "filepicker.h"
#include <QDir>
#include <QFile>
#include <QScriptEngine>
#include <QScriptValue>
#include <QDebug>

FilePicker::FilePicker(QObject *parent) : QObject(parent) {}

QStringList FilePicker::findJsonFiles()
{
    QStringList result;

    // Common Symbian locations to search
    QStringList searchPaths;
    searchPaths << "E:/"
                << "E:/Data"
                << "E:/Others"
                << "E:/Documents"
                << "C:/Data"
                << "C:/Data/Others"
                << "C:/Data/Downloads";

    foreach (const QString &path, searchPaths) {
        QDir dir(path);
        if (!dir.exists()) continue;

        QStringList files = dir.entryList(
            QStringList() << "*.json",
            QDir::Files,
            QDir::Name
        );

        foreach (const QString &file, files) {
            result << dir.absoluteFilePath(file);
        }
    }

    return result;
}

void FilePicker::loadFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit loadError("Cannot open file: " + path);
        return;
    }

    QString json = QString::fromUtf8(file.readAll());
    file.close();

    // Parse JSON using QScriptEngine (Qt 4.7 compatible)
    QScriptEngine engine;
    QScriptValue root = engine.evaluate("(" + json + ")");

    if (engine.hasUncaughtException()) {
        emit loadError("Invalid JSON file");
        return;
    }

    // Try "installed" type (Desktop/CLI app)
    QScriptValue installed = root.property("installed");
    QScriptValue web       = root.property("web");

    QScriptValue obj;
    if (installed.isObject())
        obj = installed;
    else if (web.isObject())
        obj = web;
    else {
        emit loadError("Unknown JSON format — expected 'installed' or 'web' key");
        return;
    }

    QString clientId     = obj.property("client_id").toString();
    QString clientSecret = obj.property("client_secret").toString();

    if (clientId.isEmpty()) {
        emit loadError("client_id not found in file");
        return;
    }

    emit credentialsLoaded(clientId, clientSecret);
}
