#include <algorithm>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMimeData>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTreeView>
#include <QTreeWidget>

#include "DesktopService.h"
#include "editors.h"
#include "PathHelpers.h"
#include "UiHelpers.h"
#include "WorkspacesView.h"

namespace {
using ui_helpers::button;
using ui_helpers::field;
using ui_helpers::label;
using ui_helpers::prepare_dialog;

QString kind_name(const workspace_item_kind kind) {
    switch (kind) {
    case workspace_item_kind::application:
        return QCoreApplication::translate("Workspaces", "Application");
    case workspace_item_kind::file:
        return QCoreApplication::translate("Workspaces", "File");
    case workspace_item_kind::folder:
        return QCoreApplication::translate("Workspaces", "Folder");
    case workspace_item_kind::url:
        return QCoreApplication::translate("Workspaces", "Link");
    }
    return {};
}

QString add_resource_title(const workspace_item_kind kind) {
    switch (kind) {
    case workspace_item_kind::application:
        return QCoreApplication::translate("Workspaces", "Add application");
    case workspace_item_kind::file:
        return QCoreApplication::translate("Workspaces", "Add file");
    case workspace_item_kind::folder:
        return QCoreApplication::translate("Workspaces", "Add folder");
    case workspace_item_kind::url:
        return QCoreApplication::translate("Workspaces", "Add link");
    }
    return {};
}

QString kind_icon(const workspace_item_kind kind) {
    switch (kind) {
    case workspace_item_kind::application:
        return QStringLiteral("app");
    case workspace_item_kind::file:
        return QStringLiteral("file");
    case workspace_item_kind::folder:
        return QStringLiteral("folder");
    case workspace_item_kind::url:
        return QStringLiteral("external");
    }
    return {};
}

QIcon resource_icon(const workspace_item &item, const LibraryModel &model) {
    if (item.kind == workspace_item_kind::application)
        if (const auto *app = model.find(item.application_id))
            return model.application_icon(*app);
    return appearance::icon(kind_icon(item.kind));
}

void setup_table(QTreeWidget *list, const QStringList &headers, const QString &name) {
    list->setHeaderLabels(headers);
    list->setAccessibleName(name);
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setAlternatingRowColors(true);
    list->setIconSize({22, 22});
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < headers.size(); ++column)
        list->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    list->header()->setStretchLastSection(false);
}

QTreeWidget *table(const QStringList &headers, const QString &name) {
    auto *list = new QTreeWidget;
    setup_table(list, headers, name);
    return list;
}

class ResourceOrder : public QTreeWidget {
public:
    std::function<void(int, int)> reorder;

protected:
    void startDrag(Qt::DropActions) override {
        QDrag drag(this);
        drag.setMimeData(model()->mimeData(selectedIndexes()));
        drag.exec(Qt::MoveAction);
    }

    void dropEvent(QDropEvent *event) override {
        if (event->source() != this || !currentItem())
            return;
        const auto source = indexOfTopLevelItem(currentItem());
        auto *destination = itemAt(event->position().toPoint());
        const auto after = destination && event->position().y() >= visualItemRect(destination).center().y();
        const auto insertion = destination ? indexOfTopLevelItem(destination) + after : topLevelItemCount();
        const auto target = qBound(0, insertion - (source < insertion ? 1 : 0), topLevelItemCount() - 1);
        if (source != target)
            reorder(source, target);
        event->acceptProposedAction();
    }
};

QTreeWidgetItem *row(QTreeWidget *list, const QStringList &values) {
    auto *item = new QTreeWidgetItem(list, values);
    for (int column = 0; column < values.size(); ++column)
        item->setToolTip(column, values[column].toHtmlEscaped());
    return item;
}

QDialogButtonBox *dialog_buttons(QPushButton *primary) {
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    buttons->addButton(primary, QDialogButtonBox::AcceptRole);
    return buttons;
}

QFrame *empty_card(const QString &title, const QString &hint, const QString &icon_name) {
    auto *card = new QFrame;
    card->setProperty("card", true);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->addStretch();
    auto *symbol = new QLabel;
    symbol->setPixmap(appearance::icon(icon_name).pixmap(36, 36));
    layout->addWidget(symbol, 0, Qt::AlignHCenter);
    auto *heading = label(title, "subheading");
    heading->setAlignment(Qt::AlignCenter);
    layout->addWidget(heading);
    auto *description = label(hint, "muted");
    description->setAlignment(Qt::AlignCenter);
    layout->addWidget(description);
    layout->addStretch();
    return card;
}

