#include <memory>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTabWidget>

#include "DesktopService.h"
#include "ProfileView.h"
#include "UiHelpers.h"

namespace {
using namespace ui_helpers;

QString manifest_summary(const profile_manifest &manifest) {
    const auto created = QDateTime::fromString(manifest.created_utc, Qt::ISODateWithMs);
    QStringList lines{
        QCoreApplication::translate("XLibrary", "Created: %1")
            .arg(created.isValid() ? created.toLocalTime().toString(Qt::TextDate) : manifest.created_utc),
        QCoreApplication::translate("XLibrary", "Fora.Deck version: %1").arg(manifest.application_version),
        QCoreApplication::translate("XLibrary", "Database schema: %1").arg(manifest.schema_version)};
    lines.append(QCoreApplication::translate("XLibrary", "Backup format: %1").arg(manifest.format_version));
    const QList<QPair<QString, QString>> entities{
        {"applications", QCoreApplication::translate("XLibrary", "Applications: %1")},
        {"desktop_application", QCoreApplication::translate("XLibrary", "Application details: %1")},
        {"launch_target", QCoreApplication::translate("XLibrary", "Application targets: %1")},
        {"launch_configuration", QCoreApplication::translate("XLibrary", "Launch configurations: %1")},
        {"tags", QCoreApplication::translate("XLibrary", "Tags: %1")},
        {"application_alias", QCoreApplication::translate("XLibrary", "Search aliases: %1")},
        {"saved_filter", QCoreApplication::translate("XLibrary", "Saved filters: %1")},
        {"application_tags", QCoreApplication::translate("XLibrary", "Tag assignments: %1")},
        {"desktop_scenario", QCoreApplication::translate("XLibrary", "Scenarios: %1")},
        {"desktop_scenario_step", QCoreApplication::translate("XLibrary", "Scenario steps: %1")},
        {"workspace", QCoreApplication::translate("Workspaces", "Workspaces (%1)")},
        {"workspace_item", QCoreApplication::translate("Workspaces", "Workspace resources (%1)")},
        {"workspace_window", QCoreApplication::translate("Workspaces", "Saved window placements (%1)")},
        {"discovery_source", QCoreApplication::translate("XLibrary", "Discovery sources: %1")},
        {"history", QCoreApplication::translate("XLibrary", "Launch history entries: %1")},
        {"settings", QCoreApplication::translate("XLibrary", "Settings: %1")}};
    for (const auto &[key, text] : entities)
        if (manifest.counts.contains(key))
            lines.append(text.arg(manifest.counts.value(key)));
    for (auto it = manifest.counts.cbegin(); it != manifest.counts.cend(); ++it) {
        bool displayed = false;
        for (const auto &entity : entities)
            displayed = displayed || entity.first == it.key();
        if (!displayed)
            lines.append(it.key() + ": " + QString::number(it.value()));
    }
    return lines.join('\n');
}

void select_root_recovery(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    bool accepted = false;
    const auto old_root = QDir::fromNativeSeparators(QInputDialog::getText(
        parent, QCoreApplication::translate("XLibrary", "Recover moved folder"),
        QCoreApplication::translate("XLibrary", "Previous absolute folder path:"), QLineEdit::Normal, {}, &accepted));
    if (!accepted || old_root.isEmpty())
        return;
    if (!QFileInfo(old_root).isAbsolute()) {
        service->notify(QCoreApplication::translate("XLibrary", "Enter an absolute path to the previous folder."));
        return;
    }
    const auto new_root = QFileDialog::getExistingDirectory(
        parent, QCoreApplication::translate("XLibrary", "Select the replacement folder"));
    if (new_root.isEmpty())
        return;

    auto *dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QCoreApplication::translate("XLibrary", "Preview path recovery"));
    prepare_dialog(dialog, context, {1040, 600}, {640, 400});
    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(label(QCoreApplication::translate(
        "XLibrary", "Select the valid replacements to apply. Application IDs, arguments, tags, favorites, history, "
                    "and scenario steps are preserved.")));
    auto *status = label(QCoreApplication::translate("XLibrary", "Checking replacement paths…"), "muted");
    layout->addWidget(status);
    auto *table = new QTableWidget(0, 6);
    table->setAccessibleName(QCoreApplication::translate("XLibrary", "Path recovery preview"));
    table->setHorizontalHeaderLabels({QCoreApplication::translate("XLibrary", "Application"),
                                      QCoreApplication::translate("XLibrary", "Previous path"),
                                      QCoreApplication::translate("XLibrary", "Replacement path"),
                                      QCoreApplication::translate("XLibrary", "Previous working directory"),
                                      QCoreApplication::translate("XLibrary", "Replacement working directory"),
                                      QCoreApplication::translate("XLibrary", "Status")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setWordWrap(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->hide();
    layout->addWidget(table, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("XLibrary", "Cancel"));
    auto *apply =
        buttons->addButton(QCoreApplication::translate("XLibrary", "Apply selected"), QDialogButtonBox::AcceptRole);
    apply->setProperty("primary", true);
    apply->setEnabled(false);
    layout->addWidget(buttons);
    const auto request_id = std::make_shared<quint64>(0);
    const auto changes = std::make_shared<QVector<path_relocation>>();
    const auto update = [=] {
        qsizetype selected = 0;
        for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 0)->checkState() == Qt::Checked && changes->at(row).issue.isEmpty())
                ++selected;
        apply->setEnabled(selected > 0 && !service->profile_busy() && !service->restart_required() &&
                          !service->is_discovering() && !service->is_scenario_active());
        if (!changes->isEmpty())
            status->setText(QCoreApplication::translate("XLibrary", "Selected replacements: %1 of %2")
                                .arg(selected)
                                .arg(changes->size()));
    };
    QObject::connect(service, &DesktopService::paths_preview_ready, dialog,
                     [=](const quint64 id, const QVector<path_relocation> &result) {
                         if (id != *request_id)
                             return;
                         const QSignalBlocker blocker(table);
                         *changes = result;
                         table->setRowCount(static_cast<int>(result.size()));
                         for (int row = 0; row < table->rowCount(); ++row) {
                             const auto &change = result.at(row);
                             const QStringList values{
                                 change.name,
                                 QDir::toNativeSeparators(change.old_path),
                                 QDir::toNativeSeparators(change.new_path),
                                 QDir::toNativeSeparators(change.old_directory),
                                 QDir::toNativeSeparators(change.new_directory),
                                 !change.issue.isEmpty() ? change.issue
                                 : change.uses_existing_configuration
                                     ? QCoreApplication::translate("XLibrary", "Uses existing launch configuration")
                                     : QCoreApplication::translate("XLibrary", "Ready")};
                             for (int column = 0; column < values.size(); ++column) {
                                 auto *item = new QTableWidgetItem(values.at(column));
                                 item->setToolTip(values.at(column));
                                 if (column == 0) {
                                     item->setCheckState(change.issue.isEmpty() ? Qt::Checked : Qt::Unchecked);
                                     if (!change.issue.isEmpty())
                                         item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
                                 }
                                 table->setItem(row, column, item);
                             }
                         }
                         status->setText(QCoreApplication::translate("XLibrary", "No applications match this folder."));
                         update();
                     });
    QObject::connect(table, &QTableWidget::itemChanged, dialog, update);
    QObject::connect(service, &DesktopService::profile_state_changed, dialog, update);
    QObject::connect(service, &DesktopService::discovery_changed, dialog, update);
    QObject::connect(service, &DesktopService::scenario_changed, dialog, [=] { update(); });
    QObject::connect(service, &DesktopService::operation_changed, dialog, [=](const operation_result &result) {
        if (result.id == *request_id &&
            (result.state == operation_state::error || result.state == operation_state::cancelled))
            dialog->reject();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
        QVector<path_relocation> selected;
        for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 0)->checkState() == Qt::Checked && changes->at(row).issue.isEmpty())
                selected.append(changes->at(row));
        if (selected.isEmpty())
            return;
        service->recover_paths(selected);
        dialog->accept();
    });
    dialog->open();
    *request_id = service->preview_paths(old_root, new_root);
    if (!*request_id)
        dialog->reject();
}
}

