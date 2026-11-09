#include <algorithm>
#include <cctype>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QScopeGuard>
#include <stdexcept>

#include "LaunchArguments.h"
#include "Manager.h"

Q_LOGGING_CATEGORY(poll_log, "xlibrary.poll", QtWarningMsg)

std::string Manager::to_lower(std::string data) {
    std::ranges::transform(data, data.begin(), [](const unsigned char c) { return std::tolower(c); });
    return data;
}

bool Manager::app_exists(const Application &application) const {
    for (auto &app : this->get_apps()) {
        if (app == application) {
            return true;
        }
    }
    return false;
}

void Manager::add_application(const Application &application) {
    if (!this->app_exists(application)) {
        app_record record;
        record.name = QString::fromStdString(application.get_file_title());
        record.path = path_text(application.get_file_path());
        catalog.applications.append(std::move(record));
        applications.push_back(application);
    }
}

void Manager::add_applications(const std::vector<Application> &applications) {
    for (auto &app : applications) {
        this->add_application(app);
    }
}

std::uint32_t Manager::launch(const std::string &title) const {
    for (auto &app : applications) {
        if (this->to_lower(app.get_file_title()) == this->to_lower(title)) {
            return app.launch();
        }
    }
    throw std::invalid_argument(title + " is not in this manager");
}

Heartbeat Manager::heartbeat(const std::string &title, const std::uint32_t process_id) const {
    for (const auto &app : this->get_apps()) {
        if (this->to_lower(app.get_file_title()) == this->to_lower(title)) {
            return Heartbeat(process_id, app.get_file_path());
        }
    }
    throw std::invalid_argument(title + " is not in this manager");
}

std::vector<Application> Manager::get_apps() const {
    std::vector<Application> result;
    result.reserve(catalog.applications.size());
    for (const auto &record : catalog.applications) {
        if (!Application::file_target(record))
            continue;
        const auto found = std::ranges::find_if(std::as_const(applications), [&](const auto &app) {
            return app.get_file_title() == record.name.toStdString() &&
                   app.get_file_path() == filesystem_path(record.path);
        });
        if (found != applications.cend())
            result.push_back(*found);
        else
            result.emplace_back(record.name.toStdString(), filesystem_path(record.path));
    }
    applications = std::move(result);
    return applications;
}

std::vector<std::string> Manager::get_names() const {
    std::vector<std::string> result{};
    for (auto &app : this->get_apps()) {
        result.push_back(app.get_file_title());
    }
    return result;
}

std::vector<std::filesystem::path> Manager::get_paths() const {
    std::vector<std::filesystem::path> result{};
    for (auto &app : this->get_apps()) {
        result.push_back(app.get_file_path());
    }
    return result;
}

