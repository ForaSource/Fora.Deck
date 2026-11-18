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
#include <QLineEdit>
#include <QMimeData>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTreeWidget>

#include "DesktopService.h"
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
        return QCoreApplication::translate("Workspaces", "URL");
    }
    return {};
}

void setup_table(QTreeWidget *list, const QStringList &headers, const QString &name) {
    list->setHeaderLabels(headers);
    list->setAccessibleName(name);
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setAlternatingRowColors(true);
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

QDialogButtonBox *dialog_buttons(const QString &accept) {
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *primary = buttons->addButton(accept, QDialogButtonBox::AcceptRole);
    primary->setProperty("primary", true);
    return buttons;
}

std::optional<workspace_item> edit_item(DesktopService &service, const ui_context &context, QWidget *parent,
                                        const workspace_item &original, const bool editing) {
    QDialog dialog(parent);
    dialog.setWindowTitle(editing ? QCoreApplication::translate("Workspaces", "Edit resource")
                                  : QCoreApplication::translate("Workspaces", "Add resource"));
    prepare_dialog(&dialog, context, {600, 560}, {340, 280});
    auto *layout = new QVBoxLayout(&dialog);
    auto *contents = new QWidget;
    auto *form = new QVBoxLayout(contents);
    auto *kind = new QComboBox;
    for (const auto option : {workspace_item_kind::application, workspace_item_kind::file, workspace_item_kind::folder,
                              workspace_item_kind::url})
        kind->addItem(kind_name(option), static_cast<int>(option));
    kind->setCurrentIndex(static_cast<int>(original.kind));
    field(form, QCoreApplication::translate("Workspaces", "Resource type"), kind);

    auto *options = new QStackedWidget;
    form->addWidget(options);
    auto *application_options = new QWidget;
    auto *applications_form = new QVBoxLayout(application_options);
    applications_form->setContentsMargins(0, 0, 0, 0);
    auto *app = new QComboBox;
    app->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    app->setMinimumContentsLength(14);
    if (editing && original.kind == workspace_item_kind::application && !original.application_id)
        app->addItem(QCoreApplication::translate("Workspaces", "Missing application · %1").arg(original.display_name),
                     0);
    const auto catalog = service.catalog().applications;
    for (const auto &entry : catalog) {
        app->addItem(entry.name, entry.id);
        app->setItemData(app->count() - 1, entry.path, Qt::ToolTipRole);
    }
    const auto current_app = app->findData(original.application_id);
    if (current_app >= 0)
        app->setCurrentIndex(current_app);
    field(applications_form, QCoreApplication::translate("Workspaces", "Library application"), app);

    auto *profile = new QComboBox;
    field(applications_form, QCoreApplication::translate("Workspaces", "Browser launch profile"), profile);
    auto *reuse =
        new QCheckBox(QCoreApplication::translate("Workspaces", "Reuse existing window when identity is verified"));
    reuse->setChecked(original.reuse_existing);
    applications_form->addWidget(reuse);

    const auto load_profiles = [&] {
        const auto id = app->currentData().toLongLong();
        const auto found = std::ranges::find(catalog, id, &app_record::id);
        const auto previous = profile->currentData().toString();
        const auto selected = profile->count() ? previous : original.launch_profile;
        profile->clear();
        profile->addItem(QCoreApplication::translate("Workspaces", "Use application default"), QString{});
        if (found != catalog.cend() && !Application::launch_modes(*found).isEmpty()) {
            profile->addItem(QCoreApplication::translate("Workspaces", "Use configured launch"), "configured");
            for (const auto &option : service.launch_profiles(*found))
                profile->addItem(option.name, option.id);
        }
        if (id == original.application_id && !selected.isEmpty() && profile->findData(selected) < 0)
            profile->addItem(QCoreApplication::translate("Workspaces", "Unavailable profile · %1").arg(selected),
                             selected);
        profile->setCurrentIndex(qMax(0, profile->findData(selected)));
        profile->setEnabled(profile->count() > 1);
    };
    QObject::connect(app, &QComboBox::currentIndexChanged, &dialog, load_profiles);
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
    applications_form->addWidget(placement);
    applications_form->addStretch();
    options->addWidget(application_options);

    auto *target_options = new QWidget;
    auto *target_form = new QVBoxLayout(target_options);
    target_form->setContentsMargins(0, 0, 0, 0);
    auto *target = new QLineEdit(original.target);
    target->setMaxLength(4096);
    field(target_form, QCoreApplication::translate("Workspaces", "Path or URL"), target);
    auto *browse = button(QCoreApplication::translate("Workspaces", "Browse…"), "quiet");
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
        options->setCurrentIndex(kind->currentIndex() == 0 ? 0 : 1);
        browse->setVisible(kind->currentIndex() != static_cast<int>(workspace_item_kind::url));
    };
    QObject::connect(kind, &QComboBox::currentIndexChanged, &dialog, show_options);
    show_options();
    layout->addWidget(ui_helpers::scroll(contents), 1);
    auto *error = label({}, "muted");
    layout->addWidget(error);
    auto *buttons = dialog_buttons(QCoreApplication::translate("Workspaces", "Apply"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    std::optional<workspace_item> result;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        workspace_item item;
        item.id = original.id;
        item.kind = static_cast<workspace_item_kind>(kind->currentData().toInt());
        item.enabled = original.enabled;
        if (item.kind == workspace_item_kind::application) {
            item.application_id = app->currentData().toLongLong();
            if (!item.application_id &&
                !(editing && original.kind == workspace_item_kind::application && !original.application_id)) {
                error->setText(QCoreApplication::translate("Workspaces", "Choose an application from the library."));
                app->setFocus();
                return;
            }
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
        const auto issue = xlibrary::workspace_item_issue(item);
        if (!issue.isEmpty()) {
            error->setText(issue);
            if (item.kind != workspace_item_kind::application)
                target->setFocus();
            return;
        }
        result = std::move(item);
        dialog.accept();
    });
    dialog.exec();
    return result;
}

void edit_workspace(DesktopService &service, const ui_context &context, QWidget *parent, workspace_record record) {
    QDialog dialog(parent);
    dialog.setWindowTitle(record.id ? QCoreApplication::translate("Workspaces", "Edit workspace")
                                    : QCoreApplication::translate("Workspaces", "New workspace"));
    prepare_dialog(&dialog, context, {680, 510}, {350, 320});
    auto *layout = new QVBoxLayout(&dialog);
    auto *name = new QLineEdit(record.name);
    name->setMaxLength(128);
    field(layout, QCoreApplication::translate("Workspaces", "Workspace name"), name);
    auto *items = new ResourceOrder;
    setup_table(
        items,
        {QCoreApplication::translate("Workspaces", "Resource"), QCoreApplication::translate("Workspaces", "Type")},
        QCoreApplication::translate("Workspaces", "Resources in launch order"));
    items->setDragDropMode(QAbstractItemView::InternalMove);
    items->setDropIndicatorShown(true);
    layout->addWidget(items, 1);
    auto *tools = new QGridLayout;
    layout->addLayout(tools);
    auto *add = button(QCoreApplication::translate("Workspaces", "Add resource"), "quiet", "add");
    auto *edit = button(QCoreApplication::translate("Workspaces", "Edit resource"), "quiet");
    auto *remove = button(QCoreApplication::translate("Workspaces", "Remove resource"), "quiet");
    auto *up = button(QCoreApplication::translate("Workspaces", "Move up"), "quiet");
    auto *down = button(QCoreApplication::translate("Workspaces", "Move down"), "quiet");
    tools->addWidget(add, 0, 0);
    tools->addWidget(edit, 0, 1);
    tools->addWidget(remove, 0, 2);
    tools->addWidget(up, 1, 0);
    tools->addWidget(down, 1, 1);
    auto *error = label({}, "muted");
    layout->addWidget(error);
    auto *buttons = dialog_buttons(QCoreApplication::translate("Workspaces", "Save workspace"));
    layout->addWidget(buttons);
    quint64 save_request = 0;
    const auto selected = [&] { return items->indexOfTopLevelItem(items->currentItem()); };
    const auto sync_buttons = [&] {
        const auto index = selected();
        add->setEnabled(!save_request && record.items.size() < xlibrary::workspace_item_limit);
        edit->setEnabled(!save_request && index >= 0);
        remove->setEnabled(edit->isEnabled());
        up->setEnabled(edit->isEnabled() && index > 0);
        down->setEnabled(edit->isEnabled() && index + 1 < record.items.size());
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
        items->setCurrentItem(items->topLevelItem(index));
        sync_buttons();
    };
    QObject::connect(items, &QTreeWidget::itemSelectionChanged, &dialog, sync_buttons);
    QObject::connect(items, &QTreeWidget::itemChanged, &dialog, [&](QTreeWidgetItem *item, int) {
        record.items[items->indexOfTopLevelItem(item)].enabled = item->checkState(0) == Qt::Checked;
    });
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
        if (auto item = edit_item(service, context, &dialog, {}, false)) {
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
        const auto issue = xlibrary::workspace_record_issue(record);
        error->setText(issue);
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
            save_request = 0;
            buttons->setEnabled(true);
            name->setEnabled(true);
            items->setEnabled(true);
            sync_buttons();
        }
    });
    fill(0);
    name->setFocus();
    dialog.exec();
}

