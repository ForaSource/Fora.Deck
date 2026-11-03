#ifndef XLIBRARY_GITHUB_TOKEN_STORE_H
#define XLIBRARY_GITHUB_TOKEN_STORE_H

#include <QObject>
#include <QSettings>

namespace QKeychain {
class Job;
}

class GithubTokenStore : public QObject {
    Q_OBJECT
private:
    enum class operation { read, write, remove };
    QSettings &settings;
    QString key;
    bool busy = false;
    void start_job(QKeychain::Job *job, operation action);

public:
    static bool pending_changes(const QSettings &settings);
    static void lock_profile(QSettings &settings, bool locked);
    explicit GithubTokenStore(QSettings &settings, QObject *parent = nullptr);
    bool has_token() const;
    bool is_busy() const { return busy; }
    void read();
    void save(const QString &token);
    void remove();

signals:
    void busy_changed();
    void read_finished(const QString &token);
    void changed(bool stored);
    void failed(const QString &message);
};

#endif
