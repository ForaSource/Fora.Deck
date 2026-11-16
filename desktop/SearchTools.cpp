#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSignalBlocker>

#include "DesktopService.h"
#include "PlatformIntegration.h"
#include "SearchTools.h"
#include "UiHelpers.h"

namespace {
using namespace ui_helpers;

void edit_filter(DesktopService &service, QLineEdit &input, QWidget *const parent, const saved_filter &record) {
    auto *dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(record.id ? QCoreApplication::translate("XLibrary", "Rename filter")
                                     : QCoreApplication::translate("XLibrary", "Save current filter"));
    dialog->setMinimumWidth(360);
    auto *layout = new QVBoxLayout(dialog);
    auto *name = new QLineEdit(record.name);
    name->setMaxLength(128);
    field(layout, QCoreApplication::translate("XLibrary", "Filter name"), name);
    auto *error = label("", "error");
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=, &service, record = record]() mutable {
        record.name = name->text().trimmed();
        if (record.name.isEmpty()) {
            error->setText(QCoreApplication::translate("XLibrary", "Enter a filter name."));
            return;
        }
        buttons->button(QDialogButtonBox::Save)->setEnabled(false);
        name->setEnabled(false);
        dialog->setProperty("request_id", QVariant::fromValue(service.save_filter(record)));
    });
    QObject::connect(&service, &DesktopService::operation_changed, dialog, [=](const operation_result &result) {
        if (result.id != dialog->property("request_id").toULongLong() || result.state == operation_state::loading)
            return;
        if (result.state == operation_state::completed)
            dialog->accept();
        else {
            error->setText(result.error.user_message);
            name->setEnabled(true);
            buttons->button(QDialogButtonBox::Save)->setEnabled(true);
        }
    });
    QObject::connect(dialog, &QDialog::finished, &input, [&input] { input.setFocus(); });
    dialog->open();
    name->setFocus();
    name->selectAll();
}
}

