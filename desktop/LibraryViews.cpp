#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QStyledItemDelegate>
#include <QTreeView>

#include "appearance.h"
#include "Application.h"
#include "DesktopService.h"
#include "editors.h"
#include "LibraryViews.h"
#include "localization.h"
#include "NotificationBanner.h"
#include "ProfileView.h"
#include "SearchResults.h"
#include "SearchTools.h"
#include "SelectionButton.h"
#include "UiHelpers.h"

namespace {
using namespace ui_helpers;

class SearchInput : public QLineEdit {
public:
    std::function<void(int)> move_selection;
    std::function<void()> synchronize_search;
    QList<QAction *> application_actions;

protected:
    bool event(QEvent *const event) override {
        const bool handled = QLineEdit::event(event);
        if (event->type() == QEvent::ShortcutOverride && !event->isAccepted() && synchronize_search) {
            const auto *key_event = static_cast<QKeyEvent *>(event);
            const QKeySequence sequence(key_event->keyCombination());
            const QKeySequence without_keypad(
                QKeyCombination(key_event->modifiers() & ~Qt::KeypadModifier, key_event->keyCombination().key()));
            for (auto *action : application_actions) {
                if (action->shortcuts().contains(sequence) || action->shortcuts().contains(without_keypad)) {
                    // Apply any pending request before selecting an action and checking its availability.
                    synchronize_search();
                    break;
                }
            }
        }
        return handled;
    }

    void keyPressEvent(QKeyEvent *const event) override {
        if (move_selection && (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up)) {
            move_selection(event->key() == Qt::Key_Down ? 1 : -1);
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }
};

class ApplicationList : public QTreeView {
    void update_columns() {
        if (!model() || model()->columnCount() < 3)
            return;
        const int unit = qMax(1, fontMetrics().height());
        const int width = viewport()->width();
        setColumnHidden(1, width < unit * 34);
        setColumnHidden(2, width < unit * 22);
        header()->resizeSection(1, qBound(unit * 7, width / 4, unit * 15));
        header()->resizeSection(2, qBound(unit * 8, width / 5, unit * 11));
    }

protected:
    void resizeEvent(QResizeEvent *const event) override {
        QTreeView::resizeEvent(event);
        update_columns();
    }
    void changeEvent(QEvent *const event) override {
        QTreeView::changeEvent(event);
        if (event->type() == QEvent::FontChange)
            update_columns();
    }

public:
    using QTreeView::QTreeView;
};

class ApplicationDelegate : public QStyledItemDelegate {
    const QIcon fallback_icon = appearance::icon("app");
    const QIcon favorite_icon = appearance::icon("favorite");

    static int scaled_size(const QStyleOptionViewItem &option, const int size) {
        return qRound(size * qMax(14, option.font.pixelSize()) / 14.0);
    }

public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        return {100, qMax(scaled_size(option, 52), option.fontMetrics.height() + scaled_size(option, 20))};
    }
    void paint(QPainter *const painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setFont(option.font);
        const auto inset = scaled_size(option, 2);
        const auto padding = scaled_size(option, 12);
        const auto rect = option.rect.adjusted(0, inset, 0, -inset);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        auto row_rect = rect;
        if (const auto *view = qobject_cast<const QTreeView *>(parent())) {
            row_rect.setLeft(inset);
            row_rect.setRight(view->viewport()->width() - inset - 1);
        }
        painter->setClipRect(option.rect, Qt::IntersectClip);
        if (selected || hover) {
            const auto radius = 10;
            if (selected)
                appearance::paint_selection(*painter, QRectF(row_rect).adjusted(0.5, 0.5, -0.5, -0.5), option.palette,
                                            radius);
            else {
                painter->setBrush(option.palette.alternateBase());
                painter->setPen(Qt::NoPen);
                painter->drawRoundedRect(row_rect, radius, radius);
            }
        }
        const auto text_color = option.palette.color(QPalette::Text);
        painter->setPen(text_color);
        auto text_rect = rect.adjusted(padding, 0, -padding, 0);
        if (index.column() == 0) {
            const int size = qMin(scaled_size(option, 32), rect.height() - padding);
            const QRect icon_rect(text_rect.left(), rect.center().y() - size / 2, size, size);
            auto icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            if (icon.isNull())
                icon = fallback_icon;
            icon.paint(painter, icon_rect);
            text_rect.setLeft(icon_rect.right() + padding);
            if (index.data(LibraryModel::favorite_role).toBool()) {
                const auto star_size = scaled_size(option, 16);
                favorite_icon.paint(painter, QRect(text_rect.right() - star_size, rect.center().y() - star_size / 2,
                                                   star_size, star_size));
                text_rect.adjust(0, 0, -scaled_size(option, 24), 0);
            }
            auto name_font = option.font;
            name_font.setPixelSize(scaled_size(option, 15));
            painter->setFont(name_font);
            painter->drawText(
                text_rect, Qt::AlignVCenter,
                QFontMetrics(name_font).elidedText(index.data().toString(), Qt::ElideRight, text_rect.width()));
        } else if (index.column() == 1) {
            QFont small = option.font;
            small.setPixelSize(scaled_size(option, 12));
            painter->setFont(small);
            const QFontMetrics metrics(small);
            const auto badge_padding = scaled_size(option, 8);
            const auto badge_height = qMax(scaled_size(option, 24), metrics.height() + scaled_size(option, 4));
            const auto badge_radius = scaled_size(option, 6);
            const auto tags = index.data(LibraryModel::tags_role).toStringList();
            int x = text_rect.left();
            for (qsizetype i = 0; i < tags.size(); ++i) {
                QString text = '#' + tags[i];
                int width = metrics.horizontalAdvance(text) + badge_padding * 2;
                if (x + width > text_rect.right() && i > 0) {
                    text = '+' + QString::number(tags.size() - i);
                    width = metrics.horizontalAdvance(text) + badge_padding * 2;
                }
                width = qMin(width, text_rect.right() - x);
                if (width < scaled_size(option, 20))
                    break;
                const QRect badge(x, rect.center().y() - badge_height / 2, width, badge_height);
                painter->setBrush(option.palette.color(QPalette::Button));
                painter->setPen(Qt::NoPen);
                painter->drawRoundedRect(badge, badge_radius, badge_radius);
                painter->setPen(text_color);
                painter->drawText(badge.adjusted(badge_padding, 0, -badge_padding, 0), Qt::AlignVCenter,
                                  metrics.elidedText(text, Qt::ElideRight, width - badge_padding * 2));
                x += width + scaled_size(option, 5);
                if (text.startsWith('+'))
                    break;
            }
        } else {
            const bool running = index.data(LibraryModel::running_role).toBool();
            const bool available = index.data(LibraryModel::available_role).toBool();
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(running ? "#85A28A" : available ? "#80828A" : "#C2AA7D"));
            const auto radius = scaled_size(option, 7) / 2.0;
            painter->drawEllipse(QPointF(text_rect.left() + scaled_size(option, 4), rect.center().y()), radius, radius);
            text_rect.adjust(scaled_size(option, 16), 0, 0, 0);
            auto small = option.font;
            small.setPixelSize(scaled_size(option, 12));
            painter->setFont(small);
            painter->setPen(option.palette.color(QPalette::PlaceholderText));
            painter->drawText(
                text_rect, Qt::AlignVCenter,
                QFontMetrics(small).elidedText(index.data().toString(), Qt::ElideRight, text_rect.width()));
        }
        painter->restore();
    }
};