std::optional<workspace_item> edit_item(DesktopService &service, const ui_context &context, QWidget *parent,
                                        const workspace_item &original, const bool editing) {
    QDialog dialog(parent);
    dialog.setWindowTitle(editing ? QCoreApplication::translate("Workspaces", "Edit resource")
                                  : add_resource_title(original.kind));
    prepare_dialog(&dialog, context,
                   original.kind == workspace_item_kind::application ? QSize(640, 580) : QSize(540, 290), {340, 260});
    auto *layout = new QVBoxLayout(&dialog);
    auto *contents = new QWidget;
    auto *form = new QVBoxLayout(contents);
    form->setContentsMargins(0, 0, 0, 0);
    auto *kind = new QComboBox(contents);
    for (const auto option : {workspace_item_kind::application, workspace_item_kind::file, workspace_item_kind::folder,
                              workspace_item_kind::url})
        kind->addItem(appearance::icon(kind_icon(option)), kind_name(option), static_cast<int>(option));
    kind->setCurrentIndex(static_cast<int>(original.kind));
    if (editing)
        field(form, QCoreApplication::translate("Workspaces", "Resource type"), kind);
    else
        kind->hide();

    auto *options = new QStackedWidget;
    form->addWidget(options, 1);
    auto *application_options = new QWidget;
    auto *applications_form = new QVBoxLayout(application_options);
    applications_form->setContentsMargins(0, 0, 0, 0);
    const bool keep_missing = editing && original.kind == workspace_item_kind::application && !original.application_id;
    if (keep_missing)
        applications_form->addWidget(label(
            QCoreApplication::translate("Workspaces", "Missing application · %1").arg(original.display_name), "muted"));
    auto *search = new QLineEdit;
    search->setPlaceholderText(QCoreApplication::translate("Workspaces", "Search library applications…"));
    search->setAccessibleName(search->placeholderText());
    search->setClearButtonEnabled(true);
    applications_form->addWidget(search);
    auto *filter = new LibraryFilter(&dialog);
    filter->setSourceModel(&service.model);
    auto *apps = new QTreeView;
    apps->setModel(filter);
    apps->setRootIsDecorated(false);
    apps->setUniformRowHeights(true);
    apps->setAlternatingRowColors(true);
    apps->setHeaderHidden(true);
    apps->setIconSize({24, 24});
    apps->setAccessibleName(QCoreApplication::translate("Workspaces", "Library applications"));
    apps->setColumnHidden(1, true);
    apps->setColumnHidden(2, true);
    apps->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    const auto app_row = service.model.row_for_id(original.application_id);
    if (app_row >= 0) {
        const auto index = filter->mapFromSource(service.model.index(app_row, 0));
        apps->setCurrentIndex(index);
        apps->scrollTo(index);
    }
    auto *app_empty = label({}, "muted");
    auto *app_results = new QStackedWidget;
    app_results->addWidget(apps);
    app_results->addWidget(app_empty);
    applications_form->addWidget(app_results, 1);
    const auto sync_app_results = [&] {
        const bool has_results = filter->rowCount() > 0;
        app_empty->setText(search->text().trimmed().isEmpty()
                               ? QCoreApplication::translate("Workspaces", "Add an application to the library first.")
                               : QCoreApplication::translate("Workspaces", "No applications match your search."));
        app_results->setCurrentWidget(has_results ? static_cast<QWidget *>(apps) : app_empty);
    };
    QObject::connect(filter, &QAbstractItemModel::rowsInserted, &dialog, sync_app_results);
    QObject::connect(filter, &QAbstractItemModel::rowsRemoved, &dialog, sync_app_results);
    QObject::connect(filter, &QAbstractItemModel::modelReset, &dialog, sync_app_results);
    QObject::connect(search, &QLineEdit::textChanged, &dialog, [&](const QString &query) {
        apps->selectionModel()->clear();
        filter->set_query(query);
        sync_app_results();
    });
    sync_app_results();
    const auto app_id = [&] { return apps->currentIndex().data(LibraryModel::id_role).toLongLong(); };

    auto *advanced_button =
        button(QCoreApplication::translate("Workspaces", "Launch and window options"), "quiet", "settings");
    advanced_button->setCheckable(true);
    applications_form->addWidget(advanced_button, 0, Qt::AlignLeft);
    auto *advanced = new QWidget;
    auto *advanced_form = new QVBoxLayout(advanced);
    advanced_form->setContentsMargins(0, 0, 0, 0);
    applications_form->addWidget(advanced);
    advanced->hide();
    QObject::connect(advanced_button, &QPushButton::toggled, advanced, &QWidget::setVisible);
    auto *profile_field = new QWidget;
    auto *profile_form = new QVBoxLayout(profile_field);
    profile_form->setContentsMargins(0, 0, 0, 0);
    auto *profile = new QComboBox;
    field(profile_form, QCoreApplication::translate("Workspaces", "Browser launch profile"), profile);
    advanced_form->addWidget(profile_field);
    auto *reuse =
        new QCheckBox(QCoreApplication::translate("Workspaces", "Reuse existing window when identity is verified"));
    reuse->setChecked(original.reuse_existing);
    advanced_form->addWidget(reuse);

    QHash<qint64, QString> selected_profiles;
    selected_profiles.insert(original.application_id, original.launch_profile);
    QObject::connect(profile, &QComboBox::activated, &dialog,
                     [&](int) { selected_profiles.insert(app_id(), profile->currentData().toString()); });
    const auto load_profiles = [&] {
        const auto id = app_id();
        const auto *found = service.model.find(id);
        const auto selected = selected_profiles.value(id);
        profile->clear();
        profile->addItem(QCoreApplication::translate("Workspaces", "Use application default"), QString{});
        if (found && !Application::launch_modes(*found).isEmpty()) {
            profile->addItem(QCoreApplication::translate("Workspaces", "Use configured launch"), "configured");
            for (const auto &option : service.launch_profiles(*found))
                profile->addItem(option.name, option.id);
        }
        if (id == original.application_id && !selected.isEmpty() && profile->findData(selected) < 0)
            profile->addItem(QCoreApplication::translate("Workspaces", "Unavailable profile · %1").arg(selected),
                             selected);
        profile->setCurrentIndex(qMax(0, profile->findData(selected)));
        profile_field->setVisible(profile->count() > 1);
    };
    QObject::connect(apps->selectionModel(), &QItemSelectionModel::currentChanged, &dialog, load_profiles);
    QObject::connect(filter, &QAbstractItemModel::modelReset, &dialog, load_profiles);
    QObject::connect(&service, &DesktopService::launch_profiles_changed, &dialog, load_profiles);
    load_profiles();

    const auto capabilities = service.workspace_runs().desktop();
    auto *placement = new QGroupBox(QCoreApplication::translate("Workspaces", "Restore window placement"));
    placement->setCheckable(true);
    placement->setChecked(original.placement.has_value());
    placement->setVisible(capabilities.can_place);
    placement->setAccessibleName(placement->title());
    auto *placement_form = new QFormLayout(placement);
    placement_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto *monitor = new QComboBox;
    monitor->addItem(QCoreApplication::translate("Workspaces", "Primary monitor"), QString{});
    for (const auto &screen : capabilities.monitors)
        monitor->addItem(screen.id, screen.id);
    auto *maximized = new QCheckBox(QCoreApplication::translate("Workspaces", "Maximized"));
    const auto geometry = original.placement ? original.placement->geometry : QRectF(0.05, 0.05, 0.7, 0.8);
    if (original.placement) {
        const auto &id = original.placement->monitor_id;
        if (monitor->findData(id) < 0)
            monitor->addItem(QCoreApplication::translate("Workspaces", "Unavailable monitor · %1").arg(id), id);
        monitor->setCurrentIndex(monitor->findData(id));
        maximized->setChecked(original.placement->maximized);
    }
    placement_form->addRow(QCoreApplication::translate("Workspaces", "&Monitor"), monitor);
    const auto spin = [&](const QString &caption, const double value, const double minimum) {
        auto *control = new QDoubleSpinBox;
        control->setDecimals(2);
        control->setRange(minimum, 100);
        control->setSuffix(" %");
        control->setValue(value * 100);
        control->setAccessibleName(caption);
        placement_form->addRow(caption, control);
        return control;
    };
    auto *left = spin(QCoreApplication::translate("Workspaces", "Left offset"), geometry.x(), 0);
    auto *top = spin(QCoreApplication::translate("Workspaces", "Top offset"), geometry.y(), 0);
    auto *width = spin(QCoreApplication::translate("Workspaces", "Width"), geometry.width(), 0.01);
    auto *height = spin(QCoreApplication::translate("Workspaces", "Height"), geometry.height(), 0.01);
    placement_form->addRow(maximized);
    advanced_form->addWidget(placement);
    options->addWidget(application_options);

    auto *target_options = new QWidget;
    auto *target_form = new QVBoxLayout(target_options);
    target_form->setContentsMargins(0, 0, 0, 0);
    auto *target = new QLineEdit(original.target);
    target->setMaxLength(4096);
    auto *target_label = label({}, "muted");
    target_label->setBuddy(target);
    target_form->addWidget(target_label);
    target_form->addWidget(target);
    auto *browse = button(QCoreApplication::translate("Workspaces", "Browse…"), nullptr, "folder");
    target_form->addWidget(browse, 0, Qt::AlignLeft);
    target_form->addStretch();
    options->addWidget(target_options);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
        const auto selected =
            kind->currentIndex() == static_cast<int>(workspace_item_kind::folder)
                ? QFileDialog::getExistingDirectory(&dialog, kind_name(workspace_item_kind::folder), target->text())
                : QFileDialog::getOpenFileName(&dialog, kind_name(workspace_item_kind::file), target->text());
        if (!selected.isEmpty())
            target->setText(selected);
    });
    const auto show_options = [&] {
        const auto selected_kind = static_cast<workspace_item_kind>(kind->currentData().toInt());
        const bool is_link = selected_kind == workspace_item_kind::url;
        options->setCurrentIndex(selected_kind == workspace_item_kind::application ? 0 : 1);
        const auto caption = is_link ? QCoreApplication::translate("Workspaces", "Web address")
                                     : QCoreApplication::translate("Workspaces", "File or folder path");
        target_label->setText(caption);
        target->setAccessibleName(caption);
        target->setPlaceholderText(is_link ? QStringLiteral("https://") : QString{});
        browse->setVisible(!is_link);
    };
    QObject::connect(kind, &QComboBox::currentIndexChanged, &dialog, show_options);
    show_options();
    layout->addWidget(ui_helpers::scroll(contents), 1);
    auto *error = label({}, "muted");
    error->hide();
    layout->addWidget(error);
    auto *apply = button(editing ? QCoreApplication::translate("Workspaces", "Apply")
                                 : QCoreApplication::translate("Workspaces", "Add to workspace"),
                         "primary", editing ? QStringLiteral("save") : QStringLiteral("add"));
    auto *buttons = dialog_buttons(apply);
    const auto sync_apply = [&] {
        const bool is_app = options->currentIndex() == 0;
        apply->setEnabled(is_app ? app_id() != 0 || keep_missing : !target->text().trimmed().isEmpty());
        error->hide();
    };
    QObject::connect(apps->selectionModel(), &QItemSelectionModel::currentChanged, &dialog, sync_apply);
    QObject::connect(filter, &QAbstractItemModel::modelReset, &dialog, sync_apply);
    QObject::connect(target, &QLineEdit::textChanged, &dialog, sync_apply);
    QObject::connect(options, &QStackedWidget::currentChanged, &dialog, sync_apply);
    QObject::connect(search, &QLineEdit::returnPressed, &dialog, [&] {
        if (filter->rowCount()) {
            apps->setCurrentIndex(filter->index(0, 0));
            apps->setFocus();
        }
    });
    QObject::connect(apps, &QTreeView::activated, &dialog, [&] {
        if (!editing && apply->isEnabled())
            apply->click();
    });
    sync_apply();
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    std::optional<workspace_item> result;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        workspace_item item;
        item.id = original.id;
        item.kind = static_cast<workspace_item_kind>(kind->currentData().toInt());
        item.enabled = original.enabled;
        if (item.kind == workspace_item_kind::application) {
            item.application_id = app_id();
            if (!item.application_id && !keep_missing)
                return;
            item.display_name = item.application_id ? QString{} : original.display_name;
            item.launch_profile = profile->currentData().toString();
            item.reuse_existing = reuse->isChecked();
            if (capabilities.can_place && placement->isChecked()) {
                const auto frame = maximized->isChecked() ? QRectF(0, 0, 1, 1)
                                                          : QRectF(left->value() / 100, top->value() / 100,
                                                                   width->value() / 100, height->value() / 100);
                item.placement = workspace_placement{monitor->currentData().toString(), frame, maximized->isChecked()};
            } else if (!capabilities.can_place && item.application_id == original.application_id) {
                item.placement = original.placement;
            }
        } else {
            item.target = item.kind == workspace_item_kind::url ? target->text().trimmed() : target->text();
        }
        const auto issue = foradeck::workspace_item_issue(item);
        if (!issue.isEmpty()) {
            error->setText(issue);
            error->show();
            if (item.kind != workspace_item_kind::application)
                target->setFocus();
            return;
        }
        result = std::move(item);
        dialog.accept();
    });
    (original.kind == workspace_item_kind::application ? search : target)->setFocus();
    dialog.exec();
    return result;
}