void Manager::initialize(const quint64 id) {
    try {
        if (monitor) {
            monitor->stop();
            delete monitor;
            monitor = nullptr;
        }
        if (database_path.isEmpty())
            database_path = path_text(Database::get_database_path());
        emit location_changed(database_path);
        store.reset();
        store = std::make_unique<Database>(database_path);
        emit schema_changed(store->schema_version());
        refresh();
        monitor = new QTimer(this);
        monitor->setInterval(2500);
        connect(monitor, &QTimer::timeout, this, [this] { poll(); });
        monitor->start();
        emit result({id, "library", operation_state::ready, {}});
    } catch (const std::exception &error) {
        store.reset();
        sqlite3 *raw = nullptr;
        if (sqlite3_open_v2(database_path.toUtf8().constData(), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            try {
                emit schema_changed(xlibrary::read_schema_version(raw));
            } catch (...) {
                // Повреждённая БД не должна скрывать исходную ошибку открытия.
            }
        }
        if (raw)
            sqlite3_close_v2(raw);
        emit result(
            {id, "library", operation_state::error, exception_error("library", error, operation_error_source::sqlite)});
    }
}

void Manager::restore(const quint64 id, const QString &path) {
    try {
        if (database_path.isEmpty())
            database_path = path_text(Database::get_database_path());
        store.reset();
        const auto preserved = Database::restore_backup(path, database_path);
        initialize(id);
        if (store)
            emit notification(
                preserved.isEmpty()
                    ? QCoreApplication::translate("XLibrary", "Library restored.")
                    : QCoreApplication::translate("XLibrary", "Library restored. Previous files: %1").arg(preserved));
    } catch (const std::exception &error) {
        emit result({id, "library", operation_state::error,
                     exception_error("library", error, operation_error_source::filesystem)});
    }
}

void Manager::refresh() {
    catalog = store->read();
    for (auto &record : catalog.applications) {
        record.running = last_running.contains(normalized_path(Application::process_path(record)));
    }
    emit loaded(catalog);
    poll(true);
}

void Manager::poll(const bool force) {
    const bool measured = poll_log().isDebugEnabled();
    QElapsedTimer elapsed;
    if (measured)
        elapsed.start();
    const auto report = qScopeGuard([&] {
        if (measured)
            qCDebug(poll_log).nospace() << "poll records=" << catalog.applications.size()
                                        << " elapsed_us=" << elapsed.nsecsElapsed() / 1000 << " force=" << force;
    });
    const bool check_availability = force || availability_invalidated || !availability_age.isValid() ||
                                    availability_age.elapsed() >= availability_interval_ms;
    QSet<QString> unavailable = check_availability ? QSet<QString>{} : last_unavailable;
    QSet<QString> missing_volumes = check_availability ? QSet<QString>{} : last_missing_volumes;
    const auto previous_roots = known_volume_roots;
    bool locations_changed = false;
    const auto observe_location = [&](const QString &path) {
        if (!QFileInfo(path).isAbsolute() || !QFileInfo::exists(path))
            return;
        const QStorageInfo volume(path);
        if (!volume.isValid() || !volume.isReady() || volume.device().isEmpty())
            return;
        const QStringList location{normalized_path(volume.rootPath()), QString::fromLatin1(volume.device().toHex())};
        const auto key = normalized_path(path);
        // Do not replace a known mount with its surviving parent after a disk is disconnected.
        if (!verified_locations.contains(key)) {
            verified_locations.insert(key, location);
            locations_changed = true;
        }
    };
    bool capabilities_changed = false;
    auto paths = last_running;
    try {
        if (check_availability) {
            for (auto &record : catalog.applications) {
                if (const auto error = Application::new_instance_error(record); error != record.new_instance_error) {
                    record.new_instance_error = error;
                    capabilities_changed = true;
                }
                record.available = Application::target_available(record);
                if (Application::file_target(record)) {
                    observe_location(record.path);
                    if (Application::target_kind(record) == "desktop")
                        observe_location(record.executable_path);
                } else if (Application::target_kind(record) == "steam") {
                    observe_location(record.installation_directory);
                    for (const auto &source : record.discovery_sources)
                        observe_location(source);
                    observe_location(record.discovery_source);
                }
                record.volume_unavailable = false;
                if (!Application::file_target(record)) {
                    if (Application::target_kind(record) == "steam" && record.available) {
                        const QStorageInfo volume(record.installation_directory);
                        const auto root = QDir::fromNativeSeparators(volume.rootPath());
                        if (volume.isValid() && !root.isEmpty() && root != "/" && !known_volume_roots.contains(root))
                            known_volume_roots.append(root);
                    }
                    if (!record.available)
                        unavailable.insert(Application::launch_key(record));
                    continue;
                }
                if (record.available) {
                    const QStorageInfo volume(record.path);
                    if (const auto root = QDir::fromNativeSeparators(volume.rootPath());
                        volume.isValid() && !root.isEmpty() && root != "/" && !known_volume_roots.contains(root))
                        known_volume_roots.append(root);
                } else {
                    auto roots = known_volume_roots;
#ifdef Q_OS_WIN
                    if (const auto path = QDir::fromNativeSeparators(record.path); path.size() >= 3 && path[1] == ':')
                        roots.append(path.left(3));
                    else if (path.startsWith("//")) {
                        if (const auto end = path.indexOf('/', path.indexOf('/', 2) + 1); end > 2)
                            roots.append(path.left(end));
                    }
#endif
                    for (const auto &root : roots) {
                        if (const auto prefix = root.endsWith('/') ? root : root + '/';
                            !record.path.startsWith(prefix, path_case_sensitivity))
                            continue;
                        if (const QStorageInfo volume(root);
                            !volume.isValid() || !volume.isReady() ||
                            normalized_path(volume.rootPath()) != normalized_path(root)) {
                            record.volume_unavailable = true;
                            missing_volumes.insert(normalized_path(record.path));
                            break;
                        }
                    }
                }
                if (!record.available)
                    unavailable.insert(normalized_path(record.path));
            }
            if (!availability_watcher) {
                availability_watcher = std::make_unique<QFileSystemWatcher>(this);
                connect(availability_watcher.get(), &QFileSystemWatcher::directoryChanged, this,
                        [this] { availability_invalidated = true; });
            }
            QSet<QString> watched;
            const auto watch_parent = [&](const QString &path) {
                if (watched.size() < availability_watch_limit && QFileInfo(path).isAbsolute()) {
                    const auto parent = QFileInfo(path).absolutePath();
                    if (QDir(parent).exists())
                        watched.insert(parent);
                }
            };
            // Parent directories observe deletion/recreation; mount parents observe many removable-volume events.
            for (const auto &root : known_volume_roots)
                watch_parent(root);
            for (const auto &record : catalog.applications) {
                watch_parent(Application::file_target(record) ? record.path : record.installation_directory);
                if (watched.size() >= availability_watch_limit)
                    break;
            }
            const auto previous = availability_watcher->directories();
            const QSet<QString> old(previous.cbegin(), previous.cend());
            const auto removed = (old - watched).values();
            const auto added = (watched - old).values();
            if (!removed.isEmpty())
                availability_watcher->removePaths(removed);
            if (!added.isEmpty())
                availability_watcher->addPaths(added);
            availability_invalidated = false;
            availability_age.restart();
        }
        paths = Heartbeat::running_paths(catalog.applications);
        if (monitor_state == operation_state::error)
            emit result({monitor_request_id, "monitor", operation_state::ready, {}});
        monitor_state = operation_state::ready;
    } catch (const std::exception &error) {
        availability_invalidated = true;
        if (monitor_state != operation_state::error) {
            monitor_request_id = ++*request_sequence;
            emit result({monitor_request_id, "monitor", operation_state::error,
                         exception_error("monitor", error, operation_error_source::platform)});
        }
        monitor_state = operation_state::error;
        return;
    }
    for (auto &record : catalog.applications)
        record.running = paths.contains(normalized_path(Application::process_path(record)));
    if (known_volume_roots != previous_roots)
        emit volumes_observed(known_volume_roots);
    if (locations_changed)
        emit locations_observed(verified_locations);
    if (force || capabilities_changed || paths != last_running || unavailable != last_unavailable ||
        missing_volumes != last_missing_volumes) {
        last_running = paths;
        last_unavailable = unavailable;
        last_missing_volumes = missing_volumes;
        emit state_changed(catalog.applications);
    }
}

void Manager::save_application(const app_record &record) {
    const auto current = std::ranges::find_if(std::as_const(this->catalog.applications),
                                              [&](const app_record &item) { return item.id == record.id; });
    if (Application::file_target(record) &&
        (record.id == 0 || (current != this->catalog.applications.cend() && current->path != record.path)))
        Application::resolve_application(record.path);
    const auto id = this->store->save_application(record);
    this->refresh();
    emit this->application_saved(this->request_id, id);
}

void Manager::resolve_application(const QString &path) {
    emit this->resolved(this->request_id, path, Application::resolve_application(path));
}

void Manager::remove_applications(const QVector<qint64> &ids, const bool remember) {
    if (ids.isEmpty())
        return;
    QStringList dismissed;
    if (remember) {
        const QSet<qint64> selected(ids.cbegin(), ids.cend());
        for (const auto &record : this->store->read().applications)
            if (selected.contains(record.id))
                dismissed.append(Application::target_key(record));
    }
    this->store->remove_applications(ids);
    if (!dismissed.isEmpty())
        emit discovery_dismissed(dismissed);
    emit this->undo_changed(this->store->can_undo_removal());
    this->refresh();
}

void Manager::undo_removal() {
    this->store->undo_removal();
    emit this->undo_changed(this->store->can_undo_removal());
    this->refresh();
    emit this->notification(
        QCoreApplication::translate("XLibrary", "Removed applications and scenario links restored."));
}

void Manager::update_metadata(const QVector<qint64> &ids, const QStringList &add_tags, const QStringList &remove_tags,
                              const int favorite) {
    this->store->update_metadata(ids, add_tags, remove_tags, favorite);
    this->refresh();
}

void Manager::save_filter(const saved_filter &record) {
    this->store->save_filter(record);
    this->refresh();
}

void Manager::remove_filter(const qint64 id) {
    this->store->remove_filter(id);
    this->refresh();
}

void Manager::save_tag(const QString &old_name, const QString &new_name) {
    this->store->save_tag(old_name, new_name);
    emit this->tag_renamed(old_name, new_name);
    this->refresh();
}

void Manager::remove_tag(const QString &name) {
    this->store->remove_tag(name);
    emit this->tag_renamed(name, {});
    this->refresh();
}

void Manager::save_scenario(const scenario_record &record) {
    const auto id = this->store->save_scenario(record);
    this->refresh();
    emit this->scenario_saved(this->request_id, id);
}

void Manager::remove_scenario(const qint64 id) {
    this->store->remove_scenario(id);
    this->refresh();
}

void Manager::export_catalog(const QString &path) {
    const auto too_large = [] {
        return std::invalid_argument(
            QCoreApplication::translate("XLibrary",
                                        "The JSON catalog exceeds 32 MiB or 50000 applications. Export was not saved. "
                                        "Use a database backup for a larger library.")
                .toStdString());
    };
    if (this->catalog.applications.size() > catalog_limits::applications)
        throw too_large();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "Could not open the export file.").toStdString());
    qint64 written = 0;
    const auto write = [&](const QByteArray &contents) {
        if (contents.size() > catalog_limits::json_bytes - written)
            throw too_large();
        if (file.write(contents) != contents.size())
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Could not save the export.").toStdString());
        written += contents.size();
    };
    write("{\"format\":1,\"applications\":[");
    bool first = true;
    for (const auto &record : this->catalog.applications) {
        try {
            Database::validate_application(record);
        } catch (const std::invalid_argument &error) {
            throw std::invalid_argument(QCoreApplication::translate("XLibrary", "Cannot export %1: %2")
                                            .arg(record.path, QString::fromUtf8(error.what()))
                                            .toStdString());
        }
        const auto contents = QJsonDocument(QJsonObject{{"name", record.name},
                                                        {"path", record.path},
                                                        {"target_kind", Application::target_kind(record)},
                                                        {"installation_directory", record.installation_directory},
                                                        {"executable_path", record.executable_path},
                                                        {"icon_path", record.icon_path},
                                                        {"arguments", record.arguments},
                                                        {"default_launch_profile", record.default_launch_profile},
                                                        {"working_directory", record.working_directory},
                                                        {"favorite", record.favorite},
                                                        {"tags", QJsonArray::fromStringList(record.tags)},
                                                        {"aliases", QJsonArray::fromStringList(record.aliases)}})
                                  .toJson(QJsonDocument::Compact);
        if (!first)
            write(",");
        first = false;
        write(contents);
    }
    write("]}\n");
    if (!file.commit())
        throw std::runtime_error(QCoreApplication::translate("XLibrary", "Could not save the export.").toStdString());
    emit this->notification(QCoreApplication::translate("XLibrary", "Library exported."));
}

