#ifndef XLIBRARY_UPDATE_CHECKER_H
#define XLIBRARY_UPDATE_CHECKER_H

#include <memory>
#include <optional>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include "OperationTypes.h"

class QNetworkReply;

class UpdateChecker : public QObject {
    Q_OBJECT
private:
    enum class request_kind { check, download };
    struct cached_check {
        QElapsedTimer age;
        bool authenticated = false;
        QJsonObject release;
        operation_state state = operation_state::completed;
        operation_error error;
    };
    QHash<QByteArray, cached_check> check_cache;
    QByteArray check_key;
    QString update_channel = "stable";
    bool check_authenticated = false;
    bool channel_changing = false;
    QNetworkAccessManager network;
    QTimer deadline;
    QNetworkReply *reply = nullptr;
    QByteArray response;
    QByteArray authorization_token;
    QJsonObject latest_release;
    int release_page = 0;
    operation_state state = operation_state::ready;
    request_kind current_request = request_kind::check;
    quint64 request_sequence = 0;
    quint64 request_id = 0;
    bool cacheable_check = false;
    QString release_tag;
    QUrl release_url;
    QUrl installer_url;
    QByteArray installer_digest;
    qint64 installer_size = 0;
    std::unique_ptr<QTemporaryDir> download_directory;
    QFile installer;
    QCryptographicHash download_hash{QCryptographicHash::Sha256};
    void clear_available_release();
    void request_releases();
    void read_response();
    void finish_reply();
    void select_release();
    void stop_request();
    quint64 begin_request(request_kind kind);
    void finish_request(operation_state result, const operation_error &error = {});
    void fail(const QString &message, const QString &technical_reason,
              operation_error_source source = operation_error_source::updates);
    void cache_result(operation_state result = operation_state::completed, const operation_error &error = {});

public:
    struct release_selection {
        QJsonObject release;
        QString error;
        bool update_available = false;
    };
    // Pure selection used by every API page and by offline callers. Equal versions retain the first record.
    static release_selection select_release_page(const QByteArray &json, const QString &current_version,
                                                 const QString &channel, const QJsonObject &previous = {});
    static std::optional<int> compare_release_versions(const QString &left, const QString &right);
    QString channel() const { return update_channel; }
    void set_channel(const QString &channel);
    explicit UpdateChecker(QObject *parent = nullptr);
    ~UpdateChecker() override;
    bool is_busy() const { return state == operation_state::loading; }
    QString available_version() const { return release_tag; }
    QUrl available_release_url() const { return release_url; }
    bool can_install() const;
    // Нулевой ID означает, что новый запрос не принят, пока выполняется предыдущий.
    quint64 check(const QString &token = {});
    quint64 download();
    void cancel();
    bool launch_installer(QString &error);

signals:
    void operation_changed(const operation_result &result);
    void check_started();
    void channel_changed();
    void check_cancelled();
    void busy_changed();
    void update_available();
    void up_to_date(bool releases_exist);
    void failed(const QString &message);
    void download_progress(qint64 received, qint64 total);
    void installer_ready();
};

#endif