bool edit_workspace(DesktopService &service, const ui_context &context, QWidget *parent, workspace_record record,
                    const bool from_capture = false) {
    QDialog dialog(parent);
    dialog.setWindowTitle(record.id ? QCoreApplication::translate("Workspaces", "Edit workspace")
                                    : QCoreApplication::translate("Workspaces", "New workspace"));
    prepare_dialog(&dialog, context, {700, 580}, {390, 440});
    auto *layout = new QVBoxLayout(&dialog);
    auto *name = new QLineEdit(record.name);
    name->setMaxLength(128);
    name->setPlaceholderText(QCoreApplication::translate("Workspaces", "e.g. Design, Study, or Work"));
    field(layout, QCoreApplication::translate("Workspaces", "Workspace name"), name);

    layout->addWidget(label(QCoreApplication::translate("Workspaces", "Add to workspace"), "subheading"));
    auto *add_actions = new QGridLayout;
    layout->addLayout(add_actions);
    QVector<QPushButton *> add_buttons;
    for (const auto kind : {workspace_item_kind::application, workspace_item_kind::file, workspace_item_kind::folder,
                            workspace_item_kind::url}) {
        const auto position = static_cast<int>(add_buttons.size());
        auto *add = button(kind_name(kind), nullptr, kind_icon(kind));
        add->setAccessibleName(add_resource_title(kind));
        add_actions->addWidget(add, position / 2, position % 2);
        add_buttons.append(add);
    }
    layout->addSpacing(6);
    auto *resource_hint = label(
        QCoreApplication::translate("Workspaces", "Drag to reorder. Clear a checkbox to skip that resource."), "muted");
    layout->addWidget(resource_hint);
    auto *contents = new QStackedWidget;
    layout->addWidget(contents, 1);
    auto *items = new ResourceOrder;
    setup_table(
        items,
        {QCoreApplication::translate("Workspaces", "Resource"), QCoreApplication::translate("Workspaces", "Type")},
        QCoreApplication::translate("Workspaces", "Resources in launch order"));
    items->setDragDropMode(QAbstractItemView::InternalMove);
    items->setDropIndicatorShown(true);
    contents->addWidget(items);
    auto *empty = empty_card(
        QCoreApplication::translate("Workspaces", "Nothing to open yet"),
        QCoreApplication::translate("Workspaces", "Use the buttons above to add your first resource."), "workspaces");
    contents->addWidget(empty);
    auto *actions = new QWidget;
    auto *tools = new QHBoxLayout(actions);
    tools->setContentsMargins(0, 0, 0, 0);
    auto *edit = button(QCoreApplication::translate("Workspaces", "Edit"), nullptr, "edit");
    auto *remove = button(QCoreApplication::translate("Workspaces", "Remove"), nullptr, "trash");
    auto *up = button({}, nullptr, "chevron_up");
    auto *down = button({}, nullptr, "chevron_down");
    up->setAccessibleName(QCoreApplication::translate("Workspaces", "Move up"));
    down->setAccessibleName(QCoreApplication::translate("Workspaces", "Move down"));
    up->setToolTip(up->accessibleName());
    down->setToolTip(down->accessibleName());
    tools->addWidget(edit);
    tools->addWidget(remove);
    tools->addStretch();
    tools->addWidget(up);
    tools->addWidget(down);
    layout->addWidget(actions);
    auto *error = label({}, "muted");
    error->hide();
    layout->addWidget(error);
    auto *save = button(QCoreApplication::translate("Workspaces", "Save workspace"), "primary", "save");
    auto *buttons = dialog_buttons(save);
    if (from_capture) {
        auto *back = buttons->button(QDialogButtonBox::Cancel);
        back->setText(QCoreApplication::translate("Workspaces", "Back to windows"));
    }
    layout->addWidget(buttons);
    quint64 save_request = 0;
    const auto selected = [&] { return items->indexOfTopLevelItem(items->currentItem()); };
    const auto sync_buttons = [&] {
        const auto index = selected();
        for (auto *add : add_buttons)
            add->setEnabled(!save_request && record.items.size() < foradeck::workspace_item_limit);
        save->setEnabled(!save_request && !record.items.isEmpty() && !name->text().trimmed().isEmpty());
        edit->setEnabled(!save_request && index >= 0);
        remove->setEnabled(edit->isEnabled());
        up->setEnabled(edit->isEnabled() && index > 0);
        down->setEnabled(edit->isEnabled() && index + 1 < record.items.size());
    };
    const auto refresh_icons = [&] {
        const QSignalBlocker blocked(items);
        for (int index = 0; index < items->topLevelItemCount(); ++index)
            items->topLevelItem(index)->setIcon(0, resource_icon(record.items[index], service.model));
    };
    const auto fill = [&](const int index) {
        const QSignalBlocker blocked(items);
        items->clear();
        for (const auto &item : record.items) {
            auto text = workspace_item_label(item, service.catalog().applications);
            if (item.kind == workspace_item_kind::application && !item.application_id)
                text = QCoreApplication::translate("Workspaces", "Missing application · %1").arg(text);
            auto *entry = row(items, {text, kind_name(item.kind)});
            entry->setCheckState(0, item.enabled ? Qt::Checked : Qt::Unchecked);
            entry->setFlags(entry->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        }
        refresh_icons();
        items->setCurrentItem(items->topLevelItem(index));
        contents->setCurrentWidget(record.items.isEmpty() ? static_cast<QWidget *>(empty) : items);
        actions->setVisible(!record.items.isEmpty());
        resource_hint->setVisible(!record.items.isEmpty());
        error->hide();
        sync_buttons();
    };
    QObject::connect(&service.model, &LibraryModel::icon_ready, &dialog, refresh_icons);
    QObject::connect(name, &QLineEdit::textChanged, &dialog, [&] {
        error->hide();
        sync_buttons();
    });
    QObject::connect(items, &QTreeWidget::itemSelectionChanged, &dialog, sync_buttons);
    QObject::connect(items, &QTreeWidget::itemChanged, &dialog, [&](QTreeWidgetItem *item, int) {
        record.items[items->indexOfTopLevelItem(item)].enabled = item->checkState(0) == Qt::Checked;
    });
    for (int index = 0; index < add_buttons.size(); ++index)
        QObject::connect(add_buttons[index], &QPushButton::clicked, &dialog, [&, index] {
            workspace_item initial;
            initial.kind = static_cast<workspace_item_kind>(index);
            if (auto item = edit_item(service, context, &dialog, initial, false)) {
                record.items.append(std::move(*item));
                fill(static_cast<int>(record.items.size() - 1));
            }
        });
    const auto edit_selected = [&] {
        const auto index = selected();
        if (index >= 0 && !save_request)
            if (auto item = edit_item(service, context, &dialog, record.items[index], true)) {
                record.items[index] = std::move(*item);
                fill(index);
            }
    };
    QObject::connect(edit, &QPushButton::clicked, &dialog, edit_selected);
    QObject::connect(items, &QTreeWidget::itemActivated, &dialog, edit_selected);
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
        const auto index = selected();
        record.items.removeAt(index);
        fill(qMin(index, static_cast<int>(record.items.size()) - 1));
    });
    items->reorder = [&](const int from, const int to) {
        if (!save_request) {
            record.items.move(from, to);
            fill(to);
        }
    };
    const auto move = [&](const int offset) {
        const auto index = selected();
        record.items.move(index, index + offset);
        fill(index + offset);
    };
    QObject::connect(up, &QPushButton::clicked, &dialog, [&] { move(-1); });
    QObject::connect(down, &QPushButton::clicked, &dialog, [&] { move(1); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        record.name = name->text().trimmed();
        const auto issue = foradeck::workspace_record_issue(record);
        error->setText(issue);
        error->setVisible(!issue.isEmpty());
        if (!issue.isEmpty()) {
            (record.name.isEmpty() ? static_cast<QWidget *>(name) : items)->setFocus();
            return;
        }
        save_request = service.save_workspace(record);
        if (save_request) {
            buttons->setEnabled(false);
            name->setEnabled(false);
            items->setEnabled(false);
            sync_buttons();
        }
    });
    QObject::connect(&service, &DesktopService::workspace_saved, &dialog, [&](const quint64 request, qint64) {
        if (request == save_request)
            dialog.accept();
    });
    QObject::connect(&service, &DesktopService::operation_changed, &dialog, [&](const operation_result &result) {
        if (save_request && save_request == result.id && result.state == operation_state::error) {
            error->setText(result.error.user_message);
            error->show();
            save_request = 0;
            buttons->setEnabled(true);
            name->setEnabled(true);
            items->setEnabled(true);
            sync_buttons();
        }
    });
    fill(0);
    name->setFocus();
    return dialog.exec() == QDialog::Accepted;
}