void Manager::import_catalog(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "Could not read the import file.").toStdString());
    const auto contents = file.read(catalog_limits::json_bytes + 1);
    if (file.error() != QFileDevice::NoError)
        throw std::runtime_error(file.errorString().toStdString());
    this->error_source = operation_error_source::validation;
    if (contents.size() > catalog_limits::json_bytes)
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "The JSON catalog exceeds 32 MiB. Import cancelled.")
                .toStdString());
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(contents, &error);
    if (error.error != QJsonParseError::NoError)
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "JSON error at offset %1: %2. Import cancelled.")
                .arg(error.offset)
                .arg(error.errorString())
                .toStdString());
    const auto root = document.object();
    if (!document.isObject())
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "Field root: expected a JSON object. Import cancelled.")
                .toStdString());
    if (!root.value("format").isDouble() || root.value("format").toDouble() != 1)
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "Field format: only version 1 is supported. Import cancelled.")
                .toStdString());
    if (!root.value("applications").isArray())
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "Field applications: expected an array. Import cancelled.")
                .toStdString());
    const auto applications = root.value("applications").toArray();
    if (applications.size() > catalog_limits::applications)
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "The JSON catalog exceeds 50000 applications. Import cancelled.")
                .toStdString());
    QVector<app_record> records;
    for (const auto &value : applications) {
        const auto fail = [&](const QString &field, const QString &reason) {
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary",
                                            "Record %1, field %2: %3. Import cancelled; changes were not saved.")
                    .arg(records.size() + 1)
                    .arg(field, reason)
                    .toStdString());
        };
        if (!value.isObject())
            fail("applications[]", QCoreApplication::translate("XLibrary", "expected an object"));
        const auto object = value.toObject();
        const auto text = [&](const QString &field, const bool required = false) {
            const auto item = object.value(field);
            if ((!item.isUndefined() || required) && !item.isString())
                fail(field, QCoreApplication::translate("XLibrary", "expected a string"));
            const auto result = item.toString();
            if (result.contains(QChar::Null))
                fail(field, QCoreApplication::translate("XLibrary", "null characters are not allowed"));
            return result;
        };
        app_record record;
        record.name = text("name", true).trimmed();
        record.path = QDir::fromNativeSeparators(text("path", true));
        record.target_kind = text("target_kind");
        record.installation_directory = text("installation_directory");
        record.executable_path = text("executable_path");
        record.icon_path = text("icon_path");
        record.arguments = text("arguments");
        record.default_launch_profile = text("default_launch_profile");
        record.working_directory = text("working_directory");
        if (object.contains("favorite") && !object.value("favorite").isBool())
            fail("favorite", QCoreApplication::translate("XLibrary", "expected a boolean"));
        record.favorite = object.value("favorite").toBool();
        if (object.contains("tags") && !object.value("tags").isArray())
            fail("tags", QCoreApplication::translate("XLibrary", "expected an array of strings"));
        const auto tags = object.value("tags").toArray();
        if (tags.size() > catalog_limits::tags_per_application)
            fail("tags", QCoreApplication::translate("XLibrary", "at most 256 tags are allowed"));
        qsizetype tag_index = 0;
        for (const auto &tag : tags) {
            const auto field = QString("tags[%1]").arg(++tag_index);
            if (!tag.isString())
                fail(field, QCoreApplication::translate("XLibrary", "expected a string"));
            const auto name = tag.toString().trimmed();
            if (!valid_tag_name(name))
                fail(field, QCoreApplication::translate("XLibrary", "a tag must contain 1 to 64 characters, without "
                                                                    "spaces, commas, #, or null characters"));
            record.tags.append(name);
        }
        record.tags.removeDuplicates();
        if (object.contains("aliases") && !object.value("aliases").isArray())
            fail("aliases", QCoreApplication::translate("XLibrary", "expected an array of strings"));
        const auto aliases = object.value("aliases").toArray();
        if (aliases.size() > 32)
            fail("aliases", QCoreApplication::translate(
                                "XLibrary", "Use up to 32 aliases, each on one line with up to 256 characters."));
        for (const auto &alias : aliases) {
            if (!alias.isString() || alias.toString().trimmed().isEmpty() || alias.toString().size() > 256 ||
                alias.toString().contains(QChar::Null) || alias.toString().contains('\n') ||
                alias.toString().contains('\r'))
                fail("aliases", QCoreApplication::translate(
                                    "XLibrary", "Use up to 32 aliases, each on one line with up to 256 characters."));
            if (const auto name = alias.toString().trimmed(); !record.aliases.contains(name, Qt::CaseInsensitive))
                record.aliases.append(name);
        }
        if (record.name.isEmpty() || record.name.size() > 256)
            fail("name", QCoreApplication::translate("XLibrary", "the name must contain 1 to 256 characters"));
