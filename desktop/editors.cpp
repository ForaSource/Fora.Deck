#include <algorithm>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextDocument>
#include <QTimer>

#include "Application.h"
#include "DesktopService.h"
#include "editors.h"
#include "localization.h"
#include "UiHelpers.h"

namespace {
using namespace ui_helpers;

QStringList parse_tags(const QString &input) {
    QStringList result;
    for (auto tag : input.split(',', Qt::SkipEmptyParts)) {
        tag = tag.trimmed();
        if (tag.startsWith('#'))
            tag.remove(0, 1);
        if (!tag.isEmpty() && !has_tag(result, tag))
            result.append(tag);
    }
    return result;
}

QDialog *application_editor(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    const auto record = context.parameters.value("record").value<app_record>();
    auto *dialog = new QDialog(parent);
    dialog->setWindowTitle(record.id ? QCoreApplication::translate("XLibrary", "Edit application")
                                     : QCoreApplication::translate("XLibrary", "Add application"));
    prepare_dialog(dialog, context, {640, 700}, {420, 320});
    auto *root = new QVBoxLayout(dialog);
    root->setContentsMargins(24, 24, 24, 24);
    root->addWidget(label(dialog->windowTitle(), "heading"));
    auto *form = new QWidget;
    auto *layout = new QVBoxLayout(form);
    layout->setContentsMargins(0, 16, 0, 8);
    layout->setSpacing(6);
    auto *target_type = new QComboBox;
    target_type->setObjectName("application_target_type");
    target_type->addItem(QCoreApplication::translate("XLibrary", "Application file"), "executable");
    target_type->addItem(QCoreApplication::translate("XLibrary", "Desktop shortcut"), "desktop");
    target_type->addItem(QCoreApplication::translate("XLibrary", "Steam game (AppID)"), "steam");
    target_type->addItem(QCoreApplication::translate("XLibrary", "Windows settings URI"), "system_uri");
    target_type->addItem(QCoreApplication::translate("XLibrary", "Windows application (AUMID)"), "aumid");
    target_type->setCurrentIndex(qMax(0, target_type->findData(Application::target_kind(record))));
    field(layout, QCoreApplication::translate("XLibrary", "Launch target type"), target_type);
    auto *path = new QLineEdit;
    path->setMaxLength(static_cast<int>(catalog_limits::text_length));
    path->setText(record.path);
    path->setObjectName("application_path");
    auto *browse = button(QCoreApplication::translate("XLibrary", "Browse…"));
    auto *file_row = new QWidget;
    auto *file_layout = new QHBoxLayout(file_row);
    file_layout->setContentsMargins(0, 0, 0, 0);
    file_layout->addWidget(path, 1);
    file_layout->addWidget(browse);
    field(layout, QCoreApplication::translate("XLibrary", "Launch target"), file_row);
    auto *name = new QLineEdit(record.name);
    name->setObjectName("application_name");
    name->setMaxLength(256);
    field(layout, QCoreApplication::translate("XLibrary", "Name"), name);
    auto *aliases = new QPlainTextEdit(record.aliases.join('\n'));
    aliases->setMaximumHeight(90);
    aliases->setTabChangesFocus(true);
    aliases->setObjectName("application_aliases");
    field(layout, QCoreApplication::translate("XLibrary", "Search aliases, one per line"), aliases);
    auto *arguments = new QLineEdit;
    arguments->setMaxLength(static_cast<int>(catalog_limits::text_length));
    arguments->setText(record.arguments);
    arguments->setObjectName("application_arguments");
    field(layout, QCoreApplication::translate("XLibrary", "Launch arguments"), arguments);
    auto *directory = new QLineEdit;
    directory->setMaxLength(static_cast<int>(catalog_limits::text_length));
    directory->setText(record.working_directory);
    directory->setPlaceholderText(QCoreApplication::translate("XLibrary", "Default: application folder"));
    field(layout, QCoreApplication::translate("XLibrary", "Working directory"), directory);
    auto *profiles = new QComboBox;
    profiles->setObjectName("application_launch_profile");
    const auto refresh_profiles = [=] {
        const auto selected = profiles->count() ? profiles->currentData().toString() : record.default_launch_profile;
        const QSignalBlocker blocker(profiles);
        profiles->clear();
        profiles->addItem(QCoreApplication::translate("XLibrary", "Configured launch arguments"), QString{});
        auto candidate = record;
        candidate.path = path->text();
        candidate.target_kind = target_type->currentData().toString();
        for (const auto &profile : service->launch_profiles(candidate))
            profiles->addItem(profile.icon_path.isEmpty() ? appearance::icon(profile.kind == "private" ? "lock" : "app")
                                                          : QIcon(profile.icon_path),
                              profile.name, profile.id);
        if (!selected.isEmpty() && profiles->findData(selected) < 0)
            profiles->addItem(QCoreApplication::translate("XLibrary", "Unavailable profile: %1").arg(selected),
                              selected);
        profiles->setCurrentIndex(qMax(0, profiles->findData(selected)));
        profiles->setEnabled(profiles->count() > 1);
    };
    field(layout, QCoreApplication::translate("XLibrary", "Default browser launch"), profiles);
    refresh_profiles();
    QObject::connect(service, &DesktopService::launch_profiles_changed, dialog, [=](const QString &changed_path) {
        if (changed_path == path->text())
            refresh_profiles();
    });
    const auto update_target_fields = [=] {
        const auto kind = target_type->currentData().toString();
        const bool file = kind == "executable" || kind == "desktop";
        browse->setEnabled(file);
        arguments->setEnabled(file);
        directory->setEnabled(file);
        path->setPlaceholderText(kind == "steam"        ? QString("730")
                                 : kind == "system_uri" ? QString("ms-settings:")
                                 : kind == "aumid"      ? QString("PackageFamilyName!ApplicationId")
                                                        : QString{});
        refresh_profiles();
    };
    QObject::connect(target_type, &QComboBox::currentIndexChanged, dialog, update_target_fields);
    QObject::connect(path, &QLineEdit::editingFinished, dialog, refresh_profiles);
    update_target_fields();
    auto *shortcut = new QKeySequenceEdit(service->target_shortcut(false, record.id));
    shortcut->setMaximumSequenceLength(1);
    shortcut->setObjectName("application_hotkey");
    field(layout, QCoreApplication::translate("XLibrary", "Global hotkey"), shortcut);
    if (record.id > 0)
        layout->addWidget(label(QString("XLibrary --launch-id %1").arg(record.id), "muted"));
    auto *tags = new QLineEdit(record.tags.join(", "));
    tags->setObjectName("application_tags");
    tags->setPlaceholderText(QCoreApplication::translate("XLibrary", "development, work"));
    field(layout, QCoreApplication::translate("XLibrary", "Tags separated by commas"), tags);
    auto *tag_search = new QLineEdit;
    tag_search->setPlaceholderText(QCoreApplication::translate("XLibrary", "Search tags"));
    tag_search->setAccessibleName(tag_search->placeholderText());
    layout->addWidget(tag_search);
    auto *tag_list = new QListWidget;
    tag_list->setMaximumHeight(140);
    for (const auto &tag : service->catalog().tags) {
        qsizetype usage = 0;
        for (const auto &application : service->catalog().applications)
            usage += has_tag(application.tags, tag);
        auto *item = new QListWidgetItem(QString("#%1 (%2)").arg(tag, QString::number(usage)), tag_list);
        item->setData(Qt::UserRole, tag);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(has_tag(record.tags, tag) ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(tag_list);
    QObject::connect(tag_search, &QLineEdit::textChanged, dialog, [=](const QString &text) {
        for (int row = 0; row < tag_list->count(); ++row)
            tag_list->item(row)->setHidden(!tag_list->item(row)->text().contains(text.trimmed(), Qt::CaseInsensitive));
    });
    QObject::connect(tag_list, &QListWidget::itemChanged, dialog, [=](QListWidgetItem *const item) {
        auto names = parse_tags(tags->text());
        const auto name = item->data(Qt::UserRole).toString();
        names.removeIf([&](const QString &tag) { return tag_name_key(tag) == tag_name_key(name); });
        if (item->checkState() == Qt::Checked)
            names.append(name);
        tags->setText(names.join(", "));
    });
    QObject::connect(tags, &QLineEdit::textChanged, dialog, [=](const QString &text) {
        const QSignalBlocker blocker(tag_list);
        const auto names = parse_tags(text);
        for (int row = 0; row < tag_list->count(); ++row) {
            auto *item = tag_list->item(row);
            item->setCheckState(has_tag(names, item->data(Qt::UserRole).toString()) ? Qt::Checked : Qt::Unchecked);
        }
    });
    auto *favorite = new QCheckBox(QCoreApplication::translate("XLibrary", "Favorite"));
    favorite->setChecked(record.favorite);
    layout->addWidget(favorite);
    auto *error = label("", "error");
    layout->addStretch();
    root->addWidget(scroll(form), 1);
    root->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save);
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("XLibrary", "Cancel"));
    auto *save = buttons->button(QDialogButtonBox::Save);
    save->setText(record.id ? QCoreApplication::translate("XLibrary", "Save")
                            : QCoreApplication::translate("XLibrary", "Add"));
    save->setProperty("primary", true);
    root->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(browse, &QPushButton::clicked, dialog, [=] {
        const auto file = QFileDialog::getOpenFileName(
            dialog, QCoreApplication::translate("XLibrary", "Select an application"), {},
#ifdef Q_OS_WIN
            QCoreApplication::translate("XLibrary", "Applications (*.exe *.lnk *.bat *.cmd);;All files (*)")
#else
                                                       QCoreApplication::translate("XLibrary", "All files (*);;Application shortcuts (*.desktop);;AppImage (*.AppImage)")
#endif
        );
        if (!file.isEmpty()) {
            path->setText(file);
            error->clear();
            browse->setEnabled(false);
            dialog->setProperty("resolve_request_id", QVariant::fromValue(service->resolve_application(file)));
        }
    });
    QObject::connect(service, &DesktopService::application_resolved, dialog,
                     [=](const quint64 request_id, const QString &requested_path, const app_record &resolved) {
                         if (request_id != dialog->property("resolve_request_id").toULongLong())
                             return;
                         if (QDir::cleanPath(path->text()) != QDir::cleanPath(requested_path))
                             return;
                         path->setText(resolved.path);
                         target_type->setCurrentIndex(
                             qMax(0, target_type->findData(Application::target_kind(resolved))));
                         refresh_profiles();
                         if (name->text().isEmpty() || name->text() == name->property("resolved_name").toString())
                             name->setText(resolved.name);
                         name->setProperty("resolved_name", resolved.name);
                         arguments->setText(resolved.arguments);
                         directory->setText(resolved.working_directory);
                     });
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
        app_record updated = record;
        updated.id = dialog->property("saved_application_id").isValid()
                         ? dialog->property("saved_application_id").toLongLong()
                         : record.id;
        updated.name = name->text().trimmed();
        updated.target_kind = target_type->currentData().toString();
        updated.path =
            Application::file_target(updated) ? QDir::fromNativeSeparators(path->text()) : path->text().trimmed();
        updated.default_launch_profile = profiles->currentData().toString();
        updated.arguments = arguments->isEnabled() ? arguments->text() : QString{};
        updated.working_directory = directory->isEnabled() ? directory->text() : QString{};
        if (tags->text() != record.tags.join(", "))
            updated.tags = parse_tags(tags->text());
        if (aliases->document()->isModified()) {
            updated.aliases.clear();
            for (auto alias : aliases->toPlainText().split('\n', Qt::SkipEmptyParts)) {
                alias = alias.trimmed();
                if (!alias.isEmpty() && !std::ranges::any_of(updated.aliases, [&](const QString &existing) {
                        return tag_name_key(existing) == tag_name_key(alias);
                    }))
                    updated.aliases.append(alias);
            }
        }
        updated.favorite = favorite->isChecked();
        try {
            if (updated.name.isEmpty())
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Enter an application name.").toStdString());
            Application::validate_launch_target(updated);
        } catch (const std::exception &failure) {
            error->setText(QString::fromUtf8(failure.what()));
            return;
        }
        QString shortcut_error;
        if (!service->validate_target_shortcut(false, updated.id, shortcut->keySequence(), shortcut_error)) {
            error->setText(shortcut_error);
            return;
        }
        if (!updated.working_directory.isEmpty() && !QFileInfo(updated.working_directory).isAbsolute()) {
            error->setText(QCoreApplication::translate("XLibrary", "The working directory must be an absolute path."));
            return;
        }
        for (const auto &tag : updated.tags)
            if (!valid_tag_name(tag)) {
                error->setText(
                    QCoreApplication::translate("XLibrary", "Tag: up to 64 characters, without spaces, commas, or #."));
                return;
            }
#ifdef Q_OS_WIN
        if (QFileInfo(updated.path).suffix().compare("lnk", Qt::CaseInsensitive) == 0) {
            browse->setEnabled(false);
            dialog->setProperty("resolve_request_id", QVariant::fromValue(service->resolve_application(updated.path)));
            error->setText(
                QCoreApplication::translate("XLibrary", "Shortcut loaded. Check its parameters and save again."));
            return;
        }
#endif
        error->clear();
        save->setEnabled(false);
        dialog->setProperty("save_request_id", QVariant::fromValue(service->save_application(std::move(updated))));
    });
    QObject::connect(service, &DesktopService::application_saved, dialog, [=](quint64 request_id, qint64 id) {
        if (request_id == dialog->property("save_request_id").toULongLong())
            dialog->setProperty("saved_application_id", id);
    });
    QObject::connect(service, &DesktopService::operation_changed, dialog, [=](const operation_result &result) {
        if (result.operation == "target_shortcut" && dialog->property("waiting_shortcut").toBool()) {
            if (result.state == operation_state::loading)
                dialog->setProperty("shortcut_request_id", QVariant::fromValue(result.id));
            else if (result.id == dialog->property("shortcut_request_id").toULongLong()) {
                dialog->setProperty("waiting_shortcut", false);
                if (result.state == operation_state::completed)
                    dialog->accept();
                else {
                    save->setEnabled(true);
                    error->setText(result.error.user_message);
                }
            }
            return;
        }
        if (result.state == operation_state::loading)
            return;
        if (result.id == dialog->property("resolve_request_id").toULongLong()) {
            dialog->setProperty("resolve_request_id", {});
            update_target_fields();
            if (result.state == operation_state::error)
                error->setText(result.error.user_message);
        }
        if (result.id == dialog->property("save_request_id").toULongLong()) {
            dialog->setProperty("save_request_id", {});
            if (result.state == operation_state::completed) {
                QString shortcut_error;
                const auto id = dialog->property("saved_application_id").toLongLong();
                const bool changed = service->target_shortcut(false, id) != shortcut->keySequence();
                dialog->setProperty("waiting_shortcut", changed);
                if (service->set_target_shortcut(false, id, shortcut->keySequence(), shortcut_error)) {
                    if (!changed)
                        dialog->accept();
                } else {
                    dialog->setProperty("waiting_shortcut", false);
                    save->setEnabled(true);
                    error->setText(shortcut_error);
                }
            } else {
                save->setEnabled(true);
                error->setText(result.error.user_message);
            }
        }
    });
    return dialog;
}