void capture_preview(DesktopService &service, const ui_context &context, QWidget *parent,
                     const QVector<workspace_capture_candidate> &candidates) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QCoreApplication::translate("Workspaces", "Save current desktop"));
    prepare_dialog(&dialog, context, {710, 440}, {340, 280});
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(label(QCoreApplication::translate(
        "Workspaces", "Select one window per application. You can add other resources before saving.")));
    auto *list = table({QCoreApplication::translate("Workspaces", "Detected window"),
                        QCoreApplication::translate("Workspaces", "Application and monitor")},
                       QCoreApplication::translate("Workspaces", "Detected windows"));
    list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(list, 1);
    QHash<qint64, int> window_counts;
    for (const auto &candidate : candidates)
        if (candidate.item)
            ++window_counts[candidate.item->application_id];
    for (const auto &candidate : candidates) {
        const auto title = candidate.window.title.isEmpty()
                               ? QCoreApplication::translate("Workspaces", "Untitled window")
                               : candidate.window.title;
        auto app = candidate.item ? candidate.item->display_name : candidate.issue;
        if (!candidate.monitor.isEmpty())
            app += " · " + candidate.monitor;
        const bool ambiguous = candidate.item && window_counts.value(candidate.item->application_id) > 1;
        if (ambiguous)
            app += " · " + QCoreApplication::translate("Workspaces", "Several windows for this application");
        auto *entry = row(list, {title, app});
        if (candidate.item)
            entry->setCheckState(0, ambiguous ? Qt::Unchecked : Qt::Checked);
    }
    auto *error = label(
        candidates.isEmpty() ? QCoreApplication::translate("Workspaces", "No eligible windows were found.") : QString{},
        "muted");
    layout->addWidget(error);
    auto *buttons = dialog_buttons(QCoreApplication::translate("Workspaces", "Continue to editor"));
    layout->addWidget(buttons);
    workspace_record record;
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        record.items.clear();
        QSet<qint64> selected;
        for (int index = 0; index < list->topLevelItemCount(); ++index) {
            const auto &candidate = candidates[index];
            if (!candidate.item || list->topLevelItem(index)->checkState(0) != Qt::Checked)
                continue;
            if (selected.contains(candidate.item->application_id)) {
                error->setText(
                    QCoreApplication::translate("Workspaces", "Select only one window for each application."));
                list->setCurrentItem(list->topLevelItem(index));
                return;
            }
            selected.insert(candidate.item->application_id);
            record.items.append(*candidate.item);
        }
        if (record.items.size() > xlibrary::workspace_item_limit) {
            error->setText(QCoreApplication::translate("Workspaces", "Select up to %1 windows.")
                               .arg(xlibrary::workspace_item_limit));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted)
        edit_workspace(service, context, parent, std::move(record));
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
    auto *buttons = dialog_buttons(QCoreApplication::translate("Workspaces", "Run workspace"));
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
    layout->addWidget(label(QCoreApplication::translate("Workspaces", "Workspaces"), "heading"));
    auto *header = new QHBoxLayout;
    auto *create = button(QCoreApplication::translate("Workspaces", "New workspace"), "primary", "add");
    auto *capture = button(QCoreApplication::translate("Workspaces", "Save current desktop"), "quiet");
    header->addWidget(create);
    header->addWidget(capture);
    header->addStretch();
    layout->addLayout(header);
    auto *limitation = label({}, "muted");
    layout->addWidget(limitation);
    auto *empty = label(
        QCoreApplication::translate(
            "Workspaces",
            "Open the apps, files, folders and links for your work in one action. Create a workspace to get started."),
        "muted");
    layout->addWidget(empty);
    auto *list = table(
        {QCoreApplication::translate("Workspaces", "Name"), QCoreApplication::translate("Workspaces", "Resources")},
        QCoreApplication::translate("Workspaces", "Saved workspaces"));
    layout->addWidget(list, 2);
    auto *action_panel = new QWidget;
    auto *actions = new QHBoxLayout(action_panel);
    actions->setContentsMargins(0, 0, 0, 0);
    auto *run = button(QCoreApplication::translate("Workspaces", "Run"), "primary", "play");
    auto *edit = button(QCoreApplication::translate("Workspaces", "Edit"), "quiet");
    auto *duplicate = button(QCoreApplication::translate("Workspaces", "Duplicate"), "quiet");
    auto *remove = button(QCoreApplication::translate("Workspaces", "Delete"), "quiet");
    actions->addWidget(run);
    actions->addWidget(edit);
    actions->addWidget(duplicate);
    actions->addWidget(remove);
    actions->addStretch();
    layout->addWidget(action_panel);
    auto *empty_space = new QWidget;
    layout->addWidget(empty_space, 5);

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
    auto *cancel = button(QCoreApplication::translate("Workspaces", "Cancel run"), "quiet");
    auto *export_report = button(QCoreApplication::translate("Workspaces", "Export run diagnostics…"), "quiet");
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
        if (workspace)
            for (const auto &item : workspace->items) {
                auto status =
                    !item.enabled ? QCoreApplication::translate("Workspaces", "Disabled") : kind_name(item.kind);
                if (item.kind == workspace_item_kind::application &&
                    std::ranges::find(apps, item.application_id, &app_record::id) == apps.cend())
                    status = QCoreApplication::translate("Workspaces", "Missing application");
                else if (!xlibrary::workspace_item_issue(item).isEmpty())
                    status = QCoreApplication::translate("Workspaces", "Invalid resource");
                row(resources, {workspace_item_label(item, apps), status});
            }
        tabs->setTabEnabled(0, workspace != nullptr);
        if (workspace && runs->report().isEmpty())
            tabs->setCurrentIndex(0);
        sync_controls();
    };
    const auto reload = [=](const qint64 requested) {
        const QSignalBlocker blocked(list);
        const auto id = requested ? requested : selection();
        list->clear();
        for (const auto &workspace : service->catalog().workspaces) {
            auto *item = row(list, {workspace.name, QString::number(workspace.items.size())});
            item->setData(0, Qt::UserRole, workspace.id);
            if (workspace.id == id)
                list->setCurrentItem(item);
        }
        if (!list->currentItem())
            list->setCurrentItem(list->topLevelItem(0));
        empty->setVisible(list->topLevelItemCount() == 0);
        list->setVisible(list->topLevelItemCount() > 0);
        action_panel->setVisible(list->topLevelItemCount() > 0);
        const auto show_tabs = list->topLevelItemCount() > 0 || !runs->report().isEmpty();
        tabs->setVisible(show_tabs);
        empty_space->setVisible(!show_tabs);
        show_selected();
    };
    const auto render_run = [=] {
        const auto &steps = runs->report();
        report->setVisible(!steps.isEmpty());
        progress_title->setVisible(!steps.isEmpty());
        progress->setVisible(!steps.isEmpty());
        tabs->setTabEnabled(run_tab, !steps.isEmpty());
        tabs->setVisible(!steps.isEmpty() || selected());
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
