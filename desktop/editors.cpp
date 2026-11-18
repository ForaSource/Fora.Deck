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
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabBar>
#include <QTableWidget>
#include <QTextDocument>
#include <QTimer>
#include <QWindow>

#include "Application.h"
#include "DesktopService.h"
#include "editors.h"
#include "localization.h"
#include "UiHelpers.h"

void confirm_application_termination(DesktopService &service, const qint64 id, QWidget *const parent) {
    const auto *record = service.model.find(id);
    if (!record || !record->running)
        return;
    const auto name = record->name;
    if (ui_helpers::confirm(parent, QCoreApplication::translate("XLibrary", "Terminate application"),
                            QCoreApplication::translate(
                                "XLibrary", "Force terminate all processes of %1? Unsaved work may be lost.")
                                .arg(name), QCoreApplication::translate("XLibrary", "Terminate")))
        service.terminate_application(id);
}

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
    const bool editing = record.id > 0;
    const auto *settings = context.settings;
    auto *dialog = new QDialog(parent);
    dialog->setWindowTitle(editing ? QCoreApplication::translate("XLibrary", "Edit application")
                                   : QCoreApplication::translate("XLibrary", "Add application"));
    dialog->setProperty("fit_content", true);
    auto *fit_timer = new QTimer(dialog);
    fit_timer->setSingleShot(true);
    const auto request_fit = [fit_timer] { fit_timer->start(0); };
    auto *root = new QVBoxLayout(dialog);
    const int margin = appearance::scaled(*settings, 18);
    root->setContentsMargins(margin, margin, margin, margin);
    root->setSpacing(appearance::scaled(*settings, 10));
    const auto form_spacing = appearance::scaled(*settings, 8);
    auto *heading = new QHBoxLayout;
    heading->setSpacing(form_spacing);
    auto *heading_icon = new QLabel;
    const auto heading_size = appearance::scaled(*settings, 24);
    heading_icon->setPixmap(
        appearance::icon(editing ? "edit" : "add_app", dialog->palette().color(QPalette::Link))
            .pixmap(heading_size, heading_size));
    heading->addWidget(heading_icon);
    heading->addWidget(label(dialog->windowTitle(), "heading"), 1);
    root->addLayout(heading);
    const auto add_field = [=](QVBoxLayout *const layout, const QString &text, QWidget *const control,
                               QWidget *const buddy = nullptr) {
        auto *fields = new QVBoxLayout;
        fields->setSpacing(appearance::scaled(*settings, 4));
        auto *caption = label(text, "muted");
        auto *focus_control = buddy ? buddy : control;
        caption->setBuddy(focus_control);
        if (focus_control->accessibleName().isEmpty())
            focus_control->setAccessibleName(text);
        fields->addWidget(caption);
        fields->addWidget(control);
        layout->addLayout(fields);
        return caption;
    };
    auto *form = new QWidget;
    form->setBackgroundRole(QPalette::Base);
    auto *form_layout = new QVBoxLayout(form);
    form_layout->setContentsMargins(0, 2, 0, 2);
    form_layout->setSpacing(form_spacing);
    auto *target_type = new QComboBox;
    target_type->setObjectName("application_target_type");
    target_type->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    target_type->setMinimumContentsLength(10);
    if (editing) {
        target_type->addItem(QCoreApplication::translate("XLibrary", "Application file"), "executable");
        target_type->addItem(QCoreApplication::translate("XLibrary", "Desktop shortcut"), "desktop");
    } else {
        target_type->addItem(QCoreApplication::translate("XLibrary", "Application file or shortcut"), QString{});
    }
    target_type->addItem(QCoreApplication::translate("XLibrary", "Steam game (AppID)"), "steam");
    target_type->addItem(QCoreApplication::translate("XLibrary", "Windows settings URI"), "system_uri");
    target_type->addItem(QCoreApplication::translate("XLibrary", "Windows application (AUMID)"), "aumid");
    if (editing)
        target_type->setCurrentIndex(qMax(0, target_type->findData(Application::target_kind(record))));
    auto *path = new QLineEdit;
    path->setMaxLength(static_cast<int>(catalog_limits::text_length));
    path->setText(record.path);
    path->setObjectName("application_path");
    auto *browse = button(QCoreApplication::translate("XLibrary", "Choose file…"), nullptr, "folder");
    auto *file_row = new QWidget;
    auto *file_layout = new QHBoxLayout(file_row);
    file_layout->setContentsMargins(0, 0, 0, 0);
    file_layout->addWidget(path, 1);
    file_layout->addWidget(browse);
    auto *path_caption =
        add_field(form_layout, QCoreApplication::translate("XLibrary", "Launch target"), file_row, path);
    auto *name = new QLineEdit(record.name);
    name->setObjectName("application_name");
    name->setMaxLength(256);
    const auto read_target = [=] {
        app_record target;
        target.target_kind = target_type->currentData().toString();
        const bool file = Application::file_target(target);
        target.path = file ? QDir::fromNativeSeparators(path->text()) : path->text().trimmed();
#ifdef Q_OS_WIN
        if (file) {
            // Explorer's Copy as path wraps the absolute file path in quotes.
            const auto pasted = target.path.trimmed();
            if (pasted.size() >= 2 && pasted.startsWith('"') && pasted.endsWith('"'))
                target.path = pasted.mid(1, pasted.size() - 2);
        }
#endif
        target.target_kind = Application::target_kind(target);
        return target;
    };
    auto *name_row = new QWidget;
    auto *name_layout = new QHBoxLayout(name_row);
    name_layout->setContentsMargins(0, 0, 0, 0);
    name_layout->setSpacing(form_spacing);
    auto *preview = new QLabel;
    preview->setObjectName("application_icon_preview");
    preview->setAlignment(Qt::AlignCenter);
    const int tile_size = appearance::scaled(*settings, 36);
    preview->setFixedSize(tile_size, tile_size);
    name_layout->addWidget(preview);
    name_layout->addWidget(name, 1);
    add_field(form_layout, QCoreApplication::translate("XLibrary", "Name"), name_row, name);

    const int icon_size = appearance::scaled(*settings, 24);
    const auto show_icon = [=](const app_record &target) {
        preview->setPixmap(service->model.application_icon(target).pixmap(
            QSize(icon_size, icon_size), preview->devicePixelRatioF()));
    };
    auto *icon_timer = new QTimer(preview);
    icon_timer->setSingleShot(true);
    icon_timer->setInterval(160);
    const auto original_kind = Application::target_kind(record);
    const auto original_path = Application::file_target(record) ? QDir::fromNativeSeparators(record.path) : record.path;
    const auto refresh_icon = [=] {
        const auto target = read_target();
        show_icon(target.target_kind == original_kind && target.path == original_path ? record : target);
    };
    const auto schedule_icon = [=] {
        app_record placeholder;
        placeholder.target_kind = target_type->currentData().toString();
        show_icon(placeholder);
        icon_timer->start();
    };
    QObject::connect(path, &QLineEdit::textChanged, preview, schedule_icon);
    QObject::connect(target_type, &QComboBox::currentIndexChanged, preview, schedule_icon);
    QObject::connect(icon_timer, &QTimer::timeout, preview, refresh_icon);
    QObject::connect(&service->model, &LibraryModel::icon_ready, preview, [=] {
        if (!icon_timer->isActive())
            refresh_icon();
    });
    refresh_icon();

    auto *more = button(QCoreApplication::translate("XLibrary", "More options"), "quiet", "chevron_right");
    more->setProperty("disclosure", true);
    more->setCheckable(true);
    more->setAutoDefault(false);
    form_layout->addWidget(more, 0, Qt::AlignLeft);
    auto *options = new QWidget;
    form_layout->addWidget(options);
    auto *options_layout = new QVBoxLayout(options);
    options_layout->setContentsMargins(0, 0, 0, 0);
    options_layout->setSpacing(form_spacing);
    auto *tabs = new QTabBar;
    tabs->setObjectName("application_options_tabs");
    tabs->setDrawBase(false);
    tabs->setExpanding(false);
    tabs->setElideMode(Qt::ElideRight);
    const int launch_tab =
        tabs->addTab(appearance::icon("play"), QCoreApplication::translate("XLibrary", "Launch settings"));
    const int library_tab =
        tabs->addTab(appearance::icon("tag"), QCoreApplication::translate("XLibrary", "In the library"));
    options_layout->addWidget(tabs);
    auto *launch_page = new QWidget;
    auto *library_page = new QWidget;
    options_layout->addWidget(launch_page);
    options_layout->addWidget(library_page);
    auto *launch_layout = new QVBoxLayout(launch_page);
    auto *library_layout = new QVBoxLayout(library_page);
    for (auto *page_layout : {launch_layout, library_layout}) {
        page_layout->setContentsMargins(0, 2, 0, 0);
        page_layout->setSpacing(form_spacing);
    }
    add_field(launch_layout, QCoreApplication::translate("XLibrary", "Launch type"), target_type);
    library_page->hide();
    QObject::connect(tabs, &QTabBar::currentChanged, dialog, [=](const int index) {
        launch_page->setVisible(index == launch_tab);
        library_page->setVisible(index == library_tab);
        request_fit();
    });
    options->hide();
    QObject::connect(more, &QPushButton::toggled, dialog, [=](const bool expanded) {
        const QString icon_name = expanded ? "chevron_down" : "chevron_right";
        more->setProperty("appearance_icon", icon_name);
        more->setIcon(appearance::icon(icon_name));
        options->setVisible(expanded);
        request_fit();
    });
    auto *aliases = new QPlainTextEdit(record.aliases.join('\n'));
    aliases->setTabChangesFocus(true);
    aliases->setObjectName("application_aliases");
    auto *file_options = new QWidget;
    auto *file_options_layout = new QVBoxLayout(file_options);
    file_options_layout->setContentsMargins(0, 0, 0, 0);
    file_options_layout->setSpacing(form_spacing);
    launch_layout->addWidget(file_options);
    auto *arguments = new QLineEdit;
    arguments->setMaxLength(static_cast<int>(catalog_limits::text_length));
    arguments->setText(record.arguments);
    arguments->setObjectName("application_arguments");
    add_field(file_options_layout, QCoreApplication::translate("XLibrary", "Launch arguments"), arguments);
    auto *directory = new QLineEdit;
    directory->setMaxLength(static_cast<int>(catalog_limits::text_length));
    directory->setText(record.working_directory);
    directory->setPlaceholderText(QCoreApplication::translate("XLibrary", "Default: application folder"));
    add_field(file_options_layout, QCoreApplication::translate("XLibrary", "Working directory"), directory);
    auto *profiles = new QComboBox;
    profiles->setObjectName("application_launch_profile");
    profiles->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    profiles->setMinimumContentsLength(10);
    auto *profile_field = new QWidget;
    auto *profile_layout = new QVBoxLayout(profile_field);
    profile_layout->setContentsMargins(0, 0, 0, 0);
    profile_layout->setSpacing(6);
    add_field(profile_layout, QCoreApplication::translate("XLibrary", "Default browser launch"), profiles);
    launch_layout->addWidget(profile_field);
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
        const bool has_profiles = profiles->count() > 1;
        profiles->setEnabled(has_profiles);
        profile_field->setVisible(has_profiles);
        request_fit();
    };
    QObject::connect(service, &DesktopService::launch_profiles_changed, dialog, [=](const QString &changed_path) {
        if (changed_path == path->text())
            refresh_profiles();
    });
    const auto update_target_fields = [=] {
        const auto kind = target_type->currentData().toString();
        const bool file = kind.isEmpty() || kind == "executable" || kind == "desktop";
        browse->setVisible(file);
        browse->setEnabled(file && !dialog->property("resolve_request_id").isValid());
        arguments->setEnabled(file);
        directory->setEnabled(file);
        file_options->setVisible(file);
        path_caption->setText(file ? QCoreApplication::translate("XLibrary", "Application file or shortcut")
                                   : target_type->currentText());
        name->setPlaceholderText(!editing && file ? QCoreApplication::translate("XLibrary", "Filled in from the file")
                                                  : QString{});
        path->setAccessibleName(path_caption->text());
        const auto file_hint = QCoreApplication::translate("XLibrary", "Or paste a file path");
        path->setPlaceholderText(kind == "steam"        ? QString("730")
                                 : kind == "system_uri" ? QString("ms-settings:")
                                 : kind == "aumid"      ? QString("PackageFamilyName!ApplicationId")
                                                        : file_hint);
        refresh_profiles();
    };
    QObject::connect(target_type, &QComboBox::currentIndexChanged, dialog, update_target_fields);
    QObject::connect(path, &QLineEdit::editingFinished, dialog, refresh_profiles);
    update_target_fields();
    auto *shortcut = new QKeySequenceEdit(service->target_shortcut(false, record.id));
    shortcut->setMaximumSequenceLength(1);
    shortcut->setClearButtonEnabled(true);
    shortcut->setObjectName("application_hotkey");
    add_field(launch_layout, QCoreApplication::translate("XLibrary", "Global hotkey"), shortcut);
    if (editing) {
        auto *command = label(QString("XLibrary --launch-id %1").arg(record.id), "muted");
        command->setTextInteractionFlags(Qt::TextSelectableByMouse);
        launch_layout->addWidget(command);
    }
    auto *tags = new QLineEdit(record.tags.join(", "));
    tags->setObjectName("application_tags");
    tags->setPlaceholderText(QCoreApplication::translate("XLibrary", "development, work"));
    add_field(library_layout, QCoreApplication::translate("XLibrary", "Tags separated by commas"), tags);
    auto *tag_search = new QLineEdit;
    tag_search->setPlaceholderText(QCoreApplication::translate("XLibrary", "Search tags"));
    tag_search->setAccessibleName(tag_search->placeholderText());
    library_layout->addWidget(tag_search);
    auto *tag_list = new QListWidget;
    tag_list->setObjectName("application_editor_tags");
    tag_list->setProperty("tag_selector", true);
    for (const auto &tag : service->catalog().tags) {
        qsizetype usage = 0;
        for (const auto &application : service->catalog().applications)
            usage += has_tag(application.tags, tag);
        auto *item = new QListWidgetItem(QString("#%1 (%2)").arg(tag, QString::number(usage)), tag_list);
        item->setData(Qt::UserRole, tag);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(has_tag(record.tags, tag) ? Qt::Checked : Qt::Unchecked);
    }
    library_layout->addWidget(tag_list);
    const auto fit_tag_list = [=] {
        int height = 2 * tag_list->frameWidth();
        int rows = 0;
        for (int index = 0; index < tag_list->count() && rows < 4; ++index) {
            if (tag_list->item(index)->isHidden())
                continue;
            height += tag_list->sizeHintForRow(index);
            ++rows;
        }
        tag_list->setFixedHeight(height);
        tag_list->setVisible(rows > 0);
    };
    tag_search->setVisible(tag_list->count() > 4);
    tag_list->ensurePolished();
    fit_tag_list();
    QObject::connect(tag_search, &QLineEdit::textChanged, dialog, [=](const QString &text) {
        for (int row = 0; row < tag_list->count(); ++row)
            tag_list->item(row)->setHidden(!tag_list->item(row)->text().contains(text.trimmed(), Qt::CaseInsensitive));
        fit_tag_list();
        request_fit();
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
    library_layout->addWidget(favorite);
    add_field(library_layout, QCoreApplication::translate("XLibrary", "Search aliases, one per line"), aliases);
    aliases->ensurePolished();
    aliases->setFixedHeight(2 * aliases->fontMetrics().lineSpacing() + appearance::scaled(*settings, 16));
    auto *error = label("", "error");
    const auto set_error = [=](const QString &message) {
        error->setText(message);
        error->setVisible(!message.isEmpty());
        request_fit();
    };
    error->hide();
    form_layout->addStretch();
    auto *body = scroll(form);
    root->addWidget(body, 1);
    root->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save);
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("XLibrary", "Cancel"));
    auto *save = buttons->button(QDialogButtonBox::Save);
    save->setText(editing ? QCoreApplication::translate("XLibrary", "Save")
                          : QCoreApplication::translate("XLibrary", "Add"));
    save->setProperty("primary", true);
    save->setDefault(true);
    root->addWidget(buttons);
    const auto focus_name = context.parameters.value("focus_field").toString();
    auto *focus = focus_name.isEmpty() ? nullptr : dialog->findChild<QWidget *>(focus_name);
    if (focus && (launch_page->isAncestorOf(focus) || library_page->isAncestorOf(focus))) {
        tabs->setCurrentIndex(library_page->isAncestorOf(focus) ? library_tab : launch_tab);
        more->setChecked(true);
    }
    const auto fit = [=] {
        const auto available = dialog->screen()->availableGeometry().marginsRemoved({12, 12, 12, 12});
        const auto frame_size = dialog->frameGeometry().size() - dialog->size();
        const auto limit = available.size() - frame_size;
        if (limit.isEmpty())
            return;
        const int width = qMin(appearance::scaled(*settings, 484), limit.width());
        const int inner_width = qMax(1, width - 2 * margin);
        const int form_width = qMax(inner_width, form->minimumSizeHint().width());
        int height = 2 * margin;
        for (int index = 0; index < root->count(); ++index) {
            auto *item = root->itemAt(index);
            if (item->isEmpty())
                continue;
            if (item->widget() == body) {
                height += form->hasHeightForWidth() ? form->heightForWidth(form_width) : form->sizeHint().height();
                if (form_width > inner_width)
                    height += body->horizontalScrollBar()->sizeHint().height();
            } else {
                height += item->hasHeightForWidth() ? item->heightForWidth(inner_width) : item->sizeHint().height();
            }
            height += root->spacing();
        }
        dialog->setFixedSize(width, qMin(height - root->spacing(), limit.height()));
        if (dialog->isVisible()) {
            const auto frame = dialog->frameGeometry();
            const int max_x = qMax(available.left(), available.right() - frame.width() + 1);
            const int max_y = qMax(available.top(), available.bottom() - frame.height() + 1);
            const QPoint position(qBound(available.left(), frame.x(), max_x),
                                  qBound(available.top(), frame.y(), max_y));
            if (frame.topLeft() != position)
                dialog->move(dialog->pos() + position - frame.topLeft());
        }
    };
    dialog->ensurePolished();
    fit();
    QObject::connect(fit_timer, &QTimer::timeout, dialog, fit);
    QObject::connect(more, &QPushButton::toggled, body, [=](const bool expanded) {
        if (!expanded)
            return;
        QObject::connect(fit_timer, &QTimer::timeout, body, [=] {
            // The scrollbar range follows the resized viewport on the next layout pass.
            QTimer::singleShot(0, body, [=] {
                if (tabs->isVisible())
                    body->verticalScrollBar()->setValue(more->mapTo(form, QPoint{}).y() - form_spacing);
            });
        }, Qt::SingleShotConnection);
    });
    QTimer::singleShot(0, dialog, [=] {
        if (auto *window = dialog->windowHandle())
            QObject::connect(window, &QWindow::screenChanged, dialog, request_fit);
        request_fit();
        if (focus)
            QTimer::singleShot(0, focus, [=] {
                focus->setFocus(Qt::OtherFocusReason);
                body->ensureWidgetVisible(focus);
            });
    });
    const auto show_option_error = [=](const QString &message, const int tab) {
        tabs->setCurrentIndex(tab);
        more->setChecked(true);
        set_error(message);
    };
    const auto update_save = [=] {
        save->setEnabled(!dialog->property("resolve_request_id").isValid() &&
                         !dialog->property("save_request_id").isValid() &&
                         !dialog->property("waiting_shortcut").toBool() &&
                         (editing || !path->text().trimmed().isEmpty()));
    };
    const auto resolve_file = [=](const QString &file, const bool add_after_resolution) {
        path->setText(file);
        path->setReadOnly(true);
        target_type->setEnabled(false);
        browse->setEnabled(false);
        dialog->setProperty("add_after_resolution", add_after_resolution);
        dialog->setProperty("resolved_path", {});
        dialog->setProperty("resolve_request_id", QVariant::fromValue(service->resolve_application(file)));
        update_save();
    };
    QObject::connect(path, &QLineEdit::textChanged, dialog, update_save);
    QObject::connect(path, &QLineEdit::textEdited, dialog, [=] { set_error({}); });
    (editing ? static_cast<QWidget *>(name) : browse)->setFocus();
    update_save();
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(browse, &QPushButton::clicked, dialog, [=] {
#ifdef Q_OS_WIN
        const auto filter =
            QCoreApplication::translate("XLibrary", "Applications (*.exe *.lnk *.bat *.cmd);;All files (*)");
        // Read the shortcut itself to retain its name, arguments and working directory.
        constexpr QFileDialog::Options options = QFileDialog::DontResolveSymlinks;
#else
        const auto filter = QCoreApplication::translate(
            "XLibrary", "All files (*);;Application shortcuts (*.desktop);;AppImage (*.AppImage)");
        constexpr QFileDialog::Options options;
#endif
        const auto file = QFileDialog::getOpenFileName(
            dialog, QCoreApplication::translate("XLibrary", "Select an application"), {}, filter, nullptr, options);
        if (!file.isEmpty()) {
            set_error({});
            resolve_file(file, false);
        }
    });
    QObject::connect(service, &DesktopService::application_resolved, dialog,
                     [=](const quint64 request_id, const QString &requested_path, const app_record &resolved) {
                         if (request_id != dialog->property("resolve_request_id").toULongLong())
                             return;
                         if (QDir::cleanPath(path->text()) != QDir::cleanPath(requested_path))
                             return;
                         path->setText(resolved.path);
                         dialog->setProperty("resolved_path", path->text());
                         if (editing)
                             target_type->setCurrentIndex(
                                 qMax(0, target_type->findData(Application::target_kind(resolved))));
                         refresh_profiles();
                         if (name->text().trimmed().isEmpty() ||
                             name->text() == name->property("resolved_name").toString()) {
                             const auto suggested = resolved.name.trimmed().isEmpty()
                                                        ? QFileInfo(resolved.path).completeBaseName()
                                                        : resolved.name;
                             name->setText(suggested);
                             name->setProperty("resolved_name", name->text());
                         }
                         if (editing || !arguments->isModified())
                             arguments->setText(resolved.arguments);
                         if (editing || !directory->isModified())
                             directory->setText(resolved.working_directory);
                     });
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
        set_error({});
        app_record updated = record;
        updated.id = dialog->property("saved_application_id").isValid()
                         ? dialog->property("saved_application_id").toLongLong()
                         : record.id;
        updated.name = name->text().trimmed();
        const auto target = read_target();
        updated.target_kind = target.target_kind;
        updated.path = target.path;
        if (updated.target_kind != Application::target_kind(record) || updated.path != record.path) {
            updated.installation_directory.clear();
            updated.executable_path.clear();
            updated.icon_path.clear();
        }
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
        const bool needs_resolution = !editing && Application::file_target(updated) &&
                                      updated.path != dialog->property("resolved_path").toString();
        if (needs_resolution && !QFileInfo(updated.path).isAbsolute()) {
            set_error(QCoreApplication::translate("XLibrary", "Paste the full path or use Choose file…"));
            return;
        }
        try {
            if (updated.name.isEmpty() && !needs_resolution)
                throw std::invalid_argument(
                    QCoreApplication::translate("XLibrary", "Enter an application name.").toStdString());
            Application::validate_launch_target(updated);
        } catch (const std::exception &failure) {
            set_error(QString::fromUtf8(failure.what()));
            return;
        }
        if (needs_resolution) {
            resolve_file(updated.path, true);
            return;
        }
        QString shortcut_error;
        if (!service->validate_target_shortcut(false, updated.id, shortcut->keySequence(), shortcut_error)) {
            show_option_error(shortcut_error, launch_tab);
            return;
        }
        if (!updated.working_directory.isEmpty() && !QFileInfo(updated.working_directory).isAbsolute()) {
            show_option_error(
                QCoreApplication::translate("XLibrary", "The working directory must be an absolute path."), launch_tab);
            return;
        }
        for (const auto &tag : updated.tags)
            if (!valid_tag_name(tag)) {
                show_option_error(
                    QCoreApplication::translate("XLibrary", "Tag: up to 64 characters, without spaces, commas, or #."),
                    library_tab);
                return;
            }