#ifdef Q_OS_WIN
        if (Application::file_target(record) && QFileInfo(record.path).suffix().isEmpty())
            fail("path", QCoreApplication::translate("XLibrary", "a Windows target requires a filename extension"));
#endif
        if (!record.working_directory.isEmpty() && !QFileInfo(record.working_directory).isAbsolute())
            fail("working_directory",
                 QCoreApplication::translate("XLibrary", "expected an empty string or an absolute path"));
        try {
            Database::validate_application(record);
        } catch (const std::invalid_argument &error) {
            fail("application", QString::fromUtf8(error.what()));
        }
        records.append(std::move(record));
    }
    this->error_source = operation_error_source::sqlite;
    this->defer_result = true;
    emit this->preview_ready(this->request_id, records, {});
}

QStringList Manager::scenario_issues(const scenario_record &record, const QVector<app_record> &applications) {
    QStringList issues;
    QHash<QString, QString> checked_targets;
    bool enabled = false;
    for (qsizetype index = 0; index < record.steps.size(); ++index) {
        const auto &step = record.steps[index];
        if (!step.enabled)
            continue;
        enabled = true;
        QString reason;
        QString name;
        QString key;
        const auto found = std::find_if(applications.cbegin(), applications.cend(),
                                        [&](const auto &app) { return app.id == step.application_id; });
        try {
            if (found == applications.cend())
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Missing application").toStdString());
            name = found->name;
            if (step.delay_ms < 0)
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Invalid step delay").toStdString());
            const auto profile = step.launch_profile.isEmpty() ? found->default_launch_profile : step.launch_profile;
            key = QString::number(step.application_id) + ':' + profile;
            if (checked_targets.contains(key)) {
                reason = checked_targets.value(key);
                if (!reason.isEmpty())
                    issues.append(
                        QCoreApplication::translate("XLibrary", "Step %1 · %2: %3").arg(index + 1).arg(name, reason));
                continue;
            }
            const auto app = Application::with_launch_profile(*found, profile);
            Database::validate_application(app);
            if (!Application::target_available(app))
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "The application file is unavailable.").toStdString());
            if (!app.working_directory.isEmpty() && !QFileInfo(app.working_directory).isDir())
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "The working directory is unavailable.").toStdString());
#ifdef Q_OS_LINUX
            if (Application::file_target(app)) {
                const auto resolved = Application::resolve_application(app.path);
                if (app.working_directory.isEmpty() && !resolved.working_directory.isEmpty() &&
                    !QFileInfo(resolved.working_directory).isDir())
                    throw std::invalid_argument(
                        QCoreApplication::translate("XLibrary", "The working directory is unavailable.").toStdString());
            }
            (void)xlibrary::split_launch_arguments(app.arguments.toStdString());
