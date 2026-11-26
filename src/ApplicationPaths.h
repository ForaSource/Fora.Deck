#ifndef FORADECK_APPLICATION_PATHS_H
#define FORADECK_APPLICATION_PATHS_H

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <stdexcept>

namespace application_paths {
inline bool portable_mode() {
    return QFileInfo(QCoreApplication::applicationDirPath() + "/portable.flag").isFile();
}

inline bool bundled_runtime() {
#ifdef Q_OS_LINUX
    if (!portable_mode() || !qEnvironmentVariableIsSet("FORADECK_APPIMAGE"))
        return false;
    const auto app_dir = qEnvironmentVariable("APPDIR");
    const QFileInfo bundled_binary(QDir(app_dir).filePath("usr/bin/foradeck"));
    const auto current_binary = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    // APPIMAGE may belong to another application that launched Fora.Deck.
    if (!QFileInfo(app_dir).isAbsolute() || !QFileInfo(app_dir).isDir() || current_binary.isEmpty() ||
        bundled_binary.canonicalFilePath() != current_binary)
        throw std::runtime_error(
            QCoreApplication::translate("Fora.Deck", "Invalid Fora.Deck AppImage launch paths.").toStdString());
    return true;
#else
    return false;
#endif
}

inline QString appimage_file() {
    if (!bundled_runtime())
        return {};
    const auto image = qEnvironmentVariable("FORADECK_APPIMAGE");
    // A manually extracted AppDir uses the regular portable profile.
    if (image.isEmpty())
        return {};
    const QFileInfo image_info(image);
    if (!image_info.isAbsolute() || !image_info.isFile() || !image_info.isExecutable())
        throw std::runtime_error(
            QCoreApplication::translate("Fora.Deck", "Invalid Fora.Deck AppImage file path.").toStdString());
    return QDir::cleanPath(image);
}
}

#endif
