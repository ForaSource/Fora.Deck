#include <algorithm>
#include <optional>
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSysInfo>

#include "PlatformIntegration.h"
#include "UpdateChecker.h"

namespace {
constexpr qint64 metadata_limit = 4 * 1024 * 1024;
constexpr qint64 installer_limit = 512 * 1024 * 1024;
constexpr qint64 anonymous_check_interval = 60000;
constexpr qint64 authenticated_check_interval = 5000;
constexpr auto repository_path = "/Lappland-Julia/XLibrary/releases/";

struct release_version {
    QStringList numbers;
    QStringList prerelease;
};

std::optional<release_version> parse_version(const QString &tag) {
    static const QRegularExpression pattern(
        R"(^v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$)");
    const auto match = pattern.match(tag);
    if (!match.hasMatch() || match.capturedLength() != tag.size())
        return std::nullopt;
    release_version result{{match.captured(1), match.captured(2), match.captured(3)}, {}};
    if (!match.captured(4).isEmpty()) {
        result.prerelease = match.captured(4).split('.');
        for (const auto &part : result.prerelease) {
            const bool numeric = std::all_of(part.begin(), part.end(), [](const QChar c) { return c.isDigit(); });
            if (numeric && part.size() > 1 && part.startsWith('0'))
                return std::nullopt;
        }
    }
    return result;
}

int compare_number(const QString &left, const QString &right) {
    // String comparison avoids overflow on large version numbers from external JSON.
    if (left.size() != right.size())
        return left.size() < right.size() ? -1 : 1;
    return QString::compare(left, right, Qt::CaseSensitive);
}

int compare_versions(const release_version &left, const release_version &right) {
    for (qsizetype index = 0; index < 3; ++index) {
        const auto order = compare_number(left.numbers[index], right.numbers[index]);
        if (order != 0)
            return order;
    }
    if (left.prerelease.isEmpty() || right.prerelease.isEmpty())
        return left.prerelease.isEmpty() == right.prerelease.isEmpty() ? 0 : left.prerelease.isEmpty() ? 1 : -1;
    for (qsizetype index = 0; index < qMin(left.prerelease.size(), right.prerelease.size()); ++index) {
        const auto &a = left.prerelease[index];
        const auto &b = right.prerelease[index];
        const bool a_numeric = std::all_of(a.begin(), a.end(), [](const QChar c) { return c.isDigit(); });
        const bool b_numeric = std::all_of(b.begin(), b.end(), [](const QChar c) { return c.isDigit(); });
        const auto order = a_numeric && b_numeric   ? compare_number(a, b)
                           : a_numeric != b_numeric ? (a_numeric ? -1 : 1)
                                                    : QString::compare(a, b, Qt::CaseSensitive);
        if (order != 0)
            return order;
    }
    return left.prerelease.size() == right.prerelease.size()  ? 0
           : left.prerelease.size() < right.prerelease.size() ? -1
                                                              : 1;
}

bool secure_url(const QUrl &url) {
    return url.isValid() && url.scheme() == "https" && url.userInfo().isEmpty() &&
           (url.port() == -1 || url.port() == 443) && !url.hasFragment();
}

bool release_link(const QUrl &url, const QString &tag) {
    return secure_url(url) && url.host() == "github.com" && !url.hasQuery() &&
           url.path() == QString(repository_path) + "tag/" + tag;
}

bool asset_link(const QUrl &url, const QString &tag, const QString &name) {
    return secure_url(url) && url.host() == "github.com" && !url.hasQuery() &&
           url.path() == QString(repository_path) + "download/" + tag + '/' + name;
}

bool download_redirect(const QUrl &url, const QUrl &original) {
    return secure_url(url) && (url == original || url.host() == "release-assets.githubusercontent.com" ||
                               url.host() == "objects.githubusercontent.com");
}

QNetworkRequest request(const QUrl &url) {
    QNetworkRequest result(url);
    result.setRawHeader("User-Agent", "XLibrary/" + QCoreApplication::applicationVersion().toLatin1());
    result.setRawHeader("Accept-Encoding", "identity");
    result.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    result.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    result.setTransferTimeout(15000);
    result.setMaximumRedirectsAllowed(5);
    return result;
}
}

UpdateChecker::UpdateChecker(QObject *const parent) : QObject(parent) {
    qRegisterMetaType<operation_result>();
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, [this] {
        fail(QCoreApplication::translate("XLibrary", "The update request timed out. Please try again."),
             "request_timeout");
    });
}

UpdateChecker::~UpdateChecker() {
    stop_request();
}

void UpdateChecker::stop_request() {
    deadline.stop();
    if (reply) {
        auto *pending = reply;
        reply = nullptr;
        pending->disconnect(this);
        pending->abort();
        pending->deleteLater();
    }
    installer.close();
    response.clear();
    authorization_token.clear();
    download_directory.reset();
}