void capture_preview(DesktopService &service, const ui_context &context, QWidget *parent,
                     const QVector<workspace_capture_candidate> &candidates) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QCoreApplication::translate("Workspaces", "Capture open windows"));
    prepare_dialog(&dialog, context, {780, 540}, {400, 320});
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(label(QCoreApplication::translate("Workspaces", "Choose windows to save"), "heading"));
    layout->addWidget(
        label(QCoreApplication::translate(
                  "Workspaces", "Check one window per application. Where prompted, choose a library launch option."),
              "muted"));
    auto *list = table({QCoreApplication::translate("Workspaces", "Detected window"),
                        QCoreApplication::translate("Workspaces", "Library application or status")},
                       QCoreApplication::translate("Workspaces", "Detected windows"));
    list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    list->setUniformRowHeights(false);
    layout->addWidget(list, 1);
    list->setVisible(!candidates.isEmpty());
    if (candidates.isEmpty())
        layout->addWidget(
            empty_card(QCoreApplication::translate("Workspaces", "No eligible windows were found."),
                       QCoreApplication::translate("Workspaces", "You can still build a workspace manually."),
                       "workspaces"),
            1);

    auto *manual_hint =
        label(QCoreApplication::translate("Workspaces",
                                          "Choosing a different library app adds it without the window position."),
              "muted");
    manual_hint->hide();
    layout->addWidget(manual_hint);
    auto *status = label({}, "muted");
    layout->addWidget(status);
    auto *next = button({}, "primary", "chevron_right");
    auto *buttons = dialog_buttons(next);
    layout->addWidget(buttons);

    QVector<std::optional<workspace_item>> assignments(candidates.size());
    QVector<QComboBox *> choices(candidates.size(), nullptr);
    QVector<workspace_item> selected;
    const auto refresh_selection = [&] {
        selected.clear();
        QSet<qint64> used;
        bool duplicate = false;
        for (int index = 0; index < list->topLevelItemCount(); ++index) {
            const auto &item = assignments[index];
            if (!item || list->topLevelItem(index)->checkState(0) != Qt::Checked)
                continue;
            duplicate = duplicate || used.contains(item->application_id);
            used.insert(item->application_id);
            selected.append(*item);
        }
        const bool over_limit = selected.size() > foradeck::workspace_item_limit;
        next->setEnabled(!duplicate && !over_limit);
        next->setText(selected.isEmpty() ? QCoreApplication::translate("Workspaces", "Create manually")
                                         : QCoreApplication::translate("Workspaces", "Review workspace"));
        if (duplicate)
            status->setText(QCoreApplication::translate("Workspaces", "Select only one window for each application."));
        else if (over_limit)
            status->setText(QCoreApplication::translate("Workspaces", "Select up to %1 windows.")
                                .arg(foradeck::workspace_item_limit));
        else
            status->setText(
                QCoreApplication::translate("Workspaces", "Applications selected: %1").arg(selected.size()));
        status->setVisible(!candidates.isEmpty());
    };
    const auto refresh_icons = [&] {
        const QSignalBlocker blocked(list);
        for (int index = 0; index < list->topLevelItemCount(); ++index) {
            auto *entry = list->topLevelItem(index);
            const auto &candidate = candidates[index];
            if (auto *choice = choices[index])
                for (int option = 0; option < candidate.launch_options.size(); ++option)
                    choice->setItemIcon(option + 1, resource_icon(candidate.launch_options[option], service.model));
            if (const auto &item = assignments[index]) {
                entry->setIcon(0, resource_icon(*item, service.model));
            } else {
                app_record observed;
                observed.path = candidate.window.executable;
                observed.target_kind = QStringLiteral("executable");
                entry->setIcon(0, service.model.application_icon(observed));
            }
        }
    };
    const auto set_assignment = [&](const int index, std::optional<workspace_item> item) {
        {
            const QSignalBlocker blocked(list);
            assignments[index] = std::move(item);
            list->topLevelItem(index)->setData(0, Qt::CheckStateRole,
                                               assignments[index] ? QVariant(Qt::Checked) : QVariant{});
        }
        refresh_selection();
        refresh_icons();
    };

    QSet<qint64> preselected;
    for (int index = 0; index < candidates.size(); ++index) {
        const auto &candidate = candidates[index];
        const auto title = candidate.window.title.isEmpty()
                               ? QCoreApplication::translate("Workspaces", "Untitled window")
                               : candidate.window.title;
        auto app = candidate.item ? candidate.item->display_name : candidate.issue;
        if (candidate.item && candidate.item->placement)
            app += " · " + QCoreApplication::translate("Workspaces", "Window layout");
        auto *entry = row(list, {title, app});
        if (!candidate.window.executable.isEmpty())
            entry->setToolTip(0, (title + '\n' + candidate.window.executable).toHtmlEscaped());
        if (candidate.item) {
            assignments[index] = candidate.item;
            const auto id = candidate.item->application_id;
            const bool already_selected = preselected.contains(id);
            const bool checked = !already_selected && preselected.size() < foradeck::workspace_item_limit;
            if (checked)
                preselected.insert(id);
            entry->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
            if (already_selected)
                entry->setToolTip(1, QCoreApplication::translate("Workspaces", "Several windows for this application"));
        } else if (!candidate.launch_options.isEmpty()) {
            auto *choice = new QComboBox;
            choices[index] = choice;
            choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            choice->setMinimumContentsLength(10);
            const auto prompt = QCoreApplication::translate("Workspaces", "Choose launch option…");
            choice->addItem(prompt);
            choice->setAccessibleName(prompt + ' ' + title);
            for (const auto &option : candidate.launch_options) {
                auto name = option.display_name;
                if (const auto *record = service.model.find(option.application_id)) {
                    QStringList details;
                    for (const auto &value :
                         {record->arguments, record->default_launch_profile, record->working_directory})
                        if (!value.isEmpty())
                            details.append(value);
                    if (!details.isEmpty())
                        name += " · " + details.join(" · ");
                    choice->addItem(name);
                    choice->setItemData(choice->count() - 1, (name + '\n' + record->path).toHtmlEscaped(),
                                        Qt::ToolTipRole);
                } else {
                    choice->addItem(name);
                }
            }
            entry->setSizeHint(0, {0, choice->sizeHint().height() + 4});
            list->setItemWidget(entry, 1, choice);
            QObject::connect(choice, &QComboBox::currentIndexChanged, &dialog, [&, index](const int option) {
                set_assignment(index,
                               option > 0 ? std::optional(candidates[index].launch_options[option - 1]) : std::nullopt);
            });
        } else {
            manual_hint->show();
            entry->setText(1, {});
            auto *manual = new QWidget;
            auto *manual_layout = new QVBoxLayout(manual);
            manual_layout->setContentsMargins(4, 3, 4, 3);
            auto *reason = label(candidate.issue, "muted");
            manual_layout->addWidget(reason);
            const auto choose_text = QCoreApplication::translate("Workspaces", "Choose application…");
            auto *add_file =
                button(QCoreApplication::translate("Workspaces", "Add file to library…"), nullptr, "add_app");
            add_file->setAutoDefault(false);
            add_file->setAccessibleName(add_file->text() + ' ' + title);
            add_file->setToolTip(candidate.window.executable.toHtmlEscaped());
            manual_layout->addWidget(add_file, 0, Qt::AlignLeft);
            auto *choose = button(choose_text, "quiet", "library");
            choose->setAutoDefault(false);
            choose->setAccessibleName(choose_text + ' ' + title);
            manual_layout->addWidget(choose, 0, Qt::AlignLeft);
            entry->setSizeHint(0, {0, manual->sizeHint().height()});
            list->setItemWidget(entry, 1, manual);
            const auto select_app = [&, index, entry, reason](workspace_item item) {
                auto name = workspace_item_label(item, service.catalog().applications);
                if (item.placement)
                    name += " · " + QCoreApplication::translate("Workspaces", "Window layout");
                reason->setText(name);
                entry->setToolTip(1, name.toHtmlEscaped());
                set_assignment(index, std::move(item));
            };
            QObject::connect(choose, &QPushButton::clicked, &dialog, [&, index, select_app] {
                auto initial = assignments[index].value_or(workspace_item{});
                initial.placement.reset();
                if (auto item = edit_item(service, context, &dialog, initial, false))
                    select_app(std::move(*item));
            });
            QObject::connect(add_file, &QPushButton::clicked, &dialog, [&, index, select_app, add_file] {
                const auto &observed = candidates[index];
                const auto id = add_application_from_file(service, observed.window.executable, &dialog);
                const auto *app = service.model.find(id);
                if (!app)
                    return;
                workspace_item item;
                item.application_id = id;
                if (!observed.window.executable.isEmpty() && Application::target_kind(*app) == "executable" &&
                    normalized_path(app->path) == normalized_path(observed.window.executable)) {
                    item.placement = observed.window_placement;
                    add_file->setEnabled(false);
                }
                select_app(std::move(item));
            });
        }
    }
    QObject::connect(&service.model, &LibraryModel::icon_ready, &dialog, refresh_icons);
    QObject::connect(list, &QTreeWidget::itemChanged, &dialog, refresh_selection);
    refresh_icons();
    refresh_selection();
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (edit_workspace(service, context, &dialog, {0, {}, selected}, true))
            dialog.accept();
    });
    dialog.exec();
}