QDialog *tag_editor(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    const auto old_name = context.parameters.value("tag").toString();
    auto *dialog = new QDialog(parent);
    dialog->setWindowTitle(old_name.isEmpty() ? QCoreApplication::translate("XLibrary", "Add tag")
                                              : QCoreApplication::translate("XLibrary", "Edit tag"));
    prepare_dialog(dialog, context, {440, 260}, {320, 220});
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->addWidget(label(dialog->windowTitle(), "subheading"));
    auto *name = new QLineEdit(old_name);
    name->setMaxLength(64);
    name->setAccessibleName(QCoreApplication::translate("XLibrary", "Tag name"));
    name->setPlaceholderText(QCoreApplication::translate("XLibrary", "Tag name"));
    layout->addWidget(name);
    layout->addWidget(
        label(QCoreApplication::translate("XLibrary", "Up to 64 characters, without spaces, commas, or #."), "muted"));
    auto *error = label("", "error");
    layout->addWidget(error);
    layout->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(QCoreApplication::translate("XLibrary", "Save"));
    buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("XLibrary", "Cancel"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
        const auto tag = name->text().trimmed();
        if (!valid_tag_name(tag)) {
            error->setText(
                QCoreApplication::translate("XLibrary", "Up to 64 characters, without spaces, commas, or #."));
            name->setFocus();
            return;
        }
        error->clear();
        buttons->button(QDialogButtonBox::Save)->setEnabled(false);
        dialog->setProperty("save_request_id", QVariant::fromValue(service->save_tag(old_name, tag)));
    });
    QObject::connect(service, &DesktopService::operation_changed, dialog, [=](const operation_result &result) {
        if (result.id == dialog->property("save_request_id").toULongLong() &&
            result.state != operation_state::loading) {
            dialog->setProperty("save_request_id", {});
            if (result.state == operation_state::completed)
                dialog->accept();
            else {
                buttons->button(QDialogButtonBox::Save)->setEnabled(true);
                error->setText(result.error.user_message);
            }
        }
    });
    return dialog;
}

