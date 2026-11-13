#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QListWidget>
#include <QScreen>
#include <QScrollBar>
#include <QTableWidget>

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
        {QCoreApplication::translate("XLibrary", "Name"), QCoreApplication::translate("XLibrary", "Source"),
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
    if (scan) {
        table->setColumnHidden(1, true);
        table->setColumnHidden(3, true);
        table->setColumnHidden(4, true);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
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
        if (duplicate)
            name->setToolTip(
                QCoreApplication::translate("XLibrary", "This launch configuration is already in the library."));
        table->setItem(row, 0, name);
        const QStringList values{record.discovery_source.isEmpty()
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

void show_bulk_editor(const ui_context &context) {
    auto *service = static_cast<DesktopService *>(context.service);
    const auto ids = service->selections();
    if (ids.isEmpty())
        return;
    auto *dialog = make_dialog(context, QCoreApplication::translate("XLibrary", "Edit selected applications"));
    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(label(QCoreApplication::translate("XLibrary", "Selected applications: %1").arg(ids.size())));
    auto *favorite = new QCheckBox(QCoreApplication::translate("XLibrary", "Favorite"));
    favorite->setTristate(true);
    qsizetype favorites = 0;
    for (const auto id : ids)
        if (const auto *record = service->model.find(id); record && record->favorite)
            ++favorites;
    const auto favorite_state = favorites == 0            ? Qt::Unchecked
                                : favorites == ids.size() ? Qt::Checked
                                                          : Qt::PartiallyChecked;
    favorite->setCheckState(favorite_state);
    favorite->setToolTip(QCoreApplication::translate("XLibrary", "Mixed means keep each application's current value."));
    layout->addWidget(favorite);
    auto *search = new QLineEdit;
    search->setPlaceholderText(QCoreApplication::translate("XLibrary", "Search tags"));
    search->setAccessibleName(search->placeholderText());
    layout->addWidget(search);
    auto *tags = new QListWidget;
    for (const auto &tag : service->catalog().tags) {
        qsizetype assigned = 0;
        qsizetype usage = 0;
        for (const auto &record : service->catalog().applications) {
            if (has_tag(record.tags, tag)) {
                ++usage;
                if (ids.contains(record.id))
                    ++assigned;
            }
        }
        auto *item = new QListWidgetItem(QString("#%1 (%2)").arg(tag, QString::number(usage)), tags);
        item->setData(Qt::UserRole, tag);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsUserTristate);
        const auto state = assigned == 0 ? Qt::Unchecked : assigned == ids.size() ? Qt::Checked : Qt::PartiallyChecked;
        item->setData(Qt::UserRole + 1, static_cast<int>(state));
        item->setCheckState(state);
        item->setToolTip(QCoreApplication::translate("XLibrary", "Mixed means keep each application's current value."));
    }
    layout->addWidget(tags, 1);
    QObject::connect(search, &QLineEdit::textChanged, tags, [=](const QString &text) {
        for (int row = 0; row < tags->count(); ++row)
            tags->item(row)->setHidden(!tags->item(row)->text().contains(text.trimmed(), Qt::CaseInsensitive));
    });
    auto *new_tags = new QLineEdit;
    new_tags->setPlaceholderText(QCoreApplication::translate("XLibrary", "Add tags separated by commas"));
    new_tags->setAccessibleName(new_tags->placeholderText());
    layout->addWidget(new_tags);
    auto *error = label({}, "error");
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(QCoreApplication::translate("XLibrary", "Save"));
    buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("XLibrary", "Cancel"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
        QStringList add_tags;
        QStringList remove_tags;
        for (int row = 0; row < tags->count(); ++row) {
            const auto *item = tags->item(row);
            if (item->checkState() == item->data(Qt::UserRole + 1).toInt() ||
                item->checkState() == Qt::PartiallyChecked)
                continue;
            (item->checkState() == Qt::Checked ? add_tags : remove_tags).append(item->data(Qt::UserRole).toString());
        }
        for (auto tag : new_tags->text().split(',', Qt::SkipEmptyParts)) {
            tag = tag.trimmed();
            if (tag.startsWith('#'))
                tag.remove(0, 1);
            if (!valid_tag_name(tag)) {
                error->setText(
                    QCoreApplication::translate("XLibrary", "Tag: up to 64 characters, without spaces, commas, or #."));
                return;
            }
            if (!has_tag(add_tags, tag))
                add_tags.append(tag);
        }
        buttons->button(QDialogButtonBox::Save)->setEnabled(false);
        const auto state = favorite->checkState();
        const int favorite_value = state == favorite_state || state == Qt::PartiallyChecked ? -1
                                   : state == Qt::Checked                                   ? 1
                                                                                            : 0;
        dialog->setProperty("save_request_id",
                            QVariant::fromValue(service->update_metadata(ids, add_tags, remove_tags, favorite_value)));
    });
    QObject::connect(service, &DesktopService::operation_changed, dialog, [=](const operation_result &result) {
        if (result.id != dialog->property("save_request_id").toULongLong() || result.state == operation_state::loading)
            return;
        if (result.state == operation_state::completed)
            dialog->accept();
        else {
            buttons->button(QDialogButtonBox::Save)->setEnabled(true);
            error->setText(result.error.user_message);
        }
    });
    dialog->show();
}