QWidget *navigation(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = new QFrame(parent);
    widget->setProperty("panel", true);
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(12, 20, 12, 16);
    layout->setSpacing(8);
    auto *brand = new QHBoxLayout;
    brand->setSpacing(10);
    auto *logo = new QLabel;
    logo->setPixmap(appearance::logo().pixmap(32, 32));
    logo->setFixedSize(32, 32);
    brand->addWidget(logo);
    brand->addWidget(label("XLibrary", "brand"), 1);
    layout->addLayout(brand);
    layout->addSpacing(16);
    auto *group = new QButtonGroup(widget);
    group->setExclusive(true);
    const QList<QStringList> links{{"library", QCoreApplication::translate("XLibrary", "Library"), "library"},
                                   {"favorites", QCoreApplication::translate("XLibrary", "Favorites"), "favorite"},
                                   {"running", QCoreApplication::translate("XLibrary", "Running applications"), "play"},
                                   {"scenarios", QCoreApplication::translate("XLibrary", "Scenarios"), "scenarios"}};
    for (const auto &link : links) {
        auto *item = new SelectionButton(link[1]);
        item->setIcon(appearance::icon(link[2]));
        item->setProperty("navigation_section", link[0]);
        item->setChecked(link[0] == "library");
        item->setMinimumHeight(30);
        group->addButton(item);
        layout->addWidget(item);
        QObject::connect(item, &QPushButton::clicked, widget, [=] { service->navigate(link[0]); });
    }
    layout->addSpacing(20);
    layout->addWidget(label(QCoreApplication::translate("XLibrary", "My tags"), "muted"));
    auto *tag_search = new QLineEdit;
    tag_search->setPlaceholderText(QCoreApplication::translate("XLibrary", "Search tags"));
    tag_search->setAccessibleName(tag_search->placeholderText());
    layout->addWidget(tag_search);
    auto *tags = new QListWidget;
    tags->setObjectName("tag_list");
    tags->setProperty("panel", true);
    tags->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(tags, 1);
    auto rebuild = [=] {
        const QSignalBlocker blocker(tags);
        const auto current = service->tag();
        tags->clear();
        for (const auto &tag : service->catalog().tags) {
            qsizetype usage = 0;
            for (const auto &record : service->catalog().applications)
                usage += has_tag(record.tags, tag);
            auto *item = new QListWidgetItem(QString("#%1 (%2)").arg(tag, QString::number(usage)), tags);
            item->setData(Qt::UserRole, tag);
            item->setHidden(!tag.contains(tag_search->text().trimmed(), Qt::CaseInsensitive));
            item->setSizeHint({100, appearance::scaled(*context.settings, 36)});
            if (tag == current)
                tags->setCurrentItem(item);
        }
    };
    QObject::connect(tag_search, &QLineEdit::textChanged, widget, [=](const QString &text) {
        for (int row = 0; row < tags->count(); ++row)
            tags->item(row)->setHidden(
                !tags->item(row)->data(Qt::UserRole).toString().contains(text.trimmed(), Qt::CaseInsensitive));
    });
    QObject::connect(service, &DesktopService::catalog_changed, widget, rebuild);
    QObject::connect(service, &DesktopService::navigation_changed, widget, [=] {
        group->setExclusive(false);
        for (auto *item : group->buttons())
            item->setChecked(service->tag().isEmpty() &&
                             item->property("navigation_section").toString() == service->section());
        group->setExclusive(true);
        if (service->tag().isEmpty())
            tags->setCurrentItem(nullptr);
    });
    QObject::connect(tags, &QListWidget::currentItemChanged, widget, [=](QListWidgetItem *const item) {
        if (!item)
            return;
        group->setExclusive(false);
        for (auto *entry : group->buttons())
            entry->setChecked(false);
        group->setExclusive(true);
        service->navigate("library", item->data(Qt::UserRole).toString());
    });
    QObject::connect(tags, &QListWidget::customContextMenuRequested, widget, [=](const QPoint &position) {
        const auto *item = tags->itemAt(position);
        if (!item)
            return;
        const auto tag = item->data(Qt::UserRole).toString();
        QMenu menu;
        auto *edit = menu.addAction(QCoreApplication::translate("XLibrary", "Rename"));
        auto *remove = menu.addAction(QCoreApplication::translate("XLibrary", "Remove tag"));
        const auto *chosen = menu.exec(tags->viewport()->mapToGlobal(position));
        if (chosen == edit) {
            auto edit_context = context;
            edit_context.parameters.insert("tag", tag);
            open_dialog("tag_editor", edit_context);
        }
        if (chosen == remove &&
            confirm(widget, QCoreApplication::translate("XLibrary", "Remove tag"),
                    QCoreApplication::translate("XLibrary", "Remove #") + tag +
                        QCoreApplication::translate(
                            "XLibrary", " from all applications? Saved filters will also stop using this tag.")))
            service->remove_tag(tag);
    });
    layout->addWidget(action_button(context.actions.value("add_tag"), true));
    layout->addSpacing(8);
    layout->addWidget(action_button(context.actions.value("add_application")));
    rebuild();
    return widget;
}

