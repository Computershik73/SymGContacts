#ifndef FILEPICKER_H
#define FILEPICKER_H

#include <QObject>
#include <QStringList>

// Simple file picker — scans common Symbian locations for .json files
// and lets user select one via QML list
class FilePicker : public QObject
{
    Q_OBJECT
public:
    explicit FilePicker(QObject *parent = 0);

    // Scans E:\ and C:\Data for .json files, returns list
    Q_INVOKABLE QStringList findJsonFiles();

    // Reads and parses client_secret.json, emits credentialsLoaded
    Q_INVOKABLE void loadFile(const QString &path);

signals:
    void credentialsLoaded(const QString &clientId, const QString &clientSecret);
    void loadError(const QString &message);
};

#endif // FILEPICKER_H