bool confirm_run(const ui_context &context, QWidget *parent, const QStringList &warnings) {
    if (warnings.isEmpty())
        return true;
    QDialog dialog(parent);
    dialog.setWindowTitle(QCoreApplication::translate("Workspaces", "Workspace preflight"));
    prepare_dialog(&dialog, context, {650, 400}, {340, 240});
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(
        label(QCoreApplication::translate("Workspaces", "Review these warnings before opening resources.")));
    auto *list = table({QCoreApplication::translate("Workspaces", "Warnings")},
                       QCoreApplication::translate("Workspaces", "Preflight warnings"));
    for (const auto &warning : warnings)
        row(list, {warning});
    layout->addWidget(list, 1);
    auto *buttons =
        dialog_buttons(button(QCoreApplication::translate("Workspaces", "Run workspace"), "primary", "play"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted;
}
}

QWidget *workspaces_view(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *runs = &service->workspace_runs();
    auto *page = new QWidget(parent);
    page->setObjectName("workspaces_page");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    layout->addWidget(label(QCoreApplication::translate("Workspaces", "Workspaces"), "heading"));
    layout->addWidget(label(
        QCoreApplication::translate("Workspaces", "Open a group of applications, files, folders and links together."),
        "muted"));
    auto *header = new QHBoxLayout;
    auto *create = button(QCoreApplication::translate("Workspaces", "New workspace"), "primary", "add");
    auto *capture = button(QCoreApplication::translate("Workspaces", "Capture open windows"), nullptr, "workspaces");
    header->addWidget(create);
    header->addWidget(capture);
    header->addStretch();
    layout->addLayout(header);
    auto *limitation = label({}, "muted");
    layout->addWidget(limitation);
    auto *empty = empty_card(
        QCoreApplication::translate("Workspaces", "No workspaces yet"),
        QCoreApplication::translate("Workspaces", "Start with an empty workspace or capture the windows already open."),
        "workspaces");
    layout->addWidget(empty, 1);
    auto *list = table(
        {QCoreApplication::translate("Workspaces", "Name"), QCoreApplication::translate("Workspaces", "Resources")},
        QCoreApplication::translate("Workspaces", "Saved workspaces"));
    layout->addWidget(list, 2);
    auto *workspace_title = label({}, "subheading");
    layout->addWidget(workspace_title);
    auto *action_panel = new QWidget;
    auto *actions = new QHBoxLayout(action_panel);
    actions->setContentsMargins(0, 0, 0, 0);
    auto *run = button(QCoreApplication::translate("Workspaces", "Run workspace"), "primary", "play");
    auto *edit = button(QCoreApplication::translate("Workspaces", "Edit"), nullptr, "edit");
    auto *duplicate = button(QCoreApplication::translate("Workspaces", "Duplicate"), nullptr, "copy");
    auto *remove = button(QCoreApplication::translate("Workspaces", "Delete"), "quiet", "trash");
    actions->addWidget(run);
    actions->addWidget(edit);
    actions->addWidget(duplicate);
    actions->addWidget(remove);
    actions->addStretch();
    layout->addWidget(action_panel);

    auto *tabs = new QTabWidget;
    tabs->setObjectName("workspaces_tabs");
    tabs->tabBar()->setObjectName("workspace_report_tabs");
    layout->addWidget(tabs, 5);
    auto *resources = table(
        {QCoreApplication::translate("Workspaces", "Resource"), QCoreApplication::translate("Workspaces", "Status")},
        QCoreApplication::translate("Workspaces", "Selected workspace resources"));
    tabs->addTab(resources, QCoreApplication::translate("Workspaces", "Resources"));
    auto *last_run = new QWidget;
    auto *run_layout = new QVBoxLayout(last_run);
    const auto run_tab = tabs->addTab(last_run, QCoreApplication::translate("Workspaces", "Last run"));
    auto *progress_title = label({}, "heading");
    run_layout->addWidget(progress_title);
    auto *progress = new QProgressBar;
    progress->setAccessibleName(QCoreApplication::translate("Workspaces", "Workspace run progress"));
    run_layout->addWidget(progress);
    auto *report = table({QCoreApplication::translate("Workspaces", "Resource"),
                          QCoreApplication::translate("Workspaces", "Status"),
                          QCoreApplication::translate("Workspaces", "Details")},
                         QCoreApplication::translate("Workspaces", "Workspace run report"));
    report->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    run_layout->addWidget(report, 3);
    auto *cancel = button(QCoreApplication::translate("Workspaces", "Cancel run"), nullptr, "stop");
    auto *export_report =
        button(QCoreApplication::translate("Workspaces", "Export run diagnostics…"), nullptr, "export");
    auto *run_actions = new QHBoxLayout;
    run_actions->addWidget(cancel);
    run_actions->addWidget(export_report);
    run_actions->addStretch();
    run_layout->addLayout(run_actions);

    const auto selection = [=]() -> qint64 {
        return list->currentItem() ? list->currentItem()->data(0, Qt::UserRole).toLongLong() : 0;
    };
    const auto selected = [=]() -> const workspace_record * {
        const auto &workspaces = service->catalog().workspaces;
        const auto found = std::ranges::find(workspaces, selection(), &workspace_record::id);
        return found == workspaces.cend() ? nullptr : &*found;
    };
    const auto sync_controls = [=] {
        const auto locked = !service->is_ready() || service->profile_busy() || service->restart_required();
        const auto busy = runs->active() || runs->capturing() || service->is_scenario_active();
        create->setEnabled(!locked && !busy);
        capture->setEnabled(create->isEnabled() && runs->desktop().can_capture);
        run->setEnabled(create->isEnabled() && selected());
        edit->setEnabled(run->isEnabled());
        remove->setEnabled(run->isEnabled());
        duplicate->setEnabled(run->isEnabled());
        cancel->setVisible(runs->active());
        export_report->setVisible(runs->outcome() != workspace_outcome::none);
        export_report->setEnabled(!locked);
        const auto text = runs->capturing() ? QCoreApplication::translate("Workspaces", "Checking open windows…")
                                            : runs->desktop().limitation;
        limitation->setText(text);
        limitation->setVisible(!text.isEmpty());
    };
    const auto show_selected = [=] {
        resources->clear();
        const auto *workspace = selected();
        const auto &apps = service->catalog().applications;
        workspace_title->setText(workspace ? workspace->name : QString{});
        workspace_title->setVisible(workspace != nullptr);
        if (workspace)
            for (const auto &item : workspace->items) {
                auto status =
                    !item.enabled ? QCoreApplication::translate("Workspaces", "Disabled") : kind_name(item.kind);
                if (item.kind == workspace_item_kind::application &&
                    std::ranges::find(apps, item.application_id, &app_record::id) == apps.cend())
                    status = QCoreApplication::translate("Workspaces", "Missing application");
                else if (!foradeck::workspace_item_issue(item).isEmpty())
                    status = QCoreApplication::translate("Workspaces", "Invalid resource");
                else if (item.enabled && item.placement)
                    status = QCoreApplication::translate("Workspaces", "Window layout");
                row(resources, {workspace_item_label(item, apps), status})
                    ->setIcon(0, resource_icon(item, service->model));
            }
        tabs->setTabEnabled(0, workspace != nullptr);
        if (workspace && runs->report().isEmpty())
            tabs->setCurrentIndex(0);
        sync_controls();
    };
    QObject::connect(&service->model, &LibraryModel::icon_ready, page, [=] {
        if (const auto *workspace = selected())
            for (int index = 0; index < resources->topLevelItemCount(); ++index)
                resources->topLevelItem(index)->setIcon(0, resource_icon(workspace->items[index], service->model));
    });
    const auto sync_panels = [=] {
        const bool has_workspaces = list->topLevelItemCount() > 0;
        const bool has_report = !runs->report().isEmpty();
        empty->setVisible(!has_workspaces && !has_report);
        list->setVisible(has_workspaces);
        action_panel->setVisible(has_workspaces);
        tabs->setVisible(has_workspaces || has_report);
    };
    const auto reload = [=](const qint64 requested) {
        const QSignalBlocker blocked(list);
        const auto id = requested ? requested : selection();
        list->clear();
        for (const auto &workspace : service->catalog().workspaces) {
            auto *item = row(list, {workspace.name, QString::number(workspace.items.size())});
            item->setData(0, Qt::UserRole, workspace.id);
            item->setIcon(0, appearance::icon("workspaces"));
            if (workspace.id == id)
                list->setCurrentItem(item);
        }
        if (!list->currentItem())
            list->setCurrentItem(list->topLevelItem(0));
        sync_panels();
        show_selected();
    };
    const auto render_run = [=] {
        const auto &steps = runs->report();
        report->setVisible(!steps.isEmpty());
        progress_title->setVisible(!steps.isEmpty());
        progress->setVisible(!steps.isEmpty());
        tabs->setTabEnabled(run_tab, !steps.isEmpty());
        sync_panels();
        if (!steps.isEmpty()) {
            if (last_run->property("run_id").toULongLong() != runs->run_id()) {
                last_run->setProperty("run_id", runs->run_id());
                tabs->setCurrentIndex(run_tab);
            }
            const auto outcome = workspace_outcome_text(runs->outcome());
            progress_title->setText(outcome.isEmpty() ? runs->name() : runs->name() + " · " + outcome);
            if (report->topLevelItemCount() != steps.size()) {
                report->clear();
                for (qsizetype index = 0; index < steps.size(); ++index)
                    row(report, {});
            }
            int finished = 0;
            for (int index = 0; index < steps.size(); ++index) {
                const auto &step = steps[index];
                auto *item = report->topLevelItem(index);
                auto status = workspace_step_text(step.state);
                if (step.reused && step.state == workspace_step_state::positioned)
                    status = workspace_step_text(workspace_step_state::reused) + " · " + status;
                const QStringList texts{step.label, status, step.detail};
                for (int column = 0; column < texts.size(); ++column) {
                    item->setText(column, texts[column]);
                    item->setToolTip(column, texts[column].toHtmlEscaped());
                }
                if (step.state == workspace_step_state::disabled || step.state >= workspace_step_state::opened)
                    ++finished;
            }
            progress->setRange(0, static_cast<int>(steps.size()));
            progress->setValue(finished);
        }
        sync_controls();
    };

    QObject::connect(service, &DesktopService::catalog_changed, page, [=] { reload(0); });
    QObject::connect(service, &DesktopService::workspace_saved, page, [=](quint64, const qint64 id) { reload(id); });
    QObject::connect(service, &DesktopService::profile_state_changed, page, sync_controls);
    QObject::connect(service, &DesktopService::scenario_changed, page, sync_controls);
    QObject::connect(list, &QTreeWidget::itemSelectionChanged, page, show_selected);
    QObject::connect(runs, &WorkspaceCoordinator::changed, page, render_run);
    QObject::connect(runs, &WorkspaceCoordinator::capabilities_changed, page, sync_controls);
    QObject::connect(create, &QPushButton::clicked, page, [=] { edit_workspace(*service, context, page, {}); });
    QObject::connect(edit, &QPushButton::clicked, page, [=] {
        if (const auto *workspace = selected())
            edit_workspace(*service, context, page, *workspace);
    });
    QObject::connect(duplicate, &QPushButton::clicked, page, [=] {
        if (const auto *workspace = selected()) {
            auto copy = *workspace;
            copy.id = 0;
            copy.name = QCoreApplication::translate("Workspaces", "Copy of %1").arg(copy.name.left(112));
            for (auto &item : copy.items)
                item.id = 0;
            edit_workspace(*service, context, page, std::move(copy));
        }
    });
    QObject::connect(remove, &QPushButton::clicked, page, [=] {
        const auto *workspace = selected();
        if (!workspace)
            return;
        const auto id = workspace->id;
        if (ui_helpers::confirm(page, QCoreApplication::translate("Workspaces", "Delete workspace"),
                                QCoreApplication::translate("Workspaces", "Delete “%1”?").arg(workspace->name),
                                QCoreApplication::translate("Workspaces", "Delete")))
            service->remove_workspace(id);
    });
    QObject::connect(run, &QPushButton::clicked, page, [=] { service->prepare_workspace(selection()); });
    QObject::connect(list, &QTreeWidget::itemActivated, page, [=] {
        if (run->isEnabled())
            run->click();
    });
    QObject::connect(runs, &WorkspaceCoordinator::preflight_ready, page,
                     [=](const quint64 id, const QStringList &warnings) {
                         if (confirm_run(context, page, warnings)) {
                             if (runs->start(id))
                                 cancel->setFocus();
                         } else if (runs->run_id() == id) {
                             runs->cancel();
                         }
                     });
    QObject::connect(cancel, &QPushButton::clicked, runs, &WorkspaceCoordinator::cancel);
    QObject::connect(capture, &QPushButton::clicked, page, [=] {
        service->capture_workspace();
        sync_controls();
    });
    QObject::connect(runs, &WorkspaceCoordinator::capture_ready, page, [=](quint64, const auto &candidates) {
        if (runs->desktop().can_capture)
            capture_preview(*service, context, page, candidates);
    });
    QObject::connect(export_report, &QPushButton::clicked, page, [=] {
        const auto path = QFileDialog::getSaveFileName(
            page, QCoreApplication::translate("Workspaces", "Export run diagnostics"), "workspace-run.json",
            QCoreApplication::translate("Workspaces", "JSON files (*.json)"));
        if (!path.isEmpty())
            service->export_workspace_diagnostics(path);
    });
    QObject::connect(service, &DesktopService::navigation_changed, page, [=] {
        if (service->section() == "workspaces")
            runs->check_capabilities();
    });
    reload(0);
    render_run();
    return page;
}
