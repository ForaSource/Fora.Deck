#include <algorithm>
#include <cctype>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <stdexcept>

#include "Manager.h"

std::string Manager::to_lower(std::string data) {
    std::ranges::transform(data, data.begin(), [](unsigned char c) { return std::tolower(c); });
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

std::uint32_t Manager::launch(const std::string title) {
    this->get_apps();
    for (auto &app : applications) {
        if (this->to_lower(app.get_file_title()) == this->to_lower(title)) {
            return app.launch();
        }
    }
    throw std::invalid_argument(title + " is not in this manager");
}

Heartbeat Manager::heartbeat(const std::string title, std::uint32_t process_id) const {
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
        const auto found = std::find_if(applications.cbegin(), applications.cend(), [&](const auto &app) {
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

void Manager::initialize(quint64 id) {
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

void Manager::restore(quint64 id, const QString &path) {
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
        record.running = last_running.contains(normalized_path(record.path));
    }
    emit loaded(catalog);
    poll(true);
}

void Manager::poll(bool force) {
    QSet<QString> unavailable;
    QSet<QString> missing_volumes;
    const auto previous_roots = known_volume_roots;
    bool capabilities_changed = false;
    auto paths = last_running;
    try {
        for (auto &record : catalog.applications) {
            const auto error = Application::new_instance_error(record);
            if (error != record.new_instance_error) {
                record.new_instance_error = error;
                capabilities_changed = true;
            }
            record.available = QFileInfo(record.path).isFile();
            record.volume_unavailable = false;
            if (record.available) {
                const QStorageInfo volume(record.path);
                const auto root = QDir::fromNativeSeparators(volume.rootPath());
                if (volume.isValid() && !root.isEmpty() && root != "/" && !known_volume_roots.contains(root))
                    known_volume_roots.append(root);
            } else {
                auto roots = known_volume_roots;
#ifdef Q_OS_WIN
                const auto path = QDir::fromNativeSeparators(record.path);
                if (path.size() >= 3 && path[1] == ':')
                    roots.append(path.left(3));
                else if (path.startsWith("//")) {
                    const auto end = path.indexOf('/', path.indexOf('/', 2) + 1);
                    if (end > 2)
                        roots.append(path.left(end));
                }
#endif
                for (const auto &root : roots) {
                    const auto prefix = root.endsWith('/') ? root : root + '/';
                    if (!record.path.startsWith(prefix, path_case_sensitivity))
                        continue;
                    const QStorageInfo volume(root);
                    if (!volume.isValid() || !volume.isReady() ||
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
        paths = Heartbeat::running_paths(catalog.applications);
        if (monitor_state == operation_state::error)
            emit result({monitor_request_id, "monitor", operation_state::ready, {}});
        monitor_state = operation_state::ready;
    } catch (const std::exception &error) {
        if (monitor_state != operation_state::error) {
            monitor_request_id = ++*request_sequence;
            emit result({monitor_request_id, "monitor", operation_state::error,
                         exception_error("monitor", error, operation_error_source::platform)});
        }
        monitor_state = operation_state::error;
        return;
    }
    for (auto &record : catalog.applications)
        record.running = paths.contains(normalized_path(record.path));
    if (known_volume_roots != previous_roots)
        emit volumes_observed(known_volume_roots);
    if (force || capabilities_changed || paths != last_running || unavailable != last_unavailable ||
        missing_volumes != last_missing_volumes) {
        last_running = paths;
        last_unavailable = unavailable;
        last_missing_volumes = missing_volumes;
        emit state_changed(catalog.applications);
    }
}

void Manager::save_application(app_record record) {
    const auto current = std::find_if(this->catalog.applications.cbegin(), this->catalog.applications.cend(),
                                      [&](const app_record &item) { return item.id == record.id; });
    if (record.id == 0 || (current != this->catalog.applications.cend() && current->path != record.path))
        Application::resolve_application(record.path);
    const auto id = this->store->save_application(record);
    this->refresh();
    emit this->application_saved(this->request_id, id);
}

void Manager::resolve_application(const QString &path) {
    emit this->resolved(this->request_id, path, Application::resolve_application(path));
}

void Manager::remove_applications(const QVector<qint64> &ids) {
    this->store->remove_applications(ids);
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
                              int favorite) {
    this->store->update_metadata(ids, add_tags, remove_tags, favorite);
    this->refresh();
}

void Manager::save_filter(saved_filter record) {
    this->store->save_filter(record);
    this->refresh();
}

void Manager::remove_filter(qint64 id) {
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

void Manager::save_scenario(scenario_record record) {
    const auto id = this->store->save_scenario(record);
    this->refresh();
    emit this->scenario_saved(this->request_id, id);
}

void Manager::remove_scenario(qint64 id) {
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
                                                        {"arguments", record.arguments},
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
        const auto text = [&](const QString &field, bool required = false) {
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
        record.arguments = text("arguments");
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
            const auto name = alias.toString().trimmed();
            if (!record.aliases.contains(name, Qt::CaseInsensitive))
                record.aliases.append(name);
        }
        if (record.name.isEmpty() || record.name.size() > 256)
            fail("name", QCoreApplication::translate("XLibrary", "the name must contain 1 to 256 characters"));
        if (!QFileInfo(record.path).isAbsolute())
            fail("path", QCoreApplication::translate("XLibrary", "expected an absolute path"));
#ifdef Q_OS_WIN
        if (QFileInfo(record.path).suffix().isEmpty())
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

void Manager::clear_history() {
    this->store->clear_history();
    this->refresh();
    emit this->notification(QCoreApplication::translate("XLibrary", "Launch history cleared."));
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

void Manager::launch_record(qint64 id, Application::launch_mode mode, quint64 generation,
                            const std::shared_ptr<std::atomic<quint64>> &token,
                            const std::function<void(const app_record &, Application::launch_mode)> &launch_action) {
    try {
        if (generation && token->load() != generation) {
            throw DiscoveryCancelled();
        }
        const auto it = std::find_if(this->catalog.applications.cbegin(), this->catalog.applications.cend(),
                                     [id](const auto &record) { return record.id == id; });
        if (it == this->catalog.applications.cend())
            throw std::invalid_argument(
                QCoreApplication::translate("XLibrary", "The application was not found in the library.").toStdString());
        launch_action(*it, mode);
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

void Manager::execute(quint64 id, const QString &operation, const std::function<void(Manager &)> &task,
                      operation_error_source source) {
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

void Manager::apply_catalog_preview(const QVector<app_record> &applications, QVector<discovery_issue> issues,
                                    const std::shared_ptr<discovery_control> &cancelled, import_conflict_policy policy,
                                    const std::function<void(qsizetype, qsizetype)> &report_progress) {
    try {
        const auto count = this->store->import_records(applications, cancelled.get(), report_progress, &issues, policy);
        const auto message = discovery_summary(
            count.added, issues,
            QCoreApplication::translate("XLibrary", "Import complete. Updated: %1.").arg(count.updated));
        emit this->discovery_report_ready(discovery_details(message, issues));
        emit this->notification(message);
        this->refresh();
    } catch (const DiscoveryCancelled &) {
        const auto message = QCoreApplication::translate("XLibrary", "Import cancelled; changes were not saved.");
        emit this->discovery_report_ready(discovery_details(message, issues));
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
                             bool settings_ok) {
    if (!settings_ok)
        throw std::runtime_error(
            QCoreApplication::translate("XLibrary", "Could not save settings before creating a backup.").toStdString());
    ProfileBackup::create(path, database_path, settings_path);
    emit this->notification(QCoreApplication::translate("XLibrary", "Full profile backup saved: %1").arg(path));
}

void Manager::restore_profile(const QString &path, const QString &database_path, const QString &settings_path,
                              const QByteArray &fingerprint, bool settings_ok, quint64 library_id, bool old_autostart) {
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