QWidget *search_tools(DesktopService &service, QLineEdit &input, QWidget *const parent) {
    auto *widget = new QWidget(parent);
    widget->setObjectName("search_tools");
    // Keep filter controls wired for future use, but unavailable in the current UI.
    widget->setEnabled(false);
    widget->hide();
    auto *root = new QVBoxLayout(widget);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);
    auto *help =
        label(QCoreApplication::translate("XLibrary", "#tag includes a tag; -#tag excludes it. All conditions apply."),
              "muted");
    root->addWidget(help);
    auto *row = new QHBoxLayout;
    root->addLayout(row);
    auto *tags = new QComboBox;
    tags->setObjectName("search_tag_suggestions");
    tags->setAccessibleName(QCoreApplication::translate("XLibrary", "Tag suggestions"));
    tags->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    tags->setMinimumContentsLength(8);
    tags->setMaxVisibleItems(12);
    row->addWidget(tags);
    auto *filters = new QComboBox;
    filters->setObjectName("saved_filters");
    filters->setAccessibleName(QCoreApplication::translate("XLibrary", "Saved filters"));
    filters->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    filters->setMinimumContentsLength(10);
    row->addWidget(filters, 1);
    const auto compact_button = [&, widget](const QString &title, const QString &icon) {
        auto *control = button("", "quiet");
        control->setIcon(appearance::icon(icon));
        control->setAccessibleName(title);
        control->setToolTip(title);
        control->setFixedWidth(appearance::scaled(service.settings, 36));
        control->setParent(widget);
        return control;
    };
    auto *save = compact_button(QCoreApplication::translate("XLibrary", "Save filter"), "add");
    auto *rename = compact_button(QCoreApplication::translate("XLibrary", "Rename filter"), "settings");
    auto *remove = compact_button(QCoreApplication::translate("XLibrary", "Remove filter"), "trash");
    row->addWidget(save);
    row->addWidget(rename);
    row->addWidget(remove);
    auto *state_row = new QHBoxLayout;
    root->addLayout(state_row);
    auto *state = label("", "muted");
    state->setObjectName("active_search_filters");
    state_row->addWidget(state, 1);
    auto *clear = compact_button(QCoreApplication::translate("XLibrary", "Clear all filters"), "x");
    state_row->addWidget(clear);

    const auto refresh = [=, &service, &input] {
        const QSignalBlocker tag_blocker(tags);
        const QSignalBlocker filter_blocker(filters);
        tags->clear();
        tags->addItem(QCoreApplication::translate("XLibrary", "Insert tag…"));
        const auto before_cursor = input.text().left(input.cursorPosition());
        const auto token =
            before_cursor.section(QRegularExpression("\\s+", QRegularExpression::UseUnicodePropertiesOption), -1);
        const bool excluding = token.startsWith("-#");
        const bool editing_tag = excluding || token.startsWith('#');
        const auto prefix = editing_tag ? token.mid(excluding ? 2 : 1) : QString{};
        for (const auto &tag : service.catalog().tags)
            if (tag.normalized(QString::NormalizationForm_C)
                    .toCaseFolded()
                    .startsWith(prefix.normalized(QString::NormalizationForm_C).toCaseFolded())) {
                tags->addItem('#' + tag, '#' + tag);
                tags->addItem("-#" + tag, "-#" + tag);
            }
        filters->clear();
        filters->addItem(QCoreApplication::translate("XLibrary", "Saved filters"), qint64(0));
        for (const auto &record : service.catalog().filters) {
            filters->addItem(record.name, record.id);
            if (record.query == service.saved_filter_query() && record.section == service.section() &&
                record.tag == service.tag() && record.sort_mode == service.filter.sort_mode_value() &&
                record.sort_order == service.filter.sortOrder())
                filters->setCurrentIndex(filters->count() - 1);
        }
        const auto hint = SearchQuery(input.text()).tag_hint(service.catalog().tags);
        QStringList active;
        active.append(service.section() == "favorites" ? QCoreApplication::translate("XLibrary", "Favorites")
                      : service.section() == "running" ? QCoreApplication::translate("XLibrary", "Running applications")
                      : service.section() == "scenarios" ? QCoreApplication::translate("XLibrary", "Scenarios")
                                                         : QCoreApplication::translate("XLibrary", "Library"));
        for (const auto &tag : service.tags())
            active.append('#' + tag);
        if (!input.text().trimmed().isEmpty())
            active.append(input.text().simplified());
        const QStringList sorts{QCoreApplication::translate("XLibrary", "By name"),
                                QCoreApplication::translate("XLibrary", "Recent launches"),
                                QCoreApplication::translate("XLibrary", "Favorites first"),
                                QCoreApplication::translate("XLibrary", "By tags"),
                                QCoreApplication::translate("XLibrary", "By status")};
        active.append(sorts[service.filter.sort_mode_value()] + (service.filter.sortOrder() == Qt::DescendingOrder
                                                                     ? QStringLiteral(" ↓")
                                                                     : QStringLiteral(" ↑")));
        state->setText(QCoreApplication::translate("XLibrary", "Active: %1").arg(active.join(" · ")) +
                       (hint.isEmpty() ? QString{} : '\n' + hint));
        const bool enabled = service.is_ready() && !service.profile_busy() && !service.restart_required();
        save->setEnabled(enabled && service.section() != "scenarios");
        rename->setEnabled(enabled && filters->currentData().toLongLong() != 0);
        remove->setEnabled(rename->isEnabled());
        filters->setEnabled(enabled);
        tags->setEnabled(enabled && tags->count() > 1);
        clear->setEnabled(!input.text().isEmpty() || !service.tag().isEmpty() || service.section() != "library" ||
                          service.filter.sort_mode_value() != 0 || service.filter.sortOrder() != Qt::AscendingOrder);
    };
    QObject::connect(&input, &QLineEdit::textChanged, widget, refresh);
    QObject::connect(&input, &QLineEdit::cursorPositionChanged, widget, refresh);
    QObject::connect(&service, &DesktopService::catalog_changed, widget, refresh);
    QObject::connect(&service, &DesktopService::navigation_changed, widget, refresh);
    QObject::connect(&service, &DesktopService::profile_state_changed, widget, refresh);
    QObject::connect(&service.filter, &LibraryFilter::sort_changed, widget, refresh);
    QObject::connect(&service.filter, &LibraryFilter::query_changed, widget, refresh);
    QObject::connect(clear, &QPushButton::clicked, widget, [&service, &input] {
        service.clear_filters();
        input.setFocus();
    });
    QObject::connect(tags, &QComboBox::activated, widget, [=, &input](const int index) {
        if (index == 0)
            return;
        const auto text = input.text();
        auto start = input.cursorPosition();
        while (start > 0 && !text[start - 1].isSpace())
            --start;
        auto end = input.cursorPosition();
        while (end < text.size() && !text[end].isSpace())
            ++end;
        const auto token = text.mid(start, end - start);
        const auto suggestion = tags->itemData(index).toString();
        if (!token.startsWith('#') && !token.startsWith("-#")) {
            input.setCursorPosition(input.text().size());
            input.insert((input.text().isEmpty() || input.text().back().isSpace() ? QString{} : " ") + suggestion +
                         ' ');
        } else {
            input.setSelection(start, end - start);
            input.insert(suggestion + ' ');
        }
        input.setFocus();
    });
    QObject::connect(filters, &QComboBox::activated, widget, [=, &service, &input](const int index) {
        const auto id = filters->itemData(index).toLongLong();
        // Applying changes emits synchronous UI signals; never retain a reference into a widget model.
        for (const auto &record : service.catalog().filters)
            if (record.id == id) {
                const auto chosen = record;
                service.apply_filter(chosen);
                input.setFocus();
                break;
            }
    });
    QObject::connect(save, &QPushButton::clicked, widget, [=, &service, &input] {
        service.filter.set_query(input.text());
        edit_filter(service, input, widget,
                    {0,
                     {},
                     service.saved_filter_query(),
                     service.section(),
                     service.tag(),
                     service.filter.sort_mode_value(),
                     service.filter.sortOrder()});
    });
    QObject::connect(rename, &QPushButton::clicked, widget, [=, &service, &input] {
        for (const auto &record : service.catalog().filters)
            if (record.id == filters->currentData().toLongLong()) {
                edit_filter(service, input, widget, record);
                break;
            }
    });
    QObject::connect(remove, &QPushButton::clicked, widget, [=, &service] {
        const auto id = filters->currentData().toLongLong();
        if (id &&
            confirm(
                widget, QCoreApplication::translate("XLibrary", "Remove filter"),
                QCoreApplication::translate("XLibrary", "Remove the saved filter “%1”?").arg(filters->currentText())))
            service.remove_filter(id);
    });
    refresh();
    return widget;
}

