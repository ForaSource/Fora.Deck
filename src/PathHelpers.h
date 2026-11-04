#ifndef XLIBRARY_PATH_HELPERS_H
#define XLIBRARY_PATH_HELPERS_H

#include <filesystem>
#include <QDir>
#include <QFileInfo>
#include <QString>

inline std::filesystem::path filesystem_path(const QString &path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    const auto utf8 = path.toUtf8();
    return std::filesystem::path(utf8.constData());
#endif
}

inline QString path_text(const std::filesystem::path &path) {
    const auto utf8 = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char *>(utf8.data()), static_cast<qsizetype>(utf8.size()));
}

inline QString normalized_path(const QString &path) {
    const auto normalized = QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
    return normalized.toCaseFolded();
#else
    return normalized;
#endif
}

inline constexpr Qt::CaseSensitivity path_case_sensitivity =
#ifdef Q_OS_WIN
    Qt::CaseInsensitive;
#else
    Qt::CaseSensitive;
#endif

inline QString effective_working_directory(const QString &path, const QString &directory,
                                           const bool use_default_directory = true) {
#ifdef Q_OS_WIN
    // Some shortcuts incorrectly store the target EXE as their working directory.
    if ((directory.isEmpty() && use_default_directory) ||
        (!directory.isEmpty() && normalized_path(directory) == normalized_path(path)))
        return QFileInfo(path).absolutePath();
#else
    Q_UNUSED(path)
    Q_UNUSED(use_default_directory)
#endif
    return directory;
}

inline QString launch_configuration_key(const QString &path, const QString &arguments, const QString &directory,
                                        const bool use_default_directory = true) {
    // Length prefixes keep arguments containing separators unambiguous.
    const auto part = [](const QString &value) { return QString::number(value.size()) + ':' + value; };
    const auto effective_directory = effective_working_directory(path, directory, use_default_directory);
    return part(normalized_path(path)) + part(arguments) +
           part(effective_directory.isEmpty() ? QString{} : normalized_path(effective_directory));
}

#endif