void select_profile_restore(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    const auto path =
        QFileDialog::getOpenFileName(parent, QCoreApplication::translate("XLibrary", "Select a profile backup"), {},
                                     QCoreApplication::translate("XLibrary", "Fora.Deck profile (*.xlprofile)"));
    if (path.isEmpty())
        return;
    auto *dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QCoreApplication::translate("XLibrary", "Restore complete profile"));
    prepare_dialog(dialog, context, {640, 540}, {440, 360});
    auto *layout = new QVBoxLayout(dialog);
    auto *file = label(QDir::toNativeSeparators(path), "muted");
    file->setTextFormat(Qt::PlainText);
    file->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(file);
    auto *summary = new QPlainTextEdit(QCoreApplication::translate("XLibrary", "Checking profile backup…"));
    summary->setReadOnly(true);
    summary->setAccessibleName(QCoreApplication::translate("XLibrary", "Profile backup contents"));
    layout->addWidget(summary, 1);
    layout->addWidget(label(QCoreApplication::translate(
        "XLibrary", "Restore replaces the current library and settings. The current profile is preserved for recovery. "
                    "Restart Fora.Deck afterward to apply the restored settings. Applications will not be launched.")));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("XLibrary", "Cancel"));
    auto *restore =
        buttons->addButton(QCoreApplication::translate("XLibrary", "Restore profile"), QDialogButtonBox::AcceptRole);
    restore->setProperty("danger", true);
    restore->setEnabled(false);
    layout->addWidget(buttons);
    const auto request_id = std::make_shared<quint64>(0);
    const auto fingerprint = std::make_shared<QByteArray>();
    const auto update = [=] {
        restore->setEnabled(!fingerprint->isEmpty() && !service->profile_busy() && !service->restart_required() &&
                            !service->is_discovering() && !service->is_scenario_active());
    };
    QObject::connect(service, &DesktopService::profile_preview_ready, dialog,
                     [=](const quint64 id, const QString &preview_path, const profile_manifest &manifest) {
                         if (id != *request_id || preview_path != path)
                             return;
                         *fingerprint = manifest.fingerprint;
                         summary->setPlainText(manifest_summary(manifest));
                         update();
                     });
    QObject::connect(service, &DesktopService::profile_state_changed, dialog, update);
    QObject::connect(service, &DesktopService::discovery_changed, dialog, update);
    QObject::connect(service, &DesktopService::scenario_changed, dialog, [=] { update(); });
    QObject::connect(service, &DesktopService::operation_changed, dialog, [=](const operation_result &result) {
        if (result.id == *request_id &&
            (result.state == operation_state::error || result.state == operation_state::cancelled))
            dialog->reject();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
        if (fingerprint->isEmpty())
            return;
        service->restore_profile(path, *fingerprint);
        dialog->accept();
    });
    dialog->open();
    *request_id = service->preview_profile(path);
    if (!*request_id)
        dialog->reject();
}

