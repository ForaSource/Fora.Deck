#ifndef XLIBRARY_PROFILE_BACKUP_H
#define XLIBRARY_PROFILE_BACKUP_H

#include <QByteArray>
#include <QMap>
#include <QMetaType>
#include <QString>

struct profile_manifest {
    int format_version = 0;
    QString application_version;
    QString created_utc;
    int schema_version = 0;
    QMap<QString, qint64> counts;
    QByteArray fingerprint;
};

Q_DECLARE_METATYPE(profile_manifest)

class ProfileBackup {
public:
    static void create(const QString &backup_path, const QString &database_path, const QString &settings_path);
    static profile_manifest inspect(const QString &path);
    static QString restore(const QString &path, const QString &database_path, const QString &settings_path,
                           const QByteArray &fingerprint);
    // Вызывается до открытия QSettings и любых соединений с БД.
    static void recover(const QString &database_path, const QString &settings_path);
};

#endif
