#ifndef XLIBRARY_CATALOG_STORE_H
#define XLIBRARY_CATALOG_STORE_H

#include <atomic>
#include <functional>
#include <QHash>

#include "catalog_types.h"
#include "Database.h"

class CatalogStore {
private:
    Database database;
    catalog_snapshot last_removal;
    QHash<qint64, qint64> removal_configurations;
    void execute(const char *sql) const;
    void write_metadata(qint64 id, const app_record &record);
    void update_filter_tags(const QString &old_name, const QString &new_name);
    void write_source(qint64 configuration_id, const app_record &record);

public:
    explicit CatalogStore(const QString &database_path = {});
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
    void record_launch(qint64 id);
    void clear_history();
    catalog_import_result import_records(const QVector<app_record> &records, discovery_control *control = nullptr,
                                         const std::function<void(qsizetype, qsizetype)> &progress = {},
                                         QVector<discovery_issue> *issues = nullptr,
                                         import_conflict_policy policy = import_conflict_policy::skip);
    QString path() const;
};

#endif