void select_application_replacement(DesktopService *const service, const app_record &record, QWidget *const parent) {
    // Копия нужна до диалога: обновление модели может инвалидировать выбранную запись.
    const auto original = record;
    const auto path = QFileDialog::getOpenFileName(
        parent, QCoreApplication::translate("XLibrary", "Select the replacement application"),
        QFileInfo(original.path).absolutePath());
    if (path.isEmpty())
        return;
    service->recover_paths({{original.id,
                             original.name,
                             original.path,
                             path,
                             original.working_directory,
                             original.working_directory,
                             {}}});
}

QWidget *profile_view(const ui_context &context, QWidget *const parent) {
    using namespace ui_helpers;
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(24, 20, 24, 24);
    layout->setSpacing(12);
    layout->addWidget(label(QCoreApplication::translate("XLibrary", "Profile and diagnostics"), "subheading"));
    auto *diagnostics = new QPlainTextEdit;
    diagnostics->setReadOnly(true);
    diagnostics->setMinimumHeight(220);
    diagnostics->setAccessibleName(QCoreApplication::translate("XLibrary", "Current profile diagnostics"));
    layout->addWidget(diagnostics);
    auto *export_button =
        button(QCoreApplication::translate("XLibrary", "Export safe diagnostics…"), nullptr, "export");
    layout->addWidget(export_button);
    layout->addWidget(
        label(QCoreApplication::translate(
                  "XLibrary",
                  "The diagnostic export contains service versions, mode, and error codes. It excludes the application "
                  "catalog, arguments, settings contents, and tokens."),
              "muted"));
    layout->addWidget(label(QCoreApplication::translate("XLibrary", "Complete profile backup"), "subheading"));
    layout->addWidget(
        label(QCoreApplication::translate(
                  "XLibrary",
                  "A profile backup includes applications, all tags, scenarios, history, and settings. It contains "
                  "private paths and arguments. Native module files and system-stored tokens are not included."),
              "muted"));
    auto *backup = button(QCoreApplication::translate("XLibrary", "Back up profile…"), nullptr, "export");
    auto *restore = button(QCoreApplication::translate("XLibrary", "Restore profile…"), nullptr, "import");
    layout->addWidget(backup);
    layout->addWidget(restore);
    layout->addWidget(label(QCoreApplication::translate("XLibrary", "Recover application paths"), "subheading"));
    layout->addWidget(
        label(QCoreApplication::translate(
                  "XLibrary",
                  "After moving a folder, preview replacement paths for missing applications. To replace one target, "
                  "select its missing application in the library."),
              "muted"));
    auto *recover = button(QCoreApplication::translate("XLibrary", "Recover moved folder…"));
    layout->addWidget(recover);
    auto *restart_hint =
        label(QCoreApplication::translate(
                  "XLibrary",
                  "The profile was restored. Restart Fora.Deck to apply its settings before making further changes."),
              "muted");
    layout->addWidget(restart_hint);
    auto *restart = button(QCoreApplication::translate("XLibrary", "Restart Fora.Deck"), "primary");
    layout->addWidget(restart);
    layout->addStretch();
    const auto update = [=] {
        const auto text = service->diagnostics_text();
        if (diagnostics->toPlainText() != text)
            diagnostics->setPlainText(text);
        const bool idle = !service->profile_busy() && !service->restart_required();
        const bool replace_allowed = idle && !service->is_discovering() && !service->is_scenario_active();
        export_button->setEnabled(idle);
        backup->setEnabled(replace_allowed && service->is_ready());
        restore->setEnabled(replace_allowed);
        recover->setEnabled(replace_allowed && service->is_ready());
        restart_hint->setVisible(service->restart_required());
        restart->setVisible(service->restart_required());
        restart->setEnabled(!service->profile_busy());
    };
    QObject::connect(service, &DesktopService::profile_state_changed, widget, update);
    QObject::connect(service, &DesktopService::catalog_changed, widget, update);
    QObject::connect(service, &DesktopService::discovery_changed, widget, update);
    QObject::connect(service, &DesktopService::scenario_changed, widget, [=] { update(); });
    QObject::connect(service, &DesktopService::operation_changed, widget, [=] { update(); });
    QObject::connect(backup, &QPushButton::clicked, widget, [=] {
        QFileDialog dialog(widget, QCoreApplication::translate("XLibrary", "Back up complete profile"));
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setNameFilter(QCoreApplication::translate("XLibrary", "Fora.Deck profile (*.xlprofile)"));
        dialog.setDefaultSuffix("xlprofile");
        dialog.selectFile("xlibrary-" + QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd") + ".xlprofile");
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
            return;
        service->backup_profile(dialog.selectedFiles().first());
    });
    QObject::connect(restore, &QPushButton::clicked, widget, [=] { select_profile_restore(context, widget); });
    QObject::connect(recover, &QPushButton::clicked, widget, [=] { select_root_recovery(context, widget); });
    QObject::connect(export_button, &QPushButton::clicked, widget, [=] {
        const auto path = QFileDialog::getSaveFileName(
            widget, QCoreApplication::translate("XLibrary", "Export safe diagnostics"), "xlibrary-diagnostics.json",
            QCoreApplication::translate("XLibrary", "JSON files (*.json)"));
        if (!path.isEmpty())
            service->export_diagnostics(path);
    });
    QObject::connect(restart, &QPushButton::clicked, service, &DesktopService::restart_application);
    update();
    return widget;
}

void show_library_health(const ui_context &context) {
    using namespace ui_helpers;
    auto *service = static_cast<DesktopService *>(context.service);
    if (auto *existing = context.window->findChild<QDialog *>("library_health", Qt::FindDirectChildrenOnly)) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto *dialog = new QDialog(context.window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QCoreApplication::translate("XLibrary", "Library health"));
    dialog->setObjectName("library_health");
    prepare_dialog(dialog, context, {920, 640}, {640, 420});
    auto *layout = new QVBoxLayout(dialog);
    auto *summary = label({});
    auto *last_error = label({}, "muted");
    layout->addWidget(summary);
    layout->addWidget(last_error);
    auto *tabs = new QTabWidget;
    auto *table = new QTableWidget(0, 3);
    table->setObjectName("library_health_applications");
    table->setHorizontalHeaderLabels({QCoreApplication::translate("XLibrary", "Name"),
        QCoreApplication::translate("XLibrary", "Problem"), QCoreApplication::translate("XLibrary", "Target")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    auto *files = new QPlainTextEdit;
    files->setReadOnly(true);
    files->setAccessibleName(QCoreApplication::translate("XLibrary", "File index diagnostics"));
    auto *report = new QPlainTextEdit;
    report->setReadOnly(true);
    tabs->addTab(table, QCoreApplication::translate("XLibrary", "Applications"));
    tabs->addTab(files, QCoreApplication::translate("XLibrary", "File index diagnostics"));
    tabs->addTab(report, QCoreApplication::translate("XLibrary", "Scan report"));
    layout->addWidget(tabs, 1);
    auto *actions = new QHBoxLayout;
    auto *availability = action_button(context.actions.value("refresh_availability"));
    auto *locate = button(QCoreApplication::translate("XLibrary", "Locate application…"));
    auto *recover = button(QCoreApplication::translate("XLibrary", "Recover moved folder…"));
    auto *refresh_files = button(QCoreApplication::translate("XLibrary", "Refresh file index"));
    actions->addWidget(availability);
    actions->addWidget(locate);
    actions->addWidget(recover);
    actions->addWidget(refresh_files);
    layout->addLayout(actions);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    const auto selected_record = [=]() -> const app_record * {
        const auto *item = table->item(table->currentRow(), 0);
        return item ? service->model.find(item->data(Qt::UserRole).toLongLong()) : nullptr;
    };
    const auto update_actions = [=] {
        const bool idle = service->is_ready() && !service->profile_busy() && !service->restart_required();
        const auto *record = selected_record();
        locate->setEnabled(idle && record && !record->available && Application::file_target(*record));
        recover->setEnabled(idle && !service->is_discovering() && !service->is_scenario_active());
        refresh_files->setEnabled(idle && service->files->is_enabled());
    };
    const auto update = [=] {
        const auto *selected = selected_record();
        const auto selected_id = selected ? selected->id : 0;
        const QSignalBlocker blocker(table);
        table->setRowCount(0);
        qsizetype unavailable = 0;
        qsizetype disconnected = 0;
        qsizetype failed = 0;
        QHash<qint64, QString> reasons;
        for (const auto &candidate : service->removal_candidates())
            reasons.insert(candidate.id, candidate.removal_reason);
        for (const auto &record : service->catalog().applications) {
            unavailable += !record.available;
            disconnected += record.volume_unavailable;
            failed += service->launch_errors().contains(record.id);
            QStringList problems;
            if (!record.available)
                problems.append(record.volume_unavailable
                    ? QCoreApplication::translate("XLibrary", "Drive disconnected or unavailable")
                    : QCoreApplication::translate("XLibrary", "File unavailable; the entry is kept"));
            if (reasons.contains(record.id))
                problems.append(reasons.value(record.id));
            if (service->launch_errors().contains(record.id))
                problems.append(QCoreApplication::translate("XLibrary", "Launch failed this session: %1")
                    .arg(service->launch_errors().value(record.id)));
            if (problems.isEmpty())
                continue;
            const auto row = table->rowCount();
            table->insertRow(row);
            const QStringList values{record.name, problems.join('\n'),
                                     QDir::toNativeSeparators(Application::application_path(record))};
            for (int column = 0; column < 3; ++column) {
                auto *item = new QTableWidgetItem(values[column]);
                item->setToolTip(values[column]);
                item->setData(Qt::UserRole, record.id);
                table->setItem(row, column, item);
            }
            if (record.id == selected_id)
                table->setCurrentCell(row, 0);
        }
        table->resizeRowsToContents();
        summary->setText(QCoreApplication::translate("XLibrary",
            "Unavailable applications: %1; on unavailable drives: %2; removal candidates: %3; launch failures: %4.")
            .arg(unavailable).arg(disconnected).arg(reasons.size()).arg(failed));
        const auto error = service->last_error().user_message;
        last_error->setText(error.isEmpty()
            ? QCoreApplication::translate("XLibrary", "No operation errors recorded this session.")
            : QCoreApplication::translate("XLibrary", "Last operation error this session: %1").arg(error));
        files->setPlainText(service->files->diagnostics_text());
        report->setPlainText(service->discovery_report());
        update_actions();
    };
    QObject::connect(table, &QTableWidget::itemSelectionChanged, dialog, update_actions);
    QObject::connect(locate, &QPushButton::clicked, dialog, [=] {
        if (const auto *record = selected_record(); record && locate->isEnabled())
            select_application_replacement(service, *record, dialog);
    });
    QObject::connect(recover, &QPushButton::clicked, dialog, [=] { select_root_recovery(context, dialog); });
    QObject::connect(refresh_files, &QPushButton::clicked, service->files, &FileManager::rescan);
    QObject::connect(service, &DesktopService::catalog_changed, dialog, update);
    QObject::connect(service, &DesktopService::selection_changed, dialog, update);
    QObject::connect(service, &DesktopService::catalog_preview_ready, dialog, update);
    QObject::connect(service, &DesktopService::profile_state_changed, dialog, update);
    QObject::connect(service, &DesktopService::discovery_changed, dialog, update);
    QObject::connect(service, &DesktopService::operation_changed, dialog, [=] { update(); });
    QObject::connect(service, &DesktopService::application_launch_finished, dialog, [=] { update(); });
    QObject::connect(service, &DesktopService::scenario_steps_changed, dialog, update);
    QObject::connect(service, &DesktopService::discovery_report_changed, dialog, update);
    QObject::connect(service->files, &FileManager::status_changed, dialog,
                     [=] { files->setPlainText(service->files->diagnostics_text()); });
    update();
    dialog->open();
}