quint64 UpdateChecker::begin_request(const request_kind kind) {
    stop_request();
    current_request = kind;
    const auto id = ++request_sequence;
    request_id = id;
    cacheable_check = false;
    state = operation_state::loading;
    emit operation_changed({request_id, kind == request_kind::check ? "updates_check" : "updates_download", state, {}});
    emit busy_changed();
    return id;
}

void UpdateChecker::finish_request(const operation_state result, const operation_error &error) {
    state = result;
    emit operation_changed(
        {request_id, current_request == request_kind::check ? "updates_check" : "updates_download", state, error});
    emit busy_changed();
}

void UpdateChecker::fail(const QString &message, const QString &technical_reason, const operation_error_source source) {
    if (!is_busy())
        return;
    const operation_error error{message, technical_reason, source};
    if (current_request == request_kind::check && cacheable_check)
        cache_result(operation_state::error, error);
    stop_request();
    finish_request(operation_state::error, error);
    emit failed(error.user_message);
}

void UpdateChecker::cancel() {
    if (!is_busy()) {
        stop_request();
        return;
    }
    const bool checking = current_request == request_kind::check;
    if (checking && cacheable_check)
        cache_result(operation_state::cancelled);
    stop_request();
    finish_request(operation_state::cancelled);
    if (checking)
        emit check_cancelled();
}

void UpdateChecker::cache_result(const operation_state state, const operation_error &error) {
    cached_check result;
    result.age.start();
    result.release = state == operation_state::completed ? latest_release : QJsonObject{};
    result.state = state;
    result.error = error;
    check_cache.insert(check_key, result);
}

quint64 UpdateChecker::check(const QString &token) {
    if (is_busy())
        return 0;
    const auto id = begin_request(request_kind::check);
    if (request_id != id || !is_busy())
        return id;
    emit check_started();
    if (request_id != id || !is_busy())
        return id;
    const auto trimmed_token = token.trimmed();
    if (trimmed_token.size() > 1024 || !std::all_of(trimmed_token.begin(), trimmed_token.end(), [](const QChar c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        })) {
        fail(QCoreApplication::translate("XLibrary", "The GitHub token contains invalid characters."),
             "invalid_token_characters", operation_error_source::validation);
        return id;
    }
    if (!parse_version(QCoreApplication::applicationVersion())) {
        fail(QCoreApplication::translate("XLibrary", "The current application version is invalid."),
             "invalid_current_version", operation_error_source::validation);
        return id;
    }
    latest_release = {};
    release_tag.clear();
    release_url.clear();
    installer_url.clear();
    installer_digest.clear();
    installer_size = 0;
    release_page = 0;
    check_key = trimmed_token.isEmpty() ? QByteArray{}
                                        : QCryptographicHash::hash(trimmed_token.toUtf8(), QCryptographicHash::Sha256);
    // Reading the cache does not extend its lifetime; remove expired entries along with token fingerprints.
    for (auto it = check_cache.begin(); it != check_cache.end();) {
        const auto interval = it.key().isEmpty() ? anonymous_check_interval : authenticated_check_interval;
        if (it->age.elapsed() >= interval)
            it = check_cache.erase(it);
        else
            ++it;
    }
    const auto cached = check_cache.constFind(check_key);
    if (cached != check_cache.cend()) {
        if (cached->state == operation_state::error) {
            fail(cached->error.user_message, cached->error.technical_reason, cached->error.source);
        } else if (cached->state == operation_state::cancelled) {
            finish_request(operation_state::cancelled);
            emit check_cancelled();
        } else {
            latest_release = cached->release;
            select_release();
        }
        return id;
    }
    cacheable_check = true;
    authorization_token = trimmed_token.toLatin1();
    deadline.start(60000);
    request_releases();
    return id;
}

void UpdateChecker::request_releases() {
    response.clear();
    auto query =
        request(QUrl(QString("https://api.github.com/repos/Lappland-Julia/XLibrary/releases?per_page=100&page=%1")
                         .arg(++release_page)));
    query.setRawHeader("Accept", "application/vnd.github+json");
    query.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    if (!authorization_token.isEmpty())
        query.setRawHeader("Authorization", "Bearer " + authorization_token);
    reply = network.get(query);
    reply->setReadBufferSize(256 * 1024);
    connect(reply, &QNetworkReply::redirected, this, [this](const QUrl &) {
        fail(QCoreApplication::translate("XLibrary", "GitHub returned an unexpected redirect."), "unexpected_redirect");
    });
    connect(reply, &QIODevice::readyRead, this, &UpdateChecker::read_response);
    connect(reply, &QNetworkReply::finished, this, &UpdateChecker::finish_reply);
}

