#ifndef XLIBRARY_FILE_MANAGER_H
#define XLIBRARY_FILE_MANAGER_H

#include <atomic>
#include <functional>
#include <memory>
#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVector>

struct file_match {
    QString path;
    QString name;
    int rank = 0;
    bool saved = false;
    bool available = true;
};
Q_DECLARE_METATYPE(QVector<file_match>)

class FileManager : public QObject {
    Q_OBJECT
    QString database_path;
    QStringList custom_roots;
    QString scope;
    QThreadPool scan_pool;
    QThreadPool query_pool;
    QTimer refresh;
    QTimer progress_timer;
    std::shared_ptr<std::atomic<quint64>> scan_generation = std::make_shared<std::atomic<quint64>>(0);
    std::shared_ptr<std::atomic<quint64>> query_generation = std::make_shared<std::atomic<quint64>>(0);
    std::shared_ptr<std::atomic<quint64>> indexed = std::make_shared<std::atomic<quint64>>(0);
    bool scanning = false;
    int pending_saves = 0;
    bool rescan_pending = false;
    QString state;
    void start_scan();

public:
    explicit FileManager(const QString &profile_directory, const QStringList &saved_paths = {},
                         QObject *parent = nullptr);
    ~FileManager() override;
    void configure(const QStringList &roots);
    void rescan();
    void shutdown();
    QStringList persisted_saved_paths() const;
    void search(const QString &text, quint64 request, QObject *receiver,
                std::function<void(quint64, QVector<file_match>)> completed,
                std::shared_ptr<std::atomic<quint64>> field_generation);
    void saved_files(QObject *receiver, std::function<void(QVector<file_match>)> completed);
    void set_saved(const QString &path, bool saved);
    bool is_saving() const { return pending_saves > 0; }
    QString status() const { return state; }
    static bool accepts_file(const QString &path);
    static QStringList volume_roots();
signals:
    void changed();
    void index_updated();
    void busy_changed();
    void saved_paths_changed(const QStringList &paths);
    void status_changed();
    void error(const QString &message);
};
#endif