#endif
        } catch (const std::exception &error) {
            reason = QString::fromUtf8(error.what());
        }
        if (!key.isEmpty())
            checked_targets.insert(key, reason);
        if (!reason.isEmpty())
            issues.append(QCoreApplication::translate("XLibrary", "Step %1 · %2: %3")
                              .arg(index + 1)
                              .arg(name.isEmpty() ? QString::number(step.application_id) : name, reason));
    }
    if (!enabled)
        issues.append(QCoreApplication::translate("XLibrary", "The scenario has no enabled applications."));
    return issues;
}

void Manager::clear_history() {
    this->store->clear_history();
    this->refresh();
    emit this->notification(QCoreApplication::translate("XLibrary", "Usage history cleared."));
}

void Manager::preview_profile(const QString &path) {
    emit this->profile_inspected(this->request_id, path, ProfileBackup::inspect(path));
}

void Manager::preview_paths(const QString &old_root, const QString &new_root) {
    emit this->paths_inspected(this->request_id, this->store->preview_relocation(old_root, new_root));
}

void Manager::recover_paths(const QVector<path_relocation> &changes) {
    this->store->apply_relocation(changes);
    this->refresh();
    emit this->notification(
        QCoreApplication::translate("XLibrary", "Application paths recovered: %1.").arg(changes.size()));
}