QWidget *search_bar(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = new QWidget(parent);
    auto *root = new QVBoxLayout(widget);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);
    auto *layout = new QHBoxLayout;
    root->addLayout(layout);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto *search = new SearchInput;
    search->setMaxLength(static_cast<int>(catalog_limits::text_length));
    search->setObjectName("search_input");
    search->setAccessibleName(
        QCoreApplication::translate("XLibrary", "Search applications, calculate or enter a path"));
    search->setPlaceholderText(QCoreApplication::translate("XLibrary", "Search, calculate or enter a path"));
    search->setClearButtonEnabled(true);
    search->setMinimumHeight(26);
    search->addAction(appearance::icon("search"), QLineEdit::LeadingPosition);
    auto *timer = new QTimer(widget);
    timer->setInterval(35);
    timer->setSingleShot(true);
    QObject::connect(search, &QLineEdit::textChanged, timer, qOverload<>(&QTimer::start));
    QObject::connect(&service->filter, &LibraryFilter::query_changed, search, [=](const QString &text) {
        if (search->text() != text) {
            search->setText(text);
            timer->stop();
        }
    });
    QObject::connect(timer, &QTimer::timeout, widget, [=] { service->filter.set_query(search->text()); });
    QObject::connect(context.actions.value("focus_search"), &QAction::triggered, search, [=] {
        search->setFocus();
        search->selectAll();
    });
    search->synchronize_search = [=] {
        timer->stop();
        if (service->section() == "scenarios")
            service->navigate("library");
        service->filter.set_query(search->text());
    };
    for (const auto &id : {"launch", "new_instance", "open_folder", "copy_path", "remove_application"})
        search->application_actions.append(context.actions.value(id));
    search->move_selection = [=](const int offset) {
        search->synchronize_search();
        if (auto *view = context.window->findChild<QTreeView *>("application_list")) {
            const auto count = service->filter.rowCount();
            if (!count)
                return;
            const auto row = qBound(0, view->currentIndex().row() + offset, count - 1);
            const auto index = service->filter.index(row, 0);
            view->setCurrentIndex(index);
            view->scrollTo(index);
        }
    };
    layout->addWidget(search, 1);
    auto *settings = action_button(context.actions.value("settings"), false, "");
    settings->setAccessibleName(QCoreApplication::translate("XLibrary", "Settings"));
    settings->setFixedWidth(44);
    layout->addWidget(settings);
    root->addWidget(new SearchResults(*service, *search, false, widget));
    root->addWidget(search_tools(*service, *search, widget));
    return widget;
}

QWidget *discovery_status(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *container = new QWidget(parent);
    auto *container_layout = new QVBoxLayout(container);
    container_layout->setContentsMargins(0, 0, 0, 0);
    container_layout->setSpacing(0);
    auto *panel = new QFrame(container);
    auto *panel_reveal = new AnimatedReveal(panel);
    container_layout->addWidget(panel_reveal);
    auto *details_notice = new NotificationBanner(*context.settings, 0, 15000);
    container_layout->addWidget(details_notice);
    QObject::connect(service, &DesktopService::appearance_changed, details_notice,
                     &NotificationBanner::refresh_appearance);
    const auto update_details = [=] {
        if (service->is_discovering())
            return;
        if (service->discovery_report().isEmpty()) {
            details_notice->dismiss();
            return;
        }
        panel_reveal->set_expanded(false);
        QString description;
        const auto &progress = service->discovery_status();
        if (progress.percent >= 90)
            description = QCoreApplication::translate("XLibrary", "Candidates processed: %1 of %2")
                              .arg(progress.processed)
                              .arg(progress.total);
        details_notice->show_message(QCoreApplication::translate("XLibrary", "Scan result"), description,
                                     QCoreApplication::translate("XLibrary", "Last scan details…"));
    };
    QObject::connect(service, &DesktopService::discovery_report_changed, container, update_details);
    update_details();
    details_notice->activated = [=] {
        QDialog dialog(container);
        dialog.setWindowTitle(QCoreApplication::translate("XLibrary", "Scan details"));
        prepare_dialog(&dialog, context, QSize(760, 480));
        auto *dialog_layout = new QVBoxLayout(&dialog);
        auto *text = new QPlainTextEdit(service->discovery_report());
        text->setReadOnly(true);
        text->setAccessibleName(QCoreApplication::translate("XLibrary", "Scan report"));
        dialog_layout->addWidget(text);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        buttons->button(QDialogButtonBox::Close)->setText(QCoreApplication::translate("XLibrary", "Close"));
        auto *copy =
            buttons->addButton(QCoreApplication::translate("XLibrary", "Copy all"), QDialogButtonBox::ActionRole);
        QObject::connect(copy, &QPushButton::clicked, &dialog,
                         [text] { QApplication::clipboard()->setText(text->toPlainText()); });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog_layout->addWidget(buttons);
        dialog.exec();
    };
    panel->setObjectName("discovery_panel");
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(8);
    auto *header = new QHBoxLayout;
    auto *stage = label("");
    header->addWidget(stage, 1);
    auto *percentage = label("");
    percentage->setObjectName("discovery_percentage");
    percentage->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    header->addWidget(percentage);
    auto *cancel = button(QCoreApplication::translate("XLibrary", "Cancel"), "quiet");
    cancel->setAccessibleName(QCoreApplication::translate("XLibrary", "Cancel application scan"));
    QObject::connect(cancel, &QPushButton::clicked, service, &DesktopService::cancel_discovery);
    header->addWidget(cancel);
    layout->addLayout(header);
    auto *bar = new QProgressBar;
    bar->setObjectName("discovery_bar");
    bar->setAccessibleName(QCoreApplication::translate("XLibrary", "Application scan progress"));
    bar->setTextVisible(false);
    bar->setFixedHeight(8);
    layout->addWidget(bar);
    auto *counter = label("", "caption");
    layout->addWidget(counter);
    const auto update = [=] {
        const auto &progress = service->discovery_status();
        const bool active = service->is_discovering();
        if (!active) {
            panel_reveal->set_expanded(false);
            return;
        }
        stage->setText(progress.stage);
        const bool indeterminate = progress.percent < 0;
        bar->setRange(0, indeterminate ? 0 : 100);
        if (!indeterminate)
            bar->setValue(progress.percent);
        percentage->setText(indeterminate ? "…" : QString::number(progress.percent) + '%');
        counter->setText(indeterminate
                             ? QCoreApplication::translate("XLibrary", "Collecting entries from the selected sources")
                             : QString(progress.percent < 90
                                           ? QCoreApplication::translate("XLibrary", "Entries checked: %1 of %2")
                                           : QCoreApplication::translate("XLibrary", "Candidates processed: %1 of %2"))
                                   .arg(progress.processed)
                                   .arg(progress.total));
        cancel->setVisible(active);
        cancel->setEnabled(service->can_cancel_discovery());
        cancel->setText(service->is_cancelling_discovery() ? QCoreApplication::translate("XLibrary", "Cancelling…")
                                                           : QCoreApplication::translate("XLibrary", "Cancel"));
        panel_reveal->set_expanded(true);
    };
    QObject::connect(service, &DesktopService::discovery_changed, panel, [=] {
        if (service->is_discovering())
            details_notice->dismiss();
        else
            update_details();
        update();
    });
    QObject::connect(service, &DesktopService::discovery_progress_changed, panel, update);
    update();
    return container;
}

