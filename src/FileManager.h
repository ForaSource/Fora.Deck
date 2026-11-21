#ifndef XLIBRARY_FILE_MANAGER_H
#define XLIBRARY_FILE_MANAGER_H

#include <atomic>
#include <functional>
#include <memory>
#include <QCache>
#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

#include "CatalogTypes.h"
#include "SearchQuery.h"

struct file_match {
    QString path;
    QString name;
    int rank = 0;
    bool saved = false;
    bool available = true;
    qint64 use_count = 0;
    qint64 last_used = 0;
    int location = 2;
    int depth = 0;
    QStringList tags{};
    bool favorite = false;
    qint64 modified = 0;
    qint64 size = -1;
    QString extension{};
    qint64 added = 0;
    QString volume_root{};
    QString volume_device{};
};
Q_DECLARE_METATYPE(QVector<file_match>)

enum class file_search_mode { off, basic, full };

struct file_index_source {
    QString root;
    int depth = -1;
    QString volume_root;
    QString volume_device;
    qint64 attempted = 0;
    qint64 completed = 0;
    QString state;
    QStringList unavailable_directories;
};

struct file_save_result {
    // Inspection returns paths eligible to add. Saving returns paths committed by this request.
    QStringList added;
    QStringList already_saved;
    QHash<QString, QString> rejected;
    QString error;
};

class FileManager : public QObject {
    Q_OBJECT
    QString database_path;
    QStringList custom_roots;
    QStringList excluded_paths;
    file_search_mode mode = file_search_mode::off;
    QString scope;
    QVariantMap metadata;
    QThreadPool scan_pool;
    QThreadPool query_pool;
    // Accessed only by the single query worker; duplicate filenames share prepared search data.
    std::shared_ptr<QCache<QString, SearchQuery::SearchableData>> search_cache =
        std::make_shared<QCache<QString, SearchQuery::SearchableData>>(20000);
    QTimer refresh;
    QTimer progress_timer;
    QTimer source_poll;
    bool checking_sources = false;
    std::shared_ptr<std::atomic<quint64>> scan_generation = std::make_shared<std::atomic<quint64>>(0);
    std::shared_ptr<std::atomic<quint64>> query_generation = std::make_shared<std::atomic<quint64>>(0);
    std::shared_ptr<std::atomic<quint64>> indexed = std::make_shared<std::atomic<quint64>>(0);
    bool scanning = false;
    int pending_saves = 0;
    bool rescan_pending = false;
    bool pending_volume_change = false;
    QString state;
    qint64 last_refresh = 0;
    QVector<file_index_source> source_states;
    void load_source_states();
    void start_scan(const QStringList &only_roots = {}, bool allow_volume_change = false);
    void check_sources();
    void schedule_refresh();
    void update_saved_files(QStringList paths, bool saved, QObject *receiver,
                            std::function<void(file_save_result)> completed = {});

public:
    explicit FileManager(const QString &profile_directory, const QStringList &saved_paths = {},
                         QObject *parent = nullptr);
    ~FileManager() override;
    void configure(file_search_mode selected_mode, const QStringList &roots = {}, const QStringList &exclusions = {});
    bool is_enabled() const { return mode != file_search_mode::off; }
    static file_search_mode mode_from_setting(const QString &value);
    void rescan();
    void shutdown();
    QStringList persisted_saved_paths() const;
    void search(const QString &text, quint64 request, QObject *receiver,
                std::function<void(quint64, QVector<file_match>)> completed,
                std::shared_ptr<std::atomic<quint64>> field_generation, const QHash<QString, usage_record> &usage = {},
                const QStringList &tags = {}, bool favorites_only = false, int limit = 30, int sort_mode = 0);
    static QVector<file_match> filter_matches(const QString &text, QVector<file_match> matches);
    static int compare_file_order(const QString &left_extension, qint64 left_added,
                                  const QString &right_extension, qint64 right_added, int sort_mode);
    void directory_dates(const QString &request, QObject *receiver,
                         std::function<void(QHash<QString, qint64>)> completed);
    void saved_files(QObject *receiver, std::function<void(QVector<file_match>)> completed);
    void set_saved(const QString &path, bool saved);
    void inspect_saved_files(QStringList paths, QObject *receiver,
                             std::function<void(file_save_result)> completed);
    void save_files(QStringList paths, QObject *receiver, std::function<void(file_save_result)> completed);
    static QString saved_path_identity(const QString &path);
    void restore_metadata(const QVariantMap &values);
    const QVariantMap &metadata_values() const { return metadata; }
    QStringList tags_for(const QString &path) const;
    bool is_favorite(const QString &path) const;
    void update_metadata(const QString &path, const QStringList &tags, bool favorite);
    QStringList tag_names() const;
    void rename_tag(const QString &old_name, const QString &new_name);
    bool is_saving() const { return pending_saves > 0; }
    QString status() const { return state; }
    QString diagnostics_text() const;
    const QVector<file_index_source> &sources() const { return source_states; }
    static bool supports_file_extension(const QString &extension);
    static bool accepts_file(const QString &path);
    static bool can_open(const QString &path, const QString &volume_root = {}, const QString &volume_device = {});
    static QStringList volume_roots();
signals:
    void changed();
    void configuration_changed();
    void busy_changed();
    void saved_paths_changed(const QStringList &paths);
    void metadata_changed();
    void status_changed();
    void error(const QString &message);
};
#endif