void Manager::launch_record(const qint64 id, const Application::launch_mode mode, const quint64 generation,
                            const std::shared_ptr<std::atomic<quint64>> &token,
                            const std::function<void(const app_record &, Application::launch_mode)> &launch_action,
                            const QString &profile) {
    try {
        if (generation && token->load() != generation) {
            throw DiscoveryCancelled();
        }
        const auto it = std::find_if(this->catalog.applications.cbegin(), this->catalog.applications.cend(),
                                     [id](const auto &record) { return record.id == id; });
        if (it == this->catalog.applications.cend())
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "The application was not found in the library.").toStdString());
        const auto selected_profile = profile.isEmpty() ? it->default_launch_profile : profile;
        const auto record = Application::with_launch_profile(*it, selected_profile);
        const auto effective_mode = mode == Application::launch_mode::activate &&
                                            ((!selected_profile.isEmpty() && selected_profile != "configured") ||
                                             Application::requires_fresh_launch(record))
                                        ? Application::launch_mode::launch
                                        : mode;
        launch_action(record, effective_mode);
        // Ошибка истории не должна превращать успешный запуск в неуспешный или повторный.
        emit this->launched(id, {this->request_id, "launch", operation_state::completed, {}}, generation);
        try {
            this->store->record_launch(id);
            this->refresh();
        } catch (const std::exception &error) {
            const auto reason = QString::fromUtf8(error.what());
            emit this->result({this->next_request_id(),
                               "history",
                               operation_state::error,
                               {QCoreApplication::translate(
                                    "XLibrary", "The application opened, but its launch history could not be saved: %1")
                                    .arg(reason),
                                reason, operation_error_source::sqlite}});
        }
    } catch (const DiscoveryCancelled &) {
        emit this->launched(id, {this->request_id, "launch", operation_state::cancelled, {}}, generation);
        throw;
    } catch (const std::invalid_argument &error) {
        emit this->launched(id,
                            {this->request_id, "launch", operation_state::error,
                             exception_error("launch", error, operation_error_source::validation)},
                            generation);
        throw;
    } catch (const std::exception &error) {
        emit this->launched(id,
                            {this->request_id, "launch", operation_state::error,
                             exception_error("launch", error, operation_error_source::platform)},
                            generation);
        throw;
    }
}