#ifdef Q_OS_WIN
        if (Application::file_target(updated) &&
            QFileInfo(updated.path).suffix().compare("lnk", Qt::CaseInsensitive) == 0) {
            resolve_file(updated.path, false);
            set_error(
                QCoreApplication::translate("XLibrary", "Shortcut loaded. Check its parameters and save again."));
            return;
        }
#endif
        form->setEnabled(false);
        dialog->setProperty("save_request_id", QVariant::fromValue(service->save_application(std::move(updated))));
        update_save();
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
                    form->setEnabled(true);
                    update_save();
                    set_error(result.error.user_message);
                }
            }
            return;
        }
        if (result.state == operation_state::loading)
            return;
        if (result.id == dialog->property("resolve_request_id").toULongLong()) {
            dialog->setProperty("resolve_request_id", {});
            const bool add_after_resolution = dialog->property("add_after_resolution").toBool();
            dialog->setProperty("add_after_resolution", {});
            path->setReadOnly(false);
            target_type->setEnabled(true);
            update_target_fields();
            update_save();
            if (result.state == operation_state::error)
                set_error(result.error.user_message);
            else if (result.state == operation_state::completed && add_after_resolution)
                save->click();
            return;
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
                    form->setEnabled(true);
                    update_save();
                    set_error(shortcut_error);
                }
            } else {
                form->setEnabled(true);
                update_save();
                set_error(result.error.user_message);
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
    context.parameters.insert("focus_field", focus_field);
    auto *dialog = application_editor(context, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}