QWidget *file_search_switch(DesktopService &service, QWidget *parent) {
    auto *segments = new QFrame(parent);
    segments->setObjectName("file_search_switch");
    segments->setProperty("segmented", true);
    auto *layout = new QHBoxLayout(segments);
    layout->setContentsMargins(3, 3, 3, 3);
    layout->setSpacing(2);
    auto *group = new QButtonGroup(segments);
    const auto make_segment = [=](const QString &text, const QString &name) {
        auto *control = new QPushButton(text, segments);
        control->setObjectName(name);
        control->setAccessibleName(text);
        control->setToolTip(text);
        control->setProperty("segment", true);
        control->setCheckable(true);
        control->setCursor(Qt::PointingHandCursor);
        group->addButton(control);
        layout->addWidget(control);
        return control;
    };
    auto *applications = make_segment(QCoreApplication::translate("XLibrary", "Applications"), "search_applications");
    auto *files = make_segment(QCoreApplication::translate("XLibrary", "Files"), "search_files");
    const auto refresh = [=, &service] {
        segments->setVisible(service.files->is_enabled());
        applications->setChecked(!service.searching_files());
        files->setChecked(service.searching_files());
    };
    QObject::connect(applications, &QPushButton::clicked, &service, [&service] { service.set_searching_files(false); });
    QObject::connect(files, &QPushButton::clicked, &service, [&service] { service.set_searching_files(true); });
    QObject::connect(service.files, &FileManager::configuration_changed, segments, refresh);
    QObject::connect(&service, &DesktopService::navigation_changed, segments, refresh);
    refresh();
    return segments;
}