void Manager::execute(const quint64 id, const QString &operation, const std::function<void(Manager &)> &task,
                      const operation_error_source source) {
    this->request_id = id;
    this->error_source = source;
    this->task_error = {};
    this->defer_result = false;
    try {
        if (!this->store && operation != "folder" && operation != "profile_preview" && operation != "profile_restore" &&
            operation != "diagnostics") {
            this->error_source = operation_error_source::library;
            throw std::runtime_error(
                QCoreApplication::translate(
                    "XLibrary", "The library is unavailable. Restart the application after resolving the error.")
                    .toStdString());
        }
        task(*this);
        if (!this->defer_result)
            emit this->result({id, operation, operation_state::completed, {}});
    } catch (const DiscoveryCancelled &) {
        emit this->result({id, operation, operation_state::cancelled, {}});
    } catch (const std::invalid_argument &error) {
        emit this->result({id, operation, operation_state::error,
                           exception_error(operation, error, operation_error_source::validation)});
    } catch (const std::exception &error) {
        emit this->result({id, operation, operation_state::error,
                           this->task_error.source == operation_error_source::none
                               ? exception_error(operation, error, this->error_source)
                               : this->task_error});
    }
}

void Manager::preview_files(const QStringList &paths, const std::shared_ptr<discovery_control> &cancelled) {
    QVector<app_record> records;
    QVector<discovery_issue> issues;
    for (const auto &path : paths) {
        if (cancelled->requested.load())
            throw DiscoveryCancelled();
        try {
            const QFileInfo info(path);
#ifdef Q_OS_WIN
            const auto suffix = info.suffix().toLower();
            if (suffix != "exe" && suffix != "lnk" && suffix != "bat" && suffix != "cmd")
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Supported files: .exe, .lnk, .bat, .cmd.").toStdString());
#endif
            if (!info.isFile())
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "The application file is unavailable.").toStdString());
            auto record = Application::resolve_application(path);
            Database::validate_application(record);
            records.append(std::move(record));
        } catch (const std::exception &error) {
            issues.append({path, QString::fromUtf8(error.what())});
        }
    }
    this->defer_result = true;
    emit this->preview_ready(this->request_id, records, issues);
}

void Manager::review_inventory(const QVector<app_record> &records, const QVector<discovery_issue> &issues,
                               const std::shared_ptr<discovery_control> &cancelled) {
    // Refresh locations and sources only for known configurations; additions still require selection.
    store->refresh_discovery_metadata(records, *cancelled);
    refresh();
    QSet<QString> found;
    for (const auto &record : records)
        found.insert(Application::launch_key(record));
    QVector<app_record> removed;
    const bool complete = std::ranges::none_of(
        issues, [](const discovery_issue &issue) { return issue.kind == discovery_issue_kind::error; });
    for (const auto &record : store->read().applications) {
        if (cancelled->requested.load())
            throw DiscoveryCancelled();
        if (complete && monitor_state == operation_state::ready &&
            !last_running.contains(normalized_path(Application::process_path(record))) &&
            !found.contains(Application::launch_key(record)) &&
            Application::definitely_removed(record, known_volume_roots, verified_locations))
            removed.append(record);
    }
    defer_result = true;
    emit inventory_ready(request_id, records, removed, issues);
}