QDialog *scenario_editor(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    const auto record = context.parameters.value("scenario").value<scenario_record>();
    auto *dialog = new QDialog(parent);
    dialog->setWindowTitle(record.id ? QCoreApplication::translate("XLibrary", "Edit scenario")
                                     : QCoreApplication::translate("XLibrary", "Create scenario"));
    prepare_dialog(dialog, context, {820, 680}, {480, 420});
    auto *root = new QVBoxLayout(dialog);
    root->setContentsMargins(24, 24, 24, 24);
    root->addWidget(label(dialog->windowTitle(), "heading"));
    auto *form = new QWidget;
    auto *form_layout = new QVBoxLayout(form);
    form_layout->setContentsMargins(0, 8, 0, 8);
    auto *name = new QLineEdit(record.name);
    name->setAccessibleName(QCoreApplication::translate("XLibrary", "Scenario name"));
    name->setMaxLength(128);
    form_layout->addWidget(label(QCoreApplication::translate("XLibrary", "Scenario name"), "muted"));
    form_layout->addWidget(name);
    form_layout->addSpacing(16);
    form_layout->addWidget(
        label(QCoreApplication::translate("XLibrary", "Applications · delay before launch"), "muted"));
    auto *shortcut = new QKeySequenceEdit(service->target_shortcut(true, record.id));
    shortcut->setMaximumSequenceLength(1);
    shortcut->setObjectName("scenario_hotkey");
    field(form_layout, QCoreApplication::translate("XLibrary", "Global hotkey"), shortcut);
    if (record.id > 0)
        form_layout->addWidget(label(QString("XLibrary --scenario-id %1").arg(record.id), "muted"));
    auto *table = new QTableWidget(0, 4);
    table->setHorizontalHeaderLabels({QCoreApplication::translate("XLibrary", "Enabled"),
                                      QCoreApplication::translate("XLibrary", "Application"),
                                      QCoreApplication::translate("XLibrary", "Delay, s"),
                                      QCoreApplication::translate("XLibrary", "Launch profile")});
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->resizeSection(0, appearance::scaled(*context.settings, 95));
    table->horizontalHeader()->resizeSection(2, appearance::scaled(*context.settings, 140));
    table->verticalHeader()->hide();
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAccessibleName(QCoreApplication::translate("XLibrary", "Scenario steps"));
    table->setMinimumHeight(appearance::scaled(*context.settings, 120));
    auto add_step = [=](const scenario_step &step) {
        const auto row = table->rowCount();
        table->insertRow(row);
        auto *enabled = new QTableWidgetItem;
        enabled->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        enabled->setCheckState(step.enabled ? Qt::Checked : Qt::Unchecked);
        table->setItem(row, 0, enabled);
        auto *apps = new QComboBox;
        apps->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        for (const auto &application : service->model.applications())
            apps->addItem(application.name, application.id);
        apps->setAccessibleName(QCoreApplication::translate("XLibrary", "Application for step ") +
                                QString::number(row + 1));
        apps->setCurrentIndex(qMax(0, apps->findData(step.application_id)));
        table->setCellWidget(row, 1, apps);
        auto *delay = new QSpinBox;
        delay->setRange(0, 3600);
        delay->setValue(step.delay_ms / 1000);
        delay->setAccessibleName(QCoreApplication::translate("XLibrary", "Delay before step ") +
                                 QString::number(row + 1) + QCoreApplication::translate("XLibrary", ", seconds"));
        table->setCellWidget(row, 2, delay);
        auto *profiles = new QComboBox;
        profiles->setAccessibleName(QCoreApplication::translate("XLibrary", "Launch profile"));
        const auto update_profiles = [=] {
            const auto previous = profiles->count() ? profiles->currentData().toString() : step.launch_profile;
            profiles->clear();
            profiles->addItem(QCoreApplication::translate("XLibrary", "Application default"), QString{});
            if (const auto *application = service->model.find(apps->currentData().toLongLong()))
                for (const auto &profile : service->launch_profiles(*application))
                    profiles->addItem(profile.icon_path.isEmpty()
                                          ? appearance::icon(profile.kind == "private" ? "lock" : "app")
                                          : QIcon(profile.icon_path),
                                      profile.name, profile.id);
            if (!previous.isEmpty() && profiles->findData(previous) < 0)
                profiles->addItem(QCoreApplication::translate("XLibrary", "Unavailable profile: %1").arg(previous),
                                  previous);
            profiles->setCurrentIndex(qMax(0, profiles->findData(previous)));
        };
        QObject::connect(apps, &QComboBox::currentIndexChanged, dialog, [=] {
            profiles->setCurrentIndex(0);
            update_profiles();
        });
        QObject::connect(service, &DesktopService::launch_profiles_changed, profiles, [=](const QString &changed_path) {
            const auto *application = service->model.find(apps->currentData().toLongLong());
            if (application && application->path == changed_path)
                update_profiles();
        });
        update_profiles();
        table->setCellWidget(row, 3, profiles);
        table->setRowHeight(row, qMax(apps->sizeHint().height(), delay->sizeHint().height()) + 8);
    };
    for (const auto &step : record.steps)
        add_step(step);
    form_layout->addWidget(table, 1);
    auto *tools = new QGridLayout;
    auto *add = button(QCoreApplication::translate("XLibrary", "+ Add application"));
    auto *remove = button(QCoreApplication::translate("XLibrary", "Remove step"));
    auto *up = button(QCoreApplication::translate("XLibrary", "Move up"));
    auto *down = button(QCoreApplication::translate("XLibrary", "Move down"));
    tools->addWidget(add, 0, 0);
    tools->addWidget(remove, 0, 1);
    tools->addWidget(up, 1, 0);
    tools->addWidget(down, 1, 1);
    form_layout->addLayout(tools);
    root->addWidget(scroll(form), 1);
    auto read_steps = [=] {
        QVector<scenario_step> steps;
        for (int row = 0; row < table->rowCount(); ++row) {
            auto *apps = qobject_cast<QComboBox *>(table->cellWidget(row, 1));
            auto *delay = qobject_cast<QSpinBox *>(table->cellWidget(row, 2));
            auto *profiles = qobject_cast<QComboBox *>(table->cellWidget(row, 3));
            steps.append({apps->currentData().toLongLong(), delay->value() * 1000,
                          table->item(row, 0)->checkState() == Qt::Checked, profiles->currentData().toString()});
        }
        return steps;
    };
    QObject::connect(add, &QPushButton::clicked, dialog, [=] {
        if (service->model.rowCount() > 0) {
            add_step({});
            table->selectRow(table->rowCount() - 1);
        }
    });
    QObject::connect(remove, &QPushButton::clicked, dialog, [=] {
        if (table->currentRow() >= 0)
            table->removeRow(table->currentRow());
    });
    auto move_step = [=](const int offset) {
        const auto row = table->currentRow();
        if (row < 0 || row + offset < 0 || row + offset >= table->rowCount())
            return;
        auto steps = read_steps();
        steps.swapItemsAt(row, row + offset);
        table->setRowCount(0);
        for (const auto &step : steps)
            add_step(step);
        table->selectRow(row + offset);
    };
    QObject::connect(up, &QPushButton::clicked, dialog, [=] { move_step(-1); });
    QObject::connect(down, &QPushButton::clicked, dialog, [=] { move_step(1); });
    auto *error = label("", "error");
    root->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(QCoreApplication::translate("XLibrary", "Save"));
    buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("XLibrary", "Cancel"));
    auto *save_and_run = button(QCoreApplication::translate("XLibrary", "Save and run scenario"));
    save_and_run->setAutoDefault(false);
    root->addWidget(save_and_run);
    root->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    auto save_scenario = [=](const bool run) {
        scenario_record updated{dialog->property("saved_scenario_id").isValid()
                                    ? dialog->property("saved_scenario_id").toLongLong()
                                    : record.id,
                                name->text().trimmed(), read_steps()};
        QString shortcut_error;
        if (!service->validate_target_shortcut(true, updated.id, shortcut->keySequence(), shortcut_error)) {
            error->setText(shortcut_error);
            return;
        }
        if (updated.name.isEmpty() || updated.steps.isEmpty()) {
            error->setText(QCoreApplication::translate("XLibrary", "Enter a name and add an application."));
            return;
        }
        dialog->setProperty("run_after_save", run);
        error->clear();
        buttons->button(QDialogButtonBox::Save)->setEnabled(false);
        save_and_run->setEnabled(false);
        dialog->setProperty("save_request_id", QVariant::fromValue(service->save_scenario(std::move(updated))));
    };
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=] { save_scenario(false); });
    QObject::connect(save_and_run, &QPushButton::clicked, dialog, [=] { save_scenario(true); });
    QObject::connect(service, &DesktopService::scenario_saved, dialog, [=](const quint64 request_id, const qint64 id) {
        if (request_id == dialog->property("save_request_id").toULongLong())
            dialog->setProperty("saved_scenario_id", id);
    });
    QObject::connect(service, &DesktopService::operation_changed, dialog, [=](const operation_result &result) {
        if (result.operation == "target_shortcut" && dialog->property("waiting_shortcut").toBool()) {
            if (result.state == operation_state::loading)
                dialog->setProperty("shortcut_request_id", QVariant::fromValue(result.id));
            else if (result.id == dialog->property("shortcut_request_id").toULongLong()) {
                dialog->setProperty("waiting_shortcut", false);
                if (result.state == operation_state::completed) {
                    if (dialog->property("run_after_save").toBool())
                        service->run_scenario(dialog->property("saved_scenario_id").toLongLong());
                    dialog->accept();
                } else {
                    buttons->button(QDialogButtonBox::Save)->setEnabled(true);
                    save_and_run->setEnabled(true);
                    error->setText(result.error.user_message);
                }
            }
            return;
        }
        if (result.id == dialog->property("save_request_id").toULongLong() &&
            result.state != operation_state::loading) {
            dialog->setProperty("save_request_id", {});
            if (result.state == operation_state::completed) {
                QString shortcut_error;
                const auto id = dialog->property("saved_scenario_id").toLongLong();
                const bool changed = service->target_shortcut(true, id) != shortcut->keySequence();
                dialog->setProperty("waiting_shortcut", changed);
                if (service->set_target_shortcut(true, id, shortcut->keySequence(), shortcut_error)) {
                    if (!changed) {
                        if (dialog->property("run_after_save").toBool())
                            service->run_scenario(id);
                        dialog->accept();
                    }
                } else {
                    dialog->setProperty("waiting_shortcut", false);
                    buttons->button(QDialogButtonBox::Save)->setEnabled(true);
                    save_and_run->setEnabled(true);
                    error->setText(shortcut_error);
                }
            } else {
                buttons->button(QDialogButtonBox::Save)->setEnabled(true);
                save_and_run->setEnabled(true);
                error->setText(result.error.user_message);
            }
        }
    });
    return dialog;
}
}

void register_editors(UiRegistry &registry) {
    registry.add_default("application_editor", application_editor);
    registry.add_default("tag_editor", tag_editor);
    registry.add_default("scenario_editor", scenario_editor);
}

void edit_application(DesktopService &service, const qint64 id, QWidget *const parent, const QString &focus_field) {
    const auto *record = service.model.find(id);
    if (!record)
        return;
    ui_context context;
    context.service = &service;
    context.settings = &service.settings;
    context.parameters.insert("record", QVariant::fromValue(*record));
    auto *dialog = application_editor(context, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (!focus_field.isEmpty())
        if (auto *field = dialog->findChild<QWidget *>(focus_field))
            QTimer::singleShot(0, field, [field] { field->setFocus(Qt::OtherFocusReason); });
    dialog->open();
}
