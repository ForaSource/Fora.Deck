#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <qtkeychain/keychain.h>

#include "GithubTokenStore.h"
#include "ProfileSettings.h"

GithubTokenStore::GithubTokenStore(QSettings &settings, QObject *const parent) : QObject(parent), settings(settings) {
}

QString GithubTokenStore::service_name() const {
    // Existing credentials retain their original address until explicitly deleted.
    return settings.value(profile_settings::previous_token_profile).toString().isEmpty() ? "Fora.Deck" : "XLibrary";
}

bool GithubTokenStore::has_token() const {
    return settings.value(profile_settings::token_marker, false).toBool();
}

void GithubTokenStore::start_job(QKeychain::Job *const job, const operation action) {
    if (action != operation::read && settings.property("profile_locked").toBool()) {
        job->deleteLater();
        emit failed(QCoreApplication::translate("Fora.Deck", "The profile is being restored or requires a restart."));
        return;
    }
    if (action != operation::read)
        settings.setProperty("secret_writes", settings.property("secret_writes").toInt() + 1);
    const auto profile = settings.value(profile_settings::previous_token_profile, settings.fileName()).toString();
    const auto address = QFileInfo(profile).absoluteFilePath().toUtf8();
    job->setKey("github-updates/" +
                QString::fromLatin1(QCryptographicHash::hash(address, QCryptographicHash::Sha256).toHex()));
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
                "Fora.Deck",
                "The saved GitHub token was not found. Save a new token or delete it from the update settings."));
            return;
        }
        if (finished->error() != QKeychain::NoError && !(missing && action == operation::remove)) {
            emit busy_changed();
            emit failed(QCoreApplication::translate(
                "Fora.Deck",
                "Could not access the system secret store. Unlock it or check that a secret service is running."));
            return;
        }
        if (action == operation::read) {
            const auto token = static_cast<QKeychain::ReadPasswordJob *>(finished)->textData();
            emit busy_changed();
            emit read_finished(token);
            return;
        }
        settings.setValue(profile_settings::token_marker, action == operation::write);
        if (action == operation::remove)
            settings.remove(profile_settings::previous_token_profile);
        settings.sync();
        emit busy_changed();
        if (settings.status() != QSettings::NoError) {
            emit failed(QCoreApplication::translate(
                "Fora.Deck",
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
    start_job(new QKeychain::ReadPasswordJob(service_name(), this), operation::read);
}

void GithubTokenStore::save(const QString &token) {
    if (busy || token.isEmpty())
        return;
    auto *job = new QKeychain::WritePasswordJob(service_name(), this);
    job->setTextData(token);
    start_job(job, operation::write);
}

void GithubTokenStore::remove() {
    if (!busy)
        start_job(new QKeychain::DeletePasswordJob(service_name(), this), operation::remove);
}

bool GithubTokenStore::pending_changes(const QSettings &settings) {
    return settings.property("secret_writes").toInt() > 0;
}

void GithubTokenStore::lock_profile(QSettings &settings, const bool locked) {
    // Общее состояние всех объектов токена профиля не попадает в INI.
    settings.setProperty("profile_locked", locked);
}
