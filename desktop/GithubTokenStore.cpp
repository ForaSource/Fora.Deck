#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <qtkeychain/keychain.h>

#include "GithubTokenStore.h"

namespace {
constexpr auto token_setting = "updates/github_token_saved";
}

GithubTokenStore::GithubTokenStore(QSettings &settings, QObject *const parent) : QObject(parent), settings(settings) {
    // Separate local profiles must not overwrite each other's tokens.
    const auto profile = QFileInfo(settings.fileName()).absoluteFilePath().toUtf8();
    key =
        "github-updates/" + QString::fromLatin1(QCryptographicHash::hash(profile, QCryptographicHash::Sha256).toHex());
}

bool GithubTokenStore::has_token() const {
    return settings.value(token_setting, false).toBool();
}

void GithubTokenStore::start_job(QKeychain::Job *const job, const operation action) {
    if (action != operation::read && settings.property("profile_locked").toBool()) {
        job->deleteLater();
        emit failed(QCoreApplication::translate("XLibrary", "The profile is being restored or requires a restart."));
        return;
    }
    if (action != operation::read)
        settings.setProperty("secret_writes", settings.property("secret_writes").toInt() + 1);
    job->setKey(key);
    job->setInsecureFallback(false);
    busy = true;
    emit busy_changed();
    connect(job, &QKeychain::Job::finished, this, [this, action](QKeychain::Job *const finished) {
        busy = false;
        if (action != operation::read)
            settings.setProperty("secret_writes", settings.property("secret_writes").toInt() - 1);
        const bool missing = finished->error() == QKeychain::EntryNotFound;
        if (missing && action == operation::read) {
            emit busy_changed();
            emit failed(QCoreApplication::translate(
                "XLibrary",
                "The saved GitHub token was not found. Save a new token or delete it from the update settings."));
            return;
        }
        if (finished->error() != QKeychain::NoError && !(missing && action == operation::remove)) {
            emit busy_changed();
            emit failed(QCoreApplication::translate(
                "XLibrary",
                "Could not access the system secret store. Unlock it or check that a secret service is running."));
            return;
        }
        if (action == operation::read) {
            const auto token = static_cast<QKeychain::ReadPasswordJob *>(finished)->textData();
            emit busy_changed();
            emit read_finished(token);
            return;
        }
        settings.setValue(token_setting, action == operation::write);
        settings.sync();
        emit busy_changed();
        if (settings.status() != QSettings::NoError) {
            emit failed(QCoreApplication::translate(
                "XLibrary",
                "The system secret store was updated, but the settings file could not be saved. Please try again."));
            return;
        }
        emit changed(action == operation::write);
    });
    job->start();
}

void GithubTokenStore::read() {
    if (busy)
        return;
    // Without a configured token, checks do not require an available or unlocked credential store.
    if (!has_token()) {
        emit read_finished({});
        return;
    }
    start_job(new QKeychain::ReadPasswordJob("XLibrary", this), operation::read);
}

void GithubTokenStore::save(const QString &token) {
    if (busy || token.isEmpty())
        return;
    auto *job = new QKeychain::WritePasswordJob("XLibrary", this);
    job->setTextData(token);
    start_job(job, operation::write);
}

void GithubTokenStore::remove() {
    if (!busy)
        start_job(new QKeychain::DeletePasswordJob("XLibrary", this), operation::remove);
}

bool GithubTokenStore::pending_changes(const QSettings &settings) {
    return settings.property("secret_writes").toInt() > 0;
}

void GithubTokenStore::lock_profile(QSettings &settings, const bool locked) {
    // Общее состояние всех объектов токена профиля не попадает в INI.
    settings.setProperty("profile_locked", locked);
}
