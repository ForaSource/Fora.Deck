#ifndef XLIBRARY_PROFILE_SETTINGS_H
#define XLIBRARY_PROFILE_SETTINGS_H

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVariant>

namespace profile_settings {
using values = QMap<QString, QVariant>;
inline constexpr auto token_marker = "updates/github_token_saved";
values read(const QString &path);
void write(const QString &path, const values &settings);
QByteArray encode(const QVariant &value);
QVariant decode(const QByteArray &json);
void validate_keys(const values &settings);
}

#endif