QWidget *library(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = new QWidget(parent);
    auto *root = new QVBoxLayout(widget);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);
    auto *header = new QHBoxLayout;
    auto *title = label(QCoreApplication::translate("XLibrary", "Library"), "heading");
    header->addWidget(title, 1);
    auto *sort = new QComboBox;
    sort->addItems(
        {QCoreApplication::translate("XLibrary", "By name"), QCoreApplication::translate("XLibrary", "Recent launches"),
         QCoreApplication::translate("XLibrary", "Favorites first"), QCoreApplication::translate("XLibrary", "By tags"),
         QCoreApplication::translate("XLibrary", "By status")});
    sort->setAccessibleName(QCoreApplication::translate("XLibrary", "Application sort order"));
    sort->setCurrentIndex(qBound(0, service->settings.value("library/sort", 0).toInt(), 4));
    const auto order =
        service->settings.value("library/sort_order", 0).toInt() == 1 ? Qt::DescendingOrder : Qt::AscendingOrder;
    service->filter.set_sort_mode(sort->currentIndex(), order);
    QObject::connect(sort, &QComboBox::currentIndexChanged, widget,
                     [=](const int index) { service->filter.set_sort_mode(index); });
    header->addWidget(sort);
    auto *remove = action_button(context.actions.value("remove_application"), false,
                                 QCoreApplication::translate("XLibrary", "Remove"));
    remove->setAccessibleName(
        QCoreApplication::translate("XLibrary", "Remove the selected application from the library"));
    header->addWidget(remove);
    auto *scan = action_button(context.actions.value("discover"), false, "");
    scan->setAccessibleName(QCoreApplication::translate("XLibrary", "Find installed applications"));
    scan->setFixedWidth(44);
    header->addWidget(scan);
    root->addLayout(header);
    auto *progress = context.registry->create("discovery_progress", context, widget);
    root->addWidget(progress);
    auto *stack_widget = new QWidget;
    auto *stack = new QStackedLayout(stack_widget);
    auto *view = new ApplicationList;
    view->setObjectName("application_list");
    view->setModel(&service->filter);
    view->setItemDelegate(new ApplicationDelegate(view));
    view->setRootIsDecorated(false);
    view->setUniformRowHeights(true);
    view->setAllColumnsShowFocus(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setMouseTracking(true);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->header()->setStretchLastSection(false);
    view->header()->setMinimumSectionSize(70);
    view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    view->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    view->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    view->header()->setSectionsClickable(true);
    view->header()->setHighlightSections(false);
    view->header()->setSortIndicatorShown(true);
    view->header()->setSortIndicator(service->filter.sortColumn(), service->filter.sortOrder());
    view->header()->setCursor(Qt::PointingHandCursor);
    const auto sort_column = [=](const int column) {
        if (column < 0 || column >= service->filter.columnCount())
            return;
        const auto mode = column == 1 ? 3 : column == 2 ? 4 : 0;
        const auto direction = sort->currentIndex() == mode && service->filter.sortOrder() == Qt::AscendingOrder
                                   ? Qt::DescendingOrder
                                   : Qt::AscendingOrder;
        service->filter.set_sort_mode(mode, direction);
    };
    QObject::connect(view->header(), &QHeaderView::sectionClicked, widget, sort_column);
    QObject::connect(view->header(), &QHeaderView::sectionDoubleClicked, widget, sort_column);
    QObject::connect(&service->filter, &LibraryFilter::sort_changed, widget,
                     [=](const int mode, const Qt::SortOrder direction) {
                         const QSignalBlocker blocker(sort);
                         sort->setCurrentIndex(mode);
                         view->header()->setSortIndicator(service->filter.sortColumn(), direction);
                         if (service->profile_busy() || service->restart_required())
                             return;
                         service->settings.setValue("library/sort", mode);
                         service->settings.setValue("library/sort_order", static_cast<int>(direction));
                     });
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    stack->addWidget(view);
    auto *empty = new QWidget;
    auto *empty_layout = new QVBoxLayout(empty);
    empty_layout->addStretch();
    auto *empty_title = label(QCoreApplication::translate("XLibrary", "Loading the library…"), "subheading");
    empty_title->setAlignment(Qt::AlignCenter);
    empty_layout->addWidget(empty_title);
    auto *hint = label("", "muted");
    hint->setAlignment(Qt::AlignCenter);
    empty_layout->addWidget(hint);
    auto *empty_actions = new QWidget;
    auto *empty_actions_layout = new QVBoxLayout(empty_actions);
    empty_actions_layout->setContentsMargins(0, 0, 0, 0);
    empty_actions_layout->setSpacing(12);
    empty_actions_layout->addWidget(action_button(context.actions.value("add_application")));
    empty_actions_layout->addWidget(action_button(context.actions.value("discover")));
    empty_layout->addWidget(empty_actions, 0, Qt::AlignHCenter);
    auto *reset_filters = button(QCoreApplication::translate("XLibrary", "Clear all filters"));
    reset_filters->setEnabled(false);
    empty_layout->addWidget(reset_filters, 0, Qt::AlignHCenter);
    QObject::connect(reset_filters, &QPushButton::clicked, widget, [=] {
        service->clear_filters();
        if (auto *input = context.window->findChild<QLineEdit *>("search_input"))
            input->setFocus();
    });
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::PlainText);
    hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *recovery_actions = new QWidget;
    auto *recovery_layout = new QVBoxLayout(recovery_actions);
    auto *retry = new QPushButton(QCoreApplication::translate("XLibrary", "Retry"));
    auto *profile = new QPushButton(QCoreApplication::translate("XLibrary", "Open profile folder"));
    auto *restore = new QPushButton(QCoreApplication::translate("XLibrary", "Restore database backup…"));
    auto *restore_profile = new QPushButton(QCoreApplication::translate("XLibrary", "Restore complete profile…"));
    auto *restart = new QPushButton(QCoreApplication::translate("XLibrary", "Restart XLibrary"));
    recovery_layout->addWidget(retry);
    recovery_layout->addWidget(profile);
    recovery_layout->addWidget(restore);
    recovery_layout->addWidget(restore_profile);
    root->addWidget(restart);
    empty_layout->addWidget(recovery_actions, 0, Qt::AlignHCenter);
    QObject::connect(retry, &QPushButton::clicked, service, &DesktopService::retry_library);
    QObject::connect(profile, &QPushButton::clicked, service, &DesktopService::open_profile_folder);
    QObject::connect(restore, &QPushButton::clicked, widget, [=] {
        const auto path =
            QFileDialog::getOpenFileName(widget, QCoreApplication::translate("XLibrary", "Select a SQLite backup"),
                                         QFileInfo(service->library_path()).absolutePath());
        if (path.isEmpty())
            return;
        if (QMessageBox::question(widget, QCoreApplication::translate("XLibrary", "Restore library"),
                                  QCoreApplication::translate(
                                      "XLibrary",
                                      "Replace only the database with this SQLite backup? Settings are not restored. "
                                      "The current database files will be preserved."),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            service->restore_library(path);
    });
    QObject::connect(restore_profile, &QPushButton::clicked, widget, [=] { select_profile_restore(context, widget); });
    QObject::connect(restart, &QPushButton::clicked, service, &DesktopService::restart_application);
    const auto update_recovery = [=] {
        const bool allowed = !service->profile_busy() && !service->restart_required();
        retry->setEnabled(allowed);
        restore->setEnabled(allowed && !service->library_path().isEmpty());
        restore_profile->setEnabled(allowed && !service->is_discovering() && !service->is_scenario_active());
        restart->setVisible(service->restart_required());
        restart->setEnabled(!service->profile_busy());
    };
    QObject::connect(service, &DesktopService::profile_state_changed, widget, update_recovery);
    QObject::connect(service, &DesktopService::discovery_changed, widget, update_recovery);
    QObject::connect(service, &DesktopService::scenario_changed, widget, [=] { update_recovery(); });
    update_recovery();
    empty_layout->addStretch();
    stack->addWidget(empty);
    auto *library_transition = new PageTransition(stack_widget);
    root->addWidget(library_transition, 1);
    auto *footer = new QHBoxLayout;
    auto *shortcut_hint = label({}, "caption");
    const auto update_shortcut_hint = [=] {
        QStringList hints{QCoreApplication::translate("XLibrary", "↑↓  Select")};
        for (const auto &id : {"launch", "remove_application", "focus_search"}) {
            const auto *action = context.actions.value(id);
            if (!action->shortcut().isEmpty())
                hints.append(action->shortcut().toString(QKeySequence::NativeText) + "  " + action->text());
        }
        shortcut_hint->setText(hints.join("     "));
    };
    for (const auto &id : {"launch", "remove_application", "focus_search"})
        QObject::connect(context.actions.value(id), &QAction::changed, shortcut_hint, update_shortcut_hint);
    update_shortcut_hint();
    footer->addWidget(shortcut_hint, 1);
    auto *details = action_button(context.actions.value("details"), true);
    footer->addWidget(details);
    root->addLayout(footer);
    auto *selection_tools = new QHBoxLayout;
    auto *selected_count = label({});
    selection_tools->addWidget(selected_count, 1);
    auto *clear_selection = button(QCoreApplication::translate("XLibrary", "Clear selection"));
    selection_tools->addWidget(clear_selection);
    selection_tools->addWidget(action_button(context.actions.value("bulk_edit")));
    selection_tools->addWidget(action_button(context.actions.value("undo_removal")));
    root->addLayout(selection_tools);
    const auto synchronize_selection = [=] {
        QVector<qint64> ids;
        for (const auto &index : view->selectionModel()->selectedRows())
            ids.append(index.data(LibraryModel::id_role).toLongLong());
        service->select_applications(ids);
        const auto current_id = view->currentIndex().data(LibraryModel::id_role).toLongLong();
        service->select(ids.contains(current_id) ? current_id : ids.value(0));
        selected_count->setText(QCoreApplication::translate("XLibrary", "Selected applications: %1").arg(ids.size()));
        clear_selection->setEnabled(!ids.isEmpty());
    };
    auto resetting = std::make_shared<bool>(false);
    QObject::connect(&service->filter, &QAbstractItemModel::modelAboutToBeReset, widget, [=] { *resetting = true; });
    QObject::connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, widget, [=] {
        if (!*resetting)
            synchronize_selection();
    });
    QObject::connect(clear_selection, &QPushButton::clicked, widget, [=] {
        view->selectionModel()->clear();
        service->select(0);
        synchronize_selection();
    });
    auto update = [=] {
        const bool has_rows = service->filter.rowCount() > 0;
        const bool no_apps = service->is_ready() && service->model.rowCount() == 0;
        const auto *previous_page = stack->currentWidget();
        stack->setCurrentWidget(has_rows ? static_cast<QWidget *>(view) : empty);
        empty_actions->setVisible(no_apps);
        reset_filters->hide();
        const bool failed = !service->is_ready() && !service->is_opening() && !service->library_error().isEmpty();
        recovery_actions->setVisible(failed || service->restart_required());
        update_recovery();
        if (no_apps && empty_layout->indexOf(progress) < 0) {
            root->removeWidget(progress);
            empty_layout->insertWidget(empty_layout->count() - 1, progress);
            progress->show();
        } else if (!no_apps && root->indexOf(progress) < 0) {
            empty_layout->removeWidget(progress);
            root->insertWidget(1, progress);
            progress->show();
        }
        if (!service->is_ready()) {
            empty_title->setText(failed ? QCoreApplication::translate("XLibrary", "Could not open the library")
                                        : QCoreApplication::translate("XLibrary", "Loading the library…"));
            hint->setText(failed ? service->library_error() + '\n' + QDir::toNativeSeparators(service->library_path())
                                 : QString{});
        } else {
            empty_title->setText(no_apps ? QCoreApplication::translate("XLibrary", "Add your first application")
                                         : QCoreApplication::translate("XLibrary", "No results"));
            hint->setText(no_apps
                              ? QCoreApplication::translate("XLibrary", "Select a file or find installed applications.")
                              : QCoreApplication::translate("XLibrary", "Change the query or select another section."));
        }
        if (previous_page != stack->currentWidget())
            library_transition->start();
        if (!has_rows) {
            *resetting = false;
            view->selectionModel()->clear();
            synchronize_selection();
            service->select(0);
            return;
        }
        QModelIndex target;
        for (int row = 0; row < service->filter.rowCount(); ++row) {
            const auto index = service->filter.index(row, 0);
            if (index.data(LibraryModel::id_role).toLongLong() == service->selection()) {
                target = index;
                break;
            }
        }
        if (!target.isValid())
            target = service->filter.index(0, 0);
        if (*resetting) {
            const QSignalBlocker blocker(view->selectionModel());
            for (int row = 0; row < service->filter.rowCount(); ++row) {
                const auto index = service->filter.index(row, 0);
                if (service->selections().contains(index.data(LibraryModel::id_role).toLongLong()))
                    view->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
            }
            *resetting = false;
        }
        if (view->currentIndex().siblingAtColumn(0) != target)
            view->selectionModel()->setCurrentIndex(target, QItemSelectionModel::NoUpdate);
        if (view->selectionModel()->selectedRows().isEmpty())
            view->selectionModel()->select(target, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        service->select(target.data(LibraryModel::id_role).toLongLong());
        synchronize_selection();
    };
    QObject::connect(view->selectionModel(), &QItemSelectionModel::currentChanged, widget,
                     [=](const QModelIndex &index) {
                         if (index.isValid() && view->selectionModel()->isRowSelected(index.row(), {}))
                             service->select(index.data(LibraryModel::id_role).toLongLong());
                         else if (!*resetting)
                             service->select(service->selections().value(0));
                     });
    QObject::connect(view, &QTreeView::doubleClicked, widget, [=](const QModelIndex &index) {
        service->launch(index.data(LibraryModel::id_role).toLongLong());
    });
    QObject::connect(view, &QTreeView::customContextMenuRequested, widget, [=](const QPoint &position) {
        const auto index = view->indexAt(position);
        if (!index.isValid())
            return;
        if (!view->selectionModel()->isRowSelected(index.row(), {}))
            view->setCurrentIndex(index);
        else
            view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
        service->select(index.data(LibraryModel::id_role).toLongLong());
        auto *menu_widget = context.registry->create("application_menu", context, view);
        if (auto *menu = qobject_cast<QMenu *>(menu_widget))
            menu->exec(view->viewport()->mapToGlobal(position));
        delete menu_widget;
    });
    QObject::connect(service, &DesktopService::catalog_changed, widget, update);
    QObject::connect(&service->filter, &QAbstractItemModel::rowsRemoved, widget, update);
    QObject::connect(&service->filter, &QAbstractItemModel::rowsInserted, widget, update);
    QObject::connect(&service->filter, &QAbstractItemModel::modelReset, widget, update);
    QObject::connect(&service->filter, &QAbstractItemModel::layoutChanged, widget, update);
    QObject::connect(service, &DesktopService::navigation_changed, widget, [=] {
        title->setText(service->section() == "favorites" ? QCoreApplication::translate("XLibrary", "Favorites")
                       : service->section() == "running"
                           ? QCoreApplication::translate("XLibrary", "Running applications")
                           : QCoreApplication::translate("XLibrary", "Library"));
        update();
    });
    update();
    return widget;
}

QWidget *details(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *contents = new QWidget;
    contents->setProperty("panel", true);
    auto *root = new QVBoxLayout(contents);
    root->setContentsMargins(20, 24, 20, 24);
    root->setSpacing(12);
    auto *icon = new QLabel;
    icon->setFixedSize(56, 56);
    auto *name = label(QCoreApplication::translate("XLibrary", "Select an application"), "application_title");
    auto *status = label("", "muted");
    auto *identity = new QHBoxLayout;
    identity->setSpacing(12);
    identity->addWidget(icon, 0, Qt::AlignTop);
    auto *identity_text = new QVBoxLayout;
    identity_text->setSpacing(4);
    identity_text->addWidget(name);
    identity_text->addWidget(status);
    identity->addLayout(identity_text, 1);
    root->addLayout(identity);
    root->addSpacing(8);
    auto *tag_header = new QHBoxLayout;
    tag_header->addWidget(label(QCoreApplication::translate("XLibrary", "Tags"), "muted"), 1);
    auto *edit_tags =
        action_button(context.actions.value("edit_application"), true, QCoreApplication::translate("XLibrary", "Edit"));
    edit_tags->setAccessibleName(QCoreApplication::translate("XLibrary", "Edit application tags"));
    tag_header->addWidget(edit_tags);
    root->addLayout(tag_header);
    auto *tags = label("—");
    root->addWidget(tags);
    root->addSpacing(8);
    root->addWidget(label(QCoreApplication::translate("XLibrary", "Application path"), "muted"));
    auto *path = new QLineEdit;
    path->setMaxLength(static_cast<int>(catalog_limits::text_length));
    path->setReadOnly(true);
    path->setMinimumWidth(0);
    auto *path_row = new QHBoxLayout;
    path_row->setSpacing(8);
    path_row->addWidget(path, 1);
    auto *folder = action_button(context.actions.value("open_folder"), false, "");
    folder->setAccessibleName(QCoreApplication::translate("XLibrary", "Open application folder"));
    folder->setFixedWidth(44);
    path_row->addWidget(folder);
    root->addLayout(path_row);
    auto *recover_path = button(QCoreApplication::translate("XLibrary", "Locate application…"));
    root->addWidget(recover_path);
    QObject::connect(recover_path, &QPushButton::clicked, contents, [=] {
        if (const auto *record = service->selected())
            select_application_replacement(service, *record, contents);
    });
    auto *arguments = label("", "muted");
    arguments->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(arguments);
    root->addWidget(action_button(context.actions.value("copy_arguments"), true));
    auto *source = label({}, "muted");
    source->setWordWrap(true);
    source->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Long paths and arguments must wrap within the pane, not widen its scroll contents.
    for (auto *text : {name, status, tags, arguments, source}) {
        auto policy = text->sizePolicy();
        policy.setHorizontalPolicy(QSizePolicy::Ignored);
        text->setSizePolicy(policy);
        text->setMinimumWidth(0);
    }
    root->addWidget(source);
    root->addSpacing(8);
    auto *launch = action_button(context.actions.value("launch"));
    launch->setProperty("primary", true);
    root->addWidget(launch);
    auto *profile_actions = new QWidget;
    auto *profile_layout = new QVBoxLayout(profile_actions);
    profile_layout->setContentsMargins(0, 0, 0, 0);
    auto *profiles = new QComboBox;
    profiles->setAccessibleName(QCoreApplication::translate("XLibrary", "Browser launch profile"));
    profile_layout->addWidget(profiles);
    auto *launch_selected_profile = button(QCoreApplication::translate("XLibrary", "Launch selected profile"));
    profile_layout->addWidget(launch_selected_profile);
    auto *default_profile = button(QCoreApplication::translate("XLibrary", "Set as default"));
    profile_layout->addWidget(default_profile);
    root->addWidget(profile_actions);
    QObject::connect(launch_selected_profile, &QPushButton::clicked, contents, [=] {
        if (const auto *record = service->selected())
            service->request_launch_profile(record->id, profiles->currentData().toString().isEmpty()
                                                            ? QString("configured")
                                                            : profiles->currentData().toString());
    });
    QObject::connect(default_profile, &QPushButton::clicked, contents, [=] {
        if (const auto *record = service->selected()) {
            auto updated = *record;
            updated.default_launch_profile = profiles->currentData().toString();
            service->save_application(std::move(updated));
        }
    });
    root->addWidget(action_button(context.actions.value("edit_application")));
    root->addWidget(action_button(context.actions.value("favorite")));
    auto *remove = action_button(context.actions.value("remove_application"));
    remove->setProperty("danger", true);
    root->addWidget(remove);
    root->addStretch();
    auto update = [=] {
        const auto *record = service->selected();
        name->setText(record ? record->name : QCoreApplication::translate("XLibrary", "Select an application"));
        status->setText(record ? record->running     ? QCoreApplication::translate("XLibrary", "● Running")
                                 : record->available ? QCoreApplication::translate("XLibrary", "Not running")
                                 : record->volume_unavailable
                                     ? QCoreApplication::translate("XLibrary", "Drive disconnected or unavailable")
                                     : QCoreApplication::translate("XLibrary", "File unavailable; the entry is kept")
                               : QCoreApplication::translate("XLibrary", "Details will appear here."));
        tags->setText(record && !record->tags.isEmpty() ? '#' + record->tags.join("   #")
                                                        : QCoreApplication::translate("XLibrary", "No tags"));
        path->setText(record ? QDir::toNativeSeparators(record->path) : QString{});
        source->setText(
            record && !record->discovery_sources.isEmpty()
                ? QCoreApplication::translate("XLibrary", "Source: %1").arg(record->discovery_sources.join('\n'))
                : QCoreApplication::translate("XLibrary", "Source: manually added or imported"));
        path->setCursorPosition(0);
        path->setToolTip(path->text());
        recover_path->setVisible(record && Application::file_target(*record) && !record->available);
        const QSignalBlocker profile_blocker(profiles);
        const auto selected_profile = record && profiles->property("application_id").toLongLong() == record->id
                                          ? profiles->currentData().toString()
                                      : record ? record->default_launch_profile
                                               : QString{};
        profiles->setProperty("application_id", record ? record->id : 0);
        profiles->clear();
        const auto browser_profiles = record ? service->launch_profiles(*record) : QVector<launch_profile>{};
        profile_actions->setVisible(!browser_profiles.isEmpty());
        profiles->addItem(QCoreApplication::translate("XLibrary", "Configured launch arguments"), QString{});
        for (const auto &profile : browser_profiles)
            profiles->addItem(profile.icon_path.isEmpty() ? appearance::icon(profile.kind == "private" ? "lock" : "app")
                                                          : QIcon(profile.icon_path),
                              profile.name, profile.id);
        if (!selected_profile.isEmpty() && profiles->findData(selected_profile) < 0)
            profiles->addItem(QCoreApplication::translate("XLibrary", "Unavailable profile: %1").arg(selected_profile),
                              selected_profile);
        profiles->setCurrentIndex(qMax(0, profiles->findData(selected_profile)));
        const bool can_change = record && !service->profile_busy() && !service->restart_required();
        launch_selected_profile->setEnabled(can_change && record->available);
        default_profile->setEnabled(can_change);
        recover_path->setEnabled(record && !service->profile_busy() && !service->restart_required() &&
                                 !service->is_discovering() && !service->is_scenario_active());
        arguments->setText(record && !record->arguments.isEmpty()
                               ? QCoreApplication::translate("XLibrary", "Arguments: ") + record->arguments
                               : QString{});
        arguments->setVisible(!arguments->text().isEmpty());
        QIcon image = appearance::icon("app");
        if (record)
            for (int row = 0; row < service->model.rowCount(); ++row) {
                const auto index = service->model.index(row, 0);
                if (index.data(LibraryModel::id_role).toLongLong() == record->id) {
                    const auto cached = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
                    if (!cached.isNull())
                        image = cached;
                    break;
                }
            }
        icon->setPixmap(image.pixmap(56, 56));
    };
    QObject::connect(service, &DesktopService::launch_profiles_changed, contents, [=](const QString &changed_path) {
        if (const auto *record = service->selected(); record && record->path == changed_path)
            update();
    });
    QObject::connect(service, &DesktopService::selection_changed, contents, update);
    QObject::connect(service, &DesktopService::profile_state_changed, contents, update);
    QObject::connect(service, &DesktopService::discovery_changed, contents, update);
    QObject::connect(service, &DesktopService::scenario_changed, contents, [=] { update(); });
    QObject::connect(&service->model, &QAbstractItemModel::dataChanged, contents, [=] { update(); });
    update();
    auto *result = scroll(contents);
    result->setParent(parent);
    result->setProperty("panel", true);
    return result;
}

QWidget *application_menu(const ui_context &context, QWidget *const parent) {
    auto *menu = new QMenu(parent);
    for (const auto &id : {"launch", "new_instance", "open_folder", "copy_path", "copy_arguments", "favorite",
                           "edit_application", "bulk_edit", "undo_removal"})
        menu->addAction(context.actions.value(id));
    auto *service = static_cast<DesktopService *>(context.service);
    if (const auto *record = service->selected()) {
        const auto profiles = service->launch_profiles(*record);
        if (!profiles.isEmpty()) {
            auto *launch_profiles = menu->addMenu(QCoreApplication::translate("XLibrary", "Browser launch profiles"));
            auto *defaults =
                launch_profiles->addMenu(QCoreApplication::translate("XLibrary", "Default launch profile"));
            const auto application_id = record->id;
            auto *configured =
                defaults->addAction(QCoreApplication::translate("XLibrary", "Configured launch arguments"));
            configured->setCheckable(true);
            configured->setChecked(record->default_launch_profile.isEmpty());
            QObject::connect(configured, &QAction::triggered, menu, [=] {
                if (const auto *current = service->model.find(application_id)) {
                    auto updated = *current;
                    updated.default_launch_profile.clear();
                    service->save_application(std::move(updated));
                }
            });
            for (const auto &profile : profiles) {
                const auto icon = profile.icon_path.isEmpty()
                                      ? appearance::icon(profile.kind == "private" ? "lock" : "app")
                                      : QIcon(profile.icon_path);
                auto *launch = launch_profiles->addAction(icon, profile.name);
                launch->setEnabled(record->available && !service->profile_busy() && !service->restart_required());
                QObject::connect(launch, &QAction::triggered, menu,
                                 [=] { service->request_launch_profile(application_id, profile.id); });
                auto *set_default = defaults->addAction(icon, profile.name);
                set_default->setCheckable(true);
                set_default->setChecked(record->default_launch_profile == profile.id);
                QObject::connect(set_default, &QAction::triggered, menu, [=] {
                    if (const auto *current = service->model.find(application_id)) {
                        auto updated = *current;
                        updated.default_launch_profile = profile.id;
                        service->save_application(std::move(updated));
                    }
                });
            }
            defaults->setEnabled(!service->profile_busy() && !service->restart_required());
        }
    }
    if (const auto *record = service->selected(); record && Application::file_target(*record) && !record->available) {
        auto *recover = menu->addAction(QCoreApplication::translate("XLibrary", "Locate application…"));
        recover->setEnabled(!service->profile_busy() && !service->restart_required() && !service->is_discovering() &&
                            !service->is_scenario_active());
        const auto original = *record;
        QObject::connect(recover, &QAction::triggered, menu,
                         [=] { select_application_replacement(service, original, parent); });
    }
    menu->addSeparator();
    menu->addAction(context.actions.value("remove_application"));
    return menu;
}

QWidget *scenarios(const ui_context &context, QWidget *const parent) {
    auto *service = static_cast<DesktopService *>(context.service);
    auto *widget = new QWidget(parent);
    auto *root = new QVBoxLayout(widget);
    root->setContentsMargins(0, 0, 0, 0);
    auto *header = new QHBoxLayout;
    header->addWidget(label(QCoreApplication::translate("XLibrary", "Scenarios"), "heading"), 1);
    header->addWidget(action_button(context.actions.value("add_scenario")));
    root->addLayout(header);
    auto *list = new QListWidget;
    list->setSpacing(2);
    root->addWidget(list, 1);
    auto *empty = label(
        QCoreApplication::translate("XLibrary", "Create a scenario to open several applications in order."), "muted");
    root->addWidget(empty);
    auto *status = label("", "muted");
    root->addWidget(status);
    auto *tools = new QHBoxLayout;
    auto *run = button(QCoreApplication::translate("XLibrary", "Run scenario"), "primary");
    auto *edit = button(QCoreApplication::translate("XLibrary", "Edit"));
    auto *remove = button(QCoreApplication::translate("XLibrary", "Remove"));
    auto *stop = button(QCoreApplication::translate("XLibrary", "Stop"));
    tools->addWidget(run);
    tools->addWidget(edit);
    tools->addWidget(remove);
    tools->addStretch();
    tools->addWidget(stop);
    root->addLayout(tools);
    auto rebuild = [=] {
        const auto selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toLongLong() : 0;
        list->clear();
        for (const auto &record : service->catalog().scenarios) {
            auto *item = new QListWidgetItem(
                appearance::icon("scenarios"),
                record.name + QCoreApplication::translate("XLibrary", "\nApplications: %1").arg(record.steps.size()),
                list);
            item->setData(Qt::UserRole, record.id);
            item->setSizeHint({100, 64});
            if (record.id == selected)
                list->setCurrentItem(item);
        }
        if (!list->currentItem() && list->count())
            list->setCurrentRow(0);
        const bool has = list->count() > 0;
        run->setEnabled(has && !service->is_scenario_active());
        edit->setEnabled(has);
        remove->setEnabled(has);
        empty->setVisible(!has);
        stop->setEnabled(service->is_scenario_active());
    };
    auto run_selected = [=] {
        if (list->currentItem())
            service->run_scenario(list->currentItem()->data(Qt::UserRole).toLongLong());
    };
    QObject::connect(run, &QPushButton::clicked, widget, run_selected);
    QObject::connect(list, &QListWidget::itemDoubleClicked, widget, [=] { run_selected(); });
    QObject::connect(edit, &QPushButton::clicked, widget, [=] {
        if (!list->currentItem())
            return;
        const auto id = list->currentItem()->data(Qt::UserRole).toLongLong();
        for (const auto &record : service->catalog().scenarios)
            if (record.id == id) {
                auto edit_context = context;
                edit_context.parameters.insert("scenario", QVariant::fromValue(record));
                open_dialog("scenario_editor", edit_context);
                break;
            }
    });
    QObject::connect(remove, &QPushButton::clicked, widget, [=] {
        if (list->currentItem() && confirm(widget, QCoreApplication::translate("XLibrary", "Remove scenario"),
                                           QCoreApplication::translate("XLibrary", "Remove the selected scenario?")))
            service->remove_scenario(list->currentItem()->data(Qt::UserRole).toLongLong());
    });
    QObject::connect(stop, &QPushButton::clicked, service, &DesktopService::cancel_scenario);
    QObject::connect(service, &DesktopService::catalog_changed, widget, rebuild);
    QObject::connect(service, &DesktopService::scenario_changed, widget, [=](const QString &text) {
        status->setText(text);
        run->setEnabled(list->count() > 0 && !service->is_scenario_active());
        stop->setEnabled(service->is_scenario_active());
    });
    rebuild();
    return widget;
}
}

void register_library_views(UiRegistry &registry) {
    registry.add_default("discovery_progress", discovery_status);
    registry.add_default("navigation", navigation);
    registry.add_default("search_bar", search_bar);
    registry.add_default("library", library);
    registry.add_default("details", details);
    registry.add_default("details_dialog", [](const ui_context &context, QWidget *const parent) {
        auto *dialog = new QDialog(parent);
        dialog->setWindowTitle(QCoreApplication::translate("XLibrary", "Application details"));
        prepare_dialog(dialog, context, {400, 660}, {300, 320});
        auto *layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(context.registry->create("details", context, dialog), 1);
        auto *close = button(QCoreApplication::translate("XLibrary", "Close"));
        layout->addWidget(close);
        QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
        return dialog;
    });
    registry.add_default("scenarios", scenarios);
    registry.add_default("application_menu", application_menu);
}
