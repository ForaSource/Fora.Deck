#ifndef XLIBRARY_DATABASE_H
#define XLIBRARY_DATABASE_H

#include <filesystem>
#include <functional>
#include <memory>
#include <QHash>
#include <sqlite3.h>
#include <vector>

#include "Application.h"
#include "CatalogTypes.h"

class Database {
private:
    catalog_snapshot last_removal;
    QHash<qint64, qint64> removal_configurations;
    void write_metadata(qint64 id, const app_record &record);
    void update_filter_tags(const QString &old_name, const QString &new_name);
    void write_source(qint64 configuration_id, const app_record &record);
    void consolidate_discovered_application(qint64 configuration_id, const app_record &record);

    bool drop_on_add_error = true;
    std::filesystem::path file_path;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> connection{nullptr, &sqlite3_close_v2};

    void execute(const char *sql) const;
    void initialize() const;

public:
    static std::filesystem::path get_database_path();
    Database();
    explicit Database(const QString &database_path);
    Database(const std::filesystem::path &path, bool drop_on_add_error);
    explicit Database(const bool drop_on_add_error) : Database() { this->drop_on_add_error = drop_on_add_error; }
    explicit Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    std::filesystem::path setup() const;
    std::filesystem::path get_file_path() const { return this->file_path; }

    bool add_application(const Application &application) const;
    bool add_application(const std::string &file_title, const std::filesystem::path &file_path) const;

    // Совместимый поиск по имени; идентичность при записи определяется конфигурацией запуска.
    bool already_exists(const std::string &file_title) const;
    bool already_exists(const std::filesystem::path &file_path) const;

    sqlite3 *get_connection() const noexcept { return this->connection.get(); }

    std::vector<Application> get_applications() const;
    int schema_version() const;
    void backup_database(const QString &destination) const;
    QVector<path_relocation> preview_relocation(const QString &old_root, const QString &new_root) const;
    void apply_relocation(const QVector<path_relocation> &changes);
    static void validate_application(const app_record &record);
    static QString restore_backup(const QString &source, const QString &destination);
    catalog_snapshot read() const;
    qint64 save_application(const app_record &record);
    void remove_application(qint64 id);
    void remove_applications(const QVector<qint64> &ids);
    bool can_undo_removal() const { return !last_removal.applications.isEmpty(); }
    void undo_removal();
    void update_metadata(const QVector<qint64> &ids, const QStringList &add_tags, const QStringList &remove_tags,
                         int favorite);
    void save_tag(const QString &old_name, const QString &new_name);
    void save_filter(const saved_filter &record);
    void remove_filter(qint64 id);
    void remove_tag(const QString &name);
    qint64 save_scenario(const scenario_record &record);
    void remove_scenario(qint64 id);
    void save_target_shortcut(bool scenario, qint64 id, const QString &sequence);
    void record_launch(qint64 id);
    void clear_history();
    catalog_import_result import_records(const QVector<app_record> &records, discovery_control *control = nullptr,
                                         const std::function<void(qsizetype, qsizetype)> &progress = {},
                                         QVector<discovery_issue> *issues = nullptr,
                                         import_conflict_policy policy = import_conflict_policy::skip);
    QString path() const;
};

#endif //XLIBRARY_DATABASE_H