void Manager::remove_missing_applications(const QVector<app_record> &records,
                                          const std::shared_ptr<discovery_control> &cancelled) {
    const auto current = store->read();
    const auto running = Heartbeat::running_paths(current.applications);
    QVector<qint64> ids;
    for (const auto &record : records) {
        const auto found = std::ranges::find_if(current.applications, [&](const app_record &item) {
            return item.id == record.id && Application::launch_key(item) == Application::launch_key(record) &&
                   item.installation_directory == record.installation_directory &&
                   item.executable_path == record.executable_path && item.discovery_source == record.discovery_source;
        });
        if (found != current.applications.cend() &&
            !running.contains(normalized_path(Application::process_path(*found))) &&
            Application::definitely_removed(*found, known_volume_roots, verified_locations))
            ids.append(found->id);
    }
    if (cancelled->requested.load())
        throw DiscoveryCancelled();
    if (!ids.isEmpty()) {
        remove_applications(ids, false);
        emit notification(
            QCoreApplication::translate("XLibrary", "Removed uninstalled applications: %1.").arg(ids.size()));
    }
}

void Manager::apply_catalog_preview(const QVector<app_record> &applications, QVector<discovery_issue> issues,
                                    const std::shared_ptr<discovery_control> &cancelled,
                                    const import_conflict_policy policy,
                                    const std::function<void(qsizetype, qsizetype)> &report_progress,
                                    const bool quiet) {
    try {
        const auto count = this->store->import_records(applications, cancelled.get(), report_progress, &issues, policy);
        const auto message = discovery_summary(
            count.added, issues,
            QCoreApplication::translate("XLibrary", "Import complete. Updated: %1.").arg(count.updated));
        emit this->discovery_report_ready(discovery_details(message, issues));
        if (quiet) {
            if (count.added > 0)
                emit this->notification(
                    QCoreApplication::translate("XLibrary", "Added applications: %1.").arg(count.added));
        } else {
            emit this->notification(
                QCoreApplication::translate("XLibrary", "Import complete. Added: %1. Already in the library: %2.")
                    .arg(count.added)
                    .arg(count.duplicates + count.updated));
        }
        this->refresh();
    } catch (const DiscoveryCancelled &) {
        const auto message = QCoreApplication::translate("XLibrary", "Import cancelled; changes were not saved.");
        emit this->discovery_report_ready(discovery_details(message, issues));
        if (!quiet)
            emit this->notification(message);
        throw;
    } catch (const std::exception &error) {
        issues.append({"SQLite", QString::fromUtf8(error.what())});
        emit this->discovery_report_ready(discovery_details(
            QCoreApplication::translate("XLibrary", "Import failed; changes were not saved."), issues));
        throw;
    }
}

void Manager::backup_profile(const QString &path, const QString &database_path, const QString &settings_path,
                             const bool settings_ok) {
    if (!settings_ok)
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "Could not save settings before creating a backup.").toStdString());
    ProfileBackup::create(path, database_path, settings_path);
    emit this->notification(QCoreApplication::translate("XLibrary", "Full profile backup saved: %1").arg(path));
}

void Manager::restore_profile(const QString &path, const QString &database_path, const QString &settings_path,
                              const QByteArray &fingerprint, const bool settings_ok, const quint64 library_id,
                              const bool old_autostart) {
    if (this->monitor)
        this->monitor->stop();
    this->store.reset();
    try {
        if (!settings_ok)
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary", "Could not save settings before restoring the profile.")
                    .toStdString());
        const auto recovery = ProfileBackup::restore(path, database_path, settings_path, fingerprint);
        emit this->profile_replaced(recovery, old_autostart);
        this->initialize(library_id);
        if (!this->store)
            throw std::runtime_error(
                QCoreApplication::translate("XLibrary",
                                            "The profile was restored but could not be opened. Restart XLibrary.")
                    .toStdString());
    } catch (...) {
        const auto failure = std::current_exception();
        // Восстановление возвращает прежний профиль до повторного открытия соединения.
        try {
            ProfileBackup::recover(database_path, settings_path);
            this->initialize(library_id);
        } catch (const std::exception &recovery_error) {
            emit this->result({library_id, "library", operation_state::error,
                               exception_error("library", recovery_error, operation_error_source::filesystem)});
        }
        std::rethrow_exception(failure);
    }
}

void Manager::export_diagnostics(const QString &path, const QByteArray &report) {
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(report) != report.size() || !output.commit())
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "Could not save the diagnostic report.").toStdString());
    emit this->notification(QCoreApplication::translate("XLibrary", "Diagnostic report saved."));
}
