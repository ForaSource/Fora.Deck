#ifndef XLIBRARY_APPLICATION_H
#define XLIBRARY_APPLICATION_H

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <QProcessEnvironment>
#include <QSet>
#include <QVariantMap>
#include <string>

#include "CatalogTypes.h"

class Application {
private:
    std::uint32_t pid = 0;
    // Copies share a Windows handle or a Linux PID/starttime pair that guards against PID reuse.
    std::shared_ptr<void> process_handle;
    std::string file_title = "";
    std::filesystem::path file_path = "~";
    static std::string read_application_name(const std::filesystem::path &executable_path);
    void set_empty() {
        this->file_title = "";
        this->file_path = "";
    }

public:
    enum class launch_mode { launch, activate, new_instance, elevated };
    static QString target_kind(const app_record &record);
    static bool file_target(const app_record &record);
    static QString process_path(const app_record &record);
    static QString application_path(const app_record &record);
    static void validate_launch_target(const app_record &record);
    static QString launch_key(const app_record &record);
    static QString target_key(const app_record &record);
    static bool target_available(const app_record &record);
    static bool definitely_removed(const app_record &record, const QStringList &known_volume_roots,
                                   const QVariantMap &verified_locations);
    static bool requires_fresh_launch(const app_record &record);
    static QString configured_arguments(const app_record &record);
    static bool valid_launch_profile_id(const QString &profile);
    static QVector<launch_profile> launch_modes(const app_record &record);
    static QVector<launch_profile> launch_profiles(const app_record &record);
    static app_record with_launch_profile(const app_record &record, const QString &profile);
#ifdef Q_OS_WIN
    static bool windows_aumid_available(const QString &aumid);
    static QStringList windows_steam_roots();
#endif
    static QVector<app_record> discover_system(QVector<discovery_issue> &issues, const std::atomic_bool &cancelled,
                                               const std::function<void(const discovery_progress &)> &progress = {});
    static QVector<app_record> discover_steam(QVector<discovery_issue> &issues, const std::atomic_bool &cancelled,
                                              const std::function<void(const discovery_progress &)> &progress = {});
    // Explicit roots also support portable Steam installations without registry entries.
    static QVector<app_record> discover_steam(const QStringList &roots, QVector<discovery_issue> &issues,
                                              const std::atomic_bool &cancelled,
                                              const std::function<void(const discovery_progress &)> &progress = {});
    static void launch_application(const app_record &record, launch_mode mode);
    static QString new_instance_error(const app_record &record);
    static app_record resolve_application(const QString &path);
    static QVector<app_record> discover(bool system, bool user, QVector<discovery_issue> &issues,
                                        const std::atomic_bool &cancelled,
                                        const std::function<void(const discovery_progress &)> &progress = {});
#ifdef __linux__
    static QProcessEnvironment environment();
    static QString icon_name(const QString &path);
    static QSet<quint32> matching_processes(const app_record &record);
    static bool set_autostart(bool enabled, QString &error);
#endif
    std::uint32_t get_pid() const noexcept;
    std::string get_file_title() const { return this->file_title; }
    std::filesystem::path get_file_path() const { return this->file_path; }

    void set_executable(std::filesystem::path const &file_path);
    // On Windows, return 0 if Shell opened the application without an available new process PID.
    std::uint32_t launch();
    std::uint32_t launch(const std::wstring &arguments, const std::filesystem::path &working_directory);

    Application();
    explicit Application(const std::string &file_title, const std::filesystem::path &executable_path);
    explicit Application(const std::filesystem::path &executable_path);

    friend bool operator==(const Application &a, const Application &b) {
        return (a.file_path == b.file_path && a.file_title == b.file_title);
    }
};

std::ostream &operator<<(std::ostream &stream, const Application &application);

#endif //XLIBRARY_APPLICATION_H
