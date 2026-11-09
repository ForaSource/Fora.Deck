#ifndef XLIBRARY_CATALOG_TYPES_H
#define XLIBRARY_CATALOG_TYPES_H

#include <atomic>
#include <QHash>
#include <QMetaType>
#include <QStringList>
#include <QVector>
#include <stdexcept>

namespace catalog_limits {
inline constexpr qint64 json_bytes = 32 * 1024 * 1024;
inline constexpr qsizetype applications = 50000;
inline constexpr qsizetype text_length = 32768;
inline constexpr qsizetype tags_per_application = 256;
}

inline bool valid_tag_name(const QString &tag) {
    if (tag.isEmpty() || tag.size() > 64)
        return false;
    for (const auto character : tag)
        if (character.isNull() || character.isSpace() || character == ',' || character == '#')
            return false;
    return true;
}

inline QString tag_name_key(const QString &tag) {
    return tag.normalized(QString::NormalizationForm_C).toCaseFolded();
}

inline bool has_tag(const QStringList &tags, const QString &tag) {
    const auto key = tag_name_key(tag);
    for (const auto &name : tags)
        if (tag_name_key(name) == key)
            return true;
    return false;
}

struct app_record {
    qint64 id = 0;
    qint64 target_id = 0;
    qint64 configuration_id = 0;
    QString name;
    QString path;
    QString target_kind;
    QString installation_directory;
    QString executable_path;
    QString icon_path;
    QString arguments;
    QString default_launch_profile;
    QString working_directory;
    QStringList tags;
    QStringList aliases;
    bool favorite = false;
    bool available = true;
    bool volume_unavailable = false;
    bool running = false;
    QString new_instance_error;
    QString discovery_source;
    QStringList discovery_sources;
    qint64 last_launch = 0;
    qint64 launch_count = 0;
};

struct usage_record {
    qint64 count = 0;
    qint64 last_used = 0;
};

struct discovery_progress {
    int percent = -1;
    qsizetype processed = 0;
    qsizetype total = 0;
    QString stage;
};

enum class discovery_issue_kind { skipped, duplicate, error };

struct discovery_issue {
    QString source;
    QString reason;
    discovery_issue_kind kind = discovery_issue_kind::error;
};

struct catalog_import_result {
    qsizetype added = 0;
    qsizetype duplicates = 0;
    qsizetype updated = 0;
};

enum class import_conflict_policy { skip, replace_metadata };

enum class discovery_phase { running, cancelling, committing };

struct discovery_control {
    std::atomic_bool requested = false;
    std::atomic<discovery_phase> phase = discovery_phase::running;
};

class DiscoveryCancelled : public std::runtime_error {
public:
    DiscoveryCancelled() : std::runtime_error("Discovery cancelled") {}
};

struct scenario_step {
    qint64 application_id = 0;
    int delay_ms = 0;
    bool enabled = true;
    QString launch_profile;
};

struct launch_profile {
    QString id;
    QString name;
    QString kind;
    QString icon_path;
};

struct scenario_record {
    qint64 id = 0;
    QString name;
    QVector<scenario_step> steps;
};

struct saved_filter {
    qint64 id = 0;
    QString name;
    QString query;
    QString section = "library";
    QString tag;
    int sort_mode = 0;
    Qt::SortOrder sort_order = Qt::AscendingOrder;
};

struct target_shortcut {
    bool scenario = false;
    qint64 target_id = 0;
    QString sequence;
};

struct catalog_snapshot {
    QVector<app_record> applications;
    QHash<QString, usage_record> file_usage;
    QStringList tags;
    QVector<scenario_record> scenarios;
    QVector<saved_filter> filters;
    QVector<target_shortcut> target_shortcuts;
};

struct path_relocation {
    qint64 application_id = 0;
    QString name;
    QString old_path;
    QString new_path;
    QString old_directory;
    QString new_directory;
    QString issue;
    bool uses_existing_configuration = false;
};

Q_DECLARE_METATYPE(app_record)
Q_DECLARE_METATYPE(QVector<app_record>)
Q_DECLARE_METATYPE(QVector<discovery_issue>)
Q_DECLARE_METATYPE(scenario_record)
Q_DECLARE_METATYPE(catalog_snapshot)
Q_DECLARE_METATYPE(path_relocation)
Q_DECLARE_METATYPE(QVector<path_relocation>)

#endif
