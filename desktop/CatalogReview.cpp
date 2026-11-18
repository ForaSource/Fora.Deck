#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QListWidget>
#include <QScreen>
#include <QScrollBar>
#include <QTableWidget>
#include <QTimer>
#include <QWindow>

#include "CatalogReview.h"
#include "DesktopService.h"
#include "PathHelpers.h"
#include "UiHelpers.h"

namespace {
using namespace ui_helpers;

QDialog *make_dialog(const ui_context &context, const QString &title) {
    auto *dialog = new QDialog(context.window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(title);
    dialog->setWindowModality(Qt::WindowModal);
    prepare_dialog(dialog, context, {900, 650}, {480, 360});
    return dialog;
}

}

void show_catalog_review(const ui_context &context) {
    auto *service = static_cast<DesktopService *>(context.service);
    if (!service->is_reviewing())
        return;
    const bool removal = service->is_removal_preview();
    const bool scan = service->is_scan_preview();
    const auto records = service->catalog_preview();
    auto *dialog =
        make_dialog(context, removal ? QCoreApplication::translate("XLibrary", "Uninstalled applications")
                             : scan  ? QCoreApplication::translate("XLibrary", "New applications")
                                     : QCoreApplication::translate("XLibrary", "Review applications before import"));
    dialog->setObjectName("catalog_review");
    prepare_dialog(dialog, context, {800, 300}, {480, 180});
    auto *layout = new QVBoxLayout(dialog);
    if (removal)
        layout->addWidget(label(QCoreApplication::translate(
            "XLibrary",
            "These applications appear to have been uninstalled. Remove the checked entries from the library? "
            "Their paths will be checked again before removal.")));
    auto *search = new QLineEdit;
    search->setPlaceholderText(QCoreApplication::translate("XLibrary", "Search name, source or target"));
    search->setAccessibleName(search->placeholderText());
    layout->addWidget(search);
    auto *tools = new QHBoxLayout;
    auto *all = button(QCoreApplication::translate("XLibrary", "Select all"));
    auto *none = button(QCoreApplication::translate("XLibrary", "Clear selection"));
    auto *count = label({});
    tools->addWidget(all);
    tools->addWidget(none);
    tools->addWidget(count, 1);
    layout->addLayout(tools);
    auto *table = new QTableWidget(static_cast<int>(records.size()), 5);
    table->setObjectName("catalog_review_table");
    table->setHorizontalHeaderLabels(
        {QCoreApplication::translate("XLibrary", "Name"),
         removal ? QCoreApplication::translate("XLibrary", "Reason for removal") : QCoreApplication::translate("XLibrary", "Source"),
         QCoreApplication::translate("XLibrary", "Target"), QCoreApplication::translate("XLibrary", "Launch arguments"),
         QCoreApplication::translate("XLibrary", "Working directory")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setColumnWidth(0, appearance::scaled(*context.settings, 220));
    table->setColumnWidth(1, appearance::scaled(*context.settings, 160));
    table->setColumnWidth(2, appearance::scaled(*context.settings, 250));
    if (scan || removal) {
        table->setColumnHidden(1, true);
        table->setColumnHidden(3, true);
        table->setColumnHidden(4, true);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    }
    if (removal) {
        table->setColumnHidden(1, false);
        table->setColumnWidth(1, appearance::scaled(*context.settings, 300));
    }
    QSet<QString> known;
    for (const auto &record : service->catalog().applications)
        known.insert(launch_configuration_key(record.path, record.arguments, record.working_directory));
    for (int row = 0; row < records.size(); ++row) {
        const auto &record = records[row];
        const bool duplicate =
            known.contains(launch_configuration_key(record.path, record.arguments, record.working_directory));
        auto *name = new QTableWidgetItem(record.name);
        name->setFlags(name->flags() | Qt::ItemIsUserCheckable);
        name->setCheckState(Qt::Checked);
        if (duplicate && !removal)
            name->setToolTip(
                QCoreApplication::translate("XLibrary", "This launch configuration is already in the library."));
        table->setItem(row, 0, name);
        const QStringList values{removal ? record.removal_reason : record.discovery_source.isEmpty()
                                     ? QCoreApplication::translate("XLibrary", "JSON catalog")
                                     : record.discovery_source,
                                 QDir::toNativeSeparators(Application::application_path(record)), record.arguments,
                                 QDir::toNativeSeparators(record.working_directory)};
        for (int column = 1; column < 5; ++column) {
            auto *item = new QTableWidgetItem(values[column - 1]);
            item->setToolTip(item->text());
            table->setItem(row, column, item);
        }
    }
    layout->addWidget(table, 1);
    const auto update_count = [=] {
        int selected = 0;
        int visible = 0;
        for (int row = 0; row < table->rowCount(); ++row) {
            selected += table->item(row, 0)->checkState() == Qt::Checked;
            visible += !table->isRowHidden(row);
        }
        count->setText(QCoreApplication::translate("XLibrary", "Selected: %1 / %2. Shown: %3.")
                           .arg(selected)
                           .arg(table->rowCount())
                           .arg(visible));
    };
    QObject::connect(table, &QTableWidget::itemChanged, dialog, [=] { update_count(); });
    const auto set_visible_checked = [=](const Qt::CheckState state) {
        const QSignalBlocker blocker(table);
        for (int row = 0; row < table->rowCount(); ++row)
            if (!table->isRowHidden(row))
                table->item(row, 0)->setCheckState(state);
        update_count();
    };
    QObject::connect(all, &QPushButton::clicked, dialog, [=] { set_visible_checked(Qt::Checked); });
    QObject::connect(none, &QPushButton::clicked, dialog, [=] { set_visible_checked(Qt::Unchecked); });
    all->setToolTip(QCoreApplication::translate(
        "XLibrary", "Applies to the visible search results. Hidden rows keep their selection."));
    none->setToolTip(all->toolTip());
    QObject::connect(search, &QLineEdit::textChanged, dialog, [=](const QString &text) {
        for (int row = 0; row < table->rowCount(); ++row) {
            bool matches = false;
            for (int column = 0; column < table->columnCount(); ++column)
                matches |= table->item(row, column)->text().contains(text.trimmed(), Qt::CaseInsensitive);
            table->setRowHidden(row, !matches);
        }
        update_count();
    });
    auto *policy = new QComboBox;
    policy->addItems(
        {QCoreApplication::translate("XLibrary", "Skip matching launch configurations (default)"),
         QCoreApplication::translate("XLibrary", "Replace name, tags and favorite for matching configurations")});
    policy->setAccessibleName(QCoreApplication::translate("XLibrary", "Conflict policy"));
    policy->setVisible(service->is_json_preview());
    layout->addWidget(policy);
    auto *remember =
        new QCheckBox(QCoreApplication::translate("XLibrary", "Do not offer unchecked applications in future scans"));
    remember->setChecked(true);
    remember->setVisible(scan && !removal);
    layout->addWidget(remember);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)
        ->setText(removal ? QCoreApplication::translate("XLibrary", "Remove selected")
                  : scan  ? QCoreApplication::translate("XLibrary", "Add selected")
                          : QCoreApplication::translate("XLibrary", "Import selected"));
    buttons->button(QDialogButtonBox::Cancel)
        ->setText(removal ? QCoreApplication::translate("XLibrary", "Keep all")
                          : QCoreApplication::translate("XLibrary", "Cancel"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
        QVector<int> rows;
        for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 0)->checkState() == Qt::Checked)
                rows.append(row);
        service->apply_catalog_preview(rows, remember->isChecked(),
                                       policy->currentIndex() == 1 ? import_conflict_policy::replace_metadata
                                                                   : import_conflict_policy::skip);
        dialog->accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(dialog, &QDialog::rejected, service, [=] {
        if (removal)
            service->keep_missing_applications();
        else
            service->cancel_discovery();
    });
    QObject::connect(service, &DesktopService::discovery_changed, dialog, [=] {
        if (!service->is_discovering())
            dialog->reject();
    });
    update_count();
    dialog->ensurePolished();
    const auto row_height = qMax(table->verticalHeader()->defaultSectionSize(),
                                 table->fontMetrics().height() + appearance::scaled(*context.settings, 12));
    table->verticalHeader()->setDefaultSectionSize(row_height);
    const auto table_frame = table->horizontalHeader()->sizeHint().height() + 2 * table->frameWidth();
    const auto visible_rows = qBound(1, table->rowCount(), 10);
    const auto scrollbar_height = scan ? 0 : table->horizontalScrollBar()->sizeHint().height();
    // Measure the surrounding controls with a content-sized table before allowing manual resizing.
    table->setFixedHeight(table_frame + visible_rows * row_height + scrollbar_height);
    layout->activate();
    const auto available = dialog->screen()->availableGeometry().size() - QSize(32, 64);
    const auto width = qMin(appearance::scaled(*context.settings, scan ? 800 : 900), available.width());
    const auto height = layout->hasHeightForWidth() ? layout->totalHeightForWidth(width) : layout->sizeHint().height();
    table->setMaximumHeight(QWIDGETSIZE_MAX);
    table->setMinimumHeight(table_frame + row_height + scrollbar_height);
    dialog->resize(width, qMin(height, available.height()));
    dialog->show();
}

void show_application_tags(const ui_context &context) {
    auto *service = static_cast<DesktopService *>(context.service);
    const auto ids = service->selections();
    if (ids.isEmpty())
        return;
    const auto *settings = context.settings;
    const auto title = QCoreApplication::translate("XLibrary", "Tags");
    auto *dialog = new QDialog(context.window);
    dialog->setObjectName("application_tags_dialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(title);
    auto *root = new QVBoxLayout(dialog);
    const auto margin = appearance::scaled(*settings, 18);
    root->setContentsMargins(margin, margin, margin, margin);
    root->setSpacing(appearance::scaled(*settings, 10));
    auto *contents = new QWidget;
    contents->setBackgroundRole(QPalette::Base);
    auto *layout = new QVBoxLayout(contents);
    layout->setContentsMargins(0, 0, 0, 2);
    layout->setSpacing(appearance::scaled(*settings, 8));
    auto *heading = new QHBoxLayout;
    auto *icon = new QLabel;
    const auto icon_size = appearance::scaled(*settings, 24);
    icon->setPixmap(appearance::icon("tag", dialog->palette().color(QPalette::Link)).pixmap(icon_size, icon_size));
    heading->addWidget(icon);
    heading->addWidget(label(title, "heading"), 1);
    layout->addLayout(heading);

    const auto *single = ids.size() == 1 ? service->model.find(ids.front()) : nullptr;
    layout->addWidget(label(single ? single->name
                                   : QCoreApplication::translate("XLibrary", "Selected applications: %1").arg(ids.size()),
                            "muted"));
    QHash<QString, qsizetype> assignments;
    for (const auto id : ids)
        if (const auto *record = service->model.find(id))
            for (const auto &tag : record->tags)
                ++assignments[tag_name_key(tag)];

    auto *search = new QLineEdit;
    search->setPlaceholderText(QCoreApplication::translate("XLibrary", "Search tags"));
    search->setAccessibleName(search->placeholderText());
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto *tags = new QListWidget;
    tags->setProperty("tag_selector", true);
    tags->setAccessibleName(title);
    tags->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(tags);
    const auto add_row = [tags](const QString &tag, const Qt::CheckState state) {
        auto *item = new QListWidgetItem('#' + tag, tags);
        item->setData(Qt::UserRole, tag);
        item->setData(Qt::UserRole + 1, static_cast<int>(state));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        if (state == Qt::PartiallyChecked)
            item->setFlags(item->flags() | Qt::ItemIsUserTristate);
        item->setCheckState(state);
        return item;
    };
    bool mixed = false;
    for (const auto &tag : service->catalog().tags) {
        const auto assigned = assignments.value(tag_name_key(tag));
        const auto state = assigned == 0 ? Qt::Unchecked : assigned == ids.size() ? Qt::Checked : Qt::PartiallyChecked;
        add_row(tag, state);
        mixed |= state == Qt::PartiallyChecked;
    }
    auto *empty = label({}, "muted");
    layout->addWidget(empty);
    if (mixed)
        layout->addWidget(label(QCoreApplication::translate(
                                    "XLibrary", "A dash means the tag is assigned to some selected applications."),
                                "caption"));
    auto *new_tags = new QLineEdit;
    new_tags->setPlaceholderText(QCoreApplication::translate("XLibrary", "Add tags separated by commas"));
    new_tags->setAccessibleName(new_tags->placeholderText());
    auto *add = button(QCoreApplication::translate("XLibrary", "Add"), nullptr, "add");
    add->setAutoDefault(false);
    add->setEnabled(false);
    auto *new_tag_row = new QHBoxLayout;
    new_tag_row->addWidget(new_tags, 1);
    new_tag_row->addWidget(add);
    layout->addLayout(new_tag_row);
    auto *error = label({}, "error");
    layout->addWidget(error);
    error->hide();
    auto *body = scroll(contents);
    root->addWidget(body, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto *save = buttons->button(QDialogButtonBox::Save);
    save->setText(QCoreApplication::translate("XLibrary", "Save"));
    save->setProperty("primary", true);
    save->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("XLibrary", "Cancel"));
    root->addWidget(buttons);

    auto *fit_timer = new QTimer(dialog);
    fit_timer->setSingleShot(true);
    const auto request_fit = [fit_timer] { fit_timer->start(0); };
    const auto fit = [=] {
        const auto available = dialog->screen()->availableGeometry().marginsRemoved({12, 12, 12, 12});
        const auto frame_size = dialog->frameGeometry().size() - dialog->size();
        const auto limit = available.size() - frame_size;
        if (limit.isEmpty())
            return;
        const int width = qMin(appearance::scaled(*settings, 468), limit.width());
        const int content_width = qMax(width - 2 * margin, contents->minimumSizeHint().width());
        const auto content_height =
            layout->hasHeightForWidth() ? layout->totalHeightForWidth(content_width) : layout->sizeHint().height();
        const int height = 2 * margin + root->spacing() + content_height + buttons->sizeHint().height();
        dialog->setFixedSize(width, qMin(height, limit.height()));
        if (dialog->isVisible()) {
            const auto frame = dialog->frameGeometry();
            const auto position = QPoint(qBound(available.left(), frame.x(),
                                                qMax(available.left(), available.right() - frame.width() + 1)),
                                         qBound(available.top(), frame.y(),
                                                qMax(available.top(), available.bottom() - frame.height() + 1)));
            if (position != frame.topLeft())
                dialog->move(dialog->pos() + position - frame.topLeft());
        }
    };
    const auto update_list = [=] {
        const auto query = search->text().trimmed();
        int rows = 0;
        int height = 2 * tags->frameWidth();
        for (int row = 0; row < tags->count(); ++row) {
            const bool visible = tags->item(row)->text().contains(query, Qt::CaseInsensitive);
            tags->item(row)->setHidden(!visible);
            if (!visible)
                continue;
            if (rows < 6)
                height += tags->sizeHintForRow(row);
            ++rows;
        }
        search->setVisible(tags->count() > 5);
        tags->setVisible(rows > 0);
        tags->setFixedHeight(height);
        empty->setText(tags->count() == 0 ? QCoreApplication::translate("XLibrary", "No tags")
                                         : QCoreApplication::translate("XLibrary", "No matching tags"));
        empty->setVisible(rows == 0);
        request_fit();
    };
    const auto set_error = [=](const QString &text) {
        error->setText(text);
        error->setVisible(!text.isEmpty());
        request_fit();
    };
    const auto stage_tags = [=] {
        if (new_tags->text().trimmed().isEmpty())
            return true;
        QStringList names;
        for (auto tag : new_tags->text().split(',', Qt::SkipEmptyParts)) {
            tag = tag.trimmed();
            if (tag.startsWith('#'))
                tag.remove(0, 1);
            if (!valid_tag_name(tag)) {
                set_error(QCoreApplication::translate(
                    "XLibrary", "Tag: up to 64 characters, without spaces, commas, or #."));
                new_tags->setFocus();
                return false;
            }
            if (!has_tag(names, tag))
                names.append(tag);
        }
        QListWidgetItem *last = nullptr;
        for (const auto &name : names) {
            const auto key = tag_name_key(name);
            QListWidgetItem *existing = nullptr;
            for (int row = 0; row < tags->count(); ++row) {
                auto *item = tags->item(row);
                if (tag_name_key(item->data(Qt::UserRole).toString()) == key) {
                    existing = item;
                    break;
                }
            }
            last = existing ? existing : add_row(name, Qt::Unchecked);
            last->setCheckState(Qt::Checked);
        }
        new_tags->clear();
        search->clear();
        update_list();
        if (last)
            tags->scrollToItem(last);
        return true;
    };
    QObject::connect(search, &QLineEdit::textChanged, dialog, update_list);
    QObject::connect(new_tags, &QLineEdit::textChanged, dialog, [=](const QString &text) {
        add->setEnabled(!text.trimmed().isEmpty());
        set_error({});
    });
    QObject::connect(add, &QPushButton::clicked, dialog, [=] {
        if (stage_tags())
            new_tags->setFocus();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
        if (!stage_tags())
            return;
        QStringList add_tags;
        QStringList remove_tags;
        for (int row = 0; row < tags->count(); ++row) {
            const auto *item = tags->item(row);
            if (item->checkState() == item->data(Qt::UserRole + 1).toInt() ||
                item->checkState() == Qt::PartiallyChecked)
                continue;
            (item->checkState() == Qt::Checked ? add_tags : remove_tags).append(item->data(Qt::UserRole).toString());
        }
        if (add_tags.isEmpty() && remove_tags.isEmpty()) {
            dialog->accept();
            return;
        }
        contents->setEnabled(false);
        save->setEnabled(false);
        set_error({});
        dialog->setProperty("save_request_id",
                            QVariant::fromValue(service->update_metadata(ids, add_tags, remove_tags, -1)));
    });
    QObject::connect(service, &DesktopService::operation_changed, dialog, [=](const operation_result &result) {
        if (result.id != dialog->property("save_request_id").toULongLong() || result.state == operation_state::loading)
            return;
        if (result.state == operation_state::completed)
            dialog->accept();
        else {
            contents->setEnabled(true);
            save->setEnabled(true);
            set_error(result.error.user_message);
        }
    });
    QObject::connect(fit_timer, &QTimer::timeout, dialog, fit);
    dialog->ensurePolished();
    tags->ensurePolished();
    update_list();
    fit();
    (tags->count() ? static_cast<QWidget *>(tags) : new_tags)->setFocus();
    dialog->open();
    if (auto *window = dialog->windowHandle())
        QObject::connect(window, &QWindow::screenChanged, dialog, request_fit);
    request_fit();
}