void UpdateChecker::read_response() {
    if (!reply || !reply->isOpen())
        return;
    // An intermediate redirect body is not part of the installer.
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        reply->readAll();
        return;
    }
    while (reply && reply->bytesAvailable() > 0) {
        const auto bytes = reply->read(64 * 1024);
        if (bytes.isEmpty())
            break;
        if (current_request == request_kind::download) {
            if (installer.pos() > installer_size - bytes.size()) {
                fail(QCoreApplication::translate("XLibrary", "The installer size does not match the GitHub release."),
                     "installer_size_mismatch");
                return;
            }
            if (installer.write(bytes) != bytes.size()) {
                fail(
                    QCoreApplication::translate(
                        "XLibrary", "Could not write the downloaded installer. Check free disk space and permissions."),
                    installer.errorString(), operation_error_source::filesystem);
                return;
            }
            download_hash.addData(bytes);
            emit download_progress(installer.pos(), installer_size);
        } else {
            if (response.size() > metadata_limit - bytes.size()) {
                fail(QCoreApplication::translate("XLibrary", "The GitHub response is too large."),
                     "metadata_limit_exceeded");
                return;
            }
            response.append(bytes);
        }
    }
}

void UpdateChecker::finish_reply() {
    read_response();
    if (!reply)
        return;
    auto *finished = reply;
    reply = nullptr;
    finished->deleteLater();
    const auto status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto network_reason =
        QString("network_error=%1; http_status=%2").arg(static_cast<int>(finished->error())).arg(status);
    if (current_request == request_kind::check && status == 401) {
        fail(QCoreApplication::translate(
                 "XLibrary", "GitHub rejected the personal token. Replace or delete it in the update settings."),
             network_reason);
        return;
    }
    if (status == 403 || status == 429) {
        fail(QCoreApplication::translate("XLibrary", "GitHub has limited update requests. Please try again later."),
             network_reason);
        return;
    }
    if (finished->error() != QNetworkReply::NoError || status != 200) {
        fail(
            QCoreApplication::translate(
                "XLibrary", "Could not retrieve the update from GitHub. Check your internet connection and try again."),
            network_reason);
        return;
    }
    if (current_request == request_kind::download) {
        if (installer.pos() != installer_size || download_hash.result() != installer_digest) {
            fail(QCoreApplication::translate("XLibrary",
                                             "Installer verification failed. The file was removed; please try again."),
                 installer.pos() != installer_size ? "installer_size_mismatch" : "installer_digest_mismatch");
            return;
        }
        if (!installer.flush()) {
            fail(QCoreApplication::translate(
                     "XLibrary", "Could not write the downloaded installer. Check free disk space and permissions."),
                 installer.errorString(), operation_error_source::filesystem);
            return;
        }
        installer.close();
        deadline.stop();
        finish_request(operation_state::completed);
        emit installer_ready();
        return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(response, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        fail(QCoreApplication::translate("XLibrary", "GitHub returned an invalid release list."),
             QString("json_error=%1; offset=%2; expected_array").arg(static_cast<int>(error.error)).arg(error.offset));
        return;
    }
    const auto releases = document.array();
    for (const auto &value : releases) {
        if (!value.isObject()) {
            fail(QCoreApplication::translate("XLibrary", "GitHub returned an invalid release list."),
                 "invalid_release_record");
            return;
        }
        const auto release = value.toObject();
        if (!release.value("draft").isBool() || !release.value("prerelease").isBool() ||
            !release.value("tag_name").isString() || !release.value("published_at").isString()) {
            fail(QCoreApplication::translate("XLibrary", "GitHub returned an invalid release list."),
                 "invalid_release_fields");
            return;
        }
        if (release.value("draft").toBool() || release.value("published_at").toString().isEmpty())
            continue;
        const auto tag = release.value("tag_name").toString();
        const auto version = parse_version(tag);
        if (!version)
            continue;
        if (!release_link(QUrl(release.value("html_url").toString()), tag)) {
            fail(QCoreApplication::translate("XLibrary", "GitHub returned an invalid release link."),
                 "invalid_release_link");
            return;
        }
        if (latest_release.isEmpty() ||
            compare_versions(*version, *parse_version(latest_release.value("tag_name").toString())) > 0)
            latest_release = release;
    }
    if (releases.size() == 100) {
        if (release_page >= 20) {
            fail(QCoreApplication::translate("XLibrary",
                                             "The release list is too long. Open GitHub to check for updates."),
                 "release_page_limit_exceeded");
            return;
        }
        request_releases();
        return;
    }
    deadline.stop();
    cache_result();
    authorization_token.clear();
    select_release();
}

void UpdateChecker::select_release() {
    const auto current = parse_version(QCoreApplication::applicationVersion());
    if (!current) {
        fail(QCoreApplication::translate("XLibrary", "The current application version is invalid."),
             "invalid_current_version", operation_error_source::validation);
        return;
    }
    const auto latest = parse_version(latest_release.value("tag_name").toString());
    if (!latest || compare_versions(*latest, *current) <= 0) {
        finish_request(operation_state::completed);
        emit up_to_date(latest.has_value());
        return;
    }
    release_tag = latest_release.value("tag_name").toString();
    release_url = QUrl(latest_release.value("html_url").toString());
#if defined(Q_OS_WIN) && defined(__MINGW32__)
    const QString compiler = "mingw";
#elif defined(Q_OS_WIN) && defined(_MSC_VER)
    const QString compiler = "msvc";
#else
    const QString compiler;
#endif
    auto full_version = release_tag;
    if (full_version.startsWith('v'))
        full_version.remove(0, 1);
    const auto base_name = "XLibrary-" + full_version;
    const auto legacy_base_name = "XLibrary-" + latest->numbers.join('.');
    for (const auto &value : latest_release.value("assets").toArray()) {
        const auto asset = value.toObject();
        const auto name = asset.value("name").toString();
        if (compiler.isEmpty() || QSysInfo::buildCpuArchitecture() != "x86_64" ||
            (name != base_name + "-windows-x64-" + compiler + "-setup.exe" &&
             name != legacy_base_name + "-windows-x64-" + compiler + "-setup.exe" &&
             // Старые сборщики добавляли pre-release независимо от суффикса GitHub-тега.
             name != legacy_base_name + "-pre-release-windows-x64-" + compiler + "-setup.exe"))
            continue;
        const auto url = QUrl(asset.value("browser_download_url").toString());
        const auto digest = asset.value("digest").toString();
        static const QRegularExpression digest_pattern("^sha256:[0-9a-fA-F]{64}$");
        const auto size = asset.value("size").toInteger(-1);
        if (asset.value("state").toString() != "uploaded" || !asset_link(url, release_tag, name) ||
            !digest_pattern.match(digest).hasMatch() || digest.size() != 71 || size <= 0 || size > installer_limit)
            continue;
        // Do not arbitrarily choose among multiple installers serving the same purpose.
        if (!installer_url.isEmpty()) {
            installer_url.clear();
            break;
        }
        installer_url = url;
        installer_digest = QByteArray::fromHex(digest.mid(7).toLatin1());
        installer_size = size;
    }
    finish_request(operation_state::completed);
    emit update_available();
}

bool UpdateChecker::can_install() const {
    return !installer_url.isEmpty() && desktop_platform::installed_for_updates();
}

quint64 UpdateChecker::download() {
    if (is_busy())
        return 0;
    const auto id = begin_request(request_kind::download);
    if (request_id != id || !is_busy())
        return id;
    if (!can_install()) {
        fail(QCoreApplication::translate(
                 "XLibrary",
                 "This installation can no longer be updated automatically. Please open the release on GitHub."),
             "automatic_updates_unavailable", operation_error_source::platform);
        return id;
    }
    download_directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/xlibrary-update-XXXXXX");
    if (!download_directory->isValid()) {
        fail(QCoreApplication::translate("XLibrary", "Could not create a temporary folder for the update."),
             download_directory->errorString(), operation_error_source::filesystem);
        return id;
    }
    installer.setFileName(download_directory->filePath("XLibrary-setup.exe"));
    if (!installer.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        fail(QCoreApplication::translate(
                 "XLibrary", "Could not write the downloaded installer. Check free disk space and permissions."),
             installer.errorString(), operation_error_source::filesystem);
        return id;
    }
    download_hash.reset();
    deadline.start(15 * 60 * 1000);
    emit download_progress(0, installer_size);
    if (request_id != id || !is_busy())
        return id;
    reply = network.get(request(installer_url));
    reply->setReadBufferSize(256 * 1024);
    connect(reply, &QNetworkReply::redirected, this, [this](const QUrl &url) {
        if (download_redirect(url, installer_url))
            reply->redirectAllowed();
        else
            fail(QCoreApplication::translate("XLibrary", "GitHub returned an unexpected redirect."),
                 "unexpected_redirect");
    });
    connect(reply, &QIODevice::readyRead, this, &UpdateChecker::read_response);
    connect(reply, &QNetworkReply::finished, this, &UpdateChecker::finish_reply);
    return id;
}

bool UpdateChecker::launch_installer(QString &error) {
    if (is_busy() || !download_directory || !can_install() || installer.isOpen()) {
        error = QCoreApplication::translate(
            "XLibrary", "This installation can no longer be updated automatically. Please open the release on GitHub.");
        return false;
    }
    if (!desktop_platform::start_update_installer(installer.fileName(), error))
        return false;
    // The installer reads itself after XLibrary exits; keep the temporary file until installation finishes.
    download_directory->setAutoRemove(false);
    return true;
}
